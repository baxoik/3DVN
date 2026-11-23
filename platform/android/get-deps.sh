#!/bin/sh
# Downloads the libraries the Android build compiles from source into platform/android/deps
# (not kept in git): SDL2 and SDL2_ttf (which brings FreeType).
set -e
cd "$(dirname "$0")"
mkdir -p deps
SDL=2.32.10 TTF=2.24.0
fetch() {   # name url
  [ -d "deps/$1" ] && return
  echo "downloading $1"
  curl -sSL "$2" | tar -xz -C deps
}
fetch "SDL2-$SDL" "https://github.com/libsdl-org/SDL/releases/download/release-$SDL/SDL2-$SDL.tar.gz"
fetch "SDL2_ttf-$TTF" "https://github.com/libsdl-org/SDL_ttf/releases/download/release-$TTF/SDL2_ttf-$TTF.tar.gz"
ln -sfn "SDL2-$SDL" deps/SDL
ln -sfn "SDL2_ttf-$TTF" deps/SDL_ttf
echo "deps ready"
