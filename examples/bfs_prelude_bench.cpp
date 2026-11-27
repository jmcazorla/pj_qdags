#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <set>
#include <string>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <random>
#include <filesystem>
#include <algorithm>
#include <unistd.h>

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
#include "qdags/join/joins_bfs.hpp"
#include "qdags/query/triple_pattern.hpp"
#include "qdags/query/resource_manager.hpp"
#include "qdags/query/query_builder.hpp"
#include "qdags/query/query_loader.hpp"

// EXPERIMENTAL. Mide solo el prelude del join paralelo (enumeracion de las raices del nivel de corte, sin el AND
// por raiz) de dos formas:
//   dfs -- partialAND (joins.hpp), el prelude actual (un `new uint64_t[16]` por raiz)
//   bfs -- enumerate_roots_bfs (joins_bfs.hpp), por niveles y con las raices en un arreglo plano
// sobre las mismas queries, con los qdags armados como strategy_orig (un join sobre todos los patrones; solo
// queries con <= 6 atributos, el camino denso). En la primera repeticion verifica que ambas den las mismas
// raices y el mismo bv[0..nivel) (si no, MISMATCH y codigo 2). Las repeticiones alternan quien va primero.
//
// Uso:
//   bfs_prelude_bench <dir_dataset> <queries_file> <levels_csv> [--variant bv64|v5|v|sd|rrr|rrr15]
//        [--reps N] [--max_roots N] [--grid_side N]
//   bfs_prelude_bench --synthetic L,side,edges[,skew] <levels_csv> [--reps N] [--max_roots N]
//   ... --full [--time_out S]  (ignora <levels_csv>, pasar 0): compara el join completo secuencial en BFS
//        (bfs_join_bv, hasta las hojas) con el AND secuencial desde la raiz y verifica bv identico en todos los
//        niveles; --time_out S (default 60) corta el AND pesado. Salida: una linea FULL por query y FULL_SUMMARY.
//
// --max_roots N (default 20000000): un nivel con mas raices/nodos se omite para esa query (mismo tope en ambos).
// Salida: una linea PRELUDE por (query, nivel) y PRELUDE_SUMMARY por nivel con totales y la mediana de dfs/bfs.

using namespace gdj;
using namespace std::chrono;
namespace fs = std::filesystem;

static std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> out; std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else
            cur += c;
    }
    out.push_back(cur);
    return out;
}

static double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return n == 0 ? 0.0 : (n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0);
}

struct Config {
    std::string dir_dataset, queries_file, variant = "bv64", synthetic;
    std::vector<uint64_t> levels;
    uint64_t grid_side = 52000000, max_roots = 20000000, time_out = 60;
    int reps = 3;
    bool full = false;   // --full: join COMPLETO secuencial BFS vs AND secuencial (en vez del prelude)
    bool grid_side_set = false;
};

// ----- dataset sintetico -----
using Edge = std::pair<uint64_t, uint64_t>;
static std::vector<Edge> gen_edges(uint64_t side, uint64_t n, std::mt19937 &rng, double skew) {
    std::uniform_int_distribution<uint64_t> d(0, side - 1);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    auto c = [&]() -> uint64_t {
        if (skew <= 0)
            return d(rng);
        uint64_t v = (uint64_t)(side * std::pow(u(rng), skew));
        return v >= side ? side - 1 : v;
    };
    std::set<Edge> p;
    uint64_t guard = 0;
    while (p.size() < n && guard++ < n * 50) {
        uint64_t a = c(), b = c();
        p.insert({a, b});
    }
    return std::vector<Edge>(p.begin(), p.end());
}

static void write_relation(const fs::path &dir, uint64_t pid, uint64_t side, const std::vector<Edge> &e) {
    std::vector<std::vector<idx_type>> ev;
    for (auto &x : e)
        ev.push_back({(idx_type)x.first, (idx_type)x.second});
    se_quadtree_p so(ev, side, 2, 2);
    { std::ofstream o(dir / std::to_string(pid), std::ios::binary); so.serialize(o); }
    std::set<uint64_t> sv, ov;
    for (auto &x : e) {
        sv.insert(x.first);
        ov.insert(x.second);
    }
    std::vector<std::vector<idx_type>> a, b;
    for (auto v : sv)
        a.push_back({(idx_type)v});
    for (auto v : ov)
        b.push_back({(idx_type)v});
    se_quadtree_p s(a, side, 2, 1);
    { std::ofstream o(dir / (std::to_string(pid) + "s"), std::ios::binary); s.serialize(o); }
    se_quadtree_p oo(b, side, 2, 1);
    { std::ofstream o(dir / (std::to_string(pid) + "o"), std::ios::binary); oo.serialize(o); }
}

struct LevelAcc {
    uint64_t n_queries = 0, n_skipped = 0, n_mismatch = 0;
    double dfs_total_ms = 0, bfs_total_ms = 0;
    uint64_t roots_total = 0;
    std::vector<double> ratios;   // dfs/bfs por query
};

template<class RankBackend>
static int run_variant(const Config &cfg, const std::vector<query_tp> &queries, const std::string &dataset_dir) {
    using SeQuadT = se_quadtree<RankBackend>;
    using QdagT = qdag<RankBackend>;
    resource_manager<SeQuadT> rm;
    rm.load_directory(dataset_dir);
    rm.print_summary();

    std::vector<LevelAcc> acc(cfg.levels.size());
    int rc = 0;

    for (size_t qi = 0; qi < queries.size(); ++qi) {
        query_relations<SeQuadT> rel = build_qdags_from_query<SeQuadT>(queries[qi], rm);
        if (rel.quadtrees.empty()) {
            std::cout << "Query " << qi << ": skipped (no usable patterns)" << std::endl;
            continue;
        }
        if (rel.att_ext.size() > 6 || rel.quadtrees.size() > 16) {
            std::cout << "Query " << qi << ": skipped (" << rel.att_ext.size() << " attributes / " << rel.quadtrees.size()
                      << " patterns: outside the dense path <=6)" << std::endl;
            continue;
        }

        std::vector<QdagT *> Q;
        for (size_t i = 0; i < rel.quadtrees.size(); ++i) {
            att_set base = rel.att_list[i];
            Q.push_back(new QdagT(rel.quadtrees[i], base, cfg.grid_side, rel.att_ext));
        }
        const size_t nQ = Q.size();
        const size_t height = Q[0]->getHeight();
        const uint64_t nAtt = rel.att_ext.size();

        for (size_t li = 0; li < cfg.levels.size(); ++li) {
            const uint16_t cut = (uint16_t)cfg.levels[li];
            LevelAcc &A = acc[li];
            if (cut >= height) {
                ++A.n_skipped;
                continue;
            }

            std::vector<double> t_dfs, t_bfs;
            uint64_t roots_n = 0, bfs_bytes = 0, peak_frontier = 0;
            bool skipped = false, mismatch = false;

            for (int r = 0; r < cfg.reps && !skipped; ++r) {
                auto run_dfs = [&](bool keep, std::vector<uint64_t *> &troots, std::vector<uint64_t> *bv) {
                    uint64_t last_posH[cut];
                    for (uint16_t i = 0; i < cut; ++i)
                        last_posH[i] = 0;
                    uint64_t Q_roots[16] = {0};
                    bool aborted = false;
                    auto t0 = steady_clock::now();
                    partialAND<QdagT>(cut, Q, Q_roots, nQ, 0, cut, bv, last_posH, nAtt, troots, cfg.max_roots, &aborted);
                    double ms = duration<double, std::milli>(steady_clock::now() - t0).count();
                    (void)keep;
                    return aborted ? -1.0 : ms;
                };
                auto run_bfs = [&](bfs_roots &roots, std::vector<uint64_t> *bv, bfs_stats &st) {
                    auto t0 = steady_clock::now();
                    bool ok = enumerate_roots_bfs<QdagT>(cut, Q, bv, roots, cfg.max_roots, &st);
                    double ms = duration<double, std::milli>(steady_clock::now() - t0).count();
                    return ok ? ms : -1.0;
                };

                std::vector<uint64_t> bv_d[height], bv_b[height];
                std::vector<uint64_t *> troots;
                bfs_roots roots;
                bfs_stats st;
                double md = 0, mb = 0;
                if ((r + qi) % 2 == 0) {
                    md = run_dfs(true, troots, bv_d);
                    mb = run_bfs(roots, bv_b, st);
                }
                else                   {
                    mb = run_bfs(roots, bv_b, st);
                    md = run_dfs(true, troots, bv_d);
                }

                if (md < 0 || mb < 0) {
                    // Tope propio de cada metodo (DFS: raices; BFS: nodos por nivel); si alguno lo supera se omite.
                    skipped = true;
                } else {
                    if (r == 0) {
                        bool same = roots.count == troots.size();
                        for (size_t i = 0; same && i < troots.size(); ++i)
                            for (size_t q = 0; q < nQ; ++q)
                                if (roots.at(i)[q] != troots[i][q]) {
                                    same = false;
                                    break;
                                }
                        for (uint16_t l = 0; same && l < cut; ++l)
                            if (bv_d[l] != bv_b[l])
                                same = false;
                        if (!same) {
                            mismatch = true;
                        }
                        roots_n = troots.size();
                        bfs_bytes = st.peak_bytes;
                        peak_frontier = st.peak_frontier;
                    }
                    t_dfs.push_back(md); t_bfs.push_back(mb);
                }
                for (auto *p : troots)
                    delete[] p;
            }

            if (skipped) {
                ++A.n_skipped;
                std::cout << "PRELUDE;" << qi << ";" << cut << ";SKIP(max_roots)" << std::endl;
                continue;
            }
            if (mismatch) {
                ++A.n_mismatch;
                rc = 2;
                std::cout << "MISMATCH;" << qi << ";" << cut << ";bfs != dfs" << std::endl;
            }

            const double d = median(t_dfs), b = median(t_bfs);
            // DFS: new uint64_t[16] (128 B + ~16 de cabecera de malloc) + puntero en el vector.
            const uint64_t dfs_bytes = roots_n * (16 * 8 + 16 + 8);
            ++A.n_queries; A.dfs_total_ms += d; A.bfs_total_ms += b; A.roots_total += roots_n;
            if (b > 0)
                A.ratios.push_back(d / b);
            std::cout << "PRELUDE;" << qi << ";" << cut << ";roots=" << roots_n << ";peak_frontier=" << peak_frontier
                      << ";dfs_ms=" << std::fixed << std::setprecision(3) << d << ";bfs_ms=" << b
                      << ";dfs_over_bfs=" << std::setprecision(2) << (b > 0 ? d / b : 0.0)
                      << ";dfs_ns_per_root=" << std::setprecision(0) << (roots_n ? d * 1e6 / roots_n : 0.0)
                      << ";bfs_ns_per_root=" << (roots_n ? b * 1e6 / roots_n : 0.0)
                      << ";dfs_bytes_est=" << dfs_bytes << ";bfs_peak_bytes=" << bfs_bytes << std::endl;
        }
        for (auto *q : Q)
            delete q;
    }

    std::cout << "\n";
    for (size_t li = 0; li < cfg.levels.size(); ++li) {
        const LevelAcc &A = acc[li];
        std::cout << "PRELUDE_SUMMARY;level=" << cfg.levels[li] << ";queries=" << A.n_queries << ";skipped=" << A.n_skipped
                  << ";roots_total=" << A.roots_total << ";dfs_total_ms=" << std::fixed << std::setprecision(2) << A.dfs_total_ms
                  << ";bfs_total_ms=" << A.bfs_total_ms
                  << ";total_dfs_over_bfs=" << (A.bfs_total_ms > 0 ? A.dfs_total_ms / A.bfs_total_ms : 0.0)
                  << ";median_dfs_over_bfs=" << median(A.ratios)
                  << ";mismatches=" << A.n_mismatch << std::endl;
    }
    return rc;
}

// --full: join completo BFS (secuencial) vs AND secuencial, mismas queries.
template<class RankBackend>
static int run_full(const Config &cfg, const std::vector<query_tp> &queries, const std::string &dataset_dir) {
    using SeQuadT = se_quadtree<RankBackend>;
    using QdagT = qdag<RankBackend>;
    resource_manager<SeQuadT> rm;
    rm.load_directory(dataset_dir);
    rm.print_summary();

    int rc = 0;
    double dfs_sum = 0, bfs_sum = 0;
    uint64_t n_ok = 0, n_to = 0, n_skip = 0, n_mismatch = 0, max_bytes = 0, max_frontier = 0;
    std::vector<double> ratios;

    for (size_t qi = 0; qi < queries.size(); ++qi) {
        query_relations<SeQuadT> rel = build_qdags_from_query<SeQuadT>(queries[qi], rm);
        if (rel.quadtrees.empty() || rel.att_ext.size() > 6 || rel.quadtrees.size() > 16) {
            std::cout << "Query " << qi << ": skipped (no patterns or outside the dense path <=6 attributes)" << std::endl;
            continue;
        }
        std::vector<QdagT *> Q;
        for (size_t i = 0; i < rel.quadtrees.size(); ++i) {
            att_set base = rel.att_list[i];
            Q.push_back(new QdagT(rel.quadtrees[i], base, cfg.grid_side, rel.att_ext));
        }
        const size_t height = Q[0]->getHeight();
        const uint64_t nAtt = rel.att_ext.size();

        std::vector<double> t_dfs, t_bfs;
        bool timed_out = false, skipped = false, mismatch = false;
        uint64_t n_res = 0, bytes = 0, frontier = 0;

        for (int r = 0; r < cfg.reps && !timed_out && !skipped; ++r) {
            std::vector<uint64_t> bv_d[height], bv_b[height];
            auto run_dfs = [&]() {
                uint64_t last_pos[height];
                for (size_t i = 0; i < height; ++i)
                    last_pos[i] = 0;
                uint64_t Q_roots[16] = {0};
                if (cfg.time_out)
                    start_timeout_watchdog(cfg.time_out);
                auto t0 = steady_clock::now();
                AND<QdagT>(Q, Q_roots, Q.size(), 0, (uint16_t)(height - 1), bv_d, last_pos, nAtt, false, 0);
                double ms = duration<double, std::milli>(steady_clock::now() - t0).count();
                bool to = timeout_triggered.load();
                if (cfg.time_out)
                    stop_timeout_watchdog();
                return to ? -1.0 : ms;
            };
            bfs_stats st;
            auto run_bfs = [&]() {
                auto t0 = steady_clock::now();
                bool ok = bfs_join_bv<QdagT>(Q, bv_b, cfg.max_roots, &st);
                double ms = duration<double, std::milli>(steady_clock::now() - t0).count();
                return ok ? ms : -1.0;
            };
            double md, mb;
            if ((r + qi) % 2 == 0) {
                md = run_dfs();
                mb = run_bfs();
            }
            else                   {
                mb = run_bfs();
                md = run_dfs();
            }

            if (md < 0) {
                timed_out = true;
                break;
            }
            if (mb < 0) {
                skipped = true;
                break;
            }
            if (r == 0) {
                for (size_t l = 0; l < height; ++l)
                    if (bv_d[l] != bv_b[l])
                        mismatch = true;
                n_res = bv_d[height - 1].size(); bytes = st.peak_bytes; frontier = st.peak_frontier;
            }
            t_dfs.push_back(md); t_bfs.push_back(mb);
        }
        for (auto *q : Q)
            delete q;

        if (timed_out) {
            ++n_to;
            std::cout << "FULL;" << qi << ";TIMEOUT(dfs>" << cfg.time_out << "s)" << std::endl;
            continue;
        }
        if (skipped)   {
            ++n_skip;
            std::cout << "FULL;" << qi << ";SKIP(max_roots=" << cfg.max_roots << ")" << std::endl;
            continue;
        }
        if (mismatch)  {
            ++n_mismatch;
            rc = 2;
            std::cout << "MISMATCH;" << qi << ";bfs != dfs" << std::endl;
        }

        const double d = median(t_dfs), b = median(t_bfs);
        ++n_ok; dfs_sum += d; bfs_sum += b; if (b > 0) ratios.push_back(d / b);
        max_bytes = std::max(max_bytes, bytes); max_frontier = std::max(max_frontier, frontier);
        std::cout << "FULL;" << qi << ";n=" << n_res << ";peak_frontier=" << frontier << ";dfs_ms=" << std::fixed << std::setprecision(3) << d
                  << ";bfs_ms=" << b << ";dfs_over_bfs=" << std::setprecision(2) << (b > 0 ? d / b : 0.0)
                  << ";bfs_peak_bytes=" << bytes << std::endl;
    }
    std::cout << "\nFULL_SUMMARY;queries=" << n_ok << ";timeouts_dfs=" << n_to << ";skipped_max_roots=" << n_skip
              << ";dfs_total_ms=" << std::fixed << std::setprecision(1) << dfs_sum << ";bfs_total_ms=" << bfs_sum
              << ";total_dfs_over_bfs=" << std::setprecision(2) << (bfs_sum > 0 ? dfs_sum / bfs_sum : 0.0)
              << ";median_dfs_over_bfs=" << median(ratios) << ";max_peak_frontier=" << max_frontier
              << ";max_bfs_peak_bytes=" << max_bytes << ";mismatches=" << n_mismatch << std::endl;
    return rc;
}

static void usage(const char *prog) {
    std::cerr << "Usage: " << prog << " <dir_dataset> <queries_file> <levels_csv> [--variant bv64|v5|v|sd|rrr|rrr15]\n"
              << "          [--reps N] [--max_roots N] [--grid_side N]\n"
              << "     " << prog << " --synthetic L,side,edges[,skew] <levels_csv> [--reps N] [--max_roots N]\n";
}

int main(int argc, char **argv) {
    Config cfg;
    bool synthetic = argc > 1 && std::string(argv[1]) == "--synthetic";
    int opt_start;
    if (argc < (synthetic ? 4 : 4)) {
        usage(argv[0]);
        return 1;
    }
    if (synthetic) {
        cfg.synthetic = argv[2];
        for (auto &s : split(argv[3], ','))
            cfg.levels.push_back(std::stoull(s));
        opt_start = 4;
    } else {
        if (argc < 4) {
            usage(argv[0]);
            return 1;
        }
        cfg.dir_dataset = argv[1]; cfg.queries_file = argv[2];
        for (auto &s : split(argv[3], ','))
            cfg.levels.push_back(std::stoull(s));
        opt_start = 4;
    }
    for (int i = opt_start; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--reps" && i + 1 < argc)
            cfg.reps = std::max(1, std::stoi(argv[++i]));
        else if (a == "--max_roots" && i + 1 < argc)
            cfg.max_roots = std::stoull(argv[++i]);
        else if (a == "--grid_side" && i + 1 < argc) {
            cfg.grid_side = std::stoull(argv[++i]);
            cfg.grid_side_set = true;
        }
        else if (a == "--variant" && i + 1 < argc)
            cfg.variant = argv[++i];
        else if (a == "--full")
            cfg.full = true;
        else if (a == "--time_out" && i + 1 < argc)
            cfg.time_out = std::stoull(argv[++i]);
        else {
            std::cerr << "Error: unknown option or missing value: " << a << std::endl;
            usage(argv[0]);
            return 1;
        }
    }

    std::cout << "Configuration:\n  Dataset:   " << (synthetic ? "synthetic " + cfg.synthetic : cfg.dir_dataset)
              << "\n  Variant:   " << cfg.variant << "\n  Levels:    ";
    for (size_t i = 0; i < cfg.levels.size(); ++i)
        std::cout << (i ? "," : "") << cfg.levels[i];
    std::cout << "\n  Reps:      " << cfg.reps << "\n  max_roots: " << cfg.max_roots << std::endl;

    std::vector<query_tp> queries;
    std::string dataset_dir = cfg.dir_dataset;
    fs::path tmp_dir;
    if (synthetic) {
        if (cfg.variant != "bv64") {
            std::cerr << "Error: --synthetic only supports --variant bv64" << std::endl;
            return 1;
        }
        auto p = split(cfg.synthetic, ',');
        if (p.size() < 3) {
            usage(argv[0]);
            return 1;
        }
        uint64_t L = std::stoull(p[0]), side = std::stoull(p[1]), ne = std::stoull(p[2]);
        double skew = p.size() > 3 ? std::stod(p[3]) : 0.0;
        if (L > 6) {
            std::cerr << "Error: --synthetic needs L <= 6 (dense path)" << std::endl;
            return 1;
        }
        if (!cfg.grid_side_set)
            cfg.grid_side = side;
        tmp_dir = fs::temp_directory_path() / ("bfs_prelude_bench_" + std::to_string(::getpid()));
        fs::create_directories(tmp_dir);
        std::mt19937 rng(777);
        for (uint64_t i = 0; i < L; ++i)
            write_relation(tmp_dir, 100 + i, side, gen_edges(side, ne, rng, skew));
        query_tp q;
        for (uint64_t i = 0; i < L; ++i) {
            triple_pattern tp;
            tp.var_s((uint16_t)i);
            tp.const_p(100 + i);
            tp.var_o((uint16_t)((i + 1) % L));
            q.push_back(tp);
        }
        queries.push_back(q);
        dataset_dir = tmp_dir.string();
    } else {
        queries = load_all_queries(cfg.queries_file);
    }

    int rc = 1;
#define RUN(fn) \
    if (cfg.variant == "bv64")       rc = fn<rank_bv_64_p>(cfg, queries, dataset_dir); \
    else if (cfg.variant == "v5")    rc = fn<rank_vfamily_p<sdsl::rank_support_v5<>>>(cfg, queries, dataset_dir); \
    else if (cfg.variant == "v")     rc = fn<rank_vfamily_p<sdsl::rank_support_v<>>>(cfg, queries, dataset_dir); \
    else if (cfg.variant == "sd")    rc = fn<rank_sd_p>(cfg, queries, dataset_dir); \
    else if (cfg.variant == "rrr")   rc = fn<rank_rrr_p>(cfg, queries, dataset_dir); \
    else if (cfg.variant == "rrr15") rc = fn<rank_rrr15_p>(cfg, queries, dataset_dir); \
    else std::cerr << "Error: --variant must be bv64|v5|v|sd|rrr|rrr15 (got: " << cfg.variant << ")" << std::endl;
    if (cfg.full) {
        RUN(run_full)
    } else {
        RUN(run_variant)
    }
#undef RUN


    if (!tmp_dir.empty())
        fs::remove_all(tmp_dir);
    return rc;
}
