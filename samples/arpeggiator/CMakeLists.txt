cmake_minimum_required(VERSION 3.28)
project(arpeggiator LANGUAGES CXX)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/operator-mode.cmake)

op_add_mode(arpeggiator TYPE output SOURCES main.cpp)
