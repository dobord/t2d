# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.22.1)
project(t2d_native LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(t2d_native SHARED native_lib.cpp)

# Placeholder for integrating shared proto (reuse generated game.pb.* from root build if present)
set(ROOT_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../../../..)
if (EXISTS ${ROOT_DIR}/build/game.pb.cc)
	message(STATUS "Including generated proto sources from root build")
	target_sources(t2d_native PRIVATE ${ROOT_DIR}/build/game.pb.cc)
	target_include_directories(t2d_native PRIVATE ${ROOT_DIR}/build)
endif()

# Prefer Android-specific lite generated proto if present (overrides full runtime inclusion above)
set(ANDROID_LITE_PROTO_DIR ${ROOT_DIR}/android/proto/cpp)
if (EXISTS ${ANDROID_LITE_PROTO_DIR}/game.pb.cc)
	message(STATUS "Using Android lite protobuf sources")
	target_sources(t2d_native PRIVATE ${ANDROID_LITE_PROTO_DIR}/game.pb.cc)
	target_include_directories(t2d_native PRIVATE ${ANDROID_LITE_PROTO_DIR})
	target_compile_definitions(t2d_native PRIVATE T2D_PROTO_LITE=1)
endif()

# Placeholder: later link core client logic

find_library(log-lib log)

target_link_libraries(t2d_native PRIVATE ${log-lib})
