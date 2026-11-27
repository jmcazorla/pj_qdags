#ifndef QDAGS_QUERY_TOPOLOGY_PLAN_SELECTION_HPP
#define QDAGS_QUERY_TOPOLOGY_PLAN_SELECTION_HPP

#include <string>
#include <map>
#include <utility>
#include <fstream>
#include <filesystem>
#include <cstdlib>

#include "classify_query.hpp"
#include "plan_parser.hpp"

// Elige el plan por defecto de una topologia sin --plan_file/--plan_id. Los datos (topologia->archivo y
// (topologia,grupo)->plan_id) estan en query/plans/topology_plan_files.csv y topology_plan_selection.csv.

namespace gdj {

    // Estilos de plan: Normal (default), Best, WkK (kernel 1D antes de las tablas, como WeakKernel) y Leapfrog
    // (prejoins escalonados).
    enum class PlanGroup { Normal, Best, WkK, Leapfrog };

    inline PlanGroup parse_plan_group(const std::string& s) {
        if (s == "best")
            return PlanGroup::Best;
        if (s == "wk_k")
            return PlanGroup::WkK;
        if (s == "leapfrog")
            return PlanGroup::Leapfrog;
        return PlanGroup::Normal;
    }

    inline const char* to_string(PlanGroup g) {
        switch (g) {
            case PlanGroup::Best:
                return "best";
            case PlanGroup::WkK:
                return "wk_k";
            case PlanGroup::Leapfrog:
                return "leapfrog";
            default:
                return "normal";
        }
    }

    inline std::filesystem::path default_plans_dir() {
        if (const char* env = std::getenv("QDAGS_PLANS_DIR")) {
            if (env[0] != '\0')
                return std::filesystem::path(env);
        }
        return std::filesystem::path(__FILE__).parent_path() / "plans";
    }

    // topologia->archivo DSL y (topologia,grupo)->plan_id dentro de ese archivo. SINGLE_PATTERN y
    // UNKNOWN_OR_ERROR no aparecen.
    struct topology_plan_catalog {
        std::map<QueryTopology, std::string> files;
        std::map<std::pair<QueryTopology, PlanGroup>, std::string> plan_ids;
    };

    namespace detail {
        // Hasta 3 campos por ';'; el 3ro (plan_id) conserva el resto de la linea, que tiene comas propias.
        inline std::vector<std::string> split_csv_line(const std::string& line) {
            std::vector<std::string> out;
            size_t p1 = line.find(';');
            if (p1 == std::string::npos) {
                out.push_back(line);
                return out;
            }
            out.push_back(line.substr(0, p1));
            size_t p2 = line.find(';', p1 + 1);
            if (p2 == std::string::npos) {
                out.push_back(line.substr(p1 + 1));
                return out;
            }
            out.push_back(line.substr(p1 + 1, p2 - p1 - 1));
            out.push_back(line.substr(p2 + 1));
            return out;
        }

        inline bool is_blank(const std::string& s) {
            return s.find_first_not_of(" \t\r\n") == std::string::npos;
        }
    }

    inline topology_plan_catalog load_topology_plan_catalog(const std::filesystem::path& dir) {
        topology_plan_catalog cat;

        std::ifstream files_csv(dir / "topology_plan_files.csv");
        std::string line;
        bool first = true;
        while (std::getline(files_csv, line)) {
            if (first) {
                first = false;
                continue;
            } // encabezado "topology;file"
            if (detail::is_blank(line))
                continue;
            auto parts = detail::split_csv_line(line);
            if (parts.size() < 2)
                continue;
            cat.files[parse_query_topology(parts[0])] = parts[1];
        }

        std::ifstream sel_csv(dir / "topology_plan_selection.csv");
        first = true;
        while (std::getline(sel_csv, line)) {
            if (first) {
                first = false;
                continue;
            } // encabezado "topology;group;plan_id"
            if (detail::is_blank(line))
                continue;
            auto parts = detail::split_csv_line(line);
            if (parts.size() < 3)
                continue;
            QueryTopology topo = parse_query_topology(parts[0]);
            PlanGroup group = parse_plan_group(parts[1]);
            cat.plan_ids[{topo, group}] = parts[2];
        }

        return cat;
    }

    // Catalogo por defecto: se carga una vez (init estatico thread-safe) y es de solo lectura.
    inline const topology_plan_catalog& default_topology_plan_catalog() {
        static topology_plan_catalog cat = load_topology_plan_catalog(default_plans_dir());
        return cat;
    }

    // Cache de planes parseados, por (topologia, grupo). Cada archivo se parsea una vez aunque lo compartan varias
    // topologias. get() devuelve nullptr si no hay archivo, plan_id o plan para esa topologia y grupo.
    class plan_catalog_cache {
    public:
        plan_catalog_cache()
            : catalog_(&default_topology_plan_catalog()), dir_(default_plans_dir()) {}

        plan_catalog_cache(const topology_plan_catalog& catalog, std::filesystem::path dir)
            : catalog_(&catalog), dir_(std::move(dir)) {}

        const execution_plan* get(QueryTopology topo, PlanGroup group) {
            auto file_it = catalog_->files.find(topo);
            if (file_it == catalog_->files.end())
                return nullptr;

            auto id_it = catalog_->plan_ids.find({topo, group});
            if (id_it == catalog_->plan_ids.end())
                return nullptr;

            auto cached = parsed_files_.find(file_it->second);
            if (cached == parsed_files_.end())
                cached = parsed_files_.emplace(file_it->second,
                                                PlanParser::parseFile((dir_ / file_it->second).string())).first;

            auto plan_it = cached->second.find(id_it->second);
            if (plan_it == cached->second.end())
                return nullptr;
            return &plan_it->second;
        }

    private:
        const topology_plan_catalog* catalog_;
        std::filesystem::path dir_;
        std::map<std::string, std::map<std::string, execution_plan>> parsed_files_;
    };

} // namespace gdj

#endif // QDAGS_QUERY_TOPOLOGY_PLAN_SELECTION_HPP
