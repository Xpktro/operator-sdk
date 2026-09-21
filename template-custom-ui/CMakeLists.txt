cmake_minimum_required(VERSION 3.28)
project(my_ui_mode LANGUAGES CXX)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/operator-mode.cmake)

op_add_mode(my_ui_mode TYPE global FLAGS FULLSCREEN_UI SOURCES main.cpp)
