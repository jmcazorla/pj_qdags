#ifndef QDAGS_EXAMPLES_QUERY_RUNNERS_HPP
#define QDAGS_EXAMPLES_QUERY_RUNNERS_HPP

#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

#include "qdags/index/qdag_pin.hpp"
#include "qdags/join/joins.hpp"
#include "qdags/join/joins_resumable.hpp"
#include "qdags/join/joins_resumable_wide.hpp"
#include "qdags/query/classify_query.hpp"
#include "qdags/query/leapfrog_builder.hpp"
#include "qdags/query/plan_builder.hpp"
#include "qdags/query/query_builder.hpp"
#include "qdags/query/query_builder_pin.hpp"
#include "qdags/query/resource_manager.hpp"
#include "qdags/query/triple_pattern.hpp"
#include "qdags/strategy/generic_strategies.hpp"
#include "qdags/strategy/generic_strategies_kernel.hpp"
#include "qdags/strategy/generic_strategies_leapfrog.hpp"
#include "qdags/strategy/generic_strategies_plan.hpp"
#include "qdags/strategy/generic_strategies_plan_resumable.hpp"
#include "qdags/strategy/generic_strategies_resumable.hpp"

// "Correr UNA query con la estrategia X" para run_plan_hybrid: el cuerpo por query de run_plan / run_nl / run_wk /
// run_leapfrog / run_kernel / run_orig, sin bucle de queries, archivos de salida ni resumen. Los mains por
// estrategia no lo usan; la validacion de run_plan_hybrid compara contra ellos.
// Cada runner arma la metadata, decide si la estrategia aplica y corre las variantes pedidas, cada una con su
// cronometro y timeout. El resultado final usa el backend del dataset (SeQuadTOut = SeQuadT).

namespace gdj_examples {

    using namespace gdj;

    enum class RunStatus {
        Ok,
        NotApplicable,   // la estrategia no aplica a esta query ("-" en los archivos)
        TooWide          // supera el limite del motor para esta estrategia ("+")
    };

    struct ModeOutcome {
        bool ran = false;
        bool timed_out = false;
        uint64_t result = 0;
        double ms = 0.0;
        uint32_t attempts = 0;
        uint64_t coef = 0;
        bool exhaustive = false;
    };

    struct QueryOutcome {
        RunStatus status = RunStatus::Ok;
        ModeOutcome generic, adaptive, resumable;
    };

    // Variantes de ejecucion; cada runner corre las pedidas que su estrategia soporta.
    struct RunModes {
        bool generic = false, adaptive = false, resumable = false;
        bool any() const { return generic || adaptive || resumable; }
    };

    // "generic", "adaptive", "resumable" o una lista con comas; "both" = generic+adaptive, "all" = los tres.
    // Falso si hay un token desconocido.
    inline bool parse_run_modes(const std::string &s, RunModes &out) {
        out = RunModes();
        size_t start = 0;
        while (start <= s.size()) {
            size_t end = s.find(',', start);
            std::string tok = s.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (tok == "generic")
                out.generic = true;
            else if (tok == "adaptive")
                out.adaptive = true;
            else if (tok == "resumable")
                out.resumable = true;
            else if (tok == "both") {
                out.generic = true;
                out.adaptive = true;
            }
            else if (tok == "all") {
                out.generic = out.adaptive = out.resumable = true;
            }
            else
                return false;
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        return out.any();
    }

    inline std::string run_modes_to_string(const RunModes &m) {
        std::string r;
        auto add = [&](bool on, const char *n) { if (on) r += (r.empty() ? "" : ",") + std::string(n); };
        add(m.generic, "generic"); add(m.adaptive, "adaptive"); add(m.resumable, "resumable");
        return r;
    }

    struct RunnerOptions {
        uint64_t upper_bound = 0, parallel_level = 0, grid_side = 52000000, time_out = 0;
        uint64_t coef_bound = 0, init_coef = 2;
        uint32_t growth = 2, max_retries = 15;
        GrowthMode growth_mode = GrowthMode::Fixed;
        ParallelPhases parallel_phases = ParallelPhases::LastOnly;
        bool mxm = false;
        uint8_t k = 2;
        leapfrog_config leapfrog;
        wide_base_config wide;
    };

    // Estrategias que se pueden usar como fallback, en el orden de --fallback.
    enum class Strategy { Nl, Wk, Leapfrog, Kernel, Orig };

    inline const char *strategy_name(Strategy s) {
        switch (s) {
            case Strategy::Nl:
                return "one_shot"; // nombre del paper; en el codigo es nl/NonLonely
            case Strategy::Wk:
                return "wk";
            case Strategy::Leapfrog:
                return "leapfrog";
            case Strategy::Kernel:
                return "kernel";
            default:
                return "orig";
        }
    }

    inline bool parse_strategy(const std::string &s, Strategy &out) {
        if (s == "nl") {
            out = Strategy::Nl;
            return true;
        }   // alias del nombre en el codigo
        for (Strategy c : {Strategy::Nl, Strategy::Wk, Strategy::Leapfrog, Strategy::Kernel, Strategy::Orig})
            if (s == strategy_name(c)) {
                out = c;
                return true;
            }
        return false;
    }

    // Modos que tiene cada estrategia: wk los tres (como el plan); leapfrog generic y adaptive; one_shot, kernel y
    // orig solo generic (el prejoin de One-Shot es exacto). Los pedidos que no tiene se omiten.
    inline RunModes strategy_modes(Strategy s, const RunModes &requested) {
        RunModes m = requested;
        if (s == Strategy::Leapfrog || s == Strategy::Kernel || s == Strategy::Orig || s == Strategy::Nl)
            m.resumable = false;
        if (s == Strategy::Kernel || s == Strategy::Orig || s == Strategy::Nl)
            m.adaptive = false;
        return m;
    }

    // Cronometro de una variante (watchdog de timeout + hora). stop() lee la bandera de timeout ANTES de parar el
    // watchdog (start la reinicia); lo que se libera despues de stop() queda fuera del tiempo.
    class RunTimer {
    public:
        explicit RunTimer(uint64_t time_out) {
            start_timeout_watchdog(time_out);
            t0_ = std::chrono::high_resolution_clock::now();
        }
        void stop(ModeOutcome &o) {
            auto t1 = std::chrono::high_resolution_clock::now();
            o.timed_out = timeout_triggered.load();
            stop_timeout_watchdog();
            o.ms = std::chrono::duration<double, std::milli>(t1 - t0_).count();
            o.ran = true;
        }
    private:
        std::chrono::high_resolution_clock::time_point t0_;
    };

    // Origen de las tablas de una query para las estrategias del fallback (mismas plantillas en ambos casos):
    //   plain_tables:  solo variables (query_builder.hpp, qdag).
    //   pinned_tables: con S u O constante (query_builder_pin.hpp, qdag_pin); scope_t es el duenio de las fichas
    //                  y debe vivir mientras se usen.
    template<class RankBackend>
    struct plain_tables {
        using SeQuadT = se_quadtree<RankBackend>;
        using QdagT = qdag<RankBackend>;
        struct scope_t {};
        static bool supported(const query_tp &) { return true; }
        static bool wk_applicable(const query_tp &) { return true; }
        static auto orig(const query_tp &q, resource_manager<SeQuadT> &r, scope_t &, const wide_base_config &w) { return build_qdags_from_query<SeQuadT>(q, r, w); }
        static auto nl(const query_tp &q, resource_manager<SeQuadT> &r, scope_t &, const wide_base_config &w) { return build_nl_metadata<SeQuadT>(q, r, w); }
        static auto wk(const query_tp &q, resource_manager<SeQuadT> &r, scope_t &, const wide_base_config &w) { return build_wk_metadata<SeQuadT>(q, r, w); }
        static auto leapfrog(const query_tp &q, resource_manager<SeQuadT> &r, scope_t &, const leapfrog_config &c, const wide_base_config &w) { return build_leapfrog_metadata<SeQuadT>(q, r, c, w); }
    };

    template<class RankBackend>
    struct pinned_tables {
        using SeQuadT = se_quadtree<RankBackend>;
        using QdagT = qdag_pin<RankBackend>;
        using scope_t = pin_scope<RankBackend>;
        static bool supported(const query_tp &q) { return pin_supported(q); }
        static bool wk_applicable(const query_tp &q) { return wk_pin_applicable(q); }
        static auto orig(const query_tp &q, resource_manager<SeQuadT> &r, scope_t &s, const wide_base_config &w) { return build_qdags_from_query_pin<RankBackend>(q, r, s, w); }
        static auto nl(const query_tp &q, resource_manager<SeQuadT> &r, scope_t &s, const wide_base_config &w) { return build_nl_metadata_pin<RankBackend>(q, r, s, w); }
        static auto wk(const query_tp &q, resource_manager<SeQuadT> &r, scope_t &s, const wide_base_config &w) { return build_wk_metadata_pin<RankBackend>(q, r, s, w); }
        static auto leapfrog(const query_tp &q, resource_manager<SeQuadT> &r, scope_t &s, const leapfrog_config &c, const wide_base_config &w) { return build_leapfrog_metadata_pin<RankBackend>(q, r, s, c, w); }
    };

    // ---------------------------------------------------------------- plan --

    template<class SeQuadT>
    inline bool build_meta_for_topology(QueryTopology topo, const query_tp &query, resource_manager<SeQuadT> &res_mgr,
                                        const execution_plan &plan, query_metadata_plan<SeQuadT> &meta) {
        switch (topo) {
            case QueryTopology::P2: case QueryTopology::T2: case QueryTopology::Ti2:
            case QueryTopology::P3: case QueryTopology::P4:
                build_p_plan_metadata<SeQuadT>(query, res_mgr, plan, meta); break;
            case QueryTopology::T3: case QueryTopology::Ti3: case QueryTopology::J3:
            case QueryTopology::T4: case QueryTopology::Ti4: case QueryTopology::J4:
                build_tij_plan_metadata<SeQuadT>(query, res_mgr, plan, meta); break;
            case QueryTopology::S1: case QueryTopology::S2: case QueryTopology::S3: case QueryTopology::S4:
                build_s_plan_metadata<SeQuadT>(query, res_mgr, plan, meta); break;
            case QueryTopology::Tr1: case QueryTopology::Tr2:
                build_tr_plan_metadata<SeQuadT>(query, res_mgr, plan, meta); break;
            default:
                return false;
        }
        return !meta.steps.empty();
    }

    // Aplica el plan si su builder acepta la query; si no, false sin correr nada (pasa al fallback). Con un plan
    // fijo (--plan_file) el builder puede lanzar excepcion si la query no es de su topologia (p.ej. plan de P2
    // sobre P3): run_plan aborta, el hibrido lo trata como "no calza".
    template<class RankBackend>
    inline bool plan_applies(QueryTopology topo, const query_tp &query, resource_manager<se_quadtree<RankBackend>> &res_mgr,
                             const execution_plan &plan) {
        using SeQuadT = se_quadtree<RankBackend>;
        try {
            query_metadata_plan<SeQuadT> probe;
            if (!build_meta_for_topology<SeQuadT>(topo, query, res_mgr, plan, probe))
                return false;
            probe.clearResults();
            return true;
        } catch (const std::exception &) {
            return false;
        }
    }

    template<class RankBackend>
    inline QueryOutcome run_plan_query(QueryTopology topo, const query_tp &query, resource_manager<se_quadtree<RankBackend>> &res_mgr,
                                       const execution_plan &plan, const RunnerOptions &opt, const RunModes &modes) {
        using SeQuadT = se_quadtree<RankBackend>;
        using QdagT = qdag<RankBackend>;
        QueryOutcome out;

        if (modes.generic) {
            query_metadata_plan<SeQuadT> meta;
            meta.wide = opt.wide;
            build_meta_for_topology<SeQuadT>(topo, query, res_mgr, plan, meta);
            std::vector<QdagT*> all_qdags;
            std::vector<SeQuadT*> mxm_quadtrees;
            RunTimer timer(opt.time_out);
            SeQuadT *raw = strategy_plan<QdagT, SeQuadT>(
                meta, all_qdags, mxm_quadtrees, out.generic.result, opt.upper_bound, opt.parallel_level, opt.grid_side,
                opt.coef_bound, opt.mxm, opt.k, opt.parallel_phases);
            timer.stop(out.generic);
            delete raw;
            meta.clearResults();
            for (auto *q : all_qdags)
                delete q;
            for (auto *q : mxm_quadtrees)
                delete q;
        }
        if (modes.resumable) {
            query_metadata_plan<SeQuadT> meta;
            meta.wide = opt.wide;
            build_meta_for_topology<SeQuadT>(topo, query, res_mgr, plan, meta);
            std::vector<QdagT*> all_qdags;
            std::vector<SeQuadT*> mxm_quadtrees;
            ModeOutcome &o = out.resumable;
            RunTimer timer(opt.time_out);
            SeQuadT *raw = strategy_plan_resumable<QdagT, SeQuadT>(
                meta, all_qdags, mxm_quadtrees, o.result, opt.upper_bound, opt.init_coef, opt.growth, opt.max_retries,
                opt.parallel_level, opt.grid_side, opt.mxm, opt.k, &o.attempts, &o.coef, &o.exhaustive, opt.growth_mode);
            timer.stop(o);
            delete raw;
            meta.clearResults();
            for (auto *q : all_qdags)
                delete q;
            for (auto *q : mxm_quadtrees)
                delete q;
        }
        if (modes.adaptive) {
            query_metadata_plan<SeQuadT> meta;
            meta.wide = opt.wide;
            build_meta_for_topology<SeQuadT>(topo, query, res_mgr, plan, meta);
            std::vector<QdagT*> all_qdags;
            std::vector<SeQuadT*> mxm_quadtrees;
            ModeOutcome &o = out.adaptive;
            RunTimer timer(opt.time_out);
            SeQuadT *raw = strategy_plan_adaptive<QdagT, SeQuadT>(
                meta, all_qdags, mxm_quadtrees, o.result, opt.upper_bound, opt.init_coef, opt.growth, opt.max_retries,
                opt.parallel_level, opt.grid_side, opt.mxm, opt.k, opt.parallel_phases,
                &o.attempts, &o.coef, &o.exhaustive, opt.growth_mode);
            timer.stop(o);
            delete raw;
            meta.clearResults();
            for (auto *q : all_qdags)
                delete q;
            for (auto *q : mxm_quadtrees)
                delete q;
        }
        return out;
    }

    // ------------------------------------------------------------ fallbacks --

    template<class RankBackend, class Tables = plain_tables<RankBackend>>
    inline QueryOutcome run_nl_query(const query_tp &query, resource_manager<se_quadtree<RankBackend>> &res_mgr,
                                     const RunnerOptions &opt, const RunModes &modes) {
        using SeQuadT = se_quadtree<RankBackend>;
        using QdagT = typename Tables::QdagT;
        QueryOutcome out;
        if (!Tables::supported(query)) {
            out.status = RunStatus::NotApplicable;
            return out;
        }
        typename Tables::scope_t scope;
        auto meta = Tables::nl(query, res_mgr, scope, opt.wide);
        if (meta.nl_quadtrees.empty()) {
            out.status = RunStatus::NotApplicable;
            return out;
        }

        if (modes.generic) {
            std::vector<QdagT*> qd_nl, qd_f;
            SeQuadT *proj = nullptr, *fin = nullptr;
            RunTimer timer(opt.time_out);
            strategy_non_lonely<QdagT, SeQuadT>(meta, qd_nl, qd_f, proj, fin, out.generic.result,
                                                opt.upper_bound, opt.parallel_level, opt.grid_side, opt.parallel_phases);
            timer.stop(out.generic);
            for (auto *q : qd_nl)
                delete q;
            for (auto *q : qd_f)
                delete q;
            delete proj; delete fin;
        }
        // One-Shot solo tiene la variante generic: su prejoin es exacto y una pasada alcanza
        // min(upper_bound, total) (strategy_modes() descarta adaptive y resumable).
        return out;
    }

    template<class RankBackend, class Tables = plain_tables<RankBackend>>
    inline QueryOutcome run_wk_query(const query_tp &query, resource_manager<se_quadtree<RankBackend>> &res_mgr,
                                     const RunnerOptions &opt, const RunModes &modes) {
        using SeQuadT = se_quadtree<RankBackend>;
        using QdagT = typename Tables::QdagT;
        QueryOutcome out;
        if (!Tables::supported(query)) {
            out.status = RunStatus::NotApplicable;
            return out;
        }
        typename Tables::scope_t scope;
        if (!Tables::wk_applicable(query)) {
            out.status = RunStatus::NotApplicable;
            return out;
        }
        auto meta = Tables::wk(query, res_mgr, scope, opt.wide);
        if (meta.patterns.empty()) {
            out.status = RunStatus::NotApplicable;
            return out;
        }

        if (modes.generic) {
            std::vector<QdagT*> qd;
            RunTimer timer(opt.time_out);
            SeQuadT *fin = strategy_weak_kernel<QdagT, SeQuadT>(
                meta, qd, out.generic.result, opt.upper_bound, opt.coef_bound, opt.parallel_level, opt.grid_side, opt.parallel_phases);
            timer.stop(out.generic);
            for (auto *q : qd)
                delete q;
            delete fin;
        }
        if (modes.resumable) {
            std::vector<QdagT*> qd;
            ModeOutcome &o = out.resumable;
            RunTimer timer(opt.time_out);
            SeQuadT *fin = strategy_weak_kernel_resumable<QdagT, SeQuadT, SeQuadT>(
                meta, qd, o.result, opt.upper_bound, opt.init_coef, opt.growth, opt.max_retries,
                opt.parallel_level, opt.grid_side, &o.attempts, &o.coef, &o.exhaustive, opt.growth_mode);
            timer.stop(o);
            delete fin;
            for (auto *q : qd)
                delete q;
        }
        if (modes.adaptive) {
            std::vector<QdagT*> qd;
            ModeOutcome &o = out.adaptive;
            RunTimer timer(opt.time_out);
            SeQuadT *fin = strategy_weak_kernel_adaptive<QdagT, SeQuadT>(
                meta, qd, o.result, opt.upper_bound, opt.init_coef, opt.growth, opt.max_retries,
                opt.parallel_level, opt.grid_side, opt.parallel_phases, &o.attempts, &o.coef, &o.exhaustive, opt.growth_mode);
            timer.stop(o);
            delete fin;
            for (auto *q : qd)
                delete q;
        }
        return out;
    }

    template<class RankBackend, class Tables = plain_tables<RankBackend>>
    inline QueryOutcome run_leapfrog_query(const query_tp &query, resource_manager<se_quadtree<RankBackend>> &res_mgr,
                                           const RunnerOptions &opt, const RunModes &modes) {
        using SeQuadT = se_quadtree<RankBackend>;
        using QdagT = typename Tables::QdagT;
        QueryOutcome out;
        if (!Tables::supported(query)) {
            out.status = RunStatus::NotApplicable;
            return out;
        }
        typename Tables::scope_t scope;
        auto meta = Tables::leapfrog(query, res_mgr, scope, opt.leapfrog, opt.wide);
        if (meta.steps.empty() && meta.final_qts.empty()) {
            out.status = RunStatus::NotApplicable;
            return out;
        }

        if (modes.generic) {
            std::vector<QdagT*> qd;
            RunTimer timer(opt.time_out);
            SeQuadT *fin = strategy_leapfrog<QdagT, SeQuadT, SeQuadT>(
                meta, qd, out.generic.result, opt.upper_bound, opt.coef_bound, opt.parallel_level, opt.grid_side, opt.parallel_phases);
            timer.stop(out.generic);
            for (auto *q : qd)
                delete q;
            delete fin;
        }
        if (modes.adaptive) {
            std::vector<QdagT*> qd;
            ModeOutcome &o = out.adaptive;
            RunTimer timer(opt.time_out);
            SeQuadT *fin = strategy_leapfrog_adaptive<QdagT, SeQuadT, SeQuadT>(
                meta, qd, o.result, opt.upper_bound, opt.init_coef, opt.growth, opt.max_retries,
                opt.parallel_level, opt.grid_side, opt.parallel_phases, &o.attempts, &o.coef, &o.exhaustive, opt.growth_mode);
            timer.stop(o);
            delete fin;
            for (auto *q : qd)
                delete q;
        }
        return out;
    }

    // Solo variante generic (no existe kernel adaptive).
    template<class RankBackend, class Tables = plain_tables<RankBackend>>
    inline QueryOutcome run_kernel_query(const query_tp &query, resource_manager<se_quadtree<RankBackend>> &res_mgr,
                                         const RunnerOptions &opt, const RunModes &modes) {
        using SeQuadT = se_quadtree<RankBackend>;
        using QdagT = typename Tables::QdagT;
        QueryOutcome out;
        if (!Tables::supported(query)) {
            out.status = RunStatus::NotApplicable;
            return out;
        }
        typename Tables::scope_t scope;
        auto meta = Tables::nl(query, res_mgr, scope, opt.wide);
        if (meta.nl_quadtrees.empty() || meta.l_quadtrees.empty()) {
            out.status = RunStatus::NotApplicable;
            return out;
        }
        if (meta.att_nl.size() > KERNEL_MAX_NONLONELY_ATTS) {
            out.status = RunStatus::TooWide;
            return out;
        }

        if (modes.generic) {
            std::vector<QdagT*> qd;
            RunTimer timer(opt.time_out);
            SeQuadT *fin = strategy_kernel<QdagT, SeQuadT, SeQuadT>(
                meta, qd, out.generic.result, opt.upper_bound, opt.parallel_level, opt.grid_side, opt.parallel_phases);
            timer.stop(out.generic);
            for (auto *q : qd)
                delete q;
            delete fin;
        }
        return out;
    }

    // Solo variante generic (strategy_orig no tiene adaptive).
    template<class RankBackend, class Tables = plain_tables<RankBackend>>
    inline QueryOutcome run_orig_query(const query_tp &query, resource_manager<se_quadtree<RankBackend>> &res_mgr,
                                       const RunnerOptions &opt, const RunModes &modes) {
        using SeQuadT = se_quadtree<RankBackend>;
        using QdagT = typename Tables::QdagT;
        QueryOutcome out;
        if (!Tables::supported(query)) {
            out.status = RunStatus::NotApplicable;
            return out;
        }
        typename Tables::scope_t scope;
        auto rel = Tables::orig(query, res_mgr, scope, opt.wide);
        if (rel.quadtrees.empty()) {
            out.status = RunStatus::NotApplicable;
            return out;
        }

        if (modes.generic) {
            std::vector<QdagT*> qd;
            RunTimer timer(opt.time_out);
            SeQuadT *fin = strategy_orig<QdagT, SeQuadT>(rel, qd, out.generic.result, opt.upper_bound, opt.parallel_level, opt.grid_side);
            timer.stop(out.generic);
            for (auto *q : qd)
                delete q;
            delete fin;
        }
        return out;
    }

    template<class RankBackend, class Tables = plain_tables<RankBackend>>
    inline QueryOutcome run_fallback_query(Strategy s, const query_tp &query, resource_manager<se_quadtree<RankBackend>> &res_mgr,
                                           const RunnerOptions &opt, const RunModes &modes) {
        switch (s) {
            case Strategy::Nl:
                return run_nl_query<RankBackend, Tables>(query, res_mgr, opt, modes);
            case Strategy::Wk:
                return run_wk_query<RankBackend, Tables>(query, res_mgr, opt, modes);
            case Strategy::Leapfrog:
                return run_leapfrog_query<RankBackend, Tables>(query, res_mgr, opt, modes);
            case Strategy::Kernel:
                return run_kernel_query<RankBackend, Tables>(query, res_mgr, opt, modes);
            default:
                return run_orig_query<RankBackend, Tables>(query, res_mgr, opt, modes);
        }
    }

} // namespace gdj_examples

#endif // QDAGS_EXAMPLES_QUERY_RUNNERS_HPP
