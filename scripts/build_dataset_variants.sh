#!/bin/bash
# Construye un dataset de quadtrees en uno o varios backends (bv64, v5, v, sd, rrr, rrr15) con
# examples/build_dataset.
#
# Tres modos (MODE):
#   triples  lee un archivo de tripletas "s p o" (ids ya enumerados) una vez y escribe todos los backends
#            pendientes en la misma pasada.
#   pairs    lee una carpeta con una relacion "s o" por archivo, llamado <PREFIX><id_predicado> (default
#            PREFIX=prop-direct-P, el formato de ccqQDAGS-high_arity_qdag/data). Los de otro nombre
#            (prop-direct-normalized-P*, ontology-beta#*, ...) reciben ids nuevos (max+1, +2, ... en orden
#            alfabetico) con OTHERS=assign (default) o se omiten con skip; el mapa id -> archivo queda en
#            <OUT>/predicados_extra.tsv (o <OUT>/<backend>.predicados_extra.tsv si ese backend es el unico
#            pendiente), nunca dentro de la carpeta de un dataset. Cada archivo se lee una vez.
#   convert  pasa un dataset ya construido (FROM, default bv64) a los demas backends, sin releer las tripletas.
#
# Cada backend queda en <OUT>/<backend>/ y solo lo leen los ejecutables con ese --variant.
# Al terminar bien un backend se crea <OUT>/build_<backend>.done, fuera de la carpeta del dataset
# (load_directory intenta leer como indice todo archivo que haya ahi). Las corridas siguientes saltan los backends
# con marca; una corrida que se cae no la deja y se rehace completa. FORCE=true rehace todos.
#
# Uso: scripts/build_dataset_variants.sh <archivo.conf>
#
# El .conf es bash-sourceable. Variables:
#
#   MODE         triples | pairs | convert                             (default: triples)
#   INPUT        (obligatoria) archivo de tripletas (triples), carpeta de archivos <PREFIX><id> (pairs)
#                o carpeta del dataset de origen (convert)
#   OUT          (obligatoria) carpeta base de salida; los backends van en <OUT>/<backend>/
#   VARIANTS     backends a construir, separados por espacio o coma, o "all"   (default: all)
#   GRID_SIDE    lado de la grilla. triples y pairs: DEBE ser mayor que todos los ids (default 52000000;
#                la wiki completa usa 296008193). convert: vacio = k^altura de cada arbol, que da
#                la misma altura que el lado original (la consulta usa su propio GRID_SIDE)
#   ONLY_SO      true = solo la relacion SO, sin las proyecciones S y O (triples y pairs) (default: false)
#   PREFIX       solo pairs: prefijo del nombre de cada archivo                (default: prop-direct-P)
#   OTHERS       solo pairs: assign | skip, que hacer con los archivos de otro nombre (default: assign)
#   FROM         backend del dataset de origen (solo convert)                  (default: bv64)
#   FORCE        true = rehacer aunque exista la marca de terminado            (default: false)
#   BIN          carpeta de build de este repo                                  (default: <repo>/build)
#   MEMLIMIT_KB  ulimit -v del proceso, 0 = sin tope                            (default: 0)
#
# Deja en <OUT>: build_dataset.log (salida de cada corrida, con tiempos y tamanos por backend), build.conf (copia
# del .conf) y las marcas build_<backend>.done.
#
# ./scripts/build_dataset_variants.sh scripts/confs/wiki/build_dataset_wiki.conf

set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)

CONF="${1:?Usage: $0 <file.conf>}"
[ -f "$CONF" ] || { echo "config file not found: $CONF"; exit 1; }
# shellcheck source=/dev/null
source "$CONF"

MODE="${MODE:-triples}"
INPUT="${INPUT:?INPUT missing in $CONF (triples file, or source dataset folder in convert mode)}"
OUT="${OUT:?OUT missing in $CONF (output base folder)}"
VARIANTS="${VARIANTS:-all}"
GRID_SIDE="${GRID_SIDE:-}"
ONLY_SO="${ONLY_SO:-false}"
FROM="${FROM:-bv64}"
PREFIX="${PREFIX:-prop-direct-P}"
OTHERS="${OTHERS:-assign}"
FORCE="${FORCE:-false}"
BIN="${BIN:-$REPO_ROOT/build}"
MEMLIMIT_KB="${MEMLIMIT_KB:-0}"

ALL_BACKENDS=(bv64 v5 v sd rrr rrr15)
valid_backend() { local b; for b in "${ALL_BACKENDS[@]}"; do [ "$b" = "$1" ] && return 0; done; return 1; }

case "$MODE" in
    triples) [ -f "$INPUT" ] || { echo "triples file not found: $INPUT"; exit 1; }
             [ -n "$GRID_SIDE" ] || GRID_SIDE=52000000 ;;
    pairs)   [ -d "$INPUT" ] || { echo "relations folder not found: $INPUT"; exit 1; }
             [ -n "$GRID_SIDE" ] || GRID_SIDE=52000000 ;;
    convert) [ -d "$INPUT" ] || { echo "source dataset folder not found: $INPUT"; exit 1; }
             valid_backend "$FROM" || { echo "invalid FROM: $FROM (bv64|v5|v|sd|rrr|rrr15)"; exit 1; } ;;
    *) echo "invalid MODE: $MODE (triples|pairs|convert)"; exit 1 ;;
esac

EXE="$BIN/examples/build_dataset"
[ -x "$EXE" ] || { echo "$EXE not found; build it with 'cmake --build \"$BIN\"'"; exit 1; }

# Backends pedidos (valida antes de crear nada).
REQUESTED=()
if [ "$VARIANTS" = "all" ]; then
    REQUESTED=("${ALL_BACKENDS[@]}")
else
    for v in ${VARIANTS//,/ }; do
        valid_backend "$v" || { echo "invalid backend in VARIANTS: $v (bv64|v5|v|sd|rrr|rrr15|all)"; exit 1; }
        REQUESTED+=("$v")
    done
fi
[ ${#REQUESTED[@]} -gt 0 ] || { echo "VARIANTS vacio"; exit 1; }

mkdir -p "$OUT" || { echo "cannot create $OUT"; exit 1; }

# Pendientes: los que no tienen marca de terminado (o todos con FORCE=true).
PENDING=()
for v in "${REQUESTED[@]}"; do
    # convert: el backend de origen no se convierte a si mismo (y su carpeta podria estar dentro de OUT)
    if [ "$MODE" = "convert" ] && { [ "$v" = "$FROM" ] || [ "$(readlink -f "$INPUT")" = "$(readlink -f "$OUT/$v")" ]; }; then
        echo "[$v] is the source dataset -> skipped"
        continue
    fi
    if [ "$FORCE" != "true" ] && [ -f "$OUT/build_$v.done" ]; then
        echo "[$v] already built ($OUT/build_$v.done) -> skipped"
        continue
    fi
    PENDING+=("$v")
done
if [ ${#PENDING[@]} -eq 0 ]; then
    echo "nothing to build (FORCE=true to rebuild)"
    exit 0
fi

# Con un solo backend pendiente build_dataset escribe directo en la carpeta dada (<OUT>/<backend>); con varios
# recibe <OUT> y pone cada uno en su subcarpeta.
VARIANT_LIST=$(IFS=,; echo "${PENDING[*]}")
if [ ${#PENDING[@]} -eq 1 ]; then TARGET="$OUT/${PENDING[0]}"; else TARGET="$OUT"; fi
for v in "${PENDING[@]}"; do
    # una corrida anterior caida pudo dejar una carpeta a medias: se rehace limpia
    [ -d "$OUT/$v" ] && { echo "[$v] incomplete folder from a previous run: deleting and rebuilding"; rm -rf "$OUT/$v"; }
done

ARGS=( "$MODE" "$INPUT" "$TARGET" --variant "$VARIANT_LIST" )
[ -n "$GRID_SIDE" ] && ARGS+=( --grid_side "$GRID_SIDE" )
if [ "$MODE" = "triples" ] || [ "$MODE" = "pairs" ]; then
    [ "$ONLY_SO" = "true" ] && ARGS+=( --only_so )
    [ "$MODE" = "pairs" ] && ARGS+=( --prefix "$PREFIX" --others "$OTHERS" )
else
    ARGS+=( --from "$FROM" )
fi

cp "$CONF" "$OUT/build.conf"
LOG="$OUT/build_dataset.log"
{
    echo "=== $(date '+%Y-%m-%d %H:%M:%S') | $MODE | backends: $VARIANT_LIST | input: $INPUT"
    echo "=== $EXE ${ARGS[*]}"
} | tee -a "$LOG"

if [ "$MEMLIMIT_KB" != "0" ]; then ulimit -v "$MEMLIMIT_KB"; fi
"$EXE" "${ARGS[@]}" 2>&1 | tee -a "$LOG"
RC=${PIPESTATUS[0]}

if [ "$RC" -ne 0 ]; then
    echo "FAILED rc=$RC: no backend marked as done (see $LOG)" | tee -a "$LOG"
    exit "$RC"
fi
for v in "${PENDING[@]}"; do
    n=$(find "$OUT/$v" -maxdepth 1 -type f | wc -l)
    if [ "$n" -eq 0 ]; then echo "FAILED: $OUT/$v is empty" | tee -a "$LOG"; exit 1; fi
    date '+%Y-%m-%d %H:%M:%S' > "$OUT/build_$v.done"
    echo "[$v] done: $n files in $OUT/$v" | tee -a "$LOG"
done
