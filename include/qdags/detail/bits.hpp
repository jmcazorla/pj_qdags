#ifndef QDAGS_DETAIL_BITS_HPP
#define QDAGS_DETAIL_BITS_HPP

#include <cstdint>
#include <set>
#include <vector>
#include "../index/commun.hpp"

// Utilidades de bits de se_quadtree (removeBits, appendChildBitsToCoords) y de qdag (get16BitChunks).

namespace gdj {

    namespace util {

        // Quita los bits de 'p' en 'positions_to_remove' y compacta el resto hacia abajo.
        template <typename T>
        T removeBits(T p, const std::set<uint16_t>& positions_to_remove) {
            T result = 0;
            int new_bit_position = 0;
            uint16_t n_bits = sizeof(T) * 8;

            for (uint16_t original_bit_position = 0; original_bit_position < n_bits; ++original_bit_position) {
                if (positions_to_remove.count(original_bit_position) == 0) {
                    T bit_value = (p >> original_bit_position) & 1;
                    if (bit_value == 1) {
                        result |= (static_cast<T>(1) << new_bit_position);
                    }
                    new_bit_position++;
                }
            }

            return result;
        }

        // Agrega un bit de child_idx a cada coordenada (el LSB a la ultima).
        std::vector<gdj::idx_type> appendChildBitsToCoords(std::vector<gdj::idx_type> new_coords,
                                                            uint32_t child_idx) {
            int pos = new_coords.size() - 1;
            for (int i = pos; i >= 0; --i) {
                uint32_t bit_to_append = child_idx & 1;
                new_coords[i] = (new_coords[i] << 1) | bit_to_append;
                child_idx >>= 1;
            }
            return new_coords;
        }

        // Trocea un uint64_t en cuatro chunks de 16 bits (indexan las sub-tablas de extension).
        static inline void get16BitChunks(uint64_t number, uint16_t& chunk1, uint16_t& chunk2, uint16_t& chunk3, uint16_t& chunk4) {
            chunk1 = static_cast<uint16_t>(number & 0xFFFFULL);
            chunk2 = static_cast<uint16_t>((number >> 16) & 0xFFFFULL);
            chunk3 = static_cast<uint16_t>((number >> 32) & 0xFFFFULL);
            chunk4 = static_cast<uint16_t>((number >> 48) & 0xFFFFULL);
        }

    } // namespace util

} // namespace gdj

#endif // QDAGS_DETAIL_BITS_HPP
