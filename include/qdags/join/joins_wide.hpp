#ifndef QDAGS_JOINS_WIDE_HPP
#define QDAGS_JOINS_WIDE_HPP

#include <cstdint>
#include <map>
#include <vector>
#include <string>
#include <stdexcept>

#include "joins.hpp"

// Join "ancho" (7-10 atributos: children no cabe en uint64_t). Tres escalones iguales entre si y a joins.hpp;
// solo cambian el ancho de `children` y el materialize_node_* que se llama:
//   AND_wide/multiJoinWide         -- 7-8 atributos (UInt256)
//   AND_wide512/multiJoinWide512   -- 9 atributos    (UInt512)
//   AND_wide1024/multiJoinWide1024 -- 10 atributos   (UInt1024)
// multiJoinAuto() elige el escalon segun los atributos del join.

namespace gdj {

    inline uint16_t popcount_256(const gdj::UInt256& v) {
        return (uint16_t)(__builtin_popcountll(v.v3) + __builtin_popcountll(v.v2)
                         + __builtin_popcountll(v.v1) + __builtin_popcountll(v.v0));
    }

    inline bool any_bit_256(const gdj::UInt256& v) {
        return (v.v0 | v.v1 | v.v2 | v.v3) != 0;
    }

    // Posicion (0 = bit mas significativo de v3 .. 255 = menos significativo de v0) del bit prendido mas
    // significativo, que se apaga en `v`.
    inline uint16_t extract_top_bit_256(gdj::UInt256& v) {
        uint64_t msb;
        if (v.v3) {
            msb = __builtin_clzll(v.v3);
            v.v3 = (msb == 63) ? 0ULL : (v.v3 & (0xFFFFFFFFFFFFFFFFULL >> (msb + 1)));
            return (uint16_t)msb;
        }
        if (v.v2) {
            msb = __builtin_clzll(v.v2);
            v.v2 = (msb == 63) ? 0ULL : (v.v2 & (0xFFFFFFFFFFFFFFFFULL >> (msb + 1)));
            return (uint16_t)(64 + msb);
        }
        if (v.v1) {
            msb = __builtin_clzll(v.v1);
            v.v1 = (msb == 63) ? 0ULL : (v.v1 & (0xFFFFFFFFFFFFFFFFULL >> (msb + 1)));
            return (uint16_t)(128 + msb);
        }
        msb = __builtin_clzll(v.v0);
        v.v0 = (msb == 63) ? 0ULL : (v.v0 & (0xFFFFFFFFFFFFFFFFULL >> (msb + 1)));
        return (uint16_t)(192 + msb);
    }

    // Lo mismo para UIntWideN<N> (escalones 512/1024), w[N-1] primero.
    template<int N>
    inline uint16_t popcount_wideN(const gdj::UIntWideN<N>& v) {
        uint16_t total = 0;
        for (int i = N - 1; i >= 0; --i)
            total += (uint16_t)__builtin_popcountll(v.w[i]);
        return total;
    }

    template<int N>
    inline bool any_bit_wideN(const gdj::UIntWideN<N>& v) {
        for (int i = 0; i < N; ++i)
            if (v.w[i])
                return true;
        return false;
    }

    template<int N>
    inline uint16_t extract_top_bit_wideN(gdj::UIntWideN<N>& v) {
        for (int i = N - 1; i >= 0; --i) {
            if (v.w[i]) {
                uint64_t msb = __builtin_clzll(v.w[i]);
                v.w[i] = (msb == 63) ? 0ULL : (v.w[i] & (0xFFFFFFFFFFFFFFFFULL >> (msb + 1)));
                return (uint16_t)((N - 1 - i) * 64 + msb);
            }
        }
        return 0;
    }

    inline uint16_t popcount_512(const gdj::UInt512& v) { return popcount_wideN<8>(v); }
    inline bool any_bit_512(const gdj::UInt512& v) { return any_bit_wideN<8>(v); }
    inline uint16_t extract_top_bit_512(gdj::UInt512& v) { return extract_top_bit_wideN<8>(v); }

    inline uint16_t popcount_1024(const gdj::UInt1024& v) { return popcount_wideN<16>(v); }
    inline bool any_bit_1024(const gdj::UInt1024& v) { return any_bit_wideN<16>(v); }
    inline uint16_t extract_top_bit_1024(gdj::UInt1024& v) { return extract_top_bit_wideN<16>(v); }

    // Camino perezoso: con `Lazy = true` los qdag que extienden por dispersion no llenan rank_vector ni usan
    // tablas de 16 bits; guardan el nodo base y calculan el rango solo de los hijos que sobreviven al AND
    // (qdag::lazy_child_rank).
    template<class QdagT>
    inline bool any_scatter(const std::vector<QdagT *> &Q) {
        for (auto *q : Q)
            if (q->usesScatter())
                return true;
        return false;
    }

    // =========================== Escalon 256 (7-8 atributos) ===========================

    template<class QdagT, bool Lazy = false>
    bool AND_wide(std::vector<QdagT *> &Q, uint64_t *roots, uint16_t nQ,
                  uint16_t cur_level, uint16_t max_level,
                  std::vector<uint64_t> bv[], uint64_t last_pos[],
                  bool bounded_result, uint64_t upper_bound,
                  striped_counter *counter_group = nullptr, uint16_t counter_slot = 0) {
        if (timeout_triggered.load(std::memory_order_relaxed))
            return false;

        uint64_t p = Q[0]->nChildren();
        bool just_zeroes = true;

        uint16_t children_to_recurse[512];
        uint64_t i;
        uint64_t children_to_recurse_size;

        if (cur_level == max_level) {
            gdj::UInt256 children = ~gdj::UInt256();
            for (i = 0; i < nQ && any_bit_256(children); ++i) {
                if constexpr (Lazy) {
                    if (Q[i]->usesScatter()) {
                        children &= Q[i]->template materialize_scatter_last<gdj::UInt256>(cur_level, roots[i]);
                        continue;
                    }
                }
                children &= Q[i]->materialize_node_256_last(cur_level, roots[i]);
            }

            children_to_recurse_size = popcount_256(children);

            if (counter_group && bounded_result && counter_group->sum() >= upper_bound)
                children_to_recurse_size = 0;

            i = 0;
            while (i < children_to_recurse_size) {
                children_to_recurse[i] = extract_top_bit_256(children);
                ++i;
            }

            int64_t last_child = -1;
            uint16_t child;

            for (i = 0; i < children_to_recurse_size; ++i) {
                child = children_to_recurse[i];

                if (child - last_child > 1)
                    last_pos[cur_level] += (child - last_child - 1);

                last_child = child;

                if (!counter_group && bounded_result && bv[max_level].size() >= upper_bound)
                    return !just_zeroes;  // no `false`, igual que en joins.hpp

                bv[cur_level].push_back(last_pos[cur_level]++);
                just_zeroes = false;
            }

            if (counter_group && children_to_recurse_size > 0)
                counter_group->add(counter_slot, children_to_recurse_size);

            if (p - last_child > 1)
                last_pos[cur_level] += (p - last_child - 1);
        } else {
            uint64_t root_temp[16];
            uint64_t k_d[16];
            uint64_t rank_vector[16][256];
            [[maybe_unused]] BaseNode512 base_node[Lazy ? 16 : 1];

            gdj::UInt256 children = ~gdj::UInt256();
            for (i = 0; i < nQ && any_bit_256(children); ++i) {
                k_d[i] = Q[i]->getKD();
                if constexpr (Lazy) {
                    if (Q[i]->usesScatter()) {
                        children &= Q[i]->template materialize_scatter_lazy<gdj::UInt256>(cur_level, roots[i], base_node[i]);
                        continue;
                    }
                }
                children &= Q[i]->materialize_node_256(cur_level, roots[i], rank_vector[i]);
            }

            children_to_recurse_size = popcount_256(children);

            if (counter_group && bounded_result && counter_group->sum() >= upper_bound)
                children_to_recurse_size = 0;

            i = 0;
            while (i < children_to_recurse_size) {
                children_to_recurse[i] = extract_top_bit_256(children);
                ++i;
            }

            int64_t last_child = -1;
            uint16_t child;

            for (i = 0; i < children_to_recurse_size; ++i) {
                child = children_to_recurse[i];

                for (uint64_t j = 0; j < nQ; j++) {
                    if constexpr (Lazy) {
                        if (Q[j]->usesScatter()) {
                            root_temp[j] = k_d[j] * (QdagT::lazy_child_rank(base_node[j], Q[j]->getM(child)) - 1);
                            continue;
                        }
                    }
                    root_temp[j] = k_d[j] * (rank_vector[j][Q[j]->getM(child)] - 1);
                }

                if (child - last_child > 1)
                    last_pos[cur_level] += (child - last_child - 1);

                last_child = child;

                bool cutoff_hit = !counter_group && bounded_result && bv[max_level].size() >= upper_bound;
                if (!cutoff_hit &&
                    AND_wide<QdagT, Lazy>(Q, root_temp, nQ, cur_level + 1, max_level, bv, last_pos, bounded_result, upper_bound, counter_group, counter_slot)) {
                    bv[cur_level].push_back(last_pos[cur_level]++);
                    just_zeroes = false;
                } else {
                    if (!cutoff_hit)
                        last_pos[cur_level + 1] -= p;
                    last_pos[cur_level]++;
                }
            }

            if (p - last_child > 1)
                last_pos[cur_level] += (p - last_child - 1);
        }

        return !just_zeroes;
    }

    template<class QdagT, bool Lazy = false>
    bool partialAND_wide(uint16_t cut_level, std::vector<QdagT *> &Q, uint64_t *roots, uint16_t nQ,
                          uint16_t cur_level, uint16_t max_level,
                          std::vector<uint64_t> bv[], uint64_t last_pos[],
                          std::vector<uint64_t*> &thread_roots,
                         uint64_t max_roots = std::numeric_limits<uint64_t>::max(), bool *aborted = nullptr) {
        if (timeout_triggered.load(std::memory_order_relaxed)) {
            if (aborted)
                *aborted = true;
            return false;
        }

        uint64_t p = Q[0]->nChildren();
        bool just_zeroes = true;

        uint16_t children_to_recurse[512];
        uint64_t i;
        uint64_t children_to_recurse_size;

        if (cur_level == max_level) {
            if (aborted && thread_roots.size() >= max_roots) {
                *aborted = true;
                return false;
            }
            uint64_t* rts = new uint64_t[16];
            for (uint64_t j = 0; j < nQ; ++j)
                rts[j] = roots[j];
            thread_roots.push_back(rts);
            return true;
        } else {
            uint64_t root_temp[16];
            uint64_t k_d[16];
            uint64_t rank_vector[16][256];
            [[maybe_unused]] BaseNode512 base_node[Lazy ? 16 : 1];

            gdj::UInt256 children = ~gdj::UInt256();
            for (i = 0; i < nQ && any_bit_256(children); ++i) {
                k_d[i] = Q[i]->getKD();
                if constexpr (Lazy) {
                    if (Q[i]->usesScatter()) {
                        children &= Q[i]->template materialize_scatter_lazy<gdj::UInt256>(cur_level, roots[i], base_node[i]);
                        continue;
                    }
                }
                children &= Q[i]->materialize_node_256(cur_level, roots[i], rank_vector[i]);
            }

            children_to_recurse_size = popcount_256(children);
            i = 0;
            while (i < children_to_recurse_size) {
                children_to_recurse[i] = extract_top_bit_256(children);
                ++i;
            }

            int64_t last_child = -1;
            uint16_t child;

            for (i = 0; i < children_to_recurse_size; ++i) {
                child = children_to_recurse[i];

                for (uint64_t j = 0; j < nQ; j++) {
                    if constexpr (Lazy) {
                        if (Q[j]->usesScatter()) {
                            root_temp[j] = k_d[j] * (QdagT::lazy_child_rank(base_node[j], Q[j]->getM(child)) - 1);
                            continue;
                        }
                    }
                    root_temp[j] = k_d[j] * (rank_vector[j][Q[j]->getM(child)] - 1);
                }

                if (child - last_child > 1)
                    last_pos[cur_level] += (child - last_child - 1);

                last_child = child;

                bool child_ok_ = partialAND_wide<QdagT, Lazy>(cut_level, Q, root_temp, nQ, cur_level + 1, max_level, bv, last_pos, thread_roots, max_roots, aborted);
                if (aborted && *aborted)
                    return false;
                if (child_ok_) {
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

    template<class QdagT, class SeQuadT, bool Lazy>
    SeQuadT *multiJoinWide_impl(std::vector<QdagT *> &Q, bool bounded_result,
                         uint64_t upper_bound, uint64_t &n_results,
                         uint8_t cut_off_level) {
        gdj::att_set A;
        std::map<uint64_t, uint8_t> attr_map;

        uint64_t Q_roots[Q.size()];
        uint64_t max_nAttr = 0;
        for (uint64_t i = 0; i < Q.size(); i++) {
            uint64_t nAttr = Q[i]->nAttr();
            for (uint64_t j = 0; j < nAttr; j++)
                attr_map[Q[i]->getAttr(j)] = 1;
            max_nAttr = std::max(max_nAttr, nAttr);
            Q_roots[i] = 0;
        }
        for (auto it = attr_map.begin(); it != attr_map.end(); it++)
            A.push_back(it->first);

        std::vector<uint64_t*> thread_roots;

        auto height = Q[0]->getHeight();
        std::vector<uint64_t> bv[height];

        if (cut_off_level == 0) {
            uint64_t last_pos[height];
            for (uint64_t i = 0; i < height; i++)
                last_pos[i] = 0;

            AND_wide<QdagT, Lazy>(Q, Q_roots, (uint16_t)Q.size(), 0, height - 1, bv, last_pos, bounded_result, upper_bound);
        } else {
            unsigned nb_threads_hint = THREADS_BY_CORE * std::thread::hardware_concurrency();
            unsigned nb_threads = nb_threads_hint == 0 ? 8 : nb_threads_hint;

            const int cut = enumerate_roots_capped((int)cut_off_level, height, bv, thread_roots,
                [&](int c, std::vector<uint64_t*> &roots, uint64_t max_roots, bool *aborted) {
                    uint64_t last_posH[c];
                    for (int i = 0; i < c; i++)
                        last_posH[i] = 0;
                    partialAND_wide<QdagT, Lazy>(c, Q, Q_roots, (uint16_t)Q.size(), 0, c, bv, last_posH, roots, max_roots, aborted);
                });

            std::vector<uint64_t>** array_bv = new std::vector<uint64_t>*[thread_roots.size()]();

            striped_counter counter_group_storage(nb_threads);
            striped_counter *counter_group = bounded_result ? &counter_group_storage : nullptr;

            parallel_roots_static(nb_threads, thread_roots.size(), height, array_bv,
                [&](size_t i, unsigned thread_id, std::vector<uint64_t> *scratch) {
                    uint64_t last_pos[height];
                    for (uint64_t j = 0; j < height; j++)
                        last_pos[j] = 0;
                    AND_wide<QdagT, Lazy>(Q, thread_roots[i], (uint16_t)Q.size(), cut, height - 1, scratch, last_pos, bounded_result, upper_bound, counter_group, (uint16_t)thread_id);
                });

            uint64_t node_size = 1ULL << max_nAttr;
            merge_root_buffers(bv, array_bv, thread_roots, cut, height, node_size);
        }

        SeQuadT *qResult = new SeQuadT(bv, Q[0]->getK(), (uint8_t)A.size(), Q[0]->getHeight());
        n_results = bv[height - 1].size();

        return qResult;
    }

    // Camino perezoso (Lazy) si algun qdag extiende por dispersion; si no, tablas de 16 bits.
    template<class QdagT, class SeQuadT>
    SeQuadT *multiJoinWide(std::vector<QdagT *> &Q, bool bounded_result,
                    uint64_t upper_bound, uint64_t &n_results,
                    uint8_t cut_off_level = 0) {
        if (any_scatter(Q))
            return multiJoinWide_impl<QdagT, SeQuadT, true>(Q, bounded_result, upper_bound, n_results, cut_off_level);
        return multiJoinWide_impl<QdagT, SeQuadT, false>(Q, bounded_result, upper_bound, n_results, cut_off_level);
    }

    // =========================== Escalon 512 (9 atributos) ===========================
    // Igual al escalon 256, con UInt512 y materialize_node_512(_last).

    template<class QdagT, bool Lazy = false>
    bool AND_wide512(std::vector<QdagT *> &Q, uint64_t *roots, uint16_t nQ,
                      uint16_t cur_level, uint16_t max_level,
                      std::vector<uint64_t> bv[], uint64_t last_pos[],
                      bool bounded_result, uint64_t upper_bound,
                      striped_counter *counter_group = nullptr, uint16_t counter_slot = 0) {
        if (timeout_triggered.load(std::memory_order_relaxed))
            return false;

        uint64_t p = Q[0]->nChildren();
        bool just_zeroes = true;

        uint16_t children_to_recurse[1024];
        uint64_t i;
        uint64_t children_to_recurse_size;

        if (cur_level == max_level) {
            gdj::UInt512 children = ~gdj::UInt512();
            for (i = 0; i < nQ && any_bit_512(children); ++i) {
                if constexpr (Lazy) {
                    if (Q[i]->usesScatter()) {
                        children &= Q[i]->template materialize_scatter_last<gdj::UInt512>(cur_level, roots[i]);
                        continue;
                    }
                }
                children &= Q[i]->materialize_node_512_last(cur_level, roots[i]);
            }

            children_to_recurse_size = popcount_512(children);

            if (counter_group && bounded_result && counter_group->sum() >= upper_bound)
                children_to_recurse_size = 0;

            i = 0;
            while (i < children_to_recurse_size) {
                children_to_recurse[i] = extract_top_bit_512(children);
                ++i;
            }

            int64_t last_child = -1;
            uint16_t child;

            for (i = 0; i < children_to_recurse_size; ++i) {
                child = children_to_recurse[i];

                if (child - last_child > 1)
                    last_pos[cur_level] += (child - last_child - 1);

                last_child = child;

                if (!counter_group && bounded_result && bv[max_level].size() >= upper_bound)
                    return !just_zeroes;  // no `false`, igual que en joins.hpp

                bv[cur_level].push_back(last_pos[cur_level]++);
                just_zeroes = false;
            }

            if (counter_group && children_to_recurse_size > 0)
                counter_group->add(counter_slot, children_to_recurse_size);

            if (p - last_child > 1)
                last_pos[cur_level] += (p - last_child - 1);
        } else {
            uint64_t root_temp[16];
            uint64_t k_d[16];
            uint64_t rank_vector[16][256];
            [[maybe_unused]] BaseNode512 base_node[Lazy ? 16 : 1];

            gdj::UInt512 children = ~gdj::UInt512();
            for (i = 0; i < nQ && any_bit_512(children); ++i) {
                k_d[i] = Q[i]->getKD();
                if constexpr (Lazy) {
                    if (Q[i]->usesScatter()) {
                        children &= Q[i]->template materialize_scatter_lazy<gdj::UInt512>(cur_level, roots[i], base_node[i]);
                        continue;
                    }
                }
                children &= Q[i]->materialize_node_512(cur_level, roots[i], rank_vector[i]);
            }

            children_to_recurse_size = popcount_512(children);

            if (counter_group && bounded_result && counter_group->sum() >= upper_bound)
                children_to_recurse_size = 0;

            i = 0;
            while (i < children_to_recurse_size) {
                children_to_recurse[i] = extract_top_bit_512(children);
                ++i;
            }

            int64_t last_child = -1;
            uint16_t child;

            for (i = 0; i < children_to_recurse_size; ++i) {
                child = children_to_recurse[i];

                for (uint64_t j = 0; j < nQ; j++) {
                    if constexpr (Lazy) {
                        if (Q[j]->usesScatter()) {
                            root_temp[j] = k_d[j] * (QdagT::lazy_child_rank(base_node[j], Q[j]->getM(child)) - 1);
                            continue;
                        }
                    }
                    root_temp[j] = k_d[j] * (rank_vector[j][Q[j]->getM(child)] - 1);
                }

                if (child - last_child > 1)
                    last_pos[cur_level] += (child - last_child - 1);

                last_child = child;

                bool cutoff_hit = !counter_group && bounded_result && bv[max_level].size() >= upper_bound;
                if (!cutoff_hit &&
                    AND_wide512<QdagT, Lazy>(Q, root_temp, nQ, cur_level + 1, max_level, bv, last_pos, bounded_result, upper_bound, counter_group, counter_slot)) {
                    bv[cur_level].push_back(last_pos[cur_level]++);
                    just_zeroes = false;
                } else {
                    if (!cutoff_hit)
                        last_pos[cur_level + 1] -= p;
                    last_pos[cur_level]++;
                }
            }

            if (p - last_child > 1)
                last_pos[cur_level] += (p - last_child - 1);
        }

        return !just_zeroes;
    }

    template<class QdagT, bool Lazy = false>
    bool partialAND_wide512(uint16_t cut_level, std::vector<QdagT *> &Q, uint64_t *roots, uint16_t nQ,
                             uint16_t cur_level, uint16_t max_level,
                             std::vector<uint64_t> bv[], uint64_t last_pos[],
                             std::vector<uint64_t*> &thread_roots,
                            uint64_t max_roots = std::numeric_limits<uint64_t>::max(), bool *aborted = nullptr) {
        if (timeout_triggered.load(std::memory_order_relaxed)) {
            if (aborted)
                *aborted = true;
            return false;
        }

        uint64_t p = Q[0]->nChildren();
        bool just_zeroes = true;

        uint16_t children_to_recurse[1024];
        uint64_t i;
        uint64_t children_to_recurse_size;

        if (cur_level == max_level) {
            if (aborted && thread_roots.size() >= max_roots) {
                *aborted = true;
                return false;
            }
            uint64_t* rts = new uint64_t[16];
            for (uint64_t j = 0; j < nQ; ++j)
                rts[j] = roots[j];
            thread_roots.push_back(rts);
            return true;
        } else {
            uint64_t root_temp[16];
            uint64_t k_d[16];
            uint64_t rank_vector[16][256];
            [[maybe_unused]] BaseNode512 base_node[Lazy ? 16 : 1];

            gdj::UInt512 children = ~gdj::UInt512();
            for (i = 0; i < nQ && any_bit_512(children); ++i) {
                k_d[i] = Q[i]->getKD();
                if constexpr (Lazy) {
                    if (Q[i]->usesScatter()) {
                        children &= Q[i]->template materialize_scatter_lazy<gdj::UInt512>(cur_level, roots[i], base_node[i]);
                        continue;
                    }
                }
                children &= Q[i]->materialize_node_512(cur_level, roots[i], rank_vector[i]);
            }

            children_to_recurse_size = popcount_512(children);
            i = 0;
            while (i < children_to_recurse_size) {
                children_to_recurse[i] = extract_top_bit_512(children);
                ++i;
            }

            int64_t last_child = -1;
            uint16_t child;

            for (i = 0; i < children_to_recurse_size; ++i) {
                child = children_to_recurse[i];

                for (uint64_t j = 0; j < nQ; j++) {
                    if constexpr (Lazy) {
                        if (Q[j]->usesScatter()) {
                            root_temp[j] = k_d[j] * (QdagT::lazy_child_rank(base_node[j], Q[j]->getM(child)) - 1);
                            continue;
                        }
                    }
                    root_temp[j] = k_d[j] * (rank_vector[j][Q[j]->getM(child)] - 1);
                }

                if (child - last_child > 1)
                    last_pos[cur_level] += (child - last_child - 1);

                last_child = child;

                bool child_ok_ = partialAND_wide512<QdagT, Lazy>(cut_level, Q, root_temp, nQ, cur_level + 1, max_level, bv, last_pos, thread_roots, max_roots, aborted);
                if (aborted && *aborted)
                    return false;
                if (child_ok_) {
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

    template<class QdagT, class SeQuadT, bool Lazy>
    SeQuadT *multiJoinWide512_impl(std::vector<QdagT *> &Q, bool bounded_result,
                         uint64_t upper_bound, uint64_t &n_results,
                         uint8_t cut_off_level) {
        gdj::att_set A;
        std::map<uint64_t, uint8_t> attr_map;

        uint64_t Q_roots[Q.size()];
        uint64_t max_nAttr = 0;
        for (uint64_t i = 0; i < Q.size(); i++) {
            uint64_t nAttr = Q[i]->nAttr();
            for (uint64_t j = 0; j < nAttr; j++)
                attr_map[Q[i]->getAttr(j)] = 1;
            max_nAttr = std::max(max_nAttr, nAttr);
            Q_roots[i] = 0;
        }
        for (auto it = attr_map.begin(); it != attr_map.end(); it++)
            A.push_back(it->first);

        std::vector<uint64_t*> thread_roots;

        auto height = Q[0]->getHeight();
        std::vector<uint64_t> bv[height];

        if (cut_off_level == 0) {
            uint64_t last_pos[height];
            for (uint64_t i = 0; i < height; i++)
                last_pos[i] = 0;

            AND_wide512<QdagT, Lazy>(Q, Q_roots, (uint16_t)Q.size(), 0, height - 1, bv, last_pos, bounded_result, upper_bound);
        } else {
            unsigned nb_threads_hint = THREADS_BY_CORE * std::thread::hardware_concurrency();
            unsigned nb_threads = nb_threads_hint == 0 ? 8 : nb_threads_hint;

            const int cut = enumerate_roots_capped((int)cut_off_level, height, bv, thread_roots,
                [&](int c, std::vector<uint64_t*> &roots, uint64_t max_roots, bool *aborted) {
                    uint64_t last_posH[c];
                    for (int i = 0; i < c; i++)
                        last_posH[i] = 0;
                    partialAND_wide512<QdagT, Lazy>(c, Q, Q_roots, (uint16_t)Q.size(), 0, c, bv, last_posH, roots, max_roots, aborted);
                });

            std::vector<uint64_t>** array_bv = new std::vector<uint64_t>*[thread_roots.size()]();

            striped_counter counter_group_storage(nb_threads);
            striped_counter *counter_group = bounded_result ? &counter_group_storage : nullptr;

            parallel_roots_static(nb_threads, thread_roots.size(), height, array_bv,
                [&](size_t i, unsigned thread_id, std::vector<uint64_t> *scratch) {
                    uint64_t last_pos[height];
                    for (uint64_t j = 0; j < height; j++)
                        last_pos[j] = 0;
                    AND_wide512<QdagT, Lazy>(Q, thread_roots[i], (uint16_t)Q.size(), cut, height - 1, scratch, last_pos, bounded_result, upper_bound, counter_group, (uint16_t)thread_id);
                });

            uint64_t node_size = 1ULL << max_nAttr;
            merge_root_buffers(bv, array_bv, thread_roots, cut, height, node_size);
        }

        SeQuadT *qResult = new SeQuadT(bv, Q[0]->getK(), (uint8_t)A.size(), Q[0]->getHeight());
        n_results = bv[height - 1].size();

        return qResult;
    }

    // Camino perezoso (Lazy) si algun qdag extiende por dispersion; si no, tablas de 16 bits.
    template<class QdagT, class SeQuadT>
    SeQuadT *multiJoinWide512(std::vector<QdagT *> &Q, bool bounded_result,
                    uint64_t upper_bound, uint64_t &n_results,
                    uint8_t cut_off_level = 0) {
        if (any_scatter(Q))
            return multiJoinWide512_impl<QdagT, SeQuadT, true>(Q, bounded_result, upper_bound, n_results, cut_off_level);
        return multiJoinWide512_impl<QdagT, SeQuadT, false>(Q, bounded_result, upper_bound, n_results, cut_off_level);
    }

    // =========================== Escalon 1024 (10 atributos) ===========================
    // Igual al escalon 256, con UInt1024 y materialize_node_1024(_last).

    template<class QdagT, bool Lazy = false>
    bool AND_wide1024(std::vector<QdagT *> &Q, uint64_t *roots, uint16_t nQ,
                       uint16_t cur_level, uint16_t max_level,
                       std::vector<uint64_t> bv[], uint64_t last_pos[],
                       bool bounded_result, uint64_t upper_bound,
                       striped_counter *counter_group = nullptr, uint16_t counter_slot = 0) {
        if (timeout_triggered.load(std::memory_order_relaxed))
            return false;

        uint64_t p = Q[0]->nChildren();
        bool just_zeroes = true;

        uint16_t children_to_recurse[1024];
        uint64_t i;
        uint64_t children_to_recurse_size;

        if (cur_level == max_level) {
            gdj::UInt1024 children = ~gdj::UInt1024();
            for (i = 0; i < nQ && any_bit_1024(children); ++i) {
                if constexpr (Lazy) {
                    if (Q[i]->usesScatter()) {
                        children &= Q[i]->template materialize_scatter_last<gdj::UInt1024>(cur_level, roots[i]);
                        continue;
                    }
                }
                children &= Q[i]->materialize_node_1024_last(cur_level, roots[i]);
            }

            children_to_recurse_size = popcount_1024(children);

            if (counter_group && bounded_result && counter_group->sum() >= upper_bound)
                children_to_recurse_size = 0;

            i = 0;
            while (i < children_to_recurse_size) {
                children_to_recurse[i] = extract_top_bit_1024(children);
                ++i;
            }

            int64_t last_child = -1;
            uint16_t child;

            for (i = 0; i < children_to_recurse_size; ++i) {
                child = children_to_recurse[i];

                if (child - last_child > 1)
                    last_pos[cur_level] += (child - last_child - 1);

                last_child = child;

                if (!counter_group && bounded_result && bv[max_level].size() >= upper_bound)
                    return !just_zeroes;  // no `false`, igual que en joins.hpp

                bv[cur_level].push_back(last_pos[cur_level]++);
                just_zeroes = false;
            }

            if (counter_group && children_to_recurse_size > 0)
                counter_group->add(counter_slot, children_to_recurse_size);

            if (p - last_child > 1)
                last_pos[cur_level] += (p - last_child - 1);
        } else {
            uint64_t root_temp[16];
            uint64_t k_d[16];
            uint64_t rank_vector[16][256];
            [[maybe_unused]] BaseNode512 base_node[Lazy ? 16 : 1];

            gdj::UInt1024 children = ~gdj::UInt1024();
            for (i = 0; i < nQ && any_bit_1024(children); ++i) {
                k_d[i] = Q[i]->getKD();
                if constexpr (Lazy) {
                    if (Q[i]->usesScatter()) {
                        children &= Q[i]->template materialize_scatter_lazy<gdj::UInt1024>(cur_level, roots[i], base_node[i]);
                        continue;
                    }
                }
                children &= Q[i]->materialize_node_1024(cur_level, roots[i], rank_vector[i]);
            }

            children_to_recurse_size = popcount_1024(children);

            if (counter_group && bounded_result && counter_group->sum() >= upper_bound)
                children_to_recurse_size = 0;

            i = 0;
            while (i < children_to_recurse_size) {
                children_to_recurse[i] = extract_top_bit_1024(children);
                ++i;
            }

            int64_t last_child = -1;
            uint16_t child;

            for (i = 0; i < children_to_recurse_size; ++i) {
                child = children_to_recurse[i];

                for (uint64_t j = 0; j < nQ; j++) {
                    if constexpr (Lazy) {
                        if (Q[j]->usesScatter()) {
                            root_temp[j] = k_d[j] * (QdagT::lazy_child_rank(base_node[j], Q[j]->getM(child)) - 1);
                            continue;
                        }
                    }
                    root_temp[j] = k_d[j] * (rank_vector[j][Q[j]->getM(child)] - 1);
                }

                if (child - last_child > 1)
                    last_pos[cur_level] += (child - last_child - 1);

                last_child = child;

                bool cutoff_hit = !counter_group && bounded_result && bv[max_level].size() >= upper_bound;
                if (!cutoff_hit &&
                    AND_wide1024<QdagT, Lazy>(Q, root_temp, nQ, cur_level + 1, max_level, bv, last_pos, bounded_result, upper_bound, counter_group, counter_slot)) {
                    bv[cur_level].push_back(last_pos[cur_level]++);
                    just_zeroes = false;
                } else {
                    if (!cutoff_hit)
                        last_pos[cur_level + 1] -= p;
                    last_pos[cur_level]++;
                }
            }

            if (p - last_child > 1)
                last_pos[cur_level] += (p - last_child - 1);
        }

        return !just_zeroes;
    }

    template<class QdagT, bool Lazy = false>
    bool partialAND_wide1024(uint16_t cut_level, std::vector<QdagT *> &Q, uint64_t *roots, uint16_t nQ,
                              uint16_t cur_level, uint16_t max_level,
                              std::vector<uint64_t> bv[], uint64_t last_pos[],
                              std::vector<uint64_t*> &thread_roots,
                             uint64_t max_roots = std::numeric_limits<uint64_t>::max(), bool *aborted = nullptr) {
        if (timeout_triggered.load(std::memory_order_relaxed)) {
            if (aborted)
                *aborted = true;
            return false;
        }

        uint64_t p = Q[0]->nChildren();
        bool just_zeroes = true;

        uint16_t children_to_recurse[1024];
        uint64_t i;
        uint64_t children_to_recurse_size;

        if (cur_level == max_level) {
            if (aborted && thread_roots.size() >= max_roots) {
                *aborted = true;
                return false;
            }
            uint64_t* rts = new uint64_t[16];
            for (uint64_t j = 0; j < nQ; ++j)
                rts[j] = roots[j];
            thread_roots.push_back(rts);
            return true;
        } else {
            uint64_t root_temp[16];
            uint64_t k_d[16];
            uint64_t rank_vector[16][256];
            [[maybe_unused]] BaseNode512 base_node[Lazy ? 16 : 1];

            gdj::UInt1024 children = ~gdj::UInt1024();
            for (i = 0; i < nQ && any_bit_1024(children); ++i) {
                k_d[i] = Q[i]->getKD();
                if constexpr (Lazy) {
                    if (Q[i]->usesScatter()) {
                        children &= Q[i]->template materialize_scatter_lazy<gdj::UInt1024>(cur_level, roots[i], base_node[i]);
                        continue;
                    }
                }
                children &= Q[i]->materialize_node_1024(cur_level, roots[i], rank_vector[i]);
            }

            children_to_recurse_size = popcount_1024(children);
            i = 0;
            while (i < children_to_recurse_size) {
                children_to_recurse[i] = extract_top_bit_1024(children);
                ++i;
            }

            int64_t last_child = -1;
            uint16_t child;

            for (i = 0; i < children_to_recurse_size; ++i) {
                child = children_to_recurse[i];

                for (uint64_t j = 0; j < nQ; j++) {
                    if constexpr (Lazy) {
                        if (Q[j]->usesScatter()) {
                            root_temp[j] = k_d[j] * (QdagT::lazy_child_rank(base_node[j], Q[j]->getM(child)) - 1);
                            continue;
                        }
                    }
                    root_temp[j] = k_d[j] * (rank_vector[j][Q[j]->getM(child)] - 1);
                }

                if (child - last_child > 1)
                    last_pos[cur_level] += (child - last_child - 1);

                last_child = child;

                bool child_ok_ = partialAND_wide1024<QdagT, Lazy>(cut_level, Q, root_temp, nQ, cur_level + 1, max_level, bv, last_pos, thread_roots, max_roots, aborted);
                if (aborted && *aborted)
                    return false;
                if (child_ok_) {
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

    template<class QdagT, class SeQuadT, bool Lazy>
    SeQuadT *multiJoinWide1024_impl(std::vector<QdagT *> &Q, bool bounded_result,
                         uint64_t upper_bound, uint64_t &n_results,
                         uint8_t cut_off_level) {
        gdj::att_set A;
        std::map<uint64_t, uint8_t> attr_map;

        uint64_t Q_roots[Q.size()];
        uint64_t max_nAttr = 0;
        for (uint64_t i = 0; i < Q.size(); i++) {
            uint64_t nAttr = Q[i]->nAttr();
            for (uint64_t j = 0; j < nAttr; j++)
                attr_map[Q[i]->getAttr(j)] = 1;
            max_nAttr = std::max(max_nAttr, nAttr);
            Q_roots[i] = 0;
        }
        for (auto it = attr_map.begin(); it != attr_map.end(); it++)
            A.push_back(it->first);

        std::vector<uint64_t*> thread_roots;

        auto height = Q[0]->getHeight();
        std::vector<uint64_t> bv[height];

        if (cut_off_level == 0) {
            uint64_t last_pos[height];
            for (uint64_t i = 0; i < height; i++)
                last_pos[i] = 0;

            AND_wide1024<QdagT, Lazy>(Q, Q_roots, (uint16_t)Q.size(), 0, height - 1, bv, last_pos, bounded_result, upper_bound);
        } else {
            unsigned nb_threads_hint = THREADS_BY_CORE * std::thread::hardware_concurrency();
            unsigned nb_threads = nb_threads_hint == 0 ? 8 : nb_threads_hint;

            const int cut = enumerate_roots_capped((int)cut_off_level, height, bv, thread_roots,
                [&](int c, std::vector<uint64_t*> &roots, uint64_t max_roots, bool *aborted) {
                    uint64_t last_posH[c];
                    for (int i = 0; i < c; i++)
                        last_posH[i] = 0;
                    partialAND_wide1024<QdagT, Lazy>(c, Q, Q_roots, (uint16_t)Q.size(), 0, c, bv, last_posH, roots, max_roots, aborted);
                });

            std::vector<uint64_t>** array_bv = new std::vector<uint64_t>*[thread_roots.size()]();

            striped_counter counter_group_storage(nb_threads);
            striped_counter *counter_group = bounded_result ? &counter_group_storage : nullptr;

            parallel_roots_static(nb_threads, thread_roots.size(), height, array_bv,
                [&](size_t i, unsigned thread_id, std::vector<uint64_t> *scratch) {
                    uint64_t last_pos[height];
                    for (uint64_t j = 0; j < height; j++)
                        last_pos[j] = 0;
                    AND_wide1024<QdagT, Lazy>(Q, thread_roots[i], (uint16_t)Q.size(), cut, height - 1, scratch, last_pos, bounded_result, upper_bound, counter_group, (uint16_t)thread_id);
                });

            uint64_t node_size = 1ULL << max_nAttr;
            merge_root_buffers(bv, array_bv, thread_roots, cut, height, node_size);
        }

        SeQuadT *qResult = new SeQuadT(bv, Q[0]->getK(), (uint8_t)A.size(), Q[0]->getHeight());
        n_results = bv[height - 1].size();

        return qResult;
    }

    // Camino perezoso (Lazy) si algun qdag extiende por dispersion; si no, tablas de 16 bits.
    template<class QdagT, class SeQuadT>
    SeQuadT *multiJoinWide1024(std::vector<QdagT *> &Q, bool bounded_result,
                    uint64_t upper_bound, uint64_t &n_results,
                    uint8_t cut_off_level = 0) {
        if (any_scatter(Q))
            return multiJoinWide1024_impl<QdagT, SeQuadT, true>(Q, bounded_result, upper_bound, n_results, cut_off_level);
        return multiJoinWide1024_impl<QdagT, SeQuadT, false>(Q, bounded_result, upper_bound, n_results, cut_off_level);
    }

    // =========================== Dispatcher ===========================

    // Punto de entrada: <=6 multiJoinNew; 7-8 multiJoinWide; 9 multiJoinWide512; 10 multiJoinWide1024; >10 lanza.
    template<class QdagT, class SeQuadT>
    SeQuadT *multiJoinAuto(std::vector<QdagT *> &Q, bool bounded_result,
                            uint64_t upper_bound, uint8_t cut_off_level,
                            uint64_t &n_results) {
        std::map<uint64_t, uint8_t> attr_map;
        for (uint64_t i = 0; i < Q.size(); i++) {
            uint64_t nAttr = Q[i]->nAttr();
            for (uint64_t j = 0; j < nAttr; j++)
                attr_map[Q[i]->getAttr(j)] = 1;
        }
        if (attr_map.size() <= 6) {
            return multiJoinNew<QdagT, SeQuadT>(Q, bounded_result, upper_bound, cut_off_level, n_results);
        }
        if (attr_map.size() <= 8) {
            return multiJoinWide<QdagT, SeQuadT>(Q, bounded_result, upper_bound, n_results, cut_off_level);
        }
        if (attr_map.size() == 9) {
            return multiJoinWide512<QdagT, SeQuadT>(Q, bounded_result, upper_bound, n_results, cut_off_level);
        }
        if (attr_map.size() == 10) {
            return multiJoinWide1024<QdagT, SeQuadT>(Q, bounded_result, upper_bound, n_results, cut_off_level);
        }
        throw std::runtime_error("multiJoinAuto: unsupported number of joined attributes (>10): " + std::to_string(attr_map.size()));
    }

} // namespace gdj

#endif // QDAGS_JOINS_WIDE_HPP
