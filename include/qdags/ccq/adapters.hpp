#ifndef QDAGS_CCQ_ADAPTERS_HPP
#define QDAGS_CCQ_ADAPTERS_HPP

#include "qdags.hpp"   // importado: ccq::rank_bv_64, ccq::se_quadtree, ccq::qdag, ccq::initializable_array

#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <stdexcept>
#include <vector>

// Adaptadores entre las clases importadas de ccqQDAGS (no se editan) y el contrato SeQuadT/QdagT de las estrategias.

namespace ccq {

    using edge_type = se_quadtree::idx_type;

    class se_quadtree_t : public se_quadtree {
    public:
        se_quadtree_t() {
            bv = nullptr;
            active = nullptr;
            height = 0;
            k = 0;
            d = 0;
            k_d = 0;
            ref_count = 1;
        }

        se_quadtree_t(std::vector<std::vector<edge_type>> &edges, const size_type grid_side, uint8_t _k, uint8_t _d)
            : se_quadtree(edges, grid_side, _k, _d) {}

        // Desde las posiciones de unos por nivel (lo que escribe AND); `active` queda igual a bv. La altura se deduce
        // de grid_side, por eso se pasa k^(altura-1)+1.
        se_quadtree_t(std::vector<uint64_t> bv_ones[], uint8_t _k, uint8_t _d, uint16_t _height)
            : se_quadtree(bv_ones, bv_ones, grid_for_height(_k, _height), _k, _d) {
            ref_count = 1;
        }

        // Proyeccion: quita de `other` los atributos de `attributes`; `active` queda en todo 1.
        se_quadtree_t(se_quadtree_t *other, std::vector<uint16_t> &attributes, uint8_t _k, uint8_t _d, uint16_t _height) {
            k = _k;
            d = _d;
            height = _height;
            k_d = std::pow(_k, _d);
            ref_count = 1;
            const uint64_t old_kd = other->getKD();
            const uint16_t d_orig = (uint16_t)(attributes.size() + _d);
            std::vector<uint16_t> removed_bits;   // bit (desde el menos significativo) de cada atributo quitado
            for (uint16_t a : attributes)
                removed_bits.push_back((uint16_t)(d_orig - a - 1));

            bv = new rank_bv_64[height];
            active = new rank_bv_64[height];
            std::vector<std::vector<uint64_t>> cur(1, std::vector<uint64_t>(1, 0));   // un grupo de nodos origen por nodo nuevo
            for (uint16_t level = 0; level < height; level++) {
                sdsl::bit_vector k_t(k_d * cur.size(), 0);
                std::vector<std::vector<uint64_t>> next;
                for (size_t g = 0; g < cur.size(); g++) {
                    std::map<uint64_t, std::vector<uint64_t>> by_child;   // hijo nuevo -> nodos origen del nivel siguiente
                    for (uint64_t src : cur[g]) {
                        uint64_t pos = src * old_kd;
                        uint64_t one_pos = other->bv[level].rank(pos);
                        for (uint64_t p = 0; p < old_kd; p++, pos++) {
                            if ((other->bv[level].seq[pos >> 6] >> (pos & 63)) & 1ULL)
                                by_child[remove_bits(p, removed_bits)].push_back(one_pos++);
                        }
                    }
                    for (auto &kv : by_child) {
                        k_t[g * k_d + kv.first] = 1;
                        next.push_back(std::move(kv.second));
                    }
                }
                bv[level] = rank_bv_64(k_t, k_d);
                sdsl::bit_vector all_ones(k_t.size(), 1);
                active[level] = rank_bv_64(all_ones, k_d);
                cur = std::move(next);
            }
        }

        // rank_bv_64 no libera solo sus arreglos; se liberan aca si este objeto es el unico dueno.
        ~se_quadtree_t() {
            if (ref_count == 1 && bv != nullptr) {
                for (uint16_t i = 0; i < height; i++) {
                    bv[i].release();
                    if (active) active[i].release();
                }
                delete[] active;
                active = nullptr;
            }
        }

        se_quadtree_t(const se_quadtree_t &) = delete;
        se_quadtree_t &operator=(const se_quadtree_t &) = delete;

        // false si el archivo no se pudo leer entero (lo usa resource_manager).
        bool load(std::ifstream &in) {
            in.read(reinterpret_cast<char *>(&k), sizeof(k));
            in.read(reinterpret_cast<char *>(&d), sizeof(d));
            in.read(reinterpret_cast<char *>(&height), sizeof(height));
            in.read(reinterpret_cast<char *>(&k_d), sizeof(k_d));
            if (in.fail() || height == 0)
                return false;
            bv = new rank_bv_64[height];
            active = new rank_bv_64[height];
            for (uint16_t i = 0; i < height; i++) {
                bv[i] = rank_bv_64::load(in);
                if (in.fail())
                    return false;
            }
            for (uint16_t i = 0; i < height; i++) {
                active[i] = rank_bv_64::load(in);
                if (in.fail())
                    return false;
            }
            uint64_t to_size = 0;
            in.read(reinterpret_cast<char *>(&to_size), sizeof(to_size));
            if (in.fail())
                return false;
            total_ones.resize(to_size);
            if (to_size)
                in.read(reinterpret_cast<char *>(total_ones.data()), sizeof(uint64_t) * to_size);
            ref_count = 1;
            return !in.fail();
        }

        uint64_t size_in_bytes() { return size(); }

        // Unos del ultimo nivel = numero de tuplas (total_ones de ccq no se llena).
        uint64_t n_ones() { return bv[height - 1].n_ones(); }
        uint64_t total_ones_last_level() { return n_ones(); }

        rank_bv_64 *get_bv() { return bv; }

    private:
        static uint64_t grid_for_height(uint8_t k, uint16_t h) {
            uint64_t g = 1;
            for (uint16_t i = 1; i < h; i++)
                g *= k;
            return g + 1;
        }

        // Quita de x los bits en las posiciones `removed` y compacta el resto.
        static uint64_t remove_bits(uint64_t x, const std::vector<uint16_t> &removed) {
            uint64_t out = 0;
            uint16_t o = 0;
            for (uint16_t b = 0; b < 16; b++) {   // d <= 11 con k = 2
                bool skip = false;
                for (uint16_t r : removed)
                    if (r == b) { skip = true; break; }
                if (skip)
                    continue;
                out |= ((x >> b) & 1ULL) << o++;
            }
            return out;
        }
    };

    class qdag_t {
    public:
        using att_set = std::vector<uint16_t>;

        // `base`: posicion de cada atributo del arbol dentro de `att_ext`. Arma el qdag de ccq y lo extiende a att_ext.
        template<class AttExt>
        qdag_t(se_quadtree_t *Q, att_set &base, uint64_t grid_side, const AttExt &att_ext, const void * = nullptr)
            : tree_(Q), ext_(nullptr) {
            qdag::att_set ids, ext_ids;
            for (auto a : att_ext)
                ext_ids.push_back((uint64_t)a);
            for (auto b : base) {
                if (b >= ext_ids.size())
                    throw std::runtime_error("ccq::qdag_t: attribute position outside att_ext");
                ids.push_back(ext_ids[b]);
            }
            qdag b(Q, ids, grid_side, Q->getK(), Q->getD());
            ext_ = b.extend(ext_ids);
        }

        // El 5to argumento (wide_base_config) se ignora: ccq no tiene dispersion.
        template<class AttExt, class Wide>
        qdag_t(se_quadtree_t *Q, att_set &base, uint64_t grid_side, const AttExt &att_ext, const Wide &)
            : qdag_t(Q, base, grid_side, att_ext) {}

        ~qdag_t() { delete ext_; }
        qdag_t(const qdag_t &) = delete;
        qdag_t &operator=(const qdag_t &) = delete;

        uint64_t nAttr() const { return ext_->nAttr(); }
        uint64_t getAttr(uint64_t i) const { return ext_->getAttr(i); }
        uint64_t getGridSide() const { return ext_->getGridSide(); }
        uint64_t getHeight() const { return ext_->getHeight(); }
        uint8_t getK() const { return ext_->getK(); }
        uint16_t nChildren() const { return ext_->nChildren(); }
        uint64_t getKD() const { return ext_->getKD(); }
        se_quadtree_t *getQ() const { return tree_; }

        qdag *ccq() const { return ext_; }   // el que consume el AND de ccq

    private:
        se_quadtree_t *tree_;
        qdag *ext_;
    };

} // namespace ccq

#endif // QDAGS_CCQ_ADAPTERS_HPP
