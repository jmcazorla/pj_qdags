#ifndef QDAGS_STRATEGY_GENERIC_STRATEGIES_PLAN_RACE_HPP
#define QDAGS_STRATEGY_GENERIC_STRATEGIES_PLAN_RACE_HPP

#include <vector>
#include <functional>
#include <cstdint>

#include "generic_strategies_plan.hpp"
#include "generic_strategies_race.hpp"

// strategy_plan y strategy_plan_adaptive secuencial contra paralelo (raceStrategies), gana el primero. Ambos
// escriben en su query_metadata_plan, asi que las ramas no pueden compartirlo: `build_meta` es una fabrica que arma
// uno nuevo por rama.

namespace gdj {
    template<class QdagT, class SeQuadT>
    uint64_t strategy_plan_raced(std::function<query_metadata_plan<SeQuadT>()> build_meta,
                                  uint64_t upper_bound,
                                  uint64_t parallel_level,
                                  uint64_t grid_side,
                                  uint32_t coefficient_bound,
                                  bool mxm,
                                  uint8_t k,
                                  ParallelPhases parallel_phases,
                                  uint64_t time_out,
                                  RaceStats* stats = nullptr) {
        auto seq_fn = [&]() -> uint64_t {
            std::vector<QdagT*> all_qdags;
            std::vector<SeQuadT*> mxm_quadtrees;
            query_metadata_plan<SeQuadT> meta = build_meta();
            uint64_t r = 0;
            SeQuadT* result = strategy_plan<QdagT, SeQuadT>(
                meta, all_qdags, mxm_quadtrees, r, upper_bound, 0, grid_side, coefficient_bound, mxm, k, parallel_phases
            );
            delete result;
            meta.clearResults();
            delete_qdags_race(all_qdags);
            delete_quadtrees_race(mxm_quadtrees);
            return r;
        };
        auto par_fn = [&]() -> uint64_t {
            std::vector<QdagT*> all_qdags;
            std::vector<SeQuadT*> mxm_quadtrees;
            query_metadata_plan<SeQuadT> meta = build_meta();
            uint64_t r = 0;
            SeQuadT* result = strategy_plan<QdagT, SeQuadT>(
                meta, all_qdags, mxm_quadtrees, r, upper_bound, parallel_level, grid_side, coefficient_bound, mxm, k, parallel_phases
            );
            delete result;
            meta.clearResults();
            delete_qdags_race(all_qdags);
            delete_quadtrees_race(mxm_quadtrees);
            return r;
        };

        RaceResult res = raceStrategies(seq_fn, par_fn, time_out);
        if (stats) {
            stats->winner = res.winner;
            stats->seq_time = res.seq_time;
            stats->par_time = res.par_time;
        }
        return res.value;
    }

    // Version adaptive (adaptive-seq contra adaptive-par), con la misma fabrica build_meta.
    template<class QdagT, class SeQuadT>
    uint64_t strategy_plan_raced_adaptive(std::function<query_metadata_plan<SeQuadT>()> build_meta,
                                           uint64_t upper_bound,
                                           uint64_t initial_coefficient_bound,
                                           uint32_t growth_factor,
                                           uint32_t max_retries,
                                           uint64_t parallel_level,
                                           uint64_t grid_side,
                                           bool mxm,
                                           uint8_t k,
                                           ParallelPhases parallel_phases,
                                           uint64_t time_out,
                                           RaceStatsAdaptive* stats = nullptr,
                                           GrowthMode growth_mode = GrowthMode::Fixed) {
        uint32_t seq_attempts = 0, par_attempts = 0;
        uint64_t seq_coef = 0, par_coef = 0;
        bool seq_exh = false, par_exh = false;

        auto seq_fn = [&]() -> uint64_t {
            std::vector<QdagT*> all_qdags;
            std::vector<SeQuadT*> mxm_quadtrees;
            query_metadata_plan<SeQuadT> meta = build_meta();
            uint64_t r = 0;
            SeQuadT* result = strategy_plan_adaptive<QdagT, SeQuadT>(
                meta, all_qdags, mxm_quadtrees, r, upper_bound, initial_coefficient_bound, growth_factor, max_retries,
                0, grid_side, mxm, k, parallel_phases, &seq_attempts, &seq_coef, &seq_exh, growth_mode
            );
            delete result;
            meta.clearResults();
            delete_qdags_race(all_qdags);
            delete_quadtrees_race(mxm_quadtrees);
            return r;
        };
        auto par_fn = [&]() -> uint64_t {
            std::vector<QdagT*> all_qdags;
            std::vector<SeQuadT*> mxm_quadtrees;
            query_metadata_plan<SeQuadT> meta = build_meta();
            uint64_t r = 0;
            SeQuadT* result = strategy_plan_adaptive<QdagT, SeQuadT>(
                meta, all_qdags, mxm_quadtrees, r, upper_bound, initial_coefficient_bound, growth_factor, max_retries,
                parallel_level, grid_side, mxm, k, parallel_phases, &par_attempts, &par_coef, &par_exh, growth_mode
            );
            delete result;
            meta.clearResults();
            delete_qdags_race(all_qdags);
            delete_quadtrees_race(mxm_quadtrees);
            return r;
        };

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

#endif // QDAGS_STRATEGY_GENERIC_STRATEGIES_PLAN_RACE_HPP
