/*-------------------------------------------------------------------------------

	BARONY
	File: light.hpp
	Desc: prototypes for light.cpp, light-related types and prototypes

	Copyright 2013-2016 (c) Turning Wheel LLC, all rights reserved.
	See LICENSE for details.

-------------------------------------------------------------------------------*/
#pragma once
// ============================================================
// LAYERED LIGHTMAP HELPERS
//
// Lightmap data is stored one complete 2D layer after another:
//
// layer 0: width * height entries
// layer 1: width * height entries
// layer 2: width * height entries
// ...
//
// Keeping layer 0 first preserves compatibility with code that
// still treats the lightmap as an ordinary 2D array.
// ============================================================

static inline int clampLightmapLayer(int layer)
{
	return std::max(
		0,
		std::min(layer, MAPLAYERS - 1)
	);
}

static inline size_t lightmapLayerArea(
	int width,
	int height
)
{
	return static_cast<size_t>(width)
		* static_cast<size_t>(height);
}

static inline size_t lightmapIndex3D(
	int x,
	int y,
	int layer,
	int width,
	int height
)
{
	layer = clampLightmapLayer(layer);

	return static_cast<size_t>(y)
		+ static_cast<size_t>(x) * height
		+ static_cast<size_t>(layer)
			* lightmapLayerArea(width, height);
}

static inline size_t lightmapSize3D(
	int width,
	int height
)
{
	return lightmapLayerArea(width, height)
		* MAPLAYERS;
}

static inline size_t lightmapSmoothedIndex3D(
	int x,
	int y,
	int layer,
	int width,
	int height
)
{
	const size_t paddedLayerArea =
		static_cast<size_t>(width + 2)
		* static_cast<size_t>(height + 2);

	return static_cast<size_t>(y)
		+ static_cast<size_t>(x) * (height + 2)
		+ static_cast<size_t>(clampLightmapLayer(layer))
			* paddedLayerArea;
}

static inline size_t lightmapSmoothedSize3D(
	int width,
	int height
)
{
	return static_cast<size_t>(width + 2)
		* static_cast<size_t>(height + 2)
		* MAPLAYERS;
}
static inline int entityZToLightmapLayer(real_t z)
{
	const int layer = static_cast<int>(
		std::round(-z / 16.0)
	);

	return clampLightmapLayer(layer);
}
typedef struct light_t
{
	Sint32 x, y;
	Sint32 radius;
	Sint32 layer;
	vec4_t* tiles;
	int index; // which lightmap this actually exists in

	// a pointer to the light's location in a list
	node_t* node;
} light_t;

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
);

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
);

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
);

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
);

light_t* newLight(
	int index,
	Sint32 x,
	Sint32 y,
	Sint32 radius
);

light_t* newLight(
	int index,
	Sint32 x,
	Sint32 y,
	Sint32 layer,
	Sint32 radius
);
bool loadLights(bool forceLoadBaseDirectory = false);
light_t* addLight(
	Sint32 x,
	Sint32 y,
	const char* name,
	int range_bonus = 0,
	int index = 0
);
struct LightDef {
    int radius = 0;
    float r = 0.f;
    float g = 0.f;
    float b = 0.f;
	float a = 0.f;
    float falloff_exp = 1.f;
    bool shadows = false;
};
extern std::unordered_map<std::string, LightDef> lightDefs;
