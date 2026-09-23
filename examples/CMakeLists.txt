add_executable(example_observe observe_link.cpp)
target_link_libraries(example_observe PRIVATE LinkObservatory::linkobs)
linkobs_apply_warnings(example_observe)

add_executable(example_persist persist_and_restart.cpp)
target_link_libraries(example_persist PRIVATE LinkObservatory::linkobs)
linkobs_apply_warnings(example_persist)

add_executable(example_transport transport_client.cpp)
target_link_libraries(example_transport PRIVATE LinkObservatory::linkobs)
linkobs_apply_warnings(example_transport)

add_test(NAME linkobs.example.observe COMMAND example_observe)
add_test(NAME linkobs.example.persist COMMAND example_persist)
