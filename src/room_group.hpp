/*-------------------------------------------------------------------------------

	Automatia authored Room Groups

	A Room Group is editor metadata: a stable name plus an X/Y/authored-layer
	cuboid and a tile/sprite content mask.  It never encodes Entity::z or a
	playable floor; those remain properties of the entities contained by the
	group.  The fixed-capacity POD collection is intentional because the legacy
	Zed undo stack snapshots map_t storage without running C++ constructors.

-------------------------------------------------------------------------------*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

constexpr std::size_t AUTHORED_ROOM_GROUP_MAX_COUNT = 128;
constexpr std::size_t AUTHORED_ROOM_GROUP_NAME_BYTES = 64;

enum AuthoredRoomGroupContent : std::uint8_t
{
	AUTHORED_ROOM_GROUP_TILES = 1u << 0,
	AUTHORED_ROOM_GROUP_SPRITES = 1u << 1,
	AUTHORED_ROOM_GROUP_BOTH = AUTHORED_ROOM_GROUP_TILES
		| AUTHORED_ROOM_GROUP_SPRITES
};

struct AuthoredRoomGroup
{
	std::uint32_t id;
	std::int32_t x1;
	std::int32_t y1;
	std::int32_t x2;
	std::int32_t y2;
	std::int16_t bottomLayer;
	std::int16_t topLayer;
	std::uint8_t contentMask;
	std::uint8_t reserved[3];
	char name[AUTHORED_ROOM_GROUP_NAME_BYTES];
};

struct AuthoredRoomGroupCollection
{
	std::uint32_t nextID;
	std::uint32_t count;
	AuthoredRoomGroup entries[AUTHORED_ROOM_GROUP_MAX_COUNT];
};

void authoredRoomGroupsReset(AuthoredRoomGroupCollection& groups);

bool authoredRoomGroupNameIsSafe(const char* name);

int authoredRoomGroupFindByID(
	const AuthoredRoomGroupCollection& groups,
	std::uint32_t id);

int authoredRoomGroupAdd(
	AuthoredRoomGroupCollection& groups,
	const char* requestedName,
	std::int32_t x1,
	std::int32_t y1,
	std::int32_t x2,
	std::int32_t y2,
	std::int16_t bottomLayer,
	std::int16_t topLayer,
	std::uint8_t contentMask);

bool authoredRoomGroupUpdate(
	AuthoredRoomGroupCollection& groups,
	std::uint32_t id,
	const char* requestedName,
	std::int32_t x1,
	std::int32_t y1,
	std::int32_t x2,
	std::int32_t y2,
	std::int16_t bottomLayer,
	std::int16_t topLayer,
	std::uint8_t contentMask);

bool authoredRoomGroupRemove(
	AuthoredRoomGroupCollection& groups,
	std::uint32_t id);

void authoredRoomGroupsClampToMap(
	AuthoredRoomGroupCollection& groups,
	std::uint32_t width,
	std::uint32_t height,
	std::uint32_t numLayers);

bool authoredRoomGroupFullyInside(
	const AuthoredRoomGroup& group,
	std::int32_t x1,
	std::int32_t y1,
	std::int32_t x2,
	std::int32_t y2,
	std::int16_t bottomLayer,
	std::int16_t topLayer);

AuthoredRoomGroup authoredRoomGroupTranslated(
	const AuthoredRoomGroup& group,
	std::int32_t deltaX,
	std::int32_t deltaY,
	std::int16_t deltaLayer);

bool serializeAuthoredRoomGroups(
	const AuthoredRoomGroupCollection& groups,
	std::vector<std::uint8_t>& output);

bool deserializeAuthoredRoomGroups(
	const std::uint8_t* data,
	std::size_t size,
	std::uint32_t mapWidth,
	std::uint32_t mapHeight,
	std::uint32_t mapLayers,
	AuthoredRoomGroupCollection& output);
