#ifndef QDAGS_JOINS_HPP
#define QDAGS_JOINS_HPP

#include <algorithm>
#include <map>
#include <vector>
#include <cstdint>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <mutex>

#include "../index/commun.hpp"
#include "parallel_for.hpp"
#include <limits>
#include <cstdlib>

// Multi-way join recursivo sobre QDAGs, camino denso (<=6 atributos de salida). Punto de entrada: multiJoinNew
// (particion estatica en dos fases). parAND (round-robin) esta definida pero no se usa: duplica posiciones.

namespace gdj {

// Watchdog de timeout cooperativo (inline: el header se incluye desde varios .cpp).

inline std::atomic<bool> timeout_triggered{false};
inline std::condition_variable cv_watchdog;
inline std::mutex mtx_watchdog;
inline bool query_finished = false;
inline std::thread timer_thread;

// Hilo de timeout: espera `seconds` o el fin de la query; si vence, marca timeout_triggered (AND lo consulta).
inline void watchdog_safe(uint64_t seconds) {
    if (seconds == 0)
        return;
    std::unique_lock<std::mutex> lock(mtx_watchdog);
    if (!cv_watchdog.wait_for(lock, std::chrono::seconds(seconds), []{ return query_finished; })) {
        timeout_triggered.store(true, std::memory_order_relaxed);
    }
}

// Arranca el watchdog (detiene uno anterior). seconds=0: sin limite, sin hilo.
inline void start_timeout_watchdog(uint64_t seconds) {
    if (timer_thread.joinable()) {
        {
            std::lock_guard<std::mutex> lock(mtx_watchdog);
            query_finished = true;
        }
        cv_watchdog.notify_one();
        timer_thread.join();
    }

    timeout_triggered.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(mtx_watchdog);
        query_finished = false;
    }
    if (seconds > 0) {
        timer_thread = std::thread(watchdog_safe, seconds);
    }
}

// Senaliza el fin normal de la query; llamar siempre despues de start_timeout_watchdog.
inline void stop_timeout_watchdog() {
    {
        std::lock_guard<std::mutex> lock(mtx_watchdog);
        query_finished = true;
    }
    cv_watchdog.notify_one();
    if (timer_thread.joinable()) {
        timer_thread.join();
    }
}

// Contador por hilo (sin linea de cache compartida) para chequear upper_bound en la Fase 2 de multiJoinNew.
// add()/sum() se llaman una vez por nodo, no por resultado.
struct striped_counter {
    static constexpr size_t CACHE_LINE = 64;
    struct alignas(CACHE_LINE) padded_atomic {
        std::atomic<uint64_t> value{0};
    };
    std::vector<padded_atomic> slots;

    explicit striped_counter(size_t n) : slots(std::max<size_t>(n, 1)) {}

    inline void add(uint16_t thread_id, uint64_t n) {
        if (n)
            slots[thread_id].value.fetch_add(n, std::memory_order_relaxed);
    }

    inline uint64_t sum() const {
        uint64_t total = 0;
        for (auto &s : slots)
            total += s.value.load(std::memory_order_relaxed);
        return total;
    }
};

// Poda hacia arriba las posiciones de bv[] que quedaron sin hijos tras descartar buffers vacios.
inline void filterHeightBv(std::vector<uint64_t> bv[], uint8_t level, std::vector<uint64_t> &delete_pos, uint64_t node_size) {
    if (delete_pos.size() == 0)
        return;

    std::vector<uint64_t> &cur = bv[level];
    uint64_t initial_size = cur.size();
    uint64_t start = delete_pos[0];

    uint64_t node_pos = 0;
    uint64_t count_delete_node = 0;
    uint64_t count_bit_node = 0;
    uint64_t increase = 0;

    if (start > 0) {
        node_pos = cur[start - 1] / node_size;
        count_bit_node++;
    }

    std::vector<uint64_t> new_bv;
    new_bv.reserve(initial_size);
    new_bv.insert(new_bv.end(), cur.begin(), cur.begin() + start);

    std::vector<uint64_t> next_delete_pos;
    size_t del_idx = 0;
    size_t n_del = delete_pos.size();

    for (uint64_t h = start; h < initial_size; h++) {
        uint64_t aux_node_pos = cur[h] / node_size;

        if (aux_node_pos > node_pos) {
            if (count_delete_node == count_bit_node) {
                next_delete_pos.push_back(node_pos);
                increase += node_size;
            }
            node_pos = aux_node_pos;
            count_delete_node = 0;
            count_bit_node = 0;
        }

        if (del_idx < n_del && delete_pos[del_idx] == h) {
            ++del_idx;
            ++count_delete_node;
        } else {
            new_bv.push_back(cur[h] - increase);
        }
        ++count_bit_node;
    }

    if (count_delete_node == count_bit_node) {
        next_delete_pos.push_back(node_pos);
    }

    cur.swap(new_bv);

    if (next_delete_pos.size() > 0 && level > 0) {
        filterHeightBv(bv, level - 1, next_delete_pos, node_size);
    }
}

// Reparto round-robin en parallel_cut_level, con bv[] compartido bajo mutex. No se usa: sobre ese nivel varios
// hilos pueden escribir posiciones duplicadas para el mismo nodo padre.
template<class QdagT>
bool parAND(uint16_t total_threads, uint16_t thread_id, uint16_t parallel_cut_level, std::mutex &shared_mutex,
            std::vector<QdagT *> &Q, uint64_t *roots, uint16_t nQ,
            uint16_t cur_level, uint16_t max_level,
            std::vector<uint64_t> bv[], uint64_t last_pos[], uint64_t ancestor[], uint64_t nAtt,
            bool bounded_result, uint64_t upper_bound) {
    uint64_t p = Q[0]->nChildren();
    bool just_zeroes = true;
    uint64_t k_d[16];

    uint16_t children_to_recurse[512];

    uint64_t i;
    uint64_t children_to_recurse_size = 0;

    uint64_t children = 0xFFFFFFFFFFFFFFFFULL;

    if (cur_level == max_level) {
        for (i = 0; i < nQ && children; ++i)
            children &= Q[i]->materialize_node_64_last(cur_level, roots[i]);

        children_to_recurse_size = sdsl::bits::cnt(children);
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
                return false;
            else {
                shared_mutex.lock();
                bv[cur_level].push_back(last_pos[cur_level]++);
                shared_mutex.unlock();

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

        uint64_t first_node_id = 0;

        if (cur_level == parallel_cut_level) {
            uint64_t nodes_per_level = 1 << nAtt;
            uint64_t multiplier = nodes_per_level;

            for (int level = cur_level - 1; level >= 0; level--) {
                first_node_id += ancestor[level] * multiplier;
                multiplier *= nodes_per_level;
            }
        }

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

            if (cur_level == parallel_cut_level) {
                if ((first_node_id + child) % total_threads != thread_id)
                    continue;
            }

            for (uint64_t j = 0; j < nQ; j++)
                root_temp[j] = k_d[j] * (rank_vector[j][Q[j]->getM(child)] - 1);

            if (child - last_child > 1)
                last_pos[cur_level] += (child - last_child - 1);

            last_child = child;

            if (bounded_result && bv[max_level].size() >= upper_bound)
                return false;

            ancestor[cur_level] = child;
            if (parAND<QdagT>(total_threads, thread_id, parallel_cut_level, shared_mutex, Q, root_temp, nQ, cur_level + 1, max_level, bv, last_pos, ancestor, nAtt, bounded_result, upper_bound)) {
                shared_mutex.lock();
                bv[cur_level].push_back(last_pos[cur_level]++);
                shared_mutex.unlock();

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

// Nucleo secuencial del join: lo usa multiJoinNew con cut_off_level==0 y para cada raiz del nivel de corte.
// counter_group: si no es nullptr, upper_bound se mide contra su suma y no contra bv[max_level].size()
// (necesario cuando bv es un buffer privado de un hilo).
template<class QdagT>
bool AND(std::vector<QdagT *> &Q, uint64_t *roots, uint16_t nQ,
         uint16_t cur_level, uint16_t max_level,
         std::vector<uint64_t> bv[], uint64_t last_pos[], uint64_t nAtt,
         bool bounded_result, uint64_t upper_bound,
         striped_counter *counter_group = nullptr, uint16_t counter_slot = 0) {
    if (timeout_triggered.load(std::memory_order_relaxed))
        return false;

    uint64_t p = Q[0]->nChildren();
    bool just_zeroes = true;
    uint64_t k_d[16];

    uint16_t children_to_recurse[512];

    uint64_t i;
    uint64_t children_to_recurse_size = 0;

    uint64_t children = 0xFFFFFFFFFFFFFFFFULL;

    if (cur_level == max_level) {
        for (i = 0; i < nQ && children; ++i)
            children &= Q[i]->materialize_node_64_last(cur_level, roots[i]);

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
                // !just_zeroes y no `false`: si ya empujo entradas, `false` haria que el caller las deshaga.
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

        for (i = 0; i < nQ && children; ++i) {
            k_d[i] = Q[i]->getKD();
            children &= Q[i]->materialize_node_64(cur_level, roots[i], rank_vector[i]);
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

            for (uint64_t j = 0; j < nQ; j++)
                root_temp[j] = k_d[j] * (rank_vector[j][Q[j]->getM(child)] - 1);

            if (child - last_child > 1)
                last_pos[cur_level] += (child - last_child - 1);

            last_child = child;

            if (!(!counter_group && bounded_result && bv[max_level].size() >= upper_bound) &&
                AND<QdagT>(Q, root_temp, nQ, cur_level + 1, max_level, bv, last_pos, nAtt, bounded_result, upper_bound, counter_group, counter_slot)) {
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

// Fase 1 de multiJoinNew: desciende hasta cut_level y enumera en thread_roots las raices de subarbol que sobreviven.
// max_roots/aborted: tope de raices y bandera de aborto (tope alcanzado o timeout).
template<class QdagT>
bool partialAND(uint16_t cut_level, std::vector<QdagT *> &Q, uint64_t *roots, uint16_t nQ,
                 uint16_t cur_level, uint16_t max_level, std::vector<uint64_t> bv[], uint64_t last_pos[],
                 uint64_t nAtt, std::vector<uint64_t*> &thread_roots,
                 uint64_t max_roots = std::numeric_limits<uint64_t>::max(), bool *aborted = nullptr) {
    // AND no corre hasta el nivel de corte: el timeout se revisa tambien aca.
    if (timeout_triggered.load(std::memory_order_relaxed)) {
        if (aborted)
            *aborted = true;
        return false;
    }

    uint64_t p = Q[0]->nChildren();
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
        uint64_t* rts = new uint64_t[16];
        for (uint64_t i = 0; i < nQ; ++i)
            rts[i] = roots[i];
        thread_roots.push_back(rts);
        return true;
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

            bool child_ok_ = partialAND<QdagT>(cut_level, Q, root_temp, nQ, cur_level + 1, max_level, bv, last_pos, nAtt, thread_roots, max_roots, aborted);
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

// Reparto estatico de las raices del nivel de corte en bloques contiguos por hilo, cada raiz con su propio bv.
// run_root(i, thread_id, bv_de_la_raiz).
template<class RunRoot>
inline void parallel_roots_static(unsigned nb_threads, size_t n_roots, size_t height,
                                         std::vector<uint64_t> **array_bv, RunRoot &&run_root) {
    for (size_t t = 0; t < n_roots; ++t)
        array_bv[t] = new std::vector<uint64_t>[height];

    if (n_roots < nb_threads) {
        parallel_for((unsigned)n_roots, [&](int start, int end) {
            for (int thread_id = start; thread_id < end; ++thread_id)
                run_root((size_t)thread_id, (unsigned)thread_id, array_bv[thread_id]);
        });
    }
    else {
        parallel_for(nb_threads, [&](int start, int end) {
            for (int thread_id = start; thread_id < end; ++thread_id) {
                unsigned base_per_thread = (unsigned)n_roots / nb_threads;
                unsigned remainder = (unsigned)n_roots % nb_threads;
                unsigned part_per_thread = base_per_thread + ((unsigned)thread_id < remainder ? 1 : 0);
                unsigned thread_start = thread_id * base_per_thread + std::min((unsigned)thread_id, remainder);
                unsigned thread_end = thread_start + part_per_thread;
                for (size_t i = thread_start; i < thread_end; i++)
                    run_root(i, (unsigned)thread_id, array_bv[i]);
            }
        });
    }
}

// Fusiona en bv[] los resultados por raiz, poda las raices vacias y libera array_bv/thread_roots.
inline void merge_root_buffers(std::vector<uint64_t> bv[], std::vector<uint64_t> **array_bv,
                               std::vector<uint64_t *> &thread_roots, size_t cut_off_level, size_t height,
                               uint64_t node_size) {
    bool first = true;
    uint64_t base_pos = 0;
    std::vector<uint64_t> delete_high_levels;

    for (size_t i = 0; i < thread_roots.size(); i++) {
        if (array_bv[i][cut_off_level].size() == 0) {
            delete_high_levels.push_back(i);
            continue;
        }
        first = bv[cut_off_level].size() == 0;
        for (size_t j = cut_off_level; j < height; j++) {
            if (first)
                bv[j].insert(bv[j].end(), array_bv[i][j].begin(), array_bv[i][j].end());
            else {
                base_pos = ((bv[j][bv[j].size() - 1] / node_size) + 1) * node_size;
                for (size_t k = 0; k < array_bv[i][j].size(); k++)
                    bv[j].push_back(array_bv[i][j][k] + base_pos);
            }
        }
    }

    if (delete_high_levels.size() > 0)
        filterHeightBv(bv, (uint8_t)(cut_off_level - 1), delete_high_levels, node_size);

    for (size_t i = 0; i < thread_roots.size(); i++) {
        delete[] array_bv[i];
        delete[] thread_roots[i];
    }
    delete[] array_bv;
}

// Tope de raices del nivel de corte: crecen por 2^d en cada nivel y un nivel alto en un join ancho agota la
// memoria. Si la enumeracion supera join_max_roots() se aborta y se reintenta un nivel mas arriba (minimo 1);
// el resultado no cambia. Default 65536; QDAGS_JOIN_MAX_ROOTS o set_join_max_roots() lo cambian, 0 = sin tope.
inline std::atomic<int64_t>& join_max_roots_setting() {
    static std::atomic<int64_t> s{-1};
    return s;
}

inline void set_join_max_roots(int64_t n) { join_max_roots_setting().store(n < 0 ? -1 : n); }

inline uint64_t join_max_roots() {
    int64_t s = join_max_roots_setting().load(std::memory_order_relaxed);
    if (s >= 0)
        return (uint64_t)s;
    static const uint64_t from_env = [] {
        const char* e = std::getenv("QDAGS_JOIN_MAX_ROOTS");
        return (e && *e) ? (uint64_t)std::strtoull(e, nullptr, 10) : (uint64_t)65536;
    }();
    return from_env;
}

struct join_partition_counters {
    std::atomic<uint64_t> cap_reductions{0};  // veces que el tope bajo el nivel de corte
    std::atomic<uint64_t> timeout_aborts{0};  // enumeraciones abortadas por el timeout
};

inline join_partition_counters& join_partition_stats() {
    static join_partition_counters c;
    return c;
}

// Enumera las raices con `enumerate` (el partialAND que corresponda). Devuelve el nivel de corte efectivo, o 0 si
// vencio el timeout (thread_roots y bv[] quedan vacios).
template<class Enumerate>
inline int enumerate_roots_capped(int cut_level, size_t height, std::vector<uint64_t> bv[],
                                  std::vector<uint64_t *> &thread_roots, Enumerate &&enumerate) {
    const uint64_t cap = join_max_roots();
    uint64_t max_roots = cap == 0 ? std::numeric_limits<uint64_t>::max() : cap;
    int cut = cut_level;
    while (true) {
        bool aborted = false;
        enumerate(cut, thread_roots, max_roots, &aborted);
        if (!aborted)
            return cut;

        for (auto *r : thread_roots)
            delete[] r;
        thread_roots.clear();
        for (size_t l = 0; l < height; ++l)
            bv[l].clear();

        if (timeout_triggered.load(std::memory_order_relaxed)) {
            join_partition_stats().timeout_aborts.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        if (cut > 1) {
            --cut;
            join_partition_stats().cap_reductions.fetch_add(1, std::memory_order_relaxed);
        }
        else
            max_roots = std::numeric_limits<uint64_t>::max();  // no se puede bajar mas: sin tope en el nivel 1
    }
}

// Motor de dos fases: enumera las raices en cut_off_level (partialAND), las reparte entre hilos
// (parallel_roots_static) y fusiona los buffers (merge_root_buffers).
template<class QdagT, class SeQuadT>
SeQuadT *multiJoinNew(std::vector<QdagT *> &Q, bool bounded_result, uint64_t upper_bound, uint8_t cut_off_level, uint64_t &n_results) {
    gdj::att_set A;
    std::map<uint64_t, uint8_t> attr_map;

    uint64_t Q_roots[Q.size()];
    std::vector<uint64_t*> thread_roots;

    auto height = Q[0]->getHeight();
    std::vector<uint64_t> bv[height];
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

    if (cut_off_level == 0) {
        uint64_t last_pos[height];
        for (uint64_t i = 0; i < height; i++)
            last_pos[i] = 0;

        AND<QdagT>(Q, Q_roots, Q.size(), 0, height - 1, bv, last_pos, A.size(), bounded_result, upper_bound);
    }
    else {
        unsigned nb_threads_hint = THREADS_BY_CORE * std::thread::hardware_concurrency();
        unsigned nb_threads = nb_threads_hint == 0 ? 8 : nb_threads_hint;

        const int cut = enumerate_roots_capped((int)cut_off_level, height, bv, thread_roots,
            [&](int c, std::vector<uint64_t *> &roots, uint64_t max_roots, bool *aborted) {
                uint64_t last_posH[c];
                for (int i = 0; i < c; i++)
                    last_posH[i] = 0;
                partialAND<QdagT>(c, Q, Q_roots, Q.size(), 0, c, bv, last_posH, A.size(), roots, max_roots, aborted);
            });

        std::vector<uint64_t>** array_bv = new std::vector<uint64_t>*[thread_roots.size()]();

        striped_counter counter_group_storage(nb_threads);
        striped_counter *counter_group = bounded_result ? &counter_group_storage : nullptr;

        parallel_roots_static(nb_threads, thread_roots.size(), height, array_bv,
            [&](size_t i, unsigned thread_id, std::vector<uint64_t> *scratch) {
                uint64_t last_pos[height];
                for (uint64_t j = 0; j < height; j++)
                    last_pos[j] = 0;
                AND<QdagT>(Q, thread_roots[i], Q.size(), cut, height - 1, scratch, last_pos, A.size(), bounded_result, upper_bound, counter_group, (uint16_t)thread_id);
            });

        uint64_t node_size = 1 << max_nAttr;
        merge_root_buffers(bv, array_bv, thread_roots, cut, height, node_size);
    }

    SeQuadT *qResult = new SeQuadT(bv, Q[0]->getK(), (uint8_t)A.size(), Q[0]->getHeight());
    n_results = bv[height - 1].size();
    return qResult;
}

} // namespace gdj

#endif // QDAGS_JOINS_HPP
