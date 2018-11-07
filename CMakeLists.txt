cmake_minimum_required(VERSION 3.12)
project(dapper)

set(CMAKE_CXX_STANDARD 14)

include_directories(rapidjson)

add_executable(dapper
        dapper.cpp)

add_executable(dapperc
        dapperc.cpp)
