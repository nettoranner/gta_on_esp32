#!/bin/bash
# usage: run_test.sh OUTDIR [WORLD.BIN]
set -e
cd "$(dirname "$0")"
gcc -O2 -w -DGAME_C='"../main/game.c"' -o map_test map_test.c -lm
mkdir -p "$1"
./map_test "$1" $2
python3 -c "
from PIL import Image
import glob
for f in glob.glob('$1/*.ppm'):
    Image.open(f).save(f.replace('.ppm','.png'))
"
