#ifndef QDAGS_EXAMPLES_CONST_RUNS_MORE_HPP
#define QDAGS_EXAMPLES_CONST_RUNS_MORE_HPP

#include <cstdint>
#include <string>

#include "const_runs.hpp"

// Constantes en patrones para los perfiladores mixed, resumable y race (run_wk_mixed, run_wk_resumable,
// run_nl_race, run_wk_race, run_leapfrog_resumable, run_leapfrog_race, run_kernel_race). Mismo esquema y
// motivo que const_runs.hpp.

namespace gdj_examples {

    // One-Shot (solo generic; usa generic_result/generic_ms). El resultado final puede ir en otro backend (--variant_out).
    template<class RankBackendIn, class RankBackendOut>
    ConstRunResult run_nl_const(const ConstRunArgs &args, gdj::resource_manager<gdj::se_quadtree<RankBackendIn>> &res_mgr,
                                const gdj::query_tp &query);

    // WK mixed (--variant_in/--variant_out); usa generic_*/adaptive_*.
    template<class RankBackendIn, class RankBackendOut>
    ConstRunResult run_wk_mixed_const(const ConstRunArgs &args, gdj::resource_manager<gdj::se_quadtree<RankBackendIn>> &res_mgr,
                                      const gdj::query_tp &query);

    // WK resumable: una variante por query (generic_result/generic_ms, attempts/coef_used/exhaustive).
    template<class RankBackendIn, class RankBackendOut>
    ConstRunResult run_wk_resumable_const(const ConstRunArgs &args, gdj::resource_manager<gdj::se_quadtree<RankBackendIn>> &res_mgr,
                                          const gdj::query_tp &query);

    // Race: secuencial contra paralelo, gana la que termine primero.
    struct ConstRaceResult {
        bool ran = false;
        char skip_marker = '-';
        std::string skip_reason;
        uint64_t generic_result = 0, adaptive_result = 0;
        double generic_ms = 0.0, adaptive_ms = 0.0;      // tiempo de pared de la carrera
        int generic_winner = -1, adaptive_winner = -1;   // 0 = seq, 1 = par
        double seq_time = -1.0, par_time = -1.0;         // segundos, variante generic
        uint32_t seq_attempts = 0, par_attempts = 0;     // variante adaptive
        uint64_t n_prejoins = 0, n_exact = 0;            // Leapfrog
    };

    // Race: RankBackendOut es el backend del resultado final de cada rama (--variant_out).
    template<class RankBackendIn, class RankBackendOut>
    ConstRaceResult run_nl_race_const(const ConstRunArgs &args, gdj::resource_manager<gdj::se_quadtree<RankBackendIn>> &res_mgr,
                                      const gdj::query_tp &query);

    template<class RankBackendIn, class RankBackendOut>
    ConstRaceResult run_wk_race_const(const ConstRunArgs &args, gdj::resource_manager<gdj::se_quadtree<RankBackendIn>> &res_mgr,
                                      const gdj::query_tp &query);

    // Leapfrog resumable: una variante por query (como WK resumable, mas n_prejoins/n_exact); usa lf_* y parallel_phases.
    template<class RankBackendIn, class RankBackendOut>
    ConstRunResult run_leapfrog_resumable_const(const ConstRunArgs &args, gdj::resource_manager<gdj::se_quadtree<RankBackendIn>> &res_mgr,
                                                const gdj::query_tp &query);

    // Leapfrog race (generic y adaptive).
    template<class RankBackendIn, class RankBackendOut>
    ConstRaceResult run_leapfrog_race_const(const ConstRunArgs &args, gdj::resource_manager<gdj::se_quadtree<RankBackendIn>> &res_mgr,
                                            const gdj::query_tp &query);

    // Kernel race (una sola variante).
    template<class RankBackendIn, class RankBackendOut>
    ConstRaceResult run_kernel_race_const(const ConstRunArgs &args, gdj::resource_manager<gdj::se_quadtree<RankBackendIn>> &res_mgr,
                                          const gdj::query_tp &query);

} // namespace gdj_examples

#endif // QDAGS_EXAMPLES_CONST_RUNS_MORE_HPP
