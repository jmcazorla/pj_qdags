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
#include "qdags/query/query_loader.hpp"
#include "qdags/strategy/generic_strategies_mxm.hpp"
#include "levels_util.hpp"
#include "wide_base_args.hpp"

// Perfilador de strategy_matrix_multiply (basic) y strategy_matrix_multiply_wp (con proyecciones) sobre todas las
// queries de un archivo, para un backend. No es una estrategia BGP general: usa solo los dos primeros patrones
// de cada query (R(y,x) JOIN S(x,z); build_mxm_context/build_mxm_wp_context) e ignora el resto.
//
// --mode basic|wp|both (default both). "wp" exige exactamente 2 patrones con una unica variable compartida x:
// con mas de 2, cual variable es "x" depende del orden de un unordered_map interno, asi que se salta wp (basic
// sigue si el modo lo incluye).
// Solo basic usa upper_bound/parallel_level; wp corre sin cota y secuencial.
//
// Uso:
//   run_mxm <dir_dataset> <queries_file> <upper_bound> <parallel_level[,level...]>
//           [--grid_side <val>] [--time_out <val>]
//           [--mode basic|wp|both]
//           [--variant bv64|v5|v|sd|rrr|rrr15]
//           [--results_file <path>] [--times_file <path>]
//
// --results_file/--times_file: una linea por query no saltada, "query_id;basic_result;wp_result" o
// "query_id;basic_ms;wp_ms".

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
    std::string dir_dataset, queries_file, variant = "bv64", mode = "both", results_file, times_file;
    std::vector<uint64_t> levels{0};
    uint32_t reps = 1;
    uint64_t upper_bound = 0, parallel_level = 0, grid_side = 52000000, time_out = 0;
};

template<class RankBackend>
static int run_pass(const Config &cfg, resource_manager<se_quadtree<RankBackend>> &res_mgr, const std::vector<query_tp> &all_queries) {
    using SeQuadT = se_quadtree<RankBackend>;
    using QdagT = qdag<RankBackend>;


    bool run_basic = (cfg.mode != "wp");
    bool run_wp    = (cfg.mode != "basic");

    std::vector<double> basic_times, wp_times;
    uint64_t n_skipped = 0, n_wp_skipped = 0;
    std::ofstream results_out, times_out;
    if (!cfg.results_file.empty()) {
        results_out.open(cfg.results_file);
        results_out << "query_id;basic_result;wp_result\n";
    }
    if (!cfg.times_file.empty()) {
        times_out.open(cfg.times_file);
        times_out << "query_id;basic_ms;wp_ms\n";
    }

    for (size_t q_idx = 0; q_idx < all_queries.size(); ++q_idx) {
        auto &query = all_queries[q_idx];
        if (query.size() < 2) {
            std::cout << "Query " << q_idx << ": skipped (mxm needs >= 2 patterns)" << std::endl;
            n_skipped++;
            continue;
        }

        uint64_t basic_result = 0;
        double basic_ms = 0.0;
        if (run_basic) {
            query_context_mxm<SeQuadT> ctx;
            build_mxm_context<SeQuadT>(query, res_mgr, ctx);
            if (!ctx.rel_R || !ctx.rel_S) {
                std::cout << "Query " << q_idx << " [basic]: skipped (predicate has no loaded SO relation)" << std::endl;
            } else {
                std::vector<QdagT*> temp_qdags, temp_proj_qdags;
                SeQuadT *proj_R = nullptr, *proj_S = nullptr, *join_proj = nullptr, *join_final = nullptr, *proj_final = nullptr;
                start_timeout_watchdog(cfg.time_out);
                auto t0 = high_resolution_clock::now();
                basic_result = strategy_matrix_multiply<QdagT, SeQuadT>(
                    ctx.rel_R, ctx.rel_S, cfg.grid_side, proj_R, proj_S, temp_qdags, temp_proj_qdags,
                    join_proj, join_final, proj_final, cfg.upper_bound, cfg.parallel_level);
                auto t1 = high_resolution_clock::now();
                stop_timeout_watchdog();
                basic_ms = duration<double, std::milli>(t1 - t0).count();
                delete proj_R; delete proj_S; delete join_proj; delete join_final; delete proj_final;
                for (auto *q : temp_qdags)
                    delete q;
                for (auto *q : temp_proj_qdags)
                    delete q;
            }
        }

        uint64_t wp_result = 0;
        double wp_ms = 0.0;
        if (run_wp) {
            if (query.size() != 2) {
                std::cout << "Query " << q_idx << " [wp]: skipped (needs exactly 2 patterns, has "
                           << query.size() << ")" << std::endl;
                n_wp_skipped++;
            } else {
                query_context_mxm<SeQuadT> ctx;
                build_mxm_wp_context<SeQuadT>(query, res_mgr, ctx);
                if (ctx.qts_proy_o_s.size() < 2 || ctx.relevant_qts_consts.size() < 2) {
                    std::cout << "Query " << q_idx << " [wp]: skipped (missing S/O projections or SO relations)" << std::endl;
                    n_wp_skipped++;
                } else {
                    std::vector<QdagT*> temp_qdags, temp_proj_qdags;
                    SeQuadT *join_proj = nullptr, *join_final = nullptr, *proj_final = nullptr, *proj_final_x = nullptr, *proj_final_y = nullptr;
                    start_timeout_watchdog(cfg.time_out);
                    auto t0 = high_resolution_clock::now();
                    wp_result = strategy_matrix_multiply_wp<QdagT, SeQuadT>(
                        ctx, temp_qdags, temp_proj_qdags, join_proj, join_final, proj_final, proj_final_x, proj_final_y);
                    auto t1 = high_resolution_clock::now();
                    stop_timeout_watchdog();
                    wp_ms = duration<double, std::milli>(t1 - t0).count();
                    delete join_proj; delete join_final; delete proj_final; delete proj_final_x; delete proj_final_y;
                    for (auto *q : temp_qdags)
                        delete q;
                    for (auto *q : temp_proj_qdags)
                        delete q;
                }
            }
        }

        std::cout << "Query " << q_idx << ": ";
        if (run_basic)
            std::cout << "basic=" << basic_result << " (" << std::fixed << std::setprecision(3) << basic_ms << "ms)";
        if (run_basic && run_wp)
            std::cout << " | ";
        if (run_wp)
            std::cout << "wp=" << wp_result << " (" << std::fixed << std::setprecision(3) << wp_ms << "ms)";
        std::cout << std::endl;

        if (run_basic)
            basic_times.push_back(basic_ms);
        if (run_wp && query.size() == 2)
            wp_times.push_back(wp_ms);
        if (results_out.is_open())
            results_out << q_idx << ";" << basic_result << ";" << wp_result << std::endl;
        if (times_out.is_open())
            times_out << q_idx << ";" << basic_ms << ";" << wp_ms << std::endl;
    }

    auto sum = [](const std::vector<double> &v) { double s = 0; for (double x : v) s += x; return s; };
    double b_total = sum(basic_times), w_total = sum(wp_times);
    double b_avg = basic_times.empty() ? 0.0 : b_total / basic_times.size();
    double w_avg = wp_times.empty() ? 0.0 : w_total / wp_times.size();

    std::cout << "-----------------------------------------------------" << std::endl;
    std::cout << "Total queries: " << all_queries.size() << " | skipped(<2 patterns): " << n_skipped
               << " | wp skipped(!=2 patterns or no data): " << n_wp_skipped << std::endl;
    std::cout << std::left << std::setw(10) << "basic"
               << " | total " << std::fixed << std::setprecision(3) << b_total << "ms"
               << " | mean "   << std::fixed << std::setprecision(3) << b_avg << "ms"
               << " | median " << std::fixed << std::setprecision(3) << median(basic_times) << "ms" << std::endl;
    std::cout << std::left << std::setw(10) << "wp"
               << " | total " << std::fixed << std::setprecision(3) << w_total << "ms"
               << " | mean "   << std::fixed << std::setprecision(3) << w_avg << "ms"
               << " | median " << std::fixed << std::setprecision(3) << median(wp_times) << "ms" << std::endl;

    std::cout << "RESULT;" << cfg.variant << ";" << all_queries.size() << ";" << n_skipped << ";"
               << b_total << ";" << b_avg << ";" << median(basic_times) << ";"
               << w_total << ";" << w_avg << ";" << median(wp_times) << std::endl;

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
                      " [--mode basic|wp|both]"
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
        else if (arg == "--mode" && i + 1 < argc)
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

    if (cfg.mode != "basic" && cfg.mode != "wp" && cfg.mode != "both") {
        std::cerr << "Error: --mode must be basic|wp|both (got: " << cfg.mode << ")" << std::endl;
        return 1;
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
    std::cout << "  Upper Bound:     " << cfg.upper_bound << " (basic only)" << std::endl;
    std::cout << "  Parallel Level:  " << gdj_examples::levels_to_string(cfg.levels) << " (basic only)" << std::endl;
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
