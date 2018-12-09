#pragma once

#include <utility>
#include <numeric>
#include <stack>
#include <boost/functional/hash.hpp>

#include "diy/serialization.hpp"
#include "diy/grid.hpp"
#include "diy/link.hpp"
#include "diy/fmt/format.h"
#include "diy/fmt/ostream.h"
#include "diy/point.hpp"
#include <diy/master.hpp>

#include "disjoint-sets.h"

#include "reeber/amr-vertex.h"
#include "reeber/triplet-merge-tree.h"
#include "reeber/triplet-merge-tree-serialization.h"
#include "reeber/grid.h"
#include "reeber/grid-serialization.h"
#include "reeber/masked-box.h"
#include "reeber/edges.h"

#include "../../amr-merge-tree/include/fab-block.h"
#include "reeber/amr_helper.h"

namespace r = reeber;

template<class Real, unsigned D>
struct FabComponentBlock
{
    using Shape = diy::Point<int, D>;

    using Grid = r::Grid<Real, D>;
    using GridRef = r::GridRef<Real, D>;
    // index of point: first = index inside box, second = index of a box
    using AmrVertexId = r::AmrVertexId;
    using Value = typename Grid::Value;
    using MaskedBox = r::MaskedBox<D>;
    using Vertex = typename MaskedBox::Position;
    using AmrVertexContainer = std::vector<AmrVertexId>;

    using AmrEdge = r::AmrEdge;
    using AmrEdgeContainer = r::AmrEdgeContainer;
    using AmrEdgeSet = std::set<AmrEdge>;
    using VertexEdgesMap = std::map<AmrVertexId, AmrEdgeContainer>;

    using GidContainer = std::set<int>;
    using GidVector = std::vector<int>;

    using RealType = Real;

    using UnionFind = DisjointSets<AmrVertexId>;
    using VertexVertexMap = UnionFind::VertexVertexMap;
    using VertexSizeMap = UnionFind::VertexSizeMap;

    using AmrVertexSet = std::unordered_set<AmrVertexId>;

    struct VertexValue
    {
        AmrVertexId vertex;
        Real value;
    };

    using VertexDeepestMap = std::unordered_map<AmrVertexId, VertexValue>;

    template<class Vertex_>
    struct ConnectedComponent
    {
        // types
        using Vertex = Vertex_;

        // fields

        AmrVertexId global_deepest_;    // will be updated in each communication round
        const AmrVertexId original_deepest_;
        Real global_value_;
        const Real original_value_;

        AmrVertexSet current_neighbors_;
        AmrVertexSet processed_neighbors_;
        AmrEdgeContainer outgoing_edges_;

        // methods

        ConnectedComponent()
        {
        }

        ConnectedComponent(const AmrVertexId& deepest, Real value) :
                global_deepest_(deepest),
                original_deepest_(deepest),
                global_value_(value),
                original_value_(value),
                current_neighbors_({deepest}),
                processed_neighbors_({deepest})
        {
        }

        void init_current_neighbors(bool debug = false)
        {
            debug = false;
            current_neighbors_.clear();

            std::transform(outgoing_edges_.begin(), outgoing_edges_.end(),
                           std::inserter(current_neighbors_, current_neighbors_.begin()),
                           [this](const AmrEdge& e) {
                               assert(std::get<0>(e).gid == this->original_deepest_.gid);
                               assert(std::get<1>(e).gid != this->original_deepest_.gid);
                               return std::get<1>(e).gid;
                           });

            if(debug)
                fmt::print("In init_current_neighbors for component = {}, current_neighbors_.size = {}\n", original_deepest_,
                           current_neighbors_.size());
        }


        template<class EC>
        void set_edges(const EC& initial_edges, FabComponentBlock::UnionFind& disjoint_sets)
        {
            bool debug = false;

            for(const auto& e : initial_edges)
            {
                if(debug) fmt::print("in set_edges, considering edge {}\n", e);

                if(disjoint_sets.find_component(std::get<0>(e)) == original_deepest_)
                {
                    outgoing_edges_.emplace_back(e);
                    if(debug) fmt::print("in set_edges, added edge {}\n", e);
                }
            }
            init_current_neighbors();
        }

        int is_not_done() const
        {
            assert(std::includes(current_neighbors_.begin(), current_neighbors_.end(),
                                 processed_neighbors_.begin(), processed_neighbors_.end()));
            return current_neighbors_.size() > processed_neighbors_.size();
        }

        bool must_send_to_gid(int gid) const
        {
            return current_neighbors_.count(gid) == 1 and processed_neighbors_.count(gid) == 0;
        }

    };

    using Component = ConnectedComponent<reeber::AmrVertexId>;

    // data

    int gid;
    MaskedBox local_;
    GridRef fab_;

    // if relative threshold is given, we cannot determine
    // LOW values in constructor. Instead, we mark all unmasked vertices
    // ACTIVE and save the average of their values in local_sum_ and the number in local_n_unmasked_
    // Pointer to grid data is saved in fab_ and after all blocks exchange their local averages
    // we resume initialization
    Real sum_{0};
    size_t n_unmasked_{0};
    std::unordered_map<AmrVertexId, Real> vertex_values_;

    UnionFind disjoint_sets_;   // keep topology of graph of connected components
    std::vector<Component> components_;

    VertexDeepestMap vertex_to_deepest_;

    diy::DiscreteBounds domain_;

    int done_{0};

    //    // will be changed in each communication round
    //    // only for baseiline algorithm
    //    AmrEdgeSet outgoing_edges_;

    // is pre-computed once. does not change
    // only for baseiline algorithm
    AmrEdgeContainer initial_edges_;

    std::unordered_map<int, AmrEdgeContainer> gid_to_outgoing_edges_;

    std::unordered_set<AmrVertexId> new_receivers_;         // roots of local components in other blocks
                                                            // to which we should send this local component
    std::unordered_set<AmrVertexId> processed_receivers_;   // roots of components to which we have sent this local component

    GidVector original_link_gids_;

    bool negate_;

    // tracking how connected components merge - disjoint sets data structure

    int round_{0};

    // methods

    // simple getters/setters
    const diy::DiscreteBounds& domain() const
    { return domain_; }

    int refinement() const
    { return local_.refinement(); }

    int level() const
    { return local_.level(); }

    const GidVector& get_original_link_gids() const
    { return original_link_gids_; }

    FabComponentBlock(diy::GridRef<Real, D>& fab_grid,
                      int _ref,
                      int _level,
                      const diy::DiscreteBounds& _domain,
                      const diy::DiscreteBounds& bounds,
                      const diy::DiscreteBounds& core,
                      int _gid,
                      diy::AMRLink* amr_link,
                      Real rho,                                           // threshold for LOW value
                      bool _negate,
                      bool is_absolute_threshold) :
            gid(_gid),
            local_(project_point<D>(core.min), project_point<D>(core.max), project_point<D>(bounds.min),
                   project_point<D>(bounds.max), _ref, _level, gid, fab_grid.c_order()),
            fab_(fab_grid.data(), fab_grid.shape(), fab_grid.c_order()),
            domain_(_domain),
            processed_receivers_({gid}),
            negate_(_negate)
    {
        bool debug = false;

        std::string debug_prefix = "FabComponentBlock ctor, gid = " + std::to_string(gid);

        if(debug) fmt::print("{} setting mask\n", debug_prefix);

        diy::for_each(local_.mask_shape(), [this, amr_link, rho, is_absolute_threshold](const Vertex& v) {
            this->set_mask(v, amr_link, rho, is_absolute_threshold);
        });

        //        if (debug) fmt::print("gid = {}, checking mask\n", gid);
        int max_gid = 0;
        for(int i = 0; i < amr_link->size(); ++i)
        {
            max_gid = std::max(max_gid, amr_link->target(i).gid);
        }

        //local_.check_mask_validity(max_gid);

        if(is_absolute_threshold)
        {
            init(rho, amr_link);
        }
    }

    FabComponentBlock() :
            fab_(nullptr, diy::Point<int, D>::zero())
    {}

    void init(Real absolute_rho, diy::AMRLink* amr_link);

    // compare w.r.t negate_ flag
    bool cmp(Real a, Real b) const;

    void set_low(const diy::Point<int, D>& v_bounds,
                 const Real& absolute_rho);

    void set_mask(const diy::Point<int, D>& v_bounds,
                  diy::AMRLink* l,
                  const Real& rho,
                  bool is_absolute_threshold);

    // return true, if both edge vertices are in the current neighbourhood
    // no checking of mask is performed, if a vertex is LOW, function will return true.
    // Such an edge must be silently ignored in the merge procedure.
    bool edge_exists(const AmrEdge& e) const;

    // return true, if one of the edge's vertices is inside current neighbourhood
    // and the other is outside
    bool edge_goes_out(const AmrEdge& e) const;

    void compute_outgoing_edges(diy::AMRLink* l, VertexEdgesMap& vertex_to_outgoing_edges);

    void compute_original_connected_components(const VertexEdgesMap& vertex_to_outgoing_edges);

//    void compute_final_connected_components();
//
    void delete_low_edges(int sender_gid, AmrEdgeContainer& edges_from_sender);
//
    void adjust_outgoing_edges();

    bool is_component_connected_to_any_internal(const AmrVertexId& deepest);

    void sparsify_prune_original_tree() {}

//    void add_received_original_vertices(const VertexVertexMap& received_vertex_to_deepest);

    int are_all_components_done() const;

    std::vector<AmrVertexId> get_current_deepest_vertices() const;

//    int n_undone_components() const;

    int is_done_simple(const std::vector<FabComponentBlock::AmrVertexId>& vertices_to_check);

    void compute_local_integral(Real rho, Real theta);

    Real scaling_factor() const;

    std::vector<AmrVertexId> get_original_deepest_vertices() const;

    const AmrEdgeContainer& get_all_outgoing_edges()
    { return initial_edges_; }

    // v must be the deepest vertex in a local connected component
    // cannot be const - path compression!
    AmrVertexId find_component_in_disjoint_sets(AmrVertexId v);

    static void* create()
    {
        return new FabComponentBlock;
    }

    static void destroy(void* b)
    {
        delete static_cast<FabComponentBlock*>(b);
    }

    static void save(const void* b, diy::BinaryBuffer& bb);

    static void load(void* b, diy::BinaryBuffer& bb);
};


#include "fab-cc-block.hpp"
