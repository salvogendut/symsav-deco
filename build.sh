#!/bin/bash
# Build deco screensaver for SymbOS using scc

SCC="${SCC:-../scc/bin/cc}"

"$SCC" deco.c \
    -N "Deco" \
    -o deco.sav \
    -h 512

python3 add_preview.py
