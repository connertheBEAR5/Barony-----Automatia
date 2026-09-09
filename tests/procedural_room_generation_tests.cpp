#include "procedural_room.hpp"
#include "procedural_room_catalog_runtime.hpp"
#include "physfs.h"

#include <cstdio>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
#define CHECK(expression) \
	do \
	{ \
		if ( !(expression) ) \
		{ \
			std::fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, #expression); \
			return 1; \
		} \
	} while ( false )

void appendU32(std::vector<std::uint8_t>& bytes, const std::uint32_t value)
{
	for ( unsigned shift = 0; shift < 32; shift += 8 )
	{
		bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
	}
}

std::vector<std::uint8_t> envelopeWith(
	const std::vector<std::uint8_t>& definitionBytes)
{
	std::vector<std::uint8_t> payload;
	const std::vector<std::uint8_t> unknown = { 'R', 'G', 'R', 'P', 0, 0, 0, 0 };
	payload.insert(payload.end(), unknown.begin(), unknown.end());
	payload.insert(payload.end(), { 'P', 'G', 'R', 'M' });
	appendU32(payload, static_cast<std::uint32_t>(definitionBytes.size()));
	payload.insert(payload.end(), definitionBytes.begin(), definitionBytes.end());
	std::vector<std::uint8_t> bytes = { 'M', 'P', 'M', 'D', 1, 0, 0, 0 };
	appendU32(bytes, static_cast<std::uint32_t>(payload.size()));
	bytes.insert(bytes.end(), payload.begin(), payload.end());
	return bytes;
}

std::string readFile(const std::string& path)
{
	std::ifstream input(path);
	return std::string((std::istreambuf_iterator<char>(input)),
		std::istreambuf_iterator<char>());
}

bool writeBinaryFile(const std::filesystem::path& path,
	const std::vector<std::uint8_t>& bytes)
{
	std::ofstream output(path, std::ios::binary);
	if ( !output )
	{
		return false;
	}
	output.write(reinterpret_cast<const char*>(bytes.data()),
		static_cast<std::streamsize>(bytes.size()));
	return output.good();
}

int testPhysfsRuntimeDiscovery()
{
	const bool wasInitialized = PHYSFS_isInit() != 0;
	const std::filesystem::path root =
		std::filesystem::temp_directory_path()
		/ ("barony_automatia_procedural_room_"
			+ std::to_string(std::chrono::steady_clock::now()
				.time_since_epoch().count()));
	std::error_code filesystemError;
	std::filesystem::create_directories(root / "maps" / "nested",
		filesystemError);
	if ( filesystemError )
	{
		std::fprintf(stderr, "could not create PhysFS fixture directory: %s\n",
			filesystemError.message().c_str());
		return 1;
	}

	bool mounted = false;
	auto cleanup = [&]()
	{
		if ( mounted )
		{
			PHYSFS_unmount(root.string().c_str());
		}
		if ( !wasInitialized )
		{
			PHYSFS_deinit();
		}
		std::error_code ignored;
		std::filesystem::remove_all(root, ignored);
	};
	auto require = [&](const bool condition, const char* expression)
	{
		if ( !condition )
		{
			std::fprintf(stderr, "runtime discovery check failed: %s\n", expression);
			cleanup();
			return false;
		}
		return true;
	};

	if ( !wasInitialized
		&& !PHYSFS_init("procedural_room_generation_tests") )
	{
		std::fprintf(stderr, "PHYSFS_init failed: %s\n",
			PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
		cleanup();
		return 1;
	}
	if ( !PHYSFS_mount(root.string().c_str(), nullptr, 1) )
	{
		std::fprintf(stderr, "PHYSFS_mount failed: %s\n",
			PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
		cleanup();
		return 1;
	}
	mounted = true;

	ProceduralRoomDefinition mine;
	proceduralRoomDefinitionReset(mine);
	mine.enabled = 1;
	mine.weight = 250;
	if ( !proceduralRoomSetLevelset(mine, "mine") )
	{
		cleanup();
		return 1;
	}
	std::vector<std::uint8_t> mineBytes;
	if ( !serializeProceduralRoomDefinition(mine, mineBytes) )
	{
		cleanup();
		return 1;
	}

	ProceduralRoomDefinition disabled = mine;
	disabled.enabled = 0;
	std::vector<std::uint8_t> disabledBytes;
	if ( !serializeProceduralRoomDefinition(disabled, disabledBytes) )
	{
		cleanup();
		return 1;
	}
	ProceduralRoomDefinition swamp = mine;
	if ( !proceduralRoomSetLevelset(swamp, "swamp") )
	{
		cleanup();
		return 1;
	}
	std::vector<std::uint8_t> swampBytes;
	if ( !serializeProceduralRoomDefinition(swamp, swampBytes) )
	{
		cleanup();
		return 1;
	}
	ProceduralRoomDefinition custom = mine;
	custom.category = PROCEDURAL_ROOM_CATEGORY_CUSTOM;
	if ( !proceduralRoomSetCustomCategory(custom, "enemy wing") )
	{
		cleanup();
		return 1;
	}
	std::vector<std::uint8_t> customBytes;
	if ( !serializeProceduralRoomDefinition(custom, customBytes) )
	{
		cleanup();
		return 1;
	}
	const std::vector<std::uint8_t> nestedMine = envelopeWith(mineBytes);
	const std::vector<std::uint8_t> disabledRoom = envelopeWith(disabledBytes);
	const std::vector<std::uint8_t> swampRoom = envelopeWith(swampBytes);
	const std::vector<std::uint8_t> customRoom = envelopeWith(customBytes);
	std::vector<std::uint8_t> malformedRoom = nestedMine;
	malformedRoom.pop_back();
	if ( !writeBinaryFile(root / "maps" / "z_custom_room.lmp", nestedMine)
		|| !writeBinaryFile(root / "maps" / "nested" / "a_custom_room.lmp", nestedMine)
		|| !writeBinaryFile(root / "maps" / "disabled_room.lmp", disabledRoom)
		|| !writeBinaryFile(root / "maps" / "wrong_levelset.lmp", swampRoom)
		|| !writeBinaryFile(root / "maps" / "z_custom_enemy_wing.lmp", customRoom)
		|| !writeBinaryFile(root / "maps" / "bad_metadata.lmp", malformedRoom) )
	{
		cleanup();
		return 1;
	}

	ProceduralRoomCatalogRuntime::refresh();
	const ProceduralRoomCatalog& catalog =
		ProceduralRoomCatalogRuntime::current();
	if ( !require(ProceduralRoomCatalogRuntime::isBuilt(),
		"catalog runtime reports built")
		|| !require(catalog.entries().size() == 4,
			"enabled valid arbitrary .lmp files are discovered")
		|| !require(catalog.entries()[0].canonicalVirtualPath
			== "maps/nested/a_custom_room.lmp",
			"catalog is sorted by canonical virtual path")
		|| !require(catalog.matching("mine",
			PROCEDURAL_ROOM_CATEGORY_NORMAL).size() == 2,
			"matching levelset receives both mine rooms")
		|| !require(catalog.matching("swamp",
			PROCEDURAL_ROOM_CATEGORY_NORMAL).size() == 1,
			"wrong levelset remains isolated from mine pool")
		|| !require(catalog.matching("mine",
			PROCEDURAL_ROOM_CATEGORY_CUSTOM).size() == 1,
			"custom room groups are discovered by the runtime")
		|| !require(catalog.contentFingerprintEntries().size() == 4,
			"catalog exposes deterministic fingerprint entries") )
	{
		return 1;
	}
	cleanup();
	return 0;
}
}

int main()
{
	ProceduralRoomDefinition definition;
	proceduralRoomDefinitionReset(definition);
	CHECK(!definition.enabled);
	CHECK(definition.weight == PROCEDURAL_ROOM_DEFAULT_WEIGHT);
	CHECK(definition.category == PROCEDURAL_ROOM_CATEGORY_NORMAL);
	CHECK(definition.customCategory[0] == '\0');
	CHECK(proceduralRoomDefinitionIsValid(definition));
	CHECK(proceduralRoomLevelsetDescriptorCount() >= 3);
	CHECK(proceduralRoomLevelsetIsKnown("MINE"));
	CHECK(std::string(proceduralRoomLevelsetDisplayName("mine")) == "Mines");
	CHECK(proceduralRoomCategoryDescriptorCount()
		== PROCEDURAL_ROOM_CATEGORY_MAX);
	for ( std::size_t category = 0;
		category < proceduralRoomCategoryDescriptorCount(); ++category )
	{
		CHECK(proceduralRoomCategoryIsRuntimeSupported(
			static_cast<std::uint8_t>(category)));
		CHECK(proceduralRoomFindCategoryDescriptor(
			static_cast<std::uint8_t>(category)) != nullptr);
	}
	CHECK(proceduralRoomCategoryCompatibleWithLevelset(
		PROCEDURAL_ROOM_CATEGORY_SECRET_DOORWAY, "mine"));
	CHECK(!proceduralRoomCategoryCompatibleWithLevelset(
		PROCEDURAL_ROOM_CATEGORY_SECRET_DOORWAY, "hell"));
	CHECK(proceduralRoomCategoryCompatibleWithLevelset(
		PROCEDURAL_ROOM_CATEGORY_CUSTOM, "hell"));
	CHECK(std::string(proceduralRoomWeightDescription(100)) == "Standard");
	definition.enabled = 1;
	definition.weight = 100;
	CHECK(proceduralRoomSetLevelset(definition, "mine"));
	ProceduralRoomValidationContext unsavedContext;
	const auto unsavedIssues = proceduralRoomValidate(
		definition, "mine", unsavedContext);
	CHECK(!unsavedIssues.empty());
	CHECK(unsavedIssues.front().severity
		== ProceduralRoomValidationSeverity::ERROR);
	ProceduralRoomDefinition disabledDefinition = definition;
	disabledDefinition.enabled = 0;
	const auto disabledIssues = proceduralRoomValidate(
		disabledDefinition, "invalid stale key", unsavedContext);
	CHECK(disabledIssues.size() == 1);
	CHECK(disabledIssues.front().severity
		== ProceduralRoomValidationSeverity::INFO);

	definition.weight = 250;
	CHECK(proceduralRoomSetLevelset(definition, "  MiNe  "));
	CHECK(std::string(definition.levelset) == "mine");
	std::vector<std::uint8_t> encoded;
	CHECK(serializeProceduralRoomDefinition(definition, encoded));
	ProceduralRoomDefinition decoded;
	CHECK(deserializeProceduralRoomDefinition(encoded.data(), encoded.size(), decoded));
	CHECK(decoded.enabled == 1 && decoded.weight == 250);
	CHECK(decoded.category == PROCEDURAL_ROOM_CATEGORY_NORMAL);
	CHECK(std::string(decoded.levelset) == "mine");
	CHECK(decoded.customCategory[0] == '\0');

	ProceduralRoomDefinition custom = definition;
	custom.category = PROCEDURAL_ROOM_CATEGORY_CUSTOM;
	CHECK(proceduralRoomSetCustomCategory(custom, "  My Monster Rooms  "));
	CHECK(std::string(custom.customCategory) == "my monster rooms");
	CHECK(proceduralRoomDefinitionIsValid(custom));
	std::vector<std::uint8_t> customEncoded;
	CHECK(serializeProceduralRoomDefinition(custom, customEncoded));
	ProceduralRoomDefinition customDecoded;
	CHECK(deserializeProceduralRoomDefinition(customEncoded.data(),
		customEncoded.size(), customDecoded));
	CHECK(customDecoded.category == PROCEDURAL_ROOM_CATEGORY_CUSTOM);
	CHECK(std::string(customDecoded.customCategory) == "my monster rooms");
	CHECK(proceduralRoomCategoryKey(customDecoded) == "custom:my monster rooms");
	CHECK(proceduralRoomCategoryDisplayName(customDecoded)
		== "Custom: my monster rooms");
	ProceduralRoomDefinition invalidCustom = custom;
	invalidCustom.customCategory[0] = '\0';
	CHECK(!proceduralRoomDefinitionIsValid(invalidCustom));
	CHECK(!proceduralRoomSetCustomCategory(custom, "../unsafe"));

	/* Version-1 PGRM payloads remain readable after the custom-category schema
	 * extension; old built-in maps must not be migrated or rejected. */
	const std::size_t oldLevelsetLength = encoded[12]
		| (static_cast<std::size_t>(encoded[13]) << 8u);
	std::vector<std::uint8_t> legacyEncoded(encoded.begin(),
		encoded.begin() + 14 + oldLevelsetLength);
	legacyEncoded[0] = 1;
	legacyEncoded[1] = 0;
	ProceduralRoomDefinition legacyDecoded;
	CHECK(deserializeProceduralRoomDefinition(legacyEncoded.data(),
		legacyEncoded.size(), legacyDecoded));
	CHECK(legacyDecoded.category == PROCEDURAL_ROOM_CATEGORY_NORMAL);
	CHECK(legacyDecoded.customCategory[0] == '\0');

	const std::vector<std::uint8_t> mapBytes = envelopeWith(encoded);
	ProceduralRoomDefinition fromMap;
	CHECK(findProceduralRoomDefinitionInMapBytes(mapBytes.data(), mapBytes.size(), fromMap)
		== ProceduralRoomMetadataReadResult::FOUND);
	CHECK(fromMap.weight == 250 && std::string(fromMap.levelset) == "mine");
	CHECK(findProceduralRoomDefinitionInMapBytes(nullptr, 0, fromMap)
		== ProceduralRoomMetadataReadResult::ABSENT);
	CHECK(deserializeProceduralRoomDefinition(encoded.data(), encoded.size() - 1, decoded) == false);
	std::vector<std::uint8_t> malformed = encoded;
	malformed[4] = 2;
	CHECK(!deserializeProceduralRoomDefinition(malformed.data(), malformed.size(), decoded));

	ProceduralRoomCatalog catalog;
	CHECK(catalog.add("maps/z_room.lmp", "/physical/z", definition, "z"));
	ProceduralRoomDefinition rare = definition;
	rare.weight = 5;
	CHECK(catalog.add("maps/a_room.lmp", "/physical/a", rare, "a"));
	ProceduralRoomDefinition disabled = definition;
	disabled.enabled = 0;
	CHECK(!catalog.add("maps/ignored.lmp", "/physical/i", disabled, "i"));
	ProceduralRoomDefinition unsupported = definition;
	unsupported.category = PROCEDURAL_ROOM_CATEGORY_SHOP;
	CHECK(catalog.add("maps/shop.lmp", "/physical/shop", unsupported, "shop"));
	CHECK(catalog.add("maps/custom.lmp", "/physical/custom", custom, "custom"));
	CHECK(catalog.add("maps\\z_room.lmp", "/physical/other", definition, "a"));
	catalog.sortAndDeduplicate();
	CHECK(catalog.entries().size() == 4);
	CHECK(catalog.entries()[0].canonicalVirtualPath == "maps/a_room.lmp");
	CHECK(catalog.matching("MINE", PROCEDURAL_ROOM_CATEGORY_NORMAL).size() == 2);
	CHECK(catalog.matching("swamp", PROCEDURAL_ROOM_CATEGORY_NORMAL).empty());
	CHECK(catalog.matching("mine", PROCEDURAL_ROOM_CATEGORY_SHOP).size() == 1);
	CHECK(catalog.matching("mine", PROCEDURAL_ROOM_CATEGORY_CUSTOM).size() == 1);
	CHECK(catalog.matching("mine", PROCEDURAL_ROOM_CATEGORY_CUSTOM,
		"my monster rooms").size() == 1);
	CHECK(catalog.matching("mine", PROCEDURAL_ROOM_CATEGORY_CUSTOM,
		"other group").empty());
	CHECK(catalog.contentFingerprintEntries().size() == 4);

	std::vector<std::uint32_t> weights = { 100, 200, 5 };
	std::vector<bool> available = { true, true, true };
	std::size_t selected = 99;
	CHECK(ProceduralRoomCatalog::chooseWeightedIndex(weights, available, 0, selected)
		&& selected == 0);
	CHECK(ProceduralRoomCatalog::chooseWeightedIndex(weights, available, 100, selected)
		&& selected == 1);
	available[1] = false;
	CHECK(ProceduralRoomCatalog::chooseWeightedIndex(weights, available, 0, selected)
		&& selected == 0);
	std::vector<std::uint32_t> huge(1000, PROCEDURAL_ROOM_MAX_WEIGHT);
	std::vector<bool> hugeAvailable(huge.size(), true);
	CHECK(ProceduralRoomCatalog::chooseWeightedIndex(huge, hugeAvailable, 17, selected));

#ifdef BARONY_SOURCE_DIR
	const std::string root = BARONY_SOURCE_DIR;
	const std::string mapsSource = readFile(root + "/src/maps.cpp");
	const std::string filesSource = readFile(root + "/src/files.cpp");
	const std::string editorSource = readFile(root + "/src/editor.cpp");
	CHECK(mapsSource.find("ProceduralRoomCatalogRuntime::current") != std::string::npos);
	CHECK(mapsSource.find("childEntity->addToCreatureList") != std::string::npos);
	CHECK(filesSource.find("PGRM") != std::string::npos);
	CHECK(editorSource.find("editorOpenProceduralRoomProperties") != std::string::npos);
	CHECK(editorSource.find("GENERATION RULES") != std::string::npos);
	CHECK(editorSource.find("VALIDATE ROOM") != std::string::npos);
	CHECK(editorSource.find("proceduralRoomFindCategoryDescriptor") != std::string::npos);
	CHECK(editorSource.find("proceduralRoomCustomCategoryText") != std::string::npos);
	CHECK(mapsSource.find("CATEGORY_TREASURE_BRONZE") != std::string::npos);
	CHECK(mapsSource.find("CATEGORY_SECRET_DOORWAY") != std::string::npos);
	CHECK(mapsSource.find("CATEGORY_CUSTOM") != std::string::npos);
#endif
	CHECK(testPhysfsRuntimeDiscovery() == 0);
	return 0;
}
