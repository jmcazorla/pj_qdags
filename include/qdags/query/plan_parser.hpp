#ifndef QDAGS_QUERY_PLAN_PARSER_HPP
#define QDAGS_QUERY_PLAN_PARSER_HPP

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <map>
#include <regex>
#include <algorithm>

// Parser del DSL de planes (prejoins + join final) de una topologia; no depende del motor. Cada bloque, separado
// por lineas en blanco, es un plan: la primera linea es el plan_id, la segunda se ignora y las siguientes son
// "{vars_salida}: op1, op2, ..." con op = table{arista} | projection({arista},{vars}) | prejoin{vars_del_paso_previo}.

namespace gdj {

    // Tipos de variables en el ciclo de hasta 5 nodos (a..e).
    enum class VarType {
        A, B, C, D, E, NONE
    };

    inline std::ostream& operator<<(std::ostream& os, const VarType& v) {
        switch (v) {
            case VarType::A:
                os << "a";
                break;
            case VarType::B:
                os << "b";
                break;
            case VarType::C:
                os << "c";
                break;
            case VarType::D:
                os << "d";
                break;
            case VarType::E:
                os << "e";
                break;
            default:
                os << "none";
                break;
        }
        return os;
    }

    // Tipos de aristas/tablas (independiente del orden de las variables).
    enum class EdgeType {
        AB, BC, CD, DA, CA, DE, AE, NONE,
    };

    inline std::ostream& operator<<(std::ostream& os, const EdgeType& e) {
        switch (e) {
            case EdgeType::AB:
                os << "ab";
                break;
            case EdgeType::BC:
                os << "bc";
                break;
            case EdgeType::CD:
                os << "cd";
                break;
            case EdgeType::DA:
                os << "da";
                break; // para estrellas es AD.
            case EdgeType::CA:
                os << "ca";
                break; // para estrellas es AC.
            case EdgeType::DE:
                os << "de";
                break;
            case EdgeType::AE:
                os << "ae";
                break;
            default:
                os << "none";
                break;
        }
        return os;
    }

    // Tipos de operaciones soportadas en el DSL del plan.
    enum class OpType {
        PROJECTION,
        TABLE,
        PREJOIN
    };

    inline std::ostream& operator<<(std::ostream& os, const OpType& t) {
        switch (t) {
            case OpType::PROJECTION:
                os << "PROJECTION";
                break;
            case OpType::TABLE:
                os << "TABLE";
                break;
            case OpType::PREJOIN:
                os << "PREJOIN";
                break;
        }
        return os;
    }

    // Representa una operacion individual.
    struct plan_operation {
        OpType type;
        EdgeType source_edge = EdgeType::NONE;    // Para TABLE y PROJECTION
        std::vector<VarType> target_vars;         // Variables proyectadas
        std::vector<VarType> ref_vars;            // Para prejoins: variables del resultado previo
        std::string ref_prejoin_id;               // ID del prejoin del que se refieren las variables
    };

    // Representa una linea del plan (un prejoin o el join final).
    struct execution_step {
        std::string step_id;                      // Representacion textual del lado izquierdo
        std::vector<VarType> output_vars;         // Lado izquierdo del ":" parseado
        std::vector<plan_operation> operations;    // Lado derecho (lista de ops)
    };

    // Representa el plan completo de un bloque.
    struct execution_plan {
        std::string plan_id;
        std::vector<execution_step> steps;
    };


    class PlanParser {
    public:
        static std::map<std::string, execution_plan> parseFile(const std::string& filename) {
            std::map<std::string, execution_plan> plans;
            std::ifstream file(filename);
            if (!file.is_open())
                return plans;

            std::string line;
            std::vector<std::string> current_block;

            while (std::getline(file, line)) {
                if (line.empty() || line.find_first_not_of(" \t\n\r") == std::string::npos) {
                    if (!current_block.empty()) {
                        execution_plan plan = parseBlock(current_block);
                        plans[plan.plan_id] = plan;
                        current_block.clear();
                    }
                } else {
                    current_block.push_back(line);
                }
            }
            if (!current_block.empty()) {
                execution_plan plan = parseBlock(current_block);
                plans[plan.plan_id] = plan;
            }
            return plans;
        }

        // Imprime un plan de ejecucion por consola para depuracion.
        static void print(const execution_plan& plan) {
            std::cout << "Plan ID: " << plan.plan_id << std::endl;
            std::cout << "Execution plan:" << std::endl;
            for (const auto& step : plan.steps) {
                std::cout << "  {";
                for (size_t i = 0; i < step.output_vars.size(); ++i) {
                    std::cout << step.output_vars[i] << (i == step.output_vars.size() - 1 ? "" : ",");
                }
                std::cout << "}: ";

                for (size_t i = 0; i < step.operations.size(); ++i) {
                    const auto& op = step.operations[i];
                    switch (op.type) {
                        case OpType::PROJECTION:
                            std::cout << "projection({" << op.source_edge << "},{";
                            for (size_t j = 0; j < op.target_vars.size(); ++j)
                                std::cout << op.target_vars[j] << (j == op.target_vars.size() - 1 ? "" : ",");
                            std::cout << "})";
                            break;
                        case OpType::TABLE:
                            std::cout << "table {" << op.source_edge << "}";
                            break;
                        case OpType::PREJOIN:
                            std::cout << "prejoin {";
                            for (size_t j = 0; j < op.ref_vars.size(); ++j)
                                std::cout << op.ref_vars[j] << (j == op.ref_vars.size() - 1 ? "" : ",");
                            std::cout << "}";
                            break;
                    }
                    if (i < step.operations.size() - 1)
                        std::cout << ", ";
                }
                std::cout << std::endl;
            }
            std::cout << "------------------------------------------" << std::endl;
        }

        static void printAll(const std::vector<execution_plan>& plans) {
            for (const auto& plan : plans)
                print(plan);
        }

    private:
        static VarType toVarType(std::string s) {
            s = trim(s);
            if (s == "a")
                return VarType::A;
            if (s == "b")
                return VarType::B;
            if (s == "c")
                return VarType::C;
            if (s == "d")
                return VarType::D;
            if (s == "e")
                return VarType::E;
            return VarType::NONE;
        }

        static std::vector<VarType> toVarList(const std::vector<std::string>& list) {
            std::vector<VarType> res;
            for (const auto& s : list)
                res.push_back(toVarType(s));
            return res;
        }

        static EdgeType toEdgeType(std::vector<std::string> vars) {
            for (auto& v : vars)
                v = trim(v);
            std::sort(vars.begin(), vars.end());

            if (vars.size() != 2)
                return EdgeType::NONE;

            if (vars[0] == "a" && vars[1] == "b")
                return EdgeType::AB;
            if (vars[0] == "b" && vars[1] == "c")
                return EdgeType::BC;
            if (vars[0] == "c" && vars[1] == "d")
                return EdgeType::CD;
            if (vars[0] == "a" && vars[1] == "d")
                return EdgeType::DA;
            if (vars[0] == "a" && vars[1] == "c")
                return EdgeType::CA;
            if (vars[0] == "d" && vars[1] == "e")
                return EdgeType::DE;
            if (vars[0] == "a" && vars[1] == "e")
                return EdgeType::AE;

            return EdgeType::NONE;
        }

        static execution_plan parseBlock(const std::vector<std::string>& lines) {
            execution_plan plan;
            if (lines.size() < 3)
                return plan;

            plan.plan_id = lines[0];

            for (size_t i = 2; i < lines.size(); ++i) {
                if (lines[i].empty())
                    continue;
                plan.steps.push_back(parseStep(lines[i]));
            }

            return plan;
        }

        static execution_step parseStep(const std::string& line) {
            execution_step step;
            size_t colon_pos = line.find(':');
            if (colon_pos == std::string::npos)
                return step;

            std::string left = line.substr(0, colon_pos);
            step.step_id = trim(left);
            step.output_vars = toVarList(split(extractSingleBracketContent(left), ','));

            std::string right = line.substr(colon_pos + 1);
            std::vector<std::string> op_strings = splitOperations(right);

            for (const auto& op_str : op_strings) {
                if (trim(op_str).empty())
                    continue;
                step.operations.push_back(parseOperation(op_str));
            }

            return step;
        }

        static plan_operation parseOperation(std::string op_str) {
            plan_operation op;
            op_str = trim(op_str);

            if (op_str.find("projection") != std::string::npos) {
                op.type = OpType::PROJECTION;
                std::regex proj_reg("projection\\s*\\(\\{([^\\}]+)\\}\\s*,\\s*\\{([^\\}]+)\\}\\)");
                std::smatch match;
                if (std::regex_search(op_str, match, proj_reg)) {
                    op.source_edge = toEdgeType(split(match[1], ','));
                    op.target_vars = toVarList(split(match[2], ','));
                }
            }
            else if (op_str.find("table") != std::string::npos) {
                op.type = OpType::TABLE;
                std::regex table_reg("table\\s*\\{([^\\}]+)\\}");
                std::smatch match;
                if (std::regex_search(op_str, match, table_reg)) {
                    op.source_edge = toEdgeType(split(match[1], ','));
                }
            }
            else if (op_str.find("prejoin") != std::string::npos) {
                op.type = OpType::PREJOIN;
                std::regex pre_reg("prejoin\\s*\\{([^\\}]+)\\}");
                std::smatch match;
                if (std::regex_search(op_str, match, pre_reg)) {
                    op.ref_prejoin_id = "{" + trim(match[1]) + "}";
                    op.ref_vars = toVarList(split(match[1], ','));
                }
            }
            return op;
        }

        static std::vector<std::string> splitOperations(const std::string& s) {
            std::vector<std::string> res;
            int paren_level = 0;
            std::string current;
            for (char c : s) {
                if (c == '(' || c == '{')
                    paren_level++;
                if (c == ')' || c == '}')
                    paren_level--;
                if (c == ',' && paren_level == 0) {
                    res.push_back(current);
                    current.clear();
                } else {
                    current += c;
                }
            }
            if (!current.empty())
                res.push_back(current);
            return res;
        }

        static std::vector<std::string> split(const std::string& s, char delim) {
            std::vector<std::string> tokens;
            std::string token;
            std::istringstream tokenStream(s);
            while (std::getline(tokenStream, token, delim))
                tokens.push_back(trim(token));
            return tokens;
        }

        static std::string extractSingleBracketContent(const std::string& str) {
            std::regex reg("\\{([^\\}]+)\\}");
            std::smatch match;
            if (std::regex_search(str, match, reg))
                return match[1].str();
            return "";
        }

        static std::string trim(const std::string& s) {
            auto start = s.find_first_not_of(" \t\n\r");
            if (start == std::string::npos)
                return "";
            auto end = s.find_last_not_of(" \t\n\r");
            return s.substr(start, end - start + 1);
        }
    };

} // namespace gdj

#endif // QDAGS_QUERY_PLAN_PARSER_HPP
