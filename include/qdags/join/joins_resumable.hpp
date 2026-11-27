#ifndef QDAGS_JOINS_RESUMABLE_HPP
#define QDAGS_JOINS_RESUMABLE_HPP

#include <cstdint>
#include <limits>
#include <map>
#include <vector>

#include "joins.hpp"

// Join resumable: iterador que pausa sin perder su posicion en el arbol y pide "N resultados mas" sin recalcular
// el prefijo (en vez de reintentar desde cero con otro limite). Solo secuencial y camino denso (<=6 atributos).
//
// ResumableAND reescribe la recursion de AND (joins.hpp) como maquina de estados con un stack explicito de
// ANDFrame; pausar es no vaciar el stack. No chequea upper_bound: corta quien llama a next(budget)
// (ver strategy/generic_strategies_resumable.hpp).
//
// build_result() construye el resultado parcial con materialize_snapshot(), que cierra provisionalmente los
// frames abiertos solo en una copia de bv_/last_pos_.

namespace gdj {

    // Estado de un nivel de la recursion de AND. children_to_recurse[] se calcula al crear el frame; entre
    // llamadas a next() solo avanza `i` (y, en una hoja, los hijos ya empujados a bv[]).
    struct ANDFrame {
        uint16_t cur_level;
        bool is_leaf;
        uint64_t roots[16];              // posicion actual de cada QDAG en este nivel
        uint64_t rank_vector[16][64];    // solo usado si !is_leaf
        uint64_t k_d[16];                // solo usado si !is_leaf
        uint16_t children_to_recurse[512];
        uint64_t children_to_recurse_size;
        uint64_t i;                       // indice del proximo hijo a procesar
        int64_t last_child;
        bool produced_any;                 // equivalente a !just_zeroes en AND
    };

    // AND como iterador pausable. next() repetido hasta exhausted() produce el mismo bv[]/last_pos[] que AND sin
    // cota; con budgets finitos, el mismo que AND detenido en ese punto (orden MSB-primero).
    template<class QdagT>
    class ResumableAND {
    public:
        ResumableAND(std::vector<QdagT*>& Q, uint16_t max_level,
                     std::vector<uint64_t>* bv, uint64_t* last_pos)
            : Q_(Q), nQ_((uint16_t)Q.size()), max_level_(max_level), bv_(bv), last_pos_(last_pos) {
            p_ = Q_[0]->nChildren();
            uint64_t root0[16];
            for (uint16_t i = 0; i < nQ_; ++i)
                root0[i] = 0;
            push_frame(0, root0);
        }

        // Avanza hasta producir `budget` resultados nuevos o agotar el arbol; cada llamada retoma donde quedo la
        // anterior. Retorna cuantos produjo.
        uint64_t next(uint64_t budget) {
            uint64_t produced = 0;
            while (!stack_.empty()) {
                if (produced >= budget || timeout_triggered.load(std::memory_order_relaxed))
                    break;

                ANDFrame& f = stack_.back();

                if (f.i >= f.children_to_recurse_size) {
                    finalize_and_pop(f);
                    continue;
                }

                if (f.is_leaf) {
                    while (f.i < f.children_to_recurse_size && produced < budget) {
                        uint16_t child = f.children_to_recurse[f.i];
                        if ((int64_t)child - f.last_child > 1)
                            last_pos_[f.cur_level] += (uint64_t)(child - f.last_child - 1);
                        f.last_child = child;
                        bv_[f.cur_level].push_back(last_pos_[f.cur_level]++);
                        f.produced_any = true;
                        ++produced;
                        ++f.i;
                    }
                    // Si se agoto (f.i == size), se finaliza en la proxima vuelta del while.
                } else {
                    uint16_t child = f.children_to_recurse[f.i];
                    uint64_t root_temp[16];
                    for (uint16_t j = 0; j < nQ_; ++j)
                        root_temp[j] = f.k_d[j] * (f.rank_vector[j][Q_[j]->getM(child)] - 1);

                    if ((int64_t)child - f.last_child > 1)
                        last_pos_[f.cur_level] += (uint64_t)(child - f.last_child - 1);
                    f.last_child = child;

                    // f.i avanza al popear el frame hijo (finalize_and_pop), como AND tras el retorno recursivo.
                    push_frame(f.cur_level + 1, root_temp);
                }
            }
            return produced;
        }

        bool exhausted() const { return stack_.empty(); }

        // Foto del estado acumulado: copia bv_/last_pos_ y cierra provisionalmente los frames abiertos (como
        // finalize_and_pop, pero en las copias). El estado real queda intacto y next() puede seguir.
        void materialize_snapshot(std::vector<uint64_t>* bv_out, uint64_t* last_pos_out) const {
            for (uint16_t i = 0; i <= max_level_; ++i) {
                bv_out[i] = bv_[i];
                last_pos_out[i] = last_pos_[i];
            }
            bool child_produced = false;
            for (int idx = (int)stack_.size() - 1; idx >= 0; --idx) {
                const ANDFrame& f = stack_[idx];
                bool produced_any = f.produced_any || child_produced;
                if ((int64_t)p_ - f.last_child > 1)
                    last_pos_out[f.cur_level] += (uint64_t)(p_ - f.last_child - 1);
                if (idx > 0) {
                    uint16_t parent_level = stack_[idx - 1].cur_level;
                    if (produced_any) {
                        bv_out[parent_level].push_back(last_pos_out[parent_level]++);
                    } else {
                        last_pos_out[f.cur_level] -= p_;
                        last_pos_out[parent_level]++;
                    }
                }
                child_produced = produced_any;
            }
        }

    private:
        std::vector<QdagT*>& Q_;
        uint16_t nQ_;
        uint16_t max_level_;
        std::vector<uint64_t>* bv_;
        uint64_t* last_pos_;
        uint64_t p_;
        std::vector<ANDFrame> stack_;

        void push_frame(uint16_t level, uint64_t* roots) {
            stack_.emplace_back();
            ANDFrame& f = stack_.back();
            f.cur_level = level;
            f.is_leaf = (level == max_level_);
            for (uint16_t i = 0; i < nQ_; ++i)
                f.roots[i] = roots[i];
            f.i = 0;
            f.last_child = -1;
            f.produced_any = false;

            uint64_t children = 0xFFFFFFFFFFFFFFFFULL;
            if (f.is_leaf) {
                for (uint16_t i = 0; i < nQ_ && children; ++i)
                    children &= Q_[i]->materialize_node_64_last(level, f.roots[i]);
            } else {
                for (uint16_t i = 0; i < nQ_ && children; ++i) {
                    f.k_d[i] = Q_[i]->getKD();
                    children &= Q_[i]->materialize_node_64(level, f.roots[i], f.rank_vector[i]);
                }
            }

            f.children_to_recurse_size = sdsl::bits::cnt((uint64_t)children);
            uint64_t idx = 0;
            uint64_t msb;
            while (idx < f.children_to_recurse_size) {
                msb = __builtin_clzll(children);
                f.children_to_recurse[idx] = (uint16_t)msb;
                ++idx;
                children &= (((uint64_t)0xFFFFFFFFFFFFFFFFULL) >> (msb + 1));
            }
        }

        void finalize_and_pop(ANDFrame& f) {
            if ((int64_t)p_ - f.last_child > 1)
                last_pos_[f.cur_level] += (uint64_t)(p_ - f.last_child - 1);

            bool produced_any = f.produced_any;
            uint16_t level = f.cur_level;
            stack_.pop_back();

            if (!stack_.empty()) {
                ANDFrame& parent = stack_.back();
                if (produced_any) {
                    bv_[parent.cur_level].push_back(last_pos_[parent.cur_level]++);
                    parent.produced_any = true;
                } else {
                    last_pos_[level] -= p_;
                    last_pos_[parent.cur_level]++;
                }
                ++parent.i;
            }
        }
    };

    // Posee bv[]/last_pos[] y el ResumableAND que los llena; build_result() arma un SeQuadT con lo acumulado.
    // Q se guarda por referencia: debe seguir vivo mientras exista el ResumableJoin.
    template<class QdagT, class SeQuadT>
    class ResumableJoin {
    public:
        explicit ResumableJoin(std::vector<QdagT*>& Q)
            : Q_(Q), height_((uint16_t)Q[0]->getHeight()), k_(Q[0]->getK()) {
            std::map<uint64_t, uint8_t> attr_map;
            for (auto* q : Q_) {
                uint64_t nAttr = q->nAttr();
                for (uint64_t j = 0; j < nAttr; j++)
                    attr_map[q->getAttr(j)] = 1;
            }
            for (auto& kv : attr_map)
                A_.push_back(kv.first);

            bv_ = new std::vector<uint64_t>[height_];
            last_pos_ = new uint64_t[height_]();
            and_ = new ResumableAND<QdagT>(Q_, (uint16_t)(height_ - 1), bv_, last_pos_);
        }

        ~ResumableJoin() {
            delete and_;
            delete[] bv_;
            delete[] last_pos_;
        }

        ResumableJoin(const ResumableJoin&) = delete;
        ResumableJoin& operator=(const ResumableJoin&) = delete;

        uint64_t next(uint64_t budget) { return and_->next(budget); }
        bool exhausted() const { return and_->exhausted(); }
        uint64_t total_results() const { return bv_[height_ - 1].size(); }

        // SeQuadT nuevo (ownership del caller) con lo acumulado hasta ahora; no altera el iterador.
        SeQuadT* build_result() const {
            std::vector<uint64_t>* bv_snap = new std::vector<uint64_t>[height_];
            uint64_t* last_pos_snap = new uint64_t[height_];
            and_->materialize_snapshot(bv_snap, last_pos_snap);
            auto* result = new SeQuadT(bv_snap, k_, (uint8_t)A_.size(), height_);
            delete[] bv_snap;
            delete[] last_pos_snap;
            return result;
        }

    private:
        std::vector<QdagT*>& Q_;
        uint16_t height_;
        uint8_t k_;
        att_set A_;
        std::vector<uint64_t>* bv_;
        uint64_t* last_pos_;
        ResumableAND<QdagT>* and_;
    };

    // "Sin cota" para next(): no 0, que significa "no pidas nada".
    inline uint64_t unbounded_budget() { return std::numeric_limits<uint64_t>::max(); }

} // namespace gdj

#endif // QDAGS_JOINS_RESUMABLE_HPP
