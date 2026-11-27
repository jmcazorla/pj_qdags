#ifndef QDAGS_EXAMPLES_LEVELS_UTIL_HPP
#define QDAGS_EXAMPLES_LEVELS_UTIL_HPP

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Varios niveles de paralelismo y repeticiones en una sola ejecucion de un perfilador (dataset cargado una vez):
// <parallel_level> acepta una lista ("0,2,4,6") y --reps N repite cada nivel.
// Cada pasada imprime "LEVEL;<nivel>;<rep>" antes de su "RESULT;..." (el script sabe cual murio si el proceso cae).
// --results_file/--times_file aceptan los marcadores {L} (nivel) y {R} (repeticion).

namespace gdj_examples {

    inline std::vector<uint64_t> parse_levels(const std::string &s) {
        std::vector<uint64_t> out;
        std::stringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            if (tok.empty())
                throw std::invalid_argument("level list has an empty element: '" + s + "'");
            size_t pos = 0;
            uint64_t v = std::stoull(tok, &pos);
            if (pos != tok.size())
                throw std::invalid_argument("invalid level: '" + tok + "'");
            out.push_back(v);
        }
        if (out.empty())
            throw std::invalid_argument("empty level list");
        return out;
    }

    inline std::string levels_to_string(const std::vector<uint64_t> &v) {
        std::string s;
        for (size_t i = 0; i < v.size(); ++i)
            s += (i ? "," : "") + std::to_string(v[i]);
        return s;
    }

    inline std::string expand_pass_path(std::string path, uint64_t level, uint32_t rep) {
        auto replace_all = [&](const std::string &key, const std::string &val) {
            for (size_t p = path.find(key); p != std::string::npos; p = path.find(key, p + val.size()))
                path.replace(p, key.size(), val);
        };
        replace_all("{L}", std::to_string(level));
        replace_all("{R}", std::to_string(rep));
        return path;
    }

    // Con varias pasadas, una salida sin marcadores se pisaria a si misma.
    inline bool path_distinguishes_passes(const std::string &path, size_t n_levels, uint32_t reps) {
        if (path.empty())
            return true;
        bool has_l = path.find("{L}") != std::string::npos;
        bool has_r = path.find("{R}") != std::string::npos;
        return (n_levels <= 1 || has_l) && (reps <= 1 || has_r);
    }

} // namespace gdj_examples

#endif // QDAGS_EXAMPLES_LEVELS_UTIL_HPP
