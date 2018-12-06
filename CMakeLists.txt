cmake_minimum_required(VERSION 3.3)
project(x49gp)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")

add_custom_target(x49gp ALL COMMAND make WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
