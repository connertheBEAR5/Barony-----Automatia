/*-------------------------------------------------------------------------------
    PhysFS-backed procedural-room discovery.
-------------------------------------------------------------------------------*/

#include "procedural_room_catalog_runtime.hpp"

#include "physfs.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
ProceduralRoomCatalog s_catalog;
bool s_built = false;

std::string canonicalPath(std::string path)
{
	std::replace(path.begin(), path.end(), '\\', '/');
	while ( path.rfind("./", 0) == 0 )
	{
		path.erase(0, 2);
	}
	while ( !path.empty() && path.front() == '/' )
	{
		path.erase(path.begin());
	}
	return path;
}

bool hasLmpExtension(const std::string& path)
{
	if ( path.size() < 4 )
	{
		return false;
	}
	const std::string extension = path.substr(path.size() - 4);
	return std::equal(extension.begin(), extension.end(), ".lmp",
		[](const char left, const char right)
		{
			return static_cast<char>(std::tolower(static_cast<unsigned char>(left))) == right;
		});
}

void enumerateVirtualFiles(const std::string& directory,
	std::vector<std::string>& output)
{
	char** entries = PHYSFS_enumerateFiles(directory.c_str());
	if ( !entries )
	{
		return;
	}
	std::vector<std::string> names;
	for ( char** entry = entries; *entry; ++entry )
	{
		names.emplace_back(*entry);
	}
	PHYSFS_freeList(entries);
	std::sort(names.begin(), names.end());
	for ( const std::string& name : names )
	{
		if ( name.empty() || name == "." || name == ".." )
		{
			continue;
		}
		const std::string child = directory.empty()
			? name : directory + "/" + name;
		PHYSFS_Stat stat;
		if ( PHYSFS_stat(child.c_str(), &stat)
			&& stat.filetype == PHYSFS_FILETYPE_DIRECTORY )
		{
			enumerateVirtualFiles(child, output);
		}
		else if ( hasLmpExtension(child) )
		{
			output.push_back(canonicalPath(child));
		}
	}
}

bool readVirtualFile(const std::string& path, std::vector<std::uint8_t>& output)
{
	output.clear();
	PHYSFS_File* file = PHYSFS_openRead(path.c_str());
	if ( !file )
	{
		return false;
	}
	const PHYSFS_sint64 length = PHYSFS_fileLength(file);
	constexpr PHYSFS_sint64 maximumBytes = 64 * 1024 * 1024;
	if ( length <= 0 || length > maximumBytes )
	{
		PHYSFS_close(file);
		return false;
	}
	output.resize(static_cast<std::size_t>(length));
	const PHYSFS_sint64 read = PHYSFS_readBytes(file, output.data(), length);
	PHYSFS_close(file);
	if ( read != length )
	{
		output.clear();
		return false;
	}
	return true;
}

std::string fnv1a64(const std::vector<std::uint8_t>& bytes)
{
	std::uint64_t hash = 14695981039346656037ull;
	for ( const std::uint8_t byte : bytes )
	{
		hash ^= byte;
		hash *= 1099511628211ull;
	}
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%016llx",
		static_cast<unsigned long long>(hash));
	return buffer;
}

std::string resolvedPathFor(const std::string& virtualPath)
{
	const char* realDirectory = PHYSFS_getRealDir(virtualPath.c_str());
	if ( !realDirectory )
	{
		return {};
	}
	std::string path(realDirectory);
	if ( !path.empty() && path.back() != '/' && path.back() != '\\' )
	{
		path += PHYSFS_getDirSeparator();
	}
	path += virtualPath;
	return path;
}
}

void ProceduralRoomCatalogRuntime::refresh()
{
	s_catalog.clear();
	s_built = true;
	if ( !PHYSFS_isInit() )
	{
		return;
	}
	std::vector<std::string> paths;
	enumerateVirtualFiles("maps", paths);
	std::sort(paths.begin(), paths.end());
	paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
	for ( const std::string& virtualPath : paths )
	{
		std::vector<std::uint8_t> bytes;
		if ( !readVirtualFile(virtualPath, bytes) )
		{
			continue;
		}
		ProceduralRoomDefinition definition;
		const ProceduralRoomMetadataReadResult result =
			findProceduralRoomDefinitionInMapBytes(bytes.data(), bytes.size(), definition);
		if ( result == ProceduralRoomMetadataReadResult::INVALID )
		{
			/* Keep malformed optional metadata isolated to this candidate.  The
			 * normal map loader remains the authority for actual map validity. */
			continue;
		}
		if ( result != ProceduralRoomMetadataReadResult::FOUND || !definition.enabled )
		{
			continue;
		}
		if ( !proceduralRoomCategoryIsRuntimeSupported(definition.category) )
		{
			continue;
		}
		const std::string resolvedPath = resolvedPathFor(virtualPath);
		if ( resolvedPath.empty() )
		{
			continue;
		}
		s_catalog.add(virtualPath, resolvedPath, definition, fnv1a64(bytes));
	}
	s_catalog.sortAndDeduplicate();
}

const ProceduralRoomCatalog& ProceduralRoomCatalogRuntime::current()
{
	if ( !s_built )
	{
		refresh();
	}
	return s_catalog;
}

bool ProceduralRoomCatalogRuntime::isBuilt()
{
	return s_built;
}

std::vector<std::string> ProceduralRoomCatalogRuntime::contentFingerprintEntries()
{
	return current().contentFingerprintEntries();
}
