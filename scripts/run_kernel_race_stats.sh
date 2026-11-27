#!/bin/bash
# Corre examples/run_kernel_race (Kernel, version Raced) sobre un dataset+archivo de queries, una vez por cada nivel de
# PARALLEL_LEVELS, y arma una tabla de tiempo con speedup contra el primer nivel. Como run_wk_race_stats.sh y
# run_one_shot_race_stats.sh: carrera seq-vs-par por query, "ms" = tiempo de pared de la carrera completa.
#
# Kernel solo aplica a queries con variables lonely y non-lonely (a lo sumo 9 non-lonely): las demas se saltan y
# quedan en n_skipped. No hay modo adaptive.
#
# Uso: scripts/run_kernel_race_stats.sh <archivo.conf>
#
# Todos los PARALLEL_LEVELS (y REPS) corren en una ejecucion: el dataset se carga una vez. Si el proceso muere en
# un nivel (p.ej. SIGKILL por memoria), ese nivel queda FAILED_rc<N> en el resumen y se relanza con los que
# faltan (ver lib_levels.sh).
#
# El .conf es un archivo bash con asignaciones (plantilla: scripts/confs/kernel_wiki.conf). Variables:
#
#   DATASET          (obligatoria) directorio de indices ("<pid>"=SO, "<pid>s"=S, "<pid>o"=O), sin el archivo de queries
#   QUERIES          (obligatoria) archivo de queries (query_loader.hpp)
#   OUT              (obligatoria) directorio de salida. Por nivel y repeticion escribe results_p<N>_r<R>.txt
#                    ("query_id;kernel_result"; la cardinalidad no debe cambiar entre niveles/backends/corridas)
#                    y times_p<N>_r<R>.txt ("query_id;kernel_ms"), mas summary.csv (una fila por nivel, con speedup
#                    contra el primer nivel) y run.conf (copia de la configuracion).
#   BIN              carpeta de build de este repo          (default: <repo>/build)
#   VARIANT          backend: bv64|v5|v|sd|rrr|rrr15 (default bv64); el dataset debe ser de ese backend
#   VARIANT_OUT      (opcional) backend de salida del resultado final, distinto de
#                     VARIANT (p.ej. VARIANT=bv64 VARIANT_OUT=sd). Vacio = igual a VARIANT
#   UPPER_BOUND      cota de resultados por query, 0=sin limite (default: 0)
#   REPS             repeticiones por nivel (default 1); con REPS>1 el resumen muestra la media y cada una queda
#                    en results_p<N>_r<R>.txt / times_p<N>_r<R>.txt
#   GRID_SIDE        lado de la grilla logica de la query     (default: 52000000)
#   PARALLEL_LEVELS  niveles de corte a comparar, separados por espacio (default "0 2 4"; el primero es la base
#                    del speedup)
#   PARALLEL_PHASES  first_only|last_only|all -- que fases corren con PARALLEL_LEVEL:
#                     I, el join final o I + reducciones + final (default: last_only)
#   TIME_OUT         timeout de CADA CARRERA (por query), en segundos, 0=sin limite --
#                     se reenvia tal cual a raceStrategies (default: 0)
#   MEMLIMIT_KB      tope de memoria virtual por corrida (ulimit -v; default 0 = sin tope)
#   TIMEOUT          tope de tiempo de toda la ejecucion en segundos (default 0 = sin tope); al vencer, el nivel
#                    en curso queda FAILED_rc124, los que faltan NOT_RUN y no se relanza
#
#   WIDE_BASE        tables|scatter|automatic (default tables): como extienden los qdag un nodo base de d atributos
#                    en un join de dim atributos. tables = tablas de 16 bits; scatter = dispersion por bit + rango
#                    perezoso; automatic = dispersion desde d=4 en joins de 7 a 10 atributos (d=9 en dim=10 siempre
#                    dispersa). Ver wide_base_config.hpp
#   WIDE_MIN_D       con scatter, d minimo para dispersar (default 4)
#   WIDE_MIN_DIM     con scatter, dim minimo (default 7; solo existe de 7 a 10)
#
# Se puede ejecutar desde cualquier directorio.
#
# ./scripts/run_kernel_race_stats.sh scripts/confs/mi_corrida.conf

set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)

CONF="${1:?Usage: $0 <file.conf>}"
[ -f "$CONF" ] || { echo "config file not found: $CONF"; exit 1; }
# shellcheck source=/dev/null
source "$CONF"
source "$SCRIPT_DIR/lib_wide_base.sh"   # WIDE_BASE / WIDE_MIN_D / WIDE_MIN_DIM -> WIDE_ARGS

DATASET="${DATASET:?DATASET missing in $CONF (index directory, see generate_demo_dataset.cpp)}"
QUERIES="${QUERIES:?QUERIES missing in $CONF (text query file)}"
OUT="${OUT:?OUT missing in $CONF (output directory for results_p*.txt/times_p*.txt/summary.csv/run.conf)}"
BIN="${BIN:-$REPO_ROOT/build}"
VARIANT="${VARIANT:-bv64}"
VARIANT_OUT="${VARIANT_OUT:-}"
UPPER_BOUND="${UPPER_BOUND:-0}"
REPS="${REPS:-1}"
GRID_SIDE="${GRID_SIDE:-52000000}"
PARALLEL_LEVELS="${PARALLEL_LEVELS:-0 2 4}"
PARALLEL_PHASES="${PARALLEL_PHASES:-last_only}"
TIME_OUT="${TIME_OUT:-0}"
MEMLIMIT_KB="${MEMLIMIT_KB:-0}"
TIMEOUT="${TIMEOUT:-0}"

EXE="$BIN/examples/run_kernel_race"
VARIANT_OUT_ARGS=()
[ -n "$VARIANT_OUT" ] && VARIANT_OUT_ARGS=( --variant_out "$VARIANT_OUT" )
[ -x "$EXE" ] || { echo "$EXE not found; build it with 'cmake --build \"$BIN\"'"; exit 1; }

mkdir -p "$OUT"
cp "$CONF" "$OUT/run.conf"
record_wide_base "$OUT/run.conf"
SUMMARY="$OUT/summary.csv"
echo "parallel_level;n_queries;n_skipped;total_ms;mean_ms;median_ms;speedup" > "$SUMMARY"

printf '%-15s %10s %9s %14s %12s %12s %9s\n' \
       parallel_level n_queries n_skipped total_ms mean_ms median_ms speedup

source "$SCRIPT_DIR/lib_levels.sh"

LEVELS=( $PARALLEL_LEVELS )
[ ${#LEVELS[@]} -gt 0 ] || { echo "PARALLEL_LEVELS vacio"; exit 1; }

# Una sola ejecucion con todos los niveles y repeticiones: el dataset se carga una vez.
build_run_args() {
    RUN_ARGS=( "$DATASET" "$QUERIES" "$UPPER_BOUND" "$1" --reps "$REPS" --grid_side "$GRID_SIDE" --time_out "$TIME_OUT"
                --parallel_phases "$PARALLEL_PHASES"
                --variant "$VARIANT" "${VARIANT_OUT_ARGS[@]}" --results_file "$OUT/results_p{L}_r{R}.txt" --times_file "$OUT/times_p{L}_r{R}.txt" "${WIDE_ARGS[@]}" )
}

baseline=""

on_level_ok() {
    local plevel=$1 result_line=$2
    IFS=';' read -r _ _ n_queries n_skipped total avg median <<< "$result_line"

    [ -z "$baseline" ] && baseline="$total"

    speedup=$(awk -v b="$baseline" -v t="$total" 'BEGIN{ if (t+0>0) printf "%.2f", b/t; else print "-" }')

    printf '%-15s %10s %9s %14s %12s %12s %9s\n' \
           "$plevel" "$n_queries" "$n_skipped" "$total" "$avg" "$median" "$speedup"
    echo "${plevel};${n_queries};${n_skipped};${total};${avg};${median};${speedup}" >> "$SUMMARY"
}

on_level_fail() {
    local plevel=$1 rc=$2 errtail=$3
    printf '%-15s %10s %9s %14s %12s %12s %9s\n' "$plevel" - - - - - "FAILED(rc=$rc)"
    echo "${plevel};;;;;;FAILED_rc${rc}" >> "$SUMMARY"
    [ -n "$errtail" ] && echo "$errtail"
}

on_level_skipped() {
    printf '%-15s %10s %9s %14s %12s %12s %9s\n' "$1" - - - - - "NOT_RUN"
    echo "${1};;;;;;NOT_RUN" >> "$SUMMARY"
}

run_levels_driver

baseline_level=$(echo $PARALLEL_LEVELS | awk '{print $1}')
echo
echo "speedup is relative to parallel_level=${baseline_level} (first of PARALLEL_LEVELS)."
echo "Per-query results: $OUT/results_p<N>_r<R>.txt | Per-query times: $OUT/times_p<N>_r<R>.txt"
echo "Summary: $SUMMARY"
