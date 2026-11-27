#ifndef QDAG_HPP
#define QDAG_HPP

#include <sdsl/bit_vectors.hpp>
#include "commun.hpp"
#include "wide_base_config.hpp"
#include "se_quadtree.hpp"
#include "../detail/bits.hpp"

#include <vector>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <tuple>

// qdag<RankBackend>
namespace gdj {

    template<class RankBackend>
    class qdag {
        private:
            se_quadtree<RankBackend>* Q;

            type_mapping_M* M; // mapeo bit de salida

            att_set q_att_set;

            uint64_t grid_side;

            uint16_t Msize; // bits por nodo en el espacio extendido

            std::vector<std::vector<uint64_t>> tab_extend_64;   // hasta 6 atributos
            std::vector<std::vector<gdj::UInt256>> tab_extend_256; // 7 u 8 atributos
            std::vector<std::vector<gdj::UInt512>> tab_extend_512;   // 9 atributos
            std::vector<std::vector<gdj::UInt1024>> tab_extend_1024; // 10 atributos

            // Tablas de dispersion por bit: scatter_*[b] = mascara de las posiciones de salida j con M[j]==b.
            // (reemplaza tab_extend_*; pesa 2^d * W/8 bytes).
            std::vector<gdj::UInt256>  scatter_256;
            std::vector<gdj::UInt512>  scatter_512;
            std::vector<gdj::UInt1024> scatter_1024;
            bool use_scatter_ = false;   // extiende por dispersion
            uint8_t base_words_ = 0;     // palabras de 64 bits de un nodo base (d <= 9)

            void free_resources() {
                if (M != nullptr) {
                    delete[] M;
                    M = nullptr;
                }
            }

            void copy_from(const qdag& other) {
                q_att_set = other.q_att_set;
                grid_side = other.grid_side;
                Msize = other.Msize;

                if (other.M != nullptr) {
                    M = new type_mapping_M[Msize];
                    std::copy(other.M, other.M + Msize, M);
                } else {
                    M = nullptr;
                }

                scatter_256 = other.scatter_256;
                scatter_512 = other.scatter_512;
                scatter_1024 = other.scatter_1024;
                use_scatter_ = other.use_scatter_;
                base_words_ = other.base_words_;

                Q = other.Q;
            }

            void move_from(qdag&& other) noexcept {
                q_att_set = std::move(other.q_att_set);
                grid_side = other.grid_side;
                Msize = other.Msize;

                M = other.M;
                other.M = nullptr;

                scatter_256 = std::move(other.scatter_256);
                scatter_512 = std::move(other.scatter_512);
                scatter_1024 = std::move(other.scatter_1024);
                use_scatter_ = other.use_scatter_;
                base_words_ = other.base_words_;

                Q = other.Q;
                other.Q = nullptr;
            }

            // Tabla de extension para nodos de hasta 64 bits de salida (dim<=6).
            void createTableExtendV64() {
                uint64_t i, j;
                uint64_t x, B = 16;
                uint8_t part = 0;
                uint8_t _k_d = Q->getKD();
                switch (_k_d) {
                    case 2:
                        B = 4;
                        break;
                    case 4:
                        B = 16;
                        break;
                    case 8:
                        B = 256;
                        break;
                    case 16:
                        B = 65536;
                        break;
                    case 32:
                        B = 65536;
                        part = 2;
                        break;
                    case 64:
                        B = 65536;
                        part = 4;
                        break;
                    default:
                        break;
                }

                uint8_t shift_left = 64 - Msize;
                if (part == 0) {
                    std::vector<uint64_t> tab_extend_64_1;
                    for (i = 0; i < B; i++) {
                        x = 0;
                        for(j = 0; j < Msize; j++)
                            if (i & (1 << M[j]))
                                x = (x << 1) | 1;
                            else
                                x = (x << 1);

                        tab_extend_64_1.push_back((x << shift_left));
                    }
                    tab_extend_64.push_back(tab_extend_64_1);
                }
                else {  // part == 2 (dim_prime=5) o part == 4 (dim_prime=6)
                    shift_left = 64 - Msize;
                    std::vector<std::vector<uint64_t>> parts(part);
                    for (uint8_t pk = 0; pk < part; pk++) {
                        std::vector<uint64_t> tab;
                        tab.reserve(B);
                        for (i = 0; i < B; i++) {
                            x = 0;
                            for (j = 0; j < Msize; j++) {
                                x <<= 1;
                                uint16_t base_bit = M[j];
                                if (base_bit / 16 == pk && (i & (1ULL << (base_bit % 16))))
                                    x |= 1;
                            }
                            tab.push_back(x << shift_left);
                        }
                        parts[pk] = std::move(tab);
                    }
                    for (uint8_t pk = 0; pk < part; pk++)
                        tab_extend_64.push_back(std::move(parts[pk]));
                }
            }

            // para dim=7 u 8 (Msize=128/256)
            void createTableExtendV256(){
                uint64_t i, j;
                uint64_t B = 16;
                uint8_t part = 0;
                uint16_t _k_d = Q->getKD();
                switch (_k_d) {
                    case 2:
                        B = 4;
                        break;
                    case 4:
                        B = 16;
                        break;
                    case 8:
                        B = 256;
                        break;
                    case 16:
                        B = 65536;
                        break;
                    case 32:
                        B = 65536;
                        part = 2;
                        break;
                    case 64:
                        B = 65536;
                        part = 4;
                        break;
                    case 128:
                        B = 65536;
                        part = 8;
                        break;
                    case 256:
                        B = 65536;
                        part = 16;
                        break;
                    default:
                        break;
                }

                static const gdj::UInt256 ONE(0, 0, 0, 1); // bit 0 prendido

                if (part == 0) {
                    uint16_t shift_left = 256 - Msize;
                    std::vector<gdj::UInt256> tab;
                    for (i = 0; i < B; i++) {
                        gdj::UInt256 x;
                        for (j = 0; j < Msize; j++) {
                            x = x << 1;
                            if (i & (1ULL << M[j]))
                                x = x | ONE;
                        }
                        tab.push_back(x << shift_left);
                    }
                    tab_extend_256.push_back(tab);
                } else {
                    uint16_t shift_left = 256 - Msize;
                    std::vector<std::vector<gdj::UInt256>> parts(part);
                    for (uint8_t pk = 0; pk < part; pk++) {
                        std::vector<gdj::UInt256> tab;
                        tab.reserve(B);
                        for (i = 0; i < B; i++) {
                            gdj::UInt256 x;
                            for (j = 0; j < Msize; j++) {
                                x = x << 1;
                                uint16_t base_bit = M[j];
                                if (base_bit / 16 == pk && (i & (1ULL << (base_bit % 16))))
                                    x = x | ONE;
                            }
                            tab.push_back(x << shift_left);
                        }
                        parts[pk] = std::move(tab);
                    }
                    for (uint8_t pk = 0; pk < part; pk++)
                        tab_extend_256.push_back(std::move(parts[pk]));
                }
            }

            // para dim=9 (Msize=512).
            void createTableExtendV512(){
                uint64_t i, j;
                uint64_t B = 16;
                uint8_t part = 0;
                uint16_t _k_d = Q->getKD();
                switch (_k_d) {
                    case 2:
                        B = 4;
                        break;
                    case 4:
                        B = 16;
                        break;
                    case 8:
                        B = 256;
                        break;
                    case 16:
                        B = 65536;
                        break;
                    case 32:
                        B = 65536;
                        part = 2;
                        break;
                    case 64:
                        B = 65536;
                        part = 4;
                        break;
                    case 128:
                        B = 65536;
                        part = 8;
                        break;
                    case 256:
                        B = 65536;
                        part = 16;
                        break;
                    default:
                        break;
                }

                gdj::UInt512 ONE; ONE.w[0] = 1; // bit 0 prendido

                if (part == 0) {
                    uint16_t shift_left = 512 - Msize;
                    std::vector<gdj::UInt512> tab;
                    for (i = 0; i < B; i++) {
                        gdj::UInt512 x;
                        for (j = 0; j < Msize; j++) {
                            x = x << 1;
                            if (i & (1ULL << M[j]))
                                x = x | ONE;
                        }
                        tab.push_back(x << shift_left);
                    }
                    tab_extend_512.push_back(tab);
                } else {
                    uint16_t shift_left = 512 - Msize;
                    std::vector<std::vector<gdj::UInt512>> parts(part);
                    for (uint8_t pk = 0; pk < part; pk++) {
                        std::vector<gdj::UInt512> tab;
                        tab.reserve(B);
                        for (i = 0; i < B; i++) {
                            gdj::UInt512 x;
                            for (j = 0; j < Msize; j++) {
                                x = x << 1;
                                uint16_t base_bit = M[j];
                                if (base_bit / 16 == pk && (i & (1ULL << (base_bit % 16))))
                                    x = x | ONE;
                            }
                            tab.push_back(x << shift_left);
                        }
                        parts[pk] = std::move(tab);
                    }
                    for (uint8_t pk = 0; pk < part; pk++)
                        tab_extend_512.push_back(std::move(parts[pk]));
                }
            }

            // para 10 atributos (Msize=1024)
            void createTableExtendV1024(){
                uint64_t i, j;
                uint64_t B = 16;
                uint8_t part = 0;
                uint16_t _k_d = Q->getKD();
                switch (_k_d) {
                    case 2:
                        B = 4;
                        break;
                    case 4:
                        B = 16;
                        break;
                    case 8:
                        B = 256;
                        break;
                    case 16:
                        B = 65536;
                        break;
                    case 32:
                        B = 65536;
                        part = 2;
                        break;
                    case 64:
                        B = 65536;
                        part = 4;
                        break;
                    case 128:
                        B = 65536;
                        part = 8;
                        break;
                    case 256:
                        B = 65536;
                        part = 16;
                        break;
                    default:
                        break;
                }

                gdj::UInt1024 ONE; ONE.w[0] = 1; // bit 0 prendido

                if (part == 0) {
                    uint16_t shift_left = 1024 - Msize;
                    std::vector<gdj::UInt1024> tab;
                    for (i = 0; i < B; i++) {
                        gdj::UInt1024 x;
                        for (j = 0; j < Msize; j++) {
                            x = x << 1;
                            if (i & (1ULL << M[j]))
                                x = x | ONE;
                        }
                        tab.push_back(x << shift_left);
                    }
                    tab_extend_1024.push_back(tab);
                } else {
                    uint16_t shift_left = 1024 - Msize;
                    std::vector<std::vector<gdj::UInt1024>> parts(part);
                    for (uint8_t pk = 0; pk < part; pk++) {
                        std::vector<gdj::UInt1024> tab;
                        tab.reserve(B);
                        for (i = 0; i < B; i++) {
                            gdj::UInt1024 x;
                            for (j = 0; j < Msize; j++) {
                                x = x << 1;
                                uint16_t base_bit = M[j];
                                if (base_bit / 16 == pk && (i & (1ULL << (base_bit % 16))))
                                    x = x | ONE;
                            }
                            tab.push_back(x << shift_left);
                        }
                        parts[pk] = std::move(tab);
                    }
                    for (uint8_t pk = 0; pk < part; pk++)
                        tab_extend_1024.push_back(std::move(parts[pk]));
                }
            }

            const std::vector<gdj::UInt256>&  scatter_tab_(const gdj::UInt256*) const {
                return scatter_256;
            }
            const std::vector<gdj::UInt512>&  scatter_tab_(const gdj::UInt512*) const {
                return scatter_512;
            }
            const std::vector<gdj::UInt1024>& scatter_tab_(const gdj::UInt1024*) const {
                return scatter_1024;
            }

            // tabla de dispersion de tipo U (W bits de ancho).
            template<class U>
            void build_scatter_(std::vector<U>& table) {
                const uint64_t W = width_of(static_cast<const U*>(nullptr));
                if (Msize > W)
                    throw std::runtime_error("qdag::build_scatter_(): extended node does not fit the wide type.");
                table.assign(Q->getKD(), U());
                for (uint64_t j = 0; j < Msize; ++j)
                    set_bit(table[M[j]], (W - 1) - j);
            }

            // OR de table[b] sobre los bits prendidos del nodo base v
            // (b = 64*palabra + posicion del bit).
            template<class U>
            inline U scatter_extend_(const std::vector<U>& table, const uint64_t* v) const {
                U out;
                for (unsigned wi = 0; wi < base_words_; ++wi) {
                    uint64_t x = v[wi];
                    while (x) {
                        const unsigned b = (unsigned)__builtin_ctzll(x);
                        x &= x - 1;
                        or_into(out, table[(size_t)wi * 64 + b]);
                    }
                }
                return out;
            }

            // nodo base (d <= 9) en palabras de 64 bits.
            inline void read_base_words_(uint64_t level, uint64_t node, uint64_t* v) {
                switch (Q->getD()) {
                    case 1:
                    case 2:
                        v[0] = Q->get_node_last_level(level, node);
                        break;
                    case 3:
                    case 4:
                    case 5:
                    case 6:
                        v[0] = Q->get_node_64_last_level(level, node);
                        break;
                    case 7: {
                        gdj::UInt128 x = Q->get_node_128(level, node);
                        v[0] = x.lo;
                        v[1] = x.hi;
                        break;
                    }
                    case 8: {
                        gdj::UInt256 x = Q->get_node_256(level, node);
                        v[0] = x.v0;
                        v[1] = x.v1;
                        v[2] = x.v2;
                        v[3] = x.v3;
                        break;
                    }
                    case 9: {
                        gdj::UInt512 x = Q->get_node_512(level, node);
                        for (int i = 0; i < 8; ++i)
                            v[i] = x.w[i];
                        break;
                    }
                    default:
                        throw std::runtime_error("qdag::read_base_words_(): d outside [1,9].");
                }
            }

        public:
            qdag() : Q(nullptr), M(nullptr), grid_side(0), Msize(0) {}

            qdag(se_quadtree<RankBackend>* _Q_ptr, att_set &_Q_att_set_base,
                const uint64_t _Q_grid_side_base, att_set _q_att_set_for_qdag,
                const wide_base_config& wcfg = wide_base_config())
                : Q(nullptr), M(nullptr) {
                Q = _Q_ptr;

                q_att_set = _q_att_set_for_qdag;
                grid_side = _Q_grid_side_base;

                uint16_t dim = q_att_set.size();        // tamano del nuevo att_set
                uint16_t dim_prime = Q->getD();         // dimension del Q base

                uint8_t Q_k_base = Q->getK();
                Msize = std::pow(Q_k_base, dim);
                M = new type_mapping_M[Msize];

                uint64_t mask;
                uint64_t i_prime;

                for (uint64_t i_new_m = 0; i_new_m < Msize; ++i_new_m) {
                    mask = 1ULL << (dim_prime - 1);
                    i_prime = 0;

                    for (uint16_t j = 0; j < dim_prime; ++j) {
                        if (i_new_m & (1ULL << (dim - _Q_att_set_base[j] - 1))) {
                            i_prime |= mask;
                        }
                        mask >>= 1;
                    }

                    M[i_new_m] = i_prime;
                }

                base_words_ = (uint8_t)((Q->getKD() + 63) / 64);

                // Dispersion o tablas segun wcfg (ver wide_base_config.hpp). Los joins de dim <= 6 usan el motor
                // denso y nunca dispersion.
                use_scatter_ = wide_base_uses_scatter(dim_prime, dim, wcfg);

                if (use_scatter_) {
                    if (dim <= 8)
                        build_scatter_(scatter_256);
                    else if (dim == 9)
                        build_scatter_(scatter_512);
                    else
                        build_scatter_(scatter_1024);
                } else if(dim_prime <= 6 && dim <= 6 && dim >= dim_prime) {
                    createTableExtendV64();
                }  else if(dim_prime <= 8 && dim <= 8 && dim >= dim_prime) {
                    createTableExtendV256();
                } else if(dim_prime <= 8 && dim == 9 && dim >= dim_prime) {
                    createTableExtendV512();
                } else if(dim_prime <= 8 && dim == 10 && dim >= dim_prime) {
                    createTableExtendV1024();
                }
            }

            qdag(const qdag& other) : Q(nullptr), M(nullptr) {
                copy_from(other);
            }

            qdag& operator=(const qdag& other) {
                if (this != &other) {
                    free_resources();
                    copy_from(other);
                }
                return *this;
            }

            qdag(qdag&& other) noexcept : Q(nullptr), M(nullptr) {
                move_from(std::move(other));
            }

            qdag& operator=(qdag&& other) noexcept {
                if (this != &other) {
                    free_resources();
                    move_from(std::move(other));
                }
                return *this;
            }

            ~qdag() {
                free_resources();
            }

            uint64_t size_in_bytes() const {
                uint64_t s = 0;
                if (Q != nullptr) {
                    s += Q->size_in_bytes();
                }
                if (M != nullptr) {
                    s += Msize * sizeof(type_mapping_M);
                }
                s += q_att_set.size() * sizeof(uint64_t);
                s += sizeof(grid_side) + sizeof(Msize) ;
                return s;
            }

            uint64_t nAttr() const {
                return q_att_set.size();
            }
            uint64_t getAttr(uint64_t i) const {
                if (i >= q_att_set.size()) {
                    throw std::out_of_range("qdag::getAttr(): Index out of bounds for q_att_set.");
                }
                return q_att_set[i];
            }
            uint64_t getGridSide() const {
                return grid_side;
            }
            uint64_t getHeight() const {
                return Q->getHeight();
            }
            uint8_t getK() const {
                return Q->getK();
            }
            uint16_t nChildren() const {
                return Msize;
             }
            uint64_t getKD() const {
                return Q->getKD();
             }
            uint8_t getBaseD() const {
                return Q->getD();
            }
            bool usesScatter() const {
                return use_scatter_;
            }
            uint16_t getM(uint16_t i) const {
                if (i >= Msize) {
                    throw std::out_of_range("qdag::getM(): Index out of bounds for M.");
                }
                return M[i];
            }

            se_quadtree<RankBackend>* getQ() const {
                if (Q == nullptr) {
                    throw std::runtime_error("qdag::getQ(): Q is not initialized.");
                }
                return Q;
            }

            const std::vector<uint64_t> getTabExtend_64(uint8_t part) const {
                return tab_extend_64[part];
            }


            inline uint64_t materialize_node_64(uint64_t level, uint64_t node, uint64_t* rank_vector) {
                uint16_t chunk1, chunk2, chunk3, chunk4;
                uint64_t r = Q->rank(level, node);
                switch (Q->getD()) {
                    case 1:
                    case 2:
                        return tab_extend_64[0][Q->get_node(level, node, rank_vector, r)];
                    case 3:
                    case 4:
                        return tab_extend_64[0][Q->get_node_64(level, node, rank_vector, r)];
                    case 5:
                        gdj::util::get16BitChunks(Q->get_node_64(level, node, rank_vector, r), chunk1, chunk2, chunk3, chunk4);
                        return tab_extend_64[0][chunk1] | tab_extend_64[1][chunk2];
                    case 6:
                        gdj::util::get16BitChunks(Q->get_node_64(level, node, rank_vector, r), chunk1, chunk2, chunk3, chunk4);
                        return tab_extend_64[0][chunk1] | tab_extend_64[1][chunk2] | tab_extend_64[2][chunk3] | tab_extend_64[3][chunk4];
                    default:
                        throw std::runtime_error("qdag::materialize_node(): Unsupported number of attributes for materialization.");
                }
            }

            inline uint64_t materialize_node_64_last(uint64_t level, uint64_t node) {
                uint16_t chunk1, chunk2, chunk3, chunk4;
                uint64_t r = Q->rank(level, node);
                switch (Q->getD()) {
                    case 1:
                    case 2:
                        return tab_extend_64[0][Q->get_node_last_level(level, node)];
                    case 3:
                    case 4:
                        return tab_extend_64[0][Q->get_node_64_last_level(level, node)];
                    case 5:
                        gdj::util::get16BitChunks(Q->get_node_64_last_level(level, node), chunk1, chunk2, chunk3, chunk4);
                        return tab_extend_64[0][chunk1] | tab_extend_64[1][chunk2];
                    case 6:
                        gdj::util::get16BitChunks(Q->get_node_64_last_level(level, node), chunk1, chunk2, chunk3, chunk4);
                        return tab_extend_64[0][chunk1] | tab_extend_64[1][chunk2] | tab_extend_64[2][chunk3] | tab_extend_64[3][chunk4];
                    default:
                        throw std::runtime_error("qdag::materialize_node(): Unsupported number of attributes for materialization.");
                }
            }

            inline gdj::UInt256 materialize_node_256(uint64_t level, uint64_t node, uint64_t* rank_vector) {
                uint16_t c[16];
                uint64_t r = Q->rank(level, node);
                switch (Q->getD()) {
                    case 1:
                    case 2:
                        return tab_extend_256[0][Q->get_node(level, node, rank_vector, r)];
                    case 3:
                    case 4:
                        return tab_extend_256[0][Q->get_node_64(level, node, rank_vector, r)];
                    case 5:
                        gdj::util::get16BitChunks(Q->get_node_64(level, node, rank_vector, r), c[0], c[1], c[2], c[3]);
                        return tab_extend_256[0][c[0]] | tab_extend_256[1][c[1]];
                    case 6:
                        gdj::util::get16BitChunks(Q->get_node_64(level, node, rank_vector, r), c[0], c[1], c[2], c[3]);
                        return tab_extend_256[0][c[0]] | tab_extend_256[1][c[1]] | tab_extend_256[2][c[2]] | tab_extend_256[3][c[3]];
                    case 7: {
                        gdj::UInt128 v = Q->get_node_128(level, node, rank_vector, r);
                        gdj::util::get16BitChunks(v.lo, c[0], c[1], c[2], c[3]);
                        gdj::util::get16BitChunks(v.hi, c[4], c[5], c[6], c[7]);
                        gdj::UInt256 out = tab_extend_256[0][c[0]];
                        for (int k = 1; k < 8; ++k)
                            out = out | tab_extend_256[k][c[k]];
                        return out;
                    }
                    case 8: {
                        gdj::UInt256 v = Q->get_node_256(level, node, rank_vector, r);
                        gdj::util::get16BitChunks(v.v0, c[0], c[1], c[2], c[3]);
                        gdj::util::get16BitChunks(v.v1, c[4], c[5], c[6], c[7]);
                        gdj::util::get16BitChunks(v.v2, c[8], c[9], c[10], c[11]);
                        gdj::util::get16BitChunks(v.v3, c[12], c[13], c[14], c[15]);
                        gdj::UInt256 out = tab_extend_256[0][c[0]];
                        for (int k = 1; k < 16; ++k)
                            out = out | tab_extend_256[k][c[k]];
                        return out;
                    }
                    default:
                        throw std::runtime_error("qdag::materialize_node_256(): Unsupported number of attributes for materialization.");
                }
            }

            inline gdj::UInt256 materialize_node_256_last(uint64_t level, uint64_t node) {
                uint16_t c[16];
                switch (Q->getD()) {
                    case 1:
                    case 2:
                        return tab_extend_256[0][Q->get_node_last_level(level, node)];
                    case 3:
                    case 4:
                        return tab_extend_256[0][Q->get_node_64_last_level(level, node)];
                    case 5:
                        gdj::util::get16BitChunks(Q->get_node_64_last_level(level, node), c[0], c[1], c[2], c[3]);
                        return tab_extend_256[0][c[0]] | tab_extend_256[1][c[1]];
                    case 6:
                        gdj::util::get16BitChunks(Q->get_node_64_last_level(level, node), c[0], c[1], c[2], c[3]);
                        return tab_extend_256[0][c[0]] | tab_extend_256[1][c[1]] | tab_extend_256[2][c[2]] | tab_extend_256[3][c[3]];
                    case 7: {
                        gdj::UInt128 v = Q->get_node_128(level, node);
                        gdj::util::get16BitChunks(v.lo, c[0], c[1], c[2], c[3]);
                        gdj::util::get16BitChunks(v.hi, c[4], c[5], c[6], c[7]);
                        gdj::UInt256 out = tab_extend_256[0][c[0]];
                        for (int k = 1; k < 8; ++k)
                            out = out | tab_extend_256[k][c[k]];
                        return out;
                    }
                    case 8: {
                        gdj::UInt256 v = Q->get_node_256(level, node);
                        gdj::util::get16BitChunks(v.v0, c[0], c[1], c[2], c[3]);
                        gdj::util::get16BitChunks(v.v1, c[4], c[5], c[6], c[7]);
                        gdj::util::get16BitChunks(v.v2, c[8], c[9], c[10], c[11]);
                        gdj::util::get16BitChunks(v.v3, c[12], c[13], c[14], c[15]);
                        gdj::UInt256 out = tab_extend_256[0][c[0]];
                        for (int k = 1; k < 16; ++k)
                            out = out | tab_extend_256[k][c[k]];
                        return out;
                    }
                    default:
                        throw std::runtime_error("qdag::materialize_node_256_last(): Unsupported number of attributes for materialization.");
                }
            }

            inline gdj::UInt512 materialize_node_512(uint64_t level, uint64_t node, uint64_t* rank_vector) {
                uint16_t c[16];
                uint64_t r = Q->rank(level, node);
                switch (Q->getD()) {
                    case 1:
                    case 2:
                        return tab_extend_512[0][Q->get_node(level, node, rank_vector, r)];
                    case 3:
                    case 4:
                        return tab_extend_512[0][Q->get_node_64(level, node, rank_vector, r)];
                    case 5:
                        gdj::util::get16BitChunks(Q->get_node_64(level, node, rank_vector, r), c[0], c[1], c[2], c[3]);
                        return tab_extend_512[0][c[0]] | tab_extend_512[1][c[1]];
                    case 6:
                        gdj::util::get16BitChunks(Q->get_node_64(level, node, rank_vector, r), c[0], c[1], c[2], c[3]);
                        return tab_extend_512[0][c[0]] | tab_extend_512[1][c[1]] | tab_extend_512[2][c[2]] | tab_extend_512[3][c[3]];
                    case 7: {
                        gdj::UInt128 v = Q->get_node_128(level, node, rank_vector, r);
                        gdj::util::get16BitChunks(v.lo, c[0], c[1], c[2], c[3]);
                        gdj::util::get16BitChunks(v.hi, c[4], c[5], c[6], c[7]);
                        gdj::UInt512 out = tab_extend_512[0][c[0]];
                        for (int k = 1; k < 8; ++k)
                            out = out | tab_extend_512[k][c[k]];
                        return out;
                    }
                    case 8: {
                        gdj::UInt256 v = Q->get_node_256(level, node, rank_vector, r);
                        gdj::util::get16BitChunks(v.v0, c[0], c[1], c[2], c[3]);
                        gdj::util::get16BitChunks(v.v1, c[4], c[5], c[6], c[7]);
                        gdj::util::get16BitChunks(v.v2, c[8], c[9], c[10], c[11]);
                        gdj::util::get16BitChunks(v.v3, c[12], c[13], c[14], c[15]);
                        gdj::UInt512 out = tab_extend_512[0][c[0]];
                        for (int k = 1; k < 16; ++k)
                            out = out | tab_extend_512[k][c[k]];
                        return out;
                    }
                    default:
                        throw std::runtime_error("qdag::materialize_node_512(): Unsupported number of attributes for materialization.");
                }
            }

            inline gdj::UInt512 materialize_node_512_last(uint64_t level, uint64_t node) {
                uint16_t c[16];
                switch (Q->getD()) {
                    case 1:
                    case 2:
                        return tab_extend_512[0][Q->get_node_last_level(level, node)];
                    case 3:
                    case 4:
                        return tab_extend_512[0][Q->get_node_64_last_level(level, node)];
                    case 5:
                        gdj::util::get16BitChunks(Q->get_node_64_last_level(level, node), c[0], c[1], c[2], c[3]);
                        return tab_extend_512[0][c[0]] | tab_extend_512[1][c[1]];
                    case 6:
                        gdj::util::get16BitChunks(Q->get_node_64_last_level(level, node), c[0], c[1], c[2], c[3]);
                        return tab_extend_512[0][c[0]] | tab_extend_512[1][c[1]] | tab_extend_512[2][c[2]] | tab_extend_512[3][c[3]];
                    case 7: {
                        gdj::UInt128 v = Q->get_node_128(level, node);
                        gdj::util::get16BitChunks(v.lo, c[0], c[1], c[2], c[3]);
                        gdj::util::get16BitChunks(v.hi, c[4], c[5], c[6], c[7]);
                        gdj::UInt512 out = tab_extend_512[0][c[0]];
                        for (int k = 1; k < 8; ++k)
                            out = out | tab_extend_512[k][c[k]];
                        return out;
                    }
                    case 8: {
                        gdj::UInt256 v = Q->get_node_256(level, node);
                        gdj::util::get16BitChunks(v.v0, c[0], c[1], c[2], c[3]);
                        gdj::util::get16BitChunks(v.v1, c[4], c[5], c[6], c[7]);
                        gdj::util::get16BitChunks(v.v2, c[8], c[9], c[10], c[11]);
                        gdj::util::get16BitChunks(v.v3, c[12], c[13], c[14], c[15]);
                        gdj::UInt512 out = tab_extend_512[0][c[0]];
                        for (int k = 1; k < 16; ++k)
                            out = out | tab_extend_512[k][c[k]];
                        return out;
                    }
                    default:
                        throw std::runtime_error("qdag::materialize_node_512_last(): Unsupported number of attributes for materialization.");
                }
            }

            inline gdj::UInt1024 materialize_node_1024(uint64_t level, uint64_t node, uint64_t* rank_vector) {
                uint16_t c[16];
                uint64_t r = Q->rank(level, node);
                switch (Q->getD()) {
                    case 1:
                    case 2:
                        return tab_extend_1024[0][Q->get_node(level, node, rank_vector, r)];
                    case 3:
                    case 4:
                        return tab_extend_1024[0][Q->get_node_64(level, node, rank_vector, r)];
                    case 5:
                        gdj::util::get16BitChunks(Q->get_node_64(level, node, rank_vector, r), c[0], c[1], c[2], c[3]);
                        return tab_extend_1024[0][c[0]] | tab_extend_1024[1][c[1]];
                    case 6:
                        gdj::util::get16BitChunks(Q->get_node_64(level, node, rank_vector, r), c[0], c[1], c[2], c[3]);
                        return tab_extend_1024[0][c[0]] | tab_extend_1024[1][c[1]] | tab_extend_1024[2][c[2]] | tab_extend_1024[3][c[3]];
                    case 7: {
                        gdj::UInt128 v = Q->get_node_128(level, node, rank_vector, r);
                        gdj::util::get16BitChunks(v.lo, c[0], c[1], c[2], c[3]);
                        gdj::util::get16BitChunks(v.hi, c[4], c[5], c[6], c[7]);
                        gdj::UInt1024 out = tab_extend_1024[0][c[0]];
                        for (int k = 1; k < 8; ++k)
                             out = out | tab_extend_1024[k][c[k]];
                        return out;
                    }
                    case 8: {
                        gdj::UInt256 v = Q->get_node_256(level, node, rank_vector, r);
                        gdj::util::get16BitChunks(v.v0, c[0], c[1], c[2], c[3]);
                        gdj::util::get16BitChunks(v.v1, c[4], c[5], c[6], c[7]);
                        gdj::util::get16BitChunks(v.v2, c[8], c[9], c[10], c[11]);
                        gdj::util::get16BitChunks(v.v3, c[12], c[13], c[14], c[15]);
                        gdj::UInt1024 out = tab_extend_1024[0][c[0]];
                        for (int k = 1; k < 16; ++k)
                            out = out | tab_extend_1024[k][c[k]];
                        return out;
                    }
                    case 9:
                        throw std::runtime_error("qdag::materialize_node_1024(): a 9-attribute base needs the lazy path (materialize_node_1024_lazy, multiJoinWide1024 tier).");
                    default:
                        throw std::runtime_error("qdag::materialize_node_1024(): Unsupported number of attributes for materialization.");
                }
            }

            inline gdj::UInt1024 materialize_node_1024_last(uint64_t level, uint64_t node) {
                uint16_t c[16];
                switch (Q->getD()) {
                    case 1:
                    case 2:
                        return tab_extend_1024[0][Q->get_node_last_level(level, node)];
                    case 3:
                    case 4:
                        return tab_extend_1024[0][Q->get_node_64_last_level(level, node)];
                    case 5:
                        gdj::util::get16BitChunks(Q->get_node_64_last_level(level, node), c[0], c[1], c[2], c[3]);
                        return tab_extend_1024[0][c[0]] | tab_extend_1024[1][c[1]];
                    case 6:
                        gdj::util::get16BitChunks(Q->get_node_64_last_level(level, node), c[0], c[1], c[2], c[3]);
                        return tab_extend_1024[0][c[0]] | tab_extend_1024[1][c[1]] | tab_extend_1024[2][c[2]] | tab_extend_1024[3][c[3]];
                    case 7: {
                        gdj::UInt128 v = Q->get_node_128(level, node);
                        gdj::util::get16BitChunks(v.lo, c[0], c[1], c[2], c[3]);
                        gdj::util::get16BitChunks(v.hi, c[4], c[5], c[6], c[7]);
                        gdj::UInt1024 out = tab_extend_1024[0][c[0]];
                        for (int k = 1; k < 8; ++k)
                            out = out | tab_extend_1024[k][c[k]];
                        return out;
                    }
                    case 8: {
                        gdj::UInt256 v = Q->get_node_256(level, node);
                        gdj::util::get16BitChunks(v.v0, c[0], c[1], c[2], c[3]);
                        gdj::util::get16BitChunks(v.v1, c[4], c[5], c[6], c[7]);
                        gdj::util::get16BitChunks(v.v2, c[8], c[9], c[10], c[11]);
                        gdj::util::get16BitChunks(v.v3, c[12], c[13], c[14], c[15]);
                        gdj::UInt1024 out = tab_extend_1024[0][c[0]];
                        for (int k = 1; k < 16; ++k)
                            out = out | tab_extend_1024[k][c[k]];
                        return out;
                    }
                    case 9: {
                        // Base de 9 atributos (solo dentro de un join de 10)
                        if (!use_scatter_)
                            throw std::runtime_error("qdag::materialize_node_1024_last(): a 9-attribute base is only supported inside a 10-attribute join.");
                        return materialize_scatter_last<gdj::UInt1024>(level, node);
                    }
                    default:
                        throw std::runtime_error("qdag::materialize_node_1024_last(): Unsupported number of attributes for materialization.");
                }
            }

            // ---- Dispersion + rango perezoso  ----
            // U es el tipo ancho: UInt256 (dim 7 u 8), UInt512 (dim 9) o UInt1024 (dim 10).
            template<class U>
            inline U materialize_scatter_last(uint64_t level, uint64_t node) {
                uint64_t v[8];
                read_base_words_(level, node, v);
                return scatter_extend_(scatter_tab_(static_cast<const U*>(nullptr)), v);
            }
            template<class U>
            inline U materialize_scatter_lazy(uint64_t level, uint64_t node, BaseNode512& bn) {
                bn.r = Q->rank(level, node);
                read_base_words_(level, node, bn.bits);
                return scatter_extend_(scatter_tab_(static_cast<const U*>(nullptr)), bn.bits);
            }
            static inline uint64_t lazy_child_rank(const BaseNode512& bn, uint16_t base_child) {
                const unsigned wi = base_child >> 6, bi = base_child & 63;
                uint64_t rank = bn.r;
                for (unsigned k = 0; k < wi; ++k)
                    rank += (uint64_t)__builtin_popcountll(bn.bits[k]);

                const uint64_t mask = (bi == 63) ? ~0ULL : ((1ULL << (bi + 1)) - 1);
                return rank + (uint64_t)__builtin_popcountll(bn.bits[wi] & mask);
            }

            void enable_scatter() {
                if (Msize == 128 || Msize == 256) {
                    if (scatter_256.empty())
                         build_scatter_(scatter_256);
                } else if (Msize == 512) {
                    if (scatter_512.empty())
                        build_scatter_(scatter_512);
                } else if (Msize == 1024) {
                    if (scatter_1024.empty())
                        build_scatter_(scatter_1024);
                } else
                    throw std::runtime_error("qdag::enable_scatter(): dim outside [7,10].");
            }

            void enable_scatter_1024() {
                enable_scatter();
            }
            inline gdj::UInt1024 materialize_node_1024_lazy(uint64_t level, uint64_t node, BaseNode512& bn) {
                if (scatter_1024.empty())
                    throw std::runtime_error("qdag::materialize_node_1024_lazy(): missing scatter table for dim=10.");
                return materialize_scatter_lazy<gdj::UInt1024>(level, node, bn);
            }
            inline gdj::UInt1024 materialize_node_1024_scatter(uint64_t level, uint64_t node) {
                if (scatter_1024.empty())
                    throw std::runtime_error("qdag::materialize_node_1024_scatter(): enable_scatter() was not called.");
                return materialize_scatter_last<gdj::UInt1024>(level, node);
            }

    };

} // namespace gdj
#endif
