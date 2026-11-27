#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <string>
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
#include "qdags/query/query_builder.hpp"
#include "qdags/query/query_loader.hpp"
#include "qdags/strategy/generic_strategies.hpp"
#include "const_runs.hpp"
#include "levels_util.hpp"
#include "wide_base_args.hpp"

// Perfilador de strategy_orig: un solo multiJoinAuto sobre todos los qdags de la query, sin fases ni kernel.
// Sin variante adaptive (no hay fases que reintentar), asi que no hay --mode.
//
// Uso:
//   run_orig <dir_dataset> <queries_file> <upper_bound> <parallel_level[,level...]>
//            [--grid_side <val>] [--time_out <val>]
//            [--variant bv64|v5|v|sd|rrr|rrr15]
//            [--results_file <path>] [--times_file <path>]
//
// --results_file/--times_file: una linea por query no saltada, "query_id;result" o "query_id;ms".
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
    std::string dir_dataset, queries_file, variant = "bv64", results_file, times_file;
    std::vector<uint64_t> levels{0};
    uint32_t reps = 1;
    uint64_t upper_bound = 0, parallel_level = 0, grid_side = 52000000, time_out = 0;
};

template<class RankBackend>
static int run_pass(const Config &cfg, resource_manager<se_quadtree<RankBackend>> &res_mgr, const std::vector<query_tp> &all_queries) {
    using SeQuadT = se_quadtree<RankBackend>;
    using QdagT = qdag<RankBackend>;


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

    // Queries con S u O constante: se resuelven en otra unidad de compilacion (const_runs.hpp).
    auto run_with_constants = [&](const query_tp &query, size_t q_idx) __attribute__((noinline)) {
        gdj_examples::ConstRunArgs a;
        a.wide = cfg.wide;
        a.upper_bound = cfg.upper_bound; a.parallel_level = cfg.parallel_level;
        a.grid_side = cfg.grid_side; a.time_out = cfg.time_out;
        auto r = gdj_examples::run_orig_const<RankBackend>(a, res_mgr, query);
        if (!r.ran) {
            std::cout << "Query " << q_idx << " [constants]: skipped (" << r.skip_reason << ")" << std::endl;
            n_skipped++;
            return;
        }
        std::cout << "Query " << q_idx << " [constants]: result=" << r.generic_result
                   << " (" << std::fixed << std::setprecision(3) << r.generic_ms << "ms)" << std::endl;
        times.push_back(r.generic_ms);
        if (results_out.is_open())
            results_out << q_idx << ";" << r.generic_result << std::endl;
        if (times_out.is_open())
            times_out << q_idx << ";" << r.generic_ms << std::endl;
    };

    for (size_t q_idx = 0; q_idx < all_queries.size(); ++q_idx) {
        auto &query = all_queries[q_idx];
        if (has_constant_terms(query)) {
            run_with_constants(query, q_idx);
            continue;
        }
        auto rel = build_qdags_from_query<SeQuadT>(query, res_mgr, cfg.wide);
        if (rel.quadtrees.empty()) {
            std::cout << "Query " << q_idx << ": skipped (no resolvable patterns: constant P with variable S/O)" << std::endl;
            n_skipped++;
            continue;
        }

        std::vector<QdagT*> all_qdags;
        uint64_t n_results = 0;
        start_timeout_watchdog(cfg.time_out);
        auto t0 = high_resolution_clock::now();
        SeQuadT* result = strategy_orig<QdagT, SeQuadT>(
            rel, all_qdags, n_results, cfg.upper_bound, cfg.parallel_level, cfg.grid_side);
        auto t1 = high_resolution_clock::now();
        stop_timeout_watchdog();
        double ms = duration<double, std::milli>(t1 - t0).count();
        for (auto *q : all_qdags)
            delete q;
        delete result;

        std::cout << "Query " << q_idx << ": result=" << n_results
                   << " (" << std::fixed << std::setprecision(3) << ms << "ms)" << std::endl;

        times.push_back(ms);
        if (results_out.is_open())
            results_out << q_idx << ";" << n_results << std::endl;
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
static int run_variant(const Config &cfg, const std::vector<query_tp> &all_queries) {
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
            int rc = run_pass<RankBackend>(pass, res_mgr, all_queries);
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
                      " [--grid_side <val>] [--time_out <val>]"
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
        else if (arg == "--variant" && i + 1 < argc)
            cfg.variant = argv[++i];
        else if (arg == "--results_file" && i + 1 < argc)
            cfg.results_file = argv[++i];
        else if (arg == "--times_file" && i + 1 < argc)
            cfg.times_file = argv[++i];
        else if (arg == "--reps" && i + 1 < argc)
            cfg.reps = (uint32_t)std::stoul(argv[++i]);
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
    std::cout << "  Upper Bound:     " << cfg.upper_bound << std::endl;
    std::cout << "  Parallel Level:  " << gdj_examples::levels_to_string(cfg.levels) << std::endl;
    std::cout << "  Reps:            " << cfg.reps << std::endl;
    std::cout << "  Grid Side:       " << cfg.grid_side << std::endl;
    std::cout << "  Time Out:        " << cfg.time_out << " seconds" << std::endl;
    std::cout << "  Wide Base:       " << gdj_examples::wide_base_to_string(cfg.wide) << std::endl;
    std::cout << "  Results File:    " << (cfg.results_file.empty() ? "(disabled)" : cfg.results_file) << std::endl;
    std::cout << "  Times File:      " << (cfg.times_file.empty() ? "(disabled)" : cfg.times_file) << std::endl;

    auto all_queries = load_all_queries(cfg.queries_file);

    if (cfg.variant == "bv64")
        return run_variant<rank_bv_64_p>(cfg, all_queries);
    if (cfg.variant == "v5")
        return run_variant<rank_vfamily_p<sdsl::rank_support_v5<>>>(cfg, all_queries);
    if (cfg.variant == "v")
        return run_variant<rank_vfamily_p<sdsl::rank_support_v<>>>(cfg, all_queries);
    if (cfg.variant == "sd")
        return run_variant<rank_sd_p>(cfg, all_queries);
    if (cfg.variant == "rrr")
        return run_variant<rank_rrr_p>(cfg, all_queries);
    if (cfg.variant == "rrr15")
        return run_variant<rank_rrr15_p>(cfg, all_queries);

    std::cerr << "Error: --variant must be bv64|v5|v|sd|rrr|rrr15 (got: " << cfg.variant << ")" << std::endl;
    return 1;
}
