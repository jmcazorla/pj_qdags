#ifndef QDAGS_JOINS_DELTA_HPP
#define QDAGS_JOINS_DELTA_HPP

#include <cstdint>
#include <map>
#include <vector>

#include "joins.hpp"

// Copia instrumentada de AND/multiJoinNew que cuenta en deltaH[nivel] los nodos visitados por nivel. Solo
// secuencial y camino denso (<=6 atributos); con cut_off_level>0 delega en multiJoinNew y deltaH queda sin poblar.

namespace gdj {

    // AND que ademas hace deltaH[cur_level] += 1 por llamada. deltaH viene dimensionado a `height` y en cero.
    template<class QdagT>
    bool AND_delta(std::vector<QdagT *> &Q, uint64_t *roots, uint16_t nQ,
                    uint16_t cur_level, uint16_t max_level,
                    std::vector<uint64_t> bv[], uint64_t last_pos[], uint64_t nAtt,
                    bool bounded_result, uint64_t upper_bound, std::vector<uint64_t> &deltaH) {
        if (timeout_triggered.load(std::memory_order_relaxed))
            return false;

        uint64_t p = Q[0]->nChildren();
        bool just_zeroes = true;
        uint64_t k_d[16];

        uint16_t children_to_recurse[512];

        uint64_t i;
        uint64_t children_to_recurse_size = 0;

        uint64_t children = 0xFFFFFFFFFFFFFFFFULL;
        deltaH[cur_level] += 1;

        if (cur_level == max_level) {
            for (i = 0; i < nQ && children; ++i)
                children &= Q[i]->materialize_node_64_last(cur_level, roots[i]);

            children_to_recurse_size = sdsl::bits::cnt((uint64_t)children);

            i = 0;
            uint64_t msb;

            while (i < children_to_recurse_size) {
                msb = __builtin_clzll(children);
                children_to_recurse[i] = msb;
                ++i;
                children &= (((uint64_t)0xFFFFFFFFFFFFFFFFULL) >> (msb + 1));
            }

            int64_t last_child = -1;
            uint16_t child;

            for (i = 0; i < children_to_recurse_size; ++i) {
                child = children_to_recurse[i];

                if (child - last_child > 1)
                    last_pos[cur_level] += (child - last_child - 1);

                last_child = child;
                if (bounded_result && bv[max_level].size() >= upper_bound)
                    return !just_zeroes;
                else {
                    bv[cur_level].push_back(last_pos[cur_level]++);
                    just_zeroes = false;
                }
            }

            if (p - last_child > 1)
                last_pos[cur_level] += (p - last_child - 1);
        }
        else {
            uint64_t root_temp[16];
            uint64_t rank_vector[16][64];

            for (i = 0; i < nQ && children; ++i) {
                k_d[i] = Q[i]->getKD();
                children &= Q[i]->materialize_node_64(cur_level, roots[i], rank_vector[i]);
            }

            children_to_recurse_size = sdsl::bits::cnt((uint64_t)children);

            i = 0;
            uint64_t msb;
            while (i < children_to_recurse_size) {
                msb = __builtin_clzll(children);
                children_to_recurse[i] = msb;
                ++i;
                children &= (((uint64_t)0xFFFFFFFFFFFFFFFFULL) >> (msb + 1));
            }

            int64_t last_child = -1;
            uint16_t child;

            for (i = 0; i < children_to_recurse_size; ++i) {
                child = children_to_recurse[i];

                for (uint64_t j = 0; j < nQ; j++)
                    root_temp[j] = k_d[j] * (rank_vector[j][Q[j]->getM(child)] - 1);

                if (child - last_child > 1)
                    last_pos[cur_level] += (child - last_child - 1);

                last_child = child;

                if (!(bounded_result && bv[max_level].size() >= upper_bound) &&
                    AND_delta<QdagT>(Q, root_temp, nQ, cur_level + 1, max_level, bv, last_pos, nAtt, bounded_result, upper_bound, deltaH)) {
                    bv[cur_level].push_back(last_pos[cur_level]++);
                    just_zeroes = false;
                }
                else {
                    last_pos[cur_level + 1] -= p;
                    last_pos[cur_level]++;
                }
            }

            if (p - last_child > 1)
                last_pos[cur_level] += (p - last_child - 1);
        }

        return !just_zeroes;
    }

    // multiJoinNew con AND_delta; deltaH solo se puebla si cut_off_level==0 (ver cabecera).
    template<class QdagT, class SeQuadT>
    SeQuadT *multiJoin_delta(std::vector<QdagT *> &Q, bool bounded_result, uint64_t upper_bound,
                              uint8_t cut_off_level, uint64_t &n_results, std::vector<uint64_t> &deltaH) {
        if (cut_off_level > 0)
            return multiJoinNew<QdagT, SeQuadT>(Q, bounded_result, upper_bound, cut_off_level, n_results);

        att_set A;
        std::map<uint64_t, uint8_t> attr_map;

        uint64_t Q_roots[Q.size()];
        auto height = Q[0]->getHeight();
        std::vector<uint64_t> bv[height];

        for (uint64_t i = 0; i < Q.size(); i++) {
            uint64_t nAttr = Q[i]->nAttr();
            for (uint64_t j = 0; j < nAttr; j++)
                attr_map[Q[i]->getAttr(j)] = 1;
            Q_roots[i] = 0;
        }

        for (auto it = attr_map.begin(); it != attr_map.end(); it++)
            A.push_back(it->first);

        uint64_t last_pos[height];
        for (uint64_t i = 0; i < height; i++)
            last_pos[i] = 0;

        AND_delta<QdagT>(Q, Q_roots, Q.size(), 0, height - 1, bv, last_pos, A.size(), bounded_result, upper_bound, deltaH);

        SeQuadT *qResult = new SeQuadT(bv, Q[0]->getK(), (uint8_t)A.size(), Q[0]->getHeight());
        n_results = bv[height - 1].size();
        return qResult;
    }

} // namespace gdj

#endif // QDAGS_JOINS_DELTA_HPP
