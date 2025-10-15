cmake_minimum_required(VERSION 3.22.1)
project(socket)

set(CMAKE_CXX_STANDARD 20)

add_executable(client client.cpp)
add_executable(server server.cpp)
