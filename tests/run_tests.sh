#!/bin/sh
# BTM9011EP_Sade_v3 host testi.
# Gercek BTM9011EP.c'yi taklit HAL ile derler; donanim gerektirmez.
#
# MSYS2 notu: /mingw64 ve /ucrt64 DLL'leri karisirsa gcc sessizce exit 1 doner.
# Bu yuzden PATH ucrt64 ile sinirlandiriliyor.
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
PATH="/c/msys64/ucrt64/bin:/usr/bin:/bin" \
  gcc -std=c99 -Wall -Wextra -I"$DIR" -I"$DIR/.." "$DIR/test_btm.c" -o "$DIR/test_btm.exe"
"$DIR/test_btm.exe"
