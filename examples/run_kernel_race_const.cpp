// Constantes de run_kernel_race: aca, y solo aca, se instancia la estrategia sobre qdag_pin (ver const_runs.hpp y const_runs_more.hpp).
#include "const_runs_more_impl.hpp"

#define QDAGS_INSTANTIATE(In, Out) \
    template gdj_examples::ConstRaceResult gdj_examples::run_kernel_race_const<In, Out>( \
        const gdj_examples::ConstRunArgs &, gdj::resource_manager<gdj::se_quadtree<In>> &, const gdj::query_tp &);
QDAGS_CONST_BACKEND_PAIRS(QDAGS_INSTANTIATE)
#undef QDAGS_INSTANTIATE
