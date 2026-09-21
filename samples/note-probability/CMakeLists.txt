cmake_minimum_required(VERSION 3.28)
project(note_probability LANGUAGES CXX)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/operator-mode.cmake)

op_add_mode(note_probability TYPE global SOURCES main.cpp)
