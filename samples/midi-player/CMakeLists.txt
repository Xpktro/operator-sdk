cmake_minimum_required(VERSION 3.28)
project(midi_player LANGUAGES CXX)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/operator-mode.cmake)

op_add_mode(midi_player TYPE output SOURCES main.cpp)
