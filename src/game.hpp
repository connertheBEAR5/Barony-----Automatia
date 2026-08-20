/*-------------------------------------------------------------------------------

	BARONY
	File: game.hpp
	Desc: header file for the game

	Copyright 2013-2016 (c) Turning Wheel LLC, all rights reserved.
	See LICENSE for details.

-------------------------------------------------------------------------------*/

#pragma once

#include <vector>
#include <cstdint>
#include <chrono>
#include <string>
#include <memory>
#include <unordered_map>
#ifdef STEAMWORKS
#include <steam/steam_api.h>
#include "steam.hpp"
#endif

#include "interface/consolecommand.hpp"

#include "Config.hpp"

// REMEMBER TO CHANGE THIS WITH EVERY NEW OFFICIAL VERSION!!!
#ifdef NINTENDO
static const char VERSION[] = "v5.0.2";
#else
static const char VERSION[] = "v5.0.2";

/* Player-scoped mutable NPC dialogue memory. */
void customDialogueCreditAuthoredDefeat(
    const int defeatID,
    const Uint32 defeatedUID
);

// Clears session-only custom dialogue state when restarting or starting a new game.
void customDialogueResetRuntimeState();
void customDialogueResetPlayerRuntimeState(int player);

bool persistentStorySetNPCNode(const int player, const std::string& mapName, const Sint32 persistentID, const Sint32 nodeID);
Sint32 persistentStoryGetNPCNode(const int player, const std::string& mapName, const Sint32 persistentID, const Sint32 fallbackNode);
bool persistentStorySetNPCVariable(const int player, const std::string& mapName, const Sint32 persistentID, const std::string& variableID, const Sint32 value);
Sint32 persistentStoryGetNPCVariable(const int player, const std::string& mapName, const Sint32 persistentID, const std::string& variableID, const Sint32 fallbackValue);
bool persistentStorySetNPCFlag(const int player, const std::string& mapName, const Sint32 persistentID, const std::string& flagID, const bool enabled);
bool persistentStoryGetNPCFlag(const int player, const std::string& mapName, const Sint32 persistentID, const std::string& flagID);
bool persistentStorySetNPCChoiceUsed(const int player, const std::string& mapName, const Sint32 persistentID, const std::string& choiceID, const bool used);
bool persistentStoryNPCChoiceWasUsed(const int player, const std::string& mapName, const Sint32 persistentID, const std::string& choiceID);
bool persistentStorySetNPCNodeSeen(const int player, const std::string& mapName, const Sint32 persistentID, const std::string& nodeID, const bool seen);
bool persistentStoryNPCNodeWasSeen(const int player, const std::string& mapName, const Sint32 persistentID, const std::string& nodeID);


#endif
#define GAME_CODE

class Entity;

#define DEBUG 1
#define ENTITY_PACKET_LENGTH 58
#define NET_PACKET_SIZE 2048

// impulses (bound keystrokes, mousestrokes, and joystick/game controller strokes) //TODO: Player-by-player basis.
extern Uint32 impulses[NUMIMPULSES];
extern Uint32 joyimpulses[NUM_JOY_IMPULSES]; //Joystick/gamepad only impulses.

bool handleEvents(void);
void startMessages();
void resetPersistentWorldSession();
bool automatiaMagicGrimoireHasGenerated();
void automatiaMarkMagicGrimoireGenerated();
bool automatiaMagicGrimoireMerchantIsUnlocked();
bool automatiaMagicGrimoireMerchantWasPurchased();
bool automatiaUnlockMagicGrimoireMerchant();
void automatiaMarkMagicGrimoireMerchantPurchased();
void automatiaEnsureMagicGrimoireMerchantStock(Entity* merchant);
bool automatiaHerxPurpleOrbRewardHasGenerated();
void automatiaMarkHerxPurpleOrbRewardGenerated();

/*
 * Infinite Dungeon is server-authoritative. Cycle zero is the ordinary
 * campaign. Each completed Citadel increments the cycle before the party
 * loads the first generated mine floor again.
 */
Uint32 automatiaInfiniteDungeonGetCycle();
Uint32 automatiaInfiniteDungeonGetCycleSeed();
bool automatiaBeginInfiniteDungeonCycle();
void automatiaSetInfiniteDungeonStateFromServer(
	Uint32 cycle,
	Uint32 cycleSeed
);
std::string automatiaInfiniteDungeonInstanceId(
	const std::string& baseInstanceId
);
Uint32 automatiaMixInfiniteDungeonMapSeed(
	Uint32 seed,
	Sint32 dungeonLevel,
	bool secretTrack
);
bool automatiaApplyInfiniteDungeonMonsterScaling(
	Entity* monsterEntity
);

struct AutomatiaPlayerReturnPlacement
{
	bool valid = false;
	PlayableFloorId playableFloor = DEFAULT_PLAYABLE_FLOOR;
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
	double yaw = 0.0;
	double pitch = 0.0;
	double roll = 0.0;
};

struct AutomatiaPlayerLevelVisit
{
	Sint32 dungeonLevel = 0;
	bool secretLevel = false;
	Uint32 mapSeed = 0;
	std::string mapInstanceKey;
	bool hasReturnAnchor = false;
	Sint32 returnAnchorPersistentID = 0;
	PlayableFloorId returnAnchorPlayableFloor = DEFAULT_PLAYABLE_FLOOR;
	double returnAnchorX = 0.0;
	double returnAnchorY = 0.0;
	double returnAnchorZ = 0.0;
	AutomatiaPlayerReturnPlacement returnPlacement;
};

/*
 * Whole-party transitions record one independent history entry for every
 * player on the active map. Divergent transitions record only the player who
 * actually leaves. A reverse ladder consumes only that player's back stack.
 */
void recordAutomatiaPartyLevelVisit(
	const Entity* departureEntity = nullptr
);
void recordAutomatiaPlayerLevelVisit(
	int playerIndex,
	const Entity* departureEntity = nullptr
);
bool consumeAutomatiaPreviousPlayerLevel(
	int playerIndex,
	AutomatiaPlayerLevelVisit& destination,
	std::string& error
);
void restoreAutomatiaPreviousPlayerLevel(
	int playerIndex,
	const AutomatiaPlayerLevelVisit& destination
);
bool queueAutomatiaReverseTransition(
	int playerIndex,
	const AutomatiaPlayerLevelVisit& destination
);
void prepareAutomatiaReverseReturnSpawn(
	int playerIndex,
	const AutomatiaPlayerLevelVisit& destination
);

/*
 * Export/import only the player-scoped quest and NPC dialogue memory used by
 * dedicated per-character saves. World- and party-scoped story state remains
 * owned by the shared persistent-world document.
 */
bool exportAutomatiaPlayerStoryState(
    int player,
    std::string& payload,
    std::string& error);
bool importAutomatiaPlayerStoryState(
    int player,
    const std::string& payload,
    std::string& error);
void resetAutomatiaPlayerStoryState(int player);
/*
 * Custom dialogue and quest persistence.
 *
 * These functions currently operate on session memory only.
 * The server/host owns all mutations.
 */

/* Global world variables and flags. */
bool persistentStorySetWorldVariable(
    const std::string& variableID,
    Sint32 value
);

Sint32 persistentStoryGetWorldVariable(
    const std::string& variableID,
    Sint32 fallbackValue = 0
);

bool persistentStoryAddWorldVariable(
    const std::string& variableID,
    Sint32 amount
);

bool persistentStorySetWorldFlag(
    const std::string& flagID,
    bool enabled
);

bool persistentStoryGetWorldFlag(
    const std::string& flagID
);

/* Shared quest state. */
/* Player-scoped quest API. */
bool persistentStorySetQuestStage(const int player, const std::string& questID, const Sint32 stage);
Sint32 persistentStoryGetQuestStage(const int player, const std::string& questID, const Sint32 fallbackStage);
bool persistentStorySetQuestStarted(const int player, const std::string& questID, const bool started);
bool persistentStorySetQuestAccepted(const int player, const std::string& questID, const bool accepted);
bool persistentStorySetQuestCompleted(const int player, const std::string& questID, const bool completed);
bool persistentStorySetQuestFailed(const int player, const std::string& questID, const bool failed);
bool persistentStoryQuestIsStarted(const int player, const std::string& questID);
bool persistentStoryQuestIsAccepted(const int player, const std::string& questID);
bool persistentStoryQuestIsCompleted(const int player, const std::string& questID);
bool persistentStoryQuestIsFailed(const int player, const std::string& questID);

bool persistentStorySetQuestVariable(
    const int player,
    const std::string& questID,
    const std::string& variableID,
    const Sint32 value
);
Sint32 persistentStoryGetQuestVariable(
    const int player,
    const std::string& questID,
    const std::string& variableID,
    const Sint32 fallbackValue = 0
);
bool persistentStoryAddQuestVariable(
    const int player,
    const std::string& questID,
    const std::string& variableID,
    const Sint32 amount
);

bool persistentStorySetQuestObjectiveCompleted(
    const int player,
    const std::string& questID,
    const std::string& objectiveID,
    const bool completed
);
bool persistentStoryQuestObjectiveIsCompleted(
    const int player,
    const std::string& questID,
    const std::string& objectiveID
);
bool persistentStoryResetPlayerQuest(
    const int player,
    const std::string& questID
);

bool persistentStorySetQuestStage(
    const std::string& questID,
    Sint32 stage
);

Sint32 persistentStoryGetQuestStage(
    const std::string& questID,
    Sint32 fallbackStage = 0
);

bool persistentStorySetQuestStarted(
    const std::string& questID,
    bool started
);

bool persistentStorySetQuestAccepted(
    const std::string& questID,
    bool accepted
);

bool persistentStorySetQuestCompleted(
    const std::string& questID,
    bool completed
);

bool persistentStorySetQuestFailed(
    const std::string& questID,
    bool failed
);

bool persistentStoryQuestIsStarted(
    const std::string& questID
);

bool persistentStoryQuestIsAccepted(
    const std::string& questID
);

bool persistentStoryQuestIsCompleted(
    const std::string& questID
);

bool persistentStoryQuestIsFailed(
    const std::string& questID
);

bool persistentStorySetQuestVariable(
    const std::string& questID,
    const std::string& variableID,
    Sint32 value
);

Sint32 persistentStoryGetQuestVariable(
    const std::string& questID,
    const std::string& variableID,
    Sint32 fallbackValue = 0
);

bool persistentStorySetQuestFlag(
    const std::string& questID,
    const std::string& flagID,
    bool enabled
);

bool persistentStoryGetQuestFlag(
    const std::string& questID,
    const std::string& flagID
);

bool persistentStorySetQuestObjectiveComplete(
    const std::string& questID,
    const std::string& objectiveID,
    bool completed
);

bool persistentStoryQuestObjectiveIsComplete(
    const std::string& questID,
    const std::string& objectiveID
);

bool persistentStorySetQuestChoiceUsed(
    const std::string& questID,
    const std::string& choiceID,
    bool used
);

bool persistentStoryQuestChoiceWasUsed(
    const std::string& questID,
    const std::string& choiceID
);

/* Memory belonging to one persistent original-map NPC. */
bool persistentStoryAssignNPCDialogue(
    const std::string& mapName,
    Sint32 persistentID,
    const std::string& dialogueID
);

std::string persistentStoryGetNPCDialogue(
    const std::string& mapName,
    Sint32 persistentID
);

bool persistentStorySetNPCNode(
    const std::string& mapName,
    Sint32 persistentID,
    Sint32 nodeID
);

Sint32 persistentStoryGetNPCNode(
    const std::string& mapName,
    Sint32 persistentID,
    Sint32 fallbackNode = 0
);

bool persistentStorySetNPCVariable(
    const std::string& mapName,
    Sint32 persistentID,
    const std::string& variableID,
    Sint32 value
);

Sint32 persistentStoryGetNPCVariable(
    const std::string& mapName,
    Sint32 persistentID,
    const std::string& variableID,
    Sint32 fallbackValue = 0
);

bool persistentStorySetNPCFlag(
    const std::string& mapName,
    Sint32 persistentID,
    const std::string& flagID,
    bool enabled
);

bool persistentStoryGetNPCFlag(
    const std::string& mapName,
    Sint32 persistentID,
    const std::string& flagID
);

bool persistentStorySetNPCChoiceUsed(
    const std::string& mapName,
    Sint32 persistentID,
    const std::string& choiceID,
    bool used
);

bool persistentStoryNPCChoiceWasUsed(
    const std::string& mapName,
    Sint32 persistentID,
    const std::string& choiceID
);

bool persistentStorySetNPCNodeSeen(
    const std::string& mapName,
    Sint32 persistentID,
    const std::string& nodeID,
    bool seen
);

bool persistentStoryNPCNodeWasSeen(
    const std::string& mapName,
    Sint32 persistentID,
    const std::string& nodeID
);

/* Cross-map persistent-state conditions. */
bool persistentStoryMapHasState(
    const std::string& mapName
);

bool persistentStoryOriginalEntityIsRemoved(
    const std::string& mapName,
    Sint32 persistentID
);
void applyPersistentMapRemovals();
void applyPersistentMechanismStates();
/*
 * Multiplayer persistent-world synchronization.
 *
 * The server owns the authoritative registry.
 * Clients only receive and apply snapshots.
 */
void sendPersistentWorldSnapshotToClient(
    int player,
    const std::string& destinationMapName
);

void beginClientPersistentWorldSnapshot(
    const std::string& mapName
);

void receiveClientPersistentRemoval(
    Sint32 persistentID
);

void receiveClientPersistentLeverState(
    Sint32 persistentID,
    bool timedLever,
    Sint32 switchPower,
    Sint32 leverStatus,
    Sint32 leverTimerTicks,
    real_t roll
);

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
);
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
);
void receiveClientPersistentFurnitureState(
    Sint32 persistentID,
    Sint32 furnitureType,
    Sint32 furnitureHealth,
    Sint32 furnitureMaxHealth,
    bool burning,
    bool burnable
);
void receiveClientPersistentColliderState(
    Sint32 persistentID,
    Sint32 colliderCurrentHP,
    Sint32 colliderMaxHP,
    Sint32 colliderDamageTypes,
    Sint32 colliderHasCollision,
    bool burning,
    bool burnable
);
void receiveClientPersistentPowerCrystalState(
    Sint32 persistentID,
    Sint32 crystalInitialised,
    Sint32 crystalDirection,
    Sint32 crystalNumElectricityNodes,
    Sint32 crystalTurnReverse,
    Sint32 crystalSpellToActivate,
    Sint32 crystalPowerToActivate,
    Sint32 crystalCircuitStatus
);
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
);
void receiveClientPersistentBoulderTrapState(
    Sint32 persistentID,
    Sint32 trapBehavior,
    Sint32 fired,
    Sint32 refireAmount,
    Sint32 refireCounter,
    Sint32 preDelay,
    Sint32 circuitStatus,
    Sint32 sabotaged
);
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
);
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
);
void receiveClientPersistentWaterSourceState(
    Sint32 persistentID,
    bool isFountain,
    Sint32 uses,
    Sint32 mainEffect,
    Sint32 secondaryEffect
);
void receiveClientPersistentCampfireState(
    Sint32 persistentID,
    Sint32 health
);
void receiveClientPersistentWallLockState(
    Sint32 persistentID,
    Sint32 lockState,
    Sint32 power,
    Sint32 pickHealth,
    Sint32 preventExploit
);
void receiveClientPersistentWallButtonState(
    Sint32 persistentID,
    Sint32 buttonState,
    Sint32 buttonPower
);

void receiveClientPersistentPressurePlateState(
    Sint32 persistentID,
    bool permanent,
    Sint32 power,
    Sint32 interactionLock
);
void receiveClientPersistentTileState(
    Sint32 x,
    Sint32 y,
    Sint32 layer,
    Sint32 tile
);
void receiveClientPersistentTileState(
    Sint32 x,
    Sint32 y,
    Sint32 layer,
    Sint32 tile,
    PlayableFloorId playableFloor
);
void receiveClientPersistentPedestalState(
    Sint32 persistentID,
    Sint32 hasOrb,
    Sint32 powerStatus,
    Sint32 initialized,
    Sint32 inGround,
    real_t z,
    real_t velZ,
    bool passable
);
void receiveClientPersistentChestState(
    Sint32 persistentID,
    Sint32 health,
    Sint32 maxHealth,
    Sint32 locked,
    Sint32 lockpickHealth,
    Sint32 preventExploit,
    Sint32 oldHealth,
    Sint32 voidState
);
/*
 * Returns true when the persistent-world registry already owns the
 * Hermit's unique duck outside the player's inventory. This suppresses
 * the vanilla per-level fallback spawn so persistence cannot duplicate it.
 */
bool automatiaPersistentHermitDuckExists(
    int playerIndex,
    int duckColor
);

/*
 * Restores surviving original monster/NPC HP, MP and world position
 * after species initialization. Living non-mechanical creatures heal
 * five HP per revisit.
 */
bool applyPersistentMonsterLivingState(
    Entity* monsterEntity
);
bool applyPersistentShopkeeperInventory(
    Entity* shopkeeperEntity
);
void finishClientPersistentWorldSnapshot();
bool writeAutomatiaPersistentWorldSave(
    const char* path,
    const std::string& sessionId,
    const std::string& saveTransactionId,
    std::string& error
);
bool serializeAutomatiaPersistentWorldSnapshot(
    const std::string& sessionId,
    int playerIndex,
    std::string& snapshot,
    std::string& error
);
bool stageAutomatiaPersistentWorldSnapshot(
    const std::string& snapshot,
    const std::string& sessionId,
    std::string& error
);
void discardAutomatiaPersistentWorldSnapshot();
bool exportAutomatiaPersistentMinimapSnapshot(
    int playerIndex,
    std::string& mapKey,
    Sint32& width,
    Sint32& height,
    std::vector<Sint8>& tiles
);
bool importAutomatiaPersistentMinimapSnapshot(
    int playerIndex,
    const std::string& mapKey,
    Sint32 width,
    Sint32 height,
    const std::vector<Sint8>& tiles,
    bool allowResize
);
bool exportAutomatiaPersistentMinimapSnapshotForFloor(
    int playerIndex,
    const std::string& mapKey,
    PlayableFloorId playableFloor,
    Sint32& width,
    Sint32& height,
    std::vector<Sint8>& tiles
);
bool importAutomatiaPersistentMinimapSnapshotForFloor(
    int playerIndex,
    const std::string& mapKey,
    PlayableFloorId playableFloor,
    Sint32 width,
    Sint32 height,
    const std::vector<Sint8>& tiles,
    bool allowResize
);
void syncClientPersistentMinimap(bool force);
void resetClientPersistentMinimapSync();
void restoreAutomatiaPersistentMinimapForLocalPlayer();

// Stage Z3 same-map playable-floor transition transaction. The endpoint is an
// authored entity carrying PZLV ZTRN metadata. This moves only the selected
// player; follower/pathfinding traversal remains Stage Z4.
bool transitionAutomatiaPlayerThroughPlayableFloorEndpoint(
    int playerIndex,
    Entity* sourceEndpoint
);

// Applies an authoritative floor placement locally. Used by the Z3 PZTR
// server packet and by reconnect/save restoration once nonzero floors are legal.
bool applyAutomatiaPlayableFloorPlacement(
    int playerIndex,
    PlayableFloorId playableFloor,
    std::uint64_t spatialRevision,
    real_t x,
    real_t y,
    real_t z,
    real_t yaw,
    real_t pitch,
    real_t roll,
    bool requirePassable = true
);

// Drops a non-levitating player through a missing authored floor to the nearest
// lower valid playable floor at the same X/Y. Returns the number of floors
// crossed; damage policy remains in actPlayer.
bool fallAutomatiaPlayerToLowerPlayableFloor(
    int playerIndex,
    int& floorsFallen
);

bool loadAutomatiaPersistentWorldSave(
    const char* path,
    const std::string& sessionId,
    const std::string& saveTransactionId,
    std::string& error
);
void applyAutomatiaSavedPlayerPlacements();
bool automatiaHasSavedPlayerPlacement(int playerIndex);
void consumeAutomatiaSavedPlayerPlacement(int playerIndex);
bool stageAutomatiaCharacterSavedPlacement(
    int playerIndex,
    const std::string& mapFile,
    const std::string& instanceId,
    Uint64 revision,
    PlayableFloorId playableFloor,
    real_t x,
    real_t y,
    real_t z,
    real_t yaw,
    real_t pitch,
    real_t roll);
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
    real_t roll);
bool prepareAutomatiaSavedPlayerSpawnMask(bool playerSpawnMask[MAXPLAYERS]);
bool restoreAutomatiaSavedPlayerInstances();
bool prepareAutomatiaLateJoinPlayer(
    int playerIndex,
    bool returningPlayer,
    std::string& error
);
// net packet send
typedef struct packetsend_t
{
	UDPsocket sock;
	int channel;
	UDPpacket* packet;
	int num;
	int tries;
	int hostnum;
	bool mapScoped;
	Uint64 mapInstanceRevision;
	char mapInstanceKey[256];
} packetsend_t;
extern list_t safePacketsSent;
extern std::unordered_map<int, Uint32> safePacketsReceivedMap[MAXPLAYERS];
extern bool receivedclientnum;

extern Sint32 numplayers;
extern Sint32 clientnum;
extern bool intro;
extern int introstage;
extern bool gamePaused;
extern bool fadeout, fadefinished;
extern int fadealpha;
extern Entity* client_selected[MAXPLAYERS];
extern bool inrange[MAXPLAYERS];
extern bool deleteallbuttons;
extern Sint32 client_classes[MAXPLAYERS];
extern Uint32 client_keepalive[MAXPLAYERS];
extern Uint16 portnumber;
extern list_t messages;
extern list_t command_history;
extern node_t* chosen_command;
extern bool command;
extern bool noclip, godmode, buddhamode;
extern bool everybodyfriendly;
extern bool combat, combattoggle;
extern bool assailant[MAXPLAYERS];
extern bool oassailant[MAXPLAYERS];
extern int assailantTimer[MAXPLAYERS];
static const int COMBAT_MUSIC_COOLDOWN = 200; // 200 ticks of combat music before it fades away.
extern list_t removedEntities;
extern char maptoload[256], configtoload[256];
extern bool loadingmap, loadingconfig;
extern int startfloor;
extern bool skipintro;
extern Uint32 uniqueGameKey;
extern Uint32 uniqueLobbyKey;
extern bool arachnophobia_filter;
extern bool colorblind_lobby;

// definitions
extern bool showfps;
extern real_t time_diff;
extern real_t t, ot, frameval[AVERAGEFRAMES];
extern Uint32 cycles, pingtime;
extern real_t fps;
static const int NUMCLASSES = 26;
#define NUMRACES 18
#define NUMPLAYABLERACES 14
extern char address[64];
extern bool loadnextlevel;
extern int skipLevelsOnLoad;
extern bool loadingSameLevelAsCurrent;
extern std::string loadCustomNextMap;
extern Sint32 loadCustomNextTunnelID;
bool queueAutomatiaCustomTransition(
	int playerIndex,
	const std::string& destinationMap,
	Sint32 destinationTunnelID,
	int destinationLevel,
	bool destinationSecret
);
extern Uint32 forceMapSeed;
extern int currentlevel;
extern bool secretlevel;
extern bool darkmap;
extern int shaking, bobbing;

enum MessageType : Uint32 {
	MESSAGE_COMBAT = 1u << 0, // damage received or given in combat
	MESSAGE_STATUS = 1u << 1, // character status changes and passive effects
	MESSAGE_INVENTORY = 1u << 2, // inventory and item appraisal
	MESSAGE_EQUIPMENT = 1u << 3, // player equipment changes
	MESSAGE_WORLD = 1u << 4, // diegetic messages, such as speech and text
	MESSAGE_CHAT = 1u << 5, // multiplayer chat
	MESSAGE_PROGRESSION = 1u << 6, // player character progression messages (ie level-ups)
	MESSAGE_INTERACTION = 1u << 7, // player interactions with the world
	MESSAGE_INSPECTION = 1u << 8, // player inspections of world objects
	MESSAGE_HINT = 1u << 9, // special text cues and descriptive messages
	MESSAGE_OBITUARY = 1u << 10, // character death announcement
	MESSAGE_CHATTER = 1u << 11, // NPC chatter
	MESSAGE_SPAM_MISC = 1u << 28, // misc spammy messages "dropped item" "it burns!" 
	MESSAGE_COMBAT_BASIC = 1u << 29, // basic combat 'the skeleton hits!' 'you hit the skeleton!'
	MESSAGE_DEBUG = 1u << 30, // debug only messages
	MESSAGE_MISC = 1u << 31, // miscellaneous messages
};
extern Uint32 messagesEnabled;

enum PlayerClasses : int
{
	CLASS_BARBARIAN,
	CLASS_WARRIOR,
	CLASS_HEALER,
	CLASS_ROGUE,
	CLASS_WANDERER,
	CLASS_CLERIC,
	CLASS_MERCHANT,
	CLASS_WIZARD,
	CLASS_ARCANIST,
	CLASS_JOKER,
	CLASS_SEXTON,
	CLASS_NINJA,
	CLASS_MONK,
	CLASS_CONJURER,
	CLASS_ACCURSED,
	CLASS_MESMER,
	CLASS_BREWER,
	CLASS_MACHINIST,
	CLASS_PUNISHER,
	CLASS_SHAMAN,
	CLASS_HUNTER,
	CLASS_BARD,
	CLASS_SAPPER,
	CLASS_SCION,
	CLASS_HERMIT,
	CLASS_PALADIN
};

static const std::vector<std::string> playerClassInternalNames = {
	"class_barbarian",
	"class_warrior",
	"class_healer",
	"class_rogue",
	"class_wanderer",
	"class_cleric",
	"class_merchant",
	"class_wizard",
	"class_arcanist",
	"class_joker",
	"class_sexton",
	"class_ninja",
	"class_monk",
	"class_conjurer",
	"class_accursed",
	"class_mesmer",
	"class_brewer",
	"class_machinist",
	"class_punisher",
	"class_shaman",
	"class_hunter",
	"class_bard",
	"class_sapper",
	"class_scion",
	"class_hermit",
	"class_paladin"
};

static const int CLASS_SHAMAN_NUM_STARTING_SPELLS = 15;

enum PlayerRaces : int
{
	RACE_HUMAN,
	RACE_SKELETON,
	RACE_VAMPIRE,
	RACE_SUCCUBUS,
	RACE_GOATMAN,
	RACE_AUTOMATON,
	RACE_INCUBUS,
	RACE_GOBLIN,
	RACE_INSECTOID,
	RACE_RAT,
	RACE_TROLL,
	RACE_SPIDER,
	RACE_IMP,
	RACE_GNOME,
	RACE_GREMLIN,
	RACE_DRYAD,
	RACE_MYCONID,
	RACE_SALAMANDER,
	RACE_ENUM_END
};

bool achievementUnlocked(const char* achName);
void steamAchievement(const char* achName);
void steamUnsetAchievement(const char* achName);
void steamAchievementClient(int player, const char* achName);
void steamAchievementEntity(Entity* my, const char* achName); // give steam achievement to an entity, and check for valid player info.
void steamStatisticUpdate(int statisticNum, ESteamStatTypes type, int value);
void steamStatisticUpdateClient(int player, int statisticNum, ESteamStatTypes type, int value);
void steamIndicateStatisticProgress(int statisticNum, ESteamStatTypes type);
void pauseGame(int mode, int ignoreplayer);
int initGame();
void initGameDatafiles(bool moddedReload);
void initGameDatafilesAsync(bool moddedReload);
void deinitGame();
void handleButtons(void);
void gameLogic(void);

// behavior function prototypes:
void actAnimator(Entity* my);
void actRotate(Entity* my);
void actLiquid(Entity* my);
void actEmpty(Entity* my);
void actFurniture(Entity* my);
void actMCaxe(Entity* my);
void actStatueAnimator(Entity* my);
void actStatue(Entity* my);
void actDoorFrame(Entity* my);
void actDeathCam(Entity* my);
void actProjectSpiritCam(Entity* my);
void actDeathGhost(Entity* my);
void actDeathGhostLimb(Entity* my);
void actPlayerLimb(Entity* my);
void actTorch(Entity* my);
void actCrystalShard(Entity* my);
void actDoor(Entity* my);
void actHudWeapon(Entity* my);
void actHudArm(Entity* my);
void actHudShield(Entity* my);
void actHudAdditional(Entity* my);
void actHudArrowModel(Entity* my);
void actHudAdditional2(Entity* my);
void actItem(Entity* my);
void actGoldBag(Entity* my);
void actGib(Entity* my);
void actGreasePuddleSpawner(Entity* my);
void actGreasePuddle(Entity* my);
void actMiscPuddle(Entity* my);
void spawnGreasePuddleSpawner(Entity* caster, real_t x, real_t y, int duration);
void actDamageGib(Entity* my);
void actFociGib(Entity* my);
Entity* spawnFociGib(real_t x, real_t y, real_t z, real_t dir, real_t velocityBonus, Uint32 parentUid, int sprite, Uint32 seed);
Entity* spawnGib(Entity* parentent, int customGibSprite = -1);
Entity* spawnDamageGib(Entity* parentent, Sint32 dmgAmount, int gibDmgType, int displayType = 0, bool updateClients = false);
Entity* spawnGibClient(Sint16 x, Sint16 y, Sint16 z, Sint16 sprite);
Entity* spawnMiscPuddle(Entity* parentent, real_t x, real_t y, int sprite, bool updateClients = false);
void serverSpawnGibForClient(Entity* gib);
void actLadder(Entity* my);
void actLadderReverse(Entity* my);
void actLadderUp(Entity* my);
void actPlayableFloorTransition(Entity* my);
void actPortal(Entity* my);
void actWinningPortal(Entity* my);
void actFlame(Entity* my);
void actCampfire(Entity* my);
void actCauldron(Entity* my);
void actWorkbench(Entity* my);
void actMailbox(Entity* my);
Entity* spawnFlame(Entity* parentent, Sint32 sprite);
Entity* spawnFlameSprites(Entity* parentent, Sint32 sprite);
Entity* castMagic(Entity* parentent);
void actSprite(Entity* my);
void actSpriteNametag(Entity* my);
void actSpriteWorldTooltip(Entity* my);
void actSleepZ(Entity* my);
Entity* spawnBang(Sint16 x, Sint16 y, Sint16 z);
Entity* spawnExplosion(Sint16 x, Sint16 y, Sint16 z);
Entity* spawnExplosionFromSprite(Uint16 sprite, Sint16 x, Sint16 y, Sint16 z);
Entity* spawnPoof(Sint16 x, Sint16 y, Sint16 z, real_t scale, bool updateClients = false);
Entity* spawnSleepZ(Sint16 x, Sint16 y, Sint16 z);
Entity* spawnFloatingSpriteMisc(int sprite, Sint16 x, Sint16 y, Sint16 z);
void actArrow(Entity* my);
void actBoulder(Entity* my);
void actBoulderTrap(Entity* my);
void actBoulderTrapHole(Entity* my);
void actBoulderTrapEast(Entity* my);
void actBoulderTrapWest(Entity* my);
void actBoulderTrapSouth(Entity* my);
void actBoulderTrapNorth(Entity* my);
void actHeadstone(Entity* my);
void actThrown(Entity* my);
void actBeartrap(Entity* my);
void actBeartrapLaunched(Entity* my);
void actBomb(Entity* my);
void actDecoyBox(Entity* my);
void actDecoyBoxCrank(Entity* my);
void actSpearTrap(Entity* my);
void actWallBuster(Entity* my);
void actWallBuilder(Entity* my);
void actPowerCrystalBase(Entity* my);
void actPowerCrystal(Entity* my);
void actPowerCrystalParticleIdle(Entity* my);
void actPedestalBase(Entity* my);
void actPedestalOrb(Entity* my);
void actMidGamePortal(Entity* my);
void actCustomPortal(Entity* my);
void actTeleporter(Entity* my);
void actMagicTrapCeiling(Entity* my);
void actTeleportShrine(Entity* my);
void actDaedalusShrine(Entity* my);
void actAssistShrine(Entity* my);
void actBell(Entity* my);
void bellBreakBulb(Entity* my, bool minotaurBreak);
void actSpellShrine(Entity* my);
void actExpansionEndGamePortal(Entity* my);
void actSoundSource(Entity* my);
void actLightSource(Entity* my);
void actSignalTimer(Entity* my);
void actSignalGateAND(Entity* my);
void actWallLock(Entity* my);
void actWallButton(Entity* my);
void actWind(Entity* my);
void createWaterSplash(real_t x, real_t y, int lifetime);

void startMessages();
bool frameRateLimit(Uint32 maxFrameRate, bool resetAccumulator = true, bool sleep = false);
extern Uint32 networkTickrate;
extern bool gameloopFreezeEntities;
extern Uint32 serverSchedulePlayerHealthUpdate;

void drawAllPlayerCameras();

#define TOUCHRANGE 32
#define STRIKERANGE 24
#define XPSHARERANGE 99999

// function prototypes for charclass.c:
void initClass(int player);
void initClassStats(const int classnum, void* myStats);
void initShapeshiftHotbar(int player);
void deinitShapeshiftHotbar(int player);
bool playerUnlockedShamanSpell(int player, Item* item);

extern char last_ip[64];
extern char last_port[64];

//TODO: Maybe increase with level or something?
//TODO: Pause health regen during combat?
#define HEAL_TIME 600 //12 seconds. //Original time: 3600 (1 minute)
#define MAGIC_REGEN_TIME 600 // 12 seconds
#define MAGIC_REGEN_AUTOMATON_TIME 300

#define DEFAULT_HP 30
#define DEFAULT_MP 30
#define HP_MOD 5
#define MP_MOD 5

#define SPRITE_FLAME 13
#define SPRITE_CRYSTALFLAME 96

#define MAXCHARGE 30 // charging up weapons

static const int BASE_MELEE_DAMAGE = 8;
static const int BASE_RANGED_DAMAGE = 7;
static const int BASE_THROWN_DAMAGE = 6;
static const int BASE_PLAYER_UNARMED_DAMAGE = 8;

extern bool spawn_blood;
extern bool capture_mouse; //Useful for debugging when the game refuses to release the mouse when it's crashed.

#define LEVELSFILE "maps/levels.txt"
#define SECRETLEVELSFILE "maps/secretlevels.txt"
#define LENGTH_OF_LEVEL_REGION 5

#define TICKS_PER_SECOND 50
static const Uint8 TICKS_TO_PROCESS_FIRE = 30; // The amount of ticks needed until the 'BURNING' Status Effect is processed (char_fire % TICKS_TO_PROCESS_FIRE == 0)
static const int EFFECT_WITHDRAWAL_BASE_TIME = TICKS_PER_SECOND * 60 * 8; // 8 minutes base withdrawal time.

static const std::string PLAYERNAMES_MALE_FILE = "playernames-male.txt";
static const std::string PLAYERNAMES_FEMALE_FILE = "playernames-female.txt";
static const std::string NPCNAMES_MALE_FILE = "npcnames-male.txt";
static const std::string NPCNAMES_FEMALE_FILE = "npcnames-female.txt";
extern std::vector<std::string> randomPlayerNamesMale;
extern std::vector<std::string> randomPlayerNamesFemale;
extern std::vector<std::string> randomNPCNamesMale;
extern std::vector<std::string> randomNPCNamesFemale;
extern bool enabledDLCPack1;
extern bool enabledDLCPack2;
extern bool enabledDLCPack3;
extern std::vector<std::string> physFSFilesInDirectory;
void loadRandomNames();
int mapLevel(int player, int radius, int _x, int _y, bool usingSpell);
void mapLevel2(int player);
void mapFoodOnLevel(int player);
bool mapTileDiggable(const int x, const int y);
bool mapTileDiggable(const int x, const int y, PlayableFloorId playableFloor);

class TileEntityListHandler
{
private:
	static const int kMaxMapDimension = 256;
	struct AdditionalFloorGrid
	{
		// Nonzero floors use sparse tile buckets so an empty 256x256 grid is not
		// allocated for every possible playable floor. list_t addresses remain
		// stable because each occupied bucket owns its list separately.
		std::unordered_map<int, std::unique_ptr<list_t>> tiles;

		~AdditionalFloorGrid();

		AdditionalFloorGrid() = default;
		AdditionalFloorGrid(const AdditionalFloorGrid&) = delete;
		AdditionalFloorGrid& operator=(const AdditionalFloorGrid&) = delete;
	};

	std::unordered_map<PlayableFloorId, std::unique_ptr<AdditionalFloorGrid>> additionalFloorGrids;
	list_t* getTileListForFloor(int x, int y, PlayableFloorId playableFloor, bool createFloor);
public:
	// Legacy public grid remains the Z0 compatibility grid. Nonzero playable
	// floors are stored in additionalFloorGrids and must be accessed through
	// the floor-aware helpers below.
	list_t gridEntities[kMaxMapDimension][kMaxMapDimension];

	void clearTile(int x, int y);
	void clearTile(int x, int y, PlayableFloorId playableFloor);
	void emptyGridEntities();
	list_t* getTileList(int x, int y);
	list_t* getTileList(int x, int y, PlayableFloorId playableFloor);
	node_t* addEntity(Entity& entity);
	node_t* updateEntity(Entity& entity);
	std::vector<list_t*> getEntitiesWithinRadius(int u, int v, int radius);
	std::vector<list_t*> getEntitiesWithinRadius(int u, int v, int radius, PlayableFloorId playableFloor);
	std::vector<list_t*> getEntitiesWithinRadiusAroundEntity(Entity* entity, int radius);

	TileEntityListHandler()
	{
		for ( int i = 0; i < kMaxMapDimension; ++i )
		{
			for ( int j = 0; j < kMaxMapDimension; ++j )
			{
				gridEntities[i][j].first = nullptr;
				gridEntities[i][j].last = nullptr;
			}
		}
	};

	~TileEntityListHandler()
	{
		emptyGridEntities();
	};
};
extern TileEntityListHandler TileEntityList;

class DebugStatsClass
{
public:
	std::chrono::high_resolution_clock::time_point t1StartLoop;
	std::chrono::high_resolution_clock::time_point t2PostEvents;
	std::chrono::high_resolution_clock::time_point t21PostHandleMessages;
	std::chrono::high_resolution_clock::time_point t3SteamCallbacks;
	std::chrono::high_resolution_clock::time_point t4Music;
	std::chrono::high_resolution_clock::time_point t5MainDraw;
	std::chrono::high_resolution_clock::time_point t6Messages;
	std::chrono::high_resolution_clock::time_point t7Inputs;
	std::chrono::high_resolution_clock::time_point t8Status;
	std::chrono::high_resolution_clock::time_point t9GUI;
	std::chrono::high_resolution_clock::time_point t10FrameLimiter;
	std::chrono::high_resolution_clock::time_point t11End;

	std::chrono::high_resolution_clock::time_point gui1;
	std::chrono::high_resolution_clock::time_point gui2;
	std::chrono::high_resolution_clock::time_point gui3;
	std::chrono::high_resolution_clock::time_point gui4;
	std::chrono::high_resolution_clock::time_point gui5;
	std::chrono::high_resolution_clock::time_point gui6;
	std::chrono::high_resolution_clock::time_point gui7;
	std::chrono::high_resolution_clock::time_point gui8;
	std::chrono::high_resolution_clock::time_point gui9;
	std::chrono::high_resolution_clock::time_point gui10;
	std::chrono::high_resolution_clock::time_point gui11;
	std::chrono::high_resolution_clock::time_point gui12;

	std::chrono::high_resolution_clock::time_point eventsT1;
	std::chrono::high_resolution_clock::time_point eventsT2;
	std::chrono::high_resolution_clock::time_point eventsT3;
	std::chrono::high_resolution_clock::time_point eventsT4;
	std::chrono::high_resolution_clock::time_point eventsT5;
	std::chrono::high_resolution_clock::time_point eventsT6;

	std::chrono::high_resolution_clock::time_point drawWorldT1;
	std::chrono::high_resolution_clock::time_point drawWorldT2;
	std::chrono::high_resolution_clock::time_point drawWorldT3;
	std::chrono::high_resolution_clock::time_point drawWorldT4;
	std::chrono::high_resolution_clock::time_point drawWorldT5;
	std::chrono::high_resolution_clock::time_point drawWorldT6;

	std::chrono::high_resolution_clock::time_point messagesT1;

	std::chrono::high_resolution_clock::time_point t1Stored;
	std::chrono::high_resolution_clock::time_point t2Stored;
	std::chrono::high_resolution_clock::time_point t21Stored;
	std::chrono::high_resolution_clock::time_point t3Stored;
	std::chrono::high_resolution_clock::time_point t4Stored;
	std::chrono::high_resolution_clock::time_point t5Stored;
	std::chrono::high_resolution_clock::time_point t6Stored;
	std::chrono::high_resolution_clock::time_point t7Stored;
	std::chrono::high_resolution_clock::time_point t8Stored;
	std::chrono::high_resolution_clock::time_point t9Stored;
	std::chrono::high_resolution_clock::time_point t10Stored;
	std::chrono::high_resolution_clock::time_point t11Stored;

	std::chrono::high_resolution_clock::time_point eventsT1stored;
	std::chrono::high_resolution_clock::time_point eventsT2stored;
	std::chrono::high_resolution_clock::time_point eventsT3stored;
	std::chrono::high_resolution_clock::time_point eventsT4stored;
	std::chrono::high_resolution_clock::time_point eventsT5stored;
	std::chrono::high_resolution_clock::time_point eventsT6stored;

	std::chrono::high_resolution_clock::time_point messagesT1stored;

	std::chrono::high_resolution_clock::time_point messagesT2WhileLoop;
	bool handlePacketStartLoop = false;

	std::unordered_map<unsigned long, std::pair<std::string, int>> networkPackets;
	std::unordered_map<int, int> entityUpdatePackets;

	bool displayStats = false;
	char debugOutput[1024];
	char debugEventOutput[1024];

	DebugStatsClass()
	{};

	void inline storeOldTimePoints()
	{
		t1Stored = t1StartLoop;
		t2Stored = t2PostEvents;
		t21Stored = t21PostHandleMessages;
		t3Stored = t3SteamCallbacks;
		t4Stored = t4Music;
		t5Stored = t5MainDraw;
		t6Stored = t6Messages;
		t7Stored = t7Inputs;
		t8Stored = t8Status;
		t9Stored = t9GUI;
		t10Stored = t10FrameLimiter;
		t11Stored = t11End;
		eventsT1stored = eventsT1;
		eventsT2stored = eventsT2;
		eventsT3stored = eventsT3;
		eventsT4stored = eventsT4;
		eventsT5stored = eventsT5;
		eventsT6stored = eventsT6;

		messagesT1stored = messagesT1;
	};

	void storeStats();

	void storeEventStats();
};

extern ConsoleVariable<bool> cvar_enableKeepAlives;
extern ConsoleVariable<bool> cvar_map_sequence_rng;

extern DebugStatsClass DebugStats;
//extern ConsoleVariable<bool> cvar_useTimerInterpolation;

#include "draw.hpp"

class TimerExperiments
{
public:
    //static constexpr bool& bUseTimerInterpolation = *cvar_useTimerInterpolation;
    static bool bUseTimerInterpolation;
	static bool bIsInit;
	static real_t lerpFactor;
	static int timeDivision;
	static bool bDebug;
	struct Clock
	{
		using duration = std::chrono::milliseconds;
		using rep = duration::rep;
		using period = duration::period;
		using time_point = std::chrono::time_point<Clock>;
		static constexpr bool is_steady = true;

		static time_point now() noexcept
		{
			return time_point{ duration{ SDL_GetTicks() } };
		}
	};

	struct State
	{
		double acceleration;
		double velocity;
		double position;
		void resetMovement();
		void resetPosition();
		void normalize(real_t min, real_t max);
	};

	struct EntityStates
	{
		State x;
		State y;
		State z;
		State yaw;
		State pitch;
		State roll;
		void resetMovement();
		void resetPosition();
	};

	friend EntityStates operator+(EntityStates lhs, EntityStates rhs)
	{
		return{ lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z, 
			lhs.yaw + rhs.yaw, lhs.pitch + rhs.pitch, lhs.roll + rhs.roll };
	}
	friend EntityStates operator*(EntityStates lhs, double rhs)
	{
		return{ lhs.x * rhs, lhs.y * rhs, lhs.z * rhs,
			lhs.yaw * rhs, lhs.pitch * rhs, lhs.roll * rhs };
	}
	friend State operator+(State x, State y)
	{
		return{ x.acceleration + y.acceleration, x.velocity + y.velocity, x.position + y.position };
	}
	friend State operator*(State x, double y)
	{
		return{ x.acceleration * y, x.velocity * y, x.position * y };
	}

	static void
		integrate(State& state,
			std::chrono::time_point<Clock, std::chrono::duration<double>>,
			std::chrono::duration<double> dt);

	static std::chrono::duration<long long, std::ratio<1, 60>> dt;
	using duration = decltype(Clock::duration{} +dt);
	using time_point = std::chrono::time_point<Clock, duration>;

	static time_point timepoint;
	static time_point currentTime;
	static duration accumulator;

	static EntityStates cameraPreviousState[MAXPLAYERS];
	static EntityStates cameraCurrentState[MAXPLAYERS];
	static EntityStates cameraRenderState[MAXPLAYERS];

	static std::string render(State state);

	static void reset();
	static void updateClocks();
	static real_t lerpAngle(real_t angle1, real_t angle2, real_t alpha);
	static void renderCameras(view_t& camera, int player);
	static void postRenderRestore(view_t& camera, int player);
	static void updateEntityInterpolationPosition(Entity* entity);
};

void loadAchievementData(const char* path);
void sortAchievementsForDisplay();

real_t getFPSScale(real_t baseFPS);
