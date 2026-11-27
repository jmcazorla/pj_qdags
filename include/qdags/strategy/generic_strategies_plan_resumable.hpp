#ifndef QDAGS_STRATEGY_GENERIC_STRATEGIES_PLAN_RESUMABLE_HPP
#define QDAGS_STRATEGY_GENERIC_STRATEGIES_PLAN_RESUMABLE_HPP

#include <vector>
#include <algorithm>
#include <cstdint>

#include "generic_strategies_plan.hpp"
#include "coefficient_growth.hpp"
#include "generic_strategies_resumable.hpp"

// Como strategy_plan_adaptive (alcanzar upper_bound con una cota creciente) pero sin recalcular cada paso no final
// desde cero: cada uno tiene un ResumableJoinAuto que se amplia en cada intento (next(want-already)); el ultimo paso
// se recalcula siempre con multiJoinAuto. Ademas gestiona las dependencias: un paso ya estable solo se reconstruye
// si alguno de sus prejoins de entrada aun no esta terminado.
namespace gdj {

    // Plan resumable. Retorna el ultimo paso (ownership del caller; su result_qt queda en nullptr); la cardinalidad
    // va por `results_f`.
    template<class QdagT, class SeQuadT>
    SeQuadT* strategy_plan_resumable(query_metadata_plan<SeQuadT>& plan_meta,
                                      std::vector<QdagT*>& all_qdags,
                                      std::vector<SeQuadT*>& mxm_quadtrees,
                                      uint64_t& results_f,
                                      uint64_t upper_bound,
                                      uint64_t initial_coefficient_bound,
                                      uint32_t growth_factor,
                                      uint32_t max_retries,
                                      uint64_t parallel_level,
                                      uint64_t grid_side,
                                      bool mxm,
                                      uint8_t k,
                                      uint32_t* attempts_used = nullptr,
                                      uint64_t* coefficient_bound_used = nullptr,
                                      bool* was_exhaustive = nullptr,
                                      GrowthMode growth_mode = GrowthMode::Fixed) {
        results_f = 0;
        size_t n_steps = plan_meta.steps.size();
        if (n_steps == 0) {
            if (attempts_used)
                *attempts_used = 0;
            if (coefficient_bound_used)
                *coefficient_bound_used = 0;
            if (was_exhaustive)
                *was_exhaustive = true;
            return nullptr;
        }

        std::vector<std::vector<size_t>> deps(n_steps);
        for (size_t i = 0; i < n_steps; ++i) {
            auto& sm = plan_meta.steps[i];
            size_t prejoin_pos = 0;
            for (size_t op_idx = 0; op_idx < sm.op_types.size(); ++op_idx) {
                if (sm.op_types[op_idx] == OpType::PREJOIN) {
                    for (size_t prev_i = 0; prev_i < i; ++prev_i) {
                        if (plan_meta.steps[prev_i].step_id == sm.prev_prejoin_id[prejoin_pos]) {
                            deps[i].push_back(prev_i);
                            break;
                        }
                    }
                    prejoin_pos++;
                }
            }
        }

        std::vector<std::vector<QdagT*>> step_qdags(n_steps);
        std::vector<ResumableJoinAuto<QdagT, SeQuadT>*> step_join(n_steps, nullptr);
        std::vector<bool> step_join_stable(n_steps, false);
        std::vector<bool> step_done(n_steps, false);

        auto build_step_qdags = [&](size_t i, bool is_last_step) {
            auto& sm = plan_meta.steps[i];
            std::sort(sm.output_att_org.begin(), sm.output_att_org.end());
            std::vector<QdagT*>& current_qdags = step_qdags[i];
            current_qdags.clear();
            size_t prejoin_pos = 0;
            for (size_t op_idx = 0; op_idx < sm.op_types.size(); ++op_idx) {
                if (sm.op_types[op_idx] == OpType::PREJOIN) {
                    SeQuadT* prev_res = nullptr;
                    att_set prev_out_att;
                    for (size_t prev_i = 0; prev_i < i; ++prev_i) {
                        if (plan_meta.steps[prev_i].step_id == sm.prev_prejoin_id[prejoin_pos]) {
                            prev_res = plan_meta.steps[prev_i].result_qt;
                            prev_out_att = plan_meta.steps[prev_i].output_att_org;
                            break;
                        }
                    }
                    if (prev_res) {
                        QdagT* q = new QdagT(prev_res, is_last_step ? prev_out_att : sm.atts_step[op_idx], grid_side, is_last_step ? sm.output_att_org : sm.output_att_ext_prejoin, wide_base_for_resumable(plan_meta.wide));
                        current_qdags.push_back(q);
                        all_qdags.push_back(q);
                    }
                    prejoin_pos++;
                } else {
                    QdagT* q = new QdagT(sm.qts[op_idx], sm.atts_step[op_idx], grid_side, is_last_step ? sm.output_att_org : sm.output_att_ext_prejoin, wide_base_for_resumable(plan_meta.wide));
                    current_qdags.push_back(q);
                    all_qdags.push_back(q);
                }
            }
        };

        uint64_t coefficient_bound = (upper_bound == 0) ? 0 : std::max<uint64_t>(initial_coefficient_bound, 1);
        uint32_t attempt = 0;
        bool exhausted = false;

        while (true) {
            ++attempt;
            uint64_t target = coefficient_bound * upper_bound;

            for (size_t i = 0; i + 1 < n_steps; ++i) {
                if (step_done[i])
                    continue;

                bool deps_all_done = true;
                for (size_t d : deps[i])
                    if (!step_done[d]) {
                        deps_all_done = false;
                        break;
                    }

                bool need_rebuild = !deps_all_done || !step_join_stable[i] || step_join[i] == nullptr;

                if (need_rebuild) {
                    delete step_join[i];
                    step_join[i] = nullptr;
                    build_step_qdags(i, false);
                    if (step_qdags[i].empty()) {
                        step_done[i] = true;
                        continue;
                    }
                    step_join[i] = new ResumableJoinAuto<QdagT, SeQuadT>(step_qdags[i]);
                    step_join_stable[i] = deps_all_done;
                }

                uint64_t already = step_join[i]->total_results();
                uint64_t want = (target == 0) ? unbounded_budget() : target;
                if (want > already)
                    step_join[i]->next(want - already);

                if (plan_meta.steps[i].result_qt)
                    delete plan_meta.steps[i].result_qt;
                plan_meta.steps[i].result_qt = step_join[i]->build_result();

                uint64_t got = step_join[i]->total_results();
                // Agotado sobre una entrada cortada no es completo: se congela solo si sus dependencias estaban terminadas.
                if (deps_all_done && (step_join[i]->exhausted() || (target > 0 && got < target)))
                    step_done[i] = true;
            }

            {
                size_t last = n_steps - 1;
                build_step_qdags(last, true);
                uint64_t limit = upper_bound;
                uint64_t step_results = 0;
                if (plan_meta.steps[last].result_qt) {
                    delete plan_meta.steps[last].result_qt;
                    plan_meta.steps[last].result_qt = nullptr;
                }
                if (!step_qdags[last].empty())
                    plan_meta.steps[last].result_qt = multiJoinAuto<QdagT, SeQuadT>(step_qdags[last], limit > 0, limit, parallel_level, step_results);
                results_f = step_results;
            }

            bool reached_target = (upper_bound == 0) || (results_f >= upper_bound);
            bool all_done = true;
            for (size_t i = 0; i + 1 < n_steps; ++i)
                if (!step_done[i]) {
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

        for (auto* rj : step_join)
            delete rj;

        if (mxm) {
            auto& last_step = plan_meta.steps[n_steps - 1];
            if (last_step.result_qt) {
                att_set att_to_remove_final;
                for (size_t att = 1; att < k; att++)
                    att_to_remove_final.push_back((uint16_t)att);
                SeQuadT* quadtree_proj_final = new SeQuadT(last_step.result_qt, att_to_remove_final, last_step.result_qt->getK(), 2, last_step.result_qt->getHeight());
                results_f = quadtree_proj_final->total_ones_last_level();
                mxm_quadtrees.push_back(quadtree_proj_final);

                att_set att_to_remove_final1 = {AT_Y};
                SeQuadT* quadtree_proj_final_y = new SeQuadT(quadtree_proj_final, att_to_remove_final1, quadtree_proj_final->getK(), 1, last_step.result_qt->getHeight());
                mxm_quadtrees.push_back(quadtree_proj_final_y);

                att_set att_to_remove_final2 = {AT_X};
                SeQuadT* quadtree_proj_final_x = new SeQuadT(quadtree_proj_final, att_to_remove_final2, quadtree_proj_final->getK(), 1, last_step.result_qt->getHeight());
                mxm_quadtrees.push_back(quadtree_proj_final_x);
            }
        }

        SeQuadT* final_qt = plan_meta.steps[n_steps - 1].result_qt;
        plan_meta.steps[n_steps - 1].result_qt = nullptr;
        return final_qt;
    }

} // namespace gdj

#endif // QDAGS_STRATEGY_GENERIC_STRATEGIES_PLAN_RESUMABLE_HPP
