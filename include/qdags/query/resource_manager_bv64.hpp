#ifndef QDAGS_RESOURCE_MANAGER_BV64_HPP
#define QDAGS_RESOURCE_MANAGER_BV64_HPP

#include "resource_manager.hpp"
#include "../index/se_quadtree_bv64.hpp"

namespace gdj {
    using resource_manager_bv64 = resource_manager<se_quadtree_p>;
}

#endif // QDAGS_RESOURCE_MANAGER_BV64_HPP
