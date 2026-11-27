#ifndef QDAGS_STRATEGY_GENERIC_STRATEGIES_RACE_HPP
#define QDAGS_STRATEGY_GENERIC_STRATEGIES_RACE_HPP

#include <vector>
#include <cstdint>
#include <functional>

#include "generic_strategies.hpp"
#include "strategy_race.hpp"

// Familia raced: corre la version secuencial y la paralela de la misma estrategia en paralelo (raceStrategies) y
// se queda con la primera que termine. Cubre WeakKernel y NonLonely, en version fija (*_raced) y adaptive
// (*_raced_adaptive); Orig no tiene (una sola llamada a multiJoinAuto). Plan esta en generic_strategies_plan_race.hpp
// y reutiliza RaceStats, RaceStatsAdaptive, delete_qdags_race y delete_quadtrees_race de este archivo.

namespace gdj {

    struct RaceStats {
        int winner = -1;      // 0 = seq gano, 1 = par gano
        double seq_time = -1.0;
        double par_time = -1.0;
    };

    struct RaceStatsAdaptive {
        int winner = -1;      // 0 = seq gano, 1 = par gano
        double seq_time = -1.0;
        double par_time = -1.0;
        uint32_t seq_attempts = 0, par_attempts = 0;
        uint64_t seq_final_coef = 0, par_final_coef = 0;
        bool seq_exhaustive = false, par_exhaustive = false;
    };

    template<class QdagT>
    static void delete_qdags_race(std::vector<QdagT*>& qdags) {
        for (auto* q : qdags)
            delete q;
        qdags.clear();
    }

    // Para strategy_plan_raced(_adaptive): libera los quadtrees de proyeccion mxm de cada rama.
    template<class SeQuadT>
    static void delete_quadtrees_race(std::vector<SeQuadT*>& quadtrees) {
        for (auto* q : quadtrees)
            delete q;
        quadtrees.clear();
    }

    // strategy_weak_kernel secuencial contra paralela. `meta` es de solo lectura (las ramas lo comparten) y cada
    // rama arma su propio all_qdags. `SeQuadTOut`: backend del resultado final de cada rama.
    template<class QdagT, class SeQuadT, class SeQuadTOut = SeQuadT>
    uint64_t strategy_weak_kernel_raced(query_metadata_wk<SeQuadT>& meta,
                                         uint64_t upper_bound,
                                         uint64_t coefficient_bound,
                                         uint64_t parallel_level,
                                         uint64_t grid_side,
                                         ParallelPhases parallel_phases,
                                         uint64_t time_out,
                                         RaceStats* stats = nullptr) {
        auto seq_fn = [&]() -> uint64_t {
            std::vector<QdagT*> all_qdags;
            uint64_t r = 0;
            SeQuadTOut* result = strategy_weak_kernel<QdagT, SeQuadT, SeQuadTOut>(
                meta, all_qdags, r, upper_bound, coefficient_bound, 0, grid_side, parallel_phases
            );
            delete result;
            delete_qdags_race(all_qdags);
            return r;
        };
        auto par_fn = [&]() -> uint64_t {
            std::vector<QdagT*> all_qdags;
            uint64_t r = 0;
            SeQuadTOut* result = strategy_weak_kernel<QdagT, SeQuadT, SeQuadTOut>(
                meta, all_qdags, r, upper_bound, coefficient_bound, parallel_level, grid_side, parallel_phases
            );
            delete result;
            delete_qdags_race(all_qdags);
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

    // strategy_non_lonely secuencial contra paralela; `meta` es de solo lectura y cada rama arma sus propios qdags
    // y quadtrees. `SeQuadTOut`: backend del resultado final de cada rama.
    template<class QdagT, class SeQuadT, class SeQuadTOut = SeQuadT>
    uint64_t strategy_non_lonely_raced(query_metadata_nl<SeQuadT>& meta,
                                        uint64_t upper_bound,
                                        uint64_t parallel_level,
                                        uint64_t grid_side,
                                        ParallelPhases parallel_phases,
                                        uint64_t time_out,
                                        RaceStats* stats = nullptr) {
        auto seq_fn = [&]() -> uint64_t {
            std::vector<QdagT*> qdags_nl, qdags_f;
            SeQuadT* qproj = nullptr;
            SeQuadTOut* qfinal = nullptr;
            uint64_t r = 0;
            strategy_non_lonely<QdagT, SeQuadT, SeQuadTOut>(
                meta, qdags_nl, qdags_f, qproj, qfinal, r, upper_bound, 0, grid_side, parallel_phases
            );
            delete_qdags_race(qdags_nl);
            delete_qdags_race(qdags_f);
            if (qproj)
                delete qproj;
            if (qfinal)
                delete qfinal;
            return r;
        };
        auto par_fn = [&]() -> uint64_t {
            std::vector<QdagT*> qdags_nl, qdags_f;
            SeQuadT* qproj = nullptr;
            SeQuadTOut* qfinal = nullptr;
            uint64_t r = 0;
            strategy_non_lonely<QdagT, SeQuadT, SeQuadTOut>(
                meta, qdags_nl, qdags_f, qproj, qfinal, r, upper_bound, parallel_level, grid_side, parallel_phases
            );
            delete_qdags_race(qdags_nl);
            delete_qdags_race(qdags_f);
            if (qproj)
                delete qproj;
            if (qfinal)
                delete qfinal;
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

    // strategy_weak_kernel_adaptive secuencial contra paralela; `meta` es de solo lectura.
    // `SeQuadTOut`: como en strategy_weak_kernel_raced.
    template<class QdagT, class SeQuadT, class SeQuadTOut = SeQuadT>
    uint64_t strategy_weak_kernel_raced_adaptive(query_metadata_wk<SeQuadT>& meta,
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

        auto seq_fn = [&]() -> uint64_t {
            std::vector<QdagT*> all_qdags;
            uint64_t r = 0;
            SeQuadTOut* result = strategy_weak_kernel_adaptive<QdagT, SeQuadT, SeQuadTOut>(
                meta, all_qdags, r, upper_bound, initial_coefficient_bound, growth_factor, max_retries,
                0, grid_side, parallel_phases, &seq_attempts, &seq_coef, &seq_exh, growth_mode
            );
            delete result;
            delete_qdags_race(all_qdags);
            return r;
        };
        auto par_fn = [&]() -> uint64_t {
            std::vector<QdagT*> all_qdags;
            uint64_t r = 0;
            SeQuadTOut* result = strategy_weak_kernel_adaptive<QdagT, SeQuadT, SeQuadTOut>(
                meta, all_qdags, r, upper_bound, initial_coefficient_bound, growth_factor, max_retries,
                parallel_level, grid_side, parallel_phases, &par_attempts, &par_coef, &par_exh, growth_mode
            );
            delete result;
            delete_qdags_race(all_qdags);
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

#endif // QDAGS_STRATEGY_GENERIC_STRATEGIES_RACE_HPP
