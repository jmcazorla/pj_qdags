#include <iostream>
#include <string>
#include <chrono>
#include <cstdint>

#include "qdags/index/commun.hpp"
#include "qdags/index/se_quadtree_bv64.hpp"
#include "qdags/index/qdag_bv64.hpp"
#include "qdags/join/joins.hpp"
#include "qdags/join/joins_wide.hpp"
#include "qdags/query/triple_pattern.hpp"
#include "qdags/query/resource_manager_bv64.hpp"
#include "qdags/query/query_builder.hpp"
#include "qdags/query/query_loader.hpp"
#include "qdags/query/classify_query.hpp"
#include "qdags/strategy/generic_strategies.hpp"

// Ejemplo end-to-end: carga un resource_manager y un archivo de queries y ejecuta cada una con
// strategy_non_lonely (sirve para cualquier BGP con variables compartidas, sin plan por topologia).
// Datos de prueba: generate_demo_dataset.cpp. <directorio_recursos> debe contener SOLO los indices (en
// "relations/"), nunca queries.txt u otro archivo no-indice.
// Uso: run_bgp_query <directorio_recursos> <archivo_queries> [upper_bound=0] [parallel_level=0]

using namespace gdj;
using namespace std::chrono;

// Ejecuta una query de 2+ patrones con strategy_non_lonely y devuelve la cardinalidad; libera lo intermedio.
static uint64_t run_query(const query_tp &query, resource_manager_bv64 &res, uint64_t upper_bound, uint64_t parallel_level, uint64_t grid_side) {
    auto meta = build_nl_metadata<se_quadtree_p>(query, res);

    std::vector<qdag_p*> qdags_non_lonely, qdags_f;
    se_quadtree_p *quadtree_join_proj = nullptr;
    se_quadtree_p *quadtree_join_final = nullptr;
    uint64_t n_results = 0;

    strategy_non_lonely<qdag_p, se_quadtree_p>(
        meta, qdags_non_lonely, qdags_f, quadtree_join_proj, quadtree_join_final, n_results,
        upper_bound, parallel_level, grid_side);

    for (auto *q : qdags_non_lonely)
        delete q;
    for (auto *q : qdags_f)
        delete q;
    delete quadtree_join_proj;
    delete quadtree_join_final;

    return n_results;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <resources_dir> <queries_file> [upper_bound=0] [parallel_level=0]" << std::endl;
        std::cerr << "  (see generate_demo_dataset.cpp to build test data)" << std::endl;
        return 1;
    }

    std::string res_dir = argv[1];
    std::string queries_file = argv[2];
    uint64_t upper_bound = argc > 3 ? std::stoull(argv[3]) : 0;
    uint64_t parallel_level = argc > 4 ? std::stoull(argv[4]) : 0;
    // Grid side "infinito": basta que sea >= al side real de los datos.
    uint64_t grid_side = 52000000;

    resource_manager_bv64 res;
    res.load_directory(res_dir);
    res.print_summary();

    auto queries = load_all_queries(queries_file);
    std::cout << std::endl << "Loaded " << queries.size() << " queries from " << queries_file << std::endl << std::endl;

    double total_ms = 0.0;
    for (size_t i = 0; i < queries.size(); ++i) {
        auto &query = queries[i];
        QueryTopology topo = classify_query_topology(query);

        std::cout << "Query " << (i + 1) << "/" << queries.size()
                  << " (" << query.size() << " patterns, topology " << to_string(topo) << "): ";

        if (query.size() < 2) {
            // Un solo patron: la respuesta es la relacion del predicado, no hay join.
            std::cout << "no join (single pattern), solved with one get_so/get_s/get_o" << std::endl;
            continue;
        }

        auto t0 = high_resolution_clock::now();
        uint64_t n_results = run_query(query, res, upper_bound, parallel_level, grid_side);
        auto t1 = high_resolution_clock::now();
        double ms = duration_cast<duration<double, std::milli>>(t1 - t0).count();
        total_ms += ms;

        std::cout << n_results << " results in " << ms << " ms" << std::endl;
    }

    std::cout << std::endl << "RESULT;" << queries.size() << ";" << total_ms << std::endl;
    return 0;
}
