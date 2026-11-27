#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <charconv>
#include <dirent.h>

#include "qdags/index/commun.hpp"
#include "qdags/index/se_quadtree_bv64.hpp"
#include "qdags/index/se_quadtree_vfamily.hpp"
#include "qdags/index/se_quadtree_sd.hpp"
#include "qdags/index/se_quadtree_rrr.hpp"
#include "qdags/index/se_quadtree_rrr15.hpp"

// Construye un dataset de quadtrees (convencion de resource_manager::load_directory: "<pid>" = relacion SO,
// "<pid>s" = proyeccion S, "<pid>o" = proyeccion O) en uno o varios de los seis backends.
// Cada backend serializa en su propio formato: el dataset de --variant sd solo lo leen los ejecutables con
// --variant sd (o --variant_in sd), asi que hay que construir uno por backend a medir.
//
// Tres modos:
//
//   build_dataset triples <triples.txt> <dir_salida> [--variant <lista>] [--grid_side <n>] [--only_so]
//       Una tripleta "s p o" (ids numericos ya enumerados) por linea; agrupa por predicado y escribe los tres
//       quadtrees de cada uno. Una tripleta repetida no cambia el resultado.
//       --only_so omite las proyecciones S y O. --grid_side (default 52000000): lado de la grilla, mayor que
//       todos los ids.
//
//   build_dataset pairs <dir_entrada> <dir_salida> [--prefix <p>] [--others assign|skip] [--variant <lista>] [--grid_side <n>] [--only_so]
//       Un directorio con una relacion binaria por archivo ("s o" por linea) llamado <prefijo><id_predicado>
//       (default --prefix prop-direct-P, el formato de ccqQDAGS-high_arity_qdag/data). Los archivos que no
//       calzan con prefijo + solo digitos (prop-direct-normalized-P269, R1, ...) no tienen id propio y su
//       numero chocaria con el de otro predicado: con --others assign (default) reciben ids max_id+1, +2, ...
//       en orden alfabetico (max_id = mayor id <prefijo><id>; no depende de si el archivo esta vacio), y el
//       mapa "id<TAB>nombre" se escribe FUERA de los datos (el lector carga todo archivo del directorio): en
//       <dir_salida>.predicados_extra.tsv con un backend, en <dir_salida>/predicados_extra.tsv con varios.
//       Con --others skip se omiten y se cuentan. Se ignoran los ocultos (.nombre) y los vacios. Con skip el
//       resultado es el de `triples` con las tripletas "s id o".
//       Procesa un archivo por vez: la memoria es la de la relacion mas grande.
//
//   build_dataset convert <dir_entrada> <dir_salida> [--from <backend>] [--variant <lista>] [--grid_side <n>]
//       Pasa un dataset ya construido (--from, default bv64) a otros backends sin releer las tripletas.
//       --grid_side por omision es k^altura de cada arbol (misma altura que el lado original; la consulta usa
//       su propio --grid_side).
//
// --variant: uno o varios backends separados por comas, o "all" (bv64 | v5 | v | sd | rrr | rrr15; default
// bv64). Con un backend los archivos van en <dir_salida>; con varios, en <dir_salida>/<backend>/.
// Al final imprime por backend el tiempo, la memoria (total y por nivel) y el espacio en disco.

namespace fs = std::filesystem;
using namespace gdj;
using namespace std::chrono;

struct Stats {
    uint64_t ram = 0, disk = 0, files = 0;
    std::vector<uint64_t> ram_per_level;
    double seconds = 0.0;
};

static void accumulate(Stats &st, const std::vector<uint64_t> &level_sizes) {
    if (st.ram_per_level.size() < level_sizes.size())
        st.ram_per_level.resize(level_sizes.size(), 0);
    for (size_t i = 0; i < level_sizes.size(); ++i)
        st.ram_per_level[i] += level_sizes[i];
}

// Construye un quadtree de `d` atributos y lo escribe. false si no se pudo abrir el archivo de salida.
template<class SeQuadT>
static bool build_and_write(std::vector<std::vector<idx_type>> &points, uint64_t side, uint8_t k, uint8_t d,
                            const fs::path &file, Stats &st) {
    SeQuadT qt(points, side, k, d);
    std::ofstream out(file, std::ios::binary);
    if (!out.is_open())
        return false;
    qt.serialize(out);
    st.ram += qt.size_in_bytes();
    accumulate(st, qt.size_in_bytes_per_level());
    out.flush();
    st.disk += (uint64_t)out.tellp();
    st.files++;
    return true;
}

// Escribe SO, S y O del predicado `pid` a partir de sus pares (s, o).
template<class SeQuadT>
static bool write_predicate(uint64_t pid, const std::vector<std::pair<uint64_t, uint64_t>> &pairs, const fs::path &dir,
                            uint64_t side, bool only_so, Stats &st) {
    std::vector<std::vector<idx_type>> so;
    std::vector<uint64_t> subjects, objects;
    so.reserve(pairs.size());
    for (auto &p : pairs) {
        so.push_back({(idx_type)p.first, (idx_type)p.second});
        subjects.push_back(p.first);
        objects.push_back(p.second);
    }
    const std::string base = (dir / std::to_string(pid)).string();
    if (!build_and_write<SeQuadT>(so, side, 2, 2, base, st))
        return false;
    if (only_so)
        return true;
    auto unary = [](std::vector<uint64_t> v) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        std::vector<std::vector<idx_type>> out;
        out.reserve(v.size());
        for (auto x : v)
            out.push_back({(idx_type)x});
        return out;
    };
    auto s_pts = unary(subjects), o_pts = unary(objects);
    return build_and_write<SeQuadT>(s_pts, side, 2, 1, base + "s", st) && build_and_write<SeQuadT>(o_pts, side, 2, 1, base + "o", st);
}

// Los seis backends.
#define QDAGS_BACKENDS(X) \
    X(bv64,  rank_bv_64_p) \
    X(v5,    rank_vfamily_p<sdsl::rank_support_v5<>>) \
    X(v,     rank_vfamily_p<sdsl::rank_support_v<>>) \
    X(sd,    rank_sd_p) \
    X(rrr,   rank_rrr_p) \
    X(rrr15, rank_rrr15_p)

static std::vector<std::string> all_backends() {
    std::vector<std::string> v;
#define X(name, type) v.push_back(#name);
    QDAGS_BACKENDS(X)
#undef X
    return v;
}

static bool valid_backend(const std::string &b) {
    for (auto &x : all_backends())
        if (x == b)
            return true;
    return false;
}

static std::vector<std::string> parse_variants(const std::string &arg) {
    if (arg == "all")
        return all_backends();
    std::vector<std::string> out;
    std::stringstream ss(arg);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (!valid_backend(tok))
            throw std::runtime_error("invalid backend: '" + tok + "' (bv64|v5|v|sd|rrr|rrr15|all)");
        if (std::find(out.begin(), out.end(), tok) == out.end())
            out.push_back(tok);
    }
    if (out.empty())
        throw std::runtime_error("--variant is empty");
    return out;
}

// Lee un archivo con "s o" por linea. false (motivo en `err`) si no abre, hay un no-numero o queda un id suelto.
static bool read_pairs(const fs::path &file, std::vector<std::pair<uint64_t, uint64_t>> &pairs, uint64_t &max_id, std::string &err) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        err = "cannot open";
        return false;
    }
    std::string buf((size_t)in.tellg(), '\0');
    in.seekg(0);
    in.read(buf.data(), (std::streamsize)buf.size());
    const char *p = buf.data(), *end = p + buf.size();
    auto next = [&](uint64_t &v) -> int {                  // 1 = leyo un id, 0 = fin del archivo, -1 = no es un numero
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t'))
            ++p;
        if (p == end)
            return 0;
        auto r = std::from_chars(p, end, v);
        if (r.ec != std::errc())
            return -1;
        p = r.ptr;
        return 1;
    };
    uint64_t s, o;
    for (;;) {
        int a = next(s);
        if (a == 0)
            return true;
        if (a < 0) {
            err = "hay algo que no es un id numerico";
            return false;
        }
        if (next(o) != 1) {
            err = "dangling id (expected \"s o\" pairs)";
            return false;
        }
        pairs.push_back({s, o});
        max_id = std::max({max_id, s, o});
    }
}

// Escribe una relacion binaria en todos los backends pedidos.
static bool write_predicate_all(const std::vector<std::string> &variants, const std::vector<fs::path> &dirs, uint64_t pid,
                                const std::vector<std::pair<uint64_t, uint64_t>> &pairs, uint64_t side, bool only_so,
                                std::vector<Stats> &stats) {
    for (size_t i = 0; i < variants.size(); ++i) {
        auto t0 = steady_clock::now();
        bool ok = false;
#define X(name, type) if (variants[i] == #name) ok = write_predicate<se_quadtree<type>>(pid, pairs, dirs[i], side, only_so, stats[i]);
        QDAGS_BACKENDS(X)
#undef X
        stats[i].seconds += duration<double>(steady_clock::now() - t0).count();
        if (!ok) {
            std::cerr << "Error: cannot write predicate " << pid << " to " << dirs[i] << std::endl;
            return false;
        }
    }
    return true;
}

// Reescribe un quadtree ya cargado (de cualquier backend de origen) en el backend de destino.
template<class SeDst, class SeSrc>
static bool rewrite(SeSrc &src, uint64_t side_arg, const fs::path &file, Stats &st) {
    auto edges = src.get_edges();
    const uint8_t k = src.getK(), d = src.getD();
    uint64_t side = side_arg;
    if (side == 0) {                     // k^altura: misma altura que el lado original
        side = 1;
        for (uint16_t h = 0; h < src.getHeight(); ++h)
            side *= k;
    }
    return build_and_write<SeDst>(edges, side, k, d, file, st);
}

template<class SeSrc>
static int convert_from(const fs::path &in_dir, const std::vector<std::string> &variants, const std::vector<fs::path> &dirs,
                        uint64_t side_arg, std::vector<Stats> &stats) {
    uint64_t n = 0, skipped = 0;
    std::vector<fs::path> files;
    for (auto &e : fs::directory_iterator(in_dir))
        if (e.is_regular_file())
            files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (auto &f : files) {
        SeSrc src;
        std::ifstream in(f, std::ios::binary);
        if (!in.is_open() || !src.load(in)) {
            std::cerr << "  (skipping " << f.filename() << ": cannot read with the source backend)" << std::endl;
            ++skipped;
            continue;
        }
        for (size_t i = 0; i < variants.size(); ++i) {
            auto t0 = steady_clock::now();
            bool ok = false;
#define X(name, type) if (variants[i] == #name) ok = rewrite<se_quadtree<type>>(src, side_arg, dirs[i] / f.filename(), stats[i]);
            QDAGS_BACKENDS(X)
#undef X
            stats[i].seconds += duration<double>(steady_clock::now() - t0).count();
            if (!ok) {
                std::cerr << "Error: cannot write " << (dirs[i] / f.filename()) << std::endl;
                return 1;
            }
        }
        ++n;
    }
    std::cout << "Files converted: " << n << (skipped ? ", skipped: " + std::to_string(skipped) : std::string()) << std::endl;
    return n > 0 ? 0 : 1;
}

static void print_stats(const std::string &variant, const Stats &st, uint64_t total_triples) {
    const double mb = 1024.0 * 1024.0;
    std::cout << "\n[" << variant << "] " << st.files << " files, " << st.seconds << " s" << std::endl;
    std::cout << "  memory (size_in_bytes):  " << st.ram / mb << " MB" << std::endl;
    std::cout << "  disk:                    " << st.disk / mb << " MB" << std::endl;
    if (total_triples > 0) {
        std::cout << "  bytes per triple:        " << (double)st.ram / total_triples << " (memory), "
                  << (double)st.disk / total_triples << " (disk)" << std::endl;
    }
    std::cout << "  memory per level:";
    for (size_t l = 0; l < st.ram_per_level.size(); ++l)
        std::cout << " " << l << ":" << st.ram_per_level[l] / mb << "MB";
    std::cout << std::endl;
}

static int usage(const char *prog) {
    std::cerr << "Usage:\n"
              << "  " << prog << " triples <triples.txt> <output_dir> [--variant <list>|all] [--grid_side <n>] [--only_so]\n"
              << "  " << prog << " pairs <dir_with_prefix_id_files> <output_dir> [--prefix <p>] [--others assign|skip] [--variant <list>|all] [--grid_side <n>] [--only_so]\n"
              << "  " << prog << " convert <input_dir> <output_dir> [--from <backend>] [--variant <list>|all] [--grid_side <n>]\n"
              << "backends: bv64 | v5 | v | sd | rrr | rrr15" << std::endl;
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 4)
        return usage(argv[0]);
    const std::string mode = argv[1];
    const std::string input = argv[2];
    const fs::path out_base = argv[3];
    if (mode != "triples" && mode != "convert" && mode != "pairs")
        return usage(argv[0]);

    std::string variant_arg = "bv64", from = "bv64", prefix = "prop-direct-P", others = "assign";
    uint64_t grid_side = (mode == "triples" || mode == "pairs") ? 52000000 : 0;
    bool only_so = false;
    try {
        for (int i = 4; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--variant" && i + 1 < argc)
                variant_arg = argv[++i];
            else if (a == "--from" && i + 1 < argc)
                from = argv[++i];
            else if (a == "--prefix" && i + 1 < argc)
                prefix = argv[++i];
            else if (a == "--others" && i + 1 < argc)
                others = argv[++i];
            else if (a == "--grid_side" && i + 1 < argc)
                grid_side = std::stoull(argv[++i]);
            else if (a == "--only_so")
                only_so = true;
            else {
                std::cerr << "Error: unknown argument: " << a << std::endl;
                return usage(argv[0]);
            }
        }
    } catch (const std::exception &e) { std::cerr << "Error: bad arguments: " << e.what() << std::endl; return 1; }
    if ((mode == "triples" || mode == "pairs") && grid_side == 0) {
        std::cerr << "Error: --grid_side must be > 0" << std::endl;
        return 1;
    }
    if (others != "assign" && others != "skip") {
        std::cerr << "Error: --others must be assign or skip" << std::endl;
        return 1;
    }
    if (!valid_backend(from)) {
        std::cerr << "Error: invalid --from: " << from << std::endl;
        return 1;
    }

    std::vector<std::string> variants;
    try { variants = parse_variants(variant_arg); }
    catch (const std::exception &e) { std::cerr << "Error: " << e.what() << std::endl; return 1; }

    std::vector<fs::path> dirs;
    for (auto &v : variants)
        dirs.push_back(variants.size() == 1 ? out_base : out_base / v);
    for (auto &d : dirs)
        fs::create_directories(d);

    std::vector<Stats> stats(variants.size());
    uint64_t total_triples = 0;
    const auto t_start = steady_clock::now();

    if (mode == "triples") {
        std::ifstream in(input);
        if (!in.is_open()) {
            std::cerr << "Error: cannot open " << input << std::endl;
            return 1;
        }
        std::cout << "[1/2] Reading and grouping triples by predicate..." << std::endl;
        std::map<uint64_t, std::vector<std::pair<uint64_t, uint64_t>>> groups;
        uint64_t s, p, o, max_id = 0;
        while (in >> s >> p >> o) {
            groups[p].push_back({s, o});
            max_id = std::max({max_id, s, o});
            if (++total_triples % 5000000 == 0)
                std::cout << "  " << total_triples / 1000000 << "M triples\r" << std::flush;
        }
        if (total_triples == 0) {
            std::cerr << "Error: " << input << " has no \"s p o\" triples" << std::endl;
            return 1;
        }
        if (max_id >= grid_side) {
            std::cerr << "Error: ids go up to " << max_id << " but --grid_side is " << grid_side << " (must exceed all ids)" << std::endl;
            return 1;
        }
        std::cout << "  " << total_triples << " triples, " << groups.size() << " predicates" << std::endl;
        std::cout << "[2/2] Building " << (only_so ? "SO" : "SO, S and O") << " in: ";
        for (auto &v : variants)
            std::cout << v << " ";
        std::cout << std::endl;
        uint64_t done = 0;
        for (auto &[pid, pairs] : groups) {
            if (!write_predicate_all(variants, dirs, pid, pairs, grid_side, only_so, stats))
                return 1;
            pairs.clear(); pairs.shrink_to_fit();                        // libera el grupo apenas esta escrito
            if (++done % 100 == 0 || done == groups.size())
                std::cout << "  " << done << "/" << groups.size() << " predicates\r" << std::flush;
        }
        std::cout << std::endl;
    } else if (mode == "pairs") {
        if (!fs::is_directory(input)) {
            std::cerr << "Error: " << input << " is not a directory" << std::endl;
            return 1;
        }
        if (prefix.empty()) {
            std::cerr << "Error: --prefix is empty" << std::endl;
            return 1;
        }
        std::vector<std::pair<uint64_t, fs::path>> files;       // (id del predicado, archivo)
        std::vector<fs::path> other_files;                      // los que no son <prefijo><id>
        for (auto &e : fs::directory_iterator(input)) {
            if (!e.is_regular_file())
                continue;
            const std::string name = e.path().filename().string();
            if (name.empty() || name[0] == '.')
                continue;
            const std::string rest = name.rfind(prefix, 0) == 0 ? name.substr(prefix.size()) : std::string();
            if (rest.empty() || rest.size() > 18 || rest.find_first_not_of("0123456789") != std::string::npos) {
                other_files.push_back(e.path());
                continue;
            }
            files.push_back({std::stoull(rest), e.path()});
        }
        std::sort(files.begin(), files.end());
        std::sort(other_files.begin(), other_files.end());
        if (files.empty()) {
            std::cerr << "Error: " << input << " has no <prefix><id> files with prefix '" << prefix << "'" << std::endl;
            return 1;
        }
        std::string map_file;
        const uint64_t n_numbered = files.size();
        const uint64_t n_other = other_files.size();
        if (others == "assign") {
            // ids nuevos por encima del mayor: no pueden chocar con ningun <prefijo><id>
            uint64_t next = files.back().first + 1;
            fs::path out_dir = out_base;
            while (!out_dir.empty() && out_dir.filename().empty())      // ignora una "/" final
                out_dir = out_dir.parent_path();
            const fs::path map_path = variants.size() == 1 ? fs::path(out_dir.string() + ".predicados_extra.tsv")
                                                           : out_dir / "predicados_extra.tsv";
            map_file = map_path.string();
            std::ofstream map(map_path);
            if (!map.is_open()) {
                std::cerr << "Error: cannot write " << map_path << std::endl;
                return 1;
            }
            for (auto &f : other_files) {
                files.push_back({next, f});
                map << next << '\t' << f.filename().string() << '\n';
                ++next;
            }
        }
        std::cout << n_numbered << " files '" << prefix << "<id>'; " << n_other << " with other names "
                  << (others == "assign" && n_other ? "(new ids from " + std::to_string(files.back().first - n_other + 1) + ", map in " + map_file + ")"
                                         : std::string("(skipped)")) << std::endl;
        std::cout << "Building " << (only_so ? "SO" : "SO, S and O") << " in: ";
        for (auto &v : variants)
            std::cout << v << " ";
        std::cout << std::endl;
        uint64_t done = 0, empty_files = 0, max_id_all = 0;
        for (auto &[pid, path] : files) {
            std::vector<std::pair<uint64_t, uint64_t>> pairs;
            uint64_t max_id = 0;
            std::string err;
            if (!read_pairs(path, pairs, max_id, err)) {
                std::cerr << "Error: " << path.filename() << ": " << err << std::endl;
                return 1;
            }
            if (pairs.empty()) {
                ++empty_files;
                continue;
            }
            if (max_id >= grid_side) {
                std::cerr << "Error: " << path.filename() << " has ids up to " << max_id << " but --grid_side is " << grid_side
                          << " (must exceed all ids)" << std::endl;
                return 1;
            }
            max_id_all = std::max(max_id_all, max_id);
            total_triples += pairs.size();
            if (!write_predicate_all(variants, dirs, pid, pairs, grid_side, only_so, stats))
                return 1;
            if (++done % 100 == 0 || done + empty_files == files.size())
                std::cout << "  " << done + empty_files << "/" << files.size() << " files\r" << std::flush;
        }
        std::cout << std::endl;
        if (done == 0) {
            std::cerr << "Error: all files are empty" << std::endl;
            return 1;
        }
        std::cout << done << " predicates, " << total_triples << " edges, max id " << max_id_all
                  << (empty_files ? ", " + std::to_string(empty_files) + " empty files skipped" : std::string()) << std::endl;
    } else {
        if (!fs::is_directory(input)) {
            std::cerr << "Error: " << input << " is not a directory" << std::endl;
            return 1;
        }
        std::cout << "Converting " << input << " (backend " << from << ") to: ";
        for (auto &v : variants)
            std::cout << v << " ";
        std::cout << std::endl;
        int rc = 1;
#define X(name, type) if (from == #name) rc = convert_from<se_quadtree<type>>(input, variants, dirs, grid_side, stats);
        QDAGS_BACKENDS(X)
#undef X
        if (rc != 0)
            return rc;
    }

    std::cout << "\n=========== summary (" << duration<double>(steady_clock::now() - t_start).count() << " s total) ===========" << std::endl;
    for (size_t i = 0; i < variants.size(); ++i)
        print_stats(variants[i], stats[i], total_triples);
    return 0;
}
