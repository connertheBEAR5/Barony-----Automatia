/*-------------------------------------------------------------------------------

	BARONY
	File: objects.cpp
	Desc: contains object constructors and deconstructors

	Copyright 2013-2016 (c) Turning Wheel LLC, all rights reserved.
	See LICENSE for details.

-------------------------------------------------------------------------------*/

#include "main.hpp"
#include <new>
#include "entity.hpp"
#include "messages.hpp"

/*-------------------------------------------------------------------------------

	defaultDeconstructor

	Frees the memory occupied by a typical node's data. Do not use for more
	complex nodes that malloc extraneous data to themselves!

-------------------------------------------------------------------------------*/

void defaultDeconstructor(void* data)
{
	if (data != NULL)
	{
		free(data);
	}
}

/*-------------------------------------------------------------------------------

	stringDeconstructor

	Frees the memory occupied by a string.

-------------------------------------------------------------------------------*/

void stringDeconstructor(void* data)
{
	string_t* string;
	if (data != NULL)
	{
		string = (string_t*)data;
		if ( string->data != NULL )
		{
			free(string->data);
			string->data = NULL;
		}
		free(data);
	}
}

/*-------------------------------------------------------------------------------

	emptyDeconstructor

	Useful to remove a node without deallocating its data.

-------------------------------------------------------------------------------*/

void emptyDeconstructor(void* data)
{
	return;
}

/*-------------------------------------------------------------------------------

	entityDeconstructor

	Frees the memory occupied by a node pointing to an entity

-------------------------------------------------------------------------------*/

void entityDeconstructor(void* data)
{
	Entity* entity;

	if ( data != nullptr )
	{
		entity = (Entity*)data;

		//TODO: If I am part of the creaturelist, remove my node from that list.)

		//free(data);
		delete entity;
	}
}

/*-------------------------------------------------------------------------------

statDeconstructor

Frees the memory occupied by a node pointing to stat

-------------------------------------------------------------------------------*/

void statDeconstructor(void* data)
{
	Stat* stat;

	if ( data != nullptr )
	{
		stat = (Stat*)data;
		//free(data);
		delete stat;
	}
}

/*-------------------------------------------------------------------------------

	lightDeconstructor

	Frees the memory occupied by a node pointing to a light

-------------------------------------------------------------------------------*/

void lightDeconstructor(void* data)
{
	if ( data == nullptr )
	{
		return;
	}

	light_t* light = static_cast<light_t*>(data);

	if ( light->tiles != nullptr )
	{
		const int diameter = light->radius * 2 + 1;
		const int lightsize = diameter * diameter;

		const size_t mapsize =
			lightmapSize3D(
				map.width,
				map.height
			);

		for ( int localY = 0;
			localY < diameter;
			++localY )
		{
			for ( int localX = 0;
				localX < diameter;
				++localX )
			{
				const int soff =
					localY
					+ localX * diameter;

				if ( soff < 0
					|| soff >= lightsize )
				{
					continue;
				}

				const int mapX =
					localX
					+ light->x
					- light->radius;

				const int mapY =
					localY
					+ light->y
					- light->radius;

				if ( mapX < 0
					|| mapY < 0
					|| mapX >= map.width
					|| mapY >= map.height )
				{
					continue;
				}

				const size_t doff =
					lightmapIndex3D(
						mapX,
						mapY,
						light->layer,
						map.width,
						map.height
					);

				if ( doff >= mapsize )
				{
					continue;
				}

				const auto& source =
					light->tiles[soff];

				if ( light->index )
				{
					auto& destination =
						lightmaps[light->index][doff];

					destination.x -= source.x;
					destination.y -= source.y;
					destination.z -= source.z;
					destination.w -= source.w;
				}
				else
				{
					for ( int player = 0;
						player < MAXPLAYERS + 1;
						++player )
					{
						auto& destination =
							lightmaps[player][doff];

						destination.x -= source.x;
						destination.y -= source.y;
						destination.z -= source.z;
						destination.w -= source.w;
					}
				}
			}
		}

		free(light->tiles);
		light->tiles = nullptr;
	}

	free(light);
}

/*-------------------------------------------------------------------------------

	mapDeconstructor

	Frees the memory occupied by a node pointing to a map

-------------------------------------------------------------------------------*/

void mapDeconstructor(void* data)
{
	map_t* map;

	if ( data != nullptr )
	{
		map = (map_t*)data;
		if ( map->tiles != nullptr )
		{
			free(map->tiles);
		}
		if ( map->creatures )
		{
			list_FreeAll(map->creatures); //TODO: This needed?
			delete map->creatures;
		}
		if ( map->entities != nullptr )
		{
			list_FreeAll(map->entities);
			free(map->entities);
		}
		if ( map->worldUI )
		{
			list_FreeAll(map->worldUI);
			delete map->worldUI;
		}
		free(data);
	}
}

/*-------------------------------------------------------------------------------

	listDeconstructor

	Frees the memory occupied by a node pointing to a list

-------------------------------------------------------------------------------*/

void listDeconstructor(void* data)
{
	list_t* list;

	if (data != NULL)
	{
		list = (list_t*)data;
		list_FreeAll(list);
		free(data);
	}
}

/*-------------------------------------------------------------------------------

	newEntity

	Creates a new entity with empty settings and places it in the entity list

-------------------------------------------------------------------------------*/

Entity* newEntity(Sint32 sprite, Uint32 pos, list_t* entlist, list_t* creaturelist)
{
	Entity* entity = nullptr;

	// allocate memory for entity
	/*if( (entity = (Entity *) malloc(sizeof(Entity)))==NULL ) {
		printlog( "failed to allocate memory for new entity!\n" );
		exit(1);
	}*/
#ifndef NINTENDO
	bool failedToAllocate = false;
	try
	{
		entity = new Entity(sprite, pos, entlist, creaturelist);
	}
	catch (std::bad_alloc& ba)
	{
		failedToAllocate = true;
	}

	if ( failedToAllocate || !entity )
	{
		printlog("failed to allocate memory for new entity!\n");
		exit(1);
	}
#else
	entity = new Entity(sprite, pos, entlist, creaturelist);
#endif

	return entity;
}

/*-------------------------------------------------------------------------------

	newButton

	Creates a new button and places it in the button list

-------------------------------------------------------------------------------*/

button_t* newButton(void)
{
	button_t* button;

	// allocate memory for button
	if ( (button = (button_t*) malloc(sizeof(button_t))) == NULL )
	{
		printlog( "failed to allocate memory for new button!\n" );
		exit(1);
	}

	// add the button to the button list
	button->node = list_AddNodeLast(&button_l);
	button->node->element = button;
	button->node->deconstructor = &defaultDeconstructor;
	button->node->size = sizeof(button_t);

	// now set all of my data elements to ZERO or NULL
	button->x = 0;
	button->y = 0;
	button->sizex = 0;
	button->sizey = 0;
	button->visible = 1;
	button->focused = 0;
	button->key = 0;
	button->joykey = -1;
	button->pressed = false;
	button->needclick = true;
	button->action = NULL;
	strcpy(button->label, "nodef");

	button->outline = false;

	return button;
}

/*-------------------------------------------------------------------------------

	newLight

	Creates a new light and places it in the light list

-------------------------------------------------------------------------------*/

light_t* newLight(
	int index,
	Sint32 x,
	Sint32 y,
	Sint32 radius
)
{
	return newLight(
		index,
		x,
		y,
		0,
		radius
	);
}

light_t* newLight(
	int index,
	Sint32 x,
	Sint32 y,
	Sint32 layer,
	Sint32 radius
)
{
	light_t* light;

	if ( (light = static_cast<light_t*>(
		malloc(sizeof(light_t))
	)) == nullptr )
	{
		printlog(
			"failed to allocate memory for new light!\n"
		);
		exit(1);
	}

	light->node = list_AddNodeLast(&light_l);
	light->node->element = light;
	light->node->deconstructor =
		&lightDeconstructor;
	light->node->size = sizeof(light_t);

	light->index = index;
	light->x = x;
	light->y = y;
	light->layer =
		clampLightmapLayer(layer);
	light->radius = radius;

	if ( light->radius > 0 )
	{
		const auto size =
			sizeof(vec4_t)
			* (radius * 2 + 1)
			* (radius * 2 + 1);

		light->tiles =
			static_cast<vec4_t*>(
				malloc(size)
			);

		memset(light->tiles, 0, size);
	}
	else
	{
		light->tiles = nullptr;
	}

	return light;
}

/*-------------------------------------------------------------------------------

	newString

	Creates a new string and places it in a list

-------------------------------------------------------------------------------*/

string_t* newString(list_t* list, Uint32 color, Uint32 time, int player, char const * const content, ...)
{
	string_t* string;
	char str[1024] = { 0 };
	va_list argptr;
	int c, i;

	// allocate memory for string
	if ( (string = (string_t*) malloc(sizeof(string_t))) == NULL )
	{
		printlog( "failed to allocate memory for new string!\n" );
		exit(1);
	}

	if ( content )
	{
		if ( strlen(content) > 2048 )
		{
			printlog( "error creating new string: buffer overflow.\n" );
			exit(1);
		}
	}

    string->time = time;
	string->color = color;
	string->lines = 1;
	string->player = player;
	if ( content != NULL )
	{
#ifndef EDITOR
		if ( list && list == &messages )
		{
			std::string sanitizedStr = messageSanitizePercentSign(content, nullptr).c_str();
			const char* strPtr = sanitizedStr.c_str();
			// format the content
			va_start( argptr, strPtr);
			i = vsnprintf(str, 1023, strPtr, argptr);
			va_end( argptr );
		}
		else
#endif
		{
			// format the content
			va_start(argptr, content);
			i = vsnprintf(str, 1023, content, argptr);
			va_end(argptr);
		}
		string->data = (char*) malloc(sizeof(char) * (i + 1));
		if ( !string->data )
		{
			printlog( "error creating new string: couldn't allocate string data.\n" );
			exit(1);
		}
		memset(string->data, 0, sizeof(char) * (i + 1));
		for ( c = 0; c < i; c++ )
		{
			if ( str[c] == 10 )   // line feed
			{
				string->lines++;
			}
		}
		strncpy(string->data, str, i);
	}
	else
	{
		string->data = NULL;
	}

	// add the string to the list
	if ( list != NULL )
	{
		string->node = list_AddNodeLast(list);
		string->node->element = string;
		string->node->deconstructor = &stringDeconstructor;
		string->node->size = sizeof(string_t);
	}
	else
	{
		string->node = NULL;
	}

	return string;
}
