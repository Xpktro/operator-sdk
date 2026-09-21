cmake_minimum_required(VERSION 3.28)
project(my_mode LANGUAGES CXX)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/operator-mode.cmake)

op_add_mode(my_mode TYPE global SOURCES main.cpp)
