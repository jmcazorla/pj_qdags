#!/bin/bash
# Corre examples/run_wk (WeakKernel: generic + adaptive) sobre un dataset+archivo de queries, una vez por cada nivel
# de PARALLEL_LEVELS, y arma una tabla de tiempo con speedup contra el primer nivel. Tabula las dos series, una fila
# por nivel.
#
# "generic" es NewFixed (coeficiente fijo COEF_BOUND); "adaptive" reintenta con un coeficiente creciente hasta
# alcanzar UPPER_BOUND. A diferencia de One-Shot (run_one_shot_stats.sh), WeakKernel no distingue variables
# non-lonely de antemano: sirve para topologia desconocida.
#
# Uso: scripts/run_wk_stats.sh <archivo.conf>
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
#                    ("query_id;generic_result;adaptive_result"; la cardinalidad no debe cambiar entre niveles/backends/corridas)
#                    y times_p<N>_r<R>.txt ("query_id;generic_ms;adaptive_ms"), mas summary.csv (una fila por nivel, con speedup
#                    contra el primer nivel) y run.conf (copia de la configuracion).
#   BIN              carpeta de build de este repo          (default: <repo>/build)
#   VARIANT          backend: bv64|v5|v|sd|rrr|rrr15 (default bv64); el dataset debe ser de ese backend
#   VARIANT_OUT      (opcional) backend de salida distinto de VARIANT, p.ej. VARIANT=bv64
#                     VARIANT_OUT=sd: usa examples/run_wk_mixed (solo fases 3 de WeakKernel
#                     cambian de backend). Vacio = igual a VARIANT (run_wk)
#   UPPER_BOUND      cota de resultados por query, 0=sin limite (default: 0)
#   REPS             repeticiones por nivel (default 1); con REPS>1 el resumen muestra la media y cada una queda
#                    en results_p<N>_r<R>.txt / times_p<N>_r<R>.txt
#   COEF_BOUND       cota de coeficiente para "generic" (no adaptativa), 0=sin cota en esa fase (default: 0)
#   MXM              true|false: la query es una cadena de matrices (P2/P3/P4); se mide C = pi_{extremos}(resultado)
#                    en vez del join completo (--mxm; exige UPPER_BOUND=0) (default false)
#   GRID_SIDE        lado de la grilla logica de la query     (default: 52000000)
#   PARALLEL_LEVELS  niveles de corte a comparar, separados por espacio (default "0 2 4"; el primero es la base
#                    del speedup)
#   MODE             generic|adaptive|both: que variante(s) correr por query (default both)
#   INIT_COEF        cota de coeficiente inicial de "adaptive" (default: 2)
#   GROWTH           factor de crecimiento del coeficiente por reintento de "adaptive" (default 2)
#   GROWTH_MODE      fixed|proportional: crecimiento del coeficiente entre intentos (default fixed). fixed = c * GROWTH;
#                    proportional = c * clamp(L / r * 1.5, GROWTH, 64), con r los resultados del intento que no llego
#                    a UPPER_BOUND (r = 0: c * 4); GROWTH pasa a ser el factor minimo.
#   MAX_RETRIES      maximo de reintentos de "adaptive", 0=sin tope (default: 15)
#   PARALLEL_PHASES  first_only|last_only|all -- que fases de WeakKernel
#                     corren con PARALLEL_LEVEL                (default: last_only)
#   TIME_OUT         timeout POR QUERY en segundos, 0=sin limite (default: 0)
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
# ./scripts/run_wk_stats.sh scripts/confs/mi_corrida.conf

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
MXM="${MXM:-false}"
MXM_ARGS=()
[ "$MXM" = true ] && MXM_ARGS=( --mxm )
COEF_BOUND="${COEF_BOUND:-0}"
GRID_SIDE="${GRID_SIDE:-52000000}"
PARALLEL_LEVELS="${PARALLEL_LEVELS:-0 2 4}"
MODE="${MODE:-both}"
INIT_COEF="${INIT_COEF:-2}"
GROWTH="${GROWTH:-2}"
GROWTH_MODE="${GROWTH_MODE:-fixed}"
case "$GROWTH_MODE" in fixed|proportional) ;; *) echo "invalid GROWTH_MODE: $GROWTH_MODE (fixed|proportional)"; exit 1 ;; esac
MAX_RETRIES="${MAX_RETRIES:-15}"
PARALLEL_PHASES="${PARALLEL_PHASES:-last_only}"
TIME_OUT="${TIME_OUT:-0}"
MEMLIMIT_KB="${MEMLIMIT_KB:-0}"
TIMEOUT="${TIMEOUT:-0}"

if [ -n "$VARIANT_OUT" ]; then
    EXE="$BIN/examples/run_wk_mixed"
    VARIANT_ARGS=( --variant_in "$VARIANT" --variant_out "$VARIANT_OUT" )
else
    EXE="$BIN/examples/run_wk"
    VARIANT_ARGS=( --variant "$VARIANT" )
fi
[ -x "$EXE" ] || { echo "$EXE not found; build it with 'cmake --build \"$BIN\"'"; exit 1; }

# run_wk_mixed agrega el backend de salida como 3er campo de la linea RESULT;
# se quita para dejar el mismo formato que run_wk
normalize_result() {
    if [ -n "$VARIANT_OUT" ]; then sed -E 's/^(RESULT;[^;]*);[^;]*;/\1;/'; else cat; fi
}

mkdir -p "$OUT"
cp "$CONF" "$OUT/run.conf"
record_wide_base "$OUT/run.conf"
SUMMARY="$OUT/summary.csv"
echo "parallel_level;n_queries;n_skipped;generic_total_ms;generic_mean_ms;generic_median_ms;adaptive_total_ms;adaptive_mean_ms;adaptive_median_ms;speedup_generic;speedup_adaptive" > "$SUMMARY"

printf '%-15s %10s %9s %16s %14s %16s %17s %16s %18s %14s\n' \
       parallel_level n_queries n_skipped generic_total_ms generic_mean_ms generic_median_ms adaptive_total_ms adaptive_mean_ms adaptive_median_ms speedup

source "$SCRIPT_DIR/lib_levels.sh"

LEVELS=( $PARALLEL_LEVELS )
[ ${#LEVELS[@]} -gt 0 ] || { echo "PARALLEL_LEVELS vacio"; exit 1; }

# Una sola ejecucion con todos los niveles y repeticiones: el dataset se carga una vez.
build_run_args() {
    RUN_ARGS=( "$DATASET" "$QUERIES" "$UPPER_BOUND" "$1" --reps "$REPS" --grid_side "$GRID_SIDE" --coef_bound "$COEF_BOUND" --time_out "$TIME_OUT"
                --init_coef "$INIT_COEF" --growth "$GROWTH" --growth_mode "$GROWTH_MODE" --max_retries "$MAX_RETRIES" --parallel_phases "$PARALLEL_PHASES"
                --mode "$MODE" "${VARIANT_ARGS[@]}" --results_file "$OUT/results_p{L}_r{R}.txt" --times_file "$OUT/times_p{L}_r{R}.txt" "${WIDE_ARGS[@]}" "${MXM_ARGS[@]}" )
}

baseline_generic=""
baseline_adaptive=""

on_level_ok() {
    local plevel=$1 result_line=$2
    IFS=';' read -r _ _ n_queries n_skipped g_total g_avg g_median a_total a_avg a_median <<< "$result_line"

    [ -z "$baseline_generic" ] && baseline_generic="$g_total"
    [ -z "$baseline_adaptive" ] && baseline_adaptive="$a_total"

    speedup_g=$(awk -v b="$baseline_generic" -v t="$g_total" 'BEGIN{ if (t+0>0) printf "%.2f", b/t; else print "-" }')
    speedup_a=$(awk -v b="$baseline_adaptive" -v t="$a_total" 'BEGIN{ if (t+0>0) printf "%.2f", b/t; else print "-" }')

    printf '%-15s %10s %9s %16s %14s %16s %17s %16s %18s %14s\n' \
           "$plevel" "$n_queries" "$n_skipped" "$g_total" "$g_avg" "$g_median" "$a_total" "$a_avg" "$a_median" "${speedup_g}/${speedup_a}"
    echo "${plevel};${n_queries};${n_skipped};${g_total};${g_avg};${g_median};${a_total};${a_avg};${a_median};${speedup_g};${speedup_a}" >> "$SUMMARY"
}

on_level_fail() {
    local plevel=$1 rc=$2 errtail=$3
    printf '%-15s %10s %9s %16s %14s %16s %17s %16s %18s %14s\n' "$plevel" - - - - - - - - "FAILED(rc=$rc)"
    echo "${plevel};;;;;;;;;;FAILED_rc${rc}" >> "$SUMMARY"
    [ -n "$errtail" ] && echo "$errtail"
}

on_level_skipped() {
    printf '%-15s %10s %9s %16s %14s %16s %17s %16s %18s %14s\n' "$1" - - - - - - - - "NOT_RUN"
    echo "${1};;;;;;;;;;NOT_RUN" >> "$SUMMARY"
}

run_levels_driver

baseline_level=$(echo $PARALLEL_LEVELS | awk '{print $1}')
echo
echo "speedup_generic/speedup_adaptive are relative to parallel_level=${baseline_level} (first of PARALLEL_LEVELS)."
echo "Per-query results: $OUT/results_p<N>_r<R>.txt | Per-query times: $OUT/times_p<N>_r<R>.txt"
echo "Summary: $SUMMARY"
