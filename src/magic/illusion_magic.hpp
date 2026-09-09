#pragma once

#include "illusion_magic_model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class Entity;
class Item;
class Stat;
struct spell_t;
struct CastSpellProps_t;

namespace IllusionMagic
{

bool enabled();
bool contentAvailable(int spellId);
int effectiveProficiency(const Entity* caster);
int proficiencyForSpell(const Entity* caster, Stat* stats, int spellId,
	int fallbackSkillId);

// The normal cast pipeline performs animation, mana, spellbook and packet
// handling. Once it reaches authoritative effect resolution, Illusion spells
// branch here instead of pretending to be one of the legacy spell elements.
Entity* cast(Entity& caster, spell_t& spell, CastSpellProps_t* properties,
	spell_t*& channeledSpell);

// Per-tick lifetime/study maintenance. Ephemeral state is deliberately kept
// out of map/character persistence.
void tick();
void clearRuntimeState();

// Host-side projectile defence hooks. tryStoreIncoming() consumes exactly one
// eligible hostile projectile when successful. Mirror Reflect is considered
// only after ordinary Reflection reports no result.
bool tryStoreIncoming(Entity& target, Entity& projectile,
	const spell_t& incomingSpell, Entity* originalCaster, int capturedPower);
bool shouldMirrorReflect(const Entity& target, const Entity& projectile,
	const spell_t& incomingSpell, const Entity* originalCaster);
int mirrorReflectBonusDamage(const spell_t& incomingSpell);

// Actor-aware navigation/ground support. All queries derive their scope from
// the current authoritative MapInstance plus the explicit playable floor.
bool applyNavigationOverlays(int* pathMap, int width, int height,
	const Entity& actor, int playableFloor);
bool supportsActorAt(const Entity& actor, int tileX, int tileY);
bool hidesWallAt(int playableFloor, int tileX, int tileY);
bool hidesFloorAt(int playableFloor, int tileX, int tileY);
std::uint64_t visualRevision();

// Receives a bounded server-authored visual/navigation snapshot. Clients never
// choose map ownership; the current active MapInstance and packet floor form
// the receiving scope.
bool receiveOverlaySnapshot(std::uint64_t id, OverlayKind kind,
	int playableFloor, std::uint32_t ownerUid, std::uint32_t remainingTicks,
	const std::vector<std::uint32_t>& authorizedActorUids,
	const std::vector<TileCoord>& tiles);

enum class PhantasmPathPreviewTileState : std::uint8_t
{
	ExistingGround,
	NeedsIllusionSupport,
	Invalid
};

struct PhantasmPathPreview
{
	static constexpr std::size_t kMaximumTiles = 4;

	struct Tile
	{
		TileCoord coordinate;
		PhantasmPathPreviewTileState state =
			PhantasmPathPreviewTileState::Invalid;
	};

	std::array<Tile, kMaximumTiles> tiles{};
	std::size_t count = 0;
	bool castable = false;
};

// One bounded resolver drives both the local pre-cast route display and the
// authoritative bridge creation. Existing floor may be crossed, gaps/liquid
// receive selective support, and any obstacle makes the route invalid.
PhantasmPathPreview previewPhantasmPath(const Entity& caster,
	int targetTileX, int targetTileY);

// Mirror Mimic changes temporary monster perception only. It never rewrites
// faction, identity, party, inventory, or quest ownership.
bool treatsTargetAsNeutral(const Entity& observer, const Entity& target);

// Rendering-only disguise query. The authoritative actor remains present for
// targeting, collision, ownership, and name/identity UI; only its visible body
// is suppressed while its replicated hologram proxy is alive.
bool shouldHideDisguisedBody(const Entity& entity);
std::string disguiseAppearanceName(const Entity& entity);

// Inventory-selection spells reuse Barony's item-effect panel. Selection is a
// request only on clients; the server validates the pending cast, authenticated
// actor, actual inventory ownership, and duplication safety again.
bool canCreateRealDuplicate(const Item& source);
bool selectLootItem(int player, const Item& source, bool realDuplicate);
bool resolveLootSelection(Entity& owner, bool realDuplicate,
	int runtimeType, int status, int beatitude, int count,
	std::uint32_t appearance, bool identified);

struct VaultEntryView
{
	int spellId = 0;
	std::string spellName;
	int releaseMana = 1;
	int capturedPower = 1;
	std::uint32_t studyTicks = 0;
	bool readyToLearn = false;
};

std::size_t vaultSize(const Entity& owner);
std::size_t vaultLimit(const Entity& owner);
std::vector<VaultEntryView> vaultEntries(const Entity& owner);
bool releaseStoredMagic(Entity& owner, VaultSelection selection,
	std::size_t selectedIndex);
bool discardStoredMagic(Entity& owner, std::size_t selectedIndex);
bool storedReleaseInProgress();

bool requestVaultAction(int player, bool discard,
	VaultSelection selection, std::size_t selectedIndex);
bool receiveVaultSnapshot(std::uint32_t ownerUid, std::size_t capacity,
	int proficiency, const std::vector<StoredSpellSnapshot>& entries);

} // namespace IllusionMagic
