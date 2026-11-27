#ifndef QDAGS_STRATEGY_GENERIC_STRATEGIES_PLAN_HPP
#define QDAGS_STRATEGY_GENERIC_STRATEGIES_PLAN_HPP

#include <vector>
#include <algorithm>
#include <cstdint>
#include "coefficient_growth.hpp"

#include "generic_strategies.hpp"
#include "generic_strategies_mxm.hpp"
#include "../query/plan_builder.hpp"

// Ejecuta un query_metadata_plan (query/plan_builder.hpp) paso a paso: cada paso no final es un prejoin
// materializado con multiJoinAuto en step.result_qt; el ultimo es el join final en el espacio global. Con
// mxm=true (generic_strategies_mxm.hpp) el ultimo paso proyecta quitando los atributos 1..k-1 y cuenta
// total_ones_last_level(). `k` es la aridad de la cadena (P2 -> 2, P3 -> 3, P4 -> 4), no la base del quadtree.

namespace gdj {

    // Ejecuta el plan paso a paso. `mxm_quadtrees` recibe los quadtrees de proyeccion de la rama mxm y `all_qdags`
    // los qdag creados (ambos del caller). Retorna el SeQuadT* del ultimo paso (ownership del caller; su
    // result_qt queda en nullptr para que clearResults() no lo borre); la cardinalidad va por `results_f`.
    // mxm_unary_projections: con mxm=true tambien arma las proyecciones unarias _y/_x de la proyeccion final
    // (mxm_quadtrees[1]/[2]), necesarias solo si el resultado alimenta otro prejoin; false las omite.
    template<class QdagT, class SeQuadT>
    SeQuadT* strategy_plan(query_metadata_plan<SeQuadT>& plan_meta,
                            std::vector<QdagT*>& all_qdags,
                            std::vector<SeQuadT*>& mxm_quadtrees,
                            uint64_t& results_f,
                            uint64_t upper_bound,
                            uint64_t parallel_level,
                            uint64_t grid_side,
                            uint32_t coefficient_bound,
                            bool mxm,
                            uint8_t k,
                            ParallelPhases parallel_phases = ParallelPhases::LastOnly,
                            uint64_t* out_final_size_bytes = nullptr,
                            bool mxm_unary_projections = true) {
        results_f = 0;

        for (size_t i = 0; i < plan_meta.steps.size(); ++i) {
            auto& sm = plan_meta.steps[i];
            std::vector<QdagT*> current_qdags;
            uint64_t step_results = 0;

            bool is_last_step = (i == plan_meta.steps.size() - 1);
            std::sort(sm.output_att_org.begin(), sm.output_att_org.end());

            size_t prejoin_pos = 0;
            bool have_op_proj = false;
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
                        QdagT* q = new QdagT(prev_res, is_last_step ? prev_out_att : sm.atts_step[op_idx], grid_side, is_last_step ? sm.output_att_org : sm.output_att_ext_prejoin, plan_meta.wide);
                        current_qdags.push_back(q);
                        all_qdags.push_back(q);
                    }
                    prejoin_pos++;
                } else {
                    QdagT* q = new QdagT(sm.qts[op_idx], sm.atts_step[op_idx], grid_side, is_last_step ? sm.output_att_org : sm.output_att_ext_prejoin, plan_meta.wide);
                    current_qdags.push_back(q);
                    all_qdags.push_back(q);
                }
                if (sm.op_types[op_idx] == OpType::PROJECTION)
                    have_op_proj = true;
            }

            uint64_t limit = 0;
            if (coefficient_bound > 0)
                limit = is_last_step ? upper_bound : coefficient_bound * upper_bound;
            else
                limit = is_last_step ? upper_bound : (!have_op_proj ? upper_bound : 0);

            bool step_par = (parallel_phases == ParallelPhases::AllPhases)
                         || (is_last_step  && (parallel_phases == ParallelPhases::LastOnly  || parallel_phases == ParallelPhases::LastTwo))
                         || (!is_last_step && (parallel_phases == ParallelPhases::FirstOnly || parallel_phases == ParallelPhases::FirstTwo));
            uint64_t p_level = step_par ? parallel_level : 0;

            if (!current_qdags.empty())
                sm.result_qt = multiJoinAuto<QdagT, SeQuadT>(current_qdags, limit > 0, limit, p_level, step_results);

            if (is_last_step) {
                results_f = step_results;
                if (mxm && sm.result_qt) {
                    att_set att_to_remove_final;
                    for (size_t att = 1; att < k; att++)
                        att_to_remove_final.push_back((uint16_t)att);
                    SeQuadT* quadtree_proj_final = new SeQuadT(sm.result_qt, att_to_remove_final, sm.result_qt->getK(), 2, sm.result_qt->getHeight());
                    results_f = quadtree_proj_final->total_ones_last_level();
                    mxm_quadtrees.push_back(quadtree_proj_final);

                    if (mxm_unary_projections) {
                        att_set att_to_remove_final1 = {AT_Y};
                        SeQuadT* quadtree_proj_final_y = new SeQuadT(quadtree_proj_final, att_to_remove_final1, quadtree_proj_final->getK(), 1, sm.result_qt->getHeight());
                        mxm_quadtrees.push_back(quadtree_proj_final_y);

                        att_set att_to_remove_final2 = {AT_X};
                        SeQuadT* quadtree_proj_final_x = new SeQuadT(quadtree_proj_final, att_to_remove_final2, quadtree_proj_final->getK(), 1, sm.result_qt->getHeight());
                        mxm_quadtrees.push_back(quadtree_proj_final_x);
                    }
                }
                if (out_final_size_bytes && sm.result_qt)
                    *out_final_size_bytes = sm.result_qt->size_in_bytes();
            }
        }

        SeQuadT* final_qt = nullptr;
        if (!plan_meta.steps.empty()) {
            final_qt = plan_meta.steps.back().result_qt;
            plan_meta.steps.back().result_qt = nullptr;
        }
        return final_qt;
    }

    // strategy_plan adaptive: reintenta los pasos no finales con coefficient_bound creciente, sin recalcular los
    // que ya no crecieron (`step_done`). El ultimo paso siempre se recalcula. Retorna el ultimo paso (ownership
    // del caller); la cardinalidad va por `results_f`.
    template<class QdagT, class SeQuadT>
    SeQuadT* strategy_plan_adaptive(query_metadata_plan<SeQuadT>& plan_meta,
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
                                     ParallelPhases parallel_phases = ParallelPhases::LastOnly,
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

        uint64_t coefficient_bound = (upper_bound == 0) ? 0 : std::max<uint64_t>(initial_coefficient_bound, 1);
        uint32_t attempt = 0;
        std::vector<bool> step_done(n_steps, false);
        bool exhausted = false;

        // Prejoins de entrada de cada paso. Un paso esta terminado solo si no llego a la cota y sus entradas ya
        // lo estaban: sobre una entrada cortada, "no llego a la cota" no significa "completo".
        std::vector<std::vector<size_t>> deps(n_steps);
        for (size_t i = 0; i < n_steps; ++i) {
            auto& sm = plan_meta.steps[i];
            size_t prejoin_pos = 0;
            for (size_t op_idx = 0; op_idx < sm.op_types.size(); ++op_idx) {
                if (sm.op_types[op_idx] != OpType::PREJOIN)
                    continue;
                for (size_t prev_i = 0; prev_i < i; ++prev_i) {
                    if (plan_meta.steps[prev_i].step_id == sm.prev_prejoin_id[prejoin_pos]) {
                        deps[i].push_back(prev_i);
                        break;
                    }
                }
                prejoin_pos++;
            }
        }

        while (true) {
            ++attempt;

            for (size_t i = 0; i < n_steps; ++i) {
                bool is_last_step = (i == n_steps - 1);
                if (!is_last_step && step_done[i])
                    continue;
                bool deps_done = true;
                for (size_t d : deps[i])
                    if (!step_done[d]) {
                        deps_done = false;
                        break;
                    }

                auto& sm = plan_meta.steps[i];
                std::vector<QdagT*> current_qdags;
                uint64_t step_results = 0;
                std::sort(sm.output_att_org.begin(), sm.output_att_org.end());

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
                            QdagT* q = new QdagT(prev_res, is_last_step ? prev_out_att : sm.atts_step[op_idx], grid_side, is_last_step ? sm.output_att_org : sm.output_att_ext_prejoin, plan_meta.wide);
                            current_qdags.push_back(q);
                            all_qdags.push_back(q);
                        }
                        prejoin_pos++;
                    } else {
                        QdagT* q = new QdagT(sm.qts[op_idx], sm.atts_step[op_idx], grid_side, is_last_step ? sm.output_att_org : sm.output_att_ext_prejoin, plan_meta.wide);
                        current_qdags.push_back(q);
                        all_qdags.push_back(q);
                    }
                }

                if (current_qdags.empty()) {
                    if (!is_last_step)
                        step_done[i] = true;
                    continue;
                }

                uint64_t limit = is_last_step ? upper_bound : coefficient_bound * upper_bound;
                bool step_par = (parallel_phases == ParallelPhases::AllPhases)
                             || (is_last_step  && (parallel_phases == ParallelPhases::LastOnly  || parallel_phases == ParallelPhases::LastTwo))
                             || (!is_last_step && (parallel_phases == ParallelPhases::FirstOnly || parallel_phases == ParallelPhases::FirstTwo));
                uint64_t p_level = step_par ? parallel_level : 0;

                if (sm.result_qt) {
                    delete sm.result_qt;
                    sm.result_qt = nullptr;
                }
                sm.result_qt = multiJoinAuto<QdagT, SeQuadT>(current_qdags, limit > 0, limit, p_level, step_results);

                if (is_last_step)
                    results_f = step_results;
                else if ((limit == 0 || step_results < limit) && deps_done)
                    step_done[i] = true;
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

        if (mxm) {
            auto& last = plan_meta.steps[n_steps - 1];
            if (last.result_qt) {
                att_set att_to_remove_final;
                for (size_t att = 1; att < k; att++)
                    att_to_remove_final.push_back((uint16_t)att);
                SeQuadT* quadtree_proj_final = new SeQuadT(last.result_qt, att_to_remove_final, last.result_qt->getK(), 2, last.result_qt->getHeight());
                results_f = quadtree_proj_final->total_ones_last_level();
                mxm_quadtrees.push_back(quadtree_proj_final);

                att_set att_to_remove_final1 = {AT_Y};
                SeQuadT* quadtree_proj_final_y = new SeQuadT(quadtree_proj_final, att_to_remove_final1, quadtree_proj_final->getK(), 1, last.result_qt->getHeight());
                mxm_quadtrees.push_back(quadtree_proj_final_y);

                att_set att_to_remove_final2 = {AT_X};
                SeQuadT* quadtree_proj_final_x = new SeQuadT(quadtree_proj_final, att_to_remove_final2, quadtree_proj_final->getK(), 1, last.result_qt->getHeight());
                mxm_quadtrees.push_back(quadtree_proj_final_x);
            }
        }

        SeQuadT* final_qt = plan_meta.steps[n_steps - 1].result_qt;
        plan_meta.steps[n_steps - 1].result_qt = nullptr;
        return final_qt;
    }

} // namespace gdj

#endif // QDAGS_STRATEGY_GENERIC_STRATEGIES_PLAN_HPP
