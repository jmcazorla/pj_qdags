#ifndef QDAGS_QUERY_QUERY_LOADER_HPP
#define QDAGS_QUERY_QUERY_LOADER_HPP

#include <string>
#include <vector>
#include <unordered_map>
#include <sstream>
#include <fstream>
#include <stdexcept>
#include <cstdint>

#include "triple_pattern.hpp"

// Carga queries BGP de un archivo de texto, una por linea: patrones separados por '.', terminos (S P O)
// separados por espacios. '?x' es variable (mismo id dentro de la query); lo demas es una constante numerica:
// "?x 1 ?y . ?y 2 ?z". No resuelve IRIs.

namespace gdj {

    inline std::vector<std::string> split_and_trim(const std::string& s, char delim) {
        std::vector<std::string> out;
        std::istringstream stream(s);
        std::string field;
        while (std::getline(stream, field, delim)) {
            size_t start = field.find_first_not_of(" \t\r\n");
            if (start == std::string::npos)
                continue;
            size_t end = field.find_last_not_of(" \t\r\n");
            out.push_back(field.substr(start, end - start + 1));
        }
        return out;
    }

    // Parsea un patron de triple (S P O) y asigna ids a las variables locales de la query. 
    inline triple_pattern parse_triple_pattern(const std::string& text, std::unordered_map<std::string, uint16_t>& var_ids) {
        auto terms = split_and_trim(text, ' ');
        if (terms.size() != 3)
            throw std::runtime_error("parse_triple_pattern: expected 3 terms (S P O), found "
                                      + std::to_string(terms.size()) + " en '" + text + "'");

        auto term_id = [&](const std::string& name) -> uint16_t {
            auto it = var_ids.find(name);
            if (it != var_ids.end())
                return it->second;
            uint16_t id = (uint16_t)var_ids.size();
            var_ids.emplace(name, id);
            return id;
        };

        triple_pattern tp;
        if (terms[0][0] == '?')
            tp.var_s(term_id(terms[0].substr(1)));
        else
            tp.const_s(std::stoull(terms[0]));
        if (terms[1][0] == '?')
            tp.var_p(term_id(terms[1].substr(1)));
        else
            tp.const_p(std::stoull(terms[1]));
        if (terms[2][0] == '?')
            tp.var_o(term_id(terms[2].substr(1)));
        else
            tp.const_o(std::stoull(terms[2]));
        return tp;
    }

    // Parsea una query completa; los nombres de variable son locales a ella.
    inline query_tp parse_query_line(const std::string& line) {
        std::unordered_map<std::string, uint16_t> var_ids;
        query_tp query;
        for (auto& pattern_text : split_and_trim(line, '.'))
            query.push_back(parse_triple_pattern(pattern_text, var_ids));
        return query;
    }

    // Carga todas las queries de un archivo (se ignoran las lineas vacias).
    inline std::vector<query_tp> load_all_queries(const std::string& filename) {
        std::ifstream in(filename);
        if (!in.is_open())
            throw std::runtime_error("load_all_queries: cannot open '" + filename + "'");

        std::vector<query_tp> queries;
        std::string line;
        while (std::getline(in, line)) {
            if (line.find_first_not_of(" \t\r\n") == std::string::npos)
                continue;
            queries.push_back(parse_query_line(line));
        }
        return queries;
    }

} // namespace gdj

#endif // QDAGS_QUERY_QUERY_LOADER_HPP
