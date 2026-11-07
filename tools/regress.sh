#!/bin/sh
# Deterministic regression run: boot, start, a scripted two-player fight,
# 4400 frames headless; prints the state hash. Builds that emulate the same
# give the same hash (any compiler, Linux or Windows, linked-in code or a
# module built at run time), and so do a raw disk image and the CHD.
#
#   tools/regress.sh [-g ki1|ki2] PROGRAM DISK_HEAD [RUN_DIR] [WORK_DIR]
#
# -g: which game to run, with its files and inputs (default ki2).
# DISK_HEAD: the game's settings sectors (32 KB) saved earlier; the hash
# depends on them. They are written to a settings file in WORK_DIR (default
# $TMPDIR/uc-regress) before each run; the disk image is only read. RUN_DIR
# holds your game files (default: run/). DISK=path uses another disk image
# (a raw file or the CHD), ROM=name another boot ROM in RUN_DIR (another
# game version, e.g. ROM=ki-l14.u98). The DIP switches are the defaults unless KI2_DSW
# is set. Extra environment (KI2_LOAD_MODULE, KI2_BUILD, KI2_NOREC,
# KI2_DUMP_AT, KI2_WAV, ...) passes through.
set -e
game=ki2
if [ "$1" = -g ]; then game=$2; shift 2; fi
case $game in
ki1) rom=ki-l15d.u98 disk=kinst.raw ;;
ki2) rom=ki2-l14.u98 disk=kinst2.raw ;;
*) echo "unknown game $game" >&2; exit 1 ;;
esac
prog=$(realpath "$1") head=$(realpath "$2")
run=$(realpath "${3:-run}") work=${4:-${TMPDIR:-/tmp}/uc-regress}
inputs=$(realpath "$(dirname "$0")/regress-$game.txt")
image=$(realpath "${DISK:-$run/$disk}")
rom=${ROM:-$rom}
export KI2_DSW=${KI2_DSW:-fffff1ff}     # blood high, demo sounds, finishing moves, free play
export KI2_DISK=${KI2_DISK:-original}  # the default drive timing
mkdir -p "$work"
cp "$head" "$work/$game.settings"
cd "$work"
"$prog" --game $game --rom "$run/$rom" --sound "$run" --disk "$image" --settings "$work/$game.settings" \
    --headless 4400 --script "$(cat "$inputs")" 2>&1 | grep -E '^(rec:|state hash|These files)'
