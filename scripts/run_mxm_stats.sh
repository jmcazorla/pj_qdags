#!/bin/bash
# Corre examples/run_mxm (basic + wp) sobre un dataset+archivo de queries, una vez por cada nivel de PARALLEL_LEVELS, y
# arma una tabla de tiempo ("basic"/"wp" hacen el papel de "generic"/"adaptive" de run_wk_stats.sh). mxm no es una
# estrategia BGP general: usa solo los dos primeros patrones de cada query (examples/run_mxm.cpp) y solo "basic" usa
# PARALLEL_LEVEL/UPPER_BOUND ("wp" corre sin cota y secuencial).
#
# Uso: scripts/run_mxm_stats.sh <archivo.conf>
#
# Todos los PARALLEL_LEVELS (y REPS) corren en una ejecucion: el dataset se carga una vez. Si el proceso muere en
# un nivel (p.ej. SIGKILL por memoria), ese nivel queda FAILED_rc<N> en el resumen y se relanza con los que
# faltan (ver lib_levels.sh).
#
# El .conf es un archivo bash con asignaciones (plantilla: scripts/confs/*.conf.example). Variables:
#
#   DATASET          (obligatoria) directorio de indices ("<pid>"=SO, "<pid>s"=S, "<pid>o"=O), sin el archivo de queries
#   QUERIES          (obligatoria) archivo de queries (query_loader.hpp)
#   OUT              (obligatoria) directorio de salida. Por nivel y repeticion escribe results_p<N>_r<R>.txt
#                    ("query_id;basic_result;wp_result"; la cardinalidad no debe cambiar entre niveles/backends/corridas)
#                    y times_p<N>_r<R>.txt ("query_id;basic_ms;wp_ms"), mas summary.csv (una fila por nivel, con speedup
#                    contra el primer nivel) y run.conf (copia de la configuracion).
#   BIN              carpeta de build de este repo          (default: <repo>/build)
#   VARIANT          backend: bv64|v5|v|sd|rrr|rrr15 (default bv64)
#   UPPER_BOUND      cota de resultados por query, 0=sin limite -- solo
#   REPS             repeticiones por nivel (default 1); con REPS>1 el resumen muestra la media y cada una queda
#                    en results_p<N>_r<R>.txt / times_p<N>_r<R>.txt
#                     aplica a "basic"                        (default: 0)
#   GRID_SIDE        lado de la grilla logica de la query     (default: 52000000)
#   PARALLEL_LEVELS  niveles de corte a comparar, separados por espacio --
#                     solo aplica a "basic"                    (default: "0 2 4")
#   MODE             basic|wp|both -- que motor(es) correr por query
#                     (default: both)
#   TIME_OUT         timeout POR QUERY en segundos, 0=sin limite (default: 0)
#   MEMLIMIT_KB      tope de memoria virtual por corrida (ulimit -v; default 0 = sin tope)
#   TIMEOUT          tope de tiempo de toda la ejecucion en segundos (default 0 = sin tope); al vencer, el nivel
#                    en curso queda FAILED_rc124, los que faltan NOT_RUN y no se relanza
#
# ./scripts/run_mxm_stats.sh scripts/confs/mi_corrida.conf

set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)

CONF="${1:?Usage: $0 <file.conf>}"
[ -f "$CONF" ] || { echo "config file not found: $CONF"; exit 1; }
# shellcheck source=/dev/null
source "$CONF"
source "$SCRIPT_DIR/lib_wide_base.sh"   # WIDE_BASE / WIDE_MIN_D / WIDE_MIN_DIM -> WIDE_ARGS

DATASET="${DATASET:?DATASET missing in $CONF (index directory)}"
QUERIES="${QUERIES:?QUERIES missing in $CONF (text query file, >=2 patterns)}"
OUT="${OUT:?OUT missing in $CONF (output directory for results_p*.txt/times_p*.txt/summary.csv/run.conf)}"
BIN="${BIN:-$REPO_ROOT/build}"
VARIANT="${VARIANT:-bv64}"
UPPER_BOUND="${UPPER_BOUND:-0}"
REPS="${REPS:-1}"
GRID_SIDE="${GRID_SIDE:-52000000}"
PARALLEL_LEVELS="${PARALLEL_LEVELS:-0 2 4}"
MODE="${MODE:-both}"
TIME_OUT="${TIME_OUT:-0}"
MEMLIMIT_KB="${MEMLIMIT_KB:-0}"
TIMEOUT="${TIMEOUT:-0}"

EXE="$BIN/examples/run_mxm"
[ -x "$EXE" ] || { echo "$EXE not found; build it with 'cmake --build \"$BIN\"'"; exit 1; }

mkdir -p "$OUT"
cp "$CONF" "$OUT/run.conf"
record_wide_base "$OUT/run.conf"
SUMMARY="$OUT/summary.csv"
echo "parallel_level;n_queries;n_skipped;basic_total_ms;basic_mean_ms;basic_median_ms;wp_total_ms;wp_mean_ms;wp_median_ms;speedup_basic;speedup_wp" > "$SUMMARY"

printf '%-15s %10s %9s %14s %12s %14s %14s %12s %16s %14s\n' \
       parallel_level n_queries n_skipped basic_total_ms basic_mean_ms basic_median_ms wp_total_ms wp_mean_ms wp_median_ms speedup

source "$SCRIPT_DIR/lib_levels.sh"

LEVELS=( $PARALLEL_LEVELS )
[ ${#LEVELS[@]} -gt 0 ] || { echo "PARALLEL_LEVELS vacio"; exit 1; }

# Una sola ejecucion con todos los niveles y repeticiones: el dataset se carga una vez.
build_run_args() {
    local run=( "$DATASET" "$QUERIES" "$UPPER_BOUND" "$1" --reps "$REPS" --grid_side "$GRID_SIDE" --time_out "$TIME_OUT"
                --mode "$MODE" --variant "$VARIANT" --results_file "$OUT/results_p{L}_r{R}.txt" --times_file "$OUT/times_p{L}_r{R}.txt" )
    RUN_ARGS=( "${run[@]}" "${WIDE_ARGS[@]}" )
}

baseline_basic=""
baseline_wp=""

on_level_ok() {
    local plevel=$1 result_line=$2
    IFS=';' read -r _ _ n_queries n_skipped b_total b_avg b_median w_total w_avg w_median <<< "$result_line"

    [ -z "$baseline_basic" ] && baseline_basic="$b_total"
    [ -z "$baseline_wp" ] && baseline_wp="$w_total"

    speedup_b=$(awk -v b="$baseline_basic" -v t="$b_total" 'BEGIN{ if (t+0>0) printf "%.2f", b/t; else print "-" }')
    speedup_w=$(awk -v b="$baseline_wp" -v t="$w_total" 'BEGIN{ if (t+0>0) printf "%.2f", b/t; else print "-" }')

    printf '%-15s %10s %9s %14s %12s %14s %14s %12s %16s %14s\n' \
           "$plevel" "$n_queries" "$n_skipped" "$b_total" "$b_avg" "$b_median" "$w_total" "$w_avg" "$w_median" "${speedup_b}/${speedup_w}"
    echo "${plevel};${n_queries};${n_skipped};${b_total};${b_avg};${b_median};${w_total};${w_avg};${w_median};${speedup_b};${speedup_w}" >> "$SUMMARY"
}

on_level_fail() {
    local plevel=$1 rc=$2 errtail=$3
        printf '%-15s %10s %9s %14s %12s %14s %14s %12s %16s %14s\n' "$plevel" - - - - - - - - "FAILED(rc=$rc)"
        echo "${plevel};;;;;;;;;;FAILED_rc${rc}" >> "$SUMMARY"
    [ -n "$errtail" ] && echo "$errtail"
}

on_level_skipped() {
        printf '%-15s %10s %9s %14s %12s %14s %14s %12s %16s %14s\n' "$1" - - - - - - - - "NOT_RUN"
        echo "${1};;;;;;;;;;NOT_RUN" >> "$SUMMARY"
}

run_levels_driver

baseline_level=$(echo $PARALLEL_LEVELS | awk '{print $1}')
echo
echo "speedup_basic/speedup_wp are relative to parallel_level=${baseline_level} (first of PARALLEL_LEVELS)."
echo "Per-query results: $OUT/results_p<N>_r<R>.txt | Per-query times: $OUT/times_p<N>_r<R>.txt"
echo "Summary: $SUMMARY"
