#include "illusion_magic.hpp"

#include "magic.hpp"

#include "../collision.hpp"
#include "../engine/audio/sound.hpp"
#include "../entity.hpp"
#include "../game.hpp"
#include "../main.hpp"
#include "../net.hpp"
#include "../player.hpp"
#include "../interface/interface.hpp"
#include "../stat.hpp"
#include "../world_state.hpp"
#ifdef SAM_FRAMEWORK_ENABLED
#include "../sam/sam_item_registry_foundation.hpp"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <unordered_map>
#include <utility>

namespace IllusionMagic
{
namespace
{
constexpr int kDisguiseProxyOther = 1;
constexpr int kDisguiseProxyMimic = 2;
constexpr int kFakeWallModel = 1221;
constexpr std::size_t kMaximumFakeLootVisuals = 16;
constexpr std::size_t kMaximumHolograms = 32;
constexpr std::size_t kMaximumVaultOwners = MAXPLAYERS + 32;
constexpr std::uint32_t kLootSelectionLifetime = 20U * TICKS_PER_SECOND;

bool validRuntimeItemType(const int type)
{
	if ( type >= 0 && type < NUMITEMS )
	{
		return true;
	}
#ifdef SAM_FRAMEWORK_ENABLED
	return SAMItemRegistryFoundation::isRegisteredRuntimeItemId(type);
#else
	return false;
#endif
}

#ifdef SAM_FRAMEWORK_ENABLED
bool appendStableItemId(const int runtimeType, const int offset)
{
	if ( !SAMItemRegistryFoundation::isRegisteredRuntimeItemId(runtimeType) )
	{
		return true;
	}
	const std::string& stableId =
		SAMItemRegistryFoundation::stableIdForRuntimeId(runtimeType);
	const int available = NET_PACKET_SIZE - offset - 1;
	if ( stableId.empty() || available <= 0
		|| static_cast<int>(stableId.size()) > available )
	{
		printlog("[Illusion Magic] Refusing ILIT custom item without a valid packet stable id.\n");
		return false;
	}
	std::memcpy(&net_packet->data[offset], stableId.data(), stableId.size());
	net_packet->data[offset + stableId.size()] = '\0';
	net_packet->len = offset + 1 + static_cast<int>(stableId.size());
	return true;
}
#endif

struct PendingLootSelection
{
	bool realDuplicate = false;
	std::uint32_t expiresTick = 0;
};

struct ClientVaultDisplay
{
	std::size_t capacity = 0;
	int proficiency = 0;
	std::vector<StoredSpellSnapshot> entries;
};

struct RuntimeState
{
	OverlayRegistry overlays;
	std::unordered_map<std::uint32_t, StoredMagicVault> vaults;
	std::unordered_map<std::uint32_t, std::uint32_t> disguiseProxies;
	std::unordered_map<std::uint32_t, PendingLootSelection>
		pendingLootSelections;
	std::deque<std::uint32_t> fakeLootVisuals;
	std::deque<std::uint32_t> hologramVisuals;
	std::unordered_map<std::uint32_t, ClientVaultDisplay> clientVaults;
	std::uint64_t visualRevision = 1;
	bool featureWasEnabled = false;
	std::array<std::uint64_t, MAXPLAYERS> overlayRevisionSent{};
	std::array<std::uint32_t, MAXPLAYERS> overlayReceiverUid{};
	std::array<int, MAXPLAYERS> overlayReceiverFloor{};
	std::array<std::string, MAXPLAYERS> overlayReceiverInstance{};
	std::string activeInstanceKey;
};

RuntimeState state;
thread_local bool releasingStoredSpell = false;

void resetRuntimeCollections(const bool retireLiveEntities)
{
	if ( retireLiveEntities && multiplayer != CLIENT )
	{
		for ( const std::uint32_t uid : state.fakeLootVisuals )
		{
			if ( Entity* visual = uidToEntity(uid) )
			{
				visual->skill[2] = 1;
			}
		}
		for ( const std::uint32_t uid : state.hologramVisuals )
		{
			if ( Entity* hologram = uidToEntity(uid) )
			{
				hologram->setHP(0);
			}
		}
		// A startup-only flag normally never changes during play, but if an
		// authoritative host removes it, no stale Illusion status icon or
		// sustained channel should remain visible or active.
		if ( map.creatures )
		{
			for ( node_t* node = map.creatures->first; node; node = node->next )
			{
				Entity* creature = static_cast<Entity*>(node->element);
				if ( !creature || !creature->getStats() ) { continue; }
				creature->setEffect(EFF_MIRROR_OTHER, false, 0, true);
				creature->setEffect(EFF_MIRROR_MIMIC, false, 0, true);
				creature->setEffect(EFF_MIRROR_REFLECT, false, 0, true);
				creature->setEffect(EFF_STORE_MAGIC, false, 0, true);
			}
		}
	}
	state.overlays.clear();
	state.vaults.clear();
	state.disguiseProxies.clear();
	state.pendingLootSelections.clear();
	state.fakeLootVisuals.clear();
	state.hologramVisuals.clear();
	state.clientVaults.clear();
	state.overlayRevisionSent.fill(0);
	state.overlayReceiverUid.fill(0);
	state.overlayReceiverFloor.fill(0);
	state.overlayReceiverInstance.fill(std::string{});
	++state.visualRevision;
}

const StoredMagicVault* vaultForConst(const Entity& owner);

int playerIndexFor(const Entity& entity)
{
	return entity.behavior == &actPlayer && entity.skill[2] >= 0
		&& entity.skill[2] < MAXPLAYERS ? entity.skill[2] : -1;
}

SpatialScope scopeForFloor(const int playableFloor)
{
	SpatialScope scope;
	if ( const WorldInstanceIdentity* identity = worldState.activeIdentity() )
	{
		scope.mapInstance = identity->key();
	}
	scope.playableFloor = playableFloor;
	return scope;
}

bool tileInBounds(const int x, const int y)
{
	return x >= 0 && y >= 0 && x < map.width && y < map.height;
}

bool targetWithinRange(const Entity& caster, const Entity& target,
	const SpellDefinition& spellDefinition)
{
	if ( caster.playableFloor != target.playableFloor )
	{
		return false;
	}
	return spellDefinition.rangeWorldUnits <= 0
		|| std::hypot(target.x - caster.x, target.y - caster.y)
			<= spellDefinition.rangeWorldUnits;
}

Entity* targetFromProperties(Entity& caster, CastSpellProps_t* properties,
	const SpellDefinition& spellDefinition)
{
	Entity* target = properties && properties->targetUID
		? uidToEntity(properties->targetUID) : nullptr;
	if ( !target || !target->getStats()
		|| (target->behavior != &actPlayer && target->behavior != &actMonster)
		|| !targetWithinRange(caster, *target, spellDefinition) )
	{
		return nullptr;
	}
	return target;
}

bool targetTileFromProperties(const Entity& caster,
	CastSpellProps_t* properties, const SpellDefinition& spellDefinition,
	int& tileX, int& tileY)
{
	if ( !properties
		|| !std::isfinite(static_cast<double>(properties->target_x))
		|| !std::isfinite(static_cast<double>(properties->target_y)) )
	{
		return false;
	}
	tileX = static_cast<int>(std::floor(properties->target_x / 16.0));
	tileY = static_cast<int>(std::floor(properties->target_y / 16.0));
	if ( !tileInBounds(tileX, tileY) )
	{
		return false;
	}
	const real_t targetX = tileX * 16.0 + 8.0;
	const real_t targetY = tileY * 16.0 + 8.0;
	return spellDefinition.rangeWorldUnits <= 0
		|| std::hypot(targetX - caster.x, targetY - caster.y)
			<= spellDefinition.rangeWorldUnits + 8.0;
}

void effectFeedback(Entity& caster, const char* text)
{
	if ( const int player = playerIndexFor(caster); player >= 0 )
	{
		messagePlayer(player, MESSAGE_STATUS, "%s", text);
	}
	playSoundEntity(&caster, 166, 96);
	spawnMagicEffectParticles(caster.x, caster.y, caster.z, 174);
}

void failureFeedback(Entity& caster, const char* text)
{
	if ( const int player = playerIndexFor(caster); player >= 0 )
	{
		messagePlayer(player, MESSAGE_HINT, "%s", text);
	}
	playSoundEntity(&caster, 163, 64);
}

Entity* createTemporaryVisual(Entity& caster, const int sprite,
	const real_t x, const real_t y, const real_t z, const int duration)
{
	Entity* visual = newEntityWithSpatialContext(
		sprite, 1, map.entities, nullptr, &caster);
	if ( !visual )
	{
		return nullptr;
	}
	visual->x = x;
	visual->y = y;
	visual->z = z;
	visual->yaw = caster.yaw;
	visual->behavior = &actSprite;
	visual->flags[PASSABLE] = true;
	visual->flags[UNCLICKABLE] = true;
	visual->flags[UPDATENEEDED] = true;
	visual->skill[0] = 1;
	visual->skill[1] = 1;
	visual->skill[2] = std::max(1, duration);
	visual->parent = caster.getUID();
	return visual;
}

void openLootSelection(Entity& caster, const int spellId,
	const bool realDuplicate)
{
	state.pendingLootSelections[caster.getUID()] = {
		realDuplicate, ticks + kLootSelectionLifetime
	};
	const int player = playerIndexFor(caster);
	if ( player < 0 )
	{
		return;
	}
	const ItemType spellbook = static_cast<ItemType>(
		getSpellbookFromSpellID(spellId));
	if ( multiplayer == SERVER && player > 0
		&& !players[player]->isLocalPlayer() )
	{
		std::memcpy(net_packet->data, "FXSP", 4);
		net_packet->data[4] = 1;
		net_packet->data[5] = 0;
		SDLNet_Write32(spellId, &net_packet->data[6]);
		net_packet->address.host = net_clients[player - 1].host;
		net_packet->address.port = net_clients[player - 1].port;
		net_packet->len = 10;
		sendPacketSafe(net_sock, -1, net_packet, player - 1);
	}
	else
	{
		GenericGUI[player].openGUI(GUI_TYPE_ITEMFX, nullptr, 0,
			spellbook, spellId);
	}
}

ItemDuplicationSafety duplicationSafety(const Item& source)
{
	ItemDuplicationSafety safety;
	const int runtimeType = static_cast<int>(source.type);
	ItemGeneric& definition = items[runtimeType];
	safety.key = source.type == TOOL_SKELETONKEY
		|| (source.type >= KEY_STONE && source.type <= KEY_MACHINE);
	safety.artifact = (source.type >= ARTIFACT_SWORD
		&& source.type <= ARTIFACT_GLOVES)
		|| (source.type >= ARTIFACT_ORB_BLUE
			&& source.type <= ARTIFACT_ORB_GREEN)
		|| definition.hasAttribute("artifact")
		|| definition.hasAttribute("ARTIFACT");
	safety.questItem = definition.hasAttribute("quest_item")
		|| definition.hasAttribute("QUEST_ITEM");
	safety.specialProgression = source.type == MAGIC_GRIMOIRE
		|| definition.hasAttribute("special_progression")
		|| definition.hasAttribute("SPECIAL_PROGRESSION");
	safety.container = source.type == TOOL_PLAYER_LOOT_BAG
		|| source.type == CLOAK_BACKPACK
		|| definition.hasAttribute("container")
		|| definition.hasAttribute("CONTAINER");
	if ( runtimeType >= SAM_ITEM_ID_BASE )
	{
#ifdef SAM_FRAMEWORK_ENABLED
		safety.customStableDefinitionAvailable =
			SAMItemRegistryFoundation::isRegisteredRuntimeItemId(runtimeType)
			&& !SAMItemRegistryFoundation::stableIdForRuntimeId(
				runtimeType).empty();
#else
		safety.customStableDefinitionAvailable = false;
#endif
	}
	return safety;
}

Entity* createFakeLootVisual(Entity& owner, const Item& source)
{
	while ( state.fakeLootVisuals.size() >= kMaximumFakeLootVisuals )
	{
		const std::uint32_t oldest = state.fakeLootVisuals.front();
		state.fakeLootVisuals.pop_front();
		if ( Entity* visual = uidToEntity(oldest) )
		{
			visual->skill[2] = 1;
		}
	}
	Entity* visual = createTemporaryVisual(owner, itemModel(&source),
		owner.x + std::cos(owner.yaw) * 12.0,
		owner.y + std::sin(owner.yaw) * 12.0,
		owner.z - 2.0, definition(MIRROR_REFLECT_LOOT)->durationTicks);
	if ( visual )
	{
		visual->fskill[0] = 0.08;
		visual->scalex = visual->scaley = visual->scalez = 0.85;
		state.fakeLootVisuals.push_back(visual->getUID());
	}
	return visual;
}

Entity* createHologram(Entity& caster, Entity& visualTarget,
	spell_t& sourceSpell, const real_t x, const real_t y,
	const int duration, const int disguiseKind)
{
	state.hologramVisuals.erase(std::remove_if(
		state.hologramVisuals.begin(), state.hologramVisuals.end(),
		[](const std::uint32_t uid) { return uidToEntity(uid) == nullptr; }),
		state.hologramVisuals.end());
	while ( state.hologramVisuals.size() >= kMaximumHolograms )
	{
		const std::uint32_t oldest = state.hologramVisuals.front();
		state.hologramVisuals.pop_front();
		if ( Entity* hologram = uidToEntity(oldest) )
		{
			hologram->setHP(0);
		}
	}
	spellElement_t* element = sourceSpell.elements.first
		? static_cast<spellElement_t*>(sourceSpell.elements.first->element)
		: nullptr;
	if ( !element )
	{
		return nullptr;
	}
	Entity* hologram = spellEffectHologram(caster, *element, x, y);
	if ( !hologram )
	{
		return nullptr;
	}
	hologram->monsterSpecialState = visualTarget.getUID();
	hologram->skill[34] = disguiseKind;
	serverUpdateEntitySkill(hologram, 33);
	serverUpdateEntitySkill(hologram, 34);
	hologram->setEffect(EFF_MIST_FORM, true, std::max(1, duration), true);
	state.hologramVisuals.push_back(hologram->getUID());
	if ( disguiseKind != 0 )
	{
		if ( auto previous = state.disguiseProxies.find(caster.getUID());
			previous != state.disguiseProxies.end() )
		{
			if ( Entity* oldProxy = uidToEntity(previous->second) )
			{
				oldProxy->setHP(0);
			}
		}
		state.disguiseProxies[caster.getUID()] = hologram->getUID();
	}
	return hologram;
}

void addFollowerAndPartyAuthorization(Entity& caster,
	NavigationOverlay& overlay)
{
	overlay.authorizedActorUids.push_back(caster.getUID());
	if ( Stat* casterStats = caster.getStats() )
	{
		for ( node_t* node = casterStats->FOLLOWERS.first; node; node = node->next )
		{
			if ( const Uint32* uid = static_cast<const Uint32*>(node->element) )
			{
				overlay.authorizedActorUids.push_back(*uid);
			}
		}
	}

	const int casterPlayer = playerIndexFor(caster);
	const AutomatiaParty::DurablePlayerIdentity* casterIdentity = casterPlayer >= 0
		? worldState.partyManager().onlineIdentityFor(casterPlayer) : nullptr;
	const AutomatiaParty::PartyID partyId = casterIdentity
		? worldState.partyManager().partyIdForPlayer(*casterIdentity)
		: AutomatiaParty::INVALID_PARTY_ID;
	if ( partyId != AutomatiaParty::INVALID_PARTY_ID )
	{
		for ( int player = 0; player < MAXPLAYERS; ++player )
		{
			if ( !players[player] || !players[player]->entity )
			{
				continue;
			}
			const AutomatiaParty::DurablePlayerIdentity* identity =
				worldState.partyManager().onlineIdentityFor(player);
			if ( identity
				&& worldState.partyManager().partyIdForPlayer(*identity) == partyId
				&& players[player]->entity->playableFloor == caster.playableFloor )
			{
				overlay.authorizedActorUids.push_back(
					players[player]->entity->getUID());
			}
		}
	}
	std::sort(overlay.authorizedActorUids.begin(),
		overlay.authorizedActorUids.end());
	overlay.authorizedActorUids.erase(std::unique(
		overlay.authorizedActorUids.begin(),
		overlay.authorizedActorUids.end()),
		overlay.authorizedActorUids.end());
}

bool addOverlay(Entity& caster, const OverlayKind kind,
	std::vector<TileCoord> tiles, const int duration,
	const bool actorAwareSupport)
{
	NavigationOverlay overlay;
	overlay.kind = kind;
	overlay.scope = scopeForFloor(caster.playableFloor);
	overlay.ownerUid = caster.getUID();
	overlay.expiresTick = ticks + std::max(1, duration);
	overlay.tiles = std::move(tiles);
	if ( actorAwareSupport )
	{
		addFollowerAndPartyAuthorization(caster, overlay);
	}
	if ( !state.overlays.add(std::move(overlay)) )
	{
		return false;
	}
	++state.visualRevision;
	return true;
}

bool sendOverlayToPlayer(const NavigationOverlay& overlay, const int player)
{
	if ( multiplayer != SERVER || player <= 0 || player >= MAXPLAYERS
		|| !serverPlayerCanReceiveGameplayUpdates(player)
		|| !serverPlayerCanReceivePlayableFloorUpdates(
			player, overlay.scope.playableFloor) )
	{
		return false;
	}
	const std::size_t tileCount = std::min(overlay.tiles.size(),
		OverlayRegistry::kMaximumTilesPerOverlay);
	const std::size_t actorCount = std::min(overlay.authorizedActorUids.size(),
		OverlayRegistry::kMaximumAuthorizedActors);
	const int length = 21 + static_cast<int>(tileCount * 4 + actorCount * 4);
	if ( length > NET_PACKET_SIZE )
	{
		return false;
	}
	std::memcpy(net_packet->data, "ILOV", 4);
	SDLNet_Write32(static_cast<Uint32>(overlay.id), &net_packet->data[4]);
	net_packet->data[8] = static_cast<Uint8>(overlay.kind);
	SDLNet_Write16(static_cast<Uint16>(overlay.scope.playableFloor),
		&net_packet->data[9]);
	SDLNet_Write32(overlay.ownerUid, &net_packet->data[11]);
	SDLNet_Write32(overlay.expiresTick > ticks
		? overlay.expiresTick - ticks : 1U, &net_packet->data[15]);
	net_packet->data[19] = static_cast<Uint8>(tileCount);
	net_packet->data[20] = static_cast<Uint8>(actorCount);
	int offset = 21;
	for ( std::size_t index = 0; index < tileCount; ++index )
	{
		SDLNet_Write16(static_cast<Uint16>(overlay.tiles[index].x),
			&net_packet->data[offset]);
		SDLNet_Write16(static_cast<Uint16>(overlay.tiles[index].y),
			&net_packet->data[offset + 2]);
		offset += 4;
	}
	for ( std::size_t index = 0; index < actorCount; ++index )
	{
		SDLNet_Write32(overlay.authorizedActorUids[index],
			&net_packet->data[offset]);
		offset += 4;
	}
	net_packet->address.host = net_clients[player - 1].host;
	net_packet->address.port = net_clients[player - 1].port;
	net_packet->len = length;
	return sendPacketSafe(net_sock, -1, net_packet, player - 1) != 0;
}

void synchronizeOverlayRecipients()
{
	if ( multiplayer != SERVER ) { return; }
	const WorldInstanceIdentity* activeIdentity = worldState.activeIdentity();
	const std::string activeKey = activeIdentity ? activeIdentity->key()
		: std::string{};
	for ( int player = 1; player < MAXPLAYERS; ++player )
	{
		if ( !serverPlayerCanReceiveGameplayUpdates(player)
			|| !players[player] || !players[player]->entity )
		{
			state.overlayRevisionSent[player] = 0;
			state.overlayReceiverUid[player] = 0;
			state.overlayReceiverInstance[player].clear();
			continue;
		}
		Entity* receiver = players[player]->entity;
		if ( state.overlayRevisionSent[player] == state.visualRevision
			&& state.overlayReceiverUid[player] == receiver->getUID()
			&& state.overlayReceiverFloor[player] == receiver->playableFloor
			&& state.overlayReceiverInstance[player] == activeKey )
		{
			continue;
		}
		for ( const NavigationOverlay& overlay : state.overlays.entries() )
		{
			if ( overlay.scope.mapInstance == activeKey )
			{
				sendOverlayToPlayer(overlay, player);
			}
		}
		state.overlayRevisionSent[player] = state.visualRevision;
		state.overlayReceiverUid[player] = receiver->getUID();
		state.overlayReceiverFloor[player] = receiver->playableFloor;
		state.overlayReceiverInstance[player] = activeKey;
	}
}

void sendVaultSnapshot(Entity& owner, const int player)
{
	if ( multiplayer != SERVER || player <= 0 || player >= MAXPLAYERS
		|| !serverPlayerCanReceiveGameplayUpdates(player) )
	{
		return;
	}
	const StoredMagicVault* vault = vaultForConst(owner);
	const int proficiency = effectiveProficiency(&owner);
	const std::size_t entryCount = vault
		? std::min(vault->entries().size(), vaultCapacity(proficiency)) : 0;
	std::memcpy(net_packet->data, "ILVA", 4);
	SDLNet_Write32(owner.getUID(), &net_packet->data[4]);
	net_packet->data[8] = static_cast<Uint8>(std::min<std::size_t>(
		255, vaultCapacity(proficiency)));
	net_packet->data[9] = static_cast<Uint8>(std::clamp(proficiency, 0, 100));
	net_packet->data[10] = static_cast<Uint8>(entryCount);
	int offset = 11;
	for ( std::size_t index = 0; index < entryCount; ++index )
	{
		const StoredSpellSnapshot& entry = vault->entries()[index];
		SDLNet_Write16(static_cast<Uint16>(entry.spellId),
			&net_packet->data[offset]);
		SDLNet_Write16(static_cast<Uint16>(std::clamp(
			entry.originalEffectiveMana, 0, 0xffff)),
			&net_packet->data[offset + 2]);
		SDLNet_Write16(static_cast<Uint16>(std::clamp(
			entry.originalPower, 0, 0xffff)),
			&net_packet->data[offset + 4]);
		SDLNet_Write16(static_cast<Uint16>(std::clamp(
			entry.originalCasterProficiency, 0, 100)),
			&net_packet->data[offset + 6]);
		SDLNet_Write32(entry.studyTicks, &net_packet->data[offset + 8]);
		net_packet->data[offset + 12] = entry.playerLearnable ? 1 : 0;
		offset += 13;
	}
	net_packet->address.host = net_clients[player - 1].host;
	net_packet->address.port = net_clients[player - 1].port;
	net_packet->len = offset;
	sendPacketSafe(net_sock, -1, net_packet, player - 1);
}

void synchronizeVaultRecipients()
{
	if ( multiplayer != SERVER || ticks % TICKS_PER_SECOND != 0 )
	{
		return;
	}
	for ( int player = 1; player < MAXPLAYERS; ++player )
	{
		if ( !players[player] || !players[player]->entity
			|| !stats[player]
			|| !stats[player]->getEffectActive(EFF_STORE_MAGIC) )
		{
			continue;
		}
		sendVaultSnapshot(*players[player]->entity, player);
	}
}

int nativeSpellPower(Entity& owner, const int spellId)
{
	return std::max(2,
		getSpellDamageFromID(spellId, &owner, owner.getStats(), &owner));
}

int casterProficiencyForSpell(Entity& caster, const spell_t& spell)
{
	Stat* stats = caster.getStats();
	if ( !stats ) { return 0; }
	if ( isSpell(spell.ID) )
	{
		return effectiveProficiency(&caster);
	}
	if ( spell.skillID < 0 || spell.skillID >= NUMPROFICIENCIES )
	{
		return 0;
	}
	return std::clamp(stats->getModifiedProficiency(spell.skillID), 0, 100);
}

StoredMagicVault* vaultFor(const Entity& owner)
{
	auto found = state.vaults.find(owner.getUID());
	return found == state.vaults.end() ? nullptr : &found->second;
}

const StoredMagicVault* vaultForConst(const Entity& owner)
{
	auto found = state.vaults.find(owner.getUID());
	return found == state.vaults.end() ? nullptr : &found->second;
}

const char* safeSpellName(const int spellId)
{
	if ( spell_t* spell = getSpellFromID(spellId) )
	{
		return spell->getSpellName();
	}
	return "Unknown Spell";
}

void teachStudiedSpell(Entity& owner, const int spellId)
{
	const int player = playerIndexFor(owner);
	if ( player < 0 )
	{
		return;
	}
	if ( multiplayer == SERVER && player > 0
		&& !players[player]->isLocalPlayer() )
	{
		std::memcpy(net_packet->data, "ASPL", 4);
		net_packet->data[4] = static_cast<Uint8>(player);
		net_packet->data[5] = static_cast<Uint8>(spellId);
		net_packet->address.host = net_clients[player - 1].host;
		net_packet->address.port = net_clients[player - 1].port;
		net_packet->len = 6;
		sendPacketSafe(net_sock, -1, net_packet, player - 1);
	}
	else
	{
		addSpell(spellId, player, true);
	}
	messagePlayer(player, MESSAGE_PROGRESSION,
		"You have learned %s by studying its stored pattern.",
		safeSpellName(spellId));
}

bool beginSustainedStoreMagic(Entity& caster, spell_t& spell,
	spell_t*& channeledSpell)
{
	Stat* stats = caster.getStats();
	if ( !stats
		|| (state.vaults.find(caster.getUID()) == state.vaults.end()
			&& state.vaults.size() >= kMaximumVaultOwners) )
	{
		return false;
	}
	node_t* spellNode = list_AddNodeLast(&stats->magic_effects);
	spellNode->element = copySpell(&spell);
	channeledSpell = static_cast<spell_t*>(spellNode->element);
	if ( !channeledSpell )
	{
		list_RemoveNode(spellNode);
		return false;
	}
	channeledSpell->magic_effects_node = spellNode;
	channeledSpell->caster = caster.getUID();
	channeledSpell->channel_duration = definition(STORE_MAGIC)->durationTicks;
	channeledSpell->sustainEffectDissipate = EFF_STORE_MAGIC;
	spellNode->size = sizeof(spell_t);
	spellNode->deconstructor = &spellDeconstructor;
	caster.setEffect(EFF_STORE_MAGIC, true,
		channeledSpell->channel_duration, true);
	state.vaults.try_emplace(caster.getUID());
	return true;
}

void modifyReleasedSpellPower(spell_t& spell, const int power)
{
	if ( !spell.elements.first )
	{
		return;
	}
	spellElement_t* root = static_cast<spellElement_t*>(
		spell.elements.first->element);
	spellElement_t* payload = root && root->elements.first
		? static_cast<spellElement_t*>(root->elements.first->element) : root;
	if ( payload && payload->getDamage() > 0 )
	{
		payload->setDamage(std::max(1, power));
	}
}

class StoredReleaseGuard
{
public:
	StoredReleaseGuard() { releasingStoredSpell = true; }
	~StoredReleaseGuard() { releasingStoredSpell = false; }
};
} // namespace

bool enabled()
{
	return automatianModeEnabled();
}

bool contentAvailable(const int spellId)
{
	return !isSpell(spellId) || enabled();
}

int effectiveProficiency(const Entity* caster)
{
	Stat* stats = caster ? const_cast<Entity*>(caster)->getStats() : nullptr;
	return stats ? IllusionMagic::effectiveProficiency(
		stats->getModifiedProficiency(PRO_SORCERY),
		stats->getModifiedProficiency(PRO_MYSTICISM),
		stats->getModifiedProficiency(PRO_THAUMATURGY)) : 0;
}

int proficiencyForSpell(const Entity* caster, Stat* stats,
	const int spellId, const int fallbackSkillId)
{
	if ( !stats ) { return 0; }
	if ( isSpell(spellId) )
	{
		return IllusionMagic::effectiveProficiency(
			stats->getModifiedProficiency(PRO_SORCERY),
			stats->getModifiedProficiency(PRO_MYSTICISM),
			stats->getModifiedProficiency(PRO_THAUMATURGY));
	}
	if ( fallbackSkillId < 0 || fallbackSkillId >= NUMPROFICIENCIES )
	{
		return 0;
	}
	(void)caster;
	return std::clamp(stats->getModifiedProficiency(fallbackSkillId), 0, 100);
}

PhantasmPathPreview previewPhantasmPath(const Entity& caster,
	const int targetTileX, const int targetTileY)
{
	PhantasmPathPreview preview;
	const int startX = static_cast<int>(std::floor(caster.x / 16.0));
	const int startY = static_cast<int>(std::floor(caster.y / 16.0));
	const int deltaX = targetTileX - startX;
	const int deltaY = targetTileY - startY;
	const int steps = std::min<int>(
		PhantasmPathPreview::kMaximumTiles,
		std::max(std::abs(deltaX), std::abs(deltaY)));
	bool needsIllusionSupport = false;
	bool routeIsValid = steps > 0;

	for ( int step = 1; step <= steps; ++step )
	{
		PhantasmPathPreview::Tile& tile = preview.tiles[preview.count++];
		tile.coordinate.x = startX + static_cast<int>(std::lround(
			static_cast<double>(deltaX) * step / std::max(1, steps)));
		tile.coordinate.y = startY + static_cast<int>(std::lround(
			static_cast<double>(deltaY) * step / std::max(1, steps)));

		if ( !tileInBounds(tile.coordinate.x, tile.coordinate.y)
			|| map.tileAt(tile.coordinate.x, tile.coordinate.y,
				OBSTACLELAYER, caster.playableFloor) != 0 )
		{
			tile.state = PhantasmPathPreviewTileState::Invalid;
			routeIsValid = false;
			continue;
		}

		const Sint32 floorTile = map.tileAt(tile.coordinate.x,
			tile.coordinate.y, FLOORLAYER, caster.playableFloor);
		if ( floorTile == 0 || swimmingtiles[floorTile]
			|| lavatiles[floorTile] )
		{
			tile.state =
				PhantasmPathPreviewTileState::NeedsIllusionSupport;
			needsIllusionSupport = true;
		}
		else
		{
			tile.state = PhantasmPathPreviewTileState::ExistingGround;
		}
	}

	preview.castable = routeIsValid && needsIllusionSupport;
	return preview;
}

Entity* cast(Entity& caster, spell_t& spell, CastSpellProps_t* properties,
	spell_t*& channeledSpell)
{
	if ( !enabled() || multiplayer == CLIENT || !isSpell(spell.ID) )
	{
		return nullptr;
	}
	const SpellDefinition* spellDefinition = definition(spell.ID);
	if ( !spellDefinition )
	{
		return nullptr;
	}

	Entity* result = nullptr;
	Entity* target = nullptr;
	int tileX = 0;
	int tileY = 0;
	switch ( spell.ID )
	{
		case MIRROR_OTHER:
			target = targetFromProperties(caster, properties, *spellDefinition);
			if ( !target )
			{
				failureFeedback(caster, "Mirror Other needs a nearby actor.");
				break;
			}
			caster.setEffect(EFF_MIRROR_OTHER, true,
				spellDefinition->durationTicks, true);
			caster.setEffect(EFF_MIRROR_MIMIC, false, 0, true);
			result = createHologram(caster, *target, spell, caster.x, caster.y,
				spellDefinition->durationTicks, kDisguiseProxyOther);
			effectFeedback(caster, "Mirror Other reshapes your visible reflection.");
			break;

		case MIRROR_COPY:
			target = targetFromProperties(caster, properties, *spellDefinition);
			if ( !target )
			{
				failureFeedback(caster, "Mirror Copy needs a nearby creature.");
				break;
			}
			result = createHologram(caster, *target, spell,
				target->x, target->y, spellDefinition->durationTicks, 0);
			if ( result )
			{
				effectFeedback(caster, "A harmless shadow-copy takes shape.");
			}
			break;

		case MIRROR_WALL:
			if ( !targetTileFromProperties(caster, properties,
					*spellDefinition, tileX, tileY)
				|| map.tileAt(tileX, tileY, FLOORLAYER,
					caster.playableFloor) == 0
				|| map.tileAt(tileX, tileY, OBSTACLELAYER,
					caster.playableFloor) != 0
				|| !addOverlay(caster, OverlayKind::FakeWall,
					{{tileX, tileY}}, spellDefinition->durationTicks, false) )
			{
				failureFeedback(caster, "Mirror Wall needs open supported ground.");
				break;
			}
			result = createTemporaryVisual(caster, kFakeWallModel,
				tileX * 16.0 + 8.0, tileY * 16.0 + 8.0, 0.0,
				spellDefinition->durationTicks);
			effectFeedback(caster, "A passable false wall appears.");
			break;

		case MIRROR_REFLECT:
			caster.setEffect(EFF_MIRROR_REFLECT, true,
				spellDefinition->durationTicks, true);
			effectFeedback(caster, "Mirror Reflect surrounds you with a distinct ward.");
			break;

		case MIRROR_MIMIC:
			target = targetFromProperties(caster, properties, *spellDefinition);
			if ( !target || target->isBossMonster() )
			{
				failureFeedback(caster, "That creature resists Mirror Mimic.");
				break;
			}
			caster.setEffect(EFF_MIRROR_MIMIC, true,
				spellDefinition->durationTicks, true);
			caster.setEffect(EFF_MIRROR_OTHER, false, 0, true);
			result = createHologram(caster, *target, spell, caster.x, caster.y,
				spellDefinition->durationTicks, kDisguiseProxyMimic);
			effectFeedback(caster, "Mirror Mimic distorts hostile perception.");
			break;

		case MIRROR_REFLECT_LOOT:
			openLootSelection(caster, spell.ID, false);
			effectFeedback(caster,
				"Choose an owned item to create FAKE / ILLUSORY loot.");
			break;

		case PHANTASM_PATH:
		{
			if ( !targetTileFromProperties(caster, properties,
					*spellDefinition, tileX, tileY) )
			{
				failureFeedback(caster, "Phantasm Path needs a nearby gap or liquid.");
				break;
			}
			const PhantasmPathPreview preview =
				previewPhantasmPath(caster, tileX, tileY);
			std::vector<TileCoord> bridge;
			bridge.reserve(preview.count);
			for ( std::size_t index = 0; index < preview.count; ++index )
			{
				if ( preview.tiles[index].state ==
					PhantasmPathPreviewTileState::NeedsIllusionSupport )
				{
					bridge.push_back(preview.tiles[index].coordinate);
				}
			}
			if ( !preview.castable || bridge.empty() || !addOverlay(caster,
					OverlayKind::PhantasmPath, bridge,
					spellDefinition->durationTicks, true) )
			{
				failureFeedback(caster,
					"No continuous unsupported path is valid in range.");
				break;
			}
			for ( const TileCoord& tile : bridge )
			{
				createTemporaryVisual(caster, 983,
					tile.x * 16.0 + 8.0, tile.y * 16.0 + 8.0, 7.5,
					spellDefinition->durationTicks);
			}
			effectFeedback(caster, "An actor-aware phantasmal path appears.");
			break;
		}

		case MIRROR_DUPLICATE_LOOT:
			openLootSelection(caster, spell.ID, true);
			effectFeedback(caster,
				"Choose an eligible owned item for one REAL DUPLICATE.");
			break;

		case MIRAGE_WALL:
			if ( !targetTileFromProperties(caster, properties,
					*spellDefinition, tileX, tileY)
				|| map.tileAt(tileX, tileY, OBSTACLELAYER,
					caster.playableFloor) == 0
				|| !addOverlay(caster, OverlayKind::HiddenWall,
					{{tileX, tileY}}, spellDefinition->durationTicks, false) )
			{
				failureFeedback(caster, "Mirage Wall must target a real wall.");
				break;
			}
			effectFeedback(caster, "The real wall is visually suppressed; collision remains.");
			break;

		case PARANOIA:
		{
			int affected = 0;
			for ( node_t* node = map.creatures->first; node; node = node->next )
			{
				Entity* creature = static_cast<Entity*>(node->element);
				if ( !creature || creature == &caster
					|| creature->behavior != &actMonster
					|| creature->playableFloor != caster.playableFloor
					|| creature->isBossMonster()
					|| std::hypot(creature->x - caster.x,
						creature->y - caster.y) > spellDefinition->radiusWorldUnits
					|| !caster.checkEnemy(creature) )
				{
					continue;
				}
				if ( creature->setEffect(EFF_CONFUSED,
						static_cast<Uint8>(MAXPLAYERS + 1),
						spellDefinition->durationTicks, true, true, true, true) )
				{
					++affected;
				}
			}
			if ( affected )
			{
				effectFeedback(caster, "Paranoia turns enemy perception against itself.");
			}
			else
			{
				failureFeedback(caster, "No eligible enemies succumb to Paranoia.");
			}
			break;
		}

		case VERTICAL_MIRAGE:
			if ( !targetTileFromProperties(caster, properties,
					*spellDefinition, tileX, tileY)
				|| map.tileAt(tileX, tileY, FLOORLAYER,
					caster.playableFloor) == 0
				|| !addOverlay(caster, OverlayKind::FakePit,
					{{tileX, tileY}}, spellDefinition->durationTicks, false) )
			{
				failureFeedback(caster, "Vertical Mirage needs a real floor tile.");
				break;
			}
			effectFeedback(caster, "A false drop masks the real floor.");
			break;

		case SHADOW_STEP:
		{
			const real_t oldX = caster.x;
			const real_t oldY = caster.y;
			const int destinationX = static_cast<int>(std::floor(
				(caster.x - std::cos(caster.yaw) * 64.0) / 16.0));
			const int destinationY = static_cast<int>(std::floor(
				(caster.y - std::sin(caster.yaw) * 64.0) / 16.0));
			if ( !tileInBounds(destinationX, destinationY)
				|| map.tileAt(destinationX, destinationY, FLOORLAYER,
					caster.playableFloor) == 0
				|| map.tileAt(destinationX, destinationY, OBSTACLELAYER,
					caster.playableFloor) != 0 )
			{
				failureFeedback(caster, "Shadow Step's exact four-tile destination is blocked.");
				break;
			}
			Entity* clone = createHologram(caster, caster, spell,
				oldX, oldY, spellDefinition->durationTicks, 0);
			if ( !caster.teleport(destinationX, destinationY) )
			{
				if ( clone ) { clone->setHP(0); }
				failureFeedback(caster, "Shadow Step could not reach its exact destination.");
				break;
			}
			result = clone;
			effectFeedback(caster, "You step backward through your lingering shadow.");
			break;
		}

		case STORE_MAGIC:
			if ( beginSustainedStoreMagic(caster, spell, channeledSpell) )
			{
				effectFeedback(caster, "Store Magic is sustained; its bounded vault is ready.");
			}
			else
			{
				failureFeedback(caster, "Store Magic could not establish its vault.");
			}
			break;
		default:
			break;
	}
	return result;
}

void tick()
{
	if ( !enabled() )
	{
		if ( state.featureWasEnabled ) { clearRuntimeState(); }
		return;
	}
	state.featureWasEnabled = true;
	const WorldInstanceIdentity* activeIdentity = worldState.activeIdentity();
	const std::string activeInstance = activeIdentity
		? activeIdentity->key() : std::string{};
	if ( state.activeInstanceKey != activeInstance )
	{
		// Entity UIDs are runtime identities and may be reused after a map
		// activation. Drop every pointer-free UID association before it could
		// bind to an unrelated actor in the next MapInstance.
		resetRuntimeCollections(false);
		state.activeInstanceKey = activeInstance;
	}
	const std::size_t oldOverlayCount = state.overlays.size();
	state.overlays.expire(ticks);
	if ( state.overlays.size() != oldOverlayCount ) { ++state.visualRevision; }
	if ( multiplayer == CLIENT ) { return; }
	synchronizeOverlayRecipients();
	synchronizeVaultRecipients();

	for ( auto it = state.disguiseProxies.begin();
		it != state.disguiseProxies.end(); )
	{
		Entity* caster = uidToEntity(it->first);
		Entity* proxy = uidToEntity(it->second);
		Stat* casterStats = caster ? caster->getStats() : nullptr;
		const bool active = casterStats
			&& (casterStats->getEffectActive(EFF_MIRROR_OTHER)
				|| casterStats->getEffectActive(EFF_MIRROR_MIMIC));
		if ( !caster || !proxy || !active
			|| caster->playableFloor != proxy->playableFloor )
		{
			if ( proxy ) { proxy->setHP(0); }
			it = state.disguiseProxies.erase(it);
			continue;
		}
		proxy->x = caster->x;
		proxy->y = caster->y;
		proxy->z = caster->z;
		proxy->yaw = caster->yaw;
		proxy->inheritSpatialContextFrom(caster);
		TileEntityList.updateEntity(*proxy);
		++it;
	}

	for ( auto it = state.pendingLootSelections.begin();
		it != state.pendingLootSelections.end(); )
	{
		if ( !uidToEntity(it->first) || it->second.expiresTick <= ticks )
		{
			it = state.pendingLootSelections.erase(it);
		}
		else
		{
			++it;
		}
	}
	state.fakeLootVisuals.erase(std::remove_if(
		state.fakeLootVisuals.begin(), state.fakeLootVisuals.end(),
		[](const std::uint32_t uid) { return uidToEntity(uid) == nullptr; }),
		state.fakeLootVisuals.end());
	state.hologramVisuals.erase(std::remove_if(
		state.hologramVisuals.begin(), state.hologramVisuals.end(),
		[](const std::uint32_t uid) { return uidToEntity(uid) == nullptr; }),
		state.hologramVisuals.end());

	for ( auto it = state.vaults.begin(); it != state.vaults.end(); )
	{
		Entity* owner = uidToEntity(it->first);
		if ( !owner )
		{
			it = state.vaults.erase(it);
			continue;
		}
		Stat* stats = owner->getStats();
		const int proficiency = effectiveProficiency(owner);
		if ( stats && stats->getEffectActive(EFF_STORE_MAGIC) )
		{
			it->second.accrueStudy(1, proficiency);
			for ( std::size_t index = 0;
				index < it->second.entries().size(); ++index )
			{
				if ( it->second.readyToLearn(index, proficiency) )
				{
					const int learnedId = it->second.entries()[index].spellId;
					teachStudiedSpell(*owner, learnedId);
					it->second.discard(index);
					break;
				}
			}
		}
		++it;
	}
}

void clearRuntimeState()
{
	resetRuntimeCollections(true);
	state.activeInstanceKey.clear();
	state.featureWasEnabled = false;
}

bool tryStoreIncoming(Entity& target, Entity& projectile,
	const spell_t& incomingSpell, Entity* originalCaster,
	const int capturedPower)
{
	const bool learnable =
		getSpellbookFromSpellID(incomingSpell.ID) != WOODEN_SHIELD;
	Stat* targetStats = target.getStats();
	if ( !enabled() || multiplayer == CLIENT || !originalCaster
		|| target.playableFloor != projectile.playableFloor
		|| target.playableFloor != originalCaster->playableFloor
		|| !targetStats || !targetStats->getEffectActive(EFF_STORE_MAGIC)
		|| !target.checkEnemy(originalCaster)
		|| !storeMagicEligible(incomingSpell.ID, true, learnable) )
	{
		return false;
	}
	StoredSpellSnapshot snapshot;
	snapshot.spellId = incomingSpell.ID;
	snapshot.originalEffectiveMana = std::max(0,
		getCostOfSpell(const_cast<spell_t*>(&incomingSpell), originalCaster));
	snapshot.originalPower = std::max(0, capturedPower);
	snapshot.originalCasterProficiency = casterProficiencyForSpell(
		*originalCaster, incomingSpell);
	snapshot.capturedTick = ticks;
	snapshot.playerLearnable = learnable;
	auto vaultFound = state.vaults.find(target.getUID());
	if ( vaultFound == state.vaults.end()
		&& state.vaults.size() >= kMaximumVaultOwners )
	{
		return false;
	}
	StoredMagicVault& vault = state.vaults.try_emplace(
		target.getUID()).first->second;
	if ( !vault.capture(snapshot, effectiveProficiency(&target)) )
	{
		if ( const int player = playerIndexFor(target); player >= 0 )
		{
			messagePlayer(player, MESSAGE_HINT, "Stored Magic Vault is full.");
		}
		return false;
	}
	if ( const int player = playerIndexFor(target); player >= 0 )
	{
		messagePlayer(player, MESSAGE_STATUS, "Stored: %s",
			safeSpellName(incomingSpell.ID));
	}
	playSoundEntity(&target, 166, 128);
	spawnMagicEffectParticles(target.x, target.y, target.z, 174);
	return true;
}

bool shouldMirrorReflect(const Entity& target, const Entity& projectile,
	const spell_t& incomingSpell, const Entity* originalCaster)
{
	Stat* stats = const_cast<Entity&>(target).getStats();
	if ( !enabled() || multiplayer == CLIENT || !originalCaster
		|| projectile.actmagicReflectionCount != 0
		|| projectile.actmagicMirrorReflected != 0
		|| target.playableFloor != projectile.playableFloor
		|| target.playableFloor != originalCaster->playableFloor
		|| !stats || !stats->getEffectActive(EFF_MIRROR_REFLECT)
		|| !const_cast<Entity&>(target).checkEnemy(
			const_cast<Entity*>(originalCaster)) )
	{
		return false;
	}
	switch ( incomingSpell.ID )
	{
		case SPELL_REFLECT_MAGIC:
		case SPELL_ABSORB_MAGIC:
		case SPELL_SEIZE_MAGIC:
		case SPELL_MIRROR_REFLECT:
		case SPELL_STORE_MAGIC:
			return false;
		default:
			return true;
	}
}

int mirrorReflectBonusDamage(const spell_t& incomingSpell)
{
	const spellElement_t* root = incomingSpell.elements.first
		? static_cast<const spellElement_t*>(incomingSpell.elements.first->element)
		: nullptr;
	const spellElement_t* payload = root && root->elements.first
		? static_cast<const spellElement_t*>(root->elements.first->element) : root;
	return std::max(1, payload
		? const_cast<spellElement_t*>(payload)->getDamage() / 5 : 1);
}

bool applyNavigationOverlays(int* pathMap, const int width, const int height,
	const Entity& actor, const int playableFloor)
{
	if ( !enabled() || !pathMap || width <= 0 || height <= 0
		|| actor.playableFloor != playableFloor )
	{
		return false;
	}
	const SpatialScope scope = scopeForFloor(playableFloor);
	bool supportChangedConnectivity = false;
	const int actorX = std::clamp(
		static_cast<int>(std::floor(actor.x / 16.0)), 0, width - 1);
	const int actorY = std::clamp(
		static_cast<int>(std::floor(actor.y / 16.0)), 0, height - 1);
	const int actorCell = pathMap[actorY + actorX * height];
	const int actorZone = actorCell ? actorCell : 1;
	for ( const NavigationOverlay& overlay : state.overlays.entries() )
	{
		if ( !(overlay.scope == scope) ) { continue; }
		bool hostile = false;
		if ( Entity* owner = uidToEntity(static_cast<Sint32>(overlay.ownerUid)) )
		{
			hostile = const_cast<Entity&>(actor).checkEnemy(owner);
		}
		for ( const TileCoord& tile : overlay.tiles )
		{
			if ( tile.x < 0 || tile.y < 0
				|| tile.x >= width || tile.y >= height ) { continue; }
			int& cell = pathMap[tile.y + tile.x * height];
			if ( hostile && (overlay.kind == OverlayKind::FakeWall
				|| overlay.kind == OverlayKind::FakePit) )
			{
				cell = 0;
			}
			else if ( overlay.kind == OverlayKind::PhantasmPath
				&& (actor.getUID() == overlay.ownerUid
					|| std::find(overlay.authorizedActorUids.begin(),
						overlay.authorizedActorUids.end(), actor.getUID())
						!= overlay.authorizedActorUids.end()) && cell == 0 )
			{
				cell = actorZone;
				supportChangedConnectivity = true;
			}
		}
	}
	return supportChangedConnectivity;
}

bool supportsActorAt(const Entity& actor, const int tileX, const int tileY)
{
	return enabled() && tileInBounds(tileX, tileY)
		&& state.overlays.supportsActor(scopeForFloor(actor.playableFloor),
			tileX, tileY, actor.getUID());
}

bool hidesWallAt(const int playableFloor, const int tileX, const int tileY)
{
	return enabled() && state.overlays.hidesWall(
		scopeForFloor(playableFloor), tileX, tileY);
}

bool hidesFloorAt(const int playableFloor, const int tileX, const int tileY)
{
	return enabled() && state.overlays.hidesFloor(
		scopeForFloor(playableFloor), tileX, tileY);
}

std::uint64_t visualRevision() { return state.visualRevision; }

bool receiveOverlaySnapshot(const std::uint64_t id, const OverlayKind kind,
	const int playableFloor, const std::uint32_t ownerUid,
	const std::uint32_t remainingTicks,
	const std::vector<std::uint32_t>& authorizedActorUids,
	const std::vector<TileCoord>& tiles)
{
	if ( multiplayer != CLIENT || !enabled() || id == 0
		|| static_cast<unsigned>(kind)
			> static_cast<unsigned>(OverlayKind::PhantasmPath)
		|| tiles.empty()
		|| tiles.size() > OverlayRegistry::kMaximumTilesPerOverlay
		|| authorizedActorUids.size()
			> OverlayRegistry::kMaximumAuthorizedActors )
	{
		return false;
	}
	NavigationOverlay overlay;
	overlay.id = id;
	overlay.kind = kind;
	overlay.scope = scopeForFloor(playableFloor);
	overlay.ownerUid = ownerUid;
	overlay.expiresTick = ticks + std::max(1U, remainingTicks);
	overlay.authorizedActorUids = authorizedActorUids;
	overlay.tiles = tiles;
	if ( !state.overlays.add(std::move(overlay)) )
	{
		return false;
	}
	++state.visualRevision;
	return true;
}

bool treatsTargetAsNeutral(const Entity& observer, const Entity& target)
{
	if ( !enabled() || observer.behavior != &actMonster
		|| target.playableFloor != observer.playableFloor
		|| const_cast<Entity&>(observer).isBossMonster() )
	{
		return false;
	}
	Stat* stats = const_cast<Entity&>(target).getStats();
	return stats && stats->getEffectActive(EFF_MIRROR_MIMIC);
}

bool shouldHideDisguisedBody(const Entity& entity)
{
	if ( !enabled() )
	{
		return false;
	}

	const Entity* actor = nullptr;
	if ( entity.behavior == &actPlayer || entity.behavior == &actMonster )
	{
		actor = &entity;
	}
	else if ( entity.parent != 0 )
	{
		Entity* parent = uidToEntity(entity.parent);
		if ( parent && (parent->behavior == &actPlayer
			|| parent->behavior == &actMonster) )
		{
			actor = parent;
		}
	}
	if ( !actor )
	{
		return false;
	}

	for ( node_t* node = map.creatures->first; node; node = node->next )
	{
		Entity* proxy = static_cast<Entity*>(node->element);
		if ( !proxy || proxy == actor || proxy->parent != actor->getUID()
			|| proxy->skill[34] == 0
			|| proxy->playableFloor != actor->playableFloor )
		{
			continue;
		}
		Stat* proxyStats = proxy->getStats();
		if ( proxyStats && proxyStats->type == HOLOGRAM )
		{
			return true;
		}
	}
	return false;
}

std::string disguiseAppearanceName(const Entity& entity)
{
	if ( !enabled() ) { return {}; }
	for ( node_t* node = map.creatures->first; node; node = node->next )
	{
		Entity* proxy = static_cast<Entity*>(node->element);
		if ( !proxy || proxy->parent != entity.getUID()
			|| proxy->skill[34] == 0
			|| proxy->playableFloor != entity.playableFloor )
		{
			continue;
		}
		Entity* appearance = uidToEntity(proxy->monsterSpecialState);
		Stat* appearanceStats = appearance ? appearance->getStats() : nullptr;
		if ( !appearance || !appearanceStats ) { return "Unknown appearance"; }
		if ( appearance->behavior == &actPlayer
			&& appearanceStats->name[0] != '\0' )
		{
			return appearanceStats->name;
		}
		return getMonsterLocalizedName(appearanceStats->type);
	}
	return {};
}

bool canCreateRealDuplicate(const Item& source)
{
	const int runtimeType = static_cast<int>(source.type);
	return enabled() && validRuntimeItemType(runtimeType)
		&& itemCategory(const_cast<Item*>(&source)) != SPELL_CAT
		&& mayCreateRealDuplicate(duplicationSafety(source));
}

bool selectLootItem(const int player, const Item& source,
	const bool realDuplicate)
{
	if ( !enabled() || player < 0 || player >= MAXPLAYERS
		|| !players[player] || !players[player]->entity )
	{
		return false;
	}
	if ( multiplayer != CLIENT )
	{
		return resolveLootSelection(*players[player]->entity, realDuplicate,
			static_cast<int>(source.type), static_cast<int>(source.status),
			source.beatitude, source.count, source.appearance,
			source.identified);
	}
	std::memcpy(net_packet->data, "ILIT", 4);
	net_packet->data[4] = realDuplicate ? 1 : 0;
	SDLNet_Write32(static_cast<Uint32>(source.type), &net_packet->data[5]);
	SDLNet_Write32(static_cast<Uint32>(source.status), &net_packet->data[9]);
	SDLNet_Write32(static_cast<Uint32>(source.beatitude), &net_packet->data[13]);
	SDLNet_Write32(static_cast<Uint32>(source.count), &net_packet->data[17]);
	SDLNet_Write32(source.appearance, &net_packet->data[21]);
	net_packet->data[25] = source.identified ? 1 : 0;
	net_packet->data[26] = static_cast<Uint8>(player);
	net_packet->len = 27;
#ifdef SAM_FRAMEWORK_ENABLED
	if ( !appendStableItemId(static_cast<int>(source.type), 27) )
	{
		return false;
	}
#endif
	net_packet->address.host = net_server.host;
	net_packet->address.port = net_server.port;
	return sendPacketSafe(net_sock, -1, net_packet, 0) != 0;
}

bool resolveLootSelection(Entity& owner, const bool realDuplicate,
	const int runtimeType, const int status, const int beatitude,
	const int count, const std::uint32_t appearance, const bool identified)
{
	if ( !enabled() || multiplayer == CLIENT || count < 1
		|| !validRuntimeItemType(runtimeType) )
	{
		return false;
	}
	auto pending = state.pendingLootSelections.find(owner.getUID());
	if ( pending == state.pendingLootSelections.end()
		|| pending->second.expiresTick <= ticks
		|| pending->second.realDuplicate != realDuplicate )
	{
		return false;
	}
	Stat* ownerStats = owner.getStats();
	if ( !ownerStats ) { return false; }
	Item* source = nullptr;
	for ( node_t* node = ownerStats->inventory.first; node; node = node->next )
	{
		Item* candidate = static_cast<Item*>(node->element);
		if ( candidate && static_cast<int>(candidate->type) == runtimeType
			&& static_cast<int>(candidate->status) == status
			&& candidate->beatitude == beatitude
			&& candidate->count >= 1
			&& candidate->appearance == appearance
			&& candidate->identified == identified )
		{
			source = candidate;
			break;
		}
	}
	if ( !source || itemCategory(source) == SPELL_CAT )
	{
		failureFeedback(owner, "That item is not in your inventory.");
		return false;
	}
	if ( realDuplicate && !canCreateRealDuplicate(*source) )
	{
		failureFeedback(owner,
			"That quest, key, artifact, container, or progression item cannot be duplicated.");
		return false;
	}

	bool success = false;
	if ( realDuplicate )
	{
		const int ownerPlayer = playerIndexFor(owner);
		if ( ownerPlayer < 0 )
		{
			return false;
		}
		Item* duplicate = newItem(source->type, source->status,
			mirroredDuplicateBeatitude(source->beatitude), 1,
			source->appearance, source->identified, nullptr);
		if ( duplicate )
		{
			success = itemPickup(ownerPlayer, duplicate) != nullptr;
		}
		if ( success )
		{
			effectFeedback(owner,
				"A REAL mirrored duplicate enters your inventory (quantity 1).");
		}
	}
	else
	{
		success = createFakeLootVisual(owner, *source) != nullptr;
		if ( success )
		{
			effectFeedback(owner,
				"FAKE / ILLUSORY loot appears; it has no item identity or value.");
		}
	}
	if ( success )
	{
		state.pendingLootSelections.erase(pending);
	}
	return success;
}

std::size_t vaultSize(const Entity& owner)
{
	if ( multiplayer == CLIENT )
	{
		auto found = state.clientVaults.find(owner.getUID());
		return found == state.clientVaults.end()
			? 0 : found->second.entries.size();
	}
	const StoredMagicVault* vault = vaultForConst(owner);
	return vault ? vault->size() : 0;
}

std::size_t vaultLimit(const Entity& owner)
{
	if ( multiplayer == CLIENT )
	{
		auto found = state.clientVaults.find(owner.getUID());
		if ( found != state.clientVaults.end() )
		{
			return found->second.capacity;
		}
	}
	return vaultCapacity(effectiveProficiency(&owner));
}

std::vector<VaultEntryView> vaultEntries(const Entity& owner)
{
	std::vector<VaultEntryView> result;
	if ( multiplayer == CLIENT )
	{
		auto found = state.clientVaults.find(owner.getUID());
		if ( found == state.clientVaults.end() ) { return result; }
		for ( const StoredSpellSnapshot& snapshot : found->second.entries )
		{
			VaultEntryView view;
			view.spellId = snapshot.spellId;
			view.spellName = safeSpellName(snapshot.spellId);
			view.releaseMana = storedReleaseMana(
				snapshot.originalEffectiveMana,
				snapshot.originalCasterProficiency,
				found->second.proficiency);
			view.capturedPower = snapshot.originalPower;
			view.studyTicks = snapshot.studyTicks;
			view.readyToLearn = snapshot.playerLearnable
				&& vaultAllowsStudy(found->second.proficiency)
				&& snapshot.studyTicks >= 5U * 60U * TICKS_PER_SECOND;
			result.push_back(std::move(view));
		}
		return result;
	}
	const StoredMagicVault* vault = vaultForConst(owner);
	if ( !vault ) { return result; }
	const int proficiency = effectiveProficiency(&owner);
	for ( std::size_t index = 0; index < vault->entries().size(); ++index )
	{
		const StoredSpellSnapshot& snapshot = vault->entries()[index];
		VaultEntryView view;
		view.spellId = snapshot.spellId;
		view.spellName = safeSpellName(snapshot.spellId);
		view.releaseMana = storedReleaseMana(snapshot.originalEffectiveMana,
			snapshot.originalCasterProficiency, proficiency);
		view.capturedPower = snapshot.originalPower;
		view.studyTicks = snapshot.studyTicks;
		view.readyToLearn = vault->readyToLearn(index, proficiency);
		result.push_back(std::move(view));
	}
	return result;
}

bool releaseStoredMagic(Entity& owner, const VaultSelection selection,
	const std::size_t selectedIndex)
{
	StoredMagicVault* vault = vaultFor(owner);
	Stat* ownerStats = owner.getStats();
	if ( !enabled() || multiplayer == CLIENT || !vault || vault->empty()
		|| !ownerStats || !ownerStats->getEffectActive(EFF_STORE_MAGIC) )
	{
		return false;
	}
	const int proficiency = effectiveProficiency(&owner);
	const std::uint32_t randomSeed = ticks ^ owner.getUID();
	StoredMagicVault previewVault = *vault;
	StoredRelease preview;
	if ( !previewVault.release(selection, selectedIndex, randomSeed,
			proficiency, std::numeric_limits<int>::max(), preview)
		|| owner.getMP() < preview.releaseMana )
	{
		failureFeedback(owner, "Not enough mana to release stored magic.");
		return false;
	}
	spell_t* source = getSpellFromID(preview.snapshot.spellId);
	spell_t* released = source ? copySpell(source) : nullptr;
	if ( !released )
	{
		failureFeedback(owner, "That stored spell is no longer available.");
		return false;
	}
	StoredRelease release;
	if ( !vault->release(selection, selectedIndex, randomSeed, proficiency,
			nativeSpellPower(owner, preview.snapshot.spellId), release) )
	{
		spellDeconstructor(released);
		return false;
	}
	// The preview and commit execute synchronously on the authoritative server,
	// so this should only fail if a different subsystem changed MP mid-call.
	if ( !owner.safeConsumeMP(release.releaseMana) )
	{
		spellDeconstructor(released);
		return false;
	}
	modifyReleasedSpellPower(*released, release.releasePower);
	CastSpellProps_t properties;
	properties.caster_x = owner.x;
	properties.caster_y = owner.y;
	properties.target_x = owner.x + std::cos(owner.yaw) * 64.0;
	properties.target_y = owner.y + std::sin(owner.yaw) * 64.0;
	{
		StoredReleaseGuard guard;
		castSpell(owner.getUID(), released, false, false, false,
			&properties, false);
	}
	spellDeconstructor(released);
	effectFeedback(owner, "Stored magic is released at reduced cost.");
	return true;
}

bool discardStoredMagic(Entity& owner, const std::size_t selectedIndex)
{
	StoredMagicVault* vault = vaultFor(owner);
	Stat* ownerStats = owner.getStats();
	return enabled() && multiplayer != CLIENT && vault && ownerStats
		&& ownerStats->getEffectActive(EFF_STORE_MAGIC)
		&& vaultAllowsExactSelection(effectiveProficiency(&owner))
		&& vault->discard(selectedIndex);
}

bool storedReleaseInProgress() { return releasingStoredSpell; }

bool requestVaultAction(const int player, const bool discard,
	const VaultSelection selection, const std::size_t selectedIndex)
{
	if ( !enabled() || player < 0 || player >= MAXPLAYERS
		|| !players[player] || !players[player]->entity
		|| selectedIndex > 0xff )
	{
		return false;
	}
	if ( multiplayer != CLIENT )
	{
		return discard
			? discardStoredMagic(*players[player]->entity, selectedIndex)
			: releaseStoredMagic(*players[player]->entity,
				selection, selectedIndex);
	}
	std::memcpy(net_packet->data, "ILVR", 4);
	net_packet->data[4] = discard ? 1 : 0;
	net_packet->data[5] = static_cast<Uint8>(selection);
	net_packet->data[6] = static_cast<Uint8>(selectedIndex);
	net_packet->data[7] = static_cast<Uint8>(player);
	net_packet->address.host = net_server.host;
	net_packet->address.port = net_server.port;
	net_packet->len = 8;
	return sendPacketSafe(net_sock, -1, net_packet, 0) != 0;
}

bool receiveVaultSnapshot(const std::uint32_t ownerUid,
	const std::size_t capacity, const int proficiency,
	const std::vector<StoredSpellSnapshot>& entries)
{
	if ( multiplayer != CLIENT || !enabled() || ownerUid == 0
		|| capacity > 5 || entries.size() > capacity )
	{
		return false;
	}
	bool localOwner = false;
	for ( int player = 0; player < MAXPLAYERS; ++player )
	{
		if ( players[player] && players[player]->isLocalPlayer()
			&& players[player]->entity
			&& players[player]->entity->getUID() == ownerUid )
		{
			localOwner = true;
			break;
		}
	}
	if ( !localOwner ) { return false; }
	ClientVaultDisplay& display = state.clientVaults[ownerUid];
	display.capacity = capacity;
	display.proficiency = std::clamp(proficiency, 0, 100);
	display.entries = entries;
	return true;
}

} // namespace IllusionMagic
