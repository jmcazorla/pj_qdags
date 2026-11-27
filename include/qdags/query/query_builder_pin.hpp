#ifndef QDAGS_QUERY_BUILDER_PIN_HPP
#define QDAGS_QUERY_BUILDER_PIN_HPP

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "../index/commun.hpp"
#include "../index/qdag_pin.hpp"
#include "constant_terms.hpp"
#include "leapfrog_builder.hpp"
#include "query_builder.hpp"
#include "resource_manager.hpp"
#include "triple_pattern.hpp"

// Metadata para queries con patrones de sujeto u objeto constante, (?x, p, c) y (c, p, ?y): version con constantes
// de build_qdags_from_query / build_nl_metadata / build_wk_metadata y build_leapfrog_metadata. Devuelven los mismos
// tipos, asi que las estrategias se instancian con QdagT = qdag_pin<RankBackend>.

namespace gdj {

    namespace pin_detail {
        // Patron de predicado constante: binario (dos variables) o fijado (una variable y una constante).
        struct term_view {
            bool binary;        // S y O variables
            uint64_t pid;
            uint16_t vs, vo;    // binario: las dos variables
            uint16_t v;         // fijado: la variable libre
            pin_attr attr;      // fijado: que atributo es la constante
            uint64_t value;     // fijado: la constante
        };

        // false: el patron se ignora (predicado variable, o S y O constantes).
        inline bool view_of(const triple_pattern& tp, term_view& out) {
            if (tp.p_is_variable())
                return false;
            const bool sv = tp.s_is_variable(), ov = tp.o_is_variable();
            if (!sv && !ov)
                return false;
            out.pid = tp.get_var_p();
            out.binary = sv && ov;
            if (out.binary) {
                out.vs = (uint16_t)tp.get_var_s();
                out.vo = (uint16_t)tp.get_var_o();
            } else if (sv) {                 // (?x, p, c): se fija el objeto
                out.v = (uint16_t)tp.get_var_s();
                out.attr = pin_attr::object;
                out.value = tp.get_var_o();
            } else {                         // (c, p, ?y): se fija el sujeto
                out.v = (uint16_t)tp.get_var_o();
                out.attr = pin_attr::subject;
                out.value = tp.get_var_s();
            }
            return true;
        }
    }

    // ---------------------------------------------------------------- Orig --

    template<class RankBackend>
    query_relations<se_quadtree<RankBackend>> build_qdags_from_query_pin(
            const query_tp& query, resource_manager<se_quadtree<RankBackend>>& res, pin_scope<RankBackend>& pins,
            const wide_base_config& wcfg = wide_base_config()) {
        using SeQuadT = se_quadtree<RankBackend>;
        query_relations<SeQuadT> out;
        out.wide = wcfg;
        std::set<uint16_t> unique_ext;

        for (const auto& tp : query) {
            pin_detail::term_view t;
            if (!pin_detail::view_of(tp, t))
                continue;
            SeQuadT* qt = res.get_so(t.pid);
            if (t.binary) {
                unique_ext.insert(t.vs);
                unique_ext.insert(t.vo);
                if (qt) {
                    out.quadtrees.push_back(qt);
                    out.att_list.push_back({t.vs, t.vo});
                }
            } else {
                unique_ext.insert(t.v);
                if (qt) {
                    out.quadtrees.push_back(pins.make(qt, t.attr, t.value));
                    out.att_list.push_back({t.v});
                }
            }
        }

        out.att_ext.assign(unique_ext.begin(), unique_ext.end());
        return out;
    }

    // ------------------------------------------------- One-Shot y Kernel --

    template<class RankBackend>
    query_metadata_nl<se_quadtree<RankBackend>> build_nl_metadata_pin(
            const query_tp& query, resource_manager<se_quadtree<RankBackend>>& res, pin_scope<RankBackend>& pins,
            const wide_base_config& wcfg = wide_base_config()) {
        using SeQuadT = se_quadtree<RankBackend>;
        query_metadata_nl<SeQuadT> meta;
        meta.wide = wcfg;

        std::vector<pin_detail::term_view> terms;
        std::unordered_map<uint64_t, int> freq;
        for (const auto& tp : query) {
            pin_detail::term_view t;
            if (!pin_detail::view_of(tp, t))
                continue;
            terms.push_back(t);
            if (t.binary) {
                freq[t.vs]++;
                freq[t.vo]++;
            }
            else
                freq[t.v]++;
        }

        std::vector<att_set> att_nl_aux;
        std::set<uint16_t> unique_ext;

        for (const auto& t : terms) {
            if (!t.binary) {
                // Tabla unaria sobre t.v: va al prejoin si la variable es non-lonely, al join final si no.
                if (SeQuadT* qt = res.get_so(t.pid)) {
                    SeQuadT* token = pins.make(qt, t.attr, t.value);
                    if (freq[t.v] > 1) {
                        meta.nl_quadtrees.push_back(token);
                        att_nl_aux.push_back({t.v});
                    } else {
                        meta.l_quadtrees.push_back(token);
                        meta.att_list.push_back({t.v});
                    }
                }
                unique_ext.insert(t.v);
                continue;
            }

            const bool s_nl = freq[t.vs] > 1;
            const bool o_nl = freq[t.vo] > 1;

            if (s_nl && o_nl) {
                if (auto* qt = res.get_so(t.pid)) {
                    meta.nl_quadtrees.push_back(qt);
                    att_nl_aux.push_back({t.vs, t.vo});
                }
            } else if (s_nl) {
                if (auto* qt = res.get_s(t.pid)) {
                    meta.nl_quadtrees.push_back(qt);
                    att_nl_aux.push_back({t.vs});
                }
                if (auto* qt = res.get_so(t.pid)) {
                    meta.l_quadtrees.push_back(qt);
                    meta.att_list.push_back({t.vs, t.vo});
                }
            } else if (o_nl) {
                if (auto* qt = res.get_o(t.pid)) {
                    meta.nl_quadtrees.push_back(qt);
                    att_nl_aux.push_back({t.vo});
                }
                if (auto* qt = res.get_so(t.pid)) {
                    meta.l_quadtrees.push_back(qt);
                    meta.att_list.push_back({t.vs, t.vo});
                }
            } else {
                if (auto* qt = res.get_so(t.pid)) {
                    meta.l_quadtrees.push_back(qt);
                    meta.att_list.push_back({t.vs, t.vo});
                }
            }
            unique_ext.insert(t.vs);
            unique_ext.insert(t.vo);
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
        if (next_id > 0) {
            uint16_t att_max_nl = next_id - 1;
            for (uint16_t i = 0; i <= att_max_nl; ++i)
                meta.att_nl.push_back(i);
        }
        meta.att_ext.assign(unique_ext.begin(), unique_ext.end());

        return meta;
    }

    // ---------------------------------------------------------- WeakKernel --

    inline bool wk_pin_applicable(const query_tp& query) {
        std::set<uint16_t> all_vars, in_binary;
        for (const auto& tp : query) {
            pin_detail::term_view t;
            if (!pin_detail::view_of(tp, t))
                continue;
            if (t.binary) {
                all_vars.insert(t.vs);
                all_vars.insert(t.vo);
                in_binary.insert(t.vs);
                in_binary.insert(t.vo);
            }
            else
                all_vars.insert(t.v);
        }
        return !in_binary.empty() && in_binary.size() == all_vars.size();
    }

    template<class RankBackend>
    query_metadata_wk<se_quadtree<RankBackend>> build_wk_metadata_pin(
            const query_tp& query, resource_manager<se_quadtree<RankBackend>>& res, pin_scope<RankBackend>& pins,
            const wide_base_config& wcfg = wide_base_config()) {
        using SeQuadT = se_quadtree<RankBackend>;
        query_metadata_wk<SeQuadT> meta;
        meta.wide = wcfg;

        std::vector<pin_detail::term_view> terms;
        std::unordered_map<uint16_t, int> freq;
        std::set<uint16_t> all_vars;
        for (const auto& tp : query) {
            pin_detail::term_view t;
            if (!pin_detail::view_of(tp, t))
                continue;
            terms.push_back(t);
            if (t.binary) {
                freq[t.vs]++;
                freq[t.vo]++;
                all_vars.insert(t.vs);
                all_vars.insert(t.vo);
            }
            else {
                freq[t.v]++;
                all_vars.insert(t.v);
            }
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

        for (const auto& t : terms) {
            if (!t.binary) {
                // Proyeccion unaria (ya fijada) de la variable, si es kernel.
                if (freq[t.v] > 1)
                    if (SeQuadT* qt = res.get_so(t.pid))
                        meta.kernel_vars[phys_to_kidx[t.v]].unary_qt.push_back(pins.make(qt, t.attr, t.value));
                continue;
            }

            typename query_metadata_wk<SeQuadT>::pattern_data pd;
            pd.binary_qt = res.get_so(t.pid);
            pd.phys_s = t.vs;
            pd.phys_o = t.vo;

            if (freq[t.vs] > 1) {
                size_t k_idx = phys_to_kidx[t.vs];
                if (auto* qt = res.get_s(t.pid)) {
                    meta.kernel_vars[k_idx].unary_qt.push_back(qt);
                    pd.kernel_refs.push_back({k_idx, 0u});
                }
            }
            if (freq[t.vo] > 1) {
                size_t k_idx = phys_to_kidx[t.vo];
                if (auto* qt = res.get_o(t.pid)) {
                    meta.kernel_vars[k_idx].unary_qt.push_back(qt);
                    pd.kernel_refs.push_back({k_idx, 1u});
                }
            }

            meta.patterns.push_back(std::move(pd));
        }

        return meta;
    }

    // ------------------------------------------------------------ Leapfrog --

    // Mismo algoritmo que build_leapfrog_metadata.
    template<class RankBackend>
    query_metadata_lf<se_quadtree<RankBackend>> build_leapfrog_metadata_pin(
            const query_tp& query, resource_manager<se_quadtree<RankBackend>>& res, pin_scope<RankBackend>& pins,
            const leapfrog_config& cfg = leapfrog_config(), const wide_base_config& wcfg = wide_base_config()) {
        using SeQuadT = se_quadtree<RankBackend>;
        query_metadata_lf<SeQuadT> meta;
        meta.wide = wcfg;

        struct pattern {
            SeQuadT* so; SeQuadT* s; SeQuadT* o;
            uint16_t vs, vo;
        };
        struct pinned {
            SeQuadT* token;      // la tabla unaria (ficha)
            SeQuadT* loose;      // proyeccion SIN fijar: solo cota de tamano para el orden
            uint16_t v;
        };
        std::vector<pattern> patterns;
        std::vector<pinned> pinned_tabs;
        std::set<uint16_t> all_vars;

        for (const auto& tp : query) {
            pin_detail::term_view t;
            if (!pin_detail::view_of(tp, t))
                continue;
            if (t.binary) {
                all_vars.insert(t.vs);
                all_vars.insert(t.vo);
                if (auto* so = res.get_so(t.pid))
                    patterns.push_back({so, res.get_s(t.pid), res.get_o(t.pid), t.vs, t.vo});
            } else {
                all_vars.insert(t.v);
                if (auto* so = res.get_so(t.pid))
                    pinned_tabs.push_back({pins.make(so, t.attr, t.value),
                                           t.attr == pin_attr::object ? res.get_s(t.pid) : res.get_o(t.pid), t.v});
            }
        }

        meta.att_phys.assign(all_vars.begin(), all_vars.end());
        std::map<uint16_t, uint16_t> global_pos;
        for (uint16_t i = 0; i < meta.att_phys.size(); ++i) {
            global_pos[meta.att_phys[i]] = i;
            meta.att_ext.push_back(i);
        }

        auto unary_size = [](SeQuadT* qt) -> uint64_t {
            return qt->get_bv()[qt->getHeight() - 1].n_ones();
        };
        const uint64_t no_size = std::numeric_limits<uint64_t>::max();
        std::map<uint16_t, uint64_t> degree, proj_size;
        for (const auto& p : patterns) {
            degree[p.vs]++;
            if (p.vo != p.vs)
                degree[p.vo]++;
            if (!proj_size.count(p.vs))
                proj_size[p.vs] = no_size;
            if (!proj_size.count(p.vo))
                proj_size[p.vo] = no_size;
            if (p.s)
                proj_size[p.vs] = std::min<uint64_t>(proj_size[p.vs], unary_size(p.s));
            if (p.o)
                proj_size[p.vo] = std::min<uint64_t>(proj_size[p.vo], unary_size(p.o));
        }
        for (const auto& p : pinned_tabs) {
            degree[p.v]++;
            if (!proj_size.count(p.v))
                proj_size[p.v] = no_size;
            // Cota superior: la proyeccion sin fijar (el tamano real de la seleccion no se conoce).
            if (p.loose)
                proj_size[p.v] = std::min<uint64_t>(proj_size[p.v], unary_size(p.loose));
        }
        for (const auto& d : degree)
            if (d.second > 1)
                meta.n_non_lonely++;

        std::map<uint16_t, uint16_t> local_pos;
        size_t max_steps = degree.empty() ? 0 : degree.size() - 1;
        if (meta.n_non_lonely == 0)
            max_steps = 0;
        if (cfg.depth == LeapfrogDepth::NonLonely)
            max_steps = std::min(max_steps, meta.n_non_lonely);
        size_t att_cap = cfg.max_prejoin_atts > 0 ? cfg.max_prejoin_atts : leapfrog_auto_max_prejoin_atts(degree.size(), meta.n_non_lonely);
        max_steps = std::min(max_steps, std::min(att_cap, LEAPFROG_MAX_PREJOIN_ATTS));

        while (meta.order.size() < max_steps) {
            bool found = false;
            uint16_t best = 0;
            std::vector<uint64_t> best_key;
            bool best_lonely = false, best_connected = false;

            for (const auto& d : degree) {
                uint16_t v = d.first;
                if (local_pos.count(v))
                    continue;

                bool lonely = d.second <= 1;
                bool connected = meta.order.empty();
                for (const auto& p : patterns) {
                    if (connected)
                        break;
                    if (p.vs == v && local_pos.count(p.vo))
                        connected = true;
                    if (p.vo == v && local_pos.count(p.vs))
                        connected = true;
                }

                uint64_t size_key = proj_size[v];
                uint64_t degree_key = no_size - d.second;
                std::vector<uint64_t> key = {lonely ? 1u : 0u, connected ? 0u : 1u};
                if (cfg.order == LeapfrogOrder::Degree) {
                    key.push_back(degree_key);
                    key.push_back(size_key);
                }
                else                                    {
                    key.push_back(size_key);
                    key.push_back(degree_key);
                }
                key.push_back(v);

                if (!found || key < best_key) {
                    found = true; best = v; best_key = key;
                    best_lonely = lonely; best_connected = connected;
                }
            }

            if (!found || (best_lonely && !best_connected))
                break;

            local_pos[best] = (uint16_t)meta.order.size();
            meta.order.push_back(best);
        }

        bool over_approximated = false;
        for (size_t k = 0; k < meta.order.size(); ++k) {
            uint16_t x = meta.order[k];
            typename query_metadata_lf<SeQuadT>::step_data step;
            auto add_projection = [&](SeQuadT* unary) {
                if (!unary) {
                    over_approximated = true;
                    return;
                }
                step.qts.push_back(unary);
                step.atts.push_back({(uint16_t)k});
            };

            for (const auto& p : patterns) {
                if (p.vs != x && p.vo != x)
                    continue;
                if (p.vs == p.vo) {
                    add_projection(p.s);
                    add_projection(p.o);
                    over_approximated = true;
                    continue;
                }
                uint16_t other = (p.vs == x) ? p.vo : p.vs;
                auto it = local_pos.find(other);
                bool contained = (it != local_pos.end()) && (it->second <= k);

                if (contained) {
                    step.qts.push_back(p.so);
                    step.atts.push_back({local_pos[p.vs], local_pos[p.vo]});
                } else {
                    add_projection((p.vs == x) ? p.s : p.o);
                }
            }
            for (const auto& p : pinned_tabs)
                if (p.v == x)
                    add_projection(p.token);

            if (step.qts.empty()) {
                meta.order.resize(k);
                break;
            }
            step.exact = (k + 1 >= meta.n_non_lonely);
            meta.steps.push_back(std::move(step));
        }
        if (over_approximated)
            for (auto& step : meta.steps)
                step.exact = false;

        size_t n_steps = meta.steps.size();
        for (size_t k = 0; k < n_steps; ++k)
            meta.prefix_att_final.push_back(global_pos[meta.order[k]]);

        for (const auto& p : patterns) {
            auto its = local_pos.find(p.vs), ito = local_pos.find(p.vo);
            bool contained = p.vs != p.vo && its != local_pos.end() && ito != local_pos.end()
                          && its->second < n_steps && ito->second < n_steps;
            if (contained)
                continue;
            meta.final_qts.push_back(p.so);
            meta.final_atts.push_back({global_pos[p.vs], global_pos[p.vo]});
        }
        // Una tabla unaria cuya variable no quedo en el prefijo va al join final.
        for (const auto& p : pinned_tabs) {
            auto it = local_pos.find(p.v);
            if (it != local_pos.end() && it->second < n_steps)
                continue;
            meta.final_qts.push_back(p.token);
            meta.final_atts.push_back({global_pos[p.v]});
        }

        return meta;
    }

}

#endif // QDAGS_QUERY_BUILDER_PIN_HPP
