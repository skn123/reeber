include_directories (${CMAKE_CURRENT_SOURCE_DIR}/include)

add_subdirectory    (global)
add_subdirectory    (local-global)

if                  (amrex)
    add_subdirectory(amr-merge-tree)
endif()
add_subdirectory(amr-connected-components)
