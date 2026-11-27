#ifndef QDAGS_STRATEGY_COEFFICIENT_GROWTH_HPP
#define QDAGS_STRATEGY_COEFFICIENT_GROWTH_HPP

#include <cmath>
#include <cstdint>
#include <string>

// Como crece el coefficient_bound entre intentos en las variantes adaptive y resumable (WeakKernel, Leapfrog, Plan).
//   Fixed         c <- c * growth_factor
//   Proportional  c <- c * clamp(L / r * SAFETY, growth_factor, MAX), con r los resultados del join final del
//                 intento que no llego a L (r = 0: c * ZERO).
// Proportional supone resultados aproximadamente proporcionales a c. growth_factor es el factor minimo: el
// coeficiente siempre sube y max_retries acota el bucle.

namespace gdj {

    enum class GrowthMode : uint8_t { Fixed = 0, Proportional = 1 };

    // Parametros del modo Proportional.
    constexpr double GROWTH_SAFETY = 1.5;   // cubre curvas algo sublineales
    constexpr double GROWTH_ZERO   = 4.0;   // factor cuando el intento dio 0 resultados
    constexpr double GROWTH_MAX    = 64.0;  // tope del factor por intento

    inline const char* to_string(GrowthMode m) { return m == GrowthMode::Proportional ? "proportional" : "fixed"; }

    // false si el texto no es ni "fixed" ni "proportional".
    inline bool parse_growth_mode(const std::string& s, GrowthMode& out) {
        if (s == "fixed") {
            out = GrowthMode::Fixed;
            return true;
        }
        if (s == "proportional") {
            out = GrowthMode::Proportional;
            return true;
        }
        return false;
    }

    // Coeficiente del intento siguiente en modo Proportional (la estrategia lo usa en vez de `c * growth_factor`).
    // c: coeficiente del intento que termino; results: resultados de su join final (< upper_bound).
    inline uint64_t proportional_next_coefficient(uint64_t c, uint32_t growth_factor, uint64_t upper_bound, uint64_t results) {
        double min_k = growth_factor < 2 ? 2.0 : (double)growth_factor;
        double k = GROWTH_ZERO;
        if (results > 0) {
            k = (double)upper_bound / (double)results * GROWTH_SAFETY;
            if (k < min_k)
                k = min_k;
            if (k > GROWTH_MAX)
                k = GROWTH_MAX;
        } else if (k < min_k) {
            k = min_k;
        }
        double next = std::ceil((double)c * k);
        if (next >= 1.0e18)
            return (uint64_t)1.0e18;
        return (uint64_t)next;
    }

} // namespace gdj

#endif // QDAGS_STRATEGY_COEFFICIENT_GROWTH_HPP
