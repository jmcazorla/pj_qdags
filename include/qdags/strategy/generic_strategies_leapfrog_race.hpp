#ifndef QDAGS_STRATEGY_GENERIC_STRATEGIES_LEAPFROG_RACE_HPP
#define QDAGS_STRATEGY_GENERIC_STRATEGIES_LEAPFROG_RACE_HPP

#include <vector>
#include <cstdint>

#include "generic_strategies_leapfrog.hpp"
#include "generic_strategies_race.hpp"

// Leapfrog raced: strategy_leapfrog y strategy_leapfrog_adaptive secuencial contra paralela (raceStrategies), gana
// la primera. `meta` es de solo lectura y cada rama arma sus qdags y prejoins. Reutiliza RaceStats y
// RaceStatsAdaptive. No hay raced de la version resumable: su motor es secuencial.

namespace gdj {

    // `SeQuadTOut`: backend del resultado final de cada rama.
    template<class QdagT, class SeQuadT, class SeQuadTOut = SeQuadT>
    uint64_t strategy_leapfrog_raced(query_metadata_lf<SeQuadT>& meta,
                                     uint64_t upper_bound,
                                     uint64_t coefficient_bound,
                                     uint64_t parallel_level,
                                     uint64_t grid_side,
                                     ParallelPhases parallel_phases,
                                     uint64_t time_out,
                                     RaceStats* stats = nullptr) {
        auto branch = [&](uint64_t level) {
            std::vector<QdagT*> all_qdags;
            uint64_t r = 0;
            SeQuadTOut* result = strategy_leapfrog<QdagT, SeQuadT, SeQuadTOut>(
                meta, all_qdags, r, upper_bound, coefficient_bound, level, grid_side, parallel_phases);
            delete result;
            delete_qdags_race(all_qdags);
            return r;
        };
        auto seq_fn = [&]() -> uint64_t { return branch(0); };
        auto par_fn = [&]() -> uint64_t { return branch(parallel_level); };

        RaceResult res = raceStrategies(seq_fn, par_fn, time_out);
        if (stats) {
            stats->winner = res.winner;
            stats->seq_time = res.seq_time;
            stats->par_time = res.par_time;
        }
        return res.value;
    }

    // Version adaptive (adaptive-seq contra adaptive-par).
    template<class QdagT, class SeQuadT, class SeQuadTOut = SeQuadT>
    uint64_t strategy_leapfrog_raced_adaptive(query_metadata_lf<SeQuadT>& meta,
                                              uint64_t upper_bound,
                                              uint64_t initial_coefficient_bound,
                                              uint32_t growth_factor,
                                              uint32_t max_retries,
                                              uint64_t parallel_level,
                                              uint64_t grid_side,
                                              ParallelPhases parallel_phases,
                                              uint64_t time_out,
                                              RaceStatsAdaptive* stats = nullptr,
                                              GrowthMode growth_mode = GrowthMode::Fixed) {
        uint32_t seq_attempts = 0, par_attempts = 0;
        uint64_t seq_coef = 0, par_coef = 0;
        bool seq_exh = false, par_exh = false;

        auto branch = [&](uint64_t level, uint32_t* attempts, uint64_t* coef, bool* exh) {
            std::vector<QdagT*> all_qdags;
            uint64_t r = 0;
            SeQuadTOut* result = strategy_leapfrog_adaptive<QdagT, SeQuadT, SeQuadTOut>(
                meta, all_qdags, r, upper_bound, initial_coefficient_bound, growth_factor, max_retries,
                level, grid_side, parallel_phases, attempts, coef, exh, growth_mode);
            delete result;
            delete_qdags_race(all_qdags);
            return r;
        };
        auto seq_fn = [&]() -> uint64_t { return branch(0, &seq_attempts, &seq_coef, &seq_exh); };
        auto par_fn = [&]() -> uint64_t { return branch(parallel_level, &par_attempts, &par_coef, &par_exh); };

        RaceResult res = raceStrategies(seq_fn, par_fn, time_out);
        if (stats) {
            stats->winner = res.winner; stats->seq_time = res.seq_time; stats->par_time = res.par_time;
            stats->seq_attempts = seq_attempts; stats->par_attempts = par_attempts;
            stats->seq_final_coef = seq_coef; stats->par_final_coef = par_coef;
            stats->seq_exhaustive = seq_exh; stats->par_exhaustive = par_exh;
        }
        return res.value;
    }

} // namespace gdj

#endif // QDAGS_STRATEGY_GENERIC_STRATEGIES_LEAPFROG_RACE_HPP
