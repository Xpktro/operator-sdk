cmake_minimum_required(VERSION 3.28)
project(user_scale_global LANGUAGES CXX)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/operator-mode.cmake)

op_add_mode(user_scale_global TYPE global FLAGS FULLSCREEN_UI OPM_NAME user_scale SOURCES main.cpp)
