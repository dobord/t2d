cmake_minimum_required(VERSION 3.22)
project(t2d LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(T2D_BUILD_SERVER "Build the game server" ON)
option(T2D_BUILD_CLIENT "Build the desktop client (Qt)" OFF) # enable later
option(T2D_BUILD_TESTS "Build internal test executables" ON)

## Third-party dependencies via git submodules under third_party
## We intentionally avoid FetchContent here to pin revisions through submodule SHAs
## and enable reproducible builds & offline CI.

## Force-disable third_party tests/examples/tools/docs for lean build
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "yaml-cpp tests disabled" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "yaml-cpp tools disabled" FORCE)
set(YAML_CPP_BUILD_CONTRIB ON CACHE BOOL "yaml-cpp contrib enabled" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "yaml-cpp install targets disabled" FORCE)

set(LIBCORO_BUILD_TESTS OFF CACHE BOOL "libcoro tests disabled" FORCE)
set(LIBCORO_BUILD_EXAMPLES OFF CACHE BOOL "libcoro examples disabled" FORCE)
set(LIBCORO_FEATURE_TLS OFF CACHE BOOL "libcoro TLS disabled for now" FORCE)
set(CARES_BUILD_TOOLS OFF CACHE BOOL "c-ares tools disabled" FORCE)

set(BOX2D_UNIT_TESTS OFF CACHE BOOL "box2d unit tests disabled" FORCE)
set(BOX2D_SAMPLES OFF CACHE BOOL "box2d samples disabled" FORCE)
set(BOX2D_BENCHMARKS OFF CACHE BOOL "box2d benchmarks disabled" FORCE)
set(BOX2D_DOCS OFF CACHE BOOL "box2d docs disabled" FORCE)

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

if(T2D_BUILD_SERVER)
  add_executable(t2d_server
    src/server/main.cpp
    src/server/matchmaking/matchmaker.cpp
  src/server/matchmaking/session_manager.cpp
  src/server/net/listener.cpp
  src/common/framing.cpp
  src/server/game/match.cpp
  )
  target_include_directories(t2d_server PRIVATE src)
  target_link_libraries(t2d_server PRIVATE t2d_proto yaml-cpp libcoro box2d)
  target_compile_definitions(t2d_server PRIVATE T2D_VERSION="0.0.1-dev")
endif()

# Formatting target placeholder (user can integrate clang-format config later)
find_program(CLANG_FORMAT_BIN clang-format)
if(CLANG_FORMAT_BIN)
  file(GLOB_RECURSE ALL_CXX_SOURCE_FILES CONFIGURE_DEPENDS src/*.cpp src/*.hpp src/*.h include/*.hpp include/*.h)
  # Avoid target name collision if a third_party submodule already defined 'format'
  if(NOT TARGET format)
    add_custom_target(format
      COMMAND ${CLANG_FORMAT_BIN} -i ${ALL_CXX_SOURCE_FILES}
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMENT "Formatting C/C++ sources with clang-format")
  else()
    add_custom_target(t2d_format
      COMMAND ${CLANG_FORMAT_BIN} -i ${ALL_CXX_SOURCE_FILES}
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMENT "Formatting t2d C/C++ sources with clang-format (dependency already defines 'format')")
    message(STATUS "Dependency already defines target 'format'; added 't2d_format' for project sources.")
  endif()
endif()

add_executable(t2d_test_client
  src/client/test_client.cpp
  src/common/framing.cpp
)
target_link_libraries(t2d_test_client PRIVATE t2d_proto libcoro)
target_include_directories(t2d_test_client PRIVATE src)

if(T2D_BUILD_TESTS)
  add_executable(t2d_unit_session_manager
    tests/unit_session_manager.cpp
    src/server/matchmaking/session_manager.cpp
    src/common/framing.cpp
  )
  target_link_libraries(t2d_unit_session_manager PRIVATE t2d_proto libcoro yaml-cpp)
  target_include_directories(t2d_unit_session_manager PRIVATE src)

  add_executable(t2d_unit_framing
    tests/unit_framing.cpp
    src/common/framing.cpp
  )
  target_link_libraries(t2d_unit_framing PRIVATE t2d_proto libcoro)
  target_include_directories(t2d_unit_framing PRIVATE src)

  add_executable(t2d_unit_heartbeat_timeout
    tests/unit_heartbeat_timeout.cpp
    src/server/matchmaking/session_manager.cpp
  )
  target_link_libraries(t2d_unit_heartbeat_timeout PRIVATE t2d_proto libcoro yaml-cpp)
  target_include_directories(t2d_unit_heartbeat_timeout PRIVATE src)

  add_executable(t2d_e2e_match_start
    tests/e2e_match_start.cpp
    src/server/matchmaking/matchmaker.cpp
    src/server/matchmaking/session_manager.cpp
    src/server/net/listener.cpp
    src/common/framing.cpp
  src/server/game/match.cpp
  )
  target_link_libraries(t2d_e2e_match_start PRIVATE t2d_proto libcoro yaml-cpp)
  target_include_directories(t2d_e2e_match_start PRIVATE src)

  add_executable(t2d_e2e_input_move
    tests/e2e_input_move.cpp
    src/server/matchmaking/matchmaker.cpp
    src/server/matchmaking/session_manager.cpp
    src/server/net/listener.cpp
    src/common/framing.cpp
    src/server/game/match.cpp
  )
  target_link_libraries(t2d_e2e_input_move PRIVATE t2d_proto libcoro yaml-cpp)
  target_include_directories(t2d_e2e_input_move PRIVATE src)

  add_executable(t2d_e2e_heartbeat
    tests/e2e_heartbeat.cpp
    src/server/matchmaking/matchmaker.cpp
    src/server/matchmaking/session_manager.cpp
    src/server/net/listener.cpp
    src/common/framing.cpp
  src/server/game/match.cpp
  )
  target_link_libraries(t2d_e2e_heartbeat PRIVATE t2d_proto libcoro yaml-cpp)
  target_include_directories(t2d_e2e_heartbeat PRIVATE src)

  add_executable(t2d_e2e_bot_fill
    tests/e2e_bot_fill.cpp
    src/server/matchmaking/matchmaker.cpp
    src/server/matchmaking/session_manager.cpp
    src/server/net/listener.cpp
    src/common/framing.cpp
    src/server/game/match.cpp
  )
  target_link_libraries(t2d_e2e_bot_fill PRIVATE t2d_proto libcoro yaml-cpp)
  target_include_directories(t2d_e2e_bot_fill PRIVATE src)
  add_executable(t2d_e2e_bot_projectile
    tests/e2e_bot_projectile.cpp
    src/server/matchmaking/matchmaker.cpp
    src/server/matchmaking/session_manager.cpp
    src/server/net/listener.cpp
    src/common/framing.cpp
    src/server/game/match.cpp
  )
  target_link_libraries(t2d_e2e_bot_projectile PRIVATE t2d_proto libcoro yaml-cpp)
  target_include_directories(t2d_e2e_bot_projectile PRIVATE src)
  add_executable(t2d_e2e_delta_snapshots
    tests/e2e_delta_snapshots.cpp
    src/server/matchmaking/matchmaker.cpp
    src/server/matchmaking/session_manager.cpp
    src/server/net/listener.cpp
    src/common/framing.cpp
    src/server/game/match.cpp
  )
  target_link_libraries(t2d_e2e_delta_snapshots PRIVATE t2d_proto libcoro yaml-cpp)
  target_include_directories(t2d_e2e_delta_snapshots PRIVATE src)
endif()
