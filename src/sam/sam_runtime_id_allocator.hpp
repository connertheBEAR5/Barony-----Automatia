/* Lightweight stable-catalog allocation helper. Implementation consumers provide
 * the occupancy predicate, so this header knows nothing about JSON or registries. */
#pragma once

#include "sam_item_limits.hpp"

template <class IsUsed>
int firstAvailableSAMRuntimeItemId(IsUsed&& isUsed)
{
    for ( int id = SAM_ITEM_ID_BASE; id < SAM_ITEM_ID_LIMIT; ++id )
    {
        if ( !isUsed(id) )
        {
            return id;
        }
    }
    return -1;
}
