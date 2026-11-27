#ifndef QDAGS_RESOURCE_MANAGER_HPP
#define QDAGS_RESOURCE_MANAGER_HPP

#include <map>
#include <string>
#include <memory>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <cstdint>

// Carga los indices (uno por predicado) desde un directorio: en disco "<pid>"
// es la relacion binaria S-O, "<pid>s" la lista unaria de S y "<pid>o" la de O.

namespace gdj {

    template<class SeQuadT>
    class resource_manager {
    private:
        std::map<uint64_t, std::unique_ptr<SeQuadT>> so_map;
        std::map<uint64_t, std::unique_ptr<SeQuadT>> s_map;
        std::map<uint64_t, std::unique_ptr<SeQuadT>> o_map;

        uint64_t extract_id(const std::string& filename) {
            size_t last_slash = filename.find_last_of("/\\");
            std::string name = (last_slash == std::string::npos) ? filename : filename.substr(last_slash + 1);
            size_t last_digit = name.find_last_not_of("0123456789");
            std::string base = (last_digit == std::string::npos) ? name : name.substr(0, last_digit + 1);
            if (!base.empty() && (base.back() == 's' || base.back() == 'o'))
                base.pop_back();
            try { return std::stoull(base); } catch (...) { return 0; }
        }

    public:
        void load_directory(const std::string& path) {
            DIR* dir = opendir(path.c_str());
            if (!dir)
                return;
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                std::string fname = entry->d_name;
                if (fname == "." || fname == "..")
                    continue;

                uint64_t id = extract_id(fname);
                auto qt = std::make_unique<SeQuadT>();
                std::ifstream f(path + "/" + fname, std::ios::binary);
                if (f.is_open() && qt->load(f)) {
                    if (fname.back() == 's')
                        s_map[id] = std::move(qt);
                    else if (fname.back() == 'o')
                        o_map[id] = std::move(qt);
                    else
                        so_map[id] = std::move(qt);
                }
            }
            closedir(dir);
        }

        SeQuadT* get_so(uint64_t id) { return so_map.count(id) ? so_map[id].get() : nullptr; }
        SeQuadT* get_s(uint64_t id)  { return s_map.count(id)  ? s_map[id].get()  : nullptr; }
        SeQuadT* get_o(uint64_t id)  { return o_map.count(id)  ? o_map[id].get()  : nullptr; }

        void print_summary() const {
            std::cout << "Resource Summary: " << std::endl;
            std::cout << "QDAGs - S: " << s_map.size()
                      << " | O: " << o_map.size()
                      << " | SO: " << so_map.size() << std::endl;
        }
    };

}

#endif // QDAGS_RESOURCE_MANAGER_HPP
