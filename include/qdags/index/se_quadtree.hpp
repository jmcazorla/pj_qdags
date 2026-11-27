/*
    Copyright (C) 2020 Diego Arroyuelo
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    You should have received a copy of the GNU General Public License
    along with this program.  If not, see http://www.gnu.org/licenses/ .
*/
/*! \file se_quadtree.hpp
    \brief se_quadtree.hpp contains a space-efficient quadtree implementation.
    \author Diego Arroyuelo
    \Based on Francisco Montoto's k2-tree implementation (sdsl)
*/
#ifndef SE_QUADTREE_HPP
#define SE_QUADTREE_HPP

#include <deque>
#include <queue>
#include <stdexcept>
#include <tuple>
#include <set>
#include <fstream>
#include <sdsl/bit_vectors.hpp>
#include <sdsl/k2_tree_helper.hpp>
#include <sdsl/int_vector_buffer.hpp>
#include "commun.hpp"
#include "../detail/bits.hpp"

// se_quadtree<RankBackend>: arbol k^d-ario comprimido por niveles, cada
// nivel un RankBackend (rank_p.hpp/rank_rrr_p.hpp/rank_sd_p.hpp/
// rank_vfamily_p.hpp) con soporte de rank. Representa una relacion de d
// dimensiones sobre una grilla de lado k^height. Basado en el k2-tree de
// Brisaboa, Ladra, Navarro (SPIRE 2009) y en la implementacion de
// Francisco Montoto (sdsl).
/*! A k^2-tree is a compact tree structure to represent a web graph. The
 *  structure takes advantage of large empty areas of the adjacency matrix of
 *  the graph.
 *
 *  \par References
 *      [1] Brisaboa, N. R., Ladra, S., & Navarro, G. (2009, August):
 *          k2-trees for compact web graph representation. In International
 *          Symposium on String Processing and Information Retrieval
 *          (pp. 18-30). Springer Berlin Heidelberg.
 */

namespace gdj {

    // Altura de un k^d-tree sobre una grilla de lado `size`: el menor h>=1
    // tal que k^h >= size.
    inline uint16_t quadtree_height(uint64_t size, uint64_t k) {
        uint16_t h = 0;
        uint64_t p = 1;
        while (p < size) {
            ++h;
            if (p > UINT64_MAX / k)
                break; // k^h ya supera cualquier uint64_t
            p *= k;
        }
        return h > 1 ? h : 1;
    }

    template<class RankBackend>
    class se_quadtree {

    private:
            RankBackend   *bv = nullptr;

            uint16_t    height;   // numero de niveles del arbol

            uint8_t     k;
            uint8_t     d;
            size_type   k_d;
            std::vector<uint64_t> total_ones;

    protected:

        // Indice de cuadrante ([0, k^d[) de un punto dentro de la submatrizdada por offset/l.
        uint16_t get_chunk_idx(std::vector<gdj::idx_type> &point, gdj::idx_type* offset, size_type l, uint8_t _k) {
            uint16_t i, _d = point.size();
            uint16_t total_sum = 0, _k_aux = 1;

            for (i = _d-1; i > 0; --i) {
                total_sum += ((point[i]-offset[i])/l)*_k_aux;
                _k_aux *= _k;
            }

            total_sum += ((point[0]-offset[0])/l)*_k_aux;
            return total_sum;
        }

        // Construye el quadtree a partir de una lista de puntos d-dimensionales(edges), en
        // tiempo lineal sobre su cantidad.
        void build_from_edges(std::vector<std::vector<gdj::idx_type>>& edges, const size_type size, uint8_t _k, uint8_t _d) {
            k = _k;
            d = _d;
            height = quadtree_height(size, k);

            bv = new RankBackend[height];
            total_ones.resize(height);

            k_d = std::pow(k, d);

            // Sin puntos: arbol vacio valido .
            if (edges.empty()) {
                bv[0] = RankBackend(sdsl::bit_vector(k_d, 0));
                for (uint16_t lv = 1; lv < height; lv++)
                    bv[lv] = RankBackend(sdsl::bit_vector(0, 0));
                std::fill(total_ones.begin(), total_ones.end(), 0);
                return;
            }

            sdsl::bit_vector k_t = sdsl::bit_vector(k_d, 0);

            std::queue<t_part_tuple> q;
            gdj::idx_type t = 0, last_level = 0;
            gdj::idx_type i, j, r_0, c_0, it, c, r, z;
            size_type l = std::pow(k, height - 1);

            std::vector<gdj::idx_type> pos_by_chunk(k_d + 1, 0);

            gdj::idx_type* top_left_point = new gdj::idx_type[d]();
            gdj::idx_type* top_left_point_aux;

            q.push(t_part_tuple(0, edges.size(), l, top_left_point));

            size_type cur_l = l, cur_level = 0, n_ones = 0;

            while (!q.empty()) {
                std::vector<gdj::idx_type> amount_by_chunk(k_d, 0);
                std::tie(i, j, l, top_left_point) = q.front();
                q.pop();

                if (l != cur_l) {
                    cur_l = l;
                    k_t.resize(t);
                    bv[cur_level] = RankBackend(k_t);
                    total_ones[cur_level] =  bv[cur_level].n_ones();
                    cur_level++;
                    t = 0;
                    k_t.resize(0);
                    k_t = sdsl::bit_vector(k_d*n_ones, 0);
                    n_ones = 0;
                }

                // Tamano de cada cuadrante
                for (it = i; it < j; it++)
                    amount_by_chunk[get_chunk_idx(edges[it], top_left_point, l, k)] += 1;

                if (l == 1) {
                    for (it = 0; it < k_d; it++,t++)
                        if (amount_by_chunk[it] != 0)
                            k_t[t] = 1;
                        else
                            k_t[t] = 0;
                    // En l==1 no se encolan nuevos elementos; el arreglo de punto se libera aca
                    // (el delete[] del final del loop se salta por el continue).
                    delete [] top_left_point;
                    continue;
                }

                // Posicion de inicio de cada cuadrante en el vector
                pos_by_chunk[0] = i;

                for (it = 1; it < k_d; it++)
                    pos_by_chunk[it] = pos_by_chunk[it - 1] + amount_by_chunk[it - 1];
                pos_by_chunk[k_d] = j; // caso it = k_d - 1

                // Encola cada cuadrante no vacio
                for (it = 0; it < k_d; it++,t++)
                    if (amount_by_chunk[it] != 0) {
                        uint16_t p = std::pow(k, d-1);
                        gdj::idx_type it_aux = it;
                        c = it % k;
                        k_t[t] = 1;
                        n_ones++;

                        top_left_point_aux = new gdj::idx_type[d]();

                        for (z = 0; z < d-1; ++z) {
                            r = it_aux / p;
                            it_aux -= r*p;
                            p /= k;
                            top_left_point_aux[z] = top_left_point[z] + r*l;
                        }

                        top_left_point_aux[d-1] = top_left_point[d-1] + c*l;

                        q.push(t_part_tuple(pos_by_chunk[it], pos_by_chunk[it + 1],
                                            l/k, top_left_point_aux));
                    }
                    else
                        k_t[t] = 0;

                gdj::idx_type chunk;

                // Reordena el rango de edges por cuadrante, en el lugar
                for (unsigned ch = 0; ch < k_d; ch++) {
                    gdj::idx_type be = ch == 0 ? i : pos_by_chunk[ch - 1];
                    for (it = pos_by_chunk[ch]; it < be + amount_by_chunk[ch];) {
                        chunk = get_chunk_idx(edges[it],top_left_point, l, k);

                        if (pos_by_chunk[chunk] != it)
                            std::iter_swap(edges.begin() + it, edges.begin() + pos_by_chunk[chunk]);
                        else
                            it++;
                        pos_by_chunk[chunk]++;
                    }
                }
                delete [] top_left_point;
            }

            k_t.resize(t);
            bv[height-1] = RankBackend(k_t);

            total_ones[height-1] = bv[height-1].n_ones();
        }

        // Construye el nivel `level` de una proyeccion: para cada bloque del nivel original, le quita los
        // bits de att_set y agrupa por la nueva posicion resultante -- varios bloques originales pueden fusionarse
        // en uno nuevo. Recursa al siguiente nivel.
        void build_projection_level(RankBackend * bv_levels, std::set<uint16_t> &att_set, std::vector<uint64_t>& old_total_ones,
                                    size_type &old_k_d, std::queue<std::vector<uint64_t>> block_pos, uint16_t level) {
            uint64_t bit_pos = 0, new_bit_pos = 0, one_pos = 0, pos_in_k_t;
            size_t block_pos_level = block_pos.size();
            sdsl::bit_vector k_t = sdsl::bit_vector(k_d * block_pos_level, 0);
            std::map<uint16_t, std::vector<uint64_t>> new_block_pos;

            for (size_t i = 0; i < block_pos_level; i++) {
                for (size_t j = 0; j < block_pos.front().size(); j++) {
                    uint64_t pos = block_pos.front()[j] * old_k_d;
                    one_pos = bv_levels[level].rank(pos);
                    for (size_t p = 0; p < old_k_d; p++) {
                        if (bv_levels[level].get_bit(pos)) {
                            bit_pos = pos % old_k_d;
                            new_bit_pos = gdj::util::removeBits(bit_pos, att_set);
                            pos_in_k_t = new_bit_pos + i * k_d;
                            k_t[pos_in_k_t] = 1;
                            new_block_pos[new_bit_pos].push_back(one_pos++);
                        }
                        pos++;
                    }
                }

                block_pos.pop();
                for (auto const& [key_bit_pos, new_block] : new_block_pos) {
                    if (!new_block.empty())
                        block_pos.push(new_block);
                }

                new_block_pos.clear();
            }

            bv[level] = RankBackend(k_t);

            // total_ones[level] = n_ones() del bitvector recien construido
            total_ones[level] = bv[level].n_ones();

            if (level < height - 1)
                build_projection_level(bv_levels, att_set, old_total_ones, old_k_d, block_pos, level + 1);
        }

    public:

        se_quadtree() = default;

        // Construye un quadtree a partir de una lista de aristas (puntos d-dimensionales) y el lado de la grilla.
        se_quadtree(std::vector<std::vector<gdj::idx_type>>& edges, const size_type grid_side, uint8_t _k, uint8_t _d) {
            assert(grid_side > 0);
            build_from_edges(edges, grid_side, _k, _d);
        }

        // Construye un quadtree a partir de un bit_vector denso (nivel 0) y los parametros del arbol.
        se_quadtree(const sdsl::bit_vector &single_bv, uint8_t _k, uint8_t _d, uint16_t _height) {
            k = _k;
            d = _d;
            height = _height;
            height = height > 1 ? height : 1; // si size == 0

            bv = new RankBackend[height];
            total_ones.reserve(height);

            k_d = std::pow(k, d);
            uint32_t ones = 1, curr_bits = 0,
                        level_bits = 0, global_pos = 0;

            for (size_t i = 0; i < height; i++) {
                level_bits = k_d * ones;
                ones = 0;
                curr_bits = 0;
                sdsl::bit_vector k_t = sdsl::bit_vector(level_bits, 0);
                while (level_bits > curr_bits) {
                    if (global_pos >= single_bv.size()) {
                        throw std::runtime_error("Error in se_quadtree constructor with single_bv");
                    }

                    if (single_bv[global_pos]) {
                        k_t[curr_bits] = 1;
                        ones++;
                    } else {
                        k_t[curr_bits] = 0;
                    }
                    curr_bits++;
                    global_pos++;
                }
                total_ones.push_back(ones);
                bv[i] = RankBackend(k_t);
                if (ones != bv[i].n_ones())
                    throw std::runtime_error("Error in se_quadtree constructor with single_bv");
            }
        }

        // Construye un quadtree a partir de otro quadtree, eliminando los atributos indicados en `attributes`.
        se_quadtree(se_quadtree * other, std::vector<uint16_t> &attributes, uint8_t _k, uint8_t _d, uint16_t _height)
        : k(_k), d(_d), height(_height), k_d(std::pow(_k, _d)) {
            size_type  k_d_original = other->getKD();
            uint16_t n_att = attributes.size();
            uint16_t d_original = n_att + d; // dimension del quadtree original
            bv = new RankBackend[height];
            total_ones.resize(height);
            std::vector<uint64_t> first_block_pos(1, 0);
            std::queue<std::vector<uint64_t>> rank_block_pos;
            rank_block_pos.push(first_block_pos);
            std::set<uint16_t> att_pos_removed;
            for (uint16_t removed_pos : attributes)
                att_pos_removed.insert(d_original - removed_pos - 1); // orden invertido

            size_t count_level = 0;
            RankBackend* original_bv = other->get_bv();

            build_projection_level(original_bv, att_pos_removed, other->get_total_ones(), k_d_original, rank_block_pos, 0);
        }
       
        // Construye un quadtree a partir de un vector de posiciones de unos por nivel.
        se_quadtree(const std::vector<uint64_t> bv_ones[], uint8_t _k, uint8_t _d, uint16_t _height) {
            k = _k;
            d = _d;
            height = _height;

            bv = new RankBackend[height];
            total_ones.reserve(height);

            k_d = std::pow(k, d);

            for (uint16_t i = 0; i < height; ++i) {
                const std::vector<uint64_t>& ones_positions = bv_ones[i];

                uint64_t n_blocks_level = 1;
                if (i > 0)
                    n_blocks_level = total_ones[i-1];


                uint64_t level_bits_size = k_d * n_blocks_level;

                sdsl::bit_vector k_t_level(level_bits_size, 0);

                for (uint64_t pos : ones_positions)
                    k_t_level[pos] = 1;


                bv[i] = RankBackend(k_t_level);
                total_ones.push_back(bv[i].n_ones());
            }
        }

        // Construye un quadtree a partir de un vector de palabras por nivel.
        se_quadtree(const uint64_t *const level_words[], const uint64_t level_bits[],
                    uint8_t _k, uint8_t _d, uint16_t _height) {
            k = _k;
            d = _d;
            height = _height;
            k_d = std::pow(k, d);

            bv = new RankBackend[height];
            total_ones.reserve(height);
            for (uint16_t i = 0; i < height; ++i) {
                bv[i] = RankBackend(level_words[i], level_bits[i]);
                total_ones.push_back(bv[i].n_ones());
            }
        }
        // Construye un quadtree a partir de un vector de palabras por nivel y un vector de posiciones de unos por nivel.
        se_quadtree(const se_quadtree& other) {
            k = other.k;
            d = other.d;
            height = other.height;
            k_d = other.k_d;
            total_ones = other.total_ones;

            if (other.bv != nullptr) {
                bv = new RankBackend[height];

                for (uint16_t i = 0; i < height; ++i) {
                    bv[i] = other.bv[i];
                }
            }
        }

        // Move constructor
        se_quadtree(se_quadtree&& other) noexcept {
            k = other.k;
            d = other.d;
            height = other.height;
            k_d = other.k_d;

            bv = other.bv;
            total_ones = std::move(other.total_ones);

            other.bv = nullptr;
        }


        se_quadtree& operator=(const se_quadtree& other) {
            if (this != &other) {
                delete[] bv;
                bv = nullptr;

                k = other.k;
                d = other.d;
                height = other.height;
                k_d = other.k_d;
                total_ones = other.total_ones;

                if (other.bv != nullptr) {
                    bv = new RankBackend[height];
                    for (uint16_t i = 0; i < height; ++i) {
                        bv[i] = other.bv[i];
                    }
                }
            }
            return *this;
        }

        se_quadtree& operator=(se_quadtree&& other) noexcept {
            if (this != &other) {
                delete[] bv;

                k = other.k;
                d = other.d;
                height = other.height;
                k_d = other.k_d;

                bv = other.bv;
                total_ones = std::move(other.total_ones);

                other.bv = nullptr;
            }
            return *this;
        }

        ~se_quadtree() {
            delete[] bv;
        }

        uint8_t getK() const {
            return k;
        }

        uint8_t getD() const  {
            return d;
        }

        inline RankBackend* get_bv() {
            return bv;
        }

        inline std::vector<uint64_t>& get_total_ones() {
            return total_ones;
        }

        inline uint64_t rank(uint16_t level, uint64_t node) {
            return bv[level].rank(node);
        }

        inline uint8_t get_node_last_level(uint16_t level, uint64_t node) {
            if (k_d == 4)
                return bv[level].get_4_bits(node);
            else
                return bv[level].get_2_bits(node);
        }

      
        inline uint8_t get_node(uint16_t level, uint64_t node, uint64_t* rank_array, uint64_t r) {
            uint8_t nd;
            if (k_d == 4) {
                nd = bv[level].get_4_bits(node);
                switch(nd) {
                    case 0:
                        break;
                    case 1:
                        rank_array[0] = r+1;
                        break;
                    case 2:
                         rank_array[1] = r+1;
                        break;
                    case 3:
                        rank_array[0] = r+1;
                        rank_array[1] = r+2;
                        break;
                    case 4:
                         rank_array[2] = r+1;
                        break;
                    case 5:
                        rank_array[0] = r+1;
                        rank_array[2] = r+2;
                        break;
                    case 6:
                        rank_array[1] = r+1;
                        rank_array[2] = r+2;
                        break;
                    case 7:
                        rank_array[0] = r+1;
                        rank_array[1] = r+2;
                        rank_array[2] = r+3;
                        break;
                    case 8:
                        rank_array[3] = r+1;
                        break;
                    case 9:
                        rank_array[0] = r+1;
                                    rank_array[3] = r+2;
                        break;
                    case 10:
                        rank_array[1] = r+1;
                        rank_array[3] = r+2;
                        break;
                    case 11:
                            rank_array[0] = r+1;
                        rank_array[1] = r+2;
                        rank_array[3] = r+3;
                        break;
                    case 12:
                        rank_array[2] = r+1;
                        rank_array[3] = r+2;
                        break;
                    case 13:
                        rank_array[0] = r+1;
                        rank_array[2] = r+2;
                        rank_array[3] = r+3;
                        break;
                    case 14:
                        rank_array[1] = r+1;
                        rank_array[2] = r+2;
                        rank_array[3] = r+3;
                        break;
                    case 15:
                        rank_array[0] = r+1;
                        rank_array[1] = r+2;
                        rank_array[2] = r+3;
                        rank_array[3] = r+4;
                        break;
                }
            }
            else {
                nd = bv[level].get_2_bits(node);
                switch(nd) {
                    case 0:
                         break;
                    case 1:
                        rank_array[0] = r+1;
                        break;
                    case 2:
                        rank_array[1] = r+1;
                        break;
                    case 3:
                        rank_array[0] = r+1;
                        rank_array[1] = r+2;
                        break;
                }
            }

            return nd;
        }

        // Igual que get_node, para k_d = 2/4/8/16/32/64 (hasta 6 atributos).
        inline uint64_t get_node_64(uint16_t level, uint64_t node, uint64_t* rank_array, uint64_t r) {
            uint64_t nd;
            auto calculate_rank = [&](uint64_t val, uint8_t k_dim) -> uint64_t {
                uint64_t current_rank = r;

                for (uint8_t i = 0; i < k_dim; ++i) {
                    if (val & (1ULL << i)) {
                        current_rank++;
                        rank_array[i] = current_rank;
                    }
                }
                return val;
            };

            switch(k_d) {
                case 2:
                    nd = bv[level].get_2_bits(node);
                    switch(nd) {
                        case 0:
                             break;
                        case 1:
                            rank_array[0] = r+1;
                            break;
                        case 2:
                            rank_array[1] = r+1;
                            break;
                        case 3:
                            rank_array[0] = r+1;
                            rank_array[1] = r+2;
                            break;
                    }
                    return nd;
                case 4:
                    nd = bv[level].get_4_bits(node);
                    switch(nd) {
                        case 0:
                            break;
                        case 1:
                            rank_array[0] = r+1;
                            break;
                        case 2:
                             rank_array[1] = r+1;
                            break;
                        case 3:
                            rank_array[0] = r+1;
                            rank_array[1] = r+2;
                            break;
                        case 4:
                            rank_array[2] = r+1;
                            break;
                        case 5:
                            rank_array[0] = r+1;
                            rank_array[2] = r+2;
                            break;
                        case 6:
                            rank_array[1] = r+1;
                            rank_array[2] = r+2;
                            break;
                        case 7:
                            rank_array[0] = r+1;
                            rank_array[1] = r+2;
                            rank_array[2] = r+3;
                            break;
                        case 8:
                            rank_array[3] = r+1;
                            break;
                        case 9:
                            rank_array[0] = r+1;
                                rank_array[3] = r+2;
                            break;
                        case 10:
                            rank_array[1] = r+1;
                            rank_array[3] = r+2;
                            break;
                                case 11:
                                    rank_array[0] = r+1;
                            rank_array[1] = r+2;
                            rank_array[3] = r+3;
                            break;
                        case 12:
                        rank_array[2] = r+1;
                            rank_array[3] = r+2;
                            break;
                        case 13:
                            rank_array[0] = r+1;
                            rank_array[2] = r+2;
                            rank_array[3] = r+3;
                            break;
                        case 14:
                            rank_array[1] = r+1;
                            rank_array[2] = r+2;
                            rank_array[3] = r+3;
                            break;
                        case 15:
                            rank_array[0] = r+1;
                            rank_array[1] = r+2;
                            rank_array[2] = r+3;
                            rank_array[3] = r+4;
                            break;
                    }
                    return nd;
                case 8:
                    nd = bv[level].get_8_bits(node);
                    return calculate_rank(nd, 8);
                case 16:
                    nd = bv[level].get_16_bits(node);
                    return calculate_rank(nd, 16);
                case 32:
                    nd = bv[level].get_32_bits(node);
                    return calculate_rank(nd, 32);
                case 64:
                    nd = bv[level].get_64_bits(node);
                    return calculate_rank(nd, 64);
                default:
                    throw std::runtime_error("Unsupported k_d value in get_node_");
            }
        }

        inline uint64_t get_node_64_last_level(uint16_t level, uint64_t node) {
            switch(k_d) {
                case 2:
                    return bv[level].get_2_bits(node);
                case 4:
                     return bv[level].get_4_bits(node);
                case 8:
                    return bv[level].get_8_bits(node);
                case 16:
                    return bv[level].get_16_bits(node);
                case 32:
                    return bv[level].get_32_bits(node);
                case 64:
                     return bv[level].get_64_bits(node);
                default:
                    throw std::runtime_error("Unsupported k_d value in get_node_");
            }
        }

        inline gdj::UInt128 get_node_128(uint16_t level, uint64_t node) {
            return bv[level].get_128_bits(node);
        }

        inline gdj::UInt256 get_node_256(uint16_t level, uint64_t node) {
            return bv[level].get_256_bits(node);
        }

        // Nodo de d=9 (512 bits).
        inline gdj::UInt512 get_node_512(uint16_t level, uint64_t node) {
            return bv[level].get_512_bits(node);
        }

        // para nodos de 128/256 bits(d=7/8).( para joins_wide.hpp)
        inline gdj::UInt128 get_node_128(uint16_t level, uint64_t node, uint64_t* rank_array, uint64_t r) {
            gdj::UInt128 val = bv[level].get_128_bits(node);
            uint64_t current_rank = r;
            for (uint8_t i = 0; i < 64; ++i) {
                if (val.lo & (1ULL << i)) {
                    current_rank++;
                    rank_array[i] = current_rank;
                }
            }

            for (uint8_t i = 0; i < 64; ++i) {
                if (val.hi & (1ULL << i)) {
                    current_rank++;
                    rank_array[64 + i] = current_rank;
                }
            }
            return val;
        }

        inline gdj::UInt256 get_node_256(uint16_t level, uint64_t node, uint64_t* rank_array, uint64_t r) {
            gdj::UInt256 val = bv[level].get_256_bits(node);
            uint64_t current_rank = r;
            uint64_t words[4] = { val.v0, val.v1, val.v2, val.v3 };
            for (uint8_t w = 0; w < 4; ++w) {
                for (uint8_t i = 0; i < 64; ++i) {
                    if (words[w] & (1ULL << i)) {
                        current_rank++;
                        rank_array[w * 64 + i] = current_rank;
                    }
                }
            }
            return val;
        }

        // Lista (no rank) de los hijos presentes, para k_d 2 o 4.
        void get_children(uint16_t level, uint64_t node, uint64_t* children_array, uint64_t &n_children) {
            if (k_d == 4) {
                switch(bv[level].get_4_bits(node)) {
                case 0:
                    n_children = 0;
                    break;
                case 1:
                    n_children = 1;
                        children_array[0] = 0;
                    break;
                case 2:
                    n_children = 1;
                        children_array[0] = 1;
                    break;
                case 3:
                    n_children = 2;
                        children_array[0] = 0;
                        children_array[1] = 1;
                    break;
                case 4:
                    n_children = 1;
                        children_array[0] = 2;
                    break;
                case 5:
                    n_children = 2;
                        children_array[0] = 0;
                    children_array[1] = 2;
                    break;
                case 6:
                    n_children = 2;
                        children_array[0] = 1;
                    children_array[1] = 2;
                    break;
                case 7:
                    n_children = 3;
                        children_array[0] = 0;
                    children_array[1] = 1;
                    children_array[2] = 2;
                    break;
                case 8:
                    n_children = 1;
                        children_array[0] = 3;
                    break;
                case 9:
                    n_children = 2;
                    children_array[0] = 0;
                    children_array[1] = 3;
                    break;
                case 10:
                    n_children = 2;
                    children_array[0] = 1;
                    children_array[1] = 3;
                    break;
                case 11:
                        n_children = 3;
                        children_array[0] = 0;
                        children_array[1] = 1;
                        children_array[2] = 3;
                        break;
                case 12:
                    n_children = 2;
                        children_array[0] = 2;
                    children_array[1] = 3;
                    break;
                case 13:
                    n_children = 3;
                    children_array[0] = 0;
                    children_array[1] = 2;
                    children_array[2] = 3;
                    break;
                case 14:
                    n_children = 3;
                        children_array[0] = 1;
                    children_array[1] = 2;
                    children_array[2] = 3;
                    break;
                case 15:
                    n_children = 4;
                        children_array[0] = 0;
                    children_array[1] = 1;
                    children_array[2] = 2;
                    children_array[3] = 3;
                    break;
                }
            } else if (k_d == 2) {
                switch(bv[level].get_2_bits(node)) {
                    case 0:
                        n_children = 0;
                            break;
                    case 1:
                        n_children = 1;
                        children_array[0] = 0;
                        break;
                    case 2:
                        n_children = 1;
                            children_array[0] = 1;
                            break;
                    case 3:
                        n_children = 2;
                        children_array[0] = 0;
                        children_array[1] = 1;
                        break;
                }
            }
        }

        uint64_t total_ones_level(uint16_t level) {
            return total_ones[level];
        }

        uint64_t total_ones_last_level() {
            return total_ones[height-1];
        }

        uint64_t size_level(uint16_t level) {
            if (level >= height) {
                throw std::out_of_range("Level out of range in size_level");
            }
            return bv[level].size();
        }

        size_type getKD() {
            return k_d;
        }


        uint16_t getHeight() {
            return height;
        }


        uint64_t size_in_bytes(){
            uint64_t i, s = 0;
            for (i = 0; i < height; i++)
                s += bv[i].size_in_bytes();

            return s + total_ones.size() * sizeof(uint64_t);
        }

        // size_in_bytes() desglosado por nivel.
        std::vector<uint64_t> size_in_bytes_per_level() {
            std::vector<uint64_t> sizes(height);
            for (uint64_t i = 0; i < height; i++)
                sizes[i] = bv[i].size_in_bytes();
            return sizes;
        }

        void print(std::ofstream &ost) const {
            uint64_t i, j;

            for (i = 0; i < height; ++i) {
                ost << "Level " << i << ": ";

                if (bv[i].size() > 0) {
                    uint64_t current_bit_idx = 0;
                    for (j = 0; j < bv[i].size(); ++j) {
                        bool bit_val = bv[i].get_bit(j);

                        ost << (bit_val ? '1' : '0');
                        if ((j + 1) % k_d == 0 && (j + 1) < bv[i].size())
                            ost << " ";
                    }
                } else
                    ost << "(empty)";

                ost << std::endl;
            }
        }

        // Concatena todos los niveles en un unico bit_vector
        sdsl::bit_vector getSingleBitvector()  const {
            sdsl::bit_vector single_bv;
            size_t total_size = 0;

            for (uint16_t i = 0; i < height; ++i) {
                total_size += bv[i].size();
            }

            single_bv.resize(total_size);

            size_t current_pos = 0;
            for (uint16_t i = 0; i < height; ++i) {
                for (size_t j = 0; j < bv[i].size(); ++j) {
                    single_bv[current_pos++] = bv[i].get_bit(j);
                }
            }

            return single_bv;
        }

        void serialize(std::ostream& out) const {
            out.write(reinterpret_cast<const char*>(&height), sizeof(height));
            out.write(reinterpret_cast<const char*>(&k), sizeof(k));
            out.write(reinterpret_cast<const char*>(&d), sizeof(d));
            out.write(reinterpret_cast<const char*>(&k_d), sizeof(k_d));

            if (height > 0 && bv != nullptr) {
                for (uint16_t i = 0; i < height; ++i) {
                    bv[i].serialize(out);
                }
            }

            size_t total_ones_size = total_ones.size();
            out.write(reinterpret_cast<const char*>(&total_ones_size), sizeof(total_ones_size));
            if (total_ones_size > 0) {
                out.write(reinterpret_cast<const char*>(total_ones.data()), total_ones_size * sizeof(uint64_t));
            }
        }

        bool load(std::ifstream& in) {
            in.read(reinterpret_cast<char*>(&height), sizeof(height));
            in.read(reinterpret_cast<char*>(&k), sizeof(k));
            in.read(reinterpret_cast<char*>(&d), sizeof(d));
            in.read(reinterpret_cast<char*>(&k_d), sizeof(k_d));

            if (in.fail())
                return false;

            if (height > 0) {
                bv = new RankBackend[height];
                for (uint16_t i = 0; i < height; ++i) {
                    bv[i].load(in);
                    if (in.fail())
                        return false;
                }
            } else {
                bv = nullptr;
            }

            size_t total_ones_size;
            in.read(reinterpret_cast<char*>(&total_ones_size), sizeof(total_ones_size));

            if (in.fail())
                return false;

            total_ones.resize(total_ones_size);
            if (total_ones_size > 0) {
                in.read(reinterpret_cast<char*>(total_ones.data()), total_ones_size * sizeof(uint64_t));
                if (in.fail())
                    return false;
            }

            return true;
        }

        // Reconstruye todas las aristas/puntos d-dimensionales via BFS.
        std::vector<std::vector<gdj::idx_type>> get_edges() const {
            std::vector<std::vector<gdj::idx_type>> edges;

            if (height == 0 || bv == nullptr || k_d == 0) {
                return edges;
            }

            std::queue<gdj::EdgeTraversalState> q_traversal;

            std::vector<gdj::idx_type> initial_coords(d, 0);

            if (bv[0].size() > 0) {
                 q_traversal.push(gdj::EdgeTraversalState(0, 0, initial_coords));
            }

            while (!q_traversal.empty()) {
                gdj::EdgeTraversalState current_state = q_traversal.front();
                q_traversal.pop();

                uint16_t current_level = current_state.level;
                uint64_t node_idx_in_bv = current_state.node_idx_in_bv;
                std::vector<gdj::idx_type> base_coords = current_state.coords;

                uint64_t node_start_bit_in_current_level = node_idx_in_bv * k_d;

                for (uint32_t cell_offset = 0; cell_offset < k_d; ++cell_offset) {
                    uint64_t global_bit_idx = node_start_bit_in_current_level + cell_offset;
                    if (bv[current_level].get_bit(global_bit_idx)) {
                        std::vector<gdj::idx_type> new_coords = gdj::util::appendChildBitsToCoords(base_coords, cell_offset);
                        if (current_level == height - 1) {
                            edges.push_back(new_coords);
                        } else {
                            uint64_t new_node_idx_in_next_level = bv[current_level].rank(global_bit_idx); // rank es 1-based
                            q_traversal.push(gdj::EdgeTraversalState(current_level + 1, new_node_idx_in_next_level, new_coords));
                        }
                    }
                }
            }
            return edges;
        }
    };

}

#endif
