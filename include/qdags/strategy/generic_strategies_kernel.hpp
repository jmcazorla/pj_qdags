#ifndef QDAGS_STRATEGY_GENERIC_STRATEGIES_KERNEL_HPP
#define QDAGS_STRATEGY_GENERIC_STRATEGIES_KERNEL_HPP

#include <vector>
#include <cstdint>

#include "generic_strategies.hpp"

// Estrategia Kernel (GRADES-NDA'26, 4.3): misma metadata que NonLonely (build_nl_metadata), otra descomposicion.
//   1. I(A') = join de las proyecciones de cada tabla sobre los atributos non-lonely A' (como la Fase 1 de NonLonely).
//   2. Cada tabla con algun atributo lonely se reduce con la proyeccion de I sobre lo que comparte con A':
//      R'_i = R_i join pi_{A' & A_i}(I). Con tablas binarias comparte a lo mas un atributo, asi que la proyeccion
//      es siempre unaria. Una tabla sin atributos en A' no se reduce.
//   3. Join final de las R'_i, mas I salvo cuando |A'| = 1 (cada R'_i ya lo contiene).
//
// Con upper_bound > 0, I y el join final se cortan a upper_bound; I es exacto, asi que el limite queda
// garantizado en una pasada (sin version adaptive/resumable). Las reducciones nunca se cortan: recortar cada
// hermana por separado puede dejarlas con valores incompatibles.

namespace gdj {

    // Maximo de atributos non-lonely: I se re-envuelve en un qdag (hasta 9 dimensiones). A diferencia de Leapfrog,
    // I es un solo bloque y omitirlo da resultados incorrectos, asi que las queries que lo superan se saltan antes
    // de llamar a strategy_kernel (ver examples/run_kernel.cpp).
    constexpr size_t KERNEL_MAX_NONLONELY_ATTS = 9;

    namespace kernel_detail {

        // Posicion de `phys` dentro de `atts`, o -1.
        inline int position_of(const att_set& atts, uint16_t phys) {
            for (size_t i = 0; i < atts.size(); ++i)
                if (atts[i] == phys)
                    return (int)i;
            return -1;
        }

    } // namespace kernel_detail

    // Kernel: I(A') sobre los non-lonely, reduccion de cada tabla con lonely por la proyeccion unaria de I que
    // comparte, y join final en el espacio global (att_ext). Retorna el resultado (ownership del caller); la
    // cardinalidad va por `results_f` y `all_qdags` recibe los qdag creados. Sin lonely no hay nada que reducir
    // (el join de las tablas es el resultado); sin non-lonely no hay I.
    // `SeQuadTOut` es el backend del resultado final; I y las reducciones quedan en SeQuadT.
    template<class QdagT, class SeQuadT, class SeQuadTOut = SeQuadT>
    SeQuadTOut* strategy_kernel(query_metadata_nl<SeQuadT>& meta, std::vector<QdagT*>& all_qdags,
                                uint64_t& results_f,
                                uint64_t upper_bound, uint64_t parallel_level, uint64_t grid_side,
                                ParallelPhases parallel_phases = ParallelPhases::LastOnly,
                                uint64_t* out_final_size_bytes = nullptr) {
        results_f = 0;

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

        const bool has_nl = !meta.nl_quadtrees.empty();
        const bool has_l = !meta.l_quadtrees.empty();
        if (!has_nl && !has_l)
            return nullptr;

        std::vector<QdagT*> final_qdags;
        std::vector<SeQuadT*> temp_qts;

        // Sin lonely el join de las tablas es el resultado; se arma en el espacio global para quedar en el mismo
        // orden que las demas estrategias.
        if (!has_l) {
            for (size_t idx = 0; idx < meta.nl_quadtrees.size(); ++idx) {
                att_set phys;
                for (uint16_t local : meta.att_list_nl[idx])
                    phys.push_back(meta.att_nl_orig[local]);
                auto* q = new QdagT(meta.nl_quadtrees[idx], phys, grid_side, meta.att_ext, meta.wide);
                all_qdags.push_back(q);
                final_qdags.push_back(q);
            }
            SeQuadTOut* res = multiJoinAuto<QdagT, SeQuadTOut>(final_qdags, upper_bound > 0, upper_bound, phase3_cut, results_f);
            if (res && out_final_size_bytes)
                *out_final_size_bytes = res->size_in_bytes();
            return res;
        }

        // Fase 1: I(A') en el espacio local renumerado.
        const size_t d_nl = meta.att_nl.size();
        SeQuadT* I = nullptr;
        if (has_nl) {
            std::vector<QdagT*> nl_qdags;
            for (size_t idx = 0; idx < meta.nl_quadtrees.size(); ++idx)
                nl_qdags.push_back(new QdagT(meta.nl_quadtrees[idx], meta.att_list_nl[idx], grid_side, meta.att_nl, meta.wide));
            all_qdags.insert(all_qdags.end(), nl_qdags.begin(), nl_qdags.end());
            uint64_t n_i = 0;
            I = multiJoinAuto<QdagT, SeQuadT>(nl_qdags, upper_bound > 0, upper_bound, phase1_cut, n_i);
            if (!I)
                return nullptr;
            temp_qts.push_back(I);
        }

        // Fase 2: una reduccion por tabla con lonely; las proyecciones de I se comparten por variable.
        att_set att_2d = {0, 1};
        std::vector<SeQuadT*> proj_of(d_nl, nullptr);
        size_t n_reduced = 0;

        for (size_t idx = 0; idx < meta.l_quadtrees.size(); ++idx) {
            const att_set& phys = meta.att_list[idx];
            int table_pos = -1;   // posicion (0 o 1) en la tabla del atributo compartido con A'
            int local_pos = -1;   // su posicion en el espacio local de I
            for (size_t j = 0; j < phys.size() && table_pos < 0; ++j) {
                int lp = kernel_detail::position_of(meta.att_nl_orig, phys[j]);
                if (lp >= 0) {
                    table_pos = (int)j;
                    local_pos = lp;
                }
            }

            if (table_pos < 0 || !I) {
                auto* q = new QdagT(meta.l_quadtrees[idx], meta.att_list[idx], grid_side, meta.att_ext, meta.wide);
                all_qdags.push_back(q);
                final_qdags.push_back(q);
                continue;
            }

            SeQuadT* I_a = I;
            if (d_nl > 1) {
                if (!proj_of[local_pos]) {
                    std::vector<uint16_t> remove_pos;
                    for (uint16_t p = 0; p < d_nl; ++p)
                        if (p != local_pos)
                            remove_pos.push_back(p);
                    proj_of[local_pos] = new SeQuadT(I, remove_pos, I->getK(), 1, I->getHeight());
                    temp_qts.push_back(proj_of[local_pos]);
                }
                I_a = proj_of[local_pos];
            }

            att_set att_shared = {(uint16_t)table_pos};
            auto* q_table = new QdagT(meta.l_quadtrees[idx], att_2d, grid_side, att_2d);
            auto* q_proj = new QdagT(I_a, att_shared, grid_side, att_2d);
            std::vector<QdagT*> red_qdags = {q_table, q_proj};
            all_qdags.push_back(q_table);
            all_qdags.push_back(q_proj);

            uint64_t n_red = 0;
            SeQuadT* reduced = multiJoinAuto<QdagT, SeQuadT>(red_qdags, false, 0, phase2_cut, n_red);
            if (reduced)
                temp_qts.push_back(reduced);
            ++n_reduced;

            auto* q = new QdagT(reduced, meta.att_list[idx], grid_side, meta.att_ext, meta.wide);
            all_qdags.push_back(q);
            final_qdags.push_back(q);
        }

        // Fase 3: I sobra solo si A' tiene un atributo y una tabla reducida lo contiene; con una tabla binaria
        // dentro de A' su restriccion solo vive en I y se queda.
        bool binary_nl = false;
        for (const auto& a : meta.att_list_nl)
            if (a.size() > 1) {
                binary_nl = true;
                break;
            }
        bool keep_I = I && (d_nl > 1 || n_reduced == 0 || binary_nl);
        if (keep_I) {
            auto* q = new QdagT(I, meta.att_nl_orig, grid_side, meta.att_ext, meta.wide);
            all_qdags.push_back(q);
            final_qdags.push_back(q);
        }

        SeQuadTOut* final_qt = multiJoinAuto<QdagT, SeQuadTOut>(final_qdags, upper_bound > 0, upper_bound, phase3_cut, results_f);
        if (final_qt && out_final_size_bytes)
            *out_final_size_bytes = final_qt->size_in_bytes();

        for (auto* qt : temp_qts)
            delete qt;
        return final_qt;
    }

}

#endif // QDAGS_STRATEGY_GENERIC_STRATEGIES_KERNEL_HPP
