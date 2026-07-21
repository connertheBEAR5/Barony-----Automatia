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

/*-------------------------------------------------------------------------------

	lightSphereShadow

	Adds a circle of light to the lightmap at x and y with the supplied
	radius and color; casts shadows against walls

-------------------------------------------------------------------------------*/

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
	return lightSphereShadow(
		index,
		x,
		y,
		0,
		radius,
		r,
		g,
		b,
		a,
		exp
	);
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
	light_t* light =
		newLight(
			index,
			x,
			y,
			layer,
			radius
		);

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
			const int dxabs = abs(dx);
			const int dyabs = abs(dy);

			real_t a0 = dyabs * .5;
			real_t b0 = dxabs * .5;

			int u2 = u;
			int v2 = v;

			bool wallhit = true;

			const int mapindex =
				v * MAPLAYERS
				+ u * MAPLAYERS * map.height;

			for ( int z = 0;
				z < MAPLAYERS;
				++z )
			{
				if ( !map.tiles[mapindex + z]
					|| map.tiles[mapindex + z]
						== TRANSPARENT_TILE )
				{
					wallhit = false;
					break;
				}
			}

			if ( wallhit )
			{
				continue;
			}

			if ( dxabs >= dyabs )
			{
				for ( int i = 0;
					i < dxabs;
					++i )
				{
					u2 -= sgn(dx);
					b0 += dyabs;

					if ( b0 >= dxabs )
					{
						b0 -= dxabs;
						v2 -= sgn(dy);
					}

					if ( u2 >= 0
						&& u2 < map.width
						&& v2 >= 0
						&& v2 < map.height )
					{
						const int tileIndex =
							light->layer
							+ v2 * MAPLAYERS
							+ u2 * MAPLAYERS
								* map.height;

						if ( map.tiles[tileIndex]
							&& map.tiles[tileIndex]
								!= TRANSPARENT_TILE )
						{
							wallhit = true;
							break;
						}
					}
				}
			}
			else
			{
				for ( int i = 0;
					i < dyabs;
					++i )
				{
					v2 -= sgn(dy);
					a0 += dxabs;

					if ( a0 >= dyabs )
					{
						a0 -= dyabs;
						u2 -= sgn(dx);
					}

					if ( u2 >= 0
						&& u2 < map.width
						&& v2 >= 0
						&& v2 < map.height )
					{
						const int tileIndex =
							light->layer
							+ v2 * MAPLAYERS
							+ u2 * MAPLAYERS
								* map.height;

						if ( map.tiles[tileIndex]
							&& map.tiles[tileIndex]
								!= TRANSPARENT_TILE )
						{
							wallhit = true;
							break;
						}
					}
				}
			}

			if ( !wallhit
				|| (wallhit && u2 == u && v2 == v) )
			{
				const float distanceSquared =
					static_cast<float>(
						dx * dx + dy * dy
					);

				const float dist =
					exp != 1.f
					? powf(distanceSquared, exp)
					: distanceSquared;

				const float falloff =
					std::min<float>(
						dist / radius,
						1.0f
					);

				const int soff =
					(dy + radius)
					+ (dx + radius)
						* (radius * 2 + 1);

				auto& source =
					light->tiles[soff];

				source.x += r - r * falloff;
				source.y += g - g * falloff;
				source.z += b - b * falloff;
				source.w += a - a * falloff;

				const size_t doff =
					lightmapIndex3D(
						u,
						v,
						light->layer,
						map.width,
						map.height
					);

				if ( index )
				{
					auto& destination =
						lightmaps[index][doff];

					destination.x += source.x;
					destination.y += source.y;
					destination.z += source.z;
					destination.w += source.w;
				}
				else
				{
					for ( int player = 0;
						player < MAXPLAYERS + 1;
						++player )
					{
						auto& destination =
							lightmaps[player][doff];

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
	return lightSphere(
		index,
		x,
		y,
		0,
		radius,
		r,
		g,
		b,
		a,
		exp
	);
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
	light_t* light =
		newLight(
			index,
			x,
			y,
			layer,
			radius
		);

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

			const float distanceSquared =
				static_cast<float>(
					dx * dx + dy * dy
				);

			const float dist =
				exp != 1.f
					? powf(distanceSquared, exp)
					: distanceSquared;

			const float falloff =
				std::min<float>(
					dist / radius,
					1.0f
				);

			const int soff =
				(dy + radius)
				+ (dx + radius)
					* (radius * 2 + 1);

			auto& source =
				light->tiles[soff];

			source.x += r - r * falloff;
			source.y += g - g * falloff;
			source.z += b - b * falloff;
			source.w += a - a * falloff;

			const size_t doff =
				lightmapIndex3D(
					u,
					v,
					light->layer,
					map.width,
					map.height
				);

			if ( index )
			{
				auto& destination =
					lightmaps[index][doff];

				destination.x += source.x;
				destination.y += source.y;
				destination.z += source.z;
				destination.w += source.w;
			}
			else
			{
				for ( int player = 0;
					player < MAXPLAYERS + 1;
					++player )
				{
					auto& destination =
						lightmaps[player][doff];

					destination.x += source.x;
					destination.y += source.y;
					destination.z += source.z;
					destination.w += source.w;
				}
			}
		}
	}

	return light;
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

light_t* addLight(Sint32 x, Sint32 y, const char* name, int range_bonus, int index) {
    if (!name || !name[0]) {
        return nullptr;
    }
    auto find = lightDefs.find(name);
    if (find == lightDefs.end()) {
        return nullptr;
    }
    const auto& def = find->second;
    if (def.shadows) {
        return lightSphereShadow(index, x, y, std::max(def.radius + range_bonus, 1), def.r, def.g, def.b, def.a, def.falloff_exp);
    } else {
        return lightSphere(index, x, y, std::max(def.radius + range_bonus, 1), def.r, def.g, def.b, def.a, def.falloff_exp);
    }
}
