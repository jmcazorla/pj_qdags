# Arma los argumentos del camino de dispersion de qdag (--wide_base, --wide_min_d, --wide_min_dim) desde las
# variables del .conf (todas opcionales). Los run_*_stats.sh la cargan despues del .conf y agregan
# "${WIDE_ARGS[@]}" a la linea de comandos. Ver include/qdags/index/wide_base_config.hpp.
#
#   WIDE_BASE=tables|scatter|automatic
#       tables (default): tablas de 16 bits. scatter: dispersion por bit + rango perezoso para las bases de
#       d >= WIDE_MIN_D en joins de dim >= WIDE_MIN_DIM. automatic: la biblioteca elige (dispersion desde d=4 en
#       joins de 7 a 10 atributos). Una base de d=9 en dim=10 siempre dispersa.
#   WIDE_MIN_D=4     solo scatter: d minimo para dispersar
#   WIDE_MIN_DIM=7   solo scatter: dim minimo (la dispersion solo existe para joins de 7 a 10 atributos)
#
# Define WIDE_ARGS (siempre con --wide_base), agrega un sufijo a OUT cuando el modo no es tables y define
# record_wide_base (ver abajo). Un valor invalido aborta antes de crear carpetas.
# Los motores resumables (wk, plan, leapfrog) no soportan la dispersion: con automatic usan tablas; con scatter y
# bases que la usen fallan con un mensaje claro (el default WIDE_MIN_D=4 no alcanza las bases de wk).
WIDE_BASE="${WIDE_BASE:-tables}"
case "$WIDE_BASE" in
    tables|scatter|automatic) ;;
    *) echo "WIDE_BASE='$WIDE_BASE' is not valid (tables|scatter|automatic)" >&2; exit 1 ;;
esac
WIDE_ARGS=( --wide_base "$WIDE_BASE" )

# Sufijo de OUT con un modo distinto de tables: <OUT>_scatter_d<min_d>_dim<min_dim> (ej. <OUT>_scatter_d4_dim7) o
# <OUT>_automatic, para no pisar los resultados con tablas ni los de otros umbrales. Los defaults de scatter (4 y 7)
# deben coincidir con los de wide_base_config.hpp.
WIDE_OUT_SUFFIX=""
if [ "$WIDE_BASE" = "scatter" ]; then
    WIDE_MIN_D="${WIDE_MIN_D:-4}"
    WIDE_MIN_DIM="${WIDE_MIN_DIM:-7}"
    case "$WIDE_MIN_D$WIDE_MIN_DIM" in
        ''|*[!0-9]*) echo "WIDE_MIN_D='$WIDE_MIN_D' and WIDE_MIN_DIM='$WIDE_MIN_DIM' must be integers" >&2; exit 1 ;;
    esac
    WIDE_ARGS+=( --wide_min_d "$WIDE_MIN_D" --wide_min_dim "$WIDE_MIN_DIM" )
    WIDE_OUT_SUFFIX="_scatter_d${WIDE_MIN_D}_dim${WIDE_MIN_DIM}"
elif [ "$WIDE_BASE" = "automatic" ]; then
    WIDE_OUT_SUFFIX="_automatic"
fi
if [ -n "$WIDE_OUT_SUFFIX" ] && [ -n "${OUT:-}" ]; then
    OUT="${OUT%/}${WIDE_OUT_SUFFIX}"
    echo "[wide_base] WIDE_BASE=$WIDE_BASE: output folder $OUT"
fi

# Anota al final de un run.conf la configuracion efectiva de dispersion (tambien si vino del entorno), con los
# nombres WIDE_* del .conf: asi el run.conf se puede reusar como .conf y reproduce modo y carpeta de salida (el
# sufijo se recalcula). Los umbrales solo con scatter. Se llama despues de copiar el .conf:
#     cp "$CONF" "$OUT/run.conf"; record_wide_base "$OUT/run.conf"
record_wide_base() {
    local f="$1"
    {
        echo
        echo "# --- effective scatter configuration (written by scripts/lib_wide_base.sh; from the .conf or the environment) ---"
        echo "WIDE_BASE=$WIDE_BASE"
        if [ "$WIDE_BASE" = "scatter" ]; then
            echo "WIDE_MIN_D=$WIDE_MIN_D"
            echo "WIDE_MIN_DIM=$WIDE_MIN_DIM"
        fi
        if [ -n "$WIDE_OUT_SUFFIX" ]; then
            echo "# effective output folder: ${OUT:-}"
        fi
    } >> "$f"
}
true
