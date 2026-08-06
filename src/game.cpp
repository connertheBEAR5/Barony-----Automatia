/*-------------------------------------------------------------------------------

	BARONY
	File: game.cpp
	Desc: contains main game code

	Copyright 2013-2016 (c) Turning Wheel LLC, all rights reserved.
	See LICENSE for details.

-------------------------------------------------------------------------------*/

#include "main.hpp"
#include "automatia_world_save.hpp"
#include "draw.hpp"
#include "game.hpp"
#include "stat.hpp"
#include "messages.hpp"
#include "entity.hpp"
#include "files.hpp"
#include "menu.hpp"
#include "classdescriptions.hpp"
#include "interface/interface.hpp"
#include "interface/consolecommand.hpp"
#include "magic/magic.hpp"
#include "engine/audio/sound.hpp"
#include "items.hpp"
#include "init.hpp"
#include "shops.hpp"
#include "monster.hpp"
#include "scores.hpp"
#include "menu.hpp"
#include "net.hpp"
#ifdef STEAMWORKS
#include <steam/steam_api.h>
#include "steam.hpp"
#endif
#ifdef USE_PLAYFAB
#include "playfab.hpp"
#endif
#include "prng.hpp"
#include "collision.hpp"
#include "paths.hpp"
#include "player.hpp"
#include "world_state.hpp"
#ifdef SAM_FRAMEWORK_ENABLED
#include "sam/sam_item_registry_foundation.hpp"
#include "sam/framework/sam_sync.hpp"
#endif
#include "mod_tools.hpp"
#include "lobbies.hpp"
#include "interface/ui.hpp"
#include "ui/GameUI.hpp"
#include "ui/MainMenu.hpp"
#include <limits>
#include "ui/Frame.hpp"
#include "ui/Field.hpp"
#include "input.hpp"
#include "ui/Image.hpp"
#include "ui/MainMenu.hpp"
#include "ui/LoadingScreen.hpp"

#include "UnicodeDecoder.h"
#include <string>
#include <atomic>
#include <future>
#include <iterator>
#include <memory>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <cstring>
/*
 * Implemented in mechanisms.cpp.
 * Reapplies the restored output of a signal timer or AND gate.
 */
void persistentSignalControllerBroadcastOutput(
    Entity* controller
);
static std::string normalizePersistentMapKey(std::string key);
static void capturePersistentMinimap(bool requirePlayerOnActiveMap = true);
static void capturePersistentMechanismStates();
static void capturePersistentMapRemovals();

static bool parseAutomatiaMapInstanceKey(
	const std::string& key,
	WorldInstanceIdentity& identity
)
{
	const std::size_t separator = key.find('#');
	if ( separator == std::string::npos
		|| key.find('#', separator + 1) != std::string::npos )
	{
		return false;
	}
	return identity.set(
		key.substr(0, separator),
		key.substr(separator + 1)
	);
}

struct PendingAutomatiaTransition
{
	int playerIndex = -1;
	std::string destinationMap;
	std::string destinationInstanceKey;
	Sint32 destinationTunnelID = 0;
	int destinationLevel = 0;
	bool destinationSecret = false;
	Uint32 destinationMapSeed = 0;
	bool destinationGenerated = false;
	bool recordSourceVisit = true;
	bool hasReturnPlacement = false;
	AutomatiaPlayerLevelVisit reverseVisit;
};

static std::vector<PendingAutomatiaTransition> pendingAutomatiaTransitions;

bool queueAutomatiaCustomTransition(
	int playerIndex,
	const std::string& destinationMap,
	Sint32 destinationTunnelID,
	int destinationLevel,
	bool destinationSecret
)
{
	if ( multiplayer != SERVER
		|| playerIndex < 0
		|| playerIndex >= MAXPLAYERS
		|| !players[playerIndex]
		|| !players[playerIndex]->entity
		|| destinationMap.empty() )
	{
		return false;
	}
	for ( const PendingAutomatiaTransition& pending : pendingAutomatiaTransitions )
	{
		if ( pending.playerIndex == playerIndex )
		{
			return true;
		}
	}
	PendingAutomatiaTransition transition;
	transition.playerIndex = playerIndex;
	transition.destinationMap = destinationMap;
	transition.destinationTunnelID = destinationTunnelID;
	transition.destinationLevel = destinationLevel;
	transition.destinationSecret = destinationSecret;
	transition.recordSourceVisit = true;
	pendingAutomatiaTransitions.push_back(std::move(transition));
	return true;
}

bool queueAutomatiaReverseTransition(
	const int playerIndex,
	const AutomatiaPlayerLevelVisit& destination
)
{
	if ( multiplayer != SERVER
		|| playerIndex < 0
		|| playerIndex >= MAXPLAYERS
		|| client_disconnected[playerIndex]
		|| !players[playerIndex]
		|| !players[playerIndex]->entity
		|| destination.mapInstanceKey.empty() )
	{
		return false;
	}
	WorldInstanceIdentity identity;
	if ( !parseAutomatiaMapInstanceKey(
		destination.mapInstanceKey,
		identity
	) )
	{
		return false;
	}
	for ( const PendingAutomatiaTransition& pending : pendingAutomatiaTransitions )
	{
		if ( pending.playerIndex == playerIndex )
		{
			return false;
		}
	}
	PendingAutomatiaTransition transition;
	transition.playerIndex = playerIndex;
	transition.destinationMap = identity.instanceId == "world"
		? identity.mapFile
		: std::string{};
	transition.destinationInstanceKey = identity.key();
	transition.destinationLevel = destination.dungeonLevel;
	transition.destinationSecret = destination.secretLevel;
	transition.destinationMapSeed = destination.mapSeed;
	transition.destinationGenerated = identity.instanceId != "world";
	transition.recordSourceVisit = false;
	transition.hasReturnPlacement = destination.returnPlacement.valid;
	transition.reverseVisit = destination;
	pendingAutomatiaTransitions.push_back(std::move(transition));
	return true;
}

static std::string persistentStableItemId(const Sint32 runtimeId)
{
#ifdef SAM_FRAMEWORK_ENABLED
    if (SAMItemRegistryFoundation::isRegisteredRuntimeItemId(runtimeId))
    {
        return SAMItemRegistryFoundation::stableIdForRuntimeId(runtimeId);
    }
#endif
    return "";
}

static Sint32 resolvePersistentItemType(
    const std::string& stableId,
    const Sint32 legacyType
)
{
    if (stableId.empty())
    {
        return legacyType >= 0 && legacyType < NUMITEMS ? legacyType : -1;
    }
#ifdef SAM_FRAMEWORK_ENABLED
    const Sint32 runtimeId =
        SAMItemRegistryFoundation::runtimeIdForStableId(stableId);
    if (runtimeId >= 0
        && SAMItemRegistryFoundation::isRegisteredRuntimeItemId(runtimeId))
    {
        return runtimeId;
    }
#endif
    printlog(
        "[Persistent World] Custom item definition unavailable: [%s].",
        stableId.c_str()
    );
    return -1;
}
/*
 * One item stored inside a normal persistent chest.
 *
 * Void-chest inventory is global player state and is intentionally
 * excluded from per-map persistence.
 */
struct PersistentChestItemState
{
    std::string stableId;
    Sint32 type = 0;
    Sint32 status = 0;
    Sint32 beatitude = 0;
    Sint32 count = 0;

    Uint32 appearance = 0;

    bool identified = false;

    Sint32 x = 0;
    Sint32 y = 0;
	    /*
     * Additional metadata used by shop inventories.
     *
     * These remain at their defaults for ordinary chest items.
     */
    bool isDroppable = true;
    bool playerSoldItemToShop = false;
    bool itemSpecialShopConsumable = false;

    Uint8 itemRequireTradingSkillInShop = 0;
};
/*
 * One item currently existing directly in the game world.
 *
 * This structure is used for both:
 *
 * 1. Original .lmp items with a stable persistentID.
 * 2. Dynamic items dropped or spawned during gameplay.
 *
 * Entity UID relationships are deliberately not preserved because
 * runtime UIDs can refer to different entities after another map loads.
 */
struct PersistentWorldItemState
{
    std::string stableId;
    Sint32 type = 0;
    Sint32 status = 0;
    Sint32 beatitude = 0;
    Sint32 count = 0;

    Uint32 appearance = 0;
    bool identified = false;

    real_t x = 0.0;
    real_t y = 0.0;
    real_t z = 0.0;

    real_t yaw = 0.0;
    real_t pitch = 0.0;
    real_t roll = 0.0;

    real_t velX = 0.0;
    real_t velY = 0.0;
    real_t velZ = 0.0;

    Sint32 itemNotMoving = 0;
    Sint32 itemSokobanReward = 0;
    Sint32 itemStolen = 0;
    Sint32 itemShowOnMap = 0;
    Sint32 itemDelayMonsterPickingUp = 0;

    bool passable = true;
    bool invisible = false;
    bool burning = false;
    bool burnable = true;
};
/*
 * One gold pile currently existing directly in the world.
 *
 * Gold hidden inside a chest, breakable container, or other parent
 * entity is excluded. Its owning entity is responsible for recreating
 * it until the gold is released into the world.
 */
struct PersistentGoldBagState
{
    Sint32 amount = 0;
    Sint32 amountBonus = 0;

    Sint32 sokoban = 0;
    Sint32 bouncing = 0;
    Sint32 droppedByPlayer = 0;

    real_t x = 0.0;
    real_t y = 0.0;
    real_t z = 0.0;

    real_t yaw = 0.0;
    real_t pitch = 0.0;
    real_t roll = 0.0;

    real_t velX = 0.0;
    real_t velY = 0.0;
    real_t velZ = 0.0;

    bool passable = true;
    bool invisible = false;
};
/*
 * One item owned by an ordinary persistent monster.
 *
 * slot:
 * 0  = ordinary inventory item
 * 1  = weapon
 * 2  = shield
 * 3  = helmet
 * 4  = breastplate
 * 5  = gloves
 * 6  = shoes
 * 7  = cloak
 * 8  = amulet
 * 9  = ring
 * 10 = mask
 */
struct PersistentMonsterItemState
{
    std::string stableId;
    Sint32 slot = 0;

    Sint32 type = 0;
    Sint32 status = 0;
    Sint32 beatitude = 0;
    Sint32 count = 0;

    Uint32 appearance = 0;

    bool identified = false;
    bool isDroppable = true;

    Sint32 x = 0;
    Sint32 y = 0;
};
/*
 * One temporary status effect currently active on a persistent
 * monster or NPC.
 *
 * effectValue is Uint8 rather than bool because several Barony effects
 * use this byte to store effect strength or additional state.
 *
 * Only effects with a positive finite timer are captured. Permanent,
 * equipment-derived, sustained-spell, and entity-linked effects are
 * deliberately allowed to initialize normally.
 */
struct PersistentMonsterEffectState
{
    Sint32 effectID = -1;
    Uint8 effectValue = 0;
    Sint32 timer = 0;
};
struct PersistentMechanismState
{
enum class Kind : Uint8
{
    None,
    Lever,
    TimedLever,
    Gate,
    Door,
	Furniture,
	ColliderDecoration,
	PowerCrystal,
	BoulderTrap,
	SignalController,
	Bell,
	SinkOrFountain,
	Campfire,
	WallLock,
	WallButton,
	PressurePlate,
	Pedestal,
	Chest,
	ShopkeeperInventory,
    WorldItem,
    GoldBag,
    MonsterLivingState,
    SummonTrap
};

    Kind kind = Kind::None;

    // Ordinary lever.
    Sint32 switchPower = 0;
    real_t roll = 0.0;

    // Timed lever.
    Sint32 leverStatus = 0;
    Sint32 leverTimerTicks = 0;

    // Gate.
    Sint32 gateStatus = 0;
    Sint32 gateInit = 0;
    Sint32 gateRattle = 0;
    Sint32 gateInverted = 0;
    Sint32 circuitStatus = 0;

    real_t gateStartHeight = 0.0;
    real_t gateZ = 0.0;
    real_t gateVelZ = 0.0;

	    // Wooden and iron doors.
    bool doorIsIron = false;

    Sint32 doorDir = 0;
    Sint32 doorStatus = 0;
    Sint32 doorHealth = 0;
    Sint32 doorMaxHealth = 0;
    Sint32 doorLocked = 0;
    Sint32 doorSmacked = 0;
    Sint32 doorTimer = 0;

    Sint32 doorPreventLockpickExploit = 0;
    Sint32 doorForceLockedUnlocked = 0;
    Sint32 doorDisableLockpicks = 0;
    Sint32 doorDisableOpening = 0;
    Sint32 doorLockpickHealth = 0;
    Sint32 doorUnlockWhenPowered = 0;
    Sint32 doorCircuitStatus = 0;

    real_t doorStartAng = 0.0;
    real_t doorYaw = 0.0;
    real_t doorX = 0.0;
    real_t doorY = 0.0;
    real_t doorFocalY = 0.0;

    bool doorBurning = false;
    bool doorBurnable = false;
	    // Furniture: tables, chairs, beds, bunk beds and podiums.
    Sint32 furnitureType = 0;
    Sint32 furnitureHealth = 0;
    Sint32 furnitureMaxHealth = 0;
	
    bool furnitureBurning = false;
    bool furnitureBurnable = false;
    // Damageable collider decorations.
    Sint32 colliderCurrentHP = 0;
    Sint32 colliderMaxHP = 0;
    Sint32 colliderDamageTypes = 0;
    Sint32 colliderHasCollision = 0;

    bool colliderBurning = false;
    bool colliderBurnable = false;
	    // Power crystal.
    Sint32 crystalInitialised = 0;
    Sint32 crystalDirection = 0;
    Sint32 crystalNumElectricityNodes = 0;
    Sint32 crystalTurnReverse = 0;
    Sint32 crystalSpellToActivate = 0;
    Sint32 crystalCircuitStatus = 0;
	Sint32 crystalPowerToActivate = 0;

	    /*
     * Boulder trap runtime state.
     *
     * trapBehavior:
     * 1 = central
     * 2 = east
     * 3 = south
     * 4 = west
     * 5 = north
     */
    Sint32 boulderTrapBehavior = 0;
    Sint32 boulderTrapFired = 0;
    Sint32 boulderTrapRefireAmount = 0;
    Sint32 boulderTrapRefireCounter = 0;
    Sint32 boulderTrapPreDelay = 0;
    Sint32 boulderTrapCircuitStatus = 0;
    Sint32 boulderTrapSabotaged = 0;


	/*
     * Signal timer and AND-gate controller runtime state.
     *
     * signalControllerIsAND:
     * 0 = signal timer
     * 1 = AND gate
     */
    bool signalControllerIsAND = false;

    Sint32 signalSwitchPower = 0;
    Sint32 signalDelayCount = 0;
    Sint32 signalTimerCount = 0;
    Sint32 signalRepeatCount = 0;
    Sint32 signalLatchInput = 0;
    Sint32 signalANDPowerMask = 0;
    Sint32 signalCircuitStatus = 0;
    Sint32 signalInitialized = 0;

	/*
     * Bell runtime state.
     *
     * The generated bell children are rebuilt by actBell() after loading,
     * so bellInit/skill[8] is intentionally not restored here.
     */
    Sint32 bellActiveTimer = 0;
    Sint32 bellHasItem = 0;
    Sint32 bellUses = 0;
    Sint32 bellCurrentEvent = 0;
    Sint32 bellUseDelay = 0;
    Sint32 bellClapperBroken = 0;
    Sint32 bellBulbBroken = 0;
    Sint32 bellBuffType = 0;
    Sint32 bellBurningTimer = 0;
	Sint32 bellScrapCreated = 0;
    bool bellBurning = false;
    bool bellBurnable = false;
    bool bellInvisible = false;

	    /*
     * Sink and fountain runtime state.
     *
     * waterSourceIsFountain:
     * false = sink
     * true  = fountain
     */
    bool waterSourceIsFountain = false;

    /*
     * Sink:
     * skill[0] = remaining uses
     * skill[3] = effect selected for the next use
     *
     * Fountain:
     * skill[0] = full/dry
     * skill[1] = selected main effect
     * skill[3] = selected potion effect
     */
    Sint32 waterSourceUses = 0;
    Sint32 waterSourceMainEffect = 0;
    Sint32 waterSourceSecondaryEffect = 0;


	/*
     * Campfire runtime state.
     *
     * skill[3] = remaining uses / fire health
     */
    Sint32 campfireHealth = 0;

	    /*
     * Wall-lock runtime state.
     *
     * wallLockState:
     * skill[0] - current key/lock state
     *
     * wallLockPower:
     * skill[8] - current unlocked/bypassed power state
     *
     * wallLockPickHealth:
     * skill[12] - remaining lockpick health
     *
     * wallLockPreventLockpickExploit:
     * skill[14] - lockpick anti-exploit progress/state
     */
    Sint32 wallLockSavedState = 0;
    Sint32 wallLockSavedPower = 0;
    Sint32 wallLockSavedPickHealth = 50;
    Sint32 wallLockSavedPreventExploit = 0;

	    /*
     * Wall-button runtime state.
     *
     * wallButtonState:
     * 0 = released
     * 1 = pressed
     *
     * wallButtonPower:
     *  0 = no output
     * >0 = timed countdown
     * -1 = permanently active
     */
    Sint32 wallButtonState = 0;
    Sint32 wallButtonPower = 0;

    /*
     * Pressure-plate runtime state.
     *
     * pressurePlatePermanent:
     * false = ordinary plate, releases when unoccupied
     * true  = latched plate, remains active after triggering
     *
     * pressurePlatePower:
     * skill[0] current output/latch state
     *
     * pressurePlateInteractionLock:
     * skill[1], used by permanent/scripted plates to disable
     * additional entity checks.
     */
    bool pressurePlatePermanent = false;
    Sint32 pressurePlatePower = 0;
    Sint32 pressurePlateInteractionLock = 0;

	    /*
     * Orb-pedestal runtime state.
     *
     * The pedestal's floating orb is a generated child and is rebuilt
     * by assignActions(). Only the parent pedestal owns persistent
     * gameplay state.
     */
    Sint32 pedestalSavedHasOrb = 0;
    Sint32 pedestalSavedPowerStatus = 0;
    Sint32 pedestalSavedInit = 0;
    Sint32 pedestalSavedInGround = 0;

    real_t pedestalSavedZ = 4.5;
    real_t pedestalSavedVelZ = 0.0;

    bool pedestalSavedPassable = false;

	    /*
     * Normal chest runtime state.
     *
     * Chests are always restored closed because a player/chest GUI
     * interaction cannot remain active across a map transition.
     */
    Sint32 chestSavedHealth = 0;
    Sint32 chestSavedMaxHealth = 0;
    Sint32 chestSavedLocked = 0;
    Sint32 chestSavedLockpickHealth = 0;
    Sint32 chestSavedPreventExploit = 0;
    Sint32 chestSavedOldHealth = 0;
    Sint32 chestSavedVoidState = 0;

    std::vector<PersistentChestItemState>
        chestSavedInventory;


	    /*
     * Shopkeeper inventory state.
     *
     * Living/combat state is deliberately deferred to the general
     * monster-persistence stage.
     */
    Sint32 shopkeeperSavedStoreType = 0;
    Sint32 shopkeeperSavedGold = 0;
	Sint32 shopkeeperLoadsSinceRestock = 0;
    std::string shopkeeperSavedName;

    std::vector<PersistentChestItemState>
        shopkeeperSavedInventory;
    /*
    * Runtime state for an original floor item from the .lmp.
    *
    * Original items are indexed by their stable persistentID through
    * mechanismStates.
    */
    PersistentWorldItemState worldItemState;
    /*
    * Runtime state for an original gold pile from the .lmp.
    */
    PersistentGoldBagState goldBagState;
    /*
    * Surviving original monster/NPC state.
    *
    * Dead original monsters are handled separately by removedEntityIDs.
    * Dynamic summons without a persistentID are intentionally excluded.
    *
    * Mechanical creatures retain damage but do not receive passive
    * revisit healing.
    */
    Sint32 monsterSavedType = NOTHING;

    Sint32 monsterSavedHP = 0;
    Sint32 monsterSavedMAXHP = 0;
    Sint32 monsterSavedMP = 0;
    Sint32 monsterSavedMAXMP = 0;
    /*
    * Complete ordinary-monster inventory and equipped loadout.
    *
    * Shopkeepers are intentionally excluded because their inventory is
    * managed by the separate shop/restock persistence system.
    */
    std::vector<PersistentMonsterItemState>
        monsterSavedItems;

        /*
    * Temporary effects that should remain frozen while the map is
    * unloaded.
    */
    std::vector<PersistentMonsterEffectState>
        monsterSavedEffects;
    real_t monsterSavedX = 0.0;
    real_t monsterSavedY = 0.0;
    real_t monsterSavedZ = 0.0;

    real_t monsterSavedYaw = 0.0;
    real_t monsterSavedPitch = 0.0;
    real_t monsterSavedRoll = 0.0;

    /*
    * Summoning-trap runtime state.
    *
    * These correspond to skill[0] through skill[9].
    * Persisting the complete block prevents completed cycles from
    * restarting and prevents random/scaled traps from rerolling.
    */
    Sint32 summonTrapMonster = 0;
    Sint32 summonTrapCount = 0;
    Sint32 summonTrapInterval = 0;
    Sint32 summonTrapSpawnCycles = 0;
    Sint32 summonTrapPowerToDisable = 0;
    Sint32 summonTrapFailureRate = 0;
    Sint32 summonTrapFired = 0;
    Sint32 summonTrapInitialized = 0;
    Sint32 summonTrapTicksToFire = 0;
    Sint32 summonTrapPlayerProximity = 0;

// PLACE STUFF ABOVE THIS
    bool passable = false;
	//This is a shared field dont change ^
};
/*
 * Runtime boulders created by persistent map traps.
 *
 * These do not exist in the original .lmp, so they cannot use the
 * original-entity removal registry. A fresh snapshot of surviving
 * trap-created boulders is stored whenever the player leaves a map.
 */
struct PersistentBoulderState
{
    Sint32 sourceTrapPersistentID = 0;
    Sint32 sprite = 0;

    real_t x = 0.0;
    real_t y = 0.0;
    real_t z = 0.0;

    real_t yaw = 0.0;
    real_t pitch = 0.0;
    real_t roll = 0.0;

    real_t velX = 0.0;
    real_t velY = 0.0;
    real_t velZ = 0.0;

    Sint32 stopped = 0;
    Sint32 noGround = 0;
    Sint32 rolling = 0;
    Sint32 rollDirection = 0;
    Sint32 destinationX = 0;
    Sint32 destinationY = 0;
    Sint32 initialized = 0;
    Sint32 lavaExplodeTimer = -1;

    bool passable = false;
};
/*
 * One persistent tile override.
 *
 * The original map is reloaded normally. Only tile values that differ
 * from that original loaded baseline are stored here and reapplied.
 */
struct PersistentTileState
{
    Sint32 x = 0;
    Sint32 y = 0;
    Sint32 layer = 0;
    Sint32 tile = 0;
};

/*
 * Existing wall packets use 16-bit map coordinates, and runtime maps
 * use far fewer than 65536 tiles per axis.
 *
 * Packed layout:
 * bits  0-15: x
 * bits 16-31: y
 * bits 32-47: layer
 */
static Uint64 makePersistentTileKey(
    Sint32 x,
    Sint32 y,
    Sint32 layer
)
{
    return
        static_cast<Uint64>(
            static_cast<Uint16>(x)
        )
        | (
            static_cast<Uint64>(
                static_cast<Uint16>(y)
            ) << 16
        )
        | (
            static_cast<Uint64>(
                static_cast<Uint16>(layer)
            ) << 32
        );
}
struct PersistentMapRemovalState
{
    std::unordered_set<Sint32> originalEntityIDs;
    std::unordered_set<Sint32> removedEntityIDs;

    /*
     * Runtime state indexed by the stable ID inherited from the
     * original editor sprite.
     */
    std::unordered_map<
        Sint32,
        PersistentMechanismState
    > mechanismStates;
	    /*
     * Raw tile baseline captured immediately after the .lmp is loaded,
     * before persistent overrides are applied.
     *
     * This is replaced on every visit so edited map files use their
     * newest authored baseline.
     */
    std::vector<Sint32> originalTiles;

    Sint32 originalTileWidth = 0;
    Sint32 originalTileHeight = 0;
    Sint32 originalTileLayers = 0;

    /*
     * Only coordinates whose current tile differs from originalTiles.
     */
    std::unordered_map<
        Uint64,
        PersistentTileState
    > tileStates;
	    /*
     * Current surviving runtime boulders created by map traps.
     *
     * This collection is replaced, not appended, whenever map state
     * is captured.
     */
    std::vector<PersistentBoulderState> dynamicBoulders;
    /*
    * Current world items that were created during gameplay and therefore
    * do not have an original .lmp persistentID.
    *
    * This is replaced with a fresh snapshot every time the map is left.
    */
    std::vector<PersistentWorldItemState> dynamicWorldItems;
    /*
    * Gold piles created during gameplay do not have an original .lmp
    * persistent ID. Replace this vector with a fresh snapshot whenever
    * the player leaves the map.
    */
    std::vector<PersistentGoldBagState> dynamicGoldBags;

    /*
    * Negative persistent IDs belonging to runtime monsters which must be
    * recreated when this map is visited again.
    *
    * Their actual living/inventory/effect state remains stored in
    * mechanismStates using the same negative ID.
    */
    std::unordered_set<Sint32> dynamicMonsterIDs;

    bool originalIDsRegistered = false;
};
/*
 * Session-persistent custom dialogue and quest state.
 *
 * Authored dialogue definitions will be loaded separately. These
 * structures contain only mutable runtime progress and are therefore
 * suitable for eventual disk serialization.
 *
 * Important:
 * - Do not store Entity pointers.
 * - Do not store runtime entity UIDs.
 * - Original map entities are identified with normalized map keys and
 *   positive persistent IDs.
 */

/*
 * Shared progress for one named quest.
 *
 * A quest may be read or modified by multiple NPCs on multiple maps.
 */
struct PersistentQuestState
{
    bool started = false;
    bool accepted = false;
    bool completed = false;
    bool failed = false;

    Sint32 stage = 0;

    /*
     * Creator-defined quest values.
     *
     * Examples:
     * negotiated_reward = 100
     * bandits_defeated = 4
     * trust = 2
     */
    std::unordered_map<
        std::string,
        Sint32
    > variables;

    /*
     * Creator-defined Boolean facts.
     *
     * Examples:
     * asked_about_castle
     * reward_claimed
     * farmhouse_cleared
     */
    std::unordered_set<std::string> flags;

    /*
     * Stable string IDs authored by the dialogue/quest editor.
     */
    std::unordered_set<std::string> completedObjectives;
    std::unordered_set<std::string> usedChoices;
};

/*
 * Personal memory belonging to one persistent NPC.
 *
 * The registry key is:
 *
 * normalized-map-name + ":" + positive persistent ID
 *
 * Example:
 *
 * village.lmp:125
 */
struct PersistentNPCDialogueState
{
    /*
     * Authored dialogue graph assigned to this NPC.
     *
     * The graph itself is not copied into runtime persistence.
     */
    std::string dialogueID;

    /*
     * The node to evaluate when this NPC is next interacted with.
     */
    Sint32 currentNode = 0;

    bool conversationStarted = false;
    bool rewardGiven = false;

    /*
     * NPC-local creator variables and flags.
     *
     * These are intentionally separate from shared quest/world state.
     */
    std::unordered_map<
        std::string,
        Sint32
    > variables;

    std::unordered_set<std::string> flags;
    std::unordered_set<std::string> usedChoices;
    std::unordered_set<std::string> seenNodes;
};

/*
 * Story state shared across every visited map in the active session.
 */
struct PersistentWorldStoryState
{
    /*
     * Shared quest state indexed by creator-defined quest ID.
     */
    std::unordered_map<
        std::string,
        PersistentQuestState
    > quests;

    /*
     * Story values readable from any map.
     */
    std::unordered_map<
        std::string,
        Sint32
    > worldVariables;

    std::unordered_set<std::string> worldFlags;

    /*
     * NPC-local memories indexed by normalized map and persistent ID.
     */
    std::unordered_map<
        std::string,
        PersistentNPCDialogueState
    > npcDialogueStates;
};
static std::unordered_map<
    std::string,
    PersistentMapRemovalState
> persistentMapRemovalRegistry;

/*
 * Reserved positive-ID range for raw entities created by deterministic map
 * generation. Authored editor IDs remain untouched. These IDs are assigned
 * before assignActions(), then registered only after assignActions() confirms
 * that the raw marker survived or transferred its identity to a real runtime
 * entity.
 */
static constexpr Sint32 GENERATED_MAP_ENTITY_PERSISTENT_ID_BASE =
    0x40000000;
static constexpr Sint32 GENERATED_RUNTIME_ENTITY_PERSISTENT_ID_BASE =
    0x50000000;

static bool isGeneratedMapEntityPersistentID(const Sint32 persistentID)
{
    return persistentID >= GENERATED_MAP_ENTITY_PERSISTENT_ID_BASE;
}
/*
 * Session-only custom dialogue, quest, and world-story memory.
 *
 * This is deliberately separate from per-map physical state because
 * quests and world facts may be shared by many maps and NPCs.
 */
static PersistentWorldStoryState
    persistentWorldStoryState;
static AutomatiaSave::Json preservedAutomatiaWorldDocument;
static AutomatiaSave::Json pendingAutomatiaWorldDocument;

/*
 * Per-character level back stacks.
 *
 * Divergent paths require each player to own an independent route history.
 * Whole-party transitions append the same source level to every player who is
 * actually present on that map, while independent transitions append only the
 * player who leaves. The stable identity matches character-save/minimap
 * identity so reconnecting players recover their own route.
 */
using AutomatiaPlayerLevelHistory =
	std::vector<AutomatiaPlayerLevelVisit>;
static std::unordered_map<std::string, AutomatiaPlayerLevelHistory>
	automatiaPlayerLevelHistories;
static AutomatiaPlayerLevelHistory automatiaLegacyPartyLevelHistory;
constexpr std::size_t AUTOMATIA_PLAYER_LEVEL_HISTORY_LIMIT = 4096;
struct PendingAutomatiaSinglePlayerReverseReturn
{
	int playerIndex = -1;
	AutomatiaPlayerLevelVisit visit;
	bool pending = false;
};
static PendingAutomatiaSinglePlayerReverseReturn
	pendingAutomatiaSinglePlayerReverseReturn;
struct AutomatiaSavedPlayerPlacement
{
    WorldInstanceIdentity identity;
    real_t x = 0.0;
    real_t y = 0.0;
    real_t z = 0.0;
    real_t yaw = 0.0;
    real_t pitch = 0.0;
    real_t roll = 0.0;
    bool hasPosition = false;
    bool pending = false;
};
static AutomatiaSavedPlayerPlacement
    automatiaSavedPlayerPlacements[MAXPLAYERS];
/*
 * Per-character explored minimap state.
 *
 * The first key is the stable reconnect/character identity. The second key
 * is the canonical map-instance identity (for example start.lmp#world).
 * Remote clients report their own minimap progress to the authoritative
 * server; a headless server must never treat its process-local minimap array
 * as a remote player's discovery state.
 */
struct PersistentMinimapState
{
    Sint32 width = 0;
    Sint32 height = 0;
    std::vector<Sint8> tiles;
};
using PersistentMinimapMapRegistry =
    std::unordered_map<std::string, PersistentMinimapState>;
static std::unordered_map<std::string, PersistentMinimapMapRegistry>
    persistentPlayerMinimapRegistry;
/*
 * Compatibility storage for the original map-wide "minimap" field. It is
 * read once and offered as a fallback, then migrated into the player's
 * identity-scoped registry by the next capture/save.
 */
static std::unordered_map<std::string, PersistentMinimapState>
    persistentLegacyMinimapRegistry;
static Uint32 persistentMinimapServerTransferId = 0;
/*
 * The process-global minimap array survives map loads. Track the exact bound
 * map revision so stale cells are cleared once for each newly loaded map,
 * before any saved discovery is merged back into it.
 */
static std::string persistentMinimapClearedMapKey;
static std::uint64_t persistentMinimapClearedMapRevision = 0;
constexpr std::size_t PERSISTENT_MINIMAP_SERVER_CHUNK_PAYLOAD = 1600;
static_assert(
    28U + 255U + PERSISTENT_MINIMAP_SERVER_CHUNK_PAYLOAD
        <= NET_PACKET_SIZE - 9U,
    "Persistent minimap destination chunks exceed the safe-packet payload");

static std::vector<Uint8> packPersistentMinimapTilesForServer(
    const std::vector<Sint8>& tiles
)
{
    std::vector<Uint8> packed((tiles.size() * 3U + 7U) / 8U, 0);
    for (std::size_t index = 0; index < tiles.size(); ++index)
    {
        const Uint8 value = static_cast<Uint8>(
            std::max<Sint8>(0, std::min<Sint8>(4, tiles[index])));
        const std::size_t bit = index * 3U;
        const std::size_t byte = bit / 8U;
        const unsigned shift = static_cast<unsigned>(bit % 8U);
        packed[byte] |= static_cast<Uint8>(value << shift);
        if (shift > 5U && byte + 1U < packed.size())
        {
            packed[byte + 1U] |=
                static_cast<Uint8>(value >> (8U - shift));
        }
    }
    return packed;
}
/*
 * Original map entities use positive persistent IDs.
 * Explicitly persistent runtime monsters use negative IDs.
 *
 * Zero remains reserved for nonpersistent runtime entities.
 */
static Sint32 nextDynamicMonsterPersistentID = -1;
/*
 * Clients receive one complete map snapshot before the corresponding
 * LVLC/LVLR packet.
 */
static std::string clientPersistentSnapshotMapKey;
static bool clientPersistentSnapshotReceiving = false;
static bool clientPersistentSnapshotComplete = false;

static std::string persistentMinimapCharacterNameIdentity(
    const int playerIndex
)
{
    if (playerIndex < 0 || playerIndex >= MAXPLAYERS
        || !stats[playerIndex] || stats[playerIndex]->name[0] == '\0')
    {
        return "";
    }
    const std::string name = stats[playerIndex]->name;
    std::string normalized;
    normalized.reserve(name.size());
    for (const unsigned char character : name)
    {
        if (std::isalnum(character) || character == '_' || character == '-')
        {
            normalized.push_back(static_cast<char>(std::tolower(character)));
        }
        else if (!normalized.empty() && normalized.back() != '_')
        {
            normalized.push_back('_');
        }
    }
    while (!normalized.empty() && normalized.back() == '_')
    {
        normalized.pop_back();
    }
    return normalized.empty() ? "" : "character_name:" + normalized;
}

static std::string persistentMinimapPlayerIdentity(const int playerIndex)
{
    if (playerIndex < 0 || playerIndex >= MAXPLAYERS)
    {
        return "";
    }

    /*
     * A single-player world save already isolates one local character, so a
     * fixed identity is safer than depending on when the Stat name is loaded.
     * This lets minimap discovery restore during initial map construction.
     */
    if (multiplayer != CLIENT && multiplayer != SERVER)
    {
        return "singleplayer:local";
    }

    /*
     * Character-save LOCAL mode uses the normalized character name as its
     * durable identity. Prefer that same key for minimap persistence so a
     * newly generated reconnect token cannot orphan the saved discovery.
     */
    const std::string characterIdentity =
        persistentMinimapCharacterNameIdentity(playerIndex);
    if (!characterIdentity.empty())
    {
        return characterIdentity;
    }
    if (ReconnectToken::isValid(automatiaReconnectTokens[playerIndex]))
    {
        return "reconnect:" + automatiaReconnectTokens[playerIndex];
    }
    return "";
}

static void mergePersistentMinimapIdentity(
    const std::string& sourceIdentity,
    const std::string& destinationIdentity
)
{
    if (sourceIdentity.empty() || destinationIdentity.empty()
        || sourceIdentity == destinationIdentity)
    {
        return;
    }
    auto sourcePlayer =
        persistentPlayerMinimapRegistry.find(sourceIdentity);
    if (sourcePlayer == persistentPlayerMinimapRegistry.end())
    {
        return;
    }
    PersistentMinimapMapRegistry sourceMaps =
        std::move(sourcePlayer->second);
    persistentPlayerMinimapRegistry.erase(sourcePlayer);
    PersistentMinimapMapRegistry& destination =
        persistentPlayerMinimapRegistry[destinationIdentity];
    for (auto& savedMap : sourceMaps)
    {
        auto inserted = destination.emplace(savedMap.first, savedMap.second);
        if (inserted.second)
        {
            continue;
        }
        PersistentMinimapState& target = inserted.first->second;
        const PersistentMinimapState& source = savedMap.second;
        if (target.width != source.width || target.height != source.height
            || target.tiles.size() != source.tiles.size())
        {
            continue;
        }
        for (std::size_t index = 0; index < target.tiles.size(); ++index)
        {
            if (target.tiles[index] == 0 && source.tiles[index] != 0)
            {
                target.tiles[index] = source.tiles[index];
            }
        }
    }
}

static void migratePersistentMinimapFallbackIdentity(const int playerIndex)
{
    if (playerIndex < 0 || playerIndex >= MAXPLAYERS)
    {
        return;
    }
    const std::string preferredIdentity =
        persistentMinimapPlayerIdentity(playerIndex);
    if (preferredIdentity.empty())
    {
        return;
    }

    const std::string characterIdentity =
        persistentMinimapCharacterNameIdentity(playerIndex);
    mergePersistentMinimapIdentity(
        characterIdentity, preferredIdentity);

    if (ReconnectToken::isValid(automatiaReconnectTokens[playerIndex]))
    {
        mergePersistentMinimapIdentity(
            "reconnect:" + automatiaReconnectTokens[playerIndex],
            preferredIdentity);
    }
}

static int persistentMinimapLocalPlayerIndex()
{
    if (multiplayer == CLIENT)
    {
        return clientnum >= 0 && clientnum < MAXPLAYERS ? clientnum : -1;
    }
    if (multiplayer == SERVER && headless)
    {
        return -1;
    }
    return clientnum >= 0 && clientnum < MAXPLAYERS ? clientnum : 0;
}

static PersistentMinimapState* persistentMinimapStateForPlayer(
    const int playerIndex,
    const std::string& mapKey
)
{
    migratePersistentMinimapFallbackIdentity(playerIndex);
    const std::string identity =
        persistentMinimapPlayerIdentity(playerIndex);
    if (identity.empty())
    {
        return nullptr;
    }
    auto player = persistentPlayerMinimapRegistry.find(identity);
    if (player == persistentPlayerMinimapRegistry.end())
    {
        return nullptr;
    }
    auto state = player->second.find(mapKey);
    return state != player->second.end() ? &state->second : nullptr;
}

namespace
{
bool savedJsonInt(const AutomatiaSave::Json& object, const char* key, Sint32& value)
{
    if (!object.is_object() || !object.contains(key)
        || !object[key].is_number_integer())
    {
        return false;
    }
    try
    {
        const std::int64_t parsed = object[key].get<std::int64_t>();
        if (parsed < std::numeric_limits<Sint32>::min()
            || parsed > std::numeric_limits<Sint32>::max())
        {
            return false;
        }
        value = static_cast<Sint32>(parsed);
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool savedJsonUint(const AutomatiaSave::Json& object, const char* key, Uint32& value)
{
    if (!object.is_object() || !object.contains(key))
    {
        return false;
    }
    try
    {
        if (object[key].is_number_unsigned())
        {
            const std::uint64_t parsed = object[key].get<std::uint64_t>();
            if (parsed <= std::numeric_limits<Uint32>::max())
            {
                value = static_cast<Uint32>(parsed);
                return true;
            }
            return false;
        }
        Sint32 parsed = 0;
        if (savedJsonInt(object, key, parsed) && parsed >= 0)
        {
            value = static_cast<Uint32>(parsed);
            return true;
        }
    }
    catch (const std::exception&)
    {
        return false;
    }
    return false;
}

bool savedJsonBool(const AutomatiaSave::Json& object, const char* key, bool& value)
{
    if (!object.is_object() || !object.contains(key) || !object[key].is_boolean())
    {
        return false;
    }
    value = object[key].get<bool>();
    return true;
}

bool savedJsonReal(const AutomatiaSave::Json& object, const char* key, real_t& value)
{
    if (!object.is_object() || !object.contains(key) || !object[key].is_number())
    {
        return false;
    }
    try
    {
        const real_t parsed = object[key].get<real_t>();
        if (!std::isfinite(parsed))
        {
            return false;
        }
        value = parsed;
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool savedJsonTriplet(
    const AutomatiaSave::Json& object,
    const char* key,
    real_t& first,
    real_t& second,
    real_t& third
)
{
    if (!object.is_object() || !object.contains(key)
        || !object[key].is_array() || object[key].size() != 3)
    {
        return false;
    }
    try
    {
        const real_t values[3] = {
            object[key][0].get<real_t>(),
            object[key][1].get<real_t>(),
            object[key][2].get<real_t>()
        };
        if (!std::isfinite(values[0])
            || !std::isfinite(values[1])
            || !std::isfinite(values[2]))
        {
            return false;
        }
        first = values[0];
        second = values[1];
        third = values[2];
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool safeSavedStoryId(const std::string& value)
{
    if (value.empty() || value.size() > 255)
    {
        return false;
    }
    for (const unsigned char character : value)
    {
        if (character < 0x20 || character == 0x7f)
        {
            return false;
        }
    }
    return true;
}

AutomatiaSave::Json saveChestItemState(const PersistentChestItemState& item)
{
    return AutomatiaSave::Json{
        {"stable_id", item.stableId},
        {"legacy_type", item.type},
        {"status", item.status},
        {"beatitude", item.beatitude},
        {"count", item.count},
        {"appearance", item.appearance},
        {"identified", item.identified},
        {"x", item.x},
        {"y", item.y},
        {"droppable", item.isDroppable},
        {"sold_to_shop", item.playerSoldItemToShop},
        {"special_shop_consumable", item.itemSpecialShopConsumable},
        {"trading_skill_required", item.itemRequireTradingSkillInShop}
    };
}

bool restoreChestItemState(
    const AutomatiaSave::Json& saved,
    PersistentChestItemState& item
)
{
    Sint32 tradingSkill = 0;
    if (!savedJsonInt(saved, "legacy_type", item.type)
        || !savedJsonInt(saved, "status", item.status)
        || !savedJsonInt(saved, "beatitude", item.beatitude)
        || !savedJsonInt(saved, "count", item.count)
        || !savedJsonUint(saved, "appearance", item.appearance)
        || !savedJsonInt(saved, "x", item.x)
        || !savedJsonInt(saved, "y", item.y)
        || item.count <= 0)
    {
        return false;
    }
    if (saved.contains("stable_id") && saved["stable_id"].is_string()
        && saved["stable_id"].get<std::string>().size() <= 255)
    {
        item.stableId = saved["stable_id"].get<std::string>();
    }
    savedJsonBool(saved, "identified", item.identified);
    savedJsonBool(saved, "droppable", item.isDroppable);
    savedJsonBool(saved, "sold_to_shop", item.playerSoldItemToShop);
    savedJsonBool(
        saved, "special_shop_consumable", item.itemSpecialShopConsumable
    );
    if (savedJsonInt(saved, "trading_skill_required", tradingSkill)
        && tradingSkill >= 0
        && tradingSkill <= std::numeric_limits<Uint8>::max())
    {
        item.itemRequireTradingSkillInShop = static_cast<Uint8>(tradingSkill);
    }
    return true;
}

AutomatiaSave::Json saveMonsterItemState(const PersistentMonsterItemState& item)
{
    return AutomatiaSave::Json{
        {"stable_id", item.stableId},
        {"slot", item.slot},
        {"legacy_type", item.type},
        {"status", item.status},
        {"beatitude", item.beatitude},
        {"count", item.count},
        {"appearance", item.appearance},
        {"identified", item.identified},
        {"droppable", item.isDroppable},
        {"x", item.x},
        {"y", item.y}
    };
}

bool restoreMonsterItemState(
    const AutomatiaSave::Json& saved,
    PersistentMonsterItemState& item
)
{
    if (!savedJsonInt(saved, "slot", item.slot)
        || item.slot < 0 || item.slot > 10
        || !savedJsonInt(saved, "legacy_type", item.type)
        || !savedJsonInt(saved, "status", item.status)
        || !savedJsonInt(saved, "beatitude", item.beatitude)
        || !savedJsonInt(saved, "count", item.count)
        || !savedJsonUint(saved, "appearance", item.appearance)
        || !savedJsonInt(saved, "x", item.x)
        || !savedJsonInt(saved, "y", item.y)
        || item.count <= 0)
    {
        return false;
    }
    if (saved.contains("stable_id") && saved["stable_id"].is_string()
        && saved["stable_id"].get<std::string>().size() <= 255)
    {
        item.stableId = saved["stable_id"].get<std::string>();
    }
    savedJsonBool(saved, "identified", item.identified);
    savedJsonBool(saved, "droppable", item.isDroppable);
    return true;
}

AutomatiaSave::Json saveWorldItemState(const PersistentWorldItemState& item)
{
    return AutomatiaSave::Json{
        {"stable_id", item.stableId},
        {"legacy_type", item.type},
        {"status", item.status},
        {"beatitude", item.beatitude},
        {"count", item.count},
        {"appearance", item.appearance},
        {"identified", item.identified},
        {"position", {item.x, item.y, item.z}},
        {"rotation", {item.yaw, item.pitch, item.roll}},
        {"velocity", {item.velX, item.velY, item.velZ}},
        {"not_moving", item.itemNotMoving},
        {"sokoban_reward", item.itemSokobanReward},
        {"stolen", item.itemStolen},
        {"show_on_map", item.itemShowOnMap},
        {"pickup_delay", item.itemDelayMonsterPickingUp},
        {"passable", item.passable},
        {"invisible", item.invisible},
        {"burning", item.burning},
        {"burnable", item.burnable}
    };
}

bool restoreWorldItemState(
    const AutomatiaSave::Json& saved,
    PersistentWorldItemState& item
)
{
    if (!savedJsonInt(saved, "legacy_type", item.type)
        || !savedJsonInt(saved, "status", item.status)
        || !savedJsonInt(saved, "beatitude", item.beatitude)
        || !savedJsonInt(saved, "count", item.count)
        || !savedJsonUint(saved, "appearance", item.appearance)
        || item.count <= 0
        || !savedJsonTriplet(saved, "position", item.x, item.y, item.z)
        || !savedJsonTriplet(saved, "rotation", item.yaw, item.pitch, item.roll)
        || !savedJsonTriplet(saved, "velocity", item.velX, item.velY, item.velZ))
    {
        return false;
    }
    if (saved.contains("stable_id") && saved["stable_id"].is_string()
        && saved["stable_id"].get<std::string>().size() <= 255)
    {
        item.stableId = saved["stable_id"].get<std::string>();
    }
    savedJsonBool(saved, "identified", item.identified);
    savedJsonInt(saved, "not_moving", item.itemNotMoving);
    savedJsonInt(saved, "sokoban_reward", item.itemSokobanReward);
    savedJsonInt(saved, "stolen", item.itemStolen);
    savedJsonInt(saved, "show_on_map", item.itemShowOnMap);
    savedJsonInt(saved, "pickup_delay", item.itemDelayMonsterPickingUp);
    savedJsonBool(saved, "passable", item.passable);
    savedJsonBool(saved, "invisible", item.invisible);
    savedJsonBool(saved, "burning", item.burning);
    savedJsonBool(saved, "burnable", item.burnable);
    return true;
}

AutomatiaSave::Json saveGoldState(const PersistentGoldBagState& gold)
{
    return AutomatiaSave::Json{
        {"amount", gold.amount},
        {"bonus", gold.amountBonus},
        {"sokoban", gold.sokoban},
        {"bouncing", gold.bouncing},
        {"dropped_by_player", gold.droppedByPlayer},
        {"position", {gold.x, gold.y, gold.z}},
        {"rotation", {gold.yaw, gold.pitch, gold.roll}},
        {"velocity", {gold.velX, gold.velY, gold.velZ}},
        {"passable", gold.passable},
        {"invisible", gold.invisible}
    };
}

bool restoreGoldState(
    const AutomatiaSave::Json& saved,
    PersistentGoldBagState& gold
)
{
    if (!savedJsonInt(saved, "amount", gold.amount)
        || !savedJsonInt(saved, "bonus", gold.amountBonus)
        || gold.amount <= 0
        || !savedJsonTriplet(saved, "position", gold.x, gold.y, gold.z)
        || !savedJsonTriplet(saved, "rotation", gold.yaw, gold.pitch, gold.roll)
        || !savedJsonTriplet(saved, "velocity", gold.velX, gold.velY, gold.velZ))
    {
        return false;
    }
    savedJsonInt(saved, "sokoban", gold.sokoban);
    savedJsonInt(saved, "bouncing", gold.bouncing);
    savedJsonInt(saved, "dropped_by_player", gold.droppedByPlayer);
    savedJsonBool(saved, "passable", gold.passable);
    savedJsonBool(saved, "invisible", gold.invisible);
    return true;
}

AutomatiaSave::Json saveMechanismState(
    const Sint32 persistentId,
    const PersistentMechanismState& state
)
{
    using AutomatiaSave::Json;
    Json saved = {
        {"persistent_id", persistentId},
        {"payload_version", 1},
        {"kind", static_cast<Uint8>(state.kind)},
        {"switch_power", state.switchPower},
        {"roll", state.roll},
        {"lever_status", state.leverStatus},
        {"lever_timer_ticks", state.leverTimerTicks},
        {"gate_status", state.gateStatus},
        {"gate_init", state.gateInit},
        {"gate_rattle", state.gateRattle},
        {"gate_inverted", state.gateInverted},
        {"circuit_status", state.circuitStatus},
        {"gate_start_height", state.gateStartHeight},
        {"gate_z", state.gateZ},
        {"gate_velocity_z", state.gateVelZ},
        {"door_is_iron", state.doorIsIron},
        {"door_dir", state.doorDir},
        {"door_status", state.doorStatus},
        {"door_health", state.doorHealth},
        {"door_max_health", state.doorMaxHealth},
        {"door_locked", state.doorLocked},
        {"door_smacked", state.doorSmacked},
        {"door_timer", state.doorTimer},
        {"door_prevent_lockpick_exploit", state.doorPreventLockpickExploit},
        {"door_force_locked_unlocked", state.doorForceLockedUnlocked},
        {"door_disable_lockpicks", state.doorDisableLockpicks},
        {"door_disable_opening", state.doorDisableOpening},
        {"door_lockpick_health", state.doorLockpickHealth},
        {"door_unlock_when_powered", state.doorUnlockWhenPowered},
        {"door_circuit_status", state.doorCircuitStatus},
        {"door_start_angle", state.doorStartAng},
        {"door_yaw", state.doorYaw},
        {"door_x", state.doorX},
        {"door_y", state.doorY},
        {"door_focal_y", state.doorFocalY},
        {"door_burning", state.doorBurning},
        {"door_burnable", state.doorBurnable},
        {"furniture_type", state.furnitureType},
        {"furniture_health", state.furnitureHealth},
        {"furniture_max_health", state.furnitureMaxHealth},
        {"furniture_burning", state.furnitureBurning},
        {"furniture_burnable", state.furnitureBurnable},
        {"collider_health", state.colliderCurrentHP},
        {"collider_max_health", state.colliderMaxHP},
        {"collider_damage_types", state.colliderDamageTypes},
        {"collider_has_collision", state.colliderHasCollision},
        {"collider_burning", state.colliderBurning},
        {"collider_burnable", state.colliderBurnable},
        {"crystal_initialized", state.crystalInitialised},
        {"crystal_direction", state.crystalDirection},
        {"crystal_electricity_nodes", state.crystalNumElectricityNodes},
        {"crystal_turn_reverse", state.crystalTurnReverse},
        {"crystal_spell", state.crystalSpellToActivate},
        {"crystal_circuit_status", state.crystalCircuitStatus},
        {"crystal_power_to_activate", state.crystalPowerToActivate},
        {"boulder_trap_behavior", state.boulderTrapBehavior},
        {"boulder_trap_fired", state.boulderTrapFired},
        {"boulder_trap_refire_amount", state.boulderTrapRefireAmount},
        {"boulder_trap_refire_counter", state.boulderTrapRefireCounter},
        {"boulder_trap_pre_delay", state.boulderTrapPreDelay},
        {"boulder_trap_circuit_status", state.boulderTrapCircuitStatus},
        {"boulder_trap_sabotaged", state.boulderTrapSabotaged},
        {"signal_is_and", state.signalControllerIsAND},
        {"signal_switch_power", state.signalSwitchPower},
        {"signal_delay_count", state.signalDelayCount},
        {"signal_timer_count", state.signalTimerCount},
        {"signal_repeat_count", state.signalRepeatCount},
        {"signal_latch_input", state.signalLatchInput},
        {"signal_and_power_mask", state.signalANDPowerMask},
        {"signal_circuit_status", state.signalCircuitStatus},
        {"signal_initialized", state.signalInitialized},
        {"bell_active_timer", state.bellActiveTimer},
        {"bell_has_item", state.bellHasItem},
        {"bell_uses", state.bellUses},
        {"bell_current_event", state.bellCurrentEvent},
        {"bell_use_delay", state.bellUseDelay},
        {"bell_clapper_broken", state.bellClapperBroken},
        {"bell_bulb_broken", state.bellBulbBroken},
        {"bell_buff_type", state.bellBuffType},
        {"bell_burning_timer", state.bellBurningTimer},
        {"bell_scrap_created", state.bellScrapCreated},
        {"bell_burning", state.bellBurning},
        {"bell_burnable", state.bellBurnable},
        {"bell_invisible", state.bellInvisible},
        {"water_is_fountain", state.waterSourceIsFountain},
        {"water_uses", state.waterSourceUses},
        {"water_main_effect", state.waterSourceMainEffect},
        {"water_secondary_effect", state.waterSourceSecondaryEffect},
        {"campfire_health", state.campfireHealth},
        {"wall_lock_state", state.wallLockSavedState},
        {"wall_lock_power", state.wallLockSavedPower},
        {"wall_lock_pick_health", state.wallLockSavedPickHealth},
        {"wall_lock_prevent_exploit", state.wallLockSavedPreventExploit},
        {"wall_button_state", state.wallButtonState},
        {"wall_button_power", state.wallButtonPower},
        {"pressure_plate_permanent", state.pressurePlatePermanent},
        {"pressure_plate_power", state.pressurePlatePower},
        {"pressure_plate_interaction_lock", state.pressurePlateInteractionLock},
        {"pedestal_has_orb", state.pedestalSavedHasOrb},
        {"pedestal_power_status", state.pedestalSavedPowerStatus},
        {"pedestal_init", state.pedestalSavedInit},
        {"pedestal_in_ground", state.pedestalSavedInGround},
        {"pedestal_z", state.pedestalSavedZ},
        {"pedestal_velocity_z", state.pedestalSavedVelZ},
        {"pedestal_passable", state.pedestalSavedPassable},
        {"chest_health", state.chestSavedHealth},
        {"chest_max_health", state.chestSavedMaxHealth},
        {"chest_locked", state.chestSavedLocked},
        {"chest_lockpick_health", state.chestSavedLockpickHealth},
        {"chest_prevent_exploit", state.chestSavedPreventExploit},
        {"chest_old_health", state.chestSavedOldHealth},
        {"chest_void_state", state.chestSavedVoidState},
        {"shop_store_type", state.shopkeeperSavedStoreType},
        {"shop_gold", state.shopkeeperSavedGold},
        {"shop_loads_since_restock", state.shopkeeperLoadsSinceRestock},
        {"shop_name", state.shopkeeperSavedName},
        {"monster_type", state.monsterSavedType},
        {"monster_hp", state.monsterSavedHP},
        {"monster_max_hp", state.monsterSavedMAXHP},
        {"monster_mp", state.monsterSavedMP},
        {"monster_max_mp", state.monsterSavedMAXMP},
        {"monster_position", {
            state.monsterSavedX, state.monsterSavedY, state.monsterSavedZ
        }},
        {"monster_rotation", {
            state.monsterSavedYaw, state.monsterSavedPitch, state.monsterSavedRoll
        }},
        {"summon_monster", state.summonTrapMonster},
        {"summon_count", state.summonTrapCount},
        {"summon_interval", state.summonTrapInterval},
        {"summon_spawn_cycles", state.summonTrapSpawnCycles},
        {"summon_power_to_disable", state.summonTrapPowerToDisable},
        {"summon_failure_rate", state.summonTrapFailureRate},
        {"summon_fired", state.summonTrapFired},
        {"summon_initialized", state.summonTrapInitialized},
        {"summon_ticks_to_fire", state.summonTrapTicksToFire},
        {"summon_player_proximity", state.summonTrapPlayerProximity},
        {"passable", state.passable}
    };

    saved["chest_inventory"] = AutomatiaSave::Json::array();
    for (const PersistentChestItemState& item : state.chestSavedInventory)
    {
        saved["chest_inventory"].push_back(saveChestItemState(item));
    }
    saved["shop_inventory"] = AutomatiaSave::Json::array();
    for (const PersistentChestItemState& item : state.shopkeeperSavedInventory)
    {
        saved["shop_inventory"].push_back(saveChestItemState(item));
    }
    saved["world_item"] = saveWorldItemState(state.worldItemState);
    saved["gold_bag"] = saveGoldState(state.goldBagState);
    saved["monster_items"] = AutomatiaSave::Json::array();
    for (const PersistentMonsterItemState& item : state.monsterSavedItems)
    {
        saved["monster_items"].push_back(saveMonsterItemState(item));
    }
    saved["monster_effects"] = AutomatiaSave::Json::array();
    for (const PersistentMonsterEffectState& effect : state.monsterSavedEffects)
    {
        saved["monster_effects"].push_back(AutomatiaSave::Json{
            {"effect_id", effect.effectID},
            {"value", effect.effectValue},
            {"timer", effect.timer}
        });
    }
    return saved;
}

bool restoreMechanismState(
    const AutomatiaSave::Json& saved,
    Sint32& persistentId,
    PersistentMechanismState& state
)
{
    Sint32 kind = 0;
    Sint32 payloadVersion = 0;
    if (!savedJsonInt(saved, "persistent_id", persistentId)
        || persistentId == 0
        || !savedJsonInt(saved, "payload_version", payloadVersion)
        || payloadVersion != 1
        || !savedJsonInt(saved, "kind", kind)
        || kind <= static_cast<Sint32>(PersistentMechanismState::Kind::None)
        || kind > static_cast<Sint32>(PersistentMechanismState::Kind::SummonTrap))
    {
        return false;
    }
    state.kind = static_cast<PersistentMechanismState::Kind>(kind);

    savedJsonInt(saved, "switch_power", state.switchPower);
    savedJsonReal(saved, "roll", state.roll);
    savedJsonInt(saved, "lever_status", state.leverStatus);
    savedJsonInt(saved, "lever_timer_ticks", state.leverTimerTicks);
    savedJsonInt(saved, "gate_status", state.gateStatus);
    savedJsonInt(saved, "gate_init", state.gateInit);
    savedJsonInt(saved, "gate_rattle", state.gateRattle);
    savedJsonInt(saved, "gate_inverted", state.gateInverted);
    savedJsonInt(saved, "circuit_status", state.circuitStatus);
    savedJsonReal(saved, "gate_start_height", state.gateStartHeight);
    savedJsonReal(saved, "gate_z", state.gateZ);
    savedJsonReal(saved, "gate_velocity_z", state.gateVelZ);
    savedJsonBool(saved, "door_is_iron", state.doorIsIron);
    savedJsonInt(saved, "door_dir", state.doorDir);
    savedJsonInt(saved, "door_status", state.doorStatus);
    savedJsonInt(saved, "door_health", state.doorHealth);
    savedJsonInt(saved, "door_max_health", state.doorMaxHealth);
    savedJsonInt(saved, "door_locked", state.doorLocked);
    savedJsonInt(saved, "door_smacked", state.doorSmacked);
    savedJsonInt(saved, "door_timer", state.doorTimer);
    savedJsonInt(
        saved, "door_prevent_lockpick_exploit", state.doorPreventLockpickExploit
    );
    savedJsonInt(
        saved, "door_force_locked_unlocked", state.doorForceLockedUnlocked
    );
    savedJsonInt(saved, "door_disable_lockpicks", state.doorDisableLockpicks);
    savedJsonInt(saved, "door_disable_opening", state.doorDisableOpening);
    savedJsonInt(saved, "door_lockpick_health", state.doorLockpickHealth);
    savedJsonInt(saved, "door_unlock_when_powered", state.doorUnlockWhenPowered);
    savedJsonInt(saved, "door_circuit_status", state.doorCircuitStatus);
    savedJsonReal(saved, "door_start_angle", state.doorStartAng);
    savedJsonReal(saved, "door_yaw", state.doorYaw);
    savedJsonReal(saved, "door_x", state.doorX);
    savedJsonReal(saved, "door_y", state.doorY);
    savedJsonReal(saved, "door_focal_y", state.doorFocalY);
    savedJsonBool(saved, "door_burning", state.doorBurning);
    savedJsonBool(saved, "door_burnable", state.doorBurnable);
    savedJsonInt(saved, "furniture_type", state.furnitureType);
    savedJsonInt(saved, "furniture_health", state.furnitureHealth);
    savedJsonInt(saved, "furniture_max_health", state.furnitureMaxHealth);
    savedJsonBool(saved, "furniture_burning", state.furnitureBurning);
    savedJsonBool(saved, "furniture_burnable", state.furnitureBurnable);
    savedJsonInt(saved, "collider_health", state.colliderCurrentHP);
    savedJsonInt(saved, "collider_max_health", state.colliderMaxHP);
    savedJsonInt(saved, "collider_damage_types", state.colliderDamageTypes);
    savedJsonInt(saved, "collider_has_collision", state.colliderHasCollision);
    savedJsonBool(saved, "collider_burning", state.colliderBurning);
    savedJsonBool(saved, "collider_burnable", state.colliderBurnable);
    savedJsonInt(saved, "crystal_initialized", state.crystalInitialised);
    savedJsonInt(saved, "crystal_direction", state.crystalDirection);
    savedJsonInt(
        saved, "crystal_electricity_nodes", state.crystalNumElectricityNodes
    );
    savedJsonInt(saved, "crystal_turn_reverse", state.crystalTurnReverse);
    savedJsonInt(saved, "crystal_spell", state.crystalSpellToActivate);
    savedJsonInt(saved, "crystal_circuit_status", state.crystalCircuitStatus);
    savedJsonInt(saved, "crystal_power_to_activate", state.crystalPowerToActivate);
    savedJsonInt(saved, "boulder_trap_behavior", state.boulderTrapBehavior);
    savedJsonInt(saved, "boulder_trap_fired", state.boulderTrapFired);
    savedJsonInt(
        saved, "boulder_trap_refire_amount", state.boulderTrapRefireAmount
    );
    savedJsonInt(
        saved, "boulder_trap_refire_counter", state.boulderTrapRefireCounter
    );
    savedJsonInt(saved, "boulder_trap_pre_delay", state.boulderTrapPreDelay);
    savedJsonInt(
        saved, "boulder_trap_circuit_status", state.boulderTrapCircuitStatus
    );
    savedJsonInt(saved, "boulder_trap_sabotaged", state.boulderTrapSabotaged);
    savedJsonBool(saved, "signal_is_and", state.signalControllerIsAND);
    savedJsonInt(saved, "signal_switch_power", state.signalSwitchPower);
    savedJsonInt(saved, "signal_delay_count", state.signalDelayCount);
    savedJsonInt(saved, "signal_timer_count", state.signalTimerCount);
    savedJsonInt(saved, "signal_repeat_count", state.signalRepeatCount);
    savedJsonInt(saved, "signal_latch_input", state.signalLatchInput);
    savedJsonInt(saved, "signal_and_power_mask", state.signalANDPowerMask);
    savedJsonInt(saved, "signal_circuit_status", state.signalCircuitStatus);
    savedJsonInt(saved, "signal_initialized", state.signalInitialized);
    savedJsonInt(saved, "bell_active_timer", state.bellActiveTimer);
    savedJsonInt(saved, "bell_has_item", state.bellHasItem);
    savedJsonInt(saved, "bell_uses", state.bellUses);
    savedJsonInt(saved, "bell_current_event", state.bellCurrentEvent);
    savedJsonInt(saved, "bell_use_delay", state.bellUseDelay);
    savedJsonInt(saved, "bell_clapper_broken", state.bellClapperBroken);
    savedJsonInt(saved, "bell_bulb_broken", state.bellBulbBroken);
    savedJsonInt(saved, "bell_buff_type", state.bellBuffType);
    savedJsonInt(saved, "bell_burning_timer", state.bellBurningTimer);
    savedJsonInt(saved, "bell_scrap_created", state.bellScrapCreated);
    savedJsonBool(saved, "bell_burning", state.bellBurning);
    savedJsonBool(saved, "bell_burnable", state.bellBurnable);
    savedJsonBool(saved, "bell_invisible", state.bellInvisible);
    savedJsonBool(saved, "water_is_fountain", state.waterSourceIsFountain);
    savedJsonInt(saved, "water_uses", state.waterSourceUses);
    savedJsonInt(saved, "water_main_effect", state.waterSourceMainEffect);
    savedJsonInt(saved, "water_secondary_effect", state.waterSourceSecondaryEffect);
    savedJsonInt(saved, "campfire_health", state.campfireHealth);
    savedJsonInt(saved, "wall_lock_state", state.wallLockSavedState);
    savedJsonInt(saved, "wall_lock_power", state.wallLockSavedPower);
    savedJsonInt(saved, "wall_lock_pick_health", state.wallLockSavedPickHealth);
    savedJsonInt(
        saved, "wall_lock_prevent_exploit", state.wallLockSavedPreventExploit
    );
    savedJsonInt(saved, "wall_button_state", state.wallButtonState);
    savedJsonInt(saved, "wall_button_power", state.wallButtonPower);
    savedJsonBool(saved, "pressure_plate_permanent", state.pressurePlatePermanent);
    savedJsonInt(saved, "pressure_plate_power", state.pressurePlatePower);
    savedJsonInt(
        saved,
        "pressure_plate_interaction_lock",
        state.pressurePlateInteractionLock
    );
    savedJsonInt(saved, "pedestal_has_orb", state.pedestalSavedHasOrb);
    savedJsonInt(saved, "pedestal_power_status", state.pedestalSavedPowerStatus);
    savedJsonInt(saved, "pedestal_init", state.pedestalSavedInit);
    savedJsonInt(saved, "pedestal_in_ground", state.pedestalSavedInGround);
    savedJsonReal(saved, "pedestal_z", state.pedestalSavedZ);
    savedJsonReal(saved, "pedestal_velocity_z", state.pedestalSavedVelZ);
    savedJsonBool(saved, "pedestal_passable", state.pedestalSavedPassable);
    savedJsonInt(saved, "chest_health", state.chestSavedHealth);
    savedJsonInt(saved, "chest_max_health", state.chestSavedMaxHealth);
    savedJsonInt(saved, "chest_locked", state.chestSavedLocked);
    savedJsonInt(saved, "chest_lockpick_health", state.chestSavedLockpickHealth);
    savedJsonInt(saved, "chest_prevent_exploit", state.chestSavedPreventExploit);
    savedJsonInt(saved, "chest_old_health", state.chestSavedOldHealth);
    savedJsonInt(saved, "chest_void_state", state.chestSavedVoidState);
    savedJsonInt(saved, "shop_store_type", state.shopkeeperSavedStoreType);
    savedJsonInt(saved, "shop_gold", state.shopkeeperSavedGold);
    savedJsonInt(
        saved, "shop_loads_since_restock", state.shopkeeperLoadsSinceRestock
    );
    if (saved.contains("shop_name") && saved["shop_name"].is_string()
        && saved["shop_name"].get<std::string>().size() < 128)
    {
        state.shopkeeperSavedName = saved["shop_name"].get<std::string>();
    }
    savedJsonInt(saved, "monster_type", state.monsterSavedType);
    savedJsonInt(saved, "monster_hp", state.monsterSavedHP);
    savedJsonInt(saved, "monster_max_hp", state.monsterSavedMAXHP);
    savedJsonInt(saved, "monster_mp", state.monsterSavedMP);
    savedJsonInt(saved, "monster_max_mp", state.monsterSavedMAXMP);
    savedJsonTriplet(
        saved,
        "monster_position",
        state.monsterSavedX,
        state.monsterSavedY,
        state.monsterSavedZ
    );
    savedJsonTriplet(
        saved,
        "monster_rotation",
        state.monsterSavedYaw,
        state.monsterSavedPitch,
        state.monsterSavedRoll
    );
    savedJsonInt(saved, "summon_monster", state.summonTrapMonster);
    savedJsonInt(saved, "summon_count", state.summonTrapCount);
    savedJsonInt(saved, "summon_interval", state.summonTrapInterval);
    savedJsonInt(saved, "summon_spawn_cycles", state.summonTrapSpawnCycles);
    savedJsonInt(
        saved, "summon_power_to_disable", state.summonTrapPowerToDisable
    );
    savedJsonInt(saved, "summon_failure_rate", state.summonTrapFailureRate);
    savedJsonInt(saved, "summon_fired", state.summonTrapFired);
    savedJsonInt(saved, "summon_initialized", state.summonTrapInitialized);
    savedJsonInt(saved, "summon_ticks_to_fire", state.summonTrapTicksToFire);
    savedJsonInt(
        saved, "summon_player_proximity", state.summonTrapPlayerProximity
    );
    savedJsonBool(saved, "passable", state.passable);

    auto restoreChestItems = [&](const char* key, auto& destination)
    {
        if (!saved.contains(key) || !saved[key].is_array()
            || saved[key].size() > 65536)
        {
            return;
        }
        for (const auto& savedItem : saved[key])
        {
            PersistentChestItemState item;
            if (restoreChestItemState(savedItem, item))
            {
                destination.push_back(std::move(item));
            }
        }
    };
    restoreChestItems("chest_inventory", state.chestSavedInventory);
    restoreChestItems("shop_inventory", state.shopkeeperSavedInventory);

    if (state.kind == PersistentMechanismState::Kind::Chest
        && (!saved.contains("chest_inventory")
            || !saved["chest_inventory"].is_array()))
    {
        return false;
    }
    if (state.kind == PersistentMechanismState::Kind::ShopkeeperInventory
        && (!saved.contains("shop_inventory")
            || !saved["shop_inventory"].is_array()))
    {
        return false;
    }

    if (state.kind == PersistentMechanismState::Kind::WorldItem)
    {
        if (!saved.contains("world_item")
            || !restoreWorldItemState(saved["world_item"], state.worldItemState))
        {
            return false;
        }
    }
    if (state.kind == PersistentMechanismState::Kind::GoldBag)
    {
        if (!saved.contains("gold_bag")
            || !restoreGoldState(saved["gold_bag"], state.goldBagState))
        {
            return false;
        }
    }
    if (saved.contains("monster_items") && saved["monster_items"].is_array()
        && saved["monster_items"].size() <= 65536)
    {
        for (const auto& savedItem : saved["monster_items"])
        {
            PersistentMonsterItemState item;
            if (restoreMonsterItemState(savedItem, item))
            {
                state.monsterSavedItems.push_back(std::move(item));
            }
        }
    }
    if (saved.contains("monster_effects") && saved["monster_effects"].is_array()
        && saved["monster_effects"].size() <= NUMEFFECTS)
    {
        for (const auto& savedEffect : saved["monster_effects"])
        {
            PersistentMonsterEffectState effect;
            Sint32 effectValue = 0;
            if (savedJsonInt(savedEffect, "effect_id", effect.effectID)
                && effect.effectID >= 0 && effect.effectID < NUMEFFECTS
                && savedJsonInt(savedEffect, "value", effectValue)
                && effectValue >= 0
                && effectValue <= std::numeric_limits<Uint8>::max()
                && savedJsonInt(savedEffect, "timer", effect.timer)
                && effect.timer > 0)
            {
                effect.effectValue = static_cast<Uint8>(effectValue);
                state.monsterSavedEffects.push_back(effect);
            }
        }
    }
    if (state.kind == PersistentMechanismState::Kind::MonsterLivingState
        && (!saved.contains("monster_items")
            || !saved["monster_items"].is_array()
            || !saved.contains("monster_effects")
            || !saved["monster_effects"].is_array()))
    {
        return false;
    }
    return true;
}

void restoreSavedStringSet(
    const AutomatiaSave::Json& object,
    const char* key,
    std::unordered_set<std::string>& destination
)
{
    if (!object.is_object() || !object.contains(key) || !object[key].is_array()
        || object[key].size() > 65536)
    {
        return;
    }
    for (const auto& member : object[key])
    {
        if (member.is_string())
        {
            const std::string value = member.get<std::string>();
            if (safeSavedStoryId(value))
            {
                destination.insert(value);
            }
        }
    }
}

void restoreSavedVariables(
    const AutomatiaSave::Json& object,
    const char* key,
    std::unordered_map<std::string, Sint32>& destination
)
{
    if (!object.is_object() || !object.contains(key) || !object[key].is_object()
        || object[key].size() > 65536)
    {
        return;
    }
    for (auto member = object[key].begin(); member != object[key].end(); ++member)
    {
        Sint32 value = 0;
        const AutomatiaSave::Json wrapper = {{"value", member.value()}};
        if (safeSavedStoryId(member.key()) && savedJsonInt(wrapper, "value", value))
        {
            destination[member.key()] = value;
        }
    }
}

void hydratePreservedAutomatiaWorldDocument()
{
    using AutomatiaSave::Json;
    if (!preservedAutomatiaWorldDocument.is_object())
    {
        return;
    }

    auto restoreLevelVisit = [](
        const Json& savedVisit,
        AutomatiaPlayerLevelVisit& visit
    )
    {
        if (!savedVisit.is_object()
            || !savedVisit.contains("dungeon_level")
            || !savedVisit["dungeon_level"].is_number_integer()
            || !savedVisit.contains("secret_level")
            || !savedVisit["secret_level"].is_boolean()
            || !savedVisit.contains("map_seed")
            || !savedVisit["map_seed"].is_number_integer())
        {
            return false;
        }
        const std::int64_t savedLevel =
            savedVisit["dungeon_level"].get<std::int64_t>();
        const std::int64_t savedSeed =
            savedVisit["map_seed"].get<std::int64_t>();
        if (savedLevel < 0 || savedLevel > INT32_MAX
            || savedSeed < 0 || savedSeed > UINT32_MAX)
        {
            return false;
        }
        visit = AutomatiaPlayerLevelVisit{};
        visit.dungeonLevel = static_cast<Sint32>(savedLevel);
        visit.secretLevel = savedVisit["secret_level"].get<bool>();
        visit.mapSeed = static_cast<Uint32>(savedSeed);
        if (savedVisit.contains("map_instance")
            && savedVisit["map_instance"].is_string())
        {
            const std::string key =
                savedVisit["map_instance"].get<std::string>();
            WorldInstanceIdentity identity;
            if (key.size() <= 512
                && parseAutomatiaMapInstanceKey(key, identity))
            {
                visit.mapInstanceKey = identity.key();
            }
        }
        auto restoreFinite = [](
            const Json& object,
            const char* key,
            double& output
        )
        {
            if (!object.is_object() || !object.contains(key)
                || !object[key].is_number())
            {
                return false;
            }
            const double value = object[key].get<double>();
            if (!std::isfinite(value) || std::abs(value) > 1000000000.0)
            {
                return false;
            }
            output = value;
            return true;
        };
        if (savedVisit.contains("return_anchor")
            && savedVisit["return_anchor"].is_object())
        {
            const Json& anchor = savedVisit["return_anchor"];
            if (restoreFinite(anchor, "x", visit.returnAnchorX)
                && restoreFinite(anchor, "y", visit.returnAnchorY)
                && restoreFinite(anchor, "z", visit.returnAnchorZ))
            {
                visit.hasReturnAnchor = true;
                if (anchor.contains("persistent_id")
                    && anchor["persistent_id"].is_number_integer())
                {
                    const std::int64_t persistentID =
                        anchor["persistent_id"].get<std::int64_t>();
                    if (persistentID >= 0 && persistentID <= INT32_MAX)
                    {
                        visit.returnAnchorPersistentID =
                            static_cast<Sint32>(persistentID);
                    }
                }
            }
        }
        if (savedVisit.contains("return_position")
            && savedVisit["return_position"].is_object())
        {
            const Json& position = savedVisit["return_position"];
            AutomatiaPlayerReturnPlacement placement;
            if (restoreFinite(position, "x", placement.x)
                && restoreFinite(position, "y", placement.y)
                && restoreFinite(position, "z", placement.z)
                && restoreFinite(position, "yaw", placement.yaw))
            {
                restoreFinite(position, "pitch", placement.pitch);
                restoreFinite(position, "roll", placement.roll);
                placement.valid = true;
                visit.returnPlacement = placement;
            }
        }
        return !visit.mapInstanceKey.empty();
    };

    std::size_t restoredPlayerHistoryEntries = 0;
    if (preservedAutomatiaWorldDocument.contains("player_level_histories")
        && preservedAutomatiaWorldDocument["player_level_histories"].is_object()
        && preservedAutomatiaWorldDocument["player_level_histories"].size()
            <= 4096)
    {
        const Json& savedHistories =
            preservedAutomatiaWorldDocument["player_level_histories"];
        for (auto savedPlayer = savedHistories.begin();
            savedPlayer != savedHistories.end(); ++savedPlayer)
        {
            if (savedPlayer.key().empty() || savedPlayer.key().size() > 256
                || !savedPlayer.value().is_array()
                || savedPlayer.value().size()
                    > AUTOMATIA_PLAYER_LEVEL_HISTORY_LIMIT)
            {
                continue;
            }
            AutomatiaPlayerLevelHistory history;
            history.reserve(savedPlayer.value().size());
            for (const Json& savedVisit : savedPlayer.value())
            {
                AutomatiaPlayerLevelVisit visit;
                if (restoreLevelVisit(savedVisit, visit))
                {
                    history.push_back(std::move(visit));
                }
            }
            if (!history.empty())
            {
                restoredPlayerHistoryEntries += history.size();
                automatiaPlayerLevelHistories[savedPlayer.key()] =
                    std::move(history);
            }
        }
    }

    /*
     * Compatibility with saves written before divergent route histories.
     * Every player begins with a copy of this formerly shared route when their
     * stable identity first requests a history stack.
     */
    if (preservedAutomatiaWorldDocument.contains("party_level_history")
        && preservedAutomatiaWorldDocument["party_level_history"].is_array()
        && preservedAutomatiaWorldDocument["party_level_history"].size()
            <= AUTOMATIA_PLAYER_LEVEL_HISTORY_LIMIT)
    {
        for (const Json& savedVisit
            : preservedAutomatiaWorldDocument["party_level_history"])
        {
            AutomatiaPlayerLevelVisit visit;
            if (restoreLevelVisit(savedVisit, visit))
            {
                automatiaLegacyPartyLevelHistory.push_back(std::move(visit));
            }
        }
    }
    std::size_t migratedLegacyPlayers = 0;
    const std::size_t restoredLegacyEntries =
        automatiaLegacyPartyLevelHistory.size();
    if (!automatiaLegacyPartyLevelHistory.empty())
    {
        /*
         * Convert the old shared stack only for players already connected
         * while this save is hydrated. A later late-joiner must not inherit
         * another character's pre-divergence route merely because the save
         * originated before player-scoped histories existed.
         */
        for (int playerIndex = 0; playerIndex < MAXPLAYERS; ++playerIndex)
        {
            if (!players[playerIndex] || client_disconnected[playerIndex]
                || (headless && multiplayer == SERVER && playerIndex == 0))
            {
                continue;
            }
            std::string identity =
                persistentMinimapPlayerIdentity(playerIndex);
            if (identity.empty())
            {
                identity = "slot:" + std::to_string(playerIndex);
            }
            auto inserted = automatiaPlayerLevelHistories.emplace(
                identity,
                AutomatiaPlayerLevelHistory{}
            );
            if (inserted.second || inserted.first->second.empty())
            {
                inserted.first->second =
                    automatiaLegacyPartyLevelHistory;
                ++migratedLegacyPlayers;
            }
        }
        automatiaLegacyPartyLevelHistory.clear();
    }
    if (restoredPlayerHistoryEntries > 0 || restoredLegacyEntries > 0)
    {
        printlog(
            "[Ladder Reverse] Restored %zu per-player history entry(s); migrated %zu legacy shared entry(s) to %zu currently connected player(s).",
            restoredPlayerHistoryEntries,
            restoredLegacyEntries,
            migratedLegacyPlayers
        );
    }

    std::size_t restoredMaps = 0;
    std::size_t restoredTiles = 0;
    std::size_t restoredItems = 0;
    if (preservedAutomatiaWorldDocument.contains("map_instances")
        && preservedAutomatiaWorldDocument["map_instances"].is_array())
    {
        for (const Json& savedMap : preservedAutomatiaWorldDocument["map_instances"])
        {
            if (!savedMap.is_object() || !savedMap.contains("map_file")
                || !savedMap["map_file"].is_string()
                || !savedMap.contains("instance_id")
                || !savedMap["instance_id"].is_string()
                || !savedMap.contains("persistent_state")
                || !savedMap["persistent_state"].is_object())
            {
                continue;
            }
            WorldInstanceIdentity identity;
            if (!identity.set(
                    savedMap["map_file"].get<std::string>(),
                    savedMap["instance_id"].get<std::string>()
                ))
            {
                continue;
            }
            MapInstanceSummary restoredSummary;
            restoredSummary.identity = identity;
            auto restoreUnsigned = [&savedMap](
                const char* key,
                std::uint64_t maximum,
                std::uint64_t fallback
            )
            {
                if (!savedMap.contains(key)
                    || !savedMap[key].is_number_integer())
                {
                    return fallback;
                }
                const std::int64_t value = savedMap[key].get<std::int64_t>();
                return value < 0 || static_cast<std::uint64_t>(value) > maximum
                    ? fallback
                    : static_cast<std::uint64_t>(value);
            };
            restoredSummary.identity.revision = restoreUnsigned(
                "revision",
                std::numeric_limits<std::uint64_t>::max(),
                identity.revision
            );
            restoredSummary.dungeonLevel = static_cast<std::int32_t>(
                restoreUnsigned("dungeon_level", INT32_MAX, 0)
            );
            restoredSummary.mapSeed = static_cast<std::uint32_t>(
                restoreUnsigned("map_seed", UINT32_MAX, 0)
            );
            restoredSummary.nextEntityUid = static_cast<std::uint32_t>(
                restoreUnsigned("next_entity_uid", UINT32_MAX, 1)
            );
            restoredSummary.nextPersistentId = restoreUnsigned(
                "next_persistent_id",
                std::numeric_limits<std::uint64_t>::max(),
                1
            );
            restoredSummary.simulationTick = restoreUnsigned(
                "simulation_tick",
                std::numeric_limits<std::uint64_t>::max(),
                0
            );
            restoredSummary.dirty =
                savedMap.value("dirty", false);
            restoredSummary.secretLevel =
                savedMap.value("secret_level", false);
            restoredSummary.darkMap =
                savedMap.value("dark_map", false);
            worldState.registerUnloadedInstance(restoredSummary);
            const Json& persistent = savedMap["persistent_state"];
            PersistentMapRemovalState& state =
                persistentMapRemovalRegistry[identity.key()];

            if (persistent.contains("removed_entity_ids")
                && persistent["removed_entity_ids"].is_array()
                && persistent["removed_entity_ids"].size() <= 1048576)
            {
                for (const Json& member : persistent["removed_entity_ids"])
                {
                    const Json wrapper = {{"value", member}};
                    Sint32 id = 0;
                    if (savedJsonInt(wrapper, "value", id) && id != 0)
                    {
                        state.removedEntityIDs.insert(id);
                    }
                }
            }
            if (persistent.contains("dynamic_monster_ids")
                && persistent["dynamic_monster_ids"].is_array()
                && persistent["dynamic_monster_ids"].size() <= 1048576)
            {
                for (const Json& member : persistent["dynamic_monster_ids"])
                {
                    const Json wrapper = {{"value", member}};
                    Sint32 id = 0;
                    if (savedJsonInt(wrapper, "value", id) && id < 0)
                    {
                        state.dynamicMonsterIDs.insert(id);
                        if (id > std::numeric_limits<Sint32>::min())
                        {
                            nextDynamicMonsterPersistentID = std::min(
                                nextDynamicMonsterPersistentID,
                                static_cast<Sint32>(id - 1)
                            );
                        }
                    }
                }
            }
            if (persistent.contains("tile_states")
                && persistent["tile_states"].is_array()
                && persistent["tile_states"].size() <= 1048576)
            {
                for (const Json& savedTile : persistent["tile_states"])
                {
                    PersistentTileState tile;
                    if (!savedJsonInt(savedTile, "x", tile.x)
                        || !savedJsonInt(savedTile, "y", tile.y)
                        || !savedJsonInt(savedTile, "layer", tile.layer)
                        || !savedJsonInt(savedTile, "tile", tile.tile)
                        || tile.x < 0 || tile.y < 0
                        || tile.x > std::numeric_limits<Uint16>::max()
                        || tile.y > std::numeric_limits<Uint16>::max()
                        || tile.layer < 0 || tile.layer >= MAPLAYERS)
                    {
                        continue;
                    }
                    state.tileStates[makePersistentTileKey(
                        tile.x, tile.y, tile.layer
                    )] = tile;
                    ++restoredTiles;
                }
            }
            if (persistent.contains("dynamic_world_items")
                && persistent["dynamic_world_items"].is_array()
                && persistent["dynamic_world_items"].size() <= 65536)
            {
                for (const Json& savedItem : persistent["dynamic_world_items"])
                {
                    PersistentWorldItemState item;
                    if (!savedJsonInt(savedItem, "legacy_type", item.type)
                        || !savedJsonInt(savedItem, "status", item.status)
                        || !savedJsonInt(savedItem, "beatitude", item.beatitude)
                        || !savedJsonInt(savedItem, "count", item.count)
                        || !savedJsonUint(savedItem, "appearance", item.appearance)
                        || item.count <= 0
                        || !savedJsonTriplet(savedItem, "position", item.x, item.y, item.z)
                        || !savedJsonTriplet(
                            savedItem, "rotation", item.yaw, item.pitch, item.roll
                        )
                        || !savedJsonTriplet(
                            savedItem, "velocity", item.velX, item.velY, item.velZ
                        ))
                    {
                        continue;
                    }
                    if (savedItem.contains("stable_id")
                        && savedItem["stable_id"].is_string()
                        && savedItem["stable_id"].get<std::string>().size() <= 255)
                    {
                        item.stableId = savedItem["stable_id"].get<std::string>();
                    }
                    savedJsonBool(savedItem, "identified", item.identified);
                    savedJsonInt(savedItem, "not_moving", item.itemNotMoving);
                    savedJsonInt(savedItem, "sokoban_reward", item.itemSokobanReward);
                    savedJsonInt(savedItem, "stolen", item.itemStolen);
                    savedJsonInt(savedItem, "show_on_map", item.itemShowOnMap);
                    savedJsonInt(savedItem, "pickup_delay", item.itemDelayMonsterPickingUp);
                    savedJsonBool(savedItem, "passable", item.passable);
                    savedJsonBool(savedItem, "invisible", item.invisible);
                    savedJsonBool(savedItem, "burning", item.burning);
                    savedJsonBool(savedItem, "burnable", item.burnable);
                    state.dynamicWorldItems.push_back(std::move(item));
                    ++restoredItems;
                }
            }
            if (persistent.contains("dynamic_gold_bags")
                && persistent["dynamic_gold_bags"].is_array()
                && persistent["dynamic_gold_bags"].size() <= 65536)
            {
                for (const Json& savedGold : persistent["dynamic_gold_bags"])
                {
                    PersistentGoldBagState gold;
                    if (!savedJsonInt(savedGold, "amount", gold.amount)
                        || !savedJsonInt(savedGold, "bonus", gold.amountBonus)
                        || gold.amount <= 0
                        || !savedJsonTriplet(savedGold, "position", gold.x, gold.y, gold.z)
                        || !savedJsonTriplet(
                            savedGold, "rotation", gold.yaw, gold.pitch, gold.roll
                        )
                        || !savedJsonTriplet(
                            savedGold, "velocity", gold.velX, gold.velY, gold.velZ
                        ))
                    {
                        continue;
                    }
                    savedJsonInt(savedGold, "sokoban", gold.sokoban);
                    savedJsonInt(savedGold, "bouncing", gold.bouncing);
                    savedJsonInt(savedGold, "dropped_by_player", gold.droppedByPlayer);
                    savedJsonBool(savedGold, "passable", gold.passable);
                    savedJsonBool(savedGold, "invisible", gold.invisible);
                    state.dynamicGoldBags.push_back(std::move(gold));
                }
            }
            if (persistent.contains("dynamic_boulders")
                && persistent["dynamic_boulders"].is_array()
                && persistent["dynamic_boulders"].size() <= 65536)
            {
                for (const Json& savedBoulder : persistent["dynamic_boulders"])
                {
                    PersistentBoulderState boulder;
                    if (!savedJsonInt(
                            savedBoulder,
                            "source_trap_id",
                            boulder.sourceTrapPersistentID
                        )
                        || !savedJsonInt(savedBoulder, "sprite", boulder.sprite)
                        || !savedJsonTriplet(
                            savedBoulder, "position", boulder.x, boulder.y, boulder.z
                        )
                        || !savedJsonTriplet(
                            savedBoulder,
                            "rotation",
                            boulder.yaw,
                            boulder.pitch,
                            boulder.roll
                        )
                        || !savedJsonTriplet(
                            savedBoulder,
                            "velocity",
                            boulder.velX,
                            boulder.velY,
                            boulder.velZ
                        ))
                    {
                        continue;
                    }
                    savedJsonInt(savedBoulder, "stopped", boulder.stopped);
                    savedJsonInt(savedBoulder, "no_ground", boulder.noGround);
                    savedJsonInt(savedBoulder, "rolling", boulder.rolling);
                    savedJsonInt(
                        savedBoulder, "roll_direction", boulder.rollDirection
                    );
                    savedJsonInt(
                        savedBoulder, "destination_x", boulder.destinationX
                    );
                    savedJsonInt(
                        savedBoulder, "destination_y", boulder.destinationY
                    );
                    savedJsonInt(savedBoulder, "initialized", boulder.initialized);
                    savedJsonInt(
                        savedBoulder,
                        "lava_explode_timer",
                        boulder.lavaExplodeTimer
                    );
                    savedJsonBool(savedBoulder, "passable", boulder.passable);
                    state.dynamicBoulders.push_back(std::move(boulder));
                }
            }
            if (persistent.contains("mechanisms")
                && persistent["mechanisms"].is_array()
                && persistent["mechanisms"].size() <= 1048576)
            {
                for (const Json& savedMechanism : persistent["mechanisms"])
                {
                    Sint32 persistentId = 0;
                    PersistentMechanismState mechanism;
                    if (restoreMechanismState(
                            savedMechanism,
                            persistentId,
                            mechanism
                        ))
                    {
                        state.mechanismStates[persistentId] =
                            std::move(mechanism);
                    }
                }
            }
            if (persistent.contains("minimap") && persistent["minimap"].is_array()
                && persistent["minimap"].size()
                    <= MINIMAP_MAX_DIMENSION * MINIMAP_MAX_DIMENSION)
            {
                Sint32 savedWidth = 0;
                Sint32 savedHeight = 0;
                savedJsonInt(savedMap, "width", savedWidth);
                savedJsonInt(savedMap, "height", savedHeight);
                const std::size_t expected =
                    savedWidth > 0 && savedHeight > 0
                    ? static_cast<std::size_t>(savedWidth)
                        * static_cast<std::size_t>(savedHeight)
                    : 0;
                PersistentMinimapState legacy;
                legacy.width = savedWidth;
                legacy.height = savedHeight;
                legacy.tiles.reserve(persistent["minimap"].size());
                bool valid = expected == persistent["minimap"].size()
                    && savedWidth <= MINIMAP_MAX_DIMENSION
                    && savedHeight <= MINIMAP_MAX_DIMENSION;
                for (const Json& member : persistent["minimap"])
                {
                    const Json wrapper = {{"value", member}};
                    Sint32 value = 0;
                    if (!valid || !savedJsonInt(wrapper, "value", value)
                        || value < 0 || value > 4)
                    {
                        valid = false;
                        break;
                    }
                    legacy.tiles.push_back(static_cast<Sint8>(value));
                }
                if (valid)
                {
                    persistentLegacyMinimapRegistry[identity.key()] =
                        std::move(legacy);
                }
            }
            if (persistent.contains("player_minimaps")
                && persistent["player_minimaps"].is_object()
                && persistent["player_minimaps"].size() <= 4096)
            {
                const Json& savedPlayers = persistent["player_minimaps"];
                for (auto player = savedPlayers.begin();
                    player != savedPlayers.end(); ++player)
                {
                    if (player.key().empty() || player.key().size() > 256
                        || !player.value().is_object())
                    {
                        continue;
                    }
                    Sint32 width = 0;
                    Sint32 height = 0;
                    if (!savedJsonInt(player.value(), "width", width)
                        || !savedJsonInt(player.value(), "height", height)
                        || width <= 0 || height <= 0
                        || width > MINIMAP_MAX_DIMENSION
                        || height > MINIMAP_MAX_DIMENSION
                        || !player.value().contains("runs")
                        || !player.value()["runs"].is_array()
                        || player.value()["runs"].size()
                            > static_cast<std::size_t>(width)
                                * static_cast<std::size_t>(height))
                    {
                        continue;
                    }
                    const std::size_t tileCount =
                        static_cast<std::size_t>(width)
                        * static_cast<std::size_t>(height);
                    PersistentMinimapState minimapState;
                    minimapState.width = width;
                    minimapState.height = height;
                    minimapState.tiles.assign(tileCount, 0);
                    bool valid = true;
                    std::size_t previousRunEnd = 0;
                    for (const Json& run : player.value()["runs"])
                    {
                        if (!run.is_array() || run.size() != 3)
                        {
                            valid = false;
                            break;
                        }
                        const Json startWrapper = {{"value", run[0]}};
                        const Json lengthWrapper = {{"value", run[1]}};
                        const Json valueWrapper = {{"value", run[2]}};
                        Sint32 startIndex = 0;
                        Sint32 length = 0;
                        Sint32 value = 0;
                        if (!savedJsonInt(startWrapper, "value", startIndex)
                            || !savedJsonInt(lengthWrapper, "value", length)
                            || !savedJsonInt(valueWrapper, "value", value)
                            || startIndex < 0 || length <= 0
                            || value <= 0 || value > 4
                            || static_cast<std::size_t>(startIndex) >= tileCount
                            || static_cast<std::size_t>(startIndex)
                                < previousRunEnd
                            || static_cast<std::size_t>(length)
                                > tileCount
                                    - static_cast<std::size_t>(startIndex))
                        {
                            valid = false;
                            break;
                        }
                        std::fill(
                            minimapState.tiles.begin() + startIndex,
                            minimapState.tiles.begin() + startIndex + length,
                            static_cast<Sint8>(value));
                        previousRunEnd =
                            static_cast<std::size_t>(startIndex)
                            + static_cast<std::size_t>(length);
                    }
                    if (valid)
                    {
                        persistentPlayerMinimapRegistry[player.key()]
                            [identity.key()] = std::move(minimapState);
                    }
                }
            }
            ++restoredMaps;
        }
    }

    restoreSavedVariables(
        preservedAutomatiaWorldDocument,
        "world_variables",
        persistentWorldStoryState.worldVariables
    );
    restoreSavedStringSet(
        preservedAutomatiaWorldDocument,
        "world_flags",
        persistentWorldStoryState.worldFlags
    );
    if (preservedAutomatiaWorldDocument.contains("quests")
        && preservedAutomatiaWorldDocument["quests"].is_object()
        && preservedAutomatiaWorldDocument["quests"].size() <= 65536)
    {
        for (auto member = preservedAutomatiaWorldDocument["quests"].begin();
            member != preservedAutomatiaWorldDocument["quests"].end(); ++member)
        {
            if (!safeSavedStoryId(member.key()) || !member.value().is_object())
            {
                continue;
            }
            PersistentQuestState quest;
            savedJsonBool(member.value(), "started", quest.started);
            savedJsonBool(member.value(), "accepted", quest.accepted);
            savedJsonBool(member.value(), "completed", quest.completed);
            savedJsonBool(member.value(), "failed", quest.failed);
            savedJsonInt(member.value(), "stage", quest.stage);
            restoreSavedVariables(member.value(), "variables", quest.variables);
            restoreSavedStringSet(member.value(), "flags", quest.flags);
            restoreSavedStringSet(
                member.value(), "completed_objectives", quest.completedObjectives
            );
            restoreSavedStringSet(member.value(), "used_choices", quest.usedChoices);
            persistentWorldStoryState.quests[member.key()] = std::move(quest);
        }
    }
    if (preservedAutomatiaWorldDocument.contains("dialogue")
        && preservedAutomatiaWorldDocument["dialogue"].is_object()
        && preservedAutomatiaWorldDocument["dialogue"].size() <= 65536)
    {
        for (auto member = preservedAutomatiaWorldDocument["dialogue"].begin();
            member != preservedAutomatiaWorldDocument["dialogue"].end(); ++member)
        {
            if (!safeSavedStoryId(member.key()) || !member.value().is_object())
            {
                continue;
            }
            PersistentNPCDialogueState dialogue;
            if (member.value().contains("dialogue_id")
                && member.value()["dialogue_id"].is_string()
                && member.value()["dialogue_id"].get<std::string>().size() <= 255)
            {
                dialogue.dialogueID =
                    member.value()["dialogue_id"].get<std::string>();
            }
            savedJsonInt(member.value(), "current_node", dialogue.currentNode);
            savedJsonBool(
                member.value(), "conversation_started", dialogue.conversationStarted
            );
            savedJsonBool(member.value(), "reward_given", dialogue.rewardGiven);
            restoreSavedVariables(member.value(), "variables", dialogue.variables);
            restoreSavedStringSet(member.value(), "flags", dialogue.flags);
            restoreSavedStringSet(member.value(), "used_choices", dialogue.usedChoices);
            restoreSavedStringSet(member.value(), "seen_nodes", dialogue.seenNodes);
            persistentWorldStoryState.npcDialogueStates[member.key()] =
                std::move(dialogue);
        }
    }
    if (characterSaveMode == CharacterSaveMode::NONE
        && preservedAutomatiaWorldDocument.contains("players")
        && preservedAutomatiaWorldDocument["players"].is_array()
        && preservedAutomatiaWorldDocument["players"].size() <= MAXPLAYERS)
    {
        for (const Json& savedPlayer : preservedAutomatiaWorldDocument["players"])
        {
            Sint32 slot = -1;
            if (!savedJsonInt(savedPlayer, "slot", slot)
                || slot < 0 || slot >= MAXPLAYERS
                || !savedPlayer.contains("map_file")
                || !savedPlayer["map_file"].is_string()
                || !savedPlayer.contains("instance_id")
                || !savedPlayer["instance_id"].is_string())
            {
                continue;
            }
			if (headless && multiplayer == SERVER && slot == 0)
			{
				continue;
			}
            AutomatiaSavedPlayerPlacement placement;
            if (!placement.identity.set(
                    savedPlayer["map_file"].get<std::string>(),
                    savedPlayer["instance_id"].get<std::string>()
                ))
            {
                continue;
            }
            if (savedPlayer.contains("revision")
                && savedPlayer["revision"].is_number_unsigned())
            {
                placement.identity.revision =
                    savedPlayer["revision"].get<std::uint64_t>();
            }
            placement.hasPosition = savedJsonTriplet(
                savedPlayer,
                "position",
                placement.x,
                placement.y,
                placement.z
            );
            savedJsonTriplet(
                savedPlayer,
                "rotation",
                placement.yaw,
                placement.pitch,
                placement.roll
            );
            placement.pending = true;
            automatiaSavedPlayerPlacements[slot] = std::move(placement);
        }
    }
    printlog(
        "[Automatia Save] Restored %zu map record(s), %zu tile override(s), %zu dynamic item(s), %zu quest(s), and %zu dialogue record(s).",
        restoredMaps,
        restoredTiles,
        restoredItems,
        persistentWorldStoryState.quests.size(),
        persistentWorldStoryState.npcDialogueStates.size()
    );
}
}

void applyAutomatiaSavedPlayerPlacements()
{
    const WorldInstanceIdentity* activeIdentity = worldState.activeIdentity();
    if (!activeIdentity)
    {
        return;
    }
    for (int playerIndex = 0; playerIndex < MAXPLAYERS; ++playerIndex)
    {
		if (headless && multiplayer == SERVER && playerIndex == 0)
		{
			continue;
		}
        AutomatiaSavedPlayerPlacement& placement =
            automatiaSavedPlayerPlacements[playerIndex];
        if (!placement.pending
            || !placement.identity.matches(*activeIdentity)
            || !players[playerIndex]
            || !players[playerIndex]->entity)
        {
            continue;
        }
        Entity& entity = *players[playerIndex]->entity;
        bool savedTilePassable = false;
        if (placement.hasPosition
            && placement.x >= 0.0
            && placement.y >= 0.0
            && placement.x < map.width * 16.0
            && placement.y < map.height * 16.0)
        {
            const Sint32 tileX = static_cast<Sint32>(placement.x / 16.0);
            const Sint32 tileY = static_cast<Sint32>(placement.y / 16.0);
            const std::size_t obstacleIndex =
                OBSTACLELAYER
                + tileY * MAPLAYERS
                + tileX * MAPLAYERS * map.height;
            savedTilePassable = map.tiles && map.tiles[obstacleIndex] == 0;
        }
        if (placement.hasPosition
            && savedTilePassable)
        {
            entity.x = placement.x;
            entity.y = placement.y;
            entity.z = placement.z;
            entity.yaw = placement.yaw;
            entity.pitch = placement.pitch;
            entity.roll = placement.roll;
            entity.new_x = entity.x;
            entity.new_y = entity.y;
            entity.new_z = entity.z;
            entity.new_yaw = entity.yaw;
            entity.new_pitch = entity.pitch;
            entity.new_roll = entity.roll;
            entity.vel_x = 0.0;
            entity.vel_y = 0.0;
            entity.vel_z = 0.0;
            entity.lerp_ox = entity.x;
            entity.lerp_oy = entity.y;
            entity.bNeedsRenderPositionInit = true;
            for (Entity* bodypart : entity.bodyparts)
            {
                if (bodypart)
                {
                    bodypart->bNeedsRenderPositionInit = true;
                }
            }
            players[playerIndex]->player_last_x = entity.x;
            players[playerIndex]->player_last_y = entity.y;
            printlog(
                "[Character Save] Server applied saved position for player %d "
                "at %.2f, %.2f, %.2f in '%s'.",
                playerIndex,
                entity.x,
                entity.y,
                entity.z,
                activeIdentity->key().c_str());
        }
        else if (placement.hasPosition)
        {
            printlog(
                "[Character Save] Saved position for player %d was blocked or "
                "out of bounds in '%s'; using the safe Player Start fallback.",
                playerIndex,
                activeIdentity->key().c_str());
        }
        worldState.placePlayer(playerIndex, map);
        placement.pending = false;
    }
}

bool automatiaHasSavedPlayerPlacement(int playerIndex)
{
    if (playerIndex < 0 || playerIndex >= MAXPLAYERS)
    {
        return false;
    }
    const AutomatiaSavedPlayerPlacement& placement =
        automatiaSavedPlayerPlacements[playerIndex];
    return placement.pending && placement.identity.isValid();
}

void consumeAutomatiaSavedPlayerPlacement(int playerIndex)
{
    if (playerIndex < 0 || playerIndex >= MAXPLAYERS)
    {
        return;
    }
    automatiaSavedPlayerPlacements[playerIndex].pending = false;
}

bool stageAutomatiaCharacterSavedPlacement(
    int playerIndex,
    const std::string& mapFile,
    const std::string& instanceId,
    Uint64 revision,
    real_t x,
    real_t y,
    real_t z,
    real_t yaw,
    real_t pitch,
    real_t roll)
{
    if (playerIndex <= 0 || playerIndex >= MAXPLAYERS
        || !std::isfinite(static_cast<double>(x))
        || !std::isfinite(static_cast<double>(y))
        || !std::isfinite(static_cast<double>(z))
        || !std::isfinite(static_cast<double>(yaw))
        || !std::isfinite(static_cast<double>(pitch))
        || !std::isfinite(static_cast<double>(roll)))
    {
        return false;
    }

    WorldInstanceIdentity identity;
    if (!identity.set(mapFile, instanceId))
    {
        return false;
    }
    identity.revision = revision;

    AutomatiaSavedPlayerPlacement& placement =
        automatiaSavedPlayerPlacements[playerIndex];
    placement.identity = identity;
    placement.x = x;
    placement.y = y;
    placement.z = z;
    placement.yaw = yaw;
    placement.pitch = pitch;
    placement.roll = roll;
    placement.hasPosition = true;
    placement.pending = true;
    return true;
}

bool prepareAutomatiaSavedPlayerSpawnMask(bool playerSpawnMask[MAXPLAYERS])
{
    if (!playerSpawnMask)
    {
        return false;
    }
    std::fill(playerSpawnMask, playerSpawnMask + MAXPLAYERS, true);
    const WorldInstanceIdentity* activeIdentity = worldState.activeIdentity();
    if (!activeIdentity || multiplayer == CLIENT)
    {
        return false;
    }

    bool hasSavedPlacements = false;
    for (int playerIndex = 0; playerIndex < MAXPLAYERS; ++playerIndex)
    {
        if (headless && multiplayer == SERVER && playerIndex == 0)
        {
            playerSpawnMask[playerIndex] = false;
            continue;
        }
        AutomatiaSavedPlayerPlacement& placement =
            automatiaSavedPlayerPlacements[playerIndex];
        if (!placement.pending || !placement.identity.isValid())
        {
            continue;
        }
        hasSavedPlacements = true;
        const bool belongsToActiveMap =
            placement.identity.matches(*activeIdentity);
        playerSpawnMask[playerIndex] = belongsToActiveMap;
        if (!belongsToActiveMap && players[playerIndex])
        {
            worldState.removePlayer(playerIndex);
            players[playerIndex]->worldInstance = placement.identity;
            players[playerIndex]->entity = nullptr;
        }
    }
    return hasSavedPlacements;
}

bool restoreAutomatiaSavedPlayerInstances()
{
    if (multiplayer == CLIENT)
    {
        return false;
    }
    const WorldInstanceIdentity* initialIdentity = worldState.activeIdentity();
    const std::string initialKey =
        initialIdentity ? initialIdentity->key() : std::string{};
    if (initialKey.empty())
    {
        return false;
    }

    std::vector<std::string> destinationKeys;
    for (int playerIndex = 0; playerIndex < MAXPLAYERS; ++playerIndex)
    {
		if (headless && multiplayer == SERVER && playerIndex == 0)
		{
			continue;
		}
        const AutomatiaSavedPlayerPlacement& placement =
            automatiaSavedPlayerPlacements[playerIndex];
        if (!placement.pending
            || !placement.identity.isValid()
            || client_disconnected[playerIndex]
            || !players[playerIndex]
            || placement.identity.key() == initialKey)
        {
            continue;
        }
        if (std::find(destinationKeys.begin(), destinationKeys.end(),
                placement.identity.key()) == destinationKeys.end())
        {
            destinationKeys.push_back(placement.identity.key());
        }
    }
    std::sort(destinationKeys.begin(), destinationKeys.end());

    bool restoredAny = false;
    for (const std::string& destinationKey : destinationKeys)
    {
        AutomatiaSavedPlayerPlacement* representative = nullptr;
        bool playerMask[MAXPLAYERS] = {};
        for (int playerIndex = 0; playerIndex < MAXPLAYERS; ++playerIndex)
        {
			if (headless && multiplayer == SERVER && playerIndex == 0)
			{
				continue;
			}
            AutomatiaSavedPlayerPlacement& placement =
                automatiaSavedPlayerPlacements[playerIndex];
            if (placement.pending
                && placement.identity.key() == destinationKey
                && !client_disconnected[playerIndex]
                && players[playerIndex])
            {
                representative = &placement;
                playerMask[playerIndex] = true;
            }
        }
        if (!representative)
        {
            continue;
        }

        std::string error;
        MapInstance* destination = worldState.find(destinationKey);
        if (!destination || !destination->loadedMap)
        {
            const std::string fullMapPath = physfsFormatMapName(
                representative->identity.mapFile.c_str()
            );
            if (fullMapPath.empty()
                || !worldState.loadDetachedMap(
                    fullMapPath,
                    representative->identity.mapFile,
                    representative->identity.instanceId,
                    error))
            {
                printlog(
                    "[Automatia Save] Could not load saved player instance '%s': %s.",
                    destinationKey.c_str(),
                    error.empty() ? "map file was not found" : error.c_str()
                );
                continue;
            }
            destination = worldState.find(destinationKey);
        }
        if (!destination || !worldState.activate(destinationKey))
        {
            printlog(
                "[Automatia Save] Could not activate saved player instance '%s'.",
                destinationKey.c_str()
            );
            continue;
        }

        numplayers = 0;
        if (!destination->runtimeInitialized)
        {
            applyPersistentMapRemovals();
            assignActions(&map, playerMask, true, false);
            applyPersistentMechanismStates();
            generatePathMaps();
        }
        else
        {
            assignActions(&map, playerMask, true, true);
        }
        applyAutomatiaSavedPlayerPlacements();

        for (int playerIndex = 0; playerIndex < MAXPLAYERS; ++playerIndex)
        {
            if (playerMask[playerIndex]
                && players[playerIndex]
                && players[playerIndex]->entity)
            {
                restoredAny = true;
                printlog(
                    "[Automatia Save] Restored player %d into '%s'.",
                    playerIndex,
                    destinationKey.c_str()
                );
            }
        }
    }

    if (worldState.activeIdentity()
        && worldState.activeIdentity()->key() != initialKey
        && !worldState.activate(initialKey))
    {
        printlog(
            "[Automatia Save] Warning: could not restore foreground instance '%s'.",
            initialKey.c_str()
        );
    }
    return restoredAny;
}

bool prepareAutomatiaLateJoinPlayer(
    int playerIndex,
    bool returningPlayer,
    std::string& error
)
{
    error.clear();
    if (multiplayer != SERVER || playerIndex <= 0
        || playerIndex >= MAXPLAYERS || !players[playerIndex])
    {
        error = "late-join player slot is invalid";
        return false;
    }
    const WorldInstanceIdentity* initialIdentity = worldState.activeIdentity();
    const std::string initialKey =
        initialIdentity ? initialIdentity->key() : std::string{};
    if (initialKey.empty())
    {
        error = "no foreground map instance is active";
        return false;
    }
    auto restoreForeground = [&initialKey]()
    {
        if (!initialKey.empty() && worldState.activeIdentity()
            && worldState.activeIdentity()->key() != initialKey)
        {
            worldState.activate(initialKey);
        }
    };

    AutomatiaSavedPlayerPlacement& placement =
        automatiaSavedPlayerPlacements[playerIndex];
    const bool hasSavedPlacement =
        placement.pending && placement.identity.isValid();
    const bool characterFileReturn =
        returningPlayer
        && characterSaveMode != CharacterSaveMode::NONE;
    if (returningPlayer && !hasSavedPlacement && !characterFileReturn)
    {
        error = "returning player has no valid saved placement";
        return false;
    }
    WorldInstanceIdentity destinationIdentity;
    if (returningPlayer && hasSavedPlacement)
    {
        destinationIdentity = placement.identity;
    }
    else
    {
        destinationIdentity = WorldInstanceIdentity{};
        placement = AutomatiaSavedPlayerPlacement{};
    }
    const std::string destinationKey = destinationIdentity.key();
    worldState.removePlayer(playerIndex);
    players[playerIndex]->worldInstance = destinationIdentity;
    players[playerIndex]->entity = nullptr;

    MapInstance* destination = worldState.find(destinationKey);
    if (!destination || !destination->loadedMap)
    {
        const std::string fullMapPath = physfsFormatMapName(
            destinationIdentity.mapFile.c_str());
        if (fullMapPath.empty()
            || !worldState.loadDetachedMap(
                fullMapPath,
                destinationIdentity.mapFile,
                destinationIdentity.instanceId,
                error))
        {
            if (error.empty())
            {
                error = "late-join destination map could not be loaded";
            }
            restoreForeground();
            return false;
        }
        destination = worldState.find(destinationKey);
    }
    if (!destination || !worldState.activate(destinationKey))
    {
        error = "late-join destination map could not be activated";
        restoreForeground();
        return false;
    }

    bool playerMask[MAXPLAYERS] = {};
    playerMask[playerIndex] = true;
    numplayers = 0;
    if (!destination->runtimeInitialized)
    {
        applyPersistentMapRemovals();
        assignActions(&map, playerMask, true, false);
        applyPersistentMechanismStates();
        generatePathMaps();
    }
    else
    {
        assignActions(&map, playerMask, true, true);
    }
    if (!players[playerIndex]->entity)
    {
        Entity* spawnAnchor = nullptr;
		Entity* retainedPlayerStart = nullptr;
		for (node_t* node = map.entities->first; node; node = node->next)
		{
			Entity* candidate = static_cast<Entity*>(node->element);
			if (candidate && candidate->sprite == 1
				&& candidate->behavior == nullptr)
			{
				retainedPlayerStart = candidate;
				break;
			}
		}
        for (int anchorPlayer = 0; anchorPlayer < MAXPLAYERS; ++anchorPlayer)
        {
            if (anchorPlayer == playerIndex || !players[anchorPlayer]
                || !players[anchorPlayer]->entity
                || players[anchorPlayer]->worldInstance.key() != destinationKey)
            {
                continue;
            }
            spawnAnchor = players[anchorPlayer]->entity;
            break;
        }
		if (spawnAnchor || retainedPlayerStart)
        {
            Entity* fallbackStart = newEntity(1, 1, map.entities, nullptr);
            if (fallbackStart)
            {
				fallbackStart->x = spawnAnchor
					? spawnAnchor->x - 8.0
					: retainedPlayerStart->x;
				fallbackStart->y = spawnAnchor
					? spawnAnchor->y - 8.0
					: retainedPlayerStart->y;
                fallbackStart->skill[2] = playerIndex;
                fallbackStart->playerStartDir = -1;
                numplayers = 0;
                assignActions(&map, playerMask, true, true);
                printlog(
					"[Late Join] Player %d used the %s spawn fallback in '%s'.",
                    playerIndex,
					spawnAnchor ? "occupied-map" : "retained Player Start",
                    destinationKey.c_str()
                );
            }
        }
    }
    if (returningPlayer && hasSavedPlacement)
    {
        const AutomatiaSavedPlayerPlacement savedPlacement = placement;
        applyAutomatiaSavedPlayerPlacements();
        placement = savedPlacement;
    }
    else
    {
        worldState.placePlayer(playerIndex, map);
    }
    if (!players[playerIndex]->entity)
    {
        error = "late-join destination has no available Player Start";
        worldState.removePlayer(playerIndex);
        restoreForeground();
        return false;
    }

    printlog(
        "[Late Join] Prepared %s player %d in '%s'.",
        returningPlayer ? "returning" : "new",
        playerIndex,
        destinationKey.c_str());
    restoreForeground();
    return true;
}

static std::string automatiaPlayerHistoryFallbackIdentity(
    const int playerIndex
)
{
    return playerIndex >= 0 && playerIndex < MAXPLAYERS
        ? "slot:" + std::to_string(playerIndex)
        : std::string{};
}

static std::string automatiaPlayerHistoryIdentity(
    const int playerIndex
)
{
    const std::string preferred =
        persistentMinimapPlayerIdentity(playerIndex);
    const std::string fallback =
        automatiaPlayerHistoryFallbackIdentity(playerIndex);
    if (preferred.empty())
    {
        return fallback;
    }

    std::vector<std::string> previousIdentities;
    previousIdentities.push_back(fallback);
    const std::string characterIdentity =
        persistentMinimapCharacterNameIdentity(playerIndex);
    if (!characterIdentity.empty())
    {
        previousIdentities.push_back(characterIdentity);
    }
    if (playerIndex >= 0 && playerIndex < MAXPLAYERS
        && ReconnectToken::isValid(automatiaReconnectTokens[playerIndex]))
    {
        previousIdentities.push_back(
            "reconnect:" + automatiaReconnectTokens[playerIndex]
        );
    }

    AutomatiaPlayerLevelHistory& target =
        automatiaPlayerLevelHistories[preferred];
    std::unordered_set<std::string> migratedIdentities;
    for (const std::string& previousIdentity : previousIdentities)
    {
        if (previousIdentity.empty() || previousIdentity == preferred
            || !migratedIdentities.insert(previousIdentity).second)
        {
            continue;
        }
        auto previousHistory =
            automatiaPlayerLevelHistories.find(previousIdentity);
        if (previousHistory == automatiaPlayerLevelHistories.end())
        {
            continue;
        }
        if (target.empty())
        {
            target = std::move(previousHistory->second);
        }
        else if (!previousHistory->second.empty())
        {
            target.insert(
                target.begin(),
                std::make_move_iterator(previousHistory->second.begin()),
                std::make_move_iterator(previousHistory->second.end())
            );
            if (target.size() > AUTOMATIA_PLAYER_LEVEL_HISTORY_LIMIT)
            {
                target.erase(
                    target.begin(),
                    target.begin()
                        + (target.size()
                            - AUTOMATIA_PLAYER_LEVEL_HISTORY_LIMIT)
                );
            }
        }
        automatiaPlayerLevelHistories.erase(previousHistory);
        printlog(
            "[Ladder Reverse] Migrated player %d route history from identity '%s' to '%s'.",
            playerIndex,
            previousIdentity.c_str(),
            preferred.c_str()
        );
    }
    return preferred;
}

static AutomatiaPlayerLevelHistory* automatiaHistoryForPlayer(
    const int playerIndex
)
{
    const std::string identity =
        automatiaPlayerHistoryIdentity(playerIndex);
    if (identity.empty())
    {
        return nullptr;
    }
    auto inserted = automatiaPlayerLevelHistories.emplace(
        identity,
        AutomatiaPlayerLevelHistory{}
    );
    return &inserted.first->second;
}

static bool buildAutomatiaPlayerLevelVisit(
    const int playerIndex,
    const MapInstance& sourceInstance,
    const Entity& playerEntity,
    const Entity* departureEntity,
    AutomatiaPlayerLevelVisit& visit
)
{
    if (playerIndex < 0 || playerIndex >= MAXPLAYERS
        || !sourceInstance.identity.isValid()
        || !std::isfinite(static_cast<double>(playerEntity.x))
        || !std::isfinite(static_cast<double>(playerEntity.y))
        || !std::isfinite(static_cast<double>(playerEntity.z))
        || !std::isfinite(static_cast<double>(playerEntity.yaw)))
    {
        return false;
    }
    visit = AutomatiaPlayerLevelVisit{};
    visit.dungeonLevel = sourceInstance.dungeonLevel;
    visit.secretLevel = sourceInstance.secretLevel;
    visit.mapSeed = sourceInstance.mapSeed;
    visit.mapInstanceKey = sourceInstance.key();
    visit.returnPlacement.valid = true;
    visit.returnPlacement.x = playerEntity.x;
    visit.returnPlacement.y = playerEntity.y;
    visit.returnPlacement.z = playerEntity.z;
    visit.returnPlacement.yaw = playerEntity.yaw;
    visit.returnPlacement.pitch = playerEntity.pitch;
    visit.returnPlacement.roll = playerEntity.roll;
    if (departureEntity
        && std::isfinite(static_cast<double>(departureEntity->x))
        && std::isfinite(static_cast<double>(departureEntity->y))
        && std::isfinite(static_cast<double>(departureEntity->z)))
    {
        visit.hasReturnAnchor = true;
        visit.returnAnchorPersistentID = departureEntity->persistentID;
        visit.returnAnchorX = departureEntity->x;
        visit.returnAnchorY = departureEntity->y;
        visit.returnAnchorZ = departureEntity->z;
    }
    return true;
}

static bool appendAutomatiaPlayerLevelVisit(
    const int playerIndex,
    AutomatiaPlayerLevelVisit visit
)
{
    AutomatiaPlayerLevelHistory* history =
        automatiaHistoryForPlayer(playerIndex);
    if (!history || visit.mapInstanceKey.empty())
    {
        return false;
    }
    if (!history->empty())
    {
        AutomatiaPlayerLevelVisit& previous = history->back();
        const bool sameInstance =
            previous.mapInstanceKey == visit.mapInstanceKey;
        if (sameInstance)
        {
            previous = std::move(visit);
            return true;
        }
    }
    if (history->size() >= AUTOMATIA_PLAYER_LEVEL_HISTORY_LIMIT)
    {
        history->erase(history->begin());
    }
    history->push_back(std::move(visit));
    const AutomatiaPlayerLevelVisit& recorded = history->back();
    printlog(
        "[Ladder Reverse] Recorded player %d departure from level %d (%s track, seed %u, instance '%s'); personal back stack now has %zu entr%s.",
        playerIndex,
        recorded.dungeonLevel,
        recorded.secretLevel ? "secret" : "regular",
        recorded.mapSeed,
        recorded.mapInstanceKey.c_str(),
        history->size(),
        history->size() == 1 ? "y" : "ies"
    );
    return true;
}

void recordAutomatiaPlayerLevelVisit(
    const int playerIndex,
    const Entity* departureEntity
)
{
    if (multiplayer == CLIENT || playerIndex < 0
        || playerIndex >= MAXPLAYERS || !players[playerIndex])
    {
        return;
    }
    const MapInstance* sourceInstance = worldState.activeInstance();
    if (!sourceInstance)
    {
        return;
    }
    Entity* playerEntity = worldState.playerEntityFor(
        sourceInstance->key(),
        playerIndex
    );
    if (!playerEntity && players[playerIndex]->worldInstance.matches(
        sourceInstance->identity))
    {
        playerEntity = players[playerIndex]->entity;
    }
    if (!playerEntity)
    {
        return;
    }
    AutomatiaPlayerLevelVisit visit;
    if (buildAutomatiaPlayerLevelVisit(
        playerIndex,
        *sourceInstance,
        *playerEntity,
        departureEntity,
        visit))
    {
        appendAutomatiaPlayerLevelVisit(playerIndex, std::move(visit));
    }
}

void recordAutomatiaPartyLevelVisit(
    const Entity* departureEntity
)
{
    if (multiplayer == CLIENT)
    {
        return;
    }
    const MapInstance* sourceInstance = worldState.activeInstance();
    if (!sourceInstance)
    {
        return;
    }
    std::unordered_set<int> occupantSet(
        sourceInstance->playersPresent.begin(),
        sourceInstance->playersPresent.end()
    );
    for (int playerIndex = 0; playerIndex < MAXPLAYERS; ++playerIndex)
    {
        if (players[playerIndex]
            && players[playerIndex]->worldInstance.matches(
                sourceInstance->identity))
        {
            occupantSet.insert(playerIndex);
        }
    }
    std::vector<int> occupants(
        occupantSet.begin(),
        occupantSet.end()
    );
    std::sort(occupants.begin(), occupants.end());
    for (const int playerIndex : occupants)
    {
        if (playerIndex < 0 || playerIndex >= MAXPLAYERS
            || client_disconnected[playerIndex]
            || !players[playerIndex])
        {
            continue;
        }
        recordAutomatiaPlayerLevelVisit(
            playerIndex,
            departureEntity
        );
    }
}

bool consumeAutomatiaPreviousPlayerLevel(
    const int playerIndex,
    AutomatiaPlayerLevelVisit& destination,
    std::string& error
)
{
    error.clear();
    destination = AutomatiaPlayerLevelVisit{};
    if (multiplayer == CLIENT)
    {
        error = "only the server or local game may choose a previous level";
        return false;
    }
    AutomatiaPlayerLevelHistory* history =
        automatiaHistoryForPlayer(playerIndex);
    if (!history || history->empty())
    {
        error = "this player has no previously visited level to return to";
        return false;
    }
    destination = history->back();
    history->pop_back();
    if (destination.dungeonLevel < 0
        || destination.mapInstanceKey.empty())
    {
        error = "the recorded previous player level is invalid";
        return false;
    }
    printlog(
        "[Ladder Reverse] Player %d consumed previous level %d (%s track, seed %u, instance '%s'); %zu earlier entr%s remain for that player.",
        playerIndex,
        destination.dungeonLevel,
        destination.secretLevel ? "secret" : "regular",
        destination.mapSeed,
        destination.mapInstanceKey.c_str(),
        history->size(),
        history->size() == 1 ? "y" : "ies"
    );
    return true;
}

void restoreAutomatiaPreviousPlayerLevel(
    const int playerIndex,
    const AutomatiaPlayerLevelVisit& destination
)
{
    if (multiplayer == CLIENT || destination.mapInstanceKey.empty())
    {
        return;
    }
    AutomatiaPlayerLevelHistory* history =
        automatiaHistoryForPlayer(playerIndex);
    if (!history)
    {
        return;
    }
    if (history->size() >= AUTOMATIA_PLAYER_LEVEL_HISTORY_LIMIT)
    {
        history->erase(history->begin());
    }
    history->push_back(destination);
    printlog(
        "[Ladder Reverse] Restored failed transition entry for player %d: level %d (%s track, instance '%s'); personal back stack now has %zu entr%s.",
        playerIndex,
        destination.dungeonLevel,
        destination.secretLevel ? "secret" : "regular",
        destination.mapInstanceKey.c_str(),
        history->size(),
        history->size() == 1 ? "y" : "ies"
    );
}

void prepareAutomatiaReverseReturnSpawn(
    const int playerIndex,
    const AutomatiaPlayerLevelVisit& destination
)
{
    pendingAutomatiaSinglePlayerReverseReturn.playerIndex = playerIndex;
    pendingAutomatiaSinglePlayerReverseReturn.visit = destination;
    pendingAutomatiaSinglePlayerReverseReturn.pending =
        destination.returnPlacement.valid;
}

void resetPersistentWorldSession()
{
    const size_t clearedMaps =
        persistentMapRemovalRegistry.size();

    const size_t clearedQuests =
        persistentWorldStoryState.quests.size();

    const size_t clearedNPCMemories =
        persistentWorldStoryState.npcDialogueStates.size();

    const size_t clearedWorldVariables =
        persistentWorldStoryState.worldVariables.size();

    const size_t clearedWorldFlags =
        persistentWorldStoryState.worldFlags.size();

    size_t clearedPlayerLevelHistory =
        automatiaLegacyPartyLevelHistory.size();
    for (const auto& history : automatiaPlayerLevelHistories)
    {
        clearedPlayerLevelHistory += history.second.size();
    }

    persistentMapRemovalRegistry.clear();
    worldState.clear();
    for (AutomatiaSavedPlayerPlacement& placement : automatiaSavedPlayerPlacements)
    {
        placement = AutomatiaSavedPlayerPlacement{};
    }
    preservedAutomatiaWorldDocument = AutomatiaSave::Json{};
    if (pendingAutomatiaWorldDocument.is_object())
    {
        preservedAutomatiaWorldDocument =
            std::move(pendingAutomatiaWorldDocument);
        pendingAutomatiaWorldDocument = AutomatiaSave::Json{};
    }


    automatiaPlayerLevelHistories.clear();
    automatiaLegacyPartyLevelHistory.clear();
    pendingAutomatiaSinglePlayerReverseReturn =
        PendingAutomatiaSinglePlayerReverseReturn{};
    persistentPlayerMinimapRegistry.clear();
    persistentLegacyMinimapRegistry.clear();
    persistentMinimapServerTransferId = 0;
    persistentMinimapClearedMapKey.clear();
    persistentMinimapClearedMapRevision = 0;
    resetClientPersistentMinimapSync();
    persistentWorldStoryState =
        PersistentWorldStoryState{};

    nextDynamicMonsterPersistentID = -1;

    clientPersistentSnapshotMapKey.clear();
    clientPersistentSnapshotReceiving = false;
    clientPersistentSnapshotComplete = false;

    hydratePreservedAutomatiaWorldDocument();

    printlog(
        "[Persistent World] Reset session registry; cleared %zu map state record(s), %zu quest(s), %zu NPC dialogue memory record(s), %zu world variable(s), %zu world flag(s), and %zu per-player level-history entr%s.",
        clearedMaps,
        clearedQuests,
        clearedNPCMemories,
        clearedWorldVariables,
        clearedWorldFlags,
        clearedPlayerLevelHistory,
        clearedPlayerLevelHistory == 1 ? "y" : "ies"
    );
}
/*
 * Normalize either a legacy map filename or a canonical map-instance key.
 * Legacy callers intentionally resolve to the shared "world" instance.
 */
static std::string normalizePersistentMapKey(
    std::string key
)
{
    if ( key.empty() )
    {
        return "";
    }

    std::string instanceId = "world";
    const size_t separator = key.find('#');
    if ( separator != std::string::npos )
    {
        if ( key.find('#', separator + 1) != std::string::npos )
        {
            return "";
        }
        instanceId = key.substr(separator + 1);
        key = key.substr(0, separator);
    }

    WorldInstanceIdentity identity;
    if ( !identity.set(
            WorldInstanceIdentity::canonicalMapFile(key),
            instanceId
        ) )
    {
        return "";
    }
    return identity.key();
}

static bool captureAutomatiaPersistentWorldDocument(
    const std::string& sessionId,
    AutomatiaSave::Json& capturedDocument,
    std::string& error
)
{
    using AutomatiaSave::Json;

    const WorldInstanceIdentity* foregroundIdentity =
        worldState.activeIdentity();
    const std::string foregroundKey =
        foregroundIdentity ? foregroundIdentity->key() : std::string{};
    if (map.tiles && map.entities && foregroundIdentity)
    {
        for (const MapInstanceSummary& summary : worldState.instanceSummaries())
        {
            if (!summary.loaded)
            {
                continue;
            }
            if (!worldState.activate(summary.identity.key()))
            {
                error = "unable to activate map instance for save capture: "
                    + summary.identity.key();
                if (!foregroundKey.empty())
                {
                    worldState.activate(foregroundKey);
                }
                return false;
            }
            capturePersistentMinimap();
            capturePersistentMechanismStates();
            capturePersistentMapRemovals();
        }
        if (!foregroundKey.empty()
            && !worldState.activate(foregroundKey))
        {
            error = "unable to restore foreground map after save capture";
            return false;
        }
    }

    Json runtimeDocument = AutomatiaSave::captureWorldState(sessionId, worldState);
    Json document = preservedAutomatiaWorldDocument.is_object()
        ? preservedAutomatiaWorldDocument
        : AutomatiaSave::makeEmptyWorldSave(sessionId);
    for (auto member = runtimeDocument.begin(); member != runtimeDocument.end(); ++member)
    {
        if (member.key() != "map_instances")
        {
            document[member.key()] = member.value();
        }
    }

    Json mergedMaps = Json::array();
    auto mapKeyFor = [](const Json& mapEntry) -> std::string
    {
        if (!mapEntry.is_object()
            || !mapEntry.contains("map_file")
            || !mapEntry["map_file"].is_string()
            || !mapEntry.contains("instance_id")
            || !mapEntry["instance_id"].is_string())
        {
            return "";
        }
        return mapEntry["map_file"].get<std::string>()
            + "#"
            + mapEntry["instance_id"].get<std::string>();
    };
    for (const Json& runtimeMap : runtimeDocument["map_instances"])
    {
        Json mergedMap = Json::object();
        if (preservedAutomatiaWorldDocument.contains("map_instances")
            && preservedAutomatiaWorldDocument["map_instances"].is_array())
        {
            for (const Json& oldMap : preservedAutomatiaWorldDocument["map_instances"])
            {
                if (mapKeyFor(oldMap) == mapKeyFor(runtimeMap))
                {
                    mergedMap = oldMap;
                    break;
                }
            }
        }
        mergedMap.update(runtimeMap);
        mergedMaps.push_back(std::move(mergedMap));
    }
    if (preservedAutomatiaWorldDocument.contains("map_instances")
        && preservedAutomatiaWorldDocument["map_instances"].is_array())
    {
        for (const Json& oldMap : preservedAutomatiaWorldDocument["map_instances"])
        {
            bool alreadyPresent = false;
            for (const Json& mergedMap : mergedMaps)
            {
                if (mapKeyFor(mergedMap) == mapKeyFor(oldMap))
                {
                    alreadyPresent = true;
                    break;
                }
            }
            if (!alreadyPresent)
            {
                mergedMaps.push_back(oldMap);
            }
        }
    }
    document["map_instances"] = std::move(mergedMaps);
    document["saved_at_unix_ms"] =
        static_cast<std::uint64_t>(std::time(nullptr)) * 1000ULL;

    auto saveLevelVisit = [](const AutomatiaPlayerLevelVisit& visit)
    {
        Json savedVisit = {
            {"dungeon_level", visit.dungeonLevel},
            {"secret_level", visit.secretLevel},
            {"map_seed", visit.mapSeed}
        };
        if (!visit.mapInstanceKey.empty())
        {
            savedVisit["map_instance"] = visit.mapInstanceKey;
        }
        if (visit.hasReturnAnchor)
        {
            savedVisit["return_anchor"] = {
                {"persistent_id", visit.returnAnchorPersistentID},
                {"x", visit.returnAnchorX},
                {"y", visit.returnAnchorY},
                {"z", visit.returnAnchorZ}
            };
        }
        if (visit.returnPlacement.valid)
        {
            savedVisit["return_position"] = {
                {"x", visit.returnPlacement.x},
                {"y", visit.returnPlacement.y},
                {"z", visit.returnPlacement.z},
                {"yaw", visit.returnPlacement.yaw},
                {"pitch", visit.returnPlacement.pitch},
                {"roll", visit.returnPlacement.roll}
            };
        }
        return savedVisit;
    };

    document["player_level_histories"] = Json::object();
    std::vector<std::string> historyIdentities;
    historyIdentities.reserve(automatiaPlayerLevelHistories.size());
    for (const auto& history : automatiaPlayerLevelHistories)
    {
        historyIdentities.push_back(history.first);
    }
    std::sort(historyIdentities.begin(), historyIdentities.end());
    for (const std::string& identity : historyIdentities)
    {
        Json savedHistory = Json::array();
        const AutomatiaPlayerLevelHistory& history =
            automatiaPlayerLevelHistories.at(identity);
        for (const AutomatiaPlayerLevelVisit& visit : history)
        {
            savedHistory.push_back(saveLevelVisit(visit));
        }
        document["player_level_histories"][identity] =
            std::move(savedHistory);
    }
    if (document["player_level_histories"].empty())
    {
        document.erase("player_level_histories");
    }

    document["party_level_history"] = Json::array();
    for (const AutomatiaPlayerLevelVisit& visit
        : automatiaLegacyPartyLevelHistory)
    {
        document["party_level_history"].push_back(
            saveLevelVisit(visit)
        );
    }
    if (document["party_level_history"].empty())
    {
        document.erase("party_level_history");
    }

    auto sortedIntegerSet = [](const auto& values)
    {
        std::vector<Sint32> sorted(values.begin(), values.end());
        std::sort(sorted.begin(), sorted.end());
        return sorted;
    };
    auto sortedStringSet = [](const auto& values)
    {
        std::vector<std::string> sorted(values.begin(), values.end());
        std::sort(sorted.begin(), sorted.end());
        return sorted;
    };

    std::unordered_set<std::string> minimapAwareMapKeys;
    minimapAwareMapKeys.reserve(
        persistentMapRemovalRegistry.size()
        + persistentLegacyMinimapRegistry.size());
    for (const auto& entry : persistentMapRemovalRegistry)
    {
        minimapAwareMapKeys.insert(entry.first);
    }
    for (const auto& entry : persistentLegacyMinimapRegistry)
    {
        minimapAwareMapKeys.insert(entry.first);
    }
    for (const auto& player : persistentPlayerMinimapRegistry)
    {
        for (const auto& entry : player.second)
        {
            minimapAwareMapKeys.insert(entry.first);
        }
    }
    std::vector<std::string> mapKeys(
        minimapAwareMapKeys.begin(), minimapAwareMapKeys.end());
    std::sort(mapKeys.begin(), mapKeys.end());

    for (const std::string& mapKey : mapKeys)
    {
        const PersistentMapRemovalState& state =
            persistentMapRemovalRegistry[mapKey];
        Json* mapDocument = nullptr;
        for (Json& candidate : document["map_instances"])
        {
            if (candidate["map_file"].get<std::string>()
                    + "#"
                    + candidate["instance_id"].get<std::string>()
                == mapKey)
            {
                mapDocument = &candidate;
                break;
            }
        }
        if (!mapDocument)
        {
            const std::size_t separator = mapKey.find('#');
            if (separator == std::string::npos)
            {
                continue;
            }
            document["map_instances"].push_back(Json{
                {"map_file", mapKey.substr(0, separator)},
                {"instance_id", mapKey.substr(separator + 1)},
                {"revision", 0},
                {"loaded", false},
                {"persistent_state", Json::object()}
            });
            mapDocument = &document["map_instances"].back();
        }

        Json persistent = mapDocument->contains("persistent_state")
            && (*mapDocument)["persistent_state"].is_object()
            ? (*mapDocument)["persistent_state"]
            : Json::object();
        persistent["removed_entity_ids"] =
            sortedIntegerSet(state.removedEntityIDs);
        persistent["dynamic_monster_ids"] =
            sortedIntegerSet(state.dynamicMonsterIDs);

        persistent["tile_states"] = Json::array();
        std::vector<PersistentTileState> tiles;
        tiles.reserve(state.tileStates.size());
        for (const auto& tile : state.tileStates)
        {
            tiles.push_back(tile.second);
        }
        std::sort(
            tiles.begin(),
            tiles.end(),
            [](const PersistentTileState& first, const PersistentTileState& second)
            {
                if (first.x != second.x) { return first.x < second.x; }
                if (first.y != second.y) { return first.y < second.y; }
                return first.layer < second.layer;
            }
        );
        for (const PersistentTileState& tile : tiles)
        {
            persistent["tile_states"].push_back(Json{
                {"x", tile.x},
                {"y", tile.y},
                {"layer", tile.layer},
                {"tile", tile.tile}
            });
        }

        persistent["dynamic_world_items"] = Json::array();
        for (const PersistentWorldItemState& item : state.dynamicWorldItems)
        {
            persistent["dynamic_world_items"].push_back(Json{
                {"stable_id", item.stableId},
                {"legacy_type", item.type},
                {"status", item.status},
                {"beatitude", item.beatitude},
                {"count", item.count},
                {"appearance", item.appearance},
                {"identified", item.identified},
                {"position", {item.x, item.y, item.z}},
                {"rotation", {item.yaw, item.pitch, item.roll}},
                {"velocity", {item.velX, item.velY, item.velZ}},
                {"not_moving", item.itemNotMoving},
                {"sokoban_reward", item.itemSokobanReward},
                {"stolen", item.itemStolen},
                {"show_on_map", item.itemShowOnMap},
                {"pickup_delay", item.itemDelayMonsterPickingUp},
                {"passable", item.passable},
                {"invisible", item.invisible},
                {"burning", item.burning},
                {"burnable", item.burnable}
            });
        }

        persistent["dynamic_gold_bags"] = Json::array();
        for (const PersistentGoldBagState& gold : state.dynamicGoldBags)
        {
            persistent["dynamic_gold_bags"].push_back(Json{
                {"amount", gold.amount},
                {"bonus", gold.amountBonus},
                {"sokoban", gold.sokoban},
                {"bouncing", gold.bouncing},
                {"dropped_by_player", gold.droppedByPlayer},
                {"position", {gold.x, gold.y, gold.z}},
                {"rotation", {gold.yaw, gold.pitch, gold.roll}},
                {"velocity", {gold.velX, gold.velY, gold.velZ}},
                {"passable", gold.passable},
                {"invisible", gold.invisible}
            });
        }

        persistent["dynamic_boulders"] = Json::array();
        for (const PersistentBoulderState& boulder : state.dynamicBoulders)
        {
            persistent["dynamic_boulders"].push_back(Json{
                {"source_trap_id", boulder.sourceTrapPersistentID},
                {"sprite", boulder.sprite},
                {"position", {boulder.x, boulder.y, boulder.z}},
                {"rotation", {boulder.yaw, boulder.pitch, boulder.roll}},
                {"velocity", {boulder.velX, boulder.velY, boulder.velZ}},
                {"stopped", boulder.stopped},
                {"no_ground", boulder.noGround},
                {"rolling", boulder.rolling},
                {"roll_direction", boulder.rollDirection},
                {"destination_x", boulder.destinationX},
                {"destination_y", boulder.destinationY},
                {"initialized", boulder.initialized},
                {"lava_explode_timer", boulder.lavaExplodeTimer},
                {"passable", boulder.passable}
            });
        }

        persistent["mechanisms"] = Json::array();
        std::vector<Sint32> mechanismIds;
        mechanismIds.reserve(state.mechanismStates.size());
        for (const auto& mechanism : state.mechanismStates)
        {
            mechanismIds.push_back(mechanism.first);
        }
        std::sort(mechanismIds.begin(), mechanismIds.end());
        for (const Sint32 id : mechanismIds)
        {
            const PersistentMechanismState& mechanism = state.mechanismStates.at(id);
            persistent["mechanisms"].push_back(
                saveMechanismState(id, mechanism)
            );
        }

        // New minimap discovery is stored per character inside each map's
        // already-versioned persistent_state object. Preserve an unmigrated
        // legacy shared field until a real local character adopts it.
        const auto legacyMinimap = persistentLegacyMinimapRegistry.find(mapKey);
        if (legacyMinimap != persistentLegacyMinimapRegistry.end())
        {
            persistent["minimap"] = legacyMinimap->second.tiles;
        }
        else
        {
            persistent.erase("minimap");
        }
        Json savedPlayerMinimaps =
            persistent.contains("player_minimaps")
                && persistent["player_minimaps"].is_object()
            ? persistent["player_minimaps"]
            : Json::object();
        std::vector<std::string> minimapPlayerIdentities;
        minimapPlayerIdentities.reserve(persistentPlayerMinimapRegistry.size());
        for (const auto& player : persistentPlayerMinimapRegistry)
        {
            if (player.second.find(mapKey) != player.second.end())
            {
                minimapPlayerIdentities.push_back(player.first);
            }
        }
        std::sort(minimapPlayerIdentities.begin(), minimapPlayerIdentities.end());
        for (const std::string& playerIdentity : minimapPlayerIdentities)
        {
            const PersistentMinimapState& state =
                persistentPlayerMinimapRegistry.at(playerIdentity).at(mapKey);
            const std::size_t expected =
                state.width > 0 && state.height > 0
                ? static_cast<std::size_t>(state.width)
                    * static_cast<std::size_t>(state.height)
                : 0;
            if (state.width <= 0 || state.height <= 0
                || state.width > MINIMAP_MAX_DIMENSION
                || state.height > MINIMAP_MAX_DIMENSION
                || state.tiles.size() != expected)
            {
                continue;
            }
            Json runs = Json::array();
            std::size_t index = 0;
            while (index < state.tiles.size())
            {
                const Sint8 value = state.tiles[index];
                if (value <= 0 || value > 4)
                {
                    ++index;
                    continue;
                }
                const std::size_t start = index;
                while (index < state.tiles.size()
                    && state.tiles[index] == value
                    && index - start < static_cast<std::size_t>(INT32_MAX))
                {
                    ++index;
                }
                runs.push_back(Json::array({
                    static_cast<Sint32>(start),
                    static_cast<Sint32>(index - start),
                    static_cast<Sint32>(value)
                }));
            }
            Json savedPlayer =
                savedPlayerMinimaps.contains(playerIdentity)
                    && savedPlayerMinimaps[playerIdentity].is_object()
                ? savedPlayerMinimaps[playerIdentity]
                : Json::object();
            savedPlayer["width"] = state.width;
            savedPlayer["height"] = state.height;
            savedPlayer["runs"] = std::move(runs);
            savedPlayerMinimaps[playerIdentity] = std::move(savedPlayer);
        }
        if (savedPlayerMinimaps.empty())
        {
            persistent.erase("player_minimaps");
        }
        else
        {
            persistent["player_minimaps"] =
                std::move(savedPlayerMinimaps);
        }
        (*mapDocument)["persistent_state"] = std::move(persistent);
    }

    document["quests"] = Json::object();
    for (const auto& entry : persistentWorldStoryState.quests)
    {
        const PersistentQuestState& quest = entry.second;
        document["quests"][entry.first] = Json{
            {"started", quest.started},
            {"accepted", quest.accepted},
            {"completed", quest.completed},
            {"failed", quest.failed},
            {"stage", quest.stage},
            {"variables", quest.variables},
            {"flags", sortedStringSet(quest.flags)},
            {"completed_objectives", sortedStringSet(quest.completedObjectives)},
            {"used_choices", sortedStringSet(quest.usedChoices)}
        };
    }
    document["world_variables"] = persistentWorldStoryState.worldVariables;
    document["world_flags"] =
        sortedStringSet(persistentWorldStoryState.worldFlags);
    document["dialogue"] = Json::object();
    for (const auto& entry : persistentWorldStoryState.npcDialogueStates)
    {
        const PersistentNPCDialogueState& dialogue = entry.second;
        document["dialogue"][entry.first] = Json{
            {"dialogue_id", dialogue.dialogueID},
            {"current_node", dialogue.currentNode},
            {"conversation_started", dialogue.conversationStarted},
            {"reward_given", dialogue.rewardGiven},
            {"variables", dialogue.variables},
            {"flags", sortedStringSet(dialogue.flags)},
            {"used_choices", sortedStringSet(dialogue.usedChoices)},
            {"seen_nodes", sortedStringSet(dialogue.seenNodes)}
        };
    }

    document["players"] = Json::array();
    for (int playerIndex = 0; playerIndex < MAXPLAYERS; ++playerIndex)
    {
        if (characterSaveMode != CharacterSaveMode::NONE)
        {
            // Per-character files own remote character persistence. Do not
            // reserve network slots in the shared world companion document.
            continue;
        }
		if (headless && multiplayer == SERVER && playerIndex == 0)
		{
			continue;
		}
        const AutomatiaSavedPlayerPlacement& placement =
            automatiaSavedPlayerPlacements[playerIndex];
        if (placement.pending && placement.identity.isValid())
        {
            Json savedPlayer = {
                {"player_id", "slot:" + std::to_string(playerIndex)},
                {"identity_kind", "session_slot"},
                {"slot", playerIndex},
                {"map_file", placement.identity.mapFile},
                {"instance_id", placement.identity.instanceId},
                {"revision", placement.identity.revision}
            };
            if (placement.hasPosition)
            {
                savedPlayer["position"] = {
                    placement.x, placement.y, placement.z
                };
                savedPlayer["rotation"] = {
                    placement.yaw, placement.pitch, placement.roll
                };
            }
            document["players"].push_back(std::move(savedPlayer));
            continue;
        }
        if (!players[playerIndex]
            || (client_disconnected[playerIndex]
                && !players[playerIndex]->was_connected_to_game)
            || !players[playerIndex]->worldInstance.isValid())
        {
            continue;
        }
        Json savedPlayer = {
            {"player_id", "slot:" + std::to_string(playerIndex)},
            {"identity_kind", "session_slot"},
            {"slot", playerIndex},
            {"map_file", players[playerIndex]->worldInstance.mapFile},
            {"instance_id", players[playerIndex]->worldInstance.instanceId},
            {"revision", players[playerIndex]->worldInstance.revision}
        };
        Entity* savedEntity = worldState.playerEntityFor(
            players[playerIndex]->worldInstance.key(),
            playerIndex
        );
        if (savedEntity)
        {
            const Entity& entity = *savedEntity;
            savedPlayer["position"] = {entity.x, entity.y, entity.z};
            savedPlayer["rotation"] = {entity.yaw, entity.pitch, entity.roll};
        }
        document["players"].push_back(std::move(savedPlayer));
    }

    const AutomatiaSave::Result validation =
        AutomatiaSave::validate(document);
    if (!validation.ok)
    {
        error = validation.error;
        return false;
    }
    capturedDocument = std::move(document);
    error.clear();
    return true;
}

bool serializeAutomatiaPersistentWorldSnapshot(
    const std::string& sessionId,
    const int playerIndex,
    std::string& snapshot,
    std::string& error
)
{
    AutomatiaSave::Json document;
    if (!captureAutomatiaPersistentWorldDocument(
            sessionId, document, error))
    {
        snapshot.clear();
        return false;
    }

    /*
     * A late-joining client receives only its selected map instance. Global
     * story state remains in the envelope, but persistent entities, map tiles,
     * occupants, and player placements from unrelated instances must not leak
     * into the snapshot or be hydrated accidentally.
     */
    if (!document.contains("active_instance")
        || !document["active_instance"].is_string())
    {
        snapshot.clear();
        error = "persistent-world snapshot has no active map instance";
        return false;
    }
    const std::string activeInstance =
        document["active_instance"].get<std::string>();
    const std::size_t separator = activeInstance.find('#');
    if (separator == std::string::npos)
    {
        snapshot.clear();
        error = "persistent-world snapshot has an invalid active map instance";
        return false;
    }
    const std::string activeMapFile = activeInstance.substr(0, separator);
    const std::string activeInstanceId = activeInstance.substr(separator + 1);
    AutomatiaSave::Json scopedMaps = AutomatiaSave::Json::array();
    for (const AutomatiaSave::Json& savedMap : document["map_instances"])
    {
        if (savedMap.value("map_file", std::string{}) == activeMapFile
            && savedMap.value("instance_id", std::string{}) == activeInstanceId)
        {
            scopedMaps.push_back(savedMap);
        }
    }
    if (scopedMaps.size() != 1)
    {
        snapshot.clear();
        error = "active map instance is missing or duplicated in snapshot";
        return false;
    }
    AutomatiaSave::Json scopedPlayers = AutomatiaSave::Json::array();
    for (const AutomatiaSave::Json& savedPlayer : document["players"])
    {
        if (savedPlayer.value("map_file", std::string{}) == activeMapFile
            && savedPlayer.value("instance_id", std::string{}) == activeInstanceId)
        {
            scopedPlayers.push_back(savedPlayer);
        }
    }
    document["players"] = std::move(scopedPlayers);
    const std::string minimapIdentity =
        persistentMinimapPlayerIdentity(playerIndex);
    AutomatiaSave::Json& scopedPersistent =
        scopedMaps[0]["persistent_state"];
    if (scopedPersistent.is_object()
        && scopedPersistent.contains("player_minimaps")
        && scopedPersistent["player_minimaps"].is_object())
    {
        AutomatiaSave::Json scopedPlayerMinimaps =
            AutomatiaSave::Json::object();
        if (!minimapIdentity.empty()
            && scopedPersistent["player_minimaps"].contains(minimapIdentity))
        {
            scopedPlayerMinimaps[minimapIdentity] =
                scopedPersistent["player_minimaps"][minimapIdentity];
        }
        scopedPersistent["player_minimaps"] =
            std::move(scopedPlayerMinimaps);
    }
    document["map_instances"] = std::move(scopedMaps);
    document["snapshot_scope"] = "map_instance";
#ifdef SAM_FRAMEWORK_ENABLED
    document["sam_fingerprint"] = SAMSync::generateFingerprint();
#endif
    try
    {
        snapshot = document.dump();
    }
    catch (const std::exception& exception)
    {
        snapshot.clear();
        error = "unable to serialize persistent-world snapshot: "
            + std::string(exception.what());
        return false;
    }
    constexpr std::size_t maxLateJoinSnapshotBytes =
        16U * 1024U * 1024U;
    if (snapshot.empty() || snapshot.size() > maxLateJoinSnapshotBytes)
    {
        snapshot.clear();
        error = "persistent-world snapshot exceeds the 16 MiB late-join limit";
        return false;
    }
    error.clear();
    return true;
}

bool writeAutomatiaPersistentWorldSave(
    const char* path,
    const std::string& sessionId,
    std::string& error
)
{
    AutomatiaSave::Json document;
    if (!captureAutomatiaPersistentWorldDocument(
            sessionId, document, error))
    {
        return false;
    }
    const AutomatiaSave::Result result =
        AutomatiaSave::writeAtomic(path, document);
    if (result.ok)
    {
        preservedAutomatiaWorldDocument = document;
    }
    error = result.error;
    return result.ok;
}

static bool stageAutomatiaPersistentWorldDocument(
    AutomatiaSave::Json document,
    const std::string& sessionId,
    std::string& error
)
{
    const AutomatiaSave::Result validation = AutomatiaSave::validate(document);
    if (!validation.ok)
    {
        error = validation.error;
        return false;
    }
    if (document["session_id"].get<std::string>() != sessionId)
    {
        error = "world save session ID does not match the character save";
        return false;
    }
    pendingAutomatiaWorldDocument = std::move(document);
    error.clear();
    return true;
}

bool stageAutomatiaPersistentWorldSnapshot(
    const std::string& snapshot,
    const std::string& sessionId,
    std::string& error
)
{
    constexpr std::size_t maxLateJoinSnapshotBytes =
        16U * 1024U * 1024U;
    if (snapshot.empty() || snapshot.size() > maxLateJoinSnapshotBytes)
    {
        error = "persistent-world snapshot is empty or exceeds 16 MiB";
        return false;
    }
    AutomatiaSave::Json document;
    try
    {
        document = AutomatiaSave::Json::parse(snapshot);
    }
    catch (const std::exception& exception)
    {
        error = "unable to parse persistent-world snapshot: "
            + std::string(exception.what());
        return false;
    }
#ifdef SAM_FRAMEWORK_ENABLED
    if (!document.contains("sam_fingerprint")
        || !document["sam_fingerprint"].is_string()
        || document["sam_fingerprint"].get<std::string>()
            != SAMSync::generateFingerprint())
    {
        error = "persistent-world snapshot S.A.M. fingerprint does not match";
        return false;
    }
#endif
    return stageAutomatiaPersistentWorldDocument(
        std::move(document), sessionId, error);
}

void discardAutomatiaPersistentWorldSnapshot()
{
	pendingAutomatiaWorldDocument = AutomatiaSave::Json{};
}

bool loadAutomatiaPersistentWorldSave(
    const char* path,
    const std::string& sessionId,
    std::string& error
)
{
    AutomatiaSave::Json document;
    const AutomatiaSave::Result result = AutomatiaSave::load(path, document);
    if (!result.ok)
    {
        error = result.error;
        return false;
    }
    return stageAutomatiaPersistentWorldDocument(
        std::move(document), sessionId, error);
}

/*
 * Produce the canonical identity of the currently active map. The WorldState
 * registry is authoritative; the filename fallback keeps early menu/editor
 * loading paths compatible until every load is registered.
 */
static std::string getPersistentMapKey()
{
    if ( const WorldInstanceIdentity* identity = worldState.activeIdentity() )
    {
        return identity->key();
    }
    if ( map.filename[0] != '\0' )
    {
        return normalizePersistentMapKey(map.filename);
    }
    if ( map.name[0] != '\0' )
    {
        return normalizePersistentMapKey(map.name);
    }

    printlog(
        "[Persistent World] Warning: current map has no map-instance identity."
    );
    return "";
}

static void capturePersistentMinimap(
    const bool requirePlayerOnActiveMap
)
{
    const int playerIndex = persistentMinimapLocalPlayerIndex();
    const std::string mapKey = getPersistentMapKey();
    migratePersistentMinimapFallbackIdentity(playerIndex);
    const std::string playerIdentity =
        persistentMinimapPlayerIdentity(playerIndex);
    if (playerIndex < 0 || mapKey.empty() || playerIdentity.empty()
        || map.width <= 0 || map.height <= 0
        || map.width > MINIMAP_MAX_DIMENSION
        || map.height > MINIMAP_MAX_DIMENSION)
    {
        return;
    }
    if (requirePlayerOnActiveMap
        && players[playerIndex]
        && players[playerIndex]->worldInstance.isValid()
        && players[playerIndex]->worldInstance.key() != mapKey)
    {
        return;
    }
    PersistentMinimapState& saved =
        persistentPlayerMinimapRegistry[playerIdentity][mapKey];
    const std::size_t tileCount =
        static_cast<std::size_t>(map.width)
        * static_cast<std::size_t>(map.height);
    std::size_t changed = 0;
    if (saved.width != map.width || saved.height != map.height
        || saved.tiles.size() != tileCount)
    {
        saved.width = map.width;
        saved.height = map.height;
        saved.tiles.assign(tileCount, 0);
    }
    auto legacy = persistentLegacyMinimapRegistry.find(mapKey);
    if (legacy != persistentLegacyMinimapRegistry.end()
        && legacy->second.width == map.width
        && legacy->second.height == map.height
        && legacy->second.tiles.size() == tileCount)
    {
        for (std::size_t index = 0; index < tileCount; ++index)
        {
            if (saved.tiles[index] == 0 && legacy->second.tiles[index] != 0)
            {
                saved.tiles[index] = legacy->second.tiles[index];
                ++changed;
            }
        }
        persistentLegacyMinimapRegistry.erase(legacy);
    }
    std::size_t discovered = 0;
    for (Sint32 y = 0; y < map.height; ++y)
    {
        for (Sint32 x = 0; x < map.width; ++x)
        {
            const std::size_t index =
                static_cast<std::size_t>(x + y * map.width);
            const Sint8 value = minimap[y][x];
            if (value > 0 && value <= 4
                && saved.tiles[index] != value)
            {
                saved.tiles[index] = value;
                ++changed;
            }
            if (saved.tiles[index] != 0)
            {
                ++discovered;
            }
        }
    }
    if (changed > 0)
    {
        printlog(
            "[Persistent Minimap] Captured %zu new/updated tile(s), %zu total for player %d in '%s'.",
            changed, discovered, playerIndex, mapKey.c_str());
    }
}

static void restorePersistentMinimap()
{
    const int playerIndex = persistentMinimapLocalPlayerIndex();
    const std::string mapKey = getPersistentMapKey();
    if (playerIndex < 0 || mapKey.empty() || map.width <= 0 || map.height <= 0
        || map.width > MINIMAP_MAX_DIMENSION
        || map.height > MINIMAP_MAX_DIMENSION)
    {
        return;
    }

    const WorldInstanceIdentity* activeIdentity =
        worldState.activeIdentity();
    const std::uint64_t mapRevision = activeIdentity
        ? activeIdentity->revision
        : 0;
    if (persistentMinimapClearedMapKey != mapKey
        || persistentMinimapClearedMapRevision != mapRevision)
    {
        std::memset(minimap, 0, sizeof(minimap));
        persistentMinimapClearedMapKey = mapKey;
        persistentMinimapClearedMapRevision = mapRevision;
        printlog(
            "[Persistent Minimap] Cleared live minimap for newly loaded map '%s' revision %llu.",
            mapKey.c_str(),
            static_cast<unsigned long long>(mapRevision));
    }

    PersistentMinimapState* found =
        persistentMinimapStateForPlayer(playerIndex, mapKey);
    auto legacy = persistentLegacyMinimapRegistry.find(mapKey);
    if (!found && legacy != persistentLegacyMinimapRegistry.end())
    {
        found = &legacy->second;
    }
    const std::size_t tileCount =
        static_cast<std::size_t>(map.width)
        * static_cast<std::size_t>(map.height);
    if (!found || found->width != map.width || found->height != map.height
        || found->tiles.size() != tileCount)
    {
        return;
    }
    std::size_t restored = 0;
    for (Sint32 y = 0; y < map.height; ++y)
    {
        for (Sint32 x = 0; x < map.width; ++x)
        {
            const Sint8 value =
                found->tiles[static_cast<std::size_t>(x + y * map.width)];
            if (value != 0 && minimap[y][x] == 0)
            {
                minimap[y][x] = value;
                ++restored;
            }
        }
    }
    printlog(
        "[Persistent Minimap] Restored %zu discovered tile(s) for player %d in '%s'.",
        restored, playerIndex, mapKey.c_str());
}

void restoreAutomatiaPersistentMinimapForLocalPlayer()
{
    restorePersistentMinimap();
}

bool exportAutomatiaPersistentMinimapSnapshot(
    const int playerIndex,
    std::string& mapKey,
    Sint32& width,
    Sint32& height,
    std::vector<Sint8>& tiles
)
{
    mapKey.clear();
    width = 0;
    height = 0;
    tiles.clear();
    if (playerIndex != persistentMinimapLocalPlayerIndex())
    {
        return false;
    }
    capturePersistentMinimap();
    mapKey = getPersistentMapKey();
    PersistentMinimapState* state =
        persistentMinimapStateForPlayer(playerIndex, mapKey);
    if (!state)
    {
        return false;
    }
    width = state->width;
    height = state->height;
    tiles = state->tiles;
    return true;
}

bool importAutomatiaPersistentMinimapSnapshot(
    const int playerIndex,
    const std::string& requestedMapKey,
    const Sint32 width,
    const Sint32 height,
    const std::vector<Sint8>& tiles,
    const bool allowResize
)
{
    migratePersistentMinimapFallbackIdentity(playerIndex);
    const std::string playerIdentity =
        persistentMinimapPlayerIdentity(playerIndex);
    const std::string mapKey =
        normalizePersistentMapKey(requestedMapKey);
    const std::size_t tileCount =
        width > 0 && height > 0
        ? static_cast<std::size_t>(width)
            * static_cast<std::size_t>(height)
        : 0;
    if (playerIdentity.empty() || mapKey.empty()
        || width <= 0 || height <= 0
        || width > MINIMAP_MAX_DIMENSION
        || height > MINIMAP_MAX_DIMENSION
        || tiles.size() != tileCount)
    {
        return false;
    }
    PersistentMinimapMapRegistry& playerMaps =
        persistentPlayerMinimapRegistry[playerIdentity];
    auto found = playerMaps.find(mapKey);
    if (found == playerMaps.end())
    {
        if (!allowResize || playerMaps.size() >= 4096)
        {
            return false;
        }
        PersistentMinimapState initial;
        initial.width = width;
        initial.height = height;
        initial.tiles.assign(tileCount, 0);
        found = playerMaps.emplace(mapKey, std::move(initial)).first;
    }
    PersistentMinimapState& saved = found->second;
    if (saved.width != width || saved.height != height
        || saved.tiles.size() != tileCount)
    {
        if (!allowResize)
        {
            return false;
        }
        saved.width = width;
        saved.height = height;
        saved.tiles.assign(tileCount, 0);
    }
    persistentMapRemovalRegistry[mapKey];
    std::size_t discovered = 0;
    for (std::size_t index = 0; index < tileCount; ++index)
    {
        const Sint8 value = tiles[index];
        if (value > 0 && value <= 4)
        {
            saved.tiles[index] = value;
        }
        if (saved.tiles[index] != 0)
        {
            ++discovered;
        }
    }
    printlog(
        "[Persistent Minimap] Server stored %zu discovered tile(s) for player %d in '%s'.",
        discovered, playerIndex, mapKey.c_str());
    return true;
}


/*
 * Normalize a creator-defined dialogue, quest, flag, variable, choice,
 * objective, or node ID.
 *
 * IDs are case-insensitive. Spaces and unsupported punctuation become
 * underscores so editor-authored names remain safe and predictable.
 *
 * Examples:
 *
 * "Farmhouse Quest"        -> "farmhouse_quest"
 * "Bandit-Leader Dead"     -> "bandit-leader_dead"
 * "  Mara Trust  "         -> "mara_trust"
 */
static std::string normalizePersistentStoryID(
    std::string id
)
{
    std::string normalized;
    normalized.reserve(id.length());

    bool previousWasSeparator = false;

    for ( const unsigned char character : id )
    {
        if ( std::isalnum(character)
            || character == '_'
            || character == '-'
            || character == '.' )
        {
            normalized.push_back(
                static_cast<char>(
                    std::tolower(character)
                )
            );

            previousWasSeparator = false;
        }
        else
        {
            /*
             * Convert whitespace and unsupported punctuation into one
             * underscore rather than generating repeated separators.
             */
            if ( !normalized.empty()
                && !previousWasSeparator )
            {
                normalized.push_back('_');
                previousWasSeparator = true;
            }
        }
    }

    while ( !normalized.empty()
        && normalized.back() == '_' )
    {
        normalized.pop_back();
    }

    return normalized;
}

/*
 * Produce the stable registry key for one original map NPC.
 */
static std::string makePersistentNPCDialogueKey(
    const std::string& mapName,
    const Sint32 persistentID
)
{
    if ( persistentID <= 0 )
    {
        return "";
    }

    const std::string mapKey =
        normalizePersistentMapKey(mapName);

    if ( mapKey.empty() )
    {
        return "";
    }

    return
        mapKey
        + ":"
        + std::to_string(persistentID);
}
bool persistentStorySetWorldVariable(
    const std::string& variableID,
    const Sint32 value
)
{
    /*
     * The host owns persistent story progress.
     *
     * Remote clients will receive authoritative updates in a later
     * multiplayer stage.
     */
    if ( multiplayer == CLIENT )
    {
        return false;
    }

    const std::string key =
        normalizePersistentStoryID(variableID);

    if ( key.empty() )
    {
        return false;
    }

    persistentWorldStoryState.worldVariables[key] =
        value;

    return true;
}

Sint32 persistentStoryGetWorldVariable(
    const std::string& variableID,
    const Sint32 fallbackValue
)
{
    const std::string key =
        normalizePersistentStoryID(variableID);

    if ( key.empty() )
    {
        return fallbackValue;
    }

    const auto iterator =
        persistentWorldStoryState.worldVariables.find(
            key
        );

    if ( iterator
        == persistentWorldStoryState.worldVariables.end() )
    {
        return fallbackValue;
    }

    return iterator->second;
}

bool persistentStoryAddWorldVariable(
    const std::string& variableID,
    const Sint32 amount
)
{
    if ( multiplayer == CLIENT )
    {
        return false;
    }

    const std::string key =
        normalizePersistentStoryID(variableID);

    if ( key.empty() )
    {
        return false;
    }

    persistentWorldStoryState.worldVariables[key] +=
        amount;

    return true;
}

bool persistentStorySetWorldFlag(
    const std::string& flagID,
    const bool enabled
)
{
    if ( multiplayer == CLIENT )
    {
        return false;
    }

    const std::string key =
        normalizePersistentStoryID(flagID);

    if ( key.empty() )
    {
        return false;
    }

    if ( enabled )
    {
        persistentWorldStoryState.worldFlags.insert(
            key
        );
    }
    else
    {
        persistentWorldStoryState.worldFlags.erase(
            key
        );
    }

    return true;
}

bool persistentStoryGetWorldFlag(
    const std::string& flagID
)
{
    const std::string key =
        normalizePersistentStoryID(flagID);

    if ( key.empty() )
    {
        return false;
    }

    return
        persistentWorldStoryState.worldFlags.find(key)
        != persistentWorldStoryState.worldFlags.end();
}
static std::string makePersistentPlayerQuestKey(const int player, const std::string& questID)
{
    if ( player < 0 || player >= MAXPLAYERS ) return "";
    const std::string normalizedQuestID = normalizePersistentStoryID(questID);
    if ( normalizedQuestID.empty() ) return "";
    return "player_" + std::to_string(player) + ":" + normalizedQuestID;
}

/*
 * Scope-aware quest registry keys.
 *
 * player_<slot>:<quest>  - per-player quest state (existing)
 * world:<quest>          - shared by every player on the server
 * party:<quest>          - shared by the player's party (default: all connected)
 */
static std::string makePersistentScopedQuestKey(
    const std::string& scope,
    const int player,
    const std::string& questID
)
{
    const std::string normalizedQuestID = normalizePersistentStoryID(questID);
    if ( normalizedQuestID.empty() ) return "";
    if ( scope == "world" )
    {
        return "world:" + normalizedQuestID;
    }
    if ( scope == "party" )
    {
        if ( player < 0 || player >= MAXPLAYERS ) return "";
        return "party:" + normalizedQuestID;
    }
    return makePersistentPlayerQuestKey(player, questID);
}

static std::string makePersistentQuestKeyForDefinition(
    const std::string& scope,
    const int player,
    const std::string& questID
)
{
    return makePersistentScopedQuestKey(scope, player, questID);
}


bool persistentStorySetQuestVariable(
    const int player,
    const std::string& questID,
    const std::string& variableID,
    const Sint32 value
)
{
    if ( multiplayer == CLIENT ) return false;
    const std::string questKey = makePersistentPlayerQuestKey(player, questID);
    const std::string variableKey = normalizePersistentStoryID(variableID);
    if ( questKey.empty() || variableKey.empty() ) return false;
    PersistentQuestState& quest = persistentWorldStoryState.quests[questKey];
    quest.started = true;
    quest.variables[variableKey] = value;
    return true;
}

Sint32 persistentStoryGetQuestVariable(
    const int player,
    const std::string& questID,
    const std::string& variableID,
    const Sint32 fallbackValue
)
{
    const std::string questKey = makePersistentPlayerQuestKey(player, questID);
    const std::string variableKey = normalizePersistentStoryID(variableID);
    if ( questKey.empty() || variableKey.empty() ) return fallbackValue;
    const auto questIt = persistentWorldStoryState.quests.find(questKey);
    if ( questIt == persistentWorldStoryState.quests.end() ) return fallbackValue;
    const auto variableIt = questIt->second.variables.find(variableKey);
    return variableIt == questIt->second.variables.end()
        ? fallbackValue : variableIt->second;
}

bool persistentStoryAddQuestVariable(
    const int player,
    const std::string& questID,
    const std::string& variableID,
    const Sint32 amount
)
{
    return persistentStorySetQuestVariable(
        player, questID, variableID,
        persistentStoryGetQuestVariable(player, questID, variableID, 0) + amount
    );
}

bool persistentStorySetQuestObjectiveCompleted(
    const int player,
    const std::string& questID,
    const std::string& objectiveID,
    const bool completed
)
{
    if ( multiplayer == CLIENT )
    {
        return false;
    }

    const std::string questKey =
        makePersistentPlayerQuestKey(player, questID);

    const std::string objectiveKey =
        normalizePersistentStoryID(objectiveID);

    if ( questKey.empty()
        || objectiveKey.empty() )
    {
        return false;
    }

    PersistentQuestState& quest =
        persistentWorldStoryState.quests[questKey];

    quest.started = true;

    if ( completed )
    {
        quest.completedObjectives.insert(objectiveKey);
    }
    else
    {
        quest.completedObjectives.erase(objectiveKey);
    }

    return true;
}

bool persistentStoryQuestObjectiveIsCompleted(
    const int player,
    const std::string& questID,
    const std::string& objectiveID
)
{
    const std::string questKey =
        makePersistentPlayerQuestKey(player, questID);

    const std::string objectiveKey =
        normalizePersistentStoryID(objectiveID);

    if ( questKey.empty()
        || objectiveKey.empty() )
    {
        return false;
    }

    const auto iterator =
        persistentWorldStoryState.quests.find(questKey);

    if ( iterator
        == persistentWorldStoryState.quests.end() )
    {
        return false;
    }

    return
        iterator->second.completedObjectives.find(objectiveKey)
        != iterator->second.completedObjectives.end();
}

bool persistentStoryResetPlayerQuest(
    const int player,
    const std::string& questID
)
{
    if ( multiplayer == CLIENT )
    {
        return false;
    }

    const std::string questKey =
        makePersistentPlayerQuestKey(player, questID);

    if ( questKey.empty() )
    {
        return false;
    }

    persistentWorldStoryState.quests.erase(questKey);
    return true;
}

bool persistentStorySetQuestStage(const int player, const std::string& questID, const Sint32 stage)
{
    if ( multiplayer == CLIENT ) return false;
    const std::string key = makePersistentPlayerQuestKey(player, questID);
    if ( key.empty() ) return false;
    PersistentQuestState& quest = persistentWorldStoryState.quests[key];
    quest.started = true; quest.stage = stage; return true;
}
Sint32 persistentStoryGetQuestStage(const int player, const std::string& questID, const Sint32 fallbackStage)
{
    const std::string key = makePersistentPlayerQuestKey(player, questID);
    const auto it = persistentWorldStoryState.quests.find(key);
    return key.empty() || it == persistentWorldStoryState.quests.end() ? fallbackStage : it->second.stage;
}
bool persistentStorySetQuestStarted(const int player, const std::string& questID, const bool started)
{
    if ( multiplayer == CLIENT ) return false;
    const std::string key = makePersistentPlayerQuestKey(player, questID); if ( key.empty() ) return false;
    PersistentQuestState& q = persistentWorldStoryState.quests[key]; q.started = started; if ( !started ) q.accepted = false; return true;
}
bool persistentStorySetQuestAccepted(const int player, const std::string& questID, const bool accepted)
{
    if ( multiplayer == CLIENT ) return false;
    const std::string key = makePersistentPlayerQuestKey(player, questID); if ( key.empty() ) return false;
    PersistentQuestState& q = persistentWorldStoryState.quests[key]; if ( accepted && q.completed ) return false;
    q.started = true; q.accepted = accepted; return true;
}
bool persistentStorySetQuestCompleted(const int player, const std::string& questID, const bool completed)
{
    if ( multiplayer == CLIENT ) return false;
    const std::string key = makePersistentPlayerQuestKey(player, questID); if ( key.empty() ) return false;
    PersistentQuestState& q = persistentWorldStoryState.quests[key]; q.started = true; q.completed = completed;
    if ( completed ) { q.accepted = false; q.failed = false; } return true;
}
bool persistentStorySetQuestFailed(const int player, const std::string& questID, const bool failed)
{
    if ( multiplayer == CLIENT ) return false;
    const std::string key = makePersistentPlayerQuestKey(player, questID); if ( key.empty() ) return false;
    PersistentQuestState& q = persistentWorldStoryState.quests[key]; q.started = true; q.failed = failed;
    if ( failed ) { q.accepted = false; q.completed = false; } return true;
}
bool persistentStoryQuestIsStarted(const int player, const std::string& questID)
{
    const std::string key = makePersistentPlayerQuestKey(player, questID); const auto it = persistentWorldStoryState.quests.find(key);
    return !key.empty() && it != persistentWorldStoryState.quests.end() && it->second.started;
}
bool persistentStoryQuestIsAccepted(const int player, const std::string& questID)
{
    const std::string key = makePersistentPlayerQuestKey(player, questID); const auto it = persistentWorldStoryState.quests.find(key);
    return !key.empty() && it != persistentWorldStoryState.quests.end() && it->second.accepted;
}
bool persistentStoryQuestIsCompleted(const int player, const std::string& questID)
{
    const std::string key = makePersistentPlayerQuestKey(player, questID); const auto it = persistentWorldStoryState.quests.find(key);
    return !key.empty() && it != persistentWorldStoryState.quests.end() && it->second.completed;
}
bool persistentStoryQuestIsFailed(const int player, const std::string& questID)
{
    const std::string key = makePersistentPlayerQuestKey(player, questID); const auto it = persistentWorldStoryState.quests.find(key);
    return !key.empty() && it != persistentWorldStoryState.quests.end() && it->second.failed;
}

bool persistentStorySetQuestStage(
    const std::string& questID,
    const Sint32 stage
)
{
    if ( multiplayer == CLIENT )
    {
        return false;
    }

    const std::string key =
        normalizePersistentStoryID(questID);

    if ( key.empty() )
    {
        return false;
    }

    PersistentQuestState& quest =
        persistentWorldStoryState.quests[key];

    quest.started = true;
    quest.stage = stage;

    return true;
}

Sint32 persistentStoryGetQuestStage(
    const std::string& questID,
    const Sint32 fallbackStage
)
{
    const std::string key =
        normalizePersistentStoryID(questID);

    if ( key.empty() )
    {
        return fallbackStage;
    }

    const auto iterator =
        persistentWorldStoryState.quests.find(key);

    if ( iterator
        == persistentWorldStoryState.quests.end() )
    {
        return fallbackStage;
    }

    return iterator->second.stage;
}

bool persistentStorySetQuestStarted(
    const std::string& questID,
    const bool started
)
{
    if ( multiplayer == CLIENT )
    {
        return false;
    }

    const std::string key =
        normalizePersistentStoryID(questID);

    if ( key.empty() )
    {
        return false;
    }

    PersistentQuestState& quest =
        persistentWorldStoryState.quests[key];

    quest.started = started;

    if ( !started )
    {
        quest.accepted = false;
    }

    return true;
}

bool persistentStorySetQuestAccepted(
    const std::string& questID,
    const bool accepted
)
{
    if ( multiplayer == CLIENT )
    {
        return false;
    }

    const std::string key =
        normalizePersistentStoryID(questID);

    if ( key.empty() )
    {
        return false;
    }

    PersistentQuestState& quest =
        persistentWorldStoryState.quests[key];

    quest.started = true;
    quest.accepted = accepted;

    return true;
}

bool persistentStorySetQuestCompleted(
    const std::string& questID,
    const bool completed
)
{
    if ( multiplayer == CLIENT )
    {
        return false;
    }

    const std::string key =
        normalizePersistentStoryID(questID);

    if ( key.empty() )
    {
        return false;
    }

    PersistentQuestState& quest =
        persistentWorldStoryState.quests[key];

    quest.started = true;
    quest.completed = completed;

    if ( completed )
    {
        quest.failed = false;
    }

    return true;
}

bool persistentStorySetQuestFailed(
    const std::string& questID,
    const bool failed
)
{
    if ( multiplayer == CLIENT )
    {
        return false;
    }

    const std::string key =
        normalizePersistentStoryID(questID);

    if ( key.empty() )
    {
        return false;
    }

    PersistentQuestState& quest =
        persistentWorldStoryState.quests[key];

    quest.started = true;
    quest.failed = failed;

    if ( failed )
    {
        quest.completed = false;
    }

    return true;
}

bool persistentStoryQuestIsStarted(
    const std::string& questID
)
{
    const std::string key =
        normalizePersistentStoryID(questID);

    const auto iterator =
        persistentWorldStoryState.quests.find(key);

    return
        !key.empty()
        && iterator
            != persistentWorldStoryState.quests.end()
        && iterator->second.started;
}

bool persistentStoryQuestIsAccepted(
    const std::string& questID
)
{
    const std::string key =
        normalizePersistentStoryID(questID);

    const auto iterator =
        persistentWorldStoryState.quests.find(key);

    return
        !key.empty()
        && iterator
            != persistentWorldStoryState.quests.end()
        && iterator->second.accepted;
}

bool persistentStoryQuestIsCompleted(
    const std::string& questID
)
{
    const std::string key =
        normalizePersistentStoryID(questID);

    const auto iterator =
        persistentWorldStoryState.quests.find(key);

    return
        !key.empty()
        && iterator
            != persistentWorldStoryState.quests.end()
        && iterator->second.completed;
}

bool persistentStoryQuestIsFailed(
    const std::string& questID
)
{
    const std::string key =
        normalizePersistentStoryID(questID);

    const auto iterator =
        persistentWorldStoryState.quests.find(key);

    return
        !key.empty()
        && iterator
            != persistentWorldStoryState.quests.end()
        && iterator->second.failed;
}
bool persistentStorySetQuestVariable(
    const std::string& questID,
    const std::string& variableID,
    const Sint32 value
)
{
    if ( multiplayer == CLIENT )
    {
        return false;
    }

    const std::string questKey =
        normalizePersistentStoryID(questID);

    const std::string variableKey =
        normalizePersistentStoryID(variableID);

    if ( questKey.empty()
        || variableKey.empty() )
    {
        return false;
    }

    PersistentQuestState& quest =
        persistentWorldStoryState.quests[questKey];

    quest.started = true;
    quest.variables[variableKey] = value;

    return true;
}

Sint32 persistentStoryGetQuestVariable(
    const std::string& questID,
    const std::string& variableID,
    const Sint32 fallbackValue
)
{
    const std::string questKey =
        normalizePersistentStoryID(questID);

    const std::string variableKey =
        normalizePersistentStoryID(variableID);

    if ( questKey.empty()
        || variableKey.empty() )
    {
        return fallbackValue;
    }

    const auto questIterator =
        persistentWorldStoryState.quests.find(
            questKey
        );

    if ( questIterator
        == persistentWorldStoryState.quests.end() )
    {
        return fallbackValue;
    }

    const auto variableIterator =
        questIterator->second.variables.find(
            variableKey
        );

    if ( variableIterator
        == questIterator->second.variables.end() )
    {
        return fallbackValue;
    }

    return variableIterator->second;
}

bool persistentStorySetQuestFlag(
    const std::string& questID,
    const std::string& flagID,
    const bool enabled
)
{
    if ( multiplayer == CLIENT )
    {
        return false;
    }

    const std::string questKey =
        normalizePersistentStoryID(questID);

    const std::string flagKey =
        normalizePersistentStoryID(flagID);

    if ( questKey.empty()
        || flagKey.empty() )
    {
        return false;
    }

    PersistentQuestState& quest =
        persistentWorldStoryState.quests[questKey];

    quest.started = true;

    if ( enabled )
    {
        quest.flags.insert(flagKey);
    }
    else
    {
        quest.flags.erase(flagKey);
    }

    return true;
}

bool persistentStoryGetQuestFlag(
    const std::string& questID,
    const std::string& flagID
)
{
    const std::string questKey =
        normalizePersistentStoryID(questID);

    const std::string flagKey =
        normalizePersistentStoryID(flagID);

    if ( questKey.empty()
        || flagKey.empty() )
    {
        return false;
    }

    const auto questIterator =
        persistentWorldStoryState.quests.find(
            questKey
        );

    if ( questIterator
        == persistentWorldStoryState.quests.end() )
    {
        return false;
    }

    return
        questIterator->second.flags.find(flagKey)
        != questIterator->second.flags.end();
}

bool persistentStorySetQuestObjectiveComplete(
    const std::string& questID,
    const std::string& objectiveID,
    const bool completed
)
{
    if ( multiplayer == CLIENT )
    {
        return false;
    }

    const std::string questKey =
        normalizePersistentStoryID(questID);

    const std::string objectiveKey =
        normalizePersistentStoryID(objectiveID);

    if ( questKey.empty()
        || objectiveKey.empty() )
    {
        return false;
    }

    PersistentQuestState& quest =
        persistentWorldStoryState.quests[questKey];

    quest.started = true;

    if ( completed )
    {
        quest.completedObjectives.insert(
            objectiveKey
        );
    }
    else
    {
        quest.completedObjectives.erase(
            objectiveKey
        );
    }

    return true;
}

bool persistentStoryQuestObjectiveIsComplete(
    const std::string& questID,
    const std::string& objectiveID
)
{
    const std::string questKey =
        normalizePersistentStoryID(questID);

    const std::string objectiveKey =
        normalizePersistentStoryID(objectiveID);

    if ( questKey.empty()
        || objectiveKey.empty() )
    {
        return false;
    }

    const auto questIterator =
        persistentWorldStoryState.quests.find(
            questKey
        );

    if ( questIterator
        == persistentWorldStoryState.quests.end() )
    {
        return false;
    }

    return
        questIterator->second.completedObjectives.find(
            objectiveKey
        )
        != questIterator->second.completedObjectives.end();
}

bool persistentStorySetQuestChoiceUsed(
    const std::string& questID,
    const std::string& choiceID,
    const bool used
)
{
    if ( multiplayer == CLIENT )
    {
        return false;
    }

    const std::string questKey =
        normalizePersistentStoryID(questID);

    const std::string choiceKey =
        normalizePersistentStoryID(choiceID);

    if ( questKey.empty()
        || choiceKey.empty() )
    {
        return false;
    }

    PersistentQuestState& quest =
        persistentWorldStoryState.quests[questKey];

    quest.started = true;

    if ( used )
    {
        quest.usedChoices.insert(choiceKey);
    }
    else
    {
        quest.usedChoices.erase(choiceKey);
    }

    return true;
}

bool persistentStoryQuestChoiceWasUsed(
    const std::string& questID,
    const std::string& choiceID
)
{
    const std::string questKey =
        normalizePersistentStoryID(questID);

    const std::string choiceKey =
        normalizePersistentStoryID(choiceID);

    if ( questKey.empty()
        || choiceKey.empty() )
    {
        return false;
    }

    const auto questIterator =
        persistentWorldStoryState.quests.find(
            questKey
        );

    if ( questIterator
        == persistentWorldStoryState.quests.end() )
    {
        return false;
    }

    return
        questIterator->second.usedChoices.find(
            choiceKey
        )
        != questIterator->second.usedChoices.end();
}
bool persistentStoryAssignNPCDialogue(
    const std::string& mapName,
    const Sint32 persistentID,
    const std::string& dialogueID
)
{
    if ( multiplayer == CLIENT )
    {
        return false;
    }

    const std::string npcKey =
        makePersistentNPCDialogueKey(
            mapName,
            persistentID
        );

    const std::string dialogueKey =
        normalizePersistentStoryID(dialogueID);

    if ( npcKey.empty()
        || dialogueKey.empty() )
    {
        return false;
    }

    persistentWorldStoryState
        .npcDialogueStates[npcKey]
        .dialogueID = dialogueKey;

    return true;
}

std::string persistentStoryGetNPCDialogue(
    const std::string& mapName,
    const Sint32 persistentID
)
{
    const std::string npcKey =
        makePersistentNPCDialogueKey(
            mapName,
            persistentID
        );

    if ( npcKey.empty() )
    {
        return "";
    }

    const auto iterator =
        persistentWorldStoryState
            .npcDialogueStates
            .find(npcKey);

    if ( iterator
        == persistentWorldStoryState
            .npcDialogueStates
            .end() )
    {
        return "";
    }

    return iterator->second.dialogueID;
}


/* Player-scoped mutable NPC dialogue memory. */
static std::string makePersistentPlayerNPCMapNamespace(
    const int player,
    const std::string& mapName
)
{
    if ( player < 0 || player >= MAXPLAYERS )
    {
        return "";
    }

    const std::string normalizedMap =
        normalizePersistentMapKey(mapName);
    if ( normalizedMap.empty() )
    {
        return "";
    }

    /*
     * Do not use a slash here. WorldInstanceIdentity::canonicalMapFile()
     * intentionally strips directory prefixes, which previously collapsed
     * every player-scoped NPC key back onto the shared map key. The encoded
     * prefix survives the existing map-key normalizer and keeps each
     * character's dialogue memory independent.
     */
    return "player_" + std::to_string(player) + "__" + normalizedMap;
}

static std::unordered_set<std::string>
    automatiaPlayerQuestDialogueIDs[MAXPLAYERS];

static std::string automatiaPlayerQuestStoryPrefix(const int player)
{
    return "player_" + std::to_string(player) + ":";
}

static std::string automatiaPlayerNPCStoryPrefix(const int player)
{
    return "player_" + std::to_string(player) + "__";
}

static bool automatiaStringHasPrefix(
    const std::string& value,
    const std::string& prefix)
{
    return value.size() >= prefix.size()
        && value.compare(0, prefix.size(), prefix) == 0;
}

static void clearAutomatiaPlayerStoryState(const int player)
{
    automatiaPlayerQuestDialogueIDs[player].clear();

    const std::string questPrefix =
        automatiaPlayerQuestStoryPrefix(player);
    for (auto iterator = persistentWorldStoryState.quests.begin();
        iterator != persistentWorldStoryState.quests.end(); )
    {
        if (automatiaStringHasPrefix(iterator->first, questPrefix))
        {
            iterator = persistentWorldStoryState.quests.erase(iterator);
        }
        else
        {
            ++iterator;
        }
    }

    const std::string npcPrefix =
        automatiaPlayerNPCStoryPrefix(player);
    for (auto iterator = persistentWorldStoryState.npcDialogueStates.begin();
        iterator != persistentWorldStoryState.npcDialogueStates.end(); )
    {
        if (automatiaStringHasPrefix(iterator->first, npcPrefix))
        {
            iterator =
                persistentWorldStoryState.npcDialogueStates.erase(iterator);
        }
        else
        {
            ++iterator;
        }
    }
}

void resetAutomatiaPlayerStoryState(int player)
{
    if (multiplayer == CLIENT || player < 0 || player >= MAXPLAYERS)
    {
        return;
    }
    clearAutomatiaPlayerStoryState(player);
}

bool exportAutomatiaPlayerStoryState(
    int player,
    std::string& payload,
    std::string& error)
{
    payload.clear();
    error.clear();
    if (multiplayer == CLIENT || player < 0 || player >= MAXPLAYERS)
    {
        error = "player story export requires an authoritative server slot";
        return false;
    }

    using AutomatiaSave::Json;
    const auto sortedStringSet = [](const auto& values)
    {
        std::vector<std::string> sorted(values.begin(), values.end());
        std::sort(sorted.begin(), sorted.end());
        return sorted;
    };
    Json document = {
        {"version", 1},
        {"quests", Json::object()},
        {"dialogue", Json::object()},
        {"quest_dialogue_ids", Json::array()}
    };

    std::vector<std::string> liveQuestDialogueIDs;
    collectCustomDialogueQuestDefinitionIDsForPlayer(
        player,
        liveQuestDialogueIDs
    );
    for ( const std::string& dialogueID : liveQuestDialogueIDs )
    {
        if ( safeSavedStoryId(dialogueID) )
        {
            automatiaPlayerQuestDialogueIDs[player].insert(dialogueID);
        }
    }
    document["quest_dialogue_ids"] =
        sortedStringSet(automatiaPlayerQuestDialogueIDs[player]);

    const std::string questPrefix =
        automatiaPlayerQuestStoryPrefix(player);
    std::size_t questCount = 0;
    for (const auto& entry : persistentWorldStoryState.quests)
    {
        if (!automatiaStringHasPrefix(entry.first, questPrefix))
        {
            continue;
        }
        const std::string stableKey = entry.first.substr(questPrefix.size());
        if (!safeSavedStoryId(stableKey))
        {
            continue;
        }
        const PersistentQuestState& quest = entry.second;
        document["quests"][stableKey] = Json{
            {"started", quest.started},
            {"accepted", quest.accepted},
            {"completed", quest.completed},
            {"failed", quest.failed},
            {"stage", quest.stage},
            {"variables", quest.variables},
            {"flags", sortedStringSet(quest.flags)},
            {"completed_objectives", sortedStringSet(quest.completedObjectives)},
            {"used_choices", sortedStringSet(quest.usedChoices)}
        };
        ++questCount;
    }

    const std::string npcPrefix =
        automatiaPlayerNPCStoryPrefix(player);
    std::size_t dialogueCount = 0;
    for (const auto& entry : persistentWorldStoryState.npcDialogueStates)
    {
        if (!automatiaStringHasPrefix(entry.first, npcPrefix))
        {
            continue;
        }
        const std::string stableKey = entry.first.substr(npcPrefix.size());
        if (!safeSavedStoryId(stableKey))
        {
            continue;
        }
        const PersistentNPCDialogueState& dialogue = entry.second;
        document["dialogue"][stableKey] = Json{
            {"dialogue_id", dialogue.dialogueID},
            {"current_node", dialogue.currentNode},
            {"conversation_started", dialogue.conversationStarted},
            {"reward_given", dialogue.rewardGiven},
            {"variables", dialogue.variables},
            {"flags", sortedStringSet(dialogue.flags)},
            {"used_choices", sortedStringSet(dialogue.usedChoices)},
            {"seen_nodes", sortedStringSet(dialogue.seenNodes)}
        };
        ++dialogueCount;
    }

    try
    {
        payload = document.dump();
    }
    catch (const std::exception& exception)
    {
        error = "could not serialize player story state: "
            + std::string(exception.what());
        return false;
    }
    constexpr std::size_t maxPlayerStoryBytes = 1024U * 1024U;
    if (payload.size() > maxPlayerStoryBytes)
    {
        payload.clear();
        error = "player story state exceeds the 1 MiB character-save limit";
        return false;
    }

    printlog(
        "[Character Save] Captured %zu quest(s) and %zu NPC memory record(s) for player %d.",
        questCount,
        dialogueCount,
        player);
    return true;
}

bool importAutomatiaPlayerStoryState(
    int player,
    const std::string& payload,
    std::string& error)
{
    error.clear();
    if (player < 0 || player >= MAXPLAYERS
        || (multiplayer == CLIENT && player != clientnum))
    {
        error = "player story import requires the authoritative server or the owning client slot";
        return false;
    }
    constexpr std::size_t maxPlayerStoryBytes = 1024U * 1024U;
    if (payload.empty() || payload.size() > maxPlayerStoryBytes)
    {
        error = "player story payload is empty or exceeds 1 MiB";
        return false;
    }

    using AutomatiaSave::Json;
    Json document;
    try
    {
        document = Json::parse(payload);
    }
    catch (const std::exception& exception)
    {
        error = "could not parse player story state: "
            + std::string(exception.what());
        return false;
    }
    if (!document.is_object()
        || !document.contains("version")
        || !document["version"].is_number_integer()
        || document["version"].get<int>() != 1
        || !document.contains("quests")
        || !document["quests"].is_object()
        || !document.contains("dialogue")
        || !document["dialogue"].is_object()
        || (document.contains("quest_dialogue_ids")
            && (!document["quest_dialogue_ids"].is_array()
                || document["quest_dialogue_ids"].size() > 65536))
        || document["quests"].size() > 65536
        || document["dialogue"].size() > 65536)
    {
        error = "player story payload has an invalid structure";
        return false;
    }

    clearAutomatiaPlayerStoryState(player);

    std::size_t questDefinitionCount = 0;
    if ( document.contains("quest_dialogue_ids") )
    {
        for ( const Json& dialogueIDValue : document["quest_dialogue_ids"] )
        {
            if ( !dialogueIDValue.is_string() )
            {
                continue;
            }
            const std::string dialogueID =
                dialogueIDValue.get<std::string>();
            if ( !safeSavedStoryId(dialogueID) )
            {
                continue;
            }
            automatiaPlayerQuestDialogueIDs[player].insert(dialogueID);
            if ( preloadCustomDialogueQuestDefinition(dialogueID) )
            {
                ++questDefinitionCount;
            }
            else
            {
                printlog(
                    "[Custom Dialogue] Player %d story state references unavailable dialogue definition '%s'.",
                    player,
                    dialogueID.c_str()
                );
            }
        }
    }

    const std::string questPrefix =
        automatiaPlayerQuestStoryPrefix(player);
    std::size_t questCount = 0;
    for (auto member = document["quests"].begin();
        member != document["quests"].end(); ++member)
    {
        if (!safeSavedStoryId(member.key()) || !member.value().is_object())
        {
            continue;
        }
        PersistentQuestState quest;
        savedJsonBool(member.value(), "started", quest.started);
        savedJsonBool(member.value(), "accepted", quest.accepted);
        savedJsonBool(member.value(), "completed", quest.completed);
        savedJsonBool(member.value(), "failed", quest.failed);
        savedJsonInt(member.value(), "stage", quest.stage);
        restoreSavedVariables(member.value(), "variables", quest.variables);
        restoreSavedStringSet(member.value(), "flags", quest.flags);
        restoreSavedStringSet(
            member.value(), "completed_objectives", quest.completedObjectives);
        restoreSavedStringSet(
            member.value(), "used_choices", quest.usedChoices);
        persistentWorldStoryState.quests[questPrefix + member.key()] =
            std::move(quest);
        ++questCount;
    }

    const std::string npcPrefix =
        automatiaPlayerNPCStoryPrefix(player);
    std::size_t dialogueCount = 0;
    for (auto member = document["dialogue"].begin();
        member != document["dialogue"].end(); ++member)
    {
        if (!safeSavedStoryId(member.key()) || !member.value().is_object())
        {
            continue;
        }
        PersistentNPCDialogueState dialogue;
        if (member.value().contains("dialogue_id")
            && member.value()["dialogue_id"].is_string()
            && member.value()["dialogue_id"].get<std::string>().size() <= 255)
        {
            dialogue.dialogueID =
                member.value()["dialogue_id"].get<std::string>();
        }
        savedJsonInt(member.value(), "current_node", dialogue.currentNode);
        savedJsonBool(
            member.value(), "conversation_started", dialogue.conversationStarted);
        savedJsonBool(member.value(), "reward_given", dialogue.rewardGiven);
        restoreSavedVariables(member.value(), "variables", dialogue.variables);
        restoreSavedStringSet(member.value(), "flags", dialogue.flags);
        restoreSavedStringSet(
            member.value(), "used_choices", dialogue.usedChoices);
        restoreSavedStringSet(
            member.value(), "seen_nodes", dialogue.seenNodes);
        persistentWorldStoryState.npcDialogueStates[npcPrefix + member.key()] =
            std::move(dialogue);
        ++dialogueCount;
    }

    printlog(
        "[Character Save] Restored %zu quest(s), %zu NPC memory record(s), and %zu quest definition(s) for player %d.",
        questCount,
        dialogueCount,
        questDefinitionCount,
        player);
    return true;
}

bool persistentStorySetNPCNode(const int player, const std::string& mapName, const Sint32 persistentID, const Sint32 nodeID)
{
    const std::string scopedMap = makePersistentPlayerNPCMapNamespace(player, mapName);
    return !scopedMap.empty() && persistentStorySetNPCNode(scopedMap, persistentID, nodeID);
}

Sint32 persistentStoryGetNPCNode(const int player, const std::string& mapName, const Sint32 persistentID, const Sint32 fallbackNode)
{
    const std::string scopedMap = makePersistentPlayerNPCMapNamespace(player, mapName);
    return scopedMap.empty() ? fallbackNode : persistentStoryGetNPCNode(scopedMap, persistentID, fallbackNode);
}

bool persistentStorySetNPCVariable(const int player, const std::string& mapName, const Sint32 persistentID, const std::string& variableID, const Sint32 value)
{
    const std::string scopedMap = makePersistentPlayerNPCMapNamespace(player, mapName);
    return !scopedMap.empty() && persistentStorySetNPCVariable(scopedMap, persistentID, variableID, value);
}

Sint32 persistentStoryGetNPCVariable(const int player, const std::string& mapName, const Sint32 persistentID, const std::string& variableID, const Sint32 fallbackValue)
{
    const std::string scopedMap = makePersistentPlayerNPCMapNamespace(player, mapName);
    return scopedMap.empty() ? fallbackValue : persistentStoryGetNPCVariable(scopedMap, persistentID, variableID, fallbackValue);
}

bool persistentStorySetNPCFlag(const int player, const std::string& mapName, const Sint32 persistentID, const std::string& flagID, const bool enabled)
{
    const std::string scopedMap = makePersistentPlayerNPCMapNamespace(player, mapName);
    return !scopedMap.empty() && persistentStorySetNPCFlag(scopedMap, persistentID, flagID, enabled);
}

bool persistentStoryGetNPCFlag(const int player, const std::string& mapName, const Sint32 persistentID, const std::string& flagID)
{
    const std::string scopedMap = makePersistentPlayerNPCMapNamespace(player, mapName);
    return !scopedMap.empty() && persistentStoryGetNPCFlag(scopedMap, persistentID, flagID);
}

bool persistentStorySetNPCChoiceUsed(const int player, const std::string& mapName, const Sint32 persistentID, const std::string& choiceID, const bool used)
{
    const std::string scopedMap = makePersistentPlayerNPCMapNamespace(player, mapName);
    return !scopedMap.empty() && persistentStorySetNPCChoiceUsed(scopedMap, persistentID, choiceID, used);
}

bool persistentStoryNPCChoiceWasUsed(const int player, const std::string& mapName, const Sint32 persistentID, const std::string& choiceID)
{
    const std::string scopedMap = makePersistentPlayerNPCMapNamespace(player, mapName);
    return !scopedMap.empty() && persistentStoryNPCChoiceWasUsed(scopedMap, persistentID, choiceID);
}

bool persistentStorySetNPCNodeSeen(const int player, const std::string& mapName, const Sint32 persistentID, const std::string& nodeID, const bool seen)
{
    const std::string scopedMap = makePersistentPlayerNPCMapNamespace(player, mapName);
    return !scopedMap.empty() && persistentStorySetNPCNodeSeen(scopedMap, persistentID, nodeID, seen);
}

bool persistentStoryNPCNodeWasSeen(const int player, const std::string& mapName, const Sint32 persistentID, const std::string& nodeID)
{
    const std::string scopedMap = makePersistentPlayerNPCMapNamespace(player, mapName);
    return !scopedMap.empty() && persistentStoryNPCNodeWasSeen(scopedMap, persistentID, nodeID);
}

bool persistentStorySetNPCNode(
    const std::string& mapName,
    const Sint32 persistentID,
    const Sint32 nodeID
)
{
    if ( multiplayer == CLIENT )
    {
        return false;
    }

    const std::string npcKey =
        makePersistentNPCDialogueKey(
            mapName,
            persistentID
        );

    if ( npcKey.empty() )
    {
        return false;
    }

    PersistentNPCDialogueState& npcState =
        persistentWorldStoryState
            .npcDialogueStates[npcKey];

    npcState.currentNode = nodeID;
    npcState.conversationStarted = true;

    return true;
}

Sint32 persistentStoryGetNPCNode(
    const std::string& mapName,
    const Sint32 persistentID,
    const Sint32 fallbackNode
)
{
    const std::string npcKey =
        makePersistentNPCDialogueKey(
            mapName,
            persistentID
        );

    if ( npcKey.empty() )
    {
        return fallbackNode;
    }

    const auto iterator =
        persistentWorldStoryState
            .npcDialogueStates
            .find(npcKey);

    if ( iterator
        == persistentWorldStoryState
            .npcDialogueStates
            .end() )
    {
        return fallbackNode;
    }

    return iterator->second.currentNode;
}

bool persistentStorySetNPCVariable(
    const std::string& mapName,
    const Sint32 persistentID,
    const std::string& variableID,
    const Sint32 value
)
{
    if ( multiplayer == CLIENT )
    {
        return false;
    }

    const std::string npcKey =
        makePersistentNPCDialogueKey(
            mapName,
            persistentID
        );

    const std::string variableKey =
        normalizePersistentStoryID(variableID);

    if ( npcKey.empty()
        || variableKey.empty() )
    {
        return false;
    }

    persistentWorldStoryState
        .npcDialogueStates[npcKey]
        .variables[variableKey] = value;

    return true;
}

Sint32 persistentStoryGetNPCVariable(
    const std::string& mapName,
    const Sint32 persistentID,
    const std::string& variableID,
    const Sint32 fallbackValue
)
{
    const std::string npcKey =
        makePersistentNPCDialogueKey(
            mapName,
            persistentID
        );

    const std::string variableKey =
        normalizePersistentStoryID(variableID);

    if ( npcKey.empty()
        || variableKey.empty() )
    {
        return fallbackValue;
    }

    const auto npcIterator =
        persistentWorldStoryState
            .npcDialogueStates
            .find(npcKey);

    if ( npcIterator
        == persistentWorldStoryState
            .npcDialogueStates
            .end() )
    {
        return fallbackValue;
    }

    const auto variableIterator =
        npcIterator->second.variables.find(
            variableKey
        );

    if ( variableIterator
        == npcIterator->second.variables.end() )
    {
        return fallbackValue;
    }

    return variableIterator->second;
}

bool persistentStorySetNPCFlag(
    const std::string& mapName,
    const Sint32 persistentID,
    const std::string& flagID,
    const bool enabled
)
{
    if ( multiplayer == CLIENT )
    {
        return false;
    }

    const std::string npcKey =
        makePersistentNPCDialogueKey(
            mapName,
            persistentID
        );

    const std::string flagKey =
        normalizePersistentStoryID(flagID);

    if ( npcKey.empty()
        || flagKey.empty() )
    {
        return false;
    }

    PersistentNPCDialogueState& npcState =
        persistentWorldStoryState
            .npcDialogueStates[npcKey];

    if ( enabled )
    {
        npcState.flags.insert(flagKey);
    }
    else
    {
        npcState.flags.erase(flagKey);
    }

    return true;
}

bool persistentStoryGetNPCFlag(
    const std::string& mapName,
    const Sint32 persistentID,
    const std::string& flagID
)
{
    const std::string npcKey =
        makePersistentNPCDialogueKey(
            mapName,
            persistentID
        );

    const std::string flagKey =
        normalizePersistentStoryID(flagID);

    if ( npcKey.empty()
        || flagKey.empty() )
    {
        return false;
    }

    const auto npcIterator =
        persistentWorldStoryState
            .npcDialogueStates
            .find(npcKey);

    if ( npcIterator
        == persistentWorldStoryState
            .npcDialogueStates
            .end() )
    {
        return false;
    }

    return
        npcIterator->second.flags.find(flagKey)
        != npcIterator->second.flags.end();
}

bool persistentStorySetNPCChoiceUsed(
    const std::string& mapName,
    const Sint32 persistentID,
    const std::string& choiceID,
    const bool used
)
{
    if ( multiplayer == CLIENT )
    {
        return false;
    }

    const std::string npcKey =
        makePersistentNPCDialogueKey(
            mapName,
            persistentID
        );

    const std::string choiceKey =
        normalizePersistentStoryID(choiceID);

    if ( npcKey.empty()
        || choiceKey.empty() )
    {
        return false;
    }

    PersistentNPCDialogueState& npcState =
        persistentWorldStoryState
            .npcDialogueStates[npcKey];

    if ( used )
    {
        npcState.usedChoices.insert(choiceKey);
    }
    else
    {
        npcState.usedChoices.erase(choiceKey);
    }

    return true;
}

bool persistentStoryNPCChoiceWasUsed(
    const std::string& mapName,
    const Sint32 persistentID,
    const std::string& choiceID
)
{
    const std::string npcKey =
        makePersistentNPCDialogueKey(
            mapName,
            persistentID
        );

    const std::string choiceKey =
        normalizePersistentStoryID(choiceID);

    if ( npcKey.empty()
        || choiceKey.empty() )
    {
        return false;
    }

    const auto npcIterator =
        persistentWorldStoryState
            .npcDialogueStates
            .find(npcKey);

    if ( npcIterator
        == persistentWorldStoryState
            .npcDialogueStates
            .end() )
    {
        return false;
    }

    return
        npcIterator->second.usedChoices.find(
            choiceKey
        )
        != npcIterator->second.usedChoices.end();
}

bool persistentStorySetNPCNodeSeen(
    const std::string& mapName,
    const Sint32 persistentID,
    const std::string& nodeID,
    const bool seen
)
{
    if ( multiplayer == CLIENT )
    {
        return false;
    }

    const std::string npcKey =
        makePersistentNPCDialogueKey(
            mapName,
            persistentID
        );

    const std::string nodeKey =
        normalizePersistentStoryID(nodeID);

    if ( npcKey.empty()
        || nodeKey.empty() )
    {
        return false;
    }

    PersistentNPCDialogueState& npcState =
        persistentWorldStoryState
            .npcDialogueStates[npcKey];

    if ( seen )
    {
        npcState.seenNodes.insert(nodeKey);
    }
    else
    {
        npcState.seenNodes.erase(nodeKey);
    }

    return true;
}

bool persistentStoryNPCNodeWasSeen(
    const std::string& mapName,
    const Sint32 persistentID,
    const std::string& nodeID
)
{
    const std::string npcKey =
        makePersistentNPCDialogueKey(
            mapName,
            persistentID
        );

    const std::string nodeKey =
        normalizePersistentStoryID(nodeID);

    if ( npcKey.empty()
        || nodeKey.empty() )
    {
        return false;
    }

    const auto npcIterator =
        persistentWorldStoryState
            .npcDialogueStates
            .find(npcKey);

    if ( npcIterator
        == persistentWorldStoryState
            .npcDialogueStates
            .end() )
    {
        return false;
    }

    return
        npcIterator->second.seenNodes.find(
            nodeKey
        )
        != npcIterator->second.seenNodes.end();
}
bool persistentStoryMapHasState(
    const std::string& mapName
)
{
    const std::string mapKey =
        normalizePersistentMapKey(mapName);

    if ( mapKey.empty() )
    {
        return false;
    }

    return
        persistentMapRemovalRegistry.find(mapKey)
        != persistentMapRemovalRegistry.end();
}

bool persistentStoryOriginalEntityIsRemoved(
    const std::string& mapName,
    const Sint32 persistentID
)
{
    if ( persistentID <= 0 )
    {
        return false;
    }

    const std::string mapKey =
        normalizePersistentMapKey(mapName);

    if ( mapKey.empty() )
    {
        return false;
    }

    const auto mapIterator =
        persistentMapRemovalRegistry.find(mapKey);

    if ( mapIterator
        == persistentMapRemovalRegistry.end() )
    {
        /*
         * No map record means the target map has not yet supplied
         * authoritative persistent state.
         *
         * Do not incorrectly treat unknown as dead.
         */
        return false;
    }

    return
        mapIterator->second.removedEntityIDs.find(
            persistentID
        )
        != mapIterator->second.removedEntityIDs.end();
}
void beginClientPersistentWorldSnapshot(
    const std::string& mapName
)
{
    if ( multiplayer != CLIENT )
    {
        return;
    }

    clientPersistentSnapshotMapKey =
        normalizePersistentMapKey(mapName);

    clientPersistentSnapshotReceiving =
        !clientPersistentSnapshotMapKey.empty();

    clientPersistentSnapshotComplete = false;

    if ( !clientPersistentSnapshotReceiving )
    {
        printlog(
            "[Persistent World MP] Client rejected snapshot with an empty map name."
        );
        return;
    }

    /*
     * Replace the client's old copy with the new authoritative host
     * snapshot.
     */
    PersistentMapRemovalState& state =
        persistentMapRemovalRegistry[
            clientPersistentSnapshotMapKey
        ];

		state.removedEntityIDs.clear();
		state.mechanismStates.clear();
		state.tileStates.clear();
        state.dynamicBoulders.clear();
        state.dynamicWorldItems.clear();
        state.dynamicGoldBags.clear();
        state.dynamicMonsterIDs.clear();
		/*
		* The destination map has not loaded yet. Its fresh raw tiles will
		* become the new baseline inside applyPersistentMapRemovals().
		*/
		state.originalTiles.clear();
		state.originalTileWidth = 0;
		state.originalTileHeight = 0;
		state.originalTileLayers = 0;

    printlog(
        "[Persistent World MP] Client began snapshot for '%s'.",
        clientPersistentSnapshotMapKey.c_str()
    );
}
void receiveClientPersistentRemoval(
    Sint32 persistentID
)
{
    if ( multiplayer != CLIENT
        || !clientPersistentSnapshotReceiving
        || persistentID <= 0 )
    {
        return;
    }

    persistentMapRemovalRegistry[
        clientPersistentSnapshotMapKey
    ].removedEntityIDs.insert(
        persistentID
    );
}
void receiveClientPersistentLeverState(
    Sint32 persistentID,
    bool timedLever,
    Sint32 switchPower,
    Sint32 leverStatus,
    Sint32 leverTimerTicks,
    real_t roll
)
{
    if ( multiplayer != CLIENT
        || !clientPersistentSnapshotReceiving
        || persistentID <= 0 )
    {
        return;
    }

    PersistentMechanismState state;

    state.kind =
        timedLever
            ? PersistentMechanismState::Kind::TimedLever
            : PersistentMechanismState::Kind::Lever;

    state.switchPower = switchPower;
    state.leverStatus = leverStatus;
    state.leverTimerTicks = leverTimerTicks;
    state.roll = roll;

    persistentMapRemovalRegistry[
        clientPersistentSnapshotMapKey
    ].mechanismStates[persistentID] = state;
}
void receiveClientPersistentGateState(
    Sint32 persistentID,
    Sint32 gateStatus,
    Sint32 gateRattle,
    Sint32 gateInverted,
    Sint32 circuitStatus,
    real_t gateStartHeight,
    real_t gateZ,
    real_t gateVelZ,
    bool passable
)
{
    if ( multiplayer != CLIENT
        || !clientPersistentSnapshotReceiving
        || persistentID <= 0 )
    {
        return;
    }

    PersistentMechanismState state;

    state.kind =
        PersistentMechanismState::Kind::Gate;

    state.gateStatus = gateStatus;
    state.gateInit = 1;
    state.gateRattle = gateRattle;
    state.gateInverted = gateInverted;
    state.circuitStatus = circuitStatus;

    state.gateStartHeight = gateStartHeight;
    state.gateZ = gateZ;
    state.gateVelZ = gateVelZ;

    state.passable = passable;

    persistentMapRemovalRegistry[
        clientPersistentSnapshotMapKey
    ].mechanismStates[persistentID] = state;
}
void receiveClientPersistentDoorState(
    Sint32 persistentID,
    bool isIronDoor,
    Sint32 doorDir,
    Sint32 doorStatus,
    Sint32 doorHealth,
    Sint32 doorMaxHealth,
    Sint32 doorLocked,
    Sint32 doorSmacked,
    Sint32 doorTimer,
    Sint32 doorPreventLockpickExploit,
    Sint32 doorForceLockedUnlocked,
    Sint32 doorDisableLockpicks,
    Sint32 doorDisableOpening,
    Sint32 doorLockpickHealth,
    Sint32 doorUnlockWhenPowered,
    Sint32 doorCircuitStatus,
    real_t doorStartAng,
    real_t doorYaw,
    real_t doorX,
    real_t doorY,
    real_t doorFocalY,
    bool passable,
    bool burning,
    bool burnable
)
{
    if ( multiplayer != CLIENT
        || !clientPersistentSnapshotReceiving
        || persistentID <= 0 )
    {
        return;
    }

    PersistentMechanismState state;

    state.kind =
        PersistentMechanismState::Kind::Door;

    state.doorIsIron =
        isIronDoor;

    state.doorDir =
        doorDir;

    state.doorStatus =
        doorStatus;

    state.doorHealth =
        doorHealth;

    state.doorMaxHealth =
        doorMaxHealth;

    state.doorLocked =
        doorLocked;

    state.doorSmacked =
        doorSmacked;

    state.doorTimer =
        doorTimer;

    state.doorPreventLockpickExploit =
        doorPreventLockpickExploit;

    state.doorForceLockedUnlocked =
        doorForceLockedUnlocked;

    state.doorDisableLockpicks =
        doorDisableLockpicks;

    state.doorDisableOpening =
        doorDisableOpening;

    state.doorLockpickHealth =
        doorLockpickHealth;

    state.doorUnlockWhenPowered =
        doorUnlockWhenPowered;

    state.doorCircuitStatus =
        doorCircuitStatus;

    state.doorStartAng =
        doorStartAng;

    state.doorYaw =
        doorYaw;

    state.doorX =
        doorX;

    state.doorY =
        doorY;

    state.doorFocalY =
        doorFocalY;

    state.passable =
        passable;

    state.doorBurning =
        burning;

    state.doorBurnable =
        burnable;

    persistentMapRemovalRegistry[
        clientPersistentSnapshotMapKey
    ].mechanismStates[persistentID] = state;
}
void receiveClientPersistentFurnitureState(
    Sint32 persistentID,
    Sint32 furnitureType,
    Sint32 furnitureHealth,
    Sint32 furnitureMaxHealth,
    bool burning,
    bool burnable
)
{
    if ( multiplayer != CLIENT
        || !clientPersistentSnapshotReceiving
        || persistentID <= 0 )
    {
        return;
    }

    PersistentMechanismState state;

    state.kind =
        PersistentMechanismState::Kind::Furniture;

    state.furnitureType =
        furnitureType;

    state.furnitureHealth =
        furnitureHealth;

    state.furnitureMaxHealth =
        furnitureMaxHealth;

    state.furnitureBurning =
        burning;

    state.furnitureBurnable =
        burnable;

    persistentMapRemovalRegistry[
        clientPersistentSnapshotMapKey
    ].mechanismStates[persistentID] = state;
}
void receiveClientPersistentColliderState(
    Sint32 persistentID,
    Sint32 colliderCurrentHP,
    Sint32 colliderMaxHP,
    Sint32 colliderDamageTypes,
    Sint32 colliderHasCollision,
    bool burning,
    bool burnable
)
{
    if ( multiplayer != CLIENT
        || !clientPersistentSnapshotReceiving
        || persistentID <= 0 )
    {
        return;
    }

    PersistentMechanismState state;

    state.kind =
        PersistentMechanismState::Kind::ColliderDecoration;

    state.colliderCurrentHP =
        colliderCurrentHP;

    state.colliderMaxHP =
        colliderMaxHP;

    state.colliderDamageTypes =
        colliderDamageTypes;

    state.colliderHasCollision =
        colliderHasCollision;

    state.colliderBurning =
        burning;

    state.colliderBurnable =
        burnable;

    persistentMapRemovalRegistry[
        clientPersistentSnapshotMapKey
    ].mechanismStates[persistentID] = state;
}
void receiveClientPersistentPowerCrystalState(
    Sint32 persistentID,
    Sint32 crystalInitialised,
    Sint32 crystalDirection,
    Sint32 crystalNumElectricityNodes,
    Sint32 crystalTurnReverse,
    Sint32 crystalSpellToActivate,
    Sint32 crystalPowerToActivate,
    Sint32 crystalCircuitStatus
)
{
    if ( multiplayer != CLIENT
        || !clientPersistentSnapshotReceiving
        || persistentID <= 0 )
    {
        return;
    }

    PersistentMechanismState state;

    state.kind =
        PersistentMechanismState::Kind::PowerCrystal;

    state.crystalInitialised =
        crystalInitialised;

    state.crystalDirection =
        crystalDirection;

    state.crystalNumElectricityNodes =
        crystalNumElectricityNodes;

    state.crystalTurnReverse =
        crystalTurnReverse;

    state.crystalSpellToActivate =
        crystalSpellToActivate;

    state.crystalPowerToActivate =
        crystalPowerToActivate;

    state.crystalCircuitStatus =
        crystalCircuitStatus;

    persistentMapRemovalRegistry[
        clientPersistentSnapshotMapKey
    ].mechanismStates[persistentID] = state;
}
void receiveClientPersistentBoulderTrapState(
    Sint32 persistentID,
    Sint32 trapBehavior,
    Sint32 fired,
    Sint32 refireAmount,
    Sint32 refireCounter,
    Sint32 preDelay,
    Sint32 circuitStatus,
    Sint32 sabotaged
)
{
    if ( multiplayer != CLIENT
        || !clientPersistentSnapshotReceiving
        || persistentID <= 0 )
    {
        return;
    }

    PersistentMechanismState state;

    state.kind =
        PersistentMechanismState::Kind::BoulderTrap;

    state.boulderTrapBehavior =
        trapBehavior;

    state.boulderTrapFired =
        fired;

    state.boulderTrapRefireAmount =
        refireAmount;

    state.boulderTrapRefireCounter =
        refireCounter;

    state.boulderTrapPreDelay =
        preDelay;

    state.boulderTrapCircuitStatus =
        circuitStatus;

    state.boulderTrapSabotaged =
        sabotaged;

    persistentMapRemovalRegistry[
        clientPersistentSnapshotMapKey
    ].mechanismStates[persistentID] = state;
}
void receiveClientPersistentSummonTrapState(
    Sint32 persistentID,
    Sint32 monster,
    Sint32 count,
    Sint32 interval,
    Sint32 spawnCycles,
    Sint32 powerToDisable,
    Sint32 failureRate,
    Sint32 fired,
    Sint32 initialized,
    Sint32 ticksToFire,
    Sint32 playerProximity
)
{
    if ( multiplayer != CLIENT
        || !clientPersistentSnapshotReceiving
        || persistentID <= 0 )
    {
        return;
    }

    PersistentMechanismState state;

    state.kind =
        PersistentMechanismState::Kind::SummonTrap;

    state.summonTrapMonster =
        monster;

    state.summonTrapCount =
        count;

    state.summonTrapInterval =
        interval;

    state.summonTrapSpawnCycles =
        spawnCycles;

    state.summonTrapPowerToDisable =
        powerToDisable;

    state.summonTrapFailureRate =
        failureRate;

    state.summonTrapFired =
        fired;

    state.summonTrapInitialized =
        initialized;

    state.summonTrapTicksToFire =
        ticksToFire;

    state.summonTrapPlayerProximity =
        playerProximity;

    persistentMapRemovalRegistry[
        clientPersistentSnapshotMapKey
    ].mechanismStates[persistentID] = state;
}
void receiveClientPersistentSignalControllerState(
    Sint32 persistentID,
    bool isANDGate,
    Sint32 switchPower,
    Sint32 delayCount,
    Sint32 timerCount,
    Sint32 repeatCount,
    Sint32 latchInput,
    Sint32 andPowerMask,
    Sint32 circuitStatus,
    Sint32 initialized
)
{
    if ( multiplayer != CLIENT
        || !clientPersistentSnapshotReceiving
        || persistentID <= 0 )
    {
        return;
    }

    PersistentMechanismState state;

    state.kind =
        PersistentMechanismState::Kind::SignalController;

    state.signalControllerIsAND =
        isANDGate;

    state.signalSwitchPower =
        switchPower;

    state.signalDelayCount =
        delayCount;

    state.signalTimerCount =
        timerCount;

    state.signalRepeatCount =
        repeatCount;

    state.signalLatchInput =
        latchInput;

    state.signalANDPowerMask =
        andPowerMask;

    state.signalCircuitStatus =
        circuitStatus;

    state.signalInitialized =
        initialized;

    persistentMapRemovalRegistry[
        clientPersistentSnapshotMapKey
    ].mechanismStates[persistentID] = state;
}
void receiveClientPersistentBellState(
    Sint32 persistentID,
    Sint32 activeTimer,
    Sint32 hasItem,
    Sint32 uses,
    Sint32 currentEvent,
    Sint32 useDelay,
    Sint32 clapperBroken,
    Sint32 bulbBroken,
    Sint32 buffType,
    Sint32 burningTimer,
    Sint32 scrapCreated,
    bool burning,
    bool burnable,
    bool invisible
)
{
    if ( multiplayer != CLIENT
        || !clientPersistentSnapshotReceiving
        || persistentID <= 0 )
    {
        return;
    }

    PersistentMechanismState state;

    state.kind =
        PersistentMechanismState::Kind::Bell;

    state.bellActiveTimer =
        activeTimer;

    state.bellHasItem =
        hasItem;

    state.bellUses =
        uses;

    state.bellCurrentEvent =
        currentEvent;

    state.bellUseDelay =
        useDelay;

    state.bellClapperBroken =
        clapperBroken;

    state.bellBulbBroken =
        bulbBroken;

    state.bellBuffType =
        buffType;

    state.bellBurningTimer =
        burningTimer;

    state.bellBurning =
        burning;

    state.bellBurnable =
        burnable;

    state.bellInvisible =
        invisible;

	state.bellScrapCreated =
    	scrapCreated;

    persistentMapRemovalRegistry[
        clientPersistentSnapshotMapKey
    ].mechanismStates[persistentID] = state;
}
void receiveClientPersistentWaterSourceState(
    Sint32 persistentID,
    bool isFountain,
    Sint32 uses,
    Sint32 mainEffect,
    Sint32 secondaryEffect
)
{
    if ( multiplayer != CLIENT
        || !clientPersistentSnapshotReceiving
        || persistentID <= 0 )
    {
        return;
    }

    PersistentMechanismState state;

    state.kind =
        PersistentMechanismState::Kind::SinkOrFountain;

    state.waterSourceIsFountain =
        isFountain;

    state.waterSourceUses =
        uses;

    state.waterSourceMainEffect =
        mainEffect;

    state.waterSourceSecondaryEffect =
        secondaryEffect;

    persistentMapRemovalRegistry[
        clientPersistentSnapshotMapKey
    ].mechanismStates[persistentID] = state;
}
void receiveClientPersistentCampfireState(
    Sint32 persistentID,
    Sint32 health
)
{
    if ( multiplayer != CLIENT
        || !clientPersistentSnapshotReceiving
        || persistentID <= 0 )
    {
        return;
    }

    PersistentMechanismState state;

    state.kind =
        PersistentMechanismState::Kind::Campfire;

    state.campfireHealth =
        health;

    persistentMapRemovalRegistry[
        clientPersistentSnapshotMapKey
    ].mechanismStates[persistentID] = state;
}
void receiveClientPersistentWallLockState(
    Sint32 persistentID,
    Sint32 lockState,
    Sint32 power,
    Sint32 pickHealth,
    Sint32 preventExploit
)
{
    if ( multiplayer != CLIENT
        || !clientPersistentSnapshotReceiving
        || persistentID <= 0 )
    {
        return;
    }

    PersistentMechanismState state;

    state.kind =
        PersistentMechanismState::Kind::WallLock;

    state.wallLockSavedState =
        lockState;

    state.wallLockSavedPower =
        power;

    state.wallLockSavedPickHealth =
        pickHealth;

    state.wallLockSavedPreventExploit =
        preventExploit;

    persistentMapRemovalRegistry[
        clientPersistentSnapshotMapKey
    ].mechanismStates[persistentID] = state;
}
void receiveClientPersistentWallButtonState(
    Sint32 persistentID,
    Sint32 buttonState,
    Sint32 buttonPower
)
{
    if ( multiplayer != CLIENT
        || !clientPersistentSnapshotReceiving
        || persistentID <= 0 )
    {
        return;
    }

    PersistentMechanismState state;

    state.kind =
        PersistentMechanismState::Kind::WallButton;

    state.wallButtonState =
        buttonState;

    state.wallButtonPower =
        buttonPower;

    persistentMapRemovalRegistry[
        clientPersistentSnapshotMapKey
    ].mechanismStates[persistentID] = state;
}
void receiveClientPersistentPressurePlateState(
    Sint32 persistentID,
    bool permanent,
    Sint32 power,
    Sint32 interactionLock
)
{
    if ( multiplayer != CLIENT
        || !clientPersistentSnapshotReceiving
        || persistentID <= 0 )
    {
        return;
    }

    PersistentMechanismState state;

    state.kind =
        PersistentMechanismState::Kind::PressurePlate;

    state.pressurePlatePermanent =
        permanent;

    state.pressurePlatePower =
        power;

    state.pressurePlateInteractionLock =
        interactionLock;

    persistentMapRemovalRegistry[
        clientPersistentSnapshotMapKey
    ].mechanismStates[persistentID] = state;
}
void receiveClientPersistentPedestalState(
    Sint32 persistentID,
    Sint32 hasOrb,
    Sint32 powerStatus,
    Sint32 initialized,
    Sint32 inGround,
    real_t z,
    real_t velZ,
    bool passable
)
{
    if ( multiplayer != CLIENT
        || !clientPersistentSnapshotReceiving
        || persistentID <= 0 )
    {
        return;
    }

    PersistentMechanismState state;

    state.kind =
        PersistentMechanismState::Kind::Pedestal;

    state.pedestalSavedHasOrb =
        hasOrb;

    state.pedestalSavedPowerStatus =
        powerStatus;

    state.pedestalSavedInit =
        initialized;

    state.pedestalSavedInGround =
        inGround;

    state.pedestalSavedZ =
        z;

    state.pedestalSavedVelZ =
        velZ;

    state.pedestalSavedPassable =
        passable;

    persistentMapRemovalRegistry[
        clientPersistentSnapshotMapKey
    ].mechanismStates[persistentID] = state;
}
void receiveClientPersistentChestState(
    Sint32 persistentID,
    Sint32 health,
    Sint32 maxHealth,
    Sint32 locked,
    Sint32 lockpickHealth,
    Sint32 preventExploit,
    Sint32 oldHealth,
    Sint32 voidState
)
{
    if ( multiplayer != CLIENT
        || !clientPersistentSnapshotReceiving
        || persistentID <= 0 )
    {
        return;
    }

    PersistentMechanismState state;

    state.kind =
        PersistentMechanismState::Kind::Chest;

    state.chestSavedHealth =
        health;

    state.chestSavedMaxHealth =
        maxHealth;

    state.chestSavedLocked =
        locked;

    state.chestSavedLockpickHealth =
        lockpickHealth;

    state.chestSavedPreventExploit =
        preventExploit;

    state.chestSavedOldHealth =
        oldHealth;

    state.chestSavedVoidState =
        voidState;

    persistentMapRemovalRegistry[
        clientPersistentSnapshotMapKey
    ].mechanismStates[persistentID] = state;
}
void receiveClientPersistentTileState(
    Sint32 x,
    Sint32 y,
    Sint32 layer,
    Sint32 tile
)
{
    if ( multiplayer != CLIENT
        || !clientPersistentSnapshotReceiving
        || x < 0
        || y < 0
        || layer < 0
        || layer >= MAPLAYERS )
    {
        return;
    }

    PersistentTileState state;

    state.x = x;
    state.y = y;
    state.layer = layer;
    state.tile = tile;

    persistentMapRemovalRegistry[
        clientPersistentSnapshotMapKey
    ].tileStates[
        makePersistentTileKey(
            x,
            y,
            layer
        )
    ] = state;
}
void finishClientPersistentWorldSnapshot()
{
    if ( multiplayer != CLIENT
        || !clientPersistentSnapshotReceiving )
    {
        return;
    }

    const PersistentMapRemovalState& state =
        persistentMapRemovalRegistry[
            clientPersistentSnapshotMapKey
        ];

    clientPersistentSnapshotReceiving = false;
    clientPersistentSnapshotComplete = true;

printlog(
    "[Persistent World MP] Client completed snapshot for '%s': %zu removal(s), %zu mechanism state(s), %zu tile override(s).",
    clientPersistentSnapshotMapKey.c_str(),
    state.removedEntityIDs.size(),
    state.mechanismStates.size(),
    state.tileStates.size()
);
}
void sendPersistentWorldSnapshotToClient(
    int player,
    const std::string& destinationMapName
)
{
    if ( multiplayer != SERVER
        || player <= 0
        || player >= MAXPLAYERS
        || client_disconnected[player]
        || players[player]->isLocalPlayer()
        || !net_packet
        || !net_packet->data )
    {
        return;
    }

    const std::string mapKey =
        normalizePersistentMapKey(
            destinationMapName
        );

    if ( mapKey.empty() )
    {
        printlog(
            "[Persistent World MP] Cannot send snapshot to client %d: destination map name is empty.",
            player
        );
        return;
    }

    /*
     * It is valid for a destination map to have no prior record.
     * Send an empty snapshot so the client clears any stale copy.
     */
    const auto mapIterator =
        persistentMapRemovalRegistry.find(mapKey);

    const PersistentMapRemovalState* state =
        mapIterator
            == persistentMapRemovalRegistry.end()
                ? nullptr
                : &mapIterator->second;

    auto prepareClientAddress =
        [player]()
        {
            net_packet->address.host =
                net_clients[player - 1].host;

            net_packet->address.port =
                net_clients[player - 1].port;
        };

    /*
     * PWBG:
     * bytes 0-3  packet name
     * bytes 4+   null-terminated normalized map key
     */
    strcpy(
        reinterpret_cast<char*>(net_packet->data),
        "PWBG"
    );

    strcpy(
        reinterpret_cast<char*>(&net_packet->data[4]),
        mapKey.c_str()
    );

    net_packet->len =
        4 + mapKey.length() + 1;

    prepareClientAddress();

    sendPacketSafe(
        net_sock,
        -1,
        net_packet,
        player - 1
    );

    Uint32 removalsSent = 0;
    Uint32 leversSent = 0;
    Uint32 gatesSent = 0;
	Uint32 doorsSent = 0;
	Uint32 furnitureSent = 0;
	Uint32 collidersSent = 0;
	Uint32 powerCrystalsSent = 0;
	Uint32 boulderTrapsSent = 0;
	Uint32 signalControllersSent = 0;
	Uint32 bellsSent = 0;
	Uint32 waterSourcesSent = 0;
	Uint32 campfiresSent = 0;
	Uint32 wallLocksSent = 0;
	Uint32 wallButtonsSent = 0;
	Uint32 pressurePlatesSent = 0;
	Uint32 tilesSent = 0;
	Uint32 pedestalsSent = 0;
	Uint32 chestsSent = 0;
    Uint32 summonTrapsSent = 0;

    if ( state )
    {
        /*
         * PWRM:
         * 4  persistent ID
         */
        for ( const Sint32 persistentID :
            state->removedEntityIDs )
        {
            strcpy(
                reinterpret_cast<char*>(
                    net_packet->data
                ),
                "PWRM"
            );

            SDLNet_Write32(
                static_cast<Uint32>(persistentID),
                &net_packet->data[4]
            );

            net_packet->len = 8;

            prepareClientAddress();

            sendPacketSafe(
                net_sock,
                -1,
                net_packet,
                player - 1
            );

            ++removalsSent;
        }
		        /*
         * PWTL:
         *
         * bytes 0-3    packet name
         * bytes 4-7    tile x
         * bytes 8-11   tile y
         * bytes 12-15  layer
         * bytes 16-19  tile value
         */
        for ( const auto& tilePair :
            state->tileStates )
        {
            const PersistentTileState& tileState =
                tilePair.second;

            strcpy(
                reinterpret_cast<char*>(
                    net_packet->data
                ),
                "PWTL"
            );

            SDLNet_Write32(
                static_cast<Uint32>(
                    tileState.x
                ),
                &net_packet->data[4]
            );

            SDLNet_Write32(
                static_cast<Uint32>(
                    tileState.y
                ),
                &net_packet->data[8]
            );

            SDLNet_Write32(
                static_cast<Uint32>(
                    tileState.layer
                ),
                &net_packet->data[12]
            );

            SDLNet_Write32(
                static_cast<Uint32>(
                    tileState.tile
                ),
                &net_packet->data[16]
            );

            net_packet->len = 20;

            prepareClientAddress();

            sendPacketSafe(
                net_sock,
                -1,
                net_packet,
                player - 1
            );

            ++tilesSent;
        }
        for ( const auto& mechanismPair :
            state->mechanismStates )
        {
            const Sint32 persistentID =
                mechanismPair.first;

            const PersistentMechanismState& mechanism =
                mechanismPair.second;

            if ( mechanism.kind
                    == PersistentMechanismState::Kind::Lever
                || mechanism.kind
                    == PersistentMechanismState::Kind::TimedLever )
            {
                /*
                 * PWLV:
                 * 4   persistent ID
                 * 8   kind: 0 ordinary, 1 timed
                 * 9   switch power
                 * 13  lever status
                 * 17  timer ticks
                 * 21  roll multiplied by 65536
                 */
                strcpy(
                    reinterpret_cast<char*>(
                        net_packet->data
                    ),
                    "PWLV"
                );

                SDLNet_Write32(
                    static_cast<Uint32>(persistentID),
                    &net_packet->data[4]
                );

                net_packet->data[8] =
                    mechanism.kind
                        == PersistentMechanismState::Kind::TimedLever
                            ? 1
                            : 0;

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.switchPower
                    ),
                    &net_packet->data[9]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.leverStatus
                    ),
                    &net_packet->data[13]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.leverTimerTicks
                    ),
                    &net_packet->data[17]
                );

                SDLNet_Write32(
                    static_cast<Sint32>(
                        mechanism.roll * 65536.0
                    ),
                    &net_packet->data[21]
                );

                net_packet->len = 25;

                prepareClientAddress();

                sendPacketSafe(
                    net_sock,
                    -1,
                    net_packet,
                    player - 1
                );

                ++leversSent;
            }
            else if ( mechanism.kind == PersistentMechanismState::Kind::SummonTrap )
            {
                /*
                * PWST:
                *
                * bytes 0-3    packet name
                * bytes 4-7    persistent trap ID
                * bytes 8-11   selected monster
                * bytes 12-15  monsters per cycle
                * bytes 16-19  interval
                * bytes 20-23  remaining spawn cycles
                * bytes 24-27  power-to-disable setting
                * bytes 28-31  failure rate
                * bytes 32-35  fired/broken state
                * bytes 36-39  initialized state
                * bytes 40-43  ticks until next cycle
                * bytes 44-47  player proximity radius
                */
                strcpy(
                    reinterpret_cast<char*>(
                        net_packet->data
                    ),
                    "PWST"
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        persistentID
                    ),
                    &net_packet->data[4]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.summonTrapMonster
                    ),
                    &net_packet->data[8]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.summonTrapCount
                    ),
                    &net_packet->data[12]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.summonTrapInterval
                    ),
                    &net_packet->data[16]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.summonTrapSpawnCycles
                    ),
                    &net_packet->data[20]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.summonTrapPowerToDisable
                    ),
                    &net_packet->data[24]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.summonTrapFailureRate
                    ),
                    &net_packet->data[28]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.summonTrapFired
                    ),
                    &net_packet->data[32]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.summonTrapInitialized
                    ),
                    &net_packet->data[36]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.summonTrapTicksToFire
                    ),
                    &net_packet->data[40]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.summonTrapPlayerProximity
                    ),
                    &net_packet->data[44]
                );

                net_packet->len = 48;

                prepareClientAddress();

                sendPacketSafe(
                    net_sock,
                    -1,
                    net_packet,
                    player - 1
                );

                ++summonTrapsSent;
            }
			            else if ( mechanism.kind
                == PersistentMechanismState::Kind::Chest )
            {
                /*
                 * PWCH:
                 *
                 * bytes 0-3    packet name
                 * bytes 4-7    persistent ID
                 * bytes 8-11   current health
                 * bytes 12-15  maximum health
                 * bytes 16-19  locked state
                 * bytes 20-23  lockpick health
                 * bytes 24-27  anti-exploit state
                 * bytes 28-31  previous health
                 * bytes 32-35  void-chest state
                 */
                strcpy(
                    reinterpret_cast<char*>(
                        net_packet->data
                    ),
                    "PWCH"
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        persistentID
                    ),
                    &net_packet->data[4]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.chestSavedHealth
                    ),
                    &net_packet->data[8]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.chestSavedMaxHealth
                    ),
                    &net_packet->data[12]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.chestSavedLocked
                    ),
                    &net_packet->data[16]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.chestSavedLockpickHealth
                    ),
                    &net_packet->data[20]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.chestSavedPreventExploit
                    ),
                    &net_packet->data[24]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.chestSavedOldHealth
                    ),
                    &net_packet->data[28]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.chestSavedVoidState
                    ),
                    &net_packet->data[32]
                );

                net_packet->len = 36;

                prepareClientAddress();

                sendPacketSafe(
                    net_sock,
                    -1,
                    net_packet,
                    player - 1
                );

                ++chestsSent;
            }
			            else if ( mechanism.kind
                == PersistentMechanismState::Kind::WallButton )
            {
                /*
                 * PWBW:
                 *
                 * bytes 0-3    packet name
                 * bytes 4-7    persistent ID
                 * bytes 8-11   pressed/released state
                 * bytes 12-15  remaining timer or permanent -1
                 */
                strcpy(
                    reinterpret_cast<char*>(
                        net_packet->data
                    ),
                    "PWBW"
                );

                SDLNet_Write32(
                    static_cast<Uint32>(persistentID),
                    &net_packet->data[4]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.wallButtonState
                    ),
                    &net_packet->data[8]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.wallButtonPower
                    ),
                    &net_packet->data[12]
                );

                net_packet->len = 16;

                prepareClientAddress();

                sendPacketSafe(
                    net_sock,
                    -1,
                    net_packet,
                    player - 1
                );

                ++wallButtonsSent;
            }
			            else if ( mechanism.kind
                == PersistentMechanismState::Kind::PressurePlate )
            {
                /*
                 * PWPP:
                 *
                 * bytes 0-3    packet name
                 * bytes 4-7    persistent ID
                 * bytes 8-11   permanent flag
                 * bytes 12-15  current power/latch state
                 * bytes 16-19  scripted interaction lock
                 */
                strcpy(
                    reinterpret_cast<char*>(
                        net_packet->data
                    ),
                    "PWPP"
                );

                SDLNet_Write32(
                    static_cast<Uint32>(persistentID),
                    &net_packet->data[4]
                );

                SDLNet_Write32(
                    mechanism.pressurePlatePermanent
                        ? 1
                        : 0,
                    &net_packet->data[8]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.pressurePlatePower
                    ),
                    &net_packet->data[12]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.pressurePlateInteractionLock
                    ),
                    &net_packet->data[16]
                );

                net_packet->len = 20;

                prepareClientAddress();

                sendPacketSafe(
                    net_sock,
                    -1,
                    net_packet,
                    player - 1
                );

                ++pressurePlatesSent;
            }
			            else if ( mechanism.kind
                == PersistentMechanismState::Kind::Pedestal )
            {
                /*
                 * PWPD:
                 *
                 * bytes 0-3    packet name
                 * bytes 4-7    persistent ID
                 * bytes 8-11   contained orb type, 0 = empty
                 * bytes 12-15  current power status
                 * bytes 16-19  initialized state
                 * bytes 20-23  underground/emergence state
                 * bytes 24-27  z * 65536
                 * bytes 28-31  vertical velocity * 65536
                 * byte  32     passable
                 */
                strcpy(
                    reinterpret_cast<char*>(
                        net_packet->data
                    ),
                    "PWPD"
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        persistentID
                    ),
                    &net_packet->data[4]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.pedestalSavedHasOrb
                    ),
                    &net_packet->data[8]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.pedestalSavedPowerStatus
                    ),
                    &net_packet->data[12]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.pedestalSavedInit
                    ),
                    &net_packet->data[16]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.pedestalSavedInGround
                    ),
                    &net_packet->data[20]
                );

                SDLNet_Write32(
                    static_cast<Sint32>(
                        mechanism.pedestalSavedZ
                        * 65536.0
                    ),
                    &net_packet->data[24]
                );

                SDLNet_Write32(
                    static_cast<Sint32>(
                        mechanism.pedestalSavedVelZ
                        * 65536.0
                    ),
                    &net_packet->data[28]
                );

                net_packet->data[32] =
                    mechanism.pedestalSavedPassable
                        ? 1
                        : 0;

                net_packet->len = 33;

                prepareClientAddress();

                sendPacketSafe(
                    net_sock,
                    -1,
                    net_packet,
                    player - 1
                );

                ++pedestalsSent;
            }
            else if ( mechanism.kind
                == PersistentMechanismState::Kind::Gate )
            {
                /*
                 * PWGT:
                 * 4   persistent ID
                 * 8   gate status
                 * 12  gate rattle
                 * 16  gate inverted
                 * 20  circuit status
                 * 24  start height * 65536
                 * 28  z * 65536
                 * 32  vel_z * 65536
                 * 36  passable
                 */
                strcpy(
                    reinterpret_cast<char*>(
                        net_packet->data
                    ),
                    "PWGT"
                );

                SDLNet_Write32(
                    static_cast<Uint32>(persistentID),
                    &net_packet->data[4]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.gateStatus
                    ),
                    &net_packet->data[8]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.gateRattle
                    ),
                    &net_packet->data[12]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.gateInverted
                    ),
                    &net_packet->data[16]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.circuitStatus
                    ),
                    &net_packet->data[20]
                );

                SDLNet_Write32(
                    static_cast<Sint32>(
                        mechanism.gateStartHeight
                        * 65536.0
                    ),
                    &net_packet->data[24]
                );

                SDLNet_Write32(
                    static_cast<Sint32>(
                        mechanism.gateZ * 65536.0
                    ),
                    &net_packet->data[28]
                );

                SDLNet_Write32(
                    static_cast<Sint32>(
                        mechanism.gateVelZ * 65536.0
                    ),
                    &net_packet->data[32]
                );

                net_packet->data[36] =
                    mechanism.passable ? 1 : 0;

                net_packet->len = 37;

                prepareClientAddress();

                sendPacketSafe(
                    net_sock,
                    -1,
                    net_packet,
                    player - 1
                );

                ++gatesSent;
            }
			else if ( mechanism.kind
				== PersistentMechanismState::Kind::Door )
			{
				/*
				* PWDR packet:
				*
				*  4  persistent ID
				*  8  iron-door flag
				*  9  door direction
				* 13  door status
				* 17  health
				* 21  max health
				* 25  locked
				* 29  smacked direction
				* 33  timer
				* 37  anti-lockpick-exploit state
				* 41  forced lock/unlock option
				* 45  disable lockpicks
				* 49  disable opening
				* 53  lockpick health
				* 57  unlock when powered
				* 61  circuit status
				* 65  start angle * 65536
				* 69  yaw * 65536
				* 73  x * 65536
				* 77  y * 65536
				* 81  focal-y * 65536
				* 85  passable
				* 86  burning
				* 87  burnable
				*/
				strcpy(
					reinterpret_cast<char*>(
						net_packet->data
					),
					"PWDR"
				);

				SDLNet_Write32(
					static_cast<Uint32>(persistentID),
					&net_packet->data[4]
				);

				net_packet->data[8] =
					mechanism.doorIsIron ? 1 : 0;

				SDLNet_Write32(
					static_cast<Uint32>(
						mechanism.doorDir
					),
					&net_packet->data[9]
				);

				SDLNet_Write32(
					static_cast<Uint32>(
						mechanism.doorStatus
					),
					&net_packet->data[13]
				);

				SDLNet_Write32(
					static_cast<Uint32>(
						mechanism.doorHealth
					),
					&net_packet->data[17]
				);

				SDLNet_Write32(
					static_cast<Uint32>(
						mechanism.doorMaxHealth
					),
					&net_packet->data[21]
				);

				SDLNet_Write32(
					static_cast<Uint32>(
						mechanism.doorLocked
					),
					&net_packet->data[25]
				);

				SDLNet_Write32(
					static_cast<Uint32>(
						mechanism.doorSmacked
					),
					&net_packet->data[29]
				);

				SDLNet_Write32(
					static_cast<Uint32>(
						mechanism.doorTimer
					),
					&net_packet->data[33]
				);

				SDLNet_Write32(
					static_cast<Uint32>(
						mechanism.doorPreventLockpickExploit
					),
					&net_packet->data[37]
				);

				SDLNet_Write32(
					static_cast<Uint32>(
						mechanism.doorForceLockedUnlocked
					),
					&net_packet->data[41]
				);

				SDLNet_Write32(
					static_cast<Uint32>(
						mechanism.doorDisableLockpicks
					),
					&net_packet->data[45]
				);

				SDLNet_Write32(
					static_cast<Uint32>(
						mechanism.doorDisableOpening
					),
					&net_packet->data[49]
				);

				SDLNet_Write32(
					static_cast<Uint32>(
						mechanism.doorLockpickHealth
					),
					&net_packet->data[53]
				);

				SDLNet_Write32(
					static_cast<Uint32>(
						mechanism.doorUnlockWhenPowered
					),
					&net_packet->data[57]
				);

				SDLNet_Write32(
					static_cast<Uint32>(
						mechanism.doorCircuitStatus
					),
					&net_packet->data[61]
				);

				SDLNet_Write32(
					static_cast<Sint32>(
						mechanism.doorStartAng
						* 65536.0
					),
					&net_packet->data[65]
				);

				SDLNet_Write32(
					static_cast<Sint32>(
						mechanism.doorYaw
						* 65536.0
					),
					&net_packet->data[69]
				);

				SDLNet_Write32(
					static_cast<Sint32>(
						mechanism.doorX
						* 65536.0
					),
					&net_packet->data[73]
				);

				SDLNet_Write32(
					static_cast<Sint32>(
						mechanism.doorY
						* 65536.0
					),
					&net_packet->data[77]
				);

				SDLNet_Write32(
					static_cast<Sint32>(
						mechanism.doorFocalY
						* 65536.0
					),
					&net_packet->data[81]
				);

				net_packet->data[85] =
					mechanism.passable ? 1 : 0;

				net_packet->data[86] =
					mechanism.doorBurning ? 1 : 0;

				net_packet->data[87] =
					mechanism.doorBurnable ? 1 : 0;

				net_packet->len = 88;

				prepareClientAddress();

				sendPacketSafe(
					net_sock,
					-1,
					net_packet,
					player - 1
				);

				++doorsSent;
			}
			else if ( mechanism.kind
				== PersistentMechanismState::Kind::Furniture )
			{
				/*
				* PWFU:
				*
				* bytes 0-3   packet name
				* bytes 4-7   persistent ID
				* bytes 8-11  furniture type
				* bytes 12-15 current health
				* bytes 16-19 maximum health
				* byte  20    burning
				* byte  21    burnable
				*/
				strcpy(
					reinterpret_cast<char*>(
						net_packet->data
					),
					"PWFU"
				);

				SDLNet_Write32(
					static_cast<Uint32>(persistentID),
					&net_packet->data[4]
				);

				SDLNet_Write32(
					static_cast<Uint32>(
						mechanism.furnitureType
					),
					&net_packet->data[8]
				);

				SDLNet_Write32(
					static_cast<Uint32>(
						mechanism.furnitureHealth
					),
					&net_packet->data[12]
				);

				SDLNet_Write32(
					static_cast<Uint32>(
						mechanism.furnitureMaxHealth
					),
					&net_packet->data[16]
				);

				net_packet->data[20] =
					mechanism.furnitureBurning ? 1 : 0;

				net_packet->data[21] =
					mechanism.furnitureBurnable ? 1 : 0;

				net_packet->len = 22;

				prepareClientAddress();

				sendPacketSafe(
					net_sock,
					-1,
					net_packet,
					player - 1
				);

				++furnitureSent;
			}
			else if ( mechanism.kind
				== PersistentMechanismState::Kind::ColliderDecoration )
			{
				/*
				* PWCD:
				*
				* bytes 0-3   packet name
				* bytes 4-7   persistent ID
				* bytes 8-11  current HP
				* bytes 12-15 maximum HP
				* bytes 16-19 damage type
				* bytes 20-23 collision flags
				* byte  24    burning
				* byte  25    burnable
				*/
				strcpy(
					reinterpret_cast<char*>(
						net_packet->data
					),
					"PWCD"
				);

				SDLNet_Write32(
					static_cast<Uint32>(persistentID),
					&net_packet->data[4]
				);

				SDLNet_Write32(
					static_cast<Uint32>(
						mechanism.colliderCurrentHP
					),
					&net_packet->data[8]
				);

				SDLNet_Write32(
					static_cast<Uint32>(
						mechanism.colliderMaxHP
					),
					&net_packet->data[12]
				);

				SDLNet_Write32(
					static_cast<Uint32>(
						mechanism.colliderDamageTypes
					),
					&net_packet->data[16]
				);

				SDLNet_Write32(
					static_cast<Uint32>(
						mechanism.colliderHasCollision
					),
					&net_packet->data[20]
				);

				net_packet->data[24] =
					mechanism.colliderBurning ? 1 : 0;

				net_packet->data[25] =
					mechanism.colliderBurnable ? 1 : 0;

				net_packet->len = 26;

				prepareClientAddress();

				sendPacketSafe(
					net_sock,
					-1,
					net_packet,
					player - 1
				);

				++collidersSent;
			}
			else if ( mechanism.kind
				== PersistentMechanismState::Kind::PowerCrystal )
			{
				/*
				* PWPC:
				*
				* bytes 0-3   packet name
				* bytes 4-7   persistent ID
				* bytes 8-11  initialized
				* bytes 12-15 cardinal direction
				* bytes 16-19 electricity-node count
				* bytes 20-23 reverse-turn setting
				* bytes 24-27 spell requirement
				* bytes 28-31 power requirement
				* bytes 32-35 circuit status
				*/
				strcpy(
					reinterpret_cast<char*>(
						net_packet->data
					),
					"PWPC"
				);

				SDLNet_Write32(
					static_cast<Uint32>(persistentID),
					&net_packet->data[4]
				);

				SDLNet_Write32(
					static_cast<Uint32>(
						mechanism.crystalInitialised
					),
					&net_packet->data[8]
				);

				SDLNet_Write32(
					static_cast<Uint32>(
						mechanism.crystalDirection
					),
					&net_packet->data[12]
				);

				SDLNet_Write32(
					static_cast<Uint32>(
						mechanism.crystalNumElectricityNodes
					),
					&net_packet->data[16]
				);

				SDLNet_Write32(
					static_cast<Uint32>(
						mechanism.crystalTurnReverse
					),
					&net_packet->data[20]
				);

				SDLNet_Write32(
					static_cast<Uint32>(
						mechanism.crystalSpellToActivate
					),
					&net_packet->data[24]
				);

				SDLNet_Write32(
					static_cast<Uint32>(
						mechanism.crystalPowerToActivate
					),
					&net_packet->data[28]
				);

				SDLNet_Write32(
					static_cast<Uint32>(
						mechanism.crystalCircuitStatus
					),
					&net_packet->data[32]
				);

				net_packet->len = 36;

				prepareClientAddress();

				sendPacketSafe(
					net_sock,
					-1,
					net_packet,
					player - 1
				);

				++powerCrystalsSent;
			}
			else if ( mechanism.kind
                == PersistentMechanismState::Kind::SignalController )
            {
                /*
                 * PWSG:
                 *
                 * bytes 0-3    packet name
                 * bytes 4-7    persistent ID
                 * bytes 8-11   type: 0 timer, 1 AND gate
                 * bytes 12-15  current switch/output state
                 * bytes 16-19  activation-delay countdown
                 * bytes 20-23  interval countdown
                 * bytes 24-27  remaining repeat count
                 * bytes 28-31  latch-input state
                 * bytes 32-35  AND input bitmask
                 * bytes 36-39  input circuit status
                 * bytes 40-43  initialized state
                 */
                strcpy(
                    reinterpret_cast<char*>(
                        net_packet->data
                    ),
                    "PWSG"
                );

                SDLNet_Write32(
                    static_cast<Uint32>(persistentID),
                    &net_packet->data[4]
                );

                SDLNet_Write32(
                    mechanism.signalControllerIsAND
                        ? 1
                        : 0,
                    &net_packet->data[8]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.signalSwitchPower
                    ),
                    &net_packet->data[12]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.signalDelayCount
                    ),
                    &net_packet->data[16]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.signalTimerCount
                    ),
                    &net_packet->data[20]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.signalRepeatCount
                    ),
                    &net_packet->data[24]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.signalLatchInput
                    ),
                    &net_packet->data[28]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.signalANDPowerMask
                    ),
                    &net_packet->data[32]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.signalCircuitStatus
                    ),
                    &net_packet->data[36]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.signalInitialized
                    ),
                    &net_packet->data[40]
                );

                net_packet->len = 44;

                prepareClientAddress();

                sendPacketSafe(
                    net_sock,
                    -1,
                    net_packet,
                    player - 1
                );

                ++signalControllersSent;
            }
			            else if ( mechanism.kind
                == PersistentMechanismState::Kind::Campfire )
            {
                /*
                 * PWCF:
                 *
                 * bytes 0-3   packet name
                 * bytes 4-7   persistent ID
                 * bytes 8-11  remaining campfire health/uses
                 */
                strcpy(
                    reinterpret_cast<char*>(
                        net_packet->data
                    ),
                    "PWCF"
                );

                SDLNet_Write32(
                    static_cast<Uint32>(persistentID),
                    &net_packet->data[4]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.campfireHealth
                    ),
                    &net_packet->data[8]
                );

                net_packet->len = 12;

                prepareClientAddress();

                sendPacketSafe(
                    net_sock,
                    -1,
                    net_packet,
                    player - 1
                );

                ++campfiresSent;
            }
			            else if ( mechanism.kind
                == PersistentMechanismState::Kind::WallLock )
            {
                /*
                 * PWWL:
                 *
                 * bytes 0-3    packet name
                 * bytes 4-7    persistent ID
                 * bytes 8-11   wall-lock state
                 * bytes 12-15  wall-lock power
                 * bytes 16-19  lockpick health
                 * bytes 20-23  anti-lockpick-exploit state
                 */
                strcpy(
                    reinterpret_cast<char*>(
                        net_packet->data
                    ),
                    "PWWL"
                );

                SDLNet_Write32(
                    static_cast<Uint32>(persistentID),
                    &net_packet->data[4]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.wallLockSavedState
                    ),
                    &net_packet->data[8]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.wallLockSavedPower
                    ),
                    &net_packet->data[12]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.wallLockSavedPickHealth
                    ),
                    &net_packet->data[16]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.wallLockSavedPreventExploit
                    ),
                    &net_packet->data[20]
                );

                net_packet->len = 24;

                prepareClientAddress();

                sendPacketSafe(
                    net_sock,
                    -1,
                    net_packet,
                    player - 1
                );

                ++wallLocksSent;
            }
			            else if ( mechanism.kind
                == PersistentMechanismState::Kind::SinkOrFountain )
            {
                /*
                 * PWSW:
                 *
                 * bytes 0-3    packet name
                 * bytes 4-7    persistent ID
                 * bytes 8-11   type: 0 sink, 1 fountain
                 * bytes 12-15  remaining uses or full/dry state
                 * bytes 16-19  main effect
                 * bytes 20-23  secondary/next effect
                 */
                strcpy(
                    reinterpret_cast<char*>(
                        net_packet->data
                    ),
                    "PWSW"
                );

                SDLNet_Write32(
                    static_cast<Uint32>(persistentID),
                    &net_packet->data[4]
                );

                SDLNet_Write32(
                    mechanism.waterSourceIsFountain
                        ? 1
                        : 0,
                    &net_packet->data[8]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.waterSourceUses
                    ),
                    &net_packet->data[12]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.waterSourceMainEffect
                    ),
                    &net_packet->data[16]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.waterSourceSecondaryEffect
                    ),
                    &net_packet->data[20]
                );

                net_packet->len = 24;

                prepareClientAddress();

                sendPacketSafe(
                    net_sock,
                    -1,
                    net_packet,
                    player - 1
                );

                ++waterSourcesSent;
            }
			            else if ( mechanism.kind
                == PersistentMechanismState::Kind::Bell )
            {
                /*
                 * PWBL:
                 *
                 * bytes 0-3    packet name
                 * bytes 4-7    persistent ID
                 * bytes 8-11   active timer
                 * bytes 12-15  item availability
                 * bytes 16-19  remaining uses
                 * bytes 20-23  current event
                 * bytes 24-27  interaction delay
                 * bytes 28-31  clapper broken
                 * bytes 32-35  bulb broken
                 * bytes 36-39  buff type
                 * bytes 40-43  burning timer
                 * byte  44     burning
                 * byte  45     burnable
                 * byte  46     invisible
                 */
                strcpy(
                    reinterpret_cast<char*>(
                        net_packet->data
                    ),
                    "PWBL"
                );

                SDLNet_Write32(
                    static_cast<Uint32>(persistentID),
                    &net_packet->data[4]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.bellActiveTimer
                    ),
                    &net_packet->data[8]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.bellHasItem
                    ),
                    &net_packet->data[12]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.bellUses
                    ),
                    &net_packet->data[16]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.bellCurrentEvent
                    ),
                    &net_packet->data[20]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.bellUseDelay
                    ),
                    &net_packet->data[24]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.bellClapperBroken
                    ),
                    &net_packet->data[28]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.bellBulbBroken
                    ),
                    &net_packet->data[32]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.bellBuffType
                    ),
                    &net_packet->data[36]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.bellBurningTimer
                    ),
                    &net_packet->data[40]
                );

				SDLNet_Write32(
					static_cast<Uint32>(
						mechanism.bellScrapCreated
					),
					&net_packet->data[44]
				);

				net_packet->data[48] =
					mechanism.bellBurning ? 1 : 0;

				net_packet->data[49] =
					mechanism.bellBurnable ? 1 : 0;

				net_packet->data[50] =
					mechanism.bellInvisible ? 1 : 0;

				net_packet->len = 51;

                prepareClientAddress();

                sendPacketSafe(
                    net_sock,
                    -1,
                    net_packet,
                    player - 1
                );

                ++bellsSent;
            }
			else if ( mechanism.kind == PersistentMechanismState::Kind::BoulderTrap )
            {
                /*
                 * PWBT:
                 *
                 * bytes 0-3   packet name
                 * bytes 4-7   persistent ID
                 * bytes 8-11  trap behavior type
                 * bytes 12-15 fired state
                 * bytes 16-19 remaining refires
                 * bytes 20-23 refire counter
                 * bytes 24-27 remaining pre-delay
                 * bytes 28-31 circuit status
                 * bytes 32-35 sabotaged state
                 */
                strcpy(
                    reinterpret_cast<char*>(
                        net_packet->data
                    ),
                    "PWBT"
                );

                SDLNet_Write32(
                    static_cast<Uint32>(persistentID),
                    &net_packet->data[4]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.boulderTrapBehavior
                    ),
                    &net_packet->data[8]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.boulderTrapFired
                    ),
                    &net_packet->data[12]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.boulderTrapRefireAmount
                    ),
                    &net_packet->data[16]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.boulderTrapRefireCounter
                    ),
                    &net_packet->data[20]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.boulderTrapPreDelay
                    ),
                    &net_packet->data[24]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.boulderTrapCircuitStatus
                    ),
                    &net_packet->data[28]
                );

                SDLNet_Write32(
                    static_cast<Uint32>(
                        mechanism.boulderTrapSabotaged
                    ),
                    &net_packet->data[32]
                );

                net_packet->len = 36;

                prepareClientAddress();

                sendPacketSafe(
                    net_sock,
                    -1,
                    net_packet,
                    player - 1
                );

                ++boulderTrapsSent;
            }
        }
    }

    const PersistentMinimapState* minimapState =
        persistentMinimapStateForPlayer(player, mapKey);
    if (minimapState)
    {
        const std::size_t tileCount =
            minimapState->width > 0 && minimapState->height > 0
            ? static_cast<std::size_t>(minimapState->width)
                * static_cast<std::size_t>(minimapState->height)
            : 0;
        if (minimapState->width <= MINIMAP_MAX_DIMENSION
            && minimapState->height <= MINIMAP_MAX_DIMENSION
            && tileCount > 0
            && minimapState->tiles.size() == tileCount
            && mapKey.size() <= 255)
        {
            const std::vector<Uint8> packed =
                packPersistentMinimapTilesForServer(minimapState->tiles);
            const Uint16 chunkCount = static_cast<Uint16>(
                (packed.size() + PERSISTENT_MINIMAP_SERVER_CHUNK_PAYLOAD - 1U)
                / PERSISTENT_MINIMAP_SERVER_CHUNK_PAYLOAD);
            Uint32 transferId = ++persistentMinimapServerTransferId;
            if (transferId == 0)
            {
                transferId = ++persistentMinimapServerTransferId;
            }
            bool sentAllChunks = chunkCount > 0;
            for (Uint16 sequence = 0;
                sentAllChunks && sequence < chunkCount; ++sequence)
            {
                const std::size_t payloadOffset =
                    static_cast<std::size_t>(sequence)
                    * PERSISTENT_MINIMAP_SERVER_CHUNK_PAYLOAD;
                const std::size_t payloadBytes = std::min(
                    PERSISTENT_MINIMAP_SERVER_CHUNK_PAYLOAD,
                    packed.size() - payloadOffset);
                std::memcpy(net_packet->data, "MMSV", 4);
                net_packet->data[4] = static_cast<Uint8>(player);
                SDLNet_Write32(transferId, &net_packet->data[5]);
                SDLNet_Write16(
                    static_cast<Uint16>(minimapState->width),
                    &net_packet->data[9]);
                SDLNet_Write16(
                    static_cast<Uint16>(minimapState->height),
                    &net_packet->data[11]);
                net_packet->data[13] =
                    static_cast<Uint8>(mapKey.size());
                SDLNet_Write16(sequence, &net_packet->data[14]);
                SDLNet_Write16(chunkCount, &net_packet->data[16]);
                SDLNet_Write32(
                    static_cast<Uint32>(packed.size()),
                    &net_packet->data[18]);
                SDLNet_Write32(
                    static_cast<Uint32>(payloadOffset),
                    &net_packet->data[22]);
                SDLNet_Write16(
                    static_cast<Uint16>(payloadBytes),
                    &net_packet->data[26]);
                std::memcpy(
                    &net_packet->data[28], mapKey.data(), mapKey.size());
                std::memcpy(
                    &net_packet->data[28 + mapKey.size()],
                    packed.data() + payloadOffset,
                    payloadBytes);
                net_packet->len = static_cast<int>(
                    28 + mapKey.size() + payloadBytes);
                prepareClientAddress();
                sentAllChunks = sendPacketSafe(
                    net_sock, -1, net_packet, player - 1) != 0;
            }
            if (sentAllChunks)
            {
                printlog(
                    "[Persistent Minimap] Sent player %d map '%s' in %u destination snapshot chunk(s).",
                    player, mapKey.c_str(),
                    static_cast<unsigned>(chunkCount));
            }
            else
            {
                printlog(
                    "[Persistent Minimap] Failed to queue the destination snapshot for player %d map '%s'.",
                    player, mapKey.c_str());
            }
        }
    }
    else
    {
        printlog(
            "[Persistent Minimap] No saved discovery was available for player %d identity '%s' in '%s'.",
            player,
            persistentMinimapPlayerIdentity(player).c_str(),
            mapKey.c_str());
    }

    strcpy(
        reinterpret_cast<char*>(net_packet->data),
        "PWEN"
    );

    net_packet->len = 4;

    prepareClientAddress();

    sendPacketSafe(
        net_sock,
        -1,
        net_packet,
        player - 1
    );

printlog(
    "[Persistent World MP] Sent '%s' snapshot to client %d: %u removal(s), %u lever state(s), %u gate state(s), %u door state(s), %u furniture state(s), %u collider state(s), %u power crystal state(s), %u boulder trap state(s), %u signal controller state(s), %u bell state(s), %u water-source state(s), %u campfire state(s), %u wall-lock state(s), %u wall-button state(s), %u pressure-plate state(s), %u tile override(s), %u pedestal state(s), %u chest state(s).",
    mapKey.c_str(),
    player,
    removalsSent,
    leversSent,
    gatesSent,
    doorsSent,
    furnitureSent,
    collidersSent,
    powerCrystalsSent,
    boulderTrapsSent,
    signalControllersSent,
    bellsSent,
	chestsSent,
    waterSourcesSent,
    campfiresSent,
    wallLocksSent,
    wallButtonsSent,
    pressurePlatesSent,
	pedestalsSent,
    tilesSent
);
}
/*
 * Called after physfsLoadMapFile() but before assignActions().
 *
 * On the first visit, this records all persistent IDs found in the
 * original .lmp entity list.
 *
 * On later visits, it removes entities previously recorded as gone.
 */
void applyPersistentMapRemovals()
{
    const std::string mapKey =
        getPersistentMapKey();

    if ( mapKey.empty() )
    {
        return;
    }

    /*
     * Clients may only apply a complete snapshot received from the
     * authoritative host. They still never capture or invent state.
     */
    if ( multiplayer == CLIENT )
    {
        if ( !clientPersistentSnapshotComplete )
        {
            printlog(
                "[Persistent World MP] Client cannot apply removals for '%s': snapshot is incomplete.",
                mapKey.c_str()
            );
            return;
        }

        if ( clientPersistentSnapshotMapKey != mapKey )
        {
            printlog(
                "[Persistent World MP] Client cannot apply removals: loaded '%s', snapshot belongs to '%s'.",
                mapKey.c_str(),
                clientPersistentSnapshotMapKey.c_str()
            );
            return;
        }
    }

	/*
	 * Procedural map generation creates raw editor sprites with persistentID 0.
	 * Assign deterministic IDs before assignActions() so monsters, fountains,
	 * traps, mechanisms, containers, furniture, decorations, exits, and room
	 * template entities can transfer the same identity into their runtime form.
	 *
	 * Preserve the older generated-breakable ordering first so saves produced
	 * by the previous breakable-only patch continue to resolve those IDs. Every
	 * remaining generated raw sprite is then assigned in map entity-list order.
	 * Barony already requires that order to match between host and client for
	 * map-load UIDs, so it is deterministic for a shared floor seed.
	 */
	std::vector<Entity*> generatedBreakables;
	std::unordered_set<Sint32> usedPersistentIDs;

	for ( node_t* node = map.entities->first;
		node != nullptr;
		node = node->next )
	{
		Entity* entity =
			static_cast<Entity*>(node->element);

		if ( !entity )
		{
			continue;
		}

		if ( entity->persistentID > 0 )
		{
			usedPersistentIDs.insert(entity->persistentID);
			continue;
		}

		if ( entity->sprite == 1 )
		{
			// Player Starts are reusable spawn infrastructure.
			continue;
		}

		bool persistentBreakable =
			entity->isDamageableCollider();

		if ( !persistentBreakable
			&& entity->sprite == 179 )
		{
			auto colliderData =
				EditorEntityData_t::colliderData.find(
					entity->colliderDamageTypes
				);

			if ( colliderData
				!= EditorEntityData_t::colliderData.end()
				&& colliderData->second.hasOverride("hp")
				&& colliderData->second.getOverride("hp") > 0 )
			{
				persistentBreakable = true;
			}
		}

		if ( persistentBreakable )
		{
			generatedBreakables.push_back(entity);
		}
	}

	std::stable_sort(
		generatedBreakables.begin(),
		generatedBreakables.end(),
		[](const Entity* first, const Entity* second)
		{
			return std::make_tuple(
				first->x,
				first->y,
				first->z,
				first->colliderDamageTypes,
				first->colliderDecorationModel,
				first->colliderDecorationRotation,
				first->sprite,
				first->colliderContainedEntity,
				first->colliderHasCollision
			) < std::make_tuple(
				second->x,
				second->y,
				second->z,
				second->colliderDamageTypes,
				second->colliderDecorationModel,
				second->colliderDecorationRotation,
				second->sprite,
				second->colliderContainedEntity,
				second->colliderHasCollision
			);
		}
	);

	Sint32 nextGeneratedPersistentID =
		GENERATED_MAP_ENTITY_PERSISTENT_ID_BASE;

	auto assignGeneratedID =
		[&usedPersistentIDs, &nextGeneratedPersistentID](Entity* entity) -> bool
		{
			while ( nextGeneratedPersistentID > 0
				&& usedPersistentIDs.find(nextGeneratedPersistentID)
					!= usedPersistentIDs.end() )
			{
				++nextGeneratedPersistentID;
			}

			if ( !entity || nextGeneratedPersistentID <= 0 )
			{
				return false;
			}

			entity->persistentID = nextGeneratedPersistentID;
			usedPersistentIDs.insert(nextGeneratedPersistentID);
			++nextGeneratedPersistentID;
			return true;
		};

	Uint32 assignedGeneratedBreakables = 0;
	for ( Entity* entity : generatedBreakables )
	{
		if ( entity->persistentID == 0
			&& assignGeneratedID(entity) )
		{
			++assignedGeneratedBreakables;
		}
	}

	Uint32 assignedGeneratedSprites = 0;
	for ( node_t* node = map.entities->first;
		node != nullptr;
		node = node->next )
	{
		Entity* entity =
			static_cast<Entity*>(node->element);

		if ( !entity
			|| entity->persistentID != 0
			|| entity->sprite == 1 )
		{
			continue;
		}

		if ( !assignGeneratedID(entity) )
		{
			printlog(
				"[Persistent World] Warning: exhausted generated map-sprite IDs in '%s'.",
				mapKey.c_str()
			);
			break;
		}

		++assignedGeneratedSprites;
	}

	if ( assignedGeneratedBreakables > 0
		|| assignedGeneratedSprites > 0 )
	{
		printlog(
			"[Persistent World] Assigned %u stable generated breakable ID(s) and %u additional generated map-sprite ID(s) in '%s'.",
			assignedGeneratedBreakables,
			assignedGeneratedSprites,
			mapKey.c_str()
		);
	}

    PersistentMapRemovalState& state =
        persistentMapRemovalRegistry[mapKey];
	    /*
     * The map currently contains the newly loaded/generated raw tiles.
     * Save that exact data as the baseline before applying persistent
     * overrides.
     *
     * Refresh this on every visit so authored edits to the .lmp are
     * recognized during development.
     */
    const size_t rawTileCount =
        static_cast<size_t>(map.width)
        * static_cast<size_t>(map.height)
        * static_cast<size_t>(MAPLAYERS);

    state.originalTiles.assign(
        map.tiles,
        map.tiles + rawTileCount
    );

    state.originalTileWidth =
        map.width;

    state.originalTileHeight =
        map.height;

    state.originalTileLayers =
        MAPLAYERS;

    Uint32 appliedTileStates = 0;
    Uint32 rejectedTileStates = 0;

    for ( const auto& tilePair :
        state.tileStates )
    {
        const PersistentTileState& tileState =
            tilePair.second;

        if ( tileState.x < 0
            || tileState.x >= map.width
            || tileState.y < 0
            || tileState.y >= map.height
            || tileState.layer < 0
            || tileState.layer >= MAPLAYERS )
        {
            ++rejectedTileStates;

            printlog(
                "[Persistent World] Ignored invalid tile override for '%s': x=%d y=%d layer=%d tile=%d.",
                mapKey.c_str(),
                tileState.x,
                tileState.y,
                tileState.layer,
                tileState.tile
            );

            continue;
        }

        const size_t index =
            static_cast<size_t>(
                tileState.layer
            )
            + static_cast<size_t>(
                tileState.y
            ) * MAPLAYERS
            + static_cast<size_t>(
                tileState.x
            ) * MAPLAYERS
                * map.height;

        map.tiles[index] =
            tileState.tile;

        ++appliedTileStates;
    }
    Uint32 registeredIDs = 0;

    /*
     * Always scan the newly loaded unmodified map before removing
     * anything. This also lets maps gain newly added editor entities
     * during development without losing their IDs.
     */
    for ( node_t* node = map.entities->first;
        node != nullptr;
        node = node->next )
    {
        Entity* entity =
            static_cast<Entity*>(node->element);

        if ( !entity || entity->persistentID <= 0 )
        {
            continue;
        }

        if ( isGeneratedMapEntityPersistentID(entity->persistentID) )
        {
            // Register generated IDs only after assignActions() confirms the
            // raw marker survived or transferred its identity.
            continue;
        }
		if ( entity->sprite == 1 )
		{
			// Player Starts are reusable spawn infrastructure, not world
			// objects whose absence should persist across unloads or restarts.
			state.originalEntityIDs.erase(entity->persistentID);
			state.removedEntityIDs.erase(entity->persistentID);
			continue;
		}

        const auto result =
            state.originalEntityIDs.insert(
                entity->persistentID
            );

        if ( result.second )
        {
            ++registeredIDs;
        }
    }

    state.originalIDsRegistered = true;

    Uint32 removedEntities = 0;

    for ( node_t* node = map.entities->first;
        node != nullptr; )
    {
        node_t* nextNode = node->next;

        Entity* entity =
            static_cast<Entity*>(node->element);

        if ( entity
            && entity->persistentID > 0
            && state.removedEntityIDs.find(
                entity->persistentID
            ) != state.removedEntityIDs.end() )
        {
            printlog(
                "[Persistent World] Removing previously deleted entity ID %d, sprite %d, from '%s'.",
                entity->persistentID,
                entity->sprite,
                mapKey.c_str()
            );

            list_RemoveNode(entity->mynode);
            ++removedEntities;
        }

        node = nextNode;
    }
    /*
 * Recreate surviving persistent runtime monsters.
 *
 * summonMonsterNoSmoke() builds the normal monster entity and Stat.
 * Its species initializer runs on the first monster tick, after which
 * actMonster() calls applyPersistentMonsterLivingState() and replaces
 * the generated state with the saved state.
 */
Uint32 restoredDynamicMonsterSpawns = 0;
Uint32 rejectedDynamicMonsterSpawns = 0;

if ( multiplayer != CLIENT )
{
    for ( const Sint32 dynamicPersistentID :
        state.dynamicMonsterIDs )
    {
        if ( dynamicPersistentID >= 0 )
        {
            ++rejectedDynamicMonsterSpawns;
            continue;
        }

        const auto savedStateIterator =
            state.mechanismStates.find(
                dynamicPersistentID
            );

        if ( savedStateIterator
            == state.mechanismStates.end() )
        {
            ++rejectedDynamicMonsterSpawns;

            printlog(
                "[Persistent World] Dynamic monster ID %d has no saved living state in '%s'.",
                dynamicPersistentID,
                mapKey.c_str()
            );

            continue;
        }

        const PersistentMechanismState& savedState =
            savedStateIterator->second;

        if ( savedState.kind
                != PersistentMechanismState::Kind::MonsterLivingState
            && savedState.kind
                != PersistentMechanismState::Kind::ShopkeeperInventory )
        {
            ++rejectedDynamicMonsterSpawns;
            continue;
        }

        if ( savedState.monsterSavedType <= NOTHING
            || savedState.monsterSavedType >= NUMMONSTERS
            || savedState.monsterSavedHP <= 0 )
        {
            ++rejectedDynamicMonsterSpawns;

            printlog(
                "[Persistent World] Rejected invalid dynamic monster ID %d in '%s': type=%d HP=%d.",
                dynamicPersistentID,
                mapKey.c_str(),
                savedState.monsterSavedType,
                savedState.monsterSavedHP
            );

            continue;
        }

        Entity* restoredMonster =
            summonMonsterNoSmoke(
                static_cast<Monster>(
                    savedState.monsterSavedType
                ),
                static_cast<long>(
                    savedState.monsterSavedX
                ),
                static_cast<long>(
                    savedState.monsterSavedY
                ),
                true
            );

        if ( !restoredMonster )
        {
            ++rejectedDynamicMonsterSpawns;

            printlog(
                "[Persistent World] Failed to recreate dynamic monster ID %d in '%s'.",
                dynamicPersistentID,
                mapKey.c_str()
            );

            continue;
        }

        restoredMonster->persistentID =
            dynamicPersistentID;

        restoredMonster->persistentDynamicMonster =
            true;

        /*
         * Set the stored transform immediately. The full persistent
         * living-state restore repeats this after species initialization.
         */
        restoredMonster->x =
            savedState.monsterSavedX;

        restoredMonster->y =
            savedState.monsterSavedY;

        restoredMonster->z =
            savedState.monsterSavedZ;

        restoredMonster->yaw =
            savedState.monsterSavedYaw;

        restoredMonster->pitch =
            savedState.monsterSavedPitch;

        restoredMonster->roll =
            savedState.monsterSavedRoll;

        ++restoredDynamicMonsterSpawns;

        printlog(
            "[Persistent World] Recreated dynamic monster ID %d, type %d, in '%s'.",
            dynamicPersistentID,
            savedState.monsterSavedType,
            mapKey.c_str()
        );
    }
}

printlog(
    "[Persistent World] Dynamic monster recreation for '%s': %u recreated, %u rejected.",
    mapKey.c_str(),
    restoredDynamicMonsterSpawns,
    rejectedDynamicMonsterSpawns
);
printlog(
    "[Persistent World] Loaded '%s': %zu original ID(s), %zu persistent removal(s), %u newly registered ID(s), %u entity removal(s) applied, %u tile override(s) applied, %u invalid tile override(s) rejected.",
    mapKey.c_str(),
    state.originalEntityIDs.size(),
    state.removedEntityIDs.size(),
    registeredIDs,
    removedEntities,
    appliedTileStates,
    rejectedTileStates
);
}
/*
 * Store one monster inventory/equipment item.
 */
static bool capturePersistentMonsterItem(
    const Item* item,
    Sint32 slot,
    std::vector<PersistentMonsterItemState>& savedItems
)
{
    if ( !item
        || item->type < 0
        || item->type >= NUMITEMS
        || item->count <= 0 )
    {
        return false;
    }

    PersistentMonsterItemState itemState;

    itemState.slot =
        slot;

    itemState.type =
        static_cast<Sint32>(
            item->type
        );
    itemState.stableId = persistentStableItemId(itemState.type);

    itemState.status =
        static_cast<Sint32>(
            item->status
        );

    itemState.beatitude =
        item->beatitude;

    itemState.count =
        item->count;

    itemState.appearance =
        item->appearance;

    itemState.identified =
        item->identified;

    itemState.isDroppable =
        item->isDroppable;

    itemState.x =
        item->x;

    itemState.y =
        item->y;

    savedItems.push_back(
        itemState
    );

    return true;
}
/*
 * Effects that represent meaningful temporary conditions which should
 * remain paused while a map is unloaded.
 *
 * Effects not listed here are allowed to be rebuilt by monster
 * initialization, equipment, spells, AI controllers, or other runtime
 * systems.
 */
static bool isPersistentMonsterEffect(
    Sint32 effectID
)
{
    switch ( effectID )
    {
        case EFF_ASLEEP:
        case EFF_POISONED:
        case EFF_STUNNED:
        case EFF_CONFUSED:
        case EFF_INVISIBLE:
        case EFF_BLIND:
        case EFF_FAST:
        case EFF_PARALYZED:
        case EFF_BLEEDING:
        case EFF_SLOW:
        case EFF_HP_REGEN:
        case EFF_MP_REGEN:
        case EFF_PACIFY:
        case EFF_WEBBED:
        case EFF_FEAR:
        case EFF_ROOTED:
        case EFF_NAUSEA_PROTECTION:
        case EFF_WEAKNESS:
        case EFF_SPORES:
        case EFF_FROST:
            return true;

        default:
            return false;
    }
}
/*
 * Copy the living state shared by ordinary monsters, NPCs and
 * shopkeepers.
 */
static bool capturePersistentMonsterLivingState(
    const Entity* entity,
    PersistentMechanismState& savedState
)
{
    if ( !entity
        || entity->behavior != &actMonster
        || entity->persistentID == 0 )
    {
        return false;
    }

    /*
    * Positive IDs are original map monsters.
    * Negative IDs are allowed only for explicitly persistent dynamic
    * monsters.
    */
    if ( entity->persistentID < 0
        && !entity->persistentDynamicMonster )
    {
        return false;
    }

    Stat* monsterStats =
        entity->getStats();

    if ( !monsterStats
        || monsterStats->type == NOTHING
        || monsterStats->HP <= 0 )
    {
        return false;
    }

    savedState.monsterSavedType =
        static_cast<Sint32>(
            monsterStats->type
        );

    savedState.monsterSavedHP =
        monsterStats->HP;

    savedState.monsterSavedMAXHP =
        monsterStats->MAXHP;

    savedState.monsterSavedMP =
        monsterStats->MP;

    savedState.monsterSavedMAXMP =
        monsterStats->MAXMP;

    /*
 * Capture selected finite-duration status effects.
 *
 * Positive timers represent temporary active effects. Timers such as
 * -1 and -2 are commonly used for permanent or sustained effects and
 * must be rebuilt by their owning game systems instead.
 */
savedState.monsterSavedEffects.clear();

for ( Sint32 effectID = 0;
    effectID < NUMEFFECTS;
    ++effectID )
{
    if ( !isPersistentMonsterEffect(effectID) )
    {
        continue;
    }

    const Uint8 effectValue =
        monsterStats->getEffectActive(
            effectID
        );

    const Sint32 effectTimer =
        monsterStats->EFFECTS_TIMERS[effectID];

    if ( effectValue == 0
        || effectTimer <= 0 )
    {
        continue;
    }

    PersistentMonsterEffectState effectState;

    effectState.effectID =
        effectID;

    effectState.effectValue =
        effectValue;

    effectState.timer =
        effectTimer;

    savedState.monsterSavedEffects.push_back(
        effectState
    );
}
    
    /*
 * Capture the monster's original randomized loadout on the first map
 * departure, then capture any later inventory changes on subsequent
 * departures.
 *
 * Shopkeepers use their separate store-inventory persistence logic.
 */
savedState.monsterSavedItems.clear();

if ( monsterStats->type != SHOPKEEPER )
{
    std::unordered_set<const Item*>
        capturedItems;

    auto getMonsterItemSlot =
        [monsterStats](const Item* item) -> Sint32
        {
            if ( item == monsterStats->weapon )
            {
                return 1;
            }

            if ( item == monsterStats->shield )
            {
                return 2;
            }

            if ( item == monsterStats->helmet )
            {
                return 3;
            }

            if ( item == monsterStats->breastplate )
            {
                return 4;
            }

            if ( item == monsterStats->gloves )
            {
                return 5;
            }

            if ( item == monsterStats->shoes )
            {
                return 6;
            }

            if ( item == monsterStats->cloak )
            {
                return 7;
            }

            if ( item == monsterStats->amulet )
            {
                return 8;
            }

            if ( item == monsterStats->ring )
            {
                return 9;
            }

            if ( item == monsterStats->mask )
            {
                return 10;
            }

            return 0;
        };

    /*
     * Capture every item already linked to the Stat inventory.
     *
     * Equipped items may also be nodes in this list, so classify them
     * by pointer equality instead of saving a second duplicate copy.
     */
    for ( node_t* itemNode =
            monsterStats->inventory.first;
        itemNode != nullptr;
        itemNode = itemNode->next )
    {
        Item* item =
            static_cast<Item*>(
                itemNode->element
            );

        if ( !item
            || capturedItems.find(item)
                != capturedItems.end() )
        {
            continue;
        }

        capturePersistentMonsterItem(
            item,
            getMonsterItemSlot(item),
            savedState.monsterSavedItems
        );

        capturedItems.insert(item);
    }

    /*
     * Some monster equipment is allocated outside the ordinary
     * inventory list. Capture those detached pointers too.
     */
    auto captureDetachedEquipment =
        [&savedState, &capturedItems](
            Item* item,
            Sint32 slot
        )
        {
            if ( !item
                || capturedItems.find(item)
                    != capturedItems.end() )
            {
                return;
            }

            capturePersistentMonsterItem(
                item,
                slot,
                savedState.monsterSavedItems
            );

            capturedItems.insert(item);
        };

    captureDetachedEquipment(
        monsterStats->weapon,
        1
    );

    captureDetachedEquipment(
        monsterStats->shield,
        2
    );

    captureDetachedEquipment(
        monsterStats->helmet,
        3
    );

    captureDetachedEquipment(
        monsterStats->breastplate,
        4
    );

    captureDetachedEquipment(
        monsterStats->gloves,
        5
    );

    captureDetachedEquipment(
        monsterStats->shoes,
        6
    );

    captureDetachedEquipment(
        monsterStats->cloak,
        7
    );

    captureDetachedEquipment(
        monsterStats->amulet,
        8
    );

    captureDetachedEquipment(
        monsterStats->ring,
        9
    );

    captureDetachedEquipment(
        monsterStats->mask,
        10
    );
}

    savedState.monsterSavedX =
        entity->x;

    savedState.monsterSavedY =
        entity->y;

    savedState.monsterSavedZ =
        entity->z;

    savedState.monsterSavedYaw =
        entity->yaw;

    savedState.monsterSavedPitch =
        entity->pitch;

    savedState.monsterSavedRoll =
        entity->roll;

    return true;
}
/*
 * Copy one visible/world gold pile into persistent storage.
 */
static bool capturePersistentGoldBagState(
    const Entity* entity,
    PersistentGoldBagState& savedState
)
{
    if ( !entity
        || entity->behavior != &actGoldBag
        || entity->goldInContainer != 0
        || entity->goldAmount <= 0 )
    {
        return false;
    }

    savedState.amount =
        entity->goldAmount;

    savedState.amountBonus =
        entity->goldAmountBonus;

    savedState.sokoban =
        entity->goldSokoban;

    savedState.bouncing =
        entity->goldBouncing;

    savedState.droppedByPlayer =
        entity->goldDroppedByPlayer;

    savedState.x =
        entity->x;

    savedState.y =
        entity->y;

    savedState.z =
        entity->z;

    savedState.yaw =
        entity->yaw;

    savedState.pitch =
        entity->pitch;

    savedState.roll =
        entity->roll;

    savedState.velX =
        entity->vel_x;

    savedState.velY =
        entity->vel_y;

    savedState.velZ =
        entity->vel_z;

    savedState.passable =
        entity->flags[PASSABLE];

    savedState.invisible =
        entity->flags[INVISIBLE];

    return true;
}
/*
 * Apply saved gold state to either an original map gold entity or a
 * newly created dynamic gold entity.
 */
static bool applyPersistentGoldBagState(
    Entity* entity,
    const PersistentGoldBagState& savedState
)
{
    if ( !entity
        || savedState.amount <= 0 )
    {
        return false;
    }

    entity->behavior =
        &actGoldBag;

    entity->sizex = 4;
    entity->sizey = 4;

    entity->goldAmount =
        savedState.amount;

    entity->goldAmountBonus =
        savedState.amountBonus;

    entity->goldSokoban =
        savedState.sokoban;

    entity->goldBouncing =
        savedState.bouncing;

    entity->goldDroppedByPlayer =
        savedState.droppedByPlayer;

    /*
     * The original parent UID cannot be reused after loading another
     * map. Restored gold is a free-standing world entity.
     */
    entity->goldInContainer = 0;
    entity->parent = 0;

    /*
     * Sound ambience and client-only telepathy are recalculated.
     */
    entity->goldAmbience = 0;
    entity->goldTelepathy = 0;

    entity->x =
        savedState.x;

    entity->y =
        savedState.y;

    entity->z =
        savedState.z;

    entity->new_x =
        savedState.x;

    entity->new_y =
        savedState.y;

    entity->new_z =
        savedState.z;

    entity->yaw =
        savedState.yaw;

    entity->pitch =
        savedState.pitch;

    entity->roll =
        savedState.roll;

    entity->new_yaw =
        savedState.yaw;

    entity->new_pitch =
        savedState.pitch;

    entity->new_roll =
        savedState.roll;

    entity->vel_x =
        savedState.velX;

    entity->vel_y =
        savedState.velY;

    entity->vel_z =
        savedState.velZ;

    entity->flags[PASSABLE] =
        savedState.passable;

    entity->flags[INVISIBLE] =
        savedState.invisible;

    entity->flags[UPDATENEEDED] =
        true;

    entity->flags[BURNING] =
        false;

    /*
     * Barony uses a loose coin model for fewer than five gold and the
     * normal gold-bag model for larger piles.
     */
    entity->sprite =
        savedState.amount < 5
            ? 1379
            : 130;

    entity->bNeedsRenderPositionInit =
        true;

    return true;
}
/*
 * Copy a live floor-item entity into persistent storage.
 *
 * Items hidden inside another world entity are excluded. Their parent
 * mechanism owns their state and recreates them when appropriate.
 */
static bool capturePersistentWorldItemState(
    const Entity* entity,
    PersistentWorldItemState& savedState
)
{
    if ( !entity
        || entity->behavior != &actItem
        || entity->itemContainer != 0
        || entity->skill[10] < 0
        || entity->skill[10] >= NUMITEMS
        || entity->skill[13] <= 0 )
    {
        return false;
    }

    /*
     * Duck items immediately transform into duck entities inside
     * actItem(). They should not be restored as ordinary floor items.
     */
    if ( entity->skill[10] == TOOL_DUCK )
    {
        return false;
    }

    savedState.type =
        entity->skill[10];
    savedState.stableId = persistentStableItemId(savedState.type);

    savedState.status =
        entity->skill[11];

    savedState.beatitude =
        entity->skill[12];

    savedState.count =
        entity->skill[13];

    savedState.appearance =
        static_cast<Uint32>(
            entity->skill[14]
        );

    savedState.identified =
        entity->skill[15] != 0;

    savedState.x =
        entity->x;

    savedState.y =
        entity->y;

    savedState.z =
        entity->z;

    savedState.yaw =
        entity->yaw;

    savedState.pitch =
        entity->pitch;

    savedState.roll =
        entity->roll;

    savedState.velX =
        entity->vel_x;

    savedState.velY =
        entity->vel_y;

    savedState.velZ =
        entity->vel_z;

    savedState.itemNotMoving =
        entity->itemNotMoving;

    savedState.itemSokobanReward =
        entity->itemSokobanReward;

    savedState.itemStolen =
        entity->itemStolen;

    savedState.itemShowOnMap =
        entity->itemShowOnMap;

    savedState.itemDelayMonsterPickingUp =
        entity->itemDelayMonsterPickingUp;

    savedState.passable =
        entity->flags[PASSABLE];

    savedState.invisible =
        entity->flags[INVISIBLE];

    savedState.burning =
        entity->flags[BURNING];

    savedState.burnable =
        entity->flags[BURNABLE];

    return true;
}
/*
 * Apply a persistent floor-item state to an existing or newly allocated
 * actItem entity.
 */
static bool applyPersistentWorldItemState(
    Entity* entity,
    const PersistentWorldItemState& savedState
)
{
    const Sint32 resolvedType = resolvePersistentItemType(
        savedState.stableId,
        savedState.type
    );
    if ( !entity
        || resolvedType < 0
        || savedState.count <= 0
        || resolvedType == TOOL_DUCK )
    {
        return false;
    }

    entity->behavior =
        &actItem;

    entity->sizex = 4;
    entity->sizey = 4;

    entity->skill[10] =
        resolvedType;

    entity->skill[11] =
        savedState.status;

    entity->skill[12] =
        savedState.beatitude;

    entity->skill[13] =
        savedState.count;

    entity->skill[14] =
        static_cast<Sint32>(
            savedState.appearance
        );

    entity->skill[15] =
        savedState.identified ? 1 : 0;

    /*
     * Reset item life so actItem() safely rebuilds its tooltip and any
     * ordinary first-tick presentation state.
     */
    entity->skill[16] = 0;

    entity->x =
        savedState.x;

    entity->y =
        savedState.y;

    entity->z =
        savedState.z;

    entity->new_x =
        savedState.x;

    entity->new_y =
        savedState.y;

    entity->new_z =
        savedState.z;

    entity->yaw =
        savedState.yaw;

    entity->pitch =
        savedState.pitch;

    entity->roll =
        savedState.roll;

    entity->new_yaw =
        savedState.yaw;

    entity->new_pitch =
        savedState.pitch;

    entity->new_roll =
        savedState.roll;

    entity->vel_x =
        savedState.velX;

    entity->vel_y =
        savedState.velY;

    entity->vel_z =
        savedState.velZ;

    entity->itemNotMoving =
        savedState.itemNotMoving;

    entity->itemNotMovingClient =
        savedState.itemNotMoving;

    entity->itemSokobanReward =
        savedState.itemSokobanReward;

    entity->itemStolen =
        savedState.itemStolen;

    entity->itemShowOnMap =
        savedState.itemShowOnMap;

    entity->itemDelayMonsterPickingUp =
        savedState.itemDelayMonsterPickingUp;

    /*
     * Runtime UID relationships cannot safely cross map loads.
     */
    entity->parent = 0;
    entity->itemOriginalOwner = 0;
    entity->itemAutoSalvageByPlayer = 0;
    entity->itemFollowUID = 0;
    entity->itemReturnUID = 0;
    entity->itemContainer = 0;

    entity->flags[PASSABLE] =
        savedState.passable;

    entity->flags[INVISIBLE] =
        savedState.invisible;

    entity->flags[BURNING] =
        savedState.burning;

    entity->flags[BURNABLE] =
        savedState.burnable;

    entity->flags[UPDATENEEDED] =
        true;

    entity->bNeedsRenderPositionInit =
        true;

    /*
     * Calculate the correct model immediately instead of waiting for
     * the first actItem() tick.
     */
    Item* temporaryItem =
        newItemFromEntity(
            entity,
            true
        );

    if ( temporaryItem )
    {
        entity->sprite =
            itemModel(temporaryItem);

        free(temporaryItem);
    }

    return true;
}
/*
 * Convert a boulder-trap behavior function into a stable persistence
 * type. Zero means that the entity is not a supported boulder trap.
 */
static Sint32 getPersistentBoulderTrapBehavior(
    const Entity* entity
)
{
    if ( !entity )
    {
        return 0;
    }

    if ( entity->behavior == &actBoulderTrap )
    {
        return 1;
    }

    if ( entity->behavior == &actBoulderTrapEast )
    {
        return 2;
    }

    if ( entity->behavior == &actBoulderTrapSouth )
    {
        return 3;
    }

    if ( entity->behavior == &actBoulderTrapWest )
    {
        return 4;
    }

    if ( entity->behavior == &actBoulderTrapNorth )
    {
        return 5;
    }

    return 0;
}
/*
 * Capture the current state of surviving persistent levers and gates
 * before the current map is replaced.
 */
static void capturePersistentMechanismStates()
{
    if ( multiplayer == CLIENT )
    {
        return;
    }

    const std::string mapKey =
        getPersistentMapKey();

    if ( mapKey.empty() )
    {
        return;
    }

    PersistentMapRemovalState& mapState =
        persistentMapRemovalRegistry[mapKey];
    /*
     * Capture every tile that differs from the raw loaded baseline.
     *
     * Build a fresh collection each time. This allows a tile that was
     * changed and later restored to its original value to disappear
     * from persistent storage.
     */
    const size_t currentTileCount =
        static_cast<size_t>(map.width)
        * static_cast<size_t>(map.height)
        * static_cast<size_t>(MAPLAYERS);

    const bool baselineMatchesMap =
        mapState.originalTiles.size()
            == currentTileCount
        && mapState.originalTileWidth
            == map.width
        && mapState.originalTileHeight
            == map.height
        && mapState.originalTileLayers
            == MAPLAYERS;

    if ( !baselineMatchesMap )
    {
        /*
         * Defensive fallback. Normally the baseline is registered by
         * applyPersistentMapRemovals() immediately after map loading.
         *
         * Do not interpret an absent or dimension-mismatched baseline
         * as every tile in the map having changed.
         */
        mapState.originalTiles.assign(
            map.tiles,
            map.tiles + currentTileCount
        );

        mapState.originalTileWidth =
            map.width;

        mapState.originalTileHeight =
            map.height;

        mapState.originalTileLayers =
            MAPLAYERS;

        mapState.tileStates.clear();

        printlog(
            "[Persistent World] Rebuilt missing tile baseline for '%s': %zu tile value(s).",
            mapKey.c_str(),
            currentTileCount
        );
    }
    else
    {
        std::unordered_map<
            Uint64,
            PersistentTileState
        > currentTileStates;

        for ( Sint32 x = 0;
            x < map.width;
            ++x )
        {
            for ( Sint32 y = 0;
                y < map.height;
                ++y )
            {
                for ( Sint32 layer = 0;
                    layer < MAPLAYERS;
                    ++layer )
                {
                    const size_t index =
                        static_cast<size_t>(layer)
                        + static_cast<size_t>(y)
                            * MAPLAYERS
                        + static_cast<size_t>(x)
                            * MAPLAYERS
                            * map.height;

                    const Sint32 currentTile =
                        map.tiles[index];

                    const Sint32 originalTile =
                        mapState.originalTiles[index];

                    if ( currentTile == originalTile )
                    {
                        continue;
                    }

                    PersistentTileState tileState;

                    tileState.x = x;
                    tileState.y = y;
                    tileState.layer = layer;
                    tileState.tile = currentTile;

                    currentTileStates[
                        makePersistentTileKey(
                            x,
                            y,
                            layer
                        )
                    ] = tileState;
                }
            }
        }

        mapState.tileStates =
            std::move(currentTileStates);

        printlog(
            "[Persistent World] Captured %zu changed tile value(s) for '%s'.",
            mapState.tileStates.size(),
            mapKey.c_str()
        );
    }
    Uint32 capturedLevers = 0;
    Uint32 capturedTimedLevers = 0;
    Uint32 capturedGates = 0;
	Uint32 capturedDoors = 0;
	Uint32 capturedFurniture = 0;
	Uint32 capturedColliders = 0;
	Uint32 capturedPowerCrystals = 0;
	Uint32 capturedBoulderTraps = 0;
	Uint32 capturedSignalTimers = 0;
	Uint32 capturedANDGates = 0;
	Uint32 capturedDynamicBoulders = 0;
	Uint32 capturedBells = 0;
	Uint32 capturedSinks = 0;
	Uint32 capturedFountains = 0;
	Uint32 capturedCampfires = 0;
	Uint32 capturedWallLocks = 0;
	Uint32 capturedWallButtons = 0;
	Uint32 capturedNormalPressurePlates = 0;
	Uint32 capturedPermanentPressurePlates = 0;
	Uint32 capturedPedestals = 0;
	Uint32 capturedChests = 0;
	Uint32 capturedChestItems = 0;
	Uint32 capturedShopkeepers = 0;
	Uint32 capturedShopItems = 0;
    Uint32 capturedOriginalWorldItems = 0;
    Uint32 capturedDynamicWorldItems = 0;
    Uint32 capturedOriginalGoldBags = 0;
    Uint32 capturedDynamicGoldBags = 0;
    Uint32 capturedSummonTraps = 0;
	/*
	* Dynamic entities are a current-world snapshot. Replace the previous
	* snapshot so destroyed boulders do not return later.
	*/
    mapState.dynamicBoulders.clear();
    mapState.dynamicWorldItems.clear();
    mapState.dynamicGoldBags.clear();
    mapState.dynamicMonsterIDs.clear();

    /*
    * Remove the previous dynamic-monster state records.
    *
    * Positive records belong to original map entities and must remain.
    * The current surviving dynamic monsters will create a fresh negative-ID
    * snapshot below.
    */
    for ( auto stateIterator =
            mapState.mechanismStates.begin();
        stateIterator != mapState.mechanismStates.end(); )
    {
        if ( stateIterator->first < 0 )
        {
            stateIterator =
                mapState.mechanismStates.erase(
                    stateIterator
                );
        }
        else
        {
            ++stateIterator;
        }
    }
    for ( node_t* node = map.entities->first;
        node != nullptr;
        node = node->next )
    {
    Entity* entity =
        static_cast<Entity*>(node->element);

    if ( !entity )
    {
        continue;
    }

    /*
    * Assign a stable negative ID the first time an explicitly persistent
    * runtime monster is captured.
    */
    if ( entity->behavior == &actMonster
        && entity->persistentDynamicMonster
        && entity->persistentID == 0 )
    {
        entity->persistentID =
            nextDynamicMonsterPersistentID;

        --nextDynamicMonsterPersistentID;

        /*
        * Keep zero reserved even if the signed counter eventually wraps.
        * Reaching this would require more than two billion dynamic monsters
        * in one session, but preserve the invariant defensively.
        */
        if ( nextDynamicMonsterPersistentID == 0 )
        {
            nextDynamicMonsterPersistentID = -1;
        }

        printlog(
            "[Persistent World] Assigned dynamic monster persistent ID %d.",
            entity->persistentID
        );
    }

    /*
    * Preserve ordinary positive-ID entities.
    *
    * Negative IDs are accepted only for explicitly marked dynamic
    * monsters. All other runtime entities remain nonpersistent.
    */
    if ( entity->persistentID == 0 )
    {
        continue;
    }

    if ( entity->persistentID < 0
        && (
            entity->behavior != &actMonster
            || !entity->persistentDynamicMonster
        ) )
    {
        continue;
    }

    PersistentMechanismState mechanismState;

        if ( entity->behavior == &actSwitch )
        {
            mechanismState.kind =
                PersistentMechanismState::Kind::Lever;

			mechanismState.switchPower =
				entity->skill[0];

            mechanismState.roll =
                entity->roll;

            mapState.mechanismStates[
                entity->persistentID
            ] = mechanismState;

            ++capturedLevers;
        }
		        else if ( entity->behavior == &actPedestalBase )
        {
            mechanismState.kind =
                PersistentMechanismState::Kind::Pedestal;

            mechanismState.pedestalSavedHasOrb =
                entity->pedestalHasOrb;

            mechanismState.pedestalSavedPowerStatus =
                entity->pedestalPowerStatus;

            mechanismState.pedestalSavedInit =
                entity->pedestalInit;

            mechanismState.pedestalSavedInGround =
                entity->pedestalInGround;

            mechanismState.pedestalSavedZ =
                entity->z;

            mechanismState.pedestalSavedVelZ =
                entity->vel_z;

            mechanismState.pedestalSavedPassable =
                entity->flags[PASSABLE];

            mapState.mechanismStates[
                entity->persistentID
            ] = mechanismState;

            ++capturedPedestals;
        }
        else if ( entity->behavior == &actSummonTrap )
        {
            mechanismState.kind =
                PersistentMechanismState::Kind::SummonTrap;

            mechanismState.summonTrapMonster =
                entity->skill[0];

            mechanismState.summonTrapCount =
                entity->skill[1];

            mechanismState.summonTrapInterval =
                entity->skill[2];

            mechanismState.summonTrapSpawnCycles =
                entity->skill[3];

            mechanismState.summonTrapPowerToDisable =
                entity->skill[4];

            mechanismState.summonTrapFailureRate =
                entity->skill[5];

            mechanismState.summonTrapFired =
                entity->skill[6];

            mechanismState.summonTrapInitialized =
                entity->skill[7];

            mechanismState.summonTrapTicksToFire =
                entity->skill[8];

            mechanismState.summonTrapPlayerProximity =
                entity->skill[9];

            mapState.mechanismStates[
                entity->persistentID
            ] = mechanismState;

            ++capturedSummonTraps;
        }
		else if ( entity->behavior == &actMonster )
                {
                    Stat* monsterStats =
                        entity->getStats();

                    if ( !monsterStats
                        || monsterStats->type == NOTHING
                        || monsterStats->HP <= 0 )
                    {
                        continue;
                    }

                    /*
                    * Preserve the shopkeeper restock counter before replacing its
                    * previous mechanism-state record.
                    */
                    Sint32 previousLoadsSinceRestock = 0;

                    const auto previousStateIterator =
                        mapState.mechanismStates.find(
                            entity->persistentID
                        );

                    if ( previousStateIterator
                        != mapState.mechanismStates.end()
                        && previousStateIterator->second.kind
                            == PersistentMechanismState::Kind::ShopkeeperInventory )
                    {
                        previousLoadsSinceRestock =
                            previousStateIterator
                                ->second
                                .shopkeeperLoadsSinceRestock;
                    }

                    if ( monsterStats->type == SHOPKEEPER )
                    {
                        mechanismState.kind =
                            PersistentMechanismState::Kind::ShopkeeperInventory;

                        mechanismState.shopkeeperLoadsSinceRestock =
                            previousLoadsSinceRestock;

                        mechanismState.shopkeeperSavedStoreType =
                            entity->monsterStoreType;

                        mechanismState.shopkeeperSavedGold =
                            monsterStats->GOLD;

                        mechanismState.shopkeeperSavedName =
                            monsterStats->name;

                        for ( node_t* itemNode =
                                monsterStats->inventory.first;
                            itemNode != nullptr;
                            itemNode = itemNode->next )
                        {
                            Item* item =
                                static_cast<Item*>(
                                    itemNode->element
                                );

                            if ( !item
                                || item->count <= 0 )
                            {
                                continue;
                            }

                            PersistentChestItemState itemState;

                            itemState.type =
                                static_cast<Sint32>(
                                    item->type
                                );
                            itemState.stableId = persistentStableItemId(itemState.type);

                            itemState.status =
                                static_cast<Sint32>(
                                    item->status
                                );

                            itemState.beatitude =
                                item->beatitude;

                            itemState.count =
                                item->count;

                            itemState.appearance =
                                item->appearance;

                            itemState.identified =
                                item->identified;

                            itemState.x =
                                item->x;

                            itemState.y =
                                item->y;

                            itemState.isDroppable =
                                item->isDroppable;

                            itemState.playerSoldItemToShop =
                                item->playerSoldItemToShop;

                            itemState.itemSpecialShopConsumable =
                                item->itemSpecialShopConsumable;

                            itemState.itemRequireTradingSkillInShop =
                                item->itemRequireTradingSkillInShop;

                            mechanismState
                                .shopkeeperSavedInventory
                                .push_back(itemState);
                        }
                    }
                    else
                    {
                        mechanismState.kind =
                            PersistentMechanismState::Kind::MonsterLivingState;
                    }

                    if ( !capturePersistentMonsterLivingState(
                        entity,
                        mechanismState
                    ) )
                    {
                        continue;
                    }

                    mapState.mechanismStates[
                        entity->persistentID
                    ] = mechanismState;

                    if ( entity->persistentID < 0
                        && entity->persistentDynamicMonster )
                    {
                        mapState.dynamicMonsterIDs.insert(
                            entity->persistentID
                        );
                    }
                }
		        else if ( entity->behavior == &actChest )
        {
            mechanismState.kind =
                PersistentMechanismState::Kind::Chest;

            mechanismState.chestSavedHealth =
                entity->chestHealth;

            mechanismState.chestSavedMaxHealth =
                entity->chestMaxHealth;

            mechanismState.chestSavedLocked =
                entity->chestLocked;

            mechanismState.chestSavedLockpickHealth =
                entity->chestLockpickHealth;

            mechanismState.chestSavedPreventExploit =
                entity->chestPreventLockpickCapstoneExploit;

            mechanismState.chestSavedOldHealth =
                entity->chestOldHealth;

            mechanismState.chestSavedVoidState =
                entity->chestVoidState;

            /*
             * Void chests use stats[0]->void_chest_inventory, which is
             * global state rather than inventory owned by this map
             * entity. Never copy it into a per-map chest snapshot.
             */
            if ( entity->chestVoidState == 0 )
            {
                list_t* inventory =
                    entity->getChestInventoryList();

                if ( inventory )
                {
                    for ( node_t* itemNode =
                            inventory->first;
                        itemNode != nullptr;
                        itemNode = itemNode->next )
                    {
                        Item* item =
                            static_cast<Item*>(
                                itemNode->element
                            );

                        if ( !item
                            || item->count <= 0 )
                        {
                            continue;
                        }

                        PersistentChestItemState itemState;

                        itemState.type =
                            static_cast<Sint32>(
                                item->type
                            );
                        itemState.stableId = persistentStableItemId(itemState.type);

                        itemState.status =
                            static_cast<Sint32>(
                                item->status
                            );

                        itemState.beatitude =
                            item->beatitude;

                        itemState.count =
                            item->count;

                        itemState.appearance =
                            item->appearance;

                        itemState.identified =
                            item->identified;

                        itemState.x =
                            item->x;

                        itemState.y =
                            item->y;

                        mechanismState
                            .chestSavedInventory
                            .push_back(itemState);

                        ++capturedChestItems;
                    }
                }
            }

            mapState.mechanismStates[
                entity->persistentID
            ] = std::move(mechanismState);

            ++capturedChests;
        }
        else if ( entity->behavior
            == &actSwitchWithTimer )
        {
            mechanismState.kind =
                PersistentMechanismState::Kind::TimedLever;

			mechanismState.switchPower =
				entity->skill[0];

            mechanismState.leverStatus =
                entity->leverStatus;

            mechanismState.leverTimerTicks =
                entity->leverTimerTicks;

            mechanismState.roll =
                entity->roll;

            mapState.mechanismStates[
                entity->persistentID
            ] = mechanismState;

            ++capturedTimedLevers;
        }
		        else if ( entity->behavior == &actWallButton )
        {
            mechanismState.kind =
                PersistentMechanismState::Kind::WallButton;

            mechanismState.wallButtonState =
                entity->wallLockState;

            mechanismState.wallButtonPower =
                entity->wallLockPower;

            mapState.mechanismStates[
                entity->persistentID
            ] = mechanismState;

            ++capturedWallButtons;
        }
        else if ( entity->behavior == &actGate )
        {
            mechanismState.kind =
                PersistentMechanismState::Kind::Gate;

            mechanismState.gateStatus =
                entity->gateStatus;

            mechanismState.gateInit =
                entity->gateInit;

            mechanismState.gateRattle =
                entity->gateRattle;

            mechanismState.gateInverted =
                entity->gateInverted;

            mechanismState.circuitStatus =
    		entity->skill[28];

            mechanismState.gateStartHeight =
                entity->gateStartHeight;

            mechanismState.gateZ =
                entity->z;

            mechanismState.gateVelZ =
                entity->gateVelZ;

            mechanismState.passable =
                entity->flags[PASSABLE];

            mapState.mechanismStates[
                entity->persistentID
            ] = mechanismState;

            ++capturedGates;
        }
		else if ( entity->behavior == &actDoor
			|| entity->behavior == &actIronDoor )
		{
			mechanismState.kind =
				PersistentMechanismState::Kind::Door;

			mechanismState.doorIsIron =
				entity->behavior == &actIronDoor;

			mechanismState.doorDir =
				entity->doorDir;

			mechanismState.doorStatus =
				entity->doorStatus;

			mechanismState.doorHealth =
				entity->doorHealth;

			mechanismState.doorMaxHealth =
				entity->doorMaxHealth;

			mechanismState.doorLocked =
				entity->doorLocked;

			mechanismState.doorSmacked =
				entity->doorSmacked;

			mechanismState.doorTimer =
				entity->doorTimer;

			mechanismState.doorPreventLockpickExploit =
				entity->doorPreventLockpickExploit;

			mechanismState.doorForceLockedUnlocked =
				entity->doorForceLockedUnlocked;

			mechanismState.doorDisableLockpicks =
				entity->doorDisableLockpicks;

			mechanismState.doorDisableOpening =
				entity->doorDisableOpening;

			mechanismState.doorLockpickHealth =
				entity->doorLockpickHealth;

			mechanismState.doorUnlockWhenPowered =
				entity->doorUnlockWhenPowered;

			mechanismState.doorCircuitStatus =
				entity->skill[28];

			mechanismState.doorStartAng =
				entity->doorStartAng;

			mechanismState.doorYaw =
				entity->yaw;

			/*
			* Open doors shift their runtime x/y collision position by five
			* units, so exact coordinates must be saved.
			*/
			mechanismState.doorX =
				entity->x;

			mechanismState.doorY =
				entity->y;

			mechanismState.doorFocalY =
				entity->focaly;

			mechanismState.passable =
				entity->flags[PASSABLE];

			mechanismState.doorBurning =
				entity->flags[BURNING];

			mechanismState.doorBurnable =
				entity->flags[BURNABLE];

			mapState.mechanismStates[
				entity->persistentID
			] = mechanismState;

			++capturedDoors;
		}
        else if ( entity->behavior == &actGoldBag )
        {
            PersistentGoldBagState goldState;

            if ( !capturePersistentGoldBagState(
                entity,
                goldState
            ) )
            {
                continue;
            }

            mechanismState.kind =
                PersistentMechanismState::Kind::GoldBag;

            mechanismState.goldBagState =
                goldState;

            mapState.mechanismStates[
                entity->persistentID
            ] = mechanismState;

            ++capturedOriginalGoldBags;
        }
        else if ( entity->behavior == &actItem )
        {
            PersistentWorldItemState itemState;

            if ( !capturePersistentWorldItemState(
                entity,
                itemState
            ) )
            {
                continue;
            }

            mechanismState.kind =
                PersistentMechanismState::Kind::WorldItem;

            mechanismState.worldItemState =
                itemState;

            mapState.mechanismStates[
                entity->persistentID
            ] = mechanismState;

            ++capturedOriginalWorldItems;
        }
		else if ( entity->behavior == &actFurniture )
		{
			mechanismState.kind =
				PersistentMechanismState::Kind::Furniture;

			mechanismState.furnitureType =
				entity->furnitureType;

			mechanismState.furnitureHealth =
				entity->furnitureHealth;

			mechanismState.furnitureMaxHealth =
				entity->furnitureMaxHealth;

			mechanismState.furnitureBurning =
				entity->flags[BURNING];

			mechanismState.furnitureBurnable =
				entity->flags[BURNABLE];

			mapState.mechanismStates[
				entity->persistentID
			] = mechanismState;

			++capturedFurniture;
		}
		else if ( entity->behavior == &actColliderDecoration
			&& entity->isDamageableCollider() )
		{
			mechanismState.kind =
				PersistentMechanismState::Kind::ColliderDecoration;

			mechanismState.colliderCurrentHP =
				entity->colliderCurrentHP;

			mechanismState.colliderMaxHP =
				entity->colliderMaxHP;

			mechanismState.colliderDamageTypes =
				entity->colliderDamageTypes;

			mechanismState.colliderHasCollision =
				entity->colliderHasCollision;

			mechanismState.colliderBurning =
				entity->flags[BURNING];

			mechanismState.colliderBurnable =
				entity->flags[BURNABLE];

			mapState.mechanismStates[
				entity->persistentID
			] = mechanismState;

			++capturedColliders;
		}
		        else if (
            entity->behavior == &actSink
            || entity->behavior == &actFountain
        )
        {
            mechanismState.kind =
                PersistentMechanismState::Kind::SinkOrFountain;

            mechanismState.waterSourceIsFountain =
                entity->behavior == &actFountain;

            /*
             * skill[0]:
             * sink     = remaining uses
             * fountain = full/dry
             */
            mechanismState.waterSourceUses =
                entity->skill[0];

            if ( mechanismState.waterSourceIsFountain )
            {
                /*
                 * Fountain main effect and potion effect.
                 */
                mechanismState.waterSourceMainEffect =
                    entity->skill[1];

                mechanismState.waterSourceSecondaryEffect =
                    entity->skill[3];

                ++capturedFountains;
            }
            else
            {
                /*
                 * A sink chooses the effect of its next use in skill[3].
                 * skill[1] is not meaningful sink runtime state.
                 */
                mechanismState.waterSourceMainEffect = 0;

                mechanismState.waterSourceSecondaryEffect =
                    entity->skill[3];

                ++capturedSinks;
            }

            mapState.mechanismStates[
                entity->persistentID
            ] = mechanismState;
        }
		        else if ( entity->behavior == &actBell )
        {
            mechanismState.kind =
                PersistentMechanismState::Kind::Bell;

            /*
             * Bell runtime aliases from actgeneral.cpp:
             *
             * skill[0]  active timer
             * skill[1]  hidden item availability
             * skill[4]  remaining uses
             * skill[5]  current event
             * skill[7]  interaction delay
             * skill[9]  broken clapper
             * skill[10] broken bulb
             * skill[11] selected buff
             * skill[12] burning timer
             */
            mechanismState.bellActiveTimer =
                entity->skill[0];

            mechanismState.bellHasItem =
                entity->skill[1];

            mechanismState.bellUses =
                entity->skill[4];

            mechanismState.bellCurrentEvent =
                entity->skill[5];

            mechanismState.bellUseDelay =
                entity->skill[7];

            mechanismState.bellClapperBroken =
                entity->skill[9];

            mechanismState.bellBulbBroken =
                entity->skill[10];

            mechanismState.bellBuffType =
                entity->skill[11];

            mechanismState.bellBurningTimer =
                entity->skill[12];

			mechanismState.bellScrapCreated =
    			entity->skill[14];

            mechanismState.bellBurning =
                entity->flags[BURNING];

            mechanismState.bellBurnable =
                entity->flags[BURNABLE];

            mechanismState.bellInvisible =
                entity->flags[INVISIBLE];

            mapState.mechanismStates[
                entity->persistentID
            ] = mechanismState;

            ++capturedBells;
        }
		        else if ( entity->behavior == &actWallLock )
        {
            mechanismState.kind =
                PersistentMechanismState::Kind::WallLock;

            mechanismState.wallLockSavedState =
                entity->wallLockState;

            mechanismState.wallLockSavedPower =
                entity->wallLockPower;

            mechanismState.wallLockSavedPickHealth =
                entity->wallLockPickHealth;

            mechanismState.wallLockSavedPreventExploit =
                entity->wallLockPreventLockpickExploit;

            mapState.mechanismStates[
                entity->persistentID
            ] = mechanismState;

            ++capturedWallLocks;
        }
		        else if ( entity->behavior == &actCampfire )
        {
            mechanismState.kind =
                PersistentMechanismState::Kind::Campfire;

            /*
             * actCampfire:
             * skill[3] = remaining uses / fire health.
             */
            mechanismState.campfireHealth =
                entity->skill[3];

            mapState.mechanismStates[
                entity->persistentID
            ] = mechanismState;

            ++capturedCampfires;
        }
		else if (
            entity->behavior == &::actSignalTimer
            || entity->behavior == &::actSignalGateAND
        )
        {
            mechanismState.kind =
                PersistentMechanismState::Kind::SignalController;

            mechanismState.signalControllerIsAND =
                entity->behavior == &::actSignalGateAND;

            /*
             * Runtime fields:
             *
             * skill[0]  = switch_power/current pulse output
             * skill[6]  = activation delay remaining
             * skill[7]  = pulse interval remaining
             * skill[8]  = repeat count remaining
             * skill[9]  = AND-gate powered-input mask
             * skill[11] = initialization state
             * skill[28] = input circuit status
             *
             * signalTimerLatchInput is skill[4] and changes from
             * authored value 1 to runtime latched value 2.
             */
            mechanismState.signalSwitchPower =
                entity->skill[0];

            mechanismState.signalDelayCount =
                entity->skill[6];

            mechanismState.signalTimerCount =
                entity->skill[7];

            mechanismState.signalRepeatCount =
                entity->skill[8];

            mechanismState.signalLatchInput =
                entity->signalTimerLatchInput;

            mechanismState.signalANDPowerMask =
                entity->signalGateANDPowerCount;

            mechanismState.signalCircuitStatus =
                entity->skill[28];

            mechanismState.signalInitialized =
                entity->skill[11];

            mapState.mechanismStates[
                entity->persistentID
            ] = mechanismState;

            if ( mechanismState.signalControllerIsAND )
            {
                ++capturedANDGates;
            }
            else
            {
                ++capturedSignalTimers;
            }
        }
		else if (getPersistentBoulderTrapBehavior(entity) != 0)
        {
            mechanismState.kind =
                PersistentMechanismState::Kind::BoulderTrap;

            mechanismState.boulderTrapBehavior =
                getPersistentBoulderTrapBehavior(entity);

            /*
             * Public Entity aliases:
             *
             * skill[0]  = boulderTrapFired
             * skill[1]  = boulderTrapRefireAmount
             * skill[4]  = boulderTrapRefireCounter
             * skill[5]  = boulderTrapPreDelay
             * skill[28] = circuit_status
             * skill[30] = actTrapSabotaged
             */
            mechanismState.boulderTrapFired =
                entity->skill[0];

            mechanismState.boulderTrapRefireAmount =
                entity->skill[1];

            mechanismState.boulderTrapRefireCounter =
                entity->skill[4];

            mechanismState.boulderTrapPreDelay =
                entity->skill[5];

            mechanismState.boulderTrapCircuitStatus =
                entity->skill[28];

            mechanismState.boulderTrapSabotaged =
                entity->skill[30];

            mapState.mechanismStates[
                entity->persistentID
            ] = mechanismState;

            ++capturedBoulderTraps;
        }
		        else if (
            entity->behavior == &actTrap
            || entity->behavior == &actTrapPermanent
        )
        {
            mechanismState.kind =
                PersistentMechanismState::Kind::PressurePlate;

            mechanismState.pressurePlatePermanent =
                entity->behavior == &actTrapPermanent;

            mechanismState.pressurePlatePower =
                entity->skill[0];

            /*
             * skill[1] has scripted meaning for permanent plates.
             * It is unused as mutable runtime state by ordinary plates.
             */
            mechanismState.pressurePlateInteractionLock =
                mechanismState.pressurePlatePermanent
                    ? entity->skill[1]
                    : 0;

            mapState.mechanismStates[
                entity->persistentID
            ] = mechanismState;

            if ( mechanismState.pressurePlatePermanent )
            {
                ++capturedPermanentPressurePlates;
            }
            else
            {
                ++capturedNormalPressurePlates;
            }
        }
		else if ( entity->behavior == &actPowerCrystal )
		{
			mechanismState.kind =
				PersistentMechanismState::Kind::PowerCrystal;

			/*
			* Power-crystal private aliases map to these public skill slots:
			*
			* skill[1]  = crystalInitialised
			* skill[3]  = crystalTurning
			* skill[4]  = crystalTurnStartDir
			* skill[28] = circuit_status
			*/
			mechanismState.crystalInitialised =
				entity->skill[1];

			Sint32 crystalDirection = 0;

			if ( entity->skill[3] )
			{
				crystalDirection =
					entity->skill[4]
					+ (entity->crystalTurnReverse ? -1 : 1);
			}
			else
			{
				crystalDirection =
					static_cast<Sint32>(
						std::round(
							entity->yaw / (PI / 2.0)
						)
					);
			}

			crystalDirection %= 4;

			if ( crystalDirection < 0 )
			{
				crystalDirection += 4;
			}

			mechanismState.crystalDirection =
				crystalDirection;

			mechanismState.crystalNumElectricityNodes =
				entity->crystalNumElectricityNodes;

			mechanismState.crystalTurnReverse =
				entity->crystalTurnReverse;

			mechanismState.crystalSpellToActivate =
				entity->crystalSpellToActivate;

			mechanismState.crystalCircuitStatus =
				entity->skill[28];
			

			mechanismState.crystalPowerToActivate =
				entity->crystalPowerToActivate;

			mapState.mechanismStates[
				entity->persistentID
			] = mechanismState;

			++capturedPowerCrystals;
		}
    }
    /*
     * Capture surviving runtime boulders that were created by a
     * persistent boulder trap.
     *
     * Authored map boulders with no trap parent are deliberately not
     * included in this dynamic collection.
     */
    for ( node_t* node = map.entities->first;
        node != nullptr;
        node = node->next )
    {
        Entity* boulder =
            static_cast<Entity*>(node->element);

        if ( !boulder
            || boulder->behavior != &actBoulder
            || boulder->parent == 0 )
        {
            continue;
        }

        Entity* sourceTrap =
            uidToEntity(boulder->parent);

        if ( !sourceTrap
            || sourceTrap->persistentID <= 0
            || getPersistentBoulderTrapBehavior(sourceTrap) == 0 )
        {
            continue;
        }

        PersistentBoulderState boulderState;

        boulderState.sourceTrapPersistentID =
            sourceTrap->persistentID;

        boulderState.sprite =
            boulder->sprite;

        boulderState.x =
            boulder->x;

        boulderState.y =
            boulder->y;

        boulderState.z =
            boulder->z;

        boulderState.yaw =
            boulder->yaw;

        boulderState.pitch =
            boulder->pitch;

        boulderState.roll =
            boulder->roll;

        boulderState.velX =
            boulder->vel_x;

        boulderState.velY =
            boulder->vel_y;

        boulderState.velZ =
            boulder->vel_z;

        /*
         * actBoulder runtime slots:
         *
         * skill[0]  = stopped
         * skill[3]  = no ground
         * skill[4]  = rolling
         * skill[5]  = roll direction
         * skill[6]  = destination X
         * skill[7]  = destination Y
         * skill[11] = initialized
         * skill[12] = lava explosion timer
         */
        boulderState.stopped =
            boulder->skill[0];

        boulderState.noGround =
            boulder->skill[3];

        boulderState.rolling =
            boulder->skill[4];

        boulderState.rollDirection =
            boulder->skill[5];

        boulderState.destinationX =
            boulder->skill[6];

        boulderState.destinationY =
            boulder->skill[7];

        boulderState.initialized =
            boulder->skill[11];

        boulderState.lavaExplodeTimer =
            boulder->skill[12];

        boulderState.passable =
            boulder->flags[PASSABLE];

        mapState.dynamicBoulders.push_back(
            boulderState
        );

        ++capturedDynamicBoulders;
    }
    /*
 * Capture floor items created after the original .lmp loaded.
 *
 * Original authored items were captured above through their stable
 * persistent IDs. This collection contains only entities without one.
 */
for ( node_t* itemNode = map.entities->first;
    itemNode != nullptr;
    itemNode = itemNode->next )
{
    Entity* itemEntity =
        static_cast<Entity*>(
            itemNode->element
        );

    if ( !itemEntity
        || itemEntity->persistentID > 0
        || itemEntity->behavior != &actItem )
    {
        continue;
    }

    PersistentWorldItemState itemState;

    if ( !capturePersistentWorldItemState(
        itemEntity,
        itemState
    ) )
    {
        continue;
    }

    mapState.dynamicWorldItems.push_back(
        itemState
    );

    ++capturedDynamicWorldItems;
}
/*
 * Capture free-standing gold piles created during gameplay.
 */
for ( node_t* goldNode = map.entities->first;
    goldNode != nullptr;
    goldNode = goldNode->next )
{
    Entity* goldEntity =
        static_cast<Entity*>(
            goldNode->element
        );

    if ( !goldEntity
        || goldEntity->persistentID > 0
        || goldEntity->behavior != &actGoldBag )
    {
        continue;
    }

    PersistentGoldBagState goldState;

    if ( !capturePersistentGoldBagState(
        goldEntity,
        goldState
    ) )
    {
        continue;
    }

    mapState.dynamicGoldBags.push_back(
        goldState
    );

    ++capturedDynamicGoldBags;
}
printlog(
    "[Persistent World] Captured mechanism state for '%s': %u lever(s), %u timed lever(s), %u gate(s), %u door(s), %u furniture object(s), %u collider decoration(s), %u power crystal(s), %u boulder trap(s), %u signal timer(s), %u AND gate(s), %u bell(s), %u sink(s), %u fountain(s), %u campfire(s), %u wall lock(s), %u wall button(s), %u ordinary pressure plate(s), %u latched pressure plate(s), %u surviving trap boulder(s), %u pedestal(s), %u chest(s), %u chest item stack(s), %u shopkeeper(s), %u shop item stack(s), %u original floor item(s), %u dynamic floor item(s), %u original gold pile(s), %u dynamic gold pile(s).",
    mapKey.c_str(),
    capturedLevers,
    capturedTimedLevers,
    capturedGates,
    capturedDoors,
    capturedFurniture,
    capturedColliders,
    capturedPowerCrystals,
    capturedBoulderTraps,
    capturedSignalTimers,
    capturedANDGates,
    capturedBells,
    capturedSinks,
    capturedFountains,
    capturedCampfires,
    capturedWallLocks,
    capturedWallButtons,
    capturedNormalPressurePlates,
    capturedPermanentPressurePlates,
	capturedPedestals,
	capturedShopkeepers,
	capturedShopItems,
	capturedChests,
	capturedChestItems,
    capturedDynamicBoulders,
    capturedOriginalWorldItems,
    capturedDynamicWorldItems,
    capturedOriginalGoldBags,
    capturedDynamicGoldBags
);
}
/*
 * Restore runtime mechanism state after assignActions() has created
 * the lever handles and moving gate entities.
 */
/*
 * Restore runtime mechanism state after assignActions() has created
 * the lever handles and moving gate entities.
 */
static void finalizeGeneratedMapEntityPersistence(
    PersistentMapRemovalState& mapState,
    const std::string& mapKey
)
{
    Uint32 assignedRuntimeFallbackIDs = 0;
    Uint32 registeredGeneratedIDs = 0;
    Uint32 removedGeneratedEntities = 0;

    /*
     * Most raw map markers keep or transfer the 0x40000000-range identity
     * assigned before assignActions(). A few generators replace a marker
     * without forwarding that field. Catch the resulting real, stateful root
     * entity here using a separate deterministic range. Transient particles,
     * limbs, projectiles and trap-created boulders are deliberately excluded.
     */
    std::unordered_set<Sint32> usedPersistentIDs;
    for ( node_t* node = map.entities->first;
        node != nullptr;
        node = node->next )
    {
        Entity* entity =
            static_cast<Entity*>(node->element);
        if ( entity && entity->persistentID > 0 )
        {
            usedPersistentIDs.insert(entity->persistentID);
        }
    }

    Sint32 nextRuntimePersistentID =
        GENERATED_RUNTIME_ENTITY_PERSISTENT_ID_BASE;

    auto isSupportedPersistentRuntimeRoot =
        [](const Entity* entity) -> bool
        {
            if ( !entity
                || entity->persistentID != 0
                || entity->parent != 0 )
            {
                return false;
            }

            return entity->behavior == &actMonster
                || entity->behavior == &actSwitch
                || entity->behavior == &actSwitchWithTimer
                || entity->behavior == &actGate
                || entity->behavior == &actDoor
                || entity->behavior == &actIronDoor
                || entity->behavior == &actFurniture
                || entity->behavior == &actColliderDecoration
                || entity->behavior == &actPowerCrystal
                || getPersistentBoulderTrapBehavior(entity) != 0
                || entity->behavior == &::actSignalTimer
                || entity->behavior == &::actSignalGateAND
                || entity->behavior == &actBell
                || entity->behavior == &actSink
                || entity->behavior == &actFountain
                || entity->behavior == &actCampfire
                || entity->behavior == &actWallLock
                || entity->behavior == &actWallButton
                || entity->behavior == &actTrap
                || entity->behavior == &actTrapPermanent
                || entity->behavior == &actPedestalBase
                || entity->behavior == &actChest
                || entity->behavior == &actSummonTrap
                || entity->behavior == &actItem
                || entity->behavior == &actGoldBag;
        };

    for ( node_t* node = map.entities->first;
        node != nullptr;
        node = node->next )
    {
        Entity* entity =
            static_cast<Entity*>(node->element);

        if ( !isSupportedPersistentRuntimeRoot(entity) )
        {
            continue;
        }

        while ( nextRuntimePersistentID > 0
            && usedPersistentIDs.find(nextRuntimePersistentID)
                != usedPersistentIDs.end() )
        {
            ++nextRuntimePersistentID;
        }

        if ( nextRuntimePersistentID <= 0 )
        {
            printlog(
                "[Persistent World] Warning: exhausted generated runtime-entity IDs in '%s'.",
                mapKey.c_str()
            );
            break;
        }

        entity->persistentID = nextRuntimePersistentID;
        usedPersistentIDs.insert(nextRuntimePersistentID);
        ++nextRuntimePersistentID;
        ++assignedRuntimeFallbackIDs;
    }

    for ( node_t* node = map.entities->first;
        node != nullptr; )
    {
        node_t* nextNode = node->next;
        Entity* entity =
            static_cast<Entity*>(node->element);

        if ( entity
            && isGeneratedMapEntityPersistentID(entity->persistentID) )
        {
            if ( mapState.removedEntityIDs.find(entity->persistentID)
                != mapState.removedEntityIDs.end() )
            {
                printlog(
                    "[Persistent World] Removing previously deleted generated entity ID %d, sprite %d, from '%s' after action assignment.",
                    entity->persistentID,
                    entity->sprite,
                    mapKey.c_str()
                );
                list_RemoveNode(entity->mynode);
                ++removedGeneratedEntities;
            }
            else if ( mapState.originalEntityIDs.insert(entity->persistentID).second )
            {
                ++registeredGeneratedIDs;
            }
        }

        node = nextNode;
    }

    if ( assignedRuntimeFallbackIDs > 0
        || registeredGeneratedIDs > 0
        || removedGeneratedEntities > 0 )
    {
        printlog(
            "[Persistent World] Finalized generated entities for '%s': %u runtime fallback ID(s) assigned, %u persistent ID(s) registered, %u removal(s) applied.",
            mapKey.c_str(),
            assignedRuntimeFallbackIDs,
            registeredGeneratedIDs,
            removedGeneratedEntities
        );
    }
}

void applyPersistentMechanismStates()
{
    const std::string mapKey =
        getPersistentMapKey();

    if ( mapKey.empty() )
    {
        return;
    }

    /*
     * Clients may restore only a complete authoritative snapshot
     * belonging to the map they just loaded.
     */
    if ( multiplayer == CLIENT )
    {
        if ( !clientPersistentSnapshotComplete )
        {
            printlog(
                "[Persistent World MP] Client cannot restore mechanisms for '%s': snapshot is incomplete.",
                mapKey.c_str()
            );
            return;
        }

        if ( clientPersistentSnapshotMapKey != mapKey )
        {
            printlog(
                "[Persistent World MP] Client cannot restore mechanisms: loaded '%s', snapshot belongs to '%s'.",
                mapKey.c_str(),
                clientPersistentSnapshotMapKey.c_str()
            );
            return;
        }
    }

    const auto mapIterator =
        persistentMapRemovalRegistry.find(mapKey);

    if ( mapIterator
        == persistentMapRemovalRegistry.end() )
    {
        return;
    }

PersistentMapRemovalState& mapState =
    mapIterator->second;

    finalizeGeneratedMapEntityPersistence(
        mapState,
        mapKey
    );

    Uint32 restoredLevers = 0;
    Uint32 restoredTimedLevers = 0;
    Uint32 restoredGates = 0;
	Uint32 restoredDoors = 0;
	Uint32 restoredFurniture = 0;
	Uint32 restoredColliders = 0;
	Uint32 restoredPowerCrystals = 0;
	Uint32 restoredBoulderTraps = 0;
	Uint32 restoredSignalTimers = 0;
	Uint32 restoredANDGates = 0;
	Uint32 restoredDynamicBoulders = 0;
	Uint32 restoredBells = 0;
	Uint32 restoredSinks = 0;
	Uint32 restoredFountains = 0;
	Uint32 restoredCampfires = 0;
	Uint32 restoredWallLocks = 0;
	Uint32 restoredWallButtons = 0;
	Uint32 restoredNormalPressurePlates = 0;
	Uint32 restoredPermanentPressurePlates = 0;
	Uint32 restoredPedestals = 0;
	Uint32 restoredChests = 0;
	Uint32 restoredChestItems = 0;
    Uint32 restoredOriginalWorldItems = 0;
    Uint32 restoredDynamicWorldItems = 0;
    Uint32 restoredOriginalGoldBags = 0;
    Uint32 restoredDynamicGoldBags = 0;
    Uint32 restoredSummonTraps = 0;

    for ( node_t* node = map.entities->first;
        node != nullptr;
        node = node->next )
    {
        Entity* entity =
            static_cast<Entity*>(node->element);

        if ( !entity || entity->persistentID <= 0 )
        {
            continue;
        }

        const auto stateIterator =
            mapState.mechanismStates.find(
                entity->persistentID
            );

        if ( stateIterator
            == mapState.mechanismStates.end() )
        {
            continue;
        }

		const PersistentMechanismState& savedState =
            stateIterator->second;

        switch ( savedState.kind )
        {
            case PersistentMechanismState::Kind::Lever:
            {
                if ( entity->behavior != &actSwitch )
                {
                    printlog(
                        "[Persistent World] Warning: ID %d was saved as a lever but loaded with another behavior.",
                        entity->persistentID
                    );
                    break;
                }

                entity->skill[0] =
                    savedState.switchPower;

                entity->roll =
                    savedState.roll;

                entity->bNeedsRenderPositionInit = true;

                ++restoredLevers;
                break;
            }
			            case PersistentMechanismState::Kind::WallButton:
            {
                if ( entity->behavior != &actWallButton )
                {
                    printlog(
                        "[Persistent World] Warning: ID %d was saved as a wall button but loaded with another behavior.",
                        entity->persistentID
                    );

                    break;
                }

                entity->wallLockState =
                    savedState.wallButtonState;

                entity->wallLockPower =
                    savedState.wallButtonPower;

                /*
                 * Do not preserve temporary player or arrow interaction.
                 */
                entity->wallLockClientInteractDelay = 0;
                entity->wallLockPlayerInteracting = 0;

                /*
                 * Allow the normal initialization pass to position the
                 * generated button/key child and reapply inverted output.
                 *
                 * Initialization does not overwrite state or power.
                 */
                entity->wallLockInit = 0;

                ++restoredWallButtons;
                break;
            }
			            case PersistentMechanismState::Kind::Chest:
            {
                if ( entity->behavior != &actChest )
                {
                    printlog(
                        "[Persistent World] Warning: ID %d was saved as a chest but loaded with another behavior.",
                        entity->persistentID
                    );

                    break;
                }

                /*
                 * Prevent actChest() from rerolling health, maximum
                 * health, lock chance and lockpick state.
                 */
                entity->chestInit = 1;

                entity->chestHealth =
                    savedState.chestSavedHealth;

                entity->chestMaxHealth =
                    savedState.chestSavedMaxHealth;

                entity->chestLocked =
                    savedState.chestSavedLocked;

                entity->chestLockpickHealth =
                    savedState.chestSavedLockpickHealth;

                entity->chestPreventLockpickCapstoneExploit =
                    savedState.chestSavedPreventExploit;

                entity->chestOldHealth =
                    savedState.chestSavedOldHealth;

                entity->chestVoidState =
                    savedState.chestSavedVoidState;

                /*
                 * Never restore a stale player interaction.
                 */
                entity->chestStatus = 0;
                entity->chestOpener = 0;
                entity->chestLidClicked = 0;

                /*
                 * Inventory is authoritative on the server. Clients
                 * receive it through the existing CHST/chest-item
                 * network flow when someone opens the chest.
                 */
                if ( multiplayer != CLIENT
                    && entity->chestVoidState == 0 )
                {
                    list_t* inventory =
                        entity->getChestInventoryList();

                    if ( inventory )
                    {
                        /*
                         * Delete the newly generated default inventory
                         * before recreating the persistent inventory.
                         *
                         * This is the critical anti-duplication step.
                         */
                        list_FreeAll(inventory);

                        for ( const PersistentChestItemState&
                            itemState :
                            savedState.chestSavedInventory )
                        {
                            const Sint32 resolvedType = resolvePersistentItemType(
                                itemState.stableId,
                                itemState.type
                            );
                            if ( resolvedType < 0
                                || itemState.count <= 0 )
                            {
                                printlog(
                                    "[Persistent World] Ignored invalid item in chest ID %d: type=%d count=%d.",
                                    entity->persistentID,
                                    resolvedType,
                                    itemState.count
                                );

                                continue;
                            }

                            Item* restoredItem =
                                newItem(
                                    static_cast<ItemType>(
                                        resolvedType
                                    ),
                                    static_cast<Status>(
                                        itemState.status
                                    ),
                                    itemState.beatitude,
                                    itemState.count,
                                    itemState.appearance,
                                    itemState.identified,
                                    inventory
                                );

                            if ( !restoredItem )
                            {
                                continue;
                            }

                            restoredItem->x =
                                itemState.x;

                            restoredItem->y =
                                itemState.y;

                            ++restoredChestItems;
                        }
                    }
                }

                ++restoredChests;
                break;
            }
            case PersistentMechanismState::Kind::SummonTrap:
            {
                if ( entity->behavior != &actSummonTrap )
                {
                    printlog(
                        "[Persistent World] Warning: ID %d was saved as a summoning trap but loaded with another behavior.",
                        entity->persistentID
                    );

                    break;
                }

                entity->skill[0] =
                    savedState.summonTrapMonster;

                entity->skill[1] =
                    savedState.summonTrapCount;

                entity->skill[2] =
                    savedState.summonTrapInterval;

                entity->skill[3] =
                    savedState.summonTrapSpawnCycles;

                entity->skill[4] =
                    savedState.summonTrapPowerToDisable;

                entity->skill[5] =
                    savedState.summonTrapFailureRate;

                entity->skill[6] =
                    savedState.summonTrapFired;

                entity->skill[7] =
                    savedState.summonTrapInitialized;

                entity->skill[8] =
                    savedState.summonTrapTicksToFire;

                entity->skill[9] =
                    savedState.summonTrapPlayerProximity;

                ++restoredSummonTraps;
                break;
            }
			            case PersistentMechanismState::Kind::PressurePlate:
            {
                const bool loadedAsNormal =
                    entity->behavior == &actTrap;

                const bool loadedAsPermanent =
                    entity->behavior == &actTrapPermanent;

                if ( !loadedAsNormal
                    && !loadedAsPermanent )
                {
                    printlog(
                        "[Persistent World] Warning: ID %d was saved as a pressure plate but loaded with another behavior.",
                        entity->persistentID
                    );

                    break;
                }

                if ( savedState.pressurePlatePermanent
                    != loadedAsPermanent )
                {
                    printlog(
                        "[Persistent World] Warning: pressure-plate ID %d changed between ordinary and permanent behavior.",
                        entity->persistentID
                    );

                    break;
                }

                entity->skill[0] =
                    savedState.pressurePlatePower;

                if ( loadedAsPermanent )
                {
                    entity->skill[1] =
                        savedState.pressurePlateInteractionLock;

                    ++restoredPermanentPressurePlates;
                }
                else
                {
                    ++restoredNormalPressurePlates;
                }

                /*
                 * A powered plate must immediately reconstruct its
                 * output in the freshly loaded circuit network.
                 *
                 * An ordinary plate will reevaluate occupants during
                 * its next behavior tick and release naturally when
                 * no qualifying entity remains.
                 */
                if ( multiplayer != CLIENT
                    && entity->skill[0] )
                {
                    entity->switchUpdateNeighbors();
                }

                break;
            }
			            case PersistentMechanismState::Kind::Pedestal:
            {
                if ( entity->behavior != &actPedestalBase )
                {
                    printlog(
                        "[Persistent World] Warning: ID %d was saved as a pedestal but loaded with another behavior.",
                        entity->persistentID
                    );

                    break;
                }

                entity->pedestalHasOrb =
                    savedState.pedestalSavedHasOrb;

                entity->pedestalPowerStatus =
                    savedState.pedestalSavedPowerStatus;

                entity->pedestalInit =
                    savedState.pedestalSavedInit;

                entity->pedestalInGround =
                    savedState.pedestalSavedInGround;

                entity->z =
                    savedState.pedestalSavedZ;

                entity->vel_z =
                    savedState.pedestalSavedVelZ;

                entity->flags[PASSABLE] =
                    savedState.pedestalSavedPassable;

                /*
                 * The generated floating-orb child is recreated during
                 * assignActions(). Synchronize its position to the
                 * restored pedestal.
                 *
                 * Pedestal base and orb always use a 6.5-unit vertical
                 * separation:
                 *
                 * emerged:    base 4.5, orb -2.0
                 * underground base 15.5, orb 9.0
                 */
                Entity* orbEntity = nullptr;

                for ( node_t* childNode =
                        entity->children.first;
                    childNode != nullptr;
                    childNode = childNode->next )
                {
                    Entity* child =
                        static_cast<Entity*>(
                            childNode->element
                        );

                    if ( child
                        && child->behavior
                            == &actPedestalOrb )
                    {
                        orbEntity = child;
                        break;
                    }
                }

				if ( orbEntity )
				{
					orbEntity->x =
						entity->x;

					orbEntity->y =
						entity->y;

					orbEntity->z =
						entity->z - 6.5;

					orbEntity->vel_z =
						entity->vel_z;

					/*
					* These orb aliases are private Entity members, so game.cpp must
					* access their underlying public skill/fskill storage directly.
					*
					* orbInitialised      = skill[1]
					* orbHoverDirection   = skill[7]
					* orbHoverWaitTimer   = skill[8]
					* orbStartZ           = fskill[0]
					*/
					orbEntity->skill[1] = 0;
					orbEntity->skill[7] = 0;
					orbEntity->skill[8] = 0;
					orbEntity->fskill[0] =
						entity->z - 6.5;
				}

                /*
                 * Reapply the saved circuit output to the newly loaded
                 * circuit graph.
                 */
				if ( multiplayer != CLIENT
					&& entity->pedestalPowerStatus == 1 )
				{
					entity->switchUpdateNeighbors();
				}

                ++restoredPedestals;
                break;
            }
            case PersistentMechanismState::Kind::TimedLever:
            {
                if ( entity->behavior
                    != &actSwitchWithTimer )
                {
                    printlog(
                        "[Persistent World] Warning: ID %d was saved as a timed lever but loaded with another behavior.",
                        entity->persistentID
                    );
                    break;
                }

                entity->skill[0] =
                    savedState.switchPower;

                entity->leverStatus =
                    savedState.leverStatus;

                entity->leverTimerTicks =
                    std::max(
                        savedState.leverTimerTicks,
                        1
                    );

                entity->roll =
                    savedState.roll;

                entity->bNeedsRenderPositionInit = true;

                ++restoredTimedLevers;
                break;
            }

            case PersistentMechanismState::Kind::Gate:
            {
                if ( entity->behavior != &actGate )
                {
                    printlog(
                        "[Persistent World] Warning: ID %d was saved as a gate but loaded with another behavior.",
                        entity->persistentID
                    );
                    break;
                }

                entity->gateStatus =
                    savedState.gateStatus;

                entity->gateRattle =
                    savedState.gateRattle;

                entity->gateInverted =
                    savedState.gateInverted;

                entity->skill[28] =
                    savedState.circuitStatus;

                entity->gateStartHeight =
                    savedState.gateStartHeight;

                entity->z =
                    savedState.gateZ;

                entity->gateVelZ =
                    savedState.gateVelZ;

                /*
                 * Prevent actGate() from treating the restored gate
                 * position as its original starting height.
                 */
                entity->gateInit = 1;

                entity->scalex = 1.01;
                entity->scaley = 1.01;
                entity->scalez = 1.01;

                entity->flags[PASSABLE] =
                    savedState.passable;

                entity->new_z =
                    entity->z;

                entity->bNeedsRenderPositionInit = true;

                ++restoredGates;
                break;
            }
			            case PersistentMechanismState::Kind::WallLock:
            {
                if ( entity->behavior != &actWallLock )
                {
                    printlog(
                        "[Persistent World] Warning: ID %d was saved as a wall lock but loaded with another behavior.",
                        entity->persistentID
                    );

                    break;
                }

                entity->wallLockState =
                    savedState.wallLockSavedState;

                entity->wallLockPower =
                    savedState.wallLockSavedPower;

                entity->wallLockPickHealth =
                    savedState.wallLockSavedPickHealth;

                entity->wallLockPreventLockpickExploit =
                    savedState.wallLockSavedPreventExploit;

                /*
                 * Prevent actWallLock() from resetting pick health to 50
                 * and clearing the saved anti-exploit state.
                 */
                entity->wallLockInit = 1;

                /*
                 * Runtime player UIDs are invalid after changing maps.
                 * Never restore an interaction that was in progress.
                 */
                entity->wallLockClientInteractDelay = 0;
                entity->wallLockPlayerInteracting = 0;

                ++restoredWallLocks;
                break;
            }
			case PersistentMechanismState::Kind::Door:
			{
				const bool loadedAsWoodenDoor =
					entity->behavior == &actDoor;

				const bool loadedAsIronDoor =
					entity->behavior == &actIronDoor;

				if ( !loadedAsWoodenDoor
					&& !loadedAsIronDoor )
				{
					printlog(
						"[Persistent World] Warning: ID %d was saved as a door but loaded with another behavior.",
						entity->persistentID
					);
					break;
				}

				if ( savedState.doorIsIron
					!= loadedAsIronDoor )
				{
					printlog(
						"[Persistent World] Warning: ID %d changed between wooden and iron door types.",
						entity->persistentID
					);
					break;
				}

				/*
				* The fresh runtime door has not received its first behavior tick.
				* Create its interaction tooltip here because setting doorInit to
				* one prevents the normal initialization block from doing so.
				*/
				entity->createWorldUITooltip();

				entity->doorDir =
					savedState.doorDir;

				entity->doorStatus =
					savedState.doorStatus;

				entity->doorOldStatus =
					savedState.doorStatus;

				entity->doorHealth =
					savedState.doorHealth;

				entity->doorMaxHealth =
					savedState.doorMaxHealth;

				entity->doorOldHealth =
					savedState.doorHealth;

				entity->doorLocked =
					savedState.doorLocked;

				entity->doorSmacked =
					savedState.doorSmacked;

				entity->doorTimer =
					savedState.doorTimer;

				entity->doorPreventLockpickExploit =
					savedState.doorPreventLockpickExploit;

				entity->doorForceLockedUnlocked =
					savedState.doorForceLockedUnlocked;

				entity->doorDisableLockpicks =
					savedState.doorDisableLockpicks;

				entity->doorDisableOpening =
					savedState.doorDisableOpening;

				entity->doorLockpickHealth =
					savedState.doorLockpickHealth;

				entity->doorUnlockWhenPowered =
					savedState.doorUnlockWhenPowered;

				entity->skill[28] =
					savedState.doorCircuitStatus;

				entity->doorStartAng =
					savedState.doorStartAng;

				entity->yaw =
					savedState.doorYaw;

				entity->x =
					savedState.doorX;

				entity->y =
					savedState.doorY;

				entity->focaly =
					savedState.doorFocalY;

				entity->flags[PASSABLE] =
					savedState.passable;

				entity->flags[BURNING] =
					savedState.doorBurning;

				entity->flags[BURNABLE] =
					savedState.doorBurnable;

				entity->scalex = 1.01;
				entity->scaley = 1.01;
				entity->scalez = 1.01;

				/*
				* Prevent actDoor()/actIronDoor() from regenerating health,
				* lock state, starting angle, and lockpick state.
				*/
				entity->doorInit = 1;

				entity->new_x =
					entity->x;

				entity->new_y =
					entity->y;

				entity->new_yaw =
					entity->yaw;

				entity->bNeedsRenderPositionInit = true;

				++restoredDoors;
				break;
			}
			            case PersistentMechanismState::Kind::Campfire:
            {
                if ( entity->behavior != &actCampfire )
                {
                    printlog(
                        "[Persistent World] Warning: ID %d was saved as a campfire but loaded with another behavior.",
                        entity->persistentID
                    );

                    break;
                }

                entity->skill[3] =
                    savedState.campfireHealth;

                /*
                 * actCampfire() normally initializes skill[3] to
                 * MAXPLAYERS whenever skill[4] is zero.
                 *
                 * Mark it initialized so the restored health is not
                 * overwritten on the first tick.
                 */
                entity->skill[4] = 1;

                /*
                 * Light, flicker and sound state are cosmetic. Reset
                 * them so actCampfire() recreates the correct effects
                 * from the restored health value.
                 */
                entity->skill[0] = 0;
                entity->skill[1] = 0;
                entity->skill[5] = 0;

                entity->removeLightField();
                entity->light = nullptr;

                /*
                 * Setting skill[4] to 1 skips the normal initialization
                 * block, so recreate the tooltip here.
                 */
                entity->createWorldUITooltip();

                ++restoredCampfires;
                break;
            }
			case PersistentMechanismState::Kind::Furniture:
			{
				if ( entity->behavior != &actFurniture )
				{
					printlog(
						"[Persistent World] Warning: ID %d was saved as furniture but loaded with another behavior.",
						entity->persistentID
					);
					break;
				}

				if ( entity->furnitureType
					!= savedState.furnitureType )
				{
					printlog(
						"[Persistent World] Warning: furniture ID %d changed type from %d to %d.",
						entity->persistentID,
						savedState.furnitureType,
						entity->furnitureType
					);

					break;
				}

				entity->furnitureHealth =
					savedState.furnitureHealth;

				entity->furnitureMaxHealth =
					savedState.furnitureMaxHealth;

				entity->furnitureOldHealth =
					savedState.furnitureHealth;

				entity->flags[BURNING] =
					savedState.furnitureBurning;

				entity->flags[BURNABLE] =
					savedState.furnitureBurnable;

				/*
				* Prevent actFurniture() from rerolling the restored health on its
				* first behavior tick.
				*/
				entity->furnitureInit = 1;

				/*
				* Bunk beds normally create their tooltip during initialization.
				* Since initialization is skipped after restoration, recreate it.
				*/
				if ( entity->furnitureType
					== FURNITURE_BUNKBED )
				{
					entity->createWorldUITooltip();
				}

				entity->bNeedsRenderPositionInit = true;

				++restoredFurniture;
				break;
			}
			case PersistentMechanismState::Kind::ColliderDecoration:
			{
				if ( entity->behavior
					!= &actColliderDecoration )
				{
					printlog(
						"[Persistent World] Warning: ID %d was saved as collider decoration but loaded with another behavior.",
						entity->persistentID
					);

					break;
				}

				if ( !entity->isDamageableCollider() )
				{
					printlog(
						"[Persistent World] Warning: ID %d was saved as a damageable collider but loaded as an indestructible collider.",
						entity->persistentID
					);

					break;
				}

				if ( entity->colliderDamageTypes
					!= savedState.colliderDamageTypes )
				{
					printlog(
						"[Persistent World] Warning: collider ID %d changed damage type from %d to %d.",
						entity->persistentID,
						savedState.colliderDamageTypes,
						entity->colliderDamageTypes
					);

					break;
				}

				entity->colliderCurrentHP =
					savedState.colliderCurrentHP;

				entity->colliderMaxHP =
					savedState.colliderMaxHP;

				entity->colliderOldHP =
					savedState.colliderCurrentHP;

				entity->colliderHasCollision =
					savedState.colliderHasCollision;

				entity->flags[BURNING] =
					savedState.colliderBurning;

				entity->flags[BURNABLE] =
					savedState.colliderBurnable;

				/*
				* Do not force colliderInit to 1. The first behavior tick still
				* needs to perform normal collider-specific setup.
				*/
				entity->bNeedsRenderPositionInit = true;

				++restoredColliders;
				break;
			}
			            case PersistentMechanismState::Kind::SinkOrFountain:
            {
                const bool loadedAsSink =
                    entity->behavior == &actSink;

                const bool loadedAsFountain =
                    entity->behavior == &actFountain;

                if ( !loadedAsSink
                    && !loadedAsFountain )
                {
                    printlog(
                        "[Persistent World] Warning: ID %d was saved as a sink/fountain but loaded with another behavior.",
                        entity->persistentID
                    );

                    break;
                }

                if ( savedState.waterSourceIsFountain
                    != loadedAsFountain )
                {
                    printlog(
                        "[Persistent World] Warning: water-source ID %d changed between sink and fountain.",
                        entity->persistentID
                    );

                    break;
                }

                entity->skill[0] =
                    savedState.waterSourceUses;

                if ( loadedAsFountain )
                {
                    entity->skill[1] =
                        savedState.waterSourceMainEffect;

                    entity->skill[3] =
                        savedState.waterSourceSecondaryEffect;

                    ++restoredFountains;
                }
                else
                {
                    entity->skill[3] =
                        savedState.waterSourceSecondaryEffect;

                    ++restoredSinks;
                }

                /*
                 * Do not restore:
                 *
                 * skill[2] = temporary water-particle timer
                 * skill[7] = ambience timer
                 *
                 * Those are cosmetic and can restart safely.
                 */
                entity->skill[2] = 0;
                entity->skill[7] = 0;

                break;
            }
			            case PersistentMechanismState::Kind::Bell:
            {
                if ( entity->behavior != &actBell )
                {
                    printlog(
                        "[Persistent World] Warning: ID %d was saved as a bell but loaded with another behavior.",
                        entity->persistentID
                    );

                    break;
                }

                entity->skill[0] =
                    savedState.bellActiveTimer;

                entity->skill[1] =
                    savedState.bellHasItem;

                entity->skill[4] =
                    savedState.bellUses;

                entity->skill[5] =
                    savedState.bellCurrentEvent;

                entity->skill[7] =
                    savedState.bellUseDelay;

                /*
                 * Leave skill[8] at zero. actBell() must run its normal
                 * initialization and recreate its generated children.
                 */
                entity->skill[8] = 0;

                entity->skill[9] =
                    savedState.bellClapperBroken;

                entity->skill[10] =
                    savedState.bellBulbBroken;

                entity->skill[11] =
                    savedState.bellBuffType;

                entity->skill[12] =
                    savedState.bellBurningTimer;

				entity->skill[14] =
    				savedState.bellScrapCreated;

                /*
                 * Do not restore player/entity runtime references.
                 */
                entity->skill[6] = -1;
                entity->skill[13] = 0;

                entity->flags[BURNING] =
                    savedState.bellBurning;

                entity->flags[BURNABLE] =
                    savedState.bellBurnable;

                entity->flags[INVISIBLE] =
                    savedState.bellInvisible;

                ++restoredBells;
                break;
            }
			case PersistentMechanismState::Kind::SignalController:
            {
                const bool loadedAsTimer =
                    entity->behavior == &::actSignalTimer;

                const bool loadedAsANDGate =
                    entity->behavior == &::actSignalGateAND;

                if ( !loadedAsTimer
                    && !loadedAsANDGate )
                {
                    printlog(
                        "[Persistent World] Warning: ID %d was saved as a signal controller but loaded with another behavior.",
                        entity->persistentID
                    );

                    break;
                }

                if ( savedState.signalControllerIsAND
                    != loadedAsANDGate )
                {
                    printlog(
                        "[Persistent World] Warning: signal controller ID %d changed between timer and AND-gate types.",
                        entity->persistentID
                    );

                    break;
                }

                entity->skill[0] =
                    savedState.signalSwitchPower;

                entity->skill[6] =
                    savedState.signalDelayCount;

                entity->skill[7] =
                    savedState.signalTimerCount;

                entity->skill[8] =
                    savedState.signalRepeatCount;

                entity->signalTimerLatchInput =
                    savedState.signalLatchInput;

                entity->signalGateANDPowerCount =
                    savedState.signalANDPowerMask;

                entity->skill[28] =
                    savedState.signalCircuitStatus;

                /*
                 * Force the controller into initialized mode so its first
                 * tick does not treat this as a fresh map load and emit an
                 * extra inverted-output initialization pulse.
                 */
                entity->skill[11] =
                    savedState.signalInitialized != 0
                        ? savedState.signalInitialized
                        : 1;

                /*
                 * Reapply the saved output to freshly loaded circuit nodes.
                 */
                if ( multiplayer != CLIENT )
                {
                    persistentSignalControllerBroadcastOutput(
                        entity
                    );
                }

                if ( loadedAsANDGate )
                {
                    ++restoredANDGates;
                }
                else
                {
                    ++restoredSignalTimers;
                }

                break;
            }
			case PersistentMechanismState::Kind::BoulderTrap:
            {
                const Sint32 loadedBehavior =
                    getPersistentBoulderTrapBehavior(entity);

                if ( loadedBehavior == 0 )
                {
                    printlog(
                        "[Persistent World] Warning: ID %d was saved as a boulder trap but loaded with another behavior.",
                        entity->persistentID
                    );

                    break;
                }

                if ( loadedBehavior
                    != savedState.boulderTrapBehavior )
                {
                    printlog(
                        "[Persistent World] Warning: boulder trap ID %d changed behavior type from %d to %d.",
                        entity->persistentID,
                        savedState.boulderTrapBehavior,
                        loadedBehavior
                    );

                    break;
                }

                entity->skill[0] =
                    savedState.boulderTrapFired;

                entity->skill[1] =
                    savedState.boulderTrapRefireAmount;

                entity->skill[4] =
                    savedState.boulderTrapRefireCounter;

                entity->skill[5] =
                    savedState.boulderTrapPreDelay;

                entity->skill[28] =
                    savedState.boulderTrapCircuitStatus;

                entity->skill[30] =
                    savedState.boulderTrapSabotaged;

                ++restoredBoulderTraps;
                break;
            }
			case PersistentMechanismState::Kind::PowerCrystal:
			{
				if ( entity->behavior != &actPowerCrystal )
				{
					printlog(
						"[Persistent World] Warning: ID %d was saved as a power crystal but loaded with another behavior.",
						entity->persistentID
					);

					break;
				}

				Sint32 crystalDirection =
					savedState.crystalDirection % 4;

				if ( crystalDirection < 0 )
				{
					crystalDirection += 4;
				}

				/*
				* Restore a completed cardinal direction. The generated
				* electricity-node layout relies on an exact cardinal yaw.
				*/
				entity->yaw =
					crystalDirection * (PI / 2.0);

				entity->new_yaw =
					entity->yaw;

				/*
				* Private Entity aliases use these public skill slots:
				*
				* skill[1]  = crystalInitialised
				* skill[3]  = crystalTurning
				* skill[4]  = crystalTurnStartDir
				* skill[5]  = crystalGeneratedElectricityNodes
				* skill[7]  = crystalHoverDirection
				* skill[8]  = crystalHoverWaitTimer
				* skill[28] = circuit_status
				*/
				entity->skill[1] =
					savedState.crystalInitialised;

				entity->skill[3] = 0;

				entity->skill[4] =
					crystalDirection;

				entity->crystalNumElectricityNodes =
					savedState.crystalNumElectricityNodes;

				entity->crystalTurnReverse =
					savedState.crystalTurnReverse;

				entity->crystalSpellToActivate =
					savedState.crystalSpellToActivate;

				entity->crystalPowerToActivate =
					savedState.crystalPowerToActivate;

				entity->skill[28] =
					savedState.crystalCircuitStatus;

				/*
				* The generated electricity nodes are runtime children. Mark
				* them as not generated so they can be rebuilt for this load.
				*/
				entity->skill[5] = 0;
				entity->skill[7] = 0;
				entity->skill[8] = 0;

				/*
				* Circuit-powered crystals always reload inactive first.
				*
				* Their live lever/wire network will write skill[12] and reactivate
				* them when the current external circuit is actually ON.
				*
				* Normal and spell-activated crystals preserve their saved state.
				*/
				if ( entity->crystalPowerToActivate )
				{
					entity->skill[1] = 0;  // crystalInitialised
					entity->skill[3] = 0;  // crystalTurning
					entity->skill[5] = 0;  // generated nodes
					entity->skill[12] = 1; // external input OFF

					/*
					* Its output also begins OFF until external power reactivates it.
					*/
					entity->skill[28] = 1;

					entity->z =
						entity->crystalStartZ + 5;

					entity->new_z =
						entity->z;

					entity->vel_z =
						entity->crystalMaxZVelocity * 2;
				}
				else if ( entity->skill[1] )
				{
					entity->z =
						entity->crystalStartZ;

					entity->new_z =
						entity->z;

					entity->powerCrystalCreateElectricityNodes();
				}

				entity->bNeedsRenderPositionInit = true;

				++restoredPowerCrystals;
				break;
				}
                case PersistentMechanismState::Kind::GoldBag:
                {
                    if ( entity->behavior != &actGoldBag )
                    {
                        printlog(
                            "[Persistent World] Warning: entity ID %d was saved as gold but no longer uses actGoldBag.",
                            entity->persistentID
                        );

                        break;
                    }

                    if ( applyPersistentGoldBagState(
                        entity,
                        savedState.goldBagState
                    ) )
                    {
                        ++restoredOriginalGoldBags;
                    }

                    break;
                }
                case PersistentMechanismState::Kind::WorldItem:
                {
                    if ( entity->behavior != &actItem )
                    {
                    printlog(
                        "[Persistent World] Warning: entity ID %d was saved as a floor item but no longer uses actItem.",
                        entity->persistentID
                    );

                        break;
                    }

                    if ( applyPersistentWorldItemState(
                        entity,
                        savedState.worldItemState
                    ) )
                    {
                        ++restoredOriginalWorldItems;
                    }

                    break;
                }
                case PersistentMechanismState::Kind::ShopkeeperInventory:
                case PersistentMechanismState::Kind::MonsterLivingState:
                case PersistentMechanismState::Kind::None:
                default:
                    break;
        }
    }
    /*
     * Runtime boulders are host-authoritative.
     *
     * The server/single-player instance recreates them. Multiplayer
     * clients receive the recreated entities through Barony's normal
     * entity synchronization instead of creating a second local copy.
     */
    if ( multiplayer != CLIENT )
    {
        for ( const PersistentBoulderState& savedBoulder :
            mapState.dynamicBoulders )
        {
            Entity* sourceTrap = nullptr;

            for ( node_t* node = map.entities->first;
                node != nullptr;
                node = node->next )
            {
                Entity* candidate =
                    static_cast<Entity*>(node->element);

                if ( candidate
                    && candidate->persistentID
                        == savedBoulder.sourceTrapPersistentID
                    && getPersistentBoulderTrapBehavior(candidate) != 0 )
                {
                    sourceTrap = candidate;
                    break;
                }
            }

            if ( !sourceTrap )
            {
                printlog(
                    "[Persistent World] Warning: could not restore trap boulder because source trap ID %d no longer exists.",
                    savedBoulder.sourceTrapPersistentID
                );

                continue;
            }

            Entity* boulder =
                newEntity(
                    savedBoulder.sprite,
                    1,
                    map.entities,
                    nullptr
                );

            if ( !boulder )
            {
                printlog(
                    "[Persistent World] Warning: failed to allocate persistent trap boulder."
                );

                continue;
            }

            boulder->parent =
                sourceTrap->getUID();

            boulder->x =
                savedBoulder.x;

            boulder->y =
                savedBoulder.y;

            boulder->z =
                savedBoulder.z;

            boulder->yaw =
                savedBoulder.yaw;

            boulder->pitch =
                savedBoulder.pitch;

            boulder->roll =
                savedBoulder.roll;

            boulder->vel_x =
                savedBoulder.velX;

            boulder->vel_y =
                savedBoulder.velY;

            boulder->vel_z =
                savedBoulder.velZ;

            boulder->sizex = 7;
            boulder->sizey = 7;

            boulder->behavior =
                &actBoulder;

            boulder->flags[UPDATENEEDED] =
                true;

            boulder->flags[PASSABLE] =
                savedBoulder.passable;

            boulder->skill[0] =
                savedBoulder.stopped;

            boulder->skill[3] =
                savedBoulder.noGround;

            boulder->skill[4] =
                savedBoulder.rolling;

            boulder->skill[5] =
                savedBoulder.rollDirection;

            boulder->skill[6] =
                savedBoulder.destinationX;

            boulder->skill[7] =
                savedBoulder.destinationY;

            /*
             * Mark initialized so actBoulder() does not overwrite restored
             * state such as the lava explosion timer.
             */
            boulder->skill[11] =
                savedBoulder.initialized
                    ? savedBoulder.initialized
                    : 1;

            boulder->skill[12] =
                savedBoulder.lavaExplodeTimer;

            boulder->new_x =
                boulder->x;

            boulder->new_y =
                boulder->y;

            boulder->new_z =
                boulder->z;

            boulder->new_yaw =
                boulder->yaw;

            boulder->bNeedsRenderPositionInit =
                true;

            ++restoredDynamicBoulders;
        }
        /*
        * Recreate items that did not exist in the original .lmp.
        *
        * Only the host creates these entities. Multiplayer clients receive
        * them through normal entity synchronization.
        */
        for ( const PersistentWorldItemState& savedItem :
            mapState.dynamicWorldItems )
        {
            Entity* itemEntity =
                newEntity(
                    -1,
                    1,
                    map.entities,
                    nullptr
                );

            if ( !itemEntity )
            {
                printlog(
                    "[Persistent World] Warning: failed to allocate persistent dynamic floor item."
                );

                continue;
            }

            if ( !applyPersistentWorldItemState(
                itemEntity,
                savedItem
            ) )
            {
                list_RemoveNode(
                    itemEntity->mynode
                );

                continue;
            }

            ++restoredDynamicWorldItems;
        }
        /*
        * Recreate gold piles that did not exist in the original .lmp.
        *
        * Only the server or single-player instance creates them. Multiplayer
        * clients receive them through normal entity synchronization.
        */
        for ( const PersistentGoldBagState& savedGold :
            mapState.dynamicGoldBags )
        {
            Entity* goldEntity =
                newEntity(
                    savedGold.amount < 5
                        ? 1379
                        : 130,
                    1,
                    map.entities,
                    nullptr
                );

            if ( !goldEntity )
            {
                printlog(
                    "[Persistent World] Warning: failed to allocate persistent dynamic gold pile."
                );

                continue;
            }

            if ( !applyPersistentGoldBagState(
                goldEntity,
                savedGold
            ) )
            {
                list_RemoveNode(
                    goldEntity->mynode
                );

                continue;
            }

            ++restoredDynamicGoldBags;
        }
    }
printlog(
    "[Persistent World] Restored mechanism state for '%s': %u lever(s), %u timed lever(s), %u gate(s), %u door(s), %u furniture object(s), %u collider decoration(s), %u power crystal(s), %u boulder trap(s), %u signal timer(s), %u AND gate(s), %u bell(s), %u sink(s), %u fountain(s), %u campfire(s), %u wall lock(s), %u surviving trap boulder(s), %u wall button(s), %u ordinary pressure plate(s), %u latched pressure plate(s), %u pedestal(s), %u chest(s), %u chest item stack(s), %u original floor item(s), %u dynamic floor item(s), %u original gold pile(s), %u dynamic gold pile(s).",
    mapKey.c_str(),
    restoredLevers,
    restoredTimedLevers,
    restoredGates,
    restoredDoors,
    restoredFurniture,
    restoredColliders,
    restoredPowerCrystals,
    restoredBoulderTraps,
    restoredSignalTimers,
    restoredANDGates,
    restoredBells,
    restoredSinks,
    restoredFountains,
	restoredChests,
	restoredChestItems,
    restoredCampfires,
    restoredWallLocks,
	restoredPedestals,
    restoredDynamicBoulders,
	restoredWallButtons,
	restoredNormalPressurePlates,
	restoredPermanentPressurePlates,
    restoredOriginalWorldItems,
    restoredDynamicWorldItems,
    restoredOriginalGoldBags,
    restoredDynamicGoldBags
);
}
/*
 * Clear an ordinary monster's freshly initialized randomized inventory
 * and equipment without double-freeing items that are inventory nodes.
 */
static void clearPersistentMonsterGeneratedItems(
    Stat* monsterStats
)
{
    if ( !monsterStats )
    {
        return;
    }

    /*
     * Null every equipment pointer before freeing the inventory list.
     * Equipped items may point to elements owned by that list.
     */
    monsterStats->weapon = nullptr;
    monsterStats->shield = nullptr;
    monsterStats->helmet = nullptr;
    monsterStats->breastplate = nullptr;
    monsterStats->gloves = nullptr;
    monsterStats->shoes = nullptr;
    monsterStats->cloak = nullptr;
    monsterStats->amulet = nullptr;
    monsterStats->ring = nullptr;
    monsterStats->mask = nullptr;

    list_FreeAll(
        &monsterStats->inventory
    );
}
/*
 * Replace one ordinary monster's newly randomized loadout with the
 * persistent loadout captured when the map was previously left.
 */
static Uint32 restorePersistentMonsterItems(
    Entity* monsterEntity,
    Stat* monsterStats,
    const PersistentMechanismState& savedState
)
{
    if ( !monsterEntity
        || !monsterStats
        || monsterStats->type == SHOPKEEPER )
    {
        return 0;
    }

    clearPersistentMonsterGeneratedItems(
        monsterStats
    );

    Uint32 restoredItems = 0;

    for ( const PersistentMonsterItemState& itemState :
        savedState.monsterSavedItems )
    {
        const Sint32 resolvedType = resolvePersistentItemType(
            itemState.stableId,
            itemState.type
        );
        if ( resolvedType < 0
            || itemState.status < BROKEN
            || itemState.status > EXCELLENT
            || itemState.count <= 0 )
        {
            printlog(
                "[Persistent World] Ignored invalid monster item for monster ID %d: slot=%d type=%d status=%d count=%d.",
                monsterEntity->persistentID,
                itemState.slot,
                resolvedType,
                itemState.status,
                itemState.count
            );

            continue;
        }

        Item* restoredItem =
            newItem(
                static_cast<ItemType>(
                    resolvedType
                ),
                static_cast<Status>(
                    itemState.status
                ),
                itemState.beatitude,
                itemState.count,
                itemState.appearance,
                itemState.identified,
                &monsterStats->inventory
            );

        if ( !restoredItem )
        {
            continue;
        }

        restoredItem->x =
            itemState.x;

        restoredItem->y =
            itemState.y;

        restoredItem->isDroppable =
            itemState.isDroppable;

        switch ( itemState.slot )
        {
            case 1:
                monsterStats->weapon =
                    restoredItem;
                break;

            case 2:
                monsterStats->shield =
                    restoredItem;
                break;

            case 3:
                monsterStats->helmet =
                    restoredItem;
                break;

            case 4:
                monsterStats->breastplate =
                    restoredItem;
                break;

            case 5:
                monsterStats->gloves =
                    restoredItem;
                break;

            case 6:
                monsterStats->shoes =
                    restoredItem;
                break;

            case 7:
                monsterStats->cloak =
                    restoredItem;
                break;

            case 8:
                monsterStats->amulet =
                    restoredItem;
                break;

            case 9:
                monsterStats->ring =
                    restoredItem;
                break;

            case 10:
                monsterStats->mask =
                    restoredItem;
                break;

            case 0:
            default:
                /*
                 * Ordinary inventory item. It is already linked to
                 * monsterStats->inventory by newItem().
                 */
                break;
        }

        ++restoredItems;
    }

    return restoredItems;
}
/*
 * Replace selected temporary effects generated during fresh monster
 * initialization with the effects captured when the map was left.
 */
static Uint32 restorePersistentMonsterEffects(
    Entity* monsterEntity,
    Stat* monsterStats,
    const PersistentMechanismState& savedState
)
{
    if ( !monsterEntity
        || !monsterStats )
    {
        return 0;
    }

    /*
     * Clear only the allowlisted effects.
     *
     * Other effects may represent intrinsic monster properties,
     * equipment, transformations, spell controllers, or state owned by
     * another system.
     */
    for ( Sint32 effectID = 0;
        effectID < NUMEFFECTS;
        ++effectID )
    {
        if ( !isPersistentMonsterEffect(effectID) )
        {
            continue;
        }

        monsterStats->clearEffect(
            effectID
        );

        monsterStats->EFFECTS_TIMERS[
            effectID
        ] = 0;
    }

    Uint32 restoredEffects = 0;

    for ( const PersistentMonsterEffectState& effectState :
        savedState.monsterSavedEffects )
    {
        if ( effectState.effectID < 0
            || effectState.effectID >= NUMEFFECTS
            || !isPersistentMonsterEffect(
                effectState.effectID
            )
            || effectState.effectValue == 0
            || effectState.timer <= 0 )
        {
            printlog(
                "[Persistent World] Ignored invalid saved effect for monster ID %d: effect=%d value=%u timer=%d.",
                monsterEntity->persistentID,
                effectState.effectID,
                static_cast<Uint32>(
                    effectState.effectValue
                ),
                effectState.timer
            );

            continue;
        }

        monsterStats->setEffectActive(
            effectState.effectID,
            effectState.effectValue
        );

        monsterStats->EFFECTS_TIMERS[
            effectState.effectID
        ] = effectState.timer;

        ++restoredEffects;
    }

    /*
     * Directly restoring Stat effect values bypasses Entity::setEffect(),
     * so multiplayer clients have not yet received the restored effect
     * list.
     *
     * EFFE contains the entity's complete active-effect bitset. Send one
     * guaranteed update after the whole saved list has been rebuilt,
     * rather than sending one packet for every individual effect.
     */
    if ( restoredEffects > 0
        && multiplayer == SERVER )
    {
        monsterEntity->serverUpdateEffectsForEntity(
            true
        );
    }

    return restoredEffects;
}
bool applyPersistentMonsterLivingState(
    Entity* monsterEntity
)
{
    if ( multiplayer == CLIENT
        || !monsterEntity
        || monsterEntity->persistentID == 0
        || monsterEntity->behavior != &actMonster )
    {
        return false;
    }

    if ( monsterEntity->persistentID < 0
        && !monsterEntity->persistentDynamicMonster )
    {
        return false;
    }

    Stat* monsterStats =
        monsterEntity->getStats();

    if ( !monsterStats
        || monsterStats->type == NOTHING )
    {
        return false;
    }

    const std::string mapKey =
        getPersistentMapKey();

    if ( mapKey.empty() )
    {
        return false;
    }

    const auto mapIterator =
        persistentMapRemovalRegistry.find(
            mapKey
        );

    if ( mapIterator
        == persistentMapRemovalRegistry.end() )
    {
        return false;
    }

    const auto stateIterator =
        mapIterator->second.mechanismStates.find(
            monsterEntity->persistentID
        );

    if ( stateIterator
        == mapIterator->second.mechanismStates.end() )
    {
        return false;
    }

    const PersistentMechanismState& savedState =
        stateIterator->second;

    /*
     * Shopkeepers retain ShopkeeperInventory as their kind because
     * that record also owns their stock. All other supported monsters
     * use MonsterLivingState.
     */
    if ( savedState.kind
            != PersistentMechanismState::Kind::MonsterLivingState
        && savedState.kind
            != PersistentMechanismState::Kind::ShopkeeperInventory )
    {
        return false;
    }

    if ( savedState.monsterSavedType == NOTHING
        || savedState.monsterSavedHP <= 0
        || savedState.monsterSavedMAXHP <= 0 )
    {
        return false;
    }

    /*
     * Refuse to apply one species' state to a different authored
     * monster after a map file is edited.
     */
    if ( savedState.monsterSavedType
        != static_cast<Sint32>(monsterStats->type) )
    {
        printlog(
            "[Persistent World] Warning: monster ID %d changed type from %d to %d; living state was not restored.",
            monsterEntity->persistentID,
            savedState.monsterSavedType,
            static_cast<Sint32>(monsterStats->type)
        );

        return false;
    }
    /*
    * The normal species initializer has already created a randomized
    * loadout. Replace it with the inventory captured on the first visit.
    *
    * Shopkeeper store inventory remains controlled by
    * applyPersistentShopkeeperInventory().
    */
    const Uint32 restoredMonsterItems =
        restorePersistentMonsterItems(
            monsterEntity,
            monsterStats,
            savedState
        );
    /*
    * Restore temporary conditions after species initialization so the
    * initializer cannot replace them with a fresh state.
    */
    const Uint32 restoredMonsterEffects =
        restorePersistentMonsterEffects(
            monsterEntity,
            monsterStats,
            savedState
        );

    const bool mechanicalCreature =
        monsterStats->type == AUTOMATON
        || monsterStats->type == SENTRYBOT
        || monsterStats->type == GYROBOT
        || monsterStats->type == SPELLBOT
        || monsterStats->type == DUMMYBOT;

    constexpr Sint32 MONSTER_REVISIT_HEALING = 5;

    Sint32 restoredHP =
        savedState.monsterSavedHP;

    if ( !mechanicalCreature )
    {
        restoredHP +=
            MONSTER_REVISIT_HEALING;
    }

    monsterStats->MAXHP =
        std::max(
            1,
            savedState.monsterSavedMAXHP
        );

    monsterStats->HP =
        std::min(
            restoredHP,
            monsterStats->MAXHP
        );

    /*
     * OLDHP should match restored HP so the first behavior tick does
     * not interpret loading as fresh damage or healing.
     */
    monsterStats->OLDHP =
        monsterStats->HP;

    monsterStats->MAXMP =
        std::max(
            0,
            savedState.monsterSavedMAXMP
        );

    monsterStats->MP =
        std::max(
            0,
            std::min(
                savedState.monsterSavedMP,
                monsterStats->MAXMP
            )
        );

    monsterEntity->x =
        savedState.monsterSavedX;

    const real_t previousX =
        monsterEntity->x;

    const real_t previousY =
        monsterEntity->y;

    const real_t previousZ =
        monsterEntity->z;
    
    monsterEntity->y =
        savedState.monsterSavedY;

    monsterEntity->z =
        savedState.monsterSavedZ;

    monsterEntity->new_x =
        monsterEntity->x;

    monsterEntity->new_y =
        monsterEntity->y;

    monsterEntity->new_z =
        monsterEntity->z;

    monsterEntity->yaw =
        savedState.monsterSavedYaw;

    monsterEntity->pitch =
        savedState.monsterSavedPitch;

    monsterEntity->roll =
        savedState.monsterSavedRoll;

    monsterEntity->new_yaw =
        monsterEntity->yaw;

    monsterEntity->new_pitch =
        monsterEntity->pitch;

    monsterEntity->new_roll =
        monsterEntity->roll;

    /*
     * Do not restore a target UID or active path because those runtime
     * entities may no longer exist after a transition.
     */
    monsterEntity->monsterTarget = 0;
    monsterEntity->monsterTargetX =
        monsterEntity->x;

    monsterEntity->monsterTargetY =
        monsterEntity->y;

    if ( monsterEntity->path )
    {
        list_FreeAll(
            monsterEntity->path
        );

        free(
            monsterEntity->path
        );

        monsterEntity->path = nullptr;
    }

    monsterEntity->vel_x = 0.0;
    monsterEntity->vel_y = 0.0;
    monsterEntity->vel_z = 0.0;

    monsterEntity->bNeedsRenderPositionInit =
        true;

printlog(
    "[Persistent World] Restored monster ID %d type %d at %.2f/%.2f: HP %d/%d, %u inventory/equipment item(s), %u temporary effect(s)%s.",
    monsterEntity->persistentID,
    static_cast<Sint32>(
        monsterStats->type
    ),
    monsterEntity->x,
    monsterEntity->y,
    monsterStats->HP,
    monsterStats->MAXHP,
    restoredMonsterItems,
    restoredMonsterEffects,
    mechanicalCreature
        ? " without passive healing"
        : " after +5 revisit healing"
);

    return true;
}
bool applyPersistentShopkeeperInventory(
    Entity* shopkeeperEntity
)
{
    if ( multiplayer == CLIENT
        || !shopkeeperEntity
        || shopkeeperEntity->persistentID <= 0
        || shopkeeperEntity->behavior != &actMonster )
    {
        return false;
    }

    Stat* shopStats =
        shopkeeperEntity->getStats();

    if ( !shopStats
        || shopStats->type != SHOPKEEPER )
    {
        return false;
    }

    const std::string mapKey =
        getPersistentMapKey();

    if ( mapKey.empty() )
    {
        return false;
    }

    const auto mapIterator =
        persistentMapRemovalRegistry.find(
            mapKey
        );

    if ( mapIterator
        == persistentMapRemovalRegistry.end() )
    {
        return false;
    }

    const auto stateIterator =
        mapIterator->second.mechanismStates.find(
            shopkeeperEntity->persistentID
        );

    if ( stateIterator
        == mapIterator->second.mechanismStates.end() )
    {
        return false;
    }

PersistentMechanismState& savedState =
    stateIterator->second;

    if ( savedState.kind
        != PersistentMechanismState::Kind::ShopkeeperInventory )
    {
        return false;
    }
	/*
	* Restock every second return to this shopkeeper's map.
	*
	* First return:
	* restore saved stock.
	*
	* Second return:
	* keep the newly generated stock from initShopkeeper().
	*/
	++savedState.shopkeeperLoadsSinceRestock;

	constexpr Sint32 SHOPKEEPER_RESTOCK_LOADS = 2; //restock after how many map revisits

	if ( savedState.shopkeeperLoadsSinceRestock
		>= SHOPKEEPER_RESTOCK_LOADS )
	{
		savedState.shopkeeperLoadsSinceRestock = 0;

		/*
		* initShopkeeper() already generated a fresh inventory.
		* Keep that inventory and copy it into persistent storage.
		*/
		savedState.shopkeeperSavedStoreType =
			shopkeeperEntity->monsterStoreType;

		savedState.shopkeeperSavedGold =
			shopStats->GOLD;

		savedState.shopkeeperSavedName =
			shopStats->name;

		savedState.shopkeeperSavedInventory.clear();

		for ( node_t* itemNode =
				shopStats->inventory.first;
			itemNode != nullptr;
			itemNode = itemNode->next )
		{
			Item* item =
				static_cast<Item*>(
					itemNode->element
				);

			if ( !item
				|| item->count <= 0 )
			{
				continue;
			}

			PersistentChestItemState itemState;

			itemState.type =
				static_cast<Sint32>(
					item->type
				);
            itemState.stableId = persistentStableItemId(itemState.type);

			itemState.status =
				static_cast<Sint32>(
					item->status
				);

			itemState.beatitude =
				item->beatitude;

			itemState.count =
				item->count;

			itemState.appearance =
				item->appearance;

			itemState.identified =
				item->identified;

			itemState.x =
				item->x;

			itemState.y =
				item->y;

			itemState.isDroppable =
				item->isDroppable;

			itemState.playerSoldItemToShop =
				item->playerSoldItemToShop;

			itemState.itemSpecialShopConsumable =
				item->itemSpecialShopConsumable;

			itemState.itemRequireTradingSkillInShop =
				item->itemRequireTradingSkillInShop;

			savedState
				.shopkeeperSavedInventory
				.push_back(itemState);
		}

		printlog(
			"[Persistent World] Restocked shopkeeper ID %d in '%s' with %zu item stack(s).",
			shopkeeperEntity->persistentID,
			mapKey.c_str(),
			savedState.shopkeeperSavedInventory.size()
		);

		return true;
	}
    /*
     * initShopkeeper() has now generated a fresh random inventory.
     * Delete it before restoring the saved authoritative inventory.
     */
    list_FreeAll(
        &shopStats->inventory
    );

    shopkeeperEntity->monsterStoreType =
        savedState.shopkeeperSavedStoreType;

    shopStats->GOLD =
        savedState.shopkeeperSavedGold;

    if ( !savedState.shopkeeperSavedName.empty() )
    {
        strncpy(
            shopStats->name,
            savedState.shopkeeperSavedName.c_str(),
            sizeof(shopStats->name) - 1
        );

        shopStats->name[
            sizeof(shopStats->name) - 1
        ] = '\0';
    }

    Uint32 restoredItems = 0;

    for ( const PersistentChestItemState& itemState :
        savedState.shopkeeperSavedInventory )
    {
        const Sint32 resolvedType = resolvePersistentItemType(
            itemState.stableId,
            itemState.type
        );
        if ( resolvedType < 0
            || itemState.count <= 0 )
        {
            printlog(
                "[Persistent World] Ignored invalid shop item for shopkeeper ID %d: type=%d count=%d.",
                shopkeeperEntity->persistentID,
                resolvedType,
                itemState.count
            );

            continue;
        }

        Item* restoredItem =
            newItem(
                static_cast<ItemType>(
                    resolvedType
                ),
                static_cast<Status>(
                    itemState.status
                ),
                itemState.beatitude,
                itemState.count,
                itemState.appearance,
                itemState.identified,
                &shopStats->inventory
            );

        if ( !restoredItem )
        {
            continue;
        }

        restoredItem->x =
            itemState.x;

        restoredItem->y =
            itemState.y;

        restoredItem->isDroppable =
            itemState.isDroppable;

        restoredItem->playerSoldItemToShop =
            itemState.playerSoldItemToShop;

        restoredItem->itemSpecialShopConsumable =
            itemState.itemSpecialShopConsumable;

        restoredItem->itemRequireTradingSkillInShop =
            itemState.itemRequireTradingSkillInShop;

        ++restoredItems;
    }

    printlog(
        "[Persistent World] Restored shopkeeper ID %d in '%s': store type %d, gold %d, %u item stack(s).",
        shopkeeperEntity->persistentID,
        mapKey.c_str(),
        shopkeeperEntity->monsterStoreType,
        shopStats->GOLD,
        restoredItems
    );

    return true;
}
static void capturePersistentMapRemovals()
{
    // Multiplayer clients do not own world persistence.
    if ( multiplayer == CLIENT )
    {
        return;
    }

    const std::string mapKey =
        getPersistentMapKey();

    if ( mapKey.empty() )
    {
        return;
    }

    auto stateIterator =
        persistentMapRemovalRegistry.find(mapKey);

    if ( stateIterator
        == persistentMapRemovalRegistry.end()
        || !stateIterator->second.originalIDsRegistered )
    {
        /*
         * This can happen when loading into an already-running session
         * from a path that has not passed through our restore helper yet.
         * Register the currently existing IDs as the baseline rather than
         * incorrectly declaring every entity removed.
         */
        PersistentMapRemovalState& state =
            persistentMapRemovalRegistry[mapKey];

        for ( node_t* node = map.entities->first;
            node != nullptr;
            node = node->next )
        {
            Entity* entity =
                static_cast<Entity*>(node->element);

            if ( entity && entity->persistentID > 0 && entity->sprite != 1 )
            {
                state.originalEntityIDs.insert(
                    entity->persistentID
                );
            }
        }

        state.originalIDsRegistered = true;

        printlog(
            "[Persistent World] Registered fallback baseline for '%s' with %zu currently present ID(s).",
            mapKey.c_str(),
            state.originalEntityIDs.size()
        );

        return;
    }

    PersistentMapRemovalState& state =
        stateIterator->second;

    std::unordered_set<Sint32> currentlyPresentIDs;

    for ( node_t* node = map.entities->first;
        node != nullptr;
        node = node->next )
    {
        Entity* entity =
            static_cast<Entity*>(node->element);

        if ( !entity || entity->persistentID <= 0 )
        {
            continue;
        }
		if ( entity->sprite == 1 )
		{
			continue;
		}

        currentlyPresentIDs.insert(
            entity->persistentID
        );
    }

    Uint32 newlyRemovedEntities = 0;

    for ( const Sint32 originalID :
        state.originalEntityIDs )
    {
        if ( currentlyPresentIDs.find(originalID)
            != currentlyPresentIDs.end() )
        {
            continue;
        }

        const auto result =
            state.removedEntityIDs.insert(
                originalID
            );

        if ( result.second )
        {
            ++newlyRemovedEntities;

            printlog(
                "[Persistent World] Entity ID %d is missing from '%s'; recording it as removed.",
                originalID,
                mapKey.c_str()
            );
        }
    }

    printlog(
        "[Persistent World] Captured '%s': %zu entity ID(s) currently present, %u newly removed, %zu total removals.",
        mapKey.c_str(),
        currentlyPresentIDs.size(),
        newlyRemovedEntities,
        state.removedEntityIDs.size()
    );
}

static bool placePlayerAtAutomatiaReturn(
    const int playerIndex,
    const AutomatiaPlayerLevelVisit& destination
)
{
    if (playerIndex < 0 || playerIndex >= MAXPLAYERS
        || client_disconnected[playerIndex]
        || !players[playerIndex]
        || !players[playerIndex]->entity
        || !destination.returnPlacement.valid)
    {
        return false;
    }
    const WorldInstanceIdentity* activeIdentity =
        worldState.activeIdentity();
    if (!activeIdentity
        || (!destination.mapInstanceKey.empty()
            && activeIdentity->key() != destination.mapInstanceKey)
        || destination.dungeonLevel != currentlevel
        || destination.secretLevel != secretlevel)
    {
        printlog(
            "[Ladder Reverse] Refused stale return placement for player %d: requested '%s' level %d (%s), active '%s' level %d (%s).",
            playerIndex,
            destination.mapInstanceKey.c_str(),
            destination.dungeonLevel,
            destination.secretLevel ? "secret" : "regular",
            activeIdentity ? activeIdentity->key().c_str() : "unknown",
            currentlevel,
            secretlevel ? "secret" : "regular"
        );
        return false;
    }

    real_t anchorDeltaX = 0.0;
    real_t anchorDeltaY = 0.0;
    real_t anchorDeltaZ = 0.0;
    if (destination.hasReturnAnchor
        && destination.returnAnchorPersistentID > 0)
    {
        for (node_t* node = map.entities->first; node; node = node->next)
        {
            Entity* entity = static_cast<Entity*>(node->element);
            if (entity
                && entity->persistentID
                    == destination.returnAnchorPersistentID)
            {
                anchorDeltaX = entity->x - destination.returnAnchorX;
                anchorDeltaY = entity->y - destination.returnAnchorY;
                anchorDeltaZ = entity->z - destination.returnAnchorZ;
                break;
            }
        }
    }

    const real_t x = static_cast<real_t>(
        destination.returnPlacement.x + anchorDeltaX);
    const real_t y = static_cast<real_t>(
        destination.returnPlacement.y + anchorDeltaY);
    const real_t z = static_cast<real_t>(
        destination.returnPlacement.z + anchorDeltaZ);
    const real_t yaw = static_cast<real_t>(
        destination.returnPlacement.yaw);
    const real_t pitch = static_cast<real_t>(
        destination.returnPlacement.pitch);
    const real_t roll = static_cast<real_t>(
        destination.returnPlacement.roll);

    bool placementPassable = false;
    if (std::isfinite(static_cast<double>(x))
        && std::isfinite(static_cast<double>(y))
        && std::isfinite(static_cast<double>(z))
        && std::isfinite(static_cast<double>(yaw))
        && x >= 0.0 && y >= 0.0
        && x < static_cast<real_t>(map.width) * 16.0
        && y < static_cast<real_t>(map.height) * 16.0)
    {
        const Sint32 tileX = static_cast<Sint32>(x / 16.0);
        const Sint32 tileY = static_cast<Sint32>(y / 16.0);
        const std::size_t obstacleIndex =
            OBSTACLELAYER
            + tileY * MAPLAYERS
            + tileX * MAPLAYERS * map.height;
        placementPassable = map.tiles
            && map.tiles[obstacleIndex] == 0;
    }
    if (!placementPassable)
    {
        printlog(
            "[Ladder Reverse] Return placement for player %d is blocked or outside '%s'; retaining the safe Player Start.",
            playerIndex,
            activeIdentity->key().c_str()
        );
        return false;
    }

    Entity* playerEntity = players[playerIndex]->entity;
    playerEntity->x = x;
    playerEntity->y = y;
    playerEntity->z = z;
    playerEntity->yaw = yaw;
    playerEntity->pitch = pitch;
    playerEntity->roll = roll;
    playerEntity->vel_x = 0.0;
    playerEntity->vel_y = 0.0;
    playerEntity->vel_z = 0.0;
    playerEntity->new_x = x;
    playerEntity->new_y = y;
    playerEntity->new_z = z;
    playerEntity->new_yaw = yaw;
    playerEntity->new_pitch = pitch;
    playerEntity->new_roll = roll;
    playerEntity->lerp_ox = x;
    playerEntity->lerp_oy = y;
    playerEntity->bNeedsRenderPositionInit = true;
    for (Entity* bodypart : playerEntity->bodyparts)
    {
        if (bodypart)
        {
            bodypart->bNeedsRenderPositionInit = true;
        }
    }
    players[playerIndex]->player_last_x = x;
    players[playerIndex]->player_last_y = y;

    if (multiplayer == SERVER
        && playerIndex > 0
        && !players[playerIndex]->isLocalPlayer()
        && net_packet
        && net_packet->data)
    {
        std::memcpy(net_packet->data, "TNSP", 4);
        SDLNet_Write32(
            static_cast<Sint32>(x * 32.0),
            &net_packet->data[4]
        );
        SDLNet_Write32(
            static_cast<Sint32>(y * 32.0),
            &net_packet->data[8]
        );
        SDLNet_Write32(
            static_cast<Sint32>(z * 32.0),
            &net_packet->data[12]
        );
        SDLNet_Write32(
            static_cast<Sint32>(yaw * 256.0),
            &net_packet->data[16]
        );
        net_packet->address.host =
            net_clients[playerIndex - 1].host;
        net_packet->address.port =
            net_clients[playerIndex - 1].port;
        net_packet->len = 20;
        sendPacketSafe(
            net_sock,
            -1,
            net_packet,
            playerIndex - 1
        );
    }

    printlog(
        "[Ladder Reverse] Restored only player %d beside the previous exit in '%s' at %.2f, %.2f, %.2f.",
        playerIndex,
        activeIdentity->key().c_str(),
        x,
        y,
        z
    );
    return true;
}

static bool placePendingAutomatiaSinglePlayerReverseReturn()
{
    if (!pendingAutomatiaSinglePlayerReverseReturn.pending)
    {
        return false;
    }
    const int playerIndex =
        pendingAutomatiaSinglePlayerReverseReturn.playerIndex;
    const AutomatiaPlayerLevelVisit destination =
        pendingAutomatiaSinglePlayerReverseReturn.visit;
    pendingAutomatiaSinglePlayerReverseReturn =
        PendingAutomatiaSinglePlayerReverseReturn{};
    return placePlayerAtAutomatiaReturn(
        playerIndex,
        destination
    );
}

static bool placePlayersAtCustomTunnel(
    const Sint32 destinationTunnelID,
    const bool* playerMask = nullptr
)
{
    if ( destinationTunnelID <= 0 )
    {
        // ID 0 intentionally means use the normal Player Start.
        return false;
    }

    Entity* destinationTunnel = nullptr;

    for ( node_t* node = map.entities->first;
        node != nullptr;
        node = node->next )
    {
        Entity* entity =
            static_cast<Entity*>(node->element);

        if ( !entity )
        {
            continue;
        }

        if ( entity->behavior != &actCustomPortal )
        {
            continue;
        }

        if ( entity->portalCustomTunnelID
            != destinationTunnelID )
        {
            continue;
        }

        destinationTunnel = entity;
        break;
    }

    if ( !destinationTunnel )
    {
        printlog(
            "[Custom Tunnel] Destination tunnel ID %d was not found in map '%s'. Using normal Player Start.",
            destinationTunnelID,
            map.name
        );
        return false;
    }

    // Place the first player one tile behind the tunnel's facing
    // direction so they do not immediately overlap/retrigger it.
    const real_t baseX =
        destinationTunnel->x
        - cos(destinationTunnel->yaw) * 16.0;

    const real_t baseY =
        destinationTunnel->y
        - sin(destinationTunnel->yaw) * 16.0;

    static const real_t partyOffsetX[MAXPLAYERS] =
    {
        0.0,
        8.0,
        -8.0,
        0.0
    };

    static const real_t partyOffsetY[MAXPLAYERS] =
    {
        0.0,
        0.0,
        0.0,
        8.0
    };

    int placedPlayers = 0;

    for ( int player = 0;
        player < MAXPLAYERS;
        ++player )
    {
        if ( (playerMask && !playerMask[player])
            || client_disconnected[player]
            || players[player] == nullptr
            || players[player]->entity == nullptr )
        {
            continue;
        }

        Entity* playerEntity =
            players[player]->entity;

        playerEntity->x =
            baseX + partyOffsetX[player];

        playerEntity->y =
            baseY + partyOffsetY[player];

        // Preserve the player's usual standing Z while aligning
        // with vertically placed custom tunnels.
        playerEntity->z =
            destinationTunnel->z;

        playerEntity->yaw =
            destinationTunnel->yaw;

        playerEntity->vel_x = 0.0;
        playerEntity->vel_y = 0.0;
        playerEntity->vel_z = 0.0;

        playerEntity->new_x = playerEntity->x;
        playerEntity->new_y = playerEntity->y;
        playerEntity->new_z = playerEntity->z;
        playerEntity->new_yaw = playerEntity->yaw;

        // Reset interpolation so the camera does not slide from
        // the normal Player Start to the tunnel destination.
        playerEntity->bNeedsRenderPositionInit = true;
		// The server owns the final spawn position.
		// Tell a remote client where its local player must appear after
		// the destination map finishes loading.
		if ( multiplayer == SERVER
			&& player > 0
			&& !players[player]->isLocalPlayer()
			&& net_packet
			&& net_packet->data )
		{
			strcpy(
				reinterpret_cast<char*>(net_packet->data),
				"TNSP"
			);

			SDLNet_Write32(
				static_cast<Sint32>(
					playerEntity->x * 32.0
				),
				&net_packet->data[4]
			);

			SDLNet_Write32(
				static_cast<Sint32>(
					playerEntity->y * 32.0
				),
				&net_packet->data[8]
			);

			SDLNet_Write32(
				static_cast<Sint32>(
					playerEntity->z * 32.0
				),
				&net_packet->data[12]
			);

			SDLNet_Write32(
				static_cast<Sint32>(
					playerEntity->yaw * 256.0
				),
				&net_packet->data[16]
			);

			net_packet->address.host =
				net_clients[player - 1].host;

			net_packet->address.port =
				net_clients[player - 1].port;

			net_packet->len = 20;

			sendPacketSafe(
				net_sock,
				-1,
				net_packet,
				player - 1
			);

			printlog(
				"[Custom Tunnel] Sent tunnel spawn to client %d: x=%.2f y=%.2f z=%.2f yaw=%.2f.",
				player,
				playerEntity->x,
				playerEntity->y,
				playerEntity->z,
				playerEntity->yaw
			);
		}
        ++placedPlayers;
    }

    printlog(
        "[Custom Tunnel] Placed %d player(s) at tunnel ID %d in map '%s'.",
        placedPlayers,
        destinationTunnelID,
        map.name
    );

    return placedPlayers > 0;
}

static bool processAutomatiaTransition(
	const PendingAutomatiaTransition& transition,
	std::string& error
)
{
	error.clear();
	const int player = transition.playerIndex;
	if ( multiplayer != SERVER
		|| player < 0
		|| player >= MAXPLAYERS
		|| client_disconnected[player]
		|| !players[player] )
	{
		error = "player is no longer connected";
		return false;
	}
	const WorldInstanceIdentity* initialActiveIdentity =
		worldState.activeIdentity();
	const std::string initialActiveKey =
		initialActiveIdentity ? initialActiveIdentity->key() : std::string{};
	auto restoreInitialInstance = [&initialActiveKey]()
	{
		if ( !initialActiveKey.empty() )
		{
			MapInstance* initial = worldState.find(initialActiveKey);
			if ( initial && initial->loadedMap )
			{
				worldState.activate(initialActiveKey);
			}
		}
	};
	const std::string sourceKey = players[player]->worldInstance.key();
	MapInstance* source = worldState.find(sourceKey);
	Entity* sourceEntity = worldState.playerEntityFor(sourceKey, player);
	if ( !source || !sourceEntity )
	{
		error = "source instance or player entity is unavailable";
		return false;
	}

	AutomatiaPlayerLevelVisit sourceVisit;
	const bool sourceVisitReady = transition.recordSourceVisit
		&& buildAutomatiaPlayerLevelVisit(
			player,
			*source,
			*sourceEntity,
			nullptr,
			sourceVisit
		);

	std::string fullMapPath;
	WorldInstanceIdentity destinationIdentity;
	if ( !transition.destinationInstanceKey.empty() )
	{
		if ( !parseAutomatiaMapInstanceKey(
			transition.destinationInstanceKey,
			destinationIdentity
		) )
		{
			error = "destination map-instance identity is invalid";
			return false;
		}
		if ( !transition.destinationGenerated )
		{
			fullMapPath = physfsFormatMapName(
				destinationIdentity.mapFile.c_str()
			);
		}
	}
	else
	{
		fullMapPath =
			physfsFormatMapName(transition.destinationMap.c_str());
		if ( fullMapPath.empty() )
		{
			error = "destination map could not be resolved";
			return false;
		}
		const std::string mapFile =
			std::filesystem::path(fullMapPath).filename().string();
		if ( !destinationIdentity.set(mapFile, "world") )
		{
			error = "destination identity is invalid";
			return false;
		}
	}
	if ( !transition.destinationGenerated && fullMapPath.empty() )
	{
		error = "destination map could not be resolved";
		return false;
	}
	constexpr std::size_t customTransitionFixedBytes = 14 + 1 + 4 + 6;
	if ( transition.destinationMap.size()
		> NET_PACKET_SIZE - 9 - customTransitionFixedBytes )
	{
		error = "destination map name is too long for the transition packet";
		return false;
	}
	const std::string destinationKey = destinationIdentity.key();
	if ( destinationKey == sourceKey )
	{
		if ( !worldState.activate(sourceKey) )
		{
			error = "source instance could not be activated";
			return false;
		}
		bool playerMask[MAXPLAYERS] = {};
		playerMask[player] = true;
		if ( transition.hasReturnPlacement )
		{
			placePlayerAtAutomatiaReturn(
				player,
				transition.reverseVisit
			);
		}
		else
		{
			placePlayersAtCustomTunnel(
				transition.destinationTunnelID,
				playerMask
			);
		}
		if ( sourceVisitReady )
		{
			appendAutomatiaPlayerLevelVisit(
				player,
				sourceVisit
			);
		}
		if ( !initialActiveKey.empty() && initialActiveKey != sourceKey )
		{
			worldState.activate(initialActiveKey);
		}
		return true;
	}

	MapInstance* destination = worldState.find(destinationKey);
	const bool destinationWasKnown = destination != nullptr;
	if ( !destination || !destination->loadedMap )
	{
		const bool loaded = transition.destinationGenerated
			? worldState.loadDetachedGeneratedLevel(
				destinationIdentity,
				transition.destinationLevel,
				transition.destinationMapSeed,
				transition.destinationSecret,
				error
			)
			: worldState.loadDetachedMap(
				fullMapPath,
				destinationIdentity.mapFile,
				destinationIdentity.instanceId,
				error
			);
		if ( !loaded )
		{
			return false;
		}
		destination = worldState.find(destinationKey);
	}
	if ( !destination || !destination->loadedMap )
	{
		error = "destination instance is not loaded";
		return false;
	}
	if ( !transition.destinationInstanceKey.empty()
		&& destination->runtimeInitialized
		&& (destination->dungeonLevel != transition.destinationLevel
			|| destination->secretLevel != transition.destinationSecret
			|| destination->mapSeed != transition.destinationMapSeed) )
	{
		error = "loaded destination metadata does not match the player's route history";
		return false;
	}
	// Loading a previously unknown destination can insert into WorldState's
	// unordered registry and invalidate pointers obtained before that insert.
	// Reacquire the source instance (and its player entity) by stable key.
	source = worldState.find(sourceKey);
	sourceEntity = worldState.playerEntityFor(sourceKey, player);
	destination = worldState.find(destinationKey);
	if ( !source || !source->loadedMap || !sourceEntity
		|| !destination || !destination->loadedMap )
	{
		error = "source or destination instance changed while loading";
		restoreInitialInstance();
		return false;
	}
	if ( !destination->runtimeInitialized )
	{
		destination->dungeonLevel = transition.destinationLevel;
		destination->secretLevel = transition.destinationSecret;
		if ( !transition.destinationInstanceKey.empty() )
		{
			destination->mapSeed = transition.destinationMapSeed;
		}
		else if ( !destinationWasKnown )
		{
			destination->mapSeed = local_rng.rand();
		}
	}
	if ( !worldState.activate(sourceKey) )
	{
		error = "source instance could not be activated for follower transfer";
		restoreInitialInstance();
		return false;
	}
	struct TransferredFollower
	{
		Entity* source = nullptr;
		std::unique_ptr<Stat> stats;
		real_t offsetX = 0.0;
		real_t offsetY = 0.0;
	};
	std::vector<TransferredFollower> transferredFollowers;
	if ( stats[player] )
	{
		for ( node_t* node = stats[player]->FOLLOWERS.first;
			node;
			node = node->next )
		{
			if ( !node->element )
			{
				continue;
			}
			Entity* follower = uidToEntity(*static_cast<Uint32*>(node->element));
			Stat* followerStats = follower ? follower->getStats() : nullptr;
			if ( !follower || !followerStats || follower->behavior != &actMonster )
			{
				continue;
			}
			/*
			 * Match Barony's original whole-floor follower transfer. A monster
			 * can temporarily move its weapon or shield out of the equipped slot
			 * during a special attack. Finish that temporary state before copying
			 * the authoritative inventory/equipment loadout.
			 */
			if ( static_cast<int>(follower->monsterSpecialAttackUnequipSafeguard) > 0 )
			{
				follower->handleMonsterSpecialAttack(
					followerStats,
					nullptr,
					0.0,
					true
				);
			}
			transferredFollowers.push_back(TransferredFollower{
				follower,
				std::unique_ptr<Stat>(followerStats->copyStats()),
				follower->x - sourceEntity->x,
				follower->y - sourceEntity->y
			});
		}
	}
	if ( !worldState.activate(destinationKey) )
	{
		error = "destination instance could not be activated";
		restoreInitialInstance();
		return false;
	}

	const Uint32 mapLoadUidStart = destination->mapLoadEntityUidStart;
	const Uint32 runtimeUidStart = destination->runtimeEntityUidStart;
	bool playerMask[MAXPLAYERS] = {};
	playerMask[player] = true;
	if ( !destination->runtimeInitialized )
	{
		applyPersistentMapRemovals();
		numplayers = 0;
		assignActions(&map, playerMask, true, false);
		applyPersistentMechanismStates();
		generatePathMaps();
	}
	else
	{
		numplayers = 0;
		assignActions(&map, playerMask, true, true);
	}
	Entity* destinationEntity = players[player]->entity;
	if ( !destinationEntity )
	{
		Entity* spawnAnchor = nullptr;
		Entity* retainedPlayerStart = nullptr;
		for ( node_t* node = map.entities->first; node; node = node->next )
		{
			Entity* candidate = static_cast<Entity*>(node->element);
			if ( candidate && candidate->sprite == 1
				&& candidate->behavior == nullptr )
			{
				retainedPlayerStart = candidate;
				break;
			}
		}
		for ( int anchorPlayer = 0; anchorPlayer < MAXPLAYERS; ++anchorPlayer )
		{
			if ( anchorPlayer == player || !players[anchorPlayer]
				|| !players[anchorPlayer]->entity
				|| players[anchorPlayer]->worldInstance.key() != destinationKey )
			{
				continue;
			}
			spawnAnchor = players[anchorPlayer]->entity;
			break;
		}
		if ( spawnAnchor || retainedPlayerStart )
		{
			Entity* fallbackStart = newEntity(1, 1, map.entities, nullptr);
			if ( fallbackStart )
			{
				fallbackStart->x = spawnAnchor
					? spawnAnchor->x - 8.0
					: retainedPlayerStart->x;
				fallbackStart->y = spawnAnchor
					? spawnAnchor->y - 8.0
					: retainedPlayerStart->y;
				fallbackStart->z = spawnAnchor
					? spawnAnchor->z
					: retainedPlayerStart->z;
				fallbackStart->skill[2] = player;
				fallbackStart->playerStartDir = -1;
				numplayers = 0;
				assignActions(&map, playerMask, true, true);
				destinationEntity = players[player]->entity;
				printlog(
					"[World State] Custom transition player %d used the %s spawn fallback in '%s'.",
					player,
					spawnAnchor ? "occupied-map" : "retained Player Start",
					destinationKey.c_str()
				);
			}
		}
	}
	if ( !destinationEntity )
	{
		worldState.activate(sourceKey);
		restoreInitialInstance();
		error = "destination has no deferred Player Start for this player";
		return false;
	}
	if ( !worldState.placePlayer(player, map) )
	{
		worldState.activate(sourceKey);
		restoreInitialInstance();
		error = "destination player ownership could not be recorded";
		return false;
	}

	if ( player > 0
		&& !players[player]->isLocalPlayer()
		&& net_packet
		&& net_packet->data )
	{
		sendPersistentWorldSnapshotToClient(
			player,
			destinationKey
		);
		std::memcpy(net_packet->data, "LVLC", 4);
		net_packet->data[4] = transition.destinationSecret;
		SDLNet_Write32(destination->mapSeed, &net_packet->data[5]);
		SDLNet_Write32(mapLoadUidStart, &net_packet->data[9]);
		net_packet->data[13] = transition.destinationLevel;
		const std::size_t mapNameLength = transition.destinationMap.size();
		std::memcpy(
			&net_packet->data[14],
			transition.destinationMap.c_str(),
			mapNameLength
		);
		net_packet->data[14 + mapNameLength] = 0;
		const std::size_t tunnelOffset = 14 + mapNameLength + 1;
		SDLNet_Write32(
			static_cast<Uint32>(transition.destinationTunnelID),
			&net_packet->data[tunnelOffset]
		);
		const std::size_t extensionOffset = tunnelOffset + sizeof(Uint32);
		net_packet->data[extensionOffset] = 0xA1;
		net_packet->data[extensionOffset + 1] = player;
		SDLNet_Write32(runtimeUidStart, &net_packet->data[extensionOffset + 2]);
		net_packet->len = extensionOffset + 6;
		net_packet->address.host = net_clients[player - 1].host;
		net_packet->address.port = net_clients[player - 1].port;
		sendPacketSafe(net_sock, -1, net_packet, player - 1);
	}

	// LVLC must enter the reliable queue before TNSP, otherwise the old
	// source-map entity could consume the destination position.
	if ( transition.hasReturnPlacement )
	{
		placePlayerAtAutomatiaReturn(
			player,
			transition.reverseVisit
		);
	}
	else
	{
		placePlayersAtCustomTunnel(
			transition.destinationTunnelID,
			playerMask
		);
	}
	destinationEntity = players[player]->entity;
	if (destinationEntity
		&& destinationEntity->behavior == &actPlayer
		&& destinationEntity->skill[0] == 0)
	{
		// Independent arrivals are announced before the next simulation tick.
		// Initialize their server-side voxel children now so the following
		// bodypart baseline has real limb IDs and sprites to serialize.
		actPlayer(destinationEntity);
	}
	if ( player > 0
		&& !players[player]->isLocalPlayer() )
	{
		int baselineEntities = 0;
		for ( node_t* node = map.entities->first; node; node = node->next )
		{
			Entity* entity = static_cast<Entity*>(node->element);
			if ( !entity
				|| !entity->flags[UPDATENEEDED]
				|| entity->flags[NOUPDATE] )
			{
				continue;
			}
			sendEntityUDP(entity, player, true);
			++baselineEntities;
		}
		printlog(
			"[World State] Sent %d existing destination entity baseline(s) to player %d in '%s' (map UID start %u, runtime UID start %u).",
			baselineEntities,
			player,
			destinationKey.c_str(),
			mapLoadUidStart,
			runtimeUidStart
		);
		int notifiedPlayers = 0;
		for ( const int recipient : destination->playersPresent )
		{
			if ( recipient <= 0
				|| recipient >= MAXPLAYERS
				|| recipient == player
				|| client_disconnected[recipient]
				|| !players[recipient]
				|| players[recipient]->isLocalPlayer() )
			{
				continue;
			}
			sendEntityUDP(destinationEntity, recipient, true);
			++notifiedPlayers;
		}
		printlog(
			"[World State] Announced arriving player %d UID %u sprite %d to %d existing player(s) in '%s'.",
			player,
			destinationEntity ? destinationEntity->getUID() : 0,
			destinationEntity ? destinationEntity->sprite : -1,
			notifiedPlayers,
			destinationKey.c_str()
		);
		if (destinationEntity)
		{
			// The head ENTU lets an existing client construct the remote
			// actPlayer immediately. Follow it with the authoritative limb IDs
			// and visible equipment/body sprites; the original initialization
			// broadcast occurred before that client knew the new head UID.
			serverUpdateBodypartIDs(destinationEntity);
			for (int bodypart = 1; bodypart < 11; ++bodypart)
			{
				serverUpdateEntityBodypart(destinationEntity, bodypart);
			}
			serverUpdateEntityBodypart(destinationEntity, 13);
			printlog(
				"[World State] Queued arriving player %d voxel bodypart baseline in '%s'.",
				player,
				destinationKey.c_str());
		}
	}
	if ( stats[player] )
	{
		list_FreeAll(&stats[player]->FOLLOWERS);
	}
	for ( TransferredFollower& transfer : transferredFollowers )
	{
		if ( !transfer.stats )
		{
			continue;
		}
		Entity* follower = summonMonster(
			transfer.stats->type,
			destinationEntity->x + transfer.offsetX,
			destinationEntity->y + transfer.offsetY
		);
		if ( !follower || !follower->children.last )
		{
			printlog("[World State] Warning: unable to transfer one follower for player %d.", player);
			continue;
		}
		/*
		 * summonMonster() creates a fresh Stat and leaves MONSTER_INIT at 0.
		 * The copied follower Stat already contains the authoritative inventory
		 * and equipped pointers, so mark only the stats as initialized. The next
		 * actMonster() pass will still build the species bodyparts, but its
		 * init<Type>() routine will not randomize or overwrite that loadout.
		 */
		follower->skill[3] = 1;
		list_RemoveNode(follower->children.last);
		node_t* statNode = list_AddNodeLast(&follower->children);
		statNode->element = transfer.stats.release();
		statNode->deconstructor = &statDeconstructor;
		statNode->size = sizeof(Stat);
		Stat* followerStats = static_cast<Stat*>(statNode->element);
		followerStats->leader_uid = destinationEntity->getUID();
		follower->monsterAllyIndex = player;
		follower->monsterAllyClass = followerStats->allyClass;
		follower->monsterAllyPickupItems = followerStats->allyItemPickup;
		follower->flags[USERFLAG2] = true;
		serverUpdateEntityFlag(follower, USERFLAG2);
		serverUpdateEntitySkill(follower, 42);
		serverUpdateEntitySkill(follower, 46);
		serverUpdateEntitySkill(follower, 44);
		node_t* followerNode = list_AddNodeLast(&stats[player]->FOLLOWERS);
		followerNode->deconstructor = &defaultDeconstructor;
		followerNode->size = sizeof(Uint32);
		followerNode->element = std::malloc(sizeof(Uint32));
		if ( followerNode->element )
		{
			*static_cast<Uint32*>(followerNode->element) = follower->getUID();
		}
		else
		{
			list_RemoveNode(followerNode);
		}
		if ( player > 0
			&& !players[player]->isLocalPlayer()
			&& net_packet
			&& net_packet->data )
		{
			std::memcpy(net_packet->data, "LEAD", 4);
			SDLNet_Write32(follower->getUID(), &net_packet->data[4]);
			SDLNet_Write32(followerStats->type, &net_packet->data[8]);
			const std::size_t nameLength = std::min<std::size_t>(
				std::strlen(followerStats->name),
				sizeof(followerStats->name) - 1
			);
			std::memcpy(&net_packet->data[12], followerStats->name, nameLength);
			net_packet->data[12 + nameLength] = 0;
			net_packet->len = 13 + nameLength;
			net_packet->address.host = net_clients[player - 1].host;
			net_packet->address.port = net_clients[player - 1].port;
			sendPacketSafe(net_sock, -1, net_packet, player - 1);
		}
		serverUpdateAllyStat(
			player,
			follower->getUID(),
			followerStats->LVL,
			followerStats->HP,
			followerStats->MAXHP,
			followerStats->type
		);
	}

	if ( !worldState.activate(sourceKey) )
	{
		restoreInitialInstance();
		error = "source instance could not be reactivated for cleanup";
		return false;
	}
	Entity* deferredStart = newEntity(1, 1, map.entities, nullptr);
	deferredStart->x = sourceEntity->x - 8.0;
	deferredStart->y = sourceEntity->y - 8.0;
	deferredStart->z = sourceEntity->z;
	deferredStart->skill[2] = player;
	deferredStart->flags[INVISIBLE] = true;
	deferredStart->flags[NOUPDATE] = true;
	deferredStart->playerStartDir = static_cast<int>(
		std::round(sourceEntity->yaw / (PI / 4.0))
	) & 7;
	auto removeEntityTree = [](Entity* entity)
	{
		if ( !entity )
		{
			return;
		}
		const std::vector<Entity*> bodyparts = entity->bodyparts;
		for ( Entity* bodypart : bodyparts )
		{
			if ( bodypart && bodypart->mynode
				&& bodypart->mynode->list == map.entities )
			{
				list_RemoveNode(bodypart->mynode);
			}
		}
		if ( entity->mynode && entity->mynode->list == map.entities )
		{
			list_RemoveNode(entity->mynode);
		}
	};
	for ( const TransferredFollower& transfer : transferredFollowers )
	{
		removeEntityTree(transfer.source);
	}
	removeEntityTree(sourceEntity);
	capturePersistentMinimap(false);
	capturePersistentMechanismStates();
	capturePersistentMapRemovals();
	source->dirty = true;
	if ( entitiesdeleted.first )
	{
		const int deletedEntityCount = list_Size(&entitiesdeleted);
		list_FreeAll(&entitiesdeleted);
		printlog(
			"[World State] Completed %d deferred source entity deletion(s) before leaving '%s'.",
			deletedEntityCount,
			sourceKey.c_str()
		);
	}
	const bool sourceBecameEmpty = source->playersPresent.empty();
	if ( sourceBecameEmpty || players[player]->isLocalPlayer() )
	{
		if ( !worldState.activate(destinationKey) )
		{
			error = "destination instance could not be reactivated after source cleanup";
			restoreInitialInstance();
			return false;
		}
	}
	if ( sourceBecameEmpty )
	{
		if ( !worldState.unloadEmptyInstance(sourceKey) )
		{
			printlog(
				"[World State] Warning: empty source instance '%s' remained loaded.",
				sourceKey.c_str()
			);
		}
	}
	if ( !players[player]->isLocalPlayer()
		&& !initialActiveKey.empty()
		&& worldState.find(initialActiveKey)
		&& worldState.find(initialActiveKey)->loadedMap )
	{
		worldState.activate(initialActiveKey);
	}
	if ( sourceVisitReady )
	{
		appendAutomatiaPlayerLevelVisit(
			player,
			std::move(sourceVisit)
		);
	}
	printlog(
		"[World State] Player %d transitioned independently from '%s' to '%s'.",
		player,
		sourceKey.c_str(),
		destinationKey.c_str()
	);
	return destinationEntity != nullptr;
}

static void processPendingAutomatiaTransitions()
{
	if ( pendingAutomatiaTransitions.empty() )
	{
		return;
	}
	std::vector<PendingAutomatiaTransition> pending =
		std::move(pendingAutomatiaTransitions);
	pendingAutomatiaTransitions.clear();
	for ( const PendingAutomatiaTransition& transition : pending )
	{
		std::string error;
		if ( !processAutomatiaTransition(transition, error) )
		{
			if ( !transition.recordSourceVisit
				&& !transition.destinationInstanceKey.empty() )
			{
				restoreAutomatiaPreviousPlayerLevel(
					transition.playerIndex,
					transition.reverseVisit
				);
			}
			printlog(
				"[World State] Independent transition for player %d failed: %s.",
				transition.playerIndex,
				error.c_str()
			);
			messagePlayer(
				transition.playerIndex,
				MESSAGE_MISC,
				"The destination could not be loaded."
			);
		}
	}
}
#ifdef LINUX
//Sigsegv catching stuff.
#include <signal.h>
#include <string.h>
#include <execinfo.h>
#include <sys/stat.h>

static SDL_bool SDL_MouseModeBeforeSignal = SDL_FALSE;
static int SDL_MouseShowBeforeSignal = SDL_ENABLE;

static void stop_sigaction(int signal, siginfo_t* si, void* arg)
{
    SDL_MouseModeBeforeSignal = SDL_GetRelativeMouseMode();
    SDL_MouseShowBeforeSignal = SDL_ShowCursor(SDL_QUERY);
	SDL_SetRelativeMouseMode(SDL_FALSE);
	SDL_ShowCursor(SDL_ENABLE);
}

static void continue_sigaction(int signal, siginfo_t* si, void* arg)
{
	SDL_SetRelativeMouseMode(SDL_MouseModeBeforeSignal);
	SDL_ShowCursor(SDL_MouseShowBeforeSignal);
}

static void segfault_sigaction(int signal, siginfo_t* si, void* arg)
{
	SDL_SetRelativeMouseMode(SDL_FALSE); //Uncapture mouse.

	printf("Caught segfault at address %p\n", si->si_addr);
	printf("Signal %d (dumping stack):", signal);

	//Dump the stack.

    constexpr unsigned int STACK_SIZE = 32;
	void* array[STACK_SIZE];
	size_t size = backtrace(array, STACK_SIZE);
	backtrace_symbols_fd(array, size, STDERR_FILENO);

	exit(0);
}

#endif

#ifdef APPLE

#include <sys/stat.h>

#endif

#ifdef BSD

#include <sys/types.h>
#include <sys/sysctl.h>

#endif

#ifdef HAIKU

#include <FindDirectory.h>

#endif

#ifdef WINDOWS
void make_minidump(EXCEPTION_POINTERS* e)
{
	auto hDbgHelp = LoadLibraryA("dbghelp");
	if ( hDbgHelp == nullptr )
	{
		return;
	}
	auto pMiniDumpWriteDump = (decltype(&MiniDumpWriteDump))GetProcAddress(hDbgHelp, "MiniDumpWriteDump");
	if ( pMiniDumpWriteDump == nullptr )
	{
		return;
	}

	char name[PATH_MAX];
	{
		strcpy(name, "barony_crash");
		auto nameEnd = name + strlen("barony_crash");
		SYSTEMTIME t;
		GetLocalTime(&t);
		wsprintfA(nameEnd,
			"_%4d%02d%02d_%02d%02d%02d.dmp",
			t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
	}
	std::string crashlogDir = outputdir;
	crashlogDir.append(PHYSFS_getDirSeparator()).append("crashlogs");
	if ( access(crashlogDir.c_str(), F_OK) == -1 )
	{
		// crashlog folder does not exist to write to.
		printlog("Error accessing crashlogs folder, cannot create crash dump file.");
		return;
	}

	// make a new crash folder.
	char newCrashlogFolder[PATH_MAX] = "";
	strncpy(newCrashlogFolder, name, strlen(name) - strlen(".dmp")); // folder name is the dmp file without .dmp
	if ( PHYSFS_setWriteDir(crashlogDir.c_str()) ) // write to the crashlogs/ directory
	{
		if ( PHYSFS_mkdir(newCrashlogFolder) ) // make the folder to hold the .dmp file
		{
			// the full path of the .dmp file to create.
			crashlogDir.append(PHYSFS_getDirSeparator()).append(newCrashlogFolder).append(PHYSFS_getDirSeparator());
		}
		else
		{
			printlog("[PhysFS]: unsuccessfully created %s folder. Error code: %d", newCrashlogFolder, PHYSFS_getLastErrorCode());
			return;
		}
	}
	else
	{
		printlog("[PhysFS]: unsuccessfully mounted base %s folder. Error code: %d", outputdir, PHYSFS_getLastErrorCode());
		return;
	}

	std::string crashDumpFile = crashlogDir + name;

	auto hFile = CreateFileA(crashDumpFile.c_str(), GENERIC_ALL, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if ( hFile == INVALID_HANDLE_VALUE )
	{
		printlog("Error in file handle for %s, cannot create crash dump file.", crashDumpFile.c_str());
		return;
	}

	MINIDUMP_EXCEPTION_INFORMATION exceptionInfo;
	exceptionInfo.ThreadId = GetCurrentThreadId();
	exceptionInfo.ExceptionPointers = e;
	exceptionInfo.ClientPointers = FALSE;

	auto dumped = pMiniDumpWriteDump(
		GetCurrentProcess(),
		GetCurrentProcessId(),
		hFile,
		MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory),
		e ? &exceptionInfo : nullptr,
		nullptr,
		nullptr);

	CloseHandle(hFile);

	printlog("CRITICAL ERROR: Barony has encountered a crash. Submit the crashlog folder in a .zip to the developers: %s", crashlogDir.c_str());
	if ( logfile )
	{
		fclose(logfile);
	}

	// now copy the logfile into the crash folder.
	char logfilePath[PATH_MAX];
	completePath(logfilePath, "log.txt", outputdir);
	std::string crashLogFile = crashlogDir + "log.txt";
	CopyFileA(logfilePath, crashLogFile.c_str(), false);
	return;
}

LONG CALLBACK unhandled_handler(EXCEPTION_POINTERS* e)
{
	make_minidump(e);
	return EXCEPTION_CONTINUE_SEARCH;
}
#endif

ConsoleVariable<bool> cvar_enableKeepAlives("/keepalive_enabled", true);
ConsoleVariable<bool> cvar_animate_tiles("/animate_tiles", true);
ConsoleVariable<bool> cvar_map_sequence_rng("/map_sequence_rng", true);

std::vector<std::string> randomPlayerNamesMale;
std::vector<std::string> randomPlayerNamesFemale;
std::vector<std::string> randomNPCNamesMale;
std::vector<std::string> randomNPCNamesFemale;
std::vector<std::string> physFSFilesInDirectory;
TileEntityListHandler TileEntityList;
// recommended for valgrind debugging:
// res of 480x270
// /nohud
// undefine SOUND, MUSIC (see sound.h)
int game = 1;
Uint32 uniqueGameKey = 0;
Uint32 uniqueLobbyKey = 0;
DebugStatsClass DebugStats;
Uint32 networkTickrate = 0;
bool gameloopFreezeEntities = false;
Uint32 serverSchedulePlayerHealthUpdate = 0;
Uint32 serverLastPlayerHealthUpdate = 0;
Frame* cursorFrame = nullptr;
bool arachnophobia_filter = false;
bool colorblind_lobby = false; // if true, colorblind settings enforced by lobby for shared assets (player colors)

Frame::result_t framesProcResult{
    false,
    0,
    nullptr,
    false
};

#ifdef NDEBUG
Uint32 messagesEnabled = 0xffffffff & ~MESSAGE_DEBUG; // all but debug enabled
#else
Uint32 messagesEnabled = 0xffffffff; // all enabled
#endif

real_t getFPSScale(real_t baseFPS)
{
#ifndef EDITOR
	static ConsoleVariable<bool> cvar_ui_fps_scale_fixed("/ui_fps_scale_fixed", false);
	if ( *cvar_ui_fps_scale_fixed )
	{
		return baseFPS / (std::max(1U, fpsLimit));
	}
	else
	{
		return baseFPS / (std::max(1U, (unsigned int)fps));
	}
#else
	return baseFPS / (std::max(1U, (unsigned int)fps));
#endif
}

//ConsoleVariable<bool> cvar_useTimerInterpolation("/timer_interpolation_enabled", true);
TimerExperiments::time_point TimerExperiments::timepoint{};
TimerExperiments::time_point TimerExperiments::currentTime = Clock::now();
TimerExperiments::duration TimerExperiments::accumulator = std::chrono::milliseconds{ 0 };
std::chrono::duration<long long, std::ratio<1, 60>> TimerExperiments::dt = std::chrono::duration<long long, std::ratio<1, 60>>{ 1 };
TimerExperiments::EntityStates TimerExperiments::cameraPreviousState[MAXPLAYERS];
TimerExperiments::EntityStates TimerExperiments::cameraCurrentState[MAXPLAYERS];
TimerExperiments::EntityStates TimerExperiments::cameraRenderState[MAXPLAYERS];
bool TimerExperiments::bUseTimerInterpolation = true;
bool TimerExperiments::bIsInit = false;
bool TimerExperiments::bDebug = false;
real_t TimerExperiments::lerpFactor = 30.0;
void preciseSleep(double seconds);

void TimerExperiments::integrate(TimerExperiments::State& state,
	std::chrono::time_point<Clock, std::chrono::duration<double>>,
	std::chrono::duration<double> dt)
{
	//state.velocity += state.acceleration * dt / std::chrono::seconds{ 1 };
	state.position += state.velocity * dt / std::chrono::seconds{ 1 };
};

void TimerExperiments::updateEntityInterpolationPosition(Entity* entity)
{
	if ( !TimerExperiments::bUseTimerInterpolation ) { return; }
	if ( !entity ) { return; }

	entity->bUseRenderInterpolation = true;
	if ( entity->behavior == &actHudWeapon
		|| entity->behavior == &actHudShield
		|| entity->behavior == &actHudArm
		|| entity->behavior == &actHudAdditional
		|| entity->behavior == &actHudAdditional2
		|| entity->behavior == &actHudArrowModel
		|| entity->behavior == &actLeftHandMagic
		|| entity->behavior == &actRightHandMagic
		|| entity->behavior == &actCircuit
		|| entity->behavior == &actDoor
		|| entity->behavior == &actIronDoor )
	{
		entity->bUseRenderInterpolation = false;
	}
	if ( !entity->bUseRenderInterpolation )
	{
		return;
	}

	if ( entity->bNeedsRenderPositionInit )
	{
		// wait for entity to position itself in the world by setting useful x/y vlues (monster limbs etc)
		if ( abs(entity->x) > 0.01 || abs(entity->y) > 0.01 ) 
		{
			entity->bNeedsRenderPositionInit = false;

			entity->lerpCurrentState.x.position = entity->x / 16.0;
			entity->lerpCurrentState.y.position = entity->y / 16.0;
			entity->lerpCurrentState.z.position = entity->z;
			entity->lerpCurrentState.pitch.position = entity->pitch;
			entity->lerpCurrentState.yaw.position = entity->yaw;
			entity->lerpCurrentState.roll.position = entity->roll;
			entity->lerpCurrentState.resetMovement();
			entity->lerpPreviousState = entity->lerpCurrentState;
			entity->lerpRenderState = entity->lerpCurrentState;
		}
		return;
	}

	entity->lerpCurrentState.x.velocity = TimerExperiments::lerpFactor * (entity->x / 16.0 - entity->lerpCurrentState.x.position);
	entity->lerpCurrentState.y.velocity = TimerExperiments::lerpFactor * (entity->y / 16.0 - entity->lerpCurrentState.y.position);
	entity->lerpCurrentState.z.velocity = TimerExperiments::lerpFactor * (entity->z - entity->lerpCurrentState.z.position);
	real_t diff = entity->yaw - entity->lerpCurrentState.yaw.position;
	if ( diff >= PI )
	{
		diff -= 2 * PI;
	}
	else if ( diff < -PI )
	{
		diff += 2 * PI;
	}
	entity->lerpCurrentState.yaw.velocity = TimerExperiments::lerpFactor * (diff);

	diff = entity->pitch - entity->lerpCurrentState.pitch.position;
	if ( diff >= PI )
	{
		diff -= 2 * PI;
	}
	else if ( diff < -PI )
	{
		diff += 2 * PI;
	}
	entity->lerpCurrentState.pitch.velocity = TimerExperiments::lerpFactor * (diff);

	diff = entity->roll - entity->lerpCurrentState.roll.position;
	if ( diff >= PI )
	{
		diff -= 2 * PI;
	}
	else if ( diff < -PI )
	{
		diff += 2 * PI;
	}
	entity->lerpCurrentState.roll.velocity = TimerExperiments::lerpFactor * (diff);
}

void TimerExperiments::renderCameras(view_t& camera, int player)
{
	if ( !bUseTimerInterpolation )
	{
		return;
	}

	if ( players[player]->entity )
	{
		if ( players[player]->entity->skill[3] == 2 )
		{
			camera.x = TimerExperiments::cameraRenderState[player].x.position;
			camera.y = TimerExperiments::cameraRenderState[player].y.position;
			camera.ang = TimerExperiments::cameraRenderState[player].yaw.position;
			camera.vang = TimerExperiments::cameraRenderState[player].pitch.position;
			camera.z = TimerExperiments::cameraRenderState[player].z.position; // this uses PLAYER_CAMERAZ_ACCEL, not entity Z
		}
		else if ( !(players[player]->entity->skill[3] != 0) ) // skill[3] is debug cam
		{
			if ( bDebug )
			{
				printTextFormatted(font8x8_bmp, 8, 32, "Timer debug is ON");
				real_t diff = camera.ang - players[player]->entity->lerpRenderState.yaw.position;
				diff = fmod(diff, 2 * PI);
				while ( diff >= PI )
				{
					diff -= 2 * PI;
				}
				while ( diff < -PI )
				{
					diff += 2 * PI;
				}
				real_t curStateYaw = players[player]->entity->lerpCurrentState.yaw.position;
				real_t prevStateYaw = players[player]->entity->lerpPreviousState.yaw.position;
				if ( abs(diff) > PI / 8 )
				{
					messagePlayer(0, MESSAGE_DEBUG, "new: %.4f old: %.4f | current: %.4f | prev: %.4f",
						players[player]->entity->lerpRenderState.yaw.position, camera.ang, curStateYaw, prevStateYaw);
				}
				printTextFormatted(font8x8_bmp, 8, 20, "new: %.4f old: %.4f | current: %.4f | prev: %.4f",
					players[player]->entity->lerpRenderState.yaw.position, camera.ang, curStateYaw, prevStateYaw);
			}
			if ( bDebug && keystatus[SDLK_i] )
			{
				camera.x = players[player]->entity->x / 16.0;
				camera.y = players[player]->entity->y / 16.0;
				camera.ang = players[player]->entity->yaw;
				camera.vang = players[player]->entity->pitch;

				camera.z = TimerExperiments::cameraRenderState[player].z.position; // this uses PLAYER_CAMERAZ_ACCEL, not entity Z
			}
			else
			{
				camera.x = players[player]->entity->lerpRenderState.x.position;
				camera.y = players[player]->entity->lerpRenderState.y.position;
				camera.ang = players[player]->entity->lerpRenderState.yaw.position;
				camera.vang = players[player]->entity->lerpRenderState.pitch.position;

				camera.z = TimerExperiments::cameraRenderState[player].z.position; // this uses PLAYER_CAMERAZ_ACCEL, not entity Z
			}
		}
	}
	else
	{
		camera.x = TimerExperiments::cameraRenderState[player].x.position;
		camera.y = TimerExperiments::cameraRenderState[player].y.position;
		camera.ang = TimerExperiments::cameraRenderState[player].yaw.position;
		camera.vang = TimerExperiments::cameraRenderState[player].pitch.position;
		camera.z = TimerExperiments::cameraRenderState[player].z.position; // this uses PLAYER_CAMERAZ_ACCEL, not entity Z
	}
	return;
	//if ( players[player]->entity )
	//{
	//	// store original x/y by game logic
	//	playerBodypartOffsets[player].entity_ox = players[player]->entity->x;
	//	playerBodypartOffsets[player].entity_oy = players[player]->entity->y;

	//	players[player]->entity->lerp_ox = players[player]->entity->x;
	//	players[player]->entity->lerp_oy = players[player]->entity->y;

	//	// set to interpolated position
	//	players[player]->entity->x = TimerExperiments::cameraRenderState[player].x.position * 16.0;
	//	players[player]->entity->y = TimerExperiments::cameraRenderState[player].y.position * 16.0;

	//	// adjust bodyparts for this interpolation too, ignore static HUD elements
	//	playerBodypartOffsets[player].limb_newx = players[player]->entity->x - playerBodypartOffsets[player].entity_ox;
	//	playerBodypartOffsets[player].limb_newy = players[player]->entity->y - playerBodypartOffsets[player].entity_oy;
	//	for ( Entity *bodypart : players[player]->entity->bodyparts )
	//	{
	//		if ( players[player]->isLocalPlayer() )
	//		{
	//			if ( bodypart->behavior == &actHudAdditional
	//				|| bodypart->behavior == &actHudWeapon
	//				|| bodypart->behavior == &actHudArm
	//				|| bodypart->behavior == &actHudArrowModel
	//				|| bodypart->behavior == &actHudShield
	//				|| bodypart->behavior == &actLeftHandMagic
	//				|| bodypart->behavior == &actRightHandMagic )
	//			{
	//				continue;
	//			}
	//		}
	//		bodypart->x += playerBodypartOffsets[player].limb_newx;
	//		bodypart->y += playerBodypartOffsets[player].limb_newy;
	//	}
	//}
}

void TimerExperiments::postRenderRestore(view_t& camera, int player)
{
	// unused for now?
	if ( !players[player]->entity || !bUseTimerInterpolation )
	{
		reset();
		return;
	}

	players[player]->entity->x = players[player]->entity->lerp_ox;
	players[player]->entity->y = players[player]->entity->lerp_oy;
	return;
	/*players[player]->entity->x = playerBodypartOffsets[player].entity_ox;
	players[player]->entity->y = playerBodypartOffsets[player].entity_oy;
	for ( Entity *bodypart : players[player]->entity->bodyparts )
	{
		if ( players[player]->isLocalPlayer() )
		{
			if ( bodypart->behavior == &actHudAdditional
				|| bodypart->behavior == &actHudWeapon
				|| bodypart->behavior == &actHudArm
				|| bodypart->behavior == &actHudArrowModel
				|| bodypart->behavior == &actHudShield
				|| bodypart->behavior == &actLeftHandMagic
				|| bodypart->behavior == &actRightHandMagic )
			{
				continue;
			}
		}
		bodypart->x -= playerBodypartOffsets[player].limb_newx;
		bodypart->y -= playerBodypartOffsets[player].limb_newy;
	}*/
}

void TimerExperiments::State::resetMovement()
{
	velocity = 0.0;
	acceleration = 0.0;
}

void TimerExperiments::State::resetPosition()
{
	position = 0.0;
}

void TimerExperiments::State::normalize(real_t min, real_t max)
{
    real_t range = std::max(fabs(min), fabs(max));
    position = fmod(position, range);
	while ( position >= max )
	{
		position -= 2 * PI;
	}
	while ( position < min )
	{
		position += 2 * PI;
	}
}

void TimerExperiments::EntityStates::resetMovement()
{
	x.resetMovement();
	y.resetMovement();
	z.resetMovement();
	pitch.resetMovement();
	yaw.resetMovement();
	roll.resetMovement();
}

void TimerExperiments::EntityStates::resetPosition()
{
	x.resetPosition();
	y.resetPosition();
	z.resetPosition();
	pitch.resetPosition();
	yaw.resetPosition();
	roll.resetPosition();
}

void TimerExperiments::reset()
{

}


void TimerExperiments::updateClocks()
{
	if ( !bUseTimerInterpolation )
	{
		bIsInit = false;
		return;
	}

	if ( !bIsInit )
	{
		reset();
		bIsInit = true;
	}

	static ConsoleVariable<bool> cvar_frameLagCheck("/framelagcheck", false);
	if ( *cvar_frameLagCheck )
	{
		static Uint32 doneTick = 0;
		if ( doneTick != ticks && ticks % (2 * TICKS_PER_SECOND) == 0 )
		{
			doneTick = ticks;
			auto microseconds = std::chrono::microseconds((Uint64)(300000));
			preciseSleep(microseconds.count() / 1e6);
		}
	}

	static ConsoleVariable<int> cvar_frameTime("/frametimelimit", 32);
	static ConsoleVariable<bool> cvar_frameTimeAutoSet("/frametimeautolimit", true);
	int frameTimeLimit = (*cvar_frameTime);
	if ( *cvar_frameTimeAutoSet )
	{
		real_t decimal = 0.0;
		real_t ms = 1000 / fpsLimit;
		if ( fps > 0.0 )
		{
			ms = 1000 / fps;
		}
		frameTimeLimit = ms;
		if ( modf(ms, &decimal) > 0.01 )
		{
			frameTimeLimit += 1;
		}
	}

	static ConsoleVariable<float> cvar_cameraLerpFactor("/cameralerp", 30.0);
	lerpFactor = (*cvar_cameraLerpFactor);

	static ConsoleVariable<bool> cvar_lerpAutoAdjust("/autocameralerp", true);

	time_point newTime = Clock::now();
	auto frameTime = newTime - currentTime;
	if ( frameTime > std::chrono::milliseconds{ frameTimeLimit } )
	{
		frameTime = std::chrono::milliseconds{ frameTimeLimit };
	}

	currentTime = newTime;
	accumulator += frameTime;

	if ( (*cvar_lerpAutoAdjust) )
	{
		int frameTimeMillis = std::chrono::duration_cast<Clock::duration>(frameTime).count();
		real_t approxFPS = 1000.0 / frameTimeMillis;
		lerpFactor = 30.0;
		if ( approxFPS < 32.0 )
		{
			lerpFactor = std::max(1, (int)approxFPS - 2);
		}
	}

	std::vector<Entity*> entitiesToInterpolate;
	for ( node_t* node = map.entities->first; node != nullptr; node = node->next )
	{
		Entity* entity = (Entity*)node->element;
		if ( entity->bUseRenderInterpolation )
		{
			entitiesToInterpolate.push_back(entity);
		}
	}

	while ( accumulator >= dt )
	{
		for ( int i = 0; i < MAXPLAYERS; ++i )
		{
			cameraPreviousState[i] = cameraCurrentState[i];
			integrate(cameraCurrentState[i].x, timepoint, dt);
			integrate(cameraCurrentState[i].y, timepoint, dt);
			integrate(cameraCurrentState[i].z, timepoint, dt);
			integrate(cameraCurrentState[i].yaw, timepoint, dt);
			integrate(cameraCurrentState[i].pitch, timepoint, dt);
			integrate(cameraCurrentState[i].roll, timepoint, dt);
		}
		for ( auto& entity : entitiesToInterpolate )
		{
			entity->lerpPreviousState = entity->lerpCurrentState;
			integrate(entity->lerpCurrentState.x, timepoint, dt);
			integrate(entity->lerpCurrentState.y, timepoint, dt);
			integrate(entity->lerpCurrentState.z, timepoint, dt);
			integrate(entity->lerpCurrentState.yaw, timepoint, dt);
			integrate(entity->lerpCurrentState.pitch, timepoint, dt);
			integrate(entity->lerpCurrentState.roll, timepoint, dt);
		}
		timepoint += dt;
		accumulator -= dt;
	}

	double alpha = std::chrono::duration<double>{ accumulator } / dt;
	for ( int i = 0; i < MAXPLAYERS; ++i )
	{
		cameraRenderState[i] = cameraCurrentState[i] * alpha + cameraPreviousState[i] * (1.0 - alpha);
		// make sure these are limited to prevent large jumps
		cameraCurrentState[i].yaw.normalize(0, 2 * PI);
		cameraCurrentState[i].roll.normalize(0, 2 * PI);
		cameraCurrentState[i].pitch.normalize(0, 2 * PI);

		cameraRenderState[i].yaw.position = 
			lerpAngle(cameraPreviousState[i].yaw.position, cameraCurrentState[i].yaw.position, alpha);
		cameraRenderState[i].roll.position = 
			lerpAngle(cameraPreviousState[i].roll.position, cameraCurrentState[i].roll.position, alpha);
		cameraRenderState[i].pitch.position = 
			lerpAngle(cameraPreviousState[i].pitch.position, cameraCurrentState[i].pitch.position, alpha);

		cameraRenderState[i].yaw.normalize(0, 2 * PI);
		cameraRenderState[i].roll.normalize(0, 2 * PI);
		cameraRenderState[i].pitch.normalize(0, 2 * PI);

		// original angle lerp code
		//real_t a1 = cameraPreviousState[i].yaw.position;
		//real_t a2 = cameraCurrentState[i].yaw.position;
		//real_t adiff = a2 - a1;
		//cameraRenderState[i].yaw.position = a1 + alpha * (fmod(3 * PI + fmod(adiff, 2 * PI), 2 * PI) - PI);
	}
	for ( auto& entity : entitiesToInterpolate )
	{
		entity->lerpRenderState = entity->lerpCurrentState * alpha + entity->lerpPreviousState * (1.0 - alpha);
		// make sure these are limited to prevent large jumps
		entity->lerpCurrentState.yaw.normalize(0, 2 * PI);
		entity->lerpCurrentState.roll.normalize(0, 2 * PI);
		entity->lerpCurrentState.pitch.normalize(0, 2 * PI);

		entity->lerpRenderState.yaw.position = 
			lerpAngle(entity->lerpPreviousState.yaw.position, entity->lerpCurrentState.yaw.position, alpha);
		entity->lerpRenderState.roll.position = 
			lerpAngle(entity->lerpPreviousState.roll.position, entity->lerpCurrentState.roll.position, alpha);
		entity->lerpRenderState.pitch.position = 
			lerpAngle(entity->lerpPreviousState.pitch.position, entity->lerpCurrentState.pitch.position, alpha);

		entity->lerpRenderState.yaw.normalize(0, 2 * PI);
		entity->lerpRenderState.roll.normalize(0, 2 * PI);
		entity->lerpRenderState.pitch.normalize(0, 2 * PI);
	}
}

real_t TimerExperiments::lerpAngle(real_t angle1, real_t angle2, real_t alpha)
{
	real_t adiff = angle2 - angle1;
	return angle1 + alpha * (fmod(3 * PI + fmod(adiff, 2 * PI), 2 * PI) - PI);
}

std::string TimerExperiments::render(State state)
{
	using namespace std::chrono;
	static auto t = time_point_cast<seconds>(Clock::now());
	static int frame_count = 0;
	static int frame_rate = 0;
	auto pt = t;
	t = time_point_cast<seconds>(Clock::now());
	++frame_count;
	if ( t != pt )
	{
		frame_rate = frame_count;
		frame_count = 0;
	}

	char output[256] = "";
	snprintf(output, sizeof(output), "Frame rate is %d frames per second. Position = %.4f", frame_rate, state.position);
	/*if ( abs(state.velocity) > 0.01 )
	{
		printlog("FPS: %d | Velocity: %.4f | Position: %.4f", frame_rate, state.velocity, state.position);
	}*/
	return output;
}

enum DemoMode {
    STOPPED,
    RECORDING,
    PLAYING
};
static DemoMode demo_mode = DemoMode::STOPPED;
static File* demo_file = nullptr;

static void demo_stop() {
    if (demo_mode == DemoMode::STOPPED) {
        messagePlayer(clientnum, MESSAGE_MISC, "Demo is not running");
        return;
    }
    switch (demo_mode) {
    case DemoMode::PLAYING:
        if (demo_file->eof()) {
            messagePlayer(clientnum, MESSAGE_MISC, "End of demo");
        } else {
            messagePlayer(clientnum, MESSAGE_MISC, "Stopped demo playback");
        }
        break;
    case DemoMode::RECORDING:
        messagePlayer(clientnum, MESSAGE_MISC, "Stopped demo recording");
        break;
    default:
        messagePlayer(clientnum, MESSAGE_MISC, "Stopped demo");
        break;
    }
    if (demo_file) {
        FileIO::close(demo_file);
        demo_file = nullptr;
    }
    demo_mode = DemoMode::STOPPED;

    TimerExperiments::bUseTimerInterpolation = true;
}

static void demo_record(const char* filename) {
    if (demo_mode != DemoMode::STOPPED) {
        messagePlayer(clientnum, MESSAGE_MISC, "Demo must be stopped first (/demo_stop)");
        return;
    }

    if (multiplayer != SINGLE || splitscreen) {
        messagePlayer(clientnum, MESSAGE_MISC, "Demos not permitted in multiplayer");
        return;
    }

    // create demo file for recording
    char path[PATH_MAX];
    completePath(path, filename, outputdir);
    demo_file = FileIO::open(path, "wb");
    if (!demo_file) {
        messagePlayer(clientnum, MESSAGE_MISC, "failed to open demo file '%s'", path);
        return;
    }

    TimerExperiments::bUseTimerInterpolation = false; // this causes mass desyncs

    // seed RNG
    local_rng.seedTime();
    local_rng.getSeed(&uniqueGameKey, sizeof(uniqueGameKey));
    net_rng.seedBytes(&uniqueGameKey, sizeof(uniqueGameKey));
    demo_file->write(&uniqueGameKey, sizeof(uniqueGameKey), 1);

    // write player stats
    demo_file->write(&stats[clientnum]->playerRace, sizeof(Stat::playerRace), 1);
    demo_file->write(&stats[clientnum]->sex, sizeof(Stat::sex), 1);
    demo_file->write(&stats[clientnum]->stat_appearance, sizeof(Stat::stat_appearance), 1);
    demo_file->write(&client_classes[clientnum], sizeof(client_classes[clientnum]), 1);

    // write player name
    Uint32 name_len = (Uint32)strlen(stats[clientnum]->name);
    demo_file->write(&name_len, sizeof(name_len), 1);
    demo_file->write(stats[clientnum]->name, sizeof(char), name_len);

    // reset player
    stats[clientnum]->clearStats();
	initClass(clientnum);

    // start game
    doNewGame(false);

    messagePlayer(clientnum, MESSAGE_MISC, "Recording demo to '%s'", path);
    demo_mode = DemoMode::RECORDING;
}

static void demo_play(const char* filename) {
    if (demo_mode != DemoMode::STOPPED) {
        messagePlayer(clientnum, MESSAGE_MISC, "Demo must be stopped first (/demo_stop)");
        return;
    }

    if (multiplayer != SINGLE || splitscreen) {
        messagePlayer(clientnum, MESSAGE_MISC, "Demos not permitted in multiplayer");
        return;
    }

    // open demo file for playing
    char path[PATH_MAX];
    completePath(path, filename, outputdir);
    demo_file = FileIO::open(path, "rb");
    if (!demo_file) {
        messagePlayer(clientnum, MESSAGE_MISC, "failed to open demo file '%s'", path);
        return;
    }

    TimerExperiments::bUseTimerInterpolation = false; // this causes mass desyncs

    // seed RNG
    demo_file->read(&uniqueGameKey, sizeof(uniqueGameKey), 1);
    local_rng.seedBytes(&uniqueGameKey, sizeof(uniqueGameKey));
    net_rng.seedBytes(&uniqueGameKey, sizeof(uniqueGameKey));

    // read player stats
    demo_file->read(&stats[clientnum]->playerRace, sizeof(Stat::playerRace), 1);
    demo_file->read(&stats[clientnum]->sex, sizeof(Stat::sex), 1);
    demo_file->read(&stats[clientnum]->stat_appearance, sizeof(Stat::stat_appearance), 1);
    demo_file->read(&client_classes[clientnum], sizeof(client_classes[clientnum]), 1);

    // read player name
    Uint32 name_len;
    demo_file->read(&name_len, sizeof(name_len), 1);
    demo_file->read(stats[clientnum]->name, sizeof(char), name_len);
    stats[clientnum]->name[name_len] = '\0';

    // reset player
    stats[clientnum]->clearStats();
	initClass(clientnum);

    // start game
    doNewGame(false);

    messagePlayer(clientnum, MESSAGE_MISC, "Playing demo in '%s'", path);
    demo_mode = DemoMode::PLAYING;
}

static ConsoleCommand ccmd_demo_stop("/demo_stop", "stop recording or playing a demo",
    [](int argc, const char* argv[]){
    demo_stop();
    });

static ConsoleCommand ccmd_demo_record("/demo_record", "record a demo to a file (default demo.dat)",
    [](int argc, const char* argv[]){
    if (argc < 2) {
	    demo_record("demo.dat");
    } else {
	    demo_record(argv[1]);
    }
    });

static ConsoleCommand ccmd_demo_play("/demo_play", "play a recorded demo(default demo.dat)",
    [](int argc, const char* argv[]){
    if (argc < 2) {
	    demo_play("demo.dat");
    } else {
	    demo_play(argv[1]);
    }
    });

static bool automatiaEntityWasDeleted(Entity* entity)
{
	for ( node_t* node = entitiesdeleted.first; node; node = node->next )
	{
		if ( node->element == entity )
		{
			return true;
		}
	}
	return false;
}

static void simulateActiveAutomatiaInstanceEntities()
{
	auto runList = [](list_t* entities, bool updateTileIndex)
	{
		if ( !entities )
		{
			return;
		}
		for ( node_t* node = entities->first, *nextnode = nullptr;
			node != nullptr;
			node = nextnode )
		{
			nextnode = node->next;
			Entity* entity = static_cast<Entity*>(node->element);
			if ( !entity || entity->ranbehavior )
			{
				continue;
			}
			if ( !gamePaused )
			{
				++entity->ticks;
			}
			if ( !entity->behavior || gamePaused )
			{
				continue;
			}
			if ( gameloopFreezeEntities
				&& entity->behavior != &actPlayer
				&& entity->behavior != &actPlayerLimb
				&& entity->behavior != &actDeathGhost
				&& entity->behavior != &actFlame )
			{
				continue;
			}

			const int oldTileX = static_cast<int>(entity->x) >> 4;
			const int oldTileY = static_cast<int>(entity->y) >> 4;
			if ( updateTileIndex && !entity->myTileListNode )
			{
				TileEntityList.addEntity(*entity);
			}
			(*entity->behavior)(entity);
			const bool deleted = automatiaEntityWasDeleted(entity);
			if ( !deleted )
			{
				if ( updateTileIndex
					&& (oldTileX != static_cast<int>(entity->x) >> 4
						|| oldTileY != static_cast<int>(entity->y) >> 4) )
				{
					TileEntityList.updateEntity(*entity);
				}
				TimerExperiments::updateEntityInterpolationPosition(entity);
				entity->ranbehavior = true;
			}
			if ( entitiesdeleted.first )
			{
				nextnode = entities->first;
				list_FreeAll(&entitiesdeleted);
			}
		}
		for ( node_t* node = entities->first; node; node = node->next )
		{
			Entity* entity = static_cast<Entity*>(node->element);
			if ( entity )
			{
				entity->ranbehavior = false;
			}
		}
	};

	runList(map.worldUI, false);
	runList(map.entities, true);
	if ( MapInstance* instance = worldState.activeInstance() )
	{
		++instance->simulationTick;
	}
}

static void serverBroadcastActiveAutomatiaEntityUpdates()
{
	if ( multiplayer != SERVER
		|| ticks % (TICKS_PER_SECOND / 8) != 0
		|| !map.entities )
	{
		return;
	}
	for ( node_t* node = map.entities->first; node; node = node->next )
	{
		Entity* entity = static_cast<Entity*>(node->element);
		if ( !entity
			|| !entity->flags[UPDATENEEDED]
			|| entity->flags[NOUPDATE] )
		{
			continue;
		}
		const bool guarantee = entity->getUID() % (TICKS_PER_SECOND * 4)
			== ticks % (TICKS_PER_SECOND * 4);
		for ( int player = 1; player < MAXPLAYERS; ++player )
		{
			if ( !client_disconnected[player] )
			{
				sendEntityUDP(entity, player, guarantee);
			}
		}
	}
}

static void simulateOccupiedAutomatiaInstances()
{
	if ( multiplayer != SERVER || gamePaused || entitiesdeleted.first )
	{
		return;
	}
	const WorldInstanceIdentity* activeIdentity =
		worldState.activeIdentity();
	if ( !activeIdentity )
	{
		return;
	}
	const std::string foregroundKey = activeIdentity->key();
	const std::vector<std::string> occupiedKeys =
		worldState.occupiedLoadedInstanceKeys();
	for ( const std::string& key : occupiedKeys )
	{
		if ( key == foregroundKey )
		{
			continue;
		}
		MapInstance* instance = worldState.find(key);
		if ( !instance || !instance->runtimeInitialized )
		{
			continue;
		}
		if ( !worldState.activate(key) )
		{
			printlog("[World State] Unable to schedule occupied instance '%s'.", key.c_str());
			continue;
		}
		loadnextlevel = false;
		simulateActiveAutomatiaInstanceEntities();
		serverBroadcastActiveAutomatiaEntityUpdates();
		if ( loadnextlevel )
		{
			printlog(
				"[World State] Deferred a legacy whole-party exit requested in background instance '%s'.",
				key.c_str()
			);
			loadnextlevel = false;
		}
	}
	if ( worldState.activeIdentity()
		&& worldState.activeIdentity()->key() != foregroundKey
		&& !worldState.activate(foregroundKey) )
	{
		printlog("[World State] Unable to restore foreground instance '%s'.", foregroundKey.c_str());
	}
}

/*-------------------------------------------------------------------------------

	gameLogic

	Updates the gamestate; moves actors, primarily

-------------------------------------------------------------------------------*/

ConsoleVariable<bool> framesEatMouse("/gui_eat_mouseclicks", true);
static ConsoleVariable<bool> cvar_lava_use_vismap("/lava_use_vismap", true);
#ifdef NINTENDO
static ConsoleVariable<bool> cvar_lava_bubbles_enabled("/lava_bubbles_enabled", false);
#else
static ConsoleVariable<bool> cvar_lava_bubbles_enabled("/lava_bubbles_enabled", true);
#endif

static real_t drunkextend[MAXPLAYERS] = { (real_t)0.0 };

void gameLogic(void)
{
	Uint32 x;
	node_t* node, *nextnode, *node2;
	Entity* entity;
	int c = 0;
	Uint32 i = 0, j;
	bool entitydeletedself;

#ifdef NINTENDO
	(void)nxUpdateCrashMessage();
#endif

    if (!gamePaused && !loading) {
        if (demo_file) {
            // demo recording
            if (demo_mode == DemoMode::RECORDING) {
                demo_file->write(&Input::inputs[clientnum].keys, sizeof(Input::keys), 1);
                demo_file->write(&Input::inputs[clientnum].mouseButtons, sizeof(Input::mouseButtons), 1);
                demo_file->write(&keystatus, sizeof(keystatus), 1);
                demo_file->write(&mousex, sizeof(mousex), 1);
                demo_file->write(&mousey, sizeof(mousey), 1);
                demo_file->write(&mousestatus, sizeof(mousestatus), 1);
                demo_file->write(&mousexrel, sizeof(mousexrel), 1);
                demo_file->write(&mouseyrel, sizeof(mouseyrel), 1);
            }

            // demo playback
            if (demo_mode == DemoMode::PLAYING) {
                demo_file->read(&Input::inputs[clientnum].keys, sizeof(Input::keys), 1);
                demo_file->read(&Input::inputs[clientnum].mouseButtons, sizeof(Input::mouseButtons), 1);
                demo_file->read(&keystatus, sizeof(keystatus), 1);
                demo_file->read(&mousex, sizeof(mousex), 1);
                demo_file->read(&mousey, sizeof(mousey), 1);
                demo_file->read(&mousestatus, sizeof(mousestatus), 1);
                demo_file->read(&mousexrel, sizeof(mousexrel), 1);
                demo_file->read(&mouseyrel, sizeof(mouseyrel), 1);
                if (demo_file->eof()) {
                    demo_stop();
                }
            }
        }
    }

	for (auto& input : Input::inputs) {
		input.update();
		input.consumeBindingsSharedWithFaceHotbar();
	}

	int auto_appraise_lowest_time[MAXPLAYERS];
	Item* auto_appraise_target[MAXPLAYERS];
	for ( int i = 0; i < MAXPLAYERS; ++i )
	{
		auto_appraise_target[i] = nullptr;
		auto_appraise_lowest_time[i] = std::numeric_limits<int>::max();
	}

	if ( creditstage > 0 )
	{
		credittime++;
	}
	if ( intromoviestage > 0 )
	{
		intromovietime++;
	}
	if ( firstendmoviestage > 0 )
	{
		firstendmovietime++;
	}
	if ( secondendmoviestage > 0 )
	{
		secondendmovietime++;
	}
	if ( thirdendmoviestage > 0 )
	{
		thirdendmovietime++;
	}
	if ( fourthendmoviestage > 0 )
	{
		fourthendmovietime++;
	}
	for ( int i = 0; i < 8; ++i )
	{
		if ( DLCendmovieStageAndTime[i][MOVIE_STAGE] > 0 )
		{
			DLCendmovieStageAndTime[i][MOVIE_TIME]++;
		}
	}

	DebugStats.eventsT1 = std::chrono::high_resolution_clock::now();

	// camera shaking
	for (int c = 0; c < MAXPLAYERS; ++c) 
	{
		if ( !splitscreen && c != clientnum )
		{
			continue;
		}

		auto& camera_shakex = cameravars[c].shakex;
		auto& camera_shakey = cameravars[c].shakey;
		auto& camera_shakex2 = cameravars[c].shakex2;
		auto& camera_shakey2 = cameravars[c].shakey2;
		if ( shaking )
		{
			static ConsoleVariable<int> cvar_shake_max("/shake_max", 15);
			camera_shakex = std::min(camera_shakex, *cvar_shake_max / 100.0);
			camera_shakey = std::min(camera_shakey, *cvar_shake_max);

			camera_shakex2 = (camera_shakex2 + camera_shakex) * .8;
			camera_shakey2 = (camera_shakey2 + camera_shakey) * .9;
			if ( camera_shakex2 > 0 )
			{
				if ( camera_shakex2 < .02 && camera_shakex >= -.01 )
				{
					camera_shakex2 = 0;
					camera_shakex = 0;
				}
				else
				{
					camera_shakex -= .01;
				}
			}
			else if ( camera_shakex2 < 0 )
			{
				if ( camera_shakex2 > -.02 && camera_shakex <= .01 )
				{
					camera_shakex2 = 0;
					camera_shakex = 0;
				}
				else
				{
					camera_shakex += .01;
				}
			}
			if ( camera_shakey2 > 0 )
			{
				camera_shakey -= 1;
			}
			else if ( camera_shakey2 < 0 )
			{
				camera_shakey += 1;
			}
		}
		else
		{
			camera_shakex = 0;
			camera_shakey = 0;
			camera_shakex2 = 0;
			camera_shakey2 = 0;
		}
	}

	// drunkenness
	if ( !intro )
	{
	    for (int c = 0; c < MAXPLAYERS; ++c)
	    {
			players[c]->hud.followerDisplay.bCommandNPCDisabled = false;
			players[c]->hud.followerDisplay.bOpenFollowerMenuDisabled = false;
			players[c]->hud.bOpenCalloutsMenuDisabled = false;

	        if (c != clientnum && !splitscreen)
	        {
	            continue;
	        }
	        if ( stats[c]->getEffectActive(EFF_DRUNK) )
		    {
			    // goat/drunkards no spin!
			    if ( stats[c]->type == GOATMAN )
			    {
				    // return to normal.
				    if ( drunkextend[c] > 0 )
				    {
					    drunkextend[c] -= .005;
					    if ( drunkextend[c] < 0 )
					    {
						    drunkextend[c] = 0;
					    }
				    }
			    }
			    else
			    {
				    if ( drunkextend[c] < 0.5 )
				    {
					    drunkextend[c] += .005;
					    if ( drunkextend[c] > 0.5 )
					    {
						    drunkextend[c] = 0.5;
					    }
				    }
			    }
		    }
		    else
		    {
			    if ( stats[c]->getEffectActive(EFF_WITHDRAWAL) || stats[c]->getEffectActive(EFF_DISORIENTED) )
			    {
				    // special widthdrawal shakes
				    if ( drunkextend[c] < 0.2 )
				    {
					    drunkextend[c] += .005;
					    if ( drunkextend[c] > 0.2 )
					    {
						    drunkextend[c] = 0.2;
					    }
				    }
			    }
			    else
			    {
				    // return to normal.
				    if ( drunkextend[c] > 0 )
				    {
					    drunkextend[c] -= .005;
					    if ( drunkextend[c] < 0 )
					    {
						    drunkextend[c] = 0;
					    }
				    }
			    }
		    }
	    }
	}

	// fading in/out
	if ( fadeout == true )
	{
		fadealpha = std::min(fadealpha + 10, 255);
		if ( fadealpha == 255 )
		{
			fadefinished = true;
		}
	}
	else
	{
		fadealpha = std::max(0, fadealpha - 10);
	}

	// handle safe packets
	if ( !(ticks % 4) )
	{
		j = 0;
		for ( node = safePacketsSent.first; node != nullptr; node = nextnode )
		{
			nextnode = node->next;

			packetsend_t* packet = (packetsend_t*)node->element;
			//printlog("Packet resend: %d", packet->hostnum);
			resendPacketSafe(packet);
			packet->tries++;
			if ( packet->tries >= MAXTRIES )
			{
				list_RemoveNode(node);
			}
			j++;
			if ( j >= MAXDELETES )
			{
				break;
			}
		}
	}

	// spawn flame particles on burning objects
	if ( !gamePaused || (multiplayer && !client_disconnected[0]) )
	{
		for ( node = map.entities->first; node != nullptr; node = node->next )
		{
			entity = (Entity*)node->element;
			if ( entity->flags[BURNING] )
			{
				if ( !entity->flags[BURNABLE] )
				{
					entity->flags[BURNING] = false;
					continue;
				}
	            /*if ( flickerLights || entity->ticks % TICKS_PER_SECOND == 1 )
	            {
				    j = 1 + local_rng.rand() % 4;
				    for ( c = 0; c < j; ++c )
				    {
						if ( Entity* flame = spawnFlame(entity, SPRITE_FLAME) )
						{
							flame->x += local_rng.rand() % (entity->sizex * 2 + 1) - entity->sizex;
							flame->y += local_rng.rand() % (entity->sizey * 2 + 1) - entity->sizey;
							flame->z += local_rng.rand() % 5 - 2;
							if ( entity->behavior == &actBell )
							{
								flame->x += entity->focalx * cos(entity->yaw) + entity->focaly * cos(entity->yaw + PI / 2);
								flame->y += entity->focalx * sin(entity->yaw) + entity->focaly * sin(entity->yaw + PI / 2);
							}
						}
				    }
				}*/
				if ( entity->ticks % 10 == 0 )
				{
					if ( Entity* fx = spawnFlameSprites(entity, 233) )
					{
					}
				}
			}
		}
	}

	// damage indicator timers
	handleDamageIndicatorTicks();
#ifdef STEAMWORKS
	MainMenu::richPresence.process();
#endif

	if ( intro == true )
	{
        if (gearsize == 0) {
            // initialize
            gearsize = 40000 * (xres / 1280.f);
        }
        
		// rotate gear
		gearrot += 1;
		if ( gearrot >= 360 )
		{
			gearrot -= 360;
		}
		gearsize -= std::max<double>(2, gearsize / 20);
        const float smallest_size = 70 * (xres / 1280.f);
		if ( gearsize < smallest_size )
		{
			gearsize = smallest_size;
			logoalpha += 2;
		}

		// execute entity behaviors
		c = multiplayer;
		x = clientnum;
		multiplayer = SINGLE;
		clientnum = 0;
		for ( node = map.entities->first; node != nullptr; node = nextnode )
		{
			nextnode = node->next;
			entity = (Entity*)node->element;
			if ( entity && !entity->ranbehavior )
			{
				entity->ticks++;
				if ( entity->behavior != nullptr )
				{
					(*entity->behavior)(entity);
					if ( entitiesdeleted.first != nullptr )
					{
						entitydeletedself = false;
						for ( node2 = entitiesdeleted.first; node2 != nullptr; node2 = node2->next )
						{
							if ( entity == (Entity*)node2->element )
							{
								entitydeletedself = true;
								break;
							}
						}
						if ( entitydeletedself == false )
						{
							entity->ranbehavior = true;
						}
						nextnode = map.entities->first;
						list_FreeAll(&entitiesdeleted);
					}
					else
					{
						entity->ranbehavior = true;
						nextnode = node->next;
					}
				}
			}
		}
		for ( node = map.entities->first; node != nullptr; node = node->next )
		{
			entity = (Entity*)node->element;
			entity->ranbehavior = false;
		}
		multiplayer = c;
		clientnum = x;
	}
	else
	{
		static ConsoleVariable<bool> cvar_appraisal_auto_switch("/appraisal_auto_switch", true);
		DebugStats.eventsT2 = std::chrono::high_resolution_clock::now();
		if ( multiplayer != CLIENT )   // server/singleplayer code
		{
			for ( c = 0; c < MAXPLAYERS; c++ )
			{
				if ( assailantTimer[c] > 0 )
				{
					--assailantTimer[c];
					//messagePlayer(0, "music cd: %d", assailantTimer[c]);
				}
				if ( assailant[c] == true && assailantTimer[c] <= 0 )
				{
					assailant[c] = false;
					assailantTimer[c] = 0;
				}
			}

			if ( !directConnect && LobbyHandler.getHostingType() == LobbyHandler_t::LobbyServiceType::LOBBY_CROSSPLAY )
			{
#ifdef USE_EOS
				if ( multiplayer == SERVER && ticks % TICKS_PER_SECOND == 0 )
				{
					EOS.CurrentLobbyData.updateLobbyDuringGameLoop();
				}
#endif // USE_EOS
			}


			// animate tiles
			if ( !gamePaused )
			{
				int x, y, z;
				for ( x = 0; x < map.width; x++ )
				{
					for ( y = 0; y < map.height; y++ )
					{
						for ( z = 0; z < MAPLAYERS; z++ )
						{
							int index = z + y * MAPLAYERS + x * MAPLAYERS * map.height;
							if ( animatedtiles[map.tiles[index]] )
							{
								if ( z == 0 )
								{
									// water and lava noises
									if ( ticks % (TICKS_PER_SECOND * 4) == (y + x * map.height) % (TICKS_PER_SECOND * 4) && local_rng.rand() % 3 == 0 )
									{
										int coord = x + y * 1000;
										if ( map.liquidSfxPlayedTiles.find(coord) == map.liquidSfxPlayedTiles.end() )
										{
											if ( lavatiles[map.tiles[index]] )
											{
												// bubbling lava
												playSoundPosLocal( x * 16 + 8, y * 16 + 8, 155, 100 );
											}
											else if ( swimmingtiles[map.tiles[index]] )
											{
												// running water
												playSoundPosLocal( x * 16 + 8, y * 16 + 8, 135, 32 );
											}
											map.liquidSfxPlayedTiles.insert(coord);
										}
									}

									// lava bubbles
									if ( lavatiles[map.tiles[index]] && !gameloopFreezeEntities )
									{
										if ( ticks % 40 == (y + x * map.height) % 40 && local_rng.rand() % 3 == 0 )
										{
											bool doLavaParticles = *cvar_lava_bubbles_enabled;
											if ( doLavaParticles )
											{
												if ( *cvar_lava_use_vismap && !intro )
												{
													if ( x >= 0 && x < map.width && y >= 0 && y < map.height )
													{
														bool anyVismap = false;
														for ( int i = 0; i < MAXPLAYERS; ++i )
														{
															if ( !client_disconnected[i] && players[i]->isLocalPlayer() && cameras[i].vismap[y + x * map.height] )
															{
																anyVismap = true;
																break;
															}
														}
														if ( !anyVismap )
														{
															doLavaParticles = false;
														}
													}
												}
												int c, j = 1 + local_rng.rand() % 2;
												for ( c = 0; c < j && doLavaParticles; ++c )
												{
													Entity* entity = newEntity(42, 1, map.entities, nullptr); //Gib entity.
													entity->behavior = &actGib;
													entity->x = x * 16 + local_rng.rand() % 16;
													entity->y = y * 16 + local_rng.rand() % 16;
													entity->z = 7.5;
                                                    entity->ditheringDisabled = true;
													entity->flags[PASSABLE] = true;
													entity->flags[SPRITE] = true;
													entity->flags[NOUPDATE] = true;
													entity->flags[UPDATENEEDED] = false;
													entity->flags[UNCLICKABLE] = true;
													entity->sizex = 2;
													entity->sizey = 2;
													entity->fskill[3] = 0.01;
													double vel = (local_rng.rand() % 10) / 20.f;
													entity->vel_x = vel * cos(entity->yaw);
													entity->vel_y = vel * sin(entity->yaw);
													entity->vel_z = -.15 - (local_rng.rand() % 15) / 100.f;
													entity->yaw = (local_rng.rand() % 360) * PI / 180.0;
													entity->pitch = (local_rng.rand() % 360) * PI / 180.0;
													entity->roll = (local_rng.rand() % 360) * PI / 180.0;
													if ( multiplayer != CLIENT )
													{
														--entity_uids;
													}
													entity->setUID(-3);
												}
											}
										}
									}
								}
							}
						}
					}
				}
			}

			// periodic steam achievement check
			if ( ticks % TICKS_PER_SECOND == 0 )
			{
				for ( c = 0; c < MAXPLAYERS; c++ )
				{
					if ( client_disconnected[c] )
					{
						continue;
					}
					int followerCount = list_Size(&stats[c]->FOLLOWERS);
					if ( followerCount >= 3 )
					{
						steamAchievementClient(c, "BARONY_ACH_NATURAL_BORN_LEADER");
					}
					if ( stats[c]->GOLD >= 10000 )
					{
						steamAchievementClient(c, "BARONY_ACH_FILTHY_RICH");
					}
					if ( stats[c]->GOLD >= 100000 )
					{
						steamAchievementClient(c, "BARONY_ACH_GILDED");
					}

					{
						int numsongs = 0;
						if ( ((int)stats[c]->getEffectActive(EFF_ENSEMBLE_FLUTE) - 1) >= Stat::kEnsembleBreakPointTier4 )
						{
							++numsongs;
						}
						if ( ((int)stats[c]->getEffectActive(EFF_ENSEMBLE_LUTE) - 1) >= Stat::kEnsembleBreakPointTier4 )
						{
							++numsongs;
						}
						if ( ((int)stats[c]->getEffectActive(EFF_ENSEMBLE_LYRE) - 1) >= Stat::kEnsembleBreakPointTier4 )
						{
							++numsongs;
						}
						if ( ((int)stats[c]->getEffectActive(EFF_ENSEMBLE_HORN) - 1) >= Stat::kEnsembleBreakPointTier4 )
						{
							++numsongs;
						}
						if ( ((int)stats[c]->getEffectActive(EFF_ENSEMBLE_DRUM) - 1) >= Stat::kEnsembleBreakPointTier4 )
						{
							++numsongs;
						}
						if ( numsongs >= 4 )
						{
							steamAchievementClient(c, "BARONY_ACH_POWER_BALLAD");
						}
					}

					if ( stats[c]->helmet && stats[c]->helmet->type == HAT_WOLF_HOOD
						&& stats[c]->helmet->beatitude > 0 )
					{
						steamAchievementClient(c, "BARONY_ACH_PET_DA_DOG");
					}

					if ( stats[c]->helmet && stats[c]->helmet->type == ARTIFACT_HELM
						&& stats[c]->breastplate && stats[c]->breastplate->type == ARTIFACT_BREASTPIECE
						&& stats[c]->gloves && stats[c]->gloves->type == ARTIFACT_GLOVES
						&& stats[c]->cloak && stats[c]->cloak->type == ARTIFACT_CLOAK
						&& stats[c]->shoes && stats[c]->shoes->type == ARTIFACT_BOOTS )
					{
						steamAchievementClient(c, "BARONY_ACH_GIFTS_ETERNALS");
					}

					if ( stats[c]->type == SKELETON 
						&& stats[c]->weapon && stats[c]->weapon->type == ARTIFACT_AXE
						&& stats[c]->cloak && stats[c]->cloak->type == CLOAK_PROTECTION
						&& ((stats[c]->mask && stats[c]->mask->type == MASK_EYEPATCH) || !stats[c]->mask)
						&& !stats[c]->helmet
						&& !stats[c]->gloves && !stats[c]->shoes
						&& !stats[c]->breastplate && !stats[c]->ring
						&& !stats[c]->amulet && !stats[c]->shield )
					{
						// nothing but an axe and a cloak.
						steamAchievementClient(c, "BARONY_ACH_COMEDIAN");
					}

					if ( stats[c]->getEffectActive(EFF_SHRINE_RED_BUFF)
						&& stats[c]->getEffectActive(EFF_SHRINE_GREEN_BUFF)
						&& stats[c]->getEffectActive(EFF_SHRINE_BLUE_BUFF) )
					{
						steamAchievementClient(c, "BARONY_ACH_WELL_PREPARED");
					}

					/*if ( achievementStatusRhythmOfTheKnight[c] )
					{
						steamAchievementClient(c, "BARONY_ACH_RHYTHM_OF_THE_KNIGHT");
					}*/
					if ( achievementStatusThankTheTank[c] )
					{
						steamAchievementClient(c, "BARONY_ACH_THANK_THE_TANK");
					}

					int bodyguards = 0;
					int squadGhouls = 0;
					int badRomance = 0;
					int familyReunion = 0;
					int machineHead = 0;

					if ( followerCount >= 4 )
					{
						achievementObserver.playerAchievements[c].caughtInAMoshTargets.clear();
					}
					for ( node = stats[c]->FOLLOWERS.first; node != nullptr; node = node->next )
					{
						Entity* follower = nullptr;
						if ( (Uint32*)node->element )
						{
							follower = uidToEntity(*((Uint32*)node->element));
						}
						if ( follower )
						{
							Stat* followerStats = follower->getStats();
							if ( followerStats )
							{
								if ( followerStats->type == CRYSTALGOLEM )
								{
									++bodyguards;
								}
								if ( followerStats->type == GHOUL && stats[c]->type == SKELETON )
								{
									++squadGhouls;
								}
								if ( stats[c]->type == SUCCUBUS && (followerStats->type == SUCCUBUS || followerStats->type == INCUBUS) )
								{
									++badRomance;
								}
								if ( stats[c]->type == GOATMAN && followerStats->type == GOATMAN )
								{
									++familyReunion;
								}
								if ( followerStats->type == GYROBOT || followerStats->type == SENTRYBOT || followerStats->type == SPELLBOT )
								{
									machineHead |= (followerStats->type == GYROBOT);
									machineHead |= (followerStats->type == SENTRYBOT) << 1;
									machineHead |= (followerStats->type == SPELLBOT) << 2;

									if ( followerCount >= 4 && !(achievementObserver.playerAchievements[c].caughtInAMosh) )
									{
										if ( follower->monsterTarget != 0 
											&& (follower->monsterState == MONSTER_STATE_ATTACK || follower->monsterState == MONSTER_STATE_HUNT) &&
											(followerStats->type == SENTRYBOT || followerStats->type == SPELLBOT) )
										{
											auto it = achievementObserver.playerAchievements[c].caughtInAMoshTargets.find(follower->monsterTarget);
											if ( it != achievementObserver.playerAchievements[c].caughtInAMoshTargets.end() )
											{
												// key exists.
												achievementObserver.playerAchievements[c].caughtInAMoshTargets[follower->monsterTarget] += 1; // increase value
												if ( achievementObserver.playerAchievements[c].caughtInAMoshTargets[follower->monsterTarget] >= 4 )
												{
													achievementObserver.awardAchievement(c, AchievementObserver::BARONY_ACH_CAUGHT_IN_A_MOSH);
													achievementObserver.playerAchievements[c].caughtInAMosh = true;
												}
											}
											else
											{
												achievementObserver.playerAchievements[c].caughtInAMoshTargets.insert(std::make_pair(static_cast<Uint32>(follower->monsterTarget), 1));
											}
										}
									}
								}
							}
						}
					}
					if ( bodyguards >= 2 )
					{
						steamAchievementClient(c, "BARONY_ACH_BODYGUARDS");
					}
					if ( squadGhouls >= 4 )
					{
						steamAchievementClient(c, "BARONY_ACH_SQUAD_GHOULS");
					}
					if ( badRomance >= 2 )
					{
						steamAchievementClient(c, "BARONY_ACH_BAD_ROMANCE");
					}
					if ( familyReunion >= 3 )
					{
						steamAchievementClient(c, "BARONY_ACH_FAMILY_REUNION");
					}
					if ( machineHead == 7 )
					{
						steamAchievementClient(c, "BARONY_ACH_MACHINE_HEAD");
					}
					if ( achievementObserver.playerAchievements[c].ticksSpentOverclocked >= 250 )
					{
						Uint32 increase = achievementObserver.playerAchievements[c].ticksSpentOverclocked / TICKS_PER_SECOND;
						steamStatisticUpdateClient(c, STEAM_STAT_OVERCLOCKED, STEAM_STAT_INT, increase);

						// add the leftover sub-second ticks result back into the score.
						achievementObserver.playerAchievements[c].ticksSpentOverclocked = increase % TICKS_PER_SECOND;
					}
				}
				updateGameplayStatisticsInMainLoop();
			}
			for ( int i = 0; i < MAXPLAYERS; ++i )
			{
				gameplayPreferences[i].process();
			}
			updatePlayerConductsInMainLoop();
			Compendium_t::Events_t::updateEventsInMainLoop(clientnum);
			achievementObserver.updatePlayerAchievement(clientnum, AchievementObserver::BARONY_ACH_DAPPER, AchievementObserver::DAPPER_EQUIPMENT_CHECK);

			//if( TICKS_PER_SECOND )
			//generatePathMaps();
			static ConsoleVariable<bool> cvar_debug_monster_timer("/debug_monster_timer", false);
			bool debugMonsterTimer = *cvar_debug_monster_timer && !gamePaused && keystatus[SDLK_F1];
			if ( debugMonsterTimer )
			{
				printlog("loop start");
			}
			real_t accum = 0.0;
			std::map<int, real_t> entityAccum;
			DebugStats.eventsT3 = std::chrono::high_resolution_clock::now();

			// run world UI entities
			for ( node = map.worldUI->first; node != nullptr; node = nextnode )
			{
				nextnode = node->next;
				entity = (Entity*)node->element;
				if ( entity && !entity->ranbehavior )
				{
					if ( !gamePaused || (multiplayer && !client_disconnected[0]) )
					{
						++entity->ticks;
					}
					if ( entity->behavior != nullptr )
					{
						if ( !gamePaused || (multiplayer && !client_disconnected[0]) )
						{
							(*entity->behavior)(entity);
						}
						if ( entitiesdeleted.first != nullptr )
						{
							entitydeletedself = false;
							for ( node2 = entitiesdeleted.first; node2 != nullptr; node2 = node2->next )
							{
								if ( entity == (Entity*)node2->element )
								{
									//printlog("DEBUG: Entity deleted self, sprite: %d", entity->sprite);
									entitydeletedself = true;
									break;
								}
							}
							if ( entitydeletedself == false )
							{
								entity->ranbehavior = true;
							}
							nextnode = map.worldUI->first;
							list_FreeAll(&entitiesdeleted);
						}
						else
						{
							entity->ranbehavior = true;
							nextnode = node->next;
						}
					}
				}
			}

			bool loadedNextLevel = false;
			for ( node = map.entities->first; node != nullptr; node = nextnode )
			{
				nextnode = node->next;
				entity = (Entity*)node->element;
				if ( entity && !entity->ranbehavior )
				{
					if ( !gamePaused || (multiplayer && !client_disconnected[0]) )
					{
						++entity->ticks;
					}
					if ( entity->behavior != nullptr )
					{
						if ( gameloopFreezeEntities 
							&& entity->behavior != &actPlayer
							&& entity->behavior != &actPlayerLimb
							&& entity->behavior != &actDeathGhost
							&& entity->behavior != &actHudWeapon
							&& entity->behavior != &actHudShield
							&& entity->behavior != &actHudAdditional
							&& entity->behavior != &actHudAdditional2
							&& entity->behavior != &actHudArrowModel
							&& entity->behavior != &actLeftHandMagic
							&& entity->behavior != &actRightHandMagic
							&& entity->behavior != &actFlame )
						{
							TimerExperiments::updateEntityInterpolationPosition(entity);
							continue;
						}
						int ox = -1;
						int oy = -1;
						auto t = std::chrono::high_resolution_clock::now();

						if ( !gamePaused || (multiplayer && !client_disconnected[0]) )
						{
							ox = static_cast<int>(entity->x) >> 4;
							oy = static_cast<int>(entity->y) >> 4;
							if ( !entity->myTileListNode )
							{
								TileEntityList.addEntity(*entity);
							}

							/*if ( entity->getUID() >= 0 && entity->behavior != &actFlame && !entity->flags[INVISIBLE]
								&& entity->behavior != &actDoor && entity->behavior != &actDoorFrame
								&& entity->behavior != &actGate && entity->behavior != &actTorch
								&& entity->behavior != &actSprite && !entity->flags[SPRITE]
								&& entity->skill[28] == 0 )
							{
								printlog("DEBUG: Starting Entity sprite: %d", entity->sprite);
							}*/
							(*entity->behavior)(entity);
						}
						if ( entitiesdeleted.first != nullptr )
						{
							entitydeletedself = false;
							for ( node2 = entitiesdeleted.first; node2 != nullptr; node2 = node2->next )
							{
								if ( entity == (Entity*)node2->element )
								{
									//printlog("DEBUG: Entity deleted self, sprite: %d", entity->sprite);
									entitydeletedself = true;
									break;
								}
							}
							if ( entitydeletedself == false )
							{
								if ( ox != -1 && oy != -1 )
								{
									if ( ox != static_cast<int>(entity->x) >> 4
										|| oy != static_cast<int>(entity->y) >> 4 )
									{
										// if entity moved into a new tile, update it's tile position in global tile list.
										TileEntityList.updateEntity(*entity);
									}
								}
								TimerExperiments::updateEntityInterpolationPosition(entity);

								entity->ranbehavior = true;
							}
							nextnode = map.entities->first;
							list_FreeAll(&entitiesdeleted);
						}
						else
						{
							if ( ox != -1 && oy != -1 )
							{
								if ( ox != static_cast<int>(entity->x) >> 4
									|| oy != static_cast<int>(entity->y) >> 4 )
								{
									// if entity moved into a new tile, update it's tile position in global tile list.
									TileEntityList.updateEntity(*entity);
								}
							}
							TimerExperiments::updateEntityInterpolationPosition(entity);

							entity->ranbehavior = true;
							nextnode = node->next;
							if ( debugMonsterTimer )
							{
								auto t2 = std::chrono::high_resolution_clock::now();
								//printlog("%d: %d %f", entity->sprite, entity->monsterState,
								//	1000 * std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t).count());
								accum += 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t).count();
								entityAccum[entity->sprite] += 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t).count();
								/*if ( entity->sprite == 1426 || entity->sprite == 1430 )
								{
									if ( 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t).count() > 1.0 )
									{
										printlog("gnome time: uid %d | time %.2f", entity->getUID(), 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t).count());
									}
								}*/
							}

						}
					}
				}

				if ( loadnextlevel == true )
				{
					// When this flag is set, it is time to load the next level.
					loadnextlevel = false;
					loadedNextLevel = true;

					/*
					* Capture surviving mechanism properties before checking which
					* original entities disappeared.
					*/
					capturePersistentMinimap(false);
					if (multiplayer == CLIENT)
					{
						syncClientPersistentMinimap(true);
					}
					capturePersistentMechanismStates();
					capturePersistentMapRemovals();
					int totalFloorGold = 0;
					int totalFloorItems = 0;
					int totalFloorItemValue[MAXPLAYERS];
					int totalFloorMonsters = 0;
					int totalFloorEnemies[MAXPLAYERS];
					Item tmpItem;
					for ( int i = 0; i < MAXPLAYERS; ++i )
					{
						totalFloorItemValue[i] = 0;
						totalFloorEnemies[i] = 0;
					}

					for ( node = map.entities->first; node != nullptr; node = node->next )
					{
						entity = (Entity*)node->element;
						entity->flags[NOUPDATE] = true;
						if ( entity->behavior == &actGoldBag )
						{
							totalFloorGold += entity->goldAmount;
						}
						else if ( entity->behavior == &actItem )
						{
							totalFloorItems++;
							tmpItem.type = (entity->skill[10] >= 0 && entity->skill[10] < NUMITEMS) ? (ItemType)entity->skill[10] : ItemType::GEM_ROCK;
							tmpItem.status = (int)entity->skill[11] < Status::BROKEN ?
								Status::BROKEN : ((int)entity->skill[11] > EXCELLENT ? EXCELLENT : (Status)entity->skill[11]);
							tmpItem.beatitude = std::min(std::max((Sint16)-100, (Sint16)entity->skill[12]), (Sint16)100);
							tmpItem.count = std::max((Sint16)entity->skill[13], (Sint16)1);
							tmpItem.appearance = entity->skill[14];
							tmpItem.identified = entity->skill[15];

							for ( int i = 0; i < MAXPLAYERS; ++i )
							{
								if ( !client_disconnected[i] )
								{
									totalFloorItemValue[i] += tmpItem.sellValue(i);
								}
							}
						}
						else if ( entity->behavior == &actMonster )
						{
							for ( int i = 0; i < MAXPLAYERS; ++i )
							{
								if ( !client_disconnected[i] )
								{
									if ( players[i]->entity )
									{
										if ( !entity->checkFriend(players[i]->entity) )
										{
											totalFloorEnemies[i]++;
										}
									}
								}
							}
							totalFloorMonsters++;
						}
						if ( (entity->behavior == &actThrown || entity->behavior == &actParticleSapCenter) && entity->sprite == 977 )
						{
							// boomerang particle, make sure to return on level change.
							Entity* parent = uidToEntity(entity->parent);
							if ( parent && parent->behavior == &actPlayer && stats[parent->skill[2]] )
							{
								Item* item = newItemFromEntity(entity);
								if ( !item )
								{
									continue;
								}
								item->ownerUid = parent->getUID();
								Item* pickedUp = itemPickup(parent->skill[2], item);
								Uint32 color = makeColorRGB(0, 255, 0);
								messagePlayerColor(parent->skill[2], MESSAGE_EQUIPMENT, color, Language::get(3746), items[item->type].getUnidentifiedName());
								if ( pickedUp )
								{
									if ( parent->skill[2] == 0 || (parent->skill[2] > 0 && splitscreen) )
									{
										// pickedUp is the new inventory stack for server, free the original items
										free(item);
										item = nullptr;
										if ( multiplayer != CLIENT && !stats[parent->skill[2]]->weapon )
										{
											useItem(pickedUp, parent->skill[2]);
										}

										auto& hotbar_t = players[parent->skill[2]]->hotbar;
										auto& hotbar = hotbar_t.slots();

										if ( hotbar_t.magicBoomerangHotbarSlot >= 0 )
										{
											hotbar[hotbar_t.magicBoomerangHotbarSlot].item = pickedUp->uid;
											for ( int i = 0; i < NUM_HOTBAR_SLOTS; ++i )
											{
												if ( i != hotbar_t.magicBoomerangHotbarSlot && hotbar[i].item == pickedUp->uid )
												{
													hotbar[i].item = 0;
													hotbar[i].resetLastItem();
												}
											}
										}
									}
									else
									{
										free(pickedUp); // item is the picked up items (item == pickedUp)
									}
								}
							}
						}
					}

					// hack to fix these things from breaking everything...
					for ( int i = 0; i < MAXPLAYERS; ++i )
					{
						players[i]->hud.arm = nullptr;
						players[i]->hud.weapon = nullptr;
						players[i]->hud.magicLeftHand = nullptr;
						players[i]->hud.magicRightHand = nullptr;
						players[i]->hud.magicRangefinder = nullptr;
						players[i]->ghost.reset();
						FollowerMenu[i].recentEntity = nullptr;
						FollowerMenu[i].followerToCommand = nullptr;
						FollowerMenu[i].entityToInteractWith = nullptr;
						CalloutMenu[i].closeCalloutMenuGUI();
						CalloutMenu[i].callouts.clear();
					}

					// stop all sounds
#ifdef USE_FMOD
					if ( sound_group )
					{
						sound_group->stop();
					}
					if ( soundAmbient_group )
					{
						soundAmbient_group->stop();
					}
					if ( soundEnvironment_group )
					{
						soundEnvironment_group->stop();
					}
					if ( soundNotification_group )
					{
						soundNotification_group->stop();
					}
					ensembleSounds.stopPlaying(true);
					VoiceChat.deinitRecording(false);
#elif defined USE_OPENAL
					if ( sound_group )
					{
						OPENAL_ChannelGroup_Stop(sound_group);
					}
					if ( soundAmbient_group )
					{
						OPENAL_ChannelGroup_Stop(soundAmbient_group);
					}
					if ( soundEnvironment_group )
					{
						OPENAL_ChannelGroup_Stop(soundEnvironment_group);
					}
#endif
					// stop combat music
					// close chests
					for ( c = 0; c < MAXPLAYERS; ++c )
					{
						assailantTimer[c] = 0;
						if ( players[c]->isLocalPlayer() )
						{
							if ( openedChest[c] )
							{
								openedChest[c]->closeChest();
							}
						}
						else if ( c > 0 && !client_disconnected[c] )
						{
							if ( openedChest[c] )
							{
								openedChest[c]->closeChestServer();
							}
						}
					}

					// copy followers list
					list_t tempFollowers[MAXPLAYERS];
					bool bCopyFollowers = true;
					if ( gameModeManager.getMode() == GameModeManager_t::GAME_MODE_TUTORIAL )
					{
						bCopyFollowers = false;
					}

					for ( c = 0; c < MAXPLAYERS; ++c )
					{
						tempFollowers[c].first = nullptr;
						tempFollowers[c].last = nullptr;

						node_t* node;
						for ( node = stats[c]->FOLLOWERS.first; bCopyFollowers && node != nullptr; node = node->next )
						{
							Entity* follower = nullptr;
							if ( (Uint32*)node->element )
							{
								follower = uidToEntity(*((Uint32*)node->element));
							}
							if ( follower )
							{
								Stat* followerStats = follower->getStats();
								if ( (int)follower->monsterSpecialAttackUnequipSafeguard > 0 )
								{
									// force deinit of special attacks to not be invalid state on next level.
									//messagePlayer(0, MESSAGE_DEBUG, "Cleared monster special");
									follower->handleMonsterSpecialAttack(followerStats, nullptr, 0.0, true);
								}

								if ( followerStats )
								{
									node_t* newNode = list_AddNodeLast(&tempFollowers[c]);
									newNode->element = followerStats->copyStats();
									newNode->deconstructor = &statDeconstructor;
									newNode->size = sizeof(followerStats);
								}
							}
						}

						list_FreeAll(&stats[c]->FOLLOWERS);
					}

					// unlock some steam achievements
					if ( !secretlevel )
					{
						switch ( currentlevel )
						{
							case 0:
								steamAchievement("BARONY_ACH_ENTER_THE_DUNGEON");
								break;
							default:
								break;
						}
					}

					std::string prevmapname = map.name;

					bool loadingTheSameFloorAsCurrent = false;
					if ( skipLevelsOnLoad > 0 )
					{
						currentlevel += skipLevelsOnLoad;
					}
					else
					{
						if ( skipLevelsOnLoad < 0 )
						{
							currentlevel += skipLevelsOnLoad;
							if ( skipLevelsOnLoad == -1 )
							{
								loadingTheSameFloorAsCurrent = true;
							}
						}
						++currentlevel;
					}
					skipLevelsOnLoad = 0;

					// signal clients about level change
					if ( gameModeManager.currentSession.seededRun.seed == 0 && !*cvar_map_sequence_rng )
					{
						mapseed = local_rng.rand();
					}
					else
					{
						map_sequence_rng.seedBytes(&uniqueGameKey, sizeof(uniqueGameKey));
						int rng_cycles = std::max(0, currentlevel + (secretlevel ? 100 : 0));
						while ( rng_cycles > 0 )
						{
							map_sequence_rng.rand(); // dummy advance
							--rng_cycles;
						}
						mapseed = map_sequence_rng.rand();
					}
					lastEntityUIDs = entity_uids;
					if ( forceMapSeed > 0 )
					{
						mapseed = forceMapSeed;
						forceMapSeed = 0;
					}

					if ( !secretlevel )
					{
						switch ( currentlevel )
						{
							case 5:
								steamAchievement("BARONY_ACH_TWISTY_PASSAGES");
								// the observer will send out to clients.
								achievementObserver.updatePlayerAchievement(clientnum,
									AchievementObserver::Achievement::BARONY_ACH_COOP_ESCAPE_MINES, AchievementObserver::ACH_EVENT_NONE);
								break;
							case 10:
								steamAchievement("BARONY_ACH_JUNGLE_FEVER");
								break;
							case 15:
								steamAchievement("BARONY_ACH_SANDMAN");
								break;
							case 30:
								steamAchievement("BARONY_ACH_SPELUNKY");
								break;
							case 35:
								if ( ((completionTime / TICKS_PER_SECOND) / 60) <= 45 )
								{
									conductGameChallenges[CONDUCT_BLESSED_BOOTS_SPEED] = 1;
								}
								break;
							default:
								break;
						}
					}

					if ( multiplayer == SERVER && net_packet && net_packet->data )
					{
						for ( c = 1; c < MAXPLAYERS; ++c )
						{
							if ( client_disconnected[c] == true || players[c]->isLocalPlayer() )
							{
								continue;
							}
							sendPersistentWorldSnapshotToClient(
								c,
								loadCustomNextMap
							);
							if ( loadingSameLevelAsCurrent )
							{
								strcpy((char*)net_packet->data, "LVLR");
							}
							else
							{
								strcpy((char*)net_packet->data, "LVLC");
							}
							net_packet->data[4] = secretlevel;
							SDLNet_Write32(mapseed, &net_packet->data[5]);
							SDLNet_Write32(lastEntityUIDs, &net_packet->data[9]);
							net_packet->data[13] = currentlevel;

							// The custom map name begins at byte 14.
							// An empty string means use the normal level-list destination.
							const size_t customMapNameLength =
								loadCustomNextMap.length();

							if ( customMapNameLength > 0 )
							{
								strcpy(
									reinterpret_cast<char*>(
										&net_packet->data[14]
									),
									loadCustomNextMap.c_str()
								);
							}
							else
							{
								net_packet->data[14] = 0;
							}

							// Explicit null terminator for both named and unnamed maps.
							const size_t tunnelIDOffset =
								14 + customMapNameLength + 1;

							net_packet->data[
								14 + customMapNameLength
							] = 0;

							// Append the destination tunnel ID after the map-name terminator.
							// ID 0 means the destination map's normal Player Start.
							SDLNet_Write32(
								static_cast<Uint32>(
									loadCustomNextTunnelID
								),
								&net_packet->data[tunnelIDOffset]
							);

							net_packet->len =
								tunnelIDOffset + sizeof(Uint32);

							printlog(
								"[Custom Tunnel] Sending map '%s' and destination tunnel ID %d to client %d.",
								loadCustomNextMap.c_str(),
								loadCustomNextTunnelID,
								c
							);
							net_packet->address.host = net_clients[c - 1].host;
							net_packet->address.port = net_clients[c - 1].port;
							sendPacketSafe(net_sock, -1, net_packet, c - 1);
						}
					}
					loadingSameLevelAsCurrent = false;
					darkmap = false;

					bool playerDied[MAXPLAYERS] = { false };
					for ( int i = 0; i < MAXPLAYERS; ++i )
					{
						if ( stats[i] && stats[i]->HP <= 0 )
						{
							playerDied[i] = true;
						}
						if ( i == 0 || !players[i]->isLocalPlayer() )
						{
							Compendium_t::Events_t::eventUpdateWorld(i, Compendium_t::CPDM_GOLD_LEFT_BEHIND, "merchants guild", totalFloorGold);
							Compendium_t::Events_t::eventUpdateWorld(i, Compendium_t::MONSTERS_LEFT_BEHIND, "the church", totalFloorEnemies[i]);
							Compendium_t::Events_t::eventUpdateWorld(i, Compendium_t::ITEMS_LEFT_BEHIND, "merchants guild", totalFloorItems);
							Compendium_t::Events_t::eventUpdateWorld(i, Compendium_t::ITEM_VALUE_LEFT_BEHIND, "merchants guild", totalFloorItemValue[i]);
						}
					}

                    // load map file
					loading = true;
					createLevelLoadScreen(5);

					std::atomic_bool loading_done { false };

					const Sint32 requestedTunnelID =
						loadCustomNextTunnelID;

					auto loading_task = std::async(
						std::launch::async,
						[&loading_done, requestedTunnelID]()
						{
							gameplayCustomManager.readFromFile();

							if ( gameplayCustomManager.inUse() )
							{
								conductGameChallenges[CONDUCT_MODDED] = 1;
								Mods::disableSteamAchievements = true;
							}

							textSourceScript.scriptVariables.clear();
							updateLoadingScreen(10);

							int checkMapHash = -1;

							int result = physfsLoadMapFile(
								currentlevel,
								mapseed,
								false,
								&checkMapHash
							);

							/*
							* The raw editor entities now exist, but runtime behaviors have not
							* been assigned yet.
							*/
							applyPersistentMapRemovals();

							restorePersistentMinimap();
							if ( !verifyMapHash(
								map.filename,
								checkMapHash
							) )
							{
								conductGameChallenges[CONDUCT_MODDED] = 1;
								Mods::disableSteamAchievements = true;
							}

							updateLoadingScreen(50);

							numplayers = 0;
							assignActions(&map);
							applyPersistentMechanismStates();
							placePendingAutomatiaSinglePlayerReverseReturn();
							if ( requestedTunnelID > 0 )
							{
								placePlayersAtCustomTunnel(
									requestedTunnelID
								);
							}
							if ( multiplayer == CLIENT )
							{
								applyPendingTunnelSpawn();
							}
							updateLoadingScreen(55);

							generatePathMaps();
							updateLoadingScreen(99);

							loading_done = true;
							return result;
						}
					);
	                while (!loading_done)
	                {
		                doLoadingScreen();
		                std::this_thread::sleep_for(std::chrono::milliseconds(1));
	                }
	                destroyLoadingScreen();
                    clearChunks();
                    createChunks();
		            loading = false;
	                int result = loading_task.get();
					restoreAutomatiaPersistentMinimapForLocalPlayer();
					loadCustomNextTunnelID = 0;
                    for (int c = 0; c < MAXPLAYERS; ++c) {
                        auto& camera = players[c]->camera();
					    camera.globalLightModifierActive = GLOBAL_LIGHT_MODIFIER_STOPPED;
                        camera.luminance = defaultLuminance;
					}

					// clear follower menu entities.
					for ( int i = 0; i < MAXPLAYERS; ++i )
					{
						minimapPings[i].clear(); // clear minimap pings
						if ( players[i]->isLocalPlayer() )
						{
							FollowerMenu[i].closeFollowerMenuGUI(true);
							CalloutMenu[i].closeCalloutMenuGUI();
						}
						players[i]->hud.followerBars.clear();
						spellcastingAnimationManager_deactivate(&cast_animation[i]);
					}
					EnemyHPDamageBarHandler::dumpCache();
					AOEIndicators_t::cleanup();
					monsterAllyFormations.reset();
					particleTimerEmitterHitEntities.clear();
					particleTimerEffects.clear();
					monsterTrapIgnoreEntities.clear();
					minimapHighlights.clear();

					achievementObserver.updateData();

					if ( !strncmp(map.name, "Mages Guild", 11) )
					{
						for ( c = 0; c < MAXPLAYERS; ++c )
						{
							if ( players[c] && players[c]->entity )
							{
								players[c]->entity->modHP(999);
								players[c]->entity->modMP(999);
								if ( stats[c] && stats[c]->HUNGER < 1450 )
								{
									stats[c]->HUNGER = 1450;
									serverUpdateHunger(c);
								}
							}
						}
						messageLocalPlayers(MESSAGE_STATUS, Language::get(2599));

						// undo shopkeeper grudge
						for ( c = 0; c < MAXPLAYERS; ++c )
						{
							ShopkeeperPlayerHostility.resetPlayerHostility(c, true);
						}
					}

					// (special) unlock temple achievement
					if ( secretlevel && currentlevel == 8 )
					{
						steamAchievement("BARONY_ACH_TRICKS_AND_TRAPS");
					}
					// flag setting we've reached the lich
					if ( !strncmp(map.name, "Boss", 4) || !strncmp(map.name, "Sanctum", 7) )
					{
						for ( int c = 0; c < MAXPLAYERS; ++c )
						{
							steamStatisticUpdateClient(c, STEAM_STAT_BACK_TO_BASICS, STEAM_STAT_INT, 1);
						}
					}

					Player::Minimap_t::mapDetails.clear();

					if ( gameModeManager.getMode() == GameModeManager_t::GAME_MODE_TUTORIAL )
					{
						if ( gameModeManager.Tutorial.showFirstTutorialCompletedPrompt )
						{
							gameModeManager.Tutorial.createFirstTutorialCompletedPrompt();
						}
					}
					else if ( !secretlevel )
					{
						messageLocalPlayers(MESSAGE_PROGRESSION, Language::get(710), currentlevel);
					}
					else
					{
						messageLocalPlayers(MESSAGE_PROGRESSION, Language::get(711), map.name);
					}

					gameModeManager.Tutorial.showFirstTutorialCompletedPrompt = false;

					if ( !secretlevel && result )
					{
						switch ( currentlevel )
						{
							case 2:
								messageLocalPlayers(MESSAGE_HINT, Language::get(712));
								Player::Minimap_t::mapDetails.push_back(std::make_pair("secret_exit_description", Language::get(712)));
								break;
							case 3:
								messageLocalPlayers(MESSAGE_HINT, Language::get(713));
								Player::Minimap_t::mapDetails.push_back(std::make_pair("secret_exit_description", Language::get(713)));
								break;
							case 7:
								messageLocalPlayers(MESSAGE_HINT, Language::get(714));
								Player::Minimap_t::mapDetails.push_back(std::make_pair("secret_exit_description", Language::get(714)));
								break;
							case 8:
								messageLocalPlayers(MESSAGE_HINT, Language::get(715));
								Player::Minimap_t::mapDetails.push_back(std::make_pair("secret_exit_description", Language::get(715)));
								break;
							case 11:
								messageLocalPlayers(MESSAGE_HINT, Language::get(716));
								Player::Minimap_t::mapDetails.push_back(std::make_pair("secret_exit_description", Language::get(716)));
								break;
							case 13:
								messageLocalPlayers(MESSAGE_HINT, Language::get(717));
								Player::Minimap_t::mapDetails.push_back(std::make_pair("secret_exit_description", Language::get(717)));
								break;
							case 16:
								messageLocalPlayers(MESSAGE_HINT, Language::get(718));
								Player::Minimap_t::mapDetails.push_back(std::make_pair("secret_exit_description", Language::get(718)));
								break;
							case 18:
								messageLocalPlayers(MESSAGE_HINT, Language::get(719));
								Player::Minimap_t::mapDetails.push_back(std::make_pair("secret_exit_description", Language::get(719)));
								break;
							default:
								break;
						}
					}
					if ( MFLAG_DISABLETELEPORT )
					{
						Player::Minimap_t::mapDetails.push_back(std::make_pair("map_flag_disable_teleport", Language::get(2382)));
					}
					if ( MFLAG_DISABLEOPENING )
					{
						Player::Minimap_t::mapDetails.push_back(std::make_pair("map_flag_disable_opening", Language::get(2382)));
					}
					if ( MFLAG_DISABLETELEPORT || MFLAG_DISABLEOPENING )
					{
						messageLocalPlayers(MESSAGE_HINT, Language::get(2382));
					}
					if ( MFLAG_DISABLELEVITATION )
					{
						messageLocalPlayers(MESSAGE_HINT, Language::get(2383));
						Player::Minimap_t::mapDetails.push_back(std::make_pair("map_flag_disable_levitation", Language::get(2383)));
					}
					if ( MFLAG_DISABLEDIGGING )
					{
						messageLocalPlayers(MESSAGE_HINT, Language::get(2450));
						Player::Minimap_t::mapDetails.push_back(std::make_pair("map_flag_disable_digging", Language::get(2450)));
					}
					if ( MFLAG_DISABLEHUNGER )
					{
						Player::Minimap_t::mapDetails.push_back(std::make_pair("map_flag_disable_hunger", ""));
					}

					loading = false;

					fadeout = false;
					fadealpha = 255;

					for (c = 0; c < MAXPLAYERS; c++)
					{
						if (players[c] && players[c]->entity && !client_disconnected[c])
						{
							if ( stats[c] && stats[c]->getEffectActive(EFF_POLYMORPH) && stats[c]->playerPolymorphStorage != NOTHING )
							{
								players[c]->entity->effectPolymorph = stats[c]->playerPolymorphStorage;
								serverUpdateEntitySkill(players[c]->entity, 50); // update visual polymorph effect for clients.
							}
							if ( stats[c] && stats[c]->getEffectActive(EFF_SHAPESHIFT) && stats[c]->playerShapeshiftStorage != NOTHING )
							{
								players[c]->entity->effectShapeshift = stats[c]->playerShapeshiftStorage;
								serverUpdateEntitySkill(players[c]->entity, 53); // update visual polymorph effect for clients.
							}
							if ( stats[c] && stats[c]->getEffectActive(EFF_VAMPIRICAURA) && stats[c]->EFFECTS_TIMERS[EFF_VAMPIRICAURA] == -2 )
							{
								players[c]->entity->playerVampireCurse = 1;
								serverUpdateEntitySkill(players[c]->entity, 51); // update curse progression
							}

							node_t* node;
							node_t* gyrobotNode = nullptr;
							Entity* gyrobotEntity = nullptr;
							std::vector<node_t*> allyRobotNodes;
							for ( node = tempFollowers[c].first; node != NULL; node = node->next )
							{
								Stat* tempStats = (Stat*)node->element;
								if ( tempStats && tempStats->type == GYROBOT )
								{
									gyrobotNode = node;
									break;
								}
							}
							for (node = tempFollowers[c].first; node != nullptr; node = node->next)
							{
								Stat* tempStats = (Stat*)node->element;
								if ( tempStats && (tempStats->type == DUMMYBOT
									|| tempStats->type == SENTRYBOT
									|| tempStats->type == SPELLBOT) )
								{
									// gyrobot will pick up these guys into it's inventory, otherwise leave them behind.
									if ( gyrobotNode )
									{
										allyRobotNodes.push_back(node);
									}
									continue;
								}
								Entity* monster = summonMonster(tempStats->type, players[c]->entity->x, players[c]->entity->y);
								if (monster)
								{
									if ( node == gyrobotNode )
									{
										gyrobotEntity = monster;
									}
									monster->skill[3] = 1; // to mark this monster partially initialized
									list_RemoveNode(monster->children.last);

									node_t* newNode = list_AddNodeLast(&monster->children);
									newNode->element = tempStats->copyStats();
									newNode->deconstructor = &statDeconstructor;
									newNode->size = sizeof(tempStats);

									Stat* monsterStats = (Stat*)newNode->element;
									monsterStats->leader_uid = players[c]->entity->getUID();
									messagePlayerMonsterEvent(c, 0xFFFFFFFF, *monsterStats, Language::get(721), Language::get(720), MSG_COMBAT_BASIC);
									monster->flags[USERFLAG2] = true;
									serverUpdateEntityFlag(monster, USERFLAG2);
									/*if (!monsterally[HUMAN][monsterStats->type])
									{
									}*/
									monster->monsterAllyIndex = c;
									if ( multiplayer == SERVER )
									{
										serverUpdateEntitySkill(monster, 42); // update monsterAllyIndex for clients.
									}

									if ( multiplayer != CLIENT )
									{
										monster->monsterAllyClass = monsterStats->allyClass;
										monster->monsterAllyPickupItems = monsterStats->allyItemPickup;
										if ( stats[c]->playerSummonPERCHR != 0 && MonsterData_t::nameMatchesSpecialNPCName(*monsterStats, "skeleton knight") )
										{
											monster->monsterAllySummonRank = (stats[c]->playerSummonPERCHR & 0x0000FF00) >> 8;
										}
										else if ( stats[c]->playerSummon2PERCHR != 0 && MonsterData_t::nameMatchesSpecialNPCName(*monsterStats, "skeleton sentinel") )
										{
											monster->monsterAllySummonRank = (stats[c]->playerSummon2PERCHR & 0x0000FF00) >> 8;
										}
										else if ( monsterStats->getAttribute("SUMMONED_CREATURE") != "" )
										{
											monster->monsterAllySummonRank = std::stoi(monsterStats->getAttribute("SUMMONED_CREATURE"));
										}
										serverUpdateEntitySkill(monster, 46); // update monsterAllyClass
										serverUpdateEntitySkill(monster, 44); // update monsterAllyPickupItems
										serverUpdateEntitySkill(monster, 50); // update monsterAllySummonRank
									}

									newNode = list_AddNodeLast(&stats[c]->FOLLOWERS);
									newNode->deconstructor = &defaultDeconstructor;
									Uint32* myuid = (Uint32*) malloc(sizeof(Uint32));
									newNode->element = myuid;
									*myuid = monster->getUID();

									if ( monsterStats->type == HUMAN && currentlevel == 25 && !strncmp(map.name, "Mages Guild", 11) )
									{
										steamAchievementClient(c, "BARONY_ACH_ESCORT");
										Compendium_t::Events_t::eventUpdateWorld(c, Compendium_t::CPDM_HUMANS_SAVED, "the church", 1);
									}

									if ( c > 0 && multiplayer == SERVER && !players[c]->isLocalPlayer() && net_packet && net_packet->data )
									{
										strcpy((char*)net_packet->data, "LEAD");
										SDLNet_Write32((Uint32)monster->getUID(), &net_packet->data[4]);
										std::string name = monsterStats->name;
										if ( name != "" && name == MonsterData_t::getSpecialNPCName(*monsterStats) )
										{
											name = monsterStats->getAttribute("special_npc");
											name.insert(0, "$");
										}
                                        SDLNet_Write32(monsterStats->type, &net_packet->data[8]);
										strcpy((char*)(&net_packet->data[12]), name.c_str());
										net_packet->data[12 + strlen(name.c_str())] = 0;
										net_packet->address.host = net_clients[c - 1].host;
										net_packet->address.port = net_clients[c - 1].port;
										net_packet->len = 12 + strlen(name.c_str()) + 1;
										sendPacketSafe(net_sock, -1, net_packet, c - 1);

										serverUpdateAllyStat(c, monster->getUID(), monsterStats->LVL, monsterStats->HP, monsterStats->MAXHP, monsterStats->type);
									}
                                    
                                    if (players[c]->isLocalPlayer() && monsterStats->name[0] && (!monsterNameIsGeneric(*monsterStats) || monsterStats->type == SLIME)) {
                                        Entity* nametag = newEntity(-1, 1, map.entities, nullptr);
                                        nametag->x = monster->x;
                                        nametag->y = monster->y;
                                        nametag->z = monster->z - 6;
                                        nametag->sizex = 1;
                                        nametag->sizey = 1;
                                        nametag->flags[NOUPDATE] = true;
                                        nametag->flags[PASSABLE] = true;
                                        nametag->flags[SPRITE] = true;
                                        nametag->flags[UNCLICKABLE] = true;
                                        nametag->flags[BRIGHT] = true;
                                        nametag->behavior = &actSpriteNametag;
                                        nametag->parent = monster->getUID();
                                        nametag->scalex = 0.2;
                                        nametag->scaley = 0.2;
                                        nametag->scalez = 0.2;
                                        nametag->skill[0] = c;
                                        nametag->skill[1] = playerColor(c, colorblind_lobby, true);
                                    }

									if ( !FollowerMenu[c].recentEntity && players[c]->isLocalPlayer() )
									{
										FollowerMenu[c].recentEntity = monster;
									}
								}
								else
								{
									messagePlayerMonsterEvent(c, 0xFFFFFFFF, *tempStats, Language::get(723), Language::get(722), MSG_COMBAT_BASIC);
								}
							}
							if ( gyrobotEntity && !allyRobotNodes.empty() )
							{
								Stat* gyroStats = gyrobotEntity->getStats();
								for ( auto it = allyRobotNodes.begin(); gyroStats && it != allyRobotNodes.end(); ++it )
								{
									node_t* botNode = *it;
									if ( botNode )
									{
										Stat* tempStats = (Stat*)botNode->element;
										if ( tempStats )
										{
											ItemType type = WOODEN_SHIELD;
											if ( tempStats->type == SENTRYBOT )
											{
												type = TOOL_SENTRYBOT;
											}
											else if ( tempStats->type == SPELLBOT )
											{
												type = TOOL_SPELLBOT;
											}
											else if ( tempStats->type == DUMMYBOT )
											{
												type = TOOL_DUMMYBOT;
											}
											int appearance = monsterTinkeringConvertHPToAppearance(tempStats);
											if ( type != WOODEN_SHIELD )
											{
												Item* item = newItem(type, static_cast<Status>(tempStats->monsterTinkeringStatus),
													0, 1, appearance, true, &gyroStats->inventory);
											}
										}
									}
								}
							}
						}
						list_FreeAll(&tempFollowers[c]);
					}

					for ( c = 0; c < MAXPLAYERS; c++ )
					{
						Compendium_t::Events_t::onLevelChangeEvent(c, Compendium_t::Events_t::previousCurrentLevel, Compendium_t::Events_t::previousSecretlevel, prevmapname, playerDied[c]);
						players[c]->compendiumProgress.playerAliveTimeTotal = 0;
						players[c]->compendiumProgress.playerGameTimeTotal = 0;

						if ( c == clientnum && !playerDied[c] )
						{
							if ( stats[c]->type == MYCONID && stats[c]->playerRace == RACE_MYCONID && stats[c]->stat_appearance == 0
								&& stats[c]->helmet && gameStatistics[STATISTICS_NO_CAP] >= 0 )
							{
								gameStatistics[STATISTICS_NO_CAP]++;
								if ( gameStatistics[STATISTICS_NO_CAP] >= 5 )
								{
									steamAchievement("BARONY_ACH_NO_CAP");
								}
							}
							if ( stats[c]->getEffectActive(EFF_GROWTH) >= 2
								&& ((stats[c]->type == MYCONID && stats[c]->playerRace == RACE_MYCONID)
									|| (stats[c]->type == DRYAD && stats[c]->playerRace == RACE_DRYAD)) && stats[c]->stat_appearance == 0
								&& !stats[c]->helmet && gameStatistics[STATISTICS_DONT_TOUCH_HAIR] >= 0 )
							{
								gameStatistics[STATISTICS_DONT_TOUCH_HAIR]++;
								if ( gameStatistics[STATISTICS_DONT_TOUCH_HAIR] >= 25 )
								{
									steamAchievement("BARONY_ACH_DONT_TOUCH_HAIR");
								}
							}
							if ( stats[c]->type == SALAMANDER && stats[c]->playerRace == RACE_SALAMANDER && stats[c]->stat_appearance == 0
								&& stats[c]->getEffectActive(EFF_SALAMANDER_HEART) >= 3 && stats[c]->getEffectActive(EFF_SALAMANDER_HEART) <= 4
								&& gameStatistics[STATISTICS_GARGOYLES_QUEST] >= 0 )
							{
								gameStatistics[STATISTICS_GARGOYLES_QUEST]++;
								if ( gameStatistics[STATISTICS_GARGOYLES_QUEST] >= 10 )
								{
									steamAchievement("BARONY_ACH_GARGOYLES_QUEST");
								}
							}
							if ( stats[c]->type == SALAMANDER && stats[c]->playerRace == RACE_SALAMANDER && stats[c]->stat_appearance == 0
								&& stats[c]->getEffectActive(EFF_SALAMANDER_HEART) >= 1 && stats[c]->getEffectActive(EFF_SALAMANDER_HEART) <= 2
								&& gameStatistics[STATISTICS_FIRE_FIGHTER] >= 0 )
							{
								gameStatistics[STATISTICS_FIRE_FIGHTER]++;
								if ( gameStatistics[STATISTICS_FIRE_FIGHTER] >= 5 )
								{
									steamAchievement("BARONY_ACH_FIRE_FIGHTER");
								}
							}
							if ( stats[c]->type == SALAMANDER && stats[c]->playerRace == RACE_SALAMANDER && stats[c]->stat_appearance == 0
								&& !stats[c]->getEffectActive(EFF_SALAMANDER_HEART)
								&& gameStatistics[STATISTICS_DISCIPLINE] >= 0 )
							{
								gameStatistics[STATISTICS_DISCIPLINE]++;
								if ( gameStatistics[STATISTICS_DISCIPLINE] >= 25 )
								{
									steamAchievement("BARONY_ACH_DISCIPLINE");
								}
							}
						}

						// unsustain any previous effects
						node_t* spellnode;
						spellnode = stats[c]->magic_effects.first;
						while ( spellnode )
						{
							node_t* oldnode = spellnode;
							spellnode = spellnode->next;
							if ( spell_t* spell = (spell_t*)oldnode->element )
							{
								spell->magic_effects_node = NULL;
								if ( spell->sustainEffectDissipate >= 0 )
								{
									if ( stats[c]->getEffectActive(spell->sustainEffectDissipate) )
									{
										if ( stats[c]->EFFECTS_TIMERS[spell->sustainEffectDissipate] > 0 )
										{
											stats[c]->EFFECTS_TIMERS[spell->sustainEffectDissipate] = 1;
										}
									}
									list_RemoveNode(oldnode);
								}
								else
								{
									spell->sustain = false;
								}
							}
						}
					}

                    // save at end of level change
					if ( gameModeManager.allowsSaves() )
					{
						saveGame();
					}
					Compendium_t::Events_t::writeItemsSaveData();
					Compendium_t::writeUnlocksSaveData();
#ifdef LOCAL_ACHIEVEMENTS
					LocalAchievements_t::writeToFile();
#endif
					break;
				}
			}
			if ( debugMonsterTimer )
			{
				if ( accum > 5.0 )
				{
					printlog("accum: %f, tick: %d", accum, ticks);
					for ( auto& pair : entityAccum )
					{
						printlog("entity: %d, accum: %.2f", pair.first, pair.second);
					}
				}
				}
			for ( node = map.entities->first; node != nullptr; node = node->next )
			{
				entity = (Entity*)node->element;
				entity->ranbehavior = false;
			}
			for ( node = map.worldUI->first; node != nullptr; node = node->next )
			{
				entity = (Entity*)node->element;
				entity->ranbehavior = false;
			}
			processPendingAutomatiaTransitions();
			if ( MapInstance* activeInstance = worldState.activeInstance() )
			{
				++activeInstance->simulationTick;
			}
			simulateOccupiedAutomatiaInstances();
			DebugStats.eventsT4 = std::chrono::high_resolution_clock::now();
			if ( multiplayer == SERVER )
			{
				// periodically remind clients of the current level
				if ( ticks % (TICKS_PER_SECOND * 3) == 0 )
				{
					for ( c = 1; c < MAXPLAYERS; c++ )
					{
                        if ( !serverPlayerCanReceiveActiveMapUpdates(c) )
						{
							continue;
						}
						if (net_packet && net_packet->data) {
							strcpy((char*)net_packet->data, "LVLC");
							net_packet->data[4] = secretlevel;
							SDLNet_Write32(mapseed, &net_packet->data[5]);
							SDLNet_Write32(lastEntityUIDs, &net_packet->data[9]);
							net_packet->data[13] = currentlevel;
							net_packet->data[14] = 0;
							net_packet->address.host = net_clients[c - 1].host;
							net_packet->address.port = net_clients[c - 1].port;
							net_packet->len = 15;
							sendPacketSafe(net_sock, -1, net_packet, c - 1);
						}
					}
				}

				bool updatePlayerHealth = false;
				if ( serverSchedulePlayerHealthUpdate != 0 && (ticks - serverSchedulePlayerHealthUpdate) >= TICKS_PER_SECOND * 0.2 )
				{
					// update if x ticks have passed since request of health update.
					// the scheduled update needs to be reset to 0 to be set again.
					updatePlayerHealth = true;
				}
				else if ( (ticks % (TICKS_PER_SECOND * 3) == 0) 
					&& (serverLastPlayerHealthUpdate == 0 || ((ticks - serverLastPlayerHealthUpdate) >= 2 * TICKS_PER_SECOND)) 
				)
				{
					// regularly update every 3 seconds, only if the last update was more than 2 seconds ago.
					updatePlayerHealth = true;
				}

				if ( updatePlayerHealth )
				{
					// send update to all clients for global stats[NUMPLAYERS] struct
					serverLastPlayerHealthUpdate = ticks;
					serverSchedulePlayerHealthUpdate = 0;
					serverUpdatePlayerStats();
				}

				{
					const bool forceUpdate = (ticks % (TICKS_PER_SECOND * 15)) == 0; // re-send every x seconds, or when update flag is dirty.
					ShopkeeperPlayerHostility.serverSendClientUpdate(forceUpdate);
				}

				// send entity info to clients
				if ( ticks % (TICKS_PER_SECOND / 8) == 0 )
				{
					for ( node = map.entities->first; node != nullptr; node = node->next )
					{
						entity = (Entity*)node->element;
						for ( c = 1; c < MAXPLAYERS; ++c )
						{
							if ( !client_disconnected[c] )
							{
								if ( entity->flags[UPDATENEEDED] == true && entity->flags[NOUPDATE] == false )
								{
									// update entity for all clients
									if ( entity->getUID() % (TICKS_PER_SECOND * 4) == ticks % (TICKS_PER_SECOND * 4) )
									{
										sendEntityUDP(entity, c, true);
									}
									else
									{
										sendEntityUDP(entity, c, false);
									}
								}
							}
						}
					}
				}

				// handle keep alives
				if (*cvar_enableKeepAlives) {
					for ( c = 1; c < MAXPLAYERS; c++ )
					{
						if ( client_disconnected[c] || players[c]->isLocalPlayer() || !net_packet || !net_packet->data )
						{
							continue;
						}
						if ( ticks % (TICKS_PER_SECOND * 1) == 0 )
						{
							// send a keep alive every second
							strcpy((char*)net_packet->data, "KPAL");
							net_packet->data[4] = clientnum;
							net_packet->address.host = net_clients[c - 1].host;
							net_packet->address.port = net_clients[c - 1].port;
							net_packet->len = 5;
							sendPacketSafe(net_sock, -1, net_packet, c - 1);
						}
						if ( losingConnection[c] && ticks - client_keepalive[c] == 1 )
						{
							// regained connection
							losingConnection[c] = false;
							messageLocalPlayers(MESSAGE_MISC, Language::get(724), c, stats[c]->name);
						}
						else if ( !losingConnection[c] && ticks - client_keepalive[c] == TICKS_PER_SECOND * TIMEOUT_WARNING_TIME - 1 )
						{
							// give warning
							losingConnection[c] = true;
							messageLocalPlayers(MESSAGE_MISC, Language::get(725), c, stats[c]->name, TIMEOUT_TIME - TIMEOUT_WARNING_TIME);
						}
						else if ( !client_disconnected[c] && ticks - client_keepalive[c] >= TICKS_PER_SECOND * TIMEOUT_TIME - 1 )
						{
							// kick client
							messageLocalPlayers(MESSAGE_MISC, Language::get(726), c, stats[c]->name);
							strcpy((char*)net_packet->data, "KICK");
							net_packet->address.host = net_clients[c - 1].host;
							net_packet->address.port = net_clients[c - 1].port;
							net_packet->len = 4;
							sendPacketSafe(net_sock, -1, net_packet, c - 1);
							disconnectAutomatiaRemotePlayer(
								c, "connection timeout", true);
						}
					}
					PingNetworkStatus_t::update();
				}
			}

			// update clients on assailant status
			if (multiplayer != SINGLE) {
				for ( c = 1; c < MAXPLAYERS; c++ )
				{
					if ( !client_disconnected[c] && !players[c]->isLocalPlayer() )
					{
						if ( oassailant[c] != assailant[c] )
						{
							oassailant[c] = assailant[c];
							if (net_packet && net_packet->data) {
								strcpy((char*)net_packet->data, "MUSM");
								net_packet->address.host = net_clients[c - 1].host;
								net_packet->address.port = net_clients[c - 1].port;
								net_packet->data[4] = assailant[c];
								net_packet->len = 5;
								sendPacketSafe(net_sock, -1, net_packet, c - 1);
							}
						}
					}
				}
			}
			combat = assailant[0];
			for ( j = 0; j < MAXPLAYERS; j++ )
			{
				client_selected[j] = NULL;
			}

			Player::PlayerMechanics_t::ensembleMusicUpdate();

			// world UI
			Player::WorldUI_t::handleTooltips();

			AOEIndicators_t::update();

			int backpack_sizey[MAXPLAYERS];

			for ( int player = 0; player < MAXPLAYERS; ++player )
			{
				if ( !players[player]->isLocalPlayer() )
				{
					continue;
				}
				backpack_sizey[player] = players[player]->inventoryUI.DEFAULT_INVENTORY_SIZEY;
				const int inventorySizeX = players[player]->inventoryUI.getSizeX();
				auto& playerInventory = players[player]->inventoryUI;

				if ( stats[player]->cloak && stats[player]->cloak->type == CLOAK_BACKPACK && stats[player]->cloak->status != BROKEN
					&& (shouldInvertEquipmentBeatitude(stats[player]) ? abs(stats[player]->cloak->beatitude) >= 0 : stats[player]->cloak->beatitude >= 0) )
				{
					backpack_sizey[player] = playerInventory.DEFAULT_INVENTORY_SIZEY + playerInventory.getPlayerBackpackBonusSizeY();
				}

				if ( backpack_sizey[player] == playerInventory.DEFAULT_INVENTORY_SIZEY + playerInventory.getPlayerBackpackBonusSizeY() )
				{
					playerInventory.setSizeY(playerInventory.DEFAULT_INVENTORY_SIZEY + playerInventory.getPlayerBackpackBonusSizeY());
				}
				else
				{
					playerInventory.setSizeY(players[player]->inventoryUI.DEFAULT_INVENTORY_SIZEY);
				}
			}

			DebugStats.eventsT5 = std::chrono::high_resolution_clock::now();

			for ( int player = 0; player < MAXPLAYERS; ++player )
			{
				players[player]->magic.bHasUnreadNewSpell = false;
				if ( !players[player]->isLocalPlayer() )
				{
					continue;
				}

				int bloodCount = 0;
				std::map<int, int> numKeys;

				Item* previousAppraiseItem = nullptr;
				bool updateItemToAppraise = false;
				if ( players[player]->inventoryUI.appraisal.current_item != 0 && *cvar_appraisal_auto_switch )
				{
					if ( previousAppraiseItem = uidToItem(players[player]->inventoryUI.appraisal.current_item) )
					{
						if ( previousAppraiseItem->uid != players[player]->inventoryUI.appraisal.manual_appraised_item )
						{
							updateItemToAppraise = true;
						}
					}
				}

				for ( node = stats[player]->inventory.first; node != NULL; node = nextnode )
				{
					nextnode = node->next;
					Item* item = (Item*)node->element;
					if ( !item )
					{
						continue;
					}

					if ( itemCategory(item) == SPELL_CAT )
					{
						if ( item->notifyIcon )
						{
							players[player]->magic.bHasUnreadNewSpell = true;
						}
						item->spellNotifyIcon = false;
						if ( auto spell = getSpellFromItem(player, item, false) )
						{
							if ( stats[player]->getProficiency(spell->skillID) < SKILL_LEVEL_LEGENDARY
								&& spell->difficulty > (stats[player]->getProficiency(spell->skillID) - 20) )
							{
								item->spellNotifyIcon = true;
							}
						}
					}

					// unlock achievements for special collected items
					switch ( item->type )
					{
						case ARTIFACT_SWORD:
							steamAchievement("BARONY_ACH_KING_ARTHURS_BLADE");
							break;
						case ARTIFACT_MACE:
							steamAchievement("BARONY_ACH_SPUD_LORD");
							break;
						case ARTIFACT_AXE:
							steamAchievement("BARONY_ACH_THANKS_MR_SKELTAL");
							break;
						case ARTIFACT_SPEAR:
							steamAchievement("BARONY_ACH_SPEAR_OF_DESTINY");
							break;
						case KEY_STONE:
						case KEY_BONE:
						case KEY_BRONZE:
						case KEY_IRON:
						case KEY_SILVER:
						case KEY_GOLD:
						case KEY_CRYSTAL:
						case KEY_MACHINE:
							numKeys[item->type]++;
							break;
						default:
							break;
					}

					if ( ticks % TICKS_PER_SECOND == 25 )
					{
						if ( item->identified )
						{
							if ( item->type == SPELL_ITEM )
							{
								if ( auto spell = getSpellFromItem(player, item, true) )
								{
									Compendium_t::Events_t::eventUpdate(player, Compendium_t::CPDM_RUNS_COLLECTED, item->type, 1, false, spell->ID);
								}
							}
							else
							{
								Compendium_t::Events_t::eventUpdate(player, Compendium_t::CPDM_RUNS_COLLECTED, item->type, 1);
								if ( items[item->type].item_slot != ItemEquippableSlot::NO_EQUIP )
								{
									Compendium_t::Events_t::eventUpdate(player, Compendium_t::CPDM_BLESSED_MAX, item->type, item->beatitude);
								}
							}
						}
					}

					if ( item->type == FOOD_BLOOD && stats[player]->playerRace == RACE_VAMPIRE && stats[player]->stat_appearance == 0 )
					{
						bloodCount += item->count;
						if ( bloodCount >= 20 )
						{
							steamAchievement("BARONY_ACH_BLOOD_VESSELS");
						}
					}

					if ( itemCategory(item) == WEAPON || itemCategory(item) == THROWN )
					{
						if ( item->beatitude >= 10 )
						{
							steamAchievement("BARONY_ACH_BLESSED");
						}
					}

					if ( item->status == BROKEN && itemCategory(item) != SPELL_CAT
						&& item->x == Player::PaperDoll_t::ITEM_PAPERDOLL_COORDINATE )
					{
						// item was equipped, but needs a new home in the inventory.
						if ( !players[player]->inventoryUI.moveItemToFreeInventorySlot(item) )
						{
							item->x = players[player]->inventoryUI.getSizeX(); // force unequip below
						}
					}

					if ( item->type == TOOL_DUCK && players[player]->entity )
					{
						if ( item->getDuckPlayer() != player || item->status == BROKEN )
						{
							messagePlayer(player, MESSAGE_INVENTORY, Language::get(6869));
							bool droppedAll = false;
							droppedAll = dropItem(item, player, false, true);
							if ( droppedAll )
							{
								item = nullptr;
								continue;
							}
						}
					}

					// drop any inventory items you don't have room for
					if ( itemCategory(item) != SPELL_CAT 
						&& item->x != Player::PaperDoll_t::ITEM_PAPERDOLL_COORDINATE
						&& item->x != Player::PaperDoll_t::ITEM_RETURN_TO_INVENTORY_COORDINATE
						&& (item->x >= players[player]->inventoryUI.getSizeX() || item->y >= backpack_sizey[player]) )
					{
						messagePlayer(player, MESSAGE_INVENTORY, Language::get(727), item->getName());
						bool droppedAll = false;
						while ( item && item->count > 1 )
						{
							droppedAll = dropItem(item, player);
							if ( droppedAll )
							{
								item = nullptr;
							}
						}
						if ( !droppedAll )
						{
							dropItem(item, player);
						}
					}
					else
					{
						if ( auto_appraise_new_items 
							&& (players[player]->inventoryUI.appraisal.timer == 0 || (ticks % 10 == 0 && updateItemToAppraise) )
							&& !(item->identified) && players[player]->inventoryUI.appraisal.appraisalPossible(item) )
						{
							int appraisal_time = players[player]->inventoryUI.appraisal.getAppraisalTime(item);
							if ( players[player]->inventoryUI.appraisal.appraisalProgressionItems.find(item->uid)
								!= players[player]->inventoryUI.appraisal.appraisalProgressionItems.end() )
							{
								appraisal_time = std::min(appraisal_time, players[player]->inventoryUI.appraisal.appraisalProgressionItems[item->uid]);
							}
							if ( appraisal_time < auto_appraise_lowest_time[player] )
							{
								auto_appraise_target[player] = item;
								auto_appraise_lowest_time[player] = appraisal_time;
							}
						}
					}
				}

				if ( numKeys.size() >= 4 )
				{
					steamAchievement("BARONY_ACH_JANITOR");
				}
			}

			DebugStats.eventsT6 = std::chrono::high_resolution_clock::now();

			if ( kills[SHOPKEEPER] >= 3 )
			{
				steamAchievement("BARONY_ACH_PROFESSIONAL_BURGLAR");
			}
			if ( kills[HUMAN] >= 10 )
			{
				steamAchievement("BARONY_ACH_HOMICIDAL_MANIAC");
			}


			if ( gameModeManager.currentSession.challengeRun.isActive()
				&& (gameModeManager.currentSession.challengeRun.eventType == gameModeManager.currentSession.challengeRun.CHEVENT_KILLS_MONSTERS
					|| gameModeManager.currentSession.challengeRun.eventType == gameModeManager.currentSession.challengeRun.CHEVENT_KILLS_FURNITURE)
				&& gameModeManager.currentSession.challengeRun.numKills >= 0 )
			{
				/*if ( gameStatistics[STATISTICS_TOTAL_KILLS] >= gameModeManager.currentSession.challengeRun.numKills
					&& achievementObserver.playerAchievements[PLAYER_NUM].totalKillsTickUpdate )
				{
					my->setHP(0);
					my->setObituary(Language::get(6152));
					stats[PLAYER_NUM]->killer = KilledBy::FAILED_CHALLENGE;
				}*/
				if ( gameStatistics[STATISTICS_TOTAL_KILLS] >= gameModeManager.currentSession.challengeRun.numKills
					&& achievementObserver.playerAchievements[clientnum].totalKillsTickUpdate )
				{
					if ( !fadeout )
					{
						victory = 100;
						if ( gameModeManager.currentSession.challengeRun.eventType == gameModeManager.currentSession.challengeRun.CHEVENT_KILLS_FURNITURE )
						{
							victory = 101;
						}
						if ( multiplayer == SERVER )
						{
							for ( int c = 1; c < MAXPLAYERS; c++ )
							{
								if ( client_disconnected[c] == true || players[c]->isLocalPlayer() )
								{
									continue;
								}
								strcpy((char*)net_packet->data, "WING");
								net_packet->data[4] = victory;
								net_packet->data[5] = 0;
								net_packet->address.host = net_clients[c - 1].host;
								net_packet->address.port = net_clients[c - 1].port;
								net_packet->len = 6;
								sendPacketSafe(net_sock, -1, net_packet, c - 1);
							}
						}
						movie = true;
						pauseGame(2, 0);
						MainMenu::destroyMainMenu();
						MainMenu::createDummyMainMenu();
						beginFade(MainMenu::FadeDestination::Endgame);
					}
				}
			}
			achievementObserver.playerAchievements[clientnum].totalKillsTickUpdate = false;
		}
		else if ( multiplayer == CLIENT )
		{
			// keep alives
			if ( *cvar_enableKeepAlives && net_packet && net_packet->data )
			{
				if ( ticks % (TICKS_PER_SECOND * 1) == 0 )
				{
					// send a keep alive every second
					strcpy((char*)net_packet->data, "KPAL");
					net_packet->data[4] = clientnum;
					net_packet->address.host = net_server.host;
					net_packet->address.port = net_server.port;
					net_packet->len = 5;
					sendPacketSafe(net_sock, -1, net_packet, 0);
				}
				if ( losingConnection[0] && ticks - client_keepalive[0] == 1 )
				{
					// regained connection
					losingConnection[0] = false;
					messagePlayer(i, MESSAGE_MISC, Language::get(728));
				}
				else if ( !losingConnection[0] && ticks - client_keepalive[0] == TICKS_PER_SECOND * TIMEOUT_WARNING_TIME - 1 )
				{
					// give warning
					losingConnection[0] = true;
					messageLocalPlayers(MESSAGE_MISC, Language::get(729), TIMEOUT_TIME - TIMEOUT_WARNING_TIME);
				}
				else if ( !client_disconnected[c] && ticks - client_keepalive[0] >= TICKS_PER_SECOND * TIMEOUT_TIME - 1 )
				{
					// timeout
					messageLocalPlayers(MESSAGE_MISC, Language::get(730));
					MainMenu::timedOut();
					client_disconnected[0] = true;
				}
				PingNetworkStatus_t::update();
			}

			// animate tiles
			if ( !gamePaused )
			{
				int x, y, z;
				for ( x = 0; x < map.width; x++ )
				{
					for ( y = 0; y < map.height; y++ )
					{
						for ( z = 0; z < MAPLAYERS; z++ )
						{
							int index = z + y * MAPLAYERS + x * MAPLAYERS * map.height;
							if ( animatedtiles[map.tiles[index]] )
							{
								if ( z == 0 )
								{
									// water and lava noises
									if ( ticks % (TICKS_PER_SECOND * 4) == (y + x * map.height) % (TICKS_PER_SECOND * 4) && local_rng.rand() % 3 == 0 )
									{
										int coord = x + y * 1000;
										if ( map.liquidSfxPlayedTiles.find(coord) == map.liquidSfxPlayedTiles.end() )
										{
											if ( lavatiles[map.tiles[index]] )
											{
												// bubbling lava
												playSoundPosLocal(x * 16 + 8, y * 16 + 8, 155, 100);
											}
											else if ( swimmingtiles[map.tiles[index]] )
											{
												// running water
												playSoundPosLocal(x * 16 + 8, y * 16 + 8, 135, 32);
											}
											map.liquidSfxPlayedTiles.insert(coord);
										}
									}

									// lava bubbles
									if ( lavatiles[map.tiles[index]] && !gameloopFreezeEntities )
									{
										if ( ticks % 40 == (y + x * map.height) % 40 && local_rng.rand() % 3 == 0 )
										{
											bool doLavaParticles = *cvar_lava_bubbles_enabled;
											if (doLavaParticles)
											{
												if ( *cvar_lava_use_vismap && !intro )
												{
													if ( x >= 0 && x < map.width && y >= 0 && y < map.height )
													{
														bool anyVismap = false;
														for ( int i = 0; i < MAXPLAYERS; ++i )
														{
															if ( !client_disconnected[i] && players[i]->isLocalPlayer() && cameras[i].vismap[y + x * map.height] )
															{
																anyVismap = true;
																break;
															}
														}
														if ( !anyVismap )
														{
															doLavaParticles = false;
														}
													}
												}
												int c, j = 1 + local_rng.rand() % 2;
												for ( c = 0; c < j && doLavaParticles; c++ )
												{
													Entity* entity = newEntity(42, 1, map.entities, nullptr); //Gib entity.
													entity->behavior = &actGib;
													entity->x = x * 16 + local_rng.rand() % 16;
													entity->y = y * 16 + local_rng.rand() % 16;
													entity->z = 7.5;
                                                    entity->ditheringDisabled = true;
													entity->flags[PASSABLE] = true;
													entity->flags[SPRITE] = true;
													entity->flags[NOUPDATE] = true;
													entity->flags[UPDATENEEDED] = false;
													entity->flags[UNCLICKABLE] = true;
													entity->sizex = 2;
													entity->sizey = 2;
													entity->fskill[3] = 0.01;
													double vel = (local_rng.rand() % 10) / 20.f;
													entity->vel_x = vel * cos(entity->yaw);
													entity->vel_y = vel * sin(entity->yaw);
													entity->vel_z = -.15 - (local_rng.rand() % 15) / 100.f;
													entity->yaw = (local_rng.rand() % 360) * PI / 180.0;
													entity->pitch = (local_rng.rand() % 360) * PI / 180.0;
													entity->roll = (local_rng.rand() % 360) * PI / 180.0;
													if ( multiplayer != CLIENT )
													{
														entity_uids--;
													}
													entity->setUID(-3);
												}
											}
										}
									}
								}
							}
						}
					}
				}
			}

			if ( ticks % TICKS_PER_SECOND == 0 )
			{
				updateGameplayStatisticsInMainLoop();
			}


			for ( int i = 0; i < MAXPLAYERS; ++i )
			{
				gameplayPreferences[i].process();
			}
			updatePlayerConductsInMainLoop();
			Compendium_t::Events_t::updateEventsInMainLoop(clientnum);
			achievementObserver.updatePlayerAchievement(clientnum, AchievementObserver::BARONY_ACH_DAPPER, AchievementObserver::DAPPER_EQUIPMENT_CHECK);

			// ask for entity delete update
			if ( ticks % 4 == 0 && list_Size(map.entities) )
			{
				node_t* nodeToCheck = list_Node(map.entities, ticks % list_Size(map.entities));
				if ( nodeToCheck )
				{
					Entity* entity = (Entity*)nodeToCheck->element;
					if ( entity )
					{
						if ( !entity->flags[NOUPDATE]
							&& entity->behavior != &actCustomPortal
							&& entity->behavior != &actLightSource
							&& entity->getUID() > 0 && entity->getUID() != -2 && entity->getUID() != -3 && entity->getUID() != -4 )
						{
							if (net_packet && net_packet->data) {
								strcpy((char*)net_packet->data, "ENTE");
								net_packet->data[4] = clientnum;
								SDLNet_Write32(entity->getUID(), &net_packet->data[5]);
								net_packet->address.host = net_server.host;
								net_packet->address.port = net_server.port;
								net_packet->len = 9;
								sendPacket(net_sock, -1, net_packet, 0);
							}
						}
					}
				}
			}

			// run world UI entities
			for ( node = map.worldUI->first; node != nullptr; node = nextnode )
			{
				nextnode = node->next;
				entity = (Entity*)node->element;
				if ( entity && !entity->ranbehavior )
				{
					if ( !gamePaused || (multiplayer && !client_disconnected[0]) )
					{
						++entity->ticks;
					}
					if ( entity->behavior != nullptr )
					{
						if ( !gamePaused || (multiplayer && !client_disconnected[0]) )
						{
							(*entity->behavior)(entity);
						}
						if ( entitiesdeleted.first != nullptr )
						{
							entitydeletedself = false;
							for ( node2 = entitiesdeleted.first; node2 != nullptr; node2 = node2->next )
							{
								if ( entity == (Entity*)node2->element )
								{
									//printlog("DEBUG: Entity deleted self, sprite: %d", entity->sprite);
									entitydeletedself = true;
									break;
								}
							}
							if ( entitydeletedself == false )
							{
								entity->ranbehavior = true;
							}
							nextnode = map.worldUI->first;
							list_FreeAll(&entitiesdeleted);
						}
						else
						{
							entity->ranbehavior = true;
							nextnode = node->next;
						}
					}
				}
			}

			// run entity actions
			for ( node = map.entities->first; node != nullptr; node = nextnode )
			{
				nextnode = node->next;
				entity = (Entity*)node->element;
				if ( entity && !entity->ranbehavior )
				{
					if ( !gamePaused || (multiplayer && !client_disconnected[0]) )
					{
						entity->ticks++;
					}
					if ( entity->behavior != nullptr )
					{
						if ( gameloopFreezeEntities
							&& entity->behavior != &actPlayer
							&& entity->behavior != &actDeathGhost
							&& entity->behavior != &actPlayerLimb
							&& entity->behavior != &actHudWeapon
							&& entity->behavior != &actHudShield
							&& entity->behavior != &actHudAdditional
							&& entity->behavior != &actHudAdditional2
							&& entity->behavior != &actHudArrowModel
							&& entity->behavior != &actLeftHandMagic
							&& entity->behavior != &actRightHandMagic
							&& entity->behavior != &actFlame )
						{
							TimerExperiments::updateEntityInterpolationPosition(entity);
							continue;
						}
						if ( !gamePaused || (multiplayer && !client_disconnected[0]) )
						{
							(*entity->behavior)(entity);
							if ( entitiesdeleted.first != NULL )
							{
								entitydeletedself = false;
								for ( node2 = entitiesdeleted.first; node2 != NULL; node2 = node2->next )
								{
									if ( entity == (Entity*)node2->element )
									{
										entitydeletedself = true;
										break;
									}
								}
								if ( entitydeletedself == false )
								{
									entity->ranbehavior = true;
									TimerExperiments::updateEntityInterpolationPosition(entity);
								}
								nextnode = map.entities->first;
								list_FreeAll(&entitiesdeleted);
							}
							else
							{
								entity->ranbehavior = true;
								nextnode = node->next;
								if ( entity->flags[UPDATENEEDED] && !entity->flags[NOUPDATE] )
								{
									// adjust entity position
									if ( ticks - entity->lastupdate <= TICKS_PER_SECOND / 16 )
									{
										// interpolate to new position
										if ( (entity->behavior != &actPlayerLimb && entity->behavior != &actDeathGhostLimb)
											|| entity->skill[2] != clientnum )
										{
											double ox = 0, oy = 0, onewx = 0, onewy = 0;

											// move the bodyparts of these otherwise the limbs will get left behind in this adjustment.
											if ( entity->behavior == &actPlayer || entity->behavior == &actMonster
												|| entity->behavior == &actDeathGhost )
											{
												ox = entity->x;
												oy = entity->y;
												onewx = entity->new_x;
												onewy = entity->new_y;
											}
											entity->x += (entity->new_x - entity->x) / 4;
											entity->y += (entity->new_y - entity->y) / 4;
											if ( entity->behavior != &actArrow )
											{
												// client handles z in actArrow.
												entity->z += (entity->new_z - entity->z) / 4;
											}

											// move the bodyparts of these otherwise the limbs will get left behind in this adjustment.
											if ( entity->behavior == &actPlayer || entity->behavior == &actMonster
												|| entity->behavior == &actDeathGhost )
											{
												for ( Entity *bodypart : entity->bodyparts )
												{
													bodypart->x += entity->x - ox;
													bodypart->y += entity->y - oy;
													bodypart->new_x += entity->new_x - onewx;
													bodypart->new_y += entity->new_y - onewy;
												}
											}
										}
									}
									// dead reckoning
									if ( fabs(entity->vel_x) > 0.0001 || fabs(entity->vel_y) > 0.0001 )
									{
										double ox = 0, oy = 0, onewx = 0, onewy = 0;
										if ( entity->behavior == &actPlayer 
											|| entity->behavior == &actMonster
											|| entity->behavior == &actDeathGhost )
										{
											ox = entity->x;
											oy = entity->y;
											onewx = entity->new_x;
											onewy = entity->new_y;
										}
										real_t dist = clipMove(&entity->x, &entity->y, entity->vel_x, entity->vel_y, entity);
										real_t new_dist = clipMove(&entity->new_x, &entity->new_y, entity->vel_x, entity->vel_y, entity);
										if ( entity->behavior == &actPlayer || entity->behavior == &actMonster
											|| entity->behavior == &actDeathGhost )
										{
											for (Entity *bodypart : entity->bodyparts)
											{
												bodypart->x += entity->x - ox;
												bodypart->y += entity->y - oy;
												bodypart->new_x += entity->new_x - onewx;
												bodypart->new_y += entity->new_y - onewy;
											}
										}
									}
									if ( entity->behavior != &actArrow )
									{
										// client handles z in actArrow.
										entity->z += entity->vel_z;
										entity->new_z += entity->vel_z;
									}

									// rotate to new angles
									double dirYaw = entity->new_yaw - entity->yaw;
									dirYaw = fmod(dirYaw, 2 * PI);
									while ( dirYaw >= PI )
									{
										dirYaw -= PI * 2;
									}
									while ( dirYaw < -PI )
									{
										dirYaw += PI * 2;
									}
									entity->yaw += dirYaw / 3;
									while ( entity->yaw < 0 )
									{
										entity->yaw += 2 * PI;
									}
									while ( entity->yaw >= 2 * PI )
									{
										entity->yaw -= 2 * PI;
									}
									double dirPitch = entity->new_pitch - entity->pitch;
									dirPitch = fmod(dirPitch, 2 * PI);
									while ( dirPitch >= PI )
									{
										dirPitch -= PI * 2;
									}
									while ( dirPitch < -PI )
									{
										dirPitch += PI * 2;
									}
									if ( entity->behavior != &actArrow )
									{
										// client handles pitch in actArrow.
										entity->pitch += dirPitch / 3;
										while ( entity->pitch < 0 )
										{
											entity->pitch += 2 * PI;
										}
										while ( entity->pitch >= 2 * PI )
										{
											entity->pitch -= 2 * PI;
										}
									}
									double dirRoll = entity->new_roll - entity->roll;
									dirRoll = fmod(dirRoll, 2 * PI);
									while ( dirRoll >= PI )
									{
										dirRoll -= PI * 2;
									}
									while ( dirRoll < -PI )
									{
										dirRoll += PI * 2;
									}
									entity->roll += dirRoll / 3;
									while ( entity->roll < 0 )
									{
										entity->roll += 2 * PI;
									}
									while ( entity->roll >= 2 * PI )
									{
										entity->roll -= 2 * PI;
									}

									if ( entity->behavior == &actPlayer && entity->skill[2] != clientnum )
									{
										node_t* tmpNode = nullptr;
										int bodypartNum = 0;
										for ( bodypartNum = 0, tmpNode = entity->children.first; tmpNode; tmpNode = tmpNode->next, bodypartNum++ )
										{
											if ( bodypartNum < 9 )
											{
												continue;
											}
											if ( bodypartNum > 10 )
											{
												break;
											}
											// update the players' head and mask as these will otherwise wait until actPlayer to update their rotation. stops clipping.
											if ( bodypartNum == 9 || bodypartNum == 10 )
											{
												Entity* limb = (Entity*)tmpNode->element;
												if ( limb )
												{
													limb->pitch = entity->pitch;
													limb->yaw = entity->yaw;
												}
											}
										}
									}
								}
								TimerExperiments::updateEntityInterpolationPosition(entity);
							}
						}
						else
						{
							TimerExperiments::updateEntityInterpolationPosition(entity);
						}
					}
				}
			}
			for ( node = map.entities->first; node != nullptr; node = node->next )
			{
				entity = (Entity*)node->element;
				entity->ranbehavior = false;
			}
			for ( node = map.worldUI->first; node != nullptr; node = node->next )
			{
				entity = (Entity*)node->element;
				entity->ranbehavior = false;
			}

			Player::PlayerMechanics_t::ensembleMusicUpdate();

			// world UI
			Player::WorldUI_t::handleTooltips();

			AOEIndicators_t::update();

			auto& playerInventory = players[clientnum]->inventoryUI;
			const int inventorySizeX = playerInventory.getSizeX();
			int backpack_sizey = playerInventory.DEFAULT_INVENTORY_SIZEY;
			if ( stats[clientnum]->cloak && stats[clientnum]->cloak->type == CLOAK_BACKPACK && stats[clientnum]->cloak->status != BROKEN
				&& (shouldInvertEquipmentBeatitude(stats[clientnum]) ? abs(stats[clientnum]->cloak->beatitude) >= 0 : stats[clientnum]->cloak->beatitude >= 0) )
			{
				backpack_sizey += playerInventory.getPlayerBackpackBonusSizeY();
			}

			if ( backpack_sizey == playerInventory.DEFAULT_INVENTORY_SIZEY + playerInventory.getPlayerBackpackBonusSizeY() )
			{
				playerInventory.setSizeY(playerInventory.DEFAULT_INVENTORY_SIZEY + playerInventory.getPlayerBackpackBonusSizeY());
			}
			else
			{
				playerInventory.setSizeY(playerInventory.DEFAULT_INVENTORY_SIZEY);
			}

			int bloodCount = 0;
			std::map<int, int> numKeys;
			players[clientnum]->magic.bHasUnreadNewSpell = false;
			Item* previousAppraiseItem = nullptr;
			bool updateItemToAppraise = false;
			if ( players[clientnum]->inventoryUI.appraisal.current_item != 0 && *cvar_appraisal_auto_switch )
			{
				if ( previousAppraiseItem = uidToItem(players[clientnum]->inventoryUI.appraisal.current_item) )
				{
					if ( previousAppraiseItem->uid != players[clientnum]->inventoryUI.appraisal.manual_appraised_item )
					{
						updateItemToAppraise = true;
					}
				}
			}
			for ( node = stats[clientnum]->inventory.first; node != NULL; node = nextnode )
			{
				nextnode = node->next;
				Item* item = (Item*)node->element;
				if ( !item )
				{
					continue;
				}

				if ( itemCategory(item) == SPELL_CAT )
				{
					if ( item->notifyIcon )
					{
						players[clientnum]->magic.bHasUnreadNewSpell = true;
					}
					item->spellNotifyIcon = false;
					if ( auto spell = getSpellFromItem(clientnum, item, false) )
					{
						if ( stats[clientnum]->getProficiency(spell->skillID) < SKILL_LEVEL_LEGENDARY
							&& spell->difficulty > (stats[clientnum]->getProficiency(spell->skillID) - 20) )
						{
							item->spellNotifyIcon = true;
						}
					}
				}

				// unlock achievements for special collected items
				switch ( item->type )
				{
					case ARTIFACT_SWORD:
						steamAchievement("BARONY_ACH_KING_ARTHURS_BLADE");
						break;
					case ARTIFACT_MACE:
						steamAchievement("BARONY_ACH_SPUD_LORD");
						break;
					case ARTIFACT_AXE:
						steamAchievement("BARONY_ACH_THANKS_MR_SKELTAL");
						break;
					case ARTIFACT_SPEAR:
						steamAchievement("BARONY_ACH_SPEAR_OF_DESTINY");
						break;
					case KEY_STONE:
					case KEY_BONE:
					case KEY_BRONZE:
					case KEY_IRON:
					case KEY_SILVER:
					case KEY_GOLD:
					case KEY_CRYSTAL:
					case KEY_MACHINE:
						numKeys[item->type]++;
						break;
					default:
						break;
				}

				if ( ticks % TICKS_PER_SECOND == 25 )
				{
					if ( item->identified )
					{
						if ( item->type == SPELL_ITEM )
						{
							if ( auto spell = getSpellFromItem(clientnum, item, true) )
							{
								Compendium_t::Events_t::eventUpdate(clientnum, Compendium_t::CPDM_RUNS_COLLECTED, item->type, 1, false, spell->ID);
							}
						}
						else
						{
							Compendium_t::Events_t::eventUpdate(clientnum, Compendium_t::CPDM_RUNS_COLLECTED, item->type, 1);
							if ( items[item->type].item_slot != ItemEquippableSlot::NO_EQUIP )
							{
								Compendium_t::Events_t::eventUpdate(clientnum, Compendium_t::CPDM_BLESSED_MAX, item->type, item->beatitude);
							}
						}
					}
				}

				if ( itemCategory(item) == WEAPON )
				{
					if ( item->beatitude >= 10 )
					{
						steamAchievement("BARONY_ACH_BLESSED");
					}
				}

				if ( item->type == FOOD_BLOOD && stats[clientnum]->playerRace == RACE_VAMPIRE && stats[clientnum]->stat_appearance == 0 )
				{
					bloodCount += item->count;
					if ( bloodCount >= 20 )
					{
						steamAchievement("BARONY_ACH_BLOOD_VESSELS");
					}
				}

				if ( item->status == BROKEN && itemCategory(item) != SPELL_CAT
					&& item->x == Player::PaperDoll_t::ITEM_PAPERDOLL_COORDINATE )
				{
					// item was equipped, but needs a new home in the inventory.
					if ( !players[clientnum]->inventoryUI.moveItemToFreeInventorySlot(item) )
					{
						item->x = players[clientnum]->inventoryUI.getSizeX(); // force unequip below
					}
				}

				if ( item->type == TOOL_DUCK && players[clientnum]->entity )
				{
					if ( item->getDuckPlayer() != clientnum || item->status == BROKEN )
					{
						messagePlayer(clientnum, MESSAGE_INVENTORY, Language::get(6869));
						bool droppedAll = false;
						droppedAll = dropItem(item, clientnum, false, true);
						if ( droppedAll )
						{
							item = nullptr;
							continue;
						}
					}
				}

				// drop any inventory items you don't have room for
				if ( itemCategory(item) != SPELL_CAT 
					&& item->x != Player::PaperDoll_t::ITEM_PAPERDOLL_COORDINATE
					&& item->x != Player::PaperDoll_t::ITEM_RETURN_TO_INVENTORY_COORDINATE
					&& (item->x >= players[clientnum]->inventoryUI.getSizeX() || item->y >= backpack_sizey) )
				{
					messagePlayer(clientnum, MESSAGE_INVENTORY, Language::get(727), item->getName());
					bool droppedAll = false;
					while ( item && item->count > 1 )
					{
						droppedAll = dropItem(item, clientnum);
						if ( droppedAll )
						{
							item = nullptr;
						}
					}
					if ( !droppedAll )
					{
						dropItem(item, clientnum);
					}
				}
				else
				{
					if ( auto_appraise_new_items && 
						(players[clientnum]->inventoryUI.appraisal.timer == 0 || (ticks % 10 == 0 && updateItemToAppraise)) 
						&& !(item->identified) && players[clientnum]->inventoryUI.appraisal.appraisalPossible(item) )
					{
						int appraisal_time = players[clientnum]->inventoryUI.appraisal.getAppraisalTime(item);
						if ( players[clientnum]->inventoryUI.appraisal.appraisalProgressionItems.find(item->uid)
							!= players[clientnum]->inventoryUI.appraisal.appraisalProgressionItems.end() )
						{
							appraisal_time = std::min(appraisal_time, players[clientnum]->inventoryUI.appraisal.appraisalProgressionItems[item->uid]);
						}
						if (appraisal_time < auto_appraise_lowest_time[clientnum])
						{
							auto_appraise_target[clientnum] = item;
							auto_appraise_lowest_time[clientnum] = appraisal_time;
						}
					}
				}
			}

			if ( numKeys.size() >= 4 )
			{
				steamAchievement("BARONY_ACH_JANITOR");
			}

			if ( kills[SHOPKEEPER] >= 3 )
			{
				steamAchievement("BARONY_ACH_PROFESSIONAL_BURGLAR");
			}
			if ( kills[HUMAN] >= 10 )
			{
				steamAchievement("BARONY_ACH_HOMICIDAL_MANIAC");
			}
		}

		// Automatically identify items, shortest time required first
		for ( int i = 0; i < MAXPLAYERS; ++i )
		{
			if ( players[i]->isLocalPlayer() )
			{
				if ( auto_appraise_target[i] != NULL )
				{
					if ( (auto_appraise_target[i]->uid != players[i]->inventoryUI.appraisal.current_item)
						|| players[i]->inventoryUI.appraisal.timer == 0 )
					{
						players[i]->inventoryUI.appraisal.appraiseItem(auto_appraise_target[i]);
					}
				}
			}
		}
    }

    bool playeralive = false;
    for (int c = 0; c < MAXPLAYERS; ++c) {
        if (players[c] && players[c]->entity) {
            playeralive = true;
        }
    }

    // increment gameplay time
	if (!gamePaused && !intro && playeralive)
	{
		++completionTime;
		for ( int c = 0; c < MAXPLAYERS; ++c ) 
		{
			players[c]->compendiumProgress.playerAliveTimeTotal++;
		}
	}
	if ( !gamePaused && !intro )
	{
		for ( int c = 0; c < MAXPLAYERS; ++c )
		{
			players[c]->compendiumProgress.playerGameTimeTotal++;
		}
	}
}

/*-------------------------------------------------------------------------------

	handleButtons

	Draws buttons and processes clicks

-------------------------------------------------------------------------------*/

//int subx1, subx2, suby1, suby2;
//char subtext[1024];

void handleButtons(void)
{
	node_t* node;
	node_t* nextnode;
	button_t* button;
	int w = 0, h = 0;

	Sint32 mousex = inputs.getMouse(clientnum, Inputs::MouseInputs::X);
	Sint32 mousey = inputs.getMouse(clientnum, Inputs::MouseInputs::Y);
	Sint32 omousex = inputs.getMouse(clientnum, Inputs::MouseInputs::OX);
	Sint32 omousey = inputs.getMouse(clientnum, Inputs::MouseInputs::OY);

	// handle buttons
	for ( node = button_l.first; node != NULL; node = nextnode )
	{
		nextnode = node->next;
		if ( node->element == NULL )
		{
			continue;
		}
		button = (button_t*)node->element;
		if ( button == NULL )
		{
			continue;
		}
		if ( !subwindow && button->focused )
		{
			list_RemoveNode(button->node);
			continue;
		}
		//Hide "Random Character" button if not on first character creation step.
		if (!strcmp(button->label, Language::get(733)))
		{
			if (charcreation_step > 1)
			{
				button->visible = 0;
			}
			else
			{
				button->visible = 1;
			}
		}
		//Hide "Random Name" button if not on character naming screen.
		if ( !strcmp(button->label, Language::get(2498)) )
		{
			if ( charcreation_step != 4 )
			{
				button->visible = 0;
			}
			else
			{
				button->visible = 1;
			}
		}
		if ( button->visible == 0 )
		{
			continue;    // invisible buttons are not processed
		}
		getSizeOfText(ttf12, button->label, &w, &h);
		if ( subwindow && !button->focused )
		{
			// unfocused buttons do not work when a subwindow is active
			drawWindow(button->x, button->y, button->x + button->sizex, button->y + button->sizey);
			ttfPrintText(ttf12, button->x + (button->sizex - w) / 2 - 2, button->y + (button->sizey - h) / 2 + 3, button->label);
		}
		else
		{
			if ( keystatus[button->key] && button->key )
			{
				button->pressed = true;
				button->needclick = false;
			}
			if (button->joykey != -1 && inputs.bControllerRawInputPressed(clientnum, button->joykey) && rebindaction == -1 )
			{
				button->pressed = true;
				button->needclick = false;
			}
			if ( inputs.bMouseLeft(clientnum) )
			{
				if ( mousex >= button->x && mousex < button->x + button->sizex && omousex >= button->x && omousex < button->x + button->sizex )
				{
					if ( mousey >= button->y && mousey < button->y + button->sizey && omousey >= button->y && omousey < button->y + button->sizey )
					{
						node_t* node;
						for ( node = button_l.first; node != NULL; node = node->next )
						{
							if ( node->element == NULL )
							{
								continue;
							}
							button_t* button = (button_t*)node->element;
							button->pressed = false;
						}
						button->pressed = true;
					}
					else if ( !keystatus[button->key] )
					{
						button->pressed = false;
					}
				}
				else if ( !keystatus[button->key] )
				{
					button->pressed = false;
				}
				button->needclick = true;
			}
			if ( button->pressed )
			{
				drawDepressed(button->x, button->y, button->x + button->sizex, button->y + button->sizey);
				ttfPrintText(ttf12, button->x + (button->sizex - w) / 2 - 2, button->y + (button->sizey - h) / 2 + 3, button->label);
				if ( !inputs.bMouseLeft(clientnum) && !keystatus[button->key] )
				{
					if ( ( omousex >= button->x && omousex < button->x + button->sizex ) || !button->needclick )
					{
						if ( ( omousey >= button->y && omousey < button->y + button->sizey ) || !button->needclick )
						{
							keystatus[button->key] = false;
							inputs.controllerClearRawInput(clientnum, button->joykey);
							playSound(139, 64);
							if ( button->action != NULL )
							{
								(*button->action)(button); // run the button's assigned action
								if ( deleteallbuttons )
								{
									deleteallbuttons = false;
									button->pressed = false;
									break;
								}
							}
						}
					}
					button->pressed = false;
				}
			}
			else
			{
				drawWindow(button->x, button->y, button->x + button->sizex, button->y + button->sizey);
				ttfPrintText(ttf12, button->x + (button->sizex - w) / 2 - 2, button->y + (button->sizey - h) / 2 + 3, button->label);
			}
		}

		if ( button->outline )
		{
			//Draw golden border.
			//For such things as which settings tab the controller has presently selected.
			Uint32 color = makeColor( 255, 255, 0, 127);
			SDL_Rect pos;
			pos.x = button->x;
			pos.w = button->sizex;
			pos.y = button->y;
			pos.h = button->sizey;
			drawBox(&pos, color, 127);
			//Draw a 2 pixel thick box.
			pos.x = button->x + 1;
			pos.w = button->sizex - 2;
			pos.y = button->y + 1;
			pos.h = button->sizey - 2;
			drawBox(&pos, color, 127);
		}
	}
}

/*-------------------------------------------------------------------------------

	handleEvents

	Handles all SDL events; receives input, updates gamestate, etc.

-------------------------------------------------------------------------------*/

static void bindControllerToPlayer(int id, int player) {
    inputs.removeControllerWithDeviceID(id); // clear any other player using this
    inputs.setControllerID(player, id);
    inputs.getVirtualMouse(player)->draw_cursor = false;
    inputs.getVirtualMouse(player)->lastMovementFromController = true;
    printlog("(Device %d bound to player %d)", id, player);
    for (int c = 0; c < MAX_SPLITSCREEN; ++c) {
        auto& input = Input::inputs[c];
	    input.refresh();
    }
    auto& input = Input::inputs[player];
    if (event.cbutton.button == SDL_CONTROLLER_BUTTON_A)
    {
        input.consumeBinary("MenuConfirm");
    }
    if (event.cbutton.button == SDL_CONTROLLER_BUTTON_B)
    {
        input.consumeBinary("MenuBack");
    }
    if (event.cbutton.button == SDL_CONTROLLER_BUTTON_START)
    {
        input.consumeBinary("MenuStart");
    }
}

bool handleEvents(void)
{
	double d;
	int j;
	int runtimes = 0;

	// calculate app rate
	t = SDL_GetTicks();
	if (ot == 0.0) {
		ot = t;
	}
	real_t timesync = t - ot;
	ot = t;

#ifdef DEBUG_EVENT_TIMERS
	auto time1 = std::chrono::high_resolution_clock::now();
	auto time2 = std::chrono::high_resolution_clock::now();
	real_t accum = 0.0;

	time2 = std::chrono::high_resolution_clock::now();
	accum = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(time2 - time1).count();
	if ( accum > 5 )
	{
		printlog("Large tick time: [0] %f", accum);
	}
	time1 = std::chrono::high_resolution_clock::now();
#endif

	// do timer
	int numframes = 0;
	time_diff += timesync;
	constexpr real_t frame = (real_t)1000 / (real_t)TICKS_PER_SECOND;
	while (time_diff >= frame) {
		time_diff -= frame;
		++numframes;
	}

	// drop excessive frames
	constexpr int max_frames = 5;
	if (numframes > max_frames) {
		//printlog("dropped %d frames", numframes - max_frames);
		numframes = max_frames;
	}

	// calculate fps
	if ( timesync != 0 )
	{
		frameval[cycles % AVERAGEFRAMES] = 1.0 / timesync;
	}
	else
	{
		frameval[cycles % AVERAGEFRAMES] = 1.0;
	}
	d = frameval[0];
	for (j = 1; j < AVERAGEFRAMES; j++)
	{
		d += frameval[j];
	}
	fps = (d / AVERAGEFRAMES) * 1000;

	if (initialized) {
		inputs.updateAllMouse();
	}

#ifdef DEBUG_EVENT_TIMERS
	time2 = std::chrono::high_resolution_clock::now();
	accum = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(time2 - time1).count();
	if ( accum > 5 )
	{
		printlog("Large tick time: [1] %f", accum);
	}
	time1 = std::chrono::high_resolution_clock::now();
#endif

	Input::lastInputOfAnyKind = "";

#ifdef NINTENDO
	// update controllers
	nxControllersUpdate();

	// detect resolution changes
	if (nxHasResolutionChanged()) {
		int x, y;
		nxGetCurrentResolution(x, y);
		printlog("new display size: %d %d", x, y);

		if (!changeVideoMode(x, y)) {
			printlog("critical error! Attempting to abort safely...\n");
			mainloop = 0;
		}
		if (!intro) {
			MainMenu::setupSplitscreen();
		}
	}

	// detect app focus changes
	const bool asleep = nxAppOutOfFocus();
#ifdef USE_EOS
	EOS.SetSleepStatus(asleep);
#endif
	if (asleep) {
		if (!intro && !gamePaused) {
			if (!MainMenu::isMenuOpen() && !MainMenu::isCutsceneActive()) {
				pauseGame(2, 0);
			}
		}
	}
#endif

    // consume mouse buttons that were eaten by GUI
	if (!framesProcResult.usable && *framesEatMouse) {
	    for (int c = 0; c < MAXPLAYERS; ++c) {
	        if (!players[c]->shootmode) {
	            if (Input::inputs[c].consumeBinaryToggle("MenuLeftClick")) {
	                Input::inputs[c].consumeBindingsSharedWithBinding("MenuLeftClick");
	            }
	            if (Input::inputs[c].consumeBinaryToggle("MenuMiddleClick")) {
	                Input::inputs[c].consumeBindingsSharedWithBinding("MenuMiddleClick");
	            }
	            if (Input::inputs[c].consumeBinaryToggle("MenuRightClick")) {
	                Input::inputs[c].consumeBindingsSharedWithBinding("MenuRightClick");
	            }
	        }
	    }
	}

#ifdef DEBUG_EVENT_TIMERS
	time2 = std::chrono::high_resolution_clock::now();
	accum = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(time2 - time1).count();
	if ( accum > 5 )
	{
		printlog("Large tick time: [2] %f", accum);
	}
	time1 = std::chrono::high_resolution_clock::now();
#endif

	// update network state
#if defined(NINTENDO)
	if (initialized && !loading) {
		// update local wireless communication mode
		if (directConnect && multiplayer != SINGLE) {
			if (!nxHandleWireless()) {
				MainMenu::timedOut(); // handle wireless disconnect
			}
			if (multiplayer == SERVER && !intro) {
				if (ticks % TICKS_PER_SECOND == 0) {
					int numplayers = 0;
					for (int c = 0; c < MAXPLAYERS; ++c) {
						if (!client_disconnected[c]) {
							++numplayers;
						}
					}
					char address[64] = { '\0' };
					bool result = false;
					nxGetWirelessAddress(address, sizeof(address));
					if (address[0]) {
						result = nxUpdateLobby(address, MainMenu::getHostname(), svFlags, numplayers);
					}
					if (!result) {
						MainMenu::timedOut();
					}
				}
			}
		}

#ifdef USE_EOS
		// handle EOS timeouts and disconnects
		const bool connected = nxConnectedToNetwork();
		EOS.SetNetworkAvailable(connected);
		if (EOS.isInitialized() && EOS.CurrentUserInfo.isLoggedIn() && EOS.CurrentUserInfo.isValid()) {
			// I don't care if we're in the lobby browser, hosting a lobby, or playing a game.
			// Any state we are in where EOS is connected, we need to end the game immediately if
			// the network is lost. Or else Epic has a freakout.
			if (!connected) {
				MainMenu::timedOut();
			}
		}
#endif // USE_EOS
	}
#endif // NINTENDO

#ifdef DEBUG_EVENT_TIMERS
	time2 = std::chrono::high_resolution_clock::now();
	accum = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(time2 - time1).count();
	if ( accum > 5 )
	{
		printlog("Large tick time: [3] %f", accum);
	}
	time1 = std::chrono::high_resolution_clock::now();
#endif

	int mouseMovementEventLimit = 1000;
	for ( int i = 0; i < MAXPLAYERS; ++i )
	{
		if ( inputs.getPlayerIDAllowedKeyboard() == i && !intro && !gamePaused )
		{
			mouseMovementEventLimit = playerSettings[i].mouse_event_limit_mkb;
			break;
		}
	}

	static std::map<Uint32, int> mouseMotionEventsTimestamp;
	static ConsoleVariable<bool> cvar_debug_mouse_motion("/debug_mouse_motion", false);
	if ( *cvar_debug_mouse_motion )
	{
		int maxMotion = 0;
		for ( auto& val : mouseMotionEventsTimestamp )
		{
			maxMotion = std::max(val.second, maxMotion);
		}
		if ( maxMotion > 1 )
		{
			messagePlayer(clientnum, MESSAGE_HINT, "Max mouse motion events: %d", maxMotion);
		}
	}
	mouseMotionEventsTimestamp.clear();

	while ( SDL_PollEvent(&event) )   // poll SDL events
	{
#ifdef USE_IMGUI
		if ( ImGui_t::isInit )
		{
			ImGui_ImplSDL2_ProcessEvent(&event);
#ifdef DEBUG_EVENT_TIMERS
			time2 = std::chrono::high_resolution_clock::now();
			accum = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(time2 - time1).count();
			if ( accum > 5 )
			{
				printlog("Large tick time: [4] %f", accum);
			}
			time1 = std::chrono::high_resolution_clock::now();
#endif
		}
#endif
		// Global events
		switch ( event.type )
		{
			case SDL_QUIT: // if SDL receives the shutdown signal
				mainloop = 0;
				break;
			case SDL_KEYDOWN: // if a key is pressed...
			    if (demo_mode != DemoMode::STOPPED) {
			        if (event.key.keysym.sym == SDLK_ESCAPE) {
			            demo_stop();
			        }
			        else if (demo_mode == DemoMode::PLAYING) {
			            break;
			        }
			    }
				if ( command )
				{
				    static int saved_command_index = 0;
				    static std::string saved_command_str;
				    if ( event.key.keysym.sym == SDLK_TAB )
				    {
				        if (saved_command_str.empty()) {
				            saved_command_str = command_str;
				        }
				        auto find = FindConsoleCommand(saved_command_str.c_str(), saved_command_index);
				        if (find) {
				            strcpy(command_str, find);
				            ++saved_command_index;
				        } else if (saved_command_index) {
				            saved_command_index = 0;
				            find = FindConsoleCommand(saved_command_str.c_str(), saved_command_index);
				            if (find) {
				                strcpy(command_str, find);
				                ++saved_command_index;
				            }
				        }
				    }
				    else
				    {
				        saved_command_index = 0;
				        saved_command_str.clear();
				    }
					if ( event.key.keysym.sym == SDLK_UP )
					{
						if ( !chosen_command && command_history.last )   //If no command is chosen (user has not tried to go up through the commands yet...
						{
							//Assign the chosen command as the last thing the user typed.
							chosen_command = command_history.last;
							strcpy(command_str, ((string_t*)chosen_command->element)->data);
						}
						else if ( chosen_command )
						{
							//Scroll up through the list. Do nothing if already at the top.
							if ( chosen_command->prev )
							{
								chosen_command = chosen_command->prev;
								strcpy(command_str, ((string_t*)chosen_command->element)->data);
							}
						}
					}
					else if ( event.key.keysym.sym == SDLK_DOWN )
					{
						if ( chosen_command )   //If a command is chosen...
						{
							//Scroll down through the history, back to the latest command.
							if ( chosen_command->next )
							{
								//Move on to the newer command.
								chosen_command = chosen_command->next;
								strcpy(command_str, ((string_t*)chosen_command->element)->data);
							}
							else
							{
								//Already latest command. Clear the chosen command.
								chosen_command = NULL;
								strcpy(command_str, "");
							}
						}
					}
				}
				if ( SDL_IsTextInputActive() )
				{
					const size_t length = strlen(inputstr);

#ifdef APPLE
					if ( (event.key.keysym.sym == SDLK_DELETE || event.key.keysym.sym == SDLK_BACKSPACE) && length > 0 )
					{
						size_t utfOffset = 1;

						if ( length > 1 )
						{
							const uint32_t result = UTFD::ValidateUTF8Character(inputstr[length - 1]);

							if ( result > UTFD::UTF8_REJECT )
							{
								++utfOffset;
							}
						}

						inputstr[length - utfOffset] = '\0';
						cursorflash = ticks;
					}
#else

					if ( event.key.keysym.sym == SDLK_BACKSPACE && length > 0 )
					{
						size_t utfOffset = 1;

						if ( length > 1 )
						{
							const uint32_t result = UTFD::ValidateUTF8Character(inputstr[length - 1]);

							if ( result > UTFD::UTF8_REJECT )
							{
								++utfOffset;
							}
						}

						inputstr[length - utfOffset] = '\0';
						cursorflash = ticks;
					}
#endif
					else if ( event.key.keysym.sym == SDLK_c && SDL_GetModState()&KMOD_CTRL )
					{
						SDL_SetClipboardText(inputstr);
						cursorflash = ticks;
					}
					else if ( event.key.keysym.sym == SDLK_v && SDL_GetModState()&KMOD_CTRL )
					{
					    size_t destlen = strlen(inputstr);
					    size_t remaining = inputlen - destlen - 1;
					    size_t srclen = std::min(strlen(SDL_GetClipboardText()), remaining);
					    memcpy(inputstr + destlen, SDL_GetClipboardText(), srclen);
					    inputstr[destlen + srclen] = '\0';
						cursorflash = ticks;
					}
				}
#ifdef PANDORA
				// Pandora Shoulder as Mouse Button handling
				if ( event.key.keysym.sym == SDLK_RCTRL ) { // L
					mousestatus[SDL_BUTTON_LEFT] = 1; // set this mouse button to 1
					lastkeypressed = 282 + SDL_BUTTON_LEFT;
				}
				else if ( event.key.keysym.sym == SDLK_RSHIFT ) { // R
					mousestatus[SDL_BUTTON_RIGHT] = 1; // set this mouse button to 1
					lastkeypressed = 282 + SDL_BUTTON_RIGHT;
				}
				else
#endif
				{
					lastkeypressed = event.key.keysym.sym;
#ifdef APPLE
                    switch (lastkeypressed)
                    {
                        default: break;
                        case SDLK_NUMLOCKCLEAR: lastkeypressed = SDLK_KP_CLEAR; break;
                        case SDLK_PRINTSCREEN: lastkeypressed = SDLK_F13; break;
                        case SDLK_SCROLLLOCK: lastkeypressed = SDLK_F14; break;
                        case SDLK_PAUSE: lastkeypressed = SDLK_F15; break;
                    }
#endif
					keystatus[lastkeypressed] = true;
					Input::keys[lastkeypressed] = true;
					Input::lastInputOfAnyKind = SDL_GetKeyName(lastkeypressed);
				}
				break;
			case SDL_KEYUP: // if a key is unpressed...
			    if (demo_mode == DemoMode::PLAYING) {
			        break;
			    }
#ifdef PANDORA
				if ( event.key.keysym.sym == SDLK_RCTRL ) { // L
					mousestatus[SDL_BUTTON_LEFT] = 0; // set this mouse button to 0
					lastkeypressed = 282 + SDL_BUTTON_LEFT;
				}
				else if ( event.key.keysym.sym == SDLK_RSHIFT ) { // R
					mousestatus[SDL_BUTTON_RIGHT] = 0; // set this mouse button to 0
					lastkeypressed = 282 + SDL_BUTTON_RIGHT;
				}
				else
#endif
				{
                    SDL_Keycode key = event.key.keysym.sym;
#ifdef APPLE
                    switch (key)
                    {
                        default: break;
                        case SDLK_NUMLOCKCLEAR: key = SDLK_KP_CLEAR; break;
                        case SDLK_PRINTSCREEN: key = SDLK_F13; break;
                        case SDLK_SCROLLLOCK: key = SDLK_F14; break;
                        case SDLK_PAUSE: key = SDLK_F15; break;
                    }
#endif
					keystatus[key] = false;
					Input::keys[key] = false;
				}
				break;
			case SDL_TEXTINPUT:
				if ( (event.text.text[0] != 'c' && event.text.text[0] != 'C') || !(SDL_GetModState()&KMOD_CTRL) )
				{
					if ( (event.text.text[0] != 'v' && event.text.text[0] != 'V') || !(SDL_GetModState()&KMOD_CTRL) )
					{
						strncat(inputstr, event.text.text, std::max<size_t>(0, inputlen - strlen(inputstr)));
						cursorflash = ticks;
					}
				}
				break;
			case SDL_FINGERDOWN: {
				if (demo_mode == DemoMode::PLAYING) {
					break;
				}
				fingerdown = true;
				const int x = event.tfinger.x * xres;
				const int y = event.tfinger.y * yres;
				inputs.setMouse(clientnum, Inputs::MouseInputs::X, x);
				inputs.setMouse(clientnum, Inputs::MouseInputs::Y, y);
				inputs.setMouse(clientnum, Inputs::MouseInputs::OX, x);
				inputs.setMouse(clientnum, Inputs::MouseInputs::OY, y);
				//fingerx = x;
				//fingery = y;
				//ofingerx = x;
				//ofingery = y;
				//mousexrel += event.tfinger.dx * xres;
				//mouseyrel += event.tfinger.dy * yres;
				break;
			}
			case SDL_FINGERUP: {
				if (demo_mode == DemoMode::PLAYING) {
					break;
				}
				const int x = event.tfinger.x * xres;
				const int y = event.tfinger.y * yres;
				inputs.setMouse(clientnum, Inputs::MouseInputs::X, x);
				inputs.setMouse(clientnum, Inputs::MouseInputs::Y, y);
				fingerdown = false;
				//fingerx = x;
				//fingery = y;
				//mousexrel += event.tfinger.dx * xres;
				//mouseyrel += event.tfinger.dy * yres;
				break;
			}
			case SDL_FINGERMOTION: {
				if (demo_mode == DemoMode::PLAYING) {
					break;
				}
				const int x = event.tfinger.x * xres;
				const int y = event.tfinger.y * yres;
				inputs.setMouse(clientnum, Inputs::MouseInputs::X, x);
				inputs.setMouse(clientnum, Inputs::MouseInputs::Y, y);
				//fingerx = x;
				//fingery = y;
				//mousexrel += event.tfinger.dx * xres;
				//mouseyrel += event.tfinger.dy * yres;
				break;
			}
#ifndef NINTENDO
			case SDL_MOUSEBUTTONDOWN: // if a mouse button is pressed...
				if (demo_mode == DemoMode::PLAYING) {
					break;
				}
#ifdef USE_IMGUI
				if ( ImGui_t::requestingMouse() )
				{
					break;
				}
#endif // USE_IMGUI
#ifdef APPLE
                if ((keystatus[SDLK_LCTRL] || keystatus[SDLK_RCTRL]) && event.button.button == SDL_BUTTON_LEFT) {
                    mousestatus[SDL_BUTTON_RIGHT] = 1;
                    Input::mouseButtons[SDL_BUTTON_RIGHT] = 1;
                    Input::lastInputOfAnyKind = "Mouse2";
                    lastkeypressed = 284;
                } else {
                    mousestatus[event.button.button] = 1; // set this mouse button to 1
                    Input::mouseButtons[event.button.button] = 1;
                    Input::lastInputOfAnyKind = std::string("Mouse") + std::to_string(event.button.button);
                    lastkeypressed = 282 + event.button.button;
                }
#else
                mousestatus[event.button.button] = 1; // set this mouse button to 1
                Input::mouseButtons[event.button.button] = 1;
                Input::lastInputOfAnyKind = std::string("Mouse") + std::to_string(event.button.button);
                lastkeypressed = 282 + event.button.button;
#endif
				break;
			case SDL_MOUSEBUTTONUP: // if a mouse button is released...
			    if (demo_mode == DemoMode::PLAYING) {
			        break;
			    }
				mousestatus[event.button.button] = 0; // set this mouse button to 0
				Input::mouseButtons[event.button.button] = 0;
#ifdef APPLE
                mousestatus[SDL_BUTTON_RIGHT] = 0;
                Input::mouseButtons[SDL_BUTTON_RIGHT] = 0;
#endif
				break;
			case SDL_MOUSEWHEEL:
			    if (demo_mode == DemoMode::PLAYING) {
			        break;
			    }
#ifdef USE_IMGUI
				if ( ImGui_t::requestingMouse() )
				{
					break;
				}
#endif // USE_IMGUI
				if ( event.wheel.y > 0 )
				{
					mousestatus[SDL_BUTTON_WHEELUP] = 1;
					Input::mouseButtons[Input::MOUSE_WHEEL_UP] = 1;
					Input::lastInputOfAnyKind = "MouseWheelUp";
					lastkeypressed = 286;
				}
				else if ( event.wheel.y < 0 )
				{
					mousestatus[SDL_BUTTON_WHEELDOWN] = 1;
					Input::mouseButtons[Input::MOUSE_WHEEL_DOWN] = 1;
					Input::lastInputOfAnyKind = "MouseWheelDown";
					lastkeypressed = 287;
				}
				break;
			case SDL_MOUSEMOTION: // if the mouse is moved...
			{
				if ( demo_mode == DemoMode::PLAYING ) {
					break;
				}
				if ( firstmouseevent == true )
				{
					firstmouseevent = false;
					break;
				}
				menuselect = 0;
				bool doMotion = true;
				if ( mouseMovementEventLimit < 1000 )
				{
					mouseMotionEventsTimestamp[event.motion.timestamp]++;
					if ( mouseMotionEventsTimestamp[event.motion.timestamp] >= mouseMovementEventLimit )
					{
						doMotion = false;
					}
				}

				if ( doMotion )
				{
					float factorX;
					float factorY;
					int w1, w2, h1, h2;
					SDL_GL_GetDrawableSize(screen, &w1, &h1);
					SDL_GetWindowSize(screen, &w2, &h2);
					factorX = (float)w1 / w2;
					factorY = (float)h1 / h2;
					mousex = event.motion.x * factorX;
					mousey = event.motion.y * factorY;
					mousexrel += event.motion.xrel;
					mouseyrel += event.motion.yrel;
				}

				//{
				// // debug code for checking checking mouse motions during lag
				//static std::map < Uint32, std::vector<SDL_MouseMotionEvent>> evs;
				//evs[event.motion.timestamp].push_back(event.motion);
				//if ( evs[event.motion.timestamp].size() > 1 )
				//{
				//	static std::map<Uint32, std::map<Sint32, std::map<Sint32, std::map<Sint32, std::map<Sint32, int>>>>> ss;
				//	auto& val = ss[event.motion.timestamp][event.motion.x][event.motion.y][event.motion.xrel][event.motion.yrel];
				//	val++;
				//	if ( evs[event.motion.timestamp].size() >= 2000 )
				//	{
				//		mousexrel -= event.motion.xrel;
				//		mouseyrel -= event.motion.yrel;
				//		messagePlayer(0, MESSAGE_DEBUG, "cleared");
				//	}
				//}
				//if ( evs.size() > 100 )
				//{
				//	size_t m = 0;
				//	for ( auto& v : evs )
				//	{
				//		m = std::max(v.second.size(), m);
				//	}
				//	//if ( m > 25 )
				//	{
				//		for ( auto& v : evs )
				//		{
				//			//if ( v.second.size() > 25 )
				//			{
				//				int netx = 0;
				//				int nety = 0;
				//				int dupes = 0;
				//				std::map<Sint32, std::map<Sint32, std::map<Sint32, std::map<Sint32, int>>>> ss;
				//				for ( auto& s : v.second )
				//				{
				//					ss[s.x][s.y][s.xrel][s.yrel]++;
				//					if ( ss[s.x][s.y][s.xrel][s.yrel] > 1 )
				//					{
				//						++dupes;
				//					}
				//					netx += s.xrel;
				//					nety += s.yrel;
				//				}
				//				if ( dupes > 0 )
				//				{
				//					int rx = 0;
				//					int ry = 0;
				//					SDL_GetRelativeMouseState(&rx, &ry);
				//					messagePlayer(0, MESSAGE_DEBUG, "net x: %d y: %d | dupes: %d | rx: %d ry: %d", netx, nety, dupes, rx, ry);
				//				}
				//			}
				//		}
				//		messagePlayer(0, MESSAGE_DEBUG, "max dupe: %d", m);
				//	}
				//	evs.clear();
				//}
				//}

				if ( initialized )
				{
					for ( int i = 0; i < MAXPLAYERS; ++i )
					{
						if ( gamePaused || intro )
						{
							inputs.getVirtualMouse(i)->lastMovementFromController = false;
						}
						if ( inputs.bPlayerUsingKeyboardControl(i) && (!inputs.hasController(i) || gamePaused) )
						{
							inputs.getVirtualMouse(i)->lastMovementFromController = false;
							if ( !players[i]->shootmode || !players[i]->entity || gamePaused )
							{
								inputs.getVirtualMouse(i)->draw_cursor = true;
							}
						}
					}
				}
				break;
			}
			case SDL_CONTROLLERBUTTONDOWN: // if joystick button is pressed
			{
			    if (demo_mode == DemoMode::PLAYING) {
			        break;
			    }
				//joystatus[event.cbutton.button] = 1; // set this button's index to 1
				lastkeypressed = 301 + event.cbutton.button;
				char buf[32] = "";
				switch (event.cbutton.button) {
				case SDL_CONTROLLER_BUTTON_A: snprintf(buf, sizeof(buf), "Pad%dButtonA", event.cbutton.which); break;
				case SDL_CONTROLLER_BUTTON_B: snprintf(buf, sizeof(buf), "Pad%dButtonB", event.cbutton.which); break;
				case SDL_CONTROLLER_BUTTON_X: snprintf(buf, sizeof(buf), "Pad%dButtonX", event.cbutton.which); break;
				case SDL_CONTROLLER_BUTTON_Y: snprintf(buf, sizeof(buf), "Pad%dButtonY", event.cbutton.which); break;
				case SDL_CONTROLLER_BUTTON_BACK: snprintf(buf, sizeof(buf), "Pad%dButtonBack", event.cbutton.which); break;
				case SDL_CONTROLLER_BUTTON_START: snprintf(buf, sizeof(buf), "Pad%dButtonStart", event.cbutton.which); break;
				case SDL_CONTROLLER_BUTTON_LEFTSTICK: snprintf(buf, sizeof(buf), "Pad%dButtonLeftStick", event.cbutton.which); break;
				case SDL_CONTROLLER_BUTTON_RIGHTSTICK: snprintf(buf, sizeof(buf), "Pad%dButtonRightStick", event.cbutton.which); break;
				case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: snprintf(buf, sizeof(buf), "Pad%dButtonLeftBumper", event.cbutton.which); break;
				case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: snprintf(buf, sizeof(buf), "Pad%dButtonRightBumper", event.cbutton.which); break;
				case SDL_CONTROLLER_BUTTON_DPAD_LEFT: snprintf(buf, sizeof(buf), "Pad%dDpadX-", event.cbutton.which); break;
				case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: snprintf(buf, sizeof(buf), "Pad%dDpadX+", event.cbutton.which); break;
				case SDL_CONTROLLER_BUTTON_DPAD_UP: snprintf(buf, sizeof(buf), "Pad%dDpadY-", event.cbutton.which); break;
				case SDL_CONTROLLER_BUTTON_DPAD_DOWN: snprintf(buf, sizeof(buf), "Pad%dDpadY+", event.cbutton.which); break;
				}
				Input::lastInputOfAnyKind = buf;
			    if ( event.cbutton.button == SDL_CONTROLLER_BUTTON_A ||
			         event.cbutton.button == SDL_CONTROLLER_BUTTON_B ||
			         event.cbutton.button == SDL_CONTROLLER_BUTTON_START )
			    {
				    SDL_GameController* pad = SDL_GameControllerFromInstanceID(event.cbutton.which);
				    if ( !pad )
				    {
					    printlog("(Unknown pad pressed input (instance: %d), null controller returned.)\n", event.cbutton.which);
				    }
				    else
				    {
					    for ( auto& controller : game_controllers )
					    {
						    if ( controller.isActive() && controller.getControllerDevice() == pad )
						    {
			                    if ( Input::waitingToBindControllerForPlayer >= 0 )
			                    {
			                        // we are explicitly waiting to bind a controller to a specific player
			                        bindControllerToPlayer(controller.getID(), Input::waitingToBindControllerForPlayer);
								    Input::waitingToBindControllerForPlayer = -1;
							    }
							    else
							    {
							        // we are not strictly waiting to bind a controller to a player, but check if this one is already bound
							        bool alreadyBound = false;
							        for (int player = 0; player < MAXPLAYERS; ++player)
							        {
							            if (inputs.getControllerID(player) == controller.getID())
							            {
							                alreadyBound = true;
							                break;
							            }
							        }
							        if (!alreadyBound)
							        {
							            // controller is not already bound - bind it to the first player who does not have a controller
							            for (int player = 0; player < MAXPLAYERS; ++player)
							            {
							                if (multiplayer != SINGLE && player != 0)
							                {
							                    // only assign the controller to the first player in net lobbies
												// do this even for clients: player 1 controls route to other slots
												// in online multiplayer!
							                    continue;
							                }
											if (intro && multiplayer != CLIENT && MainMenu::isPlayerSlotLocked(player))
											{
												// don't assign the controller to this player if their slot is locked!
												continue;
											}
							                if (intro && multiplayer != CLIENT && MainMenu::isPlayerSignedIn(player))
							                {
							                    // in the lobby, don't assign controllers to players who are already signed in...
							                    continue;
							                }
							                if (inputs.hasController(player))
							                {
							                    // this player already has a controller
							                    continue;
							                }
							                if (!intro && inputs.getPlayerIDAllowedKeyboard() == player && (splitscreen || multiplayer != SINGLE))
							                {
							                    // this player is using a keyboard
							                    continue;
							                }
		                                    bindControllerToPlayer(controller.getID(), player);
		                                    alreadyBound = true;
						                    break;
							            }
							            if (!alreadyBound) {
							                // didn't find anybody to bind to. try again, but ignore the keyboard restriction
							                for (int player = 0; player < MAXPLAYERS; ++player)
							                {
												if (multiplayer != SINGLE && player != 0)
												{
													// only assign the controller to the first player in net lobbies
													// do this even for clients: player 1 controls route to other slots
													// in online multiplayer!
													continue;
												}
												if (intro && multiplayer != CLIENT && MainMenu::isPlayerSlotLocked(player))
												{
													// don't assign the controller to this player if their slot is locked!
													continue;
												}
							                    if (intro && multiplayer != CLIENT && MainMenu::isPlayerSignedIn(player))
							                    {
							                        // in the lobby, don't assign controllers to players who are already signed in...
							                        continue;
							                    }
							                    if (inputs.hasController(player))
							                    {
							                        // this player already has a controller
							                        continue;
							                    }
		                                        bindControllerToPlayer(controller.getID(), player);
		                                        alreadyBound = true;
						                        break;
							                }
							            }
							        }
							    }
							    break;
						    }
					    }
					}
				}
				break;
			}
			case SDL_CONTROLLERAXISMOTION:
			{
			    if (demo_mode == DemoMode::PLAYING) {
			        break;
			    }
				char buf[32] = "";
				float rebindingDeadzone = Input::getJoystickRebindingDeadzone() * 32768.f;
				switch (event.caxis.axis) {
				case SDL_CONTROLLER_AXIS_LEFTX: 
					if ( event.caxis.value < -rebindingDeadzone )
					{
						snprintf(buf, sizeof(buf), "Pad%dStickLeftX-", event.caxis.which);
					}
					else if ( event.caxis.value > rebindingDeadzone )
					{
						snprintf(buf, sizeof(buf), "Pad%dStickLeftX+", event.caxis.which);
					}
					break;
				case SDL_CONTROLLER_AXIS_LEFTY: 
					if ( event.caxis.value < -rebindingDeadzone )
					{
						snprintf(buf, sizeof(buf), "Pad%dStickLeftY-", event.caxis.which);
					}
					else if ( event.caxis.value > rebindingDeadzone )
					{
						snprintf(buf, sizeof(buf), "Pad%dStickLeftY+", event.caxis.which);
					}
					break;
				case SDL_CONTROLLER_AXIS_RIGHTX: 
					if ( event.caxis.value < -rebindingDeadzone )
					{
						snprintf(buf, sizeof(buf), "Pad%dStickRightX-", event.caxis.which);
					}
					else if ( event.caxis.value > rebindingDeadzone )
					{
						snprintf(buf, sizeof(buf), "Pad%dStickRightX+", event.caxis.which);
					}
					break;
				case SDL_CONTROLLER_AXIS_RIGHTY:
					if ( event.caxis.value < -rebindingDeadzone )
					{
						snprintf(buf, sizeof(buf), "Pad%dStickRightY-", event.caxis.which);
					}
					else if ( event.caxis.value > rebindingDeadzone )
					{
						snprintf(buf, sizeof(buf), "Pad%dStickRightY+", event.caxis.which);
					}
					break;
				case SDL_CONTROLLER_AXIS_TRIGGERLEFT: 
					if ( abs(event.caxis.value) > rebindingDeadzone )
					{
						snprintf(buf, sizeof(buf), "Pad%dLeftTrigger", event.caxis.which); 
					}
					break;
				case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: 
					if ( abs(event.caxis.value) > rebindingDeadzone )
					{
						snprintf(buf, sizeof(buf), "Pad%dRightTrigger", event.caxis.which); 
					}
					break;
				}
				if ( strcmp(buf, "") )
				{
					Input::lastInputOfAnyKind = buf;
				}
				break;
			}
			case SDL_CONTROLLERBUTTONUP:
			{
			    if (demo_mode == DemoMode::PLAYING) {
			        break;
			    }
			    break;
			}
			case SDL_CONTROLLERDEVICEADDED:
			{
			    if (demo_mode == DemoMode::PLAYING) {
			        break;
			    }
				const int sdl_device_index = event.cdevice.which; // this is an index within SDL_Numjoysticks(), not to be referred to from now on.
				if ( !SDL_IsGameController(sdl_device_index) )
				{
					printlog("Info: device %d is not a game controller! Joysticks are not supported.\n", sdl_device_index);
					break;
				}

				bool deviceAlreadyAdded = false;
				SDL_GameController* newControllerAdded = SDL_GameControllerOpen(sdl_device_index);
				if ( newControllerAdded != nullptr )
				{
					for ( auto& controller : game_controllers )
					{
						if ( controller.isActive() )
						{
							if ( controller.getControllerDevice() == newControllerAdded )
							{
								// we already have this controller in our system.
								deviceAlreadyAdded = true;
								printlog("(Device %d added, but already in use as game controller.)\n", sdl_device_index);
								break;
							}
						}
					}
				}

				if ( newControllerAdded )
				{
					SDL_GameControllerClose(newControllerAdded); // we're going to re-open this below..
					newControllerAdded = nullptr;
				}

				if ( deviceAlreadyAdded )
				{
					break;
				}

				// now find a free controller slot.
                int id = -1;
				for ( int c = 0; c < game_controllers.size(); ++c )
				{
					auto& controller = game_controllers[c];
					if ( controller.isActive() )
					{
						continue;
					}

                    id = c;
					bool result = controller.open(sdl_device_index, id);
					assert(result); // this should always succeed because we test that the device index is valid above.
					printlog("Device %d successfully initialized as game controller in slot %d.\n", sdl_device_index, id);
					controller.initBindings();
					Input::gameControllers[id] = controller.getControllerDevice();
					for (int c = 0; c < MAX_SPLITSCREEN; ++c) {
						Input::inputs[c].refresh();
					}
					break;
				}
				for ( auto& controller : game_controllers )
				{
					// haptic devices are enumerated differently than joysticks
					// reobtain haptic devices for each existing controller
					controller.reinitHaptic();
				}
#ifdef STEAMWORKS
                // on steam deck, player 1 always needs a controller.
                if (SteamUtils()->IsSteamRunningOnSteamDeck()) {
                    if (id >= 0 && !inputs.hasController(0)) {
                        bindControllerToPlayer(id, 0);
                    }
                }
#endif
				break;
			}
			case SDL_CONTROLLERDEVICEREMOVED:
			{
			    if (demo_mode == DemoMode::PLAYING) {
			        break;
			    }
				// device removed uses a 'joystick id', different to the device added event
				const int instanceID = event.cdevice.which;
				SDL_GameController* pad = SDL_GameControllerFromInstanceID(instanceID);
				if ( !pad )
				{
					printlog("(Unknown device removed as game controller, null controller returned.)\n");
				}

				for ( auto& controller : game_controllers )
				{
					if ( controller.isActive() && controller.getControllerDevice() == pad )
					{
						const int id = controller.getID();
						inputs.removeControllerWithDeviceID(id);
						printlog("Device %d removed as game controller (it was in slot %d).\n", instanceID, id);
						controller.close();
						Input::gameControllers.erase(id);
						for ( int c = 0; c < MAX_SPLITSCREEN; ++c ) {
							Input::inputs[c].refresh();
						}
					}
				}
				for ( auto& controller : game_controllers )
				{
					// haptic devices are enumerated differently than joysticks
					// reobtain haptic devices for each existing controller
					controller.reinitHaptic();
				}
				break;
			}
			case SDL_JOYDEVICEADDED:
			{
			    if (demo_mode == DemoMode::PLAYING) {
			        break;
			    }
				if ( SDL_IsGameController(event.jdevice.which) )
				{
					// this is supported by the SDL_GameController interface, no need to make a joystick for it
					break;
				}
				SDL_Joystick* joystick = SDL_JoystickOpen(event.jdevice.which);
				if (!joystick) {
					printlog("A joystick was plugged in, but no handle is available!");
				} else {
					Input::joysticks[event.jdevice.which] = joystick;
					printlog("Added joystick '%s' with device index (%d)", SDL_JoystickName(joystick), event.jdevice.which);
					printlog(" NumAxes: %d", SDL_JoystickNumAxes(joystick));
					printlog(" NumButtons: %d", SDL_JoystickNumButtons(joystick));
					printlog(" NumHats: %d", SDL_JoystickNumHats(joystick));
					for (int c = 0; c < MAX_SPLITSCREEN; ++c) {
						Input::inputs[c].refresh();
					}
				}
				break;
			}
			case SDL_JOYBUTTONDOWN:
			{
			    if (demo_mode == DemoMode::PLAYING) {
			        break;
			    }
				if ( Input::joysticks.find(event.jdevice.which) != Input::joysticks.end() )
				{
					char buf[32] = "";
					snprintf(buf, sizeof(buf), "Joy%dButton%d", event.jbutton.which, event.jbutton.button);
					Input::lastInputOfAnyKind = buf;
				}
				break;
			}
			case SDL_JOYAXISMOTION:
			{
			    if (demo_mode == DemoMode::PLAYING) {
			        break;
			    }
				if ( Input::joysticks.find(event.jdevice.which) != Input::joysticks.end() )
				{
					char buf[32] = "";
					float rebindingDeadzone = Input::getJoystickRebindingDeadzone() * 32768.f;
					if ( event.jaxis.value < -rebindingDeadzone )
					{
						snprintf(buf, sizeof(buf), "Joy%dAxis-%d", event.jaxis.which, event.jaxis.axis);
					}
					else if ( event.jaxis.value > rebindingDeadzone )
					{
						snprintf(buf, sizeof(buf), "Joy%dAxis+%d", event.jaxis.which, event.jaxis.axis);
					}
					if ( strcmp(buf, "") )
					{
						Input::lastInputOfAnyKind = buf;
					}
				}
				break;
			}
			case SDL_JOYHATMOTION:
			{
			    if (demo_mode == DemoMode::PLAYING) {
			        break;
			    }
				if ( Input::joysticks.find(event.jdevice.which) != Input::joysticks.end() )
				{
					char buf[32] = "";
					switch (event.jhat.value) {
					case SDL_HAT_LEFTUP: snprintf(buf, sizeof(buf), "Joy%dHat%dLeftUp", event.jhat.which, event.jhat.hat); break;
					case SDL_HAT_UP: snprintf(buf, sizeof(buf), "Joy%dHat%dUp", event.jhat.which, event.jhat.hat); break;
					case SDL_HAT_RIGHTUP: snprintf(buf, sizeof(buf), "Joy%dHat%dRightUp", event.jhat.which, event.jhat.hat); break;
					case SDL_HAT_RIGHT: snprintf(buf, sizeof(buf), "Joy%dHat%dRight", event.jhat.which, event.jhat.hat); break;
					case SDL_HAT_RIGHTDOWN: snprintf(buf, sizeof(buf), "Joy%dHat%dRightDown", event.jhat.which, event.jhat.hat); break;
					case SDL_HAT_DOWN: snprintf(buf, sizeof(buf), "Joy%dHat%dDown", event.jhat.which, event.jhat.hat); break;
					case SDL_HAT_LEFTDOWN: snprintf(buf, sizeof(buf), "Joy%dHat%dLeftDown", event.jhat.which, event.jhat.hat); break;
					case SDL_HAT_LEFT: snprintf(buf, sizeof(buf), "Joy%dHat%dLeft", event.jhat.which, event.jhat.hat); break;
					case SDL_HAT_CENTERED: snprintf(buf, sizeof(buf), "Joy%dHat%dCentered", event.jhat.which, event.jhat.hat); break;
					}
					Input::lastInputOfAnyKind = buf;
				}
				break;
			}
			case SDL_JOYDEVICEREMOVED:
			{
			    if (demo_mode == DemoMode::PLAYING) {
			        break;
			    }
				if ( SDL_IsGameController(event.jdevice.which) )
				{
					// this is supported by the SDL_GameController interface, no need to make a joystick for it
					break;
				}
				SDL_Joystick* joystick = SDL_JoystickFromInstanceID(event.jdevice.which);
				if (joystick == nullptr) {
					printlog("A joystick was removed, but I don't know which one!");
				} else {
					int index = -1;
					for (auto& pair : Input::joysticks) {
						SDL_Joystick* curr = pair.second;
						if (joystick == curr) {
							SDL_JoystickClose(curr);
							index = pair.first;
							printlog("Removed joystick with device index (%d), instance id (%d)", index, event.jdevice.which);
							Input::joysticks.erase(index);
							for ( int c = 0; c < MAX_SPLITSCREEN; ++c ) {
								Input::inputs[c].refresh();
							}
							break;
						}
					}
					if (index >= 0) {
						Input::joysticks.erase(index);
					}
				}
				break;
			}
#endif
			case SDL_WINDOWEVENT:
				if ( event.window.event == SDL_WINDOWEVENT_FOCUS_LOST && mute_audio_on_focus_lost )
				{
				    setGlobalVolume(0.f, 0.f, 0.f, 0.f, 0.f, 0.f);
				}
				else if ( event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED )
				{
				    setGlobalVolume(MainMenu::master_volume,
				        musvolume,
				        sfxvolume,
				        sfxAmbientVolume,
				        sfxEnvironmentVolume,
						sfxNotificationVolume);
				}
				else if (event.window.event == SDL_WINDOWEVENT_RESIZED)
				{
#if defined(NINTENDO)
					if (!changeVideoMode(event.window.data1, event.window.data2))
					{
						printlog("critical error! Attempting to abort safely...\n");
						mainloop = 0;
					}
					if (!intro) {
						MainMenu::setupSplitscreen();
					}
#else
                    float factorX, factorY;
                    {
                        int w1, w2, h1, h2;
                        SDL_GL_GetDrawableSize(screen, &w1, &h1);
                        SDL_GetWindowSize(screen, &w2, &h2);
                        factorX = (float)w1 / w2;
                        factorY = (float)h1 / h2;
                    }
                    const int x = event.window.data1 * factorX;
                    const int y = event.window.data2 * factorY;
					if (!resizeWindow(x, y))
					{
						printlog("critical error! Attempting to abort safely...\n");
						mainloop = 0;
					}
#endif
				}
				break;
		}

#ifdef DEBUG_EVENT_TIMERS
		time2 = std::chrono::high_resolution_clock::now();
		accum = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(time2 - time1).count();
		if ( accum > 5 )
		{
			printlog("Large tick time: [5] event: %d %f", event.type, accum);
		}
		time1 = std::chrono::high_resolution_clock::now();
#endif
	}

#ifdef DEBUG_EVENT_TIMERS
	time2 = std::chrono::high_resolution_clock::now();
	accum = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(time2 - time1).count();
	if ( accum > 5 )
	{
		printlog("Large tick time: [6] %f", accum);
	}
	time1 = std::chrono::high_resolution_clock::now();
#endif

	if (numframes)
	{
#ifdef SOUND
		// update listener position, music fades, etc
		if (multiplayer != SINGLE || intro) {
			sound_update(0, clientnum, 1);
		} else {
			int numplayers = 0;
			for (int c = 0; c < MAXPLAYERS; ++c) {
				if (!client_disconnected[c]) {
					++numplayers;
				}
			}
			for (int player = 0, c = 0; c < MAXPLAYERS; ++c) {
				if (!client_disconnected[c]) {
					sound_update(player, c, numplayers);
					++player;
				}
			}
		}
#endif
	}

#ifdef DEBUG_EVENT_TIMERS
	time2 = std::chrono::high_resolution_clock::now();
	accum = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(time2 - time1).count();
	if ( accum > 5 )
	{
		printlog("Large tick time: [7] %f", accum);
	}
	time1 = std::chrono::high_resolution_clock::now();
#endif

	for (int runtimes = 0; runtimes < numframes; ++runtimes)
	{
		if (!loading && initialized)
		{
			gameLogic();
			++ticks;
		}
		else
		{
			++loadingticks;
		}
	}

#ifdef DEBUG_EVENT_TIMERS
	time2 = std::chrono::high_resolution_clock::now();
	accum = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(time2 - time1).count();
	if ( accum > 5 )
	{
		printlog("Large tick time: [8] %f", accum);
	}
	time1 = std::chrono::high_resolution_clock::now();
#endif

	if (initialized)
	{
		if ( !mousestatus[SDL_BUTTON_LEFT] )
		{
			omousex = mousex;
			omousey = mousey;
		}
		inputs.updateAllOMouse();
		for ( auto& controller : game_controllers )
		{
			controller.updateButtons();
			controller.updateAxis();
		}
	}

#ifdef DEBUG_EVENT_TIMERS
	time2 = std::chrono::high_resolution_clock::now();
	accum = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(time2 - time1).count();
	if ( accum > 5 )
	{
		printlog("Large tick time: [9] %f", accum);
	}
	time1 = std::chrono::high_resolution_clock::now();
#endif

	return numframes > 0;
}

/*-------------------------------------------------------------------------------

	pauseGame

	pauses or unpauses the game, depending on its current state

-------------------------------------------------------------------------------*/

void pauseGame(int mode /* 0 == toggle, 1 == force unpause, 2 == force pause */, int ignoreplayer /* ignored */)
{
	int c;

	if ( intro )
	{
		return;
	}
	if ( mode == 1 && !gamePaused )
	{
		return;
	}
	if ( mode == 2 && gamePaused )
	{
		return;
	}
	if ( MainMenu::isCutsceneActive() )
	{
		return;
	}

	if ( (!gamePaused && mode != 1) || mode == 2 )
	{
	    playSound(500, 96);
		gamePaused = true;
		bool noOneUsingKeyboard = true;
		for (int c = 0; c < MAX_SPLITSCREEN; ++c)
		{
		    if (inputs.bPlayerUsingKeyboardControl(c) && MainMenu::isPlayerSignedIn(c) && players[c]->isLocalPlayer()) {
		        noOneUsingKeyboard = false;
		    }
		    auto& input = Input::inputs[c];
			if (input.binary("Pause Game") || (inputs.bPlayerUsingKeyboardControl(c) && keystatus[SDLK_ESCAPE] && !input.isDisabled())) {
			    MainMenu::pause_menu_owner = c;
			    break;
			}
		}
		if (noOneUsingKeyboard && keystatus[SDLK_ESCAPE]) {
		    MainMenu::pause_menu_owner = clientnum;
		}
		if ( SDL_GetRelativeMouseMode() )
		{
			SDL_SetRelativeMouseMode(SDL_FALSE);
		}
		if (keystatus[SDLK_ESCAPE]) {
			keystatus[SDLK_ESCAPE] = 0;
		}
		return; // doesn't disable the game in multiplayer anymore
		if ( multiplayer == SERVER )
		{
			for ( c = 1; c < MAXPLAYERS; c++ )
			{
				if ( client_disconnected[c] || ignoreplayer == c )
				{
					continue;
				}
				if (net_packet && net_packet->data) {
					strcpy((char*)net_packet->data, "PAUS");
					net_packet->data[4] = clientnum;
					net_packet->address.host = net_clients[c - 1].host;
					net_packet->address.port = net_clients[c - 1].port;
					net_packet->len = 5;
					sendPacketSafe(net_sock, -1, net_packet, c - 1);
				}
			}
		}
		else if ( multiplayer == CLIENT && ignoreplayer )
		{
			if (net_packet && net_packet->data) {
				strcpy((char*)net_packet->data, "PAUS");
				net_packet->data[4] = clientnum;
				net_packet->address.host = net_server.host;
				net_packet->address.port = net_server.port;
				net_packet->len = 5;
				sendPacketSafe(net_sock, -1, net_packet, 0);
			}
		}
	}
	else if ( (gamePaused && mode != 2) || mode == 1 )
	{
		buttonCloseSubwindow(NULL);
		gamePaused = false;
		if ( !SDL_GetRelativeMouseMode() && capture_mouse )
		{
            // fix for macOS: put mouse back in window before recapturing mouse
            if (EnableMouseCapture) {
                int mouse_x, mouse_y;
                SDL_GetGlobalMouseState(&mouse_x, &mouse_y);
                int x, y, w, h;
                SDL_GetWindowPosition(screen, &x, &y);
                SDL_GetWindowSize(screen, &w, &h);
                if (mouse_x < x || mouse_x >= x + w ||
                    mouse_y < y || mouse_y >= y + h) {
                    SDL_WarpMouseInWindow(screen, w/2, h/2);
                }
            }
			SDL_SetRelativeMouseMode(EnableMouseCapture);
		}
		if (keystatus[SDLK_ESCAPE]) {
			keystatus[SDLK_ESCAPE] = 0;
		}
		return; // doesn't disable the game in multiplayer anymore
		if ( multiplayer == SERVER )
		{
			for ( c = 1; c < MAXPLAYERS; c++ )
			{
				if ( client_disconnected[c] || ignoreplayer == c )
				{
					continue;
				}
				if (net_packet && net_packet->data) {
					strcpy((char*)net_packet->data, "UNPS");
					net_packet->data[4] = clientnum;
					net_packet->address.host = net_clients[c - 1].host;
					net_packet->address.port = net_clients[c - 1].port;
					net_packet->len = 5;
					sendPacketSafe(net_sock, -1, net_packet, c - 1);
				}
			}
		}
		else if ( multiplayer == CLIENT && ignoreplayer )
		{
			if (net_packet && net_packet->data) {
				strcpy((char*)net_packet->data, "UNPS");
				net_packet->data[4] = clientnum;
				net_packet->address.host = net_server.host;
				net_packet->address.port = net_server.port;
				net_packet->len = 5;
				sendPacketSafe(net_sock, -1, net_packet, 0);
			}
		}
	}
}

/*-------------------------------------------------------------------------------

	frameRateLimit

	Returns true until the correct number of frames has passed from the
	beginning of the last cycle in the main loop.

-------------------------------------------------------------------------------*/

// records the SDL_GetTicks() value at the moment the mainloop restarted
static Uint64 lastGameTickCount = 0;
static Uint64 framerateAccumulatedTicks = 0;

// credit "computerBear" for preciseSleep/preciseSleepWindows https://blat-blatnik.github.io/computerBear/making-accurate-sleep-function/
void preciseSleep(double seconds) 
{
	static double estimate = 5e-3;
	static double mean = 5e-3;
	static double m2 = 0;
	static int64_t count = 1;

	while ( seconds > estimate ) {
		auto start = std::chrono::high_resolution_clock::now();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		auto end = std::chrono::high_resolution_clock::now();

		double observed = (end - start).count() / 1e9;
		seconds -= observed;

		++count;
		double delta = observed - mean;
		mean += delta / count;
		m2 += delta * (observed - mean);
		double stddev = sqrt(m2 / (count - 1));
		estimate = mean + stddev;
	}

	// spin lock
	auto start = std::chrono::high_resolution_clock::now();
	while ( (std::chrono::high_resolution_clock::now() - start).count() / 1e9 < seconds );
}

void preciseSleepWindows(double seconds)
{
#ifdef WINDOWS
	static HANDLE timer = CreateWaitableTimer(NULL, FALSE, NULL);
	static double estimate = 5e-3;
	static double mean = 5e-3;
	static double m2 = 0;
	static int64_t count = 1;

	while ( seconds - estimate > 1e-7 ) {
		double toWait = seconds - estimate;
		LARGE_INTEGER due;
		due.QuadPart = -int64_t(toWait * 1e7);
		auto start = std::chrono::high_resolution_clock::now();
		SetWaitableTimerEx(timer, &due, 0, NULL, NULL, NULL, 0);
		WaitForSingleObject(timer, INFINITE);
		auto end = std::chrono::high_resolution_clock::now();

		double observed = (end - start).count() / 1e9;
		seconds -= observed;

		++count;
		double error = observed - toWait;
		double delta = error - mean;
		mean += delta / count;
		m2 += delta * (error - mean);
		double stddev = sqrt(m2 / (count - 1));
		estimate = mean + stddev;
	}

	// spin lock
	auto start = std::chrono::high_resolution_clock::now();
	while ( (std::chrono::high_resolution_clock::now() - start).count() / 1e9 < seconds );
#endif // WINDOWS
}

bool frameRateLimit( Uint32 maxFrameRate, bool resetAccumulator, bool sleep )
{
	if ( maxFrameRate == 0 )
	{
		return false;
	}
	float desiredFrameSeconds = 1.0f / maxFrameRate;
	Uint64 gameTickCount = SDL_GetPerformanceCounter();
	Uint64 ticksPerSecond = SDL_GetPerformanceFrequency();
	Uint64 ticksElapsed = gameTickCount - lastGameTickCount;
	lastGameTickCount = gameTickCount;
	framerateAccumulatedTicks += ticksElapsed;

	const float accumulatedSeconds = framerateAccumulatedTicks / (float)ticksPerSecond;
	const float diff = desiredFrameSeconds - accumulatedSeconds;

    static ConsoleVariable<bool> allowSleep("/timer_sleep_enabled", true,
        "allow main thread to sleep between ticks (saves power)");
	if ( diff >= 0.f )
	{
	    // we have not passed a full frame, so we must delay.
        if ( *allowSleep && sleep )
        {
            // sleep a fraction of the remaining time.
            // This saves power if you're running on battery.
#if 1
			auto microseconds = std::chrono::microseconds((Uint64)(diff * 1000000));
			preciseSleep(microseconds.count() / 1e6);
#else
            static ConsoleVariable<float> sleepLimit("/timer_sleep_limit", 0.001f);
            static ConsoleVariable<float> sleepFactor("/timer_sleep_factor", 0.97f);
            if ( diff >= *sleepLimit )
            {
				std::this_thread::sleep_for(std::chrono::microseconds((Uint64)(diff * 1000000 * (*sleepFactor))));
            }
#endif
        }
		return true;
	}
	else
	{
		if ( resetAccumulator )
		{
			framerateAccumulatedTicks = 0;
		}
		return false;
	}
}

void ingameHud()
{
	for ( int player = 0; player < MAXPLAYERS; ++player )
	{
		if ( !players[player]->isLocalPlayer() )
		{
			continue;
		}
		if ( players[player]->isLocalPlayerAlive() )
		{
			players[player]->bControlEnabled = true;
		}
		else if ( players[player]->ghost.isActive() )
		{
			players[player]->bControlEnabled = true;
		}
#ifdef USE_IMGUI
		if ( ImGui_t::isInit )
		{
			if ( ImGui_t::disablePlayerControl && player == clientnum )
			{
				players[player]->bControlEnabled = false;
			}
		}
#endif
		bool& bControlEnabled = players[player]->bControlEnabled;

	    Input& input = Input::inputs[player];

        // various UI keybinds
        if ( !gamePaused && bControlEnabled && !players[player]->usingCommand() )
        {
	        // toggle minimap
		    // player not needed to be alive
            // map window bind
			if ( players[player]->hotbar.faceMenuButtonHeld == Player::Hotbar_t::GROUP_NONE )
			{
				if ( players[player]->bUseCompactGUIHeight() && players[player]->bUseCompactGUIWidth() )
				{
					players[player]->minimap.bExpandPromptEnabled = players[player]->shootmode || players[player]->gui_mode == GUI_MODE_NONE;
					if ( players[player]->worldUI.isEnabled()
						&& players[player]->worldUI.bTooltipInView
						&& players[player]->worldUI.tooltipsInRange.size() > 1 )
					{
						std::string expandBinding = input.binding("Toggle Minimap");
						std::string cycleNextBinding = input.binding("Interact Tooltip Next");
						std::string cyclePrevBinding = input.binding("Interact Tooltip Prev");
						if ( expandBinding == cycleNextBinding
							|| expandBinding == cyclePrevBinding )
						{
							players[player]->minimap.bExpandPromptEnabled = false;
						}
					}
					if ( input.consumeBinaryToggle("Toggle Minimap") && players[player]->minimap.bExpandPromptEnabled )
					{
						openMapWindow(player);
					}
				}
				else if ( players[player]->shootmode && players[player]->minimap.bExpandPromptEnabled
					&& input.consumeBinaryToggle("Toggle Minimap") )
				{
					openMinimap(player);
				}
			}

			if ( input.consumeBinaryToggle("Open Map") )
            {
                openMapWindow(player);
            }

            // log window bind
            if ( input.consumeBinaryToggle("Open Log") )
            {
                openLogWindow(player);
            }
        }

		// inventory interface
		// player not needed to be alive
		if ( players[player]->isLocalPlayer() && !players[player]->usingCommand() && input.consumeBinaryToggle("Character Status")
			&& !gamePaused
			&& bControlEnabled )
		{
			if ( players[player]->shootmode )
			{
				players[player]->openStatusScreen(GUI_MODE_INVENTORY, INVENTORY_MODE_ITEM);
				//Player::soundStatusOpen();
			}
			else
			{
				players[player]->closeAllGUIs(CLOSEGUI_ENABLE_SHOOTMODE, CLOSEGUI_CLOSE_ALL);
				//Player::soundStatusClose();
			}
		}

		// if useItemDropdownOnGamepad, then 'b' will close inventory, with a 'couple' checks..
		if ( players[player]->isLocalPlayer() 
			&& !players[player]->shootmode
			&& (players[player]->inventoryUI.useItemDropdownOnGamepad != Player::Inventory_t::GAMEPAD_DROPDOWN_DISABLE)
			&& !inputs.getVirtualMouse(player)->draw_cursor
			&& !players[player]->usingCommand() && input.binaryToggle("MenuCancel")
			&& !players[player]->GUI.isDropdownActive()
			&& players[player]->GUI.bModuleAccessibleWithMouse(players[player]->GUI.activeModule)
			&& !inputs.getUIInteraction(player)->selectedItem
			&& !gamePaused
			&& bControlEnabled
			&& players[player]->gui_mode == GUI_MODE_INVENTORY
			&& players[player]->inventory_mode == INVENTORY_MODE_ITEM
			&& !players[player]->inventoryUI.chestGUI.bOpen
			&& !players[player]->minimap.mapWindow
			&& !players[player]->messageZone.logWindow
			&& !players[player]->shopGUI.bOpen
			&& !GenericGUI[player].isGUIOpen() )
		{
			if ( (players[player]->inventoryUI.isInteractable && players[player]->GUI.activeModule == Player::GUI_t::MODULE_INVENTORY)
				|| players[player]->GUI.activeModule == Player::GUI_t::MODULE_HOTBAR
				|| players[player]->GUI.activeModule == Player::GUI_t::MODULE_CHARACTERSHEET )
			{
				players[player]->closeAllGUIs(CLOSEGUI_ENABLE_SHOOTMODE, CLOSEGUI_CLOSE_ALL);
				input.consumeBinaryToggle("MenuCancel");
				input.consumeBindingsSharedWithBinding("MenuCancel");
                if (players[player]->hotbar.useHotbarFaceMenu)
                {
                    input.consumeBinaryToggle("Hotbar Left");
                    input.consumeBinaryToggle("Hotbar Up / Select");
                    input.consumeBinaryToggle("Hotbar Right");
                }
				Player::soundCancel();
			}
		}

		// spell list
		// player not needed to be alive
		if ( players[player]->isLocalPlayer() && !players[player]->usingCommand() && input.consumeBinaryToggle("Spell List")
			&& !gamePaused
			&& bControlEnabled )   //TODO: Move to function in interface or something?
		{
			if ( input.input("Spell List").isBindingUsingGamepad() )
			{
				// no action, gamepad doesn't use this binding
			}
			// no dropdowns/no selected item, if controller, has to be in inventory/hotbar + !shootmode
			else if ( !inputs.getUIInteraction(player)->selectedItem && !players[player]->GUI.isDropdownActive()
				&& !GenericGUI[player].isGUIOpen()
				&& !inputs.hasController(player) )
			{
				players[player]->gui_mode = GUI_MODE_INVENTORY;
				if ( players[player]->shootmode )
				{
					players[player]->openStatusScreen(GUI_MODE_INVENTORY, INVENTORY_MODE_ITEM);
				}
				players[player]->inventoryUI.cycleInventoryTab();
			}
		}

		// spellcasting
		// player needs to be alive
		if ( players[player]->isLocalPlayerAlive() 
			&& !gamePaused
			&& !players[player]->ghost.isActive() )
		{
            const bool shootmode = players[player]->shootmode;
			bool hasSpellbook = false;
			bool tryHotbarQuickCast = players[player]->hotbar.faceMenuQuickCast;
			bool tryInventoryQuickCast = players[player]->magic.doQuickCastSpell();
			bool tryTomeQuickCast = players[player]->magic.doQuickCastTome();
			if ( stats[player]->shield && itemCategory(stats[player]->shield) == SPELLBOOK )
			{
				hasSpellbook = true;
			}

			players[player]->hotbar.faceMenuQuickCast = false;
			bool allowCasting = false;
			bool castAnimationTouch = false;
			if ( tryInventoryQuickCast || tryTomeQuickCast )
			{
				allowCasting = true;
			}
			else if ( !players[player]->usingCommand() && bControlEnabled
				&& ((shootmode && inputs.hasController(player)) || (!inputs.hasController(player) && inputs.bPlayerUsingKeyboardControl(player))) )
			{
				bool hotbarFaceMenuOpen = players[player]->hotbar.faceMenuButtonHeld != Player::Hotbar_t::GROUP_NONE;
				bool castMemorizedSpell = input.binaryToggle("Cast Spell");
				bool castSpellbook = (hasSpellbook && input.binaryToggle("Defend"));

				if ( inputs.hasController(player) && cast_animation[player].spellWaitingAttackInput() )
				{
					allowCasting = false;
					castAnimationTouch = true;

					if ( FollowerMenu[player].followerMenuIsOpen() || CalloutMenu[player].calloutMenuIsOpen() )
					{
						// nothing, let menucancel close them
					}
					else if ( input.binaryToggle("MenuCancel") )
					{
						input.consumeBinaryToggle("MenuCancel");
						input.consumeBindingsSharedWithBinding("MenuCancel");
						input.consumeBinaryToggle("Hotbar Left");
						input.consumeBinaryToggle("Hotbar Up / Select");
						input.consumeBinaryToggle("Hotbar Right");

						castSpellInit(players[player]->entity->getUID(), players[player]->magic.selectedSpell(), false, false);
					}
				}
			    else if (tryHotbarQuickCast || castMemorizedSpell || castSpellbook )
			    {
				    allowCasting = true;
				    if ( tryHotbarQuickCast == false )
					{
						if ( hotbarFaceMenuOpen )
						{
							allowCasting = false;
						}
						if ( !shootmode ) // check we dont conflict with system bindings
						{
							if ( players[player]->messageZone.logWindow || players[player]->minimap.mapWindow 
								|| FollowerMenu[player].followerMenuIsOpen() || CalloutMenu[player].calloutMenuIsOpen() )
							{
								allowCasting = false;
							}
							else
							{
								if ( castMemorizedSpell )
								{
									if ( input.bindingIsSharedWithKeyboardSystemBinding("Cast Spell") )
									{
										allowCasting = false;
									}
								}
								if ( castSpellbook )
								{
									if ( input.bindingIsSharedWithKeyboardSystemBinding("Defend") )
									{
										allowCasting = false;
										input.consumeBinaryToggle("Defend");
									}
								}
							}
						}
						else
						{
							if ( FollowerMenu[player].followerMenuIsOpen() || CalloutMenu[player].calloutMenuIsOpen() )
							{
								if ( castMemorizedSpell )
								{
									if ( input.bindingIsSharedWithKeyboardSystemBinding("Cast Spell") )
									{
										allowCasting = false;
									}
								}
								if ( castSpellbook )
								{
									allowCasting = false;
									input.consumeBinaryToggle("Defend");
								}
							}
						}
				    }
					else
					{
						if ( !players[player]->hotbar.faceMenuQuickCastEnabled && !tryInventoryQuickCast )
						{
							allowCasting = false;
						}
					}

				    if ( allowCasting && castSpellbook && players[player] && players[player]->entity )
				    {
					    if ( players[player]->entity->effectShapeshift != NOTHING )
					    {
						    if ( players[player]->entity->effectShapeshift == CREATURE_IMP )
						    {
							    // imp allowed to cast via spellbook.
						    }
						    else
						    {
							    allowCasting = false;
						    }
					    }

						if ( allowCasting && players[player]->entity->isBlind() )
						{
							messagePlayer(player, MESSAGE_EQUIPMENT | MESSAGE_STATUS, Language::get(3863)); // prevent casting of spell.
							input.consumeBinaryToggle("Defend");
							allowCasting = false;
						}
				    }
				}
			}

			if ( allowCasting )
			{
				if ( players[player] && players[player]->entity )
				{
					if ( conductGameChallenges[CONDUCT_BRAWLER] || achievementBrawlerMode )
					{
						if ( achievementBrawlerMode && conductGameChallenges[CONDUCT_BRAWLER] )
						{
							messagePlayer(player, MESSAGE_MISC, Language::get(2999)); // prevent casting of spell.
						}
						else
						{
							if ( achievementBrawlerMode && players[player]->magic.selectedSpell() )
							{
								messagePlayer(player, MESSAGE_MISC, Language::get(2998)); // notify no longer eligible for achievement but still cast.
							}
							if ( tryInventoryQuickCast )
							{
								castSpellInit(players[player]->entity->getUID(), players[player]->magic.quickCastSpell(), false, false);
							}
							else if ( tryTomeQuickCast )
							{
								if ( Item* item = uidToItem(players[player]->magic.quickCastTome()) )
								{
									if ( auto spellID = item->getTomeSpellID() )
									{
										if ( spellID != SPELL_NONE )
										{
											if ( auto spell = getSpellFromID(spellID) )
											{
												if ( !cast_animation[player].active && !cast_animation[player].active_spellbook )
												{
													castSpellInit(players[player]->entity->getUID(), spell, false, true);
													if ( cast_animation[player].active )
													{
														list_RemoveNode(item->node);
													}
												}
											}
										}
									}
								}
							}
							else if ( hasSpellbook && input.consumeBinaryToggle("Defend") )
							{
								castSpellInit(players[player]->entity->getUID(), getSpellFromID(getSpellIDFromSpellbook(stats[player]->shield->type)), true, false);
							}
							else
							{
								castSpellInit(players[player]->entity->getUID(), players[player]->magic.selectedSpell(), false, false);
							}
							if ( players[player]->magic.selectedSpell() )
							{
								conductGameChallenges[CONDUCT_BRAWLER] = 0;
							}
						}
					}
					else
					{
						if ( tryInventoryQuickCast )
						{
							castSpellInit(players[player]->entity->getUID(), players[player]->magic.quickCastSpell(), false, false);
						}
						else if ( tryTomeQuickCast )
						{
							if ( Item* item = uidToItem(players[player]->magic.quickCastTome()) )
							{
								if ( auto spellID = item->getTomeSpellID() )
								{
									if ( spellID != SPELL_NONE )
									{
										if ( auto spell = getSpellFromID(spellID) )
										{
											if ( !cast_animation[player].active && !cast_animation[player].active_spellbook )
											{
												castSpellInit(players[player]->entity->getUID(), spell, false, true);
												if ( cast_animation[player].active )
												{
													list_RemoveNode(item->node);
												}
											}
										}
									}
								}
							}
						}
						else if ( hasSpellbook && input.consumeBinaryToggle("Defend") )
						{
							castSpellInit(players[player]->entity->getUID(), getSpellFromID(getSpellIDFromSpellbook(stats[player]->shield->type)), true, false);
						}
						else
						{
							castSpellInit(players[player]->entity->getUID(), players[player]->magic.selectedSpell(), false, false);
						}
					}
				}
				input.consumeBinaryToggle("Defend");
			}
			if ( !castAnimationTouch )
			{
				if ( !(cast_animation[player].stage == 4 || cast_animation[player].stage == 9) ) // allow recast if pressed during touch throw window
				{
					input.consumeBinaryToggle("Cast Spell");
				}
			}
		}
		players[player]->magic.resetQuickCastTome();
		players[player]->magic.resetQuickCastSpell();

		bool worldUIBlocksFollowerCycle = (
				players[player]->worldUI.isEnabled()
				&& players[player]->worldUI.bTooltipInView
				&& players[player]->worldUI.tooltipsInRange.size() > 1);
		if ( worldUIBlocksFollowerCycle )
		{
			std::string cycleNPCbinding = input.binding("Cycle NPCs");
			if ( cycleNPCbinding != input.binding("Interact Tooltip Next")
				&& cycleNPCbinding != input.binding("Interact Tooltip Prev") )
			{
				worldUIBlocksFollowerCycle = false;
			}
		}
		players[player]->hud.followerDisplay.bCycleNextDisabled = (worldUIBlocksFollowerCycle && players[player]->shootmode);
		bool allowCycle = true;
		if ( CalloutMenu[player].calloutMenuIsOpen() && !players[player]->shootmode )
		{
			players[player]->hud.followerDisplay.bCycleNextDisabled = true;
			allowCycle = false;
		}
		else if ( FollowerMenu[player].followerMenuIsOpen() )
		{
			std::string cycleNPCbinding = input.binding("Cycle NPCs");
			if ( cycleNPCbinding == input.binding("MenuCancel") )
			{
				allowCycle = false;
				if ( !players[player]->shootmode )
				{
					players[player]->hud.followerDisplay.bCycleNextDisabled = true;
				}
			}
		}

		if ( !players[player]->usingCommand() && input.consumeBinaryToggle("Cycle NPCs")
			&& !gamePaused
			&& bControlEnabled )
		{
			if ( (!worldUIBlocksFollowerCycle && players[player]->shootmode) || FollowerMenu[player].followerMenuIsOpen() )
			{
				if ( allowCycle )
				{
					// can select next follower in inventory or shootmode
					FollowerMenu[player].selectNextFollower();
					players[player]->characterSheet.proficienciesPage = 1;
					if ( players[player]->shootmode && !players[player]->characterSheet.lock_right_sidebar )
					{
						// from now on, allies should be displayed all times
						//players[player]->openStatusScreen(GUI_MODE_INVENTORY, INVENTORY_MODE_ITEM);
					}
				}
			}
		}
	}

	// other status
	for ( int player = 0; player < MAXPLAYERS; ++player )
	{
		if ( !players[player]->isLocalPlayer() )
		{
			continue;
		}
		if ( players[player]->shootmode == false )
		{
			if ( inputs.bPlayerUsingKeyboardControl(player) )
			{
				SDL_SetRelativeMouseMode(SDL_FALSE);
			}
		}
		else
		{
			//Do these get called every frame? Might be better to move this stuff into an if (went_back_into_shootmode) { ... } thing.
			//2-3 years later...yes, it is run every frame.
			GenericGUI[player].closeGUI();

			if ( players[player]->bookGUI.bBookOpen )
			{
				players[player]->bookGUI.closeBookGUI();
			}
			if ( players[player]->signGUI.bSignOpen )
			{
				players[player]->signGUI.closeSignGUI();
			}
			if ( players[player]->skillSheet.bSkillSheetOpen )
			{
				players[player]->skillSheet.closeSkillSheet();
			}
			players[player]->hud.closeStatusFxWindow();

			if ( capture_mouse && !gamePaused )
			{
				if ( inputs.bPlayerUsingKeyboardControl(player) )
				{
                    // fix for macOS: put mouse back in window before recapturing mouse
                    if (EnableMouseCapture) {
                        int mouse_x, mouse_y;
                        SDL_GetGlobalMouseState(&mouse_x, &mouse_y);
                        int x, y, w, h;
                        SDL_GetWindowPosition(screen, &x, &y);
                        SDL_GetWindowSize(screen, &w, &h);
                        if (mouse_x < x || mouse_x >= x + w ||
                            mouse_y < y || mouse_y >= y + h) {
                            SDL_WarpMouseInWindow(screen, w/2, h/2);
                        }
                    }
				    SDL_SetRelativeMouseMode(EnableMouseCapture);
				}
			}

		}
	}

	DebugStats.t7Inputs = std::chrono::high_resolution_clock::now();

	// Draw the static HUD elements
	if ( !nohud )
	{
		//auto tStartMinimapDraw = std::chrono::high_resolution_clock::now();
		/*auto tEndMinimapDraw = std::chrono::high_resolution_clock::now();
		double timeTaken = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(tEndMinimapDraw - tStartMinimapDraw).count();
		printlog("Minimap draw time: %.5f", timeTaken);*/
		for ( int player = 0; player < MAXPLAYERS; ++player )
		{
			if ( !players[player]->isLocalPlayer() )
			{
				continue;
			}
			//drawMinimap(player); // Draw the Minimap
			//drawStatus(player); // Draw the Status Bar (Hotbar, Hungry/Minotaur Icons, Tooltips, etc.)
		}
	}

	DebugStats.t8Status = std::chrono::high_resolution_clock::now();

	for ( int player = 0; player < MAXPLAYERS; ++player )
	{
		if ( !players[player]->isLocalPlayer() )
		{
			continue;
		}
		players[player]->hud.processHUD();
		players[player]->messageZone.processChatbox();
		updateSkillUpFrame(player);
		updateLevelUpFrame(player);
		players[player]->inventoryUI.updateSelectedItemAnimation();
		players[player]->inventoryUI.updateInventoryItemTooltip();
		players[player]->inventoryUI.updateInventoryMiscTooltip();
		players[player]->hotbar.processHotbar();
		players[player]->inventoryUI.processInventory();
		GenericGUI[player].tinkerGUI.updateTinkerMenu();
		GenericGUI[player].alchemyGUI.updateAlchemyMenu();
		GenericGUI[player].assistShrineGUI.updateAssistShrine();
		GenericGUI[player].mailboxGUI.updateMailMenu();
		GenericGUI[player].featherGUI.updateFeatherMenu();
		GenericGUI[player].itemfxGUI.updateItemEffectMenu();
		players[player]->GUI.dropdownMenu.process();
		players[player]->characterSheet.processCharacterSheet();
		players[player]->hud.updateStatusEffectFocusedWindow();
		players[player]->messageZone.processLogFrame();
		players[player]->minimap.processMapFrame();
		players[player]->skillSheet.processSkillSheet();
		players[player]->signGUI.updateSignGUI();
		players[player]->hud.updateStatusEffectTooltip(); // to create a tooltip in this order to draw over previous elements
		CalloutRadialMenu::drawCallouts(player);
		players[player]->inventoryUI.updateItemContextMenuClickFrame();
		players[player]->GUI.handleModuleNavigation(false);
		players[player]->inventoryUI.updateCursor();
		players[player]->hotbar.updateCursor();
		players[player]->hud.updateCursor();
		players[player]->hud.updateMinotaurWarning();
		//drawSkillsSheet(player);
		if ( !gamePaused )
		{
			if ( !nohud )
			{
				drawStatusNew(player);
			}
			//drawSustainedSpells(player);
		}

		// inventory and stats
		if ( players[player]->shootmode == false )
		{
			if ( players[player]->gui_mode == GUI_MODE_INVENTORY
				|| players[player]->gui_mode == GUI_MODE_SHOP )
			{
				//updateCharacterSheet(player);
				GenericGUI[player].updateGUI();
				players[player]->bookGUI.updateBookGUI();
				//updateRightSidebar(); -- 06/12/20 we don't use this but it still somehow displays stuff :D

			}
			//else if ( players[player]->gui_mode == GUI_MODE_MAGIC )
			//{
			//	updateCharacterSheet(player);
			//	//updateMagicGUI();
			//}
			//else if ( players[player]->gui_mode == GUI_MODE_SHOP
			//	|| )
			//{
			//	//updateCharacterSheet(player);
			//	//updateShopWindow(player);
			//}
		}

		static ConsoleVariable<bool> cvar_debugmouse("/debugmouse", false);
		if ( *cvar_debugmouse )
		{
			int x = players[player]->camera_x1() + 12;
			int y = players[player]->camera_y1() + 12;
			printTextFormatted(font8x8_bmp, x, y, "mx:  %4d | my:  %4d", mousex, mousey);
			printTextFormatted(font8x8_bmp, x, y + 12, "mox: %4d | moy: %4d", omousex, omousey);
			printTextFormatted(font8x8_bmp, x, y + 28, "vx:  %4d |  vy: %4d",
				inputs.getVirtualMouse(player)->x, inputs.getVirtualMouse(player)->y);
			printTextFormatted(font8x8_bmp, x, y + 40, "vox: %4d | voy: %4d",
				inputs.getVirtualMouse(player)->ox, inputs.getVirtualMouse(player)->oy);

			if ( inputs.hasController(player) )
			{
				printTextFormatted(font8x8_bmp, x, y + 60, "rawx: %4d | rawy: %4d",
					inputs.getController(player)->oldAxisRightX, inputs.getController(player)->oldAxisRightY);
				printTextFormatted(font8x8_bmp, x, y + 72, "flx: %4f | fly: %4f",
					inputs.getController(player)->oldFloatRightX, inputs.getController(player)->oldFloatRightY);
				printTextFormatted(font8x8_bmp, x, y + 84, "deadzonex: %3.1f%% | deadzoney: %3.1f%%",
					playerSettings[multiplayer ? 0 : player].leftStickDeadzone * 100 / 32767.0,
					playerSettings[multiplayer ? 0 : player].rightStickDeadzone * 100 / 32767.0);
			}
			if ( players[player]->entity )
			{
				printTextFormatted(font8x8_bmp, x, y + 100, "velx:  %4f | vely:  %4f",
					players[player]->entity->vel_x, players[player]->entity->vel_y);
			}
			if ( inputs.hasController(player) )
			{
				printTextFormatted(font8x8_bmp, x, y + 112, "leftx: %4f | lefty: %4f",
					inputs.getController(player)->getLeftXPercent(player),
					inputs.getController(player)->getLeftYPercent(player));
			}
			if ( players[player]->entity )
			{
				printTextFormatted(font8x8_bmp, x, y + 124, "%.5f | %.5f", players[player]->entity->fskill[6], players[player]->entity->fskill[7]);
			}
		}
	}

	DebugStats.t9GUI = std::chrono::high_resolution_clock::now();

	UIToastNotificationManager.drawNotifications(MainMenu::isCutsceneActive(), true); // draw this before the cursors
    static ConsoleVariable<bool> cvar_debugVMouse("/debug_virtual_mouse", false);

	// pointer in inventory screen
	for ( int player = 0; player < MAXPLAYERS; ++player )
	{
		if ( !players[player]->isLocalPlayer() )
		{
			continue;
		}

		SDL_Rect pos;

		FollowerRadialMenu& followerMenu = FollowerMenu[player];

		Frame* frame = players[player]->inventoryUI.frame;
		Frame* draggingItemFrame = nullptr;
		Frame* oldDraggingItemFrame = nullptr;
		if ( frame )
		{
			if ( draggingItemFrame = frame->findFrame("dragging inventory item") )
			{
				draggingItemFrame->setDisabled(true);
			}
			// unused for now
			if ( bUseSelectedSlotCycleAnimation )
			{
				if ( oldDraggingItemFrame = frame->findFrame("dragging inventory item old") )
				{
					oldDraggingItemFrame->setDisabled(true);
					if ( !inputs.getUIInteraction(player)->selectedItem )
					{
						if ( auto img = oldDraggingItemFrame->findImage("item sprite img") )
						{
							img->path = "";
						}
					}
				}
			}
		}

		if ( players[player]->shootmode == false )
		{
			// dragging items, player not needed to be alive
			if ( inputs.getUIInteraction(player)->selectedItem )
			{
				Item*& selectedItem = inputs.getUIInteraction(player)->selectedItem;
				if ( frame )
				{
					if ( draggingItemFrame )
					{
						if ( oldDraggingItemFrame )
						{
							oldDraggingItemFrame->setDisabled(false);
						}
						updateSlotFrameFromItem(draggingItemFrame, selectedItem);

						Frame* selectedSlotCursor = players[player]->inventoryUI.selectedItemCursorFrame;
						if ( !inputs.getVirtualMouse(player)->draw_cursor )
						{
							SDL_Rect selectedItemCursorPos = selectedSlotCursor->getSize();
							SDL_Rect oldDraggingItemPos = selectedItemCursorPos;

							int cursorOffset = players[player]->inventoryUI.cursor.cursorToSlotOffset;
							real_t animX = players[player]->inventoryUI.selectedItemAnimate.animateX;
							real_t animY = players[player]->inventoryUI.selectedItemAnimate.animateY;
							real_t maxX = (selectedItemCursorPos.w - cursorOffset * 2) / 2;
							real_t maxY = (selectedItemCursorPos.h - cursorOffset * 2) / 2;

							if ( !bUseSelectedSlotCycleAnimation )
							{
								// linear diagonal motion here
								selectedItemCursorPos.x += cursorOffset +
									(animX) * (selectedItemCursorPos.w - cursorOffset * 2) / 2;
								selectedItemCursorPos.y += cursorOffset +
									(animY) * (selectedItemCursorPos.h - cursorOffset * 2) / 2;
								draggingItemFrame->setSize(selectedItemCursorPos);
							}
							else
							{
								if ( oldDraggingItemFrame )
								{
									// option to move items in a circular motion
									real_t magnitudeX = cos(animX * PI + 5 * PI / 4);
									real_t magnitudeY = sin(animY * PI + 5 * PI / 4);
									selectedItemCursorPos.x += cursorOffset + maxX / 2 + magnitudeX * maxX;
									selectedItemCursorPos.y += cursorOffset + maxY / 2 + magnitudeY * maxY;
									draggingItemFrame->setSize(selectedItemCursorPos);

									real_t magnitudeX2 = cos(animX * PI + 1 * PI / 4);
									real_t magnitudeY2 = sin(animY * PI + 1 * PI / 4);
									oldDraggingItemPos.x += cursorOffset + maxX / 2 + magnitudeX2 * maxX;
									oldDraggingItemPos.y += cursorOffset + maxY / 2 + magnitudeY2 * maxY;
									oldDraggingItemFrame->setSize(oldDraggingItemPos);
									if ( animX == 1.0 || animY == 1.0 )
									{
										oldDraggingItemFrame->setDisabled(true);
									}
								}
							}
						}
						else if ( inputs.getVirtualMouse(player)->draw_cursor )
						{
							pos.x = inputs.getMouse(player, Inputs::X) - 15;
							pos.y = inputs.getMouse(player, Inputs::Y) - 15;
							pos.x *= ((float)Frame::virtualScreenX / (float)xres);
							pos.y *= ((float)Frame::virtualScreenY / (float)yres);
							pos.x -= players[player]->camera_virtualx1();
							pos.y -= players[player]->camera_virtualy1();
							draggingItemFrame->setSize(SDL_Rect{ pos.x, pos.y, draggingItemFrame->getSize().w, draggingItemFrame->getSize().h });
						}
					}
                    
					if ( *cvar_debugVMouse )
					{
						// debug for controllers
                        const float factorX = (float)xres / Frame::virtualScreenX;
                        const float factorY = (float)yres / Frame::virtualScreenY;
						auto cursor = Image::get("*#images/system/cursor_hand.png");
						if ( enableDebugKeys && keystatus[SDLK_j] )
						{
							cursor = Image::get("*#images/ui/Crosshairs/cursor_xB.png");
						}

                        const int w = cursor->getWidth() * factorX;
                        const int h = cursor->getHeight() * factorY;
						pos.x = inputs.getVirtualMouse(player)->x - (w / 7) - w / 2;
						pos.y = inputs.getVirtualMouse(player)->y - (h / 7) - h / 2;
						pos.x += 4;
						pos.y += 4;
						pos.w = w;
						pos.h = h;
						cursor->drawColor(nullptr, pos, SDL_Rect{ 0, 0, xres, yres }, 0xFF0000FF);
					}
				}
			}
			else if ( players[player]->isLocalPlayer() && followerMenu.selectMoveTo &&
				(followerMenu.optionSelected == ALLY_CMD_MOVETO_SELECT
					|| followerMenu.optionSelected == ALLY_CMD_ATTACK_SELECT) )
			{
				// note this currently does not get hit, moveto_select etc is shootmode
				/*auto cursor = Image::get("images/system/cursor_hand.png");
				pos.x = inputs.getMouse(player, Inputs::X) - cursor->getWidth() / 2;
				pos.y = inputs.getMouse(player, Inputs::Y) - cursor->getHeight() / 2;
				pos.x += 4;
				pos.y += 4;
				pos.w = cursor->getWidth();
				pos.h = cursor->getHeight();
				cursor->draw(nullptr, pos, SDL_Rect{0, 0, xres, yres});
				if ( followerMenu.optionSelected == ALLY_CMD_MOVETO_SELECT )
				{
					if ( followerMenu.followerToCommand
						&& (followerMenu.followerToCommand->getMonsterTypeFromSprite() == SENTRYBOT
							|| followerMenu.followerToCommand->getMonsterTypeFromSprite() == SPELLBOT)
						)
					{
						ttfPrintTextFormatted(ttf12, pos.x + 24, pos.y + 24, Language::get(3650));
					}
					else
					{
						ttfPrintTextFormatted(ttf12, pos.x + 24, pos.y + 24, Language::get(3039));
					}
				}
				else
				{
					if ( !strcmp(followerMenu.interactText, "") )
					{
						if ( followerMenu.followerToCommand )
						{
							int type = followerMenu.followerToCommand->getMonsterTypeFromSprite();
							if ( followerMenu.allowedInteractItems(type)
								|| followerMenu.allowedInteractFood(type)
								|| followerMenu.allowedInteractWorld(type)
								)
							{
								ttfPrintTextFormatted(ttf12, pos.x + 24, pos.y + 24, Language::get(4041)); // "Interact with..."
							}
							else
							{
								ttfPrintTextFormatted(ttf12, pos.x + 24, pos.y + 24, Language::get(4042)); // "Attack..."
							}
						}
						else
						{
							ttfPrintTextFormatted(ttf12, pos.x + 24, pos.y + 24, Language::get(4041)); // "Interact with..."
						}
					}
					else
					{
						ttfPrintTextFormatted(ttf12, pos.x + 24, pos.y + 24, "%s", followerMenu.interactText);
					}
				}*/
			}
			else if ( inputs.getVirtualMouse(player)->draw_cursor )
			{
#ifndef NINTENDO
                const float factorX = (float)xres / Frame::virtualScreenX;
                const float factorY = (float)yres / Frame::virtualScreenY;
				auto cursor = Image::get("*#images/system/cursor_hand.png");
				real_t& mouseAnim = inputs.getVirtualMouse(player)->mouseAnimationPercent;
				if ( Input::inputs[player].binary("MenuLeftClick") )
				{
					mouseAnim = .5;
				}
				if ( mouseAnim > .25 )
				{
					cursor = Image::get("*#images/system/cursor_hand2.png");
				}
				if ( mouseAnim > 0.0 )
				{
					mouseAnim -= .05 * getFPSScale(144.0);
				}
				if ( enableDebugKeys && keystatus[SDLK_j] )
				{
					cursor = Image::get("*#images/ui/Crosshairs/cursor_xB.png");
				}
                const int w = cursor->getWidth() * factorX;
                const int h = cursor->getHeight() * factorY;
                pos.x = inputs.getMouse(player, Inputs::X) - (mouseAnim * w / 7) - w / 2;
                pos.y = inputs.getMouse(player, Inputs::Y) - (mouseAnim * h / 7) - h / 2;
                pos.x += 4;
                pos.y += 4;
                pos.w = w;
                pos.h = h;
				if ( inputs.getUIInteraction(player)->itemMenuOpen && inputs.getUIInteraction(player)->itemMenuFromHotbar )
				{
					// adjust cursor to match selection
					pos.y -= inputs.getUIInteraction(player)->itemMenuOffsetDetectionY;
				}
				cursor->draw(nullptr, pos, SDL_Rect{0, 0, xres, yres});
#endif
			}
			else
			{
#ifndef NDEBUG
				// debug for controllers
				if ( *cvar_debugVMouse )
				{
                    const float factorX = (float)xres / Frame::virtualScreenX;
                    const float factorY = (float)yres / Frame::virtualScreenY;
					auto cursor = Image::get("*#images/system/cursor_hand.png");
					if ( enableDebugKeys && keystatus[SDLK_j] )
					{
						cursor = Image::get("*#images/ui/Crosshairs/cursor_xB.png");
					}
                    const int w = cursor->getWidth() * factorX;
                    const int h = cursor->getHeight() * factorY;
                    pos.x = inputs.getVirtualMouse(player)->x - (w / 7) - w / 2;
                    pos.y = inputs.getVirtualMouse(player)->y - (h / 7) - h / 2;
                    pos.x += 4;
                    pos.y += 4;
                    pos.w = w;
                    pos.h = h;
					cursor->drawColor(nullptr, pos, SDL_Rect{ 0, 0, xres, yres }, 0xFF0000FF);
				}
#endif
			}
		}
		else
		{
			real_t& mouseAnim = inputs.getVirtualMouse(player)->mouseAnimationPercent;
			mouseAnim = 0.0;
		}
		players[player]->hud.updateWorldTooltipPrompts();
	}

	if ( ItemTooltips.autoReload )
	{
		if ( ticks % TICKS_PER_SECOND == 0 )
		{
			ItemTooltips.readTooltipsFromFile();
		}
	}
}

void drawAllPlayerCameras() {
	DebugStats.drawWorldT1 = std::chrono::high_resolution_clock::now();
	int playercount = 0;
	for (int c = 0; c < MAXPLAYERS; ++c)
	{
		if (!client_disconnected[c] && players[c]->isLocalPlayer())
		{
			++playercount;
		}
	}
	Uint32 oldFov = ::fov;
	if ( playercount == 2 )
	{
		if ( *MainMenu::vertical_splitscreen )
		{
			::fov += 15;
		}
		else
		{
			/*if ( ::fov >= 15 )
			{
				::fov -= 15;
			}
			else
			{
				::fov = 0;
			}*/
		}
	}

	if (playercount >= 1)
	{
        // drunkenness spinning
	    double cosspin = cos(ticks % 360 * PI / 180.f) * 0.25;
	    double sinspin = sin(ticks % 360 * PI / 180.f) * 0.25;
        
        // setup a graphics frame
        beginGraphics();

		//int maximum = splitscreen ? MAXPLAYERS : 1;
		for (int c = 0; c < MAXPLAYERS; ++c)
		{
			if (client_disconnected[c])
			{
				continue;
			}
			if ( !splitscreen && c != clientnum )
			{
				continue;
			}
			auto& camera = players[c]->camera();
		    auto& globalLightModifier = players[c]->camera().globalLightModifier;
		    auto& globalLightModifierEntities = players[c]->camera().globalLightModifierEntities;
		    auto& globalLightModifierActive = players[c]->camera().globalLightModifierActive;
			if (shaking && players[c] && players[c]->entity && !gamePaused)
			{
				camera.ang += cosspin * drunkextend[c];
				camera.vang += sinspin * drunkextend[c];
			}

			if ( players[c] && players[c]->entity )
			{
				if ( usecamerasmoothing )
				{
					real_t oldYaw = players[c]->entity->yaw;
					//printText(font8x8_bmp, 20, 20, "using smooth camera");
					players[c]->movement.handlePlayerCameraBobbing(true);
					players[c]->movement.handlePlayerMovement(true);
					players[c]->movement.handlePlayerCameraUpdate(true);
					players[c]->movement.handlePlayerCameraPosition(true);

					players[c]->ghost.handleGhostCameraBobbing(true);
					players[c]->ghost.handleGhostMovement(true);
					players[c]->ghost.handleGhostCameraUpdate(true);
					//messagePlayer(0, "%3.2f | %3.2f", players[c]->entity->yaw, oldYaw);
				}
			}
   
            // activate ghost fog (if necessary)
            if (players[c]->ghost.isActive()) {
                *cvar_hdrBrightness = {0.9f, 0.9f, 1.2f, 1.0f};
                *cvar_fogColor = {0.7f, 0.7f, 1.1f, 0.25f};
				if ( !strncmp(map.filename, "fortress", 8) )
				{
					cvar_fogColor->w = 1.f;
				}
                *cvar_fogDistance = 350.f;
            }
			else if ( players[c] && players[c]->entity && players[c]->entity->isBlind() )
			{
				*cvar_fogDistance = 0.f; // no fog when blind to ensure black
			}

			// do occlusion culling from the perspective of this camera
			DebugStats.drawWorldT2 = std::chrono::high_resolution_clock::now();
			occlusionCulling(map, camera);
			glBeginCamera(&camera, true, map);

			// shared minimap progress
			if ( !splitscreen/*gameplayCustomManager.inUse() && gameplayCustomManager.minimapShareProgress && !splitscreen*/ )
			{
				for ( int i = 0; i < MAXPLAYERS; ++i )
				{
					if ( i != clientnum && players[i] && Player::getPlayerInteractEntity(i) )
					{
						if ( !players[i]->entity || (players[i]->entity && !players[i]->entity->isBlind()) )
						{
							if ( players[i]->entity && players[i]->entity->ticks < TICKS_PER_SECOND * 1 )
							{
								continue; // don't share for first x ticks due to level change warping
							}

							real_t x = camera.x;
							real_t y = camera.y;
							real_t z = camera.z;
							real_t ang = camera.ang;

							Entity* sharedViewEntity =
								Player::getPlayerInteractEntity(i);
							camera.x = sharedViewEntity->x / 16.0;
							camera.y = sharedViewEntity->y / 16.0;
							camera.z = sharedViewEntity->z * 2.0 - 2.5;
							camera.ang = sharedViewEntity->yaw;
							raycast(camera, minimap, false); // update minimap from other players' perspectives, player or ghost
							camera.x = x;
							camera.y = y;
							camera.z = z;
							camera.ang = ang;
						}
					}
				}
			}

			if ( players[c] && players[c]->entity )
			{
				if ( players[c]->entity->isBlind() )
				{
					if ( globalLightModifierActive == GLOBAL_LIGHT_MODIFIER_STOPPED
						|| (globalLightModifierActive == GLOBAL_LIGHT_MODIFIER_DISSIPATING && globalLightModifier < 1.f) )
					{
						globalLightModifierActive = GLOBAL_LIGHT_MODIFIER_INUSE;
						globalLightModifier = 0.f;
						globalLightModifierEntities = 0.f;
						if ( !intro )
						{
							bool selfTelepath = stats[c]->mask && stats[c]->mask->type == TOOL_BLINDFOLD_TELEPATHY;
							if ( selfTelepath )
							{
								for ( node_t* mapNode = map.creatures->first; mapNode != nullptr; mapNode = mapNode->next )
								{
									Entity* mapCreature = (Entity*)mapNode->element;
									if ( mapCreature &&
										(selfTelepath
											/*|| (mapCreature->getStats() && mapCreature->getStats()->getEffectActive(EFF_DETECT_ENEMY))*/
											) )
									{
										/*if ( mapCreature->getStats() && mapCreature->getStats()->getEffectActive(EFF_DETECT_ENEMY) )
										{
											mapCreature->monsterEntityRenderAsTelepath = 2;
										}
										else*/
										{
											mapCreature->monsterEntityRenderAsTelepath = 1;
										}
									}
								}
							}
						}
					}

					int PERModifier = 0;
					if ( stats[c] && stats[c]->getEffectActive(EFF_BLIND)
						&& !stats[c]->getEffectActive(EFF_ASLEEP) && !stats[c]->getEffectActive(EFF_MESSY) )
					{
						// blind but not messy or asleep = allow PER to let you see the world a little.
						PERModifier = players[c]->entity->getPER() / 5;
						if ( PERModifier < 0 )
						{
							PERModifier = 0;
						}
					}

					real_t limit = PERModifier * 0.01;
					globalLightModifier = std::min(limit, globalLightModifier + 0.0005);

					int telepathyLimit = std::min(64, 48 + players[c]->entity->getPER());
					globalLightModifierEntities = std::min(telepathyLimit / 255.0, globalLightModifierEntities + (0.2 / 255.0));
				}
				else
				{
					if ( globalLightModifierActive == GLOBAL_LIGHT_MODIFIER_INUSE )
					{
						for ( node_t* mapNode = map.creatures->first; mapNode != nullptr; mapNode = mapNode->next )
						{
							Entity* mapCreature = (Entity*)mapNode->element;
							if ( mapCreature )
							{
								mapCreature->monsterEntityRenderAsTelepath = 0;
							}
						}
					}
					globalLightModifierActive = GLOBAL_LIGHT_MODIFIER_DISSIPATING;
					globalLightModifierEntities = 0.f;
					if ( globalLightModifier < 1.f )
					{
						globalLightModifier += 0.01;
					}
					else
					{
						globalLightModifier = 1.01;
						globalLightModifierActive = GLOBAL_LIGHT_MODIFIER_STOPPED;
					}
				}
				DebugStats.drawWorldT3 = std::chrono::high_resolution_clock::now();
				if ( !players[c]->entity->isBlind() )
				{
				    raycast(camera, minimap, true); // update minimap
				}
				DebugStats.drawWorldT4 = std::chrono::high_resolution_clock::now();
				glDrawWorld(&camera, REALCOLORS);
			}
			else
			{
				// undo blindness effects
				if ( globalLightModifierActive == GLOBAL_LIGHT_MODIFIER_INUSE )
				{
					for ( node_t* mapNode = map.creatures->first; mapNode != nullptr; mapNode = mapNode->next )
					{
						Entity* mapCreature = (Entity*)mapNode->element;
						if ( mapCreature )
						{
							mapCreature->monsterEntityRenderAsTelepath = 0;
						}
					}
				}
				globalLightModifierActive = GLOBAL_LIGHT_MODIFIER_DISSIPATING;
				globalLightModifierEntities = 0.f;
				if ( globalLightModifier < 1.f )
				{
					globalLightModifier += 0.01;
				}
				else
				{
					globalLightModifier = 1.01;
					globalLightModifierActive = GLOBAL_LIGHT_MODIFIER_STOPPED;
				}

				if ( players[c] && players[c]->ghost.isActive() )
				{
					raycast(camera, minimap, false); // update minimap for ghost
				}

			    // player is dead, spectate
				glDrawWorld(&camera, REALCOLORS);
			}

			DebugStats.drawWorldT5 = std::chrono::high_resolution_clock::now();
			drawEntities3D(&camera, REALCOLORS);
			glEndCamera(&camera, true, map);
            
            // undo ghost fog
            if (players[c]->ghost.isActive() || (players[c]->entity && players[c]->entity->isBlind()) ) {
				map.setMapHDRSettings();
            }

			if (shaking && players[c] && players[c]->entity && !gamePaused)
			{
				camera.ang -= cosspin * drunkextend[c];
				camera.vang -= sinspin * drunkextend[c];
			}

			auto& cvars = cameravars[c];
			camera.ang -= cvars.shakex2;
			camera.vang -= cvars.shakey2 / 200.0;
		}
	}
	DebugStats.drawWorldT6 = std::chrono::high_resolution_clock::now();
	::fov = oldFov;
	if (!intro && multiplayer == CLIENT)
	{
		syncClientPersistentMinimap(false);
	}
}

/*-------------------------------------------------------------------------------

	main

	Initializes game resources, harbors main game loop, and cleans up
	afterwards

-------------------------------------------------------------------------------*/

static void doConsoleCommands() {
	Input& input = Input::inputs[clientnum]; // commands - uses local clientnum only
	const bool controlEnabled = players[clientnum]->bControlEnabled && !movie;

#if defined(NINTENDO) && defined(NINTENDO_DEBUG)
	// activate console
	if (input.binaryToggle("ConsoleCommand1") &&
		input.binaryToggle("ConsoleCommand2") &&
		input.binaryToggle("ConsoleCommand3"))
	{
		input.consumeBinary("ConsoleCommand1");
		input.consumeBinary("ConsoleCommand2");
		input.consumeBinary("ConsoleCommand3");
		auto result = nxKeyboard("Enter console command");
		if (result.success)
		{
			char temp[128];
			strncpy(temp, result.str.c_str(), 128);
			temp[127] = '\0';
			messagePlayer(clientnum, MESSAGE_MISC, temp);
			consoleCommand(temp);
		}
	}
#else
	// check for input to start/stop a command (enter / return keystroke, or chat binding)
	bool confirm = false;
	if (controlEnabled) {
		if (input.getPlayerControlType() != Input::playerControlType_t::PLAYER_CONTROLLED_BY_KEYBOARD) {
            if (command || !intro ) {
                if (keystatus[SDLK_RETURN]) {
                    keystatus[SDLK_RETURN] = 0;
                    confirm = true;
                }
                if (Input::keys[SDLK_RETURN]) {
                    Input::keys[SDLK_RETURN] = 0;
                    confirm = true;
                }
            }
		}
		if (input.consumeBinaryToggle("Chat")) {
			input.consumeBindingsSharedWithBinding("Chat");
            if (command || !intro ) {
                confirm = true;
            }
		}
		if ( confirm && !command )
		{
			if ( MainMenu::main_menu_frame )
			{
				if ( auto compendium = MainMenu::main_menu_frame->findFrame("compendium") )
				{
					confirm = false; // stop menuStart triggering console commands
				}
			}
		}

		if (input.consumeBinaryToggle("Console Command")) {
			input.consumeBindingsSharedWithBinding("Console Command");
			if (!command && !inputstr ) {
				confirm = true;
			}
		}

	}

	if (confirm && !command) // begin a command
	{
		// start typing a command
		if (input.binary("Console Command")) {
			strcpy(command_str, "/");
		}
		else {
			strcpy(command_str, "");
		}
		inputstr = command_str;
		inputlen = 127;
		command = true;
		cursorflash = ticks;
		if (!SDL_IsTextInputActive()) {
			SDL_StartTextInput();
		}

		// clear follower menu entities.
		for (int i = 0; i < MAXPLAYERS; ++i) {
			if (players[i]->isLocalPlayer() && inputs.bPlayerUsingKeyboardControl(i)) {
				FollowerMenu[i].closeFollowerMenuGUI();
				CalloutMenu[i].closeCalloutMenuGUI();
			}
		}
	}
	else if (command) // finish a command
	{
		// check for cancel keystroke (always ESC)
		if (keystatus[SDLK_ESCAPE])
		{
			keystatus[SDLK_ESCAPE] = 0;
			chosen_command = nullptr;
			command = false;
		}

		// check for forced cancel
		if (!controlEnabled)
		{
			if (SDL_IsTextInputActive()) {
				SDL_StopTextInput();
			}
			inputstr = nullptr;
			inputlen = 0;
			chosen_command = nullptr;
			command = false;
		}

		// set player issuing command
		int commandPlayer = clientnum;
		if (multiplayer == SINGLE) {
			for (int i = 0; i < MAXPLAYERS; ++i) {
				if (inputs.bPlayerUsingKeyboardControl(i)) {
					commandPlayer = i;
					break;
				}
			}
		}

		// set inputstr
		if (!SDL_IsTextInputActive()) {
			SDL_StartTextInput();
		}
		inputstr = command_str;
		inputlen = 127;

		// issue command
		if (confirm)
		{
			// no longer accepting input
			if (SDL_IsTextInputActive()) {
				SDL_StopTextInput();
			}
			inputstr = nullptr;
			inputlen = 0;
			command = false;

			// sanitize strings (remove format codes)
			strncpy(command_str, messageSanitizePercentSign(command_str, nullptr).c_str(), 127);

			// process string
			if (command_str[0] == '/') // slash invokes a command procedure
			{
				messagePlayer(commandPlayer, MESSAGE_MISC, command_str);
				consoleCommand(command_str);
			}
			else if (!intro) // can't send messages in multiplayer
			{
				if (multiplayer == CLIENT) // send message as a client
				{
					if (strcmp(command_str, ""))
					{
						char chatstring[256];
						strcpy(chatstring, Language::get(739));
						strcat(chatstring, command_str);
						Uint32 color = playerColor(commandPlayer, colorblind_lobby, false);
						if (messagePlayerColor(commandPlayer, MESSAGE_CHAT, color, chatstring)) {
							playSound(Message::CHAT_MESSAGE_SFX, 64);
						}

						// send message to server
						if (net_packet && net_packet->data) {
							strcpy((char*)net_packet->data, "MSGS");
							net_packet->data[4] = commandPlayer;
							SDLNet_Write32(color, &net_packet->data[5]);
							strcpy((char*)(&net_packet->data[9]), command_str);
							net_packet->address.host = net_server.host;
							net_packet->address.port = net_server.port;
							net_packet->len = 9 + strlen(command_str) + 1;
							sendPacketSafe(net_sock, -1, net_packet, 0);
						}
					}
					else
					{
						strcpy(command_str, "");
					}
				}
				else // servers (or singleplayer) broadcast typed messages
				{
					if (strcmp(command_str, ""))
					{
						char chatstring[256];
						strcpy(chatstring, Language::get(739));
						strcat(chatstring, command_str);
						Uint32 color = playerColor(commandPlayer, colorblind_lobby, false);
						if (messagePlayerColor(commandPlayer, MESSAGE_CHAT, color, chatstring)) {
							playSound(Message::CHAT_MESSAGE_SFX, 64);
						}
						if (multiplayer == SERVER)
						{
							// send message to all clients
							for (int c = 1; c < MAXPLAYERS; c++)
							{
								if (client_disconnected[c] || players[c]->isLocalPlayer())
								{
									continue;
								}
								if (net_packet && net_packet->data) {
									strcpy((char*)net_packet->data, "MSGS");
									// strncpy() does not copy N bytes if a terminating null is encountered first
									// see http://www.cplusplus.com/reference/cstring/strncpy/
									// see https://en.cppreference.com/w/c/string/byte/strncpy
									// GCC throws a warning (intended) when the length argument to strncpy() in any
									// way depends on strlen(src) to discourage this (and related) construct(s).

									strncpy(chatstring, stats[0]->name, 22);
									chatstring[std::min<size_t>(strlen(stats[0]->name), 22)] = 0; //TODO: Why are size_t and int being compared?
									strcat(chatstring, ": ");
									strcat(chatstring, command_str);
									SDLNet_Write32(color, &net_packet->data[4]);
									SDLNet_Write32((Uint32)MESSAGE_CHAT, &net_packet->data[8]);
									strcpy((char*)(&net_packet->data[12]), chatstring);
									net_packet->address.host = net_clients[c - 1].host;
									net_packet->address.port = net_clients[c - 1].port;
									net_packet->len = 12 + strlen(chatstring) + 1;
									sendPacketSafe(net_sock, -1, net_packet, c - 1);
								}
							}
						}
					}
					else
					{
						strcpy(command_str, "");
					}
				}
			}

			// save this in the command history
			if (command_str[0]) {
				saveCommand(command_str);
			}
			chosen_command = NULL;
		}
	}
	else
	{
		// make sure not to create text input events
		if (inputstr == command_str) {
			inputstr = nullptr;
		}
		if (!inputstr && SDL_IsTextInputActive()) {
			SDL_StopTextInput();
		}
	}
#endif // NINTENDO
}

#include <stdio.h>
//#include <unistd.h>
#include <stdlib.h>
#ifdef APPLE
#include <mach-o/dyld.h>
#endif

int main(int argc, char** argv)
{
#ifdef WINDOWS
	SetUnhandledExceptionFilter(unhandled_handler);
#ifdef _DEBUG
	//_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif
#endif // WINDOWS
#ifdef NINTENDO
	nxInit();
#endif // NINTENDO

#ifdef LINUX
	struct sigaction sa;

	memset(&sa, 0, sizeof(struct sigaction));
	sigemptyset(&sa.sa_mask);
	sa.sa_sigaction = segfault_sigaction;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, NULL);

	memset(&sa, 0, sizeof(struct sigaction));
	sigemptyset(&sa.sa_mask);
	sa.sa_sigaction = stop_sigaction;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSTOP, &sa, NULL);

	memset(&sa, 0, sizeof(struct sigaction));
	sigemptyset(&sa.sa_mask);
	sa.sa_sigaction = continue_sigaction;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGCONT, &sa, NULL);

	(void)chdir(BASE_DATA_DIR); // fixes a lot of headaches...
#endif

#ifndef NINTENDO
	try
#endif
	{
#if defined(APPLE) || defined(BSD) || defined(HAIKU)
#ifdef APPLE
		uint32_t buffsize = 4096;
		char binarypath[buffsize];
		int result = _NSGetExecutablePath(binarypath, &buffsize);
#elif defined(BSD)
#if defined(__FreeBSD__)
		int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
		size_t buffsize = PATH_MAX;
		char binarypath[buffsize];
		int result = sysctl(mib, 4, binarypath, &buffsize, NULL, 0);
#elif defined(__NetBSD__)
		constexpr size_t buffsize = PATH_MAX;
		char binarypath[buffsize];
		int result = (readlink("/proc/curproc/exe", binarypath, buffsize) > 0 ? 0 : -1);
#endif
#elif defined(HAIKU)
		size_t buffsize = PATH_MAX;
		char binarypath[buffsize];
		int result = find_path(B_APP_IMAGE_SYMBOL, B_FIND_PATH_IMAGE_PATH, NULL, binarypath, sizeof(binarypath)); // B_OK is 0
#endif
		if (result == 0)   //It worked.
		{
			printlog("Binary path: %s\n", binarypath);
			char* last = strrchr(binarypath, '/');
			*last = '\0';
#ifdef APPLE
			char execpath[buffsize];
			strcpy(execpath, binarypath);
			//char* last = strrchr(execpath, '/');
			//strcat(execpath, '/');
			//strcat(execpath, "/../../../");
			printlog("Chrooting to directory: %s\n", execpath);
			chdir(execpath);
			///Users/ciprian/barony/barony-sdl2-take2/barony.app/Contents/MacOS/barony
			chdir("..");
			//chdir("..");
			chdir("Resources");
			//chdir("..");
			//chdir("..");
#endif
		}
		else
		{
			printlog("Failed to get binary path. Program may not work corectly!\n");
		}
#endif
		SDL_Rect pos, src;
		int c;
		//int tilesreceived=0;
		//Mix_Music **music, *intromusic, *splashmusic, *creditsmusic;
		node_t* node;
		Entity* entity;
		//SDL_Surface *sky_bmp;
		light_t* light;

		size_t datadirsz = std::min(sizeof(datadir) - 1, strlen(BASE_DATA_DIR));
		strncpy(datadir, BASE_DATA_DIR, datadirsz);
		datadir[datadirsz] = '\0';
#ifdef WINDOWS
		strcpy(outputdir, "./");
#else
 #ifndef NINTENDO
		char *basepath = getenv("HOME");
  #ifdef USE_EOS
   #ifdef STEAMWORKS
		//Steam + EOS
		snprintf(outputdir, sizeof(outputdir), "%s/.barony", basepath);
   #else
		//Just EOS.
		std::string firstDotdir(basepath);
		firstDotdir += "/.barony/";
		if (access(firstDotdir.c_str(), F_OK) == -1)
		{
			mkdir(firstDotdir.c_str(), 0777); //Since this mkdir is not equivalent to mkdir -p, have to create each part of the path manually.
		}
		snprintf(outputdir, sizeof(outputdir), "%s/epicgames", firstDotdir.c_str());
   #endif
  #else //USE_EOS
		//No EOS. Could be Steam though. Or could also not.
		snprintf(outputdir, sizeof(outputdir), "%s/.barony", basepath);
  #endif
		if (access(outputdir, F_OK) == -1)
		{
			mkdir(outputdir, 0777);
		}
 #else // !NINTENDO
		strcpy(outputdir, "save:");
 #endif // NINTENDO
#endif
		// read command line arguments
		if ( argc > 1 )
		{
			for (c = 1; c < argc; c++)
			{
#ifdef STEAMWORKS
			    cmd_line += argv[c];
			    cmd_line += " ";
#endif
				if ( argv[c] != NULL )
				{
                    if ( !strcmp(argv[c], "--headless") || !strcmp(argv[c], "-headless") )
                    {
                        headless = true;
                        no_sound = true;
                        fullscreen = 0;
                        borderless = false;
                        xres = 320;
                        yres = 200;
                    }
                    else if ( !strcmp(argv[c], "--LAN") || !strcmp(argv[c], "--lan") )
                    {
                        headlessServerVisibility = HEADLESS_VISIBILITY_LAN;
                        strncpy(headlessBindAddress, "0.0.0.0", sizeof(headlessBindAddress) - 1);
                        headlessBindAddress[sizeof(headlessBindAddress) - 1] = '\0';
                    }
                    else if ( !strcmp(argv[c], "--private") )
                    {
                        headlessServerVisibility = HEADLESS_VISIBILITY_PRIVATE;
                    }
                    else if ( !strcmp(argv[c], "--public") )
                    {
                        headlessServerVisibility = HEADLESS_VISIBILITY_PUBLIC;
                    }
                    else if ( !strcmp(argv[c], "--late-join") )
                    {
                        headlessLateJoinRequested = true;
                    }
                    else if ( !strcmp(argv[c], "--autostart") )
                    {
                        headlessAutoStart = true;
                        headlessAutoStartDelaySeconds = 0;
                    }
                    else if ( !strncmp(argv[c], "--autostart=", 12) )
                    {
                        const int delay = atoi(argv[c] + 12);
                        if ( delay >= 0 )
                        {
                            headlessAutoStart = true;
                            headlessAutoStartDelaySeconds = static_cast<Uint32>(delay);
                        }
                    }
                    else if ( !strncmp(argv[c], "--autosave=", 11) )
                    {
                        char* end = nullptr;
                        const unsigned long interval = strtoul(argv[c] + 11, &end, 10);
                        const unsigned long maximum = 7UL * 24UL * 60UL * 60UL;
                        if ( end != argv[c] + 11 && *end == '\0' && interval <= maximum )
                        {
                            headlessAutosaveIntervalSeconds = static_cast<Uint32>(interval);
                        }
                        else
                        {
                            printlog("Headless option error: --autosave requires seconds from 0 through %lu.", maximum);
                        }
                    }
                    else if ( !strncmp(argv[c], "--save=", 7) )
                    {
                        char* end = nullptr;
                        const long slot = strtol(argv[c] + 7, &end, 10);
                        if ( end != argv[c] + 7 && *end == '\0' && slot >= 0 && slot <= 99 )
                        {
                            headlessSaveSlot = static_cast<int>(slot);
                            savegameCurrentFileIndex = headlessSaveSlot;
                        }
                        else
                        {
                            printlog("Headless option error: this build accepts --save=<slot> from 0 through 99; path targets are not enabled.");
                        }
                    }
                    else if ( !strncmp(argv[c], "--port=", 7) )
                    {
                        const int requestedPort = atoi(argv[c] + 7);
                        if ( requestedPort > 0 && requestedPort <= 65535 )
                        {
                            headlessServerPort = static_cast<Uint16>(requestedPort);
                        }
                        else
                        {
                            printlog("Headless option error: invalid --port value '%s'. Using %u.", argv[c] + 7, headlessServerPort);
                        }
                    }
                    else if ( !strncmp(argv[c], "--bind=", 7) )
                    {
                        strncpy(headlessBindAddress, argv[c] + 7, sizeof(headlessBindAddress) - 1);
                        headlessBindAddress[sizeof(headlessBindAddress) - 1] = '\0';
                    }
                    else if ( !strncmp(argv[c], "--server-name=", 14) )
                    {
                        strncpy(headlessServerName, argv[c] + 14, sizeof(headlessServerName) - 1);
                        headlessServerName[sizeof(headlessServerName) - 1] = '\0';
                    }
                    else if ( !strncmp(argv[c], "--password=", 11) )
                    {
                        strncpy(headlessServerPassword, argv[c] + 11, sizeof(headlessServerPassword) - 1);
                        headlessServerPassword[sizeof(headlessServerPassword) - 1] = '\0';
                        headlessPasswordRequested = headlessServerPassword[0] != '\0';
                    }
                    else if ( !strcmp(argv[c], "--charactersave_local")
                        || !strcmp(argv[c], "--character-save=local")
                        || !strcmp(argv[c], "--character_save=local")
                        || !strcmp(argv[c], "--charactersave=local") )
                    {
                        characterSaveMode = CharacterSaveMode::LOCAL;
                        printlog("Character save mode: LOCAL (normalized character name is the durable identity).");
                    }
                    else if ( !strcmp(argv[c], "--charactersave_steam")
                        || !strcmp(argv[c], "--character-save=steam")
                        || !strcmp(argv[c], "--character_save=steam")
                        || !strcmp(argv[c], "--charactersave=steam") )
                    {
                        characterSaveMode = CharacterSaveMode::STEAM;
                        printlog("Character save mode: STEAM (Steam ID is the durable identity).");
                    }
                    else if ( !strcmp(argv[c], "--character-save")
                        || !strcmp(argv[c], "--character_save")
                        || !strcmp(argv[c], "--charactersave") )
                    {
                        if ( c + 1 < argc && argv[c + 1] )
                        {
                            const char* mode = argv[++c];
                            if ( !strcmp(mode, "local") )
                            {
                                characterSaveMode = CharacterSaveMode::LOCAL;
                                printlog("Character save mode: LOCAL (normalized character name is the durable identity).");
                            }
                            else if ( !strcmp(mode, "steam") )
                            {
                                characterSaveMode = CharacterSaveMode::STEAM;
                                printlog("Character save mode: STEAM (Steam ID is the durable identity).");
                            }
                            else if ( !strcmp(mode, "none") )
                            {
                                characterSaveMode = CharacterSaveMode::NONE;
                                printlog("Character save mode: disabled.");
                            }
                            else
                            {
                                printlog("Headless option error: character save mode must be local, steam, or none.");
                            }
                        }
                        else
                        {
                            printlog("Headless option error: --character-save requires local, steam, or none.");
                        }
                    }
                    else if ( !strncmp(argv[c], "--character-save-interval=", 26) )
                    {
                        char* end = nullptr;
                        const unsigned long interval = strtoul(argv[c] + 26, &end, 10);
                        const unsigned long maximum = 7UL * 24UL * 60UL * 60UL;
                        if ( end != argv[c] + 26 && *end == '\0' && interval <= maximum )
                        {
                            headlessCharacterSaveIntervalSeconds = static_cast<Uint32>(interval);
                        }
                        else
                        {
                            printlog("Headless option error: --character-save-interval requires seconds from 0 through %lu.", maximum);
                        }
                    }
					else if ( !strcmp(argv[c], "-windowed") )
					{
						fullscreen = 0;
					}
					else if ( !strncmp(argv[c], "-size=", 6) )
					{
						strncpy(tempstr, argv[c] + 6, strcspn(argv[c] + 6, "x"));
						xres = std::max(320, atoi(tempstr));
						yres = std::max(200, atoi(argv[c] + 6 + strcspn(argv[c] + 6, "x") + 1));
					}
					else if ( !strncmp(argv[c], "-map=", 5) )
					{
						strcpy(maptoload, argv[c] + 5);
						loadingmap = true;
					}
					else if ( !strncmp(argv[c], "-gen=", 5) )
					{
						strcpy(maptoload, argv[c] + 5);
						loadingmap = true;
						genmap = true;
					}
					else if ( !strncmp(argv[c], "-config=", 8) )
					{
						strcpy(configtoload, argv[c] + 5);
						loadingconfig = true;
					}
					else if (!strncmp(argv[c], "-quickstart=", 12))
					{
						strcpy(classtoquickstart, argv[c] + 12);
					}
					else if (!strncmp(argv[c], "-datadir=", 9))
					{
						datadirsz = std::min(sizeof(datadir) - 1, strlen(argv[c] + 9));
						strncpy(datadir, argv[c] + 9, datadirsz);
						datadir[datadirsz] = '\0';
					}
					else if ( !strcmp(argv[c], "-nosound") )
					{
						no_sound = true;
					}
					else
					{
#ifdef USE_EOS
						EOS.CommandLineArgs.push_back(argv[c]);
#endif // USE_EOS
					}
				}
			}
		}
        printlog("Data path is %s", datadir);
        printlog("Output path is %s", outputdir);
        if ( headless )
        {
            const char* visibilityName = "loopback-only";
            if ( headlessServerVisibility == HEADLESS_VISIBILITY_LAN )
            {
                visibilityName = "LAN";
            }
            else if ( headlessServerVisibility == HEADLESS_VISIBILITY_PRIVATE )
            {
                visibilityName = "private/unlisted";
            }
            else if ( headlessServerVisibility == HEADLESS_VISIBILITY_PUBLIC )
            {
                visibilityName = "public/listed";
            }

            printlog("HEADLESS-1B enabled: dedicated-server configuration and visibility reporting.");
            printlog("Headless server name: %s", headlessServerName);
            printlog("Requested visibility: %s", visibilityName);
            printlog("Requested bind endpoint: %s:%u", headlessBindAddress, headlessServerPort);
            printlog("Password requested: %s", headlessPasswordRequested ? "yes" : "no");
            printlog("Late joining requested: %s", headlessLateJoinRequested ? "yes" : "no");
            printlog("Autosave interval: %u seconds", headlessAutosaveIntervalSeconds);
            printlog("Character save mode: %s", characterSaveMode == CharacterSaveMode::LOCAL
                ? "local-name" : characterSaveMode == CharacterSaveMode::STEAM ? "steam-id" : "disabled");
            printlog("Character save request interval: %u seconds", headlessCharacterSaveIntervalSeconds);
            printlog("Save slot: %d", headlessSaveSlot >= 0 ? headlessSaveSlot : savegameCurrentFileIndex);
            printlog("SECURITY STATUS: plain --headless opens no socket; explicit hosting mode is required.");
            printlog("SECURITY STATUS: --LAN opens the existing direct-connect UDP lobby. Public/password modes fail closed until implemented safely.");
            printlog("HEADLESS-1B note: rendering resources are still initialized through a hidden OpenGL context for compatibility.");

            char statusPath[PATH_MAX];
            snprintf(statusPath, sizeof(statusPath), "%s/headless_server_status.txt", outputdir);
            FILE* statusFile = fopen(statusPath, "w");
            if ( statusFile )
            {
                fprintf(statusFile, "Barony Automatia Headless Server Status\n");
                fprintf(statusFile, "State: startup configuration loaded; listener opens only for supported explicit hosting modes\n");
                fprintf(statusFile, "Server name: %s\n", headlessServerName);
                fprintf(statusFile, "Visibility: %s\n", visibilityName);
                fprintf(statusFile, "Bind endpoint: %s:%u\n", headlessBindAddress, headlessServerPort);
                fprintf(statusFile, "Password requested: %s\n", headlessPasswordRequested ? "yes" : "no");
                fprintf(statusFile, "Late joining requested: %s\n", headlessLateJoinRequested ? "yes" : "no");
                fprintf(statusFile, "Autosave interval: %u seconds\n", headlessAutosaveIntervalSeconds);
                fprintf(statusFile, "Character save mode: %s\n", characterSaveMode == CharacterSaveMode::LOCAL
                    ? "local-name" : characterSaveMode == CharacterSaveMode::STEAM ? "steam-id" : "disabled");
                fprintf(statusFile, "Character save request interval: %u seconds\n", headlessCharacterSaveIntervalSeconds);
                fprintf(statusFile, "Save slot: %d\n", headlessSaveSlot >= 0 ? headlessSaveSlot : savegameCurrentFileIndex);
                fprintf(statusFile, "Important: --LAN opens the direct-connect UDP listener; plain --headless stays idle.\n");
                fclose(statusFile);
                printlog("Headless status file: %s", statusPath);
            }
            else
            {
                printlog("Warning: could not write headless status file '%s'.", statusPath);
            }
        }
        
        // init sdl
        Uint32 init_flags = SDL_INIT_VIDEO | SDL_INIT_EVENTS;
        if ( !headless )
        {
            init_flags |= SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC;
        }
        if (SDL_Init(init_flags) == -1)
        {
            printlog("failed to initialize SDL: %s\n", SDL_GetError());
            return 1;
        }

        // load game config
		Input::defaultBindings();
        MainMenu::settingsReset();
        MainMenu::settingsApply();
		bool load_successful = MainMenu::settingsLoad();
		if ( load_successful ) {
			MainMenu::settingsApply();
		}
		else {
			skipintro = false;
		}

		// initialize map
		map.tiles = nullptr;
		map.entities = (list_t*) malloc(sizeof(list_t));
		map.entities->first = nullptr;
		map.entities->last = nullptr;
		map.creatures = new list_t;
		map.creatures->first = nullptr;
		map.creatures->last = nullptr;
		map.worldUI = new list_t;
		map.worldUI->first = nullptr;
		map.worldUI->last = nullptr;

		// initialize engine
		if ( (c = initApp("Barony", fullscreen)) )
		{
			printlog("Critical error: %d\n", c);
#ifdef STEAMWORKS
			SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Uh oh",
									"Barony has encountered a critical error and cannot start.\n\n"
									"Please check the log.txt file in the game directory for additional info\n"
									"and verify Steam is running. Alternatively, contact us through our website\n"
									"at https://www.baronygame.com/ for support.",
				screen);
#elif defined USE_EOS
			if ( EOS.appRequiresRestart == EOS_EResult::EOS_Success )
			{
				// restarting app from launcher.
			}
			else
			{
				SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Uh oh",
					"Barony has encountered a critical error and cannot start.\n\n"
					"Please check the log.txt file in the game directory for additional info,\n"
					"and verify the game is launched through the Epic Games Store. \n"
					"Alternatively, contact us through our website at https://www.baronygame.com/ for support.",
					screen);
			}
#else
			SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Uh oh",
									"Barony has encountered a critical error and cannot start.\n\n"
									"Please check the log.txt file in the game directory for additional info,\n"
									"or contact us through our website at https://www.baronygame.com/ for support.",
									screen);
#endif
			deinitApp();
			exit(c);
		}

		MainMenu::randomizeUsername();

		// init message
		printlog("Barony version: %s\n", VERSION);
		char buffer[32];
        getTimeAndDateFormatted(getTime(), buffer, sizeof(buffer));
		printlog("Launch time: %s\n", buffer);

		if ( (c = initGame()) )
		{
			printlog("Critical error in initGame: %d\n", c);
			SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Uh oh",
			                         "Barony has encountered a critical error and cannot start.\n\n"
			                         "Please check the log.txt file in the game directory for additional info,\n"
			                         "or contact us through our website at https://www.baronygame.com/ for support.",
			                         screen);
			deinitGame();
			deinitApp();
			exit(c);
		}
		initialized = true;

		// initialize player conducts
		setDefaultPlayerConducts();

#ifdef NINTENDO
		if (!nxIsHandheldMode()) {
			nxAssignControllers(1, 1, true, false, true, false, nullptr);
		}
		for (int c = 0; c < MAX_SPLITSCREEN; ++c) {
			game_controllers[c].open(0, c); // first parameter is not used by Nintendo.
			bindControllerToPlayer(c, c);
		}
		//inputs.setPlayerIDAllowedKeyboard(-1);
#endif

		// play splash sound
#ifdef MUSIC
        if ( !headless )
        {
            playMusic(splashmusic, false, false, false);
        }
#endif

		int indev_timer = 0;

		// main loop

		printlog("running main loop.\n");
		while (mainloop)
		{
			Frame::numFindFrameCalls = 0;
			// record the time at the start of this cycle
			lastGameTickCount = SDL_GetPerformanceCounter();
			DebugStats.t1StartLoop = std::chrono::high_resolution_clock::now();

#ifdef USE_IMGUI
			if ( ImGui_t::queueInit )
			{
				ImGui_t::queueInit = false;
				if ( !ImGui_t::isInit )
				{
					ImGui_t::init();
				}
			}
			if ( ImGui_t::queueDeinit )
			{
				ImGui_t::queueDeinit = false;
				if ( ImGui_t::isInit )
				{
					ImGui_t::deinit();
				}
			}
#endif

			doConsoleCommands();

			// game logic
			if ( !intro )
			{
				// handle network messages
				// only run up to % framerate interval (1 / (fps * networkTickrate))
				if ( networkTickrate == 0 )
				{
					networkTickrate = 2;
				}
				if ( multiplayer == CLIENT )
				{
					clientHandleMessages(fpsLimit * networkTickrate);
				}
				else if ( multiplayer == SERVER )
				{
					serverHandleMessages(fpsLimit * networkTickrate);
				}
			}
			DebugStats.t21PostHandleMessages = std::chrono::high_resolution_clock::now();
			bool ranframes = handleEvents();
			DebugStats.t2PostEvents = std::chrono::high_resolution_clock::now();
#ifdef DEBUG_EVENT_TIMERS
			real_t accum = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(DebugStats.t2PostEvents - DebugStats.t21PostHandleMessages).count();
			if ( accum > 5.0 )
			{
				printlog("Long tick time: %f", accum);
			}
#endif
			// handle steam callbacks
#ifdef STEAMWORKS
			if ( g_SteamLeaderboards )
			{
				g_SteamLeaderboards->ProcessLeaderboardUpload();
			}
			SteamAPI_RunCallbacks();
#endif
#ifdef USE_PLAYFAB
			PlayFab::PlayFabClientAPI::Update();
			playfabUser.update();
#endif
#ifdef USE_EOS
			if (EOS.PlatformHandle) {
				EOS_Platform_Tick(EOS.PlatformHandle);
			}
			if (EOS.ServerPlatformHandle) {
				EOS_Platform_Tick(EOS.ServerPlatformHandle);
			}
			EOS.StatGlobalManager.updateQueuedStats();
			EOS.AccountManager.handleLogin();
			EOS.CrossplayAccountManager.handleLogin();
#endif // USE_EOS

			DebugStats.t3SteamCallbacks = std::chrono::high_resolution_clock::now();

#ifdef USE_IMGUI
			ImGui_t::update();
#endif

			for ( int i = 0; i < MAXPLAYERS; ++i )
			{
				if ( nohud || intro || !players[i]->isLocalPlayer() || !MainMenu::isPlayerSignedIn(i) )
				{
					if (gameUIFrame[i])
					{
						gameUIFrame[i]->setDisabled(true);
					}
					StatusEffectQueue[i].resetQueue();
				}
				else
				{
					if (gameUIFrame[i])
					{
						gameUIFrame[i]->setDisabled(false);
					}
				}
			}

            if ( headless )
            {
                MainMenu::headlessDedicatedServerTick();
            }

			if ( intro )
			{
				for ( int i = 0; i < MAXPLAYERS; ++i )
				{
					players[i]->shootmode = false; //Hack because somebody put a shootmode = true where it don't belong, which might and does break stuff.
				}
				if ( introstage == -1 )
				{
					// hack to fix these things from breaking everything...
					for ( int i = 0; i < MAXPLAYERS; ++i )
					{
						players[i]->hud.arm = nullptr;
						players[i]->hud.weapon = nullptr;
						players[i]->hud.magicLeftHand = nullptr;
						players[i]->hud.magicRightHand = nullptr;
						players[i]->hud.magicRangefinder = nullptr;
						players[i]->ghost.reset();
						FollowerMenu[i].recentEntity = nullptr;
						FollowerMenu[i].followerToCommand = nullptr;
						FollowerMenu[i].entityToInteractWith = nullptr;
						CalloutMenu[i].closeCalloutMenuGUI();
						CalloutMenu[i].callouts.clear();
					}

					// black background
					drawRect(NULL, 0, 255);

#ifdef USE_FMOD
					// fmod logo
					auto fmod_logo = Image::get("images/system/fmod-logo.png");
					int w = fmod_logo->getWidth() / 3;
					int h = fmod_logo->getHeight() / 3;
					fmod_logo->drawColor(nullptr,
					    SDL_Rect{xres - w - 16, yres - h - 16, w, h},
					    SDL_Rect{0, 0, xres, yres}, makeColor(150, 150, 150, 255));
#endif

					// team splash
                    const float factor = xres / 1280.f;
					drawGear(xres / 2, yres / 2, gearsize, gearrot);
					drawLine(xres / 2 - 160 * factor, yres / 2 + 112 * factor, xres / 2 + 160 * factor, yres / 2 + 112 * factor, makeColorRGB(255, 32, 0), std::min<Uint16>(logoalpha, 255));
                    auto text = Text::get("Turning Wheel", "fonts/pixel_maz.ttf#32#2", makeColorRGB(255, 255, 255), makeColorRGB(0, 0, 0));
                    const SDL_Rect r{
                        (int)(xres - text->getWidth() * factor) / 2,
                        (int)(yres / 2 + 128 * factor),
                        (int)(text->getWidth() * factor),
                        (int)(text->getHeight() * factor)};
                    text->drawColor(SDL_Rect{0,0,0,0}, r, SDL_Rect{0, 0, xres, yres}, makeColor(255, 255, 255, std::min(logoalpha, (Uint16)255)));
					if ( logoalpha >= 255 && !fadeout )
					{
						fadeout = true;
						fadefinished = false;
					}

					bool skipButtonPressed = false;
					for ( int i = 0; i < MAXPLAYERS; ++i )
					{
						if ( Input::inputs[i].consumeBinaryToggle("MenuConfirm") )
						{
							skipButtonPressed = true;
						}
						if ( Input::inputs[i].consumeBinaryToggle("MenuCancel") )
						{
							skipButtonPressed = true;
						}
					}
					if ( Input::keys[SDLK_ESCAPE] )
					{
						Input::keys[SDLK_ESCAPE] = 0;
						skipButtonPressed = true;
					}

					if ( fadefinished || skipButtonPressed )
					{
						fadealpha = 255;
						int menuMapType = 0;
						switch ( local_rng.rand() % 4 ) // STEAM VERSION INTRO
						{
							case 0:
							case 1:
								menuMapType = loadMainMenuMap(true, false);
								break;
							case 2:
							case 3:
								menuMapType = loadMainMenuMap(false, false);
								break;
							default:
								break;
						}
						numplayers = 0;
						multiplayer = 0;
						assignActions(&map);
						generatePathMaps();
						introstage = 1;
						if ( !skipintro && !strcmp(classtoquickstart, "") )
						{
							MainMenu::beginFade(MainMenu::FadeDestination::IntroStoryScreenNoMusicFade);
						}
						else
						{
							MainMenu::beginFade(MainMenu::FadeDestination::TitleScreen);
						}
					}
				}
				else if ( introstage == 0 )
				{
					// DEPRECATED
				}
				else
				{
					if (classtoquickstart[0] != '\0')
					{
						for ( c = 0; c <= CLASS_MONK; c++ ) {
							if ( !strcmp(classtoquickstart, playerClassLangEntry(c, 0)) ) {
								client_classes[0] = c;
								break;
							}
						}
						strcpy(classtoquickstart, "");

						// set class loadout
						strcpy(stats[0]->name, "Avatar");
						stats[0]->sex = static_cast<sex_t>(local_rng.rand() % 2);
						stats[0]->stat_appearance = local_rng.rand() % NUMAPPEARANCES;
						stats[0]->clearStats();
						initClass(0);
						if ( stats[0]->playerRace != RACE_HUMAN )
						{
							stats[0]->stat_appearance = 0;
						}

						// generate unique game key
						local_rng.seedTime();
						local_rng.getSeed(&uniqueGameKey, sizeof(uniqueGameKey));
						uniqueLobbyKey = local_rng.getU32();
						net_rng.seedBytes(&uniqueGameKey, sizeof(uniqueGameKey));
						doNewGame(false);
					}
					else
					{
						// draws the menu level "backdrop"
						drawClearBuffers();
						if ( !MainMenu::isCutsceneActive() && fadealpha < 255 )
						{
							menucam.winx = 0;
							menucam.winy = 0;
							menucam.winw = xres;
							menucam.winh = yres;
							light = addLight(menucam.x, menucam.y, "mainmenu");
							occlusionCulling(map, menucam);
                            beginGraphics();
							glBeginCamera(&menucam, true, map);
							glDrawWorld(&menucam, REALCOLORS);
							drawEntities3D(&menucam, REALCOLORS);
							glEndCamera(&menucam, true, map);
							list_RemoveNode(light->node);
						}

						MainMenu::doMainMenu(!intro);
						UIToastNotificationManager.drawNotifications(MainMenu::isCutsceneActive(), true); // draw this before the cursor
                        framesProcResult = doFrames();

#ifdef USE_IMGUI
						ImGui_t::render();
#endif
#ifndef NINTENDO
						Compendium_t::updateTooltip();

						// draw mouse
						// only draw 1 cursor in the main menu
						if ( inputs.getVirtualMouse(inputs.getPlayerIDAllowedKeyboard())->draw_cursor )
						{
                            const float factorX = (float)xres / Frame::virtualScreenX;
                            const float factorY = (float)yres / Frame::virtualScreenY;
							auto cursor = Image::get("*#images/system/cursor_hand.png");
                            const int w = cursor->getWidth() * factorX;
                            const int h = cursor->getHeight() * factorY;

							if ( inputs.getPlayerIDAllowedKeyboard() >= 0 )
							{
								real_t& mouseAnim = inputs.getVirtualMouse(inputs.getPlayerIDAllowedKeyboard())->mouseAnimationPercent;
								if ( Input::inputs[inputs.getPlayerIDAllowedKeyboard()].binary("MenuLeftClick") )
								{
									mouseAnim = .5;
								}
								if ( mouseAnim > .25 )
								{
									cursor = Image::get("*#images/system/cursor_hand2.png");
								}
								if ( mouseAnim > 0.0 )
								{
									mouseAnim -= .05 * getFPSScale(144.0);
								}

								pos.x = inputs.getMouse(inputs.getPlayerIDAllowedKeyboard(), Inputs::X) - (mouseAnim * w / 7) - w / 2;
								pos.y = inputs.getMouse(inputs.getPlayerIDAllowedKeyboard(), Inputs::Y) - (mouseAnim * h / 7) - h / 2;
								pos.x += 4;
								pos.y += 4;
								pos.w = w;
								pos.h = h;
								cursor->draw(nullptr, pos, SDL_Rect{ 0, 0, xres, yres });

								if ( MainMenu::cursor_delete_mode )
								{
									auto icon = Image::get("*#images/system/Broken.png");
									pos.x = pos.x + pos.w;
									pos.y = pos.y + pos.h;
									pos.w = icon->getWidth() * 2;
									pos.h = icon->getHeight() * 2;
									icon->draw(nullptr, pos, SDL_Rect{ 0, 0, xres, yres });
								}
							}
							else
							{
								pos.x = inputs.getMouse(inputs.getPlayerIDAllowedKeyboard(), Inputs::X) - w / 2;
								pos.y = inputs.getMouse(inputs.getPlayerIDAllowedKeyboard(), Inputs::Y) - h / 2;
								pos.x += 4;
								pos.y += 4;
								pos.w = w;
								pos.h = h;
								cursor->draw(nullptr, pos, SDL_Rect{0, 0, xres, yres});

								if (MainMenu::cursor_delete_mode)
								{
									auto icon = Image::get("*#images/system/Broken.png");
									pos.x = pos.x + pos.w;
									pos.y = pos.y + pos.h;
									pos.w = icon->getWidth() * 2;
									pos.h = icon->getHeight() * 2;
									icon->draw(nullptr, pos, SDL_Rect{0, 0, xres, yres});
								}
							}
						}
#endif
					}
				}
			}
			else
			{
				if ( multiplayer == CLIENT )
				{
					// make sure shop inventory is alloc'd
					for ( int i = 0; i < MAXPLAYERS; ++i )
					{
						if ( !shopInv[i] )
						{
							shopInv[i] = (list_t*) malloc(sizeof(list_t));
							shopInv[i]->first = NULL;
							shopInv[i]->last = NULL;
						}
					}
				}
#ifdef MUSIC
				handleLevelMusic();
#endif
				DebugStats.t4Music = std::chrono::high_resolution_clock::now();

				// toggling the game menu
				bool doPause = false;
				bool doCompendium = false;
				if ( !fadeout )
				{
				    bool noOneUsingKeyboard = true;
					for ( int i = 0; i < MAXPLAYERS; ++i )
					{
					    if (inputs.bPlayerUsingKeyboardControl(i) && MainMenu::isPlayerSignedIn(i) && players[i]->isLocalPlayer()) {
					        noOneUsingKeyboard = false;
					        break;
					    }
					}
					for ( int i = 0; i < MAXPLAYERS; ++i )
					{
						if ( !players[i]->isLocalPlayer() )
						{
							continue;
						}
						const bool escapePressed =
							inputs.bPlayerUsingKeyboardControl(i) &&
							keystatus[SDLK_ESCAPE] &&
							!Input::inputs[i].isDisabled();
						if ( (Input::inputs[i].consumeBinaryToggle("Pause Game") || escapePressed) && !command )
						{
							if ( !players[i]->shootmode )
							{
								players[i]->closeAllGUIs(CLOSEGUI_ENABLE_SHOOTMODE, CLOSEGUI_CLOSE_ALL);
								players[i]->characterSheet.attributespage = 0;
								if (escapePressed) {
									keystatus[SDLK_ESCAPE] = false;
								}
							}
							else
							{
								doPause = true;
							}
							break;
						}
						const bool compendiumOpenToggle =
							!intro
							&& inputs.bPlayerUsingKeyboardControl(i)
							&& !Input::inputs[i].isDisabled()
							&& !command;
						if ( compendiumOpenToggle )
						{
							if ( Input::inputs[i].consumeBinaryToggle("Compendium") )
							{
								doCompendium = true;
							}
						}
					}
					if (noOneUsingKeyboard && keystatus[SDLK_ESCAPE]) {
					    doPause = true;
					}
				}
				if ( doPause )
				{
					if ( !gamePaused )
					{
						pauseGame(0, MAXPLAYERS);
					}
					else 
					{
						if ( MainMenu::main_menu_frame )
						{
							int dimmers = 0;
							for ( auto frame : MainMenu::main_menu_frame->getFrames() )
							{
								if ( !strcmp(frame->getName(), "dimmer") )
								{
									++dimmers;
								}
							}
							if ( dimmers == 0 )
							{
								pauseGame(0, MAXPLAYERS);
							}
							else
							{
								keystatus[SDLK_ESCAPE] = 0;
							}
						}
						else
						{
							pauseGame(0, MAXPLAYERS);
						}
					}
				}
				else if ( doCompendium )
				{
					bool isOpen = false;
					if ( MainMenu::main_menu_frame )
					{
						if ( auto compendium = MainMenu::main_menu_frame->findFrame("compendium") )
						{
							isOpen = true;
						}
					}
					if ( !isOpen )
					{
						if ( !gamePaused )
						{
							pauseGame(0, MAXPLAYERS);
						}
						if ( !gamePaused )
						{
							doCompendium = false;
						}
					}
					else if ( isOpen )
					{
						if ( gamePaused )
						{
							pauseGame(0, MAXPLAYERS);
							doCompendium = false;
							if ( !gamePaused )
							{
								MainMenu::openCompendium(); // will close
							}
						}
					}
				}

				// main drawing
				drawClearBuffers();

				if ( TimerExperiments::bUseTimerInterpolation )
				{
					if ( !(!gamePaused || (multiplayer && !client_disconnected[0])) )
					{
						// game paused, dont't process any velocities for camera
						for ( int c = 0; c < MAXPLAYERS; ++c )
						{
							TimerExperiments::cameraCurrentState[c].resetMovement();
						}
					}
					TimerExperiments::updateClocks();
					//std::string timerOutput = TimerExperiments::render(TimerExperiments::cameraRenderState[0].yaw);
					//cameras[0].x = players[0]->entity->x / 16.0;//TimerExperiments::cameraRenderState.x.position;
					//cameras[0].y = players[0]->entity->y / 16.0;//TimerExperiments::cameraRenderState.y.position;
					//cameras[0].z = TimerExperiments::cameraRenderState.z.position;
					//printTextFormatted(font8x8_bmp, 8, 20, "%s", timerOutput.c_str());
					for ( node_t* node = map.entities->first; node; node = node->next )
					{
						entity = (Entity*)node->element;
						if ( entity->bUseRenderInterpolation )
						{
							entity->lerp_ox = entity->x;
							entity->lerp_oy = entity->y;
							entity->x = entity->lerpRenderState.x.position * 16.0;
							entity->y = entity->lerpRenderState.y.position * 16.0;
						}
					}
				}


				for (int c = 0; c < MAXPLAYERS; ++c) 
				{
					auto& camera = cameras[c];
					auto& cvars = cameravars[c];
					TimerExperiments::renderCameras(camera, c);

					real_t mult = 1.0;
					if ( !intro && stats[c]->getEffectActive(EFF_DELAY_PAIN) )
					{
						mult = 0.25;
					}
					camera.ang += mult * cvars.shakex2;
					camera.vang += mult * cvars.shakey2 / 200.0;

					for ( auto& HPBar : enemyHPDamageBarHandler[c].HPBars )
					{
						HPBar.second.updateWorldCoordinates(); // update enemy bar world coordinates before drawEntities3D called
					}
					players[c]->worldUI.worldTooltipDialogue.playerDialogue.updateWorldCoordinates(); // update dialogue world coordinates before drawEntities3D called
					for ( auto& worldTooltipDialogue : players[c]->worldUI.worldTooltipDialogue.sharedDialogues )
					{
						worldTooltipDialogue.second.updateWorldCoordinates();
					}
				}

				if ( !MainMenu::isCutsceneActive() && fadealpha < 255 )
				{
					drawAllPlayerCameras();
				}

				if ( TimerExperiments::bUseTimerInterpolation )
				{
					for ( node_t* node = map.entities->first; node; node = node->next )
					{
						entity = (Entity*)node->element;
						if ( entity->bUseRenderInterpolation )
						{
							entity->x = entity->lerp_ox;
							entity->y = entity->lerp_oy;
						}
					}
				}


				for ( int player = 0; player < MAXPLAYERS; ++player )
				{
					players[player]->messageZone.updateMessages();
					CalloutMenu[player].update();
				}
				if ( !nohud )
				{
					DamageIndicatorHandler.update();
					for ( int player = 0; player < MAXPLAYERS; ++player )
					{
						if ( players[player]->isLocalPlayer() )
						{
							players[player]->messageZone.drawMessages();
						}
					}
				}

				DebugStats.t5MainDraw = std::chrono::high_resolution_clock::now();
				framesProcResult = doFrames();
				DebugStats.t6Messages = std::chrono::high_resolution_clock::now();
				ingameHud();
				Compendium_t::updateTooltip();

                static ConsoleVariable<bool> showConsumeMouseInputs("/debug_consume_mouse", false);
                if (!framesProcResult.usable) {
				    if (*showConsumeMouseInputs) {
				        printText(font8x8_bmp, 16, 16, "eating mouse input");
				    }
				} else {
				    if (*showConsumeMouseInputs) {
				        printText(font8x8_bmp, 16, 16, "NOT eating mouse input");
				    }
				}

				if ( gamePaused )
				{
					MainMenu::doMainMenu(!intro);
				}
				else
				{
					MainMenu::destroyMainMenu();

					// draw subwindow
					if ( subwindow )
					{
						drawWindowFancy(subx1, suby1, subx2, suby2);
						if ( subtext[0] != '\0')
						{
							if ( strncmp(subtext, Language::get(1133), 12) )
							{
								ttfPrintTextFormatted(ttf12, subx1 + 8, suby1 + 8, subtext);
							}
							else
							{
								ttfPrintTextFormatted(ttf16, subx1 + 8, suby1 + 8, subtext);
							}
						}
					}

					// process button actions
					handleButtons();
				}

				if ( doCompendium )
				{
					if ( gamePaused )
					{
						MainMenu::openCompendium();
					}
				}

				if ( gamePaused ) // draw after main menu windows etc.
				{
					UIToastNotificationManager.drawNotifications(MainMenu::isCutsceneActive(), true); // draw this before the cursor
				}

#ifdef USE_IMGUI
				ImGui_t::render();
#endif

#ifndef NINTENDO
				for ( int i = 0; i < MAXPLAYERS; ++i )
				{
					if ( gamePaused || players[i]->GUI.isGameoverActive() )
					{
						if ( inputs.bPlayerUsingKeyboardControl(i) && inputs.getVirtualMouse(i)->draw_cursor )
						{
                            const float factorX = (float)xres / Frame::virtualScreenX;
                            const float factorY = (float)yres / Frame::virtualScreenY;
							auto cursor = Image::get("*#images/system/cursor_hand.png");
                            const int w = cursor->getWidth() * factorX;
                            const int h = cursor->getHeight() * factorY;

							real_t& mouseAnim = inputs.getVirtualMouse(i)->mouseAnimationPercent;
							if ( Input::inputs[i].binary("MenuLeftClick") )
							{
								mouseAnim = .5;
							}
							if ( mouseAnim > .25 )
							{
								cursor = Image::get("*#images/system/cursor_hand2.png");
							}
							if ( mouseAnim > 0.0 )
							{
								mouseAnim -= .05 * getFPSScale(144.0);
							}

                            pos.x = inputs.getMouse(i, Inputs::X) - (mouseAnim * w / 7) - w / 2;
                            pos.y = inputs.getMouse(i, Inputs::Y) - (mouseAnim * h / 7) - h / 2;
                            pos.x += 4;
                            pos.y += 4;
                            pos.w = w;
                            pos.h = h;
							cursor->draw(nullptr, pos, SDL_Rect{ 0, 0, xres, yres });
						}
						continue;
					}
				}
#endif
			}

			// fade in/out effect
			if ( fadealpha > 0 )
			{
				drawRect(NULL, makeColor(0, 0, 0, 255), fadealpha);
			}

			// fps counter
			if ( showfps )
			{
			    printTextFormatted(font16x16_bmp, 8, 8, "fps = %3.1f", fps);
			}
			if ( enableDebugKeys )
			{
				printTextFormatted(font8x8_bmp, 8, 20, "gui module: %d\ngui mode: %d", players[0]->GUI.activeModule, players[0]->gui_mode);
				static ConsoleVariable<bool> cvar_map_debug("/mapdebug", false);
				if ( *cvar_map_debug )
				{
					int ix = (int)cameras[clientnum].x;
					int iy = (int)cameras[clientnum].y;
					if ( ix >= 0 && ix < map.width && iy >= 0 && iy < map.height )
					{
						printTextFormatted(font8x8_bmp, 8, 44, "pos: x: %d y: %d pathmapZone: %d", 
							ix, iy, pathMapGrounded[iy + ix * map.height]);
					}
				}
				static ConsoleVariable<bool> cvar_light_debug("/lightdebug", false);
				if ( *cvar_light_debug )
				{
					if ( players[clientnum]->entity )
					{
						int light = players[clientnum]->entity->entityLight();
						int tiles = light / 16;
						int lightAfterReductions = std::max(TOUCHRANGE, 
							players[clientnum]->entity->entityLightAfterReductions(*stats[clientnum], players[clientnum]->entity));
						int tilesAfterReductions = lightAfterReductions / 16;
						printTextFormatted(font8x8_bmp, 8, 44, "base light: %3d, tiles: %2d | modified light: %3d, tiles: %2d",
							light, tiles, lightAfterReductions, tilesAfterReductions);
					}
				}
			}

			DebugStats.t10FrameLimiter = std::chrono::high_resolution_clock::now();
			if ( logCheckMainLoopTimers )
			{
				std::chrono::duration<double> time_span = 
					std::chrono::duration_cast<std::chrono::duration<double>>(DebugStats.t10FrameLimiter - DebugStats.t11End);
				double timer = time_span.count() * 1000;
				if ( timer > ((1000.f / (fps) * 1.4)) )
				{
					DebugStats.displayStats = true;
					DebugStats.storeStats();
					DebugStats.storeEventStats();
					messagePlayer(clientnum, MESSAGE_MISC, "Timers: %f total.", timer);
				}
				if ( DebugStats.displayStats )
				{
					printTextFormatted(font8x8_bmp, 8, 200 + 20, DebugStats.debugOutput);
					printTextFormatted(font8x8_bmp, 8, 200 + 100, DebugStats.debugEventOutput);
				}
			}

			DebugTimers.printAllTimepoints();
			DebugTimers.clearAllTimepoints();

			static ConsoleVariable<bool> cvar_frame_search_count("/framesearchcount", false);
			if ( *cvar_frame_search_count )
			{
				printTextFormatted(font8x8_bmp, 300, 32, "findFrame() calls: %d / loop", Frame::numFindFrameCalls);
			}

			UIToastNotificationManager.drawNotifications(MainMenu::isCutsceneActive(), false);

#ifdef USE_THEORA_VIDEO
			for ( int i = 0; i < MAXPLAYERS; ++i )
			{
				VideoManager[i].update();
			}
#endif

			// update screen
			GO_SwapBuffers(screen);

			// screenshots
			if (Input::inputs[clientnum].consumeBinaryToggle("Screenshot") ||
			    (inputs.hasController(clientnum) && Input::inputs[clientnum].consumeBinaryToggle("GamepadScreenshot")))
			{
                if (!hdrEnabled) {
                    main_framebuffer.unbindForWriting();
                }
				takeScreenshot();
                if (!hdrEnabled) {
                    main_framebuffer.bindForWriting();
                }
			}

			// frame rate limiter
			while ( frameRateLimit(fpsLimit, true, true) )
			{
				if ( !intro )
				{
					// handle network messages
					if ( multiplayer == CLIENT )
					{
						clientHandleMessages(fpsLimit);
					}
					else if ( multiplayer == SERVER )
					{
						serverHandleMessages(fpsLimit);
					}
				}
			}

			for ( int i = 0; i < MAXPLAYERS; ++i )
			{
			    Input& input = Input::inputs[i];

				// selectedEntityGimpTimer will only allow the game to process a right click entity click 1-2 times
				// otherwise if we interacted with a menu the gimp timer does not increment. (it would have auto reset the status of IN_USE)
				if ( !input.binaryToggle("Use") )
				{
					players[i]->movement.selectedEntityGimpTimer = 0;
				}
				else
				{
					if ( players[i]->movement.selectedEntityGimpTimer >= 2 )
					{
					    input.consumeBinaryToggle("Use");
					}
				}

				players[i]->GUI.clearHoveringOverModuleButton();
			}

			if (ranframes)
			{
				Input::mouseButtons[Input::MOUSE_WHEEL_UP] = false;
				Input::mouseButtons[Input::MOUSE_WHEEL_DOWN] = false;
				mousexrel = 0;
				mouseyrel = 0;
				if (initialized)
				{
					inputs.updateAllRelMouse();
					for ( int i = 0; i < MAXPLAYERS; ++i )
					{
						if ( inputs.hasController(i) )
						{
							if (inputs.getController(i))
							{
#ifndef NINTENDO
								(void)inputs.getController(i)->handleRumble();
#endif
								inputs.getController(i)->updateButtonsReleased();
							}
						}
					}
				}
			}
			Text::dumpCacheInMainLoop();
			achievementObserver.getCurrentPlayerUids();
			DebugStats.t11End = std::chrono::high_resolution_clock::now();

			// increase the cycle count
			cycles++;
		}
		if ( !load_successful ) {
			skipintro = true;
		}

		// if alt+f4 or closing window suddenly this will restore flags to lobby settings
		gameModeManager.currentSession.restoreSavedServerFlags();

		saveConfig("default.cfg");
		MainMenu::settingsMount(false);
		(void)MainMenu::settingsSave();

		// deinit
		deinitGame();
		return deinitApp();
	}
#ifndef NINTENDO
	catch (const std::exception &exc)
	{
		// catch anything thrown within try block that derives from std::exception
		std::cerr << "UNHANDLED EXCEPTION CAUGHT: " << exc.what() << "\n";
		return 1;
	}
	catch (...)
	{
		//TODO:
		std::cerr << "UNKNOWN EXCEPTION CAUGHT!\n";
		return 1;
	}
#endif // NINTENDO

#ifdef NINTENDO
	nxTerm();
#endif // NINTENDO
}

void DebugStatsClass::storeStats()
{
	if ( !displayStats )
	{
		return;
	}
	storeOldTimePoints();
	double out1 = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(t2Stored - t21Stored).count();
	double out2 = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(t3Stored - t2Stored).count();
	double out3 = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(t5Stored - t4Stored).count();
	double out4 = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(t6Stored - t5Stored).count();
	double out5 = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(t7Stored - t6Messages).count();
	double out6 = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(t8Stored - t7Stored).count();
	double out7 = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(t9Stored - t8Stored).count();
	double out8 = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(t10Stored - t9Stored).count();
	double out9 = -1000 * std::chrono::duration_cast<std::chrono::duration<double>>(t11Stored - t10Stored).count();
	double out10 = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(t21Stored - t1Stored).count();
	snprintf(debugOutput, 1023,
		"Messages: %4.5fms\nEvents: %4.5fms\nSteamCallbacks: %4.5fms\nMainDraw: %4.5fms\nMessages: %4.5fms\nInputs: %4.5fms\nStatus: %4.5fms\nGUI: %4.5fms\nFrameLimiter: %4.5fms\nEnd: %4.5fms\n",
		out10, out1, out2, out3, out4, out5, out6, out7, out8, out9);
}

void DebugStatsClass::storeEventStats()
{
	double out1 = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(eventsT2stored - eventsT1stored).count();
	double out2 = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(eventsT3stored - eventsT2stored).count();
	double out3 = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(eventsT4stored - eventsT3stored).count();
	double out4 = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(eventsT5stored - eventsT4stored).count();
	double out5 = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(eventsT6stored - eventsT5stored).count();

	double messages1 = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(messagesT1stored - t1StartLoop).count();
	double messages2 = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(t21Stored - messagesT1stored).count();

	snprintf(debugEventOutput, 1023,
		"Events1: %4.5fms\nEvents2: %4.5fms\nEvents3: %4.5fms\nEvents4: %4.5fms\nEvents5: %4.5fms\nMessagesT1: %4.5fms\nMessagesT2: %4.5fms\n",
		out1, out2, out3, out4, out5, messages1, messages2);
}
