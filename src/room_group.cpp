/*-------------------------------------------------------------------------------
	Automatia authored Room Groups — shared editor/map-format core.
-------------------------------------------------------------------------------*/

#include "room_group.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>

namespace
{
	constexpr std::uint16_t kRoomGroupChunkVersion = 1;

	void appendU16(std::vector<std::uint8_t>& output, const std::uint16_t value)
	{
		output.push_back(static_cast<std::uint8_t>(value & 0xffu));
		output.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
	}

	void appendU32(std::vector<std::uint8_t>& output, const std::uint32_t value)
	{
		for ( unsigned shift = 0; shift < 32; shift += 8 )
		{
			output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
		}
	}

	bool readU16(const std::uint8_t* data, const std::size_t size,
		std::size_t& offset, std::uint16_t& value)
	{
		if ( !data || offset > size || size - offset < 2 )
		{
			return false;
		}
		value = static_cast<std::uint16_t>(data[offset])
			| static_cast<std::uint16_t>(data[offset + 1]) << 8u;
		offset += 2;
		return true;
	}

	bool readU32(const std::uint8_t* data, const std::size_t size,
		std::size_t& offset, std::uint32_t& value)
	{
		if ( !data || offset > size || size - offset < 4 )
		{
			return false;
		}
		value = static_cast<std::uint32_t>(data[offset])
			| static_cast<std::uint32_t>(data[offset + 1]) << 8u
			| static_cast<std::uint32_t>(data[offset + 2]) << 16u
			| static_cast<std::uint32_t>(data[offset + 3]) << 24u;
		offset += 4;
		return true;
	}

	std::string lowercase(const char* text)
	{
		std::string result = text ? text : "";
		for ( char& c : result )
		{
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return result;
	}

	std::size_t boundedNameLength(const char* text)
	{
		if ( !text )
		{
			return 0;
		}
		std::size_t length = 0;
		while ( length < AUTHORED_ROOM_GROUP_NAME_BYTES && text[length] )
		{
			++length;
		}
		return length;
	}

	std::string trimmedName(const char* requested)
	{
		std::string name = requested ? requested : "";
		while ( !name.empty()
			&& std::isspace(static_cast<unsigned char>(name.front())) )
		{
			name.erase(name.begin());
		}
		while ( !name.empty()
			&& std::isspace(static_cast<unsigned char>(name.back())) )
		{
			name.pop_back();
		}
		if ( name.size() >= AUTHORED_ROOM_GROUP_NAME_BYTES )
		{
			name.resize(AUTHORED_ROOM_GROUP_NAME_BYTES - 1);
		}
		return name;
	}

	bool nameExists(const AuthoredRoomGroupCollection& groups,
		const std::string& name, const std::uint32_t excludedID)
	{
		const std::string key = lowercase(name.c_str());
		for ( std::uint32_t i = 0;
			i < std::min<std::uint32_t>(groups.count,
				static_cast<std::uint32_t>(AUTHORED_ROOM_GROUP_MAX_COUNT)); ++i )
		{
			if ( groups.entries[i].id != excludedID
				&& lowercase(groups.entries[i].name) == key )
			{
				return true;
			}
		}
		return false;
	}

	std::string uniqueName(const AuthoredRoomGroupCollection& groups,
		const char* requested, const std::uint32_t excludedID)
	{
		std::string base = trimmedName(requested);
		if ( base.empty() )
		{
			base = "Room Group";
		}
		else if ( !authoredRoomGroupNameIsSafe(base.c_str()) )
		{
			return {};
		}
		if ( !nameExists(groups, base, excludedID) )
		{
			return base;
		}
		for ( unsigned suffix = 2; suffix < 1000000; ++suffix )
		{
			char suffixText[24];
			std::snprintf(suffixText, sizeof(suffixText), " (%u)", suffix);
			const std::size_t suffixLength = std::strlen(suffixText);
			std::string candidate = base.substr(0,
				AUTHORED_ROOM_GROUP_NAME_BYTES - 1 - suffixLength);
			candidate += suffixText;
			if ( !nameExists(groups, candidate, excludedID) )
			{
				return candidate;
			}
		}
		return {};
	}

	bool validBounds(const AuthoredRoomGroup& group,
		const std::uint32_t width, const std::uint32_t height,
		const std::uint32_t layers)
	{
		return width > 0 && height > 0 && layers > 0
			&& group.x1 >= 0 && group.y1 >= 0
			&& group.x1 <= group.x2 && group.y1 <= group.y2
			&& static_cast<std::uint32_t>(group.x2) < width
			&& static_cast<std::uint32_t>(group.y2) < height
			&& group.bottomLayer >= 0
			&& group.bottomLayer <= group.topLayer
			&& static_cast<std::uint32_t>(group.topLayer) < layers
			&& (group.contentMask & ~AUTHORED_ROOM_GROUP_BOTH) == 0
			&& (group.contentMask & AUTHORED_ROOM_GROUP_BOTH) != 0;
	}

	std::uint32_t nextAvailableID(AuthoredRoomGroupCollection& groups)
	{
		std::uint32_t candidate = groups.nextID == 0 ? 1 : groups.nextID;
		for ( std::size_t attempts = 0;
			attempts <= AUTHORED_ROOM_GROUP_MAX_COUNT; ++attempts )
		{
			if ( authoredRoomGroupFindByID(groups, candidate) < 0 )
			{
				groups.nextID = candidate == std::numeric_limits<std::uint32_t>::max()
					? 1 : candidate + 1;
				return candidate;
			}
			candidate = candidate == std::numeric_limits<std::uint32_t>::max()
				? 1 : candidate + 1;
		}
		return 0;
	}
}

void authoredRoomGroupsReset(AuthoredRoomGroupCollection& groups)
{
	std::memset(&groups, 0, sizeof(groups));
	groups.nextID = 1;
}

bool authoredRoomGroupNameIsSafe(const char* name)
{
	if ( !name || !name[0] )
	{
		return false;
	}
	const std::size_t length = boundedNameLength(name);
	if ( length == 0 || length >= AUTHORED_ROOM_GROUP_NAME_BYTES )
	{
		return false;
	}
	for ( std::size_t i = 0; i < length; ++i )
	{
		const unsigned char c = static_cast<unsigned char>(name[i]);
		if ( c < 32 || c == 127 )
		{
			return false;
		}
	}
	return true;
}

int authoredRoomGroupFindByID(const AuthoredRoomGroupCollection& groups,
	const std::uint32_t id)
{
	if ( id == 0 )
	{
		return -1;
	}
	for ( std::uint32_t i = 0;
		i < std::min<std::uint32_t>(groups.count,
			static_cast<std::uint32_t>(AUTHORED_ROOM_GROUP_MAX_COUNT)); ++i )
	{
		if ( groups.entries[i].id == id )
		{
			return static_cast<int>(i);
		}
	}
	return -1;
}

int authoredRoomGroupAdd(AuthoredRoomGroupCollection& groups,
	const char* requestedName, std::int32_t x1, std::int32_t y1,
	std::int32_t x2, std::int32_t y2, std::int16_t bottomLayer,
	std::int16_t topLayer, const std::uint8_t contentMask)
{
	if ( groups.count >= AUTHORED_ROOM_GROUP_MAX_COUNT )
	{
		return -1;
	}
	if ( x1 > x2 ) { std::swap(x1, x2); }
	if ( y1 > y2 ) { std::swap(y1, y2); }
	if ( bottomLayer > topLayer ) { std::swap(bottomLayer, topLayer); }
	if ( x1 < 0 || y1 < 0 || bottomLayer < 0
		|| (contentMask & AUTHORED_ROOM_GROUP_BOTH) == 0
		|| (contentMask & ~AUTHORED_ROOM_GROUP_BOTH) != 0 )
	{
		return -1;
	}
	const std::string name = uniqueName(groups, requestedName, 0);
	const std::uint32_t id = nextAvailableID(groups);
	if ( name.empty() || id == 0 )
	{
		return -1;
	}
	AuthoredRoomGroup& group = groups.entries[groups.count];
	std::memset(&group, 0, sizeof(group));
	group.id = id;
	group.x1 = x1;
	group.y1 = y1;
	group.x2 = x2;
	group.y2 = y2;
	group.bottomLayer = bottomLayer;
	group.topLayer = topLayer;
	group.contentMask = contentMask;
	std::snprintf(group.name, sizeof(group.name), "%s", name.c_str());
	return static_cast<int>(groups.count++);
}

bool authoredRoomGroupUpdate(AuthoredRoomGroupCollection& groups,
	const std::uint32_t id, const char* requestedName,
	std::int32_t x1, std::int32_t y1, std::int32_t x2, std::int32_t y2,
	std::int16_t bottomLayer, std::int16_t topLayer,
	const std::uint8_t contentMask)
{
	const int index = authoredRoomGroupFindByID(groups, id);
	if ( index < 0 )
	{
		return false;
	}
	if ( x1 > x2 ) { std::swap(x1, x2); }
	if ( y1 > y2 ) { std::swap(y1, y2); }
	if ( bottomLayer > topLayer ) { std::swap(bottomLayer, topLayer); }
	if ( x1 < 0 || y1 < 0 || bottomLayer < 0
		|| (contentMask & AUTHORED_ROOM_GROUP_BOTH) == 0
		|| (contentMask & ~AUTHORED_ROOM_GROUP_BOTH) != 0 )
	{
		return false;
	}
	const std::string name = uniqueName(groups, requestedName, id);
	if ( name.empty() )
	{
		return false;
	}
	AuthoredRoomGroup& group = groups.entries[index];
	group.x1 = x1;
	group.y1 = y1;
	group.x2 = x2;
	group.y2 = y2;
	group.bottomLayer = bottomLayer;
	group.topLayer = topLayer;
	group.contentMask = contentMask;
	std::memset(group.reserved, 0, sizeof(group.reserved));
	std::snprintf(group.name, sizeof(group.name), "%s", name.c_str());
	return true;
}

bool authoredRoomGroupRemove(AuthoredRoomGroupCollection& groups,
	const std::uint32_t id)
{
	const int index = authoredRoomGroupFindByID(groups, id);
	if ( index < 0 )
	{
		return false;
	}
	for ( std::uint32_t i = static_cast<std::uint32_t>(index) + 1;
		i < groups.count; ++i )
	{
		groups.entries[i - 1] = groups.entries[i];
	}
	--groups.count;
	std::memset(&groups.entries[groups.count], 0,
		sizeof(groups.entries[groups.count]));
	return true;
}

void authoredRoomGroupsClampToMap(AuthoredRoomGroupCollection& groups,
	const std::uint32_t width, const std::uint32_t height,
	const std::uint32_t numLayers)
{
	if ( width == 0 || height == 0 || numLayers == 0 )
	{
		authoredRoomGroupsReset(groups);
		return;
	}
	std::uint32_t out = 0;
	const std::int32_t maximumX = static_cast<std::int32_t>(width - 1);
	const std::int32_t maximumY = static_cast<std::int32_t>(height - 1);
	const int maximumLayer = static_cast<int>(numLayers - 1);
	for ( std::uint32_t i = 0;
		i < std::min<std::uint32_t>(groups.count,
			static_cast<std::uint32_t>(AUTHORED_ROOM_GROUP_MAX_COUNT)); ++i )
	{
		AuthoredRoomGroup group = groups.entries[i];
		group.x1 = std::clamp<std::int32_t>(group.x1, 0, maximumX);
		group.x2 = std::clamp<std::int32_t>(group.x2, 0, maximumX);
		group.y1 = std::clamp<std::int32_t>(group.y1, 0, maximumY);
		group.y2 = std::clamp<std::int32_t>(group.y2, 0, maximumY);
		group.bottomLayer = static_cast<std::int16_t>(
			std::clamp<int>(group.bottomLayer, 0, maximumLayer));
		group.topLayer = static_cast<std::int16_t>(
			std::clamp<int>(group.topLayer, 0, maximumLayer));
		if ( group.x1 > group.x2 ) { std::swap(group.x1, group.x2); }
		if ( group.y1 > group.y2 ) { std::swap(group.y1, group.y2); }
		if ( group.bottomLayer > group.topLayer )
		{
			std::swap(group.bottomLayer, group.topLayer);
		}
		if ( group.id != 0 && authoredRoomGroupNameIsSafe(group.name)
			&& validBounds(group, width, height, numLayers) )
		{
			groups.entries[out++] = group;
		}
	}
	for ( std::uint32_t i = out; i < groups.count
		&& i < AUTHORED_ROOM_GROUP_MAX_COUNT; ++i )
	{
		std::memset(&groups.entries[i], 0, sizeof(groups.entries[i]));
	}
	groups.count = out;
	if ( groups.nextID == 0 )
	{
		groups.nextID = 1;
	}
}

bool authoredRoomGroupFullyInside(const AuthoredRoomGroup& group,
	const std::int32_t x1, const std::int32_t y1,
	const std::int32_t x2, const std::int32_t y2,
	const std::int16_t bottomLayer, const std::int16_t topLayer)
{
	return group.x1 >= x1 && group.y1 >= y1
		&& group.x2 <= x2 && group.y2 <= y2
		&& group.bottomLayer >= bottomLayer
		&& group.topLayer <= topLayer;
}

AuthoredRoomGroup authoredRoomGroupTranslated(const AuthoredRoomGroup& source,
	const std::int32_t deltaX, const std::int32_t deltaY,
	const std::int16_t deltaLayer)
{
	AuthoredRoomGroup group = source;
	group.x1 += deltaX;
	group.x2 += deltaX;
	group.y1 += deltaY;
	group.y2 += deltaY;
	group.bottomLayer = static_cast<std::int16_t>(group.bottomLayer + deltaLayer);
	group.topLayer = static_cast<std::int16_t>(group.topLayer + deltaLayer);
	return group;
}

bool serializeAuthoredRoomGroups(const AuthoredRoomGroupCollection& groups,
	std::vector<std::uint8_t>& output)
{
	output.clear();
	if ( groups.count > AUTHORED_ROOM_GROUP_MAX_COUNT )
	{
		return false;
	}
	appendU16(output, kRoomGroupChunkVersion);
	appendU16(output, 0);
	appendU32(output, groups.nextID == 0 ? 1 : groups.nextID);
	appendU32(output, groups.count);
	std::set<std::uint32_t> ids;
	std::set<std::string> names;
	for ( std::uint32_t i = 0; i < groups.count; ++i )
	{
		const AuthoredRoomGroup& group = groups.entries[i];
		const std::size_t nameLength = boundedNameLength(group.name);
		if ( group.id == 0 || !ids.insert(group.id).second
			|| !authoredRoomGroupNameIsSafe(group.name)
			|| !names.insert(lowercase(group.name)).second
			|| nameLength > std::numeric_limits<std::uint16_t>::max()
			|| group.x1 < 0 || group.y1 < 0
			|| group.x1 > group.x2 || group.y1 > group.y2
			|| group.bottomLayer < 0 || group.bottomLayer > group.topLayer
			|| (group.contentMask & AUTHORED_ROOM_GROUP_BOTH) == 0
			|| (group.contentMask & ~AUTHORED_ROOM_GROUP_BOTH) != 0
			|| group.reserved[0] != 0 || group.reserved[1] != 0
			|| group.reserved[2] != 0 )
		{
			return false;
		}
		appendU32(output, group.id);
		appendU32(output, static_cast<std::uint32_t>(group.x1));
		appendU32(output, static_cast<std::uint32_t>(group.y1));
		appendU32(output, static_cast<std::uint32_t>(group.x2));
		appendU32(output, static_cast<std::uint32_t>(group.y2));
		appendU16(output, static_cast<std::uint16_t>(group.bottomLayer));
		appendU16(output, static_cast<std::uint16_t>(group.topLayer));
		output.push_back(group.contentMask);
		output.push_back(0);
		appendU16(output, static_cast<std::uint16_t>(nameLength));
		output.insert(output.end(), group.name, group.name + nameLength);
	}
	return true;
}

bool deserializeAuthoredRoomGroups(const std::uint8_t* data,
	const std::size_t size, const std::uint32_t mapWidth,
	const std::uint32_t mapHeight, const std::uint32_t mapLayers,
	AuthoredRoomGroupCollection& output)
{
	AuthoredRoomGroupCollection decoded;
	authoredRoomGroupsReset(decoded);
	std::size_t offset = 0;
	std::uint16_t version = 0;
	std::uint16_t reserved = 0;
	std::uint32_t nextID = 0;
	std::uint32_t count = 0;
	if ( !readU16(data, size, offset, version)
		|| !readU16(data, size, offset, reserved)
		|| !readU32(data, size, offset, nextID)
		|| !readU32(data, size, offset, count)
		|| version != kRoomGroupChunkVersion || reserved != 0
		|| nextID == 0 || count > AUTHORED_ROOM_GROUP_MAX_COUNT )
	{
		return false;
	}
	std::set<std::uint32_t> ids;
	std::set<std::string> names;
	for ( std::uint32_t i = 0; i < count; ++i )
	{
		AuthoredRoomGroup group = {};
		std::uint32_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
		std::uint16_t bottom = 0, top = 0, nameLength = 0;
		if ( !readU32(data, size, offset, group.id)
			|| !readU32(data, size, offset, x1)
			|| !readU32(data, size, offset, y1)
			|| !readU32(data, size, offset, x2)
			|| !readU32(data, size, offset, y2)
			|| !readU16(data, size, offset, bottom)
			|| !readU16(data, size, offset, top)
			|| offset > size || size - offset < 2 )
		{
			return false;
		}
		if ( x1 > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
			|| y1 > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
			|| x2 > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
			|| y2 > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
			|| bottom > static_cast<std::uint16_t>(std::numeric_limits<std::int16_t>::max())
			|| top > static_cast<std::uint16_t>(std::numeric_limits<std::int16_t>::max()) )
		{
			return false;
		}
		group.contentMask = data[offset++];
		if ( data[offset++] != 0
			|| !readU16(data, size, offset, nameLength)
			|| nameLength == 0
			|| nameLength >= AUTHORED_ROOM_GROUP_NAME_BYTES
			|| offset > size || size - offset < nameLength )
		{
			return false;
		}
		group.x1 = static_cast<std::int32_t>(x1);
		group.y1 = static_cast<std::int32_t>(y1);
		group.x2 = static_cast<std::int32_t>(x2);
		group.y2 = static_cast<std::int32_t>(y2);
		group.bottomLayer = static_cast<std::int16_t>(bottom);
		group.topLayer = static_cast<std::int16_t>(top);
		std::memcpy(group.name, data + offset, nameLength);
		group.name[nameLength] = '\0';
		offset += nameLength;
		if ( group.id == 0 || !ids.insert(group.id).second
			|| !authoredRoomGroupNameIsSafe(group.name)
			|| !names.insert(lowercase(group.name)).second
			|| !validBounds(group, mapWidth, mapHeight, mapLayers) )
		{
			return false;
		}
		decoded.entries[decoded.count++] = group;
	}
	if ( offset != size )
	{
		return false;
	}
	decoded.nextID = nextID;
	output = decoded;
	return true;
}
