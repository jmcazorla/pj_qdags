#ifndef QDAGS_QUERY_CLASSIFY_QUERY_HPP
#define QDAGS_QUERY_CLASSIFY_QUERY_HPP

#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <ostream>
#include <string>
#include <cstdint>

#include "triple_pattern.hpp"

// Clasifica la topologia de una query BGP (por grado de entrada/salida de sus variables) para elegir su plan.

#define QDAGS_QUERY_TOPOLOGY_LIST \
    X(SINGLE_PATTERN) \
    X(UNKNOWN_OR_ERROR) \
    X(P2) X(Ti2) X(T2) \
    X(P3) X(T3) X(Ti3) X(J3) X(Tr1) X(Tr2) \
    X(P4) X(T4) X(Ti4) X(J4) \
    X(S1) X(S2) X(S3) X(S4)

namespace gdj {

    enum class QueryTopology {
        #define X(name) name,
            QDAGS_QUERY_TOPOLOGY_LIST
        #undef X
    };

    inline const char* to_string(QueryTopology t) {
        switch (t) {
            #define X(name) case QueryTopology::name: return #name;
                    QDAGS_QUERY_TOPOLOGY_LIST
            #undef X
        }
        return "UNKNOWN";
    }

    inline std::ostream& operator<<(std::ostream& os, QueryTopology t) {
        return os << to_string(t);
    }

    // Inverso de to_string(); UNKNOWN_OR_ERROR si el string no coincide con ningun valor.
    inline QueryTopology parse_query_topology(const std::string& s) {
        #define X(name) if (s == #name) return QueryTopology::name;
            QDAGS_QUERY_TOPOLOGY_LIST
        #undef X
        return QueryTopology::UNKNOWN_OR_ERROR;
    }

    // Solo reconoce queries donde todos los patrones tienen S y O variables; con una constante en S u O da
    // UNKNOWN_OR_ERROR.
    inline QueryTopology classify_query_topology(const std::vector<triple_pattern>& query_tp) {
        if (query_tp.empty())
            return QueryTopology::UNKNOWN_OR_ERROR;
        if (query_tp.size() == 1)
            return QueryTopology::SINGLE_PATTERN;

        std::unordered_map<uint8_t, int> in_degree;
        std::unordered_map<uint8_t, int> out_degree;
        std::unordered_set<uint8_t> variable_nodes;

        for (const auto& tp : query_tp) {
            bool s_is_var = tp.s_is_variable();
            bool o_is_var = tp.o_is_variable();
            uint8_t s_id = (uint8_t)tp.get_var_s();
            uint8_t o_id = (uint8_t)tp.get_var_o();
            if (s_is_var)
                variable_nodes.insert(s_id);
            if (o_is_var)
                variable_nodes.insert(o_id);

            if (s_is_var && o_is_var) {
                out_degree[s_id]++;
                in_degree[o_id]++;
            } else {
                return QueryTopology::UNKNOWN_OR_ERROR;
            }
        }

        size_t num_nodes = variable_nodes.size();
        size_t num_triples = query_tp.size();
        uint8_t n_double = 0;

        if (num_triples == 2 && num_nodes == 3) {
            for (uint8_t node_id : variable_nodes) {
                if (out_degree[node_id] == 2)
                    return QueryTopology::T2;
                if (in_degree[node_id] == 2)
                    return QueryTopology::Ti2;
            }
            return QueryTopology::P2;
        } else if (num_triples == 3) {
            bool out_deg_2 = false;
            bool in_deg_2 = false;
            n_double = 0;
            for (uint8_t node_id : variable_nodes) {
                uint8_t in_deg = (uint8_t)in_degree[node_id];
                uint8_t out_deg = (uint8_t)out_degree[node_id];
                if (out_deg == 3)
                    return QueryTopology::T3;
                if (in_deg == 3)
                    return QueryTopology::Ti3;
                if (out_deg == 1 && in_deg == 2)
                    return QueryTopology::J3;
                if (out_deg == 1 && in_deg == 1)
                    n_double++;
                if (out_deg == 2)
                    out_deg_2 = true;
                if (in_deg == 2)
                    in_deg_2 = true;
            }
            if (num_nodes == 4 && n_double == 2)
                return QueryTopology::P3;
            if (n_double == 3)
                return QueryTopology::Tr1;
            if (in_deg_2 && out_deg_2)
                return QueryTopology::Tr2;
        } else if (num_triples == 4 && num_nodes == 5) {
            n_double = 0;
            for (uint8_t node_id : variable_nodes) {
                uint8_t in_deg = (uint8_t)in_degree[node_id];
                uint8_t out_deg = (uint8_t)out_degree[node_id];
                if (out_deg == 4)
                    return QueryTopology::T4;
                if (in_deg == 4)
                    return QueryTopology::Ti4;
                if (out_deg == 2 && in_deg == 2)
                    return QueryTopology::J4;
                if (out_deg == 1 && in_deg == 1)
                    n_double++;
            }
            if (n_double == 3)
                return QueryTopology::P4;
        } else if (num_triples == 4 && num_nodes == 4) {
            uint8_t n_double_in = 0;
            uint8_t n_double_out = 0;
            for (uint8_t node_id : variable_nodes) {
                if (out_degree[node_id] == 2)
                    n_double_out++;
                if (in_degree[node_id] == 2)
                    n_double_in++;
            }
            if (n_double_in == 2 && n_double_out == 2) {
                return QueryTopology::S2;
            } else if (n_double_in == 1 && n_double_out == 1) {
                for (const auto& tp : query_tp) {
                    uint8_t s_id = (uint8_t)tp.get_var_s();
                    uint8_t o_id = (uint8_t)tp.get_var_o();
                    if ((in_degree[s_id] == 2 && out_degree[o_id] == 2) ||
                        (out_degree[s_id] == 2 && in_degree[o_id] == 2)) {
                        return QueryTopology::S3;
                    }
                }
                return QueryTopology::S4;
            } else {
                return QueryTopology::S1;
            }
        }

        return QueryTopology::UNKNOWN_OR_ERROR;
    }

} // namespace gdj

#endif // QDAGS_QUERY_CLASSIFY_QUERY_HPP
