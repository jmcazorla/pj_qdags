#include <iostream>
#include <fstream>
#include <string>
#include <map>
#include <cstdint>

#include "qdags/query/triple_pattern.hpp"
#include "qdags/query/classify_query.hpp"
#include "qdags/query/query_loader.hpp"

// Clasifica la topologia de cada query de un archivo (classify_query_topology), para separar un archivo mixto
// por topologia antes de perfilar (run_plan/run_nl/etc esperan un archivo por topologia).
// Uso: classify_queries <queries_file> <output_file>
// Escribe "query_id;topologia" por query, en el orden de entrada, e imprime un resumen por topologia.

using namespace gdj;

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <queries_file> <output_file>" << std::endl;
        return 1;
    }

    std::string queries_file = argv[1];
    std::string output_file = argv[2];

    auto all_queries = load_all_queries(queries_file);

    std::ofstream out(output_file);
    if (!out.is_open()) {
        std::cerr << "Error: cannot open " << output_file << " for writing" << std::endl;
        return 1;
    }
    out << "query_id;topology\n";

    std::map<std::string, uint64_t> counts;
    for (size_t q_idx = 0; q_idx < all_queries.size(); ++q_idx) {
        QueryTopology topo = classify_query_topology(all_queries[q_idx]);
        std::string name = to_string(topo);
        out << q_idx << ";" << name << "\n";
        counts[name]++;
    }
    out.close();

    std::cout << "Total queries: " << all_queries.size() << std::endl;
    std::cout << "-----------------------------------------------------" << std::endl;
    for (auto const& [name, count] : counts)
        std::cout << name << ";" << count << std::endl;
    std::cout << "-----------------------------------------------------" << std::endl;
    std::cout << "Output: " << output_file << std::endl;

    return 0;
}
