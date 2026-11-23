# 3DVN for Android: SDL2, SDL2_ttf (with its own FreeType) and the 3DVN engine, built from
# source (run platform/android/get-deps.sh first). The engine runs as on the PC, through
# platform/sdl/sdl_compat.c, in its single-screen ("desktop") layout.
cmake_minimum_required(VERSION 3.18)
project(threedvn C)

set(DEPS ${CMAKE_CURRENT_SOURCE_DIR}/../../deps)
set(ENGINE ${CMAKE_CURRENT_SOURCE_DIR}/../../../..)

add_subdirectory(${DEPS}/SDL ${CMAKE_BINARY_DIR}/SDL)
set(SDL2TTF_VENDORED ON CACHE BOOL "" FORCE)
set(SDL2TTF_HARFBUZZ OFF CACHE BOOL "" FORCE)
set(SDL2TTF_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDL2TTF_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${DEPS}/SDL_ttf ${CMAKE_BINARY_DIR}/SDL_ttf)

file(GLOB ENGINE_SOURCES ${ENGINE}/source/*.c)
add_library(main SHARED ${ENGINE_SOURCES} ${ENGINE}/platform/sdl/sdl_compat.c)
target_include_directories(main PRIVATE ${ENGINE}/platform/sdl/include ${ENGINE}/source)
# games, saves and settings: <app files>/3dvn (the app starts in its files folder)
target_compile_definitions(main PRIVATE VN_SDL ROOT_DIR="./3dvn")
target_compile_options(main PRIVATE -O2 -Wall -Wno-format-truncation)
target_link_libraries(main PRIVATE SDL2::SDL2 SDL2_ttf::SDL2_ttf m log)
