#include "room_group.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
bool expect(const bool condition, const char* expression)
{
	if ( !condition )
	{
		std::cerr << "FAILED: " << expression << '\n';
	}
	return condition;
}

#define EXPECT(expression) do { if (!expect((expression), #expression)) return false; } while (false)

std::string readSource(const char* relative)
{
	std::ifstream input(std::filesystem::path(BARONY_SOURCE_DIR) / relative,
		std::ios::binary);
	std::ostringstream text;
	text << input.rdbuf();
	return input ? text.str() : std::string{};
}

bool contains(const std::string& source, const std::string& token)
{
	return source.find(token) != std::string::npos;
}

bool testNamedGroupsAndStableIdentity()
{
	AuthoredRoomGroupCollection groups;
	authoredRoomGroupsReset(groups);
	EXPECT(groups.count == 0);
	EXPECT(groups.nextID == 1);

	const int lower = authoredRoomGroupAdd(groups, "Atrium", 2, 3, 8, 9,
		0, 4, AUTHORED_ROOM_GROUP_BOTH);
	const int upper = authoredRoomGroupAdd(groups, "atrium", 12, 13, 18, 19,
		5, 31, AUTHORED_ROOM_GROUP_SPRITES);
	EXPECT(lower == 0 && upper == 1);
	EXPECT(groups.entries[lower].id != groups.entries[upper].id);
	EXPECT(std::strcmp(groups.entries[upper].name, "atrium (2)") == 0);
	EXPECT(groups.entries[upper].topLayer == 31);

	const std::uint32_t stableID = groups.entries[lower].id;
	EXPECT(authoredRoomGroupUpdate(groups, stableID, "Main Atrium",
		1, 2, 10, 11, 0, 7, AUTHORED_ROOM_GROUP_TILES));
	EXPECT(groups.entries[authoredRoomGroupFindByID(groups, stableID)].id == stableID);
	EXPECT(!authoredRoomGroupRemove(groups, 0));
	EXPECT(authoredRoomGroupRemove(groups, groups.entries[upper].id));
	EXPECT(groups.count == 1);
	EXPECT(authoredRoomGroupAdd(groups, "bad\nname", 0, 0, 1, 1,
		0, 0, AUTHORED_ROOM_GROUP_BOTH) == -1);
	return true;
}

bool testSerializationAndBoundsValidation()
{
	AuthoredRoomGroupCollection source;
	authoredRoomGroupsReset(source);
	EXPECT(authoredRoomGroupAdd(source, "Three floor room", 4, 5, 14, 15,
		2, 8, AUTHORED_ROOM_GROUP_BOTH) == 0);
	EXPECT(authoredRoomGroupAdd(source, "Air carve", 20, 1, 21, 2,
		31, 31, AUTHORED_ROOM_GROUP_TILES) == 1);

	std::vector<std::uint8_t> bytes;
	EXPECT(serializeAuthoredRoomGroups(source, bytes));
	EXPECT(!bytes.empty());

	AuthoredRoomGroupCollection decoded;
	authoredRoomGroupsReset(decoded);
	EXPECT(deserializeAuthoredRoomGroups(bytes.data(), bytes.size(),
		64, 64, 32, decoded));
	EXPECT(decoded.count == source.count);
	EXPECT(decoded.nextID == source.nextID);
	EXPECT(std::strcmp(decoded.entries[0].name, source.entries[0].name) == 0);
	EXPECT(decoded.entries[0].bottomLayer == 2);
	EXPECT(decoded.entries[0].topLayer == 8);

	AuthoredRoomGroupCollection rejected;
	authoredRoomGroupsReset(rejected);
	EXPECT(!deserializeAuthoredRoomGroups(bytes.data(), bytes.size(),
		8, 8, 32, rejected));
	EXPECT(rejected.count == 0);

	std::vector<std::uint8_t> trailing = bytes;
	trailing.push_back(0);
	EXPECT(!deserializeAuthoredRoomGroups(trailing.data(), trailing.size(),
		64, 64, 32, rejected));
	std::vector<std::uint8_t> badVersion = bytes;
	badVersion[0] = 99;
	EXPECT(!deserializeAuthoredRoomGroups(badVersion.data(), badVersion.size(),
		64, 64, 32, rejected));
	AuthoredRoomGroupCollection invalid = source;
	invalid.entries[0].x1 = -1;
	EXPECT(!serializeAuthoredRoomGroups(invalid, trailing));
	return true;
}

bool testRelativeLayerTranslationAndResize()
{
	AuthoredRoomGroup group = {};
	group.id = 17;
	group.x1 = 10;
	group.y1 = 20;
	group.x2 = 15;
	group.y2 = 25;
	group.bottomLayer = 7;
	group.topLayer = 11;
	group.contentMask = AUTHORED_ROOM_GROUP_BOTH;
	std::strcpy(group.name, "Tower");
	EXPECT(authoredRoomGroupFullyInside(group, 9, 19, 16, 26, 6, 12));

	const AuthoredRoomGroup relative = authoredRoomGroupTranslated(group,
		-10, -20, -7);
	EXPECT(relative.x1 == 0 && relative.y1 == 0);
	EXPECT(relative.bottomLayer == 0 && relative.topLayer == 4);
	const AuthoredRoomGroup placed = authoredRoomGroupTranslated(relative,
		30, 40, 18);
	EXPECT(placed.x1 == 30 && placed.y1 == 40);
	EXPECT(placed.bottomLayer == 18 && placed.topLayer == 22);

	AuthoredRoomGroupCollection groups;
	authoredRoomGroupsReset(groups);
	EXPECT(authoredRoomGroupAdd(groups, "Resize", 4, 4, 20, 20,
		4, 31, AUTHORED_ROOM_GROUP_BOTH) == 0);
	authoredRoomGroupsClampToMap(groups, 12, 10, 8);
	EXPECT(groups.entries[0].x2 == 11);
	EXPECT(groups.entries[0].y2 == 9);
	EXPECT(groups.entries[0].topLayer == 7);
	return true;
}

bool testIntegrationContracts()
{
	const std::string editor = readSource("src/editor.cpp");
	const std::string files = readSource("src/files.cpp");
	const std::string maps = readSource("src/maps.cpp");
	const std::string rooms = readSource("src/sam/framework/sam_rooms.cpp");
	const std::string catalog = readSource("src/sam/sam_content_catalog.cpp");
	EXPECT(!editor.empty() && !files.empty() && !maps.empty());
	EXPECT(contains(editor, "entityAuthoredSpriteLayer"));
	EXPECT(contains(editor, "authoredRoomGroupTranslated"));
	EXPECT(contains(files, "RGRP"));
	EXPECT(contains(files, "SMID"));
	EXPECT(contains(maps, "SAMRooms::roomsFor(levelset)"));
	EXPECT(contains(maps, "childEntity->persistentID = 0"));
	EXPECT(contains(maps, "Stable S.A.M."));
	EXPECT(contains(rooms, "sortKey"));
	EXPECT(contains(rooms, "room:"));
	EXPECT(contains(catalog, "roomEntries"));
	EXPECT(!contains(editor, "source->z / 16"));
	return true;
}
}

int main()
{
	if ( !testNamedGroupsAndStableIdentity()
		|| !testSerializationAndBoundsValidation()
		|| !testRelativeLayerTranslationAndResize()
		|| !testIntegrationContracts() )
	{
		return 1;
	}
	std::cout << "Room Group core and integration characterization passed.\n";
	return 0;
}
