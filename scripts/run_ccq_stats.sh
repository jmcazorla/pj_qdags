#!/bin/bash
# Corre examples/run_ccq (las estrategias sobre el quadtree/qdag/AND de ccqQDAGS-high_arity_qdag, include/qdags/ccq/)
# sobre un dataset en formato ccq (build_dataset_ccq) y un archivo de queries, una vez por nivel de PARALLEL_LEVELS, con
# la tabla de tiempos de run_orig_stats.sh. El AND de ccq es secuencial: PARALLEL_LEVELS > 0 corre igual secuencial
# (aviso en stderr), asi que lo normal es "0".
#
# Uso: scripts/run_ccq_stats.sh <archivo.conf>
#
# Variables del .conf (bash-sourceable; como run_orig_stats.sh salvo lo que se indica):
#   DATASET, QUERIES, OUT, BIN, REPS, GRID_SIDE, PARALLEL_LEVELS, TIME_OUT, MEMLIMIT_KB, TIMEOUT
#   STRATEGY         (obligatoria) orig | nl | wk | kernel | leapfrog | plan   (nl = One-Shot)
#   PLAN_GROUP       (plan) normal | best | wk_k | leapfrog
#   PLAN_K           (plan) k del plan wk_k (default: 2)
#   LF_ORDER, LF_DEPTH, LF_MAX_ATTS   (leapfrog) como run_leapfrog_stats.sh (defaults proj_size, full, 0 = auto)
# summary.csv agrega n_timeouts (consultas cortadas por TIME_OUT: resultado parcial; si es > 0 los tiempos de ese nivel no sirven
# para comparar). results_p<N>_r<R>.txt y times_p<N>_r<R>.txt llevan ";TIMEOUT" al final de esas consultas.
# No hay VARIANT ni WIDE_BASE: ccq no tiene backends de rank ni modo de dispersion.
#
# ./scripts/run_ccq_stats.sh <archivo.conf>

set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)

CONF="${1:?Usage: $0 <file.conf>}"
[ -f "$CONF" ] || { echo "config file not found: $CONF"; exit 1; }
# shellcheck source=/dev/null
source "$CONF"

DATASET="${DATASET:?DATASET missing in $CONF (index directory, see generate_demo_dataset.cpp)}"
QUERIES="${QUERIES:?QUERIES missing in $CONF (text query file)}"
OUT="${OUT:?OUT missing in $CONF (output directory for results_p*.txt/times_p*.txt/summary.csv/run.conf)}"
BIN="${BIN:-$REPO_ROOT/build}"
STRATEGY="${STRATEGY:?STRATEGY missing in $CONF (orig|nl|wk|kernel|leapfrog|plan)}"
PLAN_GROUP="${PLAN_GROUP:-}"
PLAN_K="${PLAN_K:-2}"
LF_ORDER="${LF_ORDER:-proj_size}"
LF_DEPTH="${LF_DEPTH:-full}"
LF_MAX_ATTS="${LF_MAX_ATTS:-0}"
UPPER_BOUND="${UPPER_BOUND:-0}"   # debe ser 0: el AND de ccq no soporta tope de resultados
REPS="${REPS:-1}"
GRID_SIDE="${GRID_SIDE:-52000000}"
PARALLEL_LEVELS="${PARALLEL_LEVELS:-0 2 4}"
TIME_OUT="${TIME_OUT:-0}"
MEMLIMIT_KB="${MEMLIMIT_KB:-0}"
TIMEOUT="${TIMEOUT:-0}"

EXE="$BIN/examples/run_ccq"
[ -x "$EXE" ] || { echo "$EXE not found; build it with 'cmake --build \"$BIN\"'"; exit 1; }

mkdir -p "$OUT"
cp "$CONF" "$OUT/run.conf"
SUMMARY="$OUT/summary.csv"
echo "parallel_level;n_queries;n_skipped;total_ms;mean_ms;median_ms;speedup;n_timeouts" > "$SUMMARY"

printf '%-15s %10s %9s %12s %10s %12s %10s %11s\n' \
       parallel_level n_queries n_skipped total_ms mean_ms median_ms speedup n_timeouts

source "$SCRIPT_DIR/lib_levels.sh"

LEVELS=( $PARALLEL_LEVELS )
[ ${#LEVELS[@]} -gt 0 ] || { echo "PARALLEL_LEVELS vacio"; exit 1; }

# Una sola ejecucion con todos los niveles y repeticiones: el dataset se carga una vez.
build_run_args() {
    local run=( "$DATASET" "$QUERIES" "$UPPER_BOUND" "$1" --strategy "$STRATEGY" --reps "$REPS" --grid_side "$GRID_SIDE" --time_out "$TIME_OUT"
                --results_file "$OUT/results_p{L}_r{R}.txt" --times_file "$OUT/times_p{L}_r{R}.txt" )
    case "$STRATEGY" in
        leapfrog) run+=( --lf_order "$LF_ORDER" --lf_depth "$LF_DEPTH" --lf_max_atts "$LF_MAX_ATTS" ) ;;
        plan)     [ -n "$PLAN_GROUP" ] || { echo "STRATEGY=plan necesita PLAN_GROUP en $CONF"; exit 1; }
                  run+=( --plan_group "$PLAN_GROUP" --k "$PLAN_K" ) ;;
    esac
    RUN_ARGS=( "${run[@]}" )
}

baseline_total=""

on_level_ok() {
    local plevel=$1 result_line=$2
    IFS=';' read -r _ _ n_queries n_skipped total avg median <<< "$result_line"

    [ -z "$baseline_total" ] && baseline_total="$total"

    speedup=$(awk -v b="$baseline_total" -v t="$total" 'BEGIN{ if (t+0>0) printf "%.2f", b/t; else print "-" }')

    # consultas cortadas por TIME_OUT (su resultado es parcial y sus ms incluyen el timeout entero): run_ccq las marca
    # con ";TIMEOUT" en results_p<N>_r1.txt. Si n_timeouts > 0 el total/mean de este nivel NO sirve para comparar tiempos.
    local n_timeouts; n_timeouts=$(grep -c ';TIMEOUT' "$OUT/results_p${plevel}_r1.txt" 2>/dev/null || true)

    printf '%-15s %10s %9s %12s %10s %12s %10s %11s\n' "$plevel" "$n_queries" "$n_skipped" "$total" "$avg" "$median" "$speedup" "${n_timeouts:-0}"
    echo "${plevel};${n_queries};${n_skipped};${total};${avg};${median};${speedup};${n_timeouts:-0}" >> "$SUMMARY"
}

on_level_fail() {
    local plevel=$1 rc=$2 errtail=$3
        printf '%-15s %10s %9s %12s %10s %12s %10s %11s\n' "$plevel" - - - - - "FAILED(rc=$rc)" -
        echo "${plevel};;;;;;FAILED_rc${rc};" >> "$SUMMARY"
    [ -n "$errtail" ] && echo "$errtail"
}

on_level_skipped() {
        printf '%-15s %10s %9s %12s %10s %12s %10s %11s\n' "$1" - - - - - "NOT_RUN" -
        echo "${1};;;;;;NOT_RUN;" >> "$SUMMARY"
}

run_levels_driver

baseline_level=$(echo $PARALLEL_LEVELS | awk '{print $1}')
echo
echo "speedup is relative to parallel_level=${baseline_level} (first of PARALLEL_LEVELS)."
echo "Per-query results: $OUT/results_p<N>_r<R>.txt | Per-query times: $OUT/times_p<N>_r<R>.txt"
echo "Summary: $SUMMARY"
