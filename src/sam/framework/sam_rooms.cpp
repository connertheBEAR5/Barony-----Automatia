/*-------------------------------------------------------------------------------
	S.A.M Framework — room injection. See sam_rooms.hpp for the ordering invariant.
-------------------------------------------------------------------------------*/

#include "sam_rooms.hpp"
#include "sam_workshop.hpp"
#include "sam_logger.hpp"
#include "sam_errors.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>

namespace
{
	const char* MOD = "ROOMS";

	// One injected room. sortKey is derived only from mod namespace and declared path,
	// never load or directory-enumeration order. A digest orders conflicting duplicate
	// keys before deduplication so an installation root cannot choose the effective bytes.
	struct Room
	{
		std::string sortKey;   // "<namespace>/<relative path>"
		std::string absPath;   // resolved absolute .lmp
		std::string digest;    // byte digest when building the multiplayer catalog
	};
	using RoomsByLevelset = std::map<std::string, std::vector<Room>>;

	std::map<std::string, std::vector<std::string>> s_byLevelset; // levelset -> sorted abs paths
	int s_count = 0;
	const std::vector<std::string> s_empty;

	std::string joinPath(const std::string& dir, const std::string& file)
	{
		if ( dir.empty() ) { return file; }
		if ( file.empty() ) { return dir; }
		const char last = dir[dir.size() - 1];
		if ( last == '/' || last == '\\' ) { return dir + file; }
		return dir + "/" + file;
	}

	// Barony's own level names are lowercase; normalise so "Mine" and "mine" mean the same
	// levelset rather than silently creating a second bucket nothing ever reads.
	std::string lower(std::string s)
	{
		for ( char& c : s ) { c = (char)std::tolower((unsigned char)c); }
		return s;
	}

	std::string canonicalRelativeKey(std::string path)
	{
		std::replace(path.begin(), path.end(), '\\', '/');
		return path;
	}

	bool digestFile(const std::string& path, std::string& output)
	{
		std::ifstream input(path.c_str(), std::ios::binary);
		if ( !input )
		{
			return false;
		}
		constexpr std::uint64_t offsetBasis = 14695981039346656037ull;
		constexpr std::uint64_t prime = 1099511628211ull;
		std::uint64_t hash = offsetBasis;
		std::array<char, 64 * 1024> bytes;
		while ( input )
		{
			input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
			const std::streamsize count = input.gcount();
			for ( std::streamsize i = 0; i < count; ++i )
			{
				hash ^= static_cast<unsigned char>(bytes[static_cast<std::size_t>(i)]);
				hash *= prime;
			}
		}
		if ( !input.eof() )
		{
			return false;
		}
		std::ostringstream text;
		text << std::hex << std::setfill('0') << std::setw(16) << hash;
		output = text.str();
		return true;
	}

	RoomsByLevelset gatherRooms(const std::vector<SAMModManifest>& mods,
		const bool logWarnings)
	{
		RoomsByLevelset gathered;
		for ( const SAMModManifest& manifest : mods )
		{
			for ( const auto& declaration : manifest.rooms )
			{
				const std::string levelset = lower(declaration.first);
				if ( levelset.empty() ) { continue; }
				for ( const std::string& relativePath : declaration.second )
				{
					if ( relativePath.empty() ) { continue; }
					if ( SAMErrors::relPathEscapes(relativePath) )
					{
						if ( logWarnings )
						{
							SAM_WARN(MOD, "Mod [" + manifest.ns + "] room path '"
								+ relativePath + "' escapes the mod folder - ignoring it.");
						}
						continue;
					}
					Room room;
					room.sortKey = manifest.ns + "/"
						+ canonicalRelativeKey(relativePath);
					room.absPath = joinPath(manifest.modPath, relativePath);
					if ( !digestFile(room.absPath, room.digest) )
					{
						if ( logWarnings )
						{
							SAM_WARN(MOD, "Mod [" + manifest.ns + "] declares missing or unreadable room '"
								+ relativePath + "' for levelset '" + levelset
								+ "' (looked for " + room.absPath + ").");
						}
						continue;
					}
					gathered[levelset].push_back(std::move(room));
				}
			}
		}

		for ( auto& levelsetRooms : gathered )
		{
			auto& rooms = levelsetRooms.second;
			std::sort(rooms.begin(), rooms.end(), [](const Room& a, const Room& b) {
				if ( a.sortKey != b.sortKey ) { return a.sortKey < b.sortKey; }
				/* A repeated namespace/path may come from malformed duplicate mod
				 * input. Select its effective bytes by digest, never by an absolute
				 * installation path that can sort differently on another peer. */
				if ( a.digest != b.digest ) { return a.digest < b.digest; }
				if ( a.absPath != b.absPath ) { return a.absPath < b.absPath; }
				return false;
			});
			rooms.erase(std::unique(rooms.begin(), rooms.end(),
				[](const Room& a, const Room& b) { return a.sortKey == b.sortKey; }),
				rooms.end());
		}
		return gathered;
	}
}

void SAMRooms::applyAll(const std::vector<SAMModManifest>& mods)
{
	clear();

	RoomsByLevelset gathered = gatherRooms(mods, true);

	for ( auto& kv : gathered )
	{
		// THE invariant. Canonical logical key plus content ordering, rather than mod load
		// order or installation path, defines the generator's shared RNG index space.
		std::vector<std::string>& out = s_byLevelset[kv.first];
		out.reserve(kv.second.size());
		for ( const Room& r : kv.second ) { out.push_back(r.absPath); }
		s_count += (int)out.size();

		SAM_INFO(MOD, "Levelset '" + kv.first + "' gains " + std::to_string(out.size())
			+ " room(s) from mods.");
	}

	if ( s_count > 0 )
	{
		SAM_INFO(MOD, "Injected " + std::to_string(s_count) + " room(s) across "
			+ std::to_string((int)s_byLevelset.size()) + " levelset(s).");
	}
}

std::vector<std::string> SAMRooms::contentFingerprintEntries(
	const std::vector<SAMModManifest>& mods)
{
	std::vector<std::string> entries;
	const RoomsByLevelset gathered = gatherRooms(mods, false);
	for ( const auto& levelsetRooms : gathered )
	{
		for ( const Room& room : levelsetRooms.second )
		{
			entries.push_back("room:" + levelsetRooms.first + ":"
				+ room.sortKey + "@" + room.digest);
		}
	}
	return entries;
}

void SAMRooms::clear()
{
	s_byLevelset.clear();
	s_count = 0;
}

bool SAMRooms::any() { return s_count > 0; }

const std::vector<std::string>& SAMRooms::roomsFor(const std::string& levelset)
{
	if ( s_byLevelset.empty() ) { return s_empty; }
	auto it = s_byLevelset.find(lower(levelset));
	return ( it != s_byLevelset.end() ) ? it->second : s_empty;
}

int SAMRooms::count() { return s_count; }
int SAMRooms::levelsets() { return (int)s_byLevelset.size(); }
