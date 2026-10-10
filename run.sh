#!/bin/sh
# Runs the native build against the mirrored game data. GTA_MODE=story|sandbox|sandbox6 picks the start (default sandbox).
cd "$(dirname "$0")"
: "${GTA_DATA:=$HOME/Downloads/GTA5Webport/playgta5-offline/mirror/playgta5.com/data}"
mkdir -p userdata
GTA_DATA="$GTA_DATA" GTA_USER="$PWD/userdata" exec ./build/mac/gta "$@" > run/game.out 2> run/game.err
