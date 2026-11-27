#!/bin/bash
# Corre examples/run_plan_hybrid (Plan + fallback) sobre un dataset+archivo de queries, una vez por cada nivel de
# PARALLEL_LEVELS: las queries cuya topologia tiene plan usan el plan y las demas todas las estrategias de FALLBACKS
# (one_shot, wk, leapfrog, kernel, orig), cada una con su tiempo.
#
# El plan se resuelve como en run_plan_stats.sh: PLAN_GROUP (catalogo, admite topologia mixta) o PLAN_FILE + PLAN_ID
# (plan fijo, solo para las topologias de su archivo; ver PLAN_TOPOLOGIES). Detalle en examples/run_plan_hybrid.cpp.
#
# Uso: scripts/run_plan_hybrid_stats.sh <archivo.conf>
#
# Todos los PARALLEL_LEVELS (y REPS) corren en una ejecucion: el dataset se carga una vez. Si el proceso muere en
# un nivel (p.ej. SIGKILL por memoria), ese nivel queda FAILED_rc<N> en el resumen y se relanza con los que
# faltan (ver lib_levels.sh).
#
# El .conf es un archivo bash con asignaciones (plantilla: scripts/confs/*.conf.example). Variables:
#
#   DATASET          (obligatoria) directorio de indices (convencion
#                     resource_manager -- "<pid>"=SO, "<pid>s"=S, "<pid>o"=O)
#   QUERIES          (obligatoria) archivo de queries de texto (puede mezclar
#                     topologias)
#   FALLBACKS        (obligatoria) estrategias para las queries sin plan,
#                     separadas por espacio, en el orden en que se reportan:
#                     one_shot wk leapfrog kernel orig (one_shot = nl en el codigo,
#                     "nl" tambien se acepta). Cada una corre las variantes de
#                     MODE que tiene: one_shot y wk, las tres; leapfrog, generic y
#                     adaptive; kernel y orig, solo generic (si no tiene ninguna
#                     de las pedidas, no corre)
#   PLAN_GROUP       normal|best|wk_k|leapfrog -- alternativa a PLAN_FILE/PLAN_ID
#   PLAN_FILE        archivo de plan (formato PlanParser) -- junto con PLAN_ID
#   PLAN_ID          que plan del archivo usar -- junto con PLAN_FILE
#   PLAN_TOPOLOGIES  (solo con PLAN_FILE) topologias a las que se aplica el plan,
#                     separadas por espacio (p.ej. "P2 T2 Ti2"). Por defecto, las
#                     que el catalogo asocia al mismo archivo; si el archivo no
#                     esta en el catalogo hay que pasarlas
#   OUT              (obligatoria) directorio de salida -- por cada
#                     PARALLEL_LEVEL y repeticion se escriben ahi:
#                       results_p<N>_r<R>.txt  query_id;topology;engine;plan_*;<f>_*...
#                       times_p<N>_r<R>.txt    mismas columnas, en ms
#                     (engine = plan | fallback; las columnas que no aplican a la
#                     fila quedan vacias; "-" = la estrategia no aplica a la query,
#                     "+" = supera el limite del motor) mas summary.csv (una fila
#                     agregada por PARALLEL_LEVEL, con el speedup de cada grupo contra
#                     el primer nivel de la lista) y run.conf (copia de este archivo)
#   BIN              carpeta de build de este repo          (default: <repo>/build)
#   VARIANT          backend: bv64|v5|v|sd|rrr|rrr15 (default bv64)
#   UPPER_BOUND      cota de resultados por query, 0=sin limite (default: 0). Con
#                     UPPER_BOUND=0 y COEF_BOUND=0 el ejecutable comprueba que todas
#                     las estrategias de una query den el mismo conteo (n_mismatch)
#   REPS             repeticiones por nivel de paralelismo (default: 1). Con REPS>1 las columnas
#                    numericas del resumen son la media sobre las repeticiones
#   COEF_BOUND       cota de coeficiente de "generic"          (default: 0)
#   GRID_SIDE        lado de la grilla logica de la query     (default: 52000000)
#   PARALLEL_LEVELS  niveles de corte a comparar, separados por espacio (default "0 2 4"; el primero es la base
#                    del speedup)
#   MODE             variantes a correr: lista separada por comas de generic,
#                     adaptive, resumable; both = generic,adaptive; all = las tres
#                     (default: both). El plan tiene las tres
#   INIT_COEF        coeficiente inicial de "adaptive"        (default: 2)
#   GROWTH           factor de crecimiento de "adaptive"      (default: 2)
#   GROWTH_MODE      fixed|proportional: crecimiento del coeficiente entre intentos (default fixed). fixed = c * GROWTH;
#                    proportional = c * clamp(L / r * 1.5, GROWTH, 64), con r los resultados del intento que no llego
#                    a UPPER_BOUND (r = 0: c * 4); GROWTH pasa a ser el factor minimo.
#   MAX_RETRIES      maximo de reintentos de "adaptive", 0=sin tope (default: 15)
#   PARALLEL_PHASES  first_only|last_only|all -- que fases corren con
#                     PARALLEL_LEVEL                           (default: last_only)
#   MXM, K           rama mxm del plan (ver run_plan_stats.sh)  (default: false, 2)
#   LF_ORDER         proj_size|degree -- orden de variables de leapfrog (default: proj_size)
#   LF_DEPTH         full|non_lonely -- profundidad de leapfrog         (default: full)
#   LF_MAX_ATTS      tope de atributos por prejoin de leapfrog, 0=auto  (default: 0)
#   TIME_OUT         timeout POR QUERY y por estrategia, en segundos, 0=sin limite (default: 0)
#   MEMLIMIT_KB      tope de memoria virtual por corrida (ulimit -v; default 0 = sin tope)
#   TIMEOUT          tope de tiempo de toda la ejecucion en segundos (default 0 = sin tope); al vencer, el nivel
#                    en curso queda FAILED_rc124, los que faltan NOT_RUN y no se relanza
#
# ./scripts/run_plan_hybrid_stats.sh scripts/confs/mi_corrida.conf

set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)

CONF="${1:?Usage: $0 <file.conf>}"
[ -f "$CONF" ] || { echo "config file not found: $CONF"; exit 1; }
# shellcheck source=/dev/null
source "$CONF"
source "$SCRIPT_DIR/lib_wide_base.sh"   # WIDE_BASE / WIDE_MIN_D / WIDE_MIN_DIM -> WIDE_ARGS

DATASET="${DATASET:?DATASET missing in $CONF (index directory)}"
QUERIES="${QUERIES:?QUERIES missing in $CONF (text query file)}"
FALLBACKS="${FALLBACKS:?FALLBACKS missing in $CONF (e.g. \"one_shot wk leapfrog\")}"
PLAN_FILE="${PLAN_FILE:-}"
PLAN_ID="${PLAN_ID:-}"
PLAN_GROUP="${PLAN_GROUP:-}"
PLAN_TOPOLOGIES="${PLAN_TOPOLOGIES:-}"
if [ -n "$PLAN_GROUP" ] && { [ -n "$PLAN_FILE" ] || [ -n "$PLAN_ID" ]; }; then
    echo "error in $CONF: PLAN_GROUP cannot be combined with PLAN_FILE/PLAN_ID (pick one)"; exit 1
fi
if [ -z "$PLAN_GROUP" ] && { [ -z "$PLAN_FILE" ] || [ -z "$PLAN_ID" ]; }; then
    echo "error in $CONF: PLAN_GROUP or the PLAN_FILE+PLAN_ID pair is required"; exit 1
fi
OUT="${OUT:?OUT missing in $CONF (output directory for results_p*.txt/times_p*.txt/summary.csv/run.conf)}"
BIN="${BIN:-$REPO_ROOT/build}"
VARIANT="${VARIANT:-bv64}"
UPPER_BOUND="${UPPER_BOUND:-0}"
REPS="${REPS:-1}"
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
MXM="${MXM:-false}"
K="${K:-2}"
LF_ORDER="${LF_ORDER:-proj_size}"
LF_DEPTH="${LF_DEPTH:-full}"
LF_MAX_ATTS="${LF_MAX_ATTS:-0}"
TIME_OUT="${TIME_OUT:-0}"
MEMLIMIT_KB="${MEMLIMIT_KB:-0}"
TIMEOUT="${TIMEOUT:-0}"

EXE="$BIN/examples/run_plan_hybrid"
[ -x "$EXE" ] || { echo "$EXE not found; build it with 'cmake --build \"$BIN\"'"; exit 1; }

FB_LIST=( $FALLBACKS )
[ ${#FB_LIST[@]} -gt 0 ] || { echo "FALLBACKS vacio en $CONF"; exit 1; }
FB_CSV=$(IFS=,; echo "${FB_LIST[*]}")
PLAN_TOPO_CSV=$(echo $PLAN_TOPOLOGIES | tr ' ' ',')

# Grupos que se reportan: el plan y cada estrategia del fallback.
GROUPS_LIST=( plan "${FB_LIST[@]}" )

mkdir -p "$OUT"
cp "$CONF" "$OUT/run.conf"
record_wide_base "$OUT/run.conf"
SUMMARY="$OUT/summary.csv"
MODE_NAMES=( generic adaptive resumable )
header="parallel_level;n_queries;n_plan;n_fallback;n_mismatch"
for m in "${MODE_NAMES[@]}"; do header+=";plan_${m}_total_ms;plan_${m}_mean_ms;plan_${m}_median_ms"; done
for f in "${FB_LIST[@]}"; do
    header+=";${f}_skipped"
    for m in "${MODE_NAMES[@]}"; do header+=";${f}_${m}_total_ms;${f}_${m}_mean_ms;${f}_${m}_median_ms"; done
done
for g in "${GROUPS_LIST[@]}"; do header+=";speedup_${g}"; done
echo "$header" > "$SUMMARY"

# Tabla de pantalla: una columna por grupo con su tiempo total generic/adaptive/resumable.
table_row() {
    local lvl=$1 nq=$2 np=$3 nf=$4 nm=$5; shift 5
    printf '%-15s %9s %7s %9s %9s' "$lvl" "$nq" "$np" "$nf" "$nm"
    local c; for c in "$@"; do printf ' %26s' "$c"; done
    printf '\n'
}
cols=()
for g in "${GROUPS_LIST[@]}"; do cols+=( "${g}_g/a/r_ms" ); done
table_row parallel_level n_queries n_plan n_fallback n_mismatch "${cols[@]}"

source "$SCRIPT_DIR/lib_levels.sh"

LEVELS=( $PARALLEL_LEVELS )
[ ${#LEVELS[@]} -gt 0 ] || { echo "PARALLEL_LEVELS vacio"; exit 1; }

# Una sola ejecucion con todos los niveles y repeticiones: el dataset se carga una vez.
build_run_args() {
    local run=( "$DATASET" "$QUERIES" "$UPPER_BOUND" "$1" --reps "$REPS" --fallback "$FB_CSV"
                --grid_side "$GRID_SIDE" --time_out "$TIME_OUT" --coef_bound "$COEF_BOUND"
                --init_coef "$INIT_COEF" --growth "$GROWTH" --growth_mode "$GROWTH_MODE" --max_retries "$MAX_RETRIES" --parallel_phases "$PARALLEL_PHASES"
                --k "$K" --lf_order "$LF_ORDER" --lf_depth "$LF_DEPTH" --lf_max_atts "$LF_MAX_ATTS"
                --mode "$MODE" --variant "$VARIANT" --results_file "$OUT/results_p{L}_r{R}.txt" --times_file "$OUT/times_p{L}_r{R}.txt" )
    if [ -n "$PLAN_GROUP" ]; then
        run+=( --plan_group "$PLAN_GROUP" )
    else
        run+=( --plan_file "$PLAN_FILE" --plan_id "$PLAN_ID" )
        [ -n "$PLAN_TOPO_CSV" ] && run+=( --plan_topologies "$PLAN_TOPO_CSV" )
    fi
    [ "$MXM" = "true" ] && run+=( --mxm )
    RUN_ARGS=( "${run[@]}" "${WIDE_ARGS[@]}" )
}

declare -A baseline=() base_mode=()

# Linea RESULT (ya promediada sobre las repeticiones):
#   RESULT;variant;n_queries;n_plan;n_fallback;n_mismatch;
#   plan: g_total;g_mean;g_median;a_total;a_mean;a_median;r_total;r_mean;r_median; y por estrategia:
#   skipped;g_total;g_mean;g_median;a_total;a_mean;a_median;r_total;r_mean;r_median   (g=generic a=adaptive r=resumable)
on_level_ok() {
    local plevel=$1 result_line=$2
    local -a f
    IFS=';' read -r -a f <<< "$result_line"
    local nq np nf_ nm
    nq=$(printf '%.0f' "${f[2]}"); np=$(printf '%.0f' "${f[3]}"); nf_=$(printf '%.0f' "${f[4]}"); nm=$(printf '%.0f' "${f[5]}")

    # Campos de cada grupo: el plan empieza en 6 (9 campos: total, media y mediana de generic, adaptive y
    # resumable); cada estrategia, en 15 + 10*i (skipped + 9).
    local -a cells=() totals=()
    local csv="${plevel};${nq};${np};${nf_};${nm};${f[6]};${f[7]};${f[8]};${f[9]};${f[10]};${f[11]};${f[12]};${f[13]};${f[14]}"
    cells+=( "${f[6]}/${f[9]}/${f[12]}" )
    totals+=( "${f[6]}" "${f[9]}" "${f[12]}" )
    local i base
    for i in "${!FB_LIST[@]}"; do
        base=$(( 15 + 10 * i ))
        csv+=";$(printf '%.0f' "${f[$base]}");${f[$((base+1))]};${f[$((base+2))]};${f[$((base+3))]};${f[$((base+4))]};${f[$((base+5))]};${f[$((base+6))]};${f[$((base+7))]};${f[$((base+8))]};${f[$((base+9))]}"
        cells+=( "${f[$((base+1))]}/${f[$((base+4))]}/${f[$((base+7))]}" )
        totals+=( "${f[$((base+1))]}" "${f[$((base+4))]}" "${f[$((base+7))]}" )
    done

    # Speedup de cada grupo: sobre el primer modo (generic, adaptive, resumable) que corre
    # en ese grupo, el mismo en todos los niveles.
    local k m sp g
    for k in "${!GROUPS_LIST[@]}"; do
        g=${GROUPS_LIST[$k]}
        if [ -z "${base_mode[$g]:-}" ]; then
            base_mode[$g]=-1
            for m in 0 1 2; do
                if awk -v t="${totals[$((3*k+m))]}" 'BEGIN{exit !(t+0>0)}'; then base_mode[$g]=$m; baseline[$g]="${totals[$((3*k+m))]}"; break; fi
            done
        fi
        if [ "${base_mode[$g]}" = -1 ]; then sp="-"
        else sp=$(awk -v b="${baseline[$g]}" -v t="${totals[$((3*k+${base_mode[$g]}))]}" 'BEGIN{ if (t+0>0) printf "%.2f", b/t; else print "-" }'); fi
        csv+=";${sp}"
    done

    table_row "$plevel" "$nq" "$np" "$nf_" "$nm" "${cells[@]}"
    echo "$csv" >> "$SUMMARY"
    if [ "$nm" != 0 ]; then
        echo "  warning: n_mismatch=$nm at level $plevel (strategies disagree; see MISMATCH in the executable output)"
    fi
}

on_level_fail() {
    local plevel=$1 rc=$2 errtail=$3
    local -a dashes=(); local g
    for g in "${GROUPS_LIST[@]}"; do dashes+=( - ); done
    table_row "$plevel" - - - "FAILED(rc=$rc)" "${dashes[@]}"
    echo "${plevel};FAILED_rc${rc}" >> "$SUMMARY"
    [ -n "$errtail" ] && echo "$errtail"
}

on_level_skipped() {
    local -a dashes=(); local g
    for g in "${GROUPS_LIST[@]}"; do dashes+=( - ); done
    table_row "$1" - - - "NOT_RUN" "${dashes[@]}"
    echo "${1};NOT_RUN" >> "$SUMMARY"
}

run_levels_driver

baseline_level=$(echo $PARALLEL_LEVELS | awk '{print $1}')
echo
echo "Each group column is generic/adaptive/resumable (total ms; 0 if the group does not run that variant). Speedup (summary.csv) is for the first mode each group runs, relative to parallel_level=${baseline_level}."
echo "Per-query results: $OUT/results_p<N>_r<R>.txt | Per-query times: $OUT/times_p<N>_r<R>.txt"
echo "Summary: $SUMMARY"
