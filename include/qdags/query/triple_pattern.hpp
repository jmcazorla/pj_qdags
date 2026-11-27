/*
 * Portado de include/triple_pattern.hpp (repo qdags_gdj).
 * Copyright (C) 2020 Author removed for double-blind evaluation
 *
 * This is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * NOTA: cabecera GPLv3 original (adaptado de un motor RDF distinto al
 * k2-tree de se_quadtree.hpp -- ver README, seccion de licencia: esta
 * libreria ya tiene mas de una fuente GPLv3 incorporada).
 */

#ifndef QDAGS_TRIPLE_PATTERN_HPP
#define QDAGS_TRIPLE_PATTERN_HPP

#include <unordered_map>
#include <string>
#include <cstdint>
#include <iostream>
#include <vector>
#include <limits>
#include "../index/commun.hpp"

// Patron de tripleta RDF (S, P, O) y consulta BGP.

namespace gdj {

    struct term_pattern {
        uint64_t value;
        bool is_variable;
    };

    // Cada termino es constante o variable (id propio de la query). att_consts: posiciones {0=S,1=P,2=O}
    // constantes, que no entran en el att_set del qdag.
    struct triple_pattern {
        term_pattern term_s;
        term_pattern term_p;
        term_pattern term_o;
        uint8_t n_variables = 0;
        att_set att_consts;

        void const_s(uint64_t s) {
            term_s.is_variable = false;
            term_s.value = s;
            att_consts.push_back(0);
        }

        void const_o(uint64_t o) {
            term_o.is_variable = false;
            term_o.value = o;
            att_consts.push_back(2);
        }

        void const_p(uint64_t p) {
            term_p.is_variable = false;
            term_p.value = p;
            att_consts.push_back(1);
        }

        uint64_t get_var_s() const { return term_s.value; }
        uint64_t get_var_p() const { return term_p.value; }
        uint64_t get_var_o() const { return term_o.value; }

        void var_s(uint64_t s) {
            term_s.is_variable = true;
            term_s.value = s;
            n_variables++;
        }

        void var_o(uint64_t o) {
            term_o.is_variable = true;
            term_o.value = o;
            n_variables++;
        }

        void var_p(uint64_t p) {
            term_p.is_variable = true;
            term_p.value = p;
            n_variables++;
        }

        bool s_is_variable() const { return term_s.is_variable; }
        bool p_is_variable() const { return term_p.is_variable; }
        bool o_is_variable() const { return term_o.is_variable; }

        std::vector<uint64_t> get_constants() const {
            std::vector<uint64_t> consts;
            if (!s_is_variable())
                consts.push_back(term_s.value);
            if (!p_is_variable())
                consts.push_back(term_p.value);
            if (!o_is_variable())
                consts.push_back(term_o.value);
            return consts;
        }

        void print(std::unordered_map<uint8_t, std::string> &ht) const {
            if (s_is_variable())
                std::cout << "?" << ht[term_s.value] << " ";
            else
                std::cout << term_s.value << " ";

            if (p_is_variable())
                std::cout << "?" << ht[term_p.value] << " ";
            else
                std::cout << term_p.value << " ";

            if (o_is_variable())
                std::cout << "?" << ht[term_o.value];
            else
                std::cout << term_o.value;
        }

        void print() const {
            if (s_is_variable())
                std::cout << "?v" << term_s.value << " ";
            else
                std::cout << term_s.value << " ";

            if (p_is_variable())
                std::cout << "?v" << term_p.value << " ";
            else
                std::cout << term_p.value << " ";

            if (o_is_variable())
                std::cout << "?v" << term_o.value;
            else
                std::cout << term_o.value;
        }
    };

    using query_tp = std::vector<gdj::triple_pattern>;

    enum class TermType { S, P, O };

    inline std::ostream& operator<<(std::ostream& os, const gdj::TermType& t) {
        switch (t) {
            case gdj::TermType::S:
                os << "S";
                break;
            case gdj::TermType::P:
                os << "P";
                break;
            case gdj::TermType::O:
                os << "O";
                break;
        }
        return os;
    }

    // Variable fisica, patrones en que aparece y costo (y tipo S/O) de su proyeccion unaria mas barata.
    struct VarInfo {
        uint16_t id;
        std::vector<size_t> triple_indices;
        size_t min_unary_cost = std::numeric_limits<size_t>::max();
        TermType type;
    };

}

#endif // QDAGS_TRIPLE_PATTERN_HPP
