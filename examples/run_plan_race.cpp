#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <string>
#include <map>
#include <functional>
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
#include "qdags/query/triple_pattern.hpp"
#include "qdags/query/resource_manager.hpp"
#include "qdags/query/query_loader.hpp"
#include "qdags/query/classify_query.hpp"
#include "qdags/query/plan_parser.hpp"
#include "qdags/query/topology_mapping.hpp"
#include "qdags/query/plan_builder.hpp"
#include "qdags/query/topology_plan_selection.hpp"
#include "qdags/strategy/generic_strategies_plan.hpp"
#include "qdags/strategy/strategy_race.hpp"
#include "qdags/strategy/generic_strategies_race.hpp"
#include "qdags/strategy/generic_strategies_plan_race.hpp"
#include "levels_util.hpp"
#include "wide_base_args.hpp"

// Perfilador de strategy_plan_raced(_adaptive): corre en dos hilos la version secuencial y la paralela y se
// queda con la que termine primero. strategy_plan escribe en su query_metadata_plan (step.result_qt por paso),
// asi que compartirlo entre las dos ramas seria una carrera de datos: la estrategia recibe una fabrica
// (std::function) que arma un query_metadata_plan nuevo por rama (ver generic_strategies_plan_race.hpp).
//
// Mismas convenciones que run_nl_race.cpp: --time_out se reenvia a raceStrategies y el "ms" por query es el
// tiempo de pared de la carrera.
// El plan se resuelve como en run_plan.cpp, una vez por query (no por rama): las dos ramas usan el mismo plan.
//
// Uso:
//   run_plan_race <dir_dataset> <queries_file> <upper_bound> <parallel_level[,nivel...]>
//                 (--plan_file <path> --plan_id "<key>" | --plan_group normal|best|wk_k|leapfrog)
//                 [--grid_side <val>] [--time_out <val>]
//                 [--init_coef <val>] [--growth <val>] [--growth_mode fixed|proportional] [--max_retries <val>] [--coef_bound <val>]
//                 [--mxm] [--k <val>]
//                 [--parallel_phases first_only|last_only|all]
//                 [--mode generic|adaptive|both]
//                 [--variant bv64|v5|v|sd|rrr|rrr15]
//                 [--results_file <path>] [--times_file <path>]

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

static std::string parallel_phases_to_string(ParallelPhases p) {
    switch (p) {
        case ParallelPhases::FirstOnly:
            return "first_only";
        case ParallelPhases::LastOnly:
            return "last_only";
        default:
            return "all";
    }
}

static std::string winner_str(int w) {
    return w == 0 ? "seq" : (w == 1 ? "par" : "?");
}

struct Config {
    gdj::wide_base_config wide;
    std::string dir_dataset, queries_file, plan_file, plan_id, plan_group, variant = "bv64", mode = "both", results_file, times_file;
    std::vector<uint64_t> levels{0};
    uint32_t reps = 1;
    uint64_t upper_bound = 0, parallel_level = 0, grid_side = 52000000, time_out = 0, init_coef = 2;
    uint32_t growth = 2, max_retries = 15, coef_bound = 0;
    GrowthMode growth_mode = GrowthMode::Fixed;
    bool mxm = false;
    uint8_t k = 2;
    ParallelPhases parallel_phases = ParallelPhases::LastOnly;
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


    bool run_generic = (cfg.mode != "adaptive");
    bool run_adaptive = (cfg.mode != "generic");

    plan_catalog_cache cache;
    PlanGroup group = parse_plan_group(cfg.plan_group);
    auto resolve_plan = [&](QueryTopology topo) -> const execution_plan* {
        return fixed_plan ? fixed_plan : cache.get(topo, group);
    };

    std::vector<double> generic_times, adaptive_times;
    uint64_t n_skipped = 0;
    std::ofstream results_out, times_out;
    if (!cfg.results_file.empty()) {
        results_out.open(cfg.results_file);
        results_out << "query_id;generic_result;adaptive_result\n";
    }
    if (!cfg.times_file.empty()) {
        times_out.open(cfg.times_file);
        times_out << "query_id;generic_ms;adaptive_ms\n";
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

        query_metadata_plan<SeQuadT> probe_meta;
        if (!build_meta_for_topology<SeQuadT>(topo, query, res_mgr, *plan, probe_meta)) {
            std::cout << "Query " << q_idx << " [" << topo << "]: skipped (topology without plan builder, "
                          "or the query does not match the topology the plan expects)" << std::endl;
            n_skipped++;
            continue;
        }
        probe_meta.clearResults();

        std::function<query_metadata_plan<SeQuadT>()> build_meta = [&]() -> query_metadata_plan<SeQuadT> {
            query_metadata_plan<SeQuadT> meta;
            meta.wide = cfg.wide;
            build_meta_for_topology<SeQuadT>(topo, query, res_mgr, *plan, meta);
            return meta;
        };

        uint64_t generic_result = 0;
        double generic_ms = 0.0;
        RaceStats stats;
        if (run_generic) {
            auto t0 = high_resolution_clock::now();
            generic_result = strategy_plan_raced<QdagT, SeQuadT>(
                build_meta, cfg.upper_bound, cfg.parallel_level, cfg.grid_side, cfg.coef_bound,
                cfg.mxm, cfg.k, cfg.parallel_phases, cfg.time_out, &stats);
            auto t1 = high_resolution_clock::now();
            generic_ms = duration<double, std::milli>(t1 - t0).count();
        }

        uint64_t adaptive_result = 0;
        double adaptive_ms = 0.0;
        RaceStatsAdaptive stats_a;
        if (run_adaptive) {
            auto t2 = high_resolution_clock::now();
            adaptive_result = strategy_plan_raced_adaptive<QdagT, SeQuadT>(
                build_meta, cfg.upper_bound, cfg.init_coef, cfg.growth, cfg.max_retries,
                cfg.parallel_level, cfg.grid_side, cfg.mxm, cfg.k, cfg.parallel_phases, cfg.time_out, &stats_a, cfg.growth_mode);
            auto t3 = high_resolution_clock::now();
            adaptive_ms = duration<double, std::milli>(t3 - t2).count();
        }

        std::cout << "Query " << q_idx << " [" << topo << "]: ";
        if (run_generic)
            std::cout << "generic=" << generic_result << " (" << std::fixed << std::setprecision(3) << generic_ms
                       << "ms, winner=" << winner_str(stats.winner)
                       << ", seq=" << stats.seq_time * 1000.0 << "ms, par=" << stats.par_time * 1000.0 << "ms)";
        if (run_generic && run_adaptive)
            std::cout << " | ";
        if (run_adaptive)
            std::cout << "adaptive=" << adaptive_result << " (" << std::fixed << std::setprecision(3) << adaptive_ms
                       << "ms, winner=" << winner_str(stats_a.winner)
                       << ", seq_attempts=" << stats_a.seq_attempts << ", par_attempts=" << stats_a.par_attempts << ")";
        std::cout << std::endl;

        if (run_generic)
            generic_times.push_back(generic_ms);
        if (run_adaptive)
            adaptive_times.push_back(adaptive_ms);
        if (results_out.is_open())
            results_out << q_idx << ";" << generic_result << ";" << adaptive_result << std::endl;
        if (times_out.is_open())
            times_out << q_idx << ";" << generic_ms << ";" << adaptive_ms << std::endl;
    }

    auto sum = [](const std::vector<double> &v) { double s = 0; for (double x : v) s += x; return s; };
    double g_total = sum(generic_times), a_total = sum(adaptive_times);
    double g_avg = generic_times.empty() ? 0.0 : g_total / generic_times.size();
    double a_avg = adaptive_times.empty() ? 0.0 : a_total / adaptive_times.size();

    std::cout << "-----------------------------------------------------" << std::endl;
    std::cout << "Total queries: " << all_queries.size() << " | skipped: " << n_skipped << std::endl;
    std::cout << std::left << std::setw(10) << "generic"
               << " | total " << std::fixed << std::setprecision(3) << g_total << "ms"
               << " | mean "   << std::fixed << std::setprecision(3) << g_avg << "ms"
               << " | median " << std::fixed << std::setprecision(3) << median(generic_times) << "ms" << std::endl;
    std::cout << std::left << std::setw(10) << "adaptive"
               << " | total " << std::fixed << std::setprecision(3) << a_total << "ms"
               << " | mean "   << std::fixed << std::setprecision(3) << a_avg << "ms"
               << " | median " << std::fixed << std::setprecision(3) << median(adaptive_times) << "ms" << std::endl;

    std::cout << "RESULT;" << cfg.variant << ";" << all_queries.size() << ";" << n_skipped << ";"
               << g_total << ";" << g_avg << ";" << median(generic_times) << ";"
               << a_total << ";" << a_avg << ";" << median(adaptive_times) << std::endl;

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
                      " [--init_coef <val>] [--growth <val>] [--growth_mode fixed|proportional] [--max_retries <val>] [--coef_bound <val>]"
                      " [--mxm] [--k <val>]"
                      " [--parallel_phases first_only|last_only|all]"
                      " [--mode generic|adaptive|both]"
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
        else if (arg == "--coef_bound" && i + 1 < argc)
            cfg.coef_bound = (uint32_t)std::stoul(argv[++i]);
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
        else if (arg == "--parallel_phases" && i + 1 < argc) {
            std::string v = argv[++i];
            cfg.parallel_phases = (v == "first_only") ? ParallelPhases::FirstOnly
                                 : (v == "last_only")  ? ParallelPhases::LastOnly
                                 : ParallelPhases::AllPhases;
        } else if (arg == "--mode" && i + 1 < argc)
            cfg.mode = argv[++i];
        else if (arg == "--variant" && i + 1 < argc)
            cfg.variant = argv[++i];
        else if (arg == "--results_file" && i + 1 < argc)
            cfg.results_file = argv[++i];
        else if (arg == "--times_file" && i + 1 < argc)
            cfg.times_file = argv[++i];
        else if (arg == "--reps" && i + 1 < argc)
            cfg.reps = (uint32_t)std::stoul(argv[++i]);
    }

    if (cfg.mode != "generic" && cfg.mode != "adaptive" && cfg.mode != "both") {
        std::cerr << "Error: --mode must be generic|adaptive|both (got: " << cfg.mode << ")" << std::endl;
        return 1;
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
    std::cout << "  Mode:            " << cfg.mode << std::endl;
    std::cout << "  Queries File:    " << cfg.queries_file << std::endl;
    if (fixed_plan) {
        std::cout << "  Plan File:       " << cfg.plan_file << std::endl;
        std::cout << "  Plan Id:         " << cfg.plan_id << " (" << fixed_plan->steps.size() << " steps)" << std::endl;
    } else {
        std::cout << "  Plan Group:      " << cfg.plan_group << " (library default catalog, "
                   << default_plans_dir().string() << ")" << std::endl;
    }
    std::cout << "  Upper Bound:     " << cfg.upper_bound << std::endl;
    std::cout << "  Parallel Level:  " << gdj_examples::levels_to_string(cfg.levels) << std::endl;
    std::cout << "  Reps:            " << cfg.reps << std::endl;
    std::cout << "  Parallel Phases: " << parallel_phases_to_string(cfg.parallel_phases) << std::endl;
    std::cout << "  Grid Side:       " << cfg.grid_side << std::endl;
    std::cout << "  Time Out:        " << cfg.time_out << " seconds (passed to raceStrategies)" << std::endl;
    std::cout << "  Wide Base:       " << gdj_examples::wide_base_to_string(cfg.wide) << std::endl;
    std::cout << "  Coef Bound (generic): " << cfg.coef_bound << std::endl;
    std::cout << "  Init Coef (adaptive): " << cfg.init_coef << std::endl;
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
