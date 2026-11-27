#ifndef QDAGS_STRATEGY_GENERIC_STRATEGIES_RESUMABLE_HPP
#define QDAGS_STRATEGY_GENERIC_STRATEGIES_RESUMABLE_HPP

#include <cmath>
#include <vector>
#include <cstdint>

#include "generic_strategies.hpp"
#include "coefficient_growth.hpp"
#include "../join/joins_resumable.hpp"
#include "../join/joins_resumable_wide.hpp"

// Familia resumable de NonLonely y WeakKernel: como las variantes adaptive (alcanzar upper_bound con una cota
// creciente) pero sin recalcular desde cero: las fases que se reintentan usan ResumableJoinAuto
// (joins_resumable_wide.hpp), que amplia el trabajo del intento anterior. Plan esta en
// generic_strategies_plan_resumable.hpp. Ninguna toma ParallelPhases: el motor resumable es secuencial y
// `parallel_level` solo afecta la fase final (multiJoinAuto).

namespace gdj {

    // WeakKernel resumable: la Fase 1 (kernel 1D) se calcula una vez sin cota; la Fase 2 tiene un ResumableJoin
    // por patron que se amplia en cada intento; la Fase 3 se recalcula siempre con multiJoinAuto. Retorna la
    // Fase 3 del ultimo intento (ownership del caller); la cardinalidad va por `results_f`. `SeQuadTOut`: backend
    // del resultado final (solo la Fase 3).
    template<class QdagT, class SeQuadT, class SeQuadTOut = SeQuadT>
    SeQuadTOut* strategy_weak_kernel_resumable(query_metadata_wk<SeQuadT>& meta, std::vector<QdagT*>& all_qdags,
                                             uint64_t& results_f,
                                             uint64_t upper_bound,
                                             uint64_t initial_coefficient_bound,
                                             uint32_t growth_factor,
                                             uint32_t max_retries,
                                             uint64_t parallel_level, uint64_t grid_side,
                                             uint32_t* attempts_used = nullptr,
                                             uint64_t* coefficient_bound_used = nullptr,
                                             bool* was_exhaustive = nullptr,
                                             GrowthMode growth_mode = GrowthMode::Fixed) {
        results_f = 0;
        std::vector<SeQuadT*> temp_qts;

        att_set att_1d = {0};
        att_set att_2d = {0, 1};

        // Fase 1: kernel 1D por variable non-lonely (una vez, sin cota).
        std::vector<SeQuadT*> kernel_qts(meta.kernel_vars.size(), nullptr);
        for (size_t k_idx = 0; k_idx < meta.kernel_vars.size(); ++k_idx) {
            auto& kv = meta.kernel_vars[k_idx];
            if (kv.unary_qt.empty())
                continue;
            std::vector<QdagT*> k_qdags;
            for (auto* qt : kv.unary_qt) {
                auto* q = new QdagT(qt, att_1d, grid_side, att_1d);
                k_qdags.push_back(q);
                all_qdags.push_back(q);
            }
            uint64_t dummy = 0;
            kernel_qts[k_idx] = multiJoinAuto<QdagT, SeQuadT>(k_qdags, false, 0, 0, dummy);
            if (kernel_qts[k_idx])
                temp_qts.push_back(kernel_qts[k_idx]);
        }

        // Fase 2: extension binaria por patron, un ResumableJoin propio por patron.
        size_t n_patterns = meta.patterns.size();
        std::vector<std::vector<QdagT*>> ext_qdags_per_pattern(n_patterns);
        std::vector<ResumableJoinAuto<QdagT, SeQuadT>*> ext_join(n_patterns, nullptr);
        std::vector<bool> pattern_done(n_patterns, false);
        std::vector<SeQuadT*> extended_qts(n_patterns, nullptr);

        for (size_t p_idx = 0; p_idx < n_patterns; ++p_idx) {
            auto& pd = meta.patterns[p_idx];
            for (auto& kr : pd.kernel_refs) {
                size_t k_idx = kr.first;
                uint8_t local_pos = kr.second;
                SeQuadT* k_qt = kernel_qts[k_idx];
                if (!k_qt)
                    continue;
                att_set att_k = {(uint16_t)local_pos};
                auto* q = new QdagT(k_qt, att_k, grid_side, att_2d);
                ext_qdags_per_pattern[p_idx].push_back(q);
                all_qdags.push_back(q);
            }
            if (pd.binary_qt) {
                auto* q = new QdagT(pd.binary_qt, att_2d, grid_side, att_2d);
                ext_qdags_per_pattern[p_idx].push_back(q);
                all_qdags.push_back(q);
            }
            if (ext_qdags_per_pattern[p_idx].empty()) {
                pattern_done[p_idx] = true;
            } else {
                ext_join[p_idx] = new ResumableJoinAuto<QdagT, SeQuadT>(ext_qdags_per_pattern[p_idx]);
            }
        }

        uint64_t coefficient_bound = (upper_bound == 0) ? 0 : std::max<uint64_t>(initial_coefficient_bound, 1);
        uint32_t attempt = 0;
        bool exhausted = false;
        SeQuadTOut* final_qt = nullptr;

        while (true) {
            ++attempt;
            uint64_t target = coefficient_bound * upper_bound;

            for (size_t p_idx = 0; p_idx < n_patterns; ++p_idx) {
                if (pattern_done[p_idx])
                    continue;
                auto* rj = ext_join[p_idx];

                uint64_t already = rj->total_results();
                uint64_t want = (target == 0) ? unbounded_budget() : target;
                if (want > already)
                    rj->next(want - already);

                if (extended_qts[p_idx])
                    temp_qts.push_back(extended_qts[p_idx]);
                extended_qts[p_idx] = rj->build_result();

                uint64_t got = rj->total_results();
                if (rj->exhausted() || (target > 0 && got < target))
                    pattern_done[p_idx] = true;
            }

            std::vector<QdagT*> final_qdags;
            for (size_t p_idx = 0; p_idx < n_patterns; ++p_idx) {
                if (!extended_qts[p_idx])
                    continue;
                auto& pd = meta.patterns[p_idx];
                att_set att_pat = {pd.phys_s, pd.phys_o};
                auto* q = new QdagT(extended_qts[p_idx], att_pat, grid_side, meta.att_ext, wide_base_for_resumable(meta.wide));
                final_qdags.push_back(q);
                all_qdags.push_back(q);
            }

            uint64_t attempt_results = 0;
            if (final_qt) {
                delete final_qt;
                final_qt = nullptr;
            }
            if (!final_qdags.empty()) {
                final_qt = multiJoinAuto<QdagT, SeQuadTOut>(final_qdags, upper_bound > 0, upper_bound, parallel_level, attempt_results);
            }
            results_f = attempt_results;

            bool reached_target = (upper_bound == 0) || (results_f >= upper_bound);
            bool all_done = true;
            for (bool d : pattern_done)
                if (!d) {
                    all_done = false;
                    break;
                }
            exhausted = all_done;
            bool retries_exhausted = (max_retries != 0) && (attempt >= max_retries);

            if (attempts_used)
                *attempts_used = attempt;
            if (coefficient_bound_used)
                *coefficient_bound_used = coefficient_bound;

            if (reached_target || exhausted || retries_exhausted)
                break;

            uint64_t next_c = coefficient_bound * growth_factor;
            if (growth_mode == GrowthMode::Proportional)
                next_c = proportional_next_coefficient(coefficient_bound, growth_factor, upper_bound, results_f);
            coefficient_bound = next_c < 1 ? 1 : next_c;
        }

        if (was_exhaustive)
            *was_exhaustive = exhausted || (upper_bound == 0);

        for (auto* rj : ext_join)
            delete rj;
        for (auto* qt : temp_qts)
            delete qt;
        for (auto* qt : extended_qts)
            delete qt; // ultimo build_result() de cada patron, nunca entro a temp_qts
        return final_qt;
    }

} // namespace gdj

#endif // QDAGS_STRATEGY_GENERIC_STRATEGIES_RESUMABLE_HPP
