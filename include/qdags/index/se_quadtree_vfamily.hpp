#ifndef QDAGS_SE_QUADTREE_VFAMILY_HPP
#define QDAGS_SE_QUADTREE_VFAMILY_HPP

#include "se_quadtree.hpp"
#include "rank_vfamily_p.hpp"

namespace gdj {
    template<class RankSupport = sdsl::rank_support_v5<>>
    using se_quadtree_vfamily_p = se_quadtree<rank_vfamily_p<RankSupport>>;

    using se_quadtree_v5_p = se_quadtree_vfamily_p<sdsl::rank_support_v5<>>;
    using se_quadtree_v_p = se_quadtree_vfamily_p<sdsl::rank_support_v<>>;
}

#endif // QDAGS_SE_QUADTREE_VFAMILY_HPP
