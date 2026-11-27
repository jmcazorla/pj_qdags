#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <string>
#include <map>
#include <chrono>
#include <cstdint>
#include <algorithm>

#include "qdags/index/commun.hpp"
#include "qdags/index/se_quadtree_bv64.hpp"
#include "qdags/index/se_quadtree_vfamily.hpp"
#include "qdags/index/se_quadtree_sd.hpp"
#include "qdags/index/se_quadtree_rrr.hpp"
#include "qdags/index/se_quadtree_rrr15.hpp"
#include "qdags/index/qdag_bv64.hpp"
#include "qdags/index/qdag_vfamily.hpp"
#include "qdags/index/qdag_sd.hpp"
#include "qdags/index/qdag_rrr.hpp"
#include "qdags/index/qdag_rrr15.hpp"
#include "qdags/join/joins.hpp"
#include "qdags/join/joins_wide.hpp"
#include "qdags/join/joins_resumable.hpp"
#include "qdags/join/joins_resumable_wide.hpp"
#include "qdags/query/triple_pattern.hpp"
#include "qdags/query/resource_manager.hpp"
#include "qdags/query/query_loader.hpp"
#include "qdags/query/classify_query.hpp"
#include "qdags/query/plan_parser.hpp"
#include "qdags/query/topology_mapping.hpp"
#include "qdags/query/plan_builder.hpp"
#include "qdags/query/topology_plan_selection.hpp"
#include "qdags/strategy/generic_strategies_plan.hpp"
#include "qdags/strategy/generic_strategies_plan_resumable.hpp"
#include "levels_util.hpp"
#include "wide_base_args.hpp"

// Perfilador de strategy_plan_resumable: amplia cada paso no final con su propio ResumableJoinAuto en vez de
// recalcularlo en cada intento (respetando las dependencias entre prejoins). Sin --mode (un solo motor) ni
// --parallel_phases (las fases resumable son secuenciales; --parallel_level solo afecta el ultimo paso).
// El plan se resuelve como en run_plan.cpp (--plan_file/--plan_id o --plan_group).
//
// Uso:
//   run_plan_resumable <dir_dataset> <queries_file> <upper_bound> <parallel_level[,nivel...]>
//                       (--plan_file <path> --plan_id "<key>" | --plan_group normal|best|wk_k|leapfrog)
//                       [--grid_side <val>] [--time_out <val>]
//                       [--init_coef <val>] [--growth <val>] [--growth_mode fixed|proportional] [--max_retries <val>]
//                       [--mxm] [--k <val>]
//                       [--variant bv64|v5|v|sd|rrr|rrr15]
//                       [--results_file <path>] [--times_file <path>]

// Varios niveles/repeticiones (--reps, {L}, {R}, "LEVEL;..."): ver levels_util.hpp.
using namespace gdj;
using namespace std::chrono;

static double median(std::vector<double> v) {
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return (n % 2 == 1) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

struct Config {
    gdj::wide_base_config wide;
    std::string dir_dataset, queries_file, plan_file, plan_id, plan_group, variant = "bv64", results_file, times_file;
    std::vector<uint64_t> levels{0};
    uint32_t reps = 1;
    uint64_t upper_bound = 0, parallel_level = 0, grid_side = 52000000, time_out = 0, init_coef = 2;
    uint32_t growth = 2, max_retries = 15;
    GrowthMode growth_mode = GrowthMode::Fixed;
    bool mxm = false;
    uint8_t k = 2;
};

template<class SeQuadT>
static bool build_meta_for_topology(QueryTopology topo, const query_tp& query, resource_manager<SeQuadT>& res_mgr,
                                     const execution_plan& plan, query_metadata_plan<SeQuadT>& meta) {
    switch (topo) {
        case QueryTopology::P2: case QueryTopology::T2: case QueryTopology::Ti2:
        case QueryTopology::P3: case QueryTopology::P4:
            build_p_plan_metadata<SeQuadT>(query, res_mgr, plan, meta); break;
        case QueryTopology::T3: case QueryTopology::Ti3: case QueryTopology::J3:
        case QueryTopology::T4: case QueryTopology::Ti4: case QueryTopology::J4:
            build_tij_plan_metadata<SeQuadT>(query, res_mgr, plan, meta); break;
        case QueryTopology::S1: case QueryTopology::S2: case QueryTopology::S3: case QueryTopology::S4:
            build_s_plan_metadata<SeQuadT>(query, res_mgr, plan, meta); break;
        case QueryTopology::Tr1: case QueryTopology::Tr2:
            build_tr_plan_metadata<SeQuadT>(query, res_mgr, plan, meta); break;
        default:
            return false;
    }
    return !meta.steps.empty();
}

template<class RankBackend>
static int run_pass(const Config &cfg, resource_manager<se_quadtree<RankBackend>> &res_mgr, const std::vector<query_tp> &all_queries, const execution_plan *fixed_plan) {
    using SeQuadT = se_quadtree<RankBackend>;
    using QdagT = qdag<RankBackend>;


    plan_catalog_cache cache;
    PlanGroup group = parse_plan_group(cfg.plan_group);
    auto resolve_plan = [&](QueryTopology topo) -> const execution_plan* {
        return fixed_plan ? fixed_plan : cache.get(topo, group);
    };

    std::vector<double> times;
    uint64_t n_skipped = 0;
    std::ofstream results_out, times_out;
    if (!cfg.results_file.empty()) {
        results_out.open(cfg.results_file);
        results_out << "query_id;result\n";
    }
    if (!cfg.times_file.empty()) {
        times_out.open(cfg.times_file);
        times_out << "query_id;ms\n";
    }

    for (size_t q_idx = 0; q_idx < all_queries.size(); ++q_idx) {
        auto &query = all_queries[q_idx];
        QueryTopology topo = classify_query_topology(query);
        const execution_plan* plan = resolve_plan(topo);

        if (!plan) {
            std::cout << "Query " << q_idx << " [" << topo << "]: skipped (--plan_group is active and the topology "
                          "has no plan in the default catalog)" << std::endl;
            n_skipped++;
            continue;
        }

        query_metadata_plan<SeQuadT> meta;

        meta.wide = cfg.wide;
        if (!build_meta_for_topology<SeQuadT>(topo, query, res_mgr, *plan, meta)) {
            std::cout << "Query " << q_idx << " [" << topo << "]: skipped (topology without plan builder, "
                          "or the query does not match the topology the plan expects)" << std::endl;
            n_skipped++;
            continue;
        }

        std::vector<QdagT*> all_qdags;
        std::vector<SeQuadT*> mxm_quadtrees;
        uint32_t attempts = 0; uint64_t coef_used = 0; bool exhaustive = false;
        uint64_t result = 0;
        start_timeout_watchdog(cfg.time_out);
        auto t0 = high_resolution_clock::now();
        SeQuadT* final_qt = strategy_plan_resumable<QdagT, SeQuadT>(
            meta, all_qdags, mxm_quadtrees, result, cfg.upper_bound, cfg.init_coef, cfg.growth, cfg.max_retries,
            cfg.parallel_level, cfg.grid_side, cfg.mxm, cfg.k, &attempts, &coef_used, &exhaustive, cfg.growth_mode);
        auto t1 = high_resolution_clock::now();
        stop_timeout_watchdog();
        double ms = duration<double, std::milli>(t1 - t0).count();
        delete final_qt;
        meta.clearResults();
        for (auto *q : all_qdags)
            delete q;
        for (auto *q : mxm_quadtrees)
            delete q;

        std::cout << "Query " << q_idx << " [" << topo << "]: result=" << result
                   << " (" << std::fixed << std::setprecision(3) << ms << "ms"
                   << ", attempts=" << attempts << ", final_coef=" << coef_used
                   << ", " << (exhaustive ? "exhaustive" : "not-exhaustive") << ")" << std::endl;

        times.push_back(ms);
        if (results_out.is_open())
            results_out << q_idx << ";" << result << std::endl;
        if (times_out.is_open())
            times_out << q_idx << ";" << ms << std::endl;
    }

    double total = 0; for (double t : times) total += t;
    double avg = times.empty() ? 0.0 : total / times.size();

    std::cout << "-----------------------------------------------------" << std::endl;
    std::cout << "Total queries: " << all_queries.size() << " | skipped: " << n_skipped << std::endl;
    std::cout << "total " << std::fixed << std::setprecision(3) << total << "ms"
               << " | mean " << std::fixed << std::setprecision(3) << avg << "ms"
               << " | median " << std::fixed << std::setprecision(3) << median(times) << "ms" << std::endl;

    std::cout << "RESULT;" << cfg.variant << ";" << all_queries.size() << ";" << n_skipped << ";"
               << total << ";" << avg << ";" << median(times) << std::endl;

    return 0;
}

// Carga el dataset una vez y corre todas las pasadas (nivel x repeticion).
template<class RankBackend>
static int run_variant(const Config &cfg, const std::vector<query_tp> &all_queries, const execution_plan *fixed_plan) {
    using SeQuadT = se_quadtree<RankBackend>;

    resource_manager<SeQuadT> res_mgr;
    res_mgr.load_directory(cfg.dir_dataset);
    std::cout << "[" << cfg.variant << "] "; res_mgr.print_summary();

    for (uint64_t level : cfg.levels) {
        for (uint32_t rep = 1; rep <= cfg.reps; ++rep) {
            Config pass = cfg;
            pass.parallel_level = level;
            pass.results_file = gdj_examples::expand_pass_path(cfg.results_file, level, rep);
            pass.times_file = gdj_examples::expand_pass_path(cfg.times_file, level, rep);
            std::cout << "LEVEL;" << level << ";" << rep << std::endl;
            int rc = run_pass<RankBackend>(pass, res_mgr, all_queries, fixed_plan);
            if (rc != 0)
                return rc;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    const int mandatory_args = 4;
    if (argc < mandatory_args + 1) {
        std::cerr << "Usage: " << argv[0]
                   << " <dir_dataset> <queries_file> <upper_bound> <parallel_level[,level...]>"
                      " (--plan_file <path> --plan_id \"<key>\" | --plan_group normal|best|wk_k|leapfrog)"
                      " [--grid_side <val>] [--time_out <val>]"
                      " [--init_coef <val>] [--growth <val>] [--growth_mode fixed|proportional] [--max_retries <val>]"
                      " [--mxm] [--k <val>]"
                      " [--variant bv64|v5|v|sd|rrr|rrr15]"
                      " [--reps <n>] [--results_file <path>] [--times_file <path>]" << gdj_examples::WIDE_BASE_USAGE << std::endl;
        return 1;
    }

    Config cfg;
    cfg.dir_dataset = argv[1];
    cfg.queries_file = argv[2];
    cfg.upper_bound = std::stoull(argv[3]);
    try { cfg.levels = gdj_examples::parse_levels(argv[4]); }
    catch (const std::exception &e) { std::cerr << "Error: invalid <parallel_level[,level...]> (" << e.what() << ")" << std::endl; return 1; }
    cfg.parallel_level = cfg.levels.front();

    for (int i = mandatory_args + 1; i < argc; i++) {
        std::string arg = argv[i];
        if (int r = gdj_examples::parse_wide_base_arg(argc, argv, i, cfg.wide)) {
            if (r < 0)
                return 1;
            continue;
        }
        if (arg == "--grid_side" && i + 1 < argc)
            cfg.grid_side = std::stoull(argv[++i]);
        else if (arg == "--time_out" && i + 1 < argc)
            cfg.time_out = std::stoull(argv[++i]);
        else if (arg == "--init_coef" && i + 1 < argc)
            cfg.init_coef = std::stoull(argv[++i]);
        else if (arg == "--growth" && i + 1 < argc)
            cfg.growth = (uint32_t)std::stoul(argv[++i]);
        else if (arg == "--growth_mode" && i + 1 < argc) {
            if (!parse_growth_mode(argv[++i], cfg.growth_mode)) {
                std::cerr << "Error: --growth_mode must be fixed|proportional (got: " << argv[i] << ")" << std::endl;
                return 1;
            }
        }
        else if (arg == "--max_retries" && i + 1 < argc)
            cfg.max_retries = (uint32_t)std::stoul(argv[++i]);
        else if (arg == "--mxm")
            cfg.mxm = true;
        else if (arg == "--k" && i + 1 < argc)
            cfg.k = (uint8_t)std::stoul(argv[++i]);
        else if (arg == "--plan_file" && i + 1 < argc)
            cfg.plan_file = argv[++i];
        else if (arg == "--plan_id" && i + 1 < argc)
            cfg.plan_id = argv[++i];
        else if (arg == "--plan_group" && i + 1 < argc)
            cfg.plan_group = argv[++i];
        else if (arg == "--variant" && i + 1 < argc)
            cfg.variant = argv[++i];
        else if (arg == "--results_file" && i + 1 < argc)
            cfg.results_file = argv[++i];
        else if (arg == "--times_file" && i + 1 < argc)
            cfg.times_file = argv[++i];
        else if (arg == "--reps" && i + 1 < argc)
            cfg.reps = (uint32_t)std::stoul(argv[++i]);
    }

    bool has_explicit_plan = !cfg.plan_file.empty() || !cfg.plan_id.empty();
    bool has_plan_group = !cfg.plan_group.empty();
    if (has_explicit_plan == has_plan_group) {
        std::cerr << "Error: pass exactly one of (--plan_file + --plan_id) or --plan_group "
                      "(not both, not neither)." << std::endl;
        return 1;
    }
    if (has_explicit_plan && (cfg.plan_file.empty() || cfg.plan_id.empty())) {
        std::cerr << "Error: --plan_file and --plan_id must be given together." << std::endl;
        return 1;
    }
    if (has_plan_group && cfg.plan_group != "normal" && cfg.plan_group != "best"
        && cfg.plan_group != "wk_k" && cfg.plan_group != "leapfrog") {
        std::cerr << "Error: --plan_group must be normal|best|wk_k|leapfrog (got: " << cfg.plan_group << ")" << std::endl;
        return 1;
    }

    std::map<std::string, execution_plan> plans;
    const execution_plan *fixed_plan = nullptr;
    if (has_explicit_plan) {
        plans = PlanParser::parseFile(cfg.plan_file);
        auto it = plans.find(cfg.plan_id);
        if (it == plans.end()) {
            std::cerr << "Error: --plan_id \"" << cfg.plan_id << "\" not found in " << cfg.plan_file << ". Available plans:" << std::endl;
            for (auto const& [key, val] : plans)
                std::cerr << "  \"" << key << "\"" << std::endl;
            return 1;
        }
        fixed_plan = &it->second;
    }

    if (cfg.reps == 0) {
        std::cerr << "Error: --reps must be >= 1" << std::endl;
        return 1;
    }
    if (!gdj_examples::path_distinguishes_passes(cfg.results_file, cfg.levels.size(), cfg.reps) ||
        !gdj_examples::path_distinguishes_passes(cfg.times_file, cfg.levels.size(), cfg.reps)) {
        std::cerr << "Error: with several levels or repetitions, --results_file/--times_file must contain {L} and, with --reps > 1, {R}" << std::endl;
        return 1;
    }

    std::cout << "Configuration:" << std::endl;
    std::cout << "  Dir Dataset:     " << cfg.dir_dataset << std::endl;
    std::cout << "  Variant:         " << cfg.variant << std::endl;
    std::cout << "  Queries File:    " << cfg.queries_file << std::endl;
    if (fixed_plan) {
        std::cout << "  Plan File:       " << cfg.plan_file << std::endl;
        std::cout << "  Plan Id:         " << cfg.plan_id << " (" << fixed_plan->steps.size() << " steps)" << std::endl;
    } else {
        std::cout << "  Plan Group:      " << cfg.plan_group << " (library default catalog, "
                   << default_plans_dir().string() << ")" << std::endl;
    }
    std::cout << "  Upper Bound:     " << cfg.upper_bound << std::endl;
    std::cout << "  Parallel Level:  " << gdj_examples::levels_to_string(cfg.levels) << " (affects only the last step)" << std::endl;
    std::cout << "  Reps:            " << cfg.reps << std::endl;
    std::cout << "  Grid Side:       " << cfg.grid_side << std::endl;
    std::cout << "  Time Out:        " << cfg.time_out << " seconds" << std::endl;
    std::cout << "  Wide Base:       " << gdj_examples::wide_base_to_string(cfg.wide) << std::endl;
    std::cout << "  Init Coef:       " << cfg.init_coef << std::endl;
    std::cout << "  Growth Factor:   " << cfg.growth << std::endl;
    std::cout << "  Growth Mode:     " << to_string(cfg.growth_mode) << std::endl;
    std::cout << "  Max Retries:     " << cfg.max_retries << (cfg.max_retries == 0 ? " (unlimited)" : "") << std::endl;
    std::cout << "  mxm:             " << (cfg.mxm ? "true" : "false") << " (k=" << (int)cfg.k << ")" << std::endl;
    std::cout << "  Results File:    " << (cfg.results_file.empty() ? "(disabled)" : cfg.results_file) << std::endl;
    std::cout << "  Times File:      " << (cfg.times_file.empty() ? "(disabled)" : cfg.times_file) << std::endl;

    auto all_queries = load_all_queries(cfg.queries_file);

    if (cfg.variant == "bv64")
        return run_variant<rank_bv_64_p>(cfg, all_queries, fixed_plan);
    if (cfg.variant == "v5")
        return run_variant<rank_vfamily_p<sdsl::rank_support_v5<>>>(cfg, all_queries, fixed_plan);
    if (cfg.variant == "v")
        return run_variant<rank_vfamily_p<sdsl::rank_support_v<>>>(cfg, all_queries, fixed_plan);
    if (cfg.variant == "sd")
        return run_variant<rank_sd_p>(cfg, all_queries, fixed_plan);
    if (cfg.variant == "rrr")
        return run_variant<rank_rrr_p>(cfg, all_queries, fixed_plan);
    if (cfg.variant == "rrr15")
        return run_variant<rank_rrr15_p>(cfg, all_queries, fixed_plan);

    std::cerr << "Error: --variant must be bv64|v5|v|sd|rrr|rrr15 (got: " << cfg.variant << ")" << std::endl;
    return 1;
}
