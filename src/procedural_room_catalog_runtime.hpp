/* Runtime PhysFS-backed procedural room catalog. */

#pragma once

#include "procedural_room.hpp"

class ProceduralRoomCatalogRuntime
{
public:
	/* Rebuild after content/mod mounts change.  Discovery is deterministic and
	 * scans the mounted virtual maps namespace, never an OS directory. */
	static void refresh();
	static const ProceduralRoomCatalog& current();
	static bool isBuilt();
	static std::vector<std::string> contentFingerprintEntries();
};

