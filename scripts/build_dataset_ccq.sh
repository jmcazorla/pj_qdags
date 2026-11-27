#!/bin/bash
# Construye, desde un .conf, el dataset en el formato de ccqQDAGS-high_arity_qdag (build_dataset_ccq) que lee run_ccq.
# Como build_dataset_variants.sh MODE=pairs, pero con un solo backend ("ccq") y la salida es OUT misma.
#
# Uso: scripts/build_dataset_ccq.sh <archivo.conf>
#
# Variables del .conf:
#   INPUT        (obligatoria) carpeta con un archivo "<PREFIX><id>" por predicado ("s o" por linea), p. ej. la data/ de ccqQDAGS
#   OUT          (obligatoria) carpeta del dataset (se crea). El mapa de ids extra queda en OUT.predicados_extra.tsv, fuera de ella
#   PREFIX       prefijo de los archivos           (default: prop-direct-P)
#   OTHERS       assign|skip: archivos de otro nombre reciben ids nuevos o se omiten (default: assign)
#   GRID_SIDE    lado de la grilla                 (default: 52000000; debe coincidir con el de las corridas)
#   ONLY_SO      true = solo los SO, sin S ni O    (default: false)
#   FORCE        true = rehacer aunque exista OUT.build.done (default: false)
#   BIN          carpeta de build de este repo     (default: <repo>/build)
#   MEMLIMIT_KB  tope de memoria virtual, ulimit -v (default: 0 = sin tope)
#
# Una corrida cortada deja la carpeta a medias y no escribe OUT.build.done: la siguiente la borra y la rehace limpia.

set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)

CONF="${1:?Usage: $0 <file.conf>}"
[ -f "$CONF" ] || { echo "config file not found: $CONF"; exit 1; }
# shellcheck source=/dev/null
source "$CONF"

INPUT="${INPUT:?INPUT missing in $CONF (folder with the <PREFIX><id> files)}"
OUT="${OUT:?OUT missing in $CONF (dataset folder)}"
PREFIX="${PREFIX:-prop-direct-P}"
OTHERS="${OTHERS:-assign}"
GRID_SIDE="${GRID_SIDE:-52000000}"
ONLY_SO="${ONLY_SO:-false}"
FORCE="${FORCE:-false}"
BIN="${BIN:-$REPO_ROOT/build}"
MEMLIMIT_KB="${MEMLIMIT_KB:-0}"

[ -d "$INPUT" ] || { echo "relations folder not found: $INPUT"; exit 1; }
EXE="$BIN/examples/build_dataset_ccq"
[ -x "$EXE" ] || { echo "$EXE not found; build it with 'cmake --build \"$BIN\"'"; exit 1; }

OUT="${OUT%/}"
DONE="$OUT.build.done"
if [ "$FORCE" != "true" ] && [ -f "$DONE" ]; then
    echo "already built ($DONE) -> nothing to do (FORCE=true to rebuild)"
    exit 0
fi
[ -d "$OUT" ] && { echo "incomplete folder from a previous run: deleting and rebuilding ($OUT)"; rm -rf "$OUT"; }
rm -f "$DONE"

ARGS=( "$INPUT" "$OUT" --prefix "$PREFIX" --others "$OTHERS" --grid_side "$GRID_SIDE" )
[ "$ONLY_SO" = "true" ] && ARGS+=( --only_so )

if [ "$MEMLIMIT_KB" != 0 ]; then ulimit -v "$MEMLIMIT_KB"; fi
"$EXE" "${ARGS[@]}"
rc=$?
if [ "$rc" != 0 ]; then
    echo "build_dataset_ccq failed (rc=$rc)"
    exit "$rc"
fi
date '+%F %T' > "$DONE"
echo "done: $OUT"
