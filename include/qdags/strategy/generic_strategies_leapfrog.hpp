#ifndef QDAGS_STRATEGY_GENERIC_STRATEGIES_LEAPFROG_HPP
#define QDAGS_STRATEGY_GENERIC_STRATEGIES_LEAPFROG_HPP

#include <vector>
#include <algorithm>
#include <cstdint>
#include "coefficient_growth.hpp"

#include "generic_strategies.hpp"
#include "../query/leapfrog_builder.hpp"

// Estrategia Leapfrog: ejecuta con multiJoinAuto la cadena de prejoins de un query_metadata_lf
// (query/leapfrog_builder.hpp) y el join final. Con upper_bound > 0, un prejoin exacto (step_data::exact) se
// corta a upper_bound; uno no exacto solo admite la cota heuristica coef*upper_bound, que la version _adaptive
// hace crecer hasta que el join final alcanza upper_bound o los prejoins no exactos se agotan.

namespace gdj {

    namespace leapfrog_detail {

        inline uint64_t step_cut_level(ParallelPhases parallel_phases, bool is_final, uint64_t parallel_level) {
            bool par = (parallel_phases == ParallelPhases::AllPhases)
                    || (is_final  && (parallel_phases == ParallelPhases::LastOnly  || parallel_phases == ParallelPhases::LastTwo))
                    || (!is_final && (parallel_phases == ParallelPhases::FirstOnly || parallel_phases == ParallelPhases::FirstTwo));
            return par ? parallel_level : 0;
        }

        // Prejoin k en el espacio local {0..k}: el anterior (dimensiones 0..k-1) mas las tablas y proyecciones del paso.
        template<class QdagT, class SeQuadT>
        SeQuadT* prejoin(query_metadata_lf<SeQuadT>& meta, size_t k, SeQuadT* prev,
                         std::vector<QdagT*>& all_qdags, uint64_t limit, uint64_t cut_level,
                         uint64_t grid_side, uint64_t& n_results) {
            auto& step = meta.steps[k];
            att_set att_step;
            for (uint16_t j = 0; j <= k; ++j)
                att_step.push_back(j);

            std::vector<QdagT*> qdags;
            if (prev) {
                att_set att_prev(att_step.begin(), att_step.end() - 1);
                qdags.push_back(new QdagT(prev, att_prev, grid_side, att_step, meta.wide));
            }
            for (size_t i = 0; i < step.qts.size(); ++i)
                qdags.push_back(new QdagT(step.qts[i], step.atts[i], grid_side, att_step, meta.wide));
            all_qdags.insert(all_qdags.end(), qdags.begin(), qdags.end());

            n_results = 0;
            return multiJoinAuto<QdagT, SeQuadT>(qdags, limit > 0, limit, cut_level, n_results);
        }

        // Join final en el espacio global: el ultimo prejoin (si hay) mas las tablas no contenidas en su prefijo.
        template<class QdagT, class SeQuadT, class SeQuadTOut>
        SeQuadTOut* final_join(query_metadata_lf<SeQuadT>& meta, SeQuadT* last_prejoin,
                               std::vector<QdagT*>& all_qdags, uint64_t upper_bound, uint64_t cut_level,
                               uint64_t grid_side, uint64_t& n_results) {
            std::vector<QdagT*> qdags;
            if (last_prejoin)
                qdags.push_back(new QdagT(last_prejoin, meta.prefix_att_final, grid_side, meta.att_ext, meta.wide));
            for (size_t i = 0; i < meta.final_qts.size(); ++i)
                qdags.push_back(new QdagT(meta.final_qts[i], meta.final_atts[i], grid_side, meta.att_ext, meta.wide));
            all_qdags.insert(all_qdags.end(), qdags.begin(), qdags.end());

            n_results = 0;
            if (qdags.empty())
                return nullptr;
            return multiJoinAuto<QdagT, SeQuadTOut>(qdags, upper_bound > 0, upper_bound, cut_level, n_results);
        }

    } // namespace leapfrog_detail

    // Leapfrog: cadena de prejoins sobre prefijos del orden de `meta` y join final en el espacio global. Con
    // upper_bound > 0 los prejoins exactos se cortan a upper_bound y los demas a `coefficient_bound * upper_bound`
    // (0 = sin cota: resultado garantizado; > 0 = heuristico, puede devolver menos de upper_bound aunque existan).
    // Retorna el resultado (ownership del caller); la cardinalidad va por `results_f`.
    // `SeQuadTOut` es el backend del resultado final; los prejoins quedan en SeQuadT.
    template<class QdagT, class SeQuadT, class SeQuadTOut = SeQuadT>
    SeQuadTOut* strategy_leapfrog(query_metadata_lf<SeQuadT>& meta, std::vector<QdagT*>& all_qdags,
                                  uint64_t& results_f,
                                  uint64_t upper_bound, uint64_t coefficient_bound, uint64_t parallel_level,
                                  uint64_t grid_side, ParallelPhases parallel_phases = ParallelPhases::LastOnly,
                                  uint64_t* out_final_size_bytes = nullptr) {
        results_f = 0;
        uint64_t prejoin_cut = leapfrog_detail::step_cut_level(parallel_phases, false, parallel_level);
        uint64_t final_cut = leapfrog_detail::step_cut_level(parallel_phases, true, parallel_level);

        SeQuadT* prev = nullptr;
        for (size_t k = 0; k < meta.steps.size(); ++k) {
            uint64_t limit = meta.steps[k].exact ? upper_bound : coefficient_bound * upper_bound;
            uint64_t dummy = 0;
            SeQuadT* cur = leapfrog_detail::prejoin<QdagT, SeQuadT>(meta, k, prev, all_qdags, limit, prejoin_cut, grid_side, dummy);
            delete prev;
            prev = cur;
        }

        SeQuadTOut* final_qt = leapfrog_detail::final_join<QdagT, SeQuadT, SeQuadTOut>(
            meta, prev, all_qdags, upper_bound, final_cut, grid_side, results_f);
        if (final_qt && out_final_size_bytes)
            *out_final_size_bytes = final_qt->size_in_bytes();

        delete prev;
        return final_qt;
    }

    // Leapfrog adaptive: reintenta con un coefficient_bound creciente hasta que el join final alcanza upper_bound,
    // los prejoins no exactos se agotan o se llega a max_retries. Memoiza solo el tramo inicial completo: un paso
    // esta completo si no llego a su cota y todos los anteriores tambien. Con todos los prejoins exactos hay un
    // unico intento. Retorna el ultimo intento. `SeQuadTOut`: ver strategy_leapfrog.
    template<class QdagT, class SeQuadT, class SeQuadTOut = SeQuadT>
    SeQuadTOut* strategy_leapfrog_adaptive(query_metadata_lf<SeQuadT>& meta, std::vector<QdagT*>& all_qdags,
                                           uint64_t& results_f,
                                           uint64_t upper_bound,
                                           uint64_t initial_coefficient_bound,
                                           uint32_t growth_factor,
                                           uint32_t max_retries,
                                           uint64_t parallel_level, uint64_t grid_side,
                                           ParallelPhases parallel_phases = ParallelPhases::LastOnly,
                                           uint32_t* attempts_used = nullptr,
                                           uint64_t* coefficient_bound_used = nullptr,
                                           bool* was_exhaustive = nullptr,
                                           GrowthMode growth_mode = GrowthMode::Fixed) {
        results_f = 0;
        uint64_t prejoin_cut = leapfrog_detail::step_cut_level(parallel_phases, false, parallel_level);
        uint64_t final_cut = leapfrog_detail::step_cut_level(parallel_phases, true, parallel_level);

        size_t n_steps = meta.steps.size();
        size_t n_inexact = 0;  // los no exactos son un tramo inicial de la cadena
        for (const auto& step : meta.steps)
            if (!step.exact)
                n_inexact++;

        uint64_t coefficient_bound = (upper_bound == 0) ? 0 : std::max<uint64_t>(initial_coefficient_bound, 1);
        uint32_t attempt = 0;

        std::vector<SeQuadT*> prejoins(n_steps, nullptr);
        size_t n_complete = 0;  // pasos 0..n_complete-1: calculados sin llegar a su cota, no se recalculan
        bool exhausted = false;
        bool timed_out = false;
        SeQuadTOut* final_qt = nullptr;

        while (true) {
            ++attempt;

            for (size_t k = n_complete; k < n_steps; ++k) {
                uint64_t limit = meta.steps[k].exact ? upper_bound : coefficient_bound * upper_bound;
                uint64_t cnt = 0;
                delete prejoins[k];
                prejoins[k] = leapfrog_detail::prejoin<QdagT, SeQuadT>(
                    meta, k, k > 0 ? prejoins[k - 1] : nullptr, all_qdags, limit, prejoin_cut, grid_side, cnt);
                if (k == n_complete && (limit == 0 || cnt < limit))
                    n_complete = k + 1;
            }

            delete final_qt;
            final_qt = leapfrog_detail::final_join<QdagT, SeQuadT, SeQuadTOut>(
                meta, n_steps > 0 ? prejoins[n_steps - 1] : nullptr, all_qdags, upper_bound, final_cut, grid_side, results_f);

            // Un join cortado por el watchdog no esta completo: no cuenta como agotado.
            timed_out = timeout_triggered.load(std::memory_order_relaxed);

            bool reached_target = (upper_bound == 0) || (results_f >= upper_bound);
            exhausted = !timed_out && (n_complete >= n_inexact);
            bool retries_exhausted = (max_retries != 0) && (attempt >= max_retries);

            if (attempts_used)
                *attempts_used = attempt;
            if (coefficient_bound_used)
                *coefficient_bound_used = coefficient_bound;

            if (reached_target || exhausted || retries_exhausted || timed_out)
                break;

            uint64_t next = coefficient_bound * growth_factor;
            if (growth_mode == GrowthMode::Proportional)
                next = proportional_next_coefficient(coefficient_bound, growth_factor, upper_bound, results_f);
            coefficient_bound = next > coefficient_bound ? next : coefficient_bound * 2;
        }

        if (was_exhaustive)
            *was_exhaustive = exhausted || (upper_bound == 0 && !timed_out);

        for (auto* qt : prejoins)
            delete qt;
        return final_qt;
    }

}

#endif // QDAGS_STRATEGY_GENERIC_STRATEGIES_LEAPFROG_HPP
