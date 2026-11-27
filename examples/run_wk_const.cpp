// Constantes de run_wk: aca, y solo aca, se instancia la estrategia sobre qdag_pin (ver const_runs.hpp).
#include "const_runs_impl.hpp"

#define QDAGS_INSTANTIATE(B) \
    template gdj_examples::ConstRunResult gdj_examples::run_wk_const<B>( \
        const gdj_examples::ConstRunArgs &, gdj::resource_manager<gdj::se_quadtree<B>> &, const gdj::query_tp &);
QDAGS_CONST_BACKENDS(QDAGS_INSTANTIATE)
#undef QDAGS_INSTANTIATE
