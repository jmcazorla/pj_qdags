#ifndef QDAGS_JOINS_BFS_HPP
#define QDAGS_JOINS_BFS_HPP

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

// EXPERIMENTAL: no esta conectado a multiJoinNew.
//
// Enumera las raices del nivel de corte por niveles (BFS), alternativa al DFS de partialAND (joins.hpp). Las
// raices quedan en un arreglo plano (sin un `new` por raiz) y el tope de memoria (`max_frontier`) se aplica por
// nivel, sin trabajo de mas.
//
// Contrato, para el mismo (Q, cut_level) que partialAND: mismas raices en el mismo orden y bv[0..cut_level)
// identico; bv[l] con l >= cut_level no se toca.
//
// Poda en dos pasadas: (1) hacia abajo guarda por nodo la mascara de hijos; (2) hacia arriba marca los nodos
// vivos (con algun hijo vivo) y emite bv[l] ya compactado.

namespace gdj {

// Raices del nivel de corte en arreglo plano: la raiz i ocupa flat[i*nQ .. i*nQ+nQ).
struct bfs_roots {
    size_t nQ = 0;
    size_t count = 0;
    std::vector<uint64_t> flat;
    const uint64_t *at(size_t i) const { return flat.data() + i * nQ; }
    uint64_t *at(size_t i) { return flat.data() + i * nQ; }
};

struct bfs_stats {
    uint64_t peak_frontier = 0;   // mayor numero de nodos en un nivel (incluye el del corte)
    uint64_t nodes_total = 0;     // nodos de todos los niveles 0..cut
    uint64_t peak_bytes = 0;      // pico estimado de memoria de trabajo (frontier + mascaras)
};

namespace detail {
// Pasada 2 (hacia arriba): desde masks[l][j] (hijos que sobreviven del nodo j del nivel l) marca los nodos vivos y
// emite bv[l] compactado. El nivel masks.size() (raices del corte o bits-hoja) no se poda.
inline void bfs_emit_levels(const std::vector<std::vector<uint64_t>> &masks, uint64_t p, std::vector<uint64_t> bv[]) {
    const int levels = (int)masks.size();
    std::vector<uint8_t> alive_child, alive;   // vivos del nivel l+1 / l
    for (int l = levels - 1; l >= 0; --l) {
        const size_t n = masks[l].size();
        alive.assign(n, 0);
        const bool below_is_leaf = (l == levels - 1);

        size_t cidx = 0;       // indice del primer hijo del nodo j en el nivel l+1
        uint64_t compact = 0;  // numero de nodo ya compactado en el nivel l
        for (size_t j = 0; j < n; ++j) {
            uint64_t m = masks[l][j];
            const size_t first_child = cidx;

            bool any = false;
            for (uint64_t mm = m; mm; mm &= mm - 1) {
                if (below_is_leaf || alive_child[cidx])
                    any = true;
                ++cidx;
            }
            alive[j] = any;
            if (!any)
                continue;

            size_t ci = first_child;
            while (m) {
                const uint64_t msb = __builtin_clzll(m);
                m &= (0xFFFFFFFFFFFFFFFFULL >> (msb + 1));
                if (below_is_leaf || alive_child[ci])
                    bv[l].push_back(compact * p + msb);
                ++ci;
            }
            ++compact;
        }
        alive_child.swap(alive);
    }
}
} // namespace detail

// Devuelve false (bv[0..cut) y out vacios) si algun nivel supera `max_frontier` nodos. nQ <= 16.
template<class QdagT>
bool enumerate_roots_bfs(uint16_t cut_level, std::vector<QdagT *> &Q, std::vector<uint64_t> bv[],
                         bfs_roots &out, uint64_t max_frontier = std::numeric_limits<uint64_t>::max(),
                         bfs_stats *stats = nullptr) {
    const size_t nQ = Q.size();
    const uint64_t p = Q[0]->nChildren();

    out.nQ = nQ;
    out.count = 0;
    out.flat.clear();
    for (uint16_t l = 0; l < cut_level; ++l)
        bv[l].clear();

    bfs_stats st;
    std::vector<uint64_t> cur(nQ, 0), next;          // frontier del nivel actual y del siguiente
    st.peak_frontier = st.nodes_total = 1;
    std::vector<std::vector<uint64_t>> masks(cut_level);   // masks[l][j]: hijos que sobreviven del nodo j del nivel l

    uint64_t rank_vector[16][64];
    uint64_t k_d[16];
    uint64_t root_temp[16];
    uint64_t mem_masks = 0;

    // Pasada 1 (hacia abajo): masks[l] y frontier del nivel l+1.
    for (uint16_t l = 0; l < cut_level; ++l) {
        const size_t n = cur.size() / nQ;
        masks[l].resize(n);
        next.clear();

        for (size_t j = 0; j < n; ++j) {
            const uint64_t *roots = &cur[j * nQ];
            uint64_t children = 0xFFFFFFFFFFFFFFFFULL;
            for (size_t i = 0; i < nQ && children; ++i) {
                k_d[i] = Q[i]->getKD();
                children &= Q[i]->materialize_node_64(l, roots[i], rank_vector[i]);
            }
            masks[l][j] = children;

            while (children) {
                const uint64_t msb = __builtin_clzll(children);
                children &= (0xFFFFFFFFFFFFFFFFULL >> (msb + 1));
                for (size_t q = 0; q < nQ; ++q)
                    root_temp[q] = k_d[q] * (rank_vector[q][Q[q]->getM((uint16_t)msb)] - 1);
                next.insert(next.end(), root_temp, root_temp + nQ);
            }
        }

        const uint64_t n_next = next.size() / nQ;
        if (n_next > max_frontier)
            return false;

        mem_masks += n * sizeof(uint64_t);
        st.peak_frontier = std::max(st.peak_frontier, n_next);
        st.nodes_total += n_next;
        st.peak_bytes = std::max(st.peak_bytes, mem_masks + (cur.capacity() + next.capacity()) * sizeof(uint64_t));
        cur.swap(next);
    }

    detail::bfs_emit_levels(masks, p, bv);

    out.count = cur.size() / nQ;
    out.flat.swap(cur);
    if (stats)
        *stats = st;
    return true;
}


// Join completo secuencial en BFS: llena bv[0..height) con el mismo resultado que AND() desde la raiz. Devuelve
// false (bv vacio) si algun nivel supera `max_frontier`. Sin upper_bound ni timeout.
template<class QdagT>
bool bfs_join_bv(std::vector<QdagT *> &Q, std::vector<uint64_t> bv[],
                 uint64_t max_frontier = std::numeric_limits<uint64_t>::max(), bfs_stats *stats = nullptr) {
    const size_t nQ = Q.size();
    const uint64_t p = Q[0]->nChildren();
    const uint16_t height = (uint16_t)Q[0]->getHeight();
    for (uint16_t l = 0; l < height; ++l)
        bv[l].clear();

    bfs_stats st;
    std::vector<uint64_t> cur(nQ, 0), next;
    st.peak_frontier = st.nodes_total = 1;
    std::vector<std::vector<uint64_t>> masks(height);

    uint64_t rank_vector[16][64];
    uint64_t k_d[16];
    uint64_t root_temp[16];
    uint64_t mem_masks = 0;

    for (uint16_t l = 0; l < height; ++l) {
        const size_t n = cur.size() / nQ;
        masks[l].resize(n);
        next.clear();
        const bool last = (l == height - 1);

        for (size_t j = 0; j < n; ++j) {
            const uint64_t *roots = &cur[j * nQ];
            uint64_t children = 0xFFFFFFFFFFFFFFFFULL;
            if (last) {
                for (size_t i = 0; i < nQ && children; ++i)
                    children &= Q[i]->materialize_node_64_last(l, roots[i]);
                masks[l][j] = children;
                continue;
            }
            for (size_t i = 0; i < nQ && children; ++i) {
                k_d[i] = Q[i]->getKD();
                children &= Q[i]->materialize_node_64(l, roots[i], rank_vector[i]);
            }
            masks[l][j] = children;

            while (children) {
                const uint64_t msb = __builtin_clzll(children);
                children &= (0xFFFFFFFFFFFFFFFFULL >> (msb + 1));
                for (size_t q = 0; q < nQ; ++q)
                    root_temp[q] = k_d[q] * (rank_vector[q][Q[q]->getM((uint16_t)msb)] - 1);
                next.insert(next.end(), root_temp, root_temp + nQ);
            }
        }

        mem_masks += n * sizeof(uint64_t);
        if (last)
            break;
        const uint64_t n_next = next.size() / nQ;
        if (n_next > max_frontier)
            return false;
        st.peak_frontier = std::max(st.peak_frontier, n_next);
        st.nodes_total += n_next;
        st.peak_bytes = std::max(st.peak_bytes, mem_masks + (cur.capacity() + next.capacity()) * sizeof(uint64_t));
        cur.swap(next);
    }

    detail::bfs_emit_levels(masks, p, bv);
    if (stats)
        *stats = st;
    return true;
}

} // namespace gdj

#endif // QDAGS_JOINS_BFS_HPP
