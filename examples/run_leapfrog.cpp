#include <iostream>
#include <iomanip>
#include <sstream>
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
#include "qdags/query/leapfrog_builder.hpp"
#include "qdags/query/query_loader.hpp"
#include "qdags/strategy/generic_strategies_leapfrog.hpp"
#include "const_runs.hpp"
#include "levels_util.hpp"
#include "mxm_final.hpp"
#include "wide_base_args.hpp"

// Perfilador de strategy_leapfrog y strategy_leapfrog_adaptive sobre todas las queries de un archivo, para un
// backend; misma estructura que run_wk.cpp.
//
// Sirve para topologia desconocida: el plan (orden de variables + cadena de prejoins) se arma por query con
// build_leapfrog_metadata. "generic" corta los prejoins no exactos a coef_bound*upper_bound (0 = sin cota);
// "adaptive" hace crecer esa cota con reintentos. Los prejoins exactos se cortan a upper_bound en los dos modos.
//
// Uso:
//   run_leapfrog <dir_dataset> <queries_file> <upper_bound> <parallel_level[,nivel...]>
//                [--grid_side <val>] [--time_out <val>] [--coef_bound <val>]
//                [--init_coef <val>] [--growth <val>] [--growth_mode fixed|proportional] [--max_retries <val>]
//                [--parallel_phases first_only|last_only|all]
//                [--mode generic|adaptive|both]
//                [--lf_order proj_size|degree] [--lf_depth full|non_lonely]
//                [--lf_max_atts <val>]
//                [--variant bv64|v5|v|sd|rrr|rrr15]
//                [--variant_out bv64|v5|v|sd|rrr|rrr15]
//                [--results_file <path>] [--times_file <path>]
//
// --lf_order: criterio principal del orden de variables (el otro desempata).
// --lf_depth: full = un prejoin por variable; non_lonely = solo las non-lonely (las lonely entran juntas en el
// join final).
// --lf_max_atts: tope de atributos de un prejoin; la cadena se corta ahi y el join final agrega el resto.
// 0 (default) = automatico: 4 hasta 5 variables; en las mas anchas, max(3, #non-lonely) para llegar al primer
// paso exacto. El motor admite hasta 9.
// --variant_out (default = --variant): backend del resultado final; solo cambia el join final.
// --mode/--variant/--results_file/--times_file: como en run_nl.cpp.

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

struct Config {
    gdj::wide_base_config wide;
    std::string dir_dataset, queries_file, variant = "bv64", variant_out, mode = "both", results_file, times_file;
    std::vector<uint64_t> levels{0};
    uint32_t reps = 1;
    bool mxm = false;   // la query es una cadena de matrices: se mide pi_{extremos}(resultado)
    uint64_t upper_bound = 0, parallel_level = 0, grid_side = 52000000, time_out = 0;
    uint64_t coef_bound = 0, init_coef = 2;
    uint32_t growth = 2, max_retries = 15;
    GrowthMode growth_mode = GrowthMode::Fixed;
    ParallelPhases parallel_phases = ParallelPhases::LastOnly;
    leapfrog_config leapfrog;
};

template<class RankBackendIn, class RankBackendOut>
static int run_pass(const Config &cfg, resource_manager<se_quadtree<RankBackendIn>> &res_mgr, const std::vector<query_tp> &all_queries) {
    using SeQuadT    = se_quadtree<RankBackendIn>;
    using QdagT      = qdag<RankBackendIn>;
    using SeQuadTOut = se_quadtree<RankBackendOut>;


    bool run_generic = (cfg.mode != "adaptive");
    bool run_adaptive = (cfg.mode != "generic");

    std::vector<double> generic_times, adaptive_times;
    uint64_t n_skipped = 0;
    std::ofstream results_out, times_out;
    if (!cfg.results_file.empty()) {
        results_out.open(cfg.results_file);
        // *_size_bytes: size_in_bytes() del resultado final en --variant_out (0 si la pasada no corrio o la
        // query tiene constantes: run_with_constants no lo mide).
        results_out << "query_id;generic_result;adaptive_result;generic_size_bytes;adaptive_size_bytes\n";
    }
    if (!cfg.times_file.empty()) {
        times_out.open(cfg.times_file);
        times_out << "query_id;generic_ms;adaptive_ms\n";
    }

    // Queries con S u O constante: se resuelven en otra unidad de compilacion (const_runs.hpp).
    auto run_with_constants = [&](const query_tp &query, size_t q_idx) __attribute__((noinline)) {
        gdj_examples::ConstRunArgs a;
        a.wide = cfg.wide;
        a.upper_bound = cfg.upper_bound; a.parallel_level = cfg.parallel_level;
        a.grid_side = cfg.grid_side; a.time_out = cfg.time_out;
        a.coef_bound = cfg.coef_bound; a.init_coef = cfg.init_coef; a.growth = cfg.growth; a.max_retries = cfg.max_retries; a.growth_mode = (uint8_t)cfg.growth_mode;
        a.parallel_phases = (uint8_t)cfg.parallel_phases;
        a.run_generic = run_generic; a.run_adaptive = run_adaptive;
        a.lf_order = (uint8_t)cfg.leapfrog.order; a.lf_depth = (uint8_t)cfg.leapfrog.depth; a.lf_max_atts = cfg.leapfrog.max_prejoin_atts;
        auto r = gdj_examples::run_leapfrog_const<RankBackendIn, RankBackendOut>(a, res_mgr, query);
        if (!r.ran) {
            std::cout << "Query " << q_idx << " [constants]: skipped (" << r.skip_reason << ")" << std::endl;
            n_skipped++;
            return;
        }
        std::cout << "Query " << q_idx << " [constants]: prejoins=" << r.n_prejoins << " (exact=" << r.n_exact << ") | ";
        if (run_generic)
            std::cout << "generic=" << r.generic_result << " (" << std::fixed << std::setprecision(3) << r.generic_ms << "ms)";
        if (run_generic && run_adaptive)
            std::cout << " | ";
        if (run_adaptive)
            std::cout << "adaptive=" << r.adaptive_result << " (" << std::fixed << std::setprecision(3) << r.adaptive_ms << "ms"
                       << ", attempts=" << r.attempts << ", final_coef=" << r.coef_used
                       << ", " << (r.exhaustive ? "exhaustive" : "not-exhaustive") << ")";
        std::cout << std::endl;
        if (run_generic)
            generic_times.push_back(r.generic_ms);
        if (run_adaptive)
            adaptive_times.push_back(r.adaptive_ms);
        if (results_out.is_open())
            // size_bytes no instrumentado en run_with_constants (ConstRunResult no lo trae).
            results_out << q_idx << ";" << r.generic_result << ";" << r.adaptive_result << ";0;0" << std::endl;
        if (times_out.is_open())
            times_out << q_idx << ";" << r.generic_ms << ";" << r.adaptive_ms << std::endl;
    };

    for (size_t q_idx = 0; q_idx < all_queries.size(); ++q_idx) {
        auto &query = all_queries[q_idx];
        if (has_constant_terms(query)) {
            run_with_constants(query, q_idx);
            continue;
        }
        auto meta = build_leapfrog_metadata<SeQuadT>(query, res_mgr, cfg.leapfrog, cfg.wide);
        if (meta.steps.empty() && meta.final_qts.empty()) {
            std::cout << "Query " << q_idx << ": skipped (no usable patterns)" << std::endl;
            n_skipped++;
            continue;
        }

        uint64_t generic_result = 0;
        double generic_ms = 0.0;
        uint64_t generic_size_bytes = 0;
        if (run_generic) {
            std::vector<QdagT*> qd_g;
            start_timeout_watchdog(cfg.time_out);
            auto t0 = high_resolution_clock::now();
            SeQuadTOut* g_final = strategy_leapfrog<QdagT, SeQuadT, SeQuadTOut>(
                meta, qd_g, generic_result, cfg.upper_bound, cfg.coef_bound, cfg.parallel_level, cfg.grid_side, cfg.parallel_phases);
            if (cfg.mxm)
                generic_result = gdj_examples::mxm_projected_count(query, g_final);
            auto t1 = high_resolution_clock::now();
            stop_timeout_watchdog();
            generic_ms = duration<double, std::milli>(t1 - t0).count();
            if (g_final)
                generic_size_bytes = g_final->size_in_bytes();
            for (auto *q : qd_g)
                delete q;
            delete g_final;
        }

        uint64_t adaptive_result = 0;
        double adaptive_ms = 0.0;
        uint64_t adaptive_size_bytes = 0;
        uint32_t attempts = 0; uint64_t coef_used = 0; bool exhaustive = false;
        if (run_adaptive) {
            std::vector<QdagT*> qd_a;
            start_timeout_watchdog(cfg.time_out);
            auto t2 = high_resolution_clock::now();
            SeQuadTOut* a_final = strategy_leapfrog_adaptive<QdagT, SeQuadT, SeQuadTOut>(
                meta, qd_a, adaptive_result, cfg.upper_bound, cfg.init_coef, cfg.growth, cfg.max_retries,
                cfg.parallel_level, cfg.grid_side, cfg.parallel_phases, &attempts, &coef_used, &exhaustive, cfg.growth_mode);
            if (cfg.mxm)
                adaptive_result = gdj_examples::mxm_projected_count(query, a_final);
            auto t3 = high_resolution_clock::now();
            stop_timeout_watchdog();
            adaptive_ms = duration<double, std::milli>(t3 - t2).count();
            if (a_final)
                adaptive_size_bytes = a_final->size_in_bytes();
            delete a_final;
            for (auto *q : qd_a)
                delete q;
        }

        size_t n_exact = 0;
        for (auto &step : meta.steps)
            if (step.exact)
                n_exact++;
        std::cout << "Query " << q_idx << ": prejoins=" << meta.steps.size() << " (exact=" << n_exact << ") | ";
        if (run_generic)
            std::cout << "generic=" << generic_result << " (" << std::fixed << std::setprecision(3) << generic_ms << "ms)";
        if (run_generic && run_adaptive)
            std::cout << " | ";
        if (run_adaptive)
            std::cout << "adaptive=" << adaptive_result << " (" << std::fixed << std::setprecision(3) << adaptive_ms << "ms"
                       << ", attempts=" << attempts << ", final_coef=" << coef_used
                       << ", " << (exhaustive ? "exhaustive" : "not-exhaustive") << ")";
        std::cout << std::endl;

        if (run_generic)
            generic_times.push_back(generic_ms);
        if (run_adaptive)
            adaptive_times.push_back(adaptive_ms);
        if (results_out.is_open())
            results_out << q_idx << ";" << generic_result << ";" << adaptive_result
                        << ";" << generic_size_bytes << ";" << adaptive_size_bytes << std::endl;
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
template<class RankBackendIn, class RankBackendOut>
static int run_variant(const Config &cfg, const std::vector<query_tp> &all_queries) {
    using SeQuadT    = se_quadtree<RankBackendIn>;

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
            int rc = run_pass<RankBackendIn, RankBackendOut>(pass, res_mgr, all_queries);
            if (rc != 0)
                return rc;
        }
    }
    return 0;
}

// Variantes disponibles; se expanden (entrada x salida) con X-macro.
#define QDAGS_VARIANT_LIST(X) \
    X(bv64,  rank_bv_64_p) \
    X(v5,    rank_vfamily_p<sdsl::rank_support_v5<>>) \
    X(v,     rank_vfamily_p<sdsl::rank_support_v<>>) \
    X(sd,    rank_sd_p) \
    X(rrr,   rank_rrr_p) \
    X(rrr15, rank_rrr15_p)

template<class RankBackendIn>
static int dispatch_out(const Config &cfg, const std::vector<query_tp> &qs) {
#define X(name, type) if (cfg.variant_out == #name) return run_variant<RankBackendIn, type>(cfg, qs);
    QDAGS_VARIANT_LIST(X)
#undef X
    std::cerr << "Error: --variant_out must be bv64|v5|v|sd|rrr|rrr15 (got: " << cfg.variant_out << ")" << std::endl;
    return 1;
}

static int dispatch_in(const Config &cfg, const std::vector<query_tp> &qs) {
#define X(name, type) if (cfg.variant == #name) return dispatch_out<type>(cfg, qs);
    QDAGS_VARIANT_LIST(X)
#undef X
    std::cerr << "Error: --variant must be bv64|v5|v|sd|rrr|rrr15 (got: " << cfg.variant << ")" << std::endl;
    return 1;
}

int main(int argc, char **argv) {
    const int mandatory_args = 4;
    if (argc < mandatory_args + 1) {
        std::cerr << "Usage: " << argv[0]
                   << " <dir_dataset> <queries_file> <upper_bound> <parallel_level[,level...]>"
                      " [--grid_side <val>] [--time_out <val>] [--coef_bound <val>]"
                      " [--init_coef <val>] [--growth <val>] [--growth_mode fixed|proportional] [--max_retries <val>]"
                      " [--parallel_phases first_only|last_only|all]"
                      " [--mode generic|adaptive|both]"
                      " [--lf_order proj_size|degree] [--lf_depth full|non_lonely]"
                      " [--lf_max_atts <val>]"
                      " [--variant bv64|v5|v|sd|rrr|rrr15]"
                      " [--variant_out bv64|v5|v|sd|rrr|rrr15]"
                      " [--reps <n>] [--mxm] [--results_file <path>] [--times_file <path>]" << gdj_examples::WIDE_BASE_USAGE << std::endl;
        return 1;
    }

    Config cfg;
    cfg.dir_dataset = argv[1];
    cfg.queries_file = argv[2];
    cfg.upper_bound = std::stoull(argv[3]);
    try { cfg.levels = gdj_examples::parse_levels(argv[4]); }
    catch (const std::exception &e) { std::cerr << "Error: invalid <parallel_level[,level...]> (" << e.what() << ")" << std::endl; return 1; }
    cfg.parallel_level = cfg.levels.front();

    std::string lf_order = "proj_size", lf_depth = "full";
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
        else if (arg == "--coef_bound" && i + 1 < argc)
            cfg.coef_bound = std::stoull(argv[++i]);
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
        else if (arg == "--parallel_phases" && i + 1 < argc) {
            std::string v = argv[++i];
            cfg.parallel_phases = (v == "first_only") ? ParallelPhases::FirstOnly
                                 : (v == "last_only")  ? ParallelPhases::LastOnly
                                 : ParallelPhases::AllPhases;
        } else if (arg == "--mode" && i + 1 < argc)
            cfg.mode = argv[++i];
        else if (arg == "--variant" && i + 1 < argc)
            cfg.variant = argv[++i];
        else if (arg == "--variant_out" && i + 1 < argc)
            cfg.variant_out = argv[++i];
        else if (arg == "--lf_order" && i + 1 < argc)
            lf_order = argv[++i];
        else if (arg == "--lf_depth" && i + 1 < argc)
            lf_depth = argv[++i];
        else if (arg == "--lf_max_atts" && i + 1 < argc)
            cfg.leapfrog.max_prejoin_atts = std::stoull(argv[++i]);
        else if (arg == "--results_file" && i + 1 < argc)
            cfg.results_file = argv[++i];
        else if (arg == "--times_file" && i + 1 < argc)
            cfg.times_file = argv[++i];
        else if (arg == "--reps" && i + 1 < argc)
            cfg.reps = (uint32_t)std::stoul(argv[++i]);
        else if (arg == "--mxm")
            cfg.mxm = true;
    }

    if (cfg.mode != "generic" && cfg.mode != "adaptive" && cfg.mode != "both") {
        std::cerr << "Error: --mode must be generic|adaptive|both (got: " << cfg.mode << ")" << std::endl;
        return 1;
    }

    if (lf_order != "proj_size" && lf_order != "degree") {
        std::cerr << "Error: --lf_order must be proj_size|degree (got: " << lf_order << ")" << std::endl;
        return 1;
    }
    if (lf_depth != "full" && lf_depth != "non_lonely") {
        std::cerr << "Error: --lf_depth must be full|non_lonely (got: " << lf_depth << ")" << std::endl;
        return 1;
    }
    cfg.leapfrog.order = parse_leapfrog_order(lf_order);
    cfg.leapfrog.depth = parse_leapfrog_depth(lf_depth);
    if (cfg.variant_out.empty())
        cfg.variant_out = cfg.variant;

    if (cfg.mxm && cfg.upper_bound != 0) {
        std::cerr << "Error: --mxm requires upper_bound 0 (a bound would project a truncated join, not the matrix)" << std::endl;
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
    std::cout << "  Variant Out:     " << cfg.variant_out << std::endl;
    std::cout << "  Mode:            " << cfg.mode << std::endl;
    std::cout << "  Leapfrog Order:  " << to_string(cfg.leapfrog.order) << std::endl;
    std::cout << "  Leapfrog Depth:  " << to_string(cfg.leapfrog.depth) << std::endl;
    std::cout << "  Leapfrog Max Atts: " << cfg.leapfrog.max_prejoin_atts << (cfg.leapfrog.max_prejoin_atts == 0 ? " (auto)" : "") << std::endl;
    std::cout << "  Queries File:    " << cfg.queries_file << std::endl;
    std::cout << "  Upper Bound:     " << cfg.upper_bound << std::endl;
    std::cout << "  Parallel Level:  " << gdj_examples::levels_to_string(cfg.levels) << std::endl;
    std::cout << "  Reps:            " << cfg.reps << std::endl;
    std::cout << "  Mxm:             " << (cfg.mxm ? "true (measures the projected matrix)" : "false") << std::endl;
    std::cout << "  Parallel Phases: " << parallel_phases_to_string(cfg.parallel_phases) << std::endl;
    std::cout << "  Grid Side:       " << cfg.grid_side << std::endl;
    std::cout << "  Time Out:        " << cfg.time_out << " seconds" << std::endl;
    std::cout << "  Wide Base:       " << gdj_examples::wide_base_to_string(cfg.wide) << std::endl;
    std::cout << "  Coef Bound (generic):  " << cfg.coef_bound << std::endl;
    std::cout << "  Init Coef (adaptive):  " << cfg.init_coef << std::endl;
    std::cout << "  Growth Factor:   " << cfg.growth << std::endl;
    std::cout << "  Growth Mode:     " << to_string(cfg.growth_mode) << std::endl;
    std::cout << "  Max Retries:     " << cfg.max_retries << (cfg.max_retries == 0 ? " (unlimited)" : "") << std::endl;
    std::cout << "  Results File:    " << (cfg.results_file.empty() ? "(disabled)" : cfg.results_file) << std::endl;
    std::cout << "  Times File:      " << (cfg.times_file.empty() ? "(disabled)" : cfg.times_file) << std::endl;

    auto all_queries = load_all_queries(cfg.queries_file);

    return dispatch_in(cfg, all_queries);
}
