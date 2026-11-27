#ifndef RANK_BV_P
#define RANK_BV_P

#include "commun.hpp"
#include <sdsl/bit_vectors.hpp>
#include <sdsl/int_vector_buffer.hpp>
#include <algorithm>
#include <iostream>
#include <stdexcept>

// rank_bv_64_p: backend de rank sobre un array plano de palabras de 64 bits
// (seq) + rank precalculado por palabra (block).
namespace gdj {

    class rank_bv_64_p {
    private:
        uint64_t* seq;
        uint32_t* block;
        uint64_t u;   // largo del bit vector
        uint64_t n;   // numero de unos

        void copy_from(const rank_bv_64_p& other) {
            u = other.u;
            n = other.n;
            uint64_t num_words = (u + 63) / 64;

            seq = new uint64_t[num_words]();
            block = new uint32_t[num_words]();

            std::copy(other.seq, other.seq + num_words, seq);
            std::copy(other.block, other.block + num_words, block);
        }

        void free_resources() {
            delete[] seq;
            delete[] block;
            seq = nullptr;
            block = nullptr;
            u = 0;
            n = 0;
        }

    public:

        rank_bv_64_p() : seq(nullptr), block(nullptr), u(0), n(0) {}

        rank_bv_64_p(const rank_bv_64_p& other) : seq(nullptr), block(nullptr) {
            if (other.seq) {
                copy_from(other);
            }
        }

        rank_bv_64_p& operator=(const rank_bv_64_p& other) {
            if (this != &other) {
                free_resources();
                if (other.seq) {
                    copy_from(other);
                }
            }
            return *this;
        }

        rank_bv_64_p(rank_bv_64_p&& other) noexcept
            : seq(other.seq), block(other.block), u(other.u), n(other.n) {
            other.seq = nullptr;
            other.block = nullptr;
            other.u = 0;
            other.n = 0;
        }

        rank_bv_64_p& operator=(rank_bv_64_p&& other) noexcept {
            if (this != &other) {
                free_resources();

                seq = other.seq;
                block = other.block;
                u = other.u;
                n = other.n;

                other.seq = nullptr;
                other.block = nullptr;
                other.u = 0;
                other.n = 0;
            }
            return *this;
        }

        ~rank_bv_64_p() {
            free_resources();
        }

        // copiando palabras de 64 bits de un sdsl::bit_vector (no bit por bit)
        rank_bv_64_p(const sdsl::bit_vector &bv) : seq(nullptr), block(nullptr) {
            build_from_words(bv.data(), bv.size());
        }

        // desde palabras ya armadas (bit i en words[i/64], posicioni%64), u = largo en bits.
        rank_bv_64_p(const uint64_t *words, uint64_t n_bits) : seq(nullptr), block(nullptr) {
            build_from_words(words, n_bits);
        }


        inline const uint64_t *data() const {
            return seq;
        }

    private:
        void build_from_words(const uint64_t *words, uint64_t n_bits) {
            u = n_bits;
            uint64_t num_words = (u + 63) / 64;
            seq = new uint64_t[num_words];
            block = new uint32_t[num_words];
            uint32_t ones = 0;
            for (uint64_t w = 0; w < num_words; ++w) {
                uint64_t x = words[w];
                // los bits por encima de n_bits en la ultima palabra quedan en 0.
                if (w == num_words - 1 && (u & 63))
                    x &= ~0ULL >> (64 - (u & 63));
                seq[w] = x;
                block[w] = ones;
                ones += __builtin_popcountll(x);
            }
            n = ones;
        }

    public:

        inline uint64_t rank(uint64_t i) const {
            return block[i >> 6] + sdsl::bits::cnt(seq[i >> 6] & ~(0xffffffffffffffff << (i & 0x3f)));
        }

        inline bool get_bit(uint64_t pos) const {
            return (seq[pos / 64] >> (pos % 64)) & 1ULL;
        }

        inline uint8_t get_2_bits(uint64_t start_pos) const {
            return ((seq[start_pos >> 6] >>(start_pos & 0x3f) ) & 0x03);
        }

        inline uint8_t get_4_bits(uint64_t start_pos) const {
            return ((seq[start_pos >> 6] >>(start_pos & 0x3f) ) & 0x0f);
        }

        inline uint8_t get_8_bits(uint64_t start_pos) const {
            if (start_pos + 8 > u)
                throw std::out_of_range("get_8_bits: Not enough bits available from start_pos.");

            uint64_t word = seq[start_pos >> 6];
            uint8_t offset = start_pos & 0x3f;
            return static_cast<uint8_t>((word >> offset) & 0xFFULL);
        }

        inline uint16_t get_16_bits(uint64_t start_pos) const {
            if (start_pos + 16 > u) {
                throw std::out_of_range("get_16_bits: Not enough bits available from start_pos.");
            }
            uint64_t word = seq[start_pos >> 6];
            uint8_t offset = start_pos & 0x3f;
            return static_cast<uint16_t>((word >> offset) & 0xFFFFULL);
        }

        inline uint32_t get_32_bits(uint64_t start_pos) const {
            if (start_pos + 32 > u)
                throw std::out_of_range("get_32_bits: Not enough bits available from start_pos.");
            uint64_t word = seq[start_pos >> 6];
            uint8_t offset = start_pos & 0x3f;
            return static_cast<uint32_t>((word >> offset) & 0xFFFFFFFFULL);
        }

        // Asume start_pos multiplo de 64: accede a una palabra de 64 bits.
        inline uint64_t get_64_bits(uint64_t start_pos) const {
            if (start_pos + 64 > u) {
                throw std::out_of_range("get_64_bits: Not enough bits available from start_pos.");
            }
            return seq[start_pos >> 6];
        }

        // accede a 2 palabras consecutivas.
        inline gdj::UInt128 get_128_bits(uint64_t start_pos) const {
            if (start_pos + 128 > u) {
                throw std::out_of_range("get_128_bits: Not enough bits available from start_pos.");
            }
            uint64_t current_word_idx = start_pos >> 6;
            gdj::UInt128 result;
            result.lo = seq[current_word_idx];
            result.hi = seq[current_word_idx + 1];
            return result;
        }

        // accede a 4 palabras consecutivas.
        inline gdj::UInt256 get_256_bits(uint64_t start_pos) const {
            if (start_pos + 256 > u) {
                throw std::out_of_range("get_256_bits: Not enough bits available from start_pos.");
            }
            uint64_t current_word_idx = start_pos >> 6;
            gdj::UInt256 result_256;
            result_256.v0 = seq[current_word_idx];
            result_256.v1 = seq[current_word_idx + 1];
            result_256.v2 = seq[current_word_idx + 2];
            result_256.v3 = seq[current_word_idx + 3];
            return result_256;
        }

        // accede a 8 palabras consecutivas(nodo de d=9, 512 bits).
        inline gdj::UInt512 get_512_bits(uint64_t start_pos) const {
            if (start_pos + 512 > u) {
                throw std::out_of_range("get_512_bits: Not enough bits available from start_pos.");
            }
            uint64_t current_word_idx = start_pos >> 6;
            gdj::UInt512 result_512;
            for (int i = 0; i < 8; ++i)
                result_512.w[i] = seq[current_word_idx + i];
            return result_512;
        }

        inline uint64_t size() const {
            return u;
        }

        inline uint64_t n_ones() const {
            return n;
        }

        inline uint64_t size_in_bytes() const {
            uint64_t num_words = (u + 63) / 64;
            return sizeof(uint64_t) * num_words + sizeof(uint32_t) * num_words +
                sizeof(uint64_t*) + sizeof(uint32_t*) + 2*sizeof(uint64_t);
        }

        void serialize(std::ostream& out) const {
            out.write(reinterpret_cast<const char*>(&u), sizeof(u));
            out.write(reinterpret_cast<const char*>(&n), sizeof(n));

            uint64_t num_words = (u + 63) / 64;

            if (num_words > 0) {
                out.write(reinterpret_cast<const char*>(seq), num_words * sizeof(uint64_t));
                out.write(reinterpret_cast<const char*>(block), num_words * sizeof(uint32_t));
            }
        }

        void load(std::istream& in) {
            free_resources();

            in.read(reinterpret_cast<char*>(&u), sizeof(u));
            in.read(reinterpret_cast<char*>(&n), sizeof(n));

            uint64_t num_words = (u + 63) / 64;

            if (num_words > 0) {
                seq = new uint64_t[num_words]();
                block = new uint32_t[num_words]();

                in.read(reinterpret_cast<char*>(seq), num_words * sizeof(uint64_t));
                in.read(reinterpret_cast<char*>(block), num_words * sizeof(uint32_t));
            }
        }
    };

}
#endif
