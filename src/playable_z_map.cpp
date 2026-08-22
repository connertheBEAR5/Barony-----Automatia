/*-------------------------------------------------------------------------------

	BARONY
	File: playable_z_map.cpp
	Desc: Shared playable-floor map geometry accessors for game and editor

-------------------------------------------------------------------------------*/

#include "main.hpp"

std::size_t map_t::playableFloorTileCount() const
{
	return static_cast<std::size_t>(width)
		* static_cast<std::size_t>(height)
		* static_cast<std::size_t>(MAPLAYERS);
}

const Sint32* map_t::tilesForPlayableFloor(const PlayableFloorId playableFloor) const
{
	if ( playableFloor == DEFAULT_PLAYABLE_FLOOR )
	{
		return tiles;
	}

	const PlayableFloorData* floor = playableFloors.find(playableFloor);
	if ( !floor || floor->tiles.size() != playableFloorTileCount() )
	{
		return nullptr;
	}
	return reinterpret_cast<const Sint32*>(floor->tiles.data());
}

Sint32* map_t::tilesForPlayableFloor(const PlayableFloorId playableFloor)
{
	if ( playableFloor == DEFAULT_PLAYABLE_FLOOR )
	{
		return tiles;
	}

	PlayableFloorData* floor = playableFloors.find(playableFloor);
	if ( !floor || floor->tiles.size() != playableFloorTileCount() )
	{
		return nullptr;
	}
	return reinterpret_cast<Sint32*>(floor->tiles.data());
}

bool map_t::playableFloorUsesAuthoredLayerStack(
	const PlayableFloorId playableFloor) const
{
	if ( playableFloor == DEFAULT_PLAYABLE_FLOOR )
	{
		return true;
	}
	const PlayableFloorData* floor = playableFloors.find(playableFloor);
	return floor && floor->derivedFromMapLayers;
}

const Sint32* map_t::tilesForPlayableFloorRendering(
	const PlayableFloorId playableFloor) const
{
	/*
	 * Z3.3C: a layer-authored floor is only a collision/interaction slice.
	 * Visually it remains part of the same authored 32-layer structure, so do
	 * not shift layer N down to render layer 0. Keeping map.tiles intact is what
	 * lets an upstairs camera still see the tower/rooms/floors beneath it.
	 */
	if ( playableFloorUsesAuthoredLayerStack(playableFloor) )
	{
		return tiles;
	}
	return tilesForPlayableFloor(playableFloor);
}

bool map_t::ensurePlayableFloorGeometry(
	const PlayableFloorId playableFloor,
	const bool copyDefaultGeometry)
{
	if ( playableFloor == DEFAULT_PLAYABLE_FLOOR )
	{
		return tiles != nullptr && playableFloorTileCount() > 0;
	}
	if ( playableFloor < 0 || playableFloor >= MAPLAYERS )
	{
		return false;
	}

	const std::size_t count = playableFloorTileCount();
	if ( count == 0 || !tiles )
	{
		return false;
	}

	PlayableFloorData* floor = playableFloors.find(playableFloor);
	if ( !floor )
	{
		PlayableFloorData newFloor;
		newFloor.id = playableFloor;
		newFloor.derivedFromMapLayers = !copyDefaultGeometry;
		if ( !playableFloors.addFloor(std::move(newFloor)) )
		{
			return false;
		}
		floor = playableFloors.find(playableFloor);
	}
	if ( !floor )
	{
		return false;
	}

	if ( floor->tiles.size() != count )
	{
		floor->tiles.assign(count, 0);
	}

	if ( copyDefaultGeometry )
	{
		/* Preserve the older Z2 characterization/compatibility path. */
		floor->derivedFromMapLayers = false;
		for ( std::size_t index = 0; index < count; ++index )
		{
			floor->tiles[index] = static_cast<std::int32_t>(tiles[index]);
		}
		return true;
	}

	/*
	 * Layer-authored runtime floor view. For playable floor N, relative map
	 * layer 0 reads authored layer N, relative layer 1 reads N+1, etc. This is
	 * the bridge from the old Barony 32-layer editor stack to stacked gameplay:
	 * the wall layer of one floor is naturally the floor layer of the next.
	 */
	floor->derivedFromMapLayers = true;
	for ( int x = 0; x < static_cast<int>(width); ++x )
	{
		for ( int y = 0; y < static_cast<int>(height); ++y )
		{
			for ( int relativeLayer = 0; relativeLayer < MAPLAYERS; ++relativeLayer )
			{
				const int authoredLayer = relativeLayer + playableFloor;
				const std::size_t destinationIndex =
					static_cast<std::size_t>(relativeLayer)
					+ static_cast<std::size_t>(y) * MAPLAYERS
					+ static_cast<std::size_t>(x) * MAPLAYERS * height;
				if ( authoredLayer >= MAPLAYERS )
				{
					floor->tiles[destinationIndex] = 0;
					continue;
				}
				const std::size_t sourceIndex =
					static_cast<std::size_t>(authoredLayer)
					+ static_cast<std::size_t>(y) * MAPLAYERS
					+ static_cast<std::size_t>(x) * MAPLAYERS * height;
				floor->tiles[destinationIndex] = static_cast<std::int32_t>(tiles[sourceIndex]);
			}
		}
	}
	return true;
}

bool map_t::findLowerPlayableFloorLanding(
	const int x,
	const int y,
	const PlayableFloorId fromFloor,
	PlayableFloorId& landingFloor,
	int& floorsFallen) const
{
	landingFloor = DEFAULT_PLAYABLE_FLOOR;
	floorsFallen = 0;
	if ( !tiles
		|| x < 0 || y < 0
		|| x >= static_cast<int>(width)
		|| y >= static_cast<int>(height)
		|| fromFloor <= DEFAULT_PLAYABLE_FLOOR
		|| fromFloor >= MAPLAYERS
		|| !playableFloorUsesAuthoredLayerStack(fromFloor) )
	{
		return false;
	}

	/*
	 * In the authored stack, playable floor N is authored tile layer N and its
	 * obstacle layer is N+1. Search downward for the first solid floor whose
	 * overlapping obstacle layer is empty. No runtime FLOR object is required
	 * yet; the landing transaction will create the derived floor view.
	 */
	for ( int candidate = static_cast<int>(fromFloor) - 1;
		candidate >= 0; --candidate )
	{
		const std::size_t floorIndex =
			static_cast<std::size_t>(candidate)
			+ static_cast<std::size_t>(y) * MAPLAYERS
			+ static_cast<std::size_t>(x) * MAPLAYERS * height;
		const int obstacleLayer = candidate + OBSTACLELAYER;
		const Sint32 floorTile = tiles[floorIndex];
		Sint32 obstacleTile = 0;
		if ( obstacleLayer >= 0 && obstacleLayer < MAPLAYERS )
		{
			const std::size_t obstacleIndex =
				static_cast<std::size_t>(obstacleLayer)
				+ static_cast<std::size_t>(y) * MAPLAYERS
				+ static_cast<std::size_t>(x) * MAPLAYERS * height;
			obstacleTile = tiles[obstacleIndex];
		}

		if ( floorTile != 0 && obstacleTile == 0 )
		{
			landingFloor = static_cast<PlayableFloorId>(candidate);
			floorsFallen = static_cast<int>(fromFloor) - candidate;
			return floorsFallen > 0;
		}
	}

	return false;
}

Sint32 map_t::tileAt(
	const int x,
	const int y,
	const int layer,
	const PlayableFloorId playableFloor) const
{
	if ( x < 0 || y < 0 || layer < 0
		|| x >= static_cast<int>(width)
		|| y >= static_cast<int>(height)
		|| layer >= MAPLAYERS )
	{
		return 0;
	}

	if ( playableFloor != DEFAULT_PLAYABLE_FLOOR )
	{
		const PlayableFloorData* floor = playableFloors.find(playableFloor);
		if ( floor && floor->derivedFromMapLayers )
		{
			const int authoredLayer = layer + playableFloor;
			if ( authoredLayer < 0 || authoredLayer >= MAPLAYERS || !tiles )
			{
				return 0;
			}
			const std::size_t authoredIndex = static_cast<std::size_t>(authoredLayer)
				+ static_cast<std::size_t>(y) * MAPLAYERS
				+ static_cast<std::size_t>(x) * MAPLAYERS * height;
			return tiles[authoredIndex];
		}
	}

	const Sint32* floorTiles = tilesForPlayableFloor(playableFloor);
	if ( !floorTiles )
	{
		return 0;
	}
	const std::size_t index = static_cast<std::size_t>(layer)
		+ static_cast<std::size_t>(y) * MAPLAYERS
		+ static_cast<std::size_t>(x) * MAPLAYERS * height;
	return floorTiles[index];
}

bool map_t::setTileAt(
	const int x,
	const int y,
	const int layer,
	const Sint32 tile,
	const PlayableFloorId playableFloor)
{
	if ( x < 0 || y < 0 || layer < 0
		|| x >= static_cast<int>(width)
		|| y >= static_cast<int>(height)
		|| layer >= MAPLAYERS )
	{
		return false;
	}
	if ( playableFloor != DEFAULT_PLAYABLE_FLOOR )
	{
		PlayableFloorData* existingFloor = playableFloors.find(playableFloor);
		if ( (!existingFloor
				|| existingFloor->tiles.size() != playableFloorTileCount())
			&& !ensurePlayableFloorGeometry(
				playableFloor,
				existingFloor && !existingFloor->derivedFromMapLayers) )
		{
			return false;
		}
	}

	PlayableFloorData* floor = playableFloor == DEFAULT_PLAYABLE_FLOOR
		? nullptr
		: playableFloors.find(playableFloor);
	if ( floor && floor->derivedFromMapLayers )
	{
		const int authoredLayer = layer + playableFloor;
		if ( authoredLayer < 0 || authoredLayer >= MAPLAYERS || !tiles )
		{
			return false;
		}
		const std::size_t authoredIndex = static_cast<std::size_t>(authoredLayer)
			+ static_cast<std::size_t>(y) * MAPLAYERS
			+ static_cast<std::size_t>(x) * MAPLAYERS * height;
		tiles[authoredIndex] = tile;

		/* Keep every already-created derived render view coherent. */
		for ( PlayableFloorData& derived : playableFloors.floors )
		{
			if ( !derived.derivedFromMapLayers || derived.tiles.size() != playableFloorTileCount() )
			{
				continue;
			}
			const int relativeLayer = authoredLayer - derived.id;
			if ( relativeLayer < 0 || relativeLayer >= MAPLAYERS )
			{
				continue;
			}
			const std::size_t derivedIndex = static_cast<std::size_t>(relativeLayer)
				+ static_cast<std::size_t>(y) * MAPLAYERS
				+ static_cast<std::size_t>(x) * MAPLAYERS * height;
			derived.tiles[derivedIndex] = tile;
		}
		return true;
	}

	Sint32* floorTiles = tilesForPlayableFloor(playableFloor);
	if ( !floorTiles )
	{
		return false;
	}
	const std::size_t index = static_cast<std::size_t>(layer)
		+ static_cast<std::size_t>(y) * MAPLAYERS
		+ static_cast<std::size_t>(x) * MAPLAYERS * height;
	floorTiles[index] = tile;
	return true;
}

bool map_t::tileHasAttribute(int x, int y, int layer, Uint32 attribute)
{
	return tileHasAttribute(x, y, layer, attribute, DEFAULT_PLAYABLE_FLOOR);
}

bool map_t::tileHasAttribute(
	const int x,
	const int y,
	const int layer,
	const Uint32 attribute,
	const PlayableFloorId playableFloor) const
{
	if ( x < 0 || y < 0 || layer < 0
		|| x >= static_cast<int>(width)
		|| y >= static_cast<int>(height)
		|| layer >= MAPLAYERS )
	{
		return false;
	}
	const Sint32 key = layer + y * MAPLAYERS + x * MAPLAYERS * height;
	if ( playableFloor == DEFAULT_PLAYABLE_FLOOR )
	{
		auto find = tileAttributes.find(key);
		return find != tileAttributes.end() && (find->second & attribute);
	}
	const PlayableFloorData* floor = playableFloors.find(playableFloor);
	if ( !floor )
	{
		return false;
	}
	if ( floor->derivedFromMapLayers )
	{
		const int authoredLayer = layer + playableFloor;
		if ( authoredLayer < 0 || authoredLayer >= MAPLAYERS )
		{
			return false;
		}
		const Sint32 authoredKey = authoredLayer + y * MAPLAYERS + x * MAPLAYERS * height;
		auto find = tileAttributes.find(authoredKey);
		return find != tileAttributes.end() && (find->second & attribute);
	}
	auto find = floor->tileAttributes.find(key);
	return find != floor->tileAttributes.end() && (find->second & attribute);
}

void map_t::setTileAttribute(
	const int x,
	const int y,
	const int layer,
	const Uint32 attribute,
	const bool enabled,
	const PlayableFloorId playableFloor)
{
	if ( x < 0 || y < 0 || layer < 0
		|| x >= static_cast<int>(width)
		|| y >= static_cast<int>(height)
		|| layer >= MAPLAYERS )
	{
		return;
	}
	const Sint32 key = layer + y * MAPLAYERS + x * MAPLAYERS * height;
	if ( playableFloor == DEFAULT_PLAYABLE_FLOOR )
	{
		Uint32& value = tileAttributes[key];
		if ( enabled )
		{
			value |= attribute;
		}
		else
		{
			value &= ~attribute;
			if ( value == 0 )
			{
				tileAttributes.erase(key);
			}
		}
		return;
	}
	PlayableFloorData* existingFloor = playableFloors.find(playableFloor);
	if ( (!existingFloor
			|| existingFloor->tiles.size() != playableFloorTileCount())
		&& !ensurePlayableFloorGeometry(
			playableFloor,
			existingFloor && !existingFloor->derivedFromMapLayers) )
	{
		return;
	}
	PlayableFloorData* floor = playableFloors.find(playableFloor);
	if ( !floor )
	{
		return;
	}
	if ( floor->derivedFromMapLayers )
	{
		const int authoredLayer = layer + playableFloor;
		if ( authoredLayer < 0 || authoredLayer >= MAPLAYERS )
		{
			return;
		}
		const Sint32 authoredKey = authoredLayer + y * MAPLAYERS + x * MAPLAYERS * height;
		Uint32& value = tileAttributes[authoredKey];
		if ( enabled )
		{
			value |= attribute;
		}
		else
		{
			value &= ~attribute;
			if ( value == 0 )
			{
				tileAttributes.erase(authoredKey);
			}
		}
		return;
	}
	std::uint32_t& value = floor->tileAttributes[key];
	if ( enabled )
	{
		value |= attribute;
	}
	else
	{
		value &= ~attribute;
		if ( value == 0 )
		{
			floor->tileAttributes.erase(key);
		}
	}
}
