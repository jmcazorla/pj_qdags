#ifndef QDAGS_EXAMPLES_CONST_RUNS_IMPL_HPP
#define QDAGS_EXAMPLES_CONST_RUNS_IMPL_HPP

// Definiciones de const_runs.hpp. Lo incluyen SOLO los run_<estrategia>_const.cpp, que instancian la funcion de
// su estrategia para cada backend; el perfilador nunca (motivo en const_runs.hpp).

// Dos funciones estan definidas sin `inline` en headers: gdj::util::appendChildBitsToCoords (detail/bits.hpp) y
// sdsl::k2_tree_ns::build_template_vector para bit_vector (sdsl/k2_tree_helper.hpp). Con dos unidades de
// compilacion el enlace da simbolo duplicado, asi que aca llevan otro nombre. Si aparece una tercera, el enlace
// falla con "multiple definition" y hay que sumarla aca.
#define appendChildBitsToCoords appendChildBitsToCoords_const_unit
#define build_template_vector build_template_vector_const_unit

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

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
#include "qdags/index/qdag_pin.hpp"
#include "qdags/join/joins.hpp"
#include "qdags/join/joins_wide.hpp"
#include "qdags/query/triple_pattern.hpp"
#include "qdags/query/resource_manager.hpp"
#include "qdags/query/query_builder.hpp"
#include "qdags/query/leapfrog_builder.hpp"
#include "qdags/query/query_builder_pin.hpp"
#include "qdags/strategy/generic_strategies.hpp"
#include "qdags/strategy/generic_strategies_kernel.hpp"
#include "qdags/strategy/generic_strategies_leapfrog.hpp"

#undef appendChildBitsToCoords
#undef build_template_vector

#include "const_runs.hpp"

namespace gdj_examples {

    namespace const_detail {
        using clock = std::chrono::high_resolution_clock;

        inline ConstRunResult skipped(char marker, const std::string &reason) {
            ConstRunResult r;
            r.ran = false; r.skip_marker = marker; r.skip_reason = reason;
            return r;
        }
        inline ConstRunResult unsupported() {
            return skipped('-', "pattern with constant subject and object: unsupported");
        }
        inline double ms_between(clock::time_point a, clock::time_point b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        }
    }

    // ---------------------------------------------------------------- Orig --

    template<class RankBackend>
    ConstRunResult run_orig_const(const ConstRunArgs &a, gdj::resource_manager<gdj::se_quadtree<RankBackend>> &res_mgr,
                                  const gdj::query_tp &query) {
        using namespace gdj;
        using SeQuadT = se_quadtree<RankBackend>;
        using QdagT = qdag_pin<RankBackend>;
        if (!pin_supported(query))
            return const_detail::unsupported();
        pin_scope<RankBackend> pins;
        auto rel = build_qdags_from_query_pin<RankBackend>(query, res_mgr, pins, a.wide);
        if (rel.quadtrees.empty())
            return const_detail::skipped('-', "no solvable patterns: constant P with variable S/O");

        ConstRunResult r;
        std::vector<QdagT*> all_qdags;
        start_timeout_watchdog(a.time_out);
        auto t0 = const_detail::clock::now();
        SeQuadT* result = strategy_orig<QdagT, SeQuadT>(rel, all_qdags, r.generic_result, a.upper_bound, a.parallel_level, a.grid_side);
        auto t1 = const_detail::clock::now();
        stop_timeout_watchdog();
        r.generic_ms = const_detail::ms_between(t0, t1);
        for (auto *q : all_qdags)
            delete q;
        delete result;
        r.ran = true;
        return r;
    }

    // ---------------------------------------------------------- WeakKernel --

    template<class RankBackend>
    ConstRunResult run_wk_const(const ConstRunArgs &a, gdj::resource_manager<gdj::se_quadtree<RankBackend>> &res_mgr,
                                const gdj::query_tp &query) {
        using namespace gdj;
        using SeQuadT = se_quadtree<RankBackend>;
        using QdagT = qdag_pin<RankBackend>;
        if (!pin_supported(query))
            return const_detail::unsupported();
        if (!wk_pin_applicable(query))
            return const_detail::skipped('-', "WeakKernel not applicable: a variable appears only in constant patterns");
        pin_scope<RankBackend> pins;
        auto meta = build_wk_metadata_pin<RankBackend>(query, res_mgr, pins, a.wide);
        if (meta.patterns.empty())
            return const_detail::skipped('-', "no usable patterns");

        ConstRunResult r;
        const ParallelPhases phases = (ParallelPhases)a.parallel_phases;
        if (a.run_generic) {
            std::vector<QdagT*> qd;
            start_timeout_watchdog(a.time_out);
            auto t0 = const_detail::clock::now();
            SeQuadT* fin = strategy_weak_kernel<QdagT, SeQuadT>(
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
            SeQuadT* fin = strategy_weak_kernel_adaptive<QdagT, SeQuadT>(
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

    // -------------------------------------------------------------- Kernel --

    template<class RankBackendIn, class RankBackendOut>
    ConstRunResult run_kernel_const(const ConstRunArgs &a, gdj::resource_manager<gdj::se_quadtree<RankBackendIn>> &res_mgr,
                                    const gdj::query_tp &query) {
        using namespace gdj;
        using SeQuadT = se_quadtree<RankBackendIn>;
        using SeQuadTOut = se_quadtree<RankBackendOut>;
        using QdagT = qdag_pin<RankBackendIn>;
        if (!pin_supported(query))
            return const_detail::unsupported();
        pin_scope<RankBackendIn> pins;
        auto meta = build_nl_metadata_pin<RankBackendIn>(query, res_mgr, pins, a.wide);
        if (meta.nl_quadtrees.empty() || meta.l_quadtrees.empty())
            return const_detail::skipped('-', "kernel needs both lonely and non-lonely variables");
        if (meta.att_nl.size() > KERNEL_MAX_NONLONELY_ATTS)
            return const_detail::skipped('+', std::to_string(meta.att_nl.size()) + " non-lonely variables, the engine supports at most "
                                              + std::to_string(KERNEL_MAX_NONLONELY_ATTS));

        ConstRunResult r;
        std::vector<QdagT*> all_qdags;
        start_timeout_watchdog(a.time_out);
        auto t0 = const_detail::clock::now();
        SeQuadTOut* final_qt = strategy_kernel<QdagT, SeQuadT, SeQuadTOut>(
            meta, all_qdags, r.generic_result, a.upper_bound, a.parallel_level, a.grid_side, (ParallelPhases)a.parallel_phases);
        auto t1 = const_detail::clock::now();
        stop_timeout_watchdog();
        r.generic_ms = const_detail::ms_between(t0, t1);
        for (auto *q : all_qdags)
            delete q;
        delete final_qt;
        r.ran = true;
        return r;
    }

    // ------------------------------------------------------------ Leapfrog --

    template<class RankBackendIn, class RankBackendOut>
    ConstRunResult run_leapfrog_const(const ConstRunArgs &a, gdj::resource_manager<gdj::se_quadtree<RankBackendIn>> &res_mgr,
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
        const ParallelPhases phases = (ParallelPhases)a.parallel_phases;
        if (a.run_generic) {
            std::vector<QdagT*> qd;
            start_timeout_watchdog(a.time_out);
            auto t0 = const_detail::clock::now();
            SeQuadTOut* fin = strategy_leapfrog<QdagT, SeQuadT, SeQuadTOut>(
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
            SeQuadTOut* fin = strategy_leapfrog_adaptive<QdagT, SeQuadT, SeQuadTOut>(
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

} // namespace gdj_examples

// Los seis backends de --variant, y sus 36 pares para --variant / --variant_out.
#define QDAGS_CONST_BACKENDS(X) \
    X(gdj::rank_bv_64_p) \
    X(gdj::rank_vfamily_p<sdsl::rank_support_v5<>>) \
    X(gdj::rank_vfamily_p<sdsl::rank_support_v<>>) \
    X(gdj::rank_sd_p) \
    X(gdj::rank_rrr_p) \
    X(gdj::rank_rrr15_p)

#define QDAGS_CONST_BACKENDS_WITH(X, A) \
    X(A, gdj::rank_bv_64_p) \
    X(A, gdj::rank_vfamily_p<sdsl::rank_support_v5<>>) \
    X(A, gdj::rank_vfamily_p<sdsl::rank_support_v<>>) \
    X(A, gdj::rank_sd_p) \
    X(A, gdj::rank_rrr_p) \
    X(A, gdj::rank_rrr15_p)

#define QDAGS_CONST_BACKEND_PAIRS(X) \
    QDAGS_CONST_BACKENDS_WITH(X, gdj::rank_bv_64_p) \
    QDAGS_CONST_BACKENDS_WITH(X, gdj::rank_vfamily_p<sdsl::rank_support_v5<>>) \
    QDAGS_CONST_BACKENDS_WITH(X, gdj::rank_vfamily_p<sdsl::rank_support_v<>>) \
    QDAGS_CONST_BACKENDS_WITH(X, gdj::rank_sd_p) \
    QDAGS_CONST_BACKENDS_WITH(X, gdj::rank_rrr_p) \
    QDAGS_CONST_BACKENDS_WITH(X, gdj::rank_rrr15_p)

#endif // QDAGS_EXAMPLES_CONST_RUNS_IMPL_HPP
