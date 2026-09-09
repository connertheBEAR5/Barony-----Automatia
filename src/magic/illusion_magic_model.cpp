#include "illusion_magic_model.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace IllusionMagic
{
namespace
{
constexpr std::uint32_t kStudyTicksRequired = 5U * 60U * 50U;

const std::array<SpellDefinition, kSpellCount> kDefinitions = {{
	{ MIRROR_OTHER, "spell_mirror_other", "Mirror Other",
		TargetType::TouchActor, 8, 0, 500, 64, 0, 0, 3, 180, 16,
		"Copies an actor's appearance without copying identity or ownership." },
	{ MIRROR_COPY, "spell_mirror_copy", "Mirror Copy",
		TargetType::TouchEnemy, 14, 40, 300, 64, 0, 0, 12, 500, 148,
		"Creates a harmless, temporary shadow-copy decoy." },
	{ MIRROR_WALL, "spell_mirror_wall", "Mirror Wall",
		TargetType::TouchFloor, 8, 20, 400, 32, 16, 0, 6, 280, 183,
		"Creates a fake passable wall that fooled hostiles route around." },
	{ MIRROR_REFLECT, "spell_mirror_reflect", "Mirror Reflect",
		TargetType::Self, 24, 60, 300, 0, 0, 0, 22, 1200, 26,
		"Reflects eligible hostile magic toward its caster with modest power." },
	{ MIRROR_MIMIC, "spell_mirror_mimic", "Mirror Mimic",
		TargetType::TouchEnemy, 20, 60, 500, 64, 0, 0, 20, 1000, 52,
		"Copies creature appearance and temporarily fools eligible hostile AI." },
	{ MIRROR_REFLECT_LOOT, "spell_mirror_reflect_loot", "Mirror Reflect Loot",
		TargetType::SelectedInventoryItem, 18, 60, 300, 0, 0, 0, 18, 800, 8,
		"Creates a visibly fake, non-persistent reflection of a held item." },
	{ PHANTASM_PATH, "spell_phantasm_path", "Phantasm Path",
		TargetType::TouchFloor, 20, 60, 400, 64, 16, 0, 24, 1300, 86,
		"Supports the caster and authorized allies over a short real gap." },
	{ MIRROR_DUPLICATE_LOOT, "spell_mirror_duplicate_loot",
		"Mirror Duplicate Loot", TargetType::SelectedInventoryItem,
		100, 100, 0, 0, 0, 0, 35, 5000, 141,
		"Creates one real safety-audited duplicate with mirrored beatitude." },
	{ MIRAGE_WALL, "spell_mirage_wall", "Mirage Wall",
		TargetType::TouchWall, 10, 20, 400, 32, 16, 0, 8, 350, 159,
		"Visually hides a real wall while preserving collision and pathing." },
	{ PARANOIA, "spell_paranoia", "Paranoia",
		TargetType::Area, 25, 60, 250, 64, 48, 0, 20, 900, 11,
		"Temporarily distorts eligible enemies' target perception." },
	{ VERTICAL_MIRAGE, "spell_vertical_mirage", "Vertical Mirage",
		TargetType::TouchFloor, 12, 40, 300, 48, 16, 0, 14, 600, 184,
		"Shows a false pit over real floor and makes fooled hostiles avoid it." },
	{ SHADOW_STEP, "spell_shadow_step", "Shadow Step",
		TargetType::Self, 12, 40, 100, 0, 0, 0, 15, 650, 51,
		"Moves exactly four tiles backward and leaves a two-second decoy." },
	{ STORE_MAGIC, "spell_store_magic", "Store Magic",
		TargetType::Self, 12, 80, 300, 0, 0, 1, 30, 1800, 155,
		"Sustains a bounded vault that captures one eligible hostile spell." }
}};

bool tileMatches(const NavigationOverlay& overlay, const SpatialScope& scope,
	int x, int y)
{
	if (!(overlay.scope == scope))
	{
		return false;
	}
	return std::find(overlay.tiles.begin(), overlay.tiles.end(), TileCoord{x, y})
		!= overlay.tiles.end();
}
}

const std::array<SpellDefinition, kSpellCount>& definitions()
{
	return kDefinitions;
}

const SpellDefinition* definition(const int spellId)
{
	if (!isSpell(spellId))
	{
		return nullptr;
	}
	return &kDefinitions[static_cast<std::size_t>(spellId - kFirstSpellId)];
}

bool isSpell(const int spellId)
{
	return spellId >= kFirstSpellId && spellId <= kLastSpellId;
}

int spellbookOffset(const int spellId)
{
	return isSpell(spellId) ? spellId - kFirstSpellId : -1;
}

int effectiveProficiency(const int sorcery, const int mysticism,
	const int thaumaturgy)
{
	return std::clamp(std::max({sorcery, mysticism, thaumaturgy}), 0, 100);
}

ProficiencyTier proficiencyTier(const int proficiency)
{
	if (proficiency >= 100) { return ProficiencyTier::Legendary; }
	if (proficiency >= 80) { return ProficiencyTier::Master; }
	if (proficiency >= 60) { return ProficiencyTier::Expert; }
	if (proficiency >= 40) { return ProficiencyTier::Skilled; }
	if (proficiency >= 20) { return ProficiencyTier::Basic; }
	return ProficiencyTier::Novice;
}

const char* proficiencyTierName(const ProficiencyTier tier)
{
	switch (tier)
	{
		case ProficiencyTier::Novice: return "Novice";
		case ProficiencyTier::Basic: return "Basic";
		case ProficiencyTier::Skilled: return "Skilled";
		case ProficiencyTier::Expert: return "Expert";
		case ProficiencyTier::Master: return "Master";
		case ProficiencyTier::Legendary: return "Legendary";
	}
	return "Novice";
}

std::size_t vaultCapacity(const int proficiency)
{
	if (proficiency >= 80) { return 5; }
	if (proficiency >= 60) { return 4; }
	if (proficiency >= 40) { return 3; }
	if (proficiency >= 20) { return 2; }
	return 1;
}

bool vaultAllowsOldestNewest(const int proficiency)
{
	return proficiency >= 40;
}

bool vaultAllowsExactSelection(const int proficiency)
{
	return proficiency >= 60;
}

bool vaultAllowsStudy(const int proficiency)
{
	return proficiency >= 80;
}

bool StoredSpellSnapshot::operator==(const StoredSpellSnapshot& rhs) const
{
	return spellId == rhs.spellId
		&& originalEffectiveMana == rhs.originalEffectiveMana
		&& originalPower == rhs.originalPower
		&& originalCasterProficiency == rhs.originalCasterProficiency
		&& capturedTick == rhs.capturedTick
		&& studyTicks == rhs.studyTicks
		&& playerLearnable == rhs.playerLearnable;
}

bool StoredMagicVault::capture(const StoredSpellSnapshot& snapshot,
	const int proficiency)
{
	if (snapshot.spellId <= 0 || snapshot.originalEffectiveMana < 0
		|| snapshot.originalPower < 0 || full(proficiency))
	{
		return false;
	}
	stored_.push_back(snapshot);
	return true;
}

bool StoredMagicVault::full(const int proficiency) const
{
	return stored_.size() >= vaultCapacity(proficiency);
}

std::size_t StoredMagicVault::size() const
{
	return stored_.size();
}

bool StoredMagicVault::empty() const
{
	return stored_.empty();
}

const std::vector<StoredSpellSnapshot>& StoredMagicVault::entries() const
{
	return stored_;
}

void StoredMagicVault::clear()
{
	stored_.clear();
}

bool StoredMagicVault::discard(const std::size_t index)
{
	if (index >= stored_.size())
	{
		return false;
	}
	stored_.erase(stored_.begin() + static_cast<std::ptrdiff_t>(index));
	return true;
}

void StoredMagicVault::accrueStudy(const std::uint32_t amount,
	const int proficiency)
{
	if (!vaultAllowsStudy(proficiency))
	{
		return;
	}
	for (auto& snapshot : stored_)
	{
		if (!snapshot.playerLearnable)
		{
			continue;
		}
		const auto room = std::numeric_limits<std::uint32_t>::max()
			- snapshot.studyTicks;
		snapshot.studyTicks += std::min(room, amount);
	}
}

bool StoredMagicVault::readyToLearn(const std::size_t index,
	const int proficiency) const
{
	return index < stored_.size()
		&& vaultAllowsStudy(proficiency)
		&& stored_[index].playerLearnable
		&& stored_[index].studyTicks >= kStudyTicksRequired;
}

bool StoredMagicVault::release(const VaultSelection requestedSelection,
	const std::size_t requestedIndex, const std::uint32_t deterministicRandom,
	const int currentProficiency, const int nativePower, StoredRelease& out)
{
	if (stored_.empty())
	{
		return false;
	}

	std::size_t index = 0;
	if (!vaultAllowsOldestNewest(currentProficiency))
	{
		index = deterministicRandom % stored_.size();
	}
	else if (!vaultAllowsExactSelection(currentProficiency))
	{
		index = requestedSelection == VaultSelection::Newest
			? stored_.size() - 1 : 0;
	}
	else
	{
		switch (requestedSelection)
		{
			case VaultSelection::Random:
				index = deterministicRandom % stored_.size();
				break;
			case VaultSelection::Newest:
				index = stored_.size() - 1;
				break;
			case VaultSelection::Exact:
				index = std::min(requestedIndex, stored_.size() - 1);
				break;
			case VaultSelection::Oldest:
			default:
				index = 0;
				break;
		}
	}

	out.snapshot = stored_[index];
	out.sourceIndex = index;
	out.releaseMana = storedReleaseMana(
		out.snapshot.originalEffectiveMana,
		out.snapshot.originalCasterProficiency,
		currentProficiency);
	out.releasePower = storedReleasePower(
		out.snapshot.originalPower,
		out.snapshot.originalCasterProficiency,
		currentProficiency,
		nativePower);
	stored_.erase(stored_.begin() + static_cast<std::ptrdiff_t>(index));
	return true;
}

int storedReleaseMana(const int originalEffectiveMana,
	const int originalProficiency, const int currentProficiency)
{
	int cost = std::max(1, (std::max(0, originalEffectiveMana) + 3) / 4);
	const int deficit = originalProficiency - currentProficiency;
	if (deficit >= 20)
	{
		cost += (deficit + 19) / 20;
	}
	return cost;
}

int storedReleasePower(const int capturedPower, const int originalProficiency,
	const int currentProficiency, const int nativePower)
{
	const int delta = currentProficiency - originalProficiency;
	int percent = 100;
	if (delta < 0)
	{
		percent = std::max(50, 100 + delta / 2);
	}
	else
	{
		percent = std::min(115, 100 + delta / 4);
	}
	int result = std::max(1, std::max(0, capturedPower) * percent / 100);
	if (nativePower > 1)
	{
		result = std::min(result, nativePower - 1);
	}
	return result;
}

bool storeMagicEligible(const int spellId, const bool projectileDelivery,
	const bool playerLearnable)
{
	if (spellId <= 0 || !projectileDelivery)
	{
		return false;
	}
	switch (spellId)
	{
		case 26:  // ordinary Reflection
		case 44:  // Amplify Magic
		case 100: // Overcharge
		case 155: // Absorb Magic
		case 156: // Seize Magic
		case MIRROR_REFLECT:
		case MIRROR_DUPLICATE_LOOT:
		case STORE_MAGIC:
			return false;
		default:
			break;
	}
	// Runtime-only NPC/debug/custom projectile definitions can carry bespoke
	// state that this bounded snapshot intentionally does not serialize. A
	// normal learnable spellbook mapping is the conservative proof that the
	// existing spell registry can safely copy and recast this spell.
	return playerLearnable;
}

bool mayCreateRealDuplicate(const ItemDuplicationSafety& safety)
{
	return !safety.questItem && !safety.key && !safety.artifact
		&& !safety.specialProgression && !safety.container
		&& safety.customStableDefinitionAvailable;
}

int mirroredDuplicateBeatitude(const int sourceBeatitude)
{
	// A duplicate never upgrades curse state: blessed becomes equivalently
	// cursed, neutral becomes mildly cursed, and cursed remains cursed.
	if (sourceBeatitude > 0)
	{
		return -sourceBeatitude;
	}
	return sourceBeatitude == 0 ? -1 : sourceBeatitude;
}

bool SpatialScope::operator==(const SpatialScope& rhs) const
{
	return mapInstance == rhs.mapInstance
		&& playableFloor == rhs.playableFloor;
}

bool TileCoord::operator==(const TileCoord& rhs) const
{
	return x == rhs.x && y == rhs.y;
}

bool OverlayRegistry::add(NavigationOverlay overlay, std::uint64_t* assignedId)
{
	if (overlay.scope.mapInstance.empty() || overlay.tiles.empty())
	{
		return false;
	}
	if (overlay.tiles.size() > kMaximumTilesPerOverlay)
	{
		overlay.tiles.resize(kMaximumTilesPerOverlay);
	}
	if (overlay.authorizedActorUids.size() > kMaximumAuthorizedActors)
	{
		overlay.authorizedActorUids.resize(kMaximumAuthorizedActors);
	}
	if (overlay.id != 0)
	{
		auto existing = std::find_if(overlays_.begin(), overlays_.end(),
			[&](const NavigationOverlay& entry) {
				return entry.id == overlay.id
					&& entry.scope == overlay.scope;
			});
		if (existing != overlays_.end())
		{
			*existing = std::move(overlay);
			if (assignedId) { *assignedId = existing->id; }
			return true;
		}
		nextId_ = std::max(nextId_, overlay.id + 1);
	}
	else
	{
		overlay.id = nextId_++;
	}
	if (overlays_.size() >= kMaximumOverlays)
	{
		return false;
	}
	if (assignedId) { *assignedId = overlay.id; }
	overlays_.push_back(std::move(overlay));
	return true;
}

void OverlayRegistry::expire(const std::uint32_t now)
{
	overlays_.erase(std::remove_if(overlays_.begin(), overlays_.end(),
		[now](const NavigationOverlay& overlay) {
			return overlay.expiresTick != 0 && overlay.expiresTick <= now;
		}), overlays_.end());
}

void OverlayRegistry::clear()
{
	overlays_.clear();
}

std::size_t OverlayRegistry::size() const
{
	return overlays_.size();
}

bool OverlayRegistry::hostileAvoids(const SpatialScope& scope,
	const int x, const int y) const
{
	return std::any_of(overlays_.begin(), overlays_.end(),
		[&](const NavigationOverlay& overlay) {
			return (overlay.kind == OverlayKind::FakeWall
				|| overlay.kind == OverlayKind::FakePit)
				&& tileMatches(overlay, scope, x, y);
		});
}

bool OverlayRegistry::hidesWall(const SpatialScope& scope,
	const int x, const int y) const
{
	return std::any_of(overlays_.begin(), overlays_.end(),
		[&](const NavigationOverlay& overlay) {
			return overlay.kind == OverlayKind::HiddenWall
				&& tileMatches(overlay, scope, x, y);
		});
}

bool OverlayRegistry::hidesFloor(const SpatialScope& scope,
	const int x, const int y) const
{
	return std::any_of(overlays_.begin(), overlays_.end(),
		[&](const NavigationOverlay& overlay) {
			return overlay.kind == OverlayKind::FakePit
				&& tileMatches(overlay, scope, x, y);
		});
}

bool OverlayRegistry::supportsActor(const SpatialScope& scope,
	const int x, const int y, const std::uint32_t actorUid) const
{
	return std::any_of(overlays_.begin(), overlays_.end(),
		[&](const NavigationOverlay& overlay) {
			if (overlay.kind != OverlayKind::PhantasmPath
				|| !tileMatches(overlay, scope, x, y))
			{
				return false;
			}
			return actorUid == overlay.ownerUid
				|| std::find(overlay.authorizedActorUids.begin(),
					overlay.authorizedActorUids.end(), actorUid)
					!= overlay.authorizedActorUids.end();
		});
}

const std::vector<NavigationOverlay>& OverlayRegistry::entries() const
{
	return overlays_;
}

} // namespace IllusionMagic
