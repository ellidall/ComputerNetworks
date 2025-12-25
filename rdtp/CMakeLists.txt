cmake_minimum_required(VERSION 3.22.1)
project(rdtp)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(rdtSender rdtSender.cpp)
add_executable(rdtReceiver rdtReceiver.cpp)
