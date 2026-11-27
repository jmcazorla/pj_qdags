#ifndef QDAGS_JOINS_RESUMABLE_WIDE_HPP
#define QDAGS_JOINS_RESUMABLE_WIDE_HPP

#include <cstdint>
#include <vector>
#include <map>
#include <stdexcept>

#include "joins_resumable.hpp"
#include "joins_wide.hpp"

// Motor resumable ancho: lo que joins_wide.hpp es a joins.hpp, para joins_resumable.hpp. ResumableAND/
// ResumableJoin llegan a 6 atributos; estas versiones replican su logica con gdj::UInt256 children y los
// helpers de bit-scan de joins_wide.hpp. Solo secuencial y solo 7-8 atributos (sin escalones 512/1024).

namespace gdj {

    // Espejo de ANDFrame con rank_vector de [16][256].
    struct ANDFrame_wide {
        uint16_t cur_level;
        bool is_leaf;
        uint64_t roots[16];
        uint64_t rank_vector[16][256];
        uint64_t k_d[16];
        uint16_t children_to_recurse[512];
        uint64_t children_to_recurse_size;
        uint64_t i;
        int64_t last_child;
        bool produced_any;
    };

    // Espejo de ResumableAND con UInt256 children. Solo push_frame() cambia (materialize_node_256/_last).
    template<class QdagT>
    class ResumableAND_wide {
    public:
        ResumableAND_wide(std::vector<QdagT*>& Q, uint16_t max_level,
                           std::vector<uint64_t>* bv, uint64_t* last_pos)
            : Q_(Q), nQ_((uint16_t)Q.size()), max_level_(max_level), bv_(bv), last_pos_(last_pos) {
            p_ = Q_[0]->nChildren();
            uint64_t root0[16];
            for (uint16_t i = 0; i < nQ_; ++i)
                root0[i] = 0;
            push_frame(0, root0);
        }

        uint64_t next(uint64_t budget) {
            uint64_t produced = 0;
            while (!stack_.empty()) {
                if (produced >= budget || timeout_triggered.load(std::memory_order_relaxed))
                    break;

                ANDFrame_wide& f = stack_.back();

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
                } else {
                    uint16_t child = f.children_to_recurse[f.i];
                    uint64_t root_temp[16];
                    for (uint16_t j = 0; j < nQ_; ++j)
                        root_temp[j] = f.k_d[j] * (f.rank_vector[j][Q_[j]->getM(child)] - 1);

                    if ((int64_t)child - f.last_child > 1)
                        last_pos_[f.cur_level] += (uint64_t)(child - f.last_child - 1);
                    f.last_child = child;

                    push_frame(f.cur_level + 1, root_temp);
                }
            }
            return produced;
        }

        bool exhausted() const { return stack_.empty(); }

        void materialize_snapshot(std::vector<uint64_t>* bv_out, uint64_t* last_pos_out) const {
            for (uint16_t i = 0; i <= max_level_; ++i) {
                bv_out[i] = bv_[i];
                last_pos_out[i] = last_pos_[i];
            }
            bool child_produced = false;
            for (int idx = (int)stack_.size() - 1; idx >= 0; --idx) {
                const ANDFrame_wide& f = stack_[idx];
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
        std::vector<ANDFrame_wide> stack_;

        void push_frame(uint16_t level, uint64_t* roots) {
            stack_.emplace_back();
            ANDFrame_wide& f = stack_.back();
            f.cur_level = level;
            f.is_leaf = (level == max_level_);
            for (uint16_t i = 0; i < nQ_; ++i)
                f.roots[i] = roots[i];
            f.i = 0;
            f.last_child = -1;
            f.produced_any = false;

            UInt256 children = ~UInt256();
            if (f.is_leaf) {
                for (uint16_t i = 0; i < nQ_ && any_bit_256(children); ++i)
                    children &= Q_[i]->materialize_node_256_last(level, f.roots[i]);
            } else {
                for (uint16_t i = 0; i < nQ_ && any_bit_256(children); ++i) {
                    f.k_d[i] = Q_[i]->getKD();
                    children &= Q_[i]->materialize_node_256(level, f.roots[i], f.rank_vector[i]);
                }
            }

            f.children_to_recurse_size = popcount_256(children);
            uint64_t idx = 0;
            while (idx < f.children_to_recurse_size) {
                f.children_to_recurse[idx] = extract_top_bit_256(children);
                ++idx;
            }
        }

        void finalize_and_pop(ANDFrame_wide& f) {
            if ((int64_t)p_ - f.last_child > 1)
                last_pos_[f.cur_level] += (uint64_t)(p_ - f.last_child - 1);

            bool produced_any = f.produced_any;
            uint16_t level = f.cur_level;
            stack_.pop_back();

            if (!stack_.empty()) {
                ANDFrame_wide& parent = stack_.back();
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

    // Espejo de ResumableJoin con ResumableAND_wide.
    template<class QdagT, class SeQuadT>
    class ResumableJoin_wide {
    public:
        explicit ResumableJoin_wide(std::vector<QdagT*>& Q)
            : Q_(Q), height_((uint16_t)Q[0]->getHeight()), k_(Q[0]->getK()) {
            // materialize_node_256 usa tablas de 16 bits, que un qdag con dispersion no tiene.
            for (auto* q : Q_)
                if (q->usesScatter())
                    throw std::runtime_error("ResumableJoin_wide: a qdag extends by scatter, which this resumable engine does not support; use wide_base_mode::tables (--wide_base tables)");
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
            and_ = new ResumableAND_wide<QdagT>(Q_, (uint16_t)(height_ - 1), bv_, last_pos_);
        }

        ~ResumableJoin_wide() {
            delete and_;
            delete[] bv_;
            delete[] last_pos_;
        }

        ResumableJoin_wide(const ResumableJoin_wide&) = delete;
        ResumableJoin_wide& operator=(const ResumableJoin_wide&) = delete;

        uint64_t next(uint64_t budget) { return and_->next(budget); }
        bool exhausted() const { return and_->exhausted(); }
        uint64_t total_results() const { return bv_[height_ - 1].size(); }

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
        ResumableAND_wide<QdagT>* and_;
    };

    // Elige el motor como multiJoinAuto: <=6 atributos ResumableJoin, 7-8 ResumableJoin_wide, mas de 8 lanza.
    template<class QdagT, class SeQuadT>
    class ResumableJoinAuto {
    public:
        explicit ResumableJoinAuto(std::vector<QdagT*>& Q) {
            std::map<uint64_t, uint8_t> attr_map;
            for (auto* q : Q) {
                uint64_t nAttr = q->nAttr();
                for (uint64_t j = 0; j < nAttr; j++)
                    attr_map[q->getAttr(j)] = 1;
            }
            if (attr_map.size() <= 6) {
                narrow_ = new ResumableJoin<QdagT, SeQuadT>(Q);
            } else if (attr_map.size() <= 8) {
                wide_ = new ResumableJoin_wide<QdagT, SeQuadT>(Q);
            } else {
                throw std::runtime_error("ResumableJoinAuto: unsupported number of attributes (>8, no wide resumable tier for that)");
            }
        }

        ~ResumableJoinAuto() {
            delete narrow_;
            delete wide_;
        }

        ResumableJoinAuto(const ResumableJoinAuto&) = delete;
        ResumableJoinAuto& operator=(const ResumableJoinAuto&) = delete;

        uint64_t next(uint64_t budget) { return narrow_ ? narrow_->next(budget) : wide_->next(budget); }
        bool exhausted() const { return narrow_ ? narrow_->exhausted() : wide_->exhausted(); }
        uint64_t total_results() const { return narrow_ ? narrow_->total_results() : wide_->total_results(); }
        SeQuadT* build_result() const { return narrow_ ? narrow_->build_result() : wide_->build_result(); }

    private:
        ResumableJoin<QdagT, SeQuadT>* narrow_ = nullptr;
        ResumableJoin_wide<QdagT, SeQuadT>* wide_ = nullptr;
    };

} // namespace gdj

#endif // QDAGS_JOINS_RESUMABLE_WIDE_HPP
