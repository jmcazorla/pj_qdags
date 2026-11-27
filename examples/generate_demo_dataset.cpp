#include <iostream>
#include <fstream>
#include <string>
#include <set>
#include <vector>
#include <random>
#include <cstdint>
#include <sys/stat.h>

#include "qdags/index/commun.hpp"
#include "qdags/index/se_quadtree_bv64.hpp"

// Datos de juguete para probar run_bgp_query: 5 predicados con aristas aleatorias (side x side), en la convencion
// de resource_manager::load_directory ("<pid>"=SO, "<pid>s"=S, "<pid>o"=O), y 4 queries de ejemplo (cadena,
// estrella, triangulo, patron unico).
// Los indices van en <directorio_salida>/relations/, NO junto a queries.txt: load_directory intenta parsear todo
// archivo del directorio como un se_quadtree y un queries.txt ahi causa bad_alloc.
// Uso: generate_demo_dataset <directorio_salida> [side=24] [seed=42]

using namespace gdj;

using Edge = std::pair<uint64_t, uint64_t>;

static std::vector<Edge> gen_random_edges(uint64_t side, uint64_t n, std::mt19937 &rng) {
    std::uniform_int_distribution<uint64_t> dist(0, side - 1);
    std::set<Edge> pts;
    while (pts.size() < n)
        pts.insert({dist(rng), dist(rng)});
    return std::vector<Edge>(pts.begin(), pts.end());
}

static std::vector<std::vector<idx_type>> to_edge_vec(const std::vector<Edge> &e) {
    std::vector<std::vector<idx_type>> v;
    for (auto &p : e)
        v.push_back({(idx_type)p.first, (idx_type)p.second});
    return v;
}

static std::vector<std::vector<idx_type>> to_unary_vec(const std::set<uint64_t> &vals) {
    std::vector<std::vector<idx_type>> v;
    for (auto x : vals)
        v.push_back({(idx_type)x});
    return v;
}

// Escribe la relacion SO y sus proyecciones unarias S y O.
static void write_relation(const std::string &dir, uint64_t pid, uint64_t side, const std::vector<Edge> &edges) {
    auto edge_vec = to_edge_vec(edges);
    se_quadtree_p qt_so(edge_vec, side, 2, 2);
    { std::ofstream out(dir + "/" + std::to_string(pid), std::ios::binary); qt_so.serialize(out); }

    std::set<uint64_t> svals, ovals;
    for (auto &e : edges) {
        svals.insert(e.first);
        ovals.insert(e.second);
    }

    auto s_vec = to_unary_vec(svals);
    se_quadtree_p qt_s(s_vec, side, 2, 1);
    { std::ofstream out(dir + "/" + std::to_string(pid) + "s", std::ios::binary); qt_s.serialize(out); }

    auto o_vec = to_unary_vec(ovals);
    se_quadtree_p qt_o(o_vec, side, 2, 1);
    { std::ofstream out(dir + "/" + std::to_string(pid) + "o", std::ios::binary); qt_o.serialize(out); }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <output_dir> [side=24] [seed=42]" << std::endl;
        return 1;
    }

    std::string out_dir = argv[1];
    uint64_t side = argc > 2 ? std::stoull(argv[2]) : 24;
    uint32_t seed = argc > 3 ? (uint32_t)std::stoul(argv[3]) : 42u;

    std::string relations_dir = out_dir + "/relations";
    mkdir(out_dir.c_str(), 0755);
    mkdir(relations_dir.c_str(), 0755);

    std::mt19937 rng(seed);
    // Predicados 1..5, 40 aristas cada uno.
    for (uint64_t pid = 1; pid <= 5; ++pid) {
        auto edges = gen_random_edges(side, 40, rng);
        write_relation(relations_dir, pid, side, edges);
        std::cerr << "predicate " << pid << ": " << edges.size() << " edges" << std::endl;
    }

    std::string queries_path = out_dir + "/queries.txt";
    {
        std::ofstream out(queries_path);
        out << "?x 1 ?y . ?y 2 ?z\n";                 // cadena de 2 saltos (P2)
        out << "?x 3 ?a . ?x 4 ?b . ?x 5 ?c\n";        // estrella de 3 (T3)
        out << "?x 1 ?y . ?y 2 ?z . ?z 3 ?x\n";        // triangulo dirigido (Tr1)
        out << "?x 1 ?y\n";                            // patron unico
    }

    std::cerr << std::endl << "Indexes written to " << relations_dir
              << " (side=" << side << ", seed=" << seed << ")" << std::endl;
    std::cerr << "Example queries in " << queries_path << std::endl;
    std::cerr << std::endl << "Try:" << std::endl;
    std::cerr << "  ./run_bgp_query " << relations_dir << " " << queries_path << std::endl;

    return 0;
}
