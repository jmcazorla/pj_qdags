#ifndef QDAGS_EXAMPLES_CONST_RUNS_HPP
#define QDAGS_EXAMPLES_CONST_RUNS_HPP

#include <cstdint>
#include <string>

#include "qdags/index/se_quadtree.hpp"
#include "qdags/index/wide_base_config.hpp"
#include "qdags/query/constant_terms.hpp"
#include "qdags/query/resource_manager.hpp"
#include "qdags/query/triple_pattern.hpp"

// Puente entre un perfilador (run_wk, run_nl, run_leapfrog, run_kernel, run_orig) y el soporte de patrones con
// S u O constante. Aca solo hay declaraciones: las funciones se definen e instancian en run_<estrategia>_const.cpp,
// la unica unidad que incluye qdag_pin.hpp y query_builder_pin.hpp.
// Por que aparte: instanciar las estrategias para qdag_pin en el archivo del perfilador cambia el codigo generado
// para las queries de solo variables (decisiones de inlining de AND y partialAND).
// El perfilador llama a la funcion de su estrategia si gdj::has_constant_terms(query); imprimir, acumular y
// escribir archivos sigue siendo cosa suya.

namespace gdj_examples {

    // Parametros de una corrida; tipos basicos porque este header no conoce los enums de las estrategias.
    struct ConstRunArgs {
        uint64_t upper_bound = 0, parallel_level = 0, grid_side = 0, time_out = 0;
        uint64_t coef_bound = 0, init_coef = 2;
        uint32_t growth = 2, max_retries = 15;
        uint8_t growth_mode = 0;          // gdj::GrowthMode (0 = fixed, 1 = proportional)
        uint8_t parallel_phases = 0;      // gdj::ParallelPhases
        bool run_generic = true, run_adaptive = true;
        uint8_t lf_order = 0, lf_depth = 0;   // gdj::LeapfrogOrder / LeapfrogDepth
        uint64_t lf_max_atts = 0;
        gdj::wide_base_config wide;       // como extienden los qdag sus bases
    };

    struct ConstRunResult {
        bool ran = false;               // false: la estrategia no corrio esta query
        char skip_marker = '-';         // '-' no aplica, '+' supera el limite del motor
        std::string skip_reason;
        uint64_t generic_result = 0, adaptive_result = 0;
        double generic_ms = 0.0, adaptive_ms = 0.0;
        uint32_t attempts = 0;          // de la variante adaptive
        uint64_t coef_used = 0;
        bool exhaustive = false;
        uint64_t n_prejoins = 0, n_exact = 0;   // Leapfrog
    };

    // Una por estrategia: corren las variantes pedidas en `args` sobre una query con constantes y devuelven
    // conteos y tiempos (con el watchdog de timeout, como el bucle del perfilador).
    template<class RankBackend>
    ConstRunResult run_orig_const(const ConstRunArgs &args, gdj::resource_manager<gdj::se_quadtree<RankBackend>> &res_mgr,
                                  const gdj::query_tp &query);

    template<class RankBackend>
    ConstRunResult run_wk_const(const ConstRunArgs &args, gdj::resource_manager<gdj::se_quadtree<RankBackend>> &res_mgr,
                                const gdj::query_tp &query);

    // RankBackendOut: backend del resultado final (--variant_out).
    template<class RankBackendIn, class RankBackendOut>
    ConstRunResult run_kernel_const(const ConstRunArgs &args, gdj::resource_manager<gdj::se_quadtree<RankBackendIn>> &res_mgr,
                                    const gdj::query_tp &query);

    template<class RankBackendIn, class RankBackendOut>
    ConstRunResult run_leapfrog_const(const ConstRunArgs &args, gdj::resource_manager<gdj::se_quadtree<RankBackendIn>> &res_mgr,
                                      const gdj::query_tp &query);

} // namespace gdj_examples

#endif // QDAGS_EXAMPLES_CONST_RUNS_HPP
