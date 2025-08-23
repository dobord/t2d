cmake_minimum_required(VERSION 3.22)
project(t2d LANGUAGES CXX)

option(T2D_ENABLE_SANITIZERS "Enable Address/Undefined sanitizers (Clang/GCC)" OFF)

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

option(T2D_BUILD_SERVER "Build the game server" ON)
option(T2D_BUILD_CLIENT "Build the desktop client (prototype; Qt UI later)" OFF)
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

# Derive version from git (fallback to 0.0.0) and pass to targets
find_package(Git QUIET)
if (GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --dirty --always
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_DESCRIBE_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif ()
if (NOT GIT_DESCRIBE_VERSION)
    set(GIT_DESCRIBE_VERSION "0.0.0-unknown")
endif ()
message(STATUS "T2D version: ${GIT_DESCRIBE_VERSION}")
set(T2D_VERSION_STR ${GIT_DESCRIBE_VERSION})

if (T2D_BUILD_SERVER)
    add_executable(
        t2d_server
        src/common/framing.cpp
        src/server/game/match.cpp
        src/server/main.cpp
        src/server/matchmaking/matchmaker.cpp
        src/server/matchmaking/session_manager.cpp
        src/server/net/listener.cpp
        src/server/net/metrics_http.cpp)
    # auth provider source
    target_sources(t2d_server PRIVATE src/server/auth/auth_provider.cpp)
    target_include_directories(t2d_server PRIVATE src)
    target_link_libraries(t2d_server PRIVATE t2d_proto yaml-cpp libcoro box2d)
    target_compile_definitions(t2d_server PRIVATE T2D_VERSION="${T2D_VERSION_STR}")
endif ()

if (T2D_BUILD_CLIENT)
    # Prototype desktop client (no UI yet). Later this will switch to a Qt/QML target.
    add_executable(t2d_desktop_client src/client/desktop_client.cpp src/common/framing.cpp)
    target_link_libraries(t2d_desktop_client PRIVATE t2d_proto libcoro)
    target_include_directories(t2d_desktop_client PRIVATE src)
    target_compile_definitions(t2d_desktop_client PRIVATE T2D_VERSION="${T2D_VERSION_STR}")
endif ()

# Formatting target placeholder (user can integrate clang-format config later)
find_program(CLANG_FORMAT_BIN clang-format)
find_program(CMAKE_FORMAT_BIN cmake-format)
if (CLANG_FORMAT_BIN)
    # Collect only first-party sources (exclude third_party explicitly)
    set(T2D_FORMAT_DIRS src include tests)
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
    # Defensive filter in case a future path adds third_party via symlink
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
            COMMENT "Formatting C/C++ sources with clang-format")
    else ()
        add_custom_target(
            t2d_format
            COMMAND ${CLANG_FORMAT_BIN} -i ${ALL_CXX_SOURCE_FILES}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT
                "Formatting t2d C/C++ sources with clang-format (dependency already defines 'format')"
        )
        message(
            STATUS
                "Dependency already defines target 'format'; added 't2d_format' for project sources."
        )
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

# Aggregate formatting target (format_all) to run both C++ and CMake formatting
if (TARGET format_cmake)
    if (TARGET format AND NOT TARGET format_all)
        add_custom_target(format_all DEPENDS format format_cmake)
    elseif (TARGET t2d_format AND NOT TARGET format_all)
        add_custom_target(format_all DEPENDS t2d_format format_cmake)
    endif ()
endif ()

add_executable(t2d_test_client src/client/test_client.cpp src/common/framing.cpp)
target_link_libraries(t2d_test_client PRIVATE t2d_proto libcoro)
target_include_directories(t2d_test_client PRIVATE src)

if (T2D_BUILD_TESTS)
    add_executable(
        t2d_unit_session_manager src/common/framing.cpp src/server/matchmaking/session_manager.cpp
                                 tests/unit_session_manager.cpp)
    target_link_libraries(t2d_unit_session_manager PRIVATE t2d_proto libcoro yaml-cpp)
    target_include_directories(t2d_unit_session_manager PRIVATE src)
    target_compile_definitions(t2d_unit_session_manager PRIVATE T2D_VERSION="${T2D_VERSION_STR}")

    add_executable(t2d_unit_framing src/common/framing.cpp tests/unit_framing.cpp)
    target_link_libraries(t2d_unit_framing PRIVATE t2d_proto libcoro)
    target_include_directories(t2d_unit_framing PRIVATE src)
    target_compile_definitions(t2d_unit_framing PRIVATE T2D_VERSION="${T2D_VERSION_STR}")

    add_executable(t2d_unit_heartbeat_timeout src/server/matchmaking/session_manager.cpp
                                              tests/unit_heartbeat_timeout.cpp)
    target_link_libraries(t2d_unit_heartbeat_timeout PRIVATE t2d_proto libcoro yaml-cpp)
    target_include_directories(t2d_unit_heartbeat_timeout PRIVATE src)
    target_compile_definitions(t2d_unit_heartbeat_timeout PRIVATE T2D_VERSION="${T2D_VERSION_STR}")

    add_executable(
        t2d_e2e_match_start
        src/common/framing.cpp
        src/server/auth/auth_provider.cpp
        src/server/game/match.cpp
        src/server/matchmaking/matchmaker.cpp
        src/server/matchmaking/session_manager.cpp
        src/server/net/listener.cpp
        tests/e2e_match_start.cpp)
    target_link_libraries(t2d_e2e_match_start PRIVATE t2d_proto libcoro yaml-cpp)
    target_include_directories(t2d_e2e_match_start PRIVATE src)
    target_compile_definitions(t2d_e2e_match_start PRIVATE T2D_VERSION="${T2D_VERSION_STR}")

    add_executable(
        t2d_e2e_input_move
        src/common/framing.cpp
        src/server/auth/auth_provider.cpp
        src/server/game/match.cpp
        src/server/matchmaking/matchmaker.cpp
        src/server/matchmaking/session_manager.cpp
        src/server/net/listener.cpp
        tests/e2e_input_move.cpp)
    target_link_libraries(t2d_e2e_input_move PRIVATE t2d_proto libcoro yaml-cpp)
    target_include_directories(t2d_e2e_input_move PRIVATE src)
    target_compile_definitions(t2d_e2e_input_move PRIVATE T2D_VERSION="${T2D_VERSION_STR}")

    add_executable(
        t2d_e2e_heartbeat
        src/common/framing.cpp
        src/server/auth/auth_provider.cpp
        src/server/game/match.cpp
        src/server/matchmaking/matchmaker.cpp
        src/server/matchmaking/session_manager.cpp
        src/server/net/listener.cpp
        tests/e2e_heartbeat.cpp)
    target_link_libraries(t2d_e2e_heartbeat PRIVATE t2d_proto libcoro yaml-cpp)
    target_include_directories(t2d_e2e_heartbeat PRIVATE src)
    target_compile_definitions(t2d_e2e_heartbeat PRIVATE T2D_VERSION="${T2D_VERSION_STR}")

    add_executable(
        t2d_e2e_bot_fill
        src/common/framing.cpp
        src/server/auth/auth_provider.cpp
        src/server/game/match.cpp
        src/server/matchmaking/matchmaker.cpp
        src/server/matchmaking/session_manager.cpp
        src/server/net/listener.cpp
        tests/e2e_bot_fill.cpp)
    target_link_libraries(t2d_e2e_bot_fill PRIVATE t2d_proto libcoro yaml-cpp)
    target_include_directories(t2d_e2e_bot_fill PRIVATE src)
    target_compile_definitions(t2d_e2e_bot_fill PRIVATE T2D_VERSION="${T2D_VERSION_STR}")
    add_executable(
        t2d_e2e_bot_projectile
        src/common/framing.cpp
        src/server/auth/auth_provider.cpp
        src/server/game/match.cpp
        src/server/matchmaking/matchmaker.cpp
        src/server/matchmaking/session_manager.cpp
        src/server/net/listener.cpp
        tests/e2e_bot_projectile.cpp)
    target_link_libraries(t2d_e2e_bot_projectile PRIVATE t2d_proto libcoro yaml-cpp)
    target_include_directories(t2d_e2e_bot_projectile PRIVATE src)
    target_compile_definitions(t2d_e2e_bot_projectile PRIVATE T2D_VERSION="${T2D_VERSION_STR}")
    add_executable(
        t2d_e2e_delta_snapshots
        src/common/framing.cpp
        src/server/auth/auth_provider.cpp
        src/server/game/match.cpp
        src/server/matchmaking/matchmaker.cpp
        src/server/matchmaking/session_manager.cpp
        src/server/net/listener.cpp
        tests/e2e_delta_snapshots.cpp)
    target_link_libraries(t2d_e2e_delta_snapshots PRIVATE t2d_proto libcoro yaml-cpp)
    target_include_directories(t2d_e2e_delta_snapshots PRIVATE src)
    target_compile_definitions(t2d_e2e_delta_snapshots PRIVATE T2D_VERSION="${T2D_VERSION_STR}")
    add_executable(
        t2d_e2e_damage_event
        src/common/framing.cpp
        src/server/auth/auth_provider.cpp
        src/server/game/match.cpp
        src/server/matchmaking/matchmaker.cpp
        src/server/matchmaking/session_manager.cpp
        src/server/net/listener.cpp
        tests/e2e_damage_event.cpp)
    target_link_libraries(t2d_e2e_damage_event PRIVATE t2d_proto libcoro yaml-cpp)
    target_include_directories(t2d_e2e_damage_event PRIVATE src)
    target_compile_definitions(t2d_e2e_damage_event PRIVATE T2D_VERSION="${T2D_VERSION_STR}")

    add_executable(
        t2d_e2e_damage_multi
        src/common/framing.cpp
        src/server/auth/auth_provider.cpp
        src/server/game/match.cpp
        src/server/matchmaking/matchmaker.cpp
        src/server/matchmaking/session_manager.cpp
        src/server/net/listener.cpp
        tests/e2e_damage_multi.cpp)
    target_link_libraries(t2d_e2e_damage_multi PRIVATE t2d_proto libcoro yaml-cpp)
    target_include_directories(t2d_e2e_damage_multi PRIVATE src)
    target_compile_definitions(t2d_e2e_damage_multi PRIVATE T2D_VERSION="${T2D_VERSION_STR}")

    add_executable(
        t2d_e2e_kill_feed
        src/common/framing.cpp
        src/server/auth/auth_provider.cpp
        src/server/game/match.cpp
        src/server/matchmaking/matchmaker.cpp
        src/server/matchmaking/session_manager.cpp
        src/server/net/listener.cpp
        tests/e2e_kill_feed.cpp)
    target_link_libraries(t2d_e2e_kill_feed PRIVATE t2d_proto libcoro yaml-cpp)
    target_include_directories(t2d_e2e_kill_feed PRIVATE src)
    target_compile_definitions(t2d_e2e_kill_feed PRIVATE T2D_VERSION="${T2D_VERSION_STR}")

    # Register tests with CTest (only if BUILD_TESTING enabled)
    set(T2D_TEST_TARGETS
        t2d_unit_session_manager
        t2d_unit_framing
        t2d_unit_heartbeat_timeout
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
