/*-------------------------------------------------------------------------------

	BARONY
	File: net.hpp
	Desc: prototypes and definitions for net.cpp

	Copyright 2013-2016 (c) Turning Wheel LLC, all rights reserved.
	See LICENSE for details.

-------------------------------------------------------------------------------*/

#pragma once

#include "late_join_state.hpp"
#include "party_chat.hpp"
#include "party_protocol.hpp"
#include "playable_z.hpp"

#include "game.hpp"
#include <cstddef>
#include <queue>

#define DEFAULT_PORT 57165
#define LOBBY_CHATBOX_LENGTH 62
#define PACKET_LIMIT 200
#define TIMEOUT_TIME 60
#define TIMEOUT_WARNING_TIME 5

extern char lobbyChatbox[LOBBY_CHATBOX_LENGTH];
extern list_t lobbyChatboxMessages;

// function prototypes for net.c:
int power(int a, int b);
int sendPacket(UDPsocket sock, int channel, UDPpacket* packet, int hostnum, bool tryReliable = false);
int sendPacketSafe(UDPsocket sock, int channel, UDPpacket* packet, int hostnum);
int resendPacketSafe(packetsend_t* packet);
bool messagePlayer(int player, Uint32 type, char const * const message, ...);
bool messageLocalPlayers(Uint32 type, char const * const message, ...);
bool messagePlayerColor(int player, Uint32 type, Uint32 color, char const * const message, ...);
bool messageLocalPlayersColor(Uint32 color, Uint32 type, char const * const message, ...);
void sendEntityUDP(Entity* entity, int c, bool guarantee);
void sendEntityTCP(Entity* entity, int c);
void sendMapSeedTCP(int c);
void sendMapTCP(int c);
void serverUpdateEntitySprite(Entity* entity);
void serverUpdateEntitySkill(Entity* entity, int skill);
void serverUpdateEntityFSkill(Entity* entity, int fskill);
void serverUpdateEntityStatFlag(Entity* entity, int flag);
void serverSpawnMiscParticles(Entity* entity, int particleType, int particleSprite, Uint32 optionalUid = 0, Uint32 duration = 0, Uint32 optionalData = 0);
void serverSpawnMiscParticlesAtLocation(Sint16 x, Sint16 y, Sint16 z, int particleType, int particleSprite, Uint32 duration = 0, Uint32 optionalData = 0, Uint32 optionalUid = 0);
void serverSpawnMiscParticlesAtLocationWithSpatialContext(Sint16 x, Sint16 y, Sint16 z, int particleType, int particleSprite, const SpatialSpawnContext& spatialContext, Uint32 duration = 0, Uint32 optionalData = 0, Uint32 optionalUid = 0);
void serverUpdateEntityFlag(Entity* entity, int flag);
void serverUpdateMapTileFlag(Sint16 x, Sint16 y, int layer, Uint32 flagSet, Uint32 flagRemove);
void serverUpdateBodypartIDs(Entity* entity);
void serverUpdateEntityBodypart(Entity* entity, int bodypart);
void serverUpdateEffects(int player);
void serverUpdateHunger(int player);
void serverUpdateSexChange(int player);
void serverUpdatePlayerStats();
void serverUpdatePlayerGameplayStats(int player, int gameplayStat, int changeval);
void serverUpdatePlayerConduct(int player, int conduct, int value);
void serverUpdatePlayerLVL();
void serverRemoveClientFollower(int player, Uint32 uidToRemove);
void serverSendItemToPickupAndEquip(int player, Item* item);
void serverUpdateAllyStat(int player, Uint32 uidToUpdate, int LVL, int HP, int MAXHP, int type);
void serverUpdatePlayerSummonStrength(int player);
void serverUpdateAllyHP(int player, Uint32 uidToUpdate, int HP, int MAXHP, bool guarantee = false);
void sendMinimapPing(Uint8 player, Uint8 x, Uint8 y, Uint8 pingType = 0, bool radius = false);
void sendAllyCommandClient(int player, Uint32 uid, int command, Uint8 x, Uint8 y, Uint32 targetUid = 0);
enum NetworkingLobbyJoinRequestResult : int
{
	NET_LOBBY_JOIN_P2P_FAILURE,
	NET_LOBBY_JOIN_P2P_SUCCESS,
	NET_LOBBY_JOIN_DIRECTIP_FAILURE,
	NET_LOBBY_JOIN_DIRECTIP_SUCCESS
};
NetworkingLobbyJoinRequestResult lobbyPlayerJoinRequest(int& outResult, bool lockedSlots[MAXPLAYERS], bool& outUseChunkedHelo);
Entity* receiveEntity(Entity* entity);
void clientActions(Entity* entity);
void clientHandleMessages(Uint32 framerateBreakInterval);
void clientHandlePacket();
void serverHandleMessages(Uint32 framerateBreakInterval);
bool handleSafePacket();
bool applyPendingTunnelSpawn();
void pollNetworkForShutdown();
void closeNetworkInterfaces();
bool serverSyncAutomatiaPlayerStoryState(int player, const char* reason);
void clientBeginLateJoinPacketDeferral(Uint32 transferId, Uint64 revision);
bool clientAcceptLateJoinCatchupBegin(const Uint8* data, std::size_t size);
bool clientAcceptLateJoinCatchupChunk(const Uint8* data, std::size_t size);
bool clientAcceptLateJoinCatchupComplete(const Uint8* data, std::size_t size);
bool clientDeferLateJoinMapPacket(const Uint8* data, std::size_t size);
void clientLateJoinMapLoaded();
void clientResetLateJoinPacketDeferral();
void clientCheckLateJoinTimeout();
void clientNoteLateJoinProgress();
bool clientLateJoinPacketDeferralActive();

// Dedicated-server character persistence and roster cleanup.
bool clientSendAutomatiaCharacterSaveNow(const char* reason = nullptr);
/*
 * Sends a final character snapshot and briefly waits for the server to
 * confirm that it reached durable storage. This is used by the in-game
 * Return/Open Main Menu path immediately before DISC is sent.
 */
bool clientSendAutomatiaCharacterSaveBeforeDisconnect(
    Uint32 timeoutMilliseconds = 1500);
bool clientReapplyAutomatiaCharacterRestoreAfterPlayerInit();
/*
 * Runtime STRT v5 carries the server-authoritative spawn position, playable
 * floor/spatial revision, exact polymorph/shapeshift targets, and a visible-player
 * mask. The mask separates
 * live actors from the dedicated endpoint and reconnect-reserved identities.
 */
bool clientStageAutomatiaLateJoinVisiblePlayerMask(
    const Uint8* data,
    std::size_t size);
bool clientAutomatiaPlayerSlotShouldHaveActor(int playerIndex);
bool clientStageAutomatiaLateJoinPosition(
    const Uint8* data,
    std::size_t size);
bool clientApplyAutomatiaLateJoinPositionAfterPlayerInit();
bool clientStageAutomatiaLateJoinTransformation(
    const Uint8* data,
    std::size_t size);
bool clientApplyAutomatiaLateJoinTransformationAfterPlayerInit();
void serverRequestAutomatiaCharacterSave(int player, const char* reason = nullptr);
void serverRequestAllAutomatiaCharacterSaves(const char* reason = nullptr);
void disconnectAutomatiaRemotePlayer(
    int player,
    const char* reason,
    bool notifyOtherClients = true);

/*
 * Party transport is session-global and recipient-specific. These backend
 * entry points intentionally expose no social UI.
 */
bool clientSendAutomatiaPartyRequest(
    const AutomatiaParty::Protocol::Request& request);
const AutomatiaParty::Protocol::PartyState& clientAutomatiaPartyState();
const AutomatiaParty::Protocol::InvitationList&
    clientAutomatiaPartyInvitations();
bool clientTakeAutomatiaPartyResult(
    AutomatiaParty::Protocol::Result& result);
void clientResetAutomatiaPartyState();

/*
 * Submits Party chat through the server-authoritative route. Remote clients
 * emit a bounded PCHT request; a listen-server local player enters the same
 * PartyManager recipient-resolution path without fabricating a network peer.
 */
bool submitAutomatiaPartyChat(
    int localPlayer,
    const std::string& message);

/*
 * Social UI facade. Remote clients still travel through authenticated PTYQ;
 * a listen-server's local player executes through the same validator and
 * reads the same WorldState-owned authority without creating a loopback
 * packet or a second client-side source of truth.
 */
bool submitAutomatiaPartyRequestForLocalPlayer(
    int localPlayerSlot,
    const AutomatiaParty::Protocol::Request& request);
bool copyAutomatiaPartySnapshotForLocalPlayer(
    int localPlayerSlot,
    AutomatiaParty::Protocol::PartyState& partyState,
    AutomatiaParty::Protocol::InvitationList& invitationList);

/*
 * The lobby receive loop uses the platform APIs directly instead of
 * NetHandler, so it must publish the authenticated P2P sender while a packet
 * is dispatched. Direct-connect packets continue to authenticate by their
 * UDP endpoint.
 */
void setLobbyPacketSenderHostIndex(int senderHostIndex);
bool lobbyPacketSenderMatchesPlayer(int playerIndex);
bool lobbyPacketSenderIsServer();

// server/game flags
extern Uint32 svFlags;
extern Uint32 settings_svFlags;
const Uint32 SV_FLAG_CHEATS  = 1 << 0;
const Uint32 SV_FLAG_FRIENDLYFIRE = 1 << 1;
const Uint32 SV_FLAG_MINOTAURS = 1 << 2;
const Uint32 SV_FLAG_HUNGER  = 1 << 3;
const Uint32 SV_FLAG_TRAPS = 1 << 4;
const Uint32 SV_FLAG_HARDCORE = 1 << 5;
const Uint32 SV_FLAG_CLASSIC = 1 << 6;
const Uint32 SV_FLAG_KEEPINVENTORY = 1 << 7;
const Uint32 SV_FLAG_LIFESAVING = 1 << 8;
const Uint32 SV_FLAG_ASSIST_ITEMS = 1 << 9;
const Uint32 SV_FLAG_INFINITE_DUNGEON = 1 << 10;
const Uint32 NUM_SERVER_FLAGS =  11;

extern bool keepInventoryGlobal;

class SteamPacketWrapper
{
	Uint8* _data;
	int _len;
	int _senderHostIndex;
	//TODO: Encapsulate CSteam ID?
public:
	SteamPacketWrapper(Uint8* data, int len, int senderHostIndex = -1);
	~SteamPacketWrapper(); //NOTE: DOES free _data. Don't keep it somewhere else or segfaults will ensue. If you're lucky.

	Uint8*& data();
	int& len();
	int senderHostIndex() const;
};

class NetHandler
{
	SDL_Thread* steam_packet_thread;
	bool continue_multithreading_steam_packets;
	SDL_mutex* game_packets_lock;
public:
	NetHandler();
	~NetHandler();
	std::queue<SteamPacketWrapper* > game_packets;

	void initializeMultithreadedPacketHandling();
	void stopMultithreadedPacketHandling();
	void toggleMultithreading(bool disableMultithreading);

	bool getContinueMultithreadingSteamPackets();

	void addGamePacket(SteamPacketWrapper* packet);

	/*
	 * This function will take the next packet in the queue, pop it off, and then return it.
	 * Returns nullptr if no packets.
	 * NOTE: You *MUST* free the data returned by this, or else you will leak memory! Such is the way of things.
	 */
	SteamPacketWrapper* getGamePacket();

	SDL_mutex* continue_multithreading_steam_packets_lock;
};
extern NetHandler* net_handler;

extern bool disableMultithreadedSteamNetworking;
extern bool disableFPSLimitOnNetworkMessages;

int steamPacketThread(void* data);
int EOSPacketThread(void* data);

void deleteMultiplayerSaveGames(); //Server function, deletes its own save and broadcasts delete packet to clients.

void handleScanPacket(); // when we receive a SCAN packet (request for lobby info)

struct PingNetworkStatus_t
{
	std::map<Uint32, Uint32> pings;
	Uint32 lastPingtime = 0;
	Uint32 lastSequence = 0;
	Uint32 oldestSequenceTicks = 0;
	Uint32 sequence = 0;
	Uint32 displayMillis = 0;
	Uint32 displayMillisImmediate = 0;
	Uint32 hudDisplayOKTicks = 0;
	bool needsUpdate = true;
	void saveDisplayMillis(bool forceUpdate = false);
	void clear()
	{
		pings.clear();
		needsUpdate = true;
		hudDisplayOKTicks = 0;
		lastPingtime = 0;
		lastSequence = 0;
		oldestSequenceTicks = 0;
		displayMillis = 0;
		sequence = 0;
		displayMillisImmediate = 0;
	}
	static bool bEnabled;
	static int pingLimitGreen;
	static int pingLimitYellow;
	static int pingLimitOrange;
	static bool pingHUDDisplayGreen;
	static bool pingHUDDisplayYellow;
	static bool pingHUDDisplayOrange;
	static bool pingHUDDisplayRed;
	static bool pingHUDShowOKBriefly;
	static bool pingHUDShowNumericValue;
	static void receive();
	static void respond();
	static void update();
	static void reset();
};
extern PingNetworkStatus_t PingNetworkStatus[MAXPLAYERS];
bool serverPlayerCanReceiveGameplayUpdates(int playerIndex);
bool serverPlayerCanReceiveActiveMapUpdates(int playerIndex);
bool serverPlayerCanReceivePlayableFloorUpdates(int playerIndex, PlayableFloorId playableFloor);
bool serverPlayerCanReceiveEntityUpdates(int playerIndex, const Entity* entity);
bool beginServerLateJoinSnapshot(
    int playerIndex,
    Uint32 transferId,
    Uint64 instanceRevision,
    Uint32 chunkCount,
    Uint32 totalBytes
);
LateJoinChunkResult acceptServerLateJoinSnapshotChunk(
    int playerIndex,
    Uint32 transferId,
    Uint64 instanceRevision,
    Uint32 sequence,
    Uint32 payloadBytes,
    Uint32 payloadChecksum
);
bool authorizeServerLateJoinPlayer(int playerIndex);
void resetServerLateJoinPlayer(int playerIndex);
