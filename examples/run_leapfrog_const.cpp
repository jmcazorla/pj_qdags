// Constantes de run_leapfrog: aca, y solo aca, se instancia strategy_leapfrog sobre qdag_pin (ver const_runs.hpp).
#include "const_runs_impl.hpp"

#define QDAGS_INSTANTIATE(In, Out) \
    template gdj_examples::ConstRunResult gdj_examples::run_leapfrog_const<In, Out>( \
        const gdj_examples::ConstRunArgs &, gdj::resource_manager<gdj::se_quadtree<In>> &, const gdj::query_tp &);
QDAGS_CONST_BACKEND_PAIRS(QDAGS_INSTANTIATE)
#undef QDAGS_INSTANTIATE
