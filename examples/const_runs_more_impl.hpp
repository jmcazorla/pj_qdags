#ifndef QDAGS_EXAMPLES_CONST_RUNS_MORE_IMPL_HPP
#define QDAGS_EXAMPLES_CONST_RUNS_MORE_IMPL_HPP

// Definiciones de const_runs_more.hpp. Lo incluyen SOLO los run_<ejecutable>_const.cpp de mixed/resumable/race.

// Los headers del motor se incluyen ANTES que const_runs_impl.hpp y con los mismos renombres (ver su comentario):
// asi son los primeros en incluir qdag.hpp, joins.hpp, etc., y los renombres valen para toda la unidad.
// Orden como en const_runs_impl.hpp: joins_wide.hpp necesita qdag_*.hpp antes.
#define appendChildBitsToCoords appendChildBitsToCoords_const_unit
#define build_template_vector build_template_vector_const_unit

#include "qdags/index/commun.hpp"
#include "qdags/index/se_quadtree_bv64.hpp"
#include "qdags/index/se_quadtree_vfamily.hpp"
#include "qdags/index/se_quadtree_sd.hpp"
#include "qdags/index/se_quadtree_rrr.hpp"
#include "qdags/index/se_quadtree_rrr15.hpp"
#include "qdags/index/qdag_bv64.hpp"
#include "qdags/index/qdag_vfamily.hpp"
#include "qdags/index/qdag_sd.hpp"
#include "qdags/index/qdag_rrr.hpp"
#include "qdags/index/qdag_rrr15.hpp"
#include "qdags/join/joins.hpp"
#include "qdags/join/joins_wide.hpp"
#include "qdags/join/joins_resumable.hpp"
#include "qdags/join/joins_resumable_wide.hpp"
#include "qdags/strategy/generic_strategies_resumable.hpp"
#include "qdags/strategy/generic_strategies_race.hpp"
#include "qdags/strategy/strategy_race.hpp"
#include "qdags/strategy/generic_strategies_leapfrog_resumable.hpp"
#include "qdags/strategy/generic_strategies_leapfrog_race.hpp"
#include "qdags/strategy/generic_strategies_kernel_race.hpp"

#undef appendChildBitsToCoords
#undef build_template_vector

#include "const_runs_impl.hpp"
#include "const_runs_more.hpp"

namespace gdj_examples {

    // ------------------------------------------------------------ One-Shot --

    template<class RankBackendIn, class RankBackendOut>
    ConstRunResult run_nl_const(const ConstRunArgs &a, gdj::resource_manager<gdj::se_quadtree<RankBackendIn>> &res_mgr,
                                const gdj::query_tp &query) {
        using namespace gdj;
        using SeQuadT = se_quadtree<RankBackendIn>;
        using SeQuadTOut = se_quadtree<RankBackendOut>;
        using QdagT = qdag_pin<RankBackendIn>;
        if (!pin_supported(query))
            return const_detail::unsupported();
        pin_scope<RankBackendIn> pins;
        auto meta = build_nl_metadata_pin<RankBackendIn>(query, res_mgr, pins, a.wide);
        if (meta.nl_quadtrees.empty())
            return const_detail::skipped('-', "no non-lonely variables");

        ConstRunResult r;
        const ParallelPhases phases = (ParallelPhases)a.parallel_phases;
        std::vector<QdagT*> qd_nl, qd_f;
        SeQuadT *proj = nullptr;
        SeQuadTOut *fin = nullptr;
        start_timeout_watchdog(a.time_out);
        auto t0 = const_detail::clock::now();
        strategy_non_lonely<QdagT, SeQuadT, SeQuadTOut>(meta, qd_nl, qd_f, proj, fin, r.generic_result,
                                                        a.upper_bound, a.parallel_level, a.grid_side, phases);
        auto t1 = const_detail::clock::now();
        stop_timeout_watchdog();
        r.generic_ms = const_detail::ms_between(t0, t1);
        for (auto *q : qd_nl)
            delete q;
        for (auto *q : qd_f)
            delete q;
        delete proj; delete fin;
        r.ran = true;
        return r;
    }

    // ----------------------------------------------------------- WK mixed --

    template<class RankBackendIn, class RankBackendOut>
    ConstRunResult run_wk_mixed_const(const ConstRunArgs &a, gdj::resource_manager<gdj::se_quadtree<RankBackendIn>> &res_mgr,
                                      const gdj::query_tp &query) {
        using namespace gdj;
        using SeQuadTIn = se_quadtree<RankBackendIn>;
        using SeQuadTOut = se_quadtree<RankBackendOut>;
        using QdagT = qdag_pin<RankBackendIn>;
        if (!pin_supported(query))
            return const_detail::unsupported();
        if (!wk_pin_applicable(query))
            return const_detail::skipped('-', "WeakKernel not applicable: a variable appears only in constant patterns");
        pin_scope<RankBackendIn> pins;
        auto meta = build_wk_metadata_pin<RankBackendIn>(query, res_mgr, pins, a.wide);
        if (meta.patterns.empty())
            return const_detail::skipped('-', "no usable patterns");

        ConstRunResult r;
        const ParallelPhases phases = (ParallelPhases)a.parallel_phases;
        if (a.run_generic) {
            std::vector<QdagT*> qd;
            start_timeout_watchdog(a.time_out);
            auto t0 = const_detail::clock::now();
            SeQuadTOut* fin = strategy_weak_kernel<QdagT, SeQuadTIn, SeQuadTOut>(
                meta, qd, r.generic_result, a.upper_bound, a.coef_bound, a.parallel_level, a.grid_side, phases);
            auto t1 = const_detail::clock::now();
            stop_timeout_watchdog();
            r.generic_ms = const_detail::ms_between(t0, t1);
            for (auto *q : qd)
                delete q;
            delete fin;
        }
        if (a.run_adaptive) {
            std::vector<QdagT*> qd;
            start_timeout_watchdog(a.time_out);
            auto t0 = const_detail::clock::now();
            SeQuadTOut* fin = strategy_weak_kernel_adaptive<QdagT, SeQuadTIn, SeQuadTOut>(
                meta, qd, r.adaptive_result, a.upper_bound, a.init_coef, a.growth, a.max_retries,
                a.parallel_level, a.grid_side, phases, &r.attempts, &r.coef_used, &r.exhaustive, (GrowthMode)a.growth_mode);
            auto t1 = const_detail::clock::now();
            stop_timeout_watchdog();
            r.adaptive_ms = const_detail::ms_between(t0, t1);
            delete fin;
            for (auto *q : qd)
                delete q;
        }
        r.ran = true;
        return r;
    }

    // ------------------------------------------------------- WK resumable --

    template<class RankBackendIn, class RankBackendOut>
    ConstRunResult run_wk_resumable_const(const ConstRunArgs &a, gdj::resource_manager<gdj::se_quadtree<RankBackendIn>> &res_mgr,
                                          const gdj::query_tp &query) {
        using namespace gdj;
        using SeQuadT = se_quadtree<RankBackendIn>;
        using SeQuadTOut = se_quadtree<RankBackendOut>;
        using QdagT = qdag_pin<RankBackendIn>;
        if (!pin_supported(query))
            return const_detail::unsupported();
        if (!wk_pin_applicable(query))
            return const_detail::skipped('-', "WeakKernel not applicable: a variable appears only in constant patterns");
        pin_scope<RankBackendIn> pins;
        auto meta = build_wk_metadata_pin<RankBackendIn>(query, res_mgr, pins, a.wide);
        if (meta.patterns.empty())
            return const_detail::skipped('-', "no usable patterns");

        ConstRunResult r;
        std::vector<QdagT*> all_qdags;
        start_timeout_watchdog(a.time_out);
        auto t0 = const_detail::clock::now();
        SeQuadTOut* fin = strategy_weak_kernel_resumable<QdagT, SeQuadT, SeQuadTOut>(
            meta, all_qdags, r.generic_result, a.upper_bound, a.init_coef, a.growth, a.max_retries,
            a.parallel_level, a.grid_side, &r.attempts, &r.coef_used, &r.exhaustive, (GrowthMode)a.growth_mode);
        auto t1 = const_detail::clock::now();
        stop_timeout_watchdog();
        r.generic_ms = const_detail::ms_between(t0, t1);
        delete fin;
        for (auto *q : all_qdags)
            delete q;
        r.ran = true;
        return r;
    }

    // ---------------------------------------------------------------- race --

    namespace const_detail {
        inline ConstRaceResult race_skipped(char marker, const std::string &reason) {
            ConstRaceResult r;
            r.ran = false; r.skip_marker = marker; r.skip_reason = reason;
            return r;
        }
    }

    template<class RankBackendIn, class RankBackendOut>
    ConstRaceResult run_nl_race_const(const ConstRunArgs &a, gdj::resource_manager<gdj::se_quadtree<RankBackendIn>> &res_mgr,
                                      const gdj::query_tp &query) {
        using namespace gdj;
        using SeQuadT = se_quadtree<RankBackendIn>;
        using SeQuadTOut = se_quadtree<RankBackendOut>;
        using QdagT = qdag_pin<RankBackendIn>;
        if (!pin_supported(query))
            return const_detail::race_skipped('-', "pattern with constant subject and object: unsupported");
        pin_scope<RankBackendIn> pins;
        auto meta = build_nl_metadata_pin<RankBackendIn>(query, res_mgr, pins, a.wide);
        if (meta.nl_quadtrees.empty())
            return const_detail::race_skipped('-', "no non-lonely variables");

        ConstRaceResult r;
        const ParallelPhases phases = (ParallelPhases)a.parallel_phases;
        if (a.run_generic) {
            RaceStats st;
            auto t0 = const_detail::clock::now();
            r.generic_result = strategy_non_lonely_raced<QdagT, SeQuadT, SeQuadTOut>(
                meta, a.upper_bound, a.parallel_level, a.grid_side, phases, a.time_out, &st);
            auto t1 = const_detail::clock::now();
            r.generic_ms = const_detail::ms_between(t0, t1);
            r.generic_winner = st.winner; r.seq_time = st.seq_time; r.par_time = st.par_time;
        }
        r.ran = true;
        return r;
    }

    template<class RankBackendIn, class RankBackendOut>
    ConstRaceResult run_wk_race_const(const ConstRunArgs &a, gdj::resource_manager<gdj::se_quadtree<RankBackendIn>> &res_mgr,
                                      const gdj::query_tp &query) {
        using namespace gdj;
        using SeQuadT = se_quadtree<RankBackendIn>;
        using SeQuadTOut = se_quadtree<RankBackendOut>;
        using QdagT = qdag_pin<RankBackendIn>;
        if (!pin_supported(query))
            return const_detail::race_skipped('-', "pattern with constant subject and object: unsupported");
        if (!wk_pin_applicable(query))
            return const_detail::race_skipped('-', "WeakKernel not applicable: a variable appears only in constant patterns");
        pin_scope<RankBackendIn> pins;
        auto meta = build_wk_metadata_pin<RankBackendIn>(query, res_mgr, pins, a.wide);
        if (meta.patterns.empty())
            return const_detail::race_skipped('-', "no usable patterns");

        ConstRaceResult r;
        const ParallelPhases phases = (ParallelPhases)a.parallel_phases;
        if (a.run_generic) {
            RaceStats st;
            auto t0 = const_detail::clock::now();
            r.generic_result = strategy_weak_kernel_raced<QdagT, SeQuadT, SeQuadTOut>(
                meta, a.upper_bound, a.coef_bound, a.parallel_level, a.grid_side, phases, a.time_out, &st);
            auto t1 = const_detail::clock::now();
            r.generic_ms = const_detail::ms_between(t0, t1);
            r.generic_winner = st.winner; r.seq_time = st.seq_time; r.par_time = st.par_time;
        }
        if (a.run_adaptive) {
            RaceStatsAdaptive st;
            auto t0 = const_detail::clock::now();
            r.adaptive_result = strategy_weak_kernel_raced_adaptive<QdagT, SeQuadT, SeQuadTOut>(
                meta, a.upper_bound, a.init_coef, a.growth, a.max_retries, a.parallel_level, a.grid_side, phases, a.time_out, &st, (GrowthMode)a.growth_mode);
            auto t1 = const_detail::clock::now();
            r.adaptive_ms = const_detail::ms_between(t0, t1);
            r.adaptive_winner = st.winner; r.seq_attempts = st.seq_attempts; r.par_attempts = st.par_attempts;
        }
        r.ran = true;
        return r;
    }

    // ------------------------------------------------- Leapfrog resumable --

    template<class RankBackendIn, class RankBackendOut>
    ConstRunResult run_leapfrog_resumable_const(const ConstRunArgs &a, gdj::resource_manager<gdj::se_quadtree<RankBackendIn>> &res_mgr,
                                                const gdj::query_tp &query) {
        using namespace gdj;
        using SeQuadT = se_quadtree<RankBackendIn>;
        using SeQuadTOut = se_quadtree<RankBackendOut>;
        using QdagT = qdag_pin<RankBackendIn>;
        if (!pin_supported(query))
            return const_detail::unsupported();
        leapfrog_config lf;
        lf.order = (LeapfrogOrder)a.lf_order;
        lf.depth = (LeapfrogDepth)a.lf_depth;
        lf.max_prejoin_atts = a.lf_max_atts;
        pin_scope<RankBackendIn> pins;
        auto meta = build_leapfrog_metadata_pin<RankBackendIn>(query, res_mgr, pins, lf, a.wide);
        if (meta.steps.empty() && meta.final_qts.empty())
            return const_detail::skipped('-', "no usable patterns");

        ConstRunResult r;
        r.n_prejoins = meta.steps.size();
        for (auto &step : meta.steps)
            if (step.exact)
                r.n_exact++;
        std::vector<QdagT*> all_qdags;
        SeQuadTOut* fin = nullptr;
        start_timeout_watchdog(a.time_out);
        auto t0 = const_detail::clock::now();
        try {
            fin = strategy_leapfrog_resumable<QdagT, SeQuadT, SeQuadTOut>(
                meta, all_qdags, r.generic_result, a.upper_bound, a.init_coef, a.growth, a.max_retries,
                a.parallel_level, a.grid_side, (ParallelPhases)a.parallel_phases, &r.attempts, &r.coef_used, &r.exhaustive, (GrowthMode)a.growth_mode);
        } catch (const std::invalid_argument &e) {
            stop_timeout_watchdog();
            for (auto *q : all_qdags)
                delete q;
            return const_detail::skipped('+', e.what());
        }
        auto t1 = const_detail::clock::now();
        stop_timeout_watchdog();
        r.generic_ms = const_detail::ms_between(t0, t1);
        delete fin;
        for (auto *q : all_qdags)
            delete q;
        r.ran = true;
        return r;
    }

    // ------------------------------------------------------ Leapfrog race --

    template<class RankBackendIn, class RankBackendOut>
    ConstRaceResult run_leapfrog_race_const(const ConstRunArgs &a, gdj::resource_manager<gdj::se_quadtree<RankBackendIn>> &res_mgr,
                                            const gdj::query_tp &query) {
        using namespace gdj;
        using SeQuadT = se_quadtree<RankBackendIn>;
        using SeQuadTOut = se_quadtree<RankBackendOut>;
        using QdagT = qdag_pin<RankBackendIn>;
        if (!pin_supported(query))
            return const_detail::race_skipped('-', "pattern with constant subject and object: unsupported");
        leapfrog_config lf;
        lf.order = (LeapfrogOrder)a.lf_order;
        lf.depth = (LeapfrogDepth)a.lf_depth;
        lf.max_prejoin_atts = a.lf_max_atts;
        pin_scope<RankBackendIn> pins;
        auto meta = build_leapfrog_metadata_pin<RankBackendIn>(query, res_mgr, pins, lf, a.wide);
        if (meta.steps.empty() && meta.final_qts.empty())
            return const_detail::race_skipped('-', "no usable patterns");

        ConstRaceResult r;
        r.n_prejoins = meta.steps.size();
        for (auto &step : meta.steps)
            if (step.exact)
                r.n_exact++;
        const ParallelPhases phases = (ParallelPhases)a.parallel_phases;
        if (a.run_generic) {
            RaceStats st;
            auto t0 = const_detail::clock::now();
            r.generic_result = strategy_leapfrog_raced<QdagT, SeQuadT, SeQuadTOut>(
                meta, a.upper_bound, a.coef_bound, a.parallel_level, a.grid_side, phases, a.time_out, &st);
            auto t1 = const_detail::clock::now();
            r.generic_ms = const_detail::ms_between(t0, t1);
            r.generic_winner = st.winner; r.seq_time = st.seq_time; r.par_time = st.par_time;
        }
        if (a.run_adaptive) {
            RaceStatsAdaptive st;
            auto t0 = const_detail::clock::now();
            r.adaptive_result = strategy_leapfrog_raced_adaptive<QdagT, SeQuadT, SeQuadTOut>(
                meta, a.upper_bound, a.init_coef, a.growth, a.max_retries, a.parallel_level, a.grid_side, phases, a.time_out, &st, (GrowthMode)a.growth_mode);
            auto t1 = const_detail::clock::now();
            r.adaptive_ms = const_detail::ms_between(t0, t1);
            r.adaptive_winner = st.winner; r.seq_attempts = st.seq_attempts; r.par_attempts = st.par_attempts;
        }
        r.ran = true;
        return r;
    }

    // -------------------------------------------------------- Kernel race --

    template<class RankBackendIn, class RankBackendOut>
    ConstRaceResult run_kernel_race_const(const ConstRunArgs &a, gdj::resource_manager<gdj::se_quadtree<RankBackendIn>> &res_mgr,
                                          const gdj::query_tp &query) {
        using namespace gdj;
        using SeQuadT = se_quadtree<RankBackendIn>;
        using SeQuadTOut = se_quadtree<RankBackendOut>;
        using QdagT = qdag_pin<RankBackendIn>;
        if (!pin_supported(query))
            return const_detail::race_skipped('-', "pattern with constant subject and object: unsupported");
        pin_scope<RankBackendIn> pins;
        auto meta = build_nl_metadata_pin<RankBackendIn>(query, res_mgr, pins, a.wide);
        if (meta.nl_quadtrees.empty() || meta.l_quadtrees.empty())
            return const_detail::race_skipped('-', "kernel needs both lonely and non-lonely variables");
        if (meta.att_nl.size() > KERNEL_MAX_NONLONELY_ATTS)
            return const_detail::race_skipped('+', std::to_string(meta.att_nl.size()) + " non-lonely variables, the engine supports at most "
                                                   + std::to_string(KERNEL_MAX_NONLONELY_ATTS));

        ConstRaceResult r;
        RaceStats st;
        auto t0 = const_detail::clock::now();
        r.generic_result = strategy_kernel_raced<QdagT, SeQuadT, SeQuadTOut>(
            meta, a.upper_bound, a.parallel_level, a.grid_side, (ParallelPhases)a.parallel_phases, a.time_out, &st);
        auto t1 = const_detail::clock::now();
        r.generic_ms = const_detail::ms_between(t0, t1);
        r.generic_winner = st.winner; r.seq_time = st.seq_time; r.par_time = st.par_time;
        r.ran = true;
        return r;
    }

} // namespace gdj_examples

#endif // QDAGS_EXAMPLES_CONST_RUNS_MORE_IMPL_HPP
