#ifndef QDAGS_QUERY_CONSTANT_TERMS_HPP
#define QDAGS_QUERY_CONSTANT_TERMS_HPP

#include <string>

#include "triple_pattern.hpp"

// Deteccion de patrones con sujeto u objeto constante, (?x, p, c) y (c, p, ?y). Header propio para no incluir el
// soporte de constantes (qdag_pin.hpp, query_builder_pin.hpp) solo por preguntar.

namespace gdj {

    // Algun patron de predicado constante tiene el sujeto o el objeto constante.
    inline bool has_constant_terms(const query_tp& query) {
        for (const auto& tp : query)
            if (!tp.p_is_variable() && (!tp.s_is_variable() || !tp.o_is_variable()))
                return true;
        return false;
    }

    // false si hay un patron que el soporte de constantes no cubre: sujeto y objeto constantes.
    inline bool pin_supported(const query_tp& query, std::string* why = nullptr) {
        for (const auto& tp : query) {
            if (tp.p_is_variable())
                continue;
            if (!tp.s_is_variable() && !tp.o_is_variable()) {
                if (why)
                    *why = "patron con sujeto y objeto constantes (comprobacion de existencia)";
                return false;
            }
        }
        return true;
    }

}

#endif // QDAGS_QUERY_CONSTANT_TERMS_HPP
