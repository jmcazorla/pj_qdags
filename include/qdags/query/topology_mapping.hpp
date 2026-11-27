#ifndef QDAGS_QUERY_TOPOLOGY_MAPPING_HPP
#define QDAGS_QUERY_TOPOLOGY_MAPPING_HPP

#include <iostream>
#include <vector>
#include <map>
#include <set>
#include <limits>
#include <algorithm>
#include <cstdint>

#include "triple_pattern.hpp"

// SquareMapping/TriangleMapping/PathMapping/StarMapping: a partir de una topologia ya clasificada
// (classify_query_topology), ordenan las variables por costo de su proyeccion unaria mas barata y les asignan los nombres a/b/c/(d/e)

namespace gdj {

    // Topologias S1-S4 (ciclo de 4 nodos a-b-c-d-a).
    struct SquareMapping {
        uint16_t a, b, c, d;

        size_t edge_ab_pid;
        std::map<uint16_t, TermType> ab_map;
        size_t edge_bc_pid;
        std::map<uint16_t, TermType> bc_map;
        size_t edge_cd_pid;
        std::map<uint16_t, TermType> cd_map;
        size_t edge_da_pid;
        std::map<uint16_t, TermType> da_map;

        bool is_valid = false;

        template<class ResMgrT>
        void build(const query_tp& query, ResMgrT& res) {
            if (query.size() != 4)
                return;

            std::map<uint16_t, VarInfo> var_map;
            std::set<uint16_t> vars_set;

            for (size_t i = 0; i < query.size(); ++i) {
                uint16_t s = (uint16_t)query[i].get_var_s();
                uint16_t o = (uint16_t)query[i].get_var_o();
                uint64_t p = query[i].get_var_p();

                vars_set.insert(s);
                vars_set.insert(o);
                var_map[s].id = s; var_map[s].triple_indices.push_back(i);
                var_map[o].id = o; var_map[o].triple_indices.push_back(i);

                size_t s_cost = res.get_s(p)->get_bv()[res.get_s(p)->getHeight() - 1].n_ones();
                size_t o_cost = res.get_o(p)->get_bv()[res.get_o(p)->getHeight() - 1].n_ones();

                if (s_cost < var_map[s].min_unary_cost) {
                    var_map[s].min_unary_cost = s_cost;
                    var_map[s].type = TermType::S;
                }
                if (o_cost < var_map[o].min_unary_cost) {
                    var_map[o].min_unary_cost = o_cost;
                    var_map[o].type = TermType::O;
                }
            }

            if (vars_set.size() != 4)
                return;

            size_t global_min = std::numeric_limits<size_t>::max();
            for (auto const& [id, info] : var_map) {
                if (info.min_unary_cost < global_min) {
                    global_min = info.min_unary_cost;
                    a = id;
                }
            }

            uint16_t n1 = (query[var_map[a].triple_indices[0]].get_var_s() == a) ?
                        (uint16_t)query[var_map[a].triple_indices[0]].get_var_o() : (uint16_t)query[var_map[a].triple_indices[0]].get_var_s();
            uint16_t n2 = (query[var_map[a].triple_indices[1]].get_var_s() == a) ?
                        (uint16_t)query[var_map[a].triple_indices[1]].get_var_o() : (uint16_t)query[var_map[a].triple_indices[1]].get_var_s();

            if (var_map[n1].min_unary_cost <= var_map[n2].min_unary_cost) {
                b = n1;
                edge_ab_pid = var_map[a].triple_indices[0];
                edge_da_pid = var_map[a].triple_indices[1];
            } else {
                b = n2;
                edge_ab_pid = var_map[a].triple_indices[1];
                edge_da_pid = var_map[a].triple_indices[0];
            }

            edge_bc_pid = (var_map[b].triple_indices[0] == edge_ab_pid) ? var_map[b].triple_indices[1] : var_map[b].triple_indices[0];
            c = (query[edge_bc_pid].get_var_s() == b) ? (uint16_t)query[edge_bc_pid].get_var_o() : (uint16_t)query[edge_bc_pid].get_var_s();

            edge_cd_pid = (var_map[c].triple_indices[0] == edge_bc_pid) ? var_map[c].triple_indices[1] : var_map[c].triple_indices[0];
            d = (query[edge_cd_pid].get_var_s() == c) ? (uint16_t)query[edge_cd_pid].get_var_o() : (uint16_t)query[edge_cd_pid].get_var_s();

            is_valid = true;

            for (size_t i = 0; i < query.size(); i++) {
                if (i == edge_ab_pid) {
                    ab_map[a] = a == query[i].get_var_s() ? TermType::S : TermType::O;
                    ab_map[b] = b == query[i].get_var_s() ? TermType::S : TermType::O;
                } else if (i == edge_bc_pid) {
                    bc_map[c] = c == query[i].get_var_s() ? TermType::S : TermType::O;
                    bc_map[b] = b == query[i].get_var_s() ? TermType::S : TermType::O;
                } else if (i == edge_cd_pid) {
                    cd_map[c] = c == query[i].get_var_s() ? TermType::S : TermType::O;
                    cd_map[d] = d == query[i].get_var_s() ? TermType::S : TermType::O;
                } else if (i == edge_da_pid) {
                    da_map[a] = a == query[i].get_var_s() ? TermType::S : TermType::O;
                    da_map[d] = d == query[i].get_var_s() ? TermType::S : TermType::O;
                }
            }
        }

        void print() const {
            if (!is_valid)
                return;
            std::cout << "[SquareMapping] Sequence: ?" << a << " -> ?" << b << " -> ?" << c << " -> ?" << d << " -> ?" << a << std::endl;
            std::cout << "  Edges: ab(idp=" << edge_ab_pid << "), bc(idp=" << edge_bc_pid << "), cd(idp=" << edge_cd_pid << "), da(idp=" << edge_da_pid << ")" << std::endl;
        }
    };

    // Topologias Tr1/Tr2 (triangulo a-b-c-a).
    struct TriangleMapping {
        uint16_t a, b, c;

        size_t edge_ab_pid;
        std::map<uint16_t, TermType> ab_map;
        size_t edge_bc_pid;
        std::map<uint16_t, TermType> bc_map;
        size_t edge_ca_pid;
        std::map<uint16_t, TermType> ca_map;

        bool is_valid = false;

        template<class ResMgrT>
        void build(const query_tp& query, ResMgrT& res) {
            if (query.size() != 3)
                return;

            std::map<uint16_t, VarInfo> var_map;
            std::set<uint16_t> vars_set;

            for (size_t i = 0; i < query.size(); ++i) {
                uint16_t s = (uint16_t)query[i].get_var_s();
                uint16_t o = (uint16_t)query[i].get_var_o();
                uint64_t p = query[i].get_var_p();

                vars_set.insert(s);
                vars_set.insert(o);
                var_map[s].id = s; var_map[s].triple_indices.push_back(i);
                var_map[o].id = o; var_map[o].triple_indices.push_back(i);

                size_t s_cost = res.get_s(p)->get_bv()[res.get_s(p)->getHeight() - 1].n_ones();
                size_t o_cost = res.get_o(p)->get_bv()[res.get_o(p)->getHeight() - 1].n_ones();

                if (s_cost < var_map[s].min_unary_cost) {
                    var_map[s].min_unary_cost = s_cost;
                    var_map[s].type = TermType::S;
                }
                if (o_cost < var_map[o].min_unary_cost) {
                    var_map[o].min_unary_cost = o_cost;
                    var_map[o].type = TermType::O;
                }
            }

            if (vars_set.size() != 3)
                return;

            size_t global_min = std::numeric_limits<size_t>::max();
            for (auto const& [id, info] : var_map) {
                if (info.min_unary_cost < global_min) {
                    global_min = info.min_unary_cost;
                    a = id;
                }
            }

            uint16_t n1 = (query[var_map[a].triple_indices[0]].get_var_s() == a) ?
                        (uint16_t)query[var_map[a].triple_indices[0]].get_var_o() : (uint16_t)query[var_map[a].triple_indices[0]].get_var_s();
            uint16_t n2 = (query[var_map[a].triple_indices[1]].get_var_s() == a) ?
                        (uint16_t)query[var_map[a].triple_indices[1]].get_var_o() : (uint16_t)query[var_map[a].triple_indices[1]].get_var_s();

            if (var_map[n1].min_unary_cost <= var_map[n2].min_unary_cost) {
                b = n1; c = n2;
                edge_ab_pid = var_map[a].triple_indices[0];
                edge_ca_pid = var_map[a].triple_indices[1];
            } else {
                b = n2; c = n1;
                edge_ab_pid = var_map[a].triple_indices[1];
                edge_ca_pid = var_map[a].triple_indices[0];
            }

            for (size_t i = 0; i < 3; i++) {
                if (i != edge_ab_pid && i != edge_ca_pid) {
                    edge_bc_pid = i;
                    break;
                }
            }

            auto fill_map = [&](std::map<uint16_t, TermType>& m, size_t pid, uint16_t v1, uint16_t v2) {
                m[v1] = (v1 == query[pid].get_var_s()) ? TermType::S : TermType::O;
                m[v2] = (v2 == query[pid].get_var_s()) ? TermType::S : TermType::O;
            };

            fill_map(ab_map, edge_ab_pid, a, b);
            fill_map(bc_map, edge_bc_pid, b, c);
            fill_map(ca_map, edge_ca_pid, c, a);

            is_valid = true;
        }

        void print() const {
            if (!is_valid)
                return;
            std::cout << "[TriangleMapping] Sequence: ?" << a << " -> ?" << b << " -> ?" << c << " -> ?" << a << std::endl;
            std::cout << "  Edges: ab(idp=" << edge_ab_pid << "), bc(idp=" << edge_bc_pid << "), ca(idp=" << edge_ca_pid << ")" << std::endl;
        }
    };

    // Topologias P2-P4 (camino a-b-c-(d)-(e)).
    struct PathMapping {
        uint16_t a, b, c, d, e;
        size_t num_edges = 0;

        size_t edge_ab_pid = 0;
        std::map<uint16_t, TermType> ab_map;
        size_t edge_bc_pid = 0;
        std::map<uint16_t, TermType> bc_map;
        size_t edge_cd_pid = 0;
        std::map<uint16_t, TermType> cd_map;
        size_t edge_de_pid = 0;
        std::map<uint16_t, TermType> de_map;

        bool is_valid = false;

        template<class ResMgrT>
        void build(const query_tp& query, ResMgrT& res) {
            num_edges = query.size();
            if (num_edges < 2 || num_edges > 4)
                return;

            std::map<uint16_t, std::vector<size_t>> adj;
            std::map<uint16_t, size_t> min_costs;
            std::set<uint16_t> vars;

            for (size_t i = 0; i < query.size(); ++i) {
                uint16_t s = (uint16_t)query[i].get_var_s();
                uint16_t o = (uint16_t)query[i].get_var_o();
                uint64_t p = query[i].get_var_p();

                adj[s].push_back(i);
                adj[o].push_back(i);
                vars.insert(s);
                vars.insert(o);

                size_t s_cost = res.get_s(p)->get_bv()[res.get_s(p)->getHeight() - 1].n_ones();
                size_t o_cost = res.get_o(p)->get_bv()[res.get_o(p)->getHeight() - 1].n_ones();

                if (min_costs.find(s) == min_costs.end() || s_cost < min_costs[s])
                    min_costs[s] = s_cost;
                if (min_costs.find(o) == min_costs.end() || o_cost < min_costs[o])
                    min_costs[o] = o_cost;
            }

            std::vector<uint16_t> ends;
            for (auto const& [var, edges] : adj) {
                if (edges.size() == 1)
                    ends.push_back(var);
            }

            if (ends.size() != 2)
                return;

            uint16_t start_node = (min_costs[ends[0]] <= min_costs[ends[1]]) ? ends[0] : ends[1];

            std::vector<uint16_t> sequence;
            std::vector<size_t> pids;
            std::set<size_t> visited_edges;
            uint16_t curr = start_node;

            sequence.push_back(curr);
            while (sequence.size() <= num_edges) {
                size_t next_edge_idx = 999;
                for (size_t edge_idx : adj[curr]) {
                    if (visited_edges.find(edge_idx) == visited_edges.end()) {
                        next_edge_idx = edge_idx;
                        break;
                    }
                }
                if (next_edge_idx == 999)
                    break;

                visited_edges.insert(next_edge_idx);
                pids.push_back(next_edge_idx);
                curr = (query[next_edge_idx].get_var_s() == curr) ? (uint16_t)query[next_edge_idx].get_var_o() : (uint16_t)query[next_edge_idx].get_var_s();
                sequence.push_back(curr);
            }

            a = sequence[0];
            b = sequence[1];
            edge_ab_pid = pids[0];

            c = sequence[2];
            edge_bc_pid = pids[1];

            if (num_edges >= 3) {
                d = sequence[3];
                edge_cd_pid = pids[2];
            }
            if (num_edges >= 4) {
                e = sequence[4];
                edge_de_pid = pids[3];
            }

            auto fill_map = [&](std::map<uint16_t, TermType>& m, size_t pid, uint16_t v1, uint16_t v2) {
                m[v1] = (v1 == query[pid].get_var_s()) ? TermType::S : TermType::O;
                m[v2] = (v2 == query[pid].get_var_s()) ? TermType::S : TermType::O;
            };

            fill_map(ab_map, edge_ab_pid, a, b);
            fill_map(bc_map, edge_bc_pid, b, c);
            if (num_edges >= 3)
                fill_map(cd_map, edge_cd_pid, c, d);
            if (num_edges >= 4)
                fill_map(de_map, edge_de_pid, d, e);

            is_valid = true;
        }

        void print() const {
            if (!is_valid)
                return;
            std::cout << "[PathMapping] Length " << num_edges << ": ?" << a << " -> ?" << b << " -> ?" << c;
            if (num_edges >= 3)
                std::cout << " -> ?" << d;
            if (num_edges >= 4)
                std::cout << " -> ?" << e;
            std::cout << std::endl;
        }
    };

    // Topologias T3-T4/Ti3-Ti4/J3-J4 (estrella, centro a, hojas b/c/d/(e)
    // ordenadas por costo).
    struct StarMapping {
        uint16_t a;
        uint16_t b = 0, c = 0, d = 0, e = 0;
        size_t num_edges = 0;

        size_t edge_ab_pid = 0;
        size_t edge_ac_pid = 0;
        size_t edge_ad_pid = 0;
        size_t edge_ae_pid = 0;

        std::map<uint16_t, TermType> ab_map;
        std::map<uint16_t, TermType> ac_map;
        std::map<uint16_t, TermType> ad_map;
        std::map<uint16_t, TermType> ae_map;

        bool is_valid = false;

        template<class ResMgrT>
        void build(const query_tp& query, ResMgrT& res) {
            num_edges = query.size();
            if (num_edges < 3 || num_edges > 4)
                return;

            std::map<uint16_t, std::vector<size_t>> adj;
            std::set<uint16_t> vars;

            for (size_t i = 0; i < query.size(); ++i) {
                uint16_t s = (uint16_t)query[i].get_var_s();
                uint16_t o = (uint16_t)query[i].get_var_o();
                adj[s].push_back(i);
                adj[o].push_back(i);
                vars.insert(s);
                vars.insert(o);
            }

            uint16_t center = 0;
            bool found_center = false;
            for (auto const& [var, edges] : adj) {
                if (edges.size() == num_edges) {
                    center = var;
                    found_center = true;
                    break;
                }
            }

            if (!found_center)
                return;
            a = center;

            struct LeafInfo {
                uint16_t id;
                size_t pid;
                size_t cost;
            };
            std::vector<LeafInfo> leaves;

            for (size_t pid : adj[a]) {
                uint16_t leaf_id = (query[pid].get_var_s() == a) ? (uint16_t)query[pid].get_var_o() : (uint16_t)query[pid].get_var_s();

                uint64_t p_val = query[pid].get_var_p();
                auto qt_leaf = (query[pid].get_var_s() == leaf_id) ? res.get_s(p_val) : res.get_o(p_val);
                size_t cost = qt_leaf->get_bv()[qt_leaf->getHeight() - 1].n_ones();

                leaves.push_back({leaf_id, pid, cost});
            }

            std::sort(leaves.begin(), leaves.end(), [](const LeafInfo& l1, const LeafInfo& l2) {
                return l1.cost < l2.cost;
            });

            b = leaves[0].id; edge_ab_pid = leaves[0].pid;
            c = leaves[1].id; edge_ac_pid = leaves[1].pid;
            d = leaves[2].id; edge_ad_pid = leaves[2].pid;

            if (num_edges == 4) {
                e = leaves[3].id; edge_ae_pid = leaves[3].pid;
            }

            auto fill_map = [&](std::map<uint16_t, TermType>& m, size_t pid, uint16_t v_hub, uint16_t v_leaf) {
                m[v_hub] = (v_hub == query[pid].get_var_s()) ? TermType::S : TermType::O;
                m[v_leaf] = (v_leaf == query[pid].get_var_s()) ? TermType::S : TermType::O;
            };

            fill_map(ab_map, edge_ab_pid, a, b);
            fill_map(ac_map, edge_ac_pid, a, c);
            fill_map(ad_map, edge_ad_pid, a, d);
            if (num_edges == 4)
                fill_map(ae_map, edge_ae_pid, a, e);

            is_valid = true;
        }

        void print() const {
            if (!is_valid)
                return;
            std::cout << "[StarMapping] Center: ?" << a << " | Leaves: ?" << b << ", ?" << c << ", ?" << d;
            if (num_edges == 4)
                std::cout << ", ?" << e;
            std::cout << " (ordered by cost)" << std::endl;
        }
    };

} // namespace gdj

#endif // QDAGS_QUERY_TOPOLOGY_MAPPING_HPP
