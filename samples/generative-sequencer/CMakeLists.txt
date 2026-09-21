cmake_minimum_required(VERSION 3.28)
project(generative_sequencer LANGUAGES CXX)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/operator-mode.cmake)

op_add_mode(generative_sequencer TYPE global SOURCES main.cpp sequencer_engine.cpp)
