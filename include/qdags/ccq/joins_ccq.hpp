#ifndef QDAGS_CCQ_JOINS_HPP
#define QDAGS_CCQ_JOINS_HPP

#include "adapters.hpp"
#include "../join/joins.hpp"   // solo para gdj::timeout_triggered

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

// Portado de ccqQDAGS joins.cpp (AND y multiJoin), secuencial. Diferencias con el original:
//   (T) AND consulta gdj::timeout_triggered al entrar.
//   (U) Sin upper_bound: el original deja el arbol mal formado al llegar al tope; aca se lanza una excepcion
//       (usar UPPER_BOUND=0).
//   (E) Devuelve un arbol vacio si algun qdag no tiene tuplas.
//   (L) Mas de 11 atributos no caben en initializable_array: excepcion.
// cut_off_level se ignora (sin paralelismo). multiJoinAuto tiene la misma firma que gdj::multiJoinAuto; las
// estrategias lo encuentran por ADL cuando QdagT = ccq::qdag_t.

namespace ccq {

    inline bool AND(qdag *Q[], uint64_t *roots, uint16_t nQ,
                    uint16_t cur_level, uint16_t max_level,
                    std::vector<uint64_t> bv[], uint64_t last_pos[], uint64_t nAtt) {
        if (gdj::timeout_triggered.load(std::memory_order_relaxed)) // (T)
            return false;

        uint64_t p = Q[0]->nChildren();
        bool just_zeroes = true;
        uint64_t k_d[nQ];

        uint16_t children_to_recurse[p];
        initializable_array C(p);

        uint64_t i;
        uint64_t children_to_recurse_size = 0;

        if (cur_level == max_level) {
            for (i = 0; i < nQ; ++i) {
                k_d[i] = Q[i]->getKD();
                Q[i]->get_children_lastlevel(roots[i], C, nQ, children_to_recurse, children_to_recurse_size, k_d[i]);
            }

            std::sort(children_to_recurse, children_to_recurse + children_to_recurse_size);

            int64_t last_child = -1;
            uint16_t child;

            for (i = 0; i < children_to_recurse_size; ++i) {
                child = children_to_recurse[i];

                if (child - last_child > 1)
                    last_pos[cur_level] += (child - last_child - 1);

                last_child = child;

                bv[cur_level].push_back(last_pos[cur_level]++);
                just_zeroes = false;
            }

            if (p - last_child > 1)
                last_pos[cur_level] += (p - last_child - 1);
        } else {
            uint64_t root_temp[nQ];
            uint64_t level_size = 0;
            for (i = 0; i < nQ; ++i)
                level_size = std::max(level_size, Q[i]->Q->bv->size());
            uint64_t rank_vector[nQ][level_size];
            for (i = 0; i < nQ; ++i) {
                k_d[i] = Q[i]->getKD();
                Q[i]->get_children(cur_level, roots[i], C, nQ, children_to_recurse, children_to_recurse_size, rank_vector[i], k_d[i], i);
            }

            std::sort(children_to_recurse, children_to_recurse + children_to_recurse_size);

            int64_t last_child = -1;
            uint16_t child;

            for (i = 0; i < children_to_recurse_size; ++i) {
                child = children_to_recurse[i];

                for (uint64_t j = 0; j < nQ; j++)
                    root_temp[j] = k_d[j] * (rank_vector[j][Q[j]->getM(child)] - 1);

                if (child - last_child > 1)
                    last_pos[cur_level] += (child - last_child - 1);

                last_child = child;

                if (AND(Q, root_temp, nQ, cur_level + 1, max_level, bv, last_pos, nAtt)) {
                    bv[cur_level].push_back(last_pos[cur_level]++);
                    just_zeroes = false;
                } else {
                    if (cur_level < max_level)
                        last_pos[cur_level + 1] -= p;
                    last_pos[cur_level]++;
                }
            }

            if (p - last_child > 1)
                last_pos[cur_level] += (p - last_child - 1);
        }
        return !just_zeroes;
    }

    // Join de qdags ya extendidos (qdag_t extiende en su constructor); resultado como arbol.
    template<class QdagT, class SeQuadT>
    SeQuadT *multiJoinAuto(std::vector<qdag_t *> &Q, bool bounded_result, uint64_t upper_bound, uint8_t cut_off_level,
                           uint64_t &n_results) {
        static_assert(std::is_same<QdagT, qdag_t>::value && std::is_same<SeQuadT, se_quadtree_t>::value,
                      "ccq::multiJoinAuto only works with ccq::qdag_t / ccq::se_quadtree_t");
        if (bounded_result || upper_bound != 0) // (U)
            throw std::runtime_error("ccq::multiJoinAuto: upper_bound is not supported; use UPPER_BOUND=0");
        const uint64_t nQ = Q.size();
        std::map<uint64_t, uint8_t> attr_map;
        for (uint64_t i = 0; i < nQ; i++)
            for (uint64_t j = 0, na = Q[i]->nAttr(); j < na; j++)
                attr_map[Q[i]->getAttr(j)] = 1;
        const uint64_t n_att = attr_map.size();
        if (n_att > 11) // (L)
            throw std::runtime_error("ccq::multiJoinAuto: more than 11 attributes (" + std::to_string(n_att) + ")");

        const auto height = Q[0]->getHeight();
        const uint8_t k = Q[0]->getK();
        std::vector<uint64_t> bv[height];

        bool any_empty = false; // (E)
        for (uint64_t i = 0; i < nQ; i++)
            if (Q[i]->getQ()->get_bv()[0].n_ones() == 0)
                any_empty = true;

        if (!any_empty) {
            qdag *Q_star[nQ];
            uint64_t Q_roots[nQ];
            for (uint64_t i = 0; i < nQ; i++) {
                Q_star[i] = Q[i]->ccq();
                Q_roots[i] = 0;
            }
            uint64_t last_pos[height];
            for (uint64_t i = 0; i < height; i++)
                last_pos[i] = 0;
            (void)cut_off_level;
            AND(Q_star, Q_roots, (uint16_t)nQ, 0, (uint16_t)(height - 1), bv, last_pos, n_att);
        }

        SeQuadT *res = new SeQuadT(bv, k, (uint8_t)n_att, (uint16_t)height);
        n_results = bv[height - 1].size();
        return res;
    }

} // namespace ccq

#endif // QDAGS_CCQ_JOINS_HPP
