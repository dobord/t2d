cmake_minimum_required(VERSION 3.22) # SPDX-License-Identifier: Apache-2.0
project(t2d LANGUAGES CXX)

# Optional local Qt path override (not versioned). Developers can create qt_local.cmake with:
#   set(CMAKE_PREFIX_PATH "/opt/Qt/6.8.3/gcc_64" ${CMAKE_PREFIX_PATH})
# or adjust QT_HOST_PATH etc. This file is git-ignored.
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/qt_local.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/qt_local.cmake)
endif ()

# Reusable helper: locate a Qt helper executable (e.g. qmlformat, qmlcachegen, qmltyperegistrar)
function (t2d_find_qt_tool OUT_VAR TOOL_NAME)
    if (${OUT_VAR})
        return()
    endif ()
    find_program(
        _tool_bin
        NAMES ${TOOL_NAME}
        HINTS ${CMAKE_PREFIX_PATH}/bin $ENV{CMAKE_PREFIX_PATH}/bin)
    if (NOT _tool_bin)
        foreach (_p IN LISTS CMAKE_PREFIX_PATH)
            if (EXISTS "${_p}/bin/${TOOL_NAME}")
                set(_tool_bin "${_p}/bin/${TOOL_NAME}")
                break()
            endif ()
        endforeach ()
    endif ()
    if (_tool_bin)
        set(${OUT_VAR}
            ${_tool_bin}
            PARENT_SCOPE)
        message(STATUS "Found ${TOOL_NAME}: ${_tool_bin}")
    else ()
        message(
            STATUS
                "${TOOL_NAME} NOT found - optional. Set CMAKE_PREFIX_PATH or provide qt_local.cmake to help locate it."
        )
    endif ()
endfunction ()

option(T2D_ENABLE_SANITIZERS "Enable Address/Undefined sanitizers (Clang/GCC)" OFF)
option(T2D_ENABLE_TSAN "Enable Thread Sanitizer (Clang/GCC)" OFF)
option(T2D_ENABLE_COVERAGE "Enable code coverage instrumentation (GCC/Clang Debug builds)" OFF)
option(T2D_ENABLE_SNAPSHOT_QUANT "Enable snapshot quantization (reduced bandwidth)" ON)
option(T2D_ENABLE_ZLIB "Enable zlib compression for snapshots (optional)" OFF)
option(T2D_ENABLE_PROFILING "Enable lightweight performance instrumentation (timers, counters)" OFF)

# Allow user to downgrade adopted policy set (NOT the required CMake program version) via
# -DCMAKE_POLICY_VERSION_MINIMUM=3.5 (for legacy behavior while still requiring a newer CMake binary).
# By default we adopt the current minimum required version (3.22). Using a lower value may surface
# older/compat modes and is not recommended for production.
set(CMAKE_POLICY_VERSION_MINIMUM
    "3.22"
    CACHE STRING "Policy version to adopt (does not lower required CMake version)")
if (CMAKE_POLICY_VERSION_MINIMUM)
    # Guard: avoid calling with a value higher than running CMake version
    if (CMAKE_POLICY_VERSION_MINIMUM VERSION_LESS_EQUAL ${CMAKE_VERSION})
        cmake_policy(VERSION ${CMAKE_POLICY_VERSION_MINIMUM})
    else ()
        message(
            WARNING
                "Requested CMAKE_POLICY_VERSION_MINIMUM=${CMAKE_POLICY_VERSION_MINIMUM} exceeds running CMake ${CMAKE_VERSION}; ignoring."
        )
    endif ()
endif ()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (T2D_ENABLE_SANITIZERS AND T2D_ENABLE_TSAN)
    message(
        FATAL_ERROR
            "Cannot enable both T2D_ENABLE_SANITIZERS (ASan/UBSan) and T2D_ENABLE_TSAN at the same time"
    )
endif ()

if (T2D_ENABLE_SANITIZERS)
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        message(STATUS "Sanitizers enabled (address,undefined)")
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    else ()
        message(
            WARNING "Sanitizers requested but not supported for compiler ${CMAKE_CXX_COMPILER_ID}")
    endif ()
endif ()

if (T2D_ENABLE_PROFILING)
    message(STATUS "Profiling instrumentation enabled (T2D_ENABLE_PROFILING)")
    # Ensure frame pointers kept & disable some inlining for clearer stacks (only if not already enforced by sanitizers)
    add_compile_options(-fno-omit-frame-pointer -fno-optimize-sibling-calls)
    # Avoid aggressive function merging/inlining of small functions for better attribution
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        add_compile_options($<$<CONFIG:Release>:-fno-inline-functions-called-once>)
    endif ()
endif ()

if (T2D_ENABLE_TSAN)
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        message(STATUS "Thread Sanitizer enabled")
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
        add_link_options(-fsanitize=thread -fno-omit-frame-pointer)
    else ()
        message(
            WARNING
                "Thread Sanitizer requested but not supported for compiler ${CMAKE_CXX_COMPILER_ID}"
        )
    endif ()
endif ()

# Coverage flags (only in Debug to avoid penalizing optimized builds)
if (T2D_ENABLE_COVERAGE)
    if (NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        message(WARNING "T2D_ENABLE_COVERAGE=ON but CMAKE_BUILD_TYPE != Debug; enabling anyway")
    endif ()
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        message(STATUS "Coverage instrumentation enabled (-fprofile-arcs -ftest-coverage)")
        # -fprofile-update=atomic avoids negative counter issues (race conditions) in multi-threaded tests
        add_compile_options(--coverage -fprofile-arcs -ftest-coverage -fprofile-update=atomic)
        add_link_options(--coverage -fprofile-arcs -ftest-coverage)
    else ()
        message(WARNING "Coverage requested but unsupported compiler ${CMAKE_CXX_COMPILER_ID}")
    endif ()
endif ()

option(T2D_BUILD_SERVER "Build the game server" ON)
option(T2D_BUILD_CLIENT "Build the desktop client (prototype; Qt UI later)" OFF)
option(T2D_BUILD_QT_CLIENT "Build the Qt/QML desktop client UI (Linux Qt 6)" OFF)
option(T2D_BUILD_TESTS "Build internal test executables" ON)

# Always enable testing (simplifies CI integration); T2D_BUILD_TESTS controls build of executables
enable_testing()
message(STATUS "Testing enabled (enable_testing())")

## Third-party dependencies via git submodules under third_party
## We intentionally avoid FetchContent here to pin revisions through submodule SHAs
## and enable reproducible builds & offline CI.

## Force-disable third_party tests/examples/tools/docs for lean build
set(YAML_CPP_BUILD_TESTS
    OFF
    CACHE BOOL "yaml-cpp tests disabled" FORCE)
set(YAML_CPP_BUILD_TOOLS
    OFF
    CACHE BOOL "yaml-cpp tools disabled" FORCE)
set(YAML_CPP_BUILD_CONTRIB
    ON
    CACHE BOOL "yaml-cpp contrib enabled" FORCE)
set(YAML_CPP_INSTALL
    OFF
    CACHE BOOL "yaml-cpp install targets disabled" FORCE)

set(LIBCORO_BUILD_TESTS
    OFF
    CACHE BOOL "libcoro tests disabled" FORCE)
set(LIBCORO_BUILD_EXAMPLES
    OFF
    CACHE BOOL "libcoro examples disabled" FORCE)
set(LIBCORO_FEATURE_TLS
    OFF
    CACHE BOOL "libcoro TLS disabled for now" FORCE)
set(CARES_BUILD_TOOLS
    OFF
    CACHE BOOL "c-ares tools disabled" FORCE)

set(BOX2D_UNIT_TESTS
    OFF
    CACHE BOOL "box2d unit tests disabled" FORCE)
set(BOX2D_SAMPLES
    OFF
    CACHE BOOL "box2d samples disabled" FORCE)
set(BOX2D_BENCHMARKS
    OFF
    CACHE BOOL "box2d benchmarks disabled" FORCE)
set(BOX2D_DOCS
    OFF
    CACHE BOOL "box2d docs disabled" FORCE)

add_subdirectory(third_party/yaml-cpp)
add_subdirectory(third_party/libcoro)
add_subdirectory(third_party/box2d)

find_package(Protobuf REQUIRED)
message(STATUS "Found Protobuf ${Protobuf_VERSION}")

# Proto generation
file(GLOB_RECURSE T2D_PROTO_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/proto/*.proto)
protobuf_generate_cpp(T2D_PROTO_SRCS T2D_PROTO_HDRS ${T2D_PROTO_FILES})

add_library(t2d_proto STATIC ${T2D_PROTO_SRCS} ${T2D_PROTO_HDRS})
target_include_directories(t2d_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(t2d_proto PUBLIC protobuf::libprotobuf)

# Derive version from git (fallback to 0.0.0) and pass to targets.
# Expose: T2D_VERSION (describe), T2D_GIT_SHA (short), T2D_BUILD_DIRTY (0/1), T2D_BUILD_DATE (UTC ISO8601)
find_package(Git QUIET)
if (GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --dirty --always
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_DESCRIBE_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} diff --quiet --name-only
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        RESULT_VARIABLE GIT_DIFF_RESULT
        OUTPUT_QUIET ERROR_QUIET)
endif ()
if (NOT GIT_DESCRIBE_VERSION)
    set(GIT_DESCRIBE_VERSION "0.0.0-unknown")
endif ()
if (NOT GIT_SHA)
    set(GIT_SHA "unknown")
endif ()
if (GIT_DIFF_RESULT EQUAL 0)
    set(GIT_DIRTY 0)
else ()
    # If describe already contains -dirty keep that semantic but also numeric flag
    set(GIT_DIRTY 1)
endif ()
string(TIMESTAMP BUILD_TIMESTAMP "%Y-%m-%dT%H:%M:%SZ" UTC)
message(
    STATUS
        "T2D version: ${GIT_DESCRIBE_VERSION} (sha: ${GIT_SHA} dirty: ${GIT_DIRTY} date: ${BUILD_TIMESTAMP})"
)
set(T2D_VERSION_STR ${GIT_DESCRIBE_VERSION})

# Central interface target for version compile definitions so every binary / test shares consistent values
add_library(t2d_version INTERFACE)
target_compile_definitions(
    t2d_version INTERFACE T2D_VERSION="${T2D_VERSION_STR}" T2D_GIT_SHA="${GIT_SHA}"
                          T2D_BUILD_DIRTY=${GIT_DIRTY} T2D_BUILD_DATE="${BUILD_TIMESTAMP}")

# Central profiling interface target (adds macro when enabled)
add_library(t2d_profiling INTERFACE)
if (T2D_ENABLE_PROFILING)
    target_compile_definitions(t2d_profiling INTERFACE T2D_PROFILING_ENABLED=1)
else ()
    target_compile_definitions(t2d_profiling INTERFACE T2D_PROFILING_ENABLED=0)
endif ()

if (T2D_BUILD_SERVER)
    add_executable(
        t2d_server
        src/common/framing.cpp
        src/server/game/match.cpp
        src/server/game/physics.cpp
        src/server/game/snapshot_compress.cpp
        src/server/main.cpp
        src/server/matchmaking/matchmaker.cpp
        src/server/matchmaking/session_manager.cpp
        src/server/net/listener.cpp
        src/server/net/metrics_http.cpp)
    # auth provider source
    target_sources(t2d_server PRIVATE src/server/auth/auth_provider.cpp)
    target_include_directories(t2d_server PRIVATE src)
    target_link_libraries(t2d_server PRIVATE t2d_proto yaml-cpp libcoro box2d)
    if (T2D_ENABLE_ZLIB)
        find_package(ZLIB REQUIRED)
        target_link_libraries(t2d_server PRIVATE ZLIB::ZLIB)
        target_compile_definitions(t2d_server PRIVATE T2D_HAS_ZLIB=1)
    endif ()
    target_link_libraries(t2d_server PRIVATE t2d_version t2d_profiling)
endif ()

if (T2D_BUILD_CLIENT)
    if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(
            STATUS
                "Desktop client build requested but disabled on non-Linux platform (${CMAKE_SYSTEM_NAME}); skipping target"
        )
    else ()
        # Prototype desktop client (no UI yet). Later this will switch to a Qt/QML target.
        add_executable(t2d_desktop_client src/client/desktop_client.cpp src/common/framing.cpp)
        target_link_libraries(t2d_desktop_client PRIVATE t2d_proto libcoro)
        target_include_directories(t2d_desktop_client PRIVATE src)
        target_link_libraries(t2d_desktop_client PRIVATE t2d_version t2d_profiling)
    endif ()
endif ()

if (T2D_BUILD_QT_CLIENT)
    if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(
            STATUS
                "Qt client requested but disabled on non-Linux platform (${CMAKE_SYSTEM_NAME}); skipping target"
        )
    else ()
        find_package(Qt6 QUIET COMPONENTS Quick Qml Gui Core)
        if (NOT Qt6_FOUND)
            message(
                WARNING
                    "Qt6 not found; disable T2D_BUILD_QT_CLIENT or install Qt6 (>=6.5) to build UI client"
            )
        else ()
            message(STATUS "Building Qt/QML client (Qt6)")
            set(CMAKE_AUTOMOC ON)
            set(CMAKE_AUTORCC ON)
            set(CMAKE_AUTOUIC ON)
            add_executable(
                t2d_qt_client
                src/client/qt/ammo_box_model.cpp
                src/client/qt/crate_model.cpp
                src/client/qt/entity_model.cpp
                src/client/qt/input_state.cpp
                src/client/qt/lobby_state.cpp
                src/client/qt/projectile_model.cpp
                src/client/qt/qt_client.cpp
                src/client/qt/timing_state.hpp)
            target_include_directories(t2d_qt_client PRIVATE src src/client/qt)
            target_link_libraries(
                t2d_qt_client
                PRIVATE Qt6::Quick
                        Qt6::Qml
                        Qt6::Gui
                        Qt6::Core
                        t2d_proto
                        libcoro)
            target_link_libraries(t2d_qt_client PRIVATE t2d_version t2d_profiling)
            # QML module & resources (minimal)
            set(T2D_QML_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/client/qt/qml)
            if (EXISTS ${T2D_QML_DIR})
                # Use qt_add_qml_module if available (Qt6)
                if (COMMAND qt_add_qml_module)
                    qt_add_qml_module(
                        t2d_qt_client
                        URI
                        T2DClient
                        VERSION
                        1.0
                        QML_FILES
                        src/client/qt/qml/Main.qml
                        src/client/qt/qml/Joystick.qml
                        src/client/qt/qml/CustomButton.qml)
                else ()
                    message(WARNING "qt_add_qml_module not available; QML files not embedded")
                endif ()
            endif ()
        endif ()
    endif ()
endif ()

# Formatting target placeholder (user can integrate clang-format config later)
find_program(CLANG_FORMAT_BIN clang-format)
find_program(CMAKE_FORMAT_BIN cmake-format)
if (CLANG_FORMAT_BIN)
    # Collect only first-party sources (exclude third_party explicitly)
    # Restrict formatting strictly to first-party source roots; do NOT recurse outside these
    set(T2D_FORMAT_DIRS src tests)
    set(ALL_CXX_SOURCE_FILES)
    foreach (_d IN LISTS T2D_FORMAT_DIRS)
        if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${_d})
            file(
                GLOB_RECURSE
                _cur
                CONFIGURE_DEPENDS
                ${_d}/*.cpp
                ${_d}/*.hpp
                ${_d}/*.h
                ${_d}/*.cc
                ${_d}/*.cxx)
            list(APPEND ALL_CXX_SOURCE_FILES ${_cur})
        endif ()
    endforeach ()
    # Remove any accidentally captured third_party paths (safety net)
    list(
        FILTER
        ALL_CXX_SOURCE_FILES
        EXCLUDE
        REGEX
        ".*/third_party/.*")
    # Avoid target name collision if a third_party submodule already defined 'format'
    if (NOT TARGET format)
        add_custom_target(
            format
            COMMAND ${CLANG_FORMAT_BIN} -i ${ALL_CXX_SOURCE_FILES}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Formatting C/C++ sources with clang-format (project sources only)")
        set(T2D_PRIMARY_FORMAT_TARGET format)
    else ()
        # A third_party dependency already defined 'format'; create dedicated project target
        if (NOT TARGET t2d_format)
            add_custom_target(
                t2d_format
                COMMAND ${CLANG_FORMAT_BIN} -i ${ALL_CXX_SOURCE_FILES}
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                COMMENT
                    "Formatting t2d C/C++ sources with clang-format (avoids third_party 'format')")
            message(
                STATUS
                    "Third-party target 'format' detected; using 't2d_format' for project sources.")
        endif ()
        set(T2D_PRIMARY_FORMAT_TARGET t2d_format)
    endif ()
endif ()

# CMake files formatting (optional, requires cmake-format)
if (CMAKE_FORMAT_BIN)
    # Restrict formatting to root CMakeLists.txt to avoid third_party/vendor templates
    set(ALL_CMAKE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt)
    if (NOT TARGET format_cmake)
        add_custom_target(
            format_cmake
            COMMAND ${CMAKE_FORMAT_BIN} -i ${ALL_CMAKE_FILES}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Formatting CMake files with cmake-format")
    endif ()
endif ()

# QML files formatting (optional, requires qmlformat from Qt)
t2d_find_qt_tool(QMLFORMAT_BIN qmlformat)
if (QMLFORMAT_BIN)
    file(
        GLOB_RECURSE ALL_QML_FILES
        RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/qt/qml/*.qml)
    # Exclude any third_party paths just in case
    list(
        FILTER
        ALL_QML_FILES
        EXCLUDE
        REGEX
        ".*/third_party/.*")
    # Filter out empty QML files (qmlformat fails to parse empty input)
    set(_non_empty_qml)
    foreach (_qml IN LISTS ALL_QML_FILES)
        file(SIZE "${CMAKE_CURRENT_SOURCE_DIR}/${_qml}" _qml_size)
        if (_qml_size GREATER 0)
            list(APPEND _non_empty_qml ${_qml})
        else ()
            message(STATUS "Skipping empty QML file (no formatting): ${_qml}")
        endif ()
    endforeach ()
    set(ALL_QML_FILES ${_non_empty_qml})
    if (ALL_QML_FILES)
        if (NOT TARGET format_qml)
            add_custom_target(
                format_qml
                COMMAND ${QMLFORMAT_BIN} --inplace ${ALL_QML_FILES}
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                COMMENT "Formatting QML files with qmlformat")
        endif ()
    endif ()
endif ()

# Shell scripts formatting (optional, requires shfmt)
find_program(SHFMT_BIN shfmt)
if (SHFMT_BIN)
    # Collect first-party shell scripts (root *.sh + scripts/**/*.sh) excluding third_party
    set(ALL_SH_FILES)
    file(
        GLOB_RECURSE _root_sh
        RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/*.sh)
    list(APPEND ALL_SH_FILES ${_root_sh})
    if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/scripts)
        file(
            GLOB_RECURSE _scripts_sh
            RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/*.sh)
        list(APPEND ALL_SH_FILES ${_scripts_sh})
    endif ()
    # Filter out any accidental third_party scripts
    list(
        FILTER
        ALL_SH_FILES
        EXCLUDE
        REGEX
        ".*/third_party/.*")
    if (ALL_SH_FILES AND NOT TARGET format_sh)
        add_custom_target(
            format_sh
            COMMAND ${SHFMT_BIN} -w ${ALL_SH_FILES}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Formatting shell scripts with shfmt")
    endif ()
endif ()

# Protobuf formatting (optional, requires buf)
find_program(BUF_BIN buf)
if (BUF_BIN)
    # Only create target if proto directory exists
    if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/proto)
        if (NOT TARGET format_proto)
            add_custom_target(
                format_proto
                COMMAND ${BUF_BIN} format -w
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                COMMENT "Formatting protobuf files with buf format")
        endif ()
    endif ()
endif ()

# Aggregate formatting target (format_all) to run all available formatting steps (C++, CMake, QML)
set(_fmt_targets)
if (TARGET t2d_format)
    list(APPEND _fmt_targets t2d_format)
elseif (TARGET format)
    # Only append generic 'format' if we did not create t2d_format (i.e. it's project-owned)
    list(APPEND _fmt_targets format)
endif ()
if (TARGET format_cmake)
    list(APPEND _fmt_targets format_cmake)
endif ()
if (TARGET format_qml)
    list(APPEND _fmt_targets format_qml)
endif ()
if (TARGET format_sh)
    list(APPEND _fmt_targets format_sh)
endif ()
if (TARGET format_proto)
    list(APPEND _fmt_targets format_proto)
endif ()
list(LENGTH _fmt_targets _fmt_len)
if (_fmt_len GREATER 0 AND NOT TARGET format_all)
    # Provide a unified target even if only one underlying formatter exists
    add_custom_target(format_all DEPENDS ${_fmt_targets})
endif ()

add_executable(t2d_test_client src/client/test_client.cpp src/common/framing.cpp)
target_link_libraries(t2d_test_client PRIVATE t2d_proto libcoro t2d_version t2d_profiling)
target_include_directories(t2d_test_client PRIVATE src)

if (T2D_BUILD_TESTS)
    add_executable(
        t2d_unit_session_manager src/common/framing.cpp src/server/matchmaking/session_manager.cpp
                                 tests/unit_session_manager.cpp)
    target_link_libraries(t2d_unit_session_manager PRIVATE t2d_proto libcoro yaml-cpp)
    target_include_directories(t2d_unit_session_manager PRIVATE src)
    target_link_libraries(t2d_unit_session_manager PRIVATE t2d_version t2d_profiling)

    add_executable(t2d_unit_framing src/common/framing.cpp tests/unit_framing.cpp)
    target_link_libraries(t2d_unit_framing PRIVATE t2d_proto libcoro)
    target_include_directories(t2d_unit_framing PRIVATE src)
    target_link_libraries(t2d_unit_framing PRIVATE t2d_version t2d_profiling)

    add_executable(t2d_unit_heartbeat_timeout src/server/matchmaking/session_manager.cpp
                                              tests/unit_heartbeat_timeout.cpp)
    target_link_libraries(t2d_unit_heartbeat_timeout PRIVATE t2d_proto libcoro yaml-cpp)
    target_include_directories(t2d_unit_heartbeat_timeout PRIVATE src)
    target_link_libraries(t2d_unit_heartbeat_timeout PRIVATE t2d_version t2d_profiling)

    add_executable(t2d_unit_snapshot_delta tests/unit_snapshot_delta.cpp)
    target_link_libraries(t2d_unit_snapshot_delta PRIVATE t2d_proto)
    target_include_directories(t2d_unit_snapshot_delta PRIVATE src)
    target_link_libraries(t2d_unit_snapshot_delta PRIVATE t2d_version t2d_profiling)
    add_executable(t2d_unit_snapshot_replay tests/unit_snapshot_replay.cpp)
    target_link_libraries(t2d_unit_snapshot_replay PRIVATE t2d_proto)
    target_include_directories(t2d_unit_snapshot_replay PRIVATE src)
    target_link_libraries(t2d_unit_snapshot_replay PRIVATE t2d_version t2d_profiling)
    add_executable(t2d_unit_framing_fuzz tests/unit_framing_fuzz.cpp)
    target_include_directories(t2d_unit_framing_fuzz PRIVATE src)
    target_link_libraries(t2d_unit_framing_fuzz PRIVATE t2d_version t2d_profiling)

    add_executable(
        t2d_e2e_match_start
        src/common/framing.cpp
        src/server/auth/auth_provider.cpp
        src/server/game/match.cpp
        src/server/game/physics.cpp
        src/server/matchmaking/matchmaker.cpp
        src/server/matchmaking/session_manager.cpp
        src/server/net/listener.cpp
        tests/e2e_match_start.cpp)
    target_link_libraries(t2d_e2e_match_start PRIVATE t2d_proto libcoro yaml-cpp box2d)
    target_include_directories(t2d_e2e_match_start PRIVATE src)
    target_link_libraries(t2d_e2e_match_start PRIVATE t2d_version t2d_profiling)

    add_executable(
        t2d_e2e_input_move
        src/common/framing.cpp
        src/server/auth/auth_provider.cpp
        src/server/game/match.cpp
        src/server/game/physics.cpp
        src/server/matchmaking/matchmaker.cpp
        src/server/matchmaking/session_manager.cpp
        src/server/net/listener.cpp
        tests/e2e_input_move.cpp)
    target_link_libraries(t2d_e2e_input_move PRIVATE t2d_proto libcoro yaml-cpp box2d)
    target_include_directories(t2d_e2e_input_move PRIVATE src)
    target_link_libraries(t2d_e2e_input_move PRIVATE t2d_version t2d_profiling)

    add_executable(
        t2d_e2e_heartbeat
        src/common/framing.cpp
        src/server/auth/auth_provider.cpp
        src/server/game/match.cpp
        src/server/game/physics.cpp
        src/server/matchmaking/matchmaker.cpp
        src/server/matchmaking/session_manager.cpp
        src/server/net/listener.cpp
        tests/e2e_heartbeat.cpp)
    target_link_libraries(t2d_e2e_heartbeat PRIVATE t2d_proto libcoro yaml-cpp box2d)
    target_include_directories(t2d_e2e_heartbeat PRIVATE src)
    target_link_libraries(t2d_e2e_heartbeat PRIVATE t2d_version t2d_profiling)

    add_executable(
        t2d_e2e_bot_fill
        src/common/framing.cpp
        src/server/auth/auth_provider.cpp
        src/server/game/match.cpp
        src/server/game/physics.cpp
        src/server/matchmaking/matchmaker.cpp
        src/server/matchmaking/session_manager.cpp
        src/server/net/listener.cpp
        tests/e2e_bot_fill.cpp)
    target_link_libraries(t2d_e2e_bot_fill PRIVATE t2d_proto libcoro yaml-cpp box2d)
    target_include_directories(t2d_e2e_bot_fill PRIVATE src)
    target_link_libraries(t2d_e2e_bot_fill PRIVATE t2d_version t2d_profiling)
    add_executable(
        t2d_e2e_bot_projectile
        src/common/framing.cpp
        src/server/auth/auth_provider.cpp
        src/server/game/match.cpp
        src/server/game/physics.cpp
        src/server/matchmaking/matchmaker.cpp
        src/server/matchmaking/session_manager.cpp
        src/server/net/listener.cpp
        tests/e2e_bot_projectile.cpp)
    target_link_libraries(t2d_e2e_bot_projectile PRIVATE t2d_proto libcoro yaml-cpp box2d)
    target_include_directories(t2d_e2e_bot_projectile PRIVATE src)
    target_link_libraries(t2d_e2e_bot_projectile PRIVATE t2d_version t2d_profiling)
    add_executable(
        t2d_e2e_delta_snapshots
        src/common/framing.cpp
        src/server/auth/auth_provider.cpp
        src/server/game/match.cpp
        src/server/game/physics.cpp
        src/server/matchmaking/matchmaker.cpp
        src/server/matchmaking/session_manager.cpp
        src/server/net/listener.cpp
        tests/e2e_delta_snapshots.cpp)
    target_link_libraries(t2d_e2e_delta_snapshots PRIVATE t2d_proto libcoro yaml-cpp box2d)
    target_include_directories(t2d_e2e_delta_snapshots PRIVATE src)
    target_link_libraries(t2d_e2e_delta_snapshots PRIVATE t2d_version t2d_profiling)
    add_executable(
        t2d_e2e_damage_event
        src/common/framing.cpp
        src/server/auth/auth_provider.cpp
        src/server/game/match.cpp
        src/server/game/physics.cpp
        src/server/matchmaking/matchmaker.cpp
        src/server/matchmaking/session_manager.cpp
        src/server/net/listener.cpp
        tests/e2e_damage_event.cpp)
    target_link_libraries(t2d_e2e_damage_event PRIVATE t2d_proto libcoro yaml-cpp box2d)
    target_include_directories(t2d_e2e_damage_event PRIVATE src)
    target_link_libraries(t2d_e2e_damage_event PRIVATE t2d_version t2d_profiling)

    add_executable(
        t2d_e2e_damage_multi
        src/common/framing.cpp
        src/server/auth/auth_provider.cpp
        src/server/game/match.cpp
        src/server/game/physics.cpp
        src/server/matchmaking/matchmaker.cpp
        src/server/matchmaking/session_manager.cpp
        src/server/net/listener.cpp
        tests/e2e_damage_multi.cpp)
    target_link_libraries(t2d_e2e_damage_multi PRIVATE t2d_proto libcoro yaml-cpp box2d)
    target_include_directories(t2d_e2e_damage_multi PRIVATE src)
    target_link_libraries(t2d_e2e_damage_multi PRIVATE t2d_version t2d_profiling)

    add_executable(
        t2d_e2e_kill_feed
        src/common/framing.cpp
        src/server/auth/auth_provider.cpp
        src/server/game/match.cpp
        src/server/game/physics.cpp
        src/server/matchmaking/matchmaker.cpp
        src/server/matchmaking/session_manager.cpp
        src/server/net/listener.cpp
        tests/e2e_kill_feed.cpp)
    target_link_libraries(t2d_e2e_kill_feed PRIVATE t2d_proto libcoro yaml-cpp box2d)
    target_include_directories(t2d_e2e_kill_feed PRIVATE src)
    target_link_libraries(t2d_e2e_kill_feed PRIVATE t2d_version t2d_profiling)

    # Register tests with CTest (only if BUILD_TESTING enabled)
    set(T2D_TEST_TARGETS
        t2d_unit_session_manager
        t2d_unit_framing
        t2d_unit_heartbeat_timeout
        t2d_unit_snapshot_delta
        t2d_unit_snapshot_replay
        t2d_unit_framing_fuzz
        t2d_e2e_match_start
        t2d_e2e_input_move
        t2d_e2e_heartbeat
        t2d_e2e_bot_fill
        t2d_e2e_bot_projectile
        t2d_e2e_delta_snapshots
        t2d_e2e_damage_event
        t2d_e2e_damage_multi
        t2d_e2e_kill_feed)
    foreach (_t IN LISTS T2D_TEST_TARGETS)
        add_test(NAME ${_t} COMMAND ${_t})
        set_tests_properties(${_t} PROPERTIES TIMEOUT 20)
    endforeach ()
    add_custom_target(t2d_tests DEPENDS ${T2D_TEST_TARGETS})
endif ()
