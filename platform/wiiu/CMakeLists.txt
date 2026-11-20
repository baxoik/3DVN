# Wii U build of 3DVN (TV shows the top screen, GamePad the bottom screen).
#   /opt/devkitpro/portlibs/wiiu/bin/powerpc-eabi-cmake -S platform/wiiu -B build-wiiu
#   cmake --build build-wiiu        -> build-wiiu/3dvn.rpx and build-wiiu/3dvn.wuhb
# Games go on the SD card in sd:/wiiu/3dvn/games/<name>/.
cmake_minimum_required(VERSION 3.10)
project(3dvn C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL REQUIRED IMPORTED_TARGET sdl2 SDL2_ttf)

file(GLOB ENGINE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/../../source/*.c)
add_executable(3dvn ${ENGINE_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/../sdl/sdl_compat.c)
target_compile_definitions(3dvn PRIVATE VN_SDL STBI_NO_THREAD_LOCALS ROOT_DIR="fs:/vol/external01/wiiu/3dvn")
target_include_directories(3dvn PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../sdl/include ${CMAKE_CURRENT_SOURCE_DIR}/../../source)
target_compile_options(3dvn PRIVATE -O2 -Wall)
target_link_libraries(3dvn PkgConfig::SDL m)

wut_create_rpx(3dvn)
wut_create_wuhb(3dvn
	NAME "3DVN"
	SHORTNAME "3DVN"
	AUTHOR "3DVN"
	ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.png")
# Homebrew Launcher: copy hbl-icon.png as icon.png next to 3dvn.rpx (sd:/wiiu/apps/3dvn/).
