#ifndef QDAGS_STRATEGY_GENERIC_STRATEGIES_MXM_HPP
#define QDAGS_STRATEGY_GENERIC_STRATEGIES_MXM_HPP

#include <unordered_map>
#include <set>
#include <vector>
#include <cstdint>

#include "../index/commun.hpp"
#include "../join/joins.hpp"
#include "../query/resource_manager.hpp"
#include "../query/triple_pattern.hpp"

// Multiplicacion de matrices sobre 2 relaciones binarias R(y,x)/S(x,z): R x S proyectado sobre (y,z)
// (strategy_matrix_multiply) o, con constantes en la query, la version con proyecciones (strategy_matrix_multiply_wp),
// que pre-une O(R)/S(S) sobre x y luego une con las relaciones de constantes. strategy_matrix_multiply_with_proj
// da lo mismo que strategy_matrix_multiply pero recibe las proyecciones ya construidas.

namespace gdj {

    inline constexpr uint16_t AT_X = 0;
    inline constexpr uint16_t AT_Y = 1;
    inline constexpr uint16_t AT_Z = 2;

    // Contexto de las 2 variantes: rel_R/rel_S en la basica (build_mxm_context); relevant_qts_consts, qts_proy_o_s,
    // att_list y att_ext en la WP (build_mxm_wp_context).
    template<class SeQuadT>
    struct query_context_mxm {
        SeQuadT* rel_R = nullptr;
        SeQuadT* rel_S = nullptr;

        std::vector<SeQuadT*> relevant_qts_consts;
        std::vector<SeQuadT*> qts_proy_o_s;

        std::vector<att_set> att_list;
        att_set att_ext;

        uint64_t grid_side = 52000000;
        uint8_t k = 2;
    };

    // Contexto basico: R = SO del primer patron, S = SO del segundo; el resto de la query se ignora.
    template<class SeQuadT>
    void build_mxm_context(const query_tp& query, resource_manager<SeQuadT>& res, query_context_mxm<SeQuadT>& ctx) {
        if (query.size() >= 2) {
            ctx.rel_R = res.get_so(query[0].get_var_p());
            ctx.rel_S = res.get_so(query[1].get_var_p());
        }
    }

    // Contexto WP: la variable compartida entre el 1er y 2do patron (x, la primera non-lonely) se pre-une por sus
    // proyecciones O/S; los demas patrones (constantes) entran como relaciones SO en el join final.
    template<class SeQuadT>
    void build_mxm_wp_context(const query_tp& query, resource_manager<SeQuadT>& res, query_context_mxm<SeQuadT>& ctx) {
        if (query.size() < 2)
            return;

        for (const auto& tp : query) {
            if (!tp.p_is_variable())
                ctx.relevant_qts_consts.push_back(res.get_so(tp.get_var_p()));
        }

        ctx.qts_proy_o_s.push_back(res.get_o(query[0].get_var_p()));
        ctx.qts_proy_o_s.push_back(res.get_s(query[1].get_var_p()));

        std::unordered_map<uint64_t, int> freq;
        for (const auto& tp : query) {
            if (tp.s_is_variable())
                freq[tp.get_var_s()]++;
            if (tp.o_is_variable())
                freq[tp.get_var_o()]++;
        }

        uint16_t att_non_lonely = 0;
        for (auto const& [var, count] : freq) {
            if (count > 1) {
                att_non_lonely = (uint16_t)var;
                break;
            }
        }

        std::set<uint16_t> unique_vars;
        for (const auto& tp : query) {
            att_set att;
            uint16_t s = (tp.get_var_s() == att_non_lonely) ? 0 : (tp.get_var_s() == 0 ? att_non_lonely : (uint16_t)tp.get_var_s());
            uint16_t o = (tp.get_var_o() == att_non_lonely) ? 0 : (tp.get_var_o() == 0 ? att_non_lonely : (uint16_t)tp.get_var_o());
            att.push_back(s); att.push_back(o);
            ctx.att_list.push_back(att);
            unique_vars.insert(s); unique_vars.insert(o);
        }
        ctx.att_ext.assign(unique_vars.begin(), unique_vars.end());
    }

    // Nucleo comun: con quadtree_proj_R/S ya construidas (del caller) hace el prefiltro sobre x, el join final de
    // 3 atributos con R/S completas y la proyeccion final sobre (y,z).
    template<class QdagT, class SeQuadT>
    uint64_t strategy_matrix_multiply_core(SeQuadT* quadtree_rel_R, SeQuadT* quadtree_proj_R,
                                            SeQuadT* quadtree_rel_S, SeQuadT* quadtree_proj_S, uint64_t grid_side,
                                            std::vector<QdagT*>& temp_qdags, std::vector<QdagT*>& temp_proj_qdags,
                                            SeQuadT*& quadtree_join_proj, SeQuadT*& quadtree_join_final,
                                            SeQuadT*& quadtree_proj_final, uint64_t upper_bound, uint64_t parallel_cut_level) {
        att_set att_R = {AT_Y, AT_X};
        att_set att_S = {AT_X, AT_Z};
        att_set att_R_ext_3 = {AT_X, AT_Y, AT_Z};
        uint8_t k = 2;

        att_set att_proj_1X = {AT_X};
        temp_qdags.push_back(new QdagT(quadtree_proj_R, att_proj_1X, grid_side, att_proj_1X));
        temp_qdags.push_back(new QdagT(quadtree_proj_S, att_proj_1X, grid_side, att_proj_1X));

        uint64_t results_join;
        quadtree_join_proj = multiJoinNew<QdagT, SeQuadT>(temp_qdags, false, 0, 0, results_join);

        temp_proj_qdags.push_back(new QdagT(quadtree_join_proj, att_proj_1X, grid_side, att_R_ext_3));
        temp_proj_qdags.push_back(new QdagT(quadtree_rel_R, att_R, grid_side, att_R_ext_3));
        temp_proj_qdags.push_back(new QdagT(quadtree_rel_S, att_S, grid_side, att_R_ext_3));

        uint64_t results_f;
        quadtree_join_final = multiJoinNew<QdagT, SeQuadT>(temp_proj_qdags, upper_bound > 0, upper_bound, parallel_cut_level, results_f);

        // Se quita AT_X (la variable del join): R x S existencializa x y deja (y,z).
        att_set att_to_remove_final = {AT_X};
        uint8_t d_proj_final = 2;
        quadtree_proj_final = new SeQuadT(quadtree_join_final, att_to_remove_final, k, d_proj_final, quadtree_rel_R->getHeight());

        return results_f;
    }

    // R(y,x) JOIN S(x,z) proyectado sobre (y,z). Los 5 quadtrees intermedios se devuelven por referencia
    // (ownership del caller).
    template<class QdagT, class SeQuadT>
    uint64_t strategy_matrix_multiply(SeQuadT* quadtree_rel_R, SeQuadT* quadtree_rel_S, uint64_t grid_side,
                                       SeQuadT*& quadtree_proj_R, SeQuadT*& quadtree_proj_S,
                                       std::vector<QdagT*>& temp_qdags, std::vector<QdagT*>& temp_proj_qdags,
                                       SeQuadT*& quadtree_join_proj, SeQuadT*& quadtree_join_final,
                                       SeQuadT*& quadtree_proj_final, uint64_t upper_bound, uint64_t parallel_cut_level) {
        att_set att_to_remove_R = {0};
        att_set att_to_remove_S = {1};
        uint8_t d_projected = 1;
        uint8_t k = 2;

        quadtree_proj_R = new SeQuadT(quadtree_rel_R, att_to_remove_R, k, d_projected, quadtree_rel_R->getHeight());
        quadtree_proj_S = new SeQuadT(quadtree_rel_S, att_to_remove_S, k, d_projected, quadtree_rel_S->getHeight());

        return strategy_matrix_multiply_core<QdagT, SeQuadT>(
            quadtree_rel_R, quadtree_proj_R, quadtree_rel_S, quadtree_proj_S, grid_side,
            temp_qdags, temp_proj_qdags, quadtree_join_proj, quadtree_join_final, quadtree_proj_final,
            upper_bound, parallel_cut_level);
    }

    // Como strategy_matrix_multiply, pero recibe quadtree_proj_R/S ya construidas y no las libera.
    template<class QdagT, class SeQuadT>
    uint64_t strategy_matrix_multiply_with_proj(SeQuadT* quadtree_rel_R, SeQuadT* quadtree_proj_R,
                                                 SeQuadT* quadtree_rel_S, SeQuadT* quadtree_proj_S, uint64_t grid_side,
                                                 std::vector<QdagT*>& temp_qdags, std::vector<QdagT*>& temp_proj_qdags,
                                                 SeQuadT*& quadtree_join_proj, SeQuadT*& quadtree_join_final,
                                                 SeQuadT*& quadtree_proj_final, uint64_t upper_bound, uint64_t parallel_cut_level) {
        return strategy_matrix_multiply_core<QdagT, SeQuadT>(
            quadtree_rel_R, quadtree_proj_R, quadtree_rel_S, quadtree_proj_S, grid_side,
            temp_qdags, temp_proj_qdags, quadtree_join_proj, quadtree_join_final, quadtree_proj_final,
            upper_bound, parallel_cut_level);
    }

    // Con proyecciones (WP): Fase 1 pre-une O(R)/S(S) sobre x; Fase 2 une ese resultado con las relaciones de
    // constantes y separa las proyecciones x/y finales.
    template<class QdagT, class SeQuadT>
    uint64_t strategy_matrix_multiply_wp(query_context_mxm<SeQuadT>& ctx,
                                          std::vector<QdagT*>& temp_qdags, std::vector<QdagT*>& temp_proj_qdags,
                                          SeQuadT*& quadtree_join_proj, SeQuadT*& quadtree_join_final, SeQuadT*& quadtree_proj_final,
                                          SeQuadT*& quadtree_proj_final_x, SeQuadT*& quadtree_proj_final_y) {
        att_set att_1 = {0};

        for (auto* qt : ctx.qts_proy_o_s)
            temp_qdags.push_back(new QdagT(qt, att_1, ctx.grid_side, att_1));

        uint64_t res_join;
        quadtree_join_proj = multiJoinNew<QdagT, SeQuadT>(temp_qdags, false, 0, 0, res_join);

        temp_proj_qdags.reserve(ctx.relevant_qts_consts.size() + 1);
        temp_proj_qdags.push_back(new QdagT(quadtree_join_proj, att_1, ctx.grid_side, ctx.att_ext));
        for (size_t i = 0; i < ctx.relevant_qts_consts.size(); ++i)
            temp_proj_qdags.push_back(new QdagT(ctx.relevant_qts_consts[i], ctx.att_list[i], ctx.grid_side, ctx.att_ext));

        uint64_t res_f;
        quadtree_join_final = multiJoinNew<QdagT, SeQuadT>(temp_proj_qdags, false, 0, 0, res_f);

        // Se quita AT_X. quadtree_proj_final_x/_y son proyecciones unarias que ningun caller lee (solo se liberan).
        att_set rem_y = {AT_Y}, rem_x = {AT_X};
        quadtree_proj_final = new SeQuadT(quadtree_join_final, rem_x, ctx.k, 2, ctx.relevant_qts_consts[0]->getHeight());
        quadtree_proj_final_x = new SeQuadT(quadtree_proj_final, rem_y, ctx.k, 1, ctx.relevant_qts_consts[0]->getHeight());
        quadtree_proj_final_y = new SeQuadT(quadtree_proj_final, rem_x, ctx.k, 1, ctx.relevant_qts_consts[0]->getHeight());
        return res_f;
    }

} // namespace gdj

#endif // QDAGS_STRATEGY_GENERIC_STRATEGIES_MXM_HPP
