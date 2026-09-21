cmake_minimum_required(VERSION 3.28)
project(velocity_remap LANGUAGES CXX)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/operator-mode.cmake)

op_add_mode(velocity_remap TYPE output SOURCES main.cpp)
