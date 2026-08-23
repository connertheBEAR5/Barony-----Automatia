/*-------------------------------------------------------------------------------

	BARONY
	File: light.cpp
	Desc: light spawning code

	Copyright 2013-2016 (c) Turning Wheel LLC, all rights reserved.
	See LICENSE for details.

-------------------------------------------------------------------------------*/

#include "main.hpp"
#include "light.hpp"
#include "draw.hpp"

AdditionalPlayableFloorLightmaps additionalPlayableFloorLightmaps;

namespace
{
void ensurePlayableFloorLightmapDimensions(
	PlayableFloorLightmapBuffers& buffers,
	int width,
	int height)
{
	const std::size_t rawSize = lightmapSize3D(width, height);
	const std::size_t smoothedSize = lightmapSmoothedSize3D(width, height);
	for ( int index = 0; index < MAXPLAYERS + 1; ++index )
	{
		if ( buffers.lightmap[index].size() != rawSize )
		{
			buffers.lightmap[index].assign(rawSize, vec4_t{});
		}
		if ( buffers.lightmapSmoothed[index].size() != smoothedSize )
		{
			buffers.lightmapSmoothed[index].assign(smoothedSize, vec4_t{});
		}
	}
}
}

std::vector<vec4_t>& lightmapForPlayableFloor(
	int index,
	PlayableFloorId playableFloor,
	int width,
	int height)
{
	index = std::max(0, std::min(index, MAXPLAYERS));
	if ( playableFloor == DEFAULT_PLAYABLE_FLOOR
		|| map.playableFloorUsesAuthoredLayerStack(playableFloor) )
	{
		/*
		 * Layer-authored floors share one world-space light volume. A floor
		 * transition changes collision height, not which lights exist. Legacy
		 * explicit FLOR geometry keeps its isolated light buffers.
		 */
		const std::size_t expected = lightmapSize3D(width, height);
		if ( lightmaps[index].size() != expected )
		{
			lightmaps[index].assign(expected, vec4_t{});
		}
		return lightmaps[index];
	}

	auto& buffers = additionalPlayableFloorLightmaps[playableFloor];
	ensurePlayableFloorLightmapDimensions(buffers, width, height);
	return buffers.lightmap[index];
}

std::vector<vec4_t>& lightmapSmoothedForPlayableFloor(
	int index,
	PlayableFloorId playableFloor,
	int width,
	int height)
{
	index = std::max(0, std::min(index, MAXPLAYERS));
	if ( playableFloor == DEFAULT_PLAYABLE_FLOOR
		|| map.playableFloorUsesAuthoredLayerStack(playableFloor) )
	{
		const std::size_t expected = lightmapSmoothedSize3D(width, height);
		if ( lightmapsSmoothed[index].size() != expected )
		{
			lightmapsSmoothed[index].assign(expected, vec4_t{});
		}
		return lightmapsSmoothed[index];
	}

	auto& buffers = additionalPlayableFloorLightmaps[playableFloor];
	ensurePlayableFloorLightmapDimensions(buffers, width, height);
	return buffers.lightmapSmoothed[index];
}

void clearAdditionalPlayableFloorLightmaps()
{
	additionalPlayableFloorLightmaps.clear();
}

void swapAdditionalPlayableFloorLightmaps(
	AdditionalPlayableFloorLightmaps& other)
{
	additionalPlayableFloorLightmaps.swap(other);
}

/*-------------------------------------------------------------------------------

	lightSphereShadow

	Adds a circle of light to the lightmap at x and y with the supplied
	radius and color; casts shadows against walls

-------------------------------------------------------------------------------*/
static inline bool lightWallBlocksAtLayer(
	int x,
	int y,
	int layer,
	PlayableFloorId playableFloor
)
{
	if ( x < 0
		|| y < 0
		|| x >= map.width
		|| y >= map.height )
	{
		return true;
	}

	layer = clampLightmapLayer(layer);

	const bool authoredStackLighting =
		map.playableFloorUsesAuthoredLayerStack(playableFloor)
		&& map.hasAuthoredPlayableFloorStack();
	int blockingLayer = layer == 0 ? OBSTACLELAYER : layer;
	if ( authoredStackLighting )
	{
		/*
		 * A structural light slice N illuminates the walkable space above
		 * authored floor N. Its horizontal walls therefore live on N +
		 * OBSTACLELAYER. Treating authored floor N itself as the wall made an
		 * upstairs light stop at the first otherwise ordinary floor tile.
		 * Keep the legacy layer-N rule for ordinary one-floor maps (including
		 * main-menu maps), where nonzero light layers predate playable Z and
		 * layer N + 1 may be a ceiling rather than that light's wall mask.
		 */
		blockingLayer = layer + OBSTACLELAYER;
		if ( blockingLayer >= MAPLAYERS )
		{
			return false;
		}
	}

	Sint32 tile = 0;
	if ( authoredStackLighting )
	{
		/*
		 * `layer` and blockingLayer are absolute authored structural indices.
		 * map.tileAt(..., playableFloor) accepts a floor-relative layer for a
		 * derived floor, so using it here would add playableFloor a second time
		 * (floor 1 wall layer 2 would incorrectly read authored layer 3).
		 */
		const Sint32* structuralTiles =
			map.tilesForPlayableFloorRendering(playableFloor);
		if ( !structuralTiles )
		{
			return true;
		}
		const std::size_t index = static_cast<std::size_t>(blockingLayer)
			+ static_cast<std::size_t>(y) * MAPLAYERS
			+ static_cast<std::size_t>(x) * MAPLAYERS * map.height;
		tile = structuralTiles[index];
	}
	else
	{
		tile = map.tileAt(x, y, blockingLayer, playableFloor);
	}

	return tile != 0
		&& tile != TRANSPARENT_TILE;
}

static bool lightTileVisibleAtLayer(
	const int sourceX,
	const int sourceY,
	const int targetX,
	const int targetY,
	const int layer,
	const PlayableFloorId playableFloor)
{
	const int dx = targetX - sourceX;
	const int dy = targetY - sourceY;
	const int dxabs = abs(dx);
	const int dyabs = abs(dy);
	real_t a0 = dyabs * .5;
	real_t b0 = dxabs * .5;
	int rayX = targetX;
	int rayY = targetY;

	if ( dxabs >= dyabs )
	{
		for ( int i = 0; i < dxabs; ++i )
		{
			rayX -= sgn(dx);
			b0 += dyabs;
			if ( b0 >= dxabs )
			{
				b0 -= dxabs;
				rayY -= sgn(dy);
			}
			if ( lightWallBlocksAtLayer(
					rayX, rayY, layer, playableFloor) )
			{
				return rayX == targetX && rayY == targetY;
			}
		}
	}
	else
	{
		for ( int i = 0; i < dyabs; ++i )
		{
			rayY -= sgn(dy);
			a0 += dxabs;
			if ( a0 >= dyabs )
			{
				a0 -= dyabs;
				rayX -= sgn(dx);
			}
			if ( lightWallBlocksAtLayer(
					rayX, rayY, layer, playableFloor) )
			{
				return rayX == targetX && rayY == targetY;
			}
		}
	}
	return true;
}

static bool lightCrossesOpenStructuralLayers(
	const int sourceX,
	const int sourceY,
	const int targetX,
	const int targetY,
	const int sourceLayer,
	const int targetLayer,
	const PlayableFloorId playableFloor)
{
	if ( sourceLayer == targetLayer )
	{
		return true;
	}
	if ( !map.playableFloorUsesAuthoredLayerStack(playableFloor) )
	{
		return false;
	}
	if ( !map.hasAuthoredPlayableFloorStack() )
	{
		return false;
	}
	const Sint32* structuralTiles =
		map.tilesForPlayableFloorRendering(playableFloor);
	if ( !structuralTiles )
	{
		return false;
	}

	const int direction = targetLayer > sourceLayer ? 1 : -1;
	const int crossings = abs(targetLayer - sourceLayer);
	for ( int step = 1; step <= crossings; ++step )
	{
		/*
		 * Sample each crossed structural plane along the 3D ray, rather than
		 * copying a lower light straight upward at the target column. This lets
		 * openings form attenuated cones while solid authored floors/ceilings
		 * continue to block light transport.
		 */
		const real_t interpolation =
			(static_cast<real_t>(step) - 0.5) / crossings;
		const int sampleX = static_cast<int>(std::floor(
			sourceX + (targetX - sourceX) * interpolation + 0.5));
		const int sampleY = static_cast<int>(std::floor(
			sourceY + (targetY - sourceY) * interpolation + 0.5));
		if ( sampleX < 0 || sampleY < 0
			|| sampleX >= map.width || sampleY >= map.height )
		{
			return false;
		}
		const int boundaryLayer = direction > 0
			? sourceLayer + step
			: sourceLayer - step + 1;
		const Sint32 boundaryTile = structuralTiles[
			static_cast<std::size_t>(boundaryLayer)
			+ static_cast<std::size_t>(sampleY) * MAPLAYERS
			+ static_cast<std::size_t>(sampleX) * MAPLAYERS * map.height];
		if ( boundaryTile != 0 && boundaryTile != TRANSPARENT_TILE )
		{
			return false;
		}
	}
	return true;
}

static void prepareLightContributionLayers(light_t& light, const float falloffExp)
{
	if ( light.radius <= 0
		|| !map.playableFloorUsesAuthoredLayerStack(light.playableFloor)
		|| !map.hasAuthoredPlayableFloorStack() )
	{
		return;
	}

	int count = 1;
	for ( int candidate = 0; candidate < MAPLAYERS; ++candidate )
	{
		if ( candidate == light.layer )
		{
			continue;
		}
		const float verticalDistance = powf(
			static_cast<float>((candidate - light.layer) * (candidate - light.layer)),
			falloffExp);
		if ( verticalDistance < light.radius )
		{
			light.contributionLayers[count++] = candidate;
		}
	}
	if ( count <= 1 )
	{
		return;
	}

	const std::size_t tilesPerLayer = static_cast<std::size_t>(
		(light.radius * 2 + 1) * (light.radius * 2 + 1));
	const std::size_t oldBytes = sizeof(vec4_t) * tilesPerLayer;
	const std::size_t newBytes = oldBytes * static_cast<std::size_t>(count);
	void* resized = std::realloc(light.tiles, newBytes);
	if ( !resized )
	{
		printlog("failed to expand structural light contribution storage!\n");
		exit(1);
	}
	light.tiles = static_cast<vec4_t*>(resized);
	memset(
		reinterpret_cast<Uint8*>(light.tiles) + oldBytes,
		0,
		newBytes - oldBytes);
	light.contributionLayerCount = count;
}
light_t* lightSphereShadow(
	int index,
	Sint32 x,
	Sint32 y,
	Sint32 radius,
	float r,
	float g,
	float b,
	float a,
	float exp
)
{
	return lightSphereShadowOnPlayableFloor(
		index,
		x,
		y,
		activeRuntimePlayableFloor(),
		activeRuntimeStructuralMapLayer(),
		radius,
		r,
		g,
		b,
		a,
		exp
	);
}

light_t* lightSphereShadowOnPlayableFloor(
	int index,
	Sint32 x,
	Sint32 y,
	PlayableFloorId playableFloor,
	Sint32 layer,
	Sint32 radius,
	float r,
	float g,
	float b,
	float a,
	float exp
)
{
	light_t* light =
		newLightOnPlayableFloor(
			index,
			x,
			y,
			playableFloor,
			layer,
			radius
		);
	prepareLightContributionLayers(*light, exp);

	r *= 255.f;
	g *= 255.f;
	b *= 255.f;
	a *= 255.f;

	for ( int v = y - radius;
		v <= y + radius;
		++v )
	{
		for ( int u = x - radius;
			u <= x + radius;
			++u )
		{
			if ( u < 0
				|| v < 0
				|| u >= map.width
				|| v >= map.height )
			{
				continue;
			}

			const int dx = u - x;
			const int dy = v - y;
			if ( !lightTileVisibleAtLayer(
					x, y, u, v, light->layer, light->playableFloor) )
			{
				continue;
			}

			const int localOffset =
				(dy + radius)
				+ (dx + radius) * (radius * 2 + 1);
			const int tilesPerLayer =
				(radius * 2 + 1) * (radius * 2 + 1);
			for ( int contribution = 0;
				contribution < light->contributionLayerCount;
				++contribution )
			{
				const int targetLayer =
					light->contributionLayers[contribution];
				if ( targetLayer != light->layer
					&& (!lightCrossesOpenStructuralLayers(
						x, y, u, v, light->layer, targetLayer,
						light->playableFloor)
						|| !lightTileVisibleAtLayer(
							x, y, u, v, targetLayer,
							light->playableFloor)) )
				{
					continue;
				}

				const int dz = targetLayer - light->layer;
				const float distanceSquared = static_cast<float>(
					dx * dx + dy * dy + dz * dz);
				const float distance = exp != 1.f
					? powf(distanceSquared, exp)
					: distanceSquared;
				const float falloff = std::min<float>(
					distance / radius, 1.0f);
				if ( falloff >= 1.f )
				{
					continue;
				}

				auto& source = light->tiles[
					contribution * tilesPerLayer + localOffset];
				source.x += r - r * falloff;
				source.y += g - g * falloff;
				source.z += b - b * falloff;
				source.w += a - a * falloff;

				const size_t doff = lightmapIndex3D(
					u, v, targetLayer, map.width, map.height);
				if ( index )
				{
					auto& destination = lightmapForPlayableFloor(
						index, light->playableFloor, map.width, map.height)[doff];
					destination.x += source.x;
					destination.y += source.y;
					destination.z += source.z;
					destination.w += source.w;
				}
				else
				{
					for ( int player = 0; player < MAXPLAYERS + 1; ++player )
					{
						auto& destination = lightmapForPlayableFloor(
							player, light->playableFloor, map.width, map.height)[doff];
						destination.x += source.x;
						destination.y += source.y;
						destination.z += source.z;
						destination.w += source.w;
					}
				}
			}
		}
	}

	return light;
}

light_t* lightSphereShadow(
	int index,
	Sint32 x,
	Sint32 y,
	Sint32 layer,
	Sint32 radius,
	float r,
	float g,
	float b,
	float a,
	float exp
)
{
	return lightSphereShadowOnPlayableFloor(
		index, x, y, activeRuntimePlayableFloor(), layer, radius,
		r, g, b, a, exp);
}

/*-------------------------------------------------------------------------------

	lightSphere

	Adds a circle of light to the lightmap at x and y with the supplied
	radius and color; casts no shadows

-------------------------------------------------------------------------------*/

light_t* lightSphere(
	int index,
	Sint32 x,
	Sint32 y,
	Sint32 radius,
	float r,
	float g,
	float b,
	float a,
	float exp
)
{
	return lightSphereOnPlayableFloor(
		index,
		x,
		y,
		activeRuntimePlayableFloor(),
		activeRuntimeStructuralMapLayer(),
		radius,
		r,
		g,
		b,
		a,
		exp
	);
}

light_t* lightSphereOnPlayableFloor(
	int index,
	Sint32 x,
	Sint32 y,
	PlayableFloorId playableFloor,
	Sint32 layer,
	Sint32 radius,
	float r,
	float g,
	float b,
	float a,
	float exp
)
{
	light_t* light =
		newLightOnPlayableFloor(
			index,
			x,
			y,
			playableFloor,
			layer,
			radius
		);
	prepareLightContributionLayers(*light, exp);

	r *= 255.f;
	g *= 255.f;
	b *= 255.f;
	a *= 255.f;

	for ( int v = y - radius;
		v <= y + radius;
		++v )
	{
		for ( int u = x - radius;
			u <= x + radius;
			++u )
		{
			if ( u < 0
				|| v < 0
				|| u >= map.width
				|| v >= map.height )
			{
				continue;
			}

			const int dx = u - x;
			const int dy = v - y;

			const int localOffset =
				(dy + radius)
				+ (dx + radius)
					* (radius * 2 + 1);
			const int tilesPerLayer =
				(radius * 2 + 1) * (radius * 2 + 1);
			for ( int contribution = 0;
				contribution < light->contributionLayerCount;
				++contribution )
			{
				const int targetLayer =
					light->contributionLayers[contribution];
				if ( targetLayer != light->layer
					&& !lightCrossesOpenStructuralLayers(
						x, y, u, v, light->layer, targetLayer,
						light->playableFloor) )
				{
					continue;
				}
				const int dz = targetLayer - light->layer;
				const float distanceSquared = static_cast<float>(
					dx * dx + dy * dy + dz * dz);
				const float distance = exp != 1.f
					? powf(distanceSquared, exp)
					: distanceSquared;
				const float falloff = std::min<float>(
					distance / radius, 1.0f);
				if ( falloff >= 1.f )
				{
					continue;
				}
				auto& source = light->tiles[
					contribution * tilesPerLayer + localOffset];
				source.x += r - r * falloff;
				source.y += g - g * falloff;
				source.z += b - b * falloff;
				source.w += a - a * falloff;
				const size_t doff = lightmapIndex3D(
					u, v, targetLayer, map.width, map.height);
				if ( index )
				{
					auto& destination = lightmapForPlayableFloor(
						index, light->playableFloor, map.width, map.height)[doff];
					destination.x += source.x;
					destination.y += source.y;
					destination.z += source.z;
					destination.w += source.w;
				}
				else
				{
					for ( int player = 0; player < MAXPLAYERS + 1; ++player )
					{
						auto& destination = lightmapForPlayableFloor(
							player, light->playableFloor, map.width, map.height)[doff];
						destination.x += source.x;
						destination.y += source.y;
						destination.z += source.z;
						destination.w += source.w;
					}
				}
			}
		}
	}

	return light;
}
light_t* lightSphere(
	int index,
	Sint32 x,
	Sint32 y,
	Sint32 layer,
	Sint32 radius,
	float r,
	float g,
	float b,
	float a,
	float exp
)
{
	return lightSphereOnPlayableFloor(
		index, x, y, activeRuntimePlayableFloor(), layer, radius,
		r, g, b, a, exp);
}

#include "rapidjson/document.h"
#include "rapidjson/filereadstream.h"
#include "files.hpp"

std::unordered_map<std::string, LightDef> lightDefs;
bool loadLights(bool forceLoadBaseDirectory) {
    if ( !PHYSFS_getRealDir("/data/lights.json") )
    {
        printlog("[JSON]: Error: Could not find file: data/lights.json");
        return false;
    }

    std::string inputPath = PHYSFS_getRealDir("/data/lights.json");
    if ( forceLoadBaseDirectory )
    {
        inputPath = BASE_DATA_DIR;
    }
    else
    {
        if ( inputPath != BASE_DATA_DIR )
        {
            loadLights(true); // force load the base directory first, then modded paths later.
        }
        else
        {
            forceLoadBaseDirectory = true;
        }
    }

    inputPath.append("/data/lights.json");

    File* fp = FileIO::open(inputPath.c_str(), "rb");
    if ( !fp )
    {
        printlog("[JSON]: Error: Could not open json file %s", inputPath.c_str());
        return false;
    }

    if ( forceLoadBaseDirectory )
    {
        lightDefs.clear();
    }
    
    char buf[65536];
    int count = (int)fp->read(buf, sizeof(buf[0]), sizeof(buf));
    buf[count] = '\0';
    rapidjson::StringStream is(buf);
    FileIO::close(fp);

    rapidjson::Document d;
    d.ParseStream(is);
    
    const auto& lights = d["lights"];
    if (lights.IsObject()) {
        for (const auto& it : lights.GetObject()) {
            LightDef def;
            const auto& name = it.name.GetString();
            const auto& radius = it.value["radius"]; def.radius = radius.GetInt();
            const auto& r = it.value["r"]; def.r = r.GetFloat();
            const auto& g = it.value["g"]; def.g = g.GetFloat();
            const auto& b = it.value["b"]; def.b = b.GetFloat();
            if ( it.value.HasMember("a") )
            {
                def.a = it.value["a"].GetFloat();
            }
            else
            {
                def.a = 0.f;
            }
            const auto& exp = it.value["falloff_exp"]; def.falloff_exp = exp.GetFloat();
            const auto& shadows = it.value["shadows"]; def.shadows = shadows.GetBool();
            lightDefs[name] = def;
        }
    }
    
    return true;
}

#ifndef EDITOR
#include "interface/consolecommand.hpp"
static ConsoleCommand ccmd_reloadLights("/reloadlights", "reload light json",
    [](int argc, const char* argv[]){
    loadLights();
    });
#endif

light_t* addLight(
	Sint32 x,
	Sint32 y,
	const char* name,
	int range_bonus,
	int index
)
{
	return addLight(
		x,
		y,
		activeRuntimeStructuralMapLayer(),
		name,
		range_bonus,
		index
	);
}

light_t* addLight(
	Sint32 x,
	Sint32 y,
	Sint32 layer,
	const char* name,
	int range_bonus,
	int index
)
{
	return addLightOnPlayableFloor(
		x, y, activeRuntimePlayableFloor(), layer, name, range_bonus, index);
}

light_t* addLightOnPlayableFloor(
	Sint32 x,
	Sint32 y,
	PlayableFloorId playableFloor,
	Sint32 layer,
	const char* name,
	int range_bonus,
	int index
)
{
	if ( !name || !name[0] )
	{
		return nullptr;
	}

	auto find = lightDefs.find(name);
	if ( find == lightDefs.end() )
	{
		return nullptr;
	}

	layer = clampLightmapLayer(layer);

	const auto& def = find->second;
	const Sint32 radius =
		std::max(def.radius + range_bonus, 1);

	if ( def.shadows )
	{
		return lightSphereShadowOnPlayableFloor(
			index,
			x,
			y,
			playableFloor,
			layer,
			radius,
			def.r,
			def.g,
			def.b,
			def.a,
			def.falloff_exp
		);
	}
	else
	{
		return lightSphereOnPlayableFloor(
			index,
			x,
			y,
			playableFloor,
			layer,
			radius,
			def.r,
			def.g,
			def.b,
			def.a,
			def.falloff_exp
		);
	}
}
