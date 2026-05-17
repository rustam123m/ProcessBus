#!/bin/sh
# hotpath_disasm.sh — dump a single object file's disassembly to a flat .S file.
#
# Usage: hotpath_disasm.sh OUT_DIR OUT_FILE OBJDUMP OBJ_FILE [EXTRA_OBJDUMP_ARGS...]
#
# Invoked by cmake/hotpath.cmake as the POST_BUILD step. Lives in a shell
# script (not inline in CMake) to avoid CMake's add_custom_command quoting
# layer mangling the embedded shell redirect.

set -e

out_dir=$1
out_file=$2
objdump=$3
obj=$4
shift 4

mkdir -p "$out_dir"
"$objdump" -d -S -C "$@" "$obj" > "$out_file"
