#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <cstdint>
#include <algorithm>

#include "qdags/ccq/joins_ccq.hpp"
#include "qdags/join/joins.hpp"
#include "qdags/join/joins_wide.hpp"
#include "qdags/query/triple_pattern.hpp"
#include "qdags/query/resource_manager.hpp"
#include "qdags/query/query_builder.hpp"
#include "qdags/query/query_loader.hpp"
#include "qdags/query/constant_terms.hpp"
#include "qdags/query/leapfrog_builder.hpp"
#include "qdags/query/classify_query.hpp"
#include "qdags/query/plan_parser.hpp"
#include "qdags/query/topology_mapping.hpp"
#include "qdags/query/plan_builder.hpp"
#include "qdags/query/topology_plan_selection.hpp"
#include "qdags/strategy/generic_strategies.hpp"
#include "qdags/strategy/generic_strategies_kernel.hpp"
#include "qdags/strategy/generic_strategies_leapfrog.hpp"
#include "qdags/strategy/generic_strategies_plan.hpp"
#include "levels_util.hpp"

// Las estrategias de pj_qdags (Orig, One-Shot, WeakKernel, Kernel, Leapfrog, Plan) sobre la implementacion de
// ccqQDAGS-high_arity_qdag copiada en include/qdags/ccq/ (scripts/import_ccq.sh, ccq/adapters.hpp): mismas
// plantillas con QdagT = ccq::qdag_t y SeQuadT = ccq::se_quadtree_t; ccq::multiJoinAuto reemplaza al motor de
// join por ADL. Un solo ejecutable, la estrategia se elige con --strategy.
//
// Uso:
//   run_ccq <dir_dataset> <queries_file> <upper_bound> <parallel_level[,nivel...]> --strategy orig|nl|wk|kernel|leapfrog|plan
//           [--grid_side <val>] [--time_out <val>] [--reps <n>]
//           [--results_file <path>] [--times_file <path>]
//           [--plan_group normal|best|wk_k|leapfrog]     (plan; obligatorio con --strategy plan)
//           [--k <val>]                                  (plan)
//           [--lf_order proj_size|degree] [--lf_depth full|non_lonely] [--lf_max_atts <val>]   (leapfrog)
//
// Dataset construido con build_dataset_ccq. <upper_bound> debe ser 0 (el AND de ccq no soporta tope). Solo variante
// generica y sin particion paralela (el AND de ccq es secuencial: parallel_level > 0 corre secuencial).
// Consultas con constantes: se saltan. Salida como los run_*; results_file "query_id;result" y times_file
// "query_id;ms", con ";TIMEOUT" al final si la consulta se corto por --time_out.
using namespace gdj;
using namespace std::chrono;

using SeQuadT = ccq::se_quadtree_t;
using QdagT = ccq::qdag_t;

static double median(std::vector<double> v) {
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return (n % 2 == 1) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

struct Config {
    std::string dir_dataset, queries_file, strategy, plan_group, results_file, times_file;
    std::vector<uint64_t> levels{0};
    uint32_t reps = 1;
    uint64_t upper_bound = 0, parallel_level = 0, grid_side = 52000000, time_out = 0, coef_bound = 0;   // coef_bound: sin cota
    uint8_t k = 2;
    leapfrog_config leapfrog;
};

// Resultado de una query (o el motivo por el que se salto).
struct QueryRun {
    bool ran = false;
    uint64_t result = 0;
    double ms = 0.0;
    bool timed_out = false;   // el watchdog cortó la query: `result` es parcial
    std::string skip_reason, tag;
};

template<class F>
static void timed(QueryRun &r, const Config &cfg, F &&body) {
    start_timeout_watchdog(cfg.time_out);
    auto t0 = high_resolution_clock::now();
    body();
    auto t1 = high_resolution_clock::now();
    r.timed_out = timeout_triggered.load(std::memory_order_relaxed);
    stop_timeout_watchdog();
    r.ms = duration<double, std::milli>(t1 - t0).count();
    r.ran = true;
}

static QueryRun run_orig(const Config &cfg, resource_manager<SeQuadT> &res, const query_tp &query) {
    QueryRun r;
    auto rel = build_qdags_from_query<SeQuadT>(query, res);
    if (rel.quadtrees.empty()) {
        r.skip_reason = "no resolvable patterns: constant P with variable S/O";
        return r;
    }
    std::vector<QdagT *> all;
    SeQuadT *result = nullptr;
    timed(r, cfg, [&] { result = strategy_orig<QdagT, SeQuadT>(rel, all, r.result, cfg.upper_bound, cfg.parallel_level, cfg.grid_side); });
    for (auto *q : all)
        delete q;
    delete result;
    return r;
}

static QueryRun run_nl(const Config &cfg, resource_manager<SeQuadT> &res, const query_tp &query) {
    QueryRun r;
    auto meta = build_nl_metadata<SeQuadT>(query, res);
    if (meta.nl_quadtrees.empty()) {
        r.skip_reason = "no non-lonely variables";
        return r;
    }
    std::vector<QdagT *> qd_nl, qd_f;
    SeQuadT *proj = nullptr, *final_qt = nullptr;
    timed(r, cfg, [&] {
        strategy_non_lonely<QdagT, SeQuadT, SeQuadT>(meta, qd_nl, qd_f, proj, final_qt, r.result, cfg.upper_bound,
                                                     cfg.parallel_level, cfg.grid_side, ParallelPhases::LastOnly);
    });
    for (auto *q : qd_nl)
        delete q;
    for (auto *q : qd_f)
        delete q;
    delete proj;
    delete final_qt;
    return r;
}

static QueryRun run_wk(const Config &cfg, resource_manager<SeQuadT> &res, const query_tp &query) {
    QueryRun r;
    auto meta = build_wk_metadata<SeQuadT>(query, res);
    if (meta.patterns.empty()) {
        r.skip_reason = "no usable patterns";
        return r;
    }
    std::vector<QdagT *> qd;
    SeQuadT *final_qt = nullptr;
    timed(r, cfg, [&] {
        final_qt = strategy_weak_kernel<QdagT, SeQuadT>(meta, qd, r.result, cfg.upper_bound, cfg.coef_bound, cfg.parallel_level,
                                                        cfg.grid_side, ParallelPhases::LastOnly);
    });
    for (auto *q : qd)
        delete q;
    delete final_qt;
    return r;
}

static QueryRun run_kernel(const Config &cfg, resource_manager<SeQuadT> &res, const query_tp &query) {
    QueryRun r;
    auto meta = build_nl_metadata<SeQuadT>(query, res);
    if (meta.nl_quadtrees.empty() || meta.l_quadtrees.empty()) {
        r.skip_reason = "kernel needs lonely and non-lonely variables";
        r.tag = "-";
        return r;
    }
    if (meta.att_nl.size() > KERNEL_MAX_NONLONELY_ATTS) {
        r.skip_reason = std::to_string(meta.att_nl.size()) + " non-lonely variables, the engine supports at most " +
                        std::to_string(KERNEL_MAX_NONLONELY_ATTS);
        r.tag = "+";
        return r;
    }
    std::vector<QdagT *> all;
    SeQuadT *final_qt = nullptr;
    timed(r, cfg, [&] {
        final_qt = strategy_kernel<QdagT, SeQuadT, SeQuadT>(meta, all, r.result, cfg.upper_bound, cfg.parallel_level, cfg.grid_side,
                                                            ParallelPhases::LastOnly);
    });
    for (auto *q : all)
        delete q;
    delete final_qt;
    return r;
}

static QueryRun run_leapfrog(const Config &cfg, resource_manager<SeQuadT> &res, const query_tp &query) {
    QueryRun r;
    auto meta = build_leapfrog_metadata<SeQuadT>(query, res, cfg.leapfrog);
    if (meta.steps.empty() && meta.final_qts.empty()) {
        r.skip_reason = "no usable patterns";
        return r;
    }
    std::vector<QdagT *> qd;
    SeQuadT *final_qt = nullptr;
    timed(r, cfg, [&] {
        final_qt = strategy_leapfrog<QdagT, SeQuadT, SeQuadT>(meta, qd, r.result, cfg.upper_bound, cfg.coef_bound, cfg.parallel_level,
                                                              cfg.grid_side, ParallelPhases::LastOnly);
    });
    for (auto *q : qd)
        delete q;
    delete final_qt;
    return r;
}

static bool build_meta_for_topology(QueryTopology topo, const query_tp &query, resource_manager<SeQuadT> &res, const execution_plan &plan,
                                    query_metadata_plan<SeQuadT> &meta) {
    switch (topo) {
        case QueryTopology::P2: case QueryTopology::T2: case QueryTopology::Ti2:
        case QueryTopology::P3: case QueryTopology::P4:
            build_p_plan_metadata<SeQuadT>(query, res, plan, meta); break;
        case QueryTopology::T3: case QueryTopology::Ti3: case QueryTopology::J3:
        case QueryTopology::T4: case QueryTopology::Ti4: case QueryTopology::J4:
            build_tij_plan_metadata<SeQuadT>(query, res, plan, meta); break;
        case QueryTopology::S1: case QueryTopology::S2: case QueryTopology::S3: case QueryTopology::S4:
            build_s_plan_metadata<SeQuadT>(query, res, plan, meta); break;
        case QueryTopology::Tr1: case QueryTopology::Tr2:
            build_tr_plan_metadata<SeQuadT>(query, res, plan, meta); break;
        default:
            return false;
    }
    return !meta.steps.empty();
}

static QueryRun run_plan(const Config &cfg, resource_manager<SeQuadT> &res, const query_tp &query, plan_catalog_cache &cache, PlanGroup group) {
    QueryRun r;
    QueryTopology topo = classify_query_topology(query);
    const execution_plan *plan = cache.get(topo, group);
    if (!plan) {
        r.skip_reason = "the topology has no plan in the default catalog";
        return r;
    }
    query_metadata_plan<SeQuadT> probe;
    if (!build_meta_for_topology(topo, query, res, *plan, probe)) {
        r.skip_reason = "topology without plan builder, or the query does not match the topology the plan expects";
        return r;
    }
    probe.clearResults();

    query_metadata_plan<SeQuadT> meta;
    build_meta_for_topology(topo, query, res, *plan, meta);
    std::vector<QdagT *> all;
    std::vector<SeQuadT *> mxm_quadtrees;
    SeQuadT *raw = nullptr;
    timed(r, cfg, [&] {
        raw = strategy_plan<QdagT, SeQuadT>(meta, all, mxm_quadtrees, r.result, cfg.upper_bound, cfg.parallel_level, cfg.grid_side,
                                            cfg.coef_bound, false, cfg.k, ParallelPhases::LastOnly);
    });
    delete raw;
    meta.clearResults();
    for (auto *q : all)
        delete q;
    for (auto *q : mxm_quadtrees)
        delete q;
    return r;
}

static int run_pass(const Config &cfg, resource_manager<SeQuadT> &res, const std::vector<query_tp> &all_queries) {
    std::vector<double> times;
    uint64_t n_skipped = 0, n_timeouts = 0;
    std::ofstream results_out, times_out;
    if (!cfg.results_file.empty()) {
        results_out.open(cfg.results_file);
        results_out << "query_id;result\n";
    }
    if (!cfg.times_file.empty()) {
        times_out.open(cfg.times_file);
        times_out << "query_id;ms\n";
    }
    plan_catalog_cache cache;
    PlanGroup group = cfg.strategy == "plan" ? parse_plan_group(cfg.plan_group) : PlanGroup::Best;

    for (size_t q_idx = 0; q_idx < all_queries.size(); ++q_idx) {
        const auto &query = all_queries[q_idx];
        QueryRun r;
        if (has_constant_terms(query)) {
            r.skip_reason = "constants not supported by run_ccq";
        } else if (cfg.strategy == "orig") {
            r = run_orig(cfg, res, query);
        } else if (cfg.strategy == "nl") {
            r = run_nl(cfg, res, query);
        } else if (cfg.strategy == "wk") {
            r = run_wk(cfg, res, query);
        } else if (cfg.strategy == "kernel") {
            r = run_kernel(cfg, res, query);
        } else if (cfg.strategy == "leapfrog") {
            r = run_leapfrog(cfg, res, query);
        } else {
            r = run_plan(cfg, res, query, cache, group);
        }
        if (!r.ran) {
            std::cout << "Query " << q_idx << ": skipped (" << r.skip_reason << ")" << std::endl;
            n_skipped++;
            if (!r.tag.empty()) {   // la fila queda, para cruzar linea a linea con otras estrategias
                if (results_out.is_open())
                    results_out << q_idx << ";" << r.tag << std::endl;
                if (times_out.is_open())
                    times_out << q_idx << ";" << r.tag << std::endl;
            }
            continue;
        }
        std::cout << "Query " << q_idx << ": result=" << r.result << " (" << std::fixed << std::setprecision(3) << r.ms << "ms)"
                  << (r.timed_out ? " [TIMEOUT: partial result]" : "") << std::endl;
        n_timeouts += r.timed_out;
        times.push_back(r.ms);
        const char *mark = r.timed_out ? ";TIMEOUT" : "";
        if (results_out.is_open())
            results_out << q_idx << ";" << r.result << mark << std::endl;
        if (times_out.is_open())
            times_out << q_idx << ";" << r.ms << mark << std::endl;
    }

    double total = 0;
    for (double t : times)
        total += t;
    double avg = times.empty() ? 0.0 : total / times.size();
    std::cout << "-----------------------------------------------------" << std::endl;
    std::cout << "Total queries: " << all_queries.size() << " | skipped: " << n_skipped << std::endl;
    std::cout << "total " << std::fixed << std::setprecision(3) << total << "ms"
              << " | mean " << avg << "ms | median " << median(times) << "ms" << std::endl;
    std::cout << "TIMEOUTS;" << n_timeouts << std::endl;
    std::cout << "RESULT;ccq;" << all_queries.size() << ";" << n_skipped << ";" << total << ";" << avg << ";" << median(times) << std::endl;
    return 0;
}

int main(int argc, char **argv) {
    const int mandatory_args = 4;
    if (argc < mandatory_args + 1) {
        std::cerr << "Usage: " << argv[0]
                  << " <dir_dataset> <queries_file> <upper_bound> <parallel_level[,level...]> --strategy orig|nl|wk|kernel|leapfrog|plan"
                     " [--grid_side <val>] [--time_out <val>] [--reps <n>] [--results_file <path>] [--times_file <path>]"
                     " [--plan_group normal|best|wk_k|leapfrog] [--k <val>]"
                     " [--lf_order proj_size|degree] [--lf_depth full|non_lonely] [--lf_max_atts <val>]" << std::endl;
        return 1;
    }
    Config cfg;
    cfg.dir_dataset = argv[1];
    cfg.queries_file = argv[2];
    cfg.upper_bound = std::stoull(argv[3]);
    try { cfg.levels = gdj_examples::parse_levels(argv[4]); }
    catch (const std::exception &e) { std::cerr << "Error: invalid <parallel_level[,level...]> (" << e.what() << ")" << std::endl; return 1; }

    std::string lf_order = "proj_size", lf_depth = "full";
    for (int i = mandatory_args + 1; i < argc; i++) {
        std::string arg = argv[i];
        bool has_val = i + 1 < argc;
        if (arg == "--strategy" && has_val) cfg.strategy = argv[++i];
        else if (arg == "--grid_side" && has_val) cfg.grid_side = std::stoull(argv[++i]);
        else if (arg == "--time_out" && has_val) cfg.time_out = std::stoull(argv[++i]);
        else if (arg == "--reps" && has_val) cfg.reps = (uint32_t)std::stoul(argv[++i]);
        else if (arg == "--results_file" && has_val) cfg.results_file = argv[++i];
        else if (arg == "--times_file" && has_val) cfg.times_file = argv[++i];
        else if (arg == "--plan_group" && has_val) cfg.plan_group = argv[++i];
        else if (arg == "--k" && has_val) cfg.k = (uint8_t)std::stoul(argv[++i]);
        else if (arg == "--lf_order" && has_val) lf_order = argv[++i];
        else if (arg == "--lf_depth" && has_val) lf_depth = argv[++i];
        else if (arg == "--lf_max_atts" && has_val) cfg.leapfrog.max_prejoin_atts = std::stoull(argv[++i]);
        else {
            std::cerr << "Error: unknown argument or missing value: " << arg << std::endl;
            return 1;
        }
    }
    if (cfg.upper_bound != 0) {
        std::cerr << "Error: <upper_bound> must be 0: the ccq AND does not support a result bound" << std::endl;
        return 1;
    }
    static const char *strategies[] = {"orig", "nl", "wk", "kernel", "leapfrog", "plan"};
    if (std::find_if(std::begin(strategies), std::end(strategies), [&](const char *s) { return cfg.strategy == s; }) == std::end(strategies)) {
        std::cerr << "Error: --strategy must be orig|nl|wk|kernel|leapfrog|plan (got: '" << cfg.strategy << "')" << std::endl;
        return 1;
    }
    if (cfg.strategy == "plan" && cfg.plan_group.empty()) {
        std::cerr << "Error: --strategy plan needs --plan_group normal|best|wk_k|leapfrog" << std::endl;
        return 1;
    }
    if (lf_order != "proj_size" && lf_order != "degree") { std::cerr << "Error: --lf_order must be proj_size|degree" << std::endl; return 1; }
    if (lf_depth != "full" && lf_depth != "non_lonely") { std::cerr << "Error: --lf_depth must be full|non_lonely" << std::endl; return 1; }
    cfg.leapfrog.order = parse_leapfrog_order(lf_order);
    cfg.leapfrog.depth = parse_leapfrog_depth(lf_depth);
    if (cfg.reps == 0) { std::cerr << "Error: --reps must be >= 1" << std::endl; return 1; }
    if (!gdj_examples::path_distinguishes_passes(cfg.results_file, cfg.levels.size(), cfg.reps) ||
        !gdj_examples::path_distinguishes_passes(cfg.times_file, cfg.levels.size(), cfg.reps)) {
        std::cerr << "Error: with several levels or repetitions, --results_file/--times_file must contain {L} and, with --reps > 1, {R}" << std::endl;
        return 1;
    }
    for (uint64_t l : cfg.levels)
        if (l > 0)
            std::cerr << "Warning: the ccq AND is sequential; parallel_level=" << l << " runs sequentially" << std::endl;

    std::cout << "Configuration:" << std::endl;
    std::cout << "  Backend:         ccq (quadtree, qdag and AND of ccqQDAGS-high_arity_qdag)" << std::endl;
    std::cout << "  Strategy:        " << cfg.strategy << (cfg.strategy == "plan" ? " (" + cfg.plan_group + ")" : "") << std::endl;
    std::cout << "  Dir Dataset:     " << cfg.dir_dataset << std::endl;
    std::cout << "  Queries File:    " << cfg.queries_file << std::endl;
    std::cout << "  Upper Bound:     " << cfg.upper_bound << std::endl;
    std::cout << "  Parallel Level:  " << gdj_examples::levels_to_string(cfg.levels) << " (ignored)" << std::endl;
    std::cout << "  Reps:            " << cfg.reps << std::endl;
    std::cout << "  Grid Side:       " << cfg.grid_side << std::endl;
    std::cout << "  Time Out:        " << cfg.time_out << " seconds" << std::endl;
    std::cout << "  Results File:    " << (cfg.results_file.empty() ? "(disabled)" : cfg.results_file) << std::endl;
    std::cout << "  Times File:      " << (cfg.times_file.empty() ? "(disabled)" : cfg.times_file) << std::endl;

    auto all_queries = load_all_queries(cfg.queries_file);

    resource_manager<SeQuadT> res_mgr;
    res_mgr.load_directory(cfg.dir_dataset);
    std::cout << "[ccq] ";
    res_mgr.print_summary();

    for (uint64_t level : cfg.levels) {
        for (uint32_t rep = 1; rep <= cfg.reps; ++rep) {
            Config pass = cfg;
            pass.parallel_level = level;
            pass.results_file = gdj_examples::expand_pass_path(cfg.results_file, level, rep);
            pass.times_file = gdj_examples::expand_pass_path(cfg.times_file, level, rep);
            std::cout << "LEVEL;" << level << ";" << rep << std::endl;
            int rc = run_pass(pass, res_mgr, all_queries);
            if (rc != 0)
                return rc;
        }
    }
    return 0;
}
