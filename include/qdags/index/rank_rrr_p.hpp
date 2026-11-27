#ifndef RANK_RRR_P
#define RANK_RRR_P

#include "commun.hpp"
#include <sdsl/rrr_vector.hpp>
#include <sdsl/util.hpp>
#include <algorithm>
#include <stdexcept>

// rank_rrr_p: misma interfaz que rank_bv_64_p, respaldada por sdsl::rrr_vector<63>

namespace gdj {

    constexpr uint16_t RRR_BLOCK_SIZE = 63; // default de sdsl::rrr_vector<>

    class rank_rrr_p {
    private:
        sdsl::rrr_vector<RRR_BLOCK_SIZE> bv_;
        sdsl::rrr_vector<RRR_BLOCK_SIZE>::rank_1_type rank_supp_;

        void rebind() { rank_supp_.set_vector(&bv_); }

        template<class t_itr>
        static sdsl::bit_vector build_from_positions(t_itr begin, t_itr end, uint64_t n) {
            sdsl::bit_vector bv(n, 0);
            for (t_itr it = begin; it != end; ++it)
                bv[*it] = 1;
            return bv;
        }

    public:
        rank_rrr_p() { rebind(); }

        rank_rrr_p(const rank_rrr_p& other) : bv_(other.bv_) { rebind(); }

        rank_rrr_p& operator=(const rank_rrr_p& other) {
            if (this != &other) {
                bv_ = other.bv_;
                rebind();
            }
            return *this;
        }

        rank_rrr_p(rank_rrr_p&& other) noexcept : bv_(std::move(other.bv_)) { rebind(); }

        rank_rrr_p& operator=(rank_rrr_p&& other) noexcept {
            if (this != &other) {
                bv_ = std::move(other.bv_);
                rebind();
            }
            return *this;
        }

        ~rank_rrr_p() = default;

        // Desde un bit_vector denso (rrr_vector no tiene builder incremental).
        rank_rrr_p(const sdsl::bit_vector& bv) : bv_(bv) { rebind(); }

        // Desde las posiciones de los unos, ya ordenadas y crecientes, con n = tamano total del nivel. 
        template<class t_itr>
        rank_rrr_p(t_itr begin, t_itr end, uint64_t n)
            : bv_(build_from_positions(begin, end, n)) { rebind(); }

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

        // Nodo de d=9 (512 bits): dos lecturas de 256 bits. Asume start_pos
        // multiplo de 64.
        inline gdj::UInt512 get_512_bits(uint64_t start_pos) const {
            if (start_pos + 512 > bv_.size())
                throw std::out_of_range("get_512_bits: Not enough bits available from start_pos.");
            return gdj::compose_512(get_256_bits(start_pos), get_256_bits(start_pos + 256));
        }

        inline uint64_t size() const {
            return bv_.size();
        }

        inline uint64_t n_ones() const {
            return rank_supp_.rank(bv_.size());
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
