#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <filesystem>
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
#include "qdags/query/topology_plan_selection.hpp"
#include "levels_util.hpp"
#include "wide_base_args.hpp"
#include "query_runners.hpp"

// Plan + fallback: cada query se ejecuta con el plan de su topologia si el catalogo tiene uno y su builder la
// acepta; las demas (topologia desconocida o fuera de las 17 definidas, grupo que no la cubre, o que no calza
// con el plan) se ejecutan con todas las estrategias de --fallback, cada una con su cronometro y timeout. Asi se
// comparan estrategias sobre las queries sin plan sin repetir las que si lo tienen. Mismo manejo de planes que
// run_plan.cpp; las estrategias del fallback vienen de query_runners.hpp.
//
// El plan se resuelve de 2 formas excluyentes, como en run_plan:
//   --plan_file <path> --plan_id "<key>"   un plan fijo, solo para las topologias que sirve (--plan_topologies)
//   --plan_group normal|best|wk_k|leapfrog cada query busca el suyo en el catalogo de la libreria
//
// --plan_topologies P2,T2,...: con plan fijo, topologias a las que se aplica; por defecto las que el catalogo
// asocia al mismo archivo (plansP2T2Ti2_f.txt -> P2,T2,Ti2), y si no esta en el catalogo hay que pasarlas. Hace
// falta porque el builder no siempre rechaza una query de otra topologia: un plan de P2 sobre un triangulo da un
// resultado equivocado sin avisar (run_plan asume la topologia del plan). Las demas van al fallback.
//
// Queries con S u O constante: van siempre al fallback (engine = fallback, no hay plan para ellas), con las
// tablas de pinned_tables (qdag_pin). WeakKernel deja "-" si alguna variable solo aparece en patrones con
// constante; un patron con S y O constantes no se soporta ("-" en todas). Las de solo variables no cambian.
//
// --mode: variantes a correr, separadas por comas: generic, adaptive, resumable (both = generic,adaptive; all =
// las tres). Cada estrategia corre las que tiene: plan y wk las tres; leapfrog generic y adaptive; one_shot,
// kernel y orig solo generic (el prejoin de One-Shot es exacto). Si ninguna pedida existe para una estrategia,
// no se ejecuta (aviso al inicio).
//
// --fallback one_shot,wk,leapfrog,kernel,orig: estrategias en el orden en que se reportan ("nl" es alias de
// one_shot, pero las columnas salen como one_shot_*). Si una no aplica a la query (one_shot/kernel sin variables
// non-lonely, kernel sin lonely, etc.) deja "-"; "+" si la query supera el limite del motor (kernel: mas de 9
// variables non-lonely).
//
// Salida (--results_file / --times_file): una fila por query, columnas en el orden en que se pasan (las que no
// aplican quedan vacias):
//   query_id;topology;engine;plan_<modo>...;<f>_<modo>...
// con <modo> = generic | adaptive | resumable (solo los que se corren) y engine = plan | fallback.
//
// Con upper_bound=0 y coef_bound=0 todas las estrategias de una query deben dar el mismo conteo; si no, se
// imprime MISMATCH y se cuenta en n_mismatch (sin las corridas con timeout ni las adaptive/resumable no
// exhaustivas). Con cota no se comprueba.
//
// Linea machine-readable, siempre con todos los campos (0 si no aplica):
//   RESULT;<variant>;n_queries;n_plan;n_fallback;n_mismatch;
//          plan: g_total;g_mean;g_median;a_total;a_mean;a_median;r_total;r_mean;r_median;
//          y por cada estrategia de --fallback, en orden:
//          n_skipped;g_total;g_mean;g_median;a_total;a_mean;a_median;r_total;r_mean;r_median
//          (g = generic, a = adaptive, r = resumable)
//
// Varios niveles/repeticiones (--reps, {L}, {R}, "LEVEL;..."): ver levels_util.hpp.
//
// Uso:
//   run_plan_hybrid <dir_dataset> <queries_file> <upper_bound> <parallel_level[,nivel...]>
//            (--plan_file <path> --plan_id "<key>" | --plan_group normal|best|wk_k|leapfrog)
//            --fallback one_shot|wk|leapfrog|kernel|orig[,...]
//            [--plan_topologies <topology>[,...]]
//            [--grid_side <val>] [--time_out <val>]
//            [--init_coef <val>] [--growth <val>] [--growth_mode fixed|proportional] [--max_retries <val>] [--coef_bound <val>]
//            [--mxm] [--k <val>] [--lf_order proj_size|degree] [--lf_depth full|non_lonely]
//            [--lf_max_atts <val>] [--parallel_phases first_only|last_only|all]
//            [--mode generic|adaptive|resumable[,...]|both|all] [--variant bv64|v5|v|sd|rrr|rrr15]
//            [--reps <n>] [--results_file <path>] [--times_file <path>]
using namespace gdj;
using namespace gdj_examples;
using namespace std::chrono;

static double median(std::vector<double> v) {
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return (n % 2 == 1) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

static double sum(const std::vector<double> &v) { double s = 0; for (double x : v) s += x; return s; }
static double mean(const std::vector<double> &v) { return v.empty() ? 0.0 : sum(v) / v.size(); }

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
    std::string dir_dataset, queries_file, plan_file, plan_id, plan_group, variant = "bv64", mode = "both", results_file, times_file;
    std::vector<uint64_t> levels{0};
    uint32_t reps = 1;
    std::vector<Strategy> fallbacks;
    std::set<QueryTopology> plan_topologies;   // solo con plan fijo
    RunModes modes;                            // de --mode
    RunnerOptions opt;
};

constexpr int N_MODES = 3;
static const char *MODE_NAMES[N_MODES] = {"generic", "adaptive", "resumable"};

static bool mode_on(const RunModes &m, int k) { return k == 0 ? m.generic : k == 1 ? m.adaptive : m.resumable; }
static const ModeOutcome &mode_of(const QueryOutcome &o, int k) { return k == 0 ? o.generic : k == 1 ? o.adaptive : o.resumable; }

// Una estrategia del fallback con los modos de --mode que de verdad tiene.
struct FallbackSlot {
    Strategy strategy;
    RunModes modes;
    bool active() const { return modes.any(); }
};

// Estadisticas de un grupo (el plan o una estrategia del fallback): tiempos por modo.
struct GroupStats {
    std::vector<double> times[N_MODES];
    uint64_t n_skipped = 0;
};

static std::string num(double x) { std::ostringstream o; o << x; return o.str(); }

template<class RankBackend>
static int run_pass(const Config &cfg, resource_manager<se_quadtree<RankBackend>> &res_mgr, const std::vector<query_tp> &all_queries, const execution_plan *fixed_plan) {
    const RunModes &plan_modes = cfg.modes;

    std::vector<FallbackSlot> slots;
    for (Strategy s : cfg.fallbacks)
        slots.push_back(FallbackSlot{s, strategy_modes(s, cfg.modes)});

    // Cabecera: query_id;topology;engine;plan_*;<f>_*...
    std::vector<std::string> columns;
    for (int k = 0; k < N_MODES; ++k)
        if (mode_on(plan_modes, k))
            columns.push_back(std::string("plan_") + MODE_NAMES[k]);
    for (auto &sl : slots)
        for (int k = 0; k < N_MODES; ++k)
            if (mode_on(sl.modes, k))
                columns.push_back(std::string(strategy_name(sl.strategy)) + "_" + MODE_NAMES[k]);
    auto header = [&]() {
        std::string h = "query_id;topology;engine";
        for (auto &c : columns)
            h += ";" + c;
        return h;
    };

    std::ofstream results_out, times_out;
    if (!cfg.results_file.empty()) {
        results_out.open(cfg.results_file);
        results_out << header() << "\n";
    }
    if (!cfg.times_file.empty())   {
        times_out.open(cfg.times_file);
        times_out << header() << "\n";
    }

    plan_catalog_cache cache;
    PlanGroup group = parse_plan_group(cfg.plan_group);
    auto resolve_plan = [&](QueryTopology topo) -> const execution_plan* {
        if (fixed_plan)
            return cfg.plan_topologies.count(topo) ? fixed_plan : nullptr;
        return cache.get(topo, group);
    };

    GroupStats plan_stats;
    std::vector<GroupStats> fb_stats(slots.size());
    uint64_t n_plan = 0, n_fallback = 0, n_mismatch = 0;
    const bool check_counts = cfg.opt.upper_bound == 0 && cfg.opt.coef_bound == 0;

    for (size_t q_idx = 0; q_idx < all_queries.size(); ++q_idx) {
        const auto &query = all_queries[q_idx];
        QueryTopology topo = classify_query_topology(query);
        const execution_plan *plan = resolve_plan(topo);

        // Una celda por columna, en el mismo orden que la cabecera.
        std::vector<std::string> res_cells(columns.size()), time_cells(columns.size());
        // (etiqueta, resultado) de las corridas comparables de esta query.
        std::vector<std::pair<std::string, uint64_t>> comparable;
        auto describe = [&](const ModeOutcome &o, int k) {
            std::ostringstream s;
            s << MODE_NAMES[k] << "=" << o.result << " (" << std::fixed << std::setprecision(3) << o.ms << "ms";
            if (k != 0)
                s << ", attempts=" << o.attempts << ", final_coef=" << o.coef << ", " << (o.exhaustive ? "exhaustive" : "not-exhaustive");
            if (o.timed_out)
                s << ", TIMEOUT";
            s << ")";
            return s.str();
        };
        // Vuelca las variantes corridas de un grupo (plan o una estrategia) a las celdas y a la consola.
        auto emit = [&](const std::string &label, const RunModes &modes, const QueryOutcome &out, GroupStats &st, size_t &col) {
            bool first = true;
            for (int k = 0; k < N_MODES; ++k) {
                if (!mode_on(modes, k))
                    continue;
                const ModeOutcome &o = mode_of(out, k);
                res_cells[col] = std::to_string(o.result);
                time_cells[col++] = num(o.ms);
                st.times[k].push_back(o.ms);
                if (check_counts && o.ran && !o.timed_out && (k == 0 || o.exhaustive))
                    comparable.push_back({label + "_" + MODE_NAMES[k], o.result});
                std::cout << (first ? "" : " | ") << describe(o, k);
                first = false;
            }
            std::cout << std::endl;
        };

        // Con S u O constante no hay plan y las tablas salen de pinned_tables (qdag_pin); el resto, de plain_tables.
        const bool with_constants = has_constant_terms(query);

        const char *engine;
        if (!with_constants && plan && plan_applies<RankBackend>(topo, query, res_mgr, *plan)) {
            engine = "plan";
            n_plan++;
            QueryOutcome out = run_plan_query<RankBackend>(topo, query, res_mgr, *plan, cfg.opt, plan_modes);
            size_t col = 0;
            std::cout << "Query " << q_idx << " [" << topo << ", plan]: ";
            emit("plan", plan_modes, out, plan_stats, col);
        } else {
            engine = "fallback";
            n_fallback++;
            size_t col = 0;
            for (int k = 0; k < N_MODES; ++k)
                col += mode_on(plan_modes, k) ? 1 : 0;   // columnas del plan: vacias
            for (size_t i = 0; i < slots.size(); ++i) {
                const FallbackSlot &sl = slots[i];
                if (!sl.active())
                    continue;
                const std::string name = strategy_name(sl.strategy);
                QueryOutcome out = with_constants
                    ? run_fallback_query<RankBackend, pinned_tables<RankBackend>>(sl.strategy, query, res_mgr, cfg.opt, sl.modes)
                    : run_fallback_query<RankBackend>(sl.strategy, query, res_mgr, cfg.opt, sl.modes);
                std::cout << "Query " << q_idx << " [" << topo << (with_constants ? ", constants" : "") << ", fallback " << name << "]: ";
                if (out.status != RunStatus::Ok) {
                    const char *marker = out.status == RunStatus::TooWide ? "+" : "-";
                    fb_stats[i].n_skipped++;
                    for (int k = 0; k < N_MODES; ++k)
                        if (mode_on(sl.modes, k)) {
                            res_cells[col] = marker;
                            time_cells[col++] = marker;
                        }
                    std::cout << "skipped (" << (out.status == RunStatus::TooWide ? "exceeds engine limit" : "not applicable to this query") << ")" << std::endl;
                    continue;
                }
                emit(name, sl.modes, out, fb_stats[i], col);
            }
        }

        if (comparable.size() > 1) {
            bool same = true;
            for (auto &c : comparable)
                if (c.second != comparable.front().second)
                    same = false;
            if (!same) {
                n_mismatch++;
                std::cout << "Query " << q_idx << " [" << topo << "]: MISMATCH";
                for (auto &c : comparable)
                    std::cout << " " << c.first << "=" << c.second;
                std::cout << std::endl;
            }
        }

        auto row = [&](const std::vector<std::string> &cells) {
            std::string r = std::to_string(q_idx) + ";" + to_string(topo) + ";" + engine;
            for (auto &c : cells)
                r += ";" + c;
            return r;
        };
        if (results_out.is_open())
            results_out << row(res_cells) << std::endl;
        if (times_out.is_open())
            times_out << row(time_cells) << std::endl;
    }

    std::cout << "-----------------------------------------------------" << std::endl;
    std::cout << "Total queries: " << all_queries.size() << " | plan: " << n_plan << " | fallback: " << n_fallback
              << " | mismatch: " << n_mismatch << std::endl;
    auto print_group = [&](const std::string &label, const GroupStats &st, const RunModes &modes) {
        for (int k = 0; k < N_MODES; ++k) {
            if (!mode_on(modes, k))
                continue;
            std::cout << std::left << std::setw(20) << (label + " " + MODE_NAMES[k])
                      << " | total " << std::fixed << std::setprecision(3) << sum(st.times[k])
                      << "ms | mean " << mean(st.times[k]) << "ms | median " << median(st.times[k]) << "ms";
            if (st.n_skipped)
                std::cout << " | skipped " << st.n_skipped;
            std::cout << std::endl;
        }
    };
    print_group("plan", plan_stats, plan_modes);
    for (size_t i = 0; i < slots.size(); ++i)
        print_group(strategy_name(slots[i].strategy), fb_stats[i], slots[i].modes);

    auto group_fields = [&](const GroupStats &st) {
        for (int k = 0; k < N_MODES; ++k)
            std::cout << ";" << sum(st.times[k]) << ";" << mean(st.times[k]) << ";" << median(st.times[k]);
    };
    std::cout << "RESULT;" << cfg.variant << ";" << all_queries.size() << ";" << n_plan << ";" << n_fallback << ";" << n_mismatch;
    group_fields(plan_stats);
    for (size_t i = 0; i < slots.size(); ++i) {
        std::cout << ";" << fb_stats[i].n_skipped;
        group_fields(fb_stats[i]);
    }
    std::cout << std::endl;

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
            pass.opt.parallel_level = level;
            pass.results_file = expand_pass_path(cfg.results_file, level, rep);
            pass.times_file = expand_pass_path(cfg.times_file, level, rep);
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
                      " --fallback one_shot|wk|leapfrog|kernel|orig[,...]"
                      " [--plan_topologies <topology>[,...]]"
                      " [--grid_side <val>] [--time_out <val>]"
                      " [--init_coef <val>] [--growth <val>] [--growth_mode fixed|proportional] [--max_retries <val>] [--coef_bound <val>]"
                      " [--mxm] [--k <val>]"
                      " [--lf_order proj_size|degree] [--lf_depth full|non_lonely] [--lf_max_atts <val>]"
                      " [--parallel_phases first_only|last_only|all]"
                      " [--mode generic|adaptive|resumable[,...]|both|all]"
                      " [--variant bv64|v5|v|sd|rrr|rrr15]"
                      " [--reps <n>] [--results_file <path>] [--times_file <path>]" << gdj_examples::WIDE_BASE_USAGE << std::endl;
        return 1;
    }

    Config cfg;
    cfg.dir_dataset = argv[1];
    cfg.queries_file = argv[2];
    cfg.opt.upper_bound = std::stoull(argv[3]);
    try { cfg.levels = parse_levels(argv[4]); }
    catch (const std::exception &e) { std::cerr << "Error: invalid <parallel_level[,level...]> (" << e.what() << ")" << std::endl; return 1; }
    cfg.opt.parallel_level = cfg.levels.front();

    std::string fallback_arg, plan_topologies_arg, lf_order = "proj_size", lf_depth = "full";
    for (int i = mandatory_args + 1; i < argc; i++) {
        std::string arg = argv[i];
        if (int r = gdj_examples::parse_wide_base_arg(argc, argv, i, cfg.opt.wide)) {
            if (r < 0)
                return 1;
            continue;
        }
        if (arg == "--grid_side" && i + 1 < argc)
            cfg.opt.grid_side = std::stoull(argv[++i]);
        else if (arg == "--time_out" && i + 1 < argc)
            cfg.opt.time_out = std::stoull(argv[++i]);
        else if (arg == "--init_coef" && i + 1 < argc)
            cfg.opt.init_coef = std::stoull(argv[++i]);
        else if (arg == "--growth" && i + 1 < argc)
            cfg.opt.growth = (uint32_t)std::stoul(argv[++i]);
        else if (arg == "--growth_mode" && i + 1 < argc) {
            if (!parse_growth_mode(argv[++i], cfg.opt.growth_mode)) {
                std::cerr << "Error: --growth_mode must be fixed|proportional (got: " << argv[i] << ")" << std::endl;
                return 1;
            }
        }
        else if (arg == "--max_retries" && i + 1 < argc)
            cfg.opt.max_retries = (uint32_t)std::stoul(argv[++i]);
        else if (arg == "--coef_bound" && i + 1 < argc)
            cfg.opt.coef_bound = std::stoull(argv[++i]);
        else if (arg == "--mxm")
            cfg.opt.mxm = true;
        else if (arg == "--k" && i + 1 < argc)
            cfg.opt.k = (uint8_t)std::stoul(argv[++i]);
        else if (arg == "--lf_order" && i + 1 < argc)
            lf_order = argv[++i];
        else if (arg == "--lf_depth" && i + 1 < argc)
            lf_depth = argv[++i];
        else if (arg == "--lf_max_atts" && i + 1 < argc)
            cfg.opt.leapfrog.max_prejoin_atts = std::stoull(argv[++i]);
        else if (arg == "--plan_file" && i + 1 < argc)
            cfg.plan_file = argv[++i];
        else if (arg == "--plan_id" && i + 1 < argc)
            cfg.plan_id = argv[++i];
        else if (arg == "--plan_group" && i + 1 < argc)
            cfg.plan_group = argv[++i];
        else if (arg == "--fallback" && i + 1 < argc)
            fallback_arg = argv[++i];
        else if (arg == "--plan_topologies" && i + 1 < argc)
            plan_topologies_arg = argv[++i];
        else if (arg == "--parallel_phases" && i + 1 < argc) {
            std::string v = argv[++i];
            cfg.opt.parallel_phases = (v == "first_only") ? ParallelPhases::FirstOnly
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

    if (!parse_run_modes(cfg.mode, cfg.modes)) {
        std::cerr << "Error: --mode must be a comma-separated list of generic|adaptive|resumable, or both|all (got: " << cfg.mode << ")" << std::endl;
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
    cfg.opt.leapfrog.order = parse_leapfrog_order(lf_order);
    cfg.opt.leapfrog.depth = parse_leapfrog_depth(lf_depth);

    if (fallback_arg.empty()) {
        std::cerr << "Error: --fallback is required (comma-separated list of one_shot|wk|leapfrog|kernel|orig)" << std::endl;
        return 1;
    }
    {
        std::stringstream ss(fallback_arg);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            Strategy s;
            if (!parse_strategy(tok, s)) {
                std::cerr << "Error: --fallback must be a list of one_shot|wk|leapfrog|kernel|orig (got: '" << tok << "')" << std::endl;
                return 1;
            }
            if (std::find(cfg.fallbacks.begin(), cfg.fallbacks.end(), s) != cfg.fallbacks.end()) {
                std::cerr << "Error: --fallback repeats strategy '" << tok << "'" << std::endl;
                return 1;
            }
            cfg.fallbacks.push_back(s);
        }
        if (cfg.fallbacks.empty()) {
            std::cerr << "Error: --fallback is empty" << std::endl;
            return 1;
        }
    }
    for (Strategy s : cfg.fallbacks)
        if (!strategy_modes(s, cfg.modes).any())
            std::cerr << "Warning: " << strategy_name(s) << " has none of the variants in --mode " << run_modes_to_string(cfg.modes)
                      << ": not run" << std::endl;

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
    if (has_plan_group && !plan_topologies_arg.empty()) {
        std::cerr << "Error: --plan_topologies applies only with --plan_file/--plan_id" << std::endl;
        return 1;
    }
    if (has_plan_group && cfg.plan_group != "normal" && cfg.plan_group != "best"
        && cfg.plan_group != "wk_k" && cfg.plan_group != "leapfrog") {
        std::cerr << "Error: --plan_group must be normal|best|wk_k|leapfrog (got: " << cfg.plan_group << ")" << std::endl;
        return 1;
    }

    // Con --plan_group no hay plan fijo: cada query resuelve el suyo en run_pass() (plan_catalog_cache).
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

        if (!plan_topologies_arg.empty()) {
            std::stringstream ss(plan_topologies_arg);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                QueryTopology t = parse_query_topology(tok);
                if (std::string(to_string(t)) != tok || t == QueryTopology::SINGLE_PATTERN || t == QueryTopology::UNKNOWN_OR_ERROR) {
                    std::cerr << "Error: --plan_topologies: unknown topology or no plan ('" << tok << "')" << std::endl;
                    return 1;
                }
                cfg.plan_topologies.insert(t);
            }
        } else {
            // Por defecto, las topologias que el catalogo asocia a este mismo archivo.
            const std::string base = std::filesystem::path(cfg.plan_file).filename().string();
            for (auto const& [topo, file] : default_topology_plan_catalog().files)
                if (file == base)
                    cfg.plan_topologies.insert(topo);
        }
        if (cfg.plan_topologies.empty()) {
            std::cerr << "Error: " << cfg.plan_file << " is not in the library catalog: pass --plan_topologies "
                          "with the topologies the plan applies to." << std::endl;
            return 1;
        }
    }

    if (cfg.reps == 0) {
        std::cerr << "Error: --reps must be >= 1" << std::endl;
        return 1;
    }
    if (!path_distinguishes_passes(cfg.results_file, cfg.levels.size(), cfg.reps) ||
        !path_distinguishes_passes(cfg.times_file, cfg.levels.size(), cfg.reps)) {
        std::cerr << "Error: with several levels or repetitions, --results_file/--times_file must contain {L} and, with --reps > 1, {R}" << std::endl;
        return 1;
    }

    std::cout << "Configuration:" << std::endl;
    std::cout << "  Dir Dataset:     " << cfg.dir_dataset << std::endl;
    std::cout << "  Variant:         " << cfg.variant << std::endl;
    std::cout << "  Mode:            " << run_modes_to_string(cfg.modes) << std::endl;
    std::cout << "  Queries File:    " << cfg.queries_file << std::endl;
    if (fixed_plan) {
        std::cout << "  Plan File:       " << cfg.plan_file << std::endl;
        std::cout << "  Plan Id:         " << cfg.plan_id << " (" << fixed_plan->steps.size() << " steps)" << std::endl;
        std::cout << "  Plan Topologies: ";
        for (auto t : cfg.plan_topologies)
            std::cout << t << " ";
        std::cout << std::endl;
    } else {
        std::cout << "  Plan Group:      " << cfg.plan_group << " (library default catalog, "
                   << default_plans_dir().string() << ")" << std::endl;
    }
    std::cout << "  Fallback:        " << fallback_arg << std::endl;
    std::cout << "  Upper Bound:     " << cfg.opt.upper_bound << std::endl;
    std::cout << "  Parallel Level:  " << levels_to_string(cfg.levels) << std::endl;
    std::cout << "  Reps:            " << cfg.reps << std::endl;
    std::cout << "  Parallel Phases: " << parallel_phases_to_string(cfg.opt.parallel_phases) << std::endl;
    std::cout << "  Grid Side:       " << cfg.opt.grid_side << std::endl;
    std::cout << "  Time Out:        " << cfg.opt.time_out << " seconds" << std::endl;
    std::cout << "  Wide Base:       " << gdj_examples::wide_base_to_string(cfg.opt.wide) << std::endl;
    std::cout << "  Coef Bound (generic): " << cfg.opt.coef_bound << std::endl;
    std::cout << "  Init Coef (adaptive): " << cfg.opt.init_coef << std::endl;
    std::cout << "  Growth Factor:   " << cfg.opt.growth << std::endl;
    std::cout << "  Growth Mode:     " << to_string(cfg.opt.growth_mode) << std::endl;
    std::cout << "  Max Retries:     " << cfg.opt.max_retries << (cfg.opt.max_retries == 0 ? " (unlimited)" : "") << std::endl;
    std::cout << "  mxm:             " << (cfg.opt.mxm ? "true" : "false") << " (k=" << (int)cfg.opt.k << ")" << std::endl;
    std::cout << "  Leapfrog:        order=" << to_string(cfg.opt.leapfrog.order) << " depth=" << to_string(cfg.opt.leapfrog.depth)
               << " max_atts=" << cfg.opt.leapfrog.max_prejoin_atts << (cfg.opt.leapfrog.max_prejoin_atts == 0 ? " (auto)" : "") << std::endl;
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
