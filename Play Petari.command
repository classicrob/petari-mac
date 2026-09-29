#!/bin/zsh
set -eu

repo_dir="$(cd -- "$(dirname -- "$0")" && pwd -P)"
app="$repo_dir/build/macos-gx/native/app/Petari.app/Contents/MacOS/Petari"
disc="${PETARI_GAME_DIR:-$repo_dir/build/game-data/RMGE01}"

if [[ ! -x "$app" ]]; then
    print -u2 -- "Build Petari first: cmake --build build/macos-gx --target petari -j 8"
    exit 1
fi
if [[ ! -d "$disc/files" ]]; then
    print -u2 -- "Extracted disc not found at $disc (expected files/). Set PETARI_GAME_DIR to its location."
    exit 1
fi

unset PETARI_SMOKE
cd -- "$repo_dir"
exec "$app" --disc "$disc" "$@"
