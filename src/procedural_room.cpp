/*-------------------------------------------------------------------------------
    Automatia procedural-room metadata and deterministic catalog primitives.
-------------------------------------------------------------------------------*/

#include "procedural_room.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>

namespace
{
constexpr std::uint16_t kProceduralRoomMetadataVersion = 2;
constexpr std::uint16_t kProceduralRoomLegacyMetadataVersion = 1;
constexpr std::size_t kDefinitionHeaderBytes = 14;

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

std::string trimAndLower(const char* text)
{
	std::string result = text ? text : "";
	while ( !result.empty()
		&& std::isspace(static_cast<unsigned char>(result.front())) )
	{
		result.erase(result.begin());
	}
	while ( !result.empty()
		&& std::isspace(static_cast<unsigned char>(result.back())) )
	{
		result.pop_back();
	}
	for ( char& c : result )
	{
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return result;
}

bool safeLevelset(const std::string& value)
{
	if ( value.empty() || value.size() >= PROCEDURAL_ROOM_LEVELSET_BYTES )
	{
		return false;
	}
	if ( value == "." || value == ".." || value.front() == '/'
		|| value.front() == '\\' || value.find('/') != std::string::npos
		|| value.find('\\') != std::string::npos
		|| value.find(':') != std::string::npos )
	{
		return false;
	}
	for ( const unsigned char c : value )
	{
		if ( c < 32 || c == 127 )
		{
			return false;
		}
	}
	return true;
}

bool safeCustomCategory(const std::string& value)
{
	if ( value.empty() || value.size() >= PROCEDURAL_ROOM_CUSTOM_CATEGORY_BYTES
		|| value == "." || value == ".." )
	{
		return false;
	}
	for ( const unsigned char c : value )
	{
		if ( c < 32 || c == 127 || c == '/' || c == '\\' || c == ':' )
		{
			return false;
		}
	}
	return true;
}

std::string canonicalVirtualPath(const std::string& path)
{
	std::string result = path;
	std::replace(result.begin(), result.end(), '\\', '/');
	while ( result.rfind("./", 0) == 0 )
	{
		result.erase(0, 2);
	}
	while ( result.rfind('/', 0) == 0 )
	{
		result.erase(0, 1);
	}
	return result;
}

bool safeVirtualPath(const std::string& path)
{
	if ( path.empty() || path.find(':') != std::string::npos )
	{
		return false;
	}
	std::size_t start = 0;
	while ( start <= path.size() )
	{
		const std::size_t end = path.find('/', start);
		const std::string component = path.substr(start,
			end == std::string::npos ? std::string::npos : end - start);
		if ( component.empty() || component == "." || component == ".." )
		{
			return false;
		}
		if ( end == std::string::npos )
		{
			break;
		}
		start = end + 1;
	}
	for ( const unsigned char c : path )
	{
		if ( c < 32 || c == 127 )
		{
			return false;
		}
	}
	return true;
}

std::string lowercasePathExtension(const std::string& path)
{
	const std::size_t dot = path.find_last_of('.');
	if ( dot == std::string::npos )
	{
		return {};
	}
	std::string extension = path.substr(dot);
	for ( char& c : extension )
	{
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return extension;
}
}

namespace
{
const ProceduralLevelsetDescriptor kLevelsetDescriptors[] =
{
	{ "mine", "Mines", 0 },
	{ "swamp", "Swamp", 1 },
	{ "labyrinth", "Labyrinth", 2 },
	{ "ruins", "Ruins", 3 },
	{ "hell", "Hell", 4 },
	{ "caves", "Caves", 5 },
	{ "citadel", "Citadel", 6 },
	{ "underworld", "Underworld", 7 }
};

const ProceduralRoomCategoryDescriptor kCategoryDescriptors[] =
{
	{
		PROCEDURAL_ROOM_CATEGORY_NORMAL,
		"Normal Room",
		"Participates in the ordinary room pool for this level set.",
		ProceduralRoomPoolKind::NORMAL,
		true
	},
	{
		PROCEDURAL_ROOM_CATEGORY_SHOP,
		"Shop Room",
		"Uses the shop-room pool; this does not change how often a shop is generated.",
		ProceduralRoomPoolKind::SHOP,
		true
	},
	{
		PROCEDURAL_ROOM_CATEGORY_TREASURE_BRONZE,
		"Treasure - Bronze",
		"Weight affects which room is chosen after the Bronze treasure category is selected.",
		ProceduralRoomPoolKind::TREASURE_BRONZE,
		true
	},
	{
		PROCEDURAL_ROOM_CATEGORY_TREASURE_IRON,
		"Treasure - Iron",
		"Weight affects which room is chosen after the Iron treasure category is selected.",
		ProceduralRoomPoolKind::TREASURE_IRON,
		true
	},
	{
		PROCEDURAL_ROOM_CATEGORY_TREASURE_GOLD,
		"Treasure - Gold",
		"Weight affects which room is chosen after the Gold treasure category is selected.",
		ProceduralRoomPoolKind::TREASURE_GOLD,
		true
	},
	{
		PROCEDURAL_ROOM_CATEGORY_TREASURE_SILVER,
		"Treasure - Silver",
		"Weight affects which room is chosen after the Silver treasure category is selected.",
		ProceduralRoomPoolKind::TREASURE_SILVER,
		true
	},
	{
		PROCEDURAL_ROOM_CATEGORY_SPECIAL,
		"Special Room",
		"Participates in the existing special-room pool for this level set.",
		ProceduralRoomPoolKind::SPECIAL,
		true
	},
	{
		PROCEDURAL_ROOM_CATEGORY_SECRET_DOORWAY,
		"Secret Doorway",
		"Uses the compatible secret-doorway pool and preserves its existing chance rules.",
		ProceduralRoomPoolKind::SECRET_DOORWAY,
		true
	},
	{
		PROCEDURAL_ROOM_CATEGORY_CUSTOM,
		"Custom Room Group...",
		"A named group of rooms added to the ordinary room pool for this level set.",
		ProceduralRoomPoolKind::NORMAL,
		true
	}
};
}

const ProceduralLevelsetDescriptor* proceduralRoomLevelsetDescriptors()
{
	return kLevelsetDescriptors;
}

std::size_t proceduralRoomLevelsetDescriptorCount()
{
	return sizeof(kLevelsetDescriptors) / sizeof(kLevelsetDescriptors[0]);
}

const ProceduralLevelsetDescriptor* proceduralRoomFindLevelsetDescriptor(
	const char* levelset)
{
	const std::string normalized = proceduralRoomNormalizeLevelset(levelset);
	for ( const ProceduralLevelsetDescriptor& descriptor : kLevelsetDescriptors )
	{
		if ( normalized == descriptor.canonicalKey )
		{
			return &descriptor;
		}
	}
	return nullptr;
}

const char* proceduralRoomLevelsetDisplayName(const char* levelset)
{
	if ( const auto* descriptor = proceduralRoomFindLevelsetDescriptor(levelset) )
	{
		return descriptor->displayName;
	}
	return levelset && levelset[0] ? levelset : "Custom Level Set";
}

bool proceduralRoomLevelsetIsKnown(const char* levelset)
{
	return proceduralRoomFindLevelsetDescriptor(levelset) != nullptr;
}

const ProceduralRoomCategoryDescriptor* proceduralRoomCategoryDescriptors()
{
	return kCategoryDescriptors;
}

std::size_t proceduralRoomCategoryDescriptorCount()
{
	return sizeof(kCategoryDescriptors) / sizeof(kCategoryDescriptors[0]);
}

const ProceduralRoomCategoryDescriptor* proceduralRoomFindCategoryDescriptor(
	const std::uint8_t category)
{
	for ( const ProceduralRoomCategoryDescriptor& descriptor : kCategoryDescriptors )
	{
		if ( descriptor.category == category )
		{
			return &descriptor;
		}
	}
	return nullptr;
}

bool proceduralRoomCategoryCompatibleWithLevelset(
	const std::uint8_t category, const char* levelset)
{
	if ( !proceduralRoomFindCategoryDescriptor(category) )
	{
		return false;
	}
	const std::string normalized = proceduralRoomNormalizeLevelset(levelset);
	if ( normalized.empty() || !safeLevelset(normalized) )
	{
		return false;
	}
	/* Secret-doorway generation has source-backed pools only for these three
	 * biomes.  Other categories retain the generator's existing levelset
	 * eligibility rules and may be used by custom/modded levelsets. */
	if ( category == PROCEDURAL_ROOM_CATEGORY_SECRET_DOORWAY )
	{
		return normalized == "mine" || normalized == "swamp"
			|| normalized == "labyrinth";
	}
	return true;
}

const char* proceduralRoomWeightDescription(const std::uint32_t weight)
{
	if ( weight <= 9 ) return "Extremely Rare";
	if ( weight <= 24 ) return "Very Rare";
	if ( weight <= 49 ) return "Rare";
	if ( weight <= 99 ) return "Uncommon";
	if ( weight == 100 ) return "Standard";
	if ( weight <= 199 ) return "Common";
	return "Very Common";
}

std::vector<ProceduralRoomValidationIssue> proceduralRoomValidate(
	const ProceduralRoomDefinition& definition,
	const char* levelset,
	const ProceduralRoomValidationContext& context)
{
	std::vector<ProceduralRoomValidationIssue> issues;
	const std::string normalizedLevelset = proceduralRoomNormalizeLevelset(levelset);
	const auto add = [&issues](const ProceduralRoomValidationSeverity severity,
		const char* code, const std::string& message)
	{
		issues.push_back({ severity, code, message });
	};

	if ( !definition.enabled )
	{
		add(ProceduralRoomValidationSeverity::INFO, "DISABLED",
			"Procedural generation is disabled for this map.");
		return issues;
	}
	if ( !context.mapSaved )
	{
		add(ProceduralRoomValidationSeverity::ERROR, "UNSAVED_MAP",
			"This map has not been saved. Save it before procedural discovery can use it.");
	}
	if ( normalizedLevelset.empty() || !safeLevelset(normalizedLevelset) )
	{
		add(ProceduralRoomValidationSeverity::ERROR, "LEVELSET_INVALID",
			"Level Set must be a non-empty safe generator key.");
	}
	if ( !proceduralRoomCategoryIsRuntimeSupported(definition.category) )
	{
		add(ProceduralRoomValidationSeverity::ERROR, "CATEGORY_UNSUPPORTED",
			"This room category has no runtime consumer.");
	}
	else if ( definition.category == PROCEDURAL_ROOM_CATEGORY_CUSTOM
		&& !proceduralRoomCustomCategoryIsValid(definition.customCategory) )
	{
		add(ProceduralRoomValidationSeverity::ERROR, "CUSTOM_CATEGORY_INVALID",
			"Enter a non-empty custom room-group name without path separators.");
	}
	else if ( !proceduralRoomCategoryCompatibleWithLevelset(
		definition.category, normalizedLevelset.c_str()) )
	{
		add(ProceduralRoomValidationSeverity::ERROR, "CATEGORY_INCOMPATIBLE",
			"This room category is not supported for the selected level set.");
	}
	if ( definition.weight == 0 || definition.weight > PROCEDURAL_ROOM_MAX_WEIGHT )
	{
		add(ProceduralRoomValidationSeverity::ERROR, "WEIGHT_INVALID",
			std::string("Weight must be between 1 and ")
			+ std::to_string(PROCEDURAL_ROOM_MAX_WEIGHT) + ".");
	}
	if ( !proceduralRoomDefinitionIsValid(definition) )
	{
		add(ProceduralRoomValidationSeverity::ERROR, "METADATA_INVALID",
			"Procedural room metadata is malformed.");
	}
	if ( context.mapSaved && !context.pathDiscoverable )
	{
		add(ProceduralRoomValidationSeverity::ERROR, "ROOM_UNDISCOVERABLE",
			"The saved room path is not discoverable through the mounted maps namespace.");
	}
	else if ( context.mapSaved )
	{
		add(ProceduralRoomValidationSeverity::OK, "ROOM_DISCOVERABLE",
			"Room is discoverable through the procedural catalog.");
	}
	if ( context.safeDuplicate )
	{
		add(ProceduralRoomValidationSeverity::WARNING, "SAFE_DUPLICATE",
			"This room is also registered by another source and will appear once after catalog deduplication.");
	}
	if ( context.unsafeConflict )
	{
		add(ProceduralRoomValidationSeverity::ERROR, "UNSAFE_CONFLICT",
			"This room has conflicting procedural and S.A.M. registration data.");
	}
	if ( proceduralRoomLevelsetIsKnown(normalizedLevelset.c_str()) )
	{
		add(ProceduralRoomValidationSeverity::OK, "LEVELSET_KNOWN",
			std::string("Level set \"")
			+ proceduralRoomLevelsetDisplayName(normalizedLevelset.c_str())
			+ "\" is recognized.");
	}
	else if ( !normalizedLevelset.empty() )
	{
		add(ProceduralRoomValidationSeverity::INFO, "LEVELSET_CUSTOM",
			"Arbitrary custom level-set keys are preserved and normalized by the runtime.");
	}
	if ( !context.safeDuplicate && !context.unsafeConflict )
	{
		add(ProceduralRoomValidationSeverity::OK, "NO_DUPLICATE",
			"No duplicate registration found.");
	}
	return issues;
}

void proceduralRoomDefinitionReset(ProceduralRoomDefinition& definition)
{
	std::memset(&definition, 0, sizeof(definition));
	definition.category = PROCEDURAL_ROOM_CATEGORY_NORMAL;
	definition.weight = PROCEDURAL_ROOM_DEFAULT_WEIGHT;
}

std::string proceduralRoomNormalizeLevelset(const char* levelset)
{
	return trimAndLower(levelset);
}

bool proceduralRoomSetLevelset(ProceduralRoomDefinition& definition,
	const char* levelset)
{
	const std::string normalized = proceduralRoomNormalizeLevelset(levelset);
	if ( !normalized.empty() && !safeLevelset(normalized) )
	{
		return false;
	}
	if ( normalized.size() >= PROCEDURAL_ROOM_LEVELSET_BYTES )
	{
		return false;
	}
	std::memset(definition.levelset, 0, sizeof(definition.levelset));
	std::memcpy(definition.levelset, normalized.data(), normalized.size());
	return true;
}

std::string proceduralRoomNormalizeCustomCategory(const char* category)
{
	return trimAndLower(category);
}

bool proceduralRoomSetCustomCategory(ProceduralRoomDefinition& definition,
	const char* category)
{
	const std::string normalized =
		proceduralRoomNormalizeCustomCategory(category);
	if ( !normalized.empty() && !safeCustomCategory(normalized) )
	{
		return false;
	}
	if ( normalized.size() >= PROCEDURAL_ROOM_CUSTOM_CATEGORY_BYTES )
	{
		return false;
	}
	std::memset(definition.customCategory, 0,
		sizeof(definition.customCategory));
	std::memcpy(definition.customCategory, normalized.data(), normalized.size());
	return true;
}

bool proceduralRoomCustomCategoryIsValid(const char* category)
{
	const std::string raw = category ? category : "";
	const std::string normalized =
		proceduralRoomNormalizeCustomCategory(category);
	return raw == normalized && (normalized.empty() || safeCustomCategory(normalized));
}

bool proceduralRoomCategoryIsRuntimeSupported(const std::uint8_t category)
{
	const auto* descriptor = proceduralRoomFindCategoryDescriptor(category);
	return descriptor != nullptr && descriptor->supported;
}

const char* proceduralRoomCategoryName(const std::uint8_t category)
{
	switch ( category )
	{
	case PROCEDURAL_ROOM_CATEGORY_NORMAL: return "normal";
	case PROCEDURAL_ROOM_CATEGORY_SHOP: return "shop";
	case PROCEDURAL_ROOM_CATEGORY_TREASURE_BRONZE: return "treasure_bronze";
	case PROCEDURAL_ROOM_CATEGORY_TREASURE_IRON: return "treasure_iron";
	case PROCEDURAL_ROOM_CATEGORY_TREASURE_GOLD: return "treasure_gold";
	case PROCEDURAL_ROOM_CATEGORY_TREASURE_SILVER: return "treasure_silver";
	case PROCEDURAL_ROOM_CATEGORY_SPECIAL: return "special";
	case PROCEDURAL_ROOM_CATEGORY_SECRET_DOORWAY: return "secret_doorway";
	case PROCEDURAL_ROOM_CATEGORY_CUSTOM: return "custom";
	default: return "unknown";
	}
}

const char* proceduralRoomCategoryDisplayName(const std::uint8_t category)
{
	if ( const auto* descriptor = proceduralRoomFindCategoryDescriptor(category) )
	{
		return descriptor->displayName;
	}
	return "Unsupported";
}

std::string proceduralRoomCategoryKey(
	const ProceduralRoomDefinition& definition)
{
	if ( definition.category == PROCEDURAL_ROOM_CATEGORY_CUSTOM )
	{
		return "custom:" + proceduralRoomNormalizeCustomCategory(
			definition.customCategory);
	}
	return proceduralRoomCategoryName(definition.category);
}

std::string proceduralRoomCategoryDisplayName(
	const ProceduralRoomDefinition& definition)
{
	if ( definition.category == PROCEDURAL_ROOM_CATEGORY_CUSTOM
		&& definition.customCategory[0] )
	{
		return std::string("Custom: ") + definition.customCategory;
	}
	return proceduralRoomCategoryDisplayName(definition.category);
}

bool proceduralRoomDefinitionIsValid(const ProceduralRoomDefinition& definition)
{
	if ( definition.enabled != 0 && definition.enabled != 1 )
	{
		return false;
	}
	if ( definition.category >= PROCEDURAL_ROOM_CATEGORY_MAX
		|| definition.reserved != 0 || definition.weight == 0
		|| definition.weight > PROCEDURAL_ROOM_MAX_WEIGHT )
	{
		return false;
	}
	std::size_t rawLength = 0;
	while ( rawLength < PROCEDURAL_ROOM_LEVELSET_BYTES
		&& definition.levelset[rawLength] != '\0' )
	{
		++rawLength;
	}
	/* A fixed-size map snapshot must contain a terminator before normalization;
	 * never let malformed metadata make strlen/string construction walk past the
	 * field. */
	if ( rawLength >= PROCEDURAL_ROOM_LEVELSET_BYTES )
	{
		return false;
	}
	const std::string rawLevelset(definition.levelset, rawLength);
	const std::string levelset = proceduralRoomNormalizeLevelset(
		rawLevelset.c_str());
	if ( levelset != rawLevelset
		|| (definition.enabled != 0 && !safeLevelset(levelset)) )
	{
		return false;
	}
	std::size_t rawCategoryLength = 0;
	while ( rawCategoryLength < PROCEDURAL_ROOM_CUSTOM_CATEGORY_BYTES
		&& definition.customCategory[rawCategoryLength] != '\0' )
	{
		++rawCategoryLength;
	}
	if ( rawCategoryLength >= PROCEDURAL_ROOM_CUSTOM_CATEGORY_BYTES )
	{
		return false;
	}
	const std::string rawCategory(definition.customCategory, rawCategoryLength);
	if ( !proceduralRoomCustomCategoryIsValid(rawCategory.c_str()) )
	{
		return false;
	}
	if ( definition.category != PROCEDURAL_ROOM_CATEGORY_CUSTOM
		&& !rawCategory.empty() )
	{
		return false;
	}
	if ( definition.category == PROCEDURAL_ROOM_CATEGORY_CUSTOM
		&& definition.enabled != 0 && !safeCustomCategory(rawCategory) )
	{
		return false;
	}
	if ( definition.enabled == 0 )
	{
		return levelset.empty() || safeLevelset(levelset);
	}
	return safeLevelset(levelset);
}

bool serializeProceduralRoomDefinition(
	const ProceduralRoomDefinition& definition,
	std::vector<std::uint8_t>& output)
{
	if ( !proceduralRoomDefinitionIsValid(definition) )
	{
		return false;
	}
	const std::size_t levelsetLength = std::strlen(definition.levelset);
	if ( levelsetLength >= PROCEDURAL_ROOM_LEVELSET_BYTES )
	{
		return false;
	}
	const std::size_t customCategoryLength =
		std::strlen(definition.customCategory);
	if ( customCategoryLength >= PROCEDURAL_ROOM_CUSTOM_CATEGORY_BYTES
		|| definition.category != PROCEDURAL_ROOM_CATEGORY_CUSTOM
		&& customCategoryLength != 0 )
	{
		return false;
	}
	output.clear();
	output.reserve(kDefinitionHeaderBytes + levelsetLength
		+ sizeof(std::uint16_t) + customCategoryLength);
	appendU16(output, kProceduralRoomMetadataVersion);
	appendU16(output, 0);
	output.push_back(definition.enabled);
	output.push_back(definition.category);
	appendU16(output, 0);
	appendU32(output, definition.weight);
	appendU16(output, static_cast<std::uint16_t>(levelsetLength));
	output.insert(output.end(), definition.levelset,
		definition.levelset + levelsetLength);
	appendU16(output, static_cast<std::uint16_t>(customCategoryLength));
	output.insert(output.end(), definition.customCategory,
		definition.customCategory + customCategoryLength);
	return true;
}

bool deserializeProceduralRoomDefinition(
	const std::uint8_t* data,
	const std::size_t size,
	ProceduralRoomDefinition& output)
{
	if ( !data || size < kDefinitionHeaderBytes )
	{
		return false;
	}
	std::size_t offset = 0;
	std::uint16_t version = 0;
	std::uint16_t reserved = 0;
	std::uint16_t flags = 0;
	std::uint16_t levelsetLength = 0;
	std::uint16_t customCategoryLength = 0;
	std::uint32_t weight = 0;
	if ( !readU16(data, size, offset, version)
		|| !readU16(data, size, offset, reserved)
		|| offset + 2 > size )
	{
		return false;
	}
	const std::uint8_t enabled = data[offset++];
	const std::uint8_t category = data[offset++];
	if ( !readU16(data, size, offset, flags)
		|| !readU32(data, size, offset, weight)
		|| !readU16(data, size, offset, levelsetLength)
		|| (version != kProceduralRoomMetadataVersion
			&& version != kProceduralRoomLegacyMetadataVersion)
		|| reserved != 0 || flags != 0
		|| levelsetLength >= PROCEDURAL_ROOM_LEVELSET_BYTES
		|| size - offset < levelsetLength )
	{
		return false;
	}
	if ( enabled > 1 || category >= PROCEDURAL_ROOM_CATEGORY_MAX )
	{
		return false;
	}
	ProceduralRoomDefinition decoded;
	proceduralRoomDefinitionReset(decoded);
	decoded.enabled = enabled;
	decoded.category = category;
	decoded.weight = weight;
	std::string levelset(reinterpret_cast<const char*>(data + offset), levelsetLength);
	offset += levelsetLength;
	if ( version == kProceduralRoomMetadataVersion )
	{
		if ( !readU16(data, size, offset, customCategoryLength)
			|| customCategoryLength >= PROCEDURAL_ROOM_CUSTOM_CATEGORY_BYTES
			|| size - offset != customCategoryLength )
		{
			return false;
		}
		const std::string customCategory(
			reinterpret_cast<const char*>(data + offset), customCategoryLength);
		if ( !proceduralRoomSetCustomCategory(decoded, customCategory.c_str()) )
		{
			return false;
		}
		offset += customCategoryLength;
	}
	else if ( size - offset != 0 )
	{
		return false;
	}
	if ( !proceduralRoomSetLevelset(decoded, levelset.c_str())
		|| !proceduralRoomDefinitionIsValid(decoded) )
	{
		return false;
	}
	output = decoded;
	return true;
}

ProceduralRoomMetadataReadResult findProceduralRoomDefinitionInMapBytes(
	const std::uint8_t* data,
	const std::size_t size,
	ProceduralRoomDefinition& output)
{
	if ( !data || size < 12 )
	{
		return ProceduralRoomMetadataReadResult::ABSENT;
	}
	bool sawEnvelope = false;
	bool sawInvalid = false;
	bool sawDefinition = false;
	ProceduralRoomDefinition decoded;
	for ( std::size_t offset = 0; offset + 12 <= size; ++offset )
	{
		if ( std::memcmp(data + offset, "MPMD", 4) != 0 )
		{
			continue;
		}
		sawEnvelope = true;
		std::size_t headerOffset = offset + 4;
		std::uint16_t version = 0;
		std::uint16_t reserved = 0;
		std::uint32_t payloadLength = 0;
		if ( !readU16(data, size, headerOffset, version)
			|| !readU16(data, size, headerOffset, reserved)
			|| !readU32(data, size, headerOffset, payloadLength)
			|| version != 1 || reserved != 0
			|| payloadLength > 16U * 1024U * 1024U
			|| payloadLength > size - headerOffset )
		{
			sawInvalid = true;
			continue;
		}
		const std::size_t payloadEnd = headerOffset + payloadLength;
		std::size_t chunkOffset = headerOffset;
		bool envelopeInvalid = false;
		bool envelopeDefinition = false;
		while ( chunkOffset < payloadEnd )
		{
			if ( payloadEnd - chunkOffset < 8 )
			{
				envelopeInvalid = true;
				break;
			}
			const char* tag = reinterpret_cast<const char*>(data + chunkOffset);
			chunkOffset += 4;
			std::uint32_t chunkLength = 0;
			if ( !readU32(data, payloadEnd, chunkOffset, chunkLength)
				|| chunkLength > payloadEnd - chunkOffset )
			{
				envelopeInvalid = true;
				break;
			}
			const std::size_t chunkEnd = chunkOffset + chunkLength;
			if ( std::memcmp(tag, "PGRM", 4) == 0 )
			{
				if ( envelopeDefinition
					|| !deserializeProceduralRoomDefinition(data + chunkOffset,
						chunkLength, decoded) )
				{
					envelopeInvalid = true;
					break;
				}
				envelopeDefinition = true;
			}
			chunkOffset = chunkEnd;
		}
		if ( envelopeInvalid || chunkOffset != payloadEnd )
		{
			sawInvalid = true;
			continue;
		}
		if ( envelopeDefinition )
		{
			output = decoded;
			sawDefinition = true;
		}
	}
	if ( sawDefinition )
	{
		return ProceduralRoomMetadataReadResult::FOUND;
	}
	return sawEnvelope && sawInvalid
		? ProceduralRoomMetadataReadResult::INVALID
		: ProceduralRoomMetadataReadResult::ABSENT;
}

bool ProceduralRoomCatalog::add(const std::string& virtualPath,
	const std::string& resolvedPath,
	const ProceduralRoomDefinition& definition,
	const std::string& contentFingerprint)
{
	if ( !definition.enabled || !proceduralRoomDefinitionIsValid(definition)
		|| !proceduralRoomCategoryIsRuntimeSupported(definition.category) )
	{
		return false;
	}
	if ( virtualPath.empty() || virtualPath.front() == '/'
		|| virtualPath.front() == '\\'
		|| virtualPath.find(':') != std::string::npos )
	{
		return false;
	}
	const std::string canonical = canonicalVirtualPath(virtualPath);
	if ( !safeVirtualPath(canonical)
		|| lowercasePathExtension(canonical) != ".lmp" )
	{
		return false;
	}
	ProceduralRoomCandidate candidate;
	candidate.canonicalVirtualPath = canonical;
	candidate.resolvedPath = resolvedPath;
	candidate.definition = definition;
	candidate.contentFingerprint = contentFingerprint;
	candidates.push_back(std::move(candidate));
	return true;
}

void ProceduralRoomCatalog::sortAndDeduplicate()
{
	std::sort(candidates.begin(), candidates.end(),
		[](const ProceduralRoomCandidate& left,
			const ProceduralRoomCandidate& right)
		{
			if ( left.canonicalVirtualPath != right.canonicalVirtualPath )
			{
				return left.canonicalVirtualPath < right.canonicalVirtualPath;
			}
			if ( left.contentFingerprint != right.contentFingerprint )
			{
				return left.contentFingerprint < right.contentFingerprint;
			}
			return left.resolvedPath < right.resolvedPath;
		});
	candidates.erase(std::unique(candidates.begin(), candidates.end(),
		[](const ProceduralRoomCandidate& left,
			const ProceduralRoomCandidate& right)
		{
			return left.canonicalVirtualPath == right.canonicalVirtualPath;
		}), candidates.end());
}

void ProceduralRoomCatalog::clear()
{
	candidates.clear();
}

const std::vector<ProceduralRoomCandidate>& ProceduralRoomCatalog::entries() const
{
	return candidates;
}

std::vector<const ProceduralRoomCandidate*> ProceduralRoomCatalog::matching(
	const std::string& levelset, const std::uint8_t category) const
{
	return matching(levelset, category, nullptr);
}

std::vector<const ProceduralRoomCandidate*> ProceduralRoomCatalog::matching(
	const std::string& levelset, const std::uint8_t category,
	const char* customCategory) const
{
	std::vector<const ProceduralRoomCandidate*> result;
	const std::string normalized = proceduralRoomNormalizeLevelset(levelset.c_str());
	const std::string normalizedCustomCategory =
		proceduralRoomNormalizeCustomCategory(customCategory);
	for ( const ProceduralRoomCandidate& candidate : candidates )
	{
		if ( candidate.definition.category != category
			|| std::strcmp(candidate.definition.levelset, normalized.c_str()) != 0 )
		{
			continue;
		}
		/* Omitting a custom name means all user-defined groups. This is the
		 * ordinary-generator query; passing a name remains useful to tooling and
		 * future category-specific consumers. */
		if ( category == PROCEDURAL_ROOM_CATEGORY_CUSTOM
			&& !normalizedCustomCategory.empty()
			&& proceduralRoomNormalizeCustomCategory(
				candidate.definition.customCategory) != normalizedCustomCategory )
		{
			continue;
		}
		if ( category == PROCEDURAL_ROOM_CATEGORY_CUSTOM
			&& candidate.definition.customCategory[0] == '\0' )
		{
			continue;
		}
		if ( candidate.definition.category == category )
		{
			result.push_back(&candidate);
		}
	}
	return result;
}

std::vector<std::string> ProceduralRoomCatalog::contentFingerprintEntries() const
{
	std::vector<std::string> result;
	result.reserve(candidates.size());
	for ( const ProceduralRoomCandidate& candidate : candidates )
	{
		result.push_back("procedural-room:" + candidate.canonicalVirtualPath
			+ ":" + candidate.definition.levelset
			+ ":" + proceduralRoomCategoryKey(candidate.definition)
			+ ":" + std::to_string(candidate.definition.weight)
			+ "@" + candidate.contentFingerprint);
	}
	return result;
}

bool ProceduralRoomCatalog::chooseWeightedIndex(
	const std::vector<std::uint32_t>& weights,
	const std::vector<bool>& available,
	const std::uint32_t randomValue,
	std::size_t& selectedIndex)
{
	if ( weights.size() != available.size() )
	{
		return false;
	}
	std::uint64_t total = 0;
	for ( std::size_t index = 0; index < weights.size(); ++index )
	{
		if ( available[index] && weights[index] > 0
			&& weights[index] <= PROCEDURAL_ROOM_MAX_WEIGHT )
		{
			total += weights[index];
		}
	}
	if ( total == 0 )
	{
		return false;
	}
	std::uint64_t roll = static_cast<std::uint64_t>(randomValue) % total;
	for ( std::size_t index = 0; index < weights.size(); ++index )
	{
		if ( !available[index] || weights[index] == 0
			|| weights[index] > PROCEDURAL_ROOM_MAX_WEIGHT )
		{
			continue;
		}
		if ( roll < weights[index] )
		{
			selectedIndex = index;
			return true;
		}
		roll -= weights[index];
	}
	return false;
}
