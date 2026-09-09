#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace IllusionMagic
{

constexpr int kFirstSpellId = 225;
constexpr int kSpellCount = 13;
constexpr int kLastSpellId = kFirstSpellId + kSpellCount - 1;

enum SpellId : int
{
	MIRROR_OTHER = kFirstSpellId,
	MIRROR_COPY,
	MIRROR_WALL,
	MIRROR_REFLECT,
	MIRROR_MIMIC,
	MIRROR_REFLECT_LOOT,
	PHANTASM_PATH,
	MIRROR_DUPLICATE_LOOT,
	MIRAGE_WALL,
	PARANOIA,
	VERTICAL_MIRAGE,
	SHADOW_STEP,
	STORE_MAGIC
};

enum class TargetType : std::uint8_t
{
	Self,
	TouchActor,
	TouchEnemy,
	TouchFloor,
	TouchWall,
	SelectedInventoryItem,
	Area
};

struct SpellDefinition
{
	int id;
	const char* internalName;
	const char* displayName;
	TargetType target;
	int mana;
	int difficulty;
	int durationTicks;
	int rangeWorldUnits;
	int radiusWorldUnits;
	int sustainMana;
	int itemLevel;
	int goldValue;
	int iconSourceSpell;
	const char* conciseEffect;
};

const std::array<SpellDefinition, kSpellCount>& definitions();
const SpellDefinition* definition(int spellId);
bool isSpell(int spellId);
int spellbookOffset(int spellId);

// A fourth fixed proficiency array entry would alter established save and
// packet contracts. Illusion progression is therefore a first-class derived
// school value backed by the strongest current magic proficiency.
int effectiveProficiency(int sorcery, int mysticism, int thaumaturgy);

enum class ProficiencyTier : std::uint8_t
{
	Novice,
	Basic,
	Skilled,
	Expert,
	Master,
	Legendary
};

ProficiencyTier proficiencyTier(int proficiency);
const char* proficiencyTierName(ProficiencyTier tier);
std::size_t vaultCapacity(int proficiency);
bool vaultAllowsOldestNewest(int proficiency);
bool vaultAllowsExactSelection(int proficiency);
bool vaultAllowsStudy(int proficiency);

struct StoredSpellSnapshot
{
	int spellId = 0;
	int originalEffectiveMana = 0;
	int originalPower = 0;
	int originalCasterProficiency = 0;
	std::uint32_t capturedTick = 0;
	std::uint32_t studyTicks = 0;
	bool playerLearnable = false;

	bool operator==(const StoredSpellSnapshot& rhs) const;
};

enum class VaultSelection : std::uint8_t
{
	Random,
	Oldest,
	Newest,
	Exact
};

struct StoredRelease
{
	StoredSpellSnapshot snapshot;
	int releaseMana = 1;
	int releasePower = 1;
	std::size_t sourceIndex = 0;
};

class StoredMagicVault
{
public:
	bool capture(const StoredSpellSnapshot& snapshot, int proficiency);
	bool full(int proficiency) const;
	std::size_t size() const;
	bool empty() const;
	const std::vector<StoredSpellSnapshot>& entries() const;
	void clear();
	bool discard(std::size_t index);
	void accrueStudy(std::uint32_t ticks, int proficiency);
	bool readyToLearn(std::size_t index, int proficiency) const;
	bool release(
		VaultSelection requestedSelection,
		std::size_t requestedIndex,
		std::uint32_t deterministicRandom,
		int currentProficiency,
		int nativePower,
		StoredRelease& out);

private:
	std::vector<StoredSpellSnapshot> stored_;
};

int storedReleaseMana(int originalEffectiveMana, int originalProficiency,
	int currentProficiency);
int storedReleasePower(int capturedPower, int originalProficiency,
	int currentProficiency, int nativePower);
bool storeMagicEligible(int spellId, bool projectileDelivery,
	bool playerLearnable);

struct ItemDuplicationSafety
{
	bool questItem = false;
	bool key = false;
	bool artifact = false;
	bool specialProgression = false;
	bool container = false;
	bool customStableDefinitionAvailable = true;
};

bool mayCreateRealDuplicate(const ItemDuplicationSafety& safety);
int mirroredDuplicateBeatitude(int sourceBeatitude);

enum class OverlayKind : std::uint8_t
{
	FakeWall,
	HiddenWall,
	FakePit,
	PhantasmPath
};

struct SpatialScope
{
	std::string mapInstance;
	int playableFloor = 0;

	bool operator==(const SpatialScope& rhs) const;
};

struct TileCoord
{
	int x = 0;
	int y = 0;

	bool operator==(const TileCoord& rhs) const;
};

struct NavigationOverlay
{
	std::uint64_t id = 0;
	OverlayKind kind = OverlayKind::FakeWall;
	SpatialScope scope;
	std::uint32_t ownerUid = 0;
	std::uint32_t expiresTick = 0;
	std::vector<std::uint32_t> authorizedActorUids;
	std::vector<TileCoord> tiles;
};

class OverlayRegistry
{
public:
	static constexpr std::size_t kMaximumOverlays = 32;
	static constexpr std::size_t kMaximumTilesPerOverlay = 8;
	static constexpr std::size_t kMaximumAuthorizedActors = 32;

	bool add(NavigationOverlay overlay, std::uint64_t* assignedId = nullptr);
	void expire(std::uint32_t now);
	void clear();
	std::size_t size() const;
	bool hostileAvoids(const SpatialScope& scope, int x, int y) const;
	bool hidesWall(const SpatialScope& scope, int x, int y) const;
	bool hidesFloor(const SpatialScope& scope, int x, int y) const;
	bool supportsActor(const SpatialScope& scope, int x, int y,
		std::uint32_t actorUid) const;
	const std::vector<NavigationOverlay>& entries() const;

private:
	std::vector<NavigationOverlay> overlays_;
	std::uint64_t nextId_ = 1;
};

} // namespace IllusionMagic
