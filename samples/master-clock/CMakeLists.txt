cmake_minimum_required(VERSION 3.28)
project(master_clock LANGUAGES CXX)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/operator-mode.cmake)

op_add_mode(master_clock TYPE global FLAGS GENERATES_CLOCK SOURCES main.cpp)
