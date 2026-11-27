#ifndef COMMON_HPP
#define COMMON_HPP

#include <vector>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <array>
#include <sdsl/k2_tree_helper.hpp>
#include <sdsl/uint128_t.hpp>
#include <sdsl/uint256_t.hpp>

// tipos, costantes y utilidades comunes a todos los qdags y sus motores.
namespace gdj {
    using att_set = std::vector<uint16_t>;
    using type_mapping_M = uint16_t;
    using idx_type = sdsl::k2_tree_ns::idx_type;
    using size_type = sdsl::k2_tree_ns::size_type;
    using t_part_tuple = std::tuple<idx_type, idx_type, size_type, idx_type*>;
    constexpr uint8_t K = 2;
    constexpr uint8_t CUT_OFF_LEVEL = 5;
    constexpr uint64_t GRID_SIDE = 52000000;
    constexpr uint64_t DEFAULT_BOUNDED_RESULT = 0;

    typedef std::array<std::string, 3> user_triple;
    typedef std::array<uint32_t, 3> spo_triple;
    typedef uint8_t spo_order_type[3];
    typedef spo_order_type spo_orders_type[6];
    const static spo_orders_type spo_orders = {{0, 1, 2},   //SPO
                                                {0, 2, 1},  //SOP
                                                {1, 2, 0},  //POS
                                                {1, 0, 2},  //PSO
                                                {2, 0, 1},  //OSP
                                                {2, 1, 0}}; //OPS

    struct EdgeTraversalState {
        uint16_t level;
        uint64_t node_idx_in_bv;
        std::vector<idx_type> coords;

        EdgeTraversalState(uint16_t l, uint64_t n, std::vector<idx_type> c)
            : level(l), node_idx_in_bv(n), coords(std::move(c)) {}
    };

    // Entero de 128 bits como 2 palabras de 64 (lo/hi).
    struct UInt128 {
        uint64_t lo;
        uint64_t hi;

        UInt128() : lo(0), hi(0) {}
        UInt128(uint64_t high_val, uint64_t low_val) : lo(low_val), hi(high_val) {}


        UInt128(const sdsl::uint128_t& v) : lo((uint64_t)v), hi((uint64_t)(v >> 64)) {}

        friend std::ostream& operator<<(std::ostream& os, const UInt128& val) {
            os << "High: " << val.hi << ", Low: " << val.lo;
            return os;
        }

        UInt128 operator&(const UInt128& other) const {
            return UInt128(this->hi & other.hi, this->lo & other.lo);
        }
        UInt128 operator|(const UInt128& other) const {
            return UInt128(this->hi | other.hi, this->lo | other.lo);
        }
        UInt128 operator^(const UInt128& other) const {
            return UInt128(this->hi ^ other.hi, this->lo ^ other.lo);
        }
        UInt128 operator~() const {
            return UInt128(~this->hi, ~this->lo);
        }

        UInt128 operator<<(unsigned int shift) const {
            if (shift == 0)
                return *this;
            if (shift >= 128)
                return UInt128(0, 0);

            UInt128 result;
            if (shift >= 64) {
                result.lo = 0;
                result.hi = this->lo << (shift - 64);
            } else {
                result.lo = this->lo << shift;
                result.hi = (this->hi << shift) | (this->lo >> (64 - shift));
            }
            return result;
        }

        UInt128 operator>>(unsigned int shift) const {
            if (shift == 0)
                return *this;
            if (shift >= 128)
                return UInt128(0, 0);

            UInt128 result;
            if (shift >= 64) {
                result.hi = 0;
                result.lo = this->hi >> (shift - 64);
            } else {
                result.hi = this->hi >> shift;
                result.lo = (this->lo >> shift) | (this->hi << (64 - shift));
            }
            return result;
        }

        UInt128& operator&=(const UInt128& other) {
            *this = *this & other;
             return *this;
        }
        UInt128& operator|=(const UInt128& other) {
            *this = *this | other;
            return *this;
        }
        UInt128& operator^=(const UInt128& other) {
            *this = *this ^ other;
            return *this;
        }
        UInt128& operator<<=(unsigned int shift) {
            *this = *this << shift;
            return *this;
         }
        UInt128& operator>>=(unsigned int shift) {
            *this = *this >> shift;
            return *this;
        }

        bool operator==(const UInt128& other) const {
            return hi == other.hi && lo == other.lo;
        }
        bool operator!=(const UInt128& other) const {
             return !(*this == other);
        }
    };

    // Entero de 256 bits como 4 palabras de 64 (v0 la menos significativa).
    struct UInt256 {
        uint64_t v0;
        uint64_t v1;
        uint64_t v2;
        uint64_t v3;

        UInt256() : v0(0), v1(0), v2(0), v3(0) {}

        UInt256(uint64_t val_v3, uint64_t val_v2, uint64_t val_v1, uint64_t val_v0)
            : v0(val_v0), v1(val_v1), v2(val_v2), v3(val_v3) {}

        UInt256(const UInt128& val) : v0(val.lo), v1(val.hi), v2(0), v3(0) {}

        UInt256(sdsl::uint256_t v)
            : v0((uint64_t)v), v1((uint64_t)(v >> 64)),
              v2((uint64_t)(v >> 128)), v3((uint64_t)(v >> 192)) {}

        friend std::ostream& operator<<(std::ostream& os, const UInt256& val) {
            os << "V3: " << val.v3 << ", V2: " << val.v2
               << ", V1: " << val.v1 << ", V0: " << val.v0;
            return os;
        }

        UInt256 operator&(const UInt256& other) const {
            return UInt256(this->v3 & other.v3, this->v2 & other.v2,
                           this->v1 & other.v1, this->v0 & other.v0);
        }
        UInt256 operator|(const UInt256& other) const {
            return UInt256(this->v3 | other.v3, this->v2 | other.v2,
                           this->v1 | other.v1, this->v0 | other.v0);
        }
        UInt256 operator^(const UInt256& other) const {
            return UInt256(this->v3 ^ other.v3, this->v2 ^ other.v2,
                           this->v1 ^ other.v1, this->v0 ^ other.v0);
        }
        UInt256 operator~() const {
            return UInt256(~this->v3, ~this->v2, ~this->v1, ~this->v0);
        }

        // shift==64/128/192 exacto se trata aparte (s==0)
        UInt256 operator<<(unsigned int shift) const {
            if (shift == 0)
                return *this;
            if (shift >= 256)
                return UInt256(0, 0, 0, 0);

            UInt256 result;
            if (shift >= 192) {
                result.v0 = 0;
                result.v1 = 0;
                 result.v2 = 0;
                result.v3 = this->v0 << (shift - 192);
            } else if (shift >= 128) {
                unsigned int s = shift - 128;
                result.v0 = 0;
                result.v1 = 0;
                result.v2 = this->v0 << s;
                result.v3 = (this->v1 << s) | (s == 0 ? 0ULL : (this->v0 >> (64 - s)));
            } else if (shift >= 64) {
                unsigned int s = shift - 64;
                result.v0 = 0;
                result.v1 = this->v0 << s;
                result.v2 = (this->v1 << s) | (s == 0 ? 0ULL : (this->v0 >> (64 - s)));
                result.v3 = (this->v2 << s) | (s == 0 ? 0ULL : (this->v1 >> (64 - s)));
            } else {
                result.v0 = this->v0 << shift;
                result.v1 = (this->v1 << shift) | (this->v0 >> (64 - shift));
                result.v2 = (this->v2 << shift) | (this->v1 >> (64 - shift));
                result.v3 = (this->v3 << shift) | (this->v2 >> (64 - shift));
            }
            return result;
        }

        UInt256 operator>>(unsigned int shift) const {
            if (shift == 0)
                return *this;
            if (shift >= 256)
                return UInt256(0, 0, 0, 0);

            UInt256 result;
            if (shift >= 192) {
                result.v3 = 0; result.v2 = 0; result.v1 = 0;
                result.v0 = this->v3 >> (shift - 192);
            } else if (shift >= 128) {
                unsigned int s = shift - 128;
                result.v3 = 0; result.v2 = 0;
                result.v1 = this->v3 >> s;
                result.v0 = (this->v2 >> s) | (s == 0 ? 0ULL : (this->v3 << (64 - s)));
            } else if (shift >= 64) {
                unsigned int s = shift - 64;
                result.v3 = 0;
                result.v2 = this->v3 >> s;
                result.v1 = (this->v2 >> s) | (s == 0 ? 0ULL : (this->v3 << (64 - s)));
                result.v0 = (this->v1 >> s) | (s == 0 ? 0ULL : (this->v2 << (64 - s)));
            } else {
                result.v0 = (this->v0 >> shift) | (this->v1 << (64 - shift));
                result.v1 = (this->v1 >> shift) | (this->v2 << (64 - shift));
                result.v2 = (this->v2 >> shift) | (this->v3 << (64 - shift));
                result.v3 = this->v3 >> shift;
            }
            return result;
        }

        UInt256& operator&=(const UInt256& other) {
            *this = *this & other;
             return *this;
        }
        UInt256& operator|=(const UInt256& other) {
            *this = *this | other;
            return *this;
        }
        UInt256& operator^=(const UInt256& other) {
            *this = *this ^ other;
            return *this;
        }
        UInt256& operator<<=(unsigned int shift) {
            *this = *this << shift;
            return *this;
        }
        UInt256& operator>>=(unsigned int shift) {
            *this = *this >> shift;
            return *this;
         }

        bool operator==(const UInt256& other) const {
            return v0 == other.v0 && v1 == other.v1 && v2 == other.v2 && v3 == other.v3;
        }
        bool operator!=(const UInt256& other) const {
             return !(*this == other);
        }
    };

    // Entero sin signo ancho generico, respaldado por NWORDS palabras de 64 bits (w[0] la menos significativa).
    template<int NWORDS>
    struct UIntWideN {
        uint64_t w[NWORDS];

        UIntWideN() {
            for (int i = 0; i < NWORDS; ++i)
                w[i] = 0;
        }

        UIntWideN operator&(const UIntWideN& o) const {
            UIntWideN r;
            for (int i = 0; i < NWORDS; ++i)
                r.w[i] = w[i] & o.w[i];
            return r;
        }
        UIntWideN operator|(const UIntWideN& o) const {
            UIntWideN r;
            for (int i = 0; i < NWORDS; ++i)
                r.w[i] = w[i] | o.w[i];
            return r;
        }
        UIntWideN operator^(const UIntWideN& o) const {
            UIntWideN r;
            for (int i = 0; i < NWORDS; ++i)
                r.w[i] = w[i] ^ o.w[i];
            return r;
        }
        UIntWideN operator~() const {
            UIntWideN r;
            for (int i = 0; i < NWORDS; ++i)
                r.w[i] = ~w[i];
            return r;
        }

        UIntWideN operator<<(unsigned int shift) const {
            UIntWideN r;
            if (shift >= (unsigned int)(NWORDS * 64))
                return r; // todo-ceros
            unsigned int word_shift = shift / 64;
            unsigned int bit_shift = shift % 64;
            for (int i = NWORDS - 1; i >= (int)word_shift; --i) {
                uint64_t hi = w[i - word_shift] << bit_shift;
                uint64_t lo = (bit_shift == 0 || i - (int)word_shift - 1 < 0)
                                  ? 0ULL : (w[i - word_shift - 1] >> (64 - bit_shift));
                r.w[i] = hi | lo;
            }
            return r;
        }

        UIntWideN operator>>(unsigned int shift) const {
            UIntWideN r;
            if (shift >= (unsigned int)(NWORDS * 64))
                return r; // todo-ceros
            unsigned int word_shift = shift / 64;
            unsigned int bit_shift = shift % 64;
            for (int i = 0; i < NWORDS - (int)word_shift; ++i) {
                uint64_t lo = w[i + word_shift] >> bit_shift;
                uint64_t hi = (bit_shift == 0 || i + (int)word_shift + 1 >= NWORDS)
                                  ? 0ULL : (w[i + word_shift + 1] << (64 - bit_shift));
                r.w[i] = lo | hi;
            }
            return r;
        }

        UIntWideN& operator&=(const UIntWideN& o) {
                *this = *this & o;
                return *this;
        }
        UIntWideN& operator|=(const UIntWideN& o) {
            *this = *this | o;
            return *this;
        }
        UIntWideN& operator^=(const UIntWideN& o) {
            *this = *this ^ o;
            return *this;
        }
        UIntWideN& operator<<=(unsigned int shift) {
            *this = *this << shift;
            return *this;
        }
        UIntWideN& operator>>=(unsigned int shift) {
            *this = *this >> shift;
            return *this;
        }

        bool operator==(const UIntWideN& o) const {
            for (int i = 0; i < NWORDS; ++i)
                if (w[i] != o.w[i])
                    return false;
            return true;
        }
        bool operator!=(const UIntWideN& o) const { return !(*this == o); }

        friend std::ostream& operator<<(std::ostream& os, const UIntWideN& v) {
            for (int i = NWORDS - 1; i >= 0; --i) {
                os << "W" << i << ": " << v.w[i];
                if (i > 0)
                    os << ", ";
            }
            return os;
        }
    };

    using UInt512  = UIntWideN<8>;
    using UInt1024 = UIntWideN<16>;

    // nodo base de d=9 (512 bits) + rango del nodo, para el rango perezoso
    // del escalon de 1024.
    struct BaseNode512 {
        uint64_t bits[8];
        uint64_t r;
    };

    // une dos mitades de 256 bits (lo = bits 0..255, hi = bits 256..511) en un UInt512.
    inline UInt512 compose_512(const UInt256& lo, const UInt256& hi) {
        UInt512 r;
        r.w[0] = lo.v0;
        r.w[1] = lo.v1;
        r.w[2] = lo.v2;
        r.w[3] = lo.v3;
        r.w[4] = hi.v0;
        r.w[5] = hi.v1;
        r.w[6] = hi.v2;
        r.w[7] = hi.v3;
        return r;
    }


    inline void set_bit(UInt256& u, uint64_t pos) {
        const uint64_t m = 1ULL << (pos & 63);
        switch (pos >> 6) {
            case 0:
                u.v0 |= m;
                break;
            case 1:
                u.v1 |= m;
                break;
            case 2:
                u.v2 |= m;
                break;
            default:
                u.v3 |= m;
        }
    }
    template<int N>
    inline void set_bit(UIntWideN<N>& u, uint64_t pos) {
        u.w[pos >> 6] |= 1ULL << (pos & 63);
    }

    // a |= b, sin temporarios anchos.
    inline void or_into(UInt256& a, const UInt256& b) {
        a.v0 |= b.v0;
        a.v1 |= b.v1;
        a.v2 |= b.v2;
        a.v3 |= b.v3;
    }
    template<int N>
    inline void or_into(UIntWideN<N>& a, const UIntWideN<N>& b) {
        for (int k = 0; k < N; ++k)
        a.w[k] |= b.w[k];
    }

    // Bits del tipo ancho
    constexpr uint64_t width_of(const UInt256*) {
        return 256;
    }
    template<int N>
    constexpr uint64_t width_of(const UIntWideN<N>*) {
        return 64 * N;
    }

}

#endif // COMMON_HPP
