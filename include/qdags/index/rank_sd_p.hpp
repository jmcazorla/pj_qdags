#ifndef RANK_SD_P
#define RANK_SD_P

#include "commun.hpp"
#include <sdsl/sd_vector.hpp>
#include <sdsl/util.hpp>
#include <algorithm>
#include <stdexcept>

// rank_sd_p: misma interfaz que rank_bv_64_p, respaldada por
// sdsl::sd_vector<> para niveles anchos y muy dispersos

namespace gdj {

    class rank_sd_p {
    private:
        sdsl::sd_vector<> bv_;
        sdsl::rank_support_sd<1> rank_supp_;

        void rebind() { rank_supp_.set_vector(&bv_); }

    public:
        rank_sd_p() { rebind(); }

        rank_sd_p(const rank_sd_p& other) : bv_(other.bv_) { rebind(); }

        rank_sd_p& operator=(const rank_sd_p& other) {
            if (this != &other) {
                bv_ = other.bv_;
                rebind();
            }
            return *this;
        }

        rank_sd_p(rank_sd_p&& other) noexcept : bv_(std::move(other.bv_)) { rebind(); }

        rank_sd_p& operator=(rank_sd_p&& other) noexcept {
            if (this != &other) {
                bv_ = std::move(other.bv_);
                rebind();
            }
            return *this;
        }

        ~rank_sd_p() = default;

        // Desde un bit_vector denso: drop-in directo, pero paga el costo de
        // materializarlo primero.
        rank_sd_p(const sdsl::bit_vector& bv) : bv_(bv) { rebind(); }

        // Desde las posiciones de los unos, ya ordenadas y crecientes (evita
        // el bit_vector denso intermedio). n = largo total del nivel.
        template<class t_itr>
        rank_sd_p(t_itr begin, t_itr end, uint64_t n) {
            sdsl::sd_vector_builder builder(n, (uint64_t)std::distance(begin, end));
            for (t_itr it = begin; it != end; ++it)
                builder.set(*it);
            bv_ = sdsl::sd_vector<>(builder);
            rebind();
        }

        inline uint64_t rank(uint64_t i) const {
            return rank_supp_.rank(i);
        }

        inline bool get_bit(uint64_t pos) const {
            return bv_[pos];
        }

        inline uint8_t get_2_bits(uint64_t start_pos) const {
            return (uint8_t)bv_.get_int(start_pos, 2);
        }

        inline uint8_t get_4_bits(uint64_t start_pos) const {
            return (uint8_t)bv_.get_int(start_pos, 4);
        }

        inline uint8_t get_8_bits(uint64_t start_pos) const {
            if (start_pos + 8 > bv_.size())
                throw std::out_of_range("get_8_bits: Not enough bits available from start_pos.");
            return (uint8_t)bv_.get_int(start_pos, 8);
        }

        inline uint16_t get_16_bits(uint64_t start_pos) const {
            if (start_pos + 16 > bv_.size())
                throw std::out_of_range("get_16_bits: Not enough bits available from start_pos.");
            return (uint16_t)bv_.get_int(start_pos, 16);
        }

        inline uint32_t get_32_bits(uint64_t start_pos) const {
            if (start_pos + 32 > bv_.size())
                throw std::out_of_range("get_32_bits: Not enough bits available from start_pos.");
            return (uint32_t)bv_.get_int(start_pos, 32);
        }

        inline uint64_t get_64_bits(uint64_t start_pos) const {
            if (start_pos + 64 > bv_.size())
                throw std::out_of_range("get_64_bits: Not enough bits available from start_pos.");
            return bv_.get_uint64(start_pos);
        }

        inline gdj::UInt128 get_128_bits(uint64_t start_pos) const {
            if (start_pos + 128 > bv_.size())
                throw std::out_of_range("get_128_bits: Not enough bits available from start_pos.");
            return gdj::UInt128(bv_.get_uint128(start_pos));
        }

        inline gdj::UInt256 get_256_bits(uint64_t start_pos) const {
            if (start_pos + 256 > bv_.size())
                throw std::out_of_range("get_256_bits: Not enough bits available from start_pos.");
            return gdj::UInt256(bv_.get_uint256(start_pos));
        }

        // Nodo de d=9 (512 bits): dos lecturas de 256 bits. Asume start_pos multiplo de 64.
        inline gdj::UInt512 get_512_bits(uint64_t start_pos) const {
            if (start_pos + 512 > bv_.size())
                throw std::out_of_range("get_512_bits: Not enough bits available from start_pos.");
            return gdj::compose_512(get_256_bits(start_pos), get_256_bits(start_pos + 256));
        }

        inline uint64_t size() const {
            return bv_.size();
        }

        inline uint64_t n_ones() const {
            return bv_.low.size();
        }

        inline uint64_t size_in_bytes() const {
            return sdsl::size_in_bytes(bv_);
        }

        void serialize(std::ostream& out) const {
            bv_.serialize(out);
        }

        void load(std::istream& in) {
            bv_.load(in);
            rebind();
        }
    };

}
#endif
