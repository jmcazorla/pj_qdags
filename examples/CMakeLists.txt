add_executable(generate_demo_dataset generate_demo_dataset.cpp)
target_link_libraries(generate_demo_dataset PRIVATE qdags::join)

add_executable(build_dataset build_dataset.cpp)
target_link_libraries(build_dataset PRIVATE qdags::join)

# ccq backend (include/qdags/ccq/): build the dataset in its format and run the strategies over its quadtree/qdag/AND.
add_executable(build_dataset_ccq build_dataset_ccq.cpp)
target_link_libraries(build_dataset_ccq PRIVATE qdags::join)

add_executable(run_ccq run_ccq.cpp)
target_link_libraries(run_ccq PRIVATE qdags::join qdags::query qdags::strategy)

add_executable(run_bgp_query run_bgp_query.cpp)
target_link_libraries(run_bgp_query PRIVATE qdags::join qdags::query qdags::strategy)

add_executable(classify_queries classify_queries.cpp)
target_link_libraries(classify_queries PRIVATE qdags::query)

add_executable(run_nl run_nl.cpp run_nl_const.cpp)
target_link_libraries(run_nl PRIVATE qdags::join qdags::query qdags::strategy)

add_executable(run_wk run_wk.cpp run_wk_const.cpp)
target_link_libraries(run_wk PRIVATE qdags::join qdags::query qdags::strategy)

add_executable(run_wk_mixed run_wk_mixed.cpp run_wk_mixed_const.cpp)
target_link_libraries(run_wk_mixed PRIVATE qdags::join qdags::query qdags::strategy)

add_executable(run_leapfrog run_leapfrog.cpp run_leapfrog_const.cpp)
target_link_libraries(run_leapfrog PRIVATE qdags::join qdags::query qdags::strategy)

add_executable(run_leapfrog_resumable run_leapfrog_resumable.cpp run_leapfrog_resumable_const.cpp)
target_link_libraries(run_leapfrog_resumable PRIVATE qdags::join qdags::query qdags::strategy)

add_executable(run_leapfrog_race run_leapfrog_race.cpp run_leapfrog_race_const.cpp)
target_link_libraries(run_leapfrog_race PRIVATE qdags::join qdags::query qdags::strategy)

add_executable(run_kernel_race run_kernel_race.cpp run_kernel_race_const.cpp)
target_link_libraries(run_kernel_race PRIVATE qdags::join qdags::query qdags::strategy)

add_executable(run_kernel run_kernel.cpp run_kernel_const.cpp)
target_link_libraries(run_kernel PRIVATE qdags::join qdags::query qdags::strategy)

add_executable(run_orig run_orig.cpp run_orig_const.cpp)
target_link_libraries(run_orig PRIVATE qdags::join qdags::query qdags::strategy)

add_executable(bfs_prelude_bench bfs_prelude_bench.cpp)
target_link_libraries(bfs_prelude_bench PRIVATE qdags::join qdags::query)

add_executable(run_mxm run_mxm.cpp)
target_link_libraries(run_mxm PRIVATE qdags::join qdags::query qdags::strategy)

add_executable(run_wk_resumable run_wk_resumable.cpp run_wk_resumable_const.cpp)
target_link_libraries(run_wk_resumable PRIVATE qdags::join qdags::query qdags::strategy)

add_executable(run_nl_race run_nl_race.cpp run_nl_race_const.cpp)
target_link_libraries(run_nl_race PRIVATE qdags::join qdags::query qdags::strategy)

add_executable(run_wk_race run_wk_race.cpp run_wk_race_const.cpp)
target_link_libraries(run_wk_race PRIVATE qdags::join qdags::query qdags::strategy)

add_executable(run_plan run_plan.cpp)
target_link_libraries(run_plan PRIVATE qdags::join qdags::query qdags::strategy)

add_executable(run_plan_hybrid run_plan_hybrid.cpp)
target_link_libraries(run_plan_hybrid PRIVATE qdags::join qdags::query qdags::strategy)

add_executable(run_plan_resumable run_plan_resumable.cpp)
target_link_libraries(run_plan_resumable PRIVATE qdags::join qdags::query qdags::strategy)

add_executable(run_plan_race run_plan_race.cpp)
target_link_libraries(run_plan_race PRIVATE qdags::join qdags::query qdags::strategy)
