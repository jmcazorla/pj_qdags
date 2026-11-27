#ifndef QDAGS_INDEX_QDAG_PIN_HPP
#define QDAGS_INDEX_QDAG_PIN_HPP

#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "commun.hpp"
#include "qdag.hpp"
#include "se_quadtree.hpp"

// Patrones con sujeto u objeto constante: (?x, p, c) y (c, p, ?y).
//
// qdag_pin<RankBackend> es un qdag que puede representar relaciones binarias con un atributo fijo (sujeto u objeto).
// Se construye a partir de un qdag<RankBackend> real y un atributo fijo, o bien a partir de una lista de aristas binarias con un atributo fijo.

namespace gdj {

    enum class pin_attr : uint8_t {
        subject = 0,   // (c, p, ?y): se fija el primer atributo de la base
        object  = 1    // (?x, p, c): se fija el segundo
    };

    // registro ficha -> (relacion real, atributo fijado, valor).
    template<class RankBackend>
    class pin_registry {
    public:
        using SeQuadT = se_quadtree<RankBackend>;

        struct entry {
            SeQuadT* real = nullptr;
            pin_attr attr = pin_attr::object;
            uint64_t value = 0;
        };

        static void add(const SeQuadT* token, const entry& e) {
            std::lock_guard<std::mutex> lock(mtx_());
            map_()[token] = e;
        }

        static void remove(const SeQuadT* token) {
            std::lock_guard<std::mutex> lock(mtx_());
            map_().erase(token);
        }

        static bool find(const SeQuadT* token, entry& out) {
            std::lock_guard<std::mutex> lock(mtx_());
            auto it = map_().find(token);
            if (it == map_().end())
                return false;
            out = it->second;
            return true;
        }
        static void clear() {
            std::lock_guard<std::mutex> lock(mtx_());
            map_().clear();
        }

        static bool empty() {
            std::lock_guard<std::mutex> lock(mtx_());
            return map_().empty();
        }

        static size_t size() {
            std::lock_guard<std::mutex> lock(mtx_());
            return map_().size();
        }

    private:
        static std::mutex& mtx_() { 
            static std::mutex m; 
            return m; 
        }
        static std::unordered_map<const SeQuadT*, entry>& map_() {
            static std::unordered_map<const SeQuadT*, entry> m;
            return m;
        }
    };


    template<class RankBackend>
    class pin_scope {
    public:
        using SeQuadT = se_quadtree<RankBackend>;

        pin_scope() = default;
        pin_scope(const pin_scope&) = delete;
        pin_scope& operator=(const pin_scope&) = delete;
        pin_scope(pin_scope&& o) noexcept : tokens_(std::move(o.tokens_)) {
            o.tokens_.clear();
        }

        pin_scope& operator=(pin_scope&& o) noexcept {
            if (this != &o) {
                release_();
                tokens_ = std::move(o.tokens_);
                o.tokens_.clear();
            }
            return *this;
        }

        ~pin_scope() {
            release_();
        }

        SeQuadT* make(SeQuadT* real, pin_attr attr, uint64_t value) {
            if (real == nullptr)
                throw std::invalid_argument("pin_scope::make(): null relation.");
            if (real->getD() != 2 || real->getK() != 2)
                throw std::invalid_argument("pin_scope::make(): only binary relations with k=2 can be pinned.");

            const uint16_t h = real->getHeight();
            const uint64_t side = h >= 63 ? (1ULL << 63) : (1ULL << h);
            std::vector<std::vector<idx_type>> no_points;
            SeQuadT* token = new SeQuadT(no_points, side, 2, 1);
            typename pin_registry<RankBackend>::entry e;
            e.real = real;
            e.attr = attr;
            e.value = value;
            pin_registry<RankBackend>::add(token, e);
            tokens_.push_back(token);
            return token;
        }

        size_t size() const {
            return tokens_.size();
        }

    private:
        void release_() {
            for (SeQuadT* t : tokens_) {
                pin_registry<RankBackend>::remove(t);
                delete t;
            }
            tokens_.clear();
        }
        std::vector<SeQuadT*> tokens_;
    };

    template<class RankBackend>
    class qdag_pin {
    public:
        using SeQuadT = se_quadtree<RankBackend>;

    private:
        // tabla sin fijar: todo se delega aca.
        std::optional<qdag<RankBackend>> inner_;

        // ---- solo para una tabla fijada ----
        bool pinned_ = false;
        SeQuadT* Q_ = nullptr;        // relacion binaria real
        bool pin_object_ = true;      // true: fijado el 2do atributo; false: el 1ro
        uint64_t pin_value_ = 0;
        bool pin_empty_ = false;
        uint16_t height_ = 0;
        att_set q_att_set_;
        uint64_t grid_side_ = 0;
        uint16_t Msize_ = 0;
        std::vector<type_mapping_M> M_;

        uint64_t tab64_[4] = {0, 0, 0, 0};
        std::vector<gdj::UInt256>  tab256_;
        std::vector<gdj::UInt512>  tab512_;
        std::vector<gdj::UInt1024> tab1024_;


        template<class U>
        void build_wide_(std::vector<U>& tab, uint64_t W) {
            tab.assign(4, U());
            for (unsigned v = 0; v < 4; ++v)
                for (uint64_t j = 0; j < Msize_; ++j)
                    if ((v >> M_[j]) & 1u)
                        set_bit(tab[v], (W - 1) - j);
        }

        void require_pinned_tier_(bool ok, const char* who) const {
            if (!ok)
                throw std::runtime_error(std::string("qdag_pin::") + who + "(): wrong tier for this qdag's dimension.");
        }

        // Digito de la constante en `level` (el nivel 0 es el bit mas significativo).
        inline unsigned digit_(uint64_t level) const {
            return (unsigned)((pin_value_ >> (height_ - 1 - level)) & 1ULL);
        }

         inline unsigned real_child_(unsigned v, unsigned delta) const {
            return pin_object_ ? ((v << 1) | delta) : ((delta << 1) | v);
        }

        inline unsigned pin_node_(uint64_t level, uint64_t node, uint64_t* rank_vector) const {
            if (pin_empty_)
                return 0;
            const unsigned nd = Q_->get_node_last_level((uint16_t)level, node);
            const uint64_t r = Q_->rank((uint16_t)level, node);
            const unsigned delta = digit_(level);
            const unsigned b0 = real_child_(0, delta), b1 = real_child_(1, delta);
            unsigned v = 0;
            if (nd & (1u << b0)) {
                v |= 1u; rank_vector[0] = r + (uint64_t)__builtin_popcount(nd & ((2u << b0) - 1u));
            }
            if (nd & (1u << b1)) {
                v |= 2u; rank_vector[1] = r + (uint64_t)__builtin_popcount(nd & ((2u << b1) - 1u));
            }

            return v;
        }

        //virtual: devuelve el nodo de salida del nivel `level` y nodo `node` de la relacion real,
        // pero solo los hijos que coinciden con el bit de la constante.
        inline unsigned pin_node_last_(uint64_t level, uint64_t node) const {
            if (pin_empty_)
                return 0;
            const unsigned nd = Q_->get_node_last_level((uint16_t)level, node);
            const unsigned delta = digit_(level);
            return ((nd >> real_child_(0, delta)) & 1u) | (((nd >> real_child_(1, delta)) & 1u) << 1);
        }

    public:
        qdag_pin() = default;
        qdag_pin(const qdag_pin&) = delete;
        qdag_pin& operator=(const qdag_pin&) = delete;

        qdag_pin(SeQuadT* q, att_set& base, const uint64_t grid_side, att_set ext,
                 const wide_base_config& wcfg = wide_base_config()) {
            typename pin_registry<RankBackend>::entry e;
            if (!pin_registry<RankBackend>::find(q, e)) {
                inner_.emplace(q, base, grid_side, ext, wcfg);
                return;
            }

            if (base.size() != 1)
                throw std::invalid_argument("qdag_pin: a pinned table has a single free attribute.");
            if (e.real == nullptr || e.real->getD() != 2 || e.real->getK() != 2)
                throw std::invalid_argument("qdag_pin: only binary relations with k=2 can be pinned.");

            pinned_ = true;
            Q_ = e.real;
            pin_object_ = (e.attr == pin_attr::object);
            pin_value_ = e.value;
            height_ = Q_->getHeight();
            pin_empty_ = height_ < 64 && (pin_value_ >> height_) != 0;
            q_att_set_ = ext;
            grid_side_ = grid_side;

            const uint16_t dim = (uint16_t)ext.size();
            if (dim < 1 || dim > 10 || base[0] >= dim)
                throw std::invalid_argument("qdag_pin: extended space outside [1,10] or attribute outside the space.");
            Msize_ = (uint16_t)(1u << dim);

            // hijo virtual de cada hijo de salida
            M_.resize(Msize_);
            for (uint64_t i = 0; i < Msize_; ++i)
                M_[i] = (type_mapping_M)((i >> (dim - base[0] - 1)) & 1ULL);

            if (dim <= 6) {
                for (unsigned v = 0; v < 4; ++v) {
                    uint64_t x = 0;
                    for (uint64_t j = 0; j < Msize_; ++j)
                        if ((v >> M_[j]) & 1u)
                            x |= 1ULL << (63 - j);
                    tab64_[v] = x;
                }
            } else if (dim <= 8) {
                build_wide_(tab256_, 256);
            } else if (dim == 9) {
                build_wide_(tab512_, 512);
            } else {
                build_wide_(tab1024_, 1024);
            }
        }

        bool isPinned() const { 
            return pinned_; 
        }

        uint64_t size_in_bytes() const {
            if (!pinned_)
                return inner_->size_in_bytes();

            return Q_->size_in_bytes() + (uint64_t)M_.size() * sizeof(type_mapping_M) + sizeof(*this)
                   + (uint64_t)tab256_.size() * sizeof(gdj::UInt256) + (uint64_t)tab512_.size() * sizeof(gdj::UInt512)
                   + (uint64_t)tab1024_.size() * sizeof(gdj::UInt1024);
        }

        uint64_t nAttr() const {
                return pinned_ ? q_att_set_.size() : inner_->nAttr();
        }

        uint64_t getAttr(uint64_t i) const {
            if (!pinned_)
                return inner_->getAttr(i);
            if (i >= q_att_set_.size())
                throw std::out_of_range("qdag_pin::getAttr(): Index out of bounds for q_att_set.");
            
                return q_att_set_[i];
        }

        uint64_t getGridSide() const {
            return pinned_ ? grid_side_ : inner_->getGridSide();
        }

        uint64_t getHeight() const {
            return pinned_ ? height_ : inner_->getHeight();
        }

        uint8_t getK() const {
            return pinned_ ? Q_->getK() : inner_->getK();
        }

        uint16_t nChildren() const {
            return pinned_ ? Msize_ : inner_->nChildren();
        }
        // de la relacion REAL: se usa para ubicar el hijo en el nivel siguiente (getKD() * (rango - 1)).
        uint64_t getKD() const {
            return pinned_ ? Q_->getKD() : inner_->getKD();
        }

        uint8_t getBaseD() const {
            return pinned_ ? (uint8_t)1 : inner_->getBaseD();
        }

        bool usesScatter() const {
            return pinned_ ? false : inner_->usesScatter();
        }

        uint16_t getM(uint16_t i) const {
            if (!pinned_)
                return inner_->getM(i);
            if (i >= Msize_)
                throw std::out_of_range("qdag_pin::getM(): Index out of bounds for M.");
            return M_[i];
        }

        SeQuadT* getQ() const {
            return pinned_ ? Q_ : inner_->getQ();
        }


        inline uint64_t materialize_node_64(uint64_t level, uint64_t node, uint64_t* rank_vector) {
            if (!pinned_)
                return inner_->materialize_node_64(level, node, rank_vector);
            return tab64_[pin_node_(level, node, rank_vector)];
        }
        
        inline uint64_t materialize_node_64_last(uint64_t level, uint64_t node) {
            if (!pinned_)
                return inner_->materialize_node_64_last(level, node);
            return tab64_[pin_node_last_(level, node)];
        }

        inline gdj::UInt256 materialize_node_256(uint64_t level, uint64_t node, uint64_t* rank_vector) {
            if (!pinned_)
                return inner_->materialize_node_256(level, node, rank_vector);

            require_pinned_tier_(!tab256_.empty(), "materialize_node_256");
            return tab256_[pin_node_(level, node, rank_vector)];
        }
        inline gdj::UInt256 materialize_node_256_last(uint64_t level, uint64_t node) {
            if (!pinned_)
                return inner_->materialize_node_256_last(level, node);

            require_pinned_tier_(!tab256_.empty(), "materialize_node_256_last");
            return tab256_[pin_node_last_(level, node)];
        }

        inline gdj::UInt512 materialize_node_512(uint64_t level, uint64_t node, uint64_t* rank_vector) {
            if (!pinned_)
                return inner_->materialize_node_512(level, node, rank_vector);
            require_pinned_tier_(!tab512_.empty(), "materialize_node_512");
            return tab512_[pin_node_(level, node, rank_vector)];
        }
        inline gdj::UInt512 materialize_node_512_last(uint64_t level, uint64_t node) {
            if (!pinned_)
                return inner_->materialize_node_512_last(level, node);
            require_pinned_tier_(!tab512_.empty(), "materialize_node_512_last");
            return tab512_[pin_node_last_(level, node)];
        }

        inline gdj::UInt1024 materialize_node_1024(uint64_t level, uint64_t node, uint64_t* rank_vector) {
            if (!pinned_)
                return inner_->materialize_node_1024(level, node, rank_vector);
            require_pinned_tier_(!tab1024_.empty(), "materialize_node_1024");
            return tab1024_[pin_node_(level, node, rank_vector)];
        }
        inline gdj::UInt1024 materialize_node_1024_last(uint64_t level, uint64_t node) {
            if (!pinned_)
                return inner_->materialize_node_1024_last(level, node);
            require_pinned_tier_(!tab1024_.empty(), "materialize_node_1024_last");
            return tab1024_[pin_node_last_(level, node)];
        }



        template<class U>
        inline U materialize_scatter_last(uint64_t level, uint64_t node) {
            if (pinned_)
                throw std::logic_error("qdag_pin::materialize_scatter_last(): a pinned table does not use scatter.");
            return inner_->template materialize_scatter_last<U>(level, node);
        }
        template<class U>
        inline U materialize_scatter_lazy(uint64_t level, uint64_t node, BaseNode512& bn) {
            if (pinned_)
                throw std::logic_error("qdag_pin::materialize_scatter_lazy(): a pinned table does not use scatter.");
            return inner_->template materialize_scatter_lazy<U>(level, node, bn);
        }
        static inline uint64_t lazy_child_rank(const BaseNode512& bn, uint16_t base_child) {
            return qdag<RankBackend>::lazy_child_rank(bn, base_child);
        }
        void enable_scatter() {
            if (pinned_)
                throw std::logic_error("qdag_pin::enable_scatter(): a pinned table does not use scatter.");
            inner_->enable_scatter();
        }
    };

}
#endif // QDAGS_INDEX_QDAG_PIN_HPP
