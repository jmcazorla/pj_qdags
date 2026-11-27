#ifndef QDAGS_JOINS_MIXED_HPP
#define QDAGS_JOINS_MIXED_HPP

#include <cassert>
#include <cstdint>
#include <map>
#include <vector>

#include "joins.hpp"

// Join denso (<=6 atributos) con operandos de 2 backends distintos (QdagT1/QdagT2) en la misma llamada, que
// joins.hpp no permite. Es copia de AND/partialAND/multiJoinNew con un cambio: cada loop sobre Q se parte en uno
// sobre Q1 (posiciones [0,nQ1) de los buffers) y otro sobre Q2 ([nQ1,nQ1+nQ2)). Q1 y Q2 no pueden estar vacios.

namespace gdj {

// Gemelo de AND para 2 tipos de operando; `roots` es un solo array: [0,nQ1) las raices de Q1 y el resto las de Q2.
template<class QdagT1, class QdagT2>
bool ANDMixed(std::vector<QdagT1 *> &Q1, std::vector<QdagT2 *> &Q2,
              uint64_t *roots, uint16_t nQ1, uint16_t nQ2,
              uint16_t cur_level, uint16_t max_level,
              std::vector<uint64_t> bv[], uint64_t last_pos[], uint64_t nAtt,
              bool bounded_result, uint64_t upper_bound,
              striped_counter *counter_group = nullptr, uint16_t counter_slot = 0) {
    if (timeout_triggered.load(std::memory_order_relaxed))
        return false;

    uint64_t p = Q1[0]->nChildren();
    bool just_zeroes = true;
    uint64_t k_d[16];

    uint16_t children_to_recurse[512];

    uint64_t i;
    uint64_t children_to_recurse_size = 0;

    uint64_t children = 0xFFFFFFFFFFFFFFFFULL;

    if (cur_level == max_level) {
        for (i = 0; i < nQ1 && children; ++i)
            children &= Q1[i]->materialize_node_64_last(cur_level, roots[i]);
        for (i = 0; i < nQ2 && children; ++i)
            children &= Q2[i]->materialize_node_64_last(cur_level, roots[nQ1 + i]);

        children_to_recurse_size = sdsl::bits::cnt((uint64_t)children);

        if (counter_group && bounded_result && counter_group->sum() >= upper_bound)
            children_to_recurse_size = 0;

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
            if (!counter_group && bounded_result && bv[max_level].size() >= upper_bound)
                return !just_zeroes;
            else {
                bv[cur_level].push_back(last_pos[cur_level]++);
                just_zeroes = false;
            }
        }

        if (counter_group && children_to_recurse_size > 0)
            counter_group->add(counter_slot, children_to_recurse_size);

        if (p - last_child > 1)
            last_pos[cur_level] += (p - last_child - 1);
    }
    else {
        uint64_t root_temp[16];
        uint64_t rank_vector[16][64];

        for (i = 0; i < nQ1 && children; ++i) {
            k_d[i] = Q1[i]->getKD();
            children &= Q1[i]->materialize_node_64(cur_level, roots[i], rank_vector[i]);
        }
        for (i = 0; i < nQ2 && children; ++i) {
            k_d[nQ1 + i] = Q2[i]->getKD();
            children &= Q2[i]->materialize_node_64(cur_level, roots[nQ1 + i], rank_vector[nQ1 + i]);
        }

        children_to_recurse_size = sdsl::bits::cnt((uint64_t)children);

        if (counter_group && bounded_result && counter_group->sum() >= upper_bound)
            children_to_recurse_size = 0;

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

            for (uint64_t j = 0; j < nQ1; j++)
                root_temp[j] = k_d[j] * (rank_vector[j][Q1[j]->getM(child)] - 1);
            for (uint64_t j = 0; j < nQ2; j++)
                root_temp[nQ1 + j] = k_d[nQ1 + j] * (rank_vector[nQ1 + j][Q2[j]->getM(child)] - 1);

            if (child - last_child > 1)
                last_pos[cur_level] += (child - last_child - 1);

            last_child = child;

            if (!(!counter_group && bounded_result && bv[max_level].size() >= upper_bound) &&
                ANDMixed<QdagT1, QdagT2>(Q1, Q2, root_temp, nQ1, nQ2, cur_level + 1, max_level, bv, last_pos, nAtt, bounded_result, upper_bound, counter_group, counter_slot)) {
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

// Gemelo de partialAND para 2 tipos de operando (fase 1 de multiJoinMixed).
template<class QdagT1, class QdagT2>
bool partialANDMixed(uint16_t cut_level, std::vector<QdagT1 *> &Q1, std::vector<QdagT2 *> &Q2,
                      uint64_t *roots, uint16_t nQ1, uint16_t nQ2,
                      uint16_t cur_level, uint16_t max_level, std::vector<uint64_t> bv[], uint64_t last_pos[],
                      uint64_t nAtt, std::vector<uint64_t *> &thread_roots,
                      uint64_t max_roots = std::numeric_limits<uint64_t>::max(), bool *aborted = nullptr) {
    if (timeout_triggered.load(std::memory_order_relaxed)) {
        if (aborted)
            *aborted = true;
        return false;
    }

    uint64_t p = Q1[0]->nChildren();
    bool just_zeroes = true;
    uint64_t k_d[16];

    uint16_t children_to_recurse[512];

    uint64_t i;
    uint64_t children_to_recurse_size = 0;

    uint64_t children = 0xFFFFFFFFFFFFFFFFULL;

    if (cur_level == max_level) {
        if (aborted && thread_roots.size() >= max_roots) {
            *aborted = true;
            return false;
        }
        uint64_t *rts = new uint64_t[16];
        for (uint64_t i2 = 0; i2 < (uint64_t)nQ1 + nQ2; ++i2)
            rts[i2] = roots[i2];
        thread_roots.push_back(rts);
        return true;
    }
    else {
        uint64_t root_temp[16];
        uint64_t rank_vector[16][64];

        for (i = 0; i < nQ1 && children; ++i) {
            k_d[i] = Q1[i]->getKD();
            children &= Q1[i]->materialize_node_64(cur_level, roots[i], rank_vector[i]);
        }
        for (i = 0; i < nQ2 && children; ++i) {
            k_d[nQ1 + i] = Q2[i]->getKD();
            children &= Q2[i]->materialize_node_64(cur_level, roots[nQ1 + i], rank_vector[nQ1 + i]);
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

            for (uint64_t j = 0; j < nQ1; j++)
                root_temp[j] = k_d[j] * (rank_vector[j][Q1[j]->getM(child)] - 1);
            for (uint64_t j = 0; j < nQ2; j++)
                root_temp[nQ1 + j] = k_d[nQ1 + j] * (rank_vector[nQ1 + j][Q2[j]->getM(child)] - 1);

            if (child - last_child > 1)
                last_pos[cur_level] += (child - last_child - 1);

            last_child = child;

            bool child_ok_ = partialANDMixed<QdagT1, QdagT2>(cut_level, Q1, Q2, root_temp, nQ1, nQ2, cur_level + 1, max_level, bv, last_pos, nAtt, thread_roots, max_roots, aborted);
            if (aborted && *aborted)
                return false;
            if (child_ok_) {
                bv[cur_level].push_back(last_pos[cur_level]++);
                just_zeroes = false;
            }
            else {
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

// Gemelo de multiJoinNew para 2 tipos de operando; el tipo de salida (SeQuadT) es independiente de ambos.
template<class QdagT1, class QdagT2, class SeQuadT>
SeQuadT *multiJoinMixed(std::vector<QdagT1 *> &Q1, std::vector<QdagT2 *> &Q2,
                         bool bounded_result, uint64_t upper_bound, uint8_t cut_off_level, uint64_t &n_results) {
    assert(!Q1.empty() && !Q2.empty());

    gdj::att_set A;
    std::map<uint64_t, uint8_t> attr_map;

    uint16_t nQ1 = (uint16_t)Q1.size();
    uint16_t nQ2 = (uint16_t)Q2.size();
    uint64_t Q_roots[(uint64_t)nQ1 + nQ2];
    std::vector<uint64_t *> thread_roots;

    auto height = Q1[0]->getHeight();
    std::vector<uint64_t> bv[height];
    uint64_t max_nAttr = 0;
    for (uint64_t i = 0; i < nQ1; i++) {
        uint64_t nAttr = Q1[i]->nAttr();
        for (uint64_t j = 0; j < nAttr; j++)
            attr_map[Q1[i]->getAttr(j)] = 1;
        max_nAttr = std::max(max_nAttr, nAttr);
        Q_roots[i] = 0;
    }
    for (uint64_t i = 0; i < nQ2; i++) {
        uint64_t nAttr = Q2[i]->nAttr();
        for (uint64_t j = 0; j < nAttr; j++)
            attr_map[Q2[i]->getAttr(j)] = 1;
        max_nAttr = std::max(max_nAttr, nAttr);
        Q_roots[nQ1 + i] = 0;
    }

    for (auto it = attr_map.begin(); it != attr_map.end(); it++)
        A.push_back(it->first);

    if (cut_off_level == 0) {
        uint64_t last_pos[height];
        for (uint64_t i = 0; i < height; i++)
            last_pos[i] = 0;

        ANDMixed<QdagT1, QdagT2>(Q1, Q2, Q_roots, nQ1, nQ2, 0, height - 1, bv, last_pos, A.size(), bounded_result, upper_bound);
    }
    else {
        unsigned nb_threads_hint = THREADS_BY_CORE * std::thread::hardware_concurrency();
        unsigned nb_threads = nb_threads_hint == 0 ? 8 : nb_threads_hint;

        const int cut = enumerate_roots_capped((int)cut_off_level, height, bv, thread_roots,
            [&](int c, std::vector<uint64_t *> &roots, uint64_t max_roots, bool *aborted) {
                uint64_t last_posH[c];
                for (int i = 0; i < c; i++)
                    last_posH[i] = 0;
                partialANDMixed<QdagT1, QdagT2>(c, Q1, Q2, Q_roots, nQ1, nQ2, 0, c, bv, last_posH, A.size(), roots, max_roots, aborted);
            });

        std::vector<uint64_t>** array_bv = new std::vector<uint64_t>*[thread_roots.size()]();

        striped_counter counter_group_storage(nb_threads);
        striped_counter *counter_group = bounded_result ? &counter_group_storage : nullptr;

        parallel_roots_static(nb_threads, thread_roots.size(), height, array_bv,
            [&](size_t i, unsigned thread_id, std::vector<uint64_t> *scratch) {
                uint64_t last_pos[height];
                for (uint64_t j = 0; j < height; j++)
                    last_pos[j] = 0;
                ANDMixed<QdagT1, QdagT2>(Q1, Q2, thread_roots[i], nQ1, nQ2, cut, height - 1, scratch, last_pos, A.size(), bounded_result, upper_bound, counter_group, (uint16_t)thread_id);
            });

        uint64_t node_size = 1 << max_nAttr;
        merge_root_buffers(bv, array_bv, thread_roots, cut, height, node_size);
    }

    SeQuadT *qResult = new SeQuadT(bv, Q1[0]->getK(), (uint8_t)A.size(), Q1[0]->getHeight());
    n_results = bv[height - 1].size();
    return qResult;
}

} // namespace gdj

#endif // QDAGS_JOINS_MIXED_HPP
