add_executable(linkobs_bench bench_ingest.cpp)
target_link_libraries(linkobs_bench PRIVATE LinkObservatory::linkobs)
linkobs_apply_warnings(linkobs_bench)

add_test(NAME linkobs.bench COMMAND linkobs_bench --records 20000 --links 16 --sources 2)
