cmake_minimum_required(VERSION 3.20)
project(RealJapan VERSION 0.2.0 LANGUAGES C CXX)

# PROJECT: REAL JAPAN native client (Windows x64 primary target, Linux for dev).
#   Linux:   cmake -S client -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
#   Windows: cmake -S client -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
#              -DCMAKE_TOOLCHAIN_FILE=client/cmake/mingw-w64-x86_64.cmake
# Third-party sources are fetched by tools/fetch_deps.sh into third_party/.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
get_filename_component(RJ_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# --- raylib (zlib licence), OpenGL 3.3 desktop -------------------------------
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
set(PLATFORM "Desktop" CACHE STRING "" FORCE)
set(OPENGL_VERSION "3.3" CACHE STRING "" FORCE)
set(USE_AUDIO ON CACHE BOOL "" FORCE)  # procedural sound (src/audio), no audio files
if(NOT EXISTS "${RJ_ROOT}/third_party/raylib/CMakeLists.txt")
  message(FATAL_ERROR "raylib not found. Run tools/fetch_deps.sh first.")
endif()
add_subdirectory("${RJ_ROOT}/third_party/raylib" raylib EXCLUDE_FROM_ALL)
target_compile_definitions(raylib PRIVATE SUPPORT_FILEFORMAT_JPG=1)  # PLATEAU photo atlases

# --- rjcore -------------------------------------------------------------------
set(RJCORE_BUILD_TESTS ON CACHE BOOL "")
add_subdirectory("${RJ_ROOT}/core" rjcore)

# --- game executable ---------------------------------------------------------
set(RJ_SOURCES
  src/main.cpp
  src/app.cpp
  src/platform/paths.cpp
  src/util/text.cpp
  src/config/settings.cpp
  src/i18n/i18n.cpp
  src/save/save.cpp
  src/world/cell.cpp
  src/world/world.cpp
  src/world/interior.cpp
  src/world/detail.cpp
  src/world/facade.cpp
  src/render/renderer.cpp
  src/render/textures.cpp
  src/render/foliage.cpp
  src/render/vehicles.cpp
  src/render/humans.cpp
  src/game/player.cpp
  src/game/town_sim.cpp
  src/game/pedestrians.cpp
  src/game/weather.cpp
  src/game/traffic_signals.cpp
  src/game/road_markings.cpp
  src/game/trains.cpp
  src/game/driving.cpp
  src/game/ferries.cpp
  src/render/ships.cpp
  src/game/aircraft.cpp
  src/game/jobs.cpp
  src/app_activities.cpp
  src/app_sound.cpp
  src/render/aircraft.cpp
  src/render/trains.cpp
  src/game/traffic.cpp
  src/ui/ui.cpp
  src/audio/audio.cpp
)
if(WIN32)
  list(APPEND RJ_SOURCES res/realjapan.rc)
endif()

add_executable(RealJapan WIN32 ${RJ_SOURCES})
target_include_directories(RealJapan PRIVATE src)
target_link_libraries(RealJapan PRIVATE rjcore raylib)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(RealJapan PRIVATE -Wall -Wextra)
endif()
if(MINGW)
  # Fully static C/C++ runtime: the .exe needs no MinGW DLLs on the user's PC.
  target_link_options(RealJapan PRIVATE -static -static-libgcc -static-libstdc++ $<$<CONFIG:Release>:-s>)
  target_link_libraries(rjcore_tests PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# Offline renders of the sound engine (tuning aid): cmake --build <dir> --target rj_audio_demo
add_executable(rj_audio_demo EXCLUDE_FROM_ALL tools/audio_demo.cpp src/audio/audio.cpp)
target_include_directories(rj_audio_demo PRIVATE src)
target_link_libraries(rj_audio_demo PRIVATE raylib)

# Stage game data next to the executable for development runs.
add_custom_target(stage_data ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${RJ_ROOT}/game/data" "$<TARGET_FILE_DIR:RealJapan>/data"
  COMMENT "Copying game data")
add_dependencies(stage_data RealJapan)
