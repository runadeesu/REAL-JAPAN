cmake_minimum_required(VERSION 3.20)
project(rjcore VERSION 0.1.0 LANGUAGES CXX)

# rjcore: engine-agnostic simulation core for PROJECT: REAL JAPAN.
# No engine dependencies. Linked into the UE5 client (LOD0/LOD1 simulation)
# and into the headless simulation daemon (LOD2/LOD3 far simulation).

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
endif()

add_library(rjcore
  src/geo/ellipsoid.cpp
  src/geo/plane_rect.cpp
  src/geo/mesh_code.cpp
  src/geo/local_frame.cpp
  src/geo/geoid.cpp
  src/world/prefecture.cpp
  src/world/address.cpp
  src/stream/streamer.cpp
  src/sim/calendar.cpp
  src/sim/sim_lod.cpp
  src/sim/npc.cpp
  src/sim/schedule.cpp
  src/sim/social.cpp
  src/interact/uis.cpp
  src/interact/components.cpp
  src/econ/ledger.cpp
  src/verify/provenance.cpp
  src/env/solar.cpp
  src/nav/grid_nav.cpp
)
target_include_directories(rjcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

option(RJCORE_BUILD_TESTS "Build rjcore tests and tools" ON)
if(RJCORE_BUILD_TESTS)
add_executable(rjcore_tests
  tests/main.cpp
  tests/test_geo.cpp
  tests/test_world.cpp
  tests/test_stream.cpp
  tests/test_calendar.cpp
  tests/test_sim_lod.cpp
  tests/test_npc.cpp
  tests/test_social.cpp
  tests/test_uis.cpp
  tests/test_ledger.cpp
  tests/test_verify.cpp
  tests/test_solar.cpp
  tests/test_nav.cpp
)
target_link_libraries(rjcore_tests PRIVATE rjcore)

add_executable(rj_slice_sim tools/rj_slice_sim.cpp)
target_link_libraries(rj_slice_sim PRIVATE rjcore)

enable_testing()
add_test(NAME rjcore_tests COMMAND rjcore_tests)
endif()
