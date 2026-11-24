cmake_minimum_required(VERSION 3.21)
project(OSCQlock VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Widgets Network)
find_package(Threads REQUIRED)

# pxs-qt (OSC, OSCQuery, the parameter tree, the look) is a submodule, and it carries pxs-core itself: nothing else to pin
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pxs-qt/CMakeLists.txt")
    message(FATAL_ERROR "third_party/pxs-qt is empty: run\n  git submodule update --init --recursive")
endif()
set(PXS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/pxs-qt EXCLUDE_FROM_ALL)

add_executable(OSCQlock main.cpp)
target_link_libraries(OSCQlock PRIVATE pxs::params pxs::tree pxs::oscquery pxs::theme Qt6::Widgets)
