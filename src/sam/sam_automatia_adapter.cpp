#include "sam_automatia_adapter.hpp"

#include "framework/sam_monsters.hpp"

#include "entity.hpp"
#include "game.hpp"
#include "items.hpp"
#include "main.hpp"
#include "playable_z.hpp"
#include "stat.hpp"

#include <cstring>

Entity* samSpawnGroundItem(
    const int runtimeItemType,
    const int status,
    const int beatitude,
    const int count,
    const int tileX,
    const int tileY)
{
    if ( runtimeItemType < 0
        || runtimeItemType >= NUM_ITEM_SLOTS
        || count <= 0
        || tileX < 0
        || tileY < 0
        || tileX >= static_cast<int>(map.width)
        || tileY >= static_cast<int>(map.height)
        || !map.entities )
    {
        return nullptr;
    }

    const PlayableFloorId floor = activeRuntimePlayableFloor();
    if ( map.tileAt(tileX, tileY, FLOORLAYER, floor) == 0
        || map.tileAt(tileX, tileY, OBSTACLELAYER, floor) != 0 )
    {
        return nullptr;
    }

    // newEntity() copies the complete thread-local SpatialSpawnContext: playable
    // floor, authored structural layer, and spatial revision. Entity::z remains a
    // local elevation and is never interpreted as either kind of layer.
    Entity* entity = newEntity(-1, 1, map.entities, nullptr);
    if ( !entity )
    {
        return nullptr;
    }

    entity->flags[INVISIBLE] = true;
    entity->flags[UPDATENEEDED] = true;
    entity->flags[PASSABLE] = true;
    entity->x = tileX * 16.0 + 8.0;
    entity->y = tileY * 16.0 + 8.0;
    entity->z = 0.0;
    entity->sizex = 4;
    entity->sizey = 4;
    entity->behavior = &actItem;
    entity->skill[10] = runtimeItemType;
    entity->skill[11] = status;
    entity->skill[12] = beatitude;
    entity->skill[13] = count;
    entity->skill[14] = 0;
    entity->skill[15] = 1;
    entity->parent = 0;
    entity->itemOriginalOwner = 0;
    return entity;
}

bool samMonsterHasTrait(const Stat* stats, const std::uint64_t traitBit)
{
    if ( !stats || traitBit == 0 )
    {
        return false;
    }
    return (SAMMonsters::traitsForName(stats->name) & traitBit) != 0;
}
