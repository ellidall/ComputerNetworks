cmake_minimum_required(VERSION 3.22.1)
project(smtp)

set(CMAKE_CXX_STANDARD 20)

find_package(OpenSSL REQUIRED)

add_executable(smtp smtp.cpp)
target_link_libraries(smtp OpenSSL::SSL OpenSSL::Crypto)
