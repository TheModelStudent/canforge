# Standalone project: proves an installed canforge can be consumed with
# find_package() and nothing else. Built by CI against the install tree, never
# by the main build.
cmake_minimum_required(VERSION 3.20)
project(canforge_consumer LANGUAGES CXX)
find_package(canforge REQUIRED COMPONENTS core)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE canforge::core)
