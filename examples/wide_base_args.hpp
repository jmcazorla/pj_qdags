#ifndef QDAGS_EXAMPLES_WIDE_BASE_ARGS_HPP
#define QDAGS_EXAMPLES_WIDE_BASE_ARGS_HPP

#include <cstdlib>
#include <iostream>
#include <string>

#include "qdags/index/wide_base_config.hpp"

// Argumentos comunes a los run_*.cpp para el camino de dispersion de qdag (wide_base_config.hpp):
// --wide_base automatic|tables|scatter, --wide_min_d <n> (default 4), --wide_min_dim <n> (default 7);
// los dos ultimos solo con scatter.

namespace gdj_examples {

    constexpr const char* WIDE_BASE_USAGE =
        " [--wide_base automatic|tables|scatter] [--wide_min_d <n>] [--wide_min_dim <n>]";

    // Consume argv[i] (y su valor) si es un argumento de dispersion. 0: no es de este grupo; 1: consumido;
    // -1: valor invalido (el mensaje ya se imprimio).
    inline int parse_wide_base_arg(int argc, char** argv, int& i, gdj::wide_base_config& w) {
        const std::string arg = argv[i];
        if (arg != "--wide_base" && arg != "--wide_min_d" && arg != "--wide_min_dim")
            return 0;
        if (i + 1 >= argc) {
            std::cerr << "Error: " << arg << " needs a value" << std::endl;
            return -1;
        }
        const std::string v = argv[++i];
        if (arg == "--wide_base") {
            if (!gdj::parse_wide_base_mode(v, w.mode)) {
                std::cerr << "Error: --wide_base must be automatic|tables|scatter (got: " << v << ")" << std::endl;
                return -1;
            }
            return 1;
        }
        char* end = nullptr;
        const unsigned long n = std::strtoul(v.c_str(), &end, 10);
        if (v.empty() || *end != '\0' || v[0] == '-' || n > 64) {
            std::cerr << "Error: " << arg << " must be an integer between 0 and 64 (got: " << v << ")" << std::endl;
            return -1;
        }
        (arg == "--wide_min_d" ? w.min_d : w.min_dim) = (unsigned)n;
        return 1;
    }

    // Texto de una linea para el banner de configuracion.
    inline std::string wide_base_to_string(const gdj::wide_base_config& w) {
        std::string s = gdj::to_string(w.mode);
        if (w.mode == gdj::wide_base_mode::scatter)
            s += " (d>=" + std::to_string(w.min_d) + ", dim>=" + std::to_string(w.min_dim) + ")";
        return s;
    }

} // namespace gdj_examples

#endif // QDAGS_EXAMPLES_WIDE_BASE_ARGS_HPP
