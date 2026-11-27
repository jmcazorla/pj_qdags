// Dataset en el formato de ccqQDAGS-high_arity_qdag (el que lee run_ccq): por predicado el quadtree SO (k=2, d=2)
// y, salvo --only_so, los unarios S y O (d=1), cada uno con su bitmap `active` por nivel. Hermano de
// build_dataset pairs (mismas opciones e ids) pero sin compartir codigo: se serializa ccq::se_quadtree.
//
// Uso:
//   build_dataset_ccq <dir_con_archivos_prefijo_id> <dir_salida> [--prefix <p>] [--others assign|skip]
//                     [--grid_side <n>] [--only_so]
//
// Entrada: un archivo "<prefijo><id>" por predicado, con "s o" por linea (prefijo por defecto prop-direct-P). Los
// de otro nombre reciben ids desde (mayor id + 1) en orden alfabetico (--others assign, default) o se omiten
// (skip); el mapa id -> archivo queda en <dir_salida>.predicados_extra.tsv, fuera del dataset (resource_manager
// carga todo archivo del directorio).
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "qdags/ccq/adapters.hpp"

namespace fs = std::filesystem;
using std::chrono::steady_clock;

struct Stats {
    uint64_t files = 0, ram = 0, disk = 0;
    double seconds = 0;
};

static bool build_and_write(std::vector<std::vector<ccq::edge_type>> &points, uint64_t side, uint8_t k, uint8_t d, const fs::path &file, Stats &st) {
    ccq::se_quadtree_t qt(points, side, k, d);
    std::ofstream out(file, std::ios::binary);
    if (!out.is_open())
        return false;
    qt.serialize(out);
    st.ram += qt.size_in_bytes();
    out.flush();
    st.disk += (uint64_t)out.tellp();
    st.files++;
    return true;
}

static bool write_predicate(uint64_t pid, const std::vector<std::pair<uint64_t, uint64_t>> &pairs, const fs::path &dir, uint64_t side,
                            bool only_so, Stats &st) {
    std::vector<std::vector<ccq::edge_type>> so;
    std::vector<uint64_t> subjects, objects;
    so.reserve(pairs.size());
    for (auto &p : pairs) {
        so.push_back({(ccq::edge_type)p.first, (ccq::edge_type)p.second});
        subjects.push_back(p.first);
        objects.push_back(p.second);
    }
    const std::string base = (dir / std::to_string(pid)).string();
    if (!build_and_write(so, side, 2, 2, base, st))
        return false;
    if (only_so)
        return true;
    auto unary = [](std::vector<uint64_t> v) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        std::vector<std::vector<ccq::edge_type>> out;
        out.reserve(v.size());
        for (auto x : v)
            out.push_back({(ccq::edge_type)x});
        return out;
    };
    auto s_pts = unary(subjects), o_pts = unary(objects);
    return build_and_write(s_pts, side, 2, 1, base + "s", st) && build_and_write(o_pts, side, 2, 1, base + "o", st);
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

static int usage(const char *prog) {
    std::cerr << "Usage: " << prog << " <dir_with_prefix_id_files> <output_dir> [--prefix <p>] [--others assign|skip] [--grid_side <n>] [--only_so]" << std::endl;
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 3)
        return usage(argv[0]);
    const fs::path input = argv[1];
    fs::path out_dir = argv[2];
    std::string prefix = "prop-direct-P", others = "assign";
    uint64_t grid_side = 52000000;
    bool only_so = false;
    try {
        for (int i = 3; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--prefix" && i + 1 < argc) prefix = argv[++i];
            else if (a == "--others" && i + 1 < argc) others = argv[++i];
            else if (a == "--grid_side" && i + 1 < argc) grid_side = std::stoull(argv[++i]);
            else if (a == "--only_so") only_so = true;
            else {
                std::cerr << "Error: unknown argument: " << a << std::endl;
                return usage(argv[0]);
            }
        }
    } catch (const std::exception &e) { std::cerr << "Error: bad arguments: " << e.what() << std::endl; return 1; }
    if (grid_side == 0 || prefix.empty() || (others != "assign" && others != "skip")) {
        std::cerr << "Error: need --grid_side > 0, non-empty --prefix and --others assign|skip" << std::endl;
        return 1;
    }
    if (!fs::is_directory(input)) {
        std::cerr << "Error: " << input << " is not a directory" << std::endl;
        return 1;
    }
    while (!out_dir.empty() && out_dir.filename().empty())      // ignora una "/" final
        out_dir = out_dir.parent_path();

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
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    if (ec || !fs::is_directory(out_dir)) {
        std::cerr << "Error: cannot create " << out_dir << std::endl;
        return 1;
    }
    const uint64_t n_numbered = files.size(), n_other = other_files.size();
    std::string map_file;
    if (others == "assign") {
        uint64_t next = files.back().first + 1;
        const fs::path map_path = out_dir.string() + ".predicados_extra.tsv";
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
    std::cout << "Building " << (only_so ? "SO" : "SO, S and O") << " in ccq format in: " << out_dir << std::endl;

    Stats st;
    auto t_start = steady_clock::now();
    uint64_t done = 0, empty_files = 0, max_id_all = 0, total_edges = 0;
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
        total_edges += pairs.size();
        if (!write_predicate(pid, pairs, out_dir, grid_side, only_so, st)) {
            std::cerr << "Error: cannot write predicate " << pid << " to " << out_dir << std::endl;
            return 1;
        }
        if (++done % 100 == 0 || done + empty_files == files.size())
            std::cout << "  " << done + empty_files << "/" << files.size() << " files\r" << std::flush;
    }
    std::cout << std::endl;
    if (done == 0) {
        std::cerr << "Error: all files are empty" << std::endl;
        return 1;
    }
    st.seconds = std::chrono::duration<double>(steady_clock::now() - t_start).count();
    std::cout << done << " predicates, " << total_edges << " edges, max id " << max_id_all
              << (empty_files ? ", " + std::to_string(empty_files) + " empty files skipped" : std::string()) << std::endl;
    std::cout << "=========== summary ===========\n"
              << "  files: " << st.files << " | disk: " << st.disk << " B (" << (st.disk >> 20) << " MiB) | memory: " << st.ram
              << " B | " << st.seconds << " s" << std::endl;
    return 0;
}
