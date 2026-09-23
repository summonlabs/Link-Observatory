# Independent downstream consumer.
#
# This project is deliberately not part of the Link Observatory build. It exists
# to prove that an installed package can be found and used by a separate CMake
# project through find_package, with no access to the source tree.

cmake_minimum_required(VERSION 3.21)
project(LinkObservatoryDownstream LANGUAGES CXX)

find_package(LinkObservatory 1.0 REQUIRED CONFIG)

add_executable(downstream_consumer main.cpp)
target_link_libraries(downstream_consumer PRIVATE LinkObservatory::linkobs)
target_compile_features(downstream_consumer PRIVATE cxx_std_20)

if(MSVC)
  target_compile_options(downstream_consumer PRIVATE /W4 /WX /permissive- /utf-8)
  target_compile_definitions(downstream_consumer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  target_compile_options(downstream_consumer PRIVATE -Wall -Wextra -Werror)
endif()

enable_testing()
add_test(NAME downstream.smoke COMMAND downstream_consumer)
