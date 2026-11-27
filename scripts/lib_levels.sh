#!/bin/bash
# Biblioteca de los *_stats.sh: corre un ejecutable con todos los niveles de PARALLEL_LEVELS en una sola
# ejecucion (dataset cargado una vez), lee su salida en vivo y entrega cada nivel terminado a quien la usa.
# Se carga con: source "$SCRIPT_DIR/lib_levels.sh"
#
# El ejecutable recibe la lista de niveles ("0,2,4,6") y --reps, e imprime "LEVEL;<nivel>;<rep>" antes de cada
# pasada y su "RESULT;..." despues.
# Si el proceso muere en un nivel (SIGKILL por memoria, abort, bad_alloc...), ese nivel queda como fallo y se
# relanza con los que faltan (el dataset se recarga solo en ese caso). Con timeout global (rc=124) no se relanza:
# el nivel en curso queda como fallo y los demas como no corridos.
#
# El script que la usa define antes de llamar run_levels_driver:
#   EXE, REPS, MEMLIMIT_KB, TIMEOUT      variables (TIMEOUT cubre TODA la ejecucion)
#   LEVELS=( ... )                       niveles, en orden
#   build_run_args <niveles_csv>         llena el array RUN_ARGS (argumentos tras EXE)
#   on_level_ok <nivel> <linea_RESULT>   linea ya promediada sobre las repeticiones
#   on_level_fail <nivel> <rc> <stderr>  stderr: ultimas lineas, ya con sangria
#   on_level_skipped <nivel>             nivel no corrido por el timeout global
#   normalize_result                     (opcional) filtro stdin->stdout de cada RESULT

# Promedia las columnas numericas (desde la 5ta) de varias lineas RESULT; las 4 primeras son las de la primera.
_aggregate_results() {
    awk -F';' 'NR==1 { one=$0 } { n++; for (i=1;i<=NF;i++) { if (n==1 && i<=4) first[i]=$i; if (i>4) s[i]+=$i } if (NF>mx) mx=NF } END { if (n==1) { print one; exit } out=first[1]; for (i=2;i<=mx;i++) out=out ";" (i<=4 ? first[i] : sprintf("%.3f", s[i]/n)); print out }'
}

run_levels_driver() {
    local -a remaining=( "${LEVELS[@]}" )
    local has_normalize=0
    declare -F normalize_result >/dev/null && has_normalize=1
    local -A got=() n_got=() done_levels=()

    while [ ${#remaining[@]} -gt 0 ]; do
        local csv; csv=$(IFS=,; echo "${remaining[*]}")
        build_run_args "$csv"
        local -a cmd=( "$EXE" "${RUN_ARGS[@]}" )
        [ "${TIMEOUT:-0}" != 0 ] && cmd=( timeout "$TIMEOUT" "${cmd[@]}" )

        local errf; errf=$(mktemp)
        local cur_level="" cur_done=1 rc="" line agg
        got=(); n_got=(); done_levels=()

        while IFS= read -r line; do
            case "$line" in
                LEVEL\;*)
                    IFS=';' read -r _ cur_level _ <<< "$line"
                    cur_done=0 ;;
                RESULT\;*)
                    [ "$has_normalize" = 1 ] && line=$(printf '%s\n' "$line" | normalize_result)
                    got[$cur_level]+="$line"$'\n'
                    n_got[$cur_level]=$(( ${n_got[$cur_level]:-0} + 1 ))
                    if [ "${n_got[$cur_level]}" -ge "$REPS" ]; then
                        agg=$(printf '%s' "${got[$cur_level]}" | _aggregate_results)
                        on_level_ok "$cur_level" "$agg"
                        cur_done=1; done_levels[$cur_level]=1
                    fi ;;
                __RC__\;*) rc=${line#__RC__;} ;;
            esac
        done < <(
            if [ "${MEMLIMIT_KB:-0}" != 0 ]; then ulimit -v "$MEMLIMIT_KB"; fi
            "${cmd[@]}" 2>"$errf"
            echo "__RC__;$?"
        )

        local errtail; errtail=$(tail -n 3 "$errf" | sed 's/^/    /'); rm -f "$errf"

        if [ "$rc" = 0 ]; then
            local l
            for l in "${remaining[@]}"; do
                [ -n "${done_levels[$l]:-}" ] || on_level_fail "$l" "sin_resultado" "$errtail"
            done
            return 0
        fi

        # El proceso murio: falla el nivel en curso o, si ya habia terminado, el siguiente (el que iba a empezar).
        local idx=-1 i
        for i in "${!remaining[@]}"; do [ "${remaining[$i]}" = "$cur_level" ] && idx=$i; done
        if [ -z "$cur_level" ]; then
            local l
            for l in "${remaining[@]}"; do on_level_fail "$l" "$rc" "$errtail"; done
            return 1
        fi
        local failed_idx=$idx
        if [ "$cur_done" = 1 ]; then
            failed_idx=$(( idx + 1 ))
            if [ "$failed_idx" -ge "${#remaining[@]}" ]; then
                echo "warning: process exited with rc=$rc after completing all levels" >&2
                return 0
            fi
        fi
        on_level_fail "${remaining[$failed_idx]}" "$rc" "$errtail"

        if [ "$rc" = 124 ]; then
            local j
            for (( j=failed_idx+1; j<${#remaining[@]}; j++ )); do on_level_skipped "${remaining[$j]}"; done
            return 1
        fi
        remaining=( "${remaining[@]:$(( failed_idx + 1 ))}" )
    done
    return 0
}
