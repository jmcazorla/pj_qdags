#ifndef QDAGS_QDAG_VFAMILY_HPP
#define QDAGS_QDAG_VFAMILY_HPP

#include "qdag.hpp"
#include "se_quadtree_vfamily.hpp"
#include "rank_vfamily_p.hpp"

namespace gdj {
    template<class RankSupport = sdsl::rank_support_v5<>>
    using qdag_vfamily_p = qdag<rank_vfamily_p<RankSupport>>;

    using qdag_v5_p = qdag_vfamily_p<sdsl::rank_support_v5<>>;
    using qdag_v_p  = qdag_vfamily_p<sdsl::rank_support_v<>>;
}

#endif // QDAGS_QDAG_VFAMILY_HPP
