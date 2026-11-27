#ifndef QDAGS_EXAMPLES_MXM_FINAL_HPP
#define QDAGS_EXAMPLES_MXM_FINAL_HPP

#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

#include "qdags/query/triple_pattern.hpp"

// --mxm de run_nl/run_wk/run_leapfrog/run_kernel: la query es una cadena de matrices A_1 x ... x A_m y se mide
// C(a_1, a_{m+1}) = pi_{a_1,a_{m+1}}(A_1 JOIN ... JOIN A_m), no el join completo. Los extremos son las variables
// lonely; las demas (non-lonely) se proyectan fuera.
// El resultado tiene una dimension por variable, en orden creciente de ids, asi que la posicion de una variable
// es su rango entre los ids.
// strategy_plan hace lo mismo con --k, pero pasa ese k como base del arbol proyectado; aca la base es la del resultado.

namespace gdj_examples {

    // Celdas (extremo, extremo) distintas del resultado de una cadena. `result` nulo (timeout) cuenta 0.
    template<class SeQuadT>
    uint64_t mxm_projected_count(const gdj::query_tp &query, SeQuadT *result) {
        if (!result)
            return 0;

        std::map<uint16_t, int> freq;
        for (const auto &tp : query) {
            if (tp.p_is_variable() || !tp.s_is_variable() || !tp.o_is_variable())
                continue;
            freq[(uint16_t)tp.get_var_s()]++;
            if (tp.get_var_o() != tp.get_var_s())
                freq[(uint16_t)tp.get_var_o()]++;
        }

        std::vector<uint16_t> remove_pos;
        uint16_t pos = 0;
        for (const auto &v : freq) {   // std::map: orden creciente de ids
            if (v.second > 1)
                remove_pos.push_back(pos);
            ++pos;
        }

        if ((size_t)result->getD() != freq.size())
            throw std::runtime_error("--mxm: result has no dimension per query variable");
        if (freq.size() - remove_pos.size() != 2)
            throw std::runtime_error("--mxm: query is not a matrix chain (exactly 2 lonely variables expected)");

        SeQuadT proj(result, remove_pos, result->getK(), 2, result->getHeight());
        return proj.total_ones_last_level();
    }

}

#endif
