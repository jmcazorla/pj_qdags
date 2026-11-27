#ifndef QDAGS_QUERY_PLAN_BUILDER_HPP
#define QDAGS_QUERY_PLAN_BUILDER_HPP

#include <vector>
#include <map>
#include <algorithm>
#include <cstdint>

#include "../index/commun.hpp"
#include "../index/wide_base_config.hpp"
#include "triple_pattern.hpp"
#include "resource_manager.hpp"
#include "plan_parser.hpp"
#include "topology_mapping.hpp"

// Cada build_*_plan_metadata traduce un execution_plan ya parseado a los quadtrees y att_set que ejecuta
// strategy_plan, usando el mapping de la topologia (topology_mapping.hpp).

namespace gdj {

    template<class SeQuadT>
    struct step_metadata {
        std::string step_id;
        std::vector<SeQuadT*> qts;
        std::vector<att_set> atts_step;
        std::vector<OpType> op_types;

        att_set output_att_org;
        att_set output_att_ext_prejoin;

        SeQuadT* result_qt = nullptr;
        std::vector<std::string> prev_prejoin_id;
    };

    template<class SeQuadT>
    struct query_metadata_plan {
        using step_metadata = gdj::step_metadata<SeQuadT>;

        std::map<VarType, uint16_t> var_map;
        std::map<EdgeType, uint64_t> edge_pids;
        std::vector<step_metadata> steps;
        att_set full_query_att_ext;
        wide_base_config wide;  // como extienden los qdag sus bases; lo fija el caller

        void clearResults() {
            for (auto& step : steps) {
                if (step.result_qt) {
                    delete step.result_qt;
                    step.result_qt = nullptr;
                }
            }
        }
    };

    // Devuelve (v1,v2) de una arista logica (AB/BC/...) en su orden real (sujeto,objeto) en la query. 

    inline void getVarsFromEdge(EdgeType et, const SquareMapping& mapping, uint16_t& v1, uint16_t& v2) {
        switch (et) {
            case EdgeType::AB:
                if (mapping.ab_map.at(mapping.a) == TermType::S) {
                    v1 = mapping.a;
                    v2 = mapping.b;
                }
                else {
                    v1 = mapping.b;
                    v2 = mapping.a;
                }
                break;
            case EdgeType::BC:
                if (mapping.bc_map.at(mapping.b) == TermType::S) {
                    v1 = mapping.b;
                    v2 = mapping.c;
                }
                else {
                    v1 = mapping.c;
                    v2 = mapping.b;
                }
                break;
            case EdgeType::CD:
                if (mapping.cd_map.at(mapping.c) == TermType::S) {
                    v1 = mapping.c;
                    v2 = mapping.d;
                }
                else {
                    v1 = mapping.d;
                    v2 = mapping.c;
                }
                break;
            case EdgeType::DA:
                if (mapping.da_map.at(mapping.d) == TermType::S) {
                    v1 = mapping.d;
                    v2 = mapping.a;
                }
                else {
                    v1 = mapping.a;
                    v2 = mapping.d;
                }
                break;
            default:
                break;
        }
    }

    inline void getVarsFromEdge(EdgeType et, const TriangleMapping& mapping, uint16_t& v1, uint16_t& v2) {
        switch (et) {
            case EdgeType::AB:
                if (mapping.ab_map.at(mapping.a) == TermType::S) {
                    v1 = mapping.a;
                    v2 = mapping.b;
                }
                else {
                    v1 = mapping.b;
                    v2 = mapping.a;
                }
                break;
            case EdgeType::BC:
                if (mapping.bc_map.at(mapping.b) == TermType::S) {
                    v1 = mapping.b;
                    v2 = mapping.c;
                }
                else {
                    v1 = mapping.c;
                    v2 = mapping.b;
                }
                break;
            case EdgeType::CA:
                if (mapping.ca_map.at(mapping.c) == TermType::S) {
                    v1 = mapping.c;
                    v2 = mapping.a;
                }
                else {
                    v1 = mapping.a;
                    v2 = mapping.c;
                }
                break;
            default:
                break;
        }
    }

    inline void getVarsFromEdge(EdgeType et, const PathMapping& mapping, uint16_t& v1, uint16_t& v2) {
        switch (et) {
            case EdgeType::AB:
                if (mapping.ab_map.at(mapping.a) == TermType::S) {
                    v1 = mapping.a;
                    v2 = mapping.b;
                }
                else {
                    v1 = mapping.b;
                    v2 = mapping.a;
                }
                break;
            case EdgeType::BC:
                if (mapping.bc_map.at(mapping.b) == TermType::S) {
                    v1 = mapping.b;
                    v2 = mapping.c;
                }
                else {
                    v1 = mapping.c;
                    v2 = mapping.b;
                }
                break;
            case EdgeType::CD:
                if (mapping.cd_map.at(mapping.c) == TermType::S) {
                    v1 = mapping.c;
                    v2 = mapping.d;
                }
                else {
                    v1 = mapping.d;
                    v2 = mapping.c;
                }
                break;
            case EdgeType::DE:
                if (mapping.de_map.at(mapping.d) == TermType::S) {
                    v1 = mapping.d;
                    v2 = mapping.e;
                }
                else {
                    v1 = mapping.e;
                    v2 = mapping.d;
                }
                break;
            default:
                break;
        }
    }

    inline void getVarsFromEdge(EdgeType et, const StarMapping& mapping, uint16_t& v1, uint16_t& v2) {
        switch (et) {
            case EdgeType::AB:
                if (mapping.ab_map.at(mapping.a) == TermType::S) {
                    v1 = mapping.a;
                    v2 = mapping.b;
                }
                else {
                    v1 = mapping.b;
                    v2 = mapping.a;
                }
                break;
            case EdgeType::CA:
                if (mapping.ac_map.at(mapping.a) == TermType::S) {
                    v1 = mapping.a;
                    v2 = mapping.c;
                }
                else {
                    v1 = mapping.c;
                    v2 = mapping.a;
                }
                break;
            case EdgeType::DA:
                if (mapping.ad_map.at(mapping.a) == TermType::S) {
                    v1 = mapping.a;
                    v2 = mapping.d;
                }
                else {
                    v1 = mapping.d;
                    v2 = mapping.a;
                }
                break;
            case EdgeType::AE:
                if (mapping.ae_map.at(mapping.a) == TermType::S) {
                    v1 = mapping.a;
                    v2 = mapping.e;
                }
                else {
                    v1 = mapping.e;
                    v2 = mapping.a;
                }
                break;
            default:
                break;
        }
    }

    // Renumera un att_set a {0..n-1} en orden de primera aparicion (espacio de los pasos no finales).
    inline std::map<uint16_t, uint16_t> getNormalizedAttributes(const att_set& original) {
        std::map<uint16_t, uint16_t> mapping;
        uint16_t next_id = 0;
        for (uint16_t attr : original) {
            if (mapping.find(attr) == mapping.end())
                mapping[attr] = next_id++;
        }
        return mapping;
    }

    // Topologias S1-S4 (ciclo de 4 nodos), via SquareMapping.
    template<class SeQuadT>
    void build_s_plan_metadata(const query_tp& query, resource_manager<SeQuadT>& res,
                                const execution_plan& plan, query_metadata_plan<SeQuadT>& meta) {
        SquareMapping squareM;
        squareM.build(query, res);
        if (!squareM.is_valid)
            return;

        meta.var_map[VarType::A] = squareM.a;
        meta.var_map[VarType::B] = squareM.b;
        meta.var_map[VarType::C] = squareM.c;
        meta.var_map[VarType::D] = squareM.d;

        meta.edge_pids[EdgeType::AB] = query[squareM.edge_ab_pid].get_var_p();
        meta.edge_pids[EdgeType::BC] = query[squareM.edge_bc_pid].get_var_p();
        meta.edge_pids[EdgeType::CD] = query[squareM.edge_cd_pid].get_var_p();
        meta.edge_pids[EdgeType::DA] = query[squareM.edge_da_pid].get_var_p();

        meta.full_query_att_ext = {squareM.a, squareM.b, squareM.c, squareM.d};

        for (size_t i = 0; i < plan.steps.size(); ++i) {
            bool is_last = (i == plan.steps.size() - 1);
            const auto& p_step = plan.steps[i];

            typename query_metadata_plan<SeQuadT>::step_metadata sm;
            sm.step_id = p_step.step_id;

            for (auto vt : p_step.output_vars)
                sm.output_att_org.push_back(meta.var_map[vt]);
            std::sort(sm.output_att_org.begin(), sm.output_att_org.end());
            std::map<uint16_t, uint16_t> step_var_map = getNormalizedAttributes(sm.output_att_org);
            for (auto vt : p_step.output_vars)
                sm.output_att_ext_prejoin.push_back(step_var_map.at(meta.var_map[vt]));
            std::sort(sm.output_att_ext_prejoin.begin(), sm.output_att_ext_prejoin.end());

            for (const auto& op : p_step.operations) {
                sm.op_types.push_back(op.type);

                if (op.type == OpType::TABLE) {
                    uint64_t pid = meta.edge_pids[op.source_edge];
                    sm.qts.push_back(res.get_so(pid));

                    uint16_t v1, v2;
                    getVarsFromEdge(op.source_edge, squareM, v1, v2);
                    att_set att_org = is_last ? att_set({v1, v2}) : att_set({step_var_map.at(v1), step_var_map.at(v2)});
                    sm.atts_step.push_back(att_org);
                }
                else if (op.type == OpType::PROJECTION) {
                    uint64_t pid = meta.edge_pids[op.source_edge];
                    VarType target_vt = op.target_vars[0];

                    bool is_subject = false;
                    if (op.source_edge == EdgeType::AB)
                        is_subject = (squareM.ab_map.at(target_vt == VarType::A ? squareM.a : squareM.b) == TermType::S);
                    else if (op.source_edge == EdgeType::BC)
                        is_subject = (squareM.bc_map.at(target_vt == VarType::B ? squareM.b : squareM.c) == TermType::S);
                    else if (op.source_edge == EdgeType::CD)
                        is_subject = (squareM.cd_map.at(target_vt == VarType::C ? squareM.c : squareM.d) == TermType::S);
                    else if (op.source_edge == EdgeType::DA)
                        is_subject = (squareM.da_map.at(target_vt == VarType::D ? squareM.d : squareM.a) == TermType::S);

                    sm.qts.push_back(is_subject ? res.get_s(pid) : res.get_o(pid));
                    att_set att_org = is_last ? att_set({ meta.var_map[target_vt] }) : att_set({ step_var_map.at(meta.var_map[target_vt]) });
                    sm.atts_step.push_back(att_org);
                }
                else if (op.type == OpType::PREJOIN) {
                    sm.qts.push_back(nullptr);
                    att_set expected_ref;
                    for (auto v : op.ref_vars)
                        expected_ref.push_back(is_last ? meta.var_map[v] : step_var_map.at(meta.var_map[v]));
                    std::sort(expected_ref.begin(), expected_ref.end());
                    sm.atts_step.push_back(expected_ref);
                    sm.prev_prejoin_id.push_back(op.ref_prejoin_id);
                }
            }
            meta.steps.push_back(sm);
        }
    }

    // Topologias Tr1-Tr2 (triangulo), via TriangleMapping.
    template<class SeQuadT>
    void build_tr_plan_metadata(const query_tp& query, resource_manager<SeQuadT>& res,
                                 const execution_plan& plan, query_metadata_plan<SeQuadT>& meta) {
        TriangleMapping triangleM;
        triangleM.build(query, res);
        if (!triangleM.is_valid)
            return;

        meta.var_map[VarType::A] = triangleM.a;
        meta.var_map[VarType::B] = triangleM.b;
        meta.var_map[VarType::C] = triangleM.c;

        meta.edge_pids[EdgeType::AB] = query[triangleM.edge_ab_pid].get_var_p();
        meta.edge_pids[EdgeType::BC] = query[triangleM.edge_bc_pid].get_var_p();
        meta.edge_pids[EdgeType::CA] = query[triangleM.edge_ca_pid].get_var_p();

        meta.full_query_att_ext = {triangleM.a, triangleM.b, triangleM.c};

        for (size_t i = 0; i < plan.steps.size(); ++i) {
            bool is_last = (i == plan.steps.size() - 1);
            const auto& p_step = plan.steps[i];

            typename query_metadata_plan<SeQuadT>::step_metadata sm;
            sm.step_id = p_step.step_id;

            for (auto vt : p_step.output_vars)
                sm.output_att_org.push_back(meta.var_map[vt]);
            std::sort(sm.output_att_org.begin(), sm.output_att_org.end());
            std::map<uint16_t, uint16_t> step_var_map = getNormalizedAttributes(sm.output_att_org);
            for (auto vt : p_step.output_vars)
                sm.output_att_ext_prejoin.push_back(step_var_map.at(meta.var_map[vt]));
            std::sort(sm.output_att_ext_prejoin.begin(), sm.output_att_ext_prejoin.end());

            for (const auto& op : p_step.operations) {
                sm.op_types.push_back(op.type);

                if (op.type == OpType::TABLE) {
                    uint64_t pid = meta.edge_pids[op.source_edge];
                    sm.qts.push_back(res.get_so(pid));

                    uint16_t v1, v2;
                    getVarsFromEdge(op.source_edge, triangleM, v1, v2);
                    att_set att_org = is_last ? att_set({v1, v2}) : att_set({step_var_map.at(v1), step_var_map.at(v2)});
                    sm.atts_step.push_back(att_org);
                }
                else if (op.type == OpType::PROJECTION) {
                    uint64_t pid = meta.edge_pids[op.source_edge];
                    VarType target_vt = op.target_vars[0];

                    bool is_subject = false;
                    if (op.source_edge == EdgeType::AB)
                        is_subject = (triangleM.ab_map.at(target_vt == VarType::A ? triangleM.a : triangleM.b) == TermType::S);
                    else if (op.source_edge == EdgeType::BC)
                        is_subject = (triangleM.bc_map.at(target_vt == VarType::B ? triangleM.b : triangleM.c) == TermType::S);
                    else if (op.source_edge == EdgeType::CA)
                        is_subject = (triangleM.ca_map.at(target_vt == VarType::C ? triangleM.c : triangleM.a) == TermType::S);

                    sm.qts.push_back(is_subject ? res.get_s(pid) : res.get_o(pid));
                    att_set att_org = is_last ? att_set({ meta.var_map[target_vt] }) : att_set({ step_var_map.at(meta.var_map[target_vt]) });
                    sm.atts_step.push_back(att_org);
                }
                else if (op.type == OpType::PREJOIN) {
                    sm.qts.push_back(nullptr);
                    att_set expected_ref;
                    for (auto v : op.ref_vars)
                        expected_ref.push_back(is_last ? meta.var_map[v] : step_var_map.at(meta.var_map[v]));
                    std::sort(expected_ref.begin(), expected_ref.end());
                    sm.atts_step.push_back(expected_ref);
                    sm.prev_prejoin_id.push_back(op.ref_prejoin_id);
                }
            }
            meta.steps.push_back(sm);
        }
    }

    // Topologias P2-P4 (camino), via PathMapping.
    template<class SeQuadT>
    void build_p_plan_metadata(const query_tp& query, resource_manager<SeQuadT>& res,
                                const execution_plan& plan, query_metadata_plan<SeQuadT>& meta) {
        PathMapping pathM;
        pathM.build(query, res);
        if (!pathM.is_valid)
            return;

        meta.var_map[VarType::A] = pathM.a;
        meta.var_map[VarType::B] = pathM.b;
        meta.var_map[VarType::C] = pathM.c;
        meta.var_map[VarType::D] = pathM.d;
        if (pathM.num_edges == 4)
            meta.var_map[VarType::E] = pathM.e;

        meta.edge_pids[EdgeType::AB] = query[pathM.edge_ab_pid].get_var_p();
        meta.edge_pids[EdgeType::BC] = query[pathM.edge_bc_pid].get_var_p();
        meta.edge_pids[EdgeType::CD] = query[pathM.edge_cd_pid].get_var_p();
        if (pathM.num_edges == 4)
            meta.edge_pids[EdgeType::DE] = query[pathM.edge_de_pid].get_var_p();

        if (pathM.num_edges == 4)
            meta.full_query_att_ext = {pathM.a, pathM.b, pathM.c, pathM.d, pathM.e};
        else
            meta.full_query_att_ext = {pathM.a, pathM.b, pathM.c, pathM.d};

        for (size_t i = 0; i < plan.steps.size(); ++i) {
            bool is_last = (i == plan.steps.size() - 1);
            const auto& p_step = plan.steps[i];

            typename query_metadata_plan<SeQuadT>::step_metadata sm;
            sm.step_id = p_step.step_id;

            for (auto vt : p_step.output_vars)
                sm.output_att_org.push_back(meta.var_map[vt]);
            std::sort(sm.output_att_org.begin(), sm.output_att_org.end());
            std::map<uint16_t, uint16_t> step_var_map = getNormalizedAttributes(sm.output_att_org);
            for (auto vt : p_step.output_vars)
                sm.output_att_ext_prejoin.push_back(step_var_map.at(meta.var_map[vt]));
            std::sort(sm.output_att_ext_prejoin.begin(), sm.output_att_ext_prejoin.end());

            for (const auto& op : p_step.operations) {
                sm.op_types.push_back(op.type);

                if (op.type == OpType::TABLE) {
                    uint64_t pid = meta.edge_pids[op.source_edge];
                    sm.qts.push_back(res.get_so(pid));

                    uint16_t v1, v2;
                    getVarsFromEdge(op.source_edge, pathM, v1, v2);
                    att_set att_org = is_last ? att_set({v1, v2}) : att_set({step_var_map.at(v1), step_var_map.at(v2)});
                    sm.atts_step.push_back(att_org);
                }
                else if (op.type == OpType::PROJECTION) {
                    uint64_t pid = meta.edge_pids[op.source_edge];
                    VarType target_vt = op.target_vars[0];

                    bool is_subject = false;
                    if (op.source_edge == EdgeType::AB)
                        is_subject = (pathM.ab_map.at(target_vt == VarType::A ? pathM.a : pathM.b) == TermType::S);
                    else if (op.source_edge == EdgeType::BC)
                        is_subject = (pathM.bc_map.at(target_vt == VarType::B ? pathM.b : pathM.c) == TermType::S);
                    else if (op.source_edge == EdgeType::CD)
                        is_subject = (pathM.cd_map.at(target_vt == VarType::C ? pathM.c : pathM.d) == TermType::S);
                    else if (op.source_edge == EdgeType::DE)
                        is_subject = (pathM.de_map.at(target_vt == VarType::D ? pathM.d : pathM.e) == TermType::S);

                    sm.qts.push_back(is_subject ? res.get_s(pid) : res.get_o(pid));
                    att_set att_org = is_last ? att_set({ meta.var_map[target_vt] }) : att_set({ step_var_map.at(meta.var_map[target_vt]) });
                    sm.atts_step.push_back(att_org);
                }
                else if (op.type == OpType::PREJOIN) {
                    sm.qts.push_back(nullptr);
                    att_set expected_ref;
                    for (auto v : op.ref_vars)
                        expected_ref.push_back(is_last ? meta.var_map[v] : step_var_map.at(meta.var_map[v]));
                    std::sort(expected_ref.begin(), expected_ref.end());
                    sm.atts_step.push_back(expected_ref);
                    sm.prev_prejoin_id.push_back(op.ref_prejoin_id);
                }
            }
            meta.steps.push_back(sm);
        }
    }

    // Topologias estrella T3-T4/Ti3-Ti4/J3-J4, via StarMapping.
    template<class SeQuadT>
    void build_tij_plan_metadata(const query_tp& query, resource_manager<SeQuadT>& res,
                                  const execution_plan& plan, query_metadata_plan<SeQuadT>& meta) {
        StarMapping starM;
        starM.build(query, res);
        if (!starM.is_valid)
            return;

        meta.var_map[VarType::A] = starM.a;
        meta.var_map[VarType::B] = starM.b;
        meta.var_map[VarType::C] = starM.c;
        meta.var_map[VarType::D] = starM.d;
        if (starM.num_edges == 4)
            meta.var_map[VarType::E] = starM.e;

        meta.edge_pids[EdgeType::AB] = query[starM.edge_ab_pid].get_var_p();
        meta.edge_pids[EdgeType::CA] = query[starM.edge_ac_pid].get_var_p();
        meta.edge_pids[EdgeType::DA] = query[starM.edge_ad_pid].get_var_p();
        if (starM.num_edges == 4)
            meta.edge_pids[EdgeType::AE] = query[starM.edge_ae_pid].get_var_p();

        if (starM.num_edges == 4)
            meta.full_query_att_ext = {starM.a, starM.b, starM.c, starM.d, starM.e};
        else
            meta.full_query_att_ext = {starM.a, starM.b, starM.c, starM.d};

        for (size_t i = 0; i < plan.steps.size(); ++i) {
            bool is_last = (i == plan.steps.size() - 1);
            const auto& p_step = plan.steps[i];

            typename query_metadata_plan<SeQuadT>::step_metadata sm;
            sm.step_id = p_step.step_id;

            for (auto vt : p_step.output_vars)
                sm.output_att_org.push_back(meta.var_map[vt]);
            std::sort(sm.output_att_org.begin(), sm.output_att_org.end());
            std::map<uint16_t, uint16_t> step_var_map = getNormalizedAttributes(sm.output_att_org);
            for (auto vt : p_step.output_vars)
                sm.output_att_ext_prejoin.push_back(step_var_map.at(meta.var_map[vt]));
            std::sort(sm.output_att_ext_prejoin.begin(), sm.output_att_ext_prejoin.end());

            for (const auto& op : p_step.operations) {
                sm.op_types.push_back(op.type);

                if (op.type == OpType::TABLE) {
                    uint64_t pid = meta.edge_pids[op.source_edge];
                    sm.qts.push_back(res.get_so(pid));

                    uint16_t v1, v2;
                    getVarsFromEdge(op.source_edge, starM, v1, v2);
                    att_set att_org = is_last ? att_set({v1, v2}) : att_set({step_var_map.at(v1), step_var_map.at(v2)});
                    sm.atts_step.push_back(att_org);
                }
                else if (op.type == OpType::PROJECTION) {
                    uint64_t pid = meta.edge_pids[op.source_edge];
                    VarType target_vt = op.target_vars[0];

                    bool is_subject = false;
                    if (op.source_edge == EdgeType::AB)
                        is_subject = (starM.ab_map.at(target_vt == VarType::A ? starM.a : starM.b) == TermType::S);
                    else if (op.source_edge == EdgeType::CA)
                        is_subject = (starM.ac_map.at(target_vt == VarType::A ? starM.a : starM.c) == TermType::S);
                    else if (op.source_edge == EdgeType::DA)
                        is_subject = (starM.ad_map.at(target_vt == VarType::A ? starM.a : starM.d) == TermType::S);
                    else if (op.source_edge == EdgeType::AE)
                        is_subject = (starM.ae_map.at(target_vt == VarType::A ? starM.a : starM.e) == TermType::S);

                    sm.qts.push_back(is_subject ? res.get_s(pid) : res.get_o(pid));
                    att_set att_org = is_last ? att_set({ meta.var_map[target_vt] }) : att_set({ step_var_map.at(meta.var_map[target_vt]) });
                    sm.atts_step.push_back(att_org);
                }
                else if (op.type == OpType::PREJOIN) {
                    sm.qts.push_back(nullptr);
                    att_set expected_ref;
                    for (auto v : op.ref_vars)
                        expected_ref.push_back(is_last ? meta.var_map[v] : step_var_map.at(meta.var_map[v]));
                    std::sort(expected_ref.begin(), expected_ref.end());
                    sm.atts_step.push_back(expected_ref);
                    sm.prev_prejoin_id.push_back(op.ref_prejoin_id);
                }
            }
            meta.steps.push_back(sm);
        }
    }

} // namespace gdj

#endif // QDAGS_QUERY_PLAN_BUILDER_HPP
