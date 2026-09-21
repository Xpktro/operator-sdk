cmake_minimum_required(VERSION 3.28)
project(delay LANGUAGES CXX)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/operator-mode.cmake)

op_add_mode(delay TYPE output SOURCES main.cpp)
