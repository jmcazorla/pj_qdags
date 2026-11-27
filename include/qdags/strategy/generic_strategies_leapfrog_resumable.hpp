#ifndef QDAGS_STRATEGY_GENERIC_STRATEGIES_LEAPFROG_RESUMABLE_HPP
#define QDAGS_STRATEGY_GENERIC_STRATEGIES_LEAPFROG_RESUMABLE_HPP

#include <vector>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <cstdint>

#include "generic_strategies_leapfrog.hpp"
#include "coefficient_growth.hpp"
#include "../join/joins_resumable.hpp"
#include "../join/joins_resumable_wide.hpp"

// Leapfrog resumable: como strategy_leapfrog_adaptive (alcanzar upper_bound con una cota creciente en los prejoins
// no exactos) pero sin recalcularlos desde cero: cada paso no exacto tiene un ResumableJoinAuto que se amplia en
// cada intento (next(want - already)). Los pasos exactos (cota fija upper_bound) no ganan nada con el motor
// resumable: usan multiJoinAuto y se repiten solo mientras su entrada siga creciendo. El join final se recalcula
// siempre.
//
// El ResumableJoin del paso k solo sigue ampliandose si el prejoin k-1 esta congelado; si no, se rehace sobre la
// entrada nueva. Un paso esta completo solo si su join se agoto y su entrada estaba completa.
//
// El motor resumable es secuencial y llega a 8 atributos: a lo mas 8 pasos no exactos (std::invalid_argument si
// no). Los pasos exactos usan los ParallelPhases como strategy_leapfrog.

namespace gdj {

    namespace leapfrog_detail {

        // Qdags del paso k en el espacio local {0..k}, como en prejoin(). Quedan en `out` (el ResumableJoin guarda
        // una referencia) y en `all_qdags` (ownership del caller).
        template<class QdagT, class SeQuadT>
        void step_qdags(query_metadata_lf<SeQuadT>& meta, size_t k, SeQuadT* prev,
                        std::vector<QdagT*>& all_qdags, uint64_t grid_side, std::vector<QdagT*>& out) {
            auto& step = meta.steps[k];
            att_set att_step;
            for (uint16_t j = 0; j <= k; ++j)
                att_step.push_back(j);

            out.clear();
            if (prev) {
                att_set att_prev(att_step.begin(), att_step.end() - 1);
                out.push_back(new QdagT(prev, att_prev, grid_side, att_step, wide_base_for_resumable(meta.wide)));
            }
            for (size_t i = 0; i < step.qts.size(); ++i)
                out.push_back(new QdagT(step.qts[i], step.atts[i], grid_side, att_step, wide_base_for_resumable(meta.wide)));
            all_qdags.insert(all_qdags.end(), out.begin(), out.end());
        }

    } // namespace leapfrog_detail

    // Leapfrog resumable: retorna el ultimo intento (ownership del caller); la cardinalidad va por `results_f`.
    // Mismos criterios de parada que strategy_leapfrog_adaptive. `SeQuadTOut`: como en strategy_leapfrog.
    template<class QdagT, class SeQuadT, class SeQuadTOut = SeQuadT>
    SeQuadTOut* strategy_leapfrog_resumable(query_metadata_lf<SeQuadT>& meta, std::vector<QdagT*>& all_qdags,
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
        if (n_inexact > 8)
            throw std::invalid_argument("strategy_leapfrog_resumable: " + std::to_string(n_inexact)
                + " inexact prejoins; the resumable engine allows at most 8 attributes (use lf_max_atts <= 8)");

        std::vector<SeQuadT*> prejoins(n_steps, nullptr);
        std::vector<std::vector<QdagT*>> step_q(n_steps);
        std::vector<ResumableJoinAuto<QdagT, SeQuadT>*> step_join(n_steps, nullptr);
        std::vector<bool> join_stable(n_steps, false);  // el join se armo sobre una entrada ya congelada
        std::vector<bool> frozen(n_steps, false);       // el prejoin k ya no cambia en intentos siguientes
        std::vector<bool> complete(n_steps, false);     // ademas, es el prejoin completo (no cortado por su cota)

        uint64_t coefficient_bound = (upper_bound == 0) ? 0 : std::max<uint64_t>(initial_coefficient_bound, 1);
        uint32_t attempt = 0;
        bool exhausted = false;
        bool timed_out = false;
        SeQuadTOut* final_qt = nullptr;

        while (true) {
            ++attempt;

            for (size_t k = 0; k < n_steps && !timed_out; ++k) {
                if (frozen[k])
                    continue;
                SeQuadT* prev = k > 0 ? prejoins[k - 1] : nullptr;
                bool deps_frozen = (k == 0) || frozen[k - 1];

                if (meta.steps[k].exact) {
                    // Cota fija upper_bound: multiJoinAuto, repetido solo mientras la entrada cambie.
                    std::vector<QdagT*> qdags;
                    leapfrog_detail::step_qdags<QdagT, SeQuadT>(meta, k, prev, all_qdags, grid_side, qdags);
                    uint64_t cnt = 0;
                    delete prejoins[k];
                    prejoins[k] = multiJoinAuto<QdagT, SeQuadT>(qdags, upper_bound > 0, upper_bound, prejoin_cut, cnt);
                    if (deps_frozen) {
                        frozen[k] = true;
                        complete[k] = (upper_bound == 0 || cnt < upper_bound);
                    }
                } else {
                    if (!step_join[k] || !join_stable[k]) {
                        delete step_join[k];
                        step_join[k] = nullptr;
                        leapfrog_detail::step_qdags<QdagT, SeQuadT>(meta, k, prev, all_qdags, grid_side, step_q[k]);
                        step_join[k] = new ResumableJoinAuto<QdagT, SeQuadT>(step_q[k]);
                        join_stable[k] = deps_frozen;
                        delete prejoins[k];
                        prejoins[k] = nullptr;
                    }
                    auto* rj = step_join[k];
                    uint64_t target = coefficient_bound * upper_bound;
                    uint64_t want = (target == 0) ? unbounded_budget() : target;
                    uint64_t already = rj->total_results();
                    uint64_t produced = want > already ? rj->next(want - already) : 0;
                    if (!prejoins[k] || produced > 0) {
                        delete prejoins[k];
                        prejoins[k] = rj->build_result();
                    }
                    if (timeout_triggered.load(std::memory_order_relaxed)) {
                        timed_out = true;
                        break;
                    }
                    if (deps_frozen && rj->exhausted()) {
                        frozen[k] = true;
                        complete[k] = true;
                    }
                }
                if (timeout_triggered.load(std::memory_order_relaxed))
                    timed_out = true;
            }

            delete final_qt;
            final_qt = leapfrog_detail::final_join<QdagT, SeQuadT, SeQuadTOut>(
                meta, n_steps > 0 ? prejoins[n_steps - 1] : nullptr, all_qdags, upper_bound, final_cut, grid_side, results_f);

            // Un join cortado por el watchdog no esta completo: no cuenta como agotado.
            timed_out = timed_out || timeout_triggered.load(std::memory_order_relaxed);

            bool inexact_complete = true;
            for (size_t k = 0; k < n_inexact; ++k)
                if (!complete[k]) {
                    inexact_complete = false;
                    break;
                }
            bool reached_target = (upper_bound == 0) || (results_f >= upper_bound);
            exhausted = !timed_out && inexact_complete;
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

        for (auto* rj : step_join)
            delete rj;
        for (auto* qt : prejoins)
            delete qt;
        return final_qt;
    }

} // namespace gdj

#endif // QDAGS_STRATEGY_GENERIC_STRATEGIES_LEAPFROG_RESUMABLE_HPP
