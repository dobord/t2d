# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.22.1)
project(t2d_native LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(t2d_native SHARED native_lib.cpp)

# Placeholder: later link core client logic

find_library(log-lib log)

target_link_libraries(t2d_native PRIVATE ${log-lib})
