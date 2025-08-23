# SPDX-License-Identifier: Apache-2.0
# Example local Qt path override (copy to qt_local.cmake and adjust). This file is versioned; the real
# qt_local.cmake is git-ignored so each developer can have custom Qt installations.
# Typical Qt online installer layout (Linux): /opt/Qt/<version>/gcc_64
# Adjust the path below to point CMake to your Qt installation prefix containing lib/cmake/Qt6*.cmake modules.

set(CMAKE_PREFIX_PATH "/opt/Qt/6.8.3/gcc_64" ${CMAKE_PREFIX_PATH})

# Alternative variables you might set instead / additionally:
# set(QT_HOST_PATH "/opt/Qt/6.8.3/gcc_64")
# set(QT_DIR "/opt/Qt/6.8.3/gcc_64/lib/cmake/Qt6")

message(STATUS "qt_local.cmake example loaded (CMAKE_PREFIX_PATH appended). Copy this file to qt_local.cmake to activate.")
