#ifndef QDAGS_STRATEGY_GENERIC_STRATEGIES_HPP
#define QDAGS_STRATEGY_GENERIC_STRATEGIES_HPP

#include <vector>
#include <cstdint>
#include <type_traits>

#include "../index/commun.hpp"
#include "../query/query_builder.hpp"
#include "../join/joins.hpp"
#include "../join/joins_wide.hpp"
#include "coefficient_growth.hpp"

// Estrategias Orig, NonLonely (One-Shot) y WeakKernel sobre multiJoinAuto, mas WeakKernel adaptive (reintenta
// con coefficient_bound creciente). Race y Resumable estan en archivos aparte.

namespace gdj {

    // Fases que reciben cut_off_level>0; el resto corre secuencial. Cada estrategia mapea sus fases sobre la
    // secuencia First..Last.
    enum class ParallelPhases : uint8_t {
        LastOnly  = 0,
        LastTwo   = 1,
        AllPhases = 2,
        FirstOnly = 3,
        FirstTwo  = 4
    };

    // Orig: un solo multiJoinAuto sobre todos los qdags de la query. `all_qdags` y el resultado son del caller.
    template<class QdagT, class SeQuadT>
    SeQuadT* strategy_orig(const query_relations<SeQuadT>& rel, std::vector<QdagT*>& all_qdags,
                            uint64_t& results,
                            uint64_t upper_bound, uint64_t parallel_cut_level, uint64_t grid_side) {
        all_qdags.reserve(all_qdags.size() + rel.quadtrees.size());
        for (size_t i = 0; i < rel.quadtrees.size(); ++i) {
            att_set base = rel.att_list[i];
            all_qdags.push_back(new QdagT(rel.quadtrees[i], base, grid_side, rel.att_ext, rel.wide));
        }
        results = 0;
        return multiJoinAuto<QdagT, SeQuadT>(all_qdags, upper_bound > 0, upper_bound, parallel_cut_level, results);
    }

    // NonLonely (One-Shot): Fase 1 pre-une las variables non-lonely en un espacio local renumerado; Fase 2 la
    // extiende con las relaciones lonely en el espacio global. Sin patrones lonely la Fase 1 es el resultado.
    // `SeQuadTOut` es el backend del resultado final (el join final no se re-envuelve en un QdagT); si difiere de
    // SeQuadT y no hay lonely, la Fase 1 se calcula directo en SeQuadTOut y `quadtree_join_proj` queda en nullptr.
    template<class QdagT, class SeQuadT, class SeQuadTOut = SeQuadT>
    SeQuadTOut* strategy_non_lonely(query_metadata_nl<SeQuadT>& meta,
                                  std::vector<QdagT*>& qdags_non_lonely, std::vector<QdagT*>& qdags_f,
                                  SeQuadT*& quadtree_join_proj, SeQuadTOut*& quadtree_join_final,
                                  uint64_t& results_f,
                                  uint64_t upper_bound, uint64_t parallel_cut_level, uint64_t grid_side,
                                  ParallelPhases parallel_phases = ParallelPhases::LastOnly,
                                  uint64_t* out_final_size_bytes = nullptr) {
        results_f = 0;

        bool phase1_par = parallel_phases == ParallelPhases::FirstOnly
                        || parallel_phases == ParallelPhases::FirstTwo
                        || parallel_phases == ParallelPhases::AllPhases;
        bool phase2_par = parallel_phases == ParallelPhases::LastOnly
                        || parallel_phases == ParallelPhases::LastTwo
                        || parallel_phases == ParallelPhases::AllPhases;
        uint64_t phase1_cut = phase1_par ? parallel_cut_level : 0;
        uint64_t phase2_cut = phase2_par ? parallel_cut_level : 0;

        for (size_t idx = 0; idx < meta.nl_quadtrees.size(); ++idx)
            qdags_non_lonely.push_back(new QdagT(meta.nl_quadtrees[idx], meta.att_list_nl[idx], grid_side, meta.att_nl, meta.wide));

        if constexpr (!std::is_same_v<SeQuadT, SeQuadTOut>) {
            if (meta.l_quadtrees.empty()) {
                // Sin lonely la Fase 1 es el resultado: se calcula ya en el backend de salida.
                quadtree_join_proj = nullptr;
                quadtree_join_final = multiJoinAuto<QdagT, SeQuadTOut>(qdags_non_lonely, upper_bound > 0, upper_bound, phase1_cut, results_f);
                if (out_final_size_bytes && quadtree_join_final)
                    *out_final_size_bytes = quadtree_join_final->size_in_bytes();
                return quadtree_join_final;
            }
        }

        quadtree_join_proj = multiJoinAuto<QdagT, SeQuadT>(qdags_non_lonely, upper_bound > 0, upper_bound, phase1_cut, results_f);

        if (meta.l_quadtrees.size() > 0) {
            qdags_f.push_back(new QdagT(quadtree_join_proj, meta.att_nl_orig, grid_side, meta.att_ext, meta.wide));
            for (size_t idx = 0; idx < meta.l_quadtrees.size(); idx++)
                qdags_f.push_back(new QdagT(meta.l_quadtrees[idx], meta.att_list[idx], grid_side, meta.att_ext, meta.wide));

            quadtree_join_final = multiJoinAuto<QdagT, SeQuadTOut>(qdags_f, upper_bound > 0, upper_bound, phase2_cut, results_f);
            if (out_final_size_bytes && quadtree_join_final)
                *out_final_size_bytes = quadtree_join_final->size_in_bytes();
            return quadtree_join_final;
        } else {
            if constexpr (std::is_same_v<SeQuadT, SeQuadTOut>) {
                if (out_final_size_bytes && quadtree_join_proj)
                    *out_final_size_bytes = quadtree_join_proj->size_in_bytes();
                return quadtree_join_proj;
            } else {
                return nullptr;   // inalcanzable: sin lonely y SeQuadTOut != SeQuadT se resolvio arriba
            }
        }
    }

    // WeakKernel: Fase 1 intersecta las proyecciones unarias de cada variable kernel (non-lonely); Fase 2 extiende
    // cada patron con sus referencias al kernel en un espacio local 2D; Fase 3 las lleva al espacio global y las
    // une. `coefficient_bound` multiplica `upper_bound` para la cota de la Fase 2 (0 = sin cota). Retorna el
    // resultado final (ownership del caller).
    // `SeQuadTOut` es el backend solo de la Fase 3: las Fases 1-2 se re-envuelven en un QdagT y deben usar el
    // backend de entrada.
    template<class QdagT, class SeQuadT, class SeQuadTOut = SeQuadT>
    SeQuadTOut* strategy_weak_kernel(query_metadata_wk<SeQuadT>& meta, std::vector<QdagT*>& all_qdags,
                                   uint64_t& results_f,
                                   uint64_t upper_bound, uint64_t coefficient_bound, uint64_t parallel_level,
                                   uint64_t grid_side, ParallelPhases parallel_phases = ParallelPhases::LastOnly,
                                   uint64_t* out_final_size_bytes = nullptr) {
        results_f = 0;
        std::vector<SeQuadT*> temp_qts;

        bool phase1_par = parallel_phases == ParallelPhases::AllPhases
                        || parallel_phases == ParallelPhases::FirstOnly
                        || parallel_phases == ParallelPhases::FirstTwo;
        bool phase2_par = parallel_phases == ParallelPhases::AllPhases
                        || parallel_phases == ParallelPhases::LastTwo
                        || parallel_phases == ParallelPhases::FirstTwo;
        bool phase3_par = parallel_phases == ParallelPhases::AllPhases
                        || parallel_phases == ParallelPhases::LastTwo
                        || parallel_phases == ParallelPhases::LastOnly;

        uint64_t phase1_cut = phase1_par ? parallel_level : 0;
        uint64_t phase2_cut = phase2_par ? parallel_level : 0;
        uint64_t phase3_cut = phase3_par ? parallel_level : 0;

        att_set att_1d = {0};
        att_set att_2d = {0, 1};

        // Fase 1: kernel 1D por variable non-lonely.
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
            kernel_qts[k_idx] = multiJoinAuto<QdagT, SeQuadT>(k_qdags, false, 0, phase1_cut, dummy);
            if (kernel_qts[k_idx])
                temp_qts.push_back(kernel_qts[k_idx]);
        }

        // Fase 2: extension binaria por patron en espacio 2D local.
        std::vector<SeQuadT*> extended_qts(meta.patterns.size(), nullptr);
        uint64_t limit = coefficient_bound * upper_bound;
        for (size_t p_idx = 0; p_idx < meta.patterns.size(); ++p_idx) {
            auto& pd = meta.patterns[p_idx];
            std::vector<QdagT*> ext_qdags;
            for (auto& kr : pd.kernel_refs) {
                size_t k_idx = kr.first;
                uint8_t local_pos = kr.second;
                SeQuadT* k_qt = kernel_qts[k_idx];
                if (!k_qt)
                    continue;
                att_set att_k = {(uint16_t)local_pos};
                auto* q = new QdagT(k_qt, att_k, grid_side, att_2d);
                ext_qdags.push_back(q);
                all_qdags.push_back(q);
            }

            if (pd.binary_qt) {
                auto* q = new QdagT(pd.binary_qt, att_2d, grid_side, att_2d);
                ext_qdags.push_back(q);
                all_qdags.push_back(q);
            }
            if (!ext_qdags.empty()) {
                uint64_t dummy = 0;
                extended_qts[p_idx] = multiJoinAuto<QdagT, SeQuadT>(ext_qdags, limit > 0, limit, phase2_cut, dummy);
                if (extended_qts[p_idx])
                    temp_qts.push_back(extended_qts[p_idx]);
            }
        }

        // Fase 3: join final en el espacio fisico global.
        std::vector<QdagT*> final_qdags;
        for (size_t p_idx = 0; p_idx < meta.patterns.size(); ++p_idx) {
            if (!extended_qts[p_idx])
                continue;
            auto& pd = meta.patterns[p_idx];
            att_set att_pat = {pd.phys_s, pd.phys_o};
            auto* q = new QdagT(extended_qts[p_idx], att_pat, grid_side, meta.att_ext, meta.wide);
            final_qdags.push_back(q);
            all_qdags.push_back(q);
        }

        SeQuadTOut* final_qt = nullptr;
        if (!final_qdags.empty()) {
            final_qt = multiJoinAuto<QdagT, SeQuadTOut>(final_qdags, upper_bound > 0, upper_bound, phase3_cut, results_f);
            if (final_qt && out_final_size_bytes)
                *out_final_size_bytes = final_qt->size_in_bytes();
        }

        for (auto* qt : temp_qts)
            delete qt;
        return final_qt;
    }

    // WeakKernel adaptive: la Fase 1 corre una vez; las Fases 2-3 reintentan con un coefficient_bound creciente,
    // sin recalcular los patrones que no llegaron a la cota (`pattern_done`). Retorna el ultimo intento.
    // `SeQuadTOut`: ver strategy_weak_kernel.
    template<class QdagT, class SeQuadT, class SeQuadTOut = SeQuadT>
    SeQuadTOut* strategy_weak_kernel_adaptive(query_metadata_wk<SeQuadT>& meta, std::vector<QdagT*>& all_qdags,
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
        std::vector<SeQuadT*> temp_qts;

        bool phase1_par = parallel_phases == ParallelPhases::AllPhases
                        || parallel_phases == ParallelPhases::FirstOnly
                        || parallel_phases == ParallelPhases::FirstTwo;
        bool phase2_par = parallel_phases == ParallelPhases::AllPhases
                        || parallel_phases == ParallelPhases::LastTwo
                        || parallel_phases == ParallelPhases::FirstTwo;
        bool phase3_par = parallel_phases == ParallelPhases::AllPhases
                        || parallel_phases == ParallelPhases::LastTwo
                        || parallel_phases == ParallelPhases::LastOnly;

        uint64_t phase1_cut = phase1_par ? parallel_level : 0;
        uint64_t phase2_cut = phase2_par ? parallel_level : 0;
        uint64_t phase3_cut = phase3_par ? parallel_level : 0;

        att_set att_1d = {0};
        att_set att_2d = {0, 1};

        // Fase 1: kernel 1D por variable non-lonely (una sola vez).
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
            kernel_qts[k_idx] = multiJoinAuto<QdagT, SeQuadT>(k_qdags, false, 0, phase1_cut, dummy);
            if (kernel_qts[k_idx])
                temp_qts.push_back(kernel_qts[k_idx]);
        }

        // Fases 2+3: bucle adaptativo con memoizacion por patron.
        uint64_t coefficient_bound = (upper_bound == 0) ? 0 : std::max<uint64_t>(initial_coefficient_bound, 1);
        uint32_t attempt = 0;

        std::vector<SeQuadT*> extended_qts(meta.patterns.size(), nullptr);
        std::vector<bool> pattern_done(meta.patterns.size(), false);
        bool exhausted = false;
        SeQuadTOut* final_qt = nullptr;

        while (true) {
            ++attempt;
            uint64_t limit = coefficient_bound * upper_bound;

            for (size_t p_idx = 0; p_idx < meta.patterns.size(); ++p_idx) {
                if (pattern_done[p_idx])
                    continue;

                auto& pd = meta.patterns[p_idx];
                std::vector<QdagT*> ext_qdags;
                for (auto& kr : pd.kernel_refs) {
                    size_t k_idx = kr.first;
                    uint8_t local_pos = kr.second;
                    SeQuadT* k_qt = kernel_qts[k_idx];
                    if (!k_qt)
                        continue;
                    att_set att_k = {(uint16_t)local_pos};
                    auto* q = new QdagT(k_qt, att_k, grid_side, att_2d);
                    ext_qdags.push_back(q);
                    all_qdags.push_back(q);
                }
                if (pd.binary_qt) {
                    auto* q = new QdagT(pd.binary_qt, att_2d, grid_side, att_2d);
                    ext_qdags.push_back(q);
                    all_qdags.push_back(q);
                }
                if (ext_qdags.empty()) {
                    pattern_done[p_idx] = true;
                    continue;
                }

                uint64_t cnt = 0;
                extended_qts[p_idx] = multiJoinAuto<QdagT, SeQuadT>(ext_qdags, limit > 0, limit, phase2_cut, cnt);
                if (extended_qts[p_idx])
                    temp_qts.push_back(extended_qts[p_idx]);

                if (limit == 0 || cnt < limit)
                    pattern_done[p_idx] = true;
            }

            std::vector<QdagT*> final_qdags;
            for (size_t p_idx = 0; p_idx < meta.patterns.size(); ++p_idx) {
                if (!extended_qts[p_idx])
                    continue;
                auto& pd = meta.patterns[p_idx];
                att_set att_pat = {pd.phys_s, pd.phys_o};
                auto* q = new QdagT(extended_qts[p_idx], att_pat, grid_side, meta.att_ext, meta.wide);
                final_qdags.push_back(q);
                all_qdags.push_back(q);
            }

            uint64_t attempt_results = 0;
            if (final_qt) {
                delete final_qt;
                final_qt = nullptr;
            }
            if (!final_qdags.empty()) {
                final_qt = multiJoinAuto<QdagT, SeQuadTOut>(final_qdags, upper_bound > 0, upper_bound, phase3_cut, attempt_results);
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

            uint64_t next = coefficient_bound * growth_factor;
            if (growth_mode == GrowthMode::Proportional)
                next = proportional_next_coefficient(coefficient_bound, growth_factor, upper_bound, results_f);
            coefficient_bound = next < 1 ? 1 : next;
        }

        if (was_exhaustive)
            *was_exhaustive = exhausted || (upper_bound == 0);

        for (auto* qt : temp_qts)
            delete qt;
        return final_qt;
    }

}

#endif // QDAGS_STRATEGY_GENERIC_STRATEGIES_HPP
