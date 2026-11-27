#ifndef QDAGS_INDEX_WIDE_BASE_CONFIG_HPP
#define QDAGS_INDEX_WIDE_BASE_CONFIG_HPP

#include <cstdint>
#include <string>

// wide_base_config: configuracion de la extension de un qdag de d atributos dentro de un join de dim atributos.
// El join de dim atributos puede extenderse por dispersion (scatter) o por tablas (tables).
// El modo automatic decide segun d y dim. La dispersion solo se usa para dim >= 7 y d >= 4, o d=9 en dim=10.

namespace gdj {

    enum class wide_base_mode : uint8_t { automatic = 0, tables = 1, scatter = 2 };

    // d minimo con el que el modo automatic usa dispersion.
    constexpr unsigned WIDE_AUTO_MIN_D = 4;

    struct wide_base_config {
        wide_base_mode mode = wide_base_mode::automatic;
        unsigned min_d = 4; // solo modo scatter
        unsigned min_dim = 7; // solo modo scatter
    };

    inline const char* to_string(wide_base_mode m) {
        switch (m) {
            case wide_base_mode::tables:
                return "tables";
            case wide_base_mode::scatter:
                return "scatter";
            default:
                return "automatic";
        }
    }

    // false si el texto no es automatic, tables ni scatter.
    inline bool parse_wide_base_mode(const std::string& s, wide_base_mode& out) {
        if (s == "automatic") {
            out = wide_base_mode::automatic;
            return true;
        }
        if (s == "tables") {
            out = wide_base_mode::tables;
            return true;
        }
        if (s == "scatter") {
            out = wide_base_mode::scatter;
            return true;
        }
        return false;
    }

    // Si un qdag con una base de d atributos dentro de un join de dim atributos extiende por dispersion.
    inline bool wide_base_uses_scatter(unsigned d, unsigned dim, const wide_base_config& c) {
        if (dim < 7 || dim > 10 || dim < d)
            return false;
        if (d == 9 && dim == 10)
            return true;
        if (d > (dim == 10 ? 9u : 8u))
            return false;
        switch (c.mode) {
            case wide_base_mode::tables:
                return false;
            case wide_base_mode::scatter:
                return d >= c.min_d && dim >= c.min_dim;
            default:
                return d >= WIDE_AUTO_MIN_D;
        }
    }

    // Configuracion para los motores resumables, que no soportan la dispersion: automatic pasa a tables.
    // Con scatter (o d=9 en dim=10) el motor lanza una excepcion al recibir un qdag por dispersion.
    inline wide_base_config wide_base_for_resumable(wide_base_config c) {
        if (c.mode == wide_base_mode::automatic)
            c.mode = wide_base_mode::tables;
        return c;
    }

} // namespace gdj

#endif // QDAGS_INDEX_WIDE_BASE_CONFIG_HPP
