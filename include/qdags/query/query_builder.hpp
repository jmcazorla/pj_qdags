#ifndef QDAGS_QUERY_BUILDER_HPP
#define QDAGS_QUERY_BUILDER_HPP

#include <vector>
#include <set>
#include <map>
#include <unordered_map>
#include <cstdint>

#include "../index/commun.hpp"
#include "../index/qdag.hpp"
#include "triple_pattern.hpp"
#include "resource_manager.hpp"

// Metadata de una query BGP de solo variables (P constante, S y O variables) para las estrategias Orig, NonLonely
// (One-Shot, Kernel) y WeakKernel. Los patrones con S/O constante se ignoran en silencio.

namespace gdj {

    // Relaciones SO de una query y el mapeo de atributos para extenderlas al espacio global.
    template<class SeQuadT>
    struct query_relations {
        std::vector<SeQuadT*> quadtrees;  // una por triple pattern resuelto (P const, S/O var)
        std::vector<att_set> att_list;    // {var_s, var_o} por quadtree, mismo orden que quadtrees
        att_set att_ext;                  // union ordenada de todas las variables de la query
        wide_base_config wide;            // como extienden los qdag sus bases
    };

    // Resuelve cada patron a su relacion S-O (get_so) y arma att_ext; descarta los de P variable o S/O constante.
    template<class SeQuadT>
    query_relations<SeQuadT> build_qdags_from_query(const query_tp& query, resource_manager<SeQuadT>& res,
                                                    const wide_base_config& wcfg = wide_base_config()) {
        query_relations<SeQuadT> out;
        out.wide = wcfg;
        std::set<uint16_t> unique_ext;

        for (const auto& tp : query) {
            if (tp.p_is_variable() || !tp.s_is_variable() || !tp.o_is_variable())
                continue;

            uint64_t pid = tp.get_var_p();
            uint16_t vs = (uint16_t)tp.get_var_s();
            uint16_t vo = (uint16_t)tp.get_var_o();

            unique_ext.insert(vs);
            unique_ext.insert(vo);

            if (auto* qt = res.get_so(pid)) {
                out.quadtrees.push_back(qt);
                out.att_list.push_back({vs, vo});
            }
        }

        out.att_ext.assign(unique_ext.begin(), unique_ext.end());
        return out;
    }

    // Un qdag por relacion, extendido al mismo att_ext con su entrada de att_list. Ownership del caller.
    template<class RankBackend>
    std::vector<qdag<RankBackend>*> make_qdags(const query_relations<se_quadtree<RankBackend>>& rel, uint64_t grid_side) {
        std::vector<qdag<RankBackend>*> qdags;
        qdags.reserve(rel.quadtrees.size());
        for (size_t i = 0; i < rel.quadtrees.size(); ++i) {
            att_set base = rel.att_list[i];  // el ctor de qdag toma att_set& (no const)
            qdags.push_back(new qdag<RankBackend>(rel.quadtrees[i], base, grid_side, rel.att_ext, rel.wide));
        }
        return qdags;
    }

    // NonLonely: separa las variables en non-lonely (>=2 patrones: se pre-unen en un espacio local renumerado) y lonely.
    template<class SeQuadT>
    struct query_metadata_nl {
        std::vector<SeQuadT*> nl_quadtrees;    // relaciones SO/S/O de variables non-lonely
        std::vector<SeQuadT*> l_quadtrees;     // relaciones SO de patrones con alguna variable lonely
        std::vector<att_set> att_list_nl;      // mapeo local->espacio nl, por nl_quadtree
        std::vector<att_set> att_list;         // mapeo local->espacio global, por l_quadtree
        att_set att_nl_orig;                   // ids fisicos de las non-lonely, en el orden renumerado
        att_set att_nl;                        // espacio local {0..att_nl_orig.size()-1}
        att_set att_ext;                       // espacio global
        wide_base_config wide;                 // como extienden los qdag sus bases
    };

    template<class SeQuadT>
    query_metadata_nl<SeQuadT> build_nl_metadata(const query_tp& query, resource_manager<SeQuadT>& res,
                                                 const wide_base_config& wcfg = wide_base_config()) {
        query_metadata_nl<SeQuadT> meta;
        meta.wide = wcfg;

        std::unordered_map<uint64_t, int> freq;
        for (const auto& tp : query) {
            if (tp.p_is_variable())
                continue;
            if (tp.s_is_variable())
                freq[tp.get_var_s()]++;
            if (tp.o_is_variable())
                freq[tp.get_var_o()]++;
        }

        std::vector<att_set> att_nl_aux;
        std::set<uint16_t> unique_ext;

        for (const auto& tp : query) {
            if (tp.p_is_variable())
                continue;
            uint64_t pid = tp.get_var_p();
            uint64_t vs = tp.get_var_s();
            uint64_t vo = tp.get_var_o();

            bool s_nl = freq[vs] > 1;
            bool o_nl = freq[vo] > 1;

            if (s_nl && o_nl) {
                if (auto* qt = res.get_so(pid)) {
                    meta.nl_quadtrees.push_back(qt);
                    att_nl_aux.push_back({(uint16_t)vs, (uint16_t)vo});
                }
            } else if (s_nl) {
                if (auto* qt = res.get_s(pid)) {
                    meta.nl_quadtrees.push_back(qt);
                    att_nl_aux.push_back({(uint16_t)vs});
                }
                if (auto* qt = res.get_so(pid)) {
                    meta.l_quadtrees.push_back(qt);
                    meta.att_list.push_back({(uint16_t)vs, (uint16_t)vo});
                }
            } else if (o_nl) {
                if (auto* qt = res.get_o(pid)) {
                    meta.nl_quadtrees.push_back(qt);
                    att_nl_aux.push_back({(uint16_t)vo});
                }
                if (auto* qt = res.get_so(pid)) {
                    meta.l_quadtrees.push_back(qt);
                    meta.att_list.push_back({(uint16_t)vs, (uint16_t)vo});
                }
            } else {
                // Las dos son lonely: no entra a ningun prejoin, pero sus variables estan en att_ext y su
                // restriccion tiene que estar en el join final.
                if (auto* qt = res.get_so(pid)) {
                    meta.l_quadtrees.push_back(qt);
                    meta.att_list.push_back({(uint16_t)vs, (uint16_t)vo});
                }
            }
            unique_ext.insert((uint16_t)vs);
            unique_ext.insert((uint16_t)vo);
        }

        std::unordered_map<uint16_t, uint16_t> old_to_new;
        uint16_t next_id = 0;
        for (const auto& vec : att_nl_aux) {
            att_set new_vec;
            for (uint16_t val : vec) {
                if (old_to_new.find(val) == old_to_new.end()) {
                    old_to_new[val] = next_id++;
                    meta.att_nl_orig.push_back(val);
                }
                new_vec.push_back(old_to_new[val]);
            }
            meta.att_list_nl.push_back(new_vec);
        }
        // next_id == 0 (sin non-lonely): sin la guarda, next_id - 1 da underflow y el for no terminaria.
        if (next_id > 0) {
            uint16_t att_max_nl = next_id - 1;
            for (uint16_t i = 0; i <= att_max_nl; ++i)
                meta.att_nl.push_back(i);
        }
        meta.att_ext.assign(unique_ext.begin(), unique_ext.end());

        return meta;
    }

    // WeakKernel: variables kernel (non-lonely) con sus proyecciones unarias, y la relacion binaria de cada
    // patron. Base de strategy_weak_kernel.
    template<class SeQuadT>
    struct query_metadata_wk {
        struct kernel_var {
            uint16_t phys_id;
            std::vector<SeQuadT*> unary_qt;  // get_s()/get_o() de cada patron que contiene esta variable
        };
        struct pattern_data {
            SeQuadT* binary_qt = nullptr;     // relacion SO del patron
            uint16_t phys_s;
            uint16_t phys_o;
            std::vector<std::pair<size_t, uint8_t>> kernel_refs;  // (indice en kernel_vars, posicion local {S=0,O=1})
        };

        std::vector<kernel_var> kernel_vars;
        std::vector<pattern_data> patterns;
        att_set att_ext;  // espacio global: ids fisicos de todas las variables, ordenados
        wide_base_config wide;  // como extienden los qdag sus bases
    };

    template<class SeQuadT>
    query_metadata_wk<SeQuadT> build_wk_metadata(const query_tp& query, resource_manager<SeQuadT>& res,
                                                 const wide_base_config& wcfg = wide_base_config()) {
        query_metadata_wk<SeQuadT> meta;
        meta.wide = wcfg;

        std::unordered_map<uint16_t, int> freq;
        std::set<uint16_t> all_vars;
        for (const auto& tp : query) {
            if (tp.p_is_variable())
                continue;
            uint16_t vs = (uint16_t)tp.get_var_s();
            uint16_t vo = (uint16_t)tp.get_var_o();
            freq[vs]++;
            freq[vo]++;
            all_vars.insert(vs);
            all_vars.insert(vo);
        }

        meta.att_ext.assign(all_vars.begin(), all_vars.end());

        std::unordered_map<uint16_t, size_t> phys_to_kidx;
        for (uint16_t phys_id : all_vars) {
            if (freq[phys_id] > 1) {
                size_t k_idx = meta.kernel_vars.size();
                phys_to_kidx[phys_id] = k_idx;
                meta.kernel_vars.push_back({phys_id, {}});
            }
        }

        for (const auto& tp : query) {
            if (tp.p_is_variable())
                continue;
            uint64_t pid = tp.get_var_p();
            uint16_t vs = (uint16_t)tp.get_var_s();
            uint16_t vo = (uint16_t)tp.get_var_o();

            typename query_metadata_wk<SeQuadT>::pattern_data pd;
            pd.binary_qt = res.get_so(pid);
            pd.phys_s = vs;
            pd.phys_o = vo;

            if (freq[vs] > 1) {
                size_t k_idx = phys_to_kidx[vs];
                if (auto* qt = res.get_s(pid)) {
                    meta.kernel_vars[k_idx].unary_qt.push_back(qt);
                    pd.kernel_refs.push_back({k_idx, 0u});
                }
            }
            if (freq[vo] > 1) {
                size_t k_idx = phys_to_kidx[vo];
                if (auto* qt = res.get_o(pid)) {
                    meta.kernel_vars[k_idx].unary_qt.push_back(qt);
                    pd.kernel_refs.push_back({k_idx, 1u});
                }
            }

            meta.patterns.push_back(std::move(pd));
        }

        return meta;
    }

}

#endif // QDAGS_QUERY_BUILDER_HPP
