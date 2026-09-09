/*-------------------------------------------------------------------------------

    Automatia procedural-room metadata and deterministic selection primitives.

    This is intentionally separate from AuthoredRoomGroup.  AuthoredRoomGroup
    describes an editor cuboid inside a map; ProceduralRoomDefinition describes
    the whole .lmp as a candidate for normal dungeon generation.

-------------------------------------------------------------------------------*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

constexpr std::size_t PROCEDURAL_ROOM_LEVELSET_BYTES = 64;
constexpr std::size_t PROCEDURAL_ROOM_CUSTOM_CATEGORY_BYTES = 64;
constexpr std::uint32_t PROCEDURAL_ROOM_DEFAULT_WEIGHT = 100;
constexpr std::uint32_t PROCEDURAL_ROOM_MAX_WEIGHT = 100000;

enum ProceduralRoomCategory : std::uint8_t
{
	PROCEDURAL_ROOM_CATEGORY_NORMAL = 0,
	PROCEDURAL_ROOM_CATEGORY_SHOP = 1,
	PROCEDURAL_ROOM_CATEGORY_TREASURE_BRONZE = 2,
	PROCEDURAL_ROOM_CATEGORY_TREASURE_IRON = 3,
	PROCEDURAL_ROOM_CATEGORY_TREASURE_GOLD = 4,
	PROCEDURAL_ROOM_CATEGORY_TREASURE_SILVER = 5,
	PROCEDURAL_ROOM_CATEGORY_SPECIAL = 6,
	PROCEDURAL_ROOM_CATEGORY_SECRET_DOORWAY = 7,
	/* A named custom group is consumed by the ordinary room generator pool.
	 * It remains a distinct persisted category so authored names do not get
	 * confused with the built-in source-backed pools. */
	PROCEDURAL_ROOM_CATEGORY_CUSTOM = 8,
	PROCEDURAL_ROOM_CATEGORY_MAX = 9
};

enum class ProceduralRoomPoolKind : std::uint8_t
{
	NORMAL,
	SHOP,
	TREASURE_BRONZE,
	TREASURE_IRON,
	TREASURE_GOLD,
	TREASURE_SILVER,
	SPECIAL,
	SECRET_DOORWAY
};

struct ProceduralLevelsetDescriptor
{
	const char* canonicalKey;
	const char* displayName;
	int sortOrder;
};

struct ProceduralRoomCategoryDescriptor
{
	std::uint8_t category;
	const char* displayName;
	const char* helpText;
	ProceduralRoomPoolKind poolKind;
	bool supported;
};

const ProceduralLevelsetDescriptor* proceduralRoomLevelsetDescriptors();
std::size_t proceduralRoomLevelsetDescriptorCount();
const ProceduralLevelsetDescriptor* proceduralRoomFindLevelsetDescriptor(
	const char* levelset);
const char* proceduralRoomLevelsetDisplayName(const char* levelset);
bool proceduralRoomLevelsetIsKnown(const char* levelset);

const ProceduralRoomCategoryDescriptor* proceduralRoomCategoryDescriptors();
std::size_t proceduralRoomCategoryDescriptorCount();
const ProceduralRoomCategoryDescriptor* proceduralRoomFindCategoryDescriptor(
	std::uint8_t category);
bool proceduralRoomCategoryCompatibleWithLevelset(
	std::uint8_t category, const char* levelset);
const char* proceduralRoomWeightDescription(std::uint32_t weight);

/* Keep this structure fixed-size/trivially-copyable.  Zed's existing undo
 * snapshots use malloc'd map_t storage, so introducing a std::string here
 * would make those snapshots unsafe. */
struct ProceduralRoomDefinition
{
	std::uint8_t enabled = 0;
	std::uint8_t category = PROCEDURAL_ROOM_CATEGORY_NORMAL;
	std::uint16_t reserved = 0;
	std::uint32_t weight = PROCEDURAL_ROOM_DEFAULT_WEIGHT;
	char levelset[PROCEDURAL_ROOM_LEVELSET_BYTES] = {};
	char customCategory[PROCEDURAL_ROOM_CUSTOM_CATEGORY_BYTES] = {};
};

static_assert(std::is_trivially_copyable<ProceduralRoomDefinition>::value,
	"Procedural room metadata must remain safe in map snapshots");

enum class ProceduralRoomValidationSeverity : std::uint8_t
{
	OK,
	INFO,
	WARNING,
	ERROR
};

struct ProceduralRoomValidationIssue
{
	ProceduralRoomValidationSeverity severity =
		ProceduralRoomValidationSeverity::INFO;
	const char* code = "";
	std::string message;
};

struct ProceduralRoomValidationContext
{
	bool mapSaved = false;
	bool pathDiscoverable = false;
	bool safeDuplicate = false;
	bool unsafeConflict = false;
};

std::vector<ProceduralRoomValidationIssue> proceduralRoomValidate(
	const ProceduralRoomDefinition& definition,
	const char* levelset,
	const ProceduralRoomValidationContext& context);

void proceduralRoomDefinitionReset(ProceduralRoomDefinition& definition);

std::string proceduralRoomNormalizeLevelset(const char* levelset);
bool proceduralRoomSetLevelset(ProceduralRoomDefinition& definition,
	const char* levelset);
std::string proceduralRoomNormalizeCustomCategory(const char* category);
bool proceduralRoomSetCustomCategory(ProceduralRoomDefinition& definition,
	const char* category);
bool proceduralRoomCustomCategoryIsValid(const char* category);

bool proceduralRoomDefinitionIsValid(const ProceduralRoomDefinition& definition);
bool proceduralRoomCategoryIsRuntimeSupported(std::uint8_t category);
const char* proceduralRoomCategoryName(std::uint8_t category);
const char* proceduralRoomCategoryDisplayName(std::uint8_t category);
std::string proceduralRoomCategoryKey(
	const ProceduralRoomDefinition& definition);
std::string proceduralRoomCategoryDisplayName(
	const ProceduralRoomDefinition& definition);

bool serializeProceduralRoomDefinition(
	const ProceduralRoomDefinition& definition,
	std::vector<std::uint8_t>& output);

bool deserializeProceduralRoomDefinition(
	const std::uint8_t* data,
	std::size_t size,
	ProceduralRoomDefinition& output);

enum class ProceduralRoomMetadataReadResult : std::uint8_t
{
	ABSENT,
	FOUND,
	INVALID
};

/* Locate a PGRM chunk in one or more MPMD envelopes embedded in a map file.
 * This parser deliberately does not know about PhysFS or map_t, which keeps
 * metadata discovery headless and makes it straightforward to test. */
ProceduralRoomMetadataReadResult findProceduralRoomDefinitionInMapBytes(
	const std::uint8_t* data,
	std::size_t size,
	ProceduralRoomDefinition& output);

struct ProceduralRoomCandidate
{
	std::string canonicalVirtualPath;
	std::string resolvedPath;
	ProceduralRoomDefinition definition{};
	std::string contentFingerprint;
};

class ProceduralRoomCatalog
{
public:
	bool add(const std::string& virtualPath, const std::string& resolvedPath,
		const ProceduralRoomDefinition& definition,
		const std::string& contentFingerprint = {});

	void sortAndDeduplicate();
	void clear();

	const std::vector<ProceduralRoomCandidate>& entries() const;
	std::vector<const ProceduralRoomCandidate*> matching(
	const std::string& levelset, std::uint8_t category) const;
	std::vector<const ProceduralRoomCandidate*> matching(
		const std::string& levelset, std::uint8_t category,
		const char* customCategory) const;
	std::vector<std::string> contentFingerprintEntries() const;

	static bool chooseWeightedIndex(
		const std::vector<std::uint32_t>& weights,
		const std::vector<bool>& available,
		std::uint32_t randomValue,
		std::size_t& selectedIndex);

private:
	std::vector<ProceduralRoomCandidate> candidates;
};
