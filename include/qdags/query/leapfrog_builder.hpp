#ifndef QDAGS_QUERY_LEAPFROG_BUILDER_HPP
#define QDAGS_QUERY_LEAPFROG_BUILDER_HPP

#include <vector>
#include <set>
#include <map>
#include <string>
#include <limits>
#include <algorithm>
#include <cstdint>

#include "../index/commun.hpp"
#include "../index/wide_base_config.hpp"
#include "triple_pattern.hpp"
#include "resource_manager.hpp"

// Arma el plan Leapfrog de cualquier query BGP de solo variables, sin clasificar su topologia ni usar el catalogo
// de planes. Base de strategy_leapfrog.

namespace gdj {

    // Criterio principal del orden (el otro desempata). ProjSize: proyeccion unaria mas chica primero. Degree: mas patrones primero.
    enum class LeapfrogOrder : uint8_t { ProjSize = 0, Degree = 1 };

    // Largo de la cadena. Full: un paso por variable hasta dejar una fuera. NonLonely: solo las
    // non-lonely; las lonely entran juntas en el join final.
    enum class LeapfrogDepth : uint8_t { Full = 0, NonLonely = 1 };

    struct leapfrog_config {
        LeapfrogOrder order = LeapfrogOrder::ProjSize;
        LeapfrogDepth depth = LeapfrogDepth::Full;
        // Tope de atributos de un prejoin (el join final agrega el resto). 0 = automatico
        // (leapfrog_auto_max_prejoin_atts).
        size_t max_prejoin_atts = 0;
    };

    inline LeapfrogOrder parse_leapfrog_order(const std::string& s) {
        return (s == "degree") ? LeapfrogOrder::Degree : LeapfrogOrder::ProjSize;
    }

    inline LeapfrogDepth parse_leapfrog_depth(const std::string& s) {
        return (s == "non_lonely") ? LeapfrogDepth::NonLonely : LeapfrogDepth::Full;
    }

    inline const char* to_string(LeapfrogOrder o) {
        return o == LeapfrogOrder::Degree ? "degree" : "proj_size";
    }

    inline const char* to_string(LeapfrogDepth d) {
        return d == LeapfrogDepth::NonLonely ? "non_lonely" : "full";
    }

    // Un qdag extiende quadtrees de hasta 9 dimensiones (la de 9 solo dentro de un join de 10); la cadena deja
    // al menos una variable fuera, asi que alcanza para queries de hasta 10 variables.
    constexpr size_t LEAPFROG_MAX_PREJOIN_ATTS = 9;

    // Tope por defecto. Envolver un prejoin de 4 o mas atributos en un qdag tiene un costo fijo  que crece con el ancho del join.
    inline size_t leapfrog_auto_max_prejoin_atts(size_t n_vars, size_t n_non_lonely) {
        const size_t floor = n_vars <= 5 ? 4 : 3;
        return std::max(floor, n_non_lonely);
    }

    template<class SeQuadT>
    struct query_metadata_lf {
        struct step_data {
            std::vector<SeQuadT*> qts;  // tablas SO y proyecciones unarias de este paso
            std::vector<att_set> atts;  // posicion local de cada dimension de qts[i]
            // El prefijo contiene todas las non-lonely: el prejoin es la proyeccion exacta del resultado y admite cortarse a upper_bound.
            bool exact = false;
        };

        std::vector<uint16_t> order;      // ids fisicos; order[k] es la variable que agrega steps[k]
        size_t n_non_lonely = 0;          // variables non-lonely de la query (van primero en `order`)
        std::vector<step_data> steps;     // prejoins; steps[k] produce I sobre order[0..k]

        std::vector<SeQuadT*> final_qts;  // tablas SO no contenidas en el ultimo prefijo
        std::vector<att_set> final_atts;  // {pos_s, pos_o} en el espacio global, por final_qt
        att_set prefix_att_final;         // posicion global de order[0..steps.size()-1]

        att_set att_ext;                  // espacio global {0..d-1}
        att_set att_phys;                 // att_phys[i] = id fisico de la dimension i del resultado (ordenados)
        wide_base_config wide;            // como extienden los qdag sus bases
    };

    // `cfg` elige el orden y el largo de la cadena. Sin non-lonely no hay prejoins.
    template<class SeQuadT>
    query_metadata_lf<SeQuadT> build_leapfrog_metadata(const query_tp& query, resource_manager<SeQuadT>& res,
                                                       const leapfrog_config& cfg = leapfrog_config(),
                                                       const wide_base_config& wcfg = wide_base_config()) {
        query_metadata_lf<SeQuadT> meta;
        meta.wide = wcfg;

        struct pattern {
            SeQuadT* so; SeQuadT* s; SeQuadT* o;
            uint16_t vs, vo;
        };
        std::vector<pattern> patterns;
        std::set<uint16_t> all_vars;

        for (const auto& tp : query) {
            if (tp.p_is_variable() || !tp.s_is_variable() || !tp.o_is_variable())
                continue;
            uint64_t pid = tp.get_var_p();
            uint16_t vs = (uint16_t)tp.get_var_s();
            uint16_t vo = (uint16_t)tp.get_var_o();
            all_vars.insert(vs);
            all_vars.insert(vo);
            if (auto* so = res.get_so(pid))
                patterns.push_back({so, res.get_s(pid), res.get_o(pid), vs, vo});
        }

        meta.att_phys.assign(all_vars.begin(), all_vars.end());
        std::map<uint16_t, uint16_t> global_pos;
        for (uint16_t i = 0; i < meta.att_phys.size(); ++i) {
            global_pos[meta.att_phys[i]] = i;
            meta.att_ext.push_back(i);
        }

        // Por variable: patrones en que aparece y tamano de su proyeccion unaria mas chica (se lee del bitvector
        // del ultimo nivel: un indice cargado de disco puede no traer total_ones).
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
        for (const auto& d : degree)
            if (d.second > 1)
                meta.n_non_lonely++;

        // Orden voraz: non-lonely primero (el primer prejoin exacto llega antes), luego las conectadas al prefijo
        // (evita productos cartesianos), luego `cfg.order`.
        std::map<uint16_t, uint16_t> local_pos;
        size_t max_steps = degree.empty() ? 0 : degree.size() - 1;  // el join final agrega al menos una variable
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
                uint64_t degree_key = no_size - d.second;  // mas patrones primero
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

            // Una lonely sin patron comun con el prefijo es de un patron aislado: queda para el join final.
            if (!found || (best_lonely && !best_connected))
                break;

            local_pos[best] = (uint16_t)meta.order.size();
            meta.order.push_back(best);
        }

        // Un paso por variable del orden. Un self-loop (?x p ?x) no entra como tabla a ningun prejoin (un qdag no
        // puede tener menos dimensiones que su quadtree): aporta sus dos proyecciones y la tabla va al join final.
        // Con eso, o si falta una proyeccion unaria, los prejoins quedan sobre-aproximados y dejan de ser exactos.
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

            if (step.qts.empty()) {  // faltan proyecciones: la cadena se corta aca
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

        return meta;
    }

}

#endif // QDAGS_QUERY_LEAPFROG_BUILDER_HPP
