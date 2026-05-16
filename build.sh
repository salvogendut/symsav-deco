#!/bin/bash
# Build deco screensaver for SymbOS using scc

SCC="${SCC:-../scc/bin/cc}"

"$SCC" deco.c deco_msx.s \
    -N "Deco" \
    -o deco.sav \
    -h 512

python3 add_preview.py
