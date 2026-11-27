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
#include "qdags/query/query_builder.hpp"
#include "qdags/query/query_loader.hpp"
#include "qdags/strategy/generic_strategies.hpp"
#include "qdags/strategy/generic_strategies_kernel.hpp"
#include "const_runs.hpp"
#include "levels_util.hpp"
#include "mxm_final.hpp"
#include "wide_base_args.hpp"

// Perfilador de strategy_kernel sobre todas las queries de un archivo, para un backend; misma estructura que
// run_wk.cpp.
//
// Kernel exige variables lonely y non-lonely, y a lo mas KERNEL_MAX_NONLONELY_ATTS non-lonely; las demas se
// saltan (cuentan en "skipped"; "-" el primer caso, "+" el segundo en --results_file/--times_file). Sin modo
// adaptive: el prejoin I es exacto, una pasada da min(upper_bound, |Q|).
//
// Uso:
//   run_kernel <dir_dataset> <queries_file> <upper_bound> <parallel_level[,nivel...]>
//              [--grid_side <val>] [--time_out <val>]
//              [--parallel_phases first_only|last_only|all]
//              [--variant bv64|v5|v|sd|rrr|rrr15]
//              [--variant_out bv64|v5|v|sd|rrr|rrr15]
//              [--results_file <path>] [--times_file <path>]
//
// --variant_out: backend del resultado final (default = --variant); el dataset debe corresponder a --variant.
// --parallel_phases: first_only = I; all = I, reducciones y final; last_only (default) = solo el join final.

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
    std::string dir_dataset, queries_file, variant = "bv64", variant_out, results_file, times_file;
    std::vector<uint64_t> levels{0};
    uint32_t reps = 1;
    bool mxm = false;   // la query es una cadena de matrices: se mide pi_{extremos}(resultado)
    uint64_t upper_bound = 0, parallel_level = 0, grid_side = 52000000, time_out = 0;
    ParallelPhases parallel_phases = ParallelPhases::LastOnly;
};

template<class RankBackendIn, class RankBackendOut>
static int run_pass(const Config &cfg, resource_manager<se_quadtree<RankBackendIn>> &res_mgr, const std::vector<query_tp> &all_queries) {
    using SeQuadT = se_quadtree<RankBackendIn>;
    using SeQuadTOut = se_quadtree<RankBackendOut>;
    using QdagT = qdag<RankBackendIn>;


    std::vector<double> times;
    uint64_t n_skipped = 0;
    std::ofstream results_out, times_out;
    if (!cfg.results_file.empty()) {
        results_out.open(cfg.results_file);
        results_out << "query_id;kernel_result\n";
    }
    if (!cfg.times_file.empty()) {
        times_out.open(cfg.times_file);
        times_out << "query_id;kernel_ms\n";
    }

    // Queries con S u O constante: se resuelven en otra unidad de compilacion (const_runs.hpp).
    auto run_with_constants = [&](const query_tp &query, size_t q_idx) __attribute__((noinline)) {
        gdj_examples::ConstRunArgs a;
        a.wide = cfg.wide;
        a.upper_bound = cfg.upper_bound; a.parallel_level = cfg.parallel_level;
        a.grid_side = cfg.grid_side; a.time_out = cfg.time_out;
        a.parallel_phases = (uint8_t)cfg.parallel_phases;
        auto r = gdj_examples::run_kernel_const<RankBackendIn, RankBackendOut>(a, res_mgr, query);
        if (!r.ran) {
            std::cout << "Query " << q_idx << " [constants]: skipped (" << r.skip_reason << ")" << std::endl;
            n_skipped++;
            if (results_out.is_open())
                results_out << q_idx << ";" << r.skip_marker << std::endl;
            if (times_out.is_open())
                times_out << q_idx << ";" << r.skip_marker << std::endl;
            return;
        }
        std::cout << "Query " << q_idx << " [constants]: kernel=" << r.generic_result << " (" << std::fixed << std::setprecision(3) << r.generic_ms << "ms)" << std::endl;
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
        auto meta = build_nl_metadata<SeQuadT>(query, res_mgr, cfg.wide);
        // "-"/"+" en vez de omitir la fila: el query_id queda en los 2 archivos (se cruzan linea a linea con
        // otras estrategias) y se ve por que se salto. Las saltadas no entran en `times`.
        if (meta.nl_quadtrees.empty() || meta.l_quadtrees.empty()) {
            std::cout << "Query " << q_idx << ": skipped (kernel needs lonely and non-lonely variables)" << std::endl;
            n_skipped++;
            if (results_out.is_open())
                results_out << q_idx << ";-" << std::endl;
            if (times_out.is_open())
                times_out << q_idx << ";-" << std::endl;
            continue;
        }
        if (meta.att_nl.size() > KERNEL_MAX_NONLONELY_ATTS) {
            std::cout << "Query " << q_idx << ": skipped (" << meta.att_nl.size() << " non-lonely variables, "
                       << "the engine supports at most " << KERNEL_MAX_NONLONELY_ATTS << ")" << std::endl;
            n_skipped++;
            if (results_out.is_open())
                results_out << q_idx << ";+" << std::endl;
            if (times_out.is_open())
                times_out << q_idx << ";+" << std::endl;
            continue;
        }

        uint64_t result = 0;
        std::vector<QdagT*> all_qdags;
        start_timeout_watchdog(cfg.time_out);
        auto t0 = high_resolution_clock::now();
        SeQuadTOut* final_qt = strategy_kernel<QdagT, SeQuadT, SeQuadTOut>(
            meta, all_qdags, result, cfg.upper_bound, cfg.parallel_level, cfg.grid_side, cfg.parallel_phases);
        if (cfg.mxm)
            result = gdj_examples::mxm_projected_count(query, final_qt);
        auto t1 = high_resolution_clock::now();
        stop_timeout_watchdog();
        double ms = duration<double, std::milli>(t1 - t0).count();
        for (auto *q : all_qdags)
            delete q;
        delete final_qt;

        std::cout << "Query " << q_idx << ": kernel=" << result << " (" << std::fixed << std::setprecision(3) << ms << "ms)" << std::endl;

        times.push_back(ms);
        if (results_out.is_open())
            results_out << q_idx << ";" << result << std::endl;
        if (times_out.is_open())
            times_out << q_idx << ";" << ms << std::endl;
    }

    double total = 0;
    for (double x : times)
        total += x;
    double avg = times.empty() ? 0.0 : total / times.size();

    std::cout << "-----------------------------------------------------" << std::endl;
    std::cout << "Total queries: " << all_queries.size() << " | skipped: " << n_skipped << std::endl;
    std::cout << std::left << std::setw(10) << "kernel"
               << " | total " << std::fixed << std::setprecision(3) << total << "ms"
               << " | mean "   << std::fixed << std::setprecision(3) << avg << "ms"
               << " | median " << std::fixed << std::setprecision(3) << median(times) << "ms" << std::endl;

    std::cout << "RESULT;" << cfg.variant << ";" << all_queries.size() << ";" << n_skipped << ";"
               << total << ";" << avg << ";" << median(times) << std::endl;

    return 0;
}

// Carga el dataset una vez y corre todas las pasadas (nivel x repeticion).
template<class RankBackendIn, class RankBackendOut>
static int run_variant(const Config &cfg, const std::vector<query_tp> &all_queries) {
    using SeQuadT = se_quadtree<RankBackendIn>;

    resource_manager<SeQuadT> res_mgr;
    res_mgr.load_directory(cfg.dir_dataset);
    std::cout << "[" << cfg.variant << " -> " << cfg.variant_out << "] "; res_mgr.print_summary();

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

#define QDAGS_VARIANT_LIST(X) \
    X("bv64",  rank_bv_64_p) \
    X("v5",    rank_vfamily_p<sdsl::rank_support_v5<>>) \
    X("v",     rank_vfamily_p<sdsl::rank_support_v<>>) \
    X("sd",    rank_sd_p) \
    X("rrr",   rank_rrr_p) \
    X("rrr15", rank_rrr15_p)

template<class RankBackendIn>
static int dispatch_out(const Config &cfg, const std::vector<query_tp> &all_queries) {
#define QDAGS_TRY_OUT(name, backend) \
    if (cfg.variant_out == name) return run_variant<RankBackendIn, backend>(cfg, all_queries);
    QDAGS_VARIANT_LIST(QDAGS_TRY_OUT)
#undef QDAGS_TRY_OUT
    std::cerr << "Error: --variant_out must be bv64|v5|v|sd|rrr|rrr15 (got: " << cfg.variant_out << ")" << std::endl;
    return 1;
}

int main(int argc, char **argv) {
    const int mandatory_args = 4;
    if (argc < mandatory_args + 1) {
        std::cerr << "Usage: " << argv[0]
                   << " <dir_dataset> <queries_file> <upper_bound> <parallel_level[,level...]>"
                      " [--grid_side <val>] [--time_out <val>]"
                      " [--parallel_phases first_only|last_only|all]"
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
        else if (arg == "--parallel_phases" && i + 1 < argc) {
            std::string v = argv[++i];
            cfg.parallel_phases = (v == "first_only") ? ParallelPhases::FirstOnly
                                 : (v == "last_only")  ? ParallelPhases::LastOnly
                                 : ParallelPhases::AllPhases;
        } else if (arg == "--variant" && i + 1 < argc)
            cfg.variant = argv[++i];
        else if (arg == "--variant_out" && i + 1 < argc)
            cfg.variant_out = argv[++i];
        else if (arg == "--results_file" && i + 1 < argc)
            cfg.results_file = argv[++i];
        else if (arg == "--times_file" && i + 1 < argc)
            cfg.times_file = argv[++i];
        else if (arg == "--reps" && i + 1 < argc)
            cfg.reps = (uint32_t)std::stoul(argv[++i]);
        else if (arg == "--mxm")
            cfg.mxm = true;
    }
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
    std::cout << "  Variant:         " << cfg.variant << " (out: " << cfg.variant_out << ")" << std::endl;
    std::cout << "  Queries File:    " << cfg.queries_file << std::endl;
    std::cout << "  Upper Bound:     " << cfg.upper_bound << std::endl;
    std::cout << "  Parallel Level:  " << gdj_examples::levels_to_string(cfg.levels) << std::endl;
    std::cout << "  Reps:            " << cfg.reps << std::endl;
    std::cout << "  Mxm:             " << (cfg.mxm ? "true (measures the projected matrix)" : "false") << std::endl;
    std::cout << "  Parallel Phases: " << parallel_phases_to_string(cfg.parallel_phases) << std::endl;
    std::cout << "  Grid Side:       " << cfg.grid_side << std::endl;
    std::cout << "  Time Out:        " << cfg.time_out << " seconds" << std::endl;
    std::cout << "  Wide Base:       " << gdj_examples::wide_base_to_string(cfg.wide) << std::endl;
    std::cout << "  Results File:    " << (cfg.results_file.empty() ? "(disabled)" : cfg.results_file) << std::endl;
    std::cout << "  Times File:      " << (cfg.times_file.empty() ? "(disabled)" : cfg.times_file) << std::endl;

    auto all_queries = load_all_queries(cfg.queries_file);

#define QDAGS_TRY_IN(name, backend) \
    if (cfg.variant == name) return dispatch_out<backend>(cfg, all_queries);
    QDAGS_VARIANT_LIST(QDAGS_TRY_IN)
#undef QDAGS_TRY_IN

    std::cerr << "Error: --variant must be bv64|v5|v|sd|rrr|rrr15 (got: " << cfg.variant << ")" << std::endl;
    return 1;
}
