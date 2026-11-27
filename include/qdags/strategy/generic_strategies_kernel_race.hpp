#ifndef QDAGS_STRATEGY_GENERIC_STRATEGIES_KERNEL_RACE_HPP
#define QDAGS_STRATEGY_GENERIC_STRATEGIES_KERNEL_RACE_HPP

#include <vector>
#include <cstdint>

#include "generic_strategies_kernel.hpp"
#include "generic_strategies_race.hpp"

// Kernel raced: strategy_kernel secuencial contra paralela (raceStrategies), gana la primera. Solo lee `meta`, asi
// que las ramas lo comparten y cada una arma sus qdags, I y reducciones. Sin version adaptive (I es exacto);
// reutiliza RaceStats.

namespace gdj {

    // `SeQuadTOut`: backend del resultado final de cada rama. El llamador salta las queries que Kernel no soporta
    // (sin lonely o sin non-lonely, o con mas de KERNEL_MAX_NONLONELY_ATTS non-lonely).
    template<class QdagT, class SeQuadT, class SeQuadTOut = SeQuadT>
    uint64_t strategy_kernel_raced(query_metadata_nl<SeQuadT>& meta,
                                   uint64_t upper_bound,
                                   uint64_t parallel_level,
                                   uint64_t grid_side,
                                   ParallelPhases parallel_phases,
                                   uint64_t time_out,
                                   RaceStats* stats = nullptr) {
        auto branch = [&](uint64_t level) {
            std::vector<QdagT*> all_qdags;
            uint64_t r = 0;
            SeQuadTOut* result = strategy_kernel<QdagT, SeQuadT, SeQuadTOut>(
                meta, all_qdags, r, upper_bound, level, grid_side, parallel_phases);
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

} // namespace gdj

#endif // QDAGS_STRATEGY_GENERIC_STRATEGIES_KERNEL_RACE_HPP
