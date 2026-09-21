cmake_minimum_required(VERSION 3.28)
project(cc_lfo LANGUAGES CXX)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/operator-mode.cmake)

op_add_mode(cc_lfo TYPE output SOURCES main.cpp)
