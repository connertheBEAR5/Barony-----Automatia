/*-------------------------------------------------------------------------------

	BARONY
	File: net.cpp
	Desc: support functions for networking

	Copyright 2013-2016 (c) Turning Wheel LLC, all rights reserved.
	See LICENSE for details.

-------------------------------------------------------------------------------*/

#include "main.hpp"
#include "draw.hpp"
#include "game.hpp"
#include "stat.hpp"
#include "net.hpp"
#include "messages.hpp"
#include "entity.hpp"
#include "files.hpp"
#include "monster.hpp"
#include "interface/interface.hpp"
#include "magic/magic.hpp"
#include "engine/audio/sound.hpp"
#include "items.hpp"
#ifdef SAM_FRAMEWORK_ENABLED
#include "sam/sam_item_registry_foundation.hpp"
#endif
#include "shops.hpp"
#include "menu.hpp"
#include "scores.hpp"
#include "collision.hpp"
#include "paths.hpp"
#ifdef STEAMWORKS
#include <steam/steam_api.h>
#include "steam.hpp"
#endif
#include "player.hpp"
#include "world_state.hpp"
#include "world_packet_scope.hpp"
#include "late_join_protocol.hpp"
#include "lan_discovery.hpp"
#include "scores.hpp"
#include "colors.hpp"
#include "mod_tools.hpp"
#include "lobbies.hpp"
#include "ui/MainMenu.hpp"
#include "ui/LoadingScreen.hpp"
#include "ui/GameUI.hpp"
#include "interface/ui.hpp"
#ifdef USE_PLAYFAB
#include "playfab.hpp"
#endif

#include <atomic>
#include <future>
#include <random>
#include <thread>

namespace
{
	constexpr Uint8 kJoinCapabilityHeloChunkV1 = 0x01;
	constexpr Uint8 kJoinCapabilityLateJoinV1 = 0x02;
	constexpr Uint8 kJoinCapabilityReconnectTokenV1 = 0x04;
	constexpr Uint8 kEntityArchetypeNone = 0;
	constexpr Uint8 kEntityArchetypeCustomPortal = 1;
	constexpr Uint8 kEntityArchetypeEditorLight = 2;
	constexpr std::size_t kEntityArchetypeOffset = 47;
	constexpr std::size_t kReconnectTokenLength = ReconnectToken::length;
	constexpr int kHeloChunkHeaderSize = 12;
	constexpr int kHeloChunkPayloadMax = 900;
	constexpr int kHeloSinglePacketMax = 1100;
	constexpr int kHeloChunkMaxCount = 32;
	static Uint16 g_heloTransferId[MAXPLAYERS] = { 0 };
	static int g_currentPacketSenderHostIndex = -1;
	static LateJoinSnapshotTransaction g_lateJoinTransactions[MAXPLAYERS];
	static LateJoinPacketCatchupBuffer g_lateJoinCatchupBuffers[MAXPLAYERS];
	static LateJoinProtocol::SnapshotAssembler g_clientLateJoinAssembler;
	static LateJoinProtocol::SnapshotAssembler g_clientLateJoinCatchupAssembler;
	static LateJoinProtocol::Begin g_clientLateJoinBegin;
	static bool g_clientLateJoinSpawnAuthorized = false;
	static bool g_clientLateJoinPacketDeferral = false;
	static bool g_clientLateJoinCatchupComplete = false;
	static bool g_clientLateJoinMapIsLoaded = false;
	static bool g_clientLateJoinReplayingPackets = false;
	static LateJoinPacketCatchupBuffer g_clientLateJoinLivePackets;
	static std::vector<std::vector<std::uint8_t>>
		g_clientLateJoinCatchupPackets;
	static Uint32 g_lateJoinWireTransferId[MAXPLAYERS] = { 0 };
	static Uint32 g_lateJoinLastProgressTick[MAXPLAYERS] = { 0 };
	static bool g_lateJoinReturningPlayer[MAXPLAYERS] = { false };
	static bool g_lateJoinClientHandshake[MAXPLAYERS] = { false };
	static bool g_processingRuntimeJoin = false;
	static Uint32 g_clientLateJoinLastProgressTick = 0;
	static Uint32 g_clientProvisionalEntityUid = 0x70000000U;
	static bool g_resendingScopedSafePacket = false;
	static bool g_removedEntityTombstonesHaveInstanceScope = false;
	static std::string g_removedEntityTombstoneInstanceKey;
	static Uint64 g_removedEntityTombstoneRevision = 0;
	constexpr Uint32 kLateJoinTimeoutTicks = 30 * TICKS_PER_SECOND;
	constexpr Uint32 kLateJoinCharacterSelectionTimeoutTicks =
		5 * 60 * TICKS_PER_SECOND;

	static std::string generateReconnectToken()
	{
		try
		{
			static constexpr char hex[] = "0123456789abcdef";
			std::random_device source;
			std::string token(kReconnectTokenLength, '0');
			for (std::size_t index = 0; index < token.size(); index += 2)
			{
				const unsigned value = source();
				token[index] = hex[(value >> 4U) & 0x0fU];
				token[index + 1] = hex[value & 0x0fU];
			}
			return token;
		}
		catch (const std::exception&)
		{
			return {};
		}
	}

	static void setRemovedEntityTombstoneScope(
		const WorldInstanceIdentity* identity)
	{
		g_removedEntityTombstonesHaveInstanceScope = identity != nullptr;
		g_removedEntityTombstoneInstanceKey = identity
			? identity->key()
			: std::string{};
		g_removedEntityTombstoneRevision = identity
			? identity->revision
			: 0;
	}

	static bool removedEntityTombstonesApplyToActiveInstance()
	{
		const WorldInstanceIdentity* active = worldState.activeIdentity();
		return removedEntityTombstoneAppliesToInstance(
			g_removedEntityTombstonesHaveInstanceScope,
			g_removedEntityTombstoneInstanceKey.c_str(),
			g_removedEntityTombstoneRevision,
			active ? active->key().c_str() : "",
			active ? active->revision : 0);
	}

	static void prepareRemovedEntityTombstonesForActiveInstance()
	{
		if (removedEntityTombstonesApplyToActiveInstance())
		{
			return;
		}
		list_FreeAll(&removedEntities);
		setRemovedEntityTombstoneScope(worldState.activeIdentity());
	}

	static bool serverPlayerSharesActiveMap(int playerIndex)
	{
		return playerIndex > 0
			&& playerIndex < MAXPLAYERS
			&& !client_disconnected[playerIndex]
			&& players[playerIndex]
			&& !players[playerIndex]->isLocalPlayer()
			&& worldState.playerSharesActiveInstance(playerIndex);
	}

	static Uint8 networkEntityArchetype(const Entity* entity)
	{
		if (!entity)
		{
			return kEntityArchetypeNone;
		}
		if (entity->behavior == &actCustomPortal)
		{
			return kEntityArchetypeCustomPortal;
		}
		if (entity->behavior == &actLightSource)
		{
			return kEntityArchetypeEditorLight;
		}
		return kEntityArchetypeNone;
	}

	static Uint8 receivedEntityArchetype()
	{
		if (!net_packet
			|| net_packet->len <= static_cast<int>(kEntityArchetypeOffset))
		{
			return kEntityArchetypeNone;
		}
		return net_packet->data[kEntityArchetypeOffset];
	}

	static Entity* findUnboundMapFixtureForEntityUpdate(Uint8 archetype)
	{
		if (!net_packet || !map.entities
			|| (archetype != kEntityArchetypeCustomPortal
				&& archetype != kEntityArchetypeEditorLight))
		{
			return nullptr;
		}
		const real_t packetX =
			static_cast<Sint16>(SDLNet_Read16(&net_packet->data[10])) / 32.0;
		const real_t packetY =
			static_cast<Sint16>(SDLNet_Read16(&net_packet->data[12])) / 32.0;
		const real_t packetZ =
			static_cast<Sint16>(SDLNet_Read16(&net_packet->data[14])) / 32.0;
		Entity* best = nullptr;
		real_t bestDistance = 0.25;
		for (node_t* node = map.entities->first; node; node = node->next)
		{
			Entity* candidate = static_cast<Entity*>(node->element);
			if (!candidate || candidate->lastupdateserver != 0)
			{
				continue;
			}
			const bool matchingBehavior =
				(archetype == kEntityArchetypeCustomPortal
					&& candidate->behavior == &actCustomPortal)
				|| (archetype == kEntityArchetypeEditorLight
					&& candidate->behavior == &actLightSource);
			if (!matchingBehavior)
			{
				continue;
			}
			const real_t distance = std::abs(candidate->x - packetX)
				+ std::abs(candidate->y - packetY)
				+ std::abs(candidate->z - packetZ);
			if (distance <= bestDistance)
			{
				best = candidate;
				bestDistance = distance;
			}
		}
		return best;
	}

	static bool currentPacketSenderMatchesPlayer(int playerIndex)
	{
		if (playerIndex <= 0 || playerIndex >= MAXPLAYERS || !net_packet)
		{
			return false;
		}
		if (directConnect)
		{
			return net_packet->address.host == net_clients[playerIndex - 1].host
				&& net_packet->address.port == net_clients[playerIndex - 1].port;
		}
		return g_currentPacketSenderHostIndex == playerIndex - 1;
	}

	static void logServerRosterState(const char* reason)
	{
		if (multiplayer != SERVER)
		{
			return;
		}
		int connected = 0;
		int reconnectReserved = 0;
		int available = 0;
		for (int player = 1; player < MAXPLAYERS; ++player)
		{
			if (!client_disconnected[player])
			{
				++connected;
			}
			else if (automatiaHasSavedPlayerPlacement(player))
			{
				++reconnectReserved;
			}
			else
			{
				++available;
			}
		}
		printlog(
			"[Roster] %s: connected=%d reconnect-reserved=%d available=%d.",
			reason ? reason : "updated", connected, reconnectReserved, available);
	}

	static bool serverRouteActiveMapPacket(
		int playerIndex, const Uint8* data, std::size_t size)
	{
		if (!serverPlayerSharesActiveMap(playerIndex))
		{
			return false;
		}
		LateJoinSnapshotTransaction& transaction =
			g_lateJoinTransactions[playerIndex];
		if (transaction.mayReceiveLiveSimulation())
		{
			return true;
		}
		const bool capture =
			transaction.phase() == LateJoinSnapshotTransaction::Phase::Receiving
			|| transaction.phase() == LateJoinSnapshotTransaction::Phase::Complete;
		if (!capture || !data || size < 4)
		{
			return false;
		}
		// Reliable retries already in flight before snapshot capture are stale.
		// Initial reliable sends reach this function before SAFE wrapping.
		if (std::memcmp(data, "SAFE", 4) == 0)
		{
			return false;
		}
		if (!g_lateJoinCatchupBuffers[playerIndex].append(data, size))
		{
			transaction.fail();
			printlog(
				"[Late Join] Packet catch-up limit exceeded for player %d; transfer will abort.",
				playerIndex);
		}
		return false;
	}

	static Uint32 moveClientEntityOutOfAuthoritativeUid(Entity* entity)
	{
		if (!entity)
		{
			return 0;
		}
		while (g_clientProvisionalEntityUid > 0x60000000U
			&& uidToEntity(static_cast<Sint32>(g_clientProvisionalEntityUid)))
		{
			--g_clientProvisionalEntityUid;
		}
		if (g_clientProvisionalEntityUid <= 0x60000000U)
		{
			return 0;
		}
		const Uint32 provisionalUid = g_clientProvisionalEntityUid--;
		const Uint32 oldUid = entity->getUID();
		entity->setUID(provisionalUid);
		if (map.entities && oldUid != provisionalUid)
		{
			for (node_t* node = map.entities->first; node; node = node->next)
			{
				Entity* child = static_cast<Entity*>(node->element);
				if (child && child != entity && child->parent == oldUid)
				{
					child->parent = provisionalUid;
				}
			}
		}
		return provisionalUid;
	}

	static void adoptAuthoritativeUidForClientPlayerHead(
		Entity* entity, Uint32 authoritativeUid)
	{
		if (!entity || authoritativeUid == 0)
		{
			return;
		}
		const Uint32 previousUid = entity->getUID();
		if (previousUid == authoritativeUid)
		{
			return;
		}
		entity->setUID(authoritativeUid);
		if (!map.entities)
		{
			return;
		}
		for (node_t* node = map.entities->first; node; node = node->next)
		{
			Entity* attached = static_cast<Entity*>(node->element);
			if (attached && attached != entity && attached->parent == previousUid)
			{
				attached->parent = authoritativeUid;
			}
		}
	}

	static void ensureClientPlayerVisualInitialized(Entity* entity)
	{
		if (multiplayer != CLIENT
			|| !entity
			|| entity->behavior != &actPlayer
			|| entity->skill[2] < 0
			|| entity->skill[2] >= MAXPLAYERS
			|| entity->skill[0] != 0)
		{
			return;
		}
		const int player = entity->skill[2];
		actPlayer(entity);
		printlog(
			"[World State] Client initialized player %d UID %u voxel model with %u child node(s).",
			player,
			entity->getUID(),
			static_cast<unsigned>(list_Size(&entity->children)));
	}

	static int decodeGameplayPacketPlayerIndex(
		const Uint8 encodedPlayer
	)
	{
		const int player =
			static_cast<int>(encodedPlayer);

		if ( player >= MAXPLAYERS )
		{
			printlog(
				"[NET]: ignoring gameplay packet with invalid player index %d",
				player
			);
			return -1;
		}
        if ( multiplayer == SERVER
            && packetUsesActiveMapScope(
                net_packet ? net_packet->data : nullptr,
                net_packet ? net_packet->len : 0
            )
            && directConnect
            && player > 0
            && (
                net_packet->address.host != net_clients[player - 1].host
                || net_packet->address.port != net_clients[player - 1].port
            ) )
        {
            printlog(
                "[NET]: ignoring map-local packet with invalid sender for player %d",
                player
            );
            return -1;
        }
        if ( multiplayer == SERVER
            && !directConnect
            && player > 0
            && packetUsesActiveMapScope(
                net_packet ? net_packet->data : nullptr,
                net_packet ? net_packet->len : 0
            )
            && g_currentPacketSenderHostIndex != player - 1 )
        {
            printlog(
                "[NET]: ignoring map-local P2P packet whose sender does not match player %d",
                player
            );
            return -1;
        }
        if ( multiplayer == SERVER
            && packetUsesActiveMapScope(
                net_packet ? net_packet->data : nullptr,
                net_packet ? net_packet->len : 0
            )
            && !worldState.playerSharesActiveInstance(player) )
        {
            const std::string playerInstanceKey =
                players[player]
                ? players[player]->worldInstance.key()
                : std::string{};
            if ( playerInstanceKey.empty()
                || !worldState.activate(playerInstanceKey)
                || !worldState.playerSharesActiveInstance(player) )
            {
                printlog(
                    "[NET]: ignoring map-local packet for unavailable instance '%s' from player %d",
                    playerInstanceKey.c_str(),
                    player
                );
                return -1;
            }
        }

		return player;
	}

#ifdef SAM_FRAMEWORK_ENABLED
    static bool resolveSAMItemTypeFromPacket(
        const int transmittedType,
        const int stableIdOffset,
        const char* packetName,
        int& resolvedType
    )
    {
        resolvedType = transmittedType;

        if ( net_packet->len > stableIdOffset )
        {
            const int payloadLength = net_packet->len - stableIdOffset;
            int stableIdLength = 0;
            while ( stableIdLength < payloadLength
                && net_packet->data[stableIdOffset + stableIdLength] != '\0' )
            {
                ++stableIdLength;
            }

            if ( stableIdLength <= 0
                || stableIdLength >= payloadLength )
            {
                printlog(
                    "[S.A.M] Refusing malformed %s stable-id payload.\n",
                    packetName
                );
                return false;
            }

            const std::string stableId(
                reinterpret_cast<const char*>(
                    &net_packet->data[stableIdOffset]
                ),
                stableIdLength
            );
            resolvedType =
                SAMItemRegistryFoundation::
                    runtimeIdForStableId(stableId);
            if ( resolvedType < 0
                || !SAMItemRegistryFoundation::
                    isRegisteredRuntimeItemId(resolvedType) )
            {
                printlog(
                    "[S.A.M] %s custom item unavailable locally: [%s]. Item rejected.\n",
                    packetName,
                    stableId.c_str()
                );
                return false;
            }

            return true;
        }

        if ( SAMItemRegistryFoundation::
            isSAMRuntimeItemId(transmittedType) )
        {
            printlog(
                "[S.A.M] Refusing numeric-only %s custom runtime %d.\n",
                packetName,
                transmittedType
            );
            return false;
        }

        return true;
    }
#endif

	static Uint16 nextHeloTransferIdForPlayer(const int player)
	{
		if ( player <= 0 || player >= MAXPLAYERS )
		{
			return 0;
		}

		++g_heloTransferId[player];
		if ( g_heloTransferId[player] == 0 )
		{
			++g_heloTransferId[player];
		}
		return g_heloTransferId[player];
	}

	static bool sendChunkedHeloDirect(
		const Uint8* heloData,
		const int heloLen,
		const Uint16 transferId,
		const int playerNumForLog
	)
	{
		if ( !heloData
			|| heloLen <= 0
			|| heloLen > NET_PACKET_SIZE )
		{
			printlog(
				"[NET]: refusing chunked HELO with invalid payload length %d",
				heloLen
			);
			return false;
		}

		const int chunkPayloadMax =
			std::min(
				kHeloChunkPayloadMax,
				NET_PACKET_SIZE - kHeloChunkHeaderSize
			);
		const int chunkCount =
			(heloLen + chunkPayloadMax - 1)
				/ chunkPayloadMax;

		if ( chunkPayloadMax <= 0
			|| chunkCount <= 0
			|| chunkCount > kHeloChunkMaxCount
			|| chunkCount > 0xFF )
		{
			printlog(
				"[NET]: refusing chunked HELO with invalid chunk count %d",
				chunkCount
			);
			return false;
		}

		printlog(
			"sending chunked HELO: player=%d transfer=%u chunks=%d total=%d",
			playerNumForLog,
			static_cast<unsigned>(transferId),
			chunkCount,
			heloLen
		);

		for ( int chunkIndex = 0;
			chunkIndex < chunkCount;
			++chunkIndex )
		{
			const int offset =
				chunkIndex * chunkPayloadMax;
			const int chunkLen =
				std::min(
					chunkPayloadMax,
					heloLen - offset
				);

			if ( chunkLen <= 0
				|| chunkLen > chunkPayloadMax )
			{
				return false;
			}

			memcpy(net_packet->data, "HLCN", 4);
			SDLNet_Write16(
				transferId,
				&net_packet->data[4]
			);
			net_packet->data[6] =
				static_cast<Uint8>(chunkIndex);
			net_packet->data[7] =
				static_cast<Uint8>(chunkCount);
			SDLNet_Write16(
				static_cast<Uint16>(heloLen),
				&net_packet->data[8]
			);
			SDLNet_Write16(
				static_cast<Uint16>(chunkLen),
				&net_packet->data[10]
			);
			memcpy(
				&net_packet->data[kHeloChunkHeaderSize],
				heloData + offset,
				chunkLen
			);
			net_packet->len =
				kHeloChunkHeaderSize + chunkLen;

			if ( !sendPacketSafe(
					net_sock,
					-1,
					net_packet,
					0
				) )
			{
				printlog(
					"[NET]: failed sending HELO chunk %d/%d",
					chunkIndex + 1,
					chunkCount
				);
				return false;
			}
		}

		return true;
	}
}

static void tryReplayClientLateJoinPackets();

void clientResetLateJoinPacketDeferral()
{
	g_clientLateJoinPacketDeferral = false;
	g_clientLateJoinCatchupComplete = false;
	g_clientLateJoinMapIsLoaded = false;
	g_clientLateJoinReplayingPackets = false;
	g_clientLateJoinCatchupAssembler.reset();
	g_clientLateJoinLivePackets.reset();
	g_clientLateJoinCatchupPackets.clear();
	g_clientLateJoinLastProgressTick = 0;
}

void clientBeginLateJoinPacketDeferral(Uint32 transferId, Uint64 revision)
{
	clientResetLateJoinPacketDeferral();
	if (transferId == 0)
	{
		return;
	}
	g_clientLateJoinBegin.transferId = transferId;
	g_clientLateJoinBegin.instanceRevision = revision;
	g_clientLateJoinPacketDeferral = true;
	g_clientLateJoinLastProgressTick = ticks;
}

bool clientAcceptLateJoinCatchupBegin(const Uint8* data, std::size_t size)
{
	LateJoinProtocol::Complete metadata;
	if (!g_clientLateJoinPacketDeferral
		|| !LateJoinProtocol::decodeCatchupBegin(data, size, metadata)
		|| metadata.transferId != g_clientLateJoinBegin.transferId
		|| metadata.instanceRevision != g_clientLateJoinBegin.instanceRevision
		|| metadata.totalBytes > LateJoinPacketCatchupBuffer::maxSerializedBytes)
	{
		return false;
	}
	LateJoinProtocol::Begin begin;
	begin.transferId = metadata.transferId;
	begin.instanceRevision = metadata.instanceRevision;
	begin.chunkCount = metadata.chunkCount;
	begin.totalBytes = metadata.totalBytes;
	begin.snapshotChecksum = metadata.snapshotChecksum;
	const bool accepted = g_clientLateJoinCatchupAssembler.begin(begin);
	if (accepted)
	{
		g_clientLateJoinLastProgressTick = ticks;
	}
	return accepted;
}

bool clientAcceptLateJoinCatchupChunk(const Uint8* data, std::size_t size)
{
	LateJoinProtocol::Chunk chunk;
	const bool accepted = g_clientLateJoinPacketDeferral
		&& LateJoinProtocol::decodeCatchupChunk(data, size, chunk)
		&& g_clientLateJoinCatchupAssembler.accept(chunk)
			!= LateJoinProtocol::ReceiveResult::Rejected;
	if (accepted)
	{
		g_clientLateJoinLastProgressTick = ticks;
	}
	return accepted;
}

bool clientAcceptLateJoinCatchupComplete(const Uint8* data, std::size_t size)
{
	LateJoinProtocol::Complete complete;
	if (!g_clientLateJoinPacketDeferral
		|| !LateJoinProtocol::decodeCatchupComplete(data, size, complete)
		|| g_clientLateJoinCatchupAssembler.finish(complete)
			!= LateJoinProtocol::ReceiveResult::Complete
		|| !LateJoinPacketCatchupBuffer::deserialize(
			g_clientLateJoinCatchupAssembler.snapshot(),
			g_clientLateJoinCatchupPackets))
	{
		return false;
	}
	g_clientLateJoinCatchupComplete = true;
	g_clientLateJoinLastProgressTick = ticks;
	tryReplayClientLateJoinPackets();
	return true;
}

void clientCheckLateJoinTimeout()
{
	if (!g_clientLateJoinPacketDeferral
		|| g_clientLateJoinLastProgressTick == 0
		|| ticks - g_clientLateJoinLastProgressTick <= kLateJoinTimeoutTicks)
	{
		return;
	}
	LateJoinProtocol::Abort abort;
	abort.playerIndex = static_cast<std::uint8_t>(clientnum);
	abort.transferId = g_clientLateJoinBegin.transferId;
	abort.instanceRevision = g_clientLateJoinBegin.instanceRevision;
	abort.reason = 1;
	const std::vector<std::uint8_t> record =
		LateJoinProtocol::encodeAbort(abort);
	if (net_packet && net_sock && !record.empty())
	{
		memcpy(net_packet->data, record.data(), record.size());
		net_packet->len = static_cast<int>(record.size());
		net_packet->address.host = net_server.host;
		net_packet->address.port = net_server.port;
		sendPacketSafe(net_sock, -1, net_packet, 0);
	}
	printlog("[Late Join] Client aborted an incomplete transfer after 30 seconds.");
	discardAutomatiaPersistentWorldSnapshot();
	g_clientLateJoinAssembler.reset();
	g_clientLateJoinSpawnAuthorized = false;
	clientResetLateJoinPacketDeferral();
}

bool clientLateJoinPacketDeferralActive()
{
	return g_clientLateJoinPacketDeferral;
}

void clientNoteLateJoinProgress()
{
	if (g_clientLateJoinPacketDeferral)
	{
		g_clientLateJoinLastProgressTick = ticks;
	}
}

bool clientDeferLateJoinMapPacket(const Uint8* data, std::size_t size)
{
	if (!g_clientLateJoinPacketDeferral || g_clientLateJoinReplayingPackets
		|| !packetUsesActiveMapScope(data, size))
	{
		return false;
	}
	if (!g_clientLateJoinLivePackets.append(data, size))
	{
		printlog("[Late Join] Client live-packet deferral limit exceeded.");
	}
	return true;
}

void clientLateJoinMapLoaded()
{
	if (!g_clientLateJoinPacketDeferral)
	{
		return;
	}
	g_clientLateJoinMapIsLoaded = true;
	tryReplayClientLateJoinPackets();
}

NetHandler* net_handler = nullptr;
struct PendingTunnelSpawn
{
    bool active = false;
    real_t x = 0.0;
    real_t y = 0.0;
    real_t z = 0.0;
    real_t yaw = 0.0;
};

static PendingTunnelSpawn pendingTunnelSpawn;
static bool pendingIndependentLevelChange = false;
static int pendingIndependentPlayer = -1;
static Uint32 pendingIndependentRuntimeUid = 0;
char last_ip[64] = "";
char last_port[64] = "";
char lobbyChatbox[LOBBY_CHATBOX_LENGTH];
list_t lobbyChatboxMessages;
bool disableMultithreadedSteamNetworking = true;
bool disableFPSLimitOnNetworkMessages = true; // always process the messages, otherwise you can get vastly behind.

// uncomment this to have the game log packet info
//#define PACKETINFO

void packetDeconstructor(void* data)
{
	packetsend_t* packetsend = (packetsend_t*)data;
	SDLNet_FreePacket(packetsend->packet);
	free(data);
}
bool applyPendingTunnelSpawn()
{
    if ( !pendingTunnelSpawn.active )
    {
        return false;
    }

    if ( clientnum < 0
        || clientnum >= MAXPLAYERS
        || players[clientnum] == nullptr
        || players[clientnum]->entity == nullptr )
    {
        return false;
    }

    Entity* playerEntity =
        players[clientnum]->entity;

    playerEntity->x =
        pendingTunnelSpawn.x;

    playerEntity->y =
        pendingTunnelSpawn.y;

    playerEntity->z =
        pendingTunnelSpawn.z;

    playerEntity->yaw =
        pendingTunnelSpawn.yaw;

    playerEntity->new_x =
        playerEntity->x;

    playerEntity->new_y =
        playerEntity->y;

    playerEntity->new_z =
        playerEntity->z;

    playerEntity->new_yaw =
        playerEntity->yaw;

    playerEntity->vel_x = 0.0;
    playerEntity->vel_y = 0.0;
    playerEntity->vel_z = 0.0;

    playerEntity->bNeedsRenderPositionInit = true;

    for ( Entity* bodypart : playerEntity->bodyparts )
    {
        if ( bodypart )
        {
            bodypart->bNeedsRenderPositionInit = true;
        }
    }

    for ( node_t* node = map.entities->first; node != nullptr; node = node->next )
    {
        Entity* entity = static_cast<Entity*>(node->element);
        if ( entity && entity->behavior == &actSpriteNametag )
        {
            if ( entity->parent == playerEntity->getUID() )
            {
                entity->bNeedsRenderPositionInit = true;
            }
        }
    }

    temporarilyDisableDithering();

    printlog(
        "[Custom Tunnel] Client applied server tunnel spawn: x=%.2f y=%.2f z=%.2f yaw=%.2f.",
        playerEntity->x,
        playerEntity->y,
        playerEntity->z,
        playerEntity->yaw
    );

    pendingTunnelSpawn.active = false;
    return true;
}
void pollNetworkForShutdown() {
	// handle network messages
	if ( !(SDL_GetTicks() % 25) && multiplayer )
	{
		int j = 0;
		node_t* node, *nextnode;
		for ( node = safePacketsSent.first; node != NULL; node = nextnode )
		{
			nextnode = node->next;

			packetsend_t* packet = (packetsend_t*)node->element;
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
#ifdef STEAMWORKS
	SteamAPI_RunCallbacks();
#endif // STEAMWORKS
#ifdef USE_EOS
	if (EOS.PlatformHandle) {
		EOS_Platform_Tick(EOS.PlatformHandle);
	}
	if (EOS.ServerPlatformHandle) {
		EOS_Platform_Tick(EOS.ServerPlatformHandle);
	}
#endif // USE_EOS
}

/*-------------------------------------------------------------------------------

	sendPacket

	when STEAMWORKS is undefined, works like SDLNet_UDP_Send and last argument
	is ignored. Otherwise, the first two arguments are ignored and the packet
	is sent with SteamNetworking()->SendP2PPacket, using the hostnum variable
	to get the steam ID of the same player number and the first two arguments
	are ignored.

-------------------------------------------------------------------------------*/

int sendPacket(UDPsocket sock, int channel, UDPpacket* packet, int hostnum, bool tryReliable)
{
    if ( multiplayer == SERVER
		&& !g_resendingScopedSafePacket
        && packetUsesActiveMapScope(packet ? packet->data : nullptr, packet ? packet->len : 0)
        && !serverRouteActiveMapPacket(
			hostnum + 1, packet ? packet->data : nullptr,
			packet ? static_cast<std::size_t>(packet->len) : 0) )
    {
        return 0;
    }
	if ( directConnect )
	{
		return SDLNet_UDP_Send(sock, channel, packet);
	}
	else
	{
	    if (hostnum < 0 || hostnum >= MAXPLAYERS) {
	        return 0;
	    }
		if ( LobbyHandler.getP2PType() == LobbyHandler_t::LobbyServiceType::LOBBY_STEAM )
		{
#ifdef STEAMWORKS
			if ( steamIDRemote[hostnum] )
			{
				return SteamNetworking()->SendP2PPacket(*static_cast<CSteamID* >(steamIDRemote[hostnum]), packet->data, packet->len, tryReliable? k_EP2PSendReliable : k_EP2PSendUnreliable, 0);
			}
			else
			{
				return 0;
			}
#endif
		}
		else if ( LobbyHandler.getP2PType() == LobbyHandler_t::LobbyServiceType::LOBBY_CROSSPLAY )
		{
#if defined USE_EOS
			EOS.SendMessageP2P(EOS.P2PConnectionInfo.getPeerIdFromIndex(hostnum), (char*)packet->data, packet->len);
			return 0;
#endif
		}
		return 0;
	}
}

/*-------------------------------------------------------------------------------

	sendPacketSafe

	works like sendPacket, but adds an additional layer of insurance to
	increase the chance of a successful transmission. When STEAMWORKS is
	undefined, the game uses its own system to increase the transmission
	success rate of a packet. Otherwise it just sends the packet with
	SendP2PPacket's k_EP2PSendReliable flag

-------------------------------------------------------------------------------*/

Uint32 packetnum = 0;
int sendPacketSafe(UDPsocket sock, int channel, UDPpacket* packet, int hostnum)
{
	if ( hostnum < 0 || hostnum >= MAXPLAYERS )
	{
		printlog("[NET]: Error - attempt to send to non-valid hostnum: %d", hostnum);
		return 0;
	}
    if ( multiplayer == SERVER
        && packetUsesActiveMapScope(packet ? packet->data : nullptr, packet ? packet->len : 0)
        && !serverRouteActiveMapPacket(
			hostnum + 1, packet ? packet->data : nullptr,
			packet ? static_cast<std::size_t>(packet->len) : 0) )
    {
        return 0;
    }

	if ( !directConnect )
	{
		if ( LobbyHandler.getP2PType() == LobbyHandler_t::LobbyServiceType::LOBBY_STEAM )
		{
#ifdef STEAMWORKS
			if ( !steamIDRemote[hostnum] )
			{
				return 0;
			}
#endif
		}
		else if ( LobbyHandler.getP2PType() == LobbyHandler_t::LobbyServiceType::LOBBY_CROSSPLAY )
		{
#if defined USE_EOS
			if ( !EOS.P2PConnectionInfo.getPeerIdFromIndex(hostnum) )
			{
				return 0;
			}
#endif
		}
	}

	packetsend_t* packetsend = (packetsend_t*) malloc(sizeof(packetsend_t));
	if (!packetsend)
	{
		return 0;
	}
	memset(packetsend, 0, sizeof(*packetsend));
	if (!(packetsend->packet = SDLNet_AllocPacket(NET_PACKET_SIZE)))
	{
		printlog("warning: packet allocation failed: %s\n", SDLNet_GetError());
		free(packetsend);
		return 0;
	}

	packetsend->hostnum = hostnum;
	packetsend->sock = sock;
	packetsend->channel = channel;
	packetsend->packet->channel = channel;
	memcpy(packetsend->packet->data + 9, packet->data, NET_PACKET_SIZE - 9);
	packetsend->packet->len = packet->len + 9;
	packetsend->packet->address.host = packet->address.host;
	packetsend->packet->address.port = packet->address.port;
	strcpy((char*)packetsend->packet->data, "SAFE");
	if ( receivedclientnum || multiplayer != CLIENT )
	{
		packetsend->packet->data[4] = clientnum;
	}
	else
	{
		packetsend->packet->data[4] = MAXPLAYERS;
	}
	SDLNet_Write32(packetnum, &packetsend->packet->data[5]);
	packetsend->num = packetnum;
	packetsend->tries = 0;
	if (multiplayer == SERVER
		&& packetUsesActiveMapScope(packet->data, packet->len))
	{
		const WorldInstanceIdentity* identity = worldState.activeIdentity();
		if (!identity)
		{
			SDLNet_FreePacket(packetsend->packet);
			free(packetsend);
			return 0;
		}
		packetsend->mapScoped = true;
		packetsend->mapInstanceRevision = identity->revision;
		stringCopy(
			packetsend->mapInstanceKey,
			identity->key().c_str(),
			sizeof(packetsend->mapInstanceKey),
			identity->key().size() + 1);
	}
	packetnum++;

	node_t* node = list_AddNodeFirst(&safePacketsSent);
	node->element = packetsend;
	node->deconstructor = &packetDeconstructor;

	if ( directConnect )
	{
		return SDLNet_UDP_Send(sock, channel, packetsend->packet);
	}
	else
	{
		if ( LobbyHandler.getP2PType() == LobbyHandler_t::LobbyServiceType::LOBBY_STEAM )
		{
#ifdef STEAMWORKS
			if ( steamIDRemote[hostnum] )
			{
				return SteamNetworking()->SendP2PPacket(*static_cast<CSteamID* >(steamIDRemote[hostnum]), packetsend->packet->data, packetsend->packet->len, k_EP2PSendReliable, 0);
			}
			else
			{
				return 0;
			}
#endif
		}
		else if ( LobbyHandler.getP2PType() == LobbyHandler_t::LobbyServiceType::LOBBY_CROSSPLAY )
		{
#if defined USE_EOS
			EOS.SendMessageP2P(EOS.P2PConnectionInfo.getPeerIdFromIndex(hostnum), packetsend->packet->data, packetsend->packet->len);
			return 0;
#endif
		}
		return 0;
	}
}

int resendPacketSafe(packetsend_t* packet)
{
	if (!packet || !packet->packet)
	{
		return 0;
	}
	if (multiplayer == SERVER && packet->mapScoped)
	{
		const int recipient = packet->hostnum + 1;
		if (recipient <= 0 || recipient >= MAXPLAYERS
			|| client_disconnected[recipient]
			|| !players[recipient]
			|| !scopedReliablePacketCanRetry(
				packet->mapInstanceKey,
				packet->mapInstanceRevision,
				players[recipient]->worldInstance.key().c_str(),
				players[recipient]->worldInstance.revision,
				g_lateJoinTransactions[recipient]
					.mayReceiveLiveSimulation()))
		{
			packet->tries = MAXTRIES;
			return 0;
		}
		g_resendingScopedSafePacket = true;
		const int result = sendPacket(
			packet->sock,
			packet->channel,
			packet->packet,
			packet->hostnum,
			true);
		g_resendingScopedSafePacket = false;
		return result;
	}
	return sendPacket(
		packet->sock,
		packet->channel,
		packet->packet,
		packet->hostnum,
		true);
}

/*-------------------------------------------------------------------------------

	power

	A simple power function designed to work only with integers.

-------------------------------------------------------------------------------*/

int power(int a, int b)
{
	int c, result = 1;
	for ( c = 0; c < b; c++ )
	{
		result *= a;
	}
	return result;
}

/*-------------------------------------------------------------------------------

messageLocalPlayers

Support function, messages all local players with the message "message"

-------------------------------------------------------------------------------*/

bool messageLocalPlayers(Uint32 type, char const * const message, ...)
{
	char str[Player::MessageZone_t::ADD_MESSAGE_BUFFER_LENGTH] = { 0 };

	va_list argptr;
	va_start(argptr, message);
	vsnprintf(str, Player::MessageZone_t::ADD_MESSAGE_BUFFER_LENGTH - 1, message, argptr);
	va_end(argptr);

    bool result = true;
	for ( int player = 0; player < MAXPLAYERS; ++player )
	{
		if ( players[player]->isLocalPlayer() )
		{
			result = messagePlayerColor(player, type, 0xFFFFFFFF, str) ? result : false;
		}
	}

	return result;
}

/*-------------------------------------------------------------------------------

	messagePlayer

	Support function, messages the player number given by "player" with the
	message "message"

-------------------------------------------------------------------------------*/

bool messagePlayer(int player, Uint32 type, char const * const message, ...)
{
	if ( player < 0 || player >= MAXPLAYERS )
	{
		return false;
	}
	char str[Player::MessageZone_t::ADD_MESSAGE_BUFFER_LENGTH] = { 0 };

	va_list argptr;
	va_start( argptr, message );
	vsnprintf( str, Player::MessageZone_t::ADD_MESSAGE_BUFFER_LENGTH - 1, message, argptr );
	va_end( argptr );

	strncpy(str, messageSanitizePercentSign(str, nullptr).c_str(), Player::MessageZone_t::ADD_MESSAGE_BUFFER_LENGTH - 1);
	str[Player::MessageZone_t::ADD_MESSAGE_BUFFER_LENGTH - 1] = '\0';

	return messagePlayerColor(player, type, 0xFFFFFFFF, str);
}

/*-------------------------------------------------------------------------------

messageLocalPlayersColor

Messages all local players with the message "message"
and color "color"

-------------------------------------------------------------------------------*/

bool messageLocalPlayersColor(Uint32 color, Uint32 type, char const * const message, ...)
{
	char str[Player::MessageZone_t::ADD_MESSAGE_BUFFER_LENGTH] = { 0 };

	va_list argptr;
	va_start(argptr, message);
	vsnprintf(str, Player::MessageZone_t::ADD_MESSAGE_BUFFER_LENGTH - 1, message, argptr);
	va_end(argptr);

    bool result = true;
	for ( int player = 0; player < MAXPLAYERS; ++player )
	{
		if ( players[player]->isLocalPlayer() )
		{
			result = messagePlayerColor(player, type, color, str) ? result : false;
		}
	}

	return result;
}

/*-------------------------------------------------------------------------------

	messagePlayerColor

	Messages the player number given by "player" with the message "message"
	and color "color"

-------------------------------------------------------------------------------*/

bool messagePlayerColor(int player, Uint32 type, Uint32 color, char const * const message, ...)
{
	char str[Player::MessageZone_t::ADD_MESSAGE_BUFFER_LENGTH] = { 0 };
	va_list argptr;

	if ( message == NULL )
	{
		return false;
	}
	if ( player < 0 || player >= MAXPLAYERS )
	{
		return false;
	}

	// format the content
	va_start( argptr, message );
	vsnprintf( str, Player::MessageZone_t::ADD_MESSAGE_BUFFER_LENGTH - 1, message, argptr );
	va_end( argptr );

	// fixes crash when reading config at start of game
	if (!initialized)
	{
		printlog("%s\n", str);
		return true;
	}
    
    // don't bother printing any message if we're not in game, it just clutters the log
    if (intro) {
        return false;
    }

    // if this is for a local player, but we've disabled this message type, don't print it!
    const bool localPlayer = players[player]->isLocalPlayer();

	bool result = false;
	if ( localPlayer )
	{
	    printlog("%s\n", str);
#ifdef NDEBUG
		if (type != MESSAGE_DEBUG) { 
			auto string = newString(&messages, color, completionTime, player, str);
			addMessageToLogWindow(player, string);
		}
#else
		auto string = newString(&messages, color, completionTime, player, str);
		addMessageToLogWindow(player, string);
#endif
	    while ( list_Size(&messages) > MESSAGE_LIST_SIZE_CAP )
	    {
		    list_RemoveNode(messages.first);
	    }
	    if (!disable_messages && (messagesEnabled & type))
	    {
	        players[player]->messageZone.addMessage(color, str);
	        result = true;
	    }
	}
	else if ( multiplayer == SERVER )
	{
		strcpy((char*)net_packet->data, "MSGS");
		SDLNet_Write32(color, &net_packet->data[4]);
		SDLNet_Write32((Uint32)type, &net_packet->data[8]);
		strcpy((char*)(&net_packet->data[12]), str);
		net_packet->address.host = net_clients[player - 1].host;
		net_packet->address.port = net_clients[player - 1].port;
		net_packet->len = 12 + strlen(str) + 1;
		sendPacketSafe(net_sock, -1, net_packet, player - 1);
	}

    // player death messages trigger this achievement
	int c;
	char tempstr[256];
	for ( c = 0; c < MAXPLAYERS; c++ )
	{
		if ( client_disconnected[c] )
		{
			continue;
		}
		snprintf(tempstr, 256, Language::get(697), stats[c]->name);
		if ( !strcmp(str, tempstr) )
		{
			steamAchievementClient(player, "BARONY_ACH_NOT_A_TEAM_PLAYER");
		}
	}

	return result;
}

/*-------------------------------------------------------------------------------

	sendEntityUDP / sendEntityTCP

	Updates given entity data for given client. Server -> client functions

-------------------------------------------------------------------------------*/

void sendEntityTCP(Entity* entity, int c)
{
	// deprecated
}

bool serverPlayerCanReceiveActiveMapUpdates(int playerIndex)
{
	if (!serverPlayerSharesActiveMap(playerIndex))
	{
		return false;
	}
	const LateJoinSnapshotTransaction::Phase phase =
		g_lateJoinTransactions[playerIndex].phase();
	return g_lateJoinTransactions[playerIndex].mayReceiveLiveSimulation()
		|| phase == LateJoinSnapshotTransaction::Phase::Receiving
		|| phase == LateJoinSnapshotTransaction::Phase::Complete;
}

bool serverPlayerCanReceiveGameplayUpdates(int playerIndex)
{
	return playerIndex > 0
		&& playerIndex < MAXPLAYERS
		&& !client_disconnected[playerIndex]
		&& players[playerIndex]
		&& !players[playerIndex]->isLocalPlayer()
		&& g_lateJoinTransactions[playerIndex].mayReceiveLiveSimulation();
}

bool beginServerLateJoinSnapshot(
    int playerIndex,
    Uint32 transferId,
    Uint64 instanceRevision,
    Uint32 chunkCount,
    Uint32 totalBytes
)
{
	if (playerIndex <= 0 || playerIndex >= MAXPLAYERS)
	{
		return false;
	}
	g_lateJoinCatchupBuffers[playerIndex].reset();
	return g_lateJoinTransactions[playerIndex].begin(
		transferId, instanceRevision, chunkCount, totalBytes);
}

LateJoinChunkResult acceptServerLateJoinSnapshotChunk(
    int playerIndex,
    Uint32 transferId,
    Uint64 instanceRevision,
    Uint32 sequence,
    Uint32 payloadBytes,
    Uint32 payloadChecksum
)
{
    if (playerIndex <= 0 || playerIndex >= MAXPLAYERS)
    {
        return LateJoinChunkResult::Rejected;
    }
    return g_lateJoinTransactions[playerIndex].acceptChunk(
        transferId,
        instanceRevision,
        sequence,
        payloadBytes,
        payloadChecksum
    );
}

bool authorizeServerLateJoinPlayer(int playerIndex)
{
    return playerIndex > 0
        && playerIndex < MAXPLAYERS
        && g_lateJoinTransactions[playerIndex].authorize();
}

void resetServerLateJoinPlayer(int playerIndex)
{
    if (playerIndex > 0 && playerIndex < MAXPLAYERS)
    {
        g_lateJoinTransactions[playerIndex].reset();
		g_lateJoinCatchupBuffers[playerIndex].reset();
		g_lateJoinLastProgressTick[playerIndex] = 0;
		g_lateJoinReturningPlayer[playerIndex] = false;
		g_lateJoinClientHandshake[playerIndex] = false;
    }
}

static bool queueLateJoinRecordForPlayer(
    int playerIndex,
    const std::vector<std::uint8_t>& record
)
{
    if (playerIndex <= 0 || playerIndex >= MAXPLAYERS
        || client_disconnected[playerIndex] || !net_packet || !net_sock
        || record.empty() || record.size() > NET_PACKET_SIZE - 9)
    {
        return false;
    }
    memcpy(net_packet->data, record.data(), record.size());
    net_packet->len = static_cast<int>(record.size());
    net_packet->address.host = net_clients[playerIndex - 1].host;
    net_packet->address.port = net_clients[playerIndex - 1].port;
    sendPacketSafe(net_sock, -1, net_packet, playerIndex - 1);
    return true;
}

static void abortServerLateJoinPlayer(int playerIndex, Uint8 reason)
{
	if (playerIndex <= 0 || playerIndex >= MAXPLAYERS)
	{
		return;
	}
	LateJoinProtocol::Abort abort;
	abort.playerIndex = static_cast<std::uint8_t>(playerIndex);
	abort.transferId = g_lateJoinTransactions[playerIndex].transferId();
	abort.instanceRevision =
		g_lateJoinTransactions[playerIndex].instanceRevision();
	abort.reason = reason ? reason : 1;
	queueLateJoinRecordForPlayer(
		playerIndex, LateJoinProtocol::encodeAbort(abort));
	worldState.removePlayer(playerIndex);
	client_disconnected[playerIndex] = true;
	resetServerLateJoinPlayer(playerIndex);
}

static bool startServerLateJoinSnapshotTransfer(int playerIndex)
{
    if (multiplayer != SERVER || playerIndex <= 0
        || playerIndex >= MAXPLAYERS || client_disconnected[playerIndex]
        || !players[playerIndex]
        || !players[playerIndex]->worldInstance.isValid())
    {
        return false;
    }

    const WorldInstanceIdentity* foreground = worldState.activeIdentity();
    const std::string foregroundKey =
        foreground ? foreground->key() : std::string{};
    const std::string destinationKey =
        players[playerIndex]->worldInstance.key();
    if (!worldState.activate(destinationKey))
    {
        printlog(
            "[Late Join] Cannot activate destination '%s' for player %d.",
            destinationKey.c_str(), playerIndex);
        return false;
    }

    std::string snapshot;
    std::string snapshotError;
    const bool captured = serializeAutomatiaPersistentWorldSnapshot(
        std::to_string(uniqueGameKey), snapshot, snapshotError);
    if (!foregroundKey.empty() && foregroundKey != destinationKey
        && !worldState.activate(foregroundKey))
    {
        printlog(
            "[Late Join] Failed to restore foreground '%s' after snapshot capture.",
            foregroundKey.c_str());
        resetServerLateJoinPlayer(playerIndex);
        return false;
    }
    if (!captured)
    {
        printlog(
            "[Late Join] Snapshot capture failed for player %d: %s",
            playerIndex, snapshotError.c_str());
        resetServerLateJoinPlayer(playerIndex);
        return false;
    }

    Uint32 transferId = ++g_lateJoinWireTransferId[playerIndex];
    if (transferId == 0)
    {
        transferId = ++g_lateJoinWireTransferId[playerIndex];
    }
    const Uint32 chunkCount = static_cast<Uint32>(
        (snapshot.size() + LateJoinProtocol::maxChunkPayload - 1)
        / LateJoinProtocol::maxChunkPayload);
    const Uint64 revision = players[playerIndex]->worldInstance.revision;
    const Uint32 checksum = LateJoinProtocol::crc32(
        reinterpret_cast<const std::uint8_t*>(snapshot.data()),
        snapshot.size());
    if (!beginServerLateJoinSnapshot(
            playerIndex, transferId, revision, chunkCount,
            static_cast<Uint32>(snapshot.size())))
    {
        return false;
    }

	// Light sources normally synchronize skill[10] only when their powered
	// state changes. A client that joins after that event would otherwise load
	// the authored default and never learn the current enabled state.
	std::size_t synchronizedLightSources = 0;
	MapInstance* destinationInstance = worldState.find(destinationKey);
	if (destinationInstance && destinationInstance->entities)
	{
		for (node_t* node = destinationInstance->entities->first;
			node; node = node->next)
		{
			Entity* entity = static_cast<Entity*>(node->element);
			if (!entity || entity->behavior != &actLightSource)
			{
				continue;
			}
			std::vector<std::uint8_t> state(13, 0);
			memcpy(state.data(), "ENTS", 4);
			LateJoinProtocol::write32(
				state, 4, static_cast<Uint32>(entity->getUID()));
			state[8] = 10;
			LateJoinProtocol::write32(
				state, 9, static_cast<Uint32>(entity->skill[10]));
			if (!g_lateJoinCatchupBuffers[playerIndex].append(
					state.data(), state.size()))
			{
				printlog(
					"[Late Join] Could not queue light-source state for player %d.",
					playerIndex);
				resetServerLateJoinPlayer(playerIndex);
				return false;
			}
			++synchronizedLightSources;
		}
	}

    LateJoinProtocol::Begin begin;
    begin.transferId = transferId;
    begin.instanceRevision = revision;
    begin.chunkCount = chunkCount;
    begin.totalBytes = static_cast<Uint32>(snapshot.size());
    begin.snapshotChecksum = checksum;
    begin.sessionKey = uniqueGameKey;
    if (!queueLateJoinRecordForPlayer(
            playerIndex, LateJoinProtocol::encodeBegin(begin)))
    {
        resetServerLateJoinPlayer(playerIndex);
        return false;
    }

    for (Uint32 sequence = 0; sequence < chunkCount; ++sequence)
    {
        const std::size_t offset =
            static_cast<std::size_t>(sequence)
            * LateJoinProtocol::maxChunkPayload;
        const std::size_t payloadSize = std::min(
            LateJoinProtocol::maxChunkPayload,
            snapshot.size() - offset);
        LateJoinProtocol::Chunk chunk;
        chunk.transferId = transferId;
        chunk.instanceRevision = revision;
        chunk.sequence = sequence;
        const std::uint8_t* payloadBegin =
            reinterpret_cast<const std::uint8_t*>(snapshot.data() + offset);
        chunk.payload.assign(payloadBegin, payloadBegin + payloadSize);
        const Uint32 chunkChecksum = LateJoinProtocol::crc32(
            chunk.payload.data(), chunk.payload.size());
        if (!queueLateJoinRecordForPlayer(
                playerIndex, LateJoinProtocol::encodeChunk(chunk)))
        {
            resetServerLateJoinPlayer(playerIndex);
            return false;
        }
        const LateJoinChunkResult chunkResult =
            acceptServerLateJoinSnapshotChunk(
                playerIndex, transferId, revision, sequence,
                static_cast<Uint32>(payloadSize), chunkChecksum);
        if (chunkResult == LateJoinChunkResult::Rejected)
        {
            resetServerLateJoinPlayer(playerIndex);
            return false;
        }
    }

    LateJoinProtocol::Complete complete;
    complete.transferId = transferId;
    complete.instanceRevision = revision;
    complete.chunkCount = chunkCount;
    complete.totalBytes = static_cast<Uint32>(snapshot.size());
    complete.snapshotChecksum = checksum;
    if (!queueLateJoinRecordForPlayer(
            playerIndex, LateJoinProtocol::encodeComplete(complete)))
    {
        resetServerLateJoinPlayer(playerIndex);
        return false;
    }
    printlog(
        "[Late Join] Queued transfer %u for player %d: %u chunk(s), %zu bytes, %zu editor light source state(s), instance '%s' revision %llu.",
        transferId, playerIndex, chunkCount, snapshot.size(),
		synchronizedLightSources,
        destinationKey.c_str(),
        static_cast<unsigned long long>(revision));
    return true;
}

static bool sendServerLateJoinStart(int playerIndex)
{
    if (playerIndex <= 0 || playerIndex >= MAXPLAYERS
        || !players[playerIndex]
        || !players[playerIndex]->worldInstance.isValid())
    {
        return false;
    }
    const WorldInstanceIdentity& identity =
        players[playerIndex]->worldInstance;
    const MapInstance* instance = worldState.find(identity.key());
    if (!instance || !instance->loadedMap
        || identity.mapFile.empty() || identity.mapFile.size() > 255)
    {
        return false;
    }
    const std::size_t metadataOffset = 19 + identity.mapFile.size();
    std::vector<std::uint8_t> record(metadataOffset + 9, 0);
    memcpy(record.data(), "STRT", 4);
    LateJoinProtocol::write32(record, 4, svFlags);
    LateJoinProtocol::write32(record, 8, uniqueGameKey);
    record[12] = g_lateJoinReturningPlayer[playerIndex] ? 1 : 0;
    LateJoinProtocol::write32(record, 13, uniqueLobbyKey);
    record[17] = 1;
    record[18] = static_cast<std::uint8_t>(identity.mapFile.size());
    memcpy(record.data() + 19, identity.mapFile.data(), identity.mapFile.size());
    LateJoinProtocol::write32(
        record, metadataOffset, static_cast<Uint32>(instance->dungeonLevel));
    LateJoinProtocol::write32(record, metadataOffset + 4, instance->mapSeed);
    record[metadataOffset + 8] = instance->secretLevel ? 1 : 0;
    return queueLateJoinRecordForPlayer(playerIndex, record);
}

static bool sendServerLateJoinCatchup(int playerIndex)
{
	if (playerIndex <= 0 || playerIndex >= MAXPLAYERS
		|| g_lateJoinCatchupBuffers[playerIndex].failed())
	{
		return false;
	}
	const std::vector<std::uint8_t> bytes =
		g_lateJoinCatchupBuffers[playerIndex].serialize();
	if (bytes.empty())
	{
		return false;
	}
	const Uint32 transferId = g_lateJoinTransactions[playerIndex].transferId();
	const Uint64 revision =
		g_lateJoinTransactions[playerIndex].instanceRevision();
	const Uint32 chunkCount = static_cast<Uint32>(
		(bytes.size() + LateJoinProtocol::maxChunkPayload - 1)
		/ LateJoinProtocol::maxChunkPayload);
	LateJoinProtocol::Complete metadata;
	metadata.transferId = transferId;
	metadata.instanceRevision = revision;
	metadata.chunkCount = chunkCount;
	metadata.totalBytes = static_cast<Uint32>(bytes.size());
	metadata.snapshotChecksum = LateJoinProtocol::crc32(
		bytes.data(), bytes.size());
	if (!queueLateJoinRecordForPlayer(
			playerIndex, LateJoinProtocol::encodeCatchupBegin(metadata)))
	{
		return false;
	}
	for (Uint32 sequence = 0; sequence < chunkCount; ++sequence)
	{
		const std::size_t offset =
			static_cast<std::size_t>(sequence)
			* LateJoinProtocol::maxChunkPayload;
		const std::size_t payloadSize = std::min(
			LateJoinProtocol::maxChunkPayload, bytes.size() - offset);
		LateJoinProtocol::Chunk chunk;
		chunk.transferId = transferId;
		chunk.instanceRevision = revision;
		chunk.sequence = sequence;
		chunk.payload.assign(
			bytes.begin() + offset, bytes.begin() + offset + payloadSize);
		if (!queueLateJoinRecordForPlayer(
				playerIndex, LateJoinProtocol::encodeCatchupChunk(chunk)))
		{
			return false;
		}
	}
	if (!queueLateJoinRecordForPlayer(
			playerIndex, LateJoinProtocol::encodeCatchupComplete(metadata)))
	{
		return false;
	}
	printlog(
		"[Late Join] Queued %zu catch-up packet(s), %zu serialized bytes for player %d transfer %u.",
		g_lateJoinCatchupBuffers[playerIndex].packetCount(),
		bytes.size(), playerIndex, transferId);
	return true;
}

void sendEntityUDP(Entity* entity, int c, bool guarantee)
{
	int j;

	if ( entity == NULL )
	{
		return;
	}
	if ( c <= 0 || c >= MAXPLAYERS )
	{
		return;
	}
    if ( !serverPlayerCanReceiveActiveMapUpdates(c) )
	{
		return;
	}

	// send entity data to the client
	strcpy((char*)net_packet->data, "ENTU");
	SDLNet_Write32((Uint32)entity->getUID(), &net_packet->data[4]);
	SDLNet_Write16((Uint16)entity->sprite, &net_packet->data[8]);
	SDLNet_Write16((Sint16)(entity->x * 32), &net_packet->data[10]);
	SDLNet_Write16((Sint16)(entity->y * 32), &net_packet->data[12]);
	SDLNet_Write16((Sint16)(entity->z * 32), &net_packet->data[14]);
	net_packet->data[16] = (Sint8)entity->sizex;
	net_packet->data[17] = (Sint8)entity->sizey;
	net_packet->data[18] = (Uint8)(entity->scalex * 128);
	net_packet->data[19] = (Uint8)(entity->scaley * 128);
	net_packet->data[20] = (Uint8)(entity->scalez * 128);
	SDLNet_Write16((Sint16)(entity->yaw * 256), &net_packet->data[21]);
	SDLNet_Write16((Sint16)(entity->pitch * 256), &net_packet->data[23]);
	SDLNet_Write16((Sint16)(entity->roll * 256), &net_packet->data[25]);
	net_packet->data[27] = (Sint8)(entity->focalx * 8);
	net_packet->data[28] = (Sint8)(entity->focaly * 8);
	net_packet->data[29] = (Sint8)(entity->focalz * 8);
	if ( entity->behavior == &actDeathGhost )
	{
		Uint32 flags = entity->skill[2];
		flags |= ((entity->monsterSpecialState) & 0xFF) << 8;
		flags |= ((entity->skill[10]) & 0xFFFF) << 16;
		SDLNet_Write32(flags, &net_packet->data[30]);
	}
	else
	{
		SDLNet_Write32(entity->skill[2], &net_packet->data[30]);
	}
	net_packet->data[34] = 0;
	net_packet->data[35] = 0;
	for (j = 0; j < 16; j++)
	{
		if ( entity->flags[j] )
		{
			net_packet->data[34 + j / 8] |= power(2, j - (j / 8) * 8);
		}
	}
	SDLNet_Write32((Uint32)ticks, &net_packet->data[36]);
	SDLNet_Write16((Sint16)(entity->vel_x * 32), &net_packet->data[40]);
	SDLNet_Write16((Sint16)(entity->vel_y * 32), &net_packet->data[42]);
	SDLNet_Write16((Sint16)(entity->vel_z * 32), &net_packet->data[44]);
	net_packet->data[46] = 0;
	for ( j = 0; j < 8; j++ )
	{
		if ( entity->flags[j + 16] )
		{
			net_packet->data[46 + j / 8] |= power(2, j - (j / 8) * 8);
		}
	}
	net_packet->data[kEntityArchetypeOffset] =
		networkEntityArchetype(entity);
	net_packet->address.host = net_clients[c - 1].host;
	net_packet->address.port = net_clients[c - 1].port;
	net_packet->len = ENTITY_PACKET_LENGTH;

	// sometimes you want more insurance that the entity update arrives
	if ( guarantee )
	{
		sendPacketSafe(net_sock, -1, net_packet, c - 1);
	}
	else
	{
		sendPacket(net_sock, -1, net_packet, c - 1);
	}
	if ( entity->clientsHaveItsStats )
	{
		entity->serverUpdateEffectsForEntity(false);
	}
}

/*-------------------------------------------------------------------------------

	sendMapSeedTCP

	Sends the seed necessary to generate the next map

-------------------------------------------------------------------------------*/

void sendMapSeedTCP(int c)
{
	// deprecated
}

/*-------------------------------------------------------------------------------

	sendMapTCP

	Sends all map data to clients

-------------------------------------------------------------------------------*/

void sendMapTCP(int c)
{
	// deprecated
}

/*-------------------------------------------------------------------------------

	serverUpdateBodypartIDs

	Updates the uid numbers of all the given bodyparts for the given entity

-------------------------------------------------------------------------------*/

void serverUpdateBodypartIDs(Entity* entity)
{
	int c;
	if ( multiplayer != SERVER || !entity )
	{
		return;
	}
	const bool monsterBodyparts = entity->behavior == &actMonster;
	const std::size_t packetLength = bodypartIdPacketLength(
		list_Size(&entity->children), monsterBodyparts);
	if (packetLength > NET_PACKET_SIZE)
	{
		printlog(
			"[NET]: refusing oversized BDYI packet for UID %u (%zu bytes).",
			entity->getUID(),
			packetLength);
		return;
	}
	int childIndex = 0;
	for (node_t* node = entity->children.first;
		node;
		node = node->next, ++childIndex)
	{
		if (childIndex < (monsterBodyparts ? 2 : 1))
		{
			continue;
		}
		if (!node->element)
		{
			printlog(
				"[NET]: refusing BDYI packet for UID %u with a null transmitted child.",
				entity->getUID());
			return;
		}
	}
	for ( c = 1; c < MAXPLAYERS; c++ )
	{
        if ( !serverPlayerCanReceiveActiveMapUpdates(c) )
		{
			continue;
		}
		strcpy((char*)net_packet->data, "BDYI");
		SDLNet_Write32(entity->getUID(), &net_packet->data[4]);
		node_t* node;
		int i;
		for ( i = 0, node = entity->children.first; node != NULL; node = node->next, i++ )
		{
			if ( i < 1 || (i < 2 && entity->behavior == &actMonster) )
			{
				continue;
			}
			Entity* tempEntity = (Entity*)node->element;
			if ( entity->behavior == &actMonster )
			{
				SDLNet_Write32(tempEntity->getUID(), &net_packet->data[8 + 4 * (i - 2)]);
			}
			else
			{
				SDLNet_Write32(tempEntity->getUID(), &net_packet->data[8 + 4 * (i - 1)]);
			}
		}
		net_packet->address.host = net_clients[c - 1].host;
		net_packet->address.port = net_clients[c - 1].port;
		net_packet->len = static_cast<int>(packetLength);
		sendPacketSafe(net_sock, -1, net_packet, c - 1);
	}
}

/*-------------------------------------------------------------------------------

	serverUpdateEntityBodypart

	Updates the given bodypart of the given entity for all clients

-------------------------------------------------------------------------------*/

//int numPlayerBodypartUpdates = 0;
//int numMonsterBodypartUpdates = 0;
//Uint32 lastbodypartTick = 0;

void serverUpdateEntityBodypart(Entity* entity, int bodypart)
{
	int c;
	if ( multiplayer != SERVER || !entity )
	{
		return;
	}
	for ( c = 1; c < MAXPLAYERS; c++ )
	{
        if ( !serverPlayerCanReceiveActiveMapUpdates(c) )
		{
			continue;
		}
		strcpy((char*)net_packet->data, "ENTB");
		SDLNet_Write32(entity->getUID(), &net_packet->data[4]);
		net_packet->data[8] = bodypart;
		node_t* node = list_Node(&entity->children, bodypart);
		if ( !node || !node->element )
		{
			continue;
		}
		Entity* tempEntity = (Entity*)node->element;
		SDLNet_Write32(tempEntity->sprite, &net_packet->data[9]);
		net_packet->data[13] = (tempEntity->flags[INVISIBLE] ? 1 : 0);
		net_packet->data[13] |= (tempEntity->flags[INVISIBLE_DITHER] ? (1 << 1) : 0);
		net_packet->address.host = net_clients[c - 1].host;
		net_packet->address.port = net_clients[c - 1].port;
		net_packet->len = 14;
		sendPacketSafe(net_sock, -1, net_packet, c - 1);
	}
	//if ( entity->behavior == &actPlayer )
	//{
	//	++numPlayerBodypartUpdates;
	//}
	//else if ( entity->behavior == &actMonster )
	//{
	//	++numMonsterBodypartUpdates;
	//}
	//if ( lastbodypartTick == 0 )
	//{
	//	lastbodypartTick = ticks;
	//}
	//if ( ticks - lastbodypartTick >= 250 )
	//{
	//	messagePlayer(0, "Bodypart updates (%ds) players: %d, monster: %d", (ticks - lastbodypartTick) / 50, numPlayerBodypartUpdates, numMonsterBodypartUpdates);
	//	lastbodypartTick = 0;
	//	numMonsterBodypartUpdates = 0;
	//	numPlayerBodypartUpdates = 0;
	//}
}

/*-------------------------------------------------------------------------------

	serverUpdateEntitySprite

	Updates the given entity's sprite for all clients

-------------------------------------------------------------------------------*/

void serverUpdateEntitySprite(Entity* entity)
{
	int c;
	if ( multiplayer != SERVER )
	{
		return;
	}
	for ( c = 1; c < MAXPLAYERS; c++ )
	{
        if ( !serverPlayerCanReceiveActiveMapUpdates(c) )
		{
			continue;
		}
		strcpy((char*)net_packet->data, "ENTA");
		SDLNet_Write32(entity->getUID(), &net_packet->data[4]);
		SDLNet_Write32(entity->sprite, &net_packet->data[8]);
		net_packet->address.host = net_clients[c - 1].host;
		net_packet->address.port = net_clients[c - 1].port;
		net_packet->len = 12;
		sendPacketSafe(net_sock, -1, net_packet, c - 1);
	}
}

/*-------------------------------------------------------------------------------

	serverUpdateEntitySkill

	Updates a specific entity skill for all clients

-------------------------------------------------------------------------------*/

void serverUpdateEntitySkill(Entity* entity, int skill)
{
	int c;
	if ( multiplayer != SERVER )
	{
		return;
	}
	for ( c = 1; c < MAXPLAYERS; c++ )
	{
        if ( !serverPlayerCanReceiveActiveMapUpdates(c) )
		{
			continue;
		}
		strcpy((char*)net_packet->data, "ENTS");
		SDLNet_Write32(entity->getUID(), &net_packet->data[4]);
		net_packet->data[8] = skill;
		SDLNet_Write32(entity->skill[skill], &net_packet->data[9]);
		net_packet->address.host = net_clients[c - 1].host;
		net_packet->address.port = net_clients[c - 1].port;
		net_packet->len = 13;
		sendPacketSafe(net_sock, -1, net_packet, c - 1);
	}
}

/*-------------------------------------------------------------------------------

serverUpdateEntitySkill

Updates a specific entity skill for all clients

-------------------------------------------------------------------------------*/

void serverUpdateEntityStatFlag(Entity* entity, int flag)
{
	int c;
	if ( multiplayer != SERVER )
	{
		return;
	}
	if ( !entity->getStats() )
	{
		return;
	}
	for ( c = 1; c < MAXPLAYERS; c++ )
	{
        if ( !serverPlayerCanReceiveActiveMapUpdates(c) )
		{
			continue;
		}
		strcpy((char*)net_packet->data, "ENSF");
		SDLNet_Write32(entity->getUID(), &net_packet->data[4]);
		net_packet->data[8] = flag;
		SDLNet_Write32(entity->getStats()->MISC_FLAGS[flag], &net_packet->data[9]);
		net_packet->address.host = net_clients[c - 1].host;
		net_packet->address.port = net_clients[c - 1].port;
		net_packet->len = 13;
		sendPacketSafe(net_sock, -1, net_packet, c - 1);
	}
}

/*-------------------------------------------------------------------------------

serverUpdateEntityFSkill

Updates a specific entity fskill for all clients

-------------------------------------------------------------------------------*/

void serverUpdateEntityFSkill(Entity* entity, int fskill)
{
	int c;
	if ( multiplayer != SERVER )
	{
		return;
	}
	for ( c = 1; c < MAXPLAYERS; c++ )
	{
        if ( !serverPlayerCanReceiveActiveMapUpdates(c) )
		{
			continue;
		}
		strcpy((char*)net_packet->data, "ENFS");
		SDLNet_Write32(entity->getUID(), &net_packet->data[4]);
		net_packet->data[8] = fskill;
		SDLNet_Write16(static_cast<Sint16>(entity->fskill[fskill] * 256), &net_packet->data[9]);
		net_packet->address.host = net_clients[c - 1].host;
		net_packet->address.port = net_clients[c - 1].port;
		net_packet->len = 11;
		sendPacketSafe(net_sock, -1, net_packet, c - 1);
	}
}

/*-------------------------------------------------------------------------------

serverSpawnMiscParticles

Spawns misc particle effects for all clients

-------------------------------------------------------------------------------*/

void serverSpawnMiscParticles(Entity* entity, int particleType, int particleSprite, Uint32 optionalUid, Uint32 duration, Uint32 optionalData)
{
	int c;
	if ( multiplayer != SERVER )
	{
		return;
	}
	for ( c = 1; c < MAXPLAYERS; c++ )
	{
        if ( !serverPlayerCanReceiveActiveMapUpdates(c) )
		{
			continue;
		}
		strcpy((char*)net_packet->data, "SPPE");
		SDLNet_Write32(entity->getUID(), &net_packet->data[4]);
		net_packet->data[8] = particleType;
		SDLNet_Write16(particleSprite, &net_packet->data[9]);
		SDLNet_Write32(optionalUid, &net_packet->data[11]);
		SDLNet_Write32(duration, &net_packet->data[15]);
		SDLNet_Write32(optionalData, &net_packet->data[19]);
		net_packet->len = 23;
		net_packet->address.host = net_clients[c - 1].host;
		net_packet->address.port = net_clients[c - 1].port;
		sendPacketSafe(net_sock, -1, net_packet, c - 1);
	}
}

/*-------------------------------------------------------------------------------

serverSpawnMiscParticlesAtLocation

Spawns misc particle effects for all clients at given coordinates.

-------------------------------------------------------------------------------*/

void serverSpawnMiscParticlesAtLocation(Sint16 x, Sint16 y, Sint16 z, int particleType, 
	int particleSprite, Uint32 duration, Uint32 optionalData, Uint32 optionalUID)
{
	int c;
	if ( multiplayer != SERVER )
	{
		return;
	}
	for ( c = 1; c < MAXPLAYERS; c++ )
	{
        if ( !serverPlayerCanReceiveActiveMapUpdates(c) )
		{
			continue;
		}
		strcpy((char*)net_packet->data, "SPPL");
		SDLNet_Write16(x, &net_packet->data[4]);
		SDLNet_Write16(y, &net_packet->data[6]);
		SDLNet_Write16(z, &net_packet->data[8]);
		net_packet->data[10] = particleType;
		SDLNet_Write16(particleSprite, &net_packet->data[11]);
		SDLNet_Write32(duration, &net_packet->data[13]);
		SDLNet_Write32(optionalData, &net_packet->data[17]);
		SDLNet_Write32(optionalUID, &net_packet->data[21]);
		net_packet->len = 25;
		net_packet->address.host = net_clients[c - 1].host;
		net_packet->address.port = net_clients[c - 1].port;
		sendPacketSafe(net_sock, -1, net_packet, c - 1);
	}
}

/*-------------------------------------------------------------------------------

	serverUpdateEntityFlag

	Updates a specific entity flag for all clients

-------------------------------------------------------------------------------*/

void serverUpdateEntityFlag(Entity* entity, int flag)
{
	int c;
	if ( multiplayer != SERVER )
	{
		return;
	}
	for ( c = 1; c < MAXPLAYERS; c++ )
	{
        if ( !serverPlayerCanReceiveActiveMapUpdates(c) )
		{
			continue;
		}
		strcpy((char*)net_packet->data, "ENTF");
		SDLNet_Write32(entity->getUID(), &net_packet->data[4]);
		net_packet->data[8] = flag;
		net_packet->data[9] = entity->flags[flag];
		net_packet->address.host = net_clients[c - 1].host;
		net_packet->address.port = net_clients[c - 1].port;
		net_packet->len = 10;
		sendPacketSafe(net_sock, -1, net_packet, c - 1);
	}
}

void serverUpdateMapTileFlag(Sint16 x, Sint16 y, int layer, Uint32 flagSet, Uint32 flagRemove)
{
	int c;
	if ( multiplayer != SERVER )
	{
		return;
	}
	for ( c = 1; c < MAXPLAYERS; c++ )
	{
        if ( !serverPlayerCanReceiveActiveMapUpdates(c) )
		{
			continue;
		}
		strcpy((char*)net_packet->data, "MAPT");
		SDLNet_Write16(x, &net_packet->data[4]);
		SDLNet_Write16(y, &net_packet->data[6]);
		SDLNet_Write32(flagSet, &net_packet->data[8]);
		SDLNet_Write32(flagRemove, &net_packet->data[12]);
		net_packet->data[16] = layer;
		net_packet->address.host = net_clients[c - 1].host;
		net_packet->address.port = net_clients[c - 1].port;
		net_packet->len = 17;
		sendPacketSafe(net_sock, -1, net_packet, c - 1);
	}
}

/*-------------------------------------------------------------------------------

	serverUpdateEffects

	Updates the status of the EFFECTS variables (blindness, drunkenness, etc.)
	for the specified client.

-------------------------------------------------------------------------------*/

void serverUpdateEffects(int player)
{
	int j;

	if ( multiplayer != SERVER || clientnum == player )
	{
		return;
	}
	if ( player <= 0 || player >= MAXPLAYERS )
	{
		return;
	}
	if ( client_disconnected[player] == true || !players[player] || players[player]->isLocalPlayer() )
	{
		return;
	}

	strcpy((char*)net_packet->data, "UPEF");
	int numBytes = NUMEFFECTS / 8;
	for ( int i = 0; i < numBytes; ++i )
	{
		net_packet->data[4 + i] = 0;
		net_packet->data[4 + numBytes + i] = 0;
	}

	std::vector<std::pair<Uint8, Uint8>> effectStrengths;
	for (j = 0; j < NUMEFFECTS; j++)
	{
		Uint8 effectValue = stats[player]->getEffectActive(j);
		if ( effectValue > 0 )
		{
			net_packet->data[4 + j / 8] |= power(2, j - (j / 8) * 8);
			if ( effectValue > 1 )
			{
				// effect index, then value
				effectStrengths.push_back(std::make_pair(static_cast<Uint8>(j & 0xFF), effectValue));
			}
		}
		if ( stats[player]->EFFECTS_TIMERS[j] < TICKS_PER_SECOND * 5 && stats[player]->EFFECTS_TIMERS[j] > 0 )
		{
			// use these bits to denote if duration is low.
			net_packet->data[4 + numBytes + j / 8] |= power(2, j - (j / 8) * 8);
		}
	}
	
	net_packet->data[4 + numBytes * 2] = (Uint8)effectStrengths.size();
	net_packet->len = 4 + numBytes * 2 + 1;
	for ( auto& pair : effectStrengths )
	{
		if ( net_packet->len + 1 >= NET_PACKET_SIZE )
		{
			// no more room
			break;
		}
		net_packet->data[net_packet->len + 0] = pair.first;
		net_packet->data[net_packet->len + 1] = pair.second;
		net_packet->len += 2;
	}

	net_packet->address.host = net_clients[player - 1].host;
	net_packet->address.port = net_clients[player - 1].port;
	sendPacketSafe(net_sock, -1, net_packet, player - 1);
}

/*-------------------------------------------------------------------------------

	serverUpdateHunger

	Updates the HUNGER variable for the specified client

-------------------------------------------------------------------------------*/

void serverUpdateHunger(int player)
{
	if ( multiplayer != SERVER || clientnum == player )
	{
		return;
	}
	if ( player <= 0 || player >= MAXPLAYERS )
	{
		return;
	}
	if ( client_disconnected[player] == true || !players[player] || players[player]->isLocalPlayer() )
	{
		return;
	}

	strcpy((char*)net_packet->data, "HNGR");
	SDLNet_Write32(stats[player]->HUNGER, &net_packet->data[4]);
	net_packet->address.host = net_clients[player - 1].host;
	net_packet->address.port = net_clients[player - 1].port;
	net_packet->len = 8;
	sendPacketSafe(net_sock, -1, net_packet, player - 1);
}

/*-------------------------------------------------------------------------------

serverUpdateSexChange

Updates all clients on specified player's sex

-------------------------------------------------------------------------------*/

void serverUpdateSexChange(int player)
{
	if ( multiplayer != SERVER || !stats[player] )
	{
		return;
	}

	for ( int c = 1; c < MAXPLAYERS; c++ )
	{
		if ( client_disconnected[c] || !players[c] || players[c]->isLocalPlayer() )
		{
			continue;
		}
		strcpy((char*)net_packet->data, "SEXU");
		net_packet->data[4] = static_cast<Uint8>(player);
		net_packet->data[5] = static_cast<Uint8>(stats[player]->sex);
		net_packet->address.host = net_clients[c - 1].host;
		net_packet->address.port = net_clients[c - 1].port;
		net_packet->len = 6;
		sendPacketSafe(net_sock, -1, net_packet, c - 1);
	}
}

/*-------------------------------------------------------------------------------

serverUpdatePlayerStats

Updates all player current HP/MP for clients

-------------------------------------------------------------------------------*/

void serverUpdatePlayerStats()
{
	if ( multiplayer != SERVER )
	{
		return;
	}

	constexpr int packetLength =
		4 + 8 * MAXPLAYERS;
	static_assert(
		packetLength <= NET_PACKET_SIZE,
		"NET_PACKET_SIZE is too small for STAT"
	);

	strcpy((char*)net_packet->data, "STAT");

	for ( int i = 0; i < MAXPLAYERS; ++i )
	{
		Uint32 packedHP = 0;
		Uint32 packedMP = 0;

		if ( stats[i] )
		{
			const Uint16 maxHP =
				static_cast<Uint16>(
					std::max(
						0,
						std::min(
							stats[i]->MAXHP,
							65535
						)
					)
				);
			const Uint16 currentHP =
				static_cast<Uint16>(
					std::max(
						0,
						std::min(
							stats[i]->HP,
							65535
						)
					)
				);
			const Uint16 maxMP =
				static_cast<Uint16>(
					std::max(
						0,
						std::min(
							stats[i]->MAXMP,
							65535
						)
					)
				);
			const Uint16 currentMP =
				static_cast<Uint16>(
					std::max(
						0,
						std::min(
							stats[i]->MP,
							65535
						)
					)
				);

			packedHP =
				static_cast<Uint32>(maxHP)
				| (
					static_cast<Uint32>(currentHP)
					<< 16
				);
			packedMP =
				static_cast<Uint32>(maxMP)
				| (
					static_cast<Uint32>(currentMP)
					<< 16
				);
		}

		SDLNet_Write32(
			packedHP,
			&net_packet->data[4 + i * 8]
		);
		SDLNet_Write32(
			packedMP,
			&net_packet->data[8 + i * 8]
		);
	}

	net_packet->len = packetLength;

	for ( int c = 1; c < MAXPLAYERS; ++c )
	{
		if ( !serverPlayerCanReceiveGameplayUpdates(c) )
		{
			continue;
		}

		net_packet->address.host =
			net_clients[c - 1].host;
		net_packet->address.port =
			net_clients[c - 1].port;
		sendPacketSafe(
			net_sock,
			-1,
			net_packet,
			c - 1
		);
	}
}

/*-------------------------------------------------------------------------------

serverUpdatePlayerGameplayStats

Updates given players gameplayStatistics value by given increment.

-------------------------------------------------------------------------------*/
void serverUpdatePlayerGameplayStats(int player, int gameplayStat, int changeval)
{
	if ( player < 0 || player >= MAXPLAYERS )
	{
		return;
	}
	if ( client_disconnected[player] )
	{
		return;
	}
	if ( player == 0 )
	{
		if ( gameplayStat == STATISTICS_TEMPT_FATE )
		{
			if ( gameStatistics[STATISTICS_TEMPT_FATE] == -1 )
			{
				// don't change, completed task.
			}
			else
			{
				if ( changeval == 5 )
				{
					gameStatistics[gameplayStat] = changeval;
				}
				else if ( changeval == 1 && gameStatistics[gameplayStat] > 0 )
				{
					gameStatistics[gameplayStat] = -1;
				}
			}
		}
		else if ( gameplayStat == STATISTICS_FORUM_TROLL )
		{
			if ( changeval == AchievementObserver::FORUM_TROLL_BREAK_WALL )
			{
				int walls = gameStatistics[gameplayStat] & 0xFF;
				walls = std::min(walls + 1, 3);
				gameStatistics[gameplayStat] = gameStatistics[gameplayStat] & 0xFFFFFF00;
				gameStatistics[gameplayStat] |= walls;
			}
			else if ( changeval == AchievementObserver::FORUM_TROLL_RECRUIT_TROLL )
			{
				int trolls = (gameStatistics[gameplayStat] >> 8) & 0xFF;
				trolls = std::min(trolls + 1, 3);
				gameStatistics[gameplayStat] = gameStatistics[gameplayStat] & 0xFFFF00FF;
				gameStatistics[gameplayStat] |= (trolls << 8);
			}
			else if ( changeval == AchievementObserver::FORUM_TROLL_FEAR )
			{
				int fears = (gameStatistics[gameplayStat] >> 16) & 0xFF;
				fears = std::min(fears + 1, 3);
				gameStatistics[gameplayStat] = gameStatistics[gameplayStat] & 0xFF00FFFF;
				gameStatistics[gameplayStat] |= (fears << 16);
			}
		}
		else if ( gameplayStat == STATISTICS_POP_QUIZ_1 || gameplayStat == STATISTICS_POP_QUIZ_2 )
		{
			int spellID = changeval;
			if ( spellID >= 30 )
			{
				spellID -= 30;
				int shifted = (1 << spellID);
				gameStatistics[gameplayStat] |= shifted;
			}
			else
			{
				int shifted = (1 << spellID);
				gameStatistics[gameplayStat] |= shifted;
			}
		}
		else if ( gameplayStat == STATISTICS_FLAVORTOWN )
		{
			gameStatistics[gameplayStat] |= changeval;
		}
		else if ( gameplayStat == STATISTICS_BARDIC_INSPIRATION )
		{
			if ( changeval == 0 )
			{
				gameStatistics[gameplayStat] = 0;
			}
			else
			{
				gameStatistics[gameplayStat] += changeval;
			}
		}
		else if ( gameplayStat == STATISTICS_PARRY_TANK )
		{
			if ( changeval == 0 )
			{
				if ( gameStatistics[gameplayStat] < 20 )
				{
					gameStatistics[gameplayStat] = 0;
				}
			}
			else
			{
				gameStatistics[gameplayStat] += changeval;
			}
		}
		else
		{
			gameStatistics[gameplayStat] += changeval;
		}
	}
	else if ( !players[player]->isLocalPlayer() )
	{
		strcpy((char*)net_packet->data, "GPST");
		SDLNet_Write32(gameplayStat, &net_packet->data[4]);
		SDLNet_Write32(changeval, &net_packet->data[8]);
		net_packet->address.host = net_clients[player - 1].host;
		net_packet->address.port = net_clients[player - 1].port;
		net_packet->len = 12;
		sendPacketSafe(net_sock, -1, net_packet, player - 1);
	}
	//messagePlayer(clientnum, MESSAGE_DEBUG, "[DEBUG]: sent: %d, %d: val %d", gameplayStat, changeval, gameStatistics[gameplayStat]);
}

void serverUpdatePlayerConduct(int player, int conduct, int value)
{
	if ( player <= 0 || player >= MAXPLAYERS )
	{
		return;
	}
	if ( client_disconnected[player] || !players[player] || players[player]->isLocalPlayer() )
	{
		return;
	}
	strcpy((char*)net_packet->data, "COND");
	SDLNet_Write16(conduct, &net_packet->data[4]);
	SDLNet_Write16(value, &net_packet->data[6]);
	net_packet->address.host = net_clients[player - 1].host;
	net_packet->address.port = net_clients[player - 1].port;
	net_packet->len = 8;
	sendPacketSafe(net_sock, -1, net_packet, player - 1);
}

/*-------------------------------------------------------------------------------

serverUpdatePlayerLVL

Updates all player current LVL for clients

-------------------------------------------------------------------------------*/

void serverUpdatePlayerLVL()
{
	if ( multiplayer != SERVER )
	{
		return;
	}

	static_assert(
		4 + MAXPLAYERS <= NET_PACKET_SIZE,
		"NET_PACKET_SIZE is too small for UPLV"
	);

	strcpy((char*)net_packet->data, "UPLV");
	for ( int i = 0; i < MAXPLAYERS; ++i )
	{
		net_packet->data[4 + i] =
			stats[i]
				? static_cast<Uint8>(
					std::max(
						0,
						std::min(
							stats[i]->LVL,
							255
						)
					)
				)
				: 0;
	}
	net_packet->len = 4 + MAXPLAYERS;

	for ( int c = 1; c < MAXPLAYERS; ++c )
	{
		if ( client_disconnected[c]
			|| !players[c]
			|| players[c]->isLocalPlayer() )
		{
			continue;
		}

		net_packet->address.host =
			net_clients[c - 1].host;
		net_packet->address.port =
			net_clients[c - 1].port;
		sendPacketSafe(
			net_sock,
			-1,
			net_packet,
			c - 1
		);
	}
}

void serverRemoveClientFollower(int player, Uint32 uidToRemove)
{
	if ( multiplayer != SERVER || player <= 0 || player >= MAXPLAYERS )
	{
		return;
	}
	if ( client_disconnected[player] || !players[player] || players[player]->isLocalPlayer() )
	{
		return;
	}

	strcpy((char*)net_packet->data, "LDEL");
	SDLNet_Write32(uidToRemove, &net_packet->data[4]);
	net_packet->address.host = net_clients[player - 1].host;
	net_packet->address.port = net_clients[player - 1].port;
	net_packet->len = 8;
	sendPacketSafe(net_sock, -1, net_packet, player - 1);
}

void serverSendItemToPickupAndEquip(int player, Item* item)
{
	if ( multiplayer != SERVER || player <= 0 || player >= MAXPLAYERS )
	{
		return;
	}
	if ( client_disconnected[player] || !players[player] || players[player]->isLocalPlayer() )
	{
		return;
	}
	if ( !item )
	{
		return;
	}

	// Send the ordinary item fields first. Vanilla packets remain exactly
	// 29 bytes for compatibility with older Barony clients.
	strcpy((char*)net_packet->data, "ITEQ");
	SDLNet_Write32((Uint32)item->type, &net_packet->data[4]);
	SDLNet_Write32((Uint32)item->status, &net_packet->data[8]);
	SDLNet_Write32((Uint32)item->beatitude, &net_packet->data[12]);
	SDLNet_Write32((Uint32)item->count, &net_packet->data[16]);
	SDLNet_Write32((Uint32)item->appearance, &net_packet->data[20]);
	SDLNet_Write32((Uint32)item->ownerUid, &net_packet->data[24]);
	net_packet->data[28] = item->identified;
	net_packet->len = 29;

#ifdef SAM_FRAMEWORK_ENABLED
	const int runtimeType =
		static_cast<int>(item->type);
	if ( SAMItemRegistryFoundation::
		isRegisteredRuntimeItemId(runtimeType) )
	{
		const std::string& stableId =
			SAMItemRegistryFoundation::
				stableIdForRuntimeId(runtimeType);

		if ( stableId.empty() )
		{
			printlog(
				"[S.A.M] Refusing ITEQ custom item runtime %d: no stable id.\n",
				runtimeType
			);
			return;
		}

		const int available =
			NET_PACKET_SIZE - 30;
		if ( available <= 0
			|| static_cast<int>(stableId.size()) > available )
		{
			printlog(
				"[S.A.M] Refusing ITEQ custom item [%s]: stable id is too long.\n",
				stableId.c_str()
			);
			return;
		}

		memcpy(
			&net_packet->data[29],
			stableId.c_str(),
			stableId.size()
		);
		net_packet->data[29 + stableId.size()] = '\0';
		net_packet->len =
			30 + static_cast<int>(stableId.size());

		printlog(
			"[S.A.M] Sending ITEQ custom item [%s] runtime %d to player %d.\n",
			stableId.c_str(),
			runtimeType,
			player
		);
	}
#endif

	net_packet->address.host = net_clients[player - 1].host;
	net_packet->address.port = net_clients[player - 1].port;
	sendPacketSafe(net_sock, -1, net_packet, player - 1);
}

void serverUpdateAllyStat(int player, Uint32 uidToUpdate, int LVL, int HP, int MAXHP, int type)
{
	if ( multiplayer != SERVER || player <= 0 || player >= MAXPLAYERS )
	{
		return;
	}
	if ( client_disconnected[player] || !players[player] || players[player]->isLocalPlayer() )
	{
		return;
	}

	strcpy((char*)net_packet->data, "NPCI");
	SDLNet_Write32(uidToUpdate, &net_packet->data[4]);
	net_packet->data[8] = static_cast<Uint8>(LVL);
	SDLNet_Write16(HP, &net_packet->data[9]);
	SDLNet_Write16(MAXHP, &net_packet->data[11]);
	net_packet->data[13] = static_cast<Uint8>(type);
	net_packet->address.host = net_clients[player - 1].host;
	net_packet->address.port = net_clients[player - 1].port;
	net_packet->len = 14;
	sendPacketSafe(net_sock, -1, net_packet, player - 1);
}

void serverUpdatePlayerSummonStrength(int player)
{
	if ( multiplayer != SERVER )
	{
		return;
	}
	if ( player <= 0 || player >= MAXPLAYERS )
	{
		return;
	}
	if ( client_disconnected[player]
		|| !stats[player]
		|| !players[player]
		|| players[player]->isLocalPlayer() )
	{
		return;
	}

	strcpy((char*)net_packet->data, "SUMS");
	SDLNet_Write32(stats[player]->playerSummonLVLHP, &net_packet->data[4]);
	SDLNet_Write32(stats[player]->playerSummonSTRDEXCONINT, &net_packet->data[8]);
	SDLNet_Write32(stats[player]->playerSummonPERCHR, &net_packet->data[12]);
	SDLNet_Write32(stats[player]->playerSummon2LVLHP, &net_packet->data[16]);
	SDLNet_Write32(stats[player]->playerSummon2STRDEXCONINT, &net_packet->data[20]);
	SDLNet_Write32(stats[player]->playerSummon2PERCHR, &net_packet->data[24]);
	net_packet->address.host = net_clients[player - 1].host;
	net_packet->address.port = net_clients[player - 1].port;
	net_packet->len = 28;
	sendPacketSafe(net_sock, -1, net_packet, player - 1);
}

void serverUpdateAllyHP(int player, Uint32 uidToUpdate, int HP, int MAXHP, bool guarantee)
{
	if ( multiplayer != SERVER )
	{
		return;
	}
	if ( player <= 0 || player >= MAXPLAYERS )
	{
		return;
	}
	if ( client_disconnected[player] || !players[player] || players[player]->isLocalPlayer() )
	{
		return;
	}

	strcpy((char*)net_packet->data, "NPCU");
	SDLNet_Write32(uidToUpdate, &net_packet->data[4]);
	SDLNet_Write16(HP, &net_packet->data[8]);
	SDLNet_Write16(MAXHP, &net_packet->data[10]);
	net_packet->address.host = net_clients[player - 1].host;
	net_packet->address.port = net_clients[player - 1].port;
	net_packet->len = 12;
	if ( !guarantee )
	{
		sendPacket(net_sock, -1, net_packet, player - 1);
	}
	else
	{
		sendPacketSafe(net_sock, -1, net_packet, player - 1);
	}
}

void sendMinimapPing(Uint8 player, Uint8 x, Uint8 y, Uint8 pingType, bool radius)
{
	if ( multiplayer == CLIENT )
	{
		// send to host to relay info.
		strcpy((char*)net_packet->data, "PMAP"); 
		net_packet->data[4] = player;
		net_packet->data[5] = x;
		net_packet->data[6] = y;
		net_packet->data[7] = pingType;
		net_packet->data[8] = radius ? 1 : 0;

		net_packet->address.host = net_server.host;
		net_packet->address.port = net_server.port;
		net_packet->len = 9;
		sendPacket(net_sock, -1, net_packet, 0);
	}
	else
	{
		for ( int c = 0; c < MAXPLAYERS; c++ )
		{
			if ( client_disconnected[c] )
			{
				continue;
			}
			if ( players[c]->isLocalPlayer() )
			{
				minimapPingAdd(player, c, MinimapPing(ticks, player, x, y, radius, (MinimapPing::PingType)pingType));
				continue;
			}

			if ( multiplayer == SERVER )
			{
				// send to all clients.
				strcpy((char*)net_packet->data, "PMAP");
				net_packet->data[4] = player;
				net_packet->data[5] = x;
				net_packet->data[6] = y;
				net_packet->data[7] = pingType;
				net_packet->data[8] = radius ? 1 : 0;

				net_packet->address.host = net_clients[c - 1].host;
				net_packet->address.port = net_clients[c - 1].port;
				net_packet->len = 9;
				sendPacketSafe(net_sock, -1, net_packet, c - 1);
			}
		}
	}
}

void sendAllyCommandClient(int player, Uint32 uid, int command, Uint8 x, Uint8 y, Uint32 targetUid)
{
	if ( multiplayer != CLIENT )
	{
		return;
	}
	//messagePlayer(clientnum, "%d", targetUid);

	// send to host.
	strcpy((char*)net_packet->data, "ALLY");
	net_packet->data[4] = player;
	net_packet->data[5] = command;
	net_packet->data[6] = x;
	net_packet->data[7] = y;
	SDLNet_Write32(uid, &net_packet->data[8]);
	net_packet->len = 12;
	if ( targetUid != 0 )
	{
		SDLNet_Write32(targetUid, &net_packet->data[12]);
		net_packet->len = 16;
	}
	net_packet->address.host = net_server.host;
	net_packet->address.port = net_server.port;
	sendPacket(net_sock, -1, net_packet, 0);
}

NetworkingLobbyJoinRequestResult lobbyPlayerJoinRequest(
	int& outResult,
	bool lockedSlots[MAXPLAYERS],
	bool& outUseChunkedHelo
)
{
    printlog("processing lobby join request\n");

	outUseChunkedHelo = false;

	/*
	 * The legacy JOIN layout is 69 bytes. New clients append one
	 * capability byte at offset 69, making the packet 70 bytes.
	 * Reject shorter packets before reading any fixed fields.
	 */
	constexpr int legacyJoinPacketLength = 69;
	constexpr int versionFieldOffset = 48;
	constexpr int versionFieldLength = 8;

	if ( !net_packet
		|| net_packet->len < legacyJoinPacketLength )
	{
		printlog(
			"[NET]: rejecting truncated JOIN packet (len=%d expected>=%d)",
			net_packet ? net_packet->len : 0,
			legacyJoinPacketLength
		);
		outResult = MAXPLAYERS + 1;
		return directConnect
			? NET_LOBBY_JOIN_DIRECTIP_FAILURE
			: NET_LOBBY_JOIN_P2P_FAILURE;
	}

	const bool clientSupportsHeloChunk =
		net_packet->len >= 70
		&& (
			net_packet->data[69]
				& kJoinCapabilityHeloChunkV1
		);
	const bool clientSupportsLateJoin =
		net_packet->len >= 70
		&& (net_packet->data[69] & kJoinCapabilityLateJoinV1);
	const bool clientSupportsReconnectToken =
		net_packet->len >= 70 + static_cast<int>(kReconnectTokenLength)
		&& (net_packet->data[69] & kJoinCapabilityReconnectTokenV1);
	const Uint32 clientms = SDLNet_Read32(&net_packet->data[57]);
	const Uint32 clientlsg = SDLNet_Read32(&net_packet->data[61]);
	const Uint32 clientlobbyKey = SDLNet_Read32(&net_packet->data[65]);
	const Uint32 expectedGameKey =
		g_processingRuntimeJoin ? uniqueGameKey : loadingsavegame;
	const bool runtimeNewPlayer =
		g_processingRuntimeJoin && clientlsg == 0;
	const bool sendSavedEquipment =
		expectedGameKey != 0 && !runtimeNewPlayer;
	SaveGameInfo savegameinfo;

	char clientVersion[versionFieldLength + 1] = { 0 };
	memcpy(
		clientVersion,
		&net_packet->data[versionFieldOffset],
		versionFieldLength
	);

	Uint32 result = MAXPLAYERS;
	if (g_processingRuntimeJoin && !clientSupportsLateJoin)
	{
		result = MAXPLAYERS + 7;
	}
	else if ( strncmp(
			VERSION,
			clientVersion,
			versionFieldLength
		) != 0 )
	{
		result = MAXPLAYERS + 1; // wrong version number
	}
	else
	{
		if ( net_packet->data[56] == 0 )
		{
			// client will enter any player spot
			for ( result = 1; result < MAXPLAYERS; result++ )
			{
				if ( client_disconnected[result] == true
					&& !lockedSlots[result]
					&& (!runtimeNewPlayer
						|| !automatiaHasSavedPlayerPlacement(result)) )
				{
					break;    // no more player slots
				}
			}
		}
		else
		{
			// client is joining a particular player spot
			result = net_packet->data[56];
			if ( result >= MAXPLAYERS || !client_disconnected[result] || lockedSlots[result] )
			{
				result = MAXPLAYERS;  // client wants to fill a space that is already filled
			}
		}
		if (expectedGameKey != 0 && !runtimeNewPlayer) {
			savegameinfo = getSaveGameInfo(false);
		}
		if (runtimeNewPlayer && net_packet->data[56] != 0)
		{
			result = MAXPLAYERS + 8;
		}
		else if (runtimeNewPlayer)
		{
			// A new late joiner intentionally has no copy of the running save.
		}
		else if ( clientlsg != expectedGameKey && expectedGameKey == 0 )
		{
			result = MAXPLAYERS + 2;  // client shouldn't load save game
		}
		else if ( clientlsg == 0 && expectedGameKey != 0 )
		{
			result = MAXPLAYERS + 3;  // client is trying to join a save game without a save of their own
		}
		else if ( clientlsg != expectedGameKey )
		{
			result = MAXPLAYERS + 4;  // client is trying to join the game with an incompatible save
		}
		else if ( expectedGameKey != 0 && savegameinfo.mapseed != clientms )
		{
			result = MAXPLAYERS + 5;  // client is trying to join the game with a slightly incompatible save (wrong level)
		}
		else if ( expectedGameKey != 0
			&& clientlobbyKey
				!= (g_processingRuntimeJoin
					? uniqueLobbyKey
					: savegameinfo.lobbykey) )
		{
			result = MAXPLAYERS + 6; // lobby key not matching
		}
		else if (g_processingRuntimeJoin && clientlsg != 0
			&& result < MAXPLAYERS
			&& (net_packet->data[56] == 0
				|| !clientSupportsReconnectToken
				|| savegameinfo.players.size() <= result
				|| !ReconnectToken::equals(
					savegameinfo.players[result].reconnect_token,
					&net_packet->data[70])))
		{
			result = MAXPLAYERS + 9;
		}
	}
	outResult = result;
	if ( result >= MAXPLAYERS )
	{

		/*
		 * On error, reply directly to the packet's existing source
		 * address. Do not use the final valid client slot as scratch
		 * storage, because player 15 may already occupy it.
		 */
		net_packet->len = 8;
		memcpy(net_packet->data, "HELO", 4);
		SDLNet_Write32(result, &net_packet->data[4]); // error code for client to interpret
		printlog("sending error code %d to client.\n", result);
		if ( directConnect )
		{
			sendPacketSafe(net_sock, -1, net_packet, 0);
			return NET_LOBBY_JOIN_DIRECTIP_FAILURE;
		}
		else
		{
			return NET_LOBBY_JOIN_P2P_FAILURE;
		}
	}
	else
	{
	    const int c = result;
		if (g_processingRuntimeJoin && clientlsg != 0
			&& savegameinfo.players.size() > static_cast<std::size_t>(c))
		{
			automatiaReconnectTokens[c] =
				savegameinfo.players[c].reconnect_token;
		}
		if (!ReconnectToken::isValid(automatiaReconnectTokens[c]))
		{
			automatiaReconnectTokens[c] = generateReconnectToken();
		}
		if (!ReconnectToken::isValid(automatiaReconnectTokens[c]))
		{
			outResult = MAXPLAYERS + 9;
			memcpy(net_packet->data, "HELO", 4);
			SDLNet_Write32(outResult, &net_packet->data[4]);
			net_packet->len = 8;
			printlog("[Late Join] Reconnect identity generation failed closed.");
			if (directConnect)
			{
				sendPacketSafe(net_sock, -1, net_packet, 0);
				return NET_LOBBY_JOIN_DIRECTIP_FAILURE;
			}
			return NET_LOBBY_JOIN_P2P_FAILURE;
		}

		// on success, client gets legit player number
		resetServerLateJoinPlayer(c);
		client_disconnected[c] = false;
		if (!g_processingRuntimeJoin)
		{
			worldState.placePlayer(c, map);
		}
        stringCopy(stats[c]->name, (const char*)net_packet->data + 4, sizeof(Stat::name), 32);
		client_classes[c] = (int)SDLNet_Read32(&net_packet->data[36]);
		stats[c]->sex = static_cast<sex_t>((int)SDLNet_Read32(&net_packet->data[40]));
		Uint32 raceAndAppearance = (Uint32)SDLNet_Read32(&net_packet->data[44]);
		stats[c]->stat_appearance = (raceAndAppearance & 0xFF00) >> 8;
		stats[c]->playerRace = (raceAndAppearance & 0xFF);
		net_clients[c - 1].host = net_packet->address.host;
		net_clients[c - 1].port = net_packet->address.port;
		client_keepalive[c] = ticks;

		printlog("client %d connected.\n", c);

		// Normal lobby clients need the provisional roster immediately. Runtime
		// clients may still change this character; their finalized JOIN is sent
		// after PLYR instead, when in-game recipients can consume it.
		for ( int x = 1; !g_processingRuntimeJoin && x < MAXPLAYERS; x++ )
		{
			if ( client_disconnected[x] || c == x )
			{
				continue;
			}
			strcpy((char*)(&net_packet->data[0]), "JOIN");
			net_packet->data[4] = c; // clientnum
			net_packet->data[5] = client_classes[c]; // class
			net_packet->data[6] = stats[c]->sex; // sex
			net_packet->data[7] = (Uint8)stats[c]->stat_appearance; // appearance
			net_packet->data[8] = (Uint8)stats[c]->playerRace; // player race
			stringCopy((char*)net_packet->data + 9, stats[c]->name, 32, sizeof(Stat::name)); // name
			net_packet->address.host = net_clients[x - 1].host;
			net_packet->address.port = net_clients[x - 1].port;
			net_packet->len = 9 + 32;
			sendPacketSafe(net_sock, -1, net_packet, x - 1);
		}
		char shortname[32] = { 0 };
		strncpy(shortname, stats[c]->name, 22);

		//newString(&lobbyChatboxMessages, 0xFFFFFFFF, "\n***   %s has joined the game   ***\n", shortname);

		// send new client their id number + info on other clients
		memcpy(net_packet->data, "HELO", 4);
		SDLNet_Write32(c, &net_packet->data[4]);
		if (sendSavedEquipment) {
			constexpr int chunk_size = 6 + 32 + 6 * 10; // 6 bytes for player stats, 32 for name, 60 for equipment
			static_assert(
				8 + MAXPLAYERS * chunk_size <= NET_PACKET_SIZE,
				"NET_PACKET_SIZE is too small for the 15-player savegame HELO payload"
			);
			for ( int x = 0; x < MAXPLAYERS; x++ )
			{
				net_packet->data[8 + x * chunk_size + 0] =
					LanDiscovery::advertisedDisconnected(
						headless, x, client_disconnected[x]); // connectedness
				net_packet->data[8 + x * chunk_size + 1] = lockedSlots[x]; // locked state
				net_packet->data[8 + x * chunk_size + 2] = client_classes[x]; // class
				net_packet->data[8 + x * chunk_size + 3] = stats[x]->sex; // sex
				net_packet->data[8 + x * chunk_size + 4] = (Uint8)stats[x]->stat_appearance; // appearance
				net_packet->data[8 + x * chunk_size + 5] = (Uint8)stats[x]->playerRace; // player race

				char shortname[32];
                stringCopy(shortname, stats[x]->name, sizeof(shortname), sizeof(Stat::name));
				memcpy(net_packet->data + 8 + x * chunk_size + 6, shortname, sizeof(shortname)); // name

				const Item* player_slots[] = {
					stats[x]->helmet,
					stats[x]->breastplate,
					stats[x]->gloves,
					stats[x]->shoes,
					stats[x]->shield,
					stats[x]->weapon,
					stats[x]->cloak,
					stats[x]->amulet,
					stats[x]->ring,
					stats[x]->mask,
				};
				constexpr int num_slots = sizeof(player_slots) / sizeof(player_slots[0]);

				for (int j = 0; j < num_slots; ++j) {
					auto slot = player_slots[j];
					if (slot) {
						SDLNet_Write16((Uint16)slot->type, net_packet->data + 8 + x * chunk_size + 6 + 32 + j * 6);
						SDLNet_Write32((Uint32)slot->appearance, net_packet->data + 8 + x * chunk_size + 6 + 32 + j * 6 + 2);
					} else {
						SDLNet_Write16(0xffff, net_packet->data + 8 + x * chunk_size + 6 + 32 + j * 6);
						SDLNet_Write32(0xffffffff, net_packet->data + 8 + x * chunk_size + 6 + 32 + j * 6 + 2);
					}
				}
			}
			net_packet->len = 8 + MAXPLAYERS * chunk_size;
		} else {
			constexpr int chunk_size = 6 + 32; // 6 bytes for player stats, 32 for name
			static_assert(
				8 + MAXPLAYERS * chunk_size <= NET_PACKET_SIZE,
				"NET_PACKET_SIZE is too small for the 15-player HELO payload"
			);
			for ( int x = 0; x < MAXPLAYERS; x++ )
			{
				net_packet->data[8 + x * chunk_size + 0] =
					LanDiscovery::advertisedDisconnected(
						headless, x, client_disconnected[x]); // connectedness
				net_packet->data[8 + x * chunk_size + 1] = lockedSlots[x]; // locked state
				net_packet->data[8 + x * chunk_size + 2] = client_classes[x]; // class
				net_packet->data[8 + x * chunk_size + 3] = stats[x]->sex; // sex
				net_packet->data[8 + x * chunk_size + 4] = (Uint8)stats[x]->stat_appearance; // appearance
				net_packet->data[8 + x * chunk_size + 5] = (Uint8)stats[x]->playerRace; // player race

				char shortname[32];
                stringCopy(shortname, stats[x]->name, sizeof(shortname), sizeof(Stat::name));
				memcpy(net_packet->data + 8 + x * chunk_size + 6, shortname, sizeof(shortname)); // name
			}
			net_packet->len = 8 + MAXPLAYERS * chunk_size;
		}
		net_packet->address.host = net_clients[c - 1].host;
		net_packet->address.port = net_clients[c - 1].port;

		if (g_processingRuntimeJoin && net_packet->len < NET_PACKET_SIZE)
		{
			net_packet->data[net_packet->len++] = 1;
		}

		const bool shouldChunkHelo =
			sendSavedEquipment
			&& net_packet->len > kHeloSinglePacketMax;
		outUseChunkedHelo =
			clientSupportsHeloChunk
			&& shouldChunkHelo;

		if ( directConnect )
		{
			if ( outUseChunkedHelo )
			{
				std::vector<Uint8> heloSnapshot(
					net_packet->len
				);
				memcpy(
					heloSnapshot.data(),
					net_packet->data,
					heloSnapshot.size()
				);

				const Uint16 transferId =
					nextHeloTransferIdForPlayer(c);

				if ( !sendChunkedHeloDirect(
						heloSnapshot.data(),
						static_cast<int>(
							heloSnapshot.size()
						),
						transferId,
						c
					) )
				{
					printlog(
						"[NET]: chunked HELO failed; using legacy HELO for player %d",
						c
					);
					memcpy(
						net_packet->data,
						heloSnapshot.data(),
						heloSnapshot.size()
					);
					net_packet->len =
						static_cast<int>(
							heloSnapshot.size()
						);
					outUseChunkedHelo = false;
					sendPacketSafe(
						net_sock,
						-1,
						net_packet,
						0
					);
				}
			}
			else
			{
				sendPacketSafe(
					net_sock,
					-1,
					net_packet,
					0
				);
			}
			memcpy(net_packet->data, "RJTK", 4);
			net_packet->data[4] = static_cast<Uint8>(c);
			memcpy(&net_packet->data[5],
				automatiaReconnectTokens[c].data(), kReconnectTokenLength);
			net_packet->len = 5 + static_cast<int>(kReconnectTokenLength);
			net_packet->address.host = net_clients[c - 1].host;
			net_packet->address.port = net_clients[c - 1].port;
			sendPacketSafe(net_sock, -1, net_packet, c - 1);

			return NET_LOBBY_JOIN_DIRECTIP_SUCCESS;
		}
		else
		{
			return NET_LOBBY_JOIN_P2P_SUCCESS;
		}
	}
}

/*-------------------------------------------------------------------------------

	receiveEntity

	receives entity data from server

-------------------------------------------------------------------------------*/

Entity* receiveEntity(Entity* entity)
{
	bool newentity = false;
	int c;

	//TODO: Find out if this is needed.
	/*bool oldeffects[NUMEFFECTS];
	Stat* entityStats = entity->getStats();

	for ( int i = 0; i < NUMEFFECTS; ++i )
	{
		if ( !entityStats )
		{
			oldeffects[i] = 0;
		}
		else
		{
			oldeffects[i] = entityStats->EFFECTS[i];
		}
	}
	//Yes, it is necessary. I don't think I like this solution though, will try something else.
	*/

    Sint32 oldSprite = 0;
	if ( entity == nullptr )
	{
		newentity = true;
		entity = newEntity((int)SDLNet_Read16(&net_packet->data[8]), 0, map.entities, nullptr);
	}
	else
	{
	    oldSprite = entity->sprite;
		entity->sprite = (int)SDLNet_Read16(&net_packet->data[8]);
	}

    // for certain monsters, we don't want to use certain bytes,
    // because voxel-animated creatures (like rats and slimes)
    // need to move vertically for their animation.
	const auto monsterType = entity->getMonsterTypeFromSprite();
	bool excludeForAnimation =
	    !newentity &&
	    entity->behavior == &actMonster &&
		(monsterType == SLIME || ((monsterType == RAT || monsterType == SCARAB) &&
	    entity->skill[8])); // MONSTER_ATTACK

	//if ( Entity::getMonsterTypeFromSprite(entity->sprite) == SPIDER )
	//{
	//	if ( arachnophobia_filter )
	//	{
	//		switch ( entity->sprite )
	//		{
	//			case 267: // spider
	//				entity->sprite = 997; // crab
	//				break;
	//			case 823: // player spider
	//				entity->sprite = 1001; // player crab
	//				break;
	//			case 1118: // shelob
	//				entity->sprite = 1189; // bubbles
	//				break;
	//			default:
	//				break;
	//		}
	//	}
	//	else
	//	{
	//		switch ( entity->sprite )
	//		{
	//			case 997: // crab
	//				entity->sprite = 997; // spider
	//				break;
	//			case 1001: // player crab
	//				entity->sprite = 823; // player spider
	//				break;
	//			case 1189: // bubbles
	//				entity->sprite = 1118; // bubbles
	//				break;
	//			default:
	//				break;
	//		}
	//	}
	//}

	if (excludeForAnimation) {
		if ( monsterType == SLIME && Entity::getMonsterTypeFromSprite(oldSprite) != SLIME )
		{
			// take this sprite as we had editor data (e.g sprite 10 or 79)
		}
		else
		{
			entity->sprite = oldSprite;
		}
	}

	if ( entity->behavior == &actItem && entity->itemFollowUID != 0 )
	{
		excludeForAnimation = true;
	}
	const bool excludeYaw =
		entity->behavior == &actMagiclightBall
		|| (entity->behavior == &actLeafPile)
		|| (entity->behavior == &actItem && entity->itemFollowUID != 0);

	entity->lastupdate = ticks;
	entity->lastupdateserver = (Uint32)SDLNet_Read32(&net_packet->data[36]);
	entity->setUID((int)SDLNet_Read32(&net_packet->data[4])); // remember who I am
	entity->new_x = ((Sint16)SDLNet_Read16(&net_packet->data[10])) / 32.0;
	entity->new_y = ((Sint16)SDLNet_Read16(&net_packet->data[12])) / 32.0;
	if (!excludeForAnimation && (newentity || monsterType != SCARAB)) {
	    entity->new_z = ((Sint16)SDLNet_Read16(&net_packet->data[14])) / 32.0;
	}
	entity->sizex = (Sint8)net_packet->data[16];
	entity->sizey = (Sint8)net_packet->data[17];
	if (newentity || monsterType != SLIME) {
	    entity->scalex = ((Uint8)net_packet->data[18]) / 128.f;
	    entity->scaley = ((Uint8)net_packet->data[19]) / 128.f;
	    entity->scalez = ((Uint8)net_packet->data[20]) / 128.f;
	}
	if ( newentity || !excludeYaw )
	{
		entity->new_yaw = ((Sint16)SDLNet_Read16(&net_packet->data[21])) / 256.0;
	}
	entity->new_pitch = ((Sint16)SDLNet_Read16(&net_packet->data[23])) / 256.0;
	entity->new_roll = ((Sint16)SDLNet_Read16(&net_packet->data[25])) / 256.0;
	if ( newentity )
	{
		entity->x = entity->new_x;
		entity->y = entity->new_y;
		entity->z = entity->new_z;
		entity->yaw = entity->new_yaw;
		entity->pitch = entity->new_pitch;
		entity->roll = entity->new_roll;
	}
	entity->focalx = ((Sint8)net_packet->data[27]) / 8.0;
	entity->focaly = ((Sint8)net_packet->data[28]) / 8.0;
	if (!excludeForAnimation) {
	    entity->focalz = ((Sint8)net_packet->data[29]) / 8.0;
	}
	for (c = 0; c < 16; ++c)
	{
		entity->flags[c] =
			(net_packet->data[34 + c / 8]
				& power(2, c - (c / 8) * 8)) != 0;
	}
	for ( c = 0; c < 8; ++c ) // new flags 16-23
	{
		entity->flags[c + 16] =
			(net_packet->data[46 + c / 8]
				& power(2, c - (c / 8) * 8)) != 0;
	}
	entity->vel_x = ((Sint16)SDLNet_Read16(&net_packet->data[40])) / 32.0;
	entity->vel_y = ((Sint16)SDLNet_Read16(&net_packet->data[42])) / 32.0;
	entity->vel_z = ((Sint16)SDLNet_Read16(&net_packet->data[44])) / 32.0;

	return entity;
}

/*-------------------------------------------------------------------------------

	clientActions

	Assigns an action to a given entity based mainly on its sprite

-------------------------------------------------------------------------------*/

void clientActions(Entity* entity)
{
	int playernum;

	// this code assigns behaviors based on the sprite (model) number
	switch ( entity->sprite )
	{
	    case 1163:
	    case 1164:
	    case 1165:
	    case 1166:
	    case 1167:
	    case 1168:
	    case 1169:
		case 1631:
		case 1:
			entity->behavior = &actDoorFrame;
			break;
		case 2:
			entity->behavior = &actDoor;
			break;
		case 1162:
			entity->behavior = &actIronDoor;
			break;
		case 3:
			entity->behavior = &actTorch;
			entity->flags[NOUPDATE] = 1;
			break;
		case 160:
		case 203:
		case 212:
		case 213:
		case 214:
		case 682:
		case 681:
		case 1398:
		case 1399:
		case 1400:
			entity->flags[NOUPDATE] = true;
			break;
		case 162:
			entity->behavior = &actCampfire;
			entity->flags[NOUPDATE] = true;
			break;
		case 131:
		{
			// Runtime catch-up clears behaviors before rebuilding them here.
			// Editor light sources are invisible by design, but must keep their
			// behavior on clients so they reconstruct their local light field.
			const bool initializeLightSource =
				entity->behavior != &actLightSource;
			entity->behavior = &actLightSource;
			entity->flags[SPRITE] = true;
			entity->flags[INVISIBLE] = true;
			entity->flags[PASSABLE] = true;
			if (initializeLightSource)
			{
				entity->removeLightField();
				entity->light = nullptr;
				entity->skill[8] = 0;
			}
			break;
		}
		case 163:
			entity->skill[2] = (int)SDLNet_Read32(&net_packet->data[30]);
			entity->behavior = &actFountain;
			break;
		case 174:
			if (SDLNet_Read32(&net_packet->data[30]) != 0)
			{
				entity->behavior = &actMagiclightBall; //TODO: Finish this here. I think this gets reassigned every time the entity is recieved? Make sure.
			}
			break;
		case 185:
			entity->behavior = &actSwitch;
			break;
		case 186:
			entity->behavior = &actGate;
			break;
		case 216:
		case 1790:
			entity->behavior = &actChestLid;
			break;
		case 254:
		case 255:
		case 256:
		case 257:
			entity->behavior = &actPortal;
			break;
		case 273:
			entity->behavior = &actMCaxe;
			break;
		case 278:
		case 279:
		case 280:
		case 281:
			entity->behavior = &actWinningPortal;
			break;
		case 282:
			entity->behavior = &actSpearTrap;
			break;
		case 578:
			entity->behavior = &actPowerCrystal;
			break;
		case 586:
			entity->behavior = &actSwitchWithTimer;
			break;
		case 601:
			entity->behavior = &actPedestalBase;
			break;
		case 602:
		case 603:
		case 604:
		case 605:
			entity->behavior = &actPedestalOrb;
			break;
		case 667:
		case 668:
			entity->behavior = &actBeartrap;
			break;
		case 674:
		case 675:
		case 676:
		case 677:
			entity->behavior = &actCeilingTile;
			entity->flags[NOUPDATE] = true;
			break;
		case 629:
			entity->behavior = &actColumn;
			entity->flags[NOUPDATE] = true;
			break;
		case 632:
		case 633:
			entity->behavior = &actPistonCam;
			entity->flags[NOUPDATE] = true;
			break;
		case 130:
		case 1379:
			entity->behavior = &actGoldBag;
			break;
		case 1481:
			entity->behavior = &actDaedalusShrine;
			break;
		case 1484:
			entity->behavior = &actAssistShrine;
			break;
		case 1622:
			entity->behavior = &actCauldron;
			break;
		case 1617:
			entity->behavior = &actWorkbench;
			break;
		case 1619:
		case 1620:
			entity->behavior = &actMailbox;
			break;
		case 1585:
		case 1586:
		case 1587:
		case 1588:
		case 1589:
		case 1590:
		case 1591:
		case 1592:
			// wall lock keys
			entity->behavior = &actEmpty;
			entity->flags[NOUPDATE] = true;
			break;
		case 1786:
			entity->behavior = &actGreasePuddleSpawner;
			entity->flags[NOUPDATE] = true;
			break;
		case 1151:
		case 1152:
			// wall buttons
			entity->behavior = &actEmpty;
			entity->flags[NOUPDATE] = true;
			break;
		case 1809:
			entity->behavior = &actParticleDemesneDoor;
			entity->flags[NOUPDATE] = true;
			break;
		case 1913:
			entity->behavior = &actLeafPile;
			break;
		case Player::Ghost_t::GHOST_MODEL_P1:
		case Player::Ghost_t::GHOST_MODEL_P2:
		case Player::Ghost_t::GHOST_MODEL_P3:
		case Player::Ghost_t::GHOST_MODEL_P4:
		case Player::Ghost_t::GHOST_MODEL_PX:
			// player ghosts
			playernum = 0xFF & SDLNet_Read32(&net_packet->data[30]);
			if ( playernum >= 0 && playernum < MAXPLAYERS )
			{
				if ( players[playernum] )
				{
					players[playernum]->ghost.my = entity;
				}
				entity->skill[2] = playernum;
				entity->behavior = &actDeathGhost;
				if ( playernum == clientnum && multiplayer == CLIENT )
				{
					entity->flags[UPDATENEEDED] = false;
				}
				else
				{
					entity->flags[UPDATENEEDED] = true;
				}
				entity->flags[PASSABLE] = true;
				entity->flags[INVISIBLE] = true;
				entity->flags[GENIUS] = true;
				Uint32 specialFlags = (SDLNet_Read32(&net_packet->data[30]) >> 8) & 0xFFFFFF;
				if ( (specialFlags & 0xFF) )
				{
					entity->monsterSpecialState = (specialFlags & 0xFF);
				}
				if ( (specialFlags >> 8) & 0xFFFF )
				{
					int cosmeticSprite = (specialFlags >> 8) & 0xFFFF;
					entity->skill[10] = cosmeticSprite;
				}
				entity->sizex = 2;
				entity->sizey = 2;
			}
			break;
		default:
			if ( entity->isPlayerHeadSprite() )
			{
				// these are all player heads
				playernum = SDLNet_Read32(&net_packet->data[30]);
				if ( playernum >= 0 && playernum < MAXPLAYERS )
				{
					if ( players[playernum] )
					{
						players[playernum]->entity = entity;
					}
					entity->skill[2] = playernum;
					entity->behavior = &actPlayer;
				}
			}
			break;
	}

	// if the above method failed, we check the value of skill[2] (stored in net_packet->data[30]) and assign an action based on that
	if ( entity->behavior == NULL )
	{
		Sint32 c = (Sint32)SDLNet_Read32(&net_packet->data[30]);
		if ( c < 0 )
		{
			switch ( c )
			{
				case -4:
					entity->behavior = &actMonster;
					entity->skill[2] = -4;
					break;
				case -5:
					entity->behavior = &actItem;
					break;
				case -6:
					entity->behavior = &actGib;
					break;
				case -7:
					entity->behavior = &actEmpty;
					if ( entity->sprite == 989 ) // boulder_lava.vox
					{
						entity->flags[BURNABLE] = true;
					}
					break;
				case -8:
					entity->behavior = &actThrown;
					break;
				case -9:
					entity->behavior = &actLiquid;
					break;
				case -10:
					entity->behavior = &actMagiclightBall;
					break;
				case -11:
					entity->behavior = &actMagicClient;
					break;
				case -12:
					entity->behavior = &actMagicClientNoLight;
					break;
				case -13:
					entity->behavior = &actParticleSapCenter;
					break;
				case -14:
					entity->behavior = &actDecoyBox;
					break;
				case -15:
					entity->behavior = &actBomb;
					break;
				case -16:
					entity->behavior = &actBoulder;
					break;
				case -18:
					entity->behavior = &actParticleFloorMagic;
					entity->flags[NOUPDATE] = true;
					break;
				default:
					if ( static_cast<Uint8>(c & 0xFF) == 17 )
					{
						entity->arrowShotByWeapon = (c >> 8) & 0xFFF;
						int dropOffModifier = (c >> 20) & 0xF;
						entity->arrowDropOffEquipmentModifier = dropOffModifier - 8;
						entity->behavior = &actArrow;
					}
					else if ( static_cast<Uint8>(c & 0xFF) == 19 )
					{
						entity->particleTimerDuration = (c >> 8) & 0xFFF;
						entity->particleTimerCountdownAction = (c >> 20) & 0xFF;
						entity->behavior = &actParticleTimer;
					}
					else if ( static_cast<Uint8>(c & 0xFF) == 20 )
					{
						entity->behavior = &actParticleFloorMagic;
						entity->skill[2] = c;
						floorMagicClientReceive(entity);
					}
					else if ( static_cast<Uint8>(c & 0xFF) == 21 )
					{
						entity->behavior = &actParticleFloorMagic;
						entity->skill[2] = c;
						entity->flags[NOUPDATE] = true;
						floorMagicClientReceive(entity);
					}
					else if ( static_cast<Uint8>(c & 0xFF) == 22 )
					{
						entity->behavior = &actParticleWave;
						entity->skill[2] = c;
						entity->flags[NOUPDATE] = true;
						particleWaveClientReceive(entity);
					}
					else if ( static_cast<Uint8>(c & 0xFF) == 23 )
					{
						entity->behavior = &actWind;
						entity->skill[2] = c;
						entity->flags[NOUPDATE] = true;
						particleWaveClientReceive(entity);
					}
					else if ( static_cast<Uint8>(c & 0xFF) == 24 )
					{
						entity->behavior = &actRadiusMagic;
						entity->skill[2] = c;
						radiusMagicClientReceive(entity);
					}
					else if ( static_cast<Uint8>(c & 0xFF) == 25 )
					{
						entity->behavior = &actColliderDecoration;
						entity->skill[2] = c;
						entity->flags[NOUPDATE] = true;
						entity->colliderDamageTypes = (c >> 8) & 0xFF;
						entity->colliderSpellEvent = (c >> 16) & 0xFF;
						Entity::colliderAssignProperties(entity, false, &map);
					}
					else if ( static_cast<Uint8>(c & 0xFF) == 26 )
					{
						entity->behavior = &actTeleporter;
						entity->skill[2] = c;
						entity->flags[NOUPDATE] = true;
						int duration = (c >> 8) & 0xFFFF;
						int dir = (c >> 24) & 0xF;
						tunnelPortalSetAttributes(entity, duration, dir);
					}
					break;
			}
		}
	}
}

/*-------------------------------------------------------------------------------

	clientHandlePacket

	Called by clientHandleMessages. Does the actual handling of a packet.

-------------------------------------------------------------------------------*/

static void changeLevel()
{
    WorldInstanceIdentity previousPlayerInstances[MAXPLAYERS];
    for ( int player = 0; player < MAXPLAYERS; ++player )
    {
        if ( players[player] )
        {
            previousPlayerInstances[player] =
                players[player]->worldInstance;
        }
    }
    // A normal or older level-change packet has no tunnel request.
    // Reset first so an unrelated later transition cannot reuse a
    // previous custom tunnel ID.
    loadCustomNextMap = "";
    loadCustomNextTunnelID = 0;
    pendingIndependentLevelChange = false;
    pendingIndependentPlayer = -1;
    pendingIndependentRuntimeUid = 0;

    constexpr size_t customMapOffset = 14;

    if ( net_packet->len > customMapOffset )
    {
        const size_t availableMapBytes =
            net_packet->len - customMapOffset;

        const char* customMapName =
            reinterpret_cast<const char*>(
                &net_packet->data[customMapOffset]
            );

        // Find the map-name terminator without reading beyond
        // the received packet.
        const size_t customMapNameLength =
            strnlen(
                customMapName,
                availableMapBytes
            );

        if ( customMapNameLength
            < availableMapBytes )
        {
            // A null terminator was found inside the packet.
            if ( customMapNameLength > 0 )
            {
                loadCustomNextMap.assign(
                    customMapName,
                    customMapNameLength
                );
            }

            const size_t tunnelIDOffset =
                customMapOffset
                + customMapNameLength
                + 1;

            // New tunnel-aware packets contain four bytes after
            // the map-name terminator. Older packets safely fall
            // through and retain tunnel ID 0.
            if ( net_packet->len
                >= tunnelIDOffset + sizeof(Uint32) )
            {
                loadCustomNextTunnelID =
                    static_cast<Sint32>(
                        SDLNet_Read32(
                            &net_packet->data[
                                tunnelIDOffset
                            ]
                        )
                    );

                const size_t extensionOffset =
                    tunnelIDOffset + sizeof(Uint32);
                if ( net_packet->len >= extensionOffset + 6
                    && net_packet->data[extensionOffset] == 0xA1 )
                {
                    pendingIndependentPlayer =
                        net_packet->data[extensionOffset + 1];
                    pendingIndependentRuntimeUid =
                        SDLNet_Read32(&net_packet->data[extensionOffset + 2]);
                    pendingIndependentLevelChange =
                        pendingIndependentPlayer == clientnum;
                }
            }
        }
        else
        {
            printlog(
                "[Custom Tunnel] Warning: level-change packet contained no map-name terminator."
            );
        }
    }

    printlog(
        "[Custom Tunnel] Client received map '%s' and destination tunnel ID %d.",
        loadCustomNextMap.c_str(),
        loadCustomNextTunnelID
    );

	if ( MainMenu::isCutsceneActive() )
	{
		introstage = 1; // return to normal game functionality
		pauseGame(1, false); // unpause game
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
	if ( openedChest[clientnum] )
	{
		closeChestClientside(clientnum);
	}

	int prevcurrentlevel = currentlevel;
	int prevsecretfloor = secretlevel;
	std::string prevmapname = map.name;

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

	MainMenu::destroyMainMenu();
	movie = false;

	// setup level change
	const int newlevel = static_cast<Sint8>(net_packet->data[13]);
	printlog("Received order to change level to %d (from %d).\n", newlevel, currentlevel);
	currentlevel = newlevel;

	if ( !secretlevel )
	{
		switch ( currentlevel )
		{
			case 5:
				steamAchievement("BARONY_ACH_TWISTY_PASSAGES");
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

	list_FreeAll(&removedEntities);
	setRemovedEntityTombstoneScope(worldState.activeIdentity());
	for ( auto node = map.entities->first; node != nullptr; node = node->next )
	{
		auto entity = (Entity*)node->element;
		auto entity2 = newEntity(entity->sprite, 1, &removedEntities, nullptr);
		entity2->setUID(entity->getUID());
	}
	for ( int i = 0; i < MAXPLAYERS; ++i )
	{
		list_FreeAll(&stats[i]->FOLLOWERS);
	}

	// load next level
	darkmap = false;
	secretlevel = net_packet->data[4];
	mapseed = SDLNet_Read32(&net_packet->data[5]);
	numplayers = 0;
	entity_uids = (Uint32)SDLNet_Read32(&net_packet->data[9]);
	printlog("Received map seed: %d. Entity UID start: %d\n", mapseed, entity_uids);

	for ( int i = 0; i < MAXPLAYERS; ++i )
	{
		minimapPings[i].clear(); // clear minimap pings
        auto& camera = players[i]->camera();
        camera.globalLightModifierActive = GLOBAL_LIGHT_MODIFIER_STOPPED;
        camera.luminance = defaultLuminance;
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

	// clear follower menu entities.
	FollowerMenu[clientnum].closeFollowerMenuGUI(true);
	CalloutMenu[clientnum].closeCalloutMenuGUI();

	bool died = stats[clientnum] && stats[clientnum]->HP <= 0;

    // load map file
	loading = true;
    createLevelLoadScreen(5);
    std::atomic_bool loading_done {false};
    auto loading_task = std::async(std::launch::async, [&loading_done](){
	    gameplayCustomManager.readFromFile();
		if ( gameplayCustomManager.inUse() )
		{
			conductGameChallenges[CONDUCT_MODDED] = 1;
			Mods::disableSteamAchievements = true;
		}
        updateLoadingScreen(10);
		int checkMapHash = -1;

		int result = physfsLoadMapFile(
			currentlevel,
			mapseed,
			false,
			&checkMapHash
		);
		if ( pendingIndependentLevelChange )
		{
			// Raw editor entities consume their ordinary load-time UIDs first.
			// The server-provided value is the shared start of runtime creation.
			entity_uids = pendingIndependentRuntimeUid;
		}

		/*
		* Multiplayer sync is a pain :( but cool!
		* Multiplayer clients receive the destination map's authoritative
		* persistence snapshot before LVLC/LVLR.
		*
		* Apply removals while the raw editor entities still exist and before
		* assignActions() creates runtime entities.
		*/
		applyPersistentMapRemovals();

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
		if ( pendingIndependentLevelChange )
		{
			bool playerMask[MAXPLAYERS] = {};
			playerMask[clientnum] = true;
			assignActions(&map, playerMask);
		}
		else
		{
			assignActions(&map);
		}

		/*
		* Lever handles and runtime gate entities now exist. Restore their
		* authoritative host-supplied state by persistent ID.
		*/
		applyPersistentMechanismStates();

		updateLoadingScreen(55);

		generatePathMaps();
        updateLoadingScreen(80);

        node_t *node, *nextnode;
	    for ( node = map.entities->first; node != nullptr; node = nextnode )
	    {
		    nextnode = node->next;
		    Entity* entity = (Entity*)node->element;
		    if ( entity->flags[NOUPDATE] )
		    {
			    list_RemoveNode(entity->mynode);    // we're anticipating this entity data from server
		    }
	    }
        updateLoadingScreen(99);

	    loading_done = true;
	    return result;
	});
    while (!loading_done)
    {
	    doLoadingScreen();
	    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    destroyLoadingScreen();
	loading = false;
    int result = loading_task.get();
    if ( pendingIndependentLevelChange )
    {
        for ( int player = 0; player < MAXPLAYERS; ++player )
        {
            if ( player != clientnum )
            {
                worldState.removePlayer(player);
                if ( players[player] )
                {
                    players[player]->worldInstance =
                        previousPlayerInstances[player];
					// This client no longer owns or simulates the remote player's
					// source map. Do not retain a pointer into the map storage that
					// was just replaced; a later map-local ENTU will bind the new
					// authoritative entity if that player follows us.
					players[player]->entity = nullptr;
                }
            }
        }
        worldState.placePlayer(clientnum, map);
        pendingIndependentLevelChange = false;
        pendingIndependentPlayer = -1;
        pendingIndependentRuntimeUid = 0;
    }
    
    clearChunks();
    createChunks();

	// (special) unlock temple achievement
	if ( secretlevel && currentlevel == 8 )
	{
		steamAchievement("BARONY_ACH_TRICKS_AND_TRAPS");
	}

	Player::Minimap_t::mapDetails.clear();

	if ( !secretlevel )
	{
		messagePlayer(clientnum, MESSAGE_PROGRESSION, Language::get(710), currentlevel);
	}
	else
	{
		messagePlayer(clientnum, MESSAGE_PROGRESSION, Language::get(711), map.name);
	}
	if ( !secretlevel && result )
	{
		switch ( currentlevel )
		{
			case 2:
				messagePlayer(clientnum, MESSAGE_HINT, Language::get(712));
				Player::Minimap_t::mapDetails.push_back(std::make_pair("secret_exit_description", Language::get(712)));
				break;
			case 3:
				messagePlayer(clientnum, MESSAGE_HINT, Language::get(713));
				Player::Minimap_t::mapDetails.push_back(std::make_pair("secret_exit_description", Language::get(713)));
				break;
			case 7:
				messagePlayer(clientnum, MESSAGE_HINT, Language::get(714));
				Player::Minimap_t::mapDetails.push_back(std::make_pair("secret_exit_description", Language::get(714)));
				break;
			case 8:
				messagePlayer(clientnum, MESSAGE_HINT, Language::get(715));
				Player::Minimap_t::mapDetails.push_back(std::make_pair("secret_exit_description", Language::get(715)));
				break;
			case 11:
				messagePlayer(clientnum, MESSAGE_HINT, Language::get(716));
				Player::Minimap_t::mapDetails.push_back(std::make_pair("secret_exit_description", Language::get(716)));
				break;
			case 13:
				messagePlayer(clientnum, MESSAGE_HINT, Language::get(717));
				Player::Minimap_t::mapDetails.push_back(std::make_pair("secret_exit_description", Language::get(717)));
				break;
			case 16:
				messagePlayer(clientnum, MESSAGE_HINT, Language::get(718));
				Player::Minimap_t::mapDetails.push_back(std::make_pair("secret_exit_description", Language::get(718)));
				break;
			case 18:
				messagePlayer(clientnum, MESSAGE_HINT, Language::get(719));
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
		messagePlayer(clientnum, MESSAGE_HINT, Language::get(2382));
	}
	if ( MFLAG_DISABLELEVITATION )
	{
		messagePlayer(clientnum, MESSAGE_HINT, Language::get(2383));
		Player::Minimap_t::mapDetails.push_back(std::make_pair("map_flag_disable_levitation", Language::get(2383)));
	}
	if ( MFLAG_DISABLEDIGGING )
	{
		messagePlayer(clientnum, MESSAGE_HINT, Language::get(2450));
		Player::Minimap_t::mapDetails.push_back(std::make_pair("map_flag_disable_digging", Language::get(2450)));
	}
	if ( MFLAG_DISABLEHUNGER )
	{
		Player::Minimap_t::mapDetails.push_back(std::make_pair("map_flag_disable_hunger", ""));
	}

	if ( !died )
	{
		if ( stats[clientnum]->type == MYCONID && stats[clientnum]->playerRace == RACE_MYCONID && stats[clientnum]->stat_appearance == 0
			&& stats[clientnum]->helmet && gameStatistics[STATISTICS_NO_CAP] >= 0 )
		{
			gameStatistics[STATISTICS_NO_CAP]++;
			if ( gameStatistics[STATISTICS_NO_CAP] >= 5 )
			{
				steamAchievement("BARONY_ACH_NO_CAP");
			}
		}
		if ( stats[clientnum]->getEffectActive(EFF_GROWTH) >= 2
			&& ((stats[clientnum]->type == MYCONID && stats[clientnum]->playerRace == RACE_MYCONID)
				|| (stats[clientnum]->type == DRYAD && stats[clientnum]->playerRace == RACE_DRYAD)) && stats[clientnum]->stat_appearance == 0
			&& !stats[clientnum]->helmet && gameStatistics[STATISTICS_DONT_TOUCH_HAIR] >= 0 )
		{
			gameStatistics[STATISTICS_DONT_TOUCH_HAIR]++;
			if ( gameStatistics[STATISTICS_DONT_TOUCH_HAIR] >= 25 )
			{
				steamAchievement("BARONY_ACH_DONT_TOUCH_HAIR");
			}
		}
		if ( stats[clientnum]->type == SALAMANDER && stats[clientnum]->playerRace == RACE_SALAMANDER && stats[clientnum]->stat_appearance == 0
			&& stats[clientnum]->getEffectActive(EFF_SALAMANDER_HEART) >= 3 && stats[clientnum]->getEffectActive(EFF_SALAMANDER_HEART) <= 4
			&& gameStatistics[STATISTICS_GARGOYLES_QUEST] >= 0 )
		{
			gameStatistics[STATISTICS_GARGOYLES_QUEST]++;
			if ( gameStatistics[STATISTICS_GARGOYLES_QUEST] >= 10 )
			{
				steamAchievement("BARONY_ACH_GARGOYLES_QUEST");
			}
		}
		if ( stats[clientnum]->type == SALAMANDER && stats[clientnum]->playerRace == RACE_SALAMANDER && stats[clientnum]->stat_appearance == 0
			&& stats[clientnum]->getEffectActive(EFF_SALAMANDER_HEART) >= 1 && stats[clientnum]->getEffectActive(EFF_SALAMANDER_HEART) <= 2
			&& gameStatistics[STATISTICS_FIRE_FIGHTER] >= 0 )
		{
			gameStatistics[STATISTICS_FIRE_FIGHTER]++;
			if ( gameStatistics[STATISTICS_FIRE_FIGHTER] >= 5 )
			{
				steamAchievement("BARONY_ACH_FIRE_FIGHTER");
			}
		}
		if ( stats[clientnum]->type == SALAMANDER && stats[clientnum]->playerRace == RACE_SALAMANDER && stats[clientnum]->stat_appearance == 0
			&& !stats[clientnum]->getEffectActive(EFF_SALAMANDER_HEART)
			&& gameStatistics[STATISTICS_DISCIPLINE] >= 0 )
		{
			gameStatistics[STATISTICS_DISCIPLINE]++;
			if ( gameStatistics[STATISTICS_DISCIPLINE] >= 25 )
			{
				steamAchievement("BARONY_ACH_DISCIPLINE");
			}
		}
	}

	Compendium_t::Events_t::onLevelChangeEvent(clientnum, prevcurrentlevel, prevsecretfloor, prevmapname, died);
	for ( int i = 0; i < MAXPLAYERS; ++i )
	{
		players[i]->compendiumProgress.playerAliveTimeTotal = 0;
		players[i]->compendiumProgress.playerGameTimeTotal = 0;
	}

	if ( gameModeManager.allowsSaves() )
	{
		saveGame();
	}

	Compendium_t::Events_t::writeItemsSaveData();
	Compendium_t::writeUnlocksSaveData();
#ifdef LOCAL_ACHIEVEMENTS
	LocalAchievements_t::writeToFile();
#endif
	printlog("Done.\n");

	if ( !strncmp(map.name, "Mages Guild", 11) )
	{
		messagePlayer(clientnum, MESSAGE_HINT, Language::get(2599));
	}
	fadeout = false;
	fadealpha = 255;
}

static std::unordered_map<Uint32, void(*)()> clientPacketHandlers = {
	{'JOIN', [](){
		if (!net_packet || net_packet->len != 41)
		{
			printlog("[Roster] Client rejected malformed in-game JOIN packet.");
			return;
		}
		const int player = net_packet->data[4];
		if (player <= 0 || player >= MAXPLAYERS || !stats[player])
		{
			printlog("[Roster] Client rejected invalid in-game JOIN slot %d.", player);
			return;
		}
		client_disconnected[player] = false;
		client_classes[player] = net_packet->data[5];
		stats[player]->sex = static_cast<sex_t>(net_packet->data[6]);
		stats[player]->stat_appearance = net_packet->data[7];
		stats[player]->playerRace = net_packet->data[8];
		stringCopy(
			stats[player]->name,
			reinterpret_cast<char*>(&net_packet->data[9]),
			sizeof(Stat::name),
			32);
		printlog("[Roster] Client accepted finalized character for player %d.", player);
	}},
	{'LJBG', [](){
		LateJoinProtocol::Begin begin;
		if (!LateJoinProtocol::decodeBegin(
				net_packet->data, net_packet->len, begin)
			|| !g_clientLateJoinAssembler.begin(begin))
		{
			g_clientLateJoinAssembler.fail();
			printlog("[Late Join] Client rejected malformed snapshot begin.");
			return;
		}
		g_clientLateJoinBegin = begin;
		g_clientLateJoinSpawnAuthorized = false;
		clientBeginLateJoinPacketDeferral(
			begin.transferId, begin.instanceRevision);
	}},
	{'LJCH', [](){
		LateJoinProtocol::Chunk chunk;
		if (!LateJoinProtocol::decodeChunk(
				net_packet->data, net_packet->len, chunk)
			|| g_clientLateJoinAssembler.accept(chunk)
				== LateJoinProtocol::ReceiveResult::Rejected)
		{
			g_clientLateJoinAssembler.fail();
			printlog("[Late Join] Client rejected corrupt snapshot chunk.");
		}
		else
		{
			clientNoteLateJoinProgress();
		}
	}},
	{'LJDN', [](){
		LateJoinProtocol::Complete complete;
		if (!LateJoinProtocol::decodeComplete(
				net_packet->data, net_packet->len, complete)
			|| g_clientLateJoinAssembler.finish(complete)
				!= LateJoinProtocol::ReceiveResult::Complete)
		{
			g_clientLateJoinAssembler.fail();
			printlog("[Late Join] Client rejected incomplete snapshot transfer.");
			return;
		}
		const std::vector<std::uint8_t>& bytes =
			g_clientLateJoinAssembler.snapshot();
		const std::string snapshot(
			reinterpret_cast<const char*>(bytes.data()), bytes.size());
		std::string snapshotError;
		const bool accepted = stageAutomatiaPersistentWorldSnapshot(
			snapshot, std::to_string(g_clientLateJoinBegin.sessionKey), snapshotError);
		LateJoinProtocol::Ready ready;
		ready.playerIndex = static_cast<std::uint8_t>(clientnum);
		ready.transferId = complete.transferId;
		ready.instanceRevision = complete.instanceRevision;
		ready.snapshotAccepted = accepted;
		const std::vector<std::uint8_t> readyPacket =
			LateJoinProtocol::encodeReady(ready);
		if (readyPacket.empty())
		{
			printlog("[Late Join] Client could not encode snapshot-ready response.");
			return;
		}
		memcpy(net_packet->data, readyPacket.data(), readyPacket.size());
		net_packet->len = static_cast<int>(readyPacket.size());
		net_packet->address.host = net_server.host;
		net_packet->address.port = net_server.port;
		sendPacketSafe(net_sock, -1, net_packet, 0);
		if (!accepted)
		{
			printlog(
				"[Late Join] Client rejected snapshot document: %s",
				snapshotError.c_str());
		}
		else
		{
			clientNoteLateJoinProgress();
		}
	}},
	{'LJOK', [](){
		LateJoinProtocol::Authorization authorization;
		if (!LateJoinProtocol::decodeAuthorization(
				net_packet->data, net_packet->len, authorization)
			|| authorization.transferId != g_clientLateJoinBegin.transferId
			|| authorization.instanceRevision
				!= g_clientLateJoinBegin.instanceRevision
			|| !authorization.spawnAuthorized
			|| !g_clientLateJoinAssembler.complete())
		{
			printlog("[Late Join] Client rejected invalid spawn authorization.");
			return;
		}
		g_clientLateJoinSpawnAuthorized = true;
		clientNoteLateJoinProgress();
		LateJoinProtocol::Ready go;
		go.playerIndex = static_cast<std::uint8_t>(clientnum);
		go.transferId = authorization.transferId;
		go.instanceRevision = authorization.instanceRevision;
		go.snapshotAccepted = true;
		const std::vector<std::uint8_t> goPacket =
			LateJoinProtocol::encodeGo(go);
		memcpy(net_packet->data, goPacket.data(), goPacket.size());
		net_packet->len = static_cast<int>(goPacket.size());
		net_packet->address.host = net_server.host;
		net_packet->address.port = net_server.port;
		sendPacketSafe(net_sock, -1, net_packet, 0);
		printlog(
			"[Late Join] Client accepted spawn authorization for transfer %u.",
			authorization.transferId);
	}},
	{'LJCB', [](){
		if (!clientAcceptLateJoinCatchupBegin(
				net_packet->data, net_packet->len))
		{
			printlog("[Late Join] Client rejected catch-up begin.");
		}
	}},
	{'LJCC', [](){
		if (!clientAcceptLateJoinCatchupChunk(
				net_packet->data, net_packet->len))
		{
			printlog("[Late Join] Client rejected catch-up chunk.");
		}
	}},
	{'LJCE', [](){
		if (!clientAcceptLateJoinCatchupComplete(
				net_packet->data, net_packet->len))
		{
			printlog("[Late Join] Client rejected catch-up completion.");
		}
	}},
	{'LJAB', [](){
		LateJoinProtocol::Abort abort;
		if (!LateJoinProtocol::decodeAbort(
				net_packet->data, net_packet->len, abort)
			|| abort.playerIndex != clientnum
			|| (abort.transferId != 0
				&& (abort.transferId != g_clientLateJoinBegin.transferId
					|| abort.instanceRevision
						!= g_clientLateJoinBegin.instanceRevision)))
		{
			printlog("[Late Join] Client ignored invalid abort record.");
			return;
		}
		discardAutomatiaPersistentWorldSnapshot();
		clientResetLateJoinPacketDeferral();
		g_clientLateJoinAssembler.reset();
		g_clientLateJoinSpawnAuthorized = false;
		printlog("[Late Join] Server aborted transfer (reason %u).",
			static_cast<unsigned>(abort.reason));
	}},
	{'RJTK', [](){
		if (net_packet->len != 5 + static_cast<int>(kReconnectTokenLength)
			|| net_packet->data[4] != clientnum)
		{
			printlog("[Late Join] Client rejected malformed reconnect token.");
			return;
		}
		const std::string token(
			reinterpret_cast<const char*>(&net_packet->data[5]),
			kReconnectTokenLength);
		if (!ReconnectToken::isValid(token))
		{
			printlog("[Late Join] Client rejected invalid reconnect token.");
			return;
		}
		automatiaReconnectTokens[clientnum] = token;
		printlog("[Late Join] Client stored reconnect identity for slot %d.",
			clientnum);
	}},
	// keep alive
	{'KPAL', [](){
		client_keepalive[0] = ticks;
	}},

	// entity update
	{'ENTU', [](){
		client_keepalive[0] = ticks; // don't timeout
		const Uint8 archetype = receivedEntityArchetype();
		const Uint32 authoritativeUid = SDLNet_Read32(&net_packet->data[4]);
		const int incomingSprite = static_cast<int>(
			SDLNet_Read16(&net_packet->data[8]));
		const int incomingPlayer = static_cast<Sint32>(
			SDLNet_Read32(&net_packet->data[30]));
		Entity *entity = uidToEntity(static_cast<Sint32>(authoritativeUid));
		if (entity && authoritativePlayerUpdateConflicts(
				Entity::isPlayerHeadSprite(incomingSprite),
				incomingPlayer,
				MAXPLAYERS,
				entity->behavior == &actPlayer,
				entity->skill[2]))
		{
			const int oldSprite = entity->sprite;
			const Uint32 provisionalUid =
				moveClientEntityOutOfAuthoritativeUid(entity);
			if (provisionalUid != 0)
			{
				printlog(
					"[World State] Client moved provisional entity sprite %d from UID %u to UID %u before accepting player %d.",
					oldSprite,
					authoritativeUid,
					provisionalUid,
					incomingPlayer);
				entity = nullptr;
			}
			else
			{
				printlog(
					"[World State] Client could not resolve authoritative player %d UID %u collision.",
					incomingPlayer,
					authoritativeUid);
				return;
			}
		}
		if (!entity
			&& incomingPlayer >= 0
			&& incomingPlayer < MAXPLAYERS
			&& players[incomingPlayer])
		{
			Entity* slotHead = players[incomingPlayer]->entity;
			if (authoritativePlayerCanAdoptSlotHead(
				Entity::isPlayerHeadSprite(incomingSprite),
				incomingPlayer,
				MAXPLAYERS,
				slotHead != nullptr,
				slotHead && slotHead->mynode
					&& slotHead->mynode->list == map.entities,
				slotHead && slotHead->behavior == &actPlayer,
				slotHead ? slotHead->skill[2] : -1))
			{
				const Uint32 provisionalUid = slotHead->getUID();
				adoptAuthoritativeUidForClientPlayerHead(
					slotHead, authoritativeUid);
				entity = slotHead;
				printlog(
					"[World State] Client adopted provisional player %d head UID %u as authoritative UID %u in '%s'.",
					incomingPlayer,
					provisionalUid,
					authoritativeUid,
					worldState.activeIdentity()
						? worldState.activeIdentity()->key().c_str()
						: "unbound");
			}
		}
		if (!entity)
		{
			// Static editor fixtures can have a different provisional UID on a
			// late client. Bind the authoritative update to the fixture at the
			// same location so its authored skills are retained.
			entity = findUnboundMapFixtureForEntityUpdate(archetype);
		}
		if ( entity )
		{
			const bool preserveCustomPortalBehavior =
				entity->behavior == &actCustomPortal
				|| archetype == kEntityArchetypeCustomPortal;
			const bool preserveEditorLightBehavior =
				entity->behavior == &actLightSource
				|| archetype == kEntityArchetypeEditorLight;
			if ( (Uint32)SDLNet_Read32(&net_packet->data[36]) < (Uint32)entity->lastupdateserver )
			{
				// old packet, not used
			}
			else if ( entity->behavior == &actPlayer && entity->skill[2] == clientnum )
			{
				// don't update my player
			}
			else if ( entity->behavior == &actDeathGhost && entity->skill[2] == clientnum )
			{
				// don't update my ghost
			}
			else if ( entity->flags[NOUPDATE] )
			{
				// inform the server that it tried to update a no-update entity
				strcpy((char*)net_packet->data, "NOUP");
				net_packet->data[4] = clientnum;
				SDLNet_Write32(entity->getUID(), &net_packet->data[5]);
				net_packet->address.host = net_server.host;
				net_packet->address.port = net_server.port;
				net_packet->len = 9;
				sendPacket(net_sock, -1, net_packet, 0);
			}
			else
			{
				// receive the entity
				receiveEntity(entity);
				if (preserveCustomPortalBehavior)
				{
					// Custom exits deliberately use an editor-selected runtime
					// sprite, so their behavior cannot be recovered from the
					// sprite switch below. Keep the map-authored behavior across
					// ordinary ENTU refreshes or the portal becomes noninteractive.
					entity->behavior = &actCustomPortal;
				}
				else if (preserveEditorLightBehavior)
				{
					// The light field is client-local. Do not tear it down on every
					// ordinary position update.
					entity->behavior = &actLightSource;
				}
				else
				{
					entity->behavior = NULL;
					clientActions(entity);
				}
			}
			if (entity->behavior == &actPlayer
				&& entity->skill[2] >= 0
				&& entity->skill[2] < MAXPLAYERS
				&& players[entity->skill[2]])
			{
				const int remotePlayer = entity->skill[2];
				const bool changedInstance = !worldState.activeIdentity()
					|| !players[remotePlayer]->worldInstance.matches(
						*worldState.activeIdentity());
				players[remotePlayer]->entity = entity;
				if (worldState.placePlayer(remotePlayer, map) && changedInstance)
				{
					printlog(
						"[World State] Client placed authoritative player %d UID %u into '%s'.",
						remotePlayer,
						entity->getUID(),
						players[remotePlayer]->worldInstance.key().c_str());
				}
				ensureClientPlayerVisualInitialized(entity);
			}
			return;
		}

		if (!removedEntityTombstonesApplyToActiveInstance()
			&& removedEntities.first)
		{
			const std::size_t staleCount = list_Size(&removedEntities);
			const WorldInstanceIdentity* active = worldState.activeIdentity();
			printlog(
				"[World State] Cleared %zu source-instance entity tombstone(s) from '%s' revision %llu before accepting UID %u in '%s' revision %llu.",
				staleCount,
				g_removedEntityTombstoneInstanceKey.c_str(),
				static_cast<unsigned long long>(
					g_removedEntityTombstoneRevision),
				authoritativeUid,
				active ? active->key().c_str() : "unbound",
				static_cast<unsigned long long>(
					active ? active->revision : 0));
			list_FreeAll(&removedEntities);
			setRemovedEntityTombstoneScope(active);
		}
		for ( auto node = removedEntities.first; node != NULL; node = node->next )
		{
			auto entity2 = (Entity*)node->element;
			if ( entity2->getUID() == (int)SDLNet_Read32(&net_packet->data[4]) )
			{
				return;
			}
		}

		entity = receiveEntity(NULL);
		// IMPORTANT! Assign actions to the objects the client has control over
		if (archetype == kEntityArchetypeCustomPortal)
		{
			entity->behavior = &actCustomPortal;
			if (entity->portalCustomSprite == 0)
			{
				entity->portalCustomSprite = entity->sprite;
			}
		}
		else if (archetype == kEntityArchetypeEditorLight)
		{
			entity->behavior = &actLightSource;
		}
		else
		{
			clientActions(entity);
		}
		if (entity->behavior == &actPlayer
			&& entity->skill[2] >= 0
			&& entity->skill[2] < MAXPLAYERS
			&& players[entity->skill[2]])
		{
			const int remotePlayer = entity->skill[2];
			players[remotePlayer]->entity = entity;
			if (worldState.placePlayer(remotePlayer, map))
			{
				printlog(
					"[World State] Client created authoritative player %d UID %u in '%s'.",
					remotePlayer,
					entity->getUID(),
					players[remotePlayer]->worldInstance.key().c_str());
			}
			ensureClientPlayerVisualInitialized(entity);
		}

		//if ( entity->behavior == &actPlayer && entity->skill[2] >= 0 && entity->skill[2] < MAXPLAYERS ) // respawned
		//{
		//	if ( !players[entity->skill[2]]->entity )
		//	{
		//		players[entity->skill[2]]->entity = entity;
		//		if ( entity->skill[2] == clientnum )
		//		{
		//			stats[entity->skill[2]]->HP = stats[entity->skill[2]]->MAXHP;
		//			node_t* nextnode = nullptr;
		//			for ( auto node = map.entities->first; node != NULL; node = nextnode )
		//			{
		//				nextnode = node->next;
		//				auto entity2 = (Entity*)node->element;
		//				if ( entity2 )
		//				{
		//					if ( entity2->behavior == &actDeathCam && entity2->skill[2] == clientnum )
		//					{
		//						list_RemoveNode(entity2->mynode);
		//					}
		//				}
		//			}
		//		}
		//	}
		//}
	}},
    
    // raise/lower shield
    {'SHLD', [](){
        const int player =
        	decodeGameplayPacketPlayerIndex(
        		net_packet->data[4]
        	);
        if ( player < 0 )
        {
        	return;
        }
        stats[player]->defending = net_packet->data[5];
    }},

    // sneaking
    {'SNEK', [](){
        const int player =
        	decodeGameplayPacketPlayerIndex(
        		net_packet->data[4]
        	);
        if ( player < 0 )
        {
        	return;
        }
        stats[player]->sneaking = net_packet->data[5];
        return;
    }},

	// ghost sneaking
	{ 'GHOD', []() {
		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( player < 0 || !players[player] )
		{
			return;
		}
		if ( players[player]->ghost.my )
		{
			players[player]->ghost.my->skill[3] = net_packet->data[5] & (1 << 0);
			players[player]->ghost.my->skill[11] = net_packet->data[5] & (1 << 1) ? 1 : 0;
		}
	}},

	// update ghost bounce
	{'GHFS', []() {
		Entity* entity = uidToEntity((int)SDLNet_Read32(&net_packet->data[4]));
		if ( entity )
		{
			entity->fskill[net_packet->data[8]] = (SDLNet_Read16(&net_packet->data[9]) / 256.0);
			playSoundEntityLocal(entity, 612 + local_rng.rand() % 3, 64);
		}
	}},

	{'EFFE', [](){
		/*
		* Packet breakdown:
		* [0][1][2][3]: "EFFE"
		* [4][5][6][7]: Entity's UID.
		* [8][9][10][11][12][13][14][15]: Entity's effects.
		*/

		Uint32 uid = static_cast<int>(SDLNet_Read32(&net_packet->data[4]));

		Entity* entity = uidToEntity(uid);

		if ( entity )
		{
			if ( entity->behavior == &actPlayer && entity->skill[2] == clientnum )
			{
				//Don't update this client's entity! Use the dedicated function for that.
				return;
			}

			Stat *stats = entity->getStats();
			if ( !stats )
			{
				entity->giveClientStats();
				stats = entity->getStats();
				if ( !stats )
				{
					return;
				}
			}

			for ( int i = 0; i < NUMEFFECTS; ++i )
			{
				if ( net_packet->data[8 + i / 8] & power(2, i - (i / 8) * 8) )
				{
					stats->setEffectValueUnsafe(i, 1);
				}
				else
				{
					stats->clearEffect(i);
				}
			}

			int numBytes = NUMEFFECTS / 8;

			int numEffectStrengths = net_packet->data[8 + numBytes];
			int index = 0;
			while ( numEffectStrengths > 0 )
			{
				int currentIndex = 8 + numBytes + 1 + index;
				if ( currentIndex + 1 >= NET_PACKET_SIZE || (currentIndex + 1 >= net_packet->len) )
				{
					// too much data to read, abort
					break;
				}
				int effectIndex = net_packet->data[currentIndex + 0];
				Uint8 effectStrength = net_packet->data[currentIndex + 1];
				stats->setEffectValueUnsafe(effectIndex, effectStrength);
				index += 2;
				--numEffectStrengths;
			}
		}
	}},

	// update entity skill
	{'ENTS', [](){
		Entity *entity = uidToEntity((int)SDLNet_Read32(&net_packet->data[4]));
		if ( entity )
		{
			entity->skill[net_packet->data[8]] = SDLNet_Read32(&net_packet->data[9]);
		}
	}},

	// update entity fskill
	{'ENFS', [](){
		Entity *entity = uidToEntity((int)SDLNet_Read32(&net_packet->data[4]));
		if ( entity )
		{
			entity->fskill[net_packet->data[8]] = static_cast<Sint16>(SDLNet_Read16(&net_packet->data[9])) / 256.0;
		}
	}},

	// update entity bodypart
	{'ENTB', [](){
		if (net_packet->len < 14)
		{
			printlog("[NET]: ignored truncated ENTB packet.");
			return;
		}
		Entity *entity = uidToEntity((int)SDLNet_Read32(&net_packet->data[4]));
		if ( entity )
		{
			const int bodypart = net_packet->data[8];
			if ((entity->behavior == &actPlayer && bodypart < 1)
				|| (entity->behavior == &actMonster && bodypart < 2))
			{
				return;
			}
			node_t* childNode = list_Node(&entity->children, bodypart);
			if ( childNode && childNode->element )
			{
				Entity* tempEntity = (Entity*)childNode->element;
				tempEntity->sprite = SDLNet_Read32(&net_packet->data[9]);
				tempEntity->skill[7] = tempEntity->sprite;
				tempEntity->flags[INVISIBLE] = (net_packet->data[13] & (1 << 0)) > 0 ? true : false;
				tempEntity->flags[INVISIBLE_DITHER] = (net_packet->data[13] & (1 << 1)) > 0 ? true : false;
			}
			/*else
			{
				if ( entity->behavior == &actPlayer )
				{
					messagePlayer(clientnum, MESSAGE_DEBUG, "actPlayer !childNode: %d", net_packet->data[8]);
				}
				else
				{
					messagePlayer(clientnum, MESSAGE_DEBUG, "!childNode: %d", net_packet->data[8]);
				}
			}*/
		}
	}},

	// bodypart ids
	{'BDYI', [](){
		if (net_packet->len < 8)
		{
			printlog("[NET]: ignored truncated BDYI packet.");
			return;
		}
		Entity *entity = uidToEntity((int)SDLNet_Read32(&net_packet->data[4]));
		if ( entity )
		{
			const std::size_t expectedLength = bodypartIdPacketLength(
				list_Size(&entity->children),
				entity->behavior == &actMonster);
			if (!bodypartIdPacketIsComplete(
				static_cast<std::size_t>(net_packet->len),
				list_Size(&entity->children),
				entity->behavior == &actMonster))
			{
				printlog(
					"[NET]: ignored truncated BDYI packet for UID %u (%d of %zu bytes).",
					entity->getUID(),
					net_packet->len,
					expectedLength);
				return;
			}
			node_t* childNode;
			int c;
			for ( c = 0, childNode = entity->children.first; childNode != nullptr; childNode = childNode->next, c++ )
			{
				if ( c < 1 || (c < 2 && entity->behavior == &actMonster) )
				{
					continue;
				}
				Entity* tempEntity = (Entity*)childNode->element;
				if ( tempEntity )
				{
					if ( entity->behavior == &actMonster )
					{
						tempEntity->setUID(SDLNet_Read32(&net_packet->data[8 + 4 * (c - 2)]));
					}
					else
					{
						tempEntity->setUID(SDLNet_Read32(&net_packet->data[8 + 4 * (c - 1)]));
					}
				}
			}
		}
	}},

	// update entity flag
	{'ENTF', [](){
		Entity *entity = uidToEntity((int)SDLNet_Read32(&net_packet->data[4]));
		if ( entity )
		{
			entity->flags[net_packet->data[8]] = net_packet->data[9];
			if ( entity->behavior == &actMonster && net_packet->data[8] == USERFLAG2 )
			{
				// we should update the flags for all bodyparts (except for human and automaton heads, don't update the other bodyparts).
				if ( !(entity->isPlayerHeadSprite() || entity->sprite == 467 || !monsterChangesColorWhenAlly(nullptr, entity)) )
				{
					int bodypart = 0;
					for ( node_t* node = entity->children.first; node != nullptr; node = node->next )
					{
						if ( bodypart >= LIMB_HUMANOID_TORSO )
						{
							Entity* tmp = (Entity*)node->element;
							if ( tmp )
							{
								tmp->flags[USERFLAG2] = entity->flags[net_packet->data[8]];
							}
						}
						++bodypart;
					}
				}
			}
		}
	}},

	// player movement correction
	{'PMOV', [](){
		if ( players[clientnum] == nullptr || players[clientnum]->entity == nullptr )
		{
			return;
		}
		players[clientnum]->entity->x = ((Sint16)SDLNet_Read16(&net_packet->data[4])) / 32.0;
		players[clientnum]->entity->y = ((Sint16)SDLNet_Read16(&net_packet->data[6])) / 32.0;
	}},

	// player ghost movement correction
	{'GMOV', []() {
		if ( players[clientnum] == nullptr || players[clientnum]->ghost.my == nullptr )
		{
			return;
		}
		players[clientnum]->ghost.my->x = ((Sint16)SDLNet_Read16(&net_packet->data[4])) / 32.0;
		players[clientnum]->ghost.my->y = ((Sint16)SDLNet_Read16(&net_packet->data[6])) / 32.0;
	}},

	// update health
	{'UPHP', [](){
		if ( (Monster)SDLNet_Read32(&net_packet->data[8]) != NOTHING )
		{
			if ( SDLNet_Read32(&net_packet->data[4]) < stats[clientnum]->HP )
			{
				cameravars[clientnum].shakex += .1;
				cameravars[clientnum].shakey += 10;
			}
			else
			{
				cameravars[clientnum].shakex += .05;
				cameravars[clientnum].shakey += 5;
			}
		}
		stats[clientnum]->HP = SDLNet_Read32(&net_packet->data[4]);
		return;
	}},

    // server sent item details.
    {'ITMU', [](){
        if ( net_packet->len < 16 )
        {
            printlog(
                "[NET]: ignoring malformed ITMU packet with length %d.\n",
                net_packet->len
            );
            return;
        }

        Uint32 uid = SDLNet_Read32(&net_packet->data[4]);
        Entity* entity = uidToEntity(uid);
        if ( entity )
        {
            Uint32 itemTypeAndIdentified = SDLNet_Read32(&net_packet->data[8]);
            Uint32 statusBeatitudeQuantityAppearance = SDLNet_Read32(&net_packet->data[12]);
            int resolvedType =
                static_cast<int>((itemTypeAndIdentified >> 16) & 0xFFFF);
            bool hasSAMStableIdPayload = false;

#ifdef SAM_FRAMEWORK_ENABLED
            constexpr int samWorldItemTypeSentinel = 0xFFFF;
            if ( resolvedType == samWorldItemTypeSentinel )
            {
                hasSAMStableIdPayload = true;
                const int stableIdOffset = 16;
                const int payloadLength =
                    net_packet->len - stableIdOffset;
                int stableIdLength = 0;
                while ( stableIdLength < payloadLength
                    && net_packet->data[stableIdOffset + stableIdLength] != '\0' )
                {
                    ++stableIdLength;
                }

                if ( stableIdLength <= 0
                    || stableIdLength >= payloadLength )
                {
                    printlog(
                        "[S.A.M] Refusing malformed ITMU stable-id payload for entity %u.\n",
                        uid
                    );
                    return;
                }

                const std::string stableId(
                    reinterpret_cast<const char*>(
                        &net_packet->data[stableIdOffset]
                    ),
                    stableIdLength
                );

                resolvedType =
                    SAMItemRegistryFoundation::
                        runtimeIdForStableId(stableId);
                if ( resolvedType < 0
                    || !SAMItemRegistryFoundation::
                        isRegisteredRuntimeItemId(resolvedType) )
                {
                    printlog(
                        "[S.A.M] ITMU world item unavailable locally: [%s]. Entity %u rejected.\n",
                        stableId.c_str(),
                        uid
                    );
                    entity->flags[INVISIBLE] = true;
                    entity->itemReceivedDetailsFromServer = 0;
                    return;
                }

                printlog(
                    "[S.A.M] Resolved ITMU world item [%s] to local runtime %d for entity %u.\n",
                    stableId.c_str(),
                    resolvedType,
                    uid
                );
            }
            else if ( SAMItemRegistryFoundation::
                isSAMRuntimeItemId(resolvedType) )
            {
                printlog(
                    "[S.A.M] Refusing numeric-only ITMU custom runtime %d for entity %u.\n",
                    resolvedType,
                    uid
                );
                entity->flags[INVISIBLE] = true;
                entity->itemReceivedDetailsFromServer = 0;
                return;
            }
#endif

            entity->skill[10] = resolvedType;
            entity->skill[15] = (itemTypeAndIdentified) & 0xFFFF;
			entity->skill[11] = static_cast<Uint8>((statusBeatitudeQuantityAppearance >> 24) & 0xFF); // status
			entity->skill[12] = static_cast<Sint8>((statusBeatitudeQuantityAppearance >> 16) & 0xFF); // beatitude
			entity->skill[13] = static_cast<Uint8>((statusBeatitudeQuantityAppearance >> 8) & 0xFF); // quantity
			entity->skill[14] = static_cast<Uint8>((statusBeatitudeQuantityAppearance) & 0xFF); // appearance
            if ( !hasSAMStableIdPayload && net_packet->len >= 18 )
            {
                if ( entity->skill[10] >= 0 && entity->skill[10] < NUMITEMS )
				{
					if ( items[entity->skill[10]].category == TOME_SPELL )
					{
						entity->skill[14] = SDLNet_Read16(&net_packet->data[16]);
					}
				}
			}
			entity->itemReceivedDetailsFromServer = 1;
		}
	}},

	// breakable dropped item
	{ 'BREK', []() {
		Uint32 uid = SDLNet_Read32(&net_packet->data[4]);
		Entity* entity = uidToEntity(uid);
		if ( entity )
		{
			if ( entity->behavior == &actItem )
			{
				//entity->flags[UPDATENEEDED] = true;
				if ( entity->flags[INVISIBLE] )
				{
					entity->flags[INVISIBLE] = false;
					entity->vel_x = (0.25 + .025 * (local_rng.rand() % 11)) * cos(entity->yaw);
					entity->vel_y = (0.25 + .025 * (local_rng.rand() % 11)) * sin(entity->yaw);
					entity->vel_z = (-40 - local_rng.rand() % 5) * .01;
					entity->itemContainer = 0;
					entity->z = 0.0;
					entity->itemNotMoving = 0;
					entity->itemNotMovingClient = 0;
					entity->flags[USERFLAG1] = false; // enable collision
				}
			}
			else if ( entity->behavior == &actGoldBag )
			{
				if ( entity->flags[INVISIBLE] )
				{
					entity->vel_x = (0.25 + .025 * (local_rng.rand() % 11)) * cos(entity->yaw);
					entity->vel_y = (0.25 + .025 * (local_rng.rand() % 11)) * sin(entity->yaw);
					entity->vel_z = (-40 - local_rng.rand() % 10) * .01;
					entity->goldBouncing = 0;
					entity->z = 0.0 - (local_rng.rand() % 3);
					entity->flags[INVISIBLE] = false;
				}
			}
		}
	}},

	{ 'DAED', []() {
		Uint32 uid = SDLNet_Read32(&net_packet->data[4]);
		if ( Entity* shrine = uidToEntity(uid) )
		{
			if ( shrine->behavior == &::actDaedalusShrine )
			{
				daedalusShrineInteract(shrine, nullptr);
			}
		}
	}},

	// bell dropped item
	{ 'BELI', []() {
		Uint32 uid = SDLNet_Read32(&net_packet->data[4]);
		Entity* entity = uidToEntity(uid);
		if ( entity )
		{
			if ( entity->behavior == &actItem )
			{
				//entity->flags[UPDATENEEDED] = true;
				if ( entity->flags[INVISIBLE] )
				{
					playSoundEntityLocal(entity, 47 + local_rng.rand() % 3, 64);
					entity->flags[INVISIBLE] = false;
					entity->vel_x = 0.0; //(0.25 + .025 * (local_rng.rand() % 11)) * cos(entity->yaw);
					entity->vel_y = 0.0; //(0.25 + .025 * (local_rng.rand() % 11)) * sin(entity->yaw);
					entity->vel_z = (-2 - local_rng.rand() % 5) * .01;
					entity->itemContainer = 0;
					entity->z = -16;
					entity->itemNotMoving = 0;
					entity->itemNotMovingClient = 0;
					entity->flags[USERFLAG1] = false; // enable collision
				}
			}
			else if ( entity->behavior == &actGoldBag )
			{
				if ( entity->flags[INVISIBLE] )
				{
					playSoundEntityLocal(entity, 242 + local_rng.rand() % 4, 64);
					entity->vel_x = 0.0;
					entity->vel_y = 0.0;
					entity->vel_z = (-2 - local_rng.rand() % 5) * .01;
					entity->goldBouncing = 0;
					entity->z = -16;
					entity->flags[INVISIBLE] = false;
				}
			}
		}
	} },

	// ghost interact item
	{ 'GHOI', []() {
		Uint32 uid = SDLNet_Read32(&net_packet->data[4]);
		Entity* entity = uidToEntity(uid);
		if ( entity )
		{
			entity->itemNotMoving = 0;
			entity->itemNotMovingClient = 0;
			entity->flags[USERFLAG1] = false; // enable collision

			entity->x = ((Sint16)SDLNet_Read16(&net_packet->data[8])) / 32.0;
			entity->y = ((Sint16)SDLNet_Read16(&net_packet->data[10])) / 32.0;
			entity->z = ((Sint16)SDLNet_Read16(&net_packet->data[12])) / 32.0;
			
			entity->vel_x = ((Sint16)SDLNet_Read16(&net_packet->data[14])) / 32.0;
			entity->vel_y = ((Sint16)SDLNet_Read16(&net_packet->data[16])) / 32.0;
			entity->vel_z = ((Sint16)SDLNet_Read16(&net_packet->data[18])) / 32.0;
		}
	}},

	// attract item
	{ 'ATTI', []() {
		Uint32 uid = SDLNet_Read32(&net_packet->data[4]);
		Entity* entity = uidToEntity(uid);
		if ( entity )
		{
			entity->itemNotMoving = 0;
			entity->itemNotMovingClient = 0;
			entity->flags[USERFLAG1] = false; // enable collision
			entity->flags[UPDATENEEDED] = true;
			entity->flags[NOUPDATE] = false;

			entity->itemFollowUID = ((Uint32)SDLNet_Read32(&net_packet->data[20]));

			entity->x = ((Sint16)SDLNet_Read16(&net_packet->data[8])) / 32.0;
			entity->y = ((Sint16)SDLNet_Read16(&net_packet->data[10])) / 32.0;
			entity->z = ((Sint16)SDLNet_Read16(&net_packet->data[12])) / 32.0;
			entity->new_z = entity->z;
			entity->itemLevitate = 1.0;
			entity->itemLevitateStartZ = entity->z;

			entity->vel_x = ((Sint16)SDLNet_Read16(&net_packet->data[14])) / 32.0;
			entity->vel_y = ((Sint16)SDLNet_Read16(&net_packet->data[16])) / 32.0;
			entity->vel_z = ((Sint16)SDLNet_Read16(&net_packet->data[18])) / 32.0;
		}
	} },

	// spawn an explosion
	{'EXPL', [](){
		Sint16 x = (Sint16)SDLNet_Read16(&net_packet->data[4]);
		Sint16 y = (Sint16)SDLNet_Read16(&net_packet->data[6]);
		Sint16 z = (Sint16)SDLNet_Read16(&net_packet->data[8]);
		spawnExplosion(x, y, z);
	}},

	// spawn an explosion, custom sprite
	{'EXPS', [](){
		Uint16 sprite = (Uint16)SDLNet_Read16(&net_packet->data[4]);
		Sint16 x = (Sint16)SDLNet_Read16(&net_packet->data[6]);
		Sint16 y = (Sint16)SDLNet_Read16(&net_packet->data[8]);
		Sint16 z = (Sint16)SDLNet_Read16(&net_packet->data[10]);
		spawnExplosionFromSprite(sprite, x, y, z);
	}},

	// spawn a bang sprite
	{'BANG', [](){
		Sint16 x = (Sint16)SDLNet_Read16(&net_packet->data[4]);
		Sint16 y = (Sint16)SDLNet_Read16(&net_packet->data[6]);
		Sint16 z = (Sint16)SDLNet_Read16(&net_packet->data[8]);
		spawnBang(x, y, z);
	}},

	// spawn a gib
	{'SPGB', [](){
		Sint16 x = (Sint16)SDLNet_Read16(&net_packet->data[4]);
		Sint16 y = (Sint16)SDLNet_Read16(&net_packet->data[6]);
		Sint16 z = (Sint16)SDLNet_Read16(&net_packet->data[8]);
		Sint16 sprite = (Sint16)SDLNet_Read16(&net_packet->data[10]);
		Entity* gib = spawnGibClient(x, y, z, sprite);
		gib->flags[SPRITE] = net_packet->data[12] & (1 << 0);
		gib->skill[5] = net_packet->data[12] & (1 << 1); // poof
		if ( !spawn_blood && !gib->flags[SPRITE] && gib->sprite != 5 )
		{
			gib->flags[INVISIBLE] = true;
		}
	}},

	// spawn a sleep Z
	{'SLEZ', [](){
		Sint16 x = (Sint16)SDLNet_Read16(&net_packet->data[4]);
		Sint16 y = (Sint16)SDLNet_Read16(&net_packet->data[6]);
		Sint16 z = (Sint16)SDLNet_Read16(&net_packet->data[8]);
		spawnSleepZ(x, y, z);
	}},

	// spawn a poof
	{ 'PUFF', []() {
		Sint16 x = (Sint16)SDLNet_Read16(&net_packet->data[4]);
		Sint16 y = (Sint16)SDLNet_Read16(&net_packet->data[6]);
		Sint16 z = (Sint16)SDLNet_Read16(&net_packet->data[8]);
		Uint16 scale = (Uint16)SDLNet_Read16(&net_packet->data[10]);
		Entity* poof = spawnPoof(x, y, z, scale / 100.0);
	}},

	// spawn a misc sprite like the sleep Z
	{'SLEM', [](){
		Sint16 x = (Sint16)SDLNet_Read16(&net_packet->data[4]);
		Sint16 y = (Sint16)SDLNet_Read16(&net_packet->data[6]);
		Sint16 z = (Sint16)SDLNet_Read16(&net_packet->data[8]);
		Sint16 sprite = (Sint16)SDLNet_Read16(&net_packet->data[10]);
		spawnFloatingSpriteMisc(sprite, x, y, z);
	}},

	// spawn magical effect particles
	{'MAGE', [](){
		Sint16 x = (Sint16)SDLNet_Read16(&net_packet->data[4]);
		Sint16 y = (Sint16)SDLNet_Read16(&net_packet->data[6]);
		Sint16 z = (Sint16)SDLNet_Read16(&net_packet->data[8]);
		Uint32 sprite = (Uint32)SDLNet_Read32(&net_packet->data[10]);
		spawnMagicEffectParticles(x, y, z, sprite);
	}},

	// spawn magical bell effect particles
	{ 'MAGB', []() {
		Uint32 uid = (Uint32)SDLNet_Read32(&net_packet->data[4]);
		if ( Entity* entity = uidToEntity(uid) )
		{
			Uint32 sprite = (Uint32)SDLNet_Read32(&net_packet->data[8]);
			spawnMagicEffectParticlesBell(entity, sprite);
		}
	} },

	// spawn misc particle effect 
	{'SPPE', [](){
		Entity *entity = uidToEntity((int)SDLNet_Read32(&net_packet->data[4]));
		if ( entity )
		{
			int particleType = static_cast<int>(net_packet->data[8]);
			int sprite = static_cast<int>(SDLNet_Read16(&net_packet->data[9]));
			switch ( particleType )
			{
				case PARTICLE_EFFECT_ABILITY_PURPLE:
					createParticleDot(entity);
					break;
				case PARTICLE_EFFECT_ABILITY_ROCK:
					createParticleRock(entity, sprite);
					break;
				case PARTICLE_EFFECT_SPIN:
					createParticleSpin(entity);
					break;
				case PARTICLE_EFFECT_SHATTERED_GEM:
					createParticleShatteredGem(entity->x, entity->y, 7.5, sprite, entity);
					break;
				case PARTICLE_EFFECT_SHADOW_INVIS:
					createParticleDropRising(entity, sprite, 1.0);
					break;
				case PARTICLE_EFFECT_INCUBUS_TELEPORT_STEAL:
				{
					Entity* spellTimer = createParticleTimer(entity, 80, sprite);
					spellTimer->particleTimerCountdownAction = PARTICLE_TIMER_ACTION_SHOOT_PARTICLES;
					spellTimer->particleTimerCountdownSprite = sprite;
					spellTimer->particleTimerPreDelay = 40;
				}
				break;
				case PARTICLE_EFFECT_INCUBUS_TELEPORT_TARGET:
				{
					Entity* spellTimer = createParticleTimer(entity, 40, sprite);
					spellTimer->particleTimerCountdownAction = PARTICLE_TIMER_ACTION_SHOOT_PARTICLES;
					spellTimer->particleTimerCountdownSprite = sprite;
				}
				break;
				case PARTICLE_EFFECT_SHADOW_TELEPORT:
				{
					Entity* spellTimer = createParticleTimer(entity, 40, sprite);
					spellTimer->particleTimerCountdownAction = PARTICLE_TIMER_ACTION_SHOOT_PARTICLES;
					spellTimer->particleTimerCountdownSprite = sprite;
				}
				break;
				case PARTICLE_EFFECT_SHRINE_TELEPORT:
				{
					Entity* spellTimer = createParticleTimer(entity, 200, sprite);
					spellTimer->particleTimerCountdownAction = PARTICLE_TIMER_ACTION_SHOOT_PARTICLES;
					spellTimer->particleTimerCountdownSprite = sprite;
					spellTimer->particleTimerPreDelay = 0;
				}
				break;
				case PARTICLE_EFFECT_DESTINY_TELEPORT:
				{
					Uint32 duration = SDLNet_Read32(&net_packet->data[11]);
					Entity* spellTimer = createParticleTimer(entity, duration, sprite);
					spellTimer->particleTimerCountdownAction = PARTICLE_TIMER_ACTION_SHOOT_PARTICLES;
					spellTimer->particleTimerCountdownSprite = sprite;
					spellTimer->particleTimerPreDelay = 0;
				}
				break;
				case PARTICLE_EFFECT_TELEPORT_PULL:
				{
					Entity* spellTimer = createParticleTimer(entity, 40, sprite);
					spellTimer->particleTimerCountdownAction = PARTICLE_TIMER_ACTION_SHOOT_PARTICLES;
					spellTimer->particleTimerCountdownSprite = sprite;
				}
				break;
				case PARTICLE_EFFECT_ERUPT:
					createParticleErupt(entity, sprite);
					break;
				case PARTICLE_EFFECT_VAMPIRIC_AURA:
					createParticleDropRising(entity, sprite, 0.5);
					break;
				case PARTICLE_EFFECT_RISING_DROP:
					createParticleDropRising(entity, sprite, 1.0);
					break;
				case PARTICLE_EFFECT_CHARM_MONSTER:
					createParticleCharmMonster(entity);
					break;
				case PARTICLE_EFFECT_SHADOW_TAG:
				{
					Uint32 uid = SDLNet_Read32(&net_packet->data[11]);
					createParticleShadowTag(entity, uid, 60 * TICKS_PER_SECOND);
					break;
				}
				case PARTICLE_EFFECT_PINPOINT:
				{
					Uint32 uid = SDLNet_Read32(&net_packet->data[11]);
					if ( sprite >= PINPOINT_PARTICLE_START && sprite < PINPOINT_PARTICLE_END )
					{
						if ( net_packet->len >= 23 )
						{
							int duration = SDLNet_Read32(&net_packet->data[15]);
							int spellID = SDLNet_Read32(&net_packet->data[19]);
							createParticleSpellPinpointTarget(entity, uid, sprite, duration, spellID);
						}
					}
					break;
				}
				case PARTICLE_EFFECT_REVENANT_CURSE:
				{
					int duration = SDLNet_Read32(&net_packet->data[15]);
					if ( Entity* fx = createParticleAestheticOrbit(entity, sprite, duration, PARTICLE_EFFECT_REVENANT_CURSE) )
					{
						fx->z = 7.5;
						fx->yaw = entity->yaw;
						fx->ditheringOverride = 6;
					}
					break;
				}
				case PARTICLE_EFFECT_SPELL_WEB_ORBIT:
					createParticleAestheticOrbit(entity, 863, 400, PARTICLE_EFFECT_SPELL_WEB_ORBIT);
					break;
				case PARTICLE_EFFECT_SMITE_PINPOINT:
				{
					for ( int i = 0; i < 3; ++i )
					{
						Entity* fx1 = createParticleAestheticOrbit(entity, 2401, 2 * TICKS_PER_SECOND, PARTICLE_EFFECT_SMITE_PINPOINT);
						fx1->yaw = entity->yaw + PI / 2 + 2 * i * PI / 3;
						fx1->fskill[4] = entity->x;
						fx1->fskill[5] = entity->y;
						fx1->x = entity->x;
						fx1->y = entity->y;
						fx1->fskill[6] = fx1->yaw;
						fx1->skill[3] = 0;
						if ( i != 0 )
						{
							fx1->actmagicNoLight = 1;
						}
					}
					break;
				}
				case PARTICLE_EFFECT_TURN_UNDEAD:
				{
					if ( Entity* fx1 = createParticleAestheticOrbit(entity, 2401, 3 * TICKS_PER_SECOND, PARTICLE_EFFECT_TURN_UNDEAD) )
					{
						fx1->yaw = entity->yaw;
						fx1->fskill[4] = entity->x;
						fx1->fskill[5] = entity->y;
						fx1->x = entity->x;
						fx1->y = entity->y;
						fx1->fskill[6] = fx1->yaw;
						fx1->skill[3] = 0;
					}
					break;
				}
				case PARTICLE_EFFECT_HOLY_FIRE:
				{
					int duration = SDLNet_Read32(&net_packet->data[15]);
					if ( Entity* fx = createParticleAestheticOrbit(entity, 288, duration, PARTICLE_EFFECT_HOLY_FIRE) )
					{
						fx->flags[SPRITE] = true;
						fx->flags[INVISIBLE] = true;
					}
					break;
				}
				case PARTICLE_EFFECT_DEFY_FLESH_ORBIT:
				{
					int duration = SDLNet_Read32(&net_packet->data[15]);
					if ( Entity* fx = createParticleAestheticOrbit(entity, 2363, duration, PARTICLE_EFFECT_DEFY_FLESH_ORBIT) )
					{
						fx->flags[INVISIBLE] = true;
					}
					break;
				}
				case PARTICLE_EFFECT_DEFY_FLESH:
				{
					int duration = SDLNet_Read32(&net_packet->data[15]);
					Sint32 dir = SDLNet_Read32(&net_packet->data[19]);
					if ( Entity* fx = createParticleAestheticOrbit(entity, 2363, duration, PARTICLE_EFFECT_DEFY_FLESH) )
					{
						fx->yaw = dir / 256.0;
						fx->flags[INVISIBLE] = true;

						fx->pitch = PI / 2;
						fx->fskill[0] = fx->yaw;
						fx->fskill[1] = PI / 4 - PI / 8;
						fx->fskill[2] = entity->z;
						fx->x = entity->x - 8.0 * cos(fx->yaw);
						fx->y = entity->y - 8.0 * sin(fx->yaw);
						fx->z = entity->z;
						fx->scalex = 0.0;
						fx->scaley = 0.0;
						fx->scalez = 0.0;
					}
					break;
				}
				case PARTICLE_EFFECT_PSYCHIC_SPEAR:
				{
					int duration = SDLNet_Read32(&net_packet->data[15]);
					Sint32 dir = SDLNet_Read32(&net_packet->data[19]);
					if ( Entity* fx = createParticleAestheticOrbit(entity, 2362, duration, PARTICLE_EFFECT_PSYCHIC_SPEAR) )
					{
						fx->yaw = dir / 256.0;
						//fx->skill[3] = spell->caster;
						fx->pitch = 0;// PI / 4;
						fx->fskill[0] = fx->yaw + PI / 2 + (local_rng.rand() % 6) * PI / 3;
						fx->fskill[1] = PI / 4 + PI / 8;// +(i + 1) * 2 * PI / 3;
						fx->x = entity->x - 8.0 * cos(fx->yaw);
						fx->y = entity->y - 8.0 * sin(fx->yaw);
						fx->z = entity->z;// -8.0;
						fx->scalex = 0.0;
						fx->scaley = 0.0;
						fx->scalez = 0.0;
					}
					break;
				}
				case PARTICLE_EFFECT_FOCI_LIGHT:
				{
					createParticleFociLight(entity, sprite, false);
					break;
				}
				case PARTICLE_EFFECT_FOCI_DARK:
				{
					createParticleFociDark(entity, sprite, false);
					break;
				}
				case PARTICLE_EFFECT_PORTAL_SPAWN:
				{
					Entity* spellTimer = createParticleTimer(entity, 100, sprite);
					spellTimer->particleTimerCountdownAction = PARTICLE_TIMER_ACTION_SPAWN_PORTAL;
					spellTimer->particleTimerCountdownSprite = 174;
					spellTimer->particleTimerEndAction = PARTICLE_EFFECT_PORTAL_SPAWN;
				}
				break;
				case PARTICLE_EFFECT_LICHFIRE_TELEPORT_STATIONARY:
				case PARTICLE_EFFECT_LICHICE_TELEPORT_STATIONARY:
				case PARTICLE_EFFECT_LICH_TELEPORT_ROAMING:
				{
					Entity* spellTimer = createParticleTimer(entity, 40, sprite);
					spellTimer->particleTimerCountdownAction = PARTICLE_TIMER_ACTION_SHOOT_PARTICLES;
					spellTimer->particleTimerCountdownSprite = sprite;
				}
				break;
				case PARTICLE_EFFECT_SLIME_SPRAY:
				{
					Entity* spellTimer = createParticleTimer(entity, 30, -1);
					spellTimer->particleTimerCountdownAction = PARTICLE_TIMER_ACTION_MAGIC_SPRAY;
					spellTimer->particleTimerCountdownSprite = sprite;
				}
				break;
				case PARTICLE_EFFECT_PLAYER_AUTOMATON_DEATH:
					createParticleExplosionCharge(entity, 174, 100, 0.25);
					if ( entity && entity->behavior == &actPlayer )
					{
						if ( entity->getMonsterTypeFromSprite() == AUTOMATON )
						{
							entity->playerAutomatonDeathCounter = 1;
							if ( entity->skill[2] == clientnum )
							{
								// this is me dying, setup the deathcam.
								entity->playerCreatedDeathCam = 1;
								Entity* entity = newEntity(-1, 1, map.entities, nullptr);
								entity->x = cameras[clientnum].x * 16;
								entity->y = cameras[clientnum].y * 16;
								entity->z = -2;
								entity->flags[NOUPDATE] = true;
								entity->flags[PASSABLE] = true;
								entity->flags[INVISIBLE] = true;
								entity->behavior = &actDeathCam;
								entity->skill[2] = clientnum;
								entity->yaw = cameras[clientnum].ang;
								entity->pitch = PI / 8;
								players[clientnum]->ghost.initTeleportLocations(entity->x / 16, entity->y / 16);
							}
						}
					}
					break;
				case PARTICLE_EFFECT_ENSEMBLE_OTHER_CAST:
					createEnsembleTargetParticleCircling(entity);
					break;
				case PARTICLE_EFFECT_ENSEMBLE_SELF_CAST:
					createEnsembleHUDParticleCircling(entity);
					break;
				case PARTICLE_EFFECT_IGNITE:
					createParticleIgnite(entity);
					break;
				case PARTICLE_EFFECT_SHATTER_OBJECTS:
					createParticleShatterObjects(entity);
					break;
				case PARTICLE_EFFECT_LIGHTNING_SEQ:
					floorMagicCreateLightningSequence(entity, entity->ticks + 1);
					break;
				case PARTICLE_EFFECT_STATIC_ORBIT:
				{
					Entity* fx = createParticleAestheticOrbit(entity, sprite, 2 * TICKS_PER_SECOND, PARTICLE_EFFECT_STATIC_ORBIT);
					fx->z = 7.5;
					fx->actmagicOrbitDist = 20;
					fx->actmagicNoLight = 1;
					break;
				}
				case PARTICLE_EFFECT_STATIC_MAXIMISE:
				{
					for ( int i = 0; i < 3; ++i )
					{
						Entity* fx = createParticleAestheticOrbit(entity, sprite, 2 * TICKS_PER_SECOND, PARTICLE_EFFECT_STATIC_ORBIT);
						fx->z = 7.5 - 2.0 * i;
						fx->scalex = 1.0;
						fx->scaley = 1.0;
						fx->scalez = 1.0;
						fx->actmagicOrbitDist = 20;
						fx->yaw += i * 2 * PI / 3;
						fx->actmagicNoLight = (i == 0 ? 0 : 1);
					}
					break;
				}
				case PARTICLE_EFFECT_CONTROL:
					for ( int i = 0; i < 4; ++i )
					{
						Entity* fx = spawnMagicParticle(entity);
						fx->sprite = sprite;
						fx->yaw = entity->yaw + i * PI / 2;
						fx->scalex = 0.7;
						fx->scaley = fx->scalex;
						fx->scalez = fx->scalex;
						fx->vel_x = 0.5 * cos(entity->yaw + i * PI / 2);
						fx->vel_y = 0.5 * sin(entity->yaw + i * PI / 2);
					}
					break;
				case PARTICLE_EFFECT_FLAMES:
				{
					int duration = SDLNet_Read32(&net_packet->data[15]);
					if( Entity* fx = createParticleAestheticOrbit(entity, 233, duration, PARTICLE_EFFECT_IGNITE_ORBIT))
					{
						fx->flags[SPRITE] = true;
						fx->x = entity->x;
						fx->y = entity->y;
						fx->fskill[0] = fx->x;
						fx->fskill[1] = fx->y;
						fx->vel_z = -0.05;
						fx->actmagicOrbitDist = 2;
						fx->fskill[2] = entity->yaw + (local_rng.rand() % 8) * PI / 4.0;
						fx->yaw = fx->fskill[2];
						fx->actmagicNoLight = 1;
					}
					break;
				}
				case PARTICLE_EFFECT_HEAT_ORBIT_SPIN:
				{
					Uint32 particle = SDLNet_Read32(&net_packet->data[11]);
					int duration = SDLNet_Read32(&net_packet->data[15]);
					for ( int i = 0; i < 2; ++i )
					{
						if ( Entity* fx = createParticleAestheticOrbit(entity, sprite, duration, PARTICLE_EFFECT_IGNITE_ORBIT) )
						{
							fx->flags[SPRITE] = true;
							fx->x = entity->x;
							fx->y = entity->y;
							fx->z = 7.5;
							fx->fskill[0] = fx->x;
							fx->fskill[1] = fx->y;
							fx->vel_z = -0.5;
							fx->actmagicOrbitDist = 5;
							fx->fskill[2] = entity->yaw + PI / 4.0 + i * PI;
							fx->yaw = fx->fskill[2];
							fx->fskill[4] = 0.25;
							if ( particle == 1 )
							{
								fx->lightBonus = vec4{ 0.f, 0.f, 0.f, 0.f };
								fx->actmagicNoLight = 1;
							}
						}
					}
					break;
				}
				case PARTICLE_EFFECT_SUMMON_FLAMES:
				{
					int duration = SDLNet_Read32(&net_packet->data[15]);
					for ( int i = 0; i < 3; ++i )
					{
						if ( Entity* fx = createParticleAestheticOrbit(entity, 233, duration, PARTICLE_EFFECT_IGNITE_ORBIT) )
						{
							fx->flags[SPRITE] = true;
							fx->x = entity->x;
							fx->y = entity->y;
							fx->fskill[0] = fx->x;
							fx->fskill[1] = fx->y;
							fx->z = -7.5;
							fx->vel_z = 0.25;
							fx->actmagicOrbitDist = 4;
							fx->fskill[2] = entity->yaw + (i) * 2 * PI / 3.0;
							fx->yaw = fx->fskill[2];
							fx->actmagicNoLight = 1;

						}
					}
					break;
				}
				case PARTICLE_EFFECT_BOLAS:
				{
					Uint32 duration = SDLNet_Read32(&net_packet->data[15]);
					createParticleBolas(entity, sprite, duration, nullptr);
				}
				break;
				default:
					break;
			}
		}
	}},

	// spawn misc particle effect at fixed location
	{'SPPL', [](){
		Sint16 particle_x = static_cast<Sint16>(SDLNet_Read16(&net_packet->data[4]));
		Sint16 particle_y = static_cast<Sint16>(SDLNet_Read16(&net_packet->data[6]));
		Sint16 particle_z = static_cast<Sint16>(SDLNet_Read16(&net_packet->data[8]));
		int particleType = static_cast<int>(net_packet->data[10]);
		int sprite = static_cast<int>(SDLNet_Read16(&net_packet->data[11]));
		//messagePlayer(1, "recv, %d, %d, %d, type: %d", particle_x, particle_y, particle_z, particleType);
		switch ( particleType )
		{
			case PARTICLE_EFFECT_SUMMON_MONSTER:
			{
				Entity* spellTimer = createParticleTimer(nullptr, 70, sprite);
				spellTimer->particleTimerCountdownAction = PARTICLE_TIMER_ACTION_SUMMON_MONSTER;
				spellTimer->particleTimerCountdownSprite = 174;
				spellTimer->particleTimerEndAction = PARTICLE_EFFECT_SUMMON_MONSTER;
				spellTimer->x = particle_x * 16.0 + 8;
				spellTimer->y = particle_y * 16.0 + 8;
				spellTimer->z = particle_z;
			}
			break;
			case PARTICLE_EFFECT_DEVIL_SUMMON_MONSTER:
			{
				Entity* spellTimer = createParticleTimer(nullptr, 70, sprite);
				spellTimer->particleTimerCountdownAction = PARTICLE_TIMER_ACTION_DEVIL_SUMMON_MONSTER;
				spellTimer->particleTimerCountdownSprite = 174;
				spellTimer->particleTimerEndAction = PARTICLE_EFFECT_SUMMON_MONSTER;
				spellTimer->x = particle_x * 16.0 + 8;
				spellTimer->y = particle_y * 16.0 + 8;
				spellTimer->z = particle_z;
			}
			break;
			case PARTICLE_EFFECT_SPELL_SUMMON:
			{
				Entity* spellTimer = createParticleTimer(nullptr, 55, sprite);
				spellTimer->particleTimerCountdownSprite = 791;
				spellTimer->particleTimerCountdownAction = PARTICLE_TIMER_ACTION_SPELL_SUMMON;
				spellTimer->particleTimerPreDelay = 40;
				spellTimer->particleTimerEndAction = PARTICLE_EFFECT_SPELL_SUMMON;
				spellTimer->x = particle_x * 16.0 + 8;
				spellTimer->y = particle_y * 16.0 + 8;
				spellTimer->z = particle_z;
			}
			break;
			case PARTICLE_EFFECT_TELEPORT_PULL_TARGET_LOCATION:
			{
				Entity* spellTimer = createParticleTimer(nullptr, 40, 593);
				spellTimer->particleTimerCountdownAction = PARTICLE_TIMER_TELEPORT_PULL_TARGET_LOCATION;
				spellTimer->particleTimerCountdownSprite = 593;
				spellTimer->x = particle_x * 16.0 + 8;
				spellTimer->y = particle_y * 16.0 + 8;
				spellTimer->z = particle_z;
				spellTimer->flags[PASSABLE] = false;
				spellTimer->sizex = 4;
				spellTimer->sizey = 4;
			}
			break;
			case PARTICLE_EFFECT_SHATTERED_GEM:
				createParticleShatteredGem(particle_x, particle_y, 7.5, sprite, nullptr);
				break;
			case PARTICLE_EFFECT_ERUPT:
				createParticleErupt(particle_x, particle_y, sprite);
				break;
			case PARTICLE_EFFECT_BOOBY_TRAP:
				createParticleBoobyTrapExplode(nullptr, particle_x, particle_y);
				break;
			case PARTICLE_EFFECT_MISC_PUDDLE:
				spawnMiscPuddle(nullptr, particle_x, particle_y, sprite);
				break;
			case PARTICLE_EFFECT_BLOOD_BUBBLE:
			{
				for ( int i = 0; i < 4; ++i )
				{
					if ( Entity* gib = spawnGibClient(particle_x, particle_y, particle_z, 5) )
					{
						gib->sprite = 5;
					}

					Entity* fx = createParticleAestheticOrbit(nullptr, 283, 1.5 * TICKS_PER_SECOND + i * 10, PARTICLE_EFFECT_BLOOD_BUBBLE);
					real_t dir = (local_rng.rand() % 360) * PI / 180.f;
					fx->x = particle_x + 4.0 * cos(dir);
					fx->y = particle_y + 4.0 * sin(dir);
					fx->z = particle_z - (local_rng.rand() % 5);
					fx->flags[SPRITE] = true;

					fx->fskill[2] = 2 * PI * (local_rng.rand() % 10) / 10.0;
					fx->fskill[3] = 0.025; // speed osc
					fx->scalex = 0.0125;
					fx->scaley = fx->scalex;
					fx->scalez = fx->scalex;
					fx->actmagicOrbitDist = 2;
					fx->actmagicOrbitStationaryX = particle_x;
					fx->actmagicOrbitStationaryY = particle_y;
				}
				break;
			}
			case PARTICLE_EFFECT_SPORE_BOMB:
				for ( int i = 0; i < 16; ++i )
				{
					Entity* gib = spawnGibClient(particle_x, particle_y, particle_z, sprite);
					gib->sprite = sprite;
					gib->yaw = i * PI / 4 + (-2 + local_rng.rand() % 5) * PI / 64;
					gib->vel_x = 1.75 * cos(gib->yaw);
					gib->vel_y = 1.75 * sin(gib->yaw);
					gib->scalex = 0.5;
					gib->scaley = 0.5;
					gib->scalez = 0.5;
					gib->z = local_rng.uniform(8, particle_z - 4);
					gib->lightBonus = vec4(0.25, 0.25, 0.25, 0.f);
				}
				break;
			case PARTICLE_EFFECT_WINDGATE:
			{
				int duration = static_cast<int>(SDLNet_Read32(&net_packet->data[13]));
				Uint32 data = SDLNet_Read32(&net_packet->data[17]);

				int wallDir = (data & 0xF);
				int length = (data >> 4) & 0xF;
				Uint32 casterUid = SDLNet_Read32(&net_packet->data[21]);
				createWindMagic(casterUid, particle_x, particle_y, duration, wallDir, length);
				break;
			}
			case PARTICLE_EFFECT_DEMESNE_DOOR:
				createParticleDemesneDoor(particle_x, particle_y, particle_z / 256.0);
				break;
			case PARTICLE_EFFECT_NULL_PARTICLE:
			{
				Entity* fx = createParticleAestheticOrbit(nullptr, sprite, TICKS_PER_SECOND / 4, PARTICLE_EFFECT_NULL_PARTICLE);
				fx->x = particle_x;
				fx->y = particle_y;
				fx->z = particle_z;
				Sint32 dir = SDLNet_Read32(&net_packet->data[17]);
				fx->yaw = dir / 256.0;
				fx->actmagicOrbitDist = 0;
				fx->actmagicNoLight = 0;
				break;
			}
			case PARTICLE_EFFECT_AREA_EFFECT:
			{
				int radius = SDLNet_Read32(&net_packet->data[13]);
				createSpellExplosionArea(sprite, nullptr, particle_x, particle_y, particle_z, radius, 0, nullptr);
				break;
			}
			case PARTICLE_EFFECT_EARTH_ELEMENTAL_DIE:
			{
				Entity* spellTimer = createParticleTimer(nullptr, TICKS_PER_SECOND, -1);
				spellTimer->x = particle_x;
				spellTimer->y = particle_y;
				spellTimer->z = particle_z;
				spellTimer->particleTimerCountdownAction = PARTICLE_TIMER_ACTION_EARTH_ELEMENTAL_DIE;
				break;
			}
			case PARTICLE_EFFECT_DUCK_SPAWN_FEATHER:
			{
				duckSpawnFeather(sprite, particle_x, particle_y, particle_z, nullptr);
				break;
			}
			case PARTICLE_EFFECT_SABOTAGE_TRAP:
			{
				Entity* spellTimer = createParticleTimer(nullptr, TICKS_PER_SECOND, -1);
				spellTimer->x = particle_x;
				spellTimer->y = particle_y;
				spellTimer->z = particle_z;
				spellTimer->particleTimerCountdownAction = PARTICLE_TIMER_ACTION_TRAP_SABOTAGED;
				break;
			}
			case PARTICLE_EFFECT_EARTH_ELEMENTAL_SUMMON_AOE:
			{
				int radius = SDLNet_Read32(&net_packet->data[13]);
				Uint32 color = SDLNet_Read32(&net_packet->data[17]);
				if ( Entity* fx = createParticleAOEIndicator(nullptr, particle_x, particle_y, 0.0, TICKS_PER_SECOND, radius) )
				{
					fx->actSpriteFollowUID = 0;
					fx->actSpriteCheckParentExists = 0;
					if ( auto indicator = AOEIndicators_t::getIndicator(fx->skill[10]) )
					{
						indicator->indicatorColor = color;
						indicator->loop = false;
						indicator->gradient = 4;
						indicator->framesPerTick = 2;
						indicator->ticksPerUpdate = 1;
						indicator->delayTicks = 0;
					}
				}
				break;
			}
			case PARTICLE_EFFECT_BASTION_MUSHROOM:
			{
				Uint32 casterUid = SDLNet_Read32(&net_packet->data[21]);
				createMushroomSpellEffect(uidToEntity(casterUid), particle_x, particle_y);
				break;
			}
			case PARTICLE_EFFECT_METEOR_STATIONARY_ORBIT:
			{
				int duration = static_cast<int>(SDLNet_Read32(&net_packet->data[13]));
				Sint32 dir = SDLNet_Read32(&net_packet->data[17]);
				if ( Entity* fx = createParticleAestheticOrbit(nullptr, 2210, duration, PARTICLE_EFFECT_METEOR_STATIONARY_ORBIT) )
				{
					fx->x = particle_x;
					fx->y = particle_y;
					fx->z = particle_z;
					fx->yaw = (dir / 256.0) + PI / 4;
				}
				if ( Entity* fx = createParticleAestheticOrbit(nullptr, 2211, duration, PARTICLE_EFFECT_METEOR_STATIONARY_ORBIT) )
				{
					fx->x = particle_x;
					fx->y = particle_y;
					fx->z = particle_z;
					fx->yaw = (dir / 256.0) - PI / 4;
				}
				break;
			}
			default:
				break;
		}
	}},

	// enemy hp bar
	{'ENHP', [](){
		Sint16 enemy_hp = SDLNet_Read16(&net_packet->data[4]);
		Sint16 enemy_maxhp = SDLNet_Read16(&net_packet->data[6]);
		Sint16 oldhp = SDLNet_Read16(&net_packet->data[8]);
		Uint32 uid = SDLNet_Read32(&net_packet->data[10]);
		bool lowPriorityTick = false;
		DamageGib gib = DMG_DEFAULT;
		if ( EnemyHPDamageBarHandler::bDamageGibTypesEnabled )
		{
			gib = (DamageGib)((net_packet->data[14] & 0xFE) >> 1); // upper 7 bits
			if ( net_packet->data[14] & 1 )
			{
				lowPriorityTick = true;
			}
		}
		else
		{
			if ( net_packet->data[14] == 1 )
			{
				lowPriorityTick = true;
			}
		}
		char enemy_name[128] = "";
		strcpy(enemy_name, (char*)(&net_packet->data[55]));
		auto details = enemyHPDamageBarHandler[clientnum].addEnemyToList(static_cast<Sint32>(enemy_hp), 
			static_cast<Sint32>(enemy_maxhp), static_cast<Sint32>(oldhp), uid, enemy_name, lowPriorityTick, gib);
		if ( details )
		{
			details->enemy_statusEffects1 = SDLNet_Read32(&net_packet->data[15]);
			details->enemy_statusEffects2 = SDLNet_Read32(&net_packet->data[19]);
			details->enemy_statusEffects3 = SDLNet_Read32(&net_packet->data[23]);
			details->enemy_statusEffects4 = SDLNet_Read32(&net_packet->data[27]);
			details->enemy_statusEffects5 = SDLNet_Read32(&net_packet->data[31]);
			details->enemy_statusEffectsLowDuration1 = SDLNet_Read32(&net_packet->data[35]);
			details->enemy_statusEffectsLowDuration2 = SDLNet_Read32(&net_packet->data[39]);
			details->enemy_statusEffectsLowDuration3 = SDLNet_Read32(&net_packet->data[43]);
			details->enemy_statusEffectsLowDuration4 = SDLNet_Read32(&net_packet->data[47]);
			details->enemy_statusEffectsLowDuration5 = SDLNet_Read32(&net_packet->data[51]);
		}
	}},

	// custom damage gib (miss/healing)
	{'DMGG', [](){
		Uint32 uid = SDLNet_Read32(&net_packet->data[4]);
		Sint16 dmg = (Sint16)SDLNet_Read16(&net_packet->data[8]);
		DamageGib gib = DMG_DEFAULT;
		gib = (DamageGib)(net_packet->data[10]);
		DamageGibDisplayType displayType = DamageGibDisplayType::DMG_GIB_NUMBER;
		if ( net_packet->data[11] == 1 )
		{
			displayType = DamageGibDisplayType::DMG_GIB_MISS;
		}
		else if ( net_packet->data[11] == 2 )
		{
			displayType = DamageGibDisplayType::DMG_GIB_SPRITE;
		}
		else if ( net_packet->data[11] == 3 )
		{
			displayType = DamageGibDisplayType::DMG_GIB_GUARD;
		}
		spawnDamageGib(uidToEntity(uid), dmg, gib, displayType);
	}},

	// ping
	{'PING', [](){
		messagePlayer(clientnum, MESSAGE_MISC, Language::get(1117), (SDL_GetTicks() - pingtime));
	}},

	// automated ping
	{'PNGU', [](){
		PingNetworkStatus_t::respond();
	}},

	// automated ping response
	{'PNGR', [](){
		PingNetworkStatus_t::receive();
	}},

	// unlock steam achievement
	{'SACH', [](){
		steamAchievement((char*)(&net_packet->data[4]));
	}},

	// update steam statistic
	{'SSTA', []() {
		const int statisticNum = static_cast<int>(net_packet->data[4]);
		int value = static_cast<int>(SDLNet_Read16(&net_packet->data[6]));
		steamStatisticUpdate(statisticNum, static_cast<ESteamStatTypes>(net_packet->data[5]), value);
	}},

	// update challenge counter
	{ 'CHCT', []() {
		int value = static_cast<int>(SDLNet_Read16(&net_packet->data[4]));
		int max = static_cast<int>(SDLNet_Read16(&net_packet->data[6]));
		const char* challengeName = "CHALLENGE_MONSTER_KILLS";
		int eventType = net_packet->data[8];
		if ( eventType == (int)GameModeManager_t::CurrentSession_t::ChallengeRun_t::CHEVENT_KILLS_FURNITURE )
		{
			challengeName = "CHALLENGE_FURNITURE_KILLS";
		}
		UIToastNotificationManager.createStatisticUpdateNotification(challengeName, value, max);
	}},

	// pause game
	{'PAUS', [](){
	    const int player =
	    	decodeGameplayPacketPlayerIndex(
	    		net_packet->data[4]
	    	);
	    if ( player < 0 )
	    {
	    	return;
	    }
		messagePlayer(clientnum, MESSAGE_MISC, Language::get(1118), stats[player]->name);
		pauseGame(2, 0);
	}},

	// unpause game
	{'UNPS', [](){
	    const int player =
	    	decodeGameplayPacketPlayerIndex(
	    		net_packet->data[4]
	    	);
	    if ( player < 0 )
	    {
	    	return;
	    }
		messagePlayer(clientnum, MESSAGE_MISC, Language::get(1119), stats[player]->name);
		pauseGame(1, 0);
	}},

	// server or player shut down
	{'DISC', [](){
	    const int player =
	    	decodeGameplayPacketPlayerIndex(
	    		net_packet->data[4]
	    	);
	    if ( player < 0 )
	    {
	    	return;
	    }
		client_disconnected[player] = true;
		if (player == 0)
		{
			// server shutdown
			if (!victory)
			{
				printlog("The remote server has shut down.\n");
				MainMenu::disconnectedFromServer("The host player\nhas ended the game.");
			}
		}
	}},

	// project spirit player
	{ 'PROJ', []() {
		if ( players[clientnum] == nullptr || !players[clientnum]->entity || stats[clientnum]->HP <= 0 )
		{
			return;
		}

		players[clientnum]->ghost.initTeleportLocations(players[clientnum]->entity->x / 16, players[clientnum]->entity->y / 16);
		players[clientnum]->ghost.spawnGhost();
		//node_t* nextnode = nullptr;
		//for ( auto node = map.entities->first; node; node = nextnode )
		//{
		//	nextnode = node->next;
		//	if ( Entity* entity = (Entity*)node->element )
		//	{
		//		if ( entity->behavior == &actProjectSpiritCam && entity->skill[2] == clientnum )
		//		{
		//			entity->removeLightField();
		//			list_RemoveNode(entity->mynode);
		//		}
		//	}
		//}

		//Uint32 uid = SDLNet_Read32(&net_packet->data[4]);
		//if ( Entity* targetEntity = uidToEntity(uid) )
		//{
		//	// projectcam
		//	Entity* entity = newEntity(-1, 1, map.entities, nullptr); //Deathcam entity.
		//	entity->x = targetEntity->x;
		//	entity->y = targetEntity->y;
		//	entity->z = -2;
		//	entity->flags[NOUPDATE] = true;
		//	entity->flags[PASSABLE] = true;
		//	entity->flags[INVISIBLE] = true;
		//	entity->behavior = &actProjectSpiritCam;
		//	entity->skill[1] = targetEntity->getUID();
		//	entity->skill[2] = clientnum;
		//	entity->yaw = targetEntity->yaw;
		//	entity->pitch = PI / 8;
		//	players[clientnum]->entity->skill[3] = 2;
		//	if ( multiplayer != CLIENT )
		//	{
		//		entity_uids--;
		//	}
		//	entity->setUID(-3);
		//}
	}},

	// teleport player
	{'TELE', [](){
		if (players[clientnum] == nullptr || !Player::getPlayerInteractEntity(clientnum) )
		{
			return;
		}
		int tele_x = net_packet->data[4];
		int tele_y = net_packet->data[5];
		Sint16 degrees = (Sint16)SDLNet_Read16(&net_packet->data[6]);
		Entity* playerEntity = Player::getPlayerInteractEntity(clientnum);
		playerEntity->yaw = degrees * PI / 180;
		playerEntity->x = (tele_x << 4) + 8;
		playerEntity->y = (tele_y << 4) + 8;
		playerEntity->bNeedsRenderPositionInit = true;
        for (auto part : playerEntity->bodyparts) {
            part->bNeedsRenderPositionInit = true;
        }
        for (auto node = map.entities->first; node != nullptr; node = node->next) {
            auto entity = (Entity*)node->element;
            if (entity && entity->behavior == &actSpriteNametag) {
                if (entity->parent == playerEntity->getUID()) {
                    entity->bNeedsRenderPositionInit = true;
                }
            }
        }
        temporarilyDisableDithering();
	}},

	// teleport player
	{'TELM', [](){
		if ( players[clientnum] == nullptr || !Player::getPlayerInteractEntity(clientnum) )
		{
			return;
		}
		int tele_x = net_packet->data[4];
		int tele_y = net_packet->data[5];
		int type = net_packet->data[6];
		Entity* playerEntity = Player::getPlayerInteractEntity(clientnum);
		playerEntity->x = (tele_x << 4) + 8;
		playerEntity->y = (tele_y << 4) + 8;
		playerEntity->bNeedsRenderPositionInit = true;
		for ( auto part : playerEntity->bodyparts ) {
			part->bNeedsRenderPositionInit = true;
		}

		// play sound effect
		if ( type == 0 || type == 1 )
		{
			playSoundEntityLocal(playerEntity, 96, 64);
		}
		else if ( type == 2 )
		{
			playSoundEntityLocal(playerEntity, 154, 64);
		}
		for ( auto node = map.entities->first; node != nullptr; node = node->next ) {
			auto entity = (Entity*)node->element;
			if ( entity && entity->behavior == &actSpriteNametag ) {
				if ( entity->parent == playerEntity->getUID() ) {
					entity->bNeedsRenderPositionInit = true;
				}
			}
		}
		temporarilyDisableDithering();
	}},

	// delete entity
	{'ENTD', [](){
		Entity *entity = uidToEntity((int)SDLNet_Read32(&net_packet->data[4]));
		if ( entity )
		{
			prepareRemovedEntityTombstonesForActiveInstance();
			auto entity2 = newEntity(entity->sprite, 1, &removedEntities, nullptr);
			if ( entity2 )
			{
				entity2->setUID(entity->getUID());
				for ( int j = 0; j < MAXPLAYERS; ++j )
				{
					if (entity == players[j]->entity )
					{
						if ( stats[j] )
						{
							for ( int effect = 0; effect < NUMEFFECTS; ++effect )
							{
								if ( effect != EFF_VAMPIRICAURA && effect != EFF_WITHDRAWAL && effect != EFF_SHAPESHIFT )
								{
									stats[j]->clearEffect(effect);
									stats[j]->EFFECTS_TIMERS[effect] = 0;
								}
							}
						}
						players[j]->entity = nullptr;
						players[j]->cleanUpOnEntityRemoval();
					}
					else if ( entity == players[j]->ghost.my )
					{
						players[j]->ghost.my = nullptr;
						players[j]->ghost.reset();
					}
				}
				if ( entity->light )
				{
					list_RemoveNode(entity->light->node);
					entity->light = nullptr;
				}
				list_RemoveNode(entity->mynode);

				// inform the server that we deleted the entity
				//strcpy((char*)net_packet->data, "ENTD");
				//net_packet->data[4] = clientnum;
				//SDLNet_Write32(entity2->getUID(), &net_packet->data[5]);
				//net_packet->address.host = net_server.host;
				//net_packet->address.port = net_server.port;
				//net_packet->len = 9;
				//sendPacket(net_sock, -1, net_packet, 0);
			}
		}
	}},

	// shake screen
	{'SHAK', [](){
		cameravars[clientnum].shakex += ((Sint8)(net_packet->data[4])) / 100.f;
		cameravars[clientnum].shakey += ((Sint8)(net_packet->data[5]));
	}},

	// no mana flash
	{ 'NOMP', []() {
		messagePlayer(clientnum, MESSAGE_MISC, Language::get(375));
		playSound(563, 64);
		if ( players[clientnum]->magic.noManaProcessedOnTick == 0 )
		{
			players[clientnum]->magic.flashNoMana();
		}
		if ( net_packet->len >= 8 )
		{
			if ( stats[clientnum]->defending && stats[clientnum]->shield )
			{
				ItemType itemType = static_cast<ItemType>(SDLNet_Read32(&net_packet->data[4]));
				if ( stats[clientnum]->shield->type == itemType )
				{
					Input& input = Input::inputs[clientnum];
					if ( input.binaryToggle("Defend") )
					{
						input.consumeBinaryToggle("Defend");
					}
				}
			}
		}
	} },

	// a torch burns out
	{'TORC', [](){
		ItemType itemType = static_cast<ItemType>(SDLNet_Read16(&net_packet->data[4]));
		Status itemStatus = static_cast<Status>(net_packet->data[6]);
		int qty = static_cast<int>(net_packet->data[7]);
		if ( stats[clientnum]->shield && stats[clientnum]->shield->type == itemType )
		{
			stats[clientnum]->shield->status = itemStatus;
			stats[clientnum]->shield->count = qty;
			if ( stats[clientnum]->shield->count <= 0 )
			{
				Item* item = stats[clientnum]->shield;
				item->count = 1; // to be consumed below
				consumeItem(item, clientnum);
				stats[clientnum]->shield = nullptr;
			}
			else
			{
				players[clientnum]->hud.shieldSwitch = true;
			}
		}
	}},

	// update equip beatitude
	{'BEAT', []() {
		Item* equipment = nullptr;
		//messagePlayer(0, "client: %d, armornum: %d, status %d", player, net_packet->data[5], net_packet->data[6]);
		switch ( net_packet->data[5] )
		{
			case 0:
				equipment = stats[clientnum]->weapon;
				break;
			case 1:
				equipment = stats[clientnum]->helmet;
				break;
			case 2:
				equipment = stats[clientnum]->breastplate;
				break;
			case 3:
				equipment = stats[clientnum]->gloves;
				break;
			case 4:
				equipment = stats[clientnum]->shoes;
				break;
			case 5:
				equipment = stats[clientnum]->shield;
				break;
			case 6:
				equipment = stats[clientnum]->cloak;
				break;
			case 7:
				equipment = stats[clientnum]->mask;
				break;
			default:
				equipment = nullptr;
				break;
		}

		if ( !equipment )
		{
			return;
		}
		int itemType = SDLNet_Read16(&net_packet->data[7]);
		if ( (int)equipment->type == itemType ) // sanity check the item type is what was changed
		{
			equipment->beatitude = net_packet->data[6] - 100; // we sent the data beatitude + 100
		}
	}},

	// update armor quality
	{'ARMR', [](){
	    Item* item;
		switch ( net_packet->data[4] )
		{
			case 0:
				item = stats[clientnum]->helmet;
				break;
			case 1:
				item = stats[clientnum]->breastplate;
				break;
			case 2:
				item = stats[clientnum]->gloves;
				break;
			case 3:
				item = stats[clientnum]->shoes;
				break;
			case 4:
				item = stats[clientnum]->shield;
				break;
			case 5:
				item = stats[clientnum]->weapon;
				break;
			case 6:
				item = stats[clientnum]->cloak;
				break;
			case 7:
				item = stats[clientnum]->amulet;
				break;
			case 8:
				item = stats[clientnum]->ring;
				break;
			case 9:
				item = stats[clientnum]->mask;
				break;
			default:
				item = NULL;
				break;
		}

		int checkType = -1;
		if ( net_packet->len >= 8 )
		{
			checkType = SDLNet_Read16(&net_packet->data[6]);
		}
		if ( item != NULL && (checkType <= -1 || (checkType >= 0 && item->type == checkType)) )
		{
			if ( item->count > 1 )
			{
				Item* pickedUp = newItem(item->type, item->status, item->beatitude, item->count - 1, item->appearance, item->identified, &stats[clientnum]->inventory);
				item->count = 1;
			}
			if ( static_cast<int>(net_packet->data[5]) > EXCELLENT )
			{
				item->status = EXCELLENT;
			}
			else if ( static_cast<int>(net_packet->data[5]) < BROKEN )
			{
				item->status = BROKEN;
				if ( net_packet->data[4] == 5 )
				{
					if ( client_classes[clientnum] == CLASS_MESMER )
					{
						if ( stats[clientnum]->weapon->type == MAGICSTAFF_CHARM )
						{
							bool foundCharmSpell = false;
							for ( node_t* spellnode = stats[clientnum]->inventory.first; spellnode != nullptr; spellnode = spellnode->next )
							{
								Item* item = (Item*)spellnode->element;
								if ( item && itemCategory(item) == SPELL_CAT )
								{
									spell_t* spell = getSpellFromItem(clientnum, item, false);
									if ( spell && spell->ID == SPELL_CHARM_MONSTER )
									{
										foundCharmSpell = true;
										break;
									}
								}
							}
							if ( !foundCharmSpell )
							{
								steamAchievement("BARONY_ACH_WHAT_NOW");
							}
						}
					}
				}
			}
			else
			{
				item->status = static_cast<Status>(net_packet->data[5]);
			}

			// spellbooks in hand crumble to nothing.
			if ( item->status == BROKEN && net_packet->data[4] == 4 && itemCategory(item) == SPELLBOOK )
			{
				consumeItem(item, clientnum);
			}
			else if ( item )
			{
				if ( players[clientnum]->isLocalPlayer() )
				{
					std::unordered_set<Uint32> appearancesOfSimilarItems;
					std::vector<Item*> itemsToReroll;
					for ( node_t* node = stats[clientnum]->inventory.first; node != NULL; node = node->next )
					{
						Item* item2 = static_cast<Item*>(node->element);
						if ( item2 && item2 != item && !itemCompare(item, item2, true) )
						{
							itemsToReroll.push_back(item2);

							// items are the same (incl. appearance!)
							// if they shouldn't stack, we need to change appearance of the new item.
							appearancesOfSimilarItems.insert(item2->appearance);
						}
					}

					for ( auto rerollItem : itemsToReroll )
					{
						Item::itemFindUniqueAppearance(rerollItem, appearancesOfSimilarItems);
						appearancesOfSimilarItems.insert(rerollItem->appearance);
					}
				}
			}
		}
	}},

    // steal armor or weapon (destroy it)
    {'STLA', [](){
        if ( net_packet->len < 26 )
        {
            printlog(
                "[NET]: ignoring malformed STLA packet with length %d.\n",
                net_packet->len
            );
            return;
        }

        Item* item = nullptr;
        int armornum = net_packet->data[4];
		switch ( armornum )
		{
			case 0:
				item = stats[clientnum]->helmet;
				break;
			case 1:
				item = stats[clientnum]->breastplate;
				break;
			case 2:
				item = stats[clientnum]->gloves;
				break;
			case 3:
				item = stats[clientnum]->shoes;
				break;
			case 4:
				item = stats[clientnum]->shield;
				break;
			case 5:
				item = stats[clientnum]->weapon;
				break;
			case 6:
				item = stats[clientnum]->cloak;
				break;
			case 7:
				item = stats[clientnum]->amulet;
				break;
			case 8:
				item = stats[clientnum]->ring;
				break;
			case 9:
				item = stats[clientnum]->mask;
				break;
			default:
				item = NULL;
				break;
		}

		
        int resolvedType =
            static_cast<int>(SDLNet_Read32(&net_packet->data[5]));
#ifdef SAM_FRAMEWORK_ENABLED
        if ( !resolveSAMItemTypeFromPacket(
            resolvedType,
            26,
            "STLA",
            resolvedType
        ) )
        {
            messagePlayer(
                clientnum,
                MESSAGE_MISC,
                "A custom equipment update was rejected because its stable ID was unavailable."
            );
            return;
        }
#endif
        ItemType checkType = static_cast<ItemType>(resolvedType);
        Status checkStatus = static_cast<Status>(SDLNet_Read32(&net_packet->data[9]));
		Sint16 checkBeatitude = static_cast<Sint16>(SDLNet_Read32(&net_packet->data[13]));
		Sint16 checkCount = static_cast<Sint16>(SDLNet_Read32(&net_packet->data[17]));
		Uint32 checkAppearance = static_cast<Uint32>(SDLNet_Read32(&net_packet->data[21]));
		bool checkIdentified = net_packet->data[25] == 1 ? true : false;
		
		if ( item )
		{
			if ( item->type == checkType
				&& item->status == checkStatus
				&& item->beatitude == checkBeatitude
				&& item->count == checkCount
				&& item->appearance == checkAppearance
				/*&& item->identified == checkIdentified*/ )
			{
				// ok
				if ( itemTypeIsQuiver(item->type) || armornum == 5 /*weapon*/ )
				{
					item->count = 0;
				}
				else
				{
					item->count--;
				}

				if ( item->count <= 0 )
				{
					Item** slot = itemSlot(stats[clientnum], item);
					if ( slot != NULL )
					{
						*slot = NULL;
					}
					if ( item )
					{
						list_RemoveNode(item->node);
					}
				}
				return;
			}
			else
			{
				item = nullptr;
			}
		}

		if ( !item )
		{
			for ( node_t* node = stats[clientnum]->inventory.first; node != nullptr; node = node->next )
			{
				if ( Item* item2 = static_cast<Item*>(node->element) )
				{
					if ( item2->type == checkType
						&& item2->status == checkStatus
						&& item2->beatitude == checkBeatitude
						&& item2->count == checkCount
						&& item2->appearance == checkAppearance
						/*&& item2->identified == checkIdentified*/ )
					{
						// next best match
						if ( itemTypeIsQuiver(item2->type) || armornum == 5 /*weapon*/ )
						{
							item2->count = 0;
						}
						else
						{
							item2->count--;
						}

						if ( item2->count <= 0 )
						{
							Item** slot = itemSlot(stats[clientnum], item2);
							if ( slot != NULL )
							{
								*slot = NULL;
							}
							if ( item2 )
							{
								list_RemoveNode(item2->node);
							}
						}
						return;
					}
				}
			}
		}
	}},

	// damage indicator
	{'DAMI', [](){
		DamageIndicatorHandler.insert(clientnum, SDLNet_Read32(&net_packet->data[4]), 
			SDLNet_Read32(&net_packet->data[8]), net_packet->data[12] == 1 ? true : false);
	} },

	// remote vibration
	{ 'BRRR', []() {
		inputs.addRumbleForHapticType(clientnum, SDLNet_Read32(&net_packet->data[4]),
			SDLNet_Read32(&net_packet->data[8]));
	}},

		// play sound position
	{'SNDP', [](){
		playSoundPos(
		    SDLNet_Read32(&net_packet->data[4]),
		    SDLNet_Read32(&net_packet->data[8]),
		    SDLNet_Read16(&net_packet->data[12]),
		    (Uint8)net_packet->data[14]);
	}},

		// play sound global
	{'SNDG', [](){
		playSound(
		    SDLNet_Read16(&net_packet->data[4]),
		    (Uint8)net_packet->data[6]);
	}},

	// play sound notification global
	{ 'SNDN', []() {
		playSoundNotification(
			SDLNet_Read16(&net_packet->data[4]),
			(Uint8)net_packet->data[6]);
	} },

	// play sound entity local
	{'SNEL', [](){
		Entity* tmp = uidToEntity(SDLNet_Read32(&net_packet->data[6]));
		int sfx = SDLNet_Read16(&net_packet->data[4]);
		if ( tmp )
		{
			if ( tmp->behavior == &actPlayer && mute_player_monster_sounds )
			{
				switch ( sfx )
				{
					case 95:
					case 70:
					case 322:
					case 323:
					case 324:
					case 329:
					case 332:
					case 333:
					case 229:
					case 230:
					case 231:
					case 232:
					case 233:
					case 234:
					case 235:
					case 291:
					case 292:
					case 293:
					case 294:
					case 60:
					case 61:
					case 62:
					case 257:
					case 258:
					case 276:
					case 277:
					case 278:
					case 502:
					case 503:
					case 504:
					case 505:
					case 506:
					case 507:
					case 508:
					case 830:
					case 831:
					case 832:
					case 833:
					case 834:
					case 835:
					case 836:
					case 837:
					case 838:
					case 839:
					case 840:
					case 841:
					case 842:
					case 843:
					case 844:
					case 845:
					case 846:
					case 847:
					case 848:
						// return early, don't play monster noises from players.
						return;
					default:
						break;
				}
			}
			playSoundEntityLocal(tmp, sfx, SDLNet_Read16(&net_packet->data[10]));
		}
	}},

	// add light
	{'ALIT', [](){
        std::vector<char> data;
        const auto len = SDLNet_Read16(&net_packet->data[8]);
        data.resize(len);
        stringCopy(data.data(), (const char*)&net_packet->data[10], data.size(), len);
		addLight(
		    SDLNet_Read16(&net_packet->data[4]),
		    SDLNet_Read16(&net_packet->data[6]),
		    data.data());
	}},

	// create wall
	{'WALC', [](){
		int y = SDLNet_Read16(&net_packet->data[6]);
		int x = SDLNet_Read16(&net_packet->data[4]);
		if ( x >= 0 && x < map.width && y >= 0 && y < map.height )
		{
			map.tiles[OBSTACLELAYER + y * MAPLAYERS + x * MAPLAYERS * map.height] = map.tiles[y * MAPLAYERS + x * MAPLAYERS * map.height];
		}

		const real_t effectOffset = 2.0;
		spawnPoof(static_cast<Sint16>(x * 16.0 - effectOffset), static_cast<Sint16>(y * 16.0 - effectOffset), 8, 1.0);
		spawnPoof(static_cast<Sint16>(x * 16.0 - effectOffset), static_cast<Sint16>(y * 16.0 + 16.0 + effectOffset), 8, 1.0);
		spawnPoof(static_cast<Sint16>(x * 16.0 + 16.0 + effectOffset), static_cast<Sint16>(y * 16.0 - effectOffset), 8, 1.0);
		spawnPoof(static_cast<Sint16>(x * 16.0 + 16.0 + effectOffset), static_cast<Sint16>(y * 16.0 + 16.0 + effectOffset), 8, 1.0);
	}},

	// destroy wall
	{'WALD', [](){
		int y = SDLNet_Read16(&net_packet->data[6]);
		int x = SDLNet_Read16(&net_packet->data[4]);
		if ( x >= 0 && x < map.width && y >= 0 && y < map.height )
		{
			map.tiles[OBSTACLELAYER + y * MAPLAYERS + x * MAPLAYERS * map.height] = 0;
		}
	}},

	// destroy wall + ceiling
	{'WACD', [](){
		int y = SDLNet_Read16(&net_packet->data[6]);
		int x = SDLNet_Read16(&net_packet->data[4]);
		if ( x >= 0 && x < map.width && y >= 0 && y < map.height )
		{
			map.tiles[OBSTACLELAYER + y * MAPLAYERS + x * MAPLAYERS * map.height] = 0;
			map.tiles[(MAPLAYERS - 1) + y * MAPLAYERS + x * MAPLAYERS * map.height] = 0;
		}
	}},

	// monster music
	{'MUSM', [](){
	    Uint8 assailant = net_packet->data[4];
		combat = assailant;
	}},

    // get item
    {'ITEM', [](){
        if ( net_packet->len < 29 )
        {
            printlog("[NET]: refusing malformed ITEM packet.\n");
            return;
        }

        int resolvedType = static_cast<int>(
            SDLNet_Read32(&net_packet->data[4])
        );
#ifdef SAM_FRAMEWORK_ENABLED
        if ( !resolveSAMItemTypeFromPacket(
            resolvedType,
            29,
            "ITEM",
            resolvedType
        ) )
        {
            return;
        }
#endif

        Item* item = newItem(
            static_cast<ItemType>(resolvedType),
            static_cast<Status>(SDLNet_Read32(&net_packet->data[8])),
            SDLNet_Read32(&net_packet->data[12]),
            SDLNet_Read32(&net_packet->data[16]),
            SDLNet_Read32(&net_packet->data[20]),
            net_packet->data[28],
            NULL);
		item->ownerUid = SDLNet_Read32(&net_packet->data[24]);
		Item* pickedUp = itemPickup(clientnum, item);
		free(item);
		if ( players[clientnum] && players[clientnum]->entity )
		{
			if ( pickedUp && pickedUp->type == BOOMERANG && !stats[clientnum]->weapon && pickedUp->ownerUid == players[clientnum]->entity->getUID() )
			{
				useItem(pickedUp, clientnum);

				auto& hotbar_t = players[clientnum]->hotbar;
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
			else if ( pickedUp && pickedUp->type == TOOL_DUCK && !stats[clientnum]->shield )
			{
				bool shapeshifted = false;
				if ( players[clientnum] && players[clientnum]->entity && players[clientnum]->entity->effectShapeshift != NOTHING )
				{
					shapeshifted = true;
				}

				if ( !shapeshifted && !intro )
				{
					useItem(pickedUp, clientnum);

					auto& hotbar_t = players[clientnum]->hotbar;
					auto& hotbar = hotbar_t.slots();
					if ( hotbar_t.magicDuckHotbarSlot >= 0 )
					{
						hotbar[hotbar_t.magicDuckHotbarSlot].item = pickedUp->uid;
						for ( int i = 0; i < NUM_HOTBAR_SLOTS; ++i )
						{
							if ( i != hotbar_t.magicDuckHotbarSlot && hotbar[i].item == pickedUp->uid )
							{
								hotbar[i].item = 0;
								hotbar[i].resetLastItem();
							}
						}
					}
				}
			}
		}
	}},

	// unequip and remove item
	{'DROP', [](){
		Item** armor = NULL;
		switch ( net_packet->data[4] )
		{
			case 0:
				armor = &stats[clientnum]->helmet;
				break;
			case 1:
				armor = &stats[clientnum]->breastplate;
				break;
			case 2:
				armor = &stats[clientnum]->gloves;
				break;
			case 3:
				armor = &stats[clientnum]->shoes;
				break;
			case 4:
				armor = &stats[clientnum]->shield;
				break;
			case 5:
				armor = &stats[clientnum]->weapon;
				break;
			case 6:
				armor = &stats[clientnum]->cloak;
				break;
			case 7:
				armor = &stats[clientnum]->amulet;
				break;
			case 8:
				armor = &stats[clientnum]->ring;
				break;
			case 9:
				armor = &stats[clientnum]->mask;
				break;
		}
		if ( !armor )
		{
			return;
		}
		if ( !(*armor) )
		{
			return;
		}

		if ( *armor == inputs.getUIInteraction(clientnum)->selectedItem )
		{
			inputs.getUIInteraction(clientnum)->selectedItem = nullptr;
			inputs.getUIInteraction(clientnum)->selectedItemFromChest = 0;
		}

		if ( (*armor)->count > 1 )
		{
			(*armor)->count--;
		}
		else
		{
			if ( (*armor)->node )
			{
				list_RemoveNode((*armor)->node);
			}
			else
			{
				free(*armor);
			}
		}
		*armor = NULL;
	}},

	// get gold
	{'GOLD', [](){
		stats[clientnum]->GOLD = SDLNet_Read32(&net_packet->data[4]);
	}},

	// open shop
	{'SHOP', [](){
		players[clientnum]->closeAllGUIs(DONT_CHANGE_SHOOTMODE, CLOSEGUI_DONT_CLOSE_INVENTORY);
		players[clientnum]->openStatusScreen(GUI_MODE_SHOP, INVENTORY_MODE_ITEM, Player::GUI_t::MODULE_SHOP);

		shopkeeper[clientnum] = (Uint32)SDLNet_Read32(&net_packet->data[4]);
		shopkeepertype[clientnum] = net_packet->data[8];
		strcpy( shopkeepername_client[clientnum], (char*)(&net_packet->data[9]) );
		shopkeepername[clientnum] = shopkeepername_client[clientnum];
		shoptimer[clientnum] = ticks - 1;
		shopspeech[clientnum] = Language::get(194 + local_rng.rand() % 3);

		players[clientnum]->shopGUI.openShop();
		return;
	}},

	// shop item
	{'SHPI', [](){
        if ( net_packet->len < 18 )
        {
            printlog(
                "[NET]: ignoring malformed SHPI packet with length %d.\n",
                net_packet->len
            );
            return;
        }
		if ( !shopInv[clientnum] )
		{
			return;
		}

        int resolvedType = static_cast<int>(SDLNet_Read32(&net_packet->data[4]));
#ifdef SAM_FRAMEWORK_ENABLED
        if ( !resolveSAMItemTypeFromPacket(
            resolvedType,
            18,
            "SHPI",
            resolvedType
        ) )
        {
            return;
        }
#endif
        ItemType type = static_cast<ItemType>(resolvedType);
        Status status = static_cast<Status>((Sint8)net_packet->data[8]);
		Sint16 beatitude = (Sint8)net_packet->data[9];
		Sint16 count = (unsigned char)net_packet->data[10];
		Uint32 appearance = SDLNet_Read32(&net_packet->data[11]);
		bool identified = (bool)(net_packet->data[15] & 1);
		bool buybackItem = (bool)((net_packet->data[15] >> 1) & 1);
		bool extraConsumable = (bool)((net_packet->data[15] >> 2) & 1);
		Uint8 requireTradingSkill = (Uint8)((net_packet->data[15] >> 4) & 0xF);
		int x = (Sint8)net_packet->data[16];
		int y = (Sint8)net_packet->data[17];
		if ( Item* item = newItem(type, status, beatitude, count, appearance, identified, shopInv[clientnum]) )
		{
			item->x = x;
			item->y = y;
			item->playerSoldItemToShop = buybackItem;
			item->itemSpecialShopConsumable = extraConsumable;
			item->itemRequireTradingSkillInShop = requireTradingSkill;
		}
	}},

	// close shop
	{'SHPC', [](){
		Uint32 id = SDLNet_Read32(&net_packet->data[4]);
		if ( id == shopkeeper[clientnum] )
		{
			closeShop(clientnum);
			players[clientnum]->closeAllGUIs(CLOSEGUI_ENABLE_SHOOTMODE, CLOSEGUI_CLOSE_ALL);
		}
	}},

	// you died
	{'UDIE', [](){
		KilledBy killer = (KilledBy)SDLNet_Read32(&net_packet->data[4]);
		stats[clientnum]->killer = killer;

		if (killer == KilledBy::MONSTER) {
		    if (net_packet->data[8]) { // named monster
		        char name[128];
		        Uint32 len = net_packet->data[8];
		        len = std::min((Uint32)(sizeof(name) - 1), len);
		        memcpy(name, &net_packet->data[13], len);
		        name[len] = '\0';
		        stats[clientnum]->killer_name = name;

				Monster monster = (Monster)SDLNet_Read32(&net_packet->data[9]);
				stats[clientnum]->killer_monster = monster;
		    } else { // anonymous monster
		        Monster monster = (Monster)SDLNet_Read32(&net_packet->data[9]);
		        stats[clientnum]->killer_monster = monster;
				stats[clientnum]->killer_name = "";
		    }
		} else if (killer == KilledBy::ITEM) {
		    ItemType item = (ItemType)SDLNet_Read32(&net_packet->data[8]);
		    stats[clientnum]->killer_item = item;
		}

		if ( players[clientnum] && players[clientnum]->entity && players[clientnum]->entity->playerCreatedDeathCam != 0 )
		{
			// don't spawn deathcam
		}
		else
		{
			Entity* entity = newEntity(-1, 1, map.entities, nullptr);
			entity->x = cameras[clientnum].x * 16;
			entity->y = cameras[clientnum].y * 16;
			entity->z = -2;
			entity->flags[NOUPDATE] = true;
			entity->flags[PASSABLE] = true;
			entity->flags[INVISIBLE] = true;
			entity->behavior = &actDeathCam;
			entity->skill[2] = clientnum;
			entity->yaw = cameras[clientnum].ang;
			entity->pitch = PI / 8;
			players[clientnum]->ghost.initTeleportLocations(entity->x / 16, entity->y / 16);
		}

		//deleteSaveGame(multiplayer); // stops save scumming c: //Not here, because it'll make the game unresumable if the game crashes but not all players have died.

		players[clientnum]->closeAllGUIs(CloseGUIShootmode::CLOSEGUI_ENABLE_SHOOTMODE, CloseGUIIgnore::CLOSEGUI_CLOSE_ALL);
		players[clientnum]->bControlEnabled = false;

#ifdef SOUND
		levelmusicplaying = true;
		combatmusicplaying = false;
		fadein_increment = default_fadein_increment * 4;
		fadeout_increment = default_fadeout_increment * 4;
		playMusic(gameovermusic, false, false, false);
#endif
		combat = false;
		assailant[clientnum] = false;
		assailantTimer[clientnum] = 0;

		if ( !keepInventoryGlobal )
		{
		    node_t* nextnode;
			for ( auto node = stats[clientnum]->inventory.first; node != NULL; node = nextnode )
			{
				nextnode = node->next;
				Item* item = (Item*)node->element;
				if ( itemCategory(item) == SPELL_CAT )
				{
					continue;    // don't drop spells on death, stupid!
				}
				if ( itemIsEquipped(item, clientnum) )
				{
					continue;
				}
				strcpy((char*)net_packet->data, "DIEI");
				SDLNet_Write32((Uint32)item->type, &net_packet->data[4]);
				SDLNet_Write32((Uint32)item->status, &net_packet->data[8]);
				SDLNet_Write32((Uint32)item->beatitude, &net_packet->data[12]);
				SDLNet_Write32((Uint32)item->count, &net_packet->data[16]);
				SDLNet_Write32((Uint32)item->appearance, &net_packet->data[20]);
				net_packet->data[24] = item->identified;
				net_packet->data[25] = clientnum;
				net_packet->data[26] = (Uint8)cameras[clientnum].x;
                net_packet->data[27] = (Uint8)cameras[clientnum].y;
                net_packet->len = 28;

#ifdef SAM_FRAMEWORK_ENABLED
                const int runtimeType = static_cast<int>(item->type);
                if ( SAMItemRegistryFoundation::isRegisteredRuntimeItemId(runtimeType) )
                {
                    const std::string& stableId =
                        SAMItemRegistryFoundation::stableIdForRuntimeId(runtimeType);
                    const int available = NET_PACKET_SIZE - 29;
                    if ( stableId.empty() )
                    {
                        printlog(
                            "[S.A.M] Refusing DIEI custom item runtime %d: no stable id.\n",
                            runtimeType
                        );
                        continue;
                    }
                    if ( available <= 0
                        || static_cast<int>(stableId.size()) > available )
                    {
                        printlog(
                            "[S.A.M] Refusing DIEI custom item [%s]: stable id is too long.\n",
                            stableId.c_str()
                        );
                        continue;
                    }
                    memcpy(&net_packet->data[28], stableId.c_str(), stableId.size());
                    net_packet->data[28 + stableId.size()] = '\0';
                    net_packet->len = 29 + static_cast<int>(stableId.size());
                }
#endif

                net_packet->address.host = net_server.host;
                net_packet->address.port = net_server.port;
                sendPacketSafe(net_sock, -1, net_packet, 0);
			}
		}
		else
		{
			// to not soft lock at Herx
			node_t *node, *nextnode;
			for ( node = stats[clientnum]->inventory.first; node != NULL; node = nextnode )
			{
				nextnode = node->next;
				Item* item = (Item*)node->element;
				if ( itemCategory(item) == SPELL_CAT )
				{
					continue;
				}
				if ( item->type == ARTIFACT_ORB_PURPLE || item->type == TOOL_DUCK )
				{
					Item** slot = itemSlot(stats[clientnum], item);
					if ( slot != nullptr )
					{
						*slot = nullptr;
					}

					players[clientnum]->paperDoll.updateSlots();

					strcpy((char*)net_packet->data, "DIEI");
					SDLNet_Write32((Uint32)item->type, &net_packet->data[4]);
					SDLNet_Write32((Uint32)item->status, &net_packet->data[8]);
					SDLNet_Write32((Uint32)item->beatitude, &net_packet->data[12]);
					SDLNet_Write32((Uint32)item->count, &net_packet->data[16]);
					SDLNet_Write32((Uint32)item->appearance, &net_packet->data[20]);
					net_packet->data[24] = item->identified;
					net_packet->data[25] = clientnum;
					net_packet->data[26] = (Uint8)cameras[clientnum].x;
                    net_packet->data[27] = (Uint8)cameras[clientnum].y;
                    net_packet->len = 28;

#ifdef SAM_FRAMEWORK_ENABLED
                    const int runtimeType = static_cast<int>(item->type);
                    if ( SAMItemRegistryFoundation::isRegisteredRuntimeItemId(runtimeType) )
                    {
                        const std::string& stableId =
                            SAMItemRegistryFoundation::stableIdForRuntimeId(runtimeType);
                        const int available = NET_PACKET_SIZE - 29;
                        if ( stableId.empty() )
                        {
                            printlog(
                                "[S.A.M] Refusing DIEI custom item runtime %d: no stable id.\n",
                                runtimeType
                            );
                            list_RemoveNode(node);
                            continue;
                        }
                        if ( available <= 0
                            || static_cast<int>(stableId.size()) > available )
                        {
                            printlog(
                                "[S.A.M] Refusing DIEI custom item [%s]: stable id is too long.\n",
                                stableId.c_str()
                            );
                            list_RemoveNode(node);
                            continue;
                        }
                        memcpy(&net_packet->data[28], stableId.c_str(), stableId.size());
                        net_packet->data[28 + stableId.size()] = '\0';
                        net_packet->len = 29 + static_cast<int>(stableId.size());
                    }
#endif

                    net_packet->address.host = net_server.host;
                    net_packet->address.port = net_server.port;
                    sendPacketSafe(net_sock, -1, net_packet, 0);
                    list_RemoveNode(node);
				}
			}
		}

		for ( node_t* mapNode = map.creatures->first; mapNode != nullptr; mapNode = mapNode->next )
		{
			Entity* mapCreature = (Entity*)mapNode->element;
			if ( mapCreature )
			{
				if ( mapCreature->monsterEntityRenderAsTelepath == 1 )
				{
					mapCreature->monsterEntityRenderAsTelepath = 0; // do a final pass to undo any telepath rendering.
				}
			}
		}
	}},

	// server forwarded a player callout
	{ 'CALL', []() {
		const int pnum =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( pnum < 0 )
		{
			return;
		}
		if ( pnum != clientnum )
		{
			Uint32 uid = SDLNet_Read32(&net_packet->data[5]);
			Entity* entity = nullptr;
			if ( uid != 0 )
			{
				entity = uidToEntity(uid);
				if ( !entity )
				{
					return;
				}
			}
			CalloutMenu[pnum].lockOnEntityUid = uid;
			CalloutRadialMenu::CalloutCommand cmd = (CalloutRadialMenu::CalloutCommand)net_packet->data[9];
			CalloutMenu[pnum].clientCalloutHelpFlags = SDLNet_Read32(&net_packet->data[10]);
			if ( uid )
			{
				if ( entity )
				{
					CalloutMenu[pnum].createParticleCallout(entity, cmd);
				}
			}
			else
			{
				real_t x = SDLNet_Read16(&net_packet->data[14]);
				real_t y = SDLNet_Read16(&net_packet->data[16]);
				CalloutMenu[pnum].createParticleCallout(
					x * 16.0 + 8.0, y * 16.0 + 8.0, -4, 0, cmd);
			}
		}
	}},

	// textbox message
	{'MSGS', [](){
		Uint32 color = SDLNet_Read32(&net_packet->data[4]);
		MessageType type = (MessageType)SDLNet_Read32(&net_packet->data[8]);
		const char* msg = (const char*)(&net_packet->data[12]);

		if ( ticks != 1 )
		{
			const bool printed = messagePlayerColor(clientnum, type, color, "%s", msg);
			if (type == MESSAGE_CHAT && printed)
			{
				playSound(Message::CHAT_MESSAGE_SFX, 64);
			}
		}
		
		if ( !strcmp(msg, Language::get(1109)) ) // "you survive through your party's persistence"
		{
			// ... or lived
			stats[clientnum]->HP = stats[clientnum]->MAXHP * 0.5;
			stats[clientnum]->MP = stats[clientnum]->MAXMP * 0.5;
			stats[clientnum]->HUNGER = 500;
			for ( int c = 0; c < NUMEFFECTS; c++ )
			{
				if ( !(c == EFF_VAMPIRICAURA && stats[clientnum]->EFFECTS_TIMERS[c] == -2)
					&& c != EFF_WITHDRAWAL && c != EFF_SHAPESHIFT )
				{
					stats[clientnum]->clearEffect(c);
					stats[clientnum]->EFFECTS_TIMERS[c] = 0;
				}
			}
		}
		else if ( !strncmp(msg, Language::get(1114), 28) ) // Zap brigade music
		{
			playSoundNotification(175, 128);
		}
		else if ( (strstr(msg, Language::get(1160))) != NULL ) // <player name> bumps you
		{
			for ( int c = 0; c < MAXPLAYERS; c++ )
			{
				if ( !strncmp(stats[c]->name, msg, strlen(stats[c]->name)) )
				{
					if (players[clientnum] && players[clientnum]->entity && players[c] && players[c]->entity)
					{
						double tangent = atan2(players[clientnum]->entity->y - players[c]->entity->y, players[clientnum]->entity->x - players[c]->entity->x);
						players[clientnum]->entity->vel_x += cos(tangent);
						players[clientnum]->entity->vel_y += sin(tangent);
					}
					break;
				}
			}
		}
		return;
	}},

	// update magic
	{'UPMP', [](){
		stats[clientnum]->MP = SDLNet_Read32(&net_packet->data[4]);
		return;
	}},

	// update effects flags
	{'UPEF', [](){
		int numBytes = NUMEFFECTS / 8;
		for (int c = 0; c < NUMEFFECTS; c++)
		{
			if ( net_packet->data[4 + c / 8]&power(2, c - (c / 8) * 8) )
			{
				stats[clientnum]->setEffectValueUnsafe(c, 1);
				if ( net_packet->data[4 + numBytes + c / 8] & power(2, c - (c / 8) * 8) ) // use these bits to denote if duration is low.
				{
					stats[clientnum]->EFFECTS_TIMERS[c] = 1;
				}
				else if ( stats[clientnum]->EFFECTS_TIMERS[c] > 0 )
				{
					stats[clientnum]->EFFECTS_TIMERS[c] = 0;
				}
			}
			else
			{
				stats[clientnum]->clearEffect(c);
				if ( stats[clientnum]->EFFECTS_TIMERS[c] > 0 )
				{
					stats[clientnum]->EFFECTS_TIMERS[c] = 0;
				}
			}
		}

		int numEffectStrengths = net_packet->data[4 + numBytes * 2];
		int index = 0;
		while ( numEffectStrengths > 0 )
		{
			int currentIndex = (4 + numBytes * 2 + 1) + index;
			if ( currentIndex + 1 >= NET_PACKET_SIZE || ((currentIndex + 1) >= net_packet->len) )
			{
				// too much data to read, abort
				break;
			}
			int effectIndex = net_packet->data[currentIndex + 0];
			Uint8 effectStrength = net_packet->data[currentIndex + 1];
			stats[clientnum]->setEffectValueUnsafe(effectIndex, effectStrength);
			index += 2;
			--numEffectStrengths;
		}
	}},

	// update entity stat flag
	{'ENSF', [](){
		Entity *entity = uidToEntity((int)SDLNet_Read32(&net_packet->data[4]));
		if ( entity )
		{
			if ( entity->getStats() )
			{
				entity->getStats()->MISC_FLAGS[net_packet->data[8]] = SDLNet_Read32(&net_packet->data[9]);
			}
		}
	}},

	// update attributes
	{'ATTR', [](){
		stats[clientnum]->STR = ((Sint8)net_packet->data[5] <= -8) ? (Uint8)net_packet->data[5] : (Sint8)net_packet->data[5];
		stats[clientnum]->DEX = ((Sint8)net_packet->data[6] <= -8) ? (Uint8)net_packet->data[6] : (Sint8)net_packet->data[6];
		stats[clientnum]->CON = ((Sint8)net_packet->data[7] <= -8) ? (Uint8)net_packet->data[7] : (Sint8)net_packet->data[7];
		stats[clientnum]->INT = ((Sint8)net_packet->data[8] <= -8) ? (Uint8)net_packet->data[8] : (Sint8)net_packet->data[8];
		stats[clientnum]->PER = ((Sint8)net_packet->data[9] <= -8) ? (Uint8)net_packet->data[9] : (Sint8)net_packet->data[9];
		stats[clientnum]->CHR = ((Sint8)net_packet->data[10] <= -8) ? (Uint8)net_packet->data[10] : (Sint8)net_packet->data[10];
		stats[clientnum]->EXP = (Uint8)net_packet->data[11];
		stats[clientnum]->LVL = (Uint8)net_packet->data[12];
		stats[clientnum]->HP = (Sint16)SDLNet_Read16(&net_packet->data[13]);
		stats[clientnum]->MAXHP = (Sint16)SDLNet_Read16(&net_packet->data[15]);
		stats[clientnum]->MP = (Sint16)SDLNet_Read16(&net_packet->data[17]);
		stats[clientnum]->MAXMP = (Sint16)SDLNet_Read16(&net_packet->data[19]);
	}},

	// level up icon timers, sets second row of icons if double stat gain is rolled.
	{'LVLI', [](){
		// Note - set to 250 ticks, higher values will require resending/using 16 bit data.
		players[clientnum]->hud.xpBar.animateState = Player::HUD_t::AnimateStates::ANIMATE_LEVELUP_RISING;
		players[clientnum]->hud.xpBar.xpLevelups++;

		stats[clientnum]->PLAYER_LVL_STAT_TIMER[STAT_STR] = (Uint8)net_packet->data[5];
		stats[clientnum]->PLAYER_LVL_STAT_TIMER[STAT_DEX] = (Uint8)net_packet->data[6];
		stats[clientnum]->PLAYER_LVL_STAT_TIMER[STAT_CON] = (Uint8)net_packet->data[7];
		stats[clientnum]->PLAYER_LVL_STAT_TIMER[STAT_INT] = (Uint8)net_packet->data[8];
		stats[clientnum]->PLAYER_LVL_STAT_TIMER[STAT_PER] = (Uint8)net_packet->data[9];
		stats[clientnum]->PLAYER_LVL_STAT_TIMER[STAT_CHR] = (Uint8)net_packet->data[10];
		stats[clientnum]->PLAYER_LVL_STAT_TIMER[STAT_STR + NUMSTATS] = (Uint8)net_packet->data[11];
		stats[clientnum]->PLAYER_LVL_STAT_TIMER[STAT_DEX + NUMSTATS] = (Uint8)net_packet->data[12];
		stats[clientnum]->PLAYER_LVL_STAT_TIMER[STAT_CON + NUMSTATS] = (Uint8)net_packet->data[13];
		stats[clientnum]->PLAYER_LVL_STAT_TIMER[STAT_INT + NUMSTATS] = (Uint8)net_packet->data[14];
		stats[clientnum]->PLAYER_LVL_STAT_TIMER[STAT_PER + NUMSTATS] = (Uint8)net_packet->data[15];
		stats[clientnum]->PLAYER_LVL_STAT_TIMER[STAT_CHR + NUMSTATS] = (Uint8)net_packet->data[16];

		std::vector<LevelUpAnimation_t::LevelUp_t::StatUp_t> StatUps;
		for ( int i = 0; i < NUMSTATS; ++i )
		{
			if ( stats[clientnum]->PLAYER_LVL_STAT_TIMER[i] > 0 )
			{
				int increase = 1;
				if ( stats[clientnum]->PLAYER_LVL_STAT_TIMER[i + NUMSTATS] > 0 )
				{
					++increase;
				}
				int currentStat = 0;
				switch ( i )
				{
					case STAT_STR:
						currentStat = stats[clientnum]->STR;
						break;
					case STAT_DEX:
						currentStat = stats[clientnum]->DEX;
						break;
					case STAT_CON:
						currentStat = stats[clientnum]->CON;
						break;
					case STAT_INT:
						currentStat = stats[clientnum]->INT;
						break;
					case STAT_PER:
						currentStat = stats[clientnum]->PER;
						break;
					case STAT_CHR:
						currentStat = stats[clientnum]->CHR;
						break;
					default:
						break;
				}
				StatUps.push_back(LevelUpAnimation_t::LevelUp_t::StatUp_t(i, currentStat - increase, increase));
			}
		}
		levelUpAnimation[clientnum].addLevelUp(stats[clientnum]->LVL - 1, 1, StatUps);
	}},

	// killed a monster
	{'MKIL', [](){
		const int monster = (int)net_packet->data[4];
		if ( monster >= 0 && monster < NUMMONSTERS )
		{
			kills[monster]++;
		}
	}},

	// update skill
	{'SKIL', [](){
	    const int pro = std::min(net_packet->data[5], (Uint8)(NUMPROFICIENCIES - 1));
		int oldSkill = stats[clientnum]->getProficiency(pro);
		stats[clientnum]->setProficiency(pro, (net_packet->data[6] & 0x7F));
		bool notify = (net_packet->data[6] & (1 << 7)) != 0;

		int statBonusSkill = getStatForProficiency(pro);

		if ( statBonusSkill >= STAT_STR )
		{
			// stat has chance for bonus point if the relevant proficiency has been trained.
			// write the last proficiency that effected the skill.
			stats[clientnum]->PLAYER_LVL_STAT_BONUS[statBonusSkill] = pro;
		}

		if ( pro == PRO_ALCHEMY )
		{
			GenericGUI[clientnum].alchemyLearnRecipeOnLevelUp(stats[clientnum]->getProficiency(pro));
		}
		if ( oldSkill < 100 )
		{
			if ( notify )
			{
				skillUpAnimation[clientnum].addSkillUp(pro, oldSkill, stats[clientnum]->getProficiency(pro) - oldSkill);
			}
		}
	}},

	//Add spell.
	{'ASPL', [](){
		addSpell(net_packet->data[5], clientnum, true);
	}},

	// update hunger
	{'HNGR', [](){
		stats[clientnum]->HUNGER = (Sint32)SDLNet_Read32(&net_packet->data[4]);
	}},

	// update player stat values
	{'STAT', [](){
		constexpr int expectedLength =
			4 + 8 * MAXPLAYERS;
		if ( net_packet->len < expectedLength )
		{
			printlog(
				"[NET]: ignoring truncated STAT packet (len=%d expected>=%d)",
				net_packet->len,
				expectedLength
			);
			return;
		}

		for ( int i = 0; i < MAXPLAYERS; ++i )
		{
			if ( !stats[i] )
			{
				continue;
			}

			const Uint32 packedHP =
				SDLNet_Read32(
					&net_packet->data[4 + i * 8]
				);
			const Uint32 packedMP =
				SDLNet_Read32(
					&net_packet->data[8 + i * 8]
				);

			stats[i]->MAXHP =
				static_cast<Sint32>(
					packedHP & 0xFFFF
				);
			stats[i]->HP =
				static_cast<Sint32>(
					(packedHP >> 16) & 0xFFFF
				);
			stats[i]->MAXMP =
				static_cast<Sint32>(
					packedMP & 0xFFFF
				);
			stats[i]->MP =
				static_cast<Sint32>(
					(packedMP >> 16) & 0xFFFF
				);
		}
	}},

	// update sex
	{'SEXU', [](){
		int player = static_cast<int>(net_packet->data[4]);
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] )
		{
			return;
		}
		stats[player]->sex = (sex_t)(net_packet->data[5]);
		//messagePlayer(clientnum, "Received player: %d sex: %d", player, stats[player]->sex);
		return;
	}},

	{'COND', [](){
		int conduct = SDLNet_Read16(&net_packet->data[4]);
		int value = SDLNet_Read16(&net_packet->data[6]);
		conductGameChallenges[conduct] = value;
		//messagePlayer(clientnum, "received %d %d, set to %d", conduct, value, conductGameChallenges[conduct]);
	}},

	// update player statistics
	{'GPST', [](){
		int gameplayStat = SDLNet_Read32(&net_packet->data[4]);
		int changeval = SDLNet_Read32(&net_packet->data[8]);
		if ( gameplayStat == STATISTICS_TEMPT_FATE )
		{
			if ( gameStatistics[STATISTICS_TEMPT_FATE] == -1 )
			{
				// don't change, completed task.
			}
			else
			{
				if ( changeval == 5 )
				{
					gameStatistics[gameplayStat] = changeval;
				}
				else if ( changeval == 1 && gameStatistics[gameplayStat] > 0 )
				{
					gameStatistics[gameplayStat] = -1;
				}
			}
		}
		else if ( gameplayStat == STATISTICS_FORUM_TROLL )
		{
			if ( changeval == AchievementObserver::FORUM_TROLL_BREAK_WALL )
			{
				int walls = gameStatistics[gameplayStat] & 0xFF;
				walls = std::min(walls + 1, 3);
				gameStatistics[gameplayStat] = gameStatistics[gameplayStat] & 0xFFFFFF00;
				gameStatistics[gameplayStat] |= walls;
			}
			else if ( changeval == AchievementObserver::FORUM_TROLL_RECRUIT_TROLL )
			{
				int trolls = (gameStatistics[gameplayStat] >> 8) & 0xFF;
				trolls = std::min(trolls + 1, 3);
				gameStatistics[gameplayStat] = gameStatistics[gameplayStat] & 0xFFFF00FF;
				gameStatistics[gameplayStat] |= (trolls << 8);
			}
			else if ( changeval == AchievementObserver::FORUM_TROLL_FEAR )
			{
				int fears = (gameStatistics[gameplayStat] >> 16) & 0xFF;
				fears = std::min(fears + 1, 3);
				gameStatistics[gameplayStat] = gameStatistics[gameplayStat] & 0xFF00FFFF;
				gameStatistics[gameplayStat] |= (fears << 16);
			}
		}
		else if ( gameplayStat == STATISTICS_POP_QUIZ_1 || gameplayStat == STATISTICS_POP_QUIZ_2 )
		{
			int spellID = changeval;
			if ( spellID >= 32 )
			{
				spellID -= 32;
				int shifted = (1 << spellID);
				gameStatistics[gameplayStat] |= shifted;
			}
			else
			{
				int shifted = (1 << spellID);
				gameStatistics[gameplayStat] |= shifted;
			}
		}
		else if ( gameplayStat == STATISTICS_FLAVORTOWN )
		{
			gameStatistics[gameplayStat] |= changeval;
		}
		else if ( gameplayStat == STATISTICS_BARDIC_INSPIRATION )
		{
			if ( changeval == 0 )
			{
				gameStatistics[gameplayStat] = 0;
			}
			else
			{
				gameStatistics[gameplayStat] += changeval;
			}
		}
		else if ( gameplayStat == STATISTICS_PARRY_TANK )
		{
			if ( changeval == 0 )
			{
				if ( gameStatistics[gameplayStat] < 20 )
				{
					gameStatistics[gameplayStat] = 0;
				}
			}
			else
			{
				gameStatistics[gameplayStat] += changeval;
			}
		}
		else
		{
			gameStatistics[gameplayStat] += changeval;
		}
		//messagePlayer(clientnum, "received: %d, %d, val: %d", gameplayStat, changeval, gameStatistics[gameplayStat]);
	}},

	// update player levels
	{'UPLV', [](){
		const int expectedLength =
			4 + MAXPLAYERS;
		if ( net_packet->len < expectedLength )
		{
			printlog(
				"[NET]: ignoring truncated UPLV packet (len=%d expected>=%d)",
				net_packet->len,
				expectedLength
			);
			return;
		}

		for ( int i = 0; i < MAXPLAYERS; ++i )
		{
			if ( stats[i] )
			{
				stats[i]->LVL =
					static_cast<Sint32>(
						net_packet->data[4 + i]
					);
			}
		}
	}},
	// Authoritative persistent gate state.
	{'PWGT', []()
	{
		if ( net_packet->len < 37 )
		{
			return;
		}

		receiveClientPersistentGateState(
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[4]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[8]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[12]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[16]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[20]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[24]
				)
			) / 65536.0,
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[28]
				)
			) / 65536.0,
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[32]
				)
			) / 65536.0,
			net_packet->data[36] != 0
		);
	}},
	// Authoritative persistent lever state.
	{'PWLV', []()
	{
		if ( net_packet->len < 25 )
		{
			return;
		}

		receiveClientPersistentLeverState(
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[4]
				)
			),
			net_packet->data[8] != 0,
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[9]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[13]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[17]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[21]
				)
			) / 65536.0
		);
	}},
	// Authoritative persistent removal.
	{'PWRM', []()
	{
		if ( net_packet->len < 8 )
		{
			return;
		}

		receiveClientPersistentRemoval(
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[4]
				)
			)
		);
	}},
	// Begin authoritative persistent-world snapshot.
	{'PWBG', []()
	{
		if ( net_packet->len < 6 )
		{
			printlog(
				"[Persistent World MP] Ignored malformed PWBG packet."
			);
			return;
		}

		const char* mapName =
			reinterpret_cast<const char*>(
				&net_packet->data[4]
			);

		const size_t availableLength =
			net_packet->len - 4;

		if ( memchr(
			mapName,
			'\0',
			availableLength
		) == nullptr )
		{
			printlog(
				"[Persistent World MP] Ignored unterminated PWBG map name."
			);
			return;
		}

		beginClientPersistentWorldSnapshot(
			mapName
		);
	}},
	// Authoritative persistent wooden/iron door state.
	{'PWDR', []()
	{
		if ( net_packet->len < 88 )
		{
			printlog(
				"[Persistent World MP] Ignored malformed PWDR packet with length %d.",
				net_packet->len
			);
			return;
		}

		receiveClientPersistentDoorState(
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[4]
				)
			),
			net_packet->data[8] != 0,
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[9]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[13]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[17]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[21]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[25]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[29]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[33]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[37]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[41]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[45]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[49]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[53]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[57]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[61]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[65]
				)
			) / 65536.0,
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[69]
				)
			) / 65536.0,
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[73]
				)
			) / 65536.0,
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[77]
				)
			) / 65536.0,
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[81]
				)
			) / 65536.0,
			net_packet->data[85] != 0,
			net_packet->data[86] != 0,
			net_packet->data[87] != 0
		);
	}},
	// Authoritative persistent furniture state.
	{'PWFU', []()
	{
		if ( net_packet->len < 22 )
		{
			printlog(
				"[Persistent World MP] Ignored malformed PWFU packet with length %d.",
				net_packet->len
			);
			return;
		}

		receiveClientPersistentFurnitureState(
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[4]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[8]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[12]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[16]
				)
			),
			net_packet->data[20] != 0,
			net_packet->data[21] != 0
		);
	}},
	// Authoritative persistent collider-decoration state.
	{'PWCD', []()
	{
		if ( net_packet->len < 26 )
		{
			printlog(
				"[Persistent World MP] Ignored malformed PWCD packet with length %d.",
				net_packet->len
			);

			return;
		}

		receiveClientPersistentColliderState(
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[4]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[8]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[12]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[16]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[20]
				)
			),
			net_packet->data[24] != 0,
			net_packet->data[25] != 0
		);
	}},
	// Authoritative persistent power-crystal state.
	{'PWPC', []()
	{
		if ( net_packet->len < 36 )
		{
			printlog(
				"[Persistent World MP] Ignored malformed PWPC packet with length %d.",
				net_packet->len
			);

			return;
		}

		receiveClientPersistentPowerCrystalState(
		static_cast<Sint32>(
			SDLNet_Read32(
				&net_packet->data[4]
			)
		),
		static_cast<Sint32>(
			SDLNet_Read32(
				&net_packet->data[8]
			)
		),
		static_cast<Sint32>(
			SDLNet_Read32(
				&net_packet->data[12]
			)
		),
		static_cast<Sint32>(
			SDLNet_Read32(
				&net_packet->data[16]
			)
		),
		static_cast<Sint32>(
			SDLNet_Read32(
				&net_packet->data[20]
			)
		),
		static_cast<Sint32>(
			SDLNet_Read32(
				&net_packet->data[24]
			)
		),
		static_cast<Sint32>(
			SDLNet_Read32(
				&net_packet->data[28]
			)
		),
		static_cast<Sint32>(
			SDLNet_Read32(
				&net_packet->data[32]
			)
		)
	);
	}},
	    // Authoritative persistent boulder-trap state.
    {'PWBT', []()
    {
        if ( net_packet->len < 36 )
        {
            printlog(
                "[Persistent World MP] Ignored malformed PWBT packet with length %d.",
                net_packet->len
            );

            return;
        }

        receiveClientPersistentBoulderTrapState(
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[4]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[8]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[12]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[16]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[20]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[24]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[28]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[32]
                )
            )
        );
    }},
	    // Authoritative persistent signal-timer / AND-gate state.
    {'PWSG', []()
    {
        if ( net_packet->len < 44 )
        {
            printlog(
                "[Persistent World MP] Ignored malformed PWSG packet with length %d.",
                net_packet->len
            );

            return;
        }

        receiveClientPersistentSignalControllerState(
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[4]
                )
            ),
            SDLNet_Read32(
                &net_packet->data[8]
            ) != 0,
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[12]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[16]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[20]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[24]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[28]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[32]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[36]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[40]
                )
            )
        );
    }},
	    // Authoritative persistent bell state.
    {'PWBL', []()
    {
        if ( net_packet->len < 51 )
        {
            printlog(
                "[Persistent World MP] Ignored malformed PWBL packet with length %d.",
                net_packet->len
            );

            return;
        }

        receiveClientPersistentBellState(
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[4]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[8]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[12]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[16]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[20]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[24]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[28]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[32]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[36]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[40]
                )
            ),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[44]
				)
			),
			net_packet->data[48] != 0,
			net_packet->data[49] != 0,
			net_packet->data[50] != 0
        );
    }},
	    // Authoritative persistent sink/fountain state.
    {'PWSW', []()
    {
        if ( net_packet->len < 24 )
        {
            printlog(
                "[Persistent World MP] Ignored malformed PWSW packet with length %d.",
                net_packet->len
            );

            return;
        }

        receiveClientPersistentWaterSourceState(
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[4]
                )
            ),
            SDLNet_Read32(
                &net_packet->data[8]
            ) != 0,
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[12]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[16]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[20]
                )
            )
        );
    }},
	    // Authoritative persistent campfire state.
    {'PWCF', []()
    {
        if ( net_packet->len < 12 )
        {
            printlog(
                "[Persistent World MP] Ignored malformed PWCF packet with length %d.",
                net_packet->len
            );

            return;
        }

        receiveClientPersistentCampfireState(
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[4]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[8]
                )
            )
        );
    }},
	    // Authoritative persistent wall-lock state.
    {'PWWL', []()
    {
        if ( net_packet->len < 24 )
        {
            printlog(
                "[Persistent World MP] Ignored malformed PWWL packet with length %d.",
                net_packet->len
            );

            return;
        }

        receiveClientPersistentWallLockState(
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[4]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[8]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[12]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[16]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[20]
                )
            )
        );
    }},
	    // Authoritative persistent wall-button state.
    {'PWBW', []()
    {
        if ( net_packet->len < 16 )
        {
            printlog(
                "[Persistent World MP] Ignored malformed PWBW packet with length %d.",
                net_packet->len
            );

            return;
        }

        receiveClientPersistentWallButtonState(
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[4]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[8]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[12]
                )
            )
        );
    }},
	    // Authoritative persistent pressure-plate state.
    {'PWPP', []()
    {
        if ( net_packet->len < 20 )
        {
            printlog(
                "[Persistent World MP] Ignored malformed PWPP packet with length %d.",
                net_packet->len
            );

            return;
        }

        receiveClientPersistentPressurePlateState(
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[4]
                )
            ),
            SDLNet_Read32(
                &net_packet->data[8]
            ) != 0,
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[12]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[16]
                )
            )
        );
    }},
	    // Authoritative persistent tile override.
    {'PWTL', []()
    {
        if ( net_packet->len < 20 )
        {
            printlog(
                "[Persistent World MP] Ignored malformed PWTL packet with length %d.",
                net_packet->len
            );

            return;
        }

        receiveClientPersistentTileState(
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[4]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[8]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[12]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[16]
                )
            )
        );
    }},
	    // Authoritative persistent orb-pedestal state.
    {'PWPD', []()
    {
        if ( net_packet->len < 33 )
        {
            printlog(
                "[Persistent World MP] Ignored malformed PWPD packet with length %d.",
                net_packet->len
            );

            return;
        }

        receiveClientPersistentPedestalState(
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[4]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[8]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[12]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[16]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[20]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[24]
                )
            ) / 65536.0,
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[28]
                )
            ) / 65536.0,
            net_packet->data[32] != 0
        );
    }},
	    // Authoritative persistent chest state.
    {'PWCH', []()
    {
        if ( net_packet->len < 36 )
        {
            printlog(
                "[Persistent World MP] Ignored malformed PWCH packet with length %d.",
                net_packet->len
            );

            return;
        }

        receiveClientPersistentChestState(
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[4]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[8]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[12]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[16]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[20]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[24]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[28]
                )
            ),
            static_cast<Sint32>(
                SDLNet_Read32(
                    &net_packet->data[32]
                )
            )
        );
    }},
	// Authoritative persistent summoning-trap state.
	{'PWST', []()
	{
		if ( net_packet->len < 48 )
		{
			printlog(
				"[Persistent World MP] Ignored malformed PWST packet with length %d.",
				net_packet->len
			);

			return;
		}

		receiveClientPersistentSummonTrapState(
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[4]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[8]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[12]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[16]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[20]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[24]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[28]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[32]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[36]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[40]
				)
			),
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[44]
				)
			)
		);
	}},
	// Finish authoritative persistent-world snapshot.
	{'PWEN', []()
	{
		finishClientPersistentWorldSnapshot();
	}},
	// level change
	{'LVLC', []()
	{
		/*
		* An LVLC packet that reports our existing level and secret-floor
		* status is normally only the server's routine consistency check.
		*
		* A custom named-map transition is different: it may intentionally
		* load another map using the same numeric level, so do not discard it
		* when a map name is present.
		*/
		const bool hasExplicitCustomMap =
			net_packet->len > 14
			&& net_packet->data[14] != 0;

		if ( currentlevel
				== static_cast<Sint8>(net_packet->data[13])
			&& secretlevel == net_packet->data[4]
			&& !hasExplicitCustomMap )
		{
			return;
		}

		/*
		* Original Barony blocks ordinary LVLC packets from warping clients
		* back to level zero. Preserve that protection for legacy packets,
		* but allow an explicit custom tunnel destination such as "start".
		*/
		if ( static_cast<Sint8>(net_packet->data[13]) == 0
			&& !hasExplicitCustomMap )
		{
			return;
		}
		printlog(
    "[Custom Tunnel] Accepting LVLC destination level %d with explicit map=%d.",
    static_cast<Sint8>(net_packet->data[13]),
    hasExplicitCustomMap ? 1 : 0
);
		changeLevel();
	}},
	// Server-authoritative spawn position after a custom tunnel load.
	{'TNSP', [](){
		if ( net_packet->len < 20 )
		{
			printlog(
				"[Custom Tunnel] Ignored malformed TNSP packet with length %d.",
				net_packet->len
			);
			return;
		}

		pendingTunnelSpawn.x =
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[4]
				)
			) / 32.0;

		pendingTunnelSpawn.y =
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[8]
				)
			) / 32.0;

		pendingTunnelSpawn.z =
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[12]
				)
			) / 32.0;

		pendingTunnelSpawn.yaw =
			static_cast<Sint32>(
				SDLNet_Read32(
					&net_packet->data[16]
				)
			) / 256.0;

		pendingTunnelSpawn.active = true;

		printlog(
			"[Custom Tunnel] Client received server tunnel spawn: x=%.2f y=%.2f z=%.2f yaw=%.2f.",
			pendingTunnelSpawn.x,
			pendingTunnelSpawn.y,
			pendingTunnelSpawn.z,
			pendingTunnelSpawn.yaw
		);

		applyPendingTunnelSpawn();
	}},
	// level reminder
	{'LVLR', [](){
		changeLevel();
	}},

	// lead a monster
	{'LEAD', [](){
		Uint32* uidnum = (Uint32*) malloc(sizeof(Uint32));
		*uidnum = (Uint32)SDLNet_Read32(&net_packet->data[4]);
		node_t* node = list_AddNodeLast(&stats[clientnum]->FOLLOWERS);
		node->element = uidnum;
		node->deconstructor = &defaultDeconstructor;
		node->size = sizeof(Uint32);

		Entity* monster = uidToEntity(*uidnum);
		if ( monster )
		{
			if ( !monster->clientsHaveItsStats )
			{
				monster->giveClientStats();
			}
			if ( monster->clientStats )
			{
                monster->clientStats->type = (Monster)SDLNet_Read32(&net_packet->data[8]);
				if ( (Sint8)net_packet->data[12] == '$' )
				{
					char buf[128];
					memset(buf, 0, sizeof(buf));
					strcpy(buf, (char*)&net_packet->data[13]);
					bool found = false;
					for ( int type = 0; type < NUMMONSTERS; ++type )
					{
						if ( MonsterData_t::monsterDataEntries[type].specialNPCs.find(buf) != MonsterData_t::monsterDataEntries[type].specialNPCs.end() )
						{
							strcpy(monster->clientStats->name, MonsterData_t::monsterDataEntries[type].specialNPCs[buf].name.c_str());
							found = true;
							break;
						}
					}
					if ( !found )
					{
						strcpy(monster->clientStats->name, buf);
					}
				}
				else
				{
					strcpy(monster->clientStats->name, (char*)&net_packet->data[12]);
				}
                if ( monster->clientStats->name[0] && (!monsterNameIsGeneric(*monster->clientStats) || monster->clientStats->type == SLIME)) 
				{
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
                    nametag->skill[0] = clientnum;
                    nametag->skill[1] = playerColor(clientnum, colorblind_lobby, true);
                }
			}
			if ( !FollowerMenu[clientnum].recentEntity )
			{
				FollowerMenu[clientnum].recentEntity = monster;
			}
		}
	}},

	// remove a monster from followers list
	{'LDEL', [](){
		Uint32 uidnum = (Uint32)SDLNet_Read32(&net_packet->data[4]);
		if ( stats[clientnum] )
		{
			for ( node_t* allyNode = stats[clientnum]->FOLLOWERS.first; allyNode != nullptr; allyNode = allyNode->next )
			{
				if ( (Uint32*)allyNode->element && *((Uint32*)allyNode->element) == uidnum )
				{
					if ( FollowerMenu[clientnum].recentEntity && (FollowerMenu[clientnum].recentEntity->getUID() == 0
						|| FollowerMenu[clientnum].recentEntity->getUID() == uidnum) )
					{
						FollowerMenu[clientnum].recentEntity = nullptr;
					}
					if ( FollowerMenu[clientnum].followerToCommand == uidToEntity(uidnum) )
					{
						FollowerMenu[clientnum].closeFollowerMenuGUI();
					}
					list_RemoveNode(allyNode);
					break;
				}
			}
		}
	}},

	// update client's follower data on level up or initial follow.
	{'NPCI', [](){
		Uint32 uidnum = (Uint32)SDLNet_Read32(&net_packet->data[4]);
		Entity* monster = uidToEntity(uidnum);
		if ( monster )
		{
			if ( !monster->clientsHaveItsStats )
			{
				monster->giveClientStats();
			}
			if ( monster->clientStats )
			{
				monster->clientStats->LVL = net_packet->data[8];
				monster->clientStats->HP = SDLNet_Read16(&net_packet->data[9]);
				monster->clientStats->MAXHP = SDLNet_Read16(&net_packet->data[11]);
				monster->clientStats->type = static_cast<Monster>(net_packet->data[13]);
			}
		}
	}},

	// update client's follower hp/maxhp data at intervals
	{'NPCU', [](){
		Uint32 uidnum = (Uint32)SDLNet_Read32(&net_packet->data[4]);
		Entity* monster = uidToEntity(uidnum);
		if ( monster )
		{
			if ( !monster->clientsHaveItsStats )
			{
				monster->giveClientStats();
			}
			if ( monster->clientStats )
			{
				monster->clientStats->HP = SDLNet_Read16(&net_packet->data[8]);
				monster->clientStats->MAXHP = SDLNet_Read16(&net_packet->data[10]);
			}
		}
	}},

	// bless my equipment
	{'BLES', [](){
		if ( stats[clientnum]->helmet )
		{
			stats[clientnum]->helmet->beatitude++;
			Compendium_t::Events_t::eventUpdate(clientnum, Compendium_t::CPDM_BLESSED_TOTAL, stats[clientnum]->helmet->type, 1);
		}
		if ( stats[clientnum]->breastplate )
		{
			stats[clientnum]->breastplate->beatitude++;
			Compendium_t::Events_t::eventUpdate(clientnum, Compendium_t::CPDM_BLESSED_TOTAL, stats[clientnum]->breastplate->type, 1);
		}
		if ( stats[clientnum]->gloves )
		{
			stats[clientnum]->gloves->beatitude++;
			Compendium_t::Events_t::eventUpdate(clientnum, Compendium_t::CPDM_BLESSED_TOTAL, stats[clientnum]->gloves->type, 1);
		}
		if ( stats[clientnum]->shoes )
		{
			stats[clientnum]->shoes->beatitude++;
			Compendium_t::Events_t::eventUpdate(clientnum, Compendium_t::CPDM_BLESSED_TOTAL, stats[clientnum]->shoes->type, 1);
		}
		if ( stats[clientnum]->shield )
		{
			stats[clientnum]->shield->beatitude++;
			Compendium_t::Events_t::eventUpdate(clientnum, Compendium_t::CPDM_BLESSED_TOTAL, stats[clientnum]->shield->type, 1);
		}
		if ( stats[clientnum]->weapon )
		{
			stats[clientnum]->weapon->beatitude++;
			Compendium_t::Events_t::eventUpdate(clientnum, Compendium_t::CPDM_BLESSED_TOTAL, stats[clientnum]->weapon->type, 1);
		}
		if ( stats[clientnum]->cloak )
		{
			stats[clientnum]->cloak->beatitude++;
			Compendium_t::Events_t::eventUpdate(clientnum, Compendium_t::CPDM_BLESSED_TOTAL, stats[clientnum]->cloak->type, 1);
		}
		if ( stats[clientnum]->amulet )
		{
			stats[clientnum]->amulet->beatitude++;
			Compendium_t::Events_t::eventUpdate(clientnum, Compendium_t::CPDM_BLESSED_TOTAL, stats[clientnum]->amulet->type, 1);
		}
		if ( stats[clientnum]->ring )
		{
			stats[clientnum]->ring->beatitude++;
			Compendium_t::Events_t::eventUpdate(clientnum, Compendium_t::CPDM_BLESSED_TOTAL, stats[clientnum]->ring->type, 1);
		}
		if ( stats[clientnum]->mask )
		{
			stats[clientnum]->mask->beatitude++;
			Compendium_t::Events_t::eventUpdate(clientnum, Compendium_t::CPDM_BLESSED_TOTAL, stats[clientnum]->mask->type, 1);
		}
	}},

	// bless one piece of my equipment
	{'BLE1', [](){
		Uint32 chosen = static_cast<Uint32>(SDLNet_Read32(&net_packet->data[4]));
		switch ( chosen )
		{
			case 0:
				if ( stats[clientnum]->helmet )
				{
					stats[clientnum]->helmet->beatitude++;
					Compendium_t::Events_t::eventUpdate(clientnum, Compendium_t::CPDM_BLESSED_TOTAL, stats[clientnum]->helmet->type, 1);
				}
				break;
			case 1:
				if ( stats[clientnum]->breastplate )
				{
					stats[clientnum]->breastplate->beatitude++;
					Compendium_t::Events_t::eventUpdate(clientnum, Compendium_t::CPDM_BLESSED_TOTAL, stats[clientnum]->breastplate->type, 1);
				}
				break;
			case 2:
				if ( stats[clientnum]->gloves )
				{
					stats[clientnum]->gloves->beatitude++;
					Compendium_t::Events_t::eventUpdate(clientnum, Compendium_t::CPDM_BLESSED_TOTAL, stats[clientnum]->gloves->type, 1);
				}
				break;
			case 3:
				if ( stats[clientnum]->shoes )
				{
					stats[clientnum]->shoes->beatitude++;
					Compendium_t::Events_t::eventUpdate(clientnum, Compendium_t::CPDM_BLESSED_TOTAL, stats[clientnum]->shoes->type, 1);
				}
				break;
			case 4:
				if ( stats[clientnum]->shield )
				{
					stats[clientnum]->shield->beatitude++;
					Compendium_t::Events_t::eventUpdate(clientnum, Compendium_t::CPDM_BLESSED_TOTAL, stats[clientnum]->shield->type, 1);
				}
				break;
			case 5:
				if ( stats[clientnum]->weapon )
				{
					stats[clientnum]->weapon->beatitude++;
					Compendium_t::Events_t::eventUpdate(clientnum, Compendium_t::CPDM_BLESSED_TOTAL, stats[clientnum]->weapon->type, 1);
				}
				break;
			case 6:
				if ( stats[clientnum]->cloak )
				{
					stats[clientnum]->cloak->beatitude++;
					Compendium_t::Events_t::eventUpdate(clientnum, Compendium_t::CPDM_BLESSED_TOTAL, stats[clientnum]->cloak->type, 1);
				}
				break;
			case 7:
				if ( stats[clientnum]->amulet )
				{
					stats[clientnum]->amulet->beatitude++;
					Compendium_t::Events_t::eventUpdate(clientnum, Compendium_t::CPDM_BLESSED_TOTAL, stats[clientnum]->amulet->type, 1);
				}
				break;
			case 8:
				if ( stats[clientnum]->ring )
				{
					stats[clientnum]->ring->beatitude++;
					Compendium_t::Events_t::eventUpdate(clientnum, Compendium_t::CPDM_BLESSED_TOTAL, stats[clientnum]->ring->type, 1);
				}
				break;
			case 9:
				if ( stats[clientnum]->mask )
				{
					stats[clientnum]->mask->beatitude++;
					Compendium_t::Events_t::eventUpdate(clientnum, Compendium_t::CPDM_BLESSED_TOTAL, stats[clientnum]->mask->type, 1);
				}
				break;
			default:
				break;
		}
	}},

	// update entity appearance (sprite)
	{'ENTA', [](){
		Entity *entity = uidToEntity((int)SDLNet_Read32(&net_packet->data[4]));
		if ( entity )
		{
			entity->sprite = SDLNet_Read32(&net_packet->data[8]);
		}
	}},

	// monster summon
	{'SUMM', [](){
		Monster monster = (Monster)SDLNet_Read32(&net_packet->data[4]);
		Sint32 x = (Sint32)SDLNet_Read32(&net_packet->data[8]);
		Sint32 y = (Sint32)SDLNet_Read32(&net_packet->data[12]);
		Uint32 uid = SDLNet_Read32(&net_packet->data[16]);
		summonMonsterClient(monster, x, y, uid);
	}},

	// monster summon
	{'SUMS', [](){
		if ( stats[clientnum] )
		{
			stats[clientnum]->playerSummonLVLHP = (Sint32)SDLNet_Read32(&net_packet->data[4]);
			stats[clientnum]->playerSummonSTRDEXCONINT = (Sint32)SDLNet_Read32(&net_packet->data[8]);
			stats[clientnum]->playerSummonPERCHR = (Sint32)SDLNet_Read32(&net_packet->data[12]);
			stats[clientnum]->playerSummon2LVLHP = (Sint32)SDLNet_Read32(&net_packet->data[16]);
			stats[clientnum]->playerSummon2STRDEXCONINT = (Sint32)SDLNet_Read32(&net_packet->data[20]);
			stats[clientnum]->playerSummon2PERCHR = (Sint32)SDLNet_Read32(&net_packet->data[24]);
		}
	}},

	//Multiplayer chest code (client).
	{'CHST', [](){
		if ( openedChest[clientnum] )
		{
			//Close the chest.
			closeChestClientside(clientnum);
		}

		Entity *entity = uidToEntity((int)SDLNet_Read32(&net_packet->data[4]));
		if ( entity )
		{
			openedChest[clientnum] = entity; //Set the opened chest to this.
			GenericGUI[clientnum].closeGUI();
			list_FreeAll(&chestInv[clientnum]);
			chestInv[clientnum].first = nullptr;
			chestInv[clientnum].last = nullptr;
			players[clientnum]->openStatusScreen(GUI_MODE_INVENTORY, INVENTORY_MODE_ITEM);
			players[clientnum]->GUI.activateModule(Player::GUI_t::MODULE_CHEST);
			bool voidChest = net_packet->data[8] == 0 ? false : true;
			players[clientnum]->inventoryUI.chestGUI.openChest(voidChest);
		}
	}},

    //Add an item to the chest.
    {'CITM', [](){
        if ( net_packet->len < 28 )
        {
            printlog(
                "[NET]: ignoring malformed CITM packet with length %d.\n",
                net_packet->len
            );
            return;
        }

        int resolvedType =
            static_cast<int>(SDLNet_Read32(&net_packet->data[4]));
#ifdef SAM_FRAMEWORK_ENABLED
        if ( !resolveSAMItemTypeFromPacket(
            resolvedType,
            28,
            "CITM",
            resolvedType
        ) )
        {
            return;
        }
#endif

        const Status status =
            static_cast<Status>(SDLNet_Read32(&net_packet->data[8]));
        const Sint16 beatitude = SDLNet_Read32(&net_packet->data[12]);
        const Sint16 count = SDLNet_Read32(&net_packet->data[16]);
        const Uint32 appearance = SDLNet_Read32(&net_packet->data[20]);
        const bool identified = net_packet->data[24] != 0;
        Item* newitem = newItem(
            static_cast<ItemType>(resolvedType),
            status,
            beatitude,
            count,
            appearance,
            identified,
            nullptr
        );
        if ( !newitem )
        {
            return;
        }

        const bool forceNewStack = net_packet->data[25] != 0;
        newitem->x = static_cast<Sint8>(net_packet->data[26]);
        newitem->y = static_cast<Sint8>(net_packet->data[27]);
        addItemToChestClientside(
            clientnum,
            newitem,
            forceNewStack,
            nullptr
        );
    }},

	//Close the chest.
	{'CCLS', [](){
		closeChestClientside(clientnum);
	}},

	//Open up the GUI to identify an item.
	{'IDEN', [](){
		if ( net_packet->data[4] == 1 ) // spellbook
		{
			int beatitude = static_cast<Sint8>(net_packet->data[5]);
			GenericGUI[clientnum].openGUI(GUI_TYPE_ITEMFX, nullptr, beatitude, getSpellbookFromSpellID(SPELL_IDENTIFY), SPELL_IDENTIFY);
		}
		else
		{
			GenericGUI[clientnum].openGUI(GUI_TYPE_ITEMFX, nullptr, 0, SPELL_ITEM, SPELL_IDENTIFY);
		}
	}},

	// Open up the Remove Curse GUI
	{'CRCU', [](){
		//Uncurse an item
		if ( net_packet->data[4] == 1 ) // spellbook
		{
			int beatitude = static_cast<Sint8>(net_packet->data[5]);
			GenericGUI[clientnum].openGUI(GUI_TYPE_ITEMFX, nullptr, beatitude, getSpellbookFromSpellID(SPELL_REMOVECURSE), SPELL_REMOVECURSE);
		}
		else
		{
			GenericGUI[clientnum].openGUI(GUI_TYPE_ITEMFX, nullptr, 0, SPELL_ITEM, SPELL_REMOVECURSE);
		}
	}},

	{'FXSP', []() {
		int spellID = SDLNet_Read32(&net_packet->data[6]);
		if ( net_packet->data[4] == 1 ) // spellbook
		{
			int beatitude = static_cast<Sint8>(net_packet->data[5]);
			GenericGUI[clientnum].openGUI(GUI_TYPE_ITEMFX, nullptr, beatitude, getSpellbookFromSpellID(spellID), spellID);
		}
		else
		{
			GenericGUI[clientnum].openGUI(GUI_TYPE_ITEMFX, nullptr, 0, SPELL_ITEM, spellID);
		}
	}},

	//Add a spell to the channeled spells list.
	{'CHAN', [](){
		if ( auto spell = getSpellFromID(SDLNet_Read32(&net_packet->data[5])) )
		{
			if ( spell_t* thespell = copySpell(spell) )
			{
				auto node = list_AddNodeLast(&channeledSpells[clientnum]);
				node->element = thespell;
				node->size = sizeof(spell_t);
				//node->deconstructor = &spellDeconstructor_Channeled;
				node->deconstructor = &spellChanneledClientDeconstructor;
				((spell_t*)(node->element))->sustain_node = node;
			}
		}		
	}},

	//Remove a spell from the channeled spells list.
	{'UNCH', [](){
		spell_t* thespell = getSpellFromID(SDLNet_Read32(&net_packet->data[5]));
		if (spellInList(&channeledSpells[clientnum], thespell))
		{
			node_t *node, *nextnode;
			for (node = channeledSpells[clientnum].first; node; node = nextnode)
			{
				nextnode = node->next;
				spell_t* spell_search = (spell_t*)node->element;
				if (spell_search->ID == thespell->ID)
				{
					list_RemoveNode(node);
					node = NULL;
				}
			}
		}
	}},

	//Map the magic. I mean magic the map. I mean magically map the level (client).
	{'MMAP', [](){
		int radius = SDLNet_Read16(&net_packet->data[4]);
		int x = SDLNet_Read16(&net_packet->data[6]);
		int y = SDLNet_Read16(&net_packet->data[8]);
		spell_magicMap(clientnum, radius, x, y);
	}},

	{'MFOD', [](){
		mapFoodOnLevel(clientnum);
	}},

	{'TKIT', [](){
		GenericGUI[clientnum].tinkeringKitDegradeOnUse(clientnum);
	}},

	// leaf pile
	{ 'LEAF', []() {
		Uint32 uid = SDLNet_Read32(&net_packet->data[4]);
		if ( Entity* entity = uidToEntity(uid) )
		{
			if ( net_packet->data[8] == 1 )
			{
				entity->skill[3] = (Sint32)net_packet->data[9];
				entity->skill[4] = (Sint32)net_packet->data[10];
				playSoundEntityLocal(entity, 754 + local_rng.rand() % 2, 64);
			}
			else if ( net_packet->data[8] == 2 )
			{
				entity->skill[7] = (Sint32)net_packet->data[9];
				entity->skill[8] = (Sint32)net_packet->data[10];
			}
		}
	} },

	// boss death
	{'BDTH', [](){
		for ( auto node = map.entities->first; node != nullptr; node = node->next )
		{
			Entity* entity = (Entity*)node->element;
			if ( strstr(map.name, "Hell") )
			{
				if ( entity->behavior == &actWinningPortal )
				{
					//entity->flags[INVISIBLE] = false;
				}
			}
			else if ( strstr(map.name, "Boss") )
			{
				if ( entity->behavior == &actPedestalBase )
				{
					entity->pedestalInit = 1;
				}
			}
		}
		if ( strstr(map.name, "Hell") )
		{
			int x, y;
			for ( y = map.height / 2 - 1; y < map.height / 2 + 2; y++ )
			{
				for ( x = 3; x < map.width / 2; x++ )
				{
					if ( !map.tiles[y * MAPLAYERS + x * MAPLAYERS * map.height] )
					{
						map.tiles[y * MAPLAYERS + x * MAPLAYERS * map.height] = 72;
					}
				}
			}
		}
	}},

	// update svFlags
	{'SVFL', [](){
		svFlags = SDLNet_Read32(&net_packet->data[4]);
		lobbyWindowSvFlags = svFlags;
	}},

	// kick
	{'KICK', [](){
		MainMenu::timedOut();
	}},

	// win the game
	{'WING', [](){
		if ( net_packet->data[4] == 100 || net_packet->data[4] == 101 )
		{
			movie = true;
			pauseGame(2, 0);
			MainMenu::destroyMainMenu();
			MainMenu::createDummyMainMenu();
			beginFade(MainMenu::FadeDestination::Endgame);
			return;
		}


		int victoryType;
		int race = RACE_HUMAN;
		if ( stats[clientnum]->playerRace != RACE_HUMAN && stats[clientnum]->stat_appearance == 0 )
		{
			race = stats[clientnum]->playerRace;
		}

		switch ( race ) {
		default: victoryType = 3; break;
		case RACE_HUMAN: victoryType = 4; break;
		case RACE_SKELETON: victoryType = 5; break;
		case RACE_VAMPIRE: victoryType = 5; break;
		case RACE_SUCCUBUS: victoryType = 5; break;
		case RACE_GOATMAN: victoryType = 3; break;
		case RACE_AUTOMATON: victoryType = 4; break;
		case RACE_INCUBUS: victoryType = 5; break;
		case RACE_GOBLIN: victoryType = 3; break;
		case RACE_INSECTOID: victoryType = 3; break;
		case RACE_RAT: victoryType = 3; break;
		case RACE_TROLL: victoryType = 3; break;
		case RACE_SPIDER: victoryType = 3; break;
		case RACE_IMP: victoryType = 5; break;
		case RACE_DRYAD: victoryType = 4; break;
		case RACE_MYCONID: victoryType = 4; break;
		case RACE_SALAMANDER: victoryType = 4; break;
		case RACE_GREMLIN: victoryType = 5; break;
		case RACE_GNOME: victoryType = 4; break;
		}
		victory = victoryType;
	    if (net_packet->data[5] == 0) { // full ending
	        switch ( race ) {
	        default:
	        case RACE_HUMAN:
			case RACE_GNOME:
			case RACE_DRYAD:
			case RACE_MYCONID:
			case RACE_SALAMANDER:
	            MainMenu::beginFade(MainMenu::FadeDestination::EndingHuman);
	            break;
	        case RACE_AUTOMATON:
	            MainMenu::beginFade(MainMenu::FadeDestination::EndingAutomaton);
	            break;
	        case RACE_GOATMAN:
	        case RACE_GOBLIN:
	        case RACE_INSECTOID:
	            MainMenu::beginFade(MainMenu::FadeDestination::EndingBeast);
	            break;
	        case RACE_SKELETON:
	        case RACE_VAMPIRE:
	        case RACE_SUCCUBUS:
	        case RACE_INCUBUS:
			case RACE_GREMLIN:
	            MainMenu::beginFade(MainMenu::FadeDestination::EndingEvil);
	            break;
	        }
	    }
	    else if (net_packet->data[5] == 1) { // classic herx ending
			victory = 1;
	        switch ( race ) {
	        default:
	        case RACE_HUMAN:
			case RACE_GNOME:
			case RACE_DRYAD:
			case RACE_MYCONID:
			case RACE_SALAMANDER:
	            MainMenu::beginFade(MainMenu::FadeDestination::ClassicEndingHuman);
	            break;
	        case RACE_AUTOMATON:
	            MainMenu::beginFade(MainMenu::FadeDestination::ClassicEndingAutomaton);
	            break;
	        case RACE_GOATMAN:
	        case RACE_GOBLIN:
	        case RACE_INSECTOID:
	            MainMenu::beginFade(MainMenu::FadeDestination::ClassicEndingBeast);
	            break;
	        case RACE_SKELETON:
	        case RACE_VAMPIRE:
	        case RACE_SUCCUBUS:
	        case RACE_INCUBUS:
			case RACE_GREMLIN:
	            MainMenu::beginFade(MainMenu::FadeDestination::ClassicEndingEvil);
	            break;
	        }
	    }
	    else if (net_packet->data[5] == 2) { // classic baphomet ending
			victory = 2;
	        switch ( race ) {
	        default:
	        case RACE_HUMAN:
			case RACE_GNOME:
			case RACE_DRYAD:
			case RACE_MYCONID:
			case RACE_SALAMANDER:
	            MainMenu::beginFade(MainMenu::FadeDestination::ClassicBaphometEndingHuman);
	            break;
	        case RACE_AUTOMATON:
	            MainMenu::beginFade(MainMenu::FadeDestination::ClassicBaphometEndingAutomaton);
	            break;
	        case RACE_GOATMAN:
	        case RACE_GOBLIN:
	        case RACE_INSECTOID:
	            MainMenu::beginFade(MainMenu::FadeDestination::ClassicBaphometEndingBeast);
	            break;
	        case RACE_SKELETON:
	        case RACE_VAMPIRE:
	        case RACE_SUCCUBUS:
	        case RACE_INCUBUS:
			case RACE_GREMLIN:
	            MainMenu::beginFade(MainMenu::FadeDestination::ClassicBaphometEndingEvil);
	            break;
	        }
	    }

		if ( victory > 0 )
		{
			int k = 0;
			for ( int c = 0; c < MAXPLAYERS; c++ )
			{
				if ( players[c] && players[c]->entity )
				{
					k++;
				}
			}
			if ( k >= 2 )
			{
				steamAchievement("BARONY_ACH_IN_GREATER_NUMBERS");
			}
		}

	    // force game to pause
        movie = true;
		pauseGame(2, false);
	}},

	// mid game cutscene
	{'MIDG', [](){
		int race = RACE_HUMAN;
		if ( stats[clientnum]->playerRace != RACE_HUMAN && stats[clientnum]->stat_appearance == 0 )
		{
			race = stats[clientnum]->playerRace;
		}
	    if (net_packet->data[4] == 0) { // herx midpoint
	        switch ( race ) {
	        default:
	        case RACE_HUMAN:
			case RACE_GNOME:
	            MainMenu::beginFade(MainMenu::FadeDestination::HerxMidpointHuman);
	            break;
	        case RACE_AUTOMATON:
	            MainMenu::beginFade(MainMenu::FadeDestination::HerxMidpointAutomaton);
	            break;
	        case RACE_GOATMAN:
	        case RACE_GOBLIN:
	        case RACE_INSECTOID:
			case RACE_DRYAD:
			case RACE_MYCONID:
			case RACE_SALAMANDER:
	            MainMenu::beginFade(MainMenu::FadeDestination::HerxMidpointBeast);
	            break;
	        case RACE_SKELETON:
	        case RACE_VAMPIRE:
	        case RACE_SUCCUBUS:
	        case RACE_INCUBUS:
			case RACE_GREMLIN:
	            MainMenu::beginFade(MainMenu::FadeDestination::HerxMidpointEvil);
	            break;
	        }
	    }
	    else if (net_packet->data[4] == 1) { // baphomet midpoint
	        switch ( race ) {
	        default:
	        case RACE_HUMAN:
			case RACE_GNOME:
	            MainMenu::beginFade(MainMenu::FadeDestination::BaphometMidpointHuman);
	            break;
	        case RACE_AUTOMATON:
	            MainMenu::beginFade(MainMenu::FadeDestination::BaphometMidpointAutomaton);
	            break;
	        case RACE_GOATMAN:
	        case RACE_GOBLIN:
	        case RACE_INSECTOID:
			case RACE_DRYAD:
			case RACE_MYCONID:
			case RACE_SALAMANDER:
	            MainMenu::beginFade(MainMenu::FadeDestination::BaphometMidpointBeast);
	            break;
	        case RACE_SKELETON:
	        case RACE_VAMPIRE:
	        case RACE_SUCCUBUS:
	        case RACE_INCUBUS:
			case RACE_GREMLIN:
	            MainMenu::beginFade(MainMenu::FadeDestination::BaphometMidpointEvil);
	            break;
	        }
	    }

	    // force game to pause
        movie = true;
		pauseGame(2, false);
	}},

	{'PMAP', [](){
		MinimapPing newPing(ticks, net_packet->data[4], 
			net_packet->data[5], 
			net_packet->data[6],
			net_packet->data[8] ? true : false,
			(MinimapPing::PingType)net_packet->data[7]);
		for ( int c = 0; c < MAXPLAYERS; ++c )
		{
			if ( players[c]->isLocalPlayer() )
			{
				minimapPingAdd(newPing.player, c, newPing);
			}
		}
	}},

	// the server sent a game player preferences update
	{'GPPR', []() {
		GameplayPreferences_t::receivePacket();
	}},

	// the server requested a game player preferences update
	{'GPPU', []() {
		gameplayPreferences[clientnum].sendToServer();
	}},

	// the server sent a game config update
	{ 'GOPT', []() {
		GameplayPreferences_t::receiveGameConfig();
	} },

	{'DASH', [](){
		if ( players[clientnum] && players[clientnum]->entity && stats[clientnum] )
		{
			real_t vel = sqrt(pow(players[clientnum]->entity->vel_y, 2) + pow(players[clientnum]->entity->vel_x, 2));
			players[clientnum]->entity->monsterKnockbackVelocity = std::min(2.25, std::max(1.0, vel));
			players[clientnum]->entity->monsterKnockbackTangentDir = atan2(players[clientnum]->entity->vel_y, players[clientnum]->entity->vel_x);
			if ( vel < 0.01 )
			{
				players[clientnum]->entity->monsterKnockbackTangentDir = players[clientnum]->entity->yaw + PI;
			}
		}
	}},

	{ 'OVRC', []() {
		if ( players[clientnum] && players[clientnum]->entity && stats[clientnum] )
		{
			cast_animation[clientnum].overcharge_init = net_packet->data[4];
		}
	} },

	{ 'KINE', []() {
	if ( players[clientnum] && players[clientnum]->entity && stats[clientnum] )
	{
		real_t vel = sqrt(pow(players[clientnum]->entity->vel_y, 2) + pow(players[clientnum]->entity->vel_x, 2));
		players[clientnum]->entity->monsterKnockbackVelocity = std::min(2.25, std::max(1.0, vel));

		real_t dir = (SDLNet_Read32(&net_packet->data[4]) / 256.0);
		players[clientnum]->entity->monsterKnockbackTangentDir = dir;
	}
} },

	// get item
	{'ITEQ', [](){
		if ( net_packet->len < 29 )
		{
			printlog(
				"[NET]: ignoring malformed ITEQ packet with length %d.\n",
				net_packet->len
			);
			return;
		}

		int resolvedType =
			static_cast<int>(
				SDLNet_Read32(&net_packet->data[4])
			);

#ifdef SAM_FRAMEWORK_ENABLED
		if ( SAMItemRegistryFoundation::
			isSAMRuntimeItemId(resolvedType) )
		{
			if ( net_packet->len <= 29 )
			{
				printlog(
					"[S.A.M] Refusing legacy numeric-only ITEQ custom item runtime %d.\n",
					resolvedType
				);
				messagePlayer(
					clientnum,
					MESSAGE_MISC,
					"A custom multiplayer item was rejected because it had no stable ID."
				);
				return;
			}

			const int payloadLength =
				net_packet->len - 29;
			int stableLength = 0;
			while ( stableLength < payloadLength
				&& net_packet->data[29 + stableLength] != '\0' )
			{
				++stableLength;
			}

			if ( stableLength <= 0
				|| stableLength >= payloadLength )
			{
				printlog(
					"[S.A.M] Refusing malformed ITEQ stable-id payload.\n"
				);
				return;
			}

			const std::string stableId(
				reinterpret_cast<const char*>(
					&net_packet->data[29]
				),
				stableLength
			);

			resolvedType =
				SAMItemRegistryFoundation::
					runtimeIdForStableId(stableId);

			if ( resolvedType < 0
				|| !SAMItemRegistryFoundation::
					isRegisteredRuntimeItemId(resolvedType) )
			{
				printlog(
					"[S.A.M] ITEQ custom item unavailable locally: [%s]. Item rejected.\n",
					stableId.c_str()
				);
				messagePlayer(
					clientnum,
					MESSAGE_MISC,
					"Required custom item is unavailable: %s",
					stableId.c_str()
				);
				return;
			}

			printlog(
				"[S.A.M] Resolved ITEQ custom item [%s] to local runtime %d.\n",
				stableId.c_str(),
				resolvedType
			);
		}
#endif

		auto item = newItem(
		    static_cast<ItemType>(resolvedType),
		    static_cast<Status>(SDLNet_Read32(&net_packet->data[8])),
		    SDLNet_Read32(&net_packet->data[12]),
		    SDLNet_Read32(&net_packet->data[16]),
		    SDLNet_Read32(&net_packet->data[20]),
		    net_packet->data[28],
		    NULL);
		if ( !item )
		{
			printlog(
				"[NET]: ITEQ failed to construct item type %d.\n",
				resolvedType
			);
			return;
		}

		item->ownerUid = SDLNet_Read32(&net_packet->data[24]);
		Item* pickedUp = itemPickup(clientnum, item);
		free(item);
		if ( players[clientnum] && players[clientnum]->entity && pickedUp )
		{
			bool oldIntro = intro;
			intro = true;
			useItem(pickedUp, clientnum);
			intro = oldIntro;
		}
	}},

	// update attributes from script
	{'SCRU', [](){
		if ( net_packet->data[25] )
		{
			bool clearStats = false;
			if ( net_packet->data[26] )
			{
				clearStats = true;
			}
			textSourceScript.playerClearInventory(clearStats);
		}
		stats[clientnum]->STR = ((Sint8)net_packet->data[5] <= -8) ? (Uint8)net_packet->data[5] : (Sint8)net_packet->data[5];
		stats[clientnum]->DEX = ((Sint8)net_packet->data[6] <= -8) ? (Uint8)net_packet->data[6] : (Sint8)net_packet->data[6];
		stats[clientnum]->CON = ((Sint8)net_packet->data[7] <= -8) ? (Uint8)net_packet->data[7] : (Sint8)net_packet->data[7];
		stats[clientnum]->INT = ((Sint8)net_packet->data[8] <= -8) ? (Uint8)net_packet->data[8] : (Sint8)net_packet->data[8];
		stats[clientnum]->PER = ((Sint8)net_packet->data[9] <= -8) ? (Uint8)net_packet->data[9] : (Sint8)net_packet->data[9];
		stats[clientnum]->CHR = ((Sint8)net_packet->data[10] <= -8) ? (Uint8)net_packet->data[10] : (Sint8)net_packet->data[10];
		stats[clientnum]->EXP = (Uint8)net_packet->data[11];
		stats[clientnum]->LVL = (Uint8)net_packet->data[12];
		stats[clientnum]->HP = (Sint16)SDLNet_Read16(&net_packet->data[13]);
		stats[clientnum]->MAXHP = (Sint16)SDLNet_Read16(&net_packet->data[15]);
		stats[clientnum]->MP = (Sint16)SDLNet_Read16(&net_packet->data[17]);
		stats[clientnum]->MAXMP = (Sint16)SDLNet_Read16(&net_packet->data[19]);
		stats[clientnum]->GOLD = (Sint32)SDLNet_Read32(&net_packet->data[21]);
		for ( int i = 0; i < NUMPROFICIENCIES; ++i )
		{
			stats[clientnum]->setProficiency(i, (Sint8)net_packet->data[27 + i]);
		}
	} },

	// update class from script
	{ 'SCRC', []() {
		if ( !net_packet || net_packet->len < 6 )
		{
			printlog(
				"[NET]: ignoring truncated SCRC packet"
			);
			return;
		}

		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		const int classnum = net_packet->data[5];
		if ( player >= 0 )
		{
			client_classes[player] = classnum;
			bool oldIntro = intro;
			intro = true;
			initClass(player);
			intro = oldIntro;
		}
	}},

	// open fullscreen sign
	{'SIGN', []() {
		Uint32 uid = SDLNet_Read32(&net_packet->data[4]);
		if ( Entity* sign = uidToEntity(uid) )
		{
			char* key = (char*)(&net_packet->data[8]);
			players[clientnum]->signGUI.openSign(key, uid);
		}
	}},

	// game restart
	{'RSTR', [](){
		svFlags = SDLNet_Read32(&net_packet->data[4]);
		uniqueGameKey = SDLNet_Read32(&net_packet->data[8]);
		local_rng.seedBytes(&uniqueGameKey, sizeof(uniqueGameKey));
		net_rng.seedBytes(&uniqueGameKey, sizeof(uniqueGameKey));
		uniqueLobbyKey = SDLNet_Read32(&net_packet->data[13]);
	    if (net_packet->data[12] == 0) {
		    loadingsavegame = 0;
			loadinglobbykey = 0;
	    }
		if ( gameModeManager.allowsSaves() )
		{
			deleteSaveGame(multiplayer);
		}
		printlog("Received order to restart game");
		MainMenu::beginFade(MainMenu::FadeDestination::GameStart);
		pauseGame(2, 0);
	}},

	// delete multiplayer save
	{'DSAV', [](){
		if ( multiplayer == CLIENT )
		{
			if ( gameModeManager.allowsSaves() )
			{
				deleteSaveGame(multiplayer);
			}
		}
	}},

	// post online hiscore
	{ 'DEND', []() {
		if ( multiplayer == CLIENT )
		{
#ifdef USE_PLAYFAB
			playfabUser.postScore(clientnum);
#endif
		}
	}},

	// custom dialogue choices from host
	{'CDCH', []() {
		if ( multiplayer != CLIENT
			|| net_packet->len < 12 )
		{
			return;
		}

		const Uint32 uid =
			SDLNet_Read32(
				&net_packet->data[4]
			);

		const auto type =
			static_cast<
				Player::WorldUI_t
					::WorldTooltipDialogue_t
					::DialogueType_t
			>(
				net_packet->data[8]
			);

		const Uint8 choiceCount =
			net_packet->data[9];

		const Uint16 messageLength =
			SDLNet_Read16(
				&net_packet->data[10]
			);

		int offset = 12;

		if ( offset + messageLength
			> net_packet->len )
		{
			printlog(
				"[Custom Dialogue] Rejected malformed CDCH base message."
			);

			return;
		}

		std::string message(
			reinterpret_cast<char*>(
				&net_packet->data[offset]
			),
			messageLength
		);

		offset += messageLength;

		std::vector<std::string> choices;
		choices.reserve(choiceCount);

		for ( Uint8 index = 0;
			index < choiceCount;
			++index )
		{
			if ( offset + 2
				> net_packet->len )
			{
				printlog(
					"[Custom Dialogue] Rejected malformed CDCH choice header."
				);

				return;
			}

			const Uint16 choiceLength =
				SDLNet_Read16(
					&net_packet->data[offset]
				);

			offset += 2;

			if ( offset + choiceLength
				> net_packet->len )
			{
				printlog(
					"[Custom Dialogue] Rejected malformed CDCH choice text."
				);

				return;
			}

			choices.emplace_back(
				reinterpret_cast<char*>(
					&net_packet->data[offset]
				),
				choiceLength
			);

			offset += choiceLength;
		}

		players[clientnum]
			->worldUI
			.worldTooltipDialogue
			.createDialogueChoiceTooltip(
				uid,
				type,
				message,
				choices
			);

		printlog(
			"[Custom Dialogue] Client received %u choice(s) for NPC UID %u.",
			static_cast<unsigned int>(
				choiceCount
			),
			uid
		);
	}},

	// text bubbles
	{'BUBL', []() {
		Uint32 uid = SDLNet_Read32(&net_packet->data[4]);
		Player::WorldUI_t::WorldTooltipDialogue_t::DialogueType_t type = 
			(Player::WorldUI_t::WorldTooltipDialogue_t::DialogueType_t)net_packet->data[8];
		const char* msg = (const char*)(&net_packet->data[9]);
		players[clientnum]->worldUI.worldTooltipDialogue.createDialogueTooltip(uid, type, msg);
		return;
	}},

	// shopkeeper player hostility
	{ 'SHPH', []() {
		ShopkeeperPlayerHostility_t::WantedLevel wantedLevel = (ShopkeeperPlayerHostility_t::WantedLevel)net_packet->data[4];
		Uint16 numKills = SDLNet_Read16(&net_packet->data[5]);
		Uint16 numAggressions = SDLNet_Read16(&net_packet->data[7]);
		Uint16 numAccessories = SDLNet_Read16(&net_packet->data[9]);
		Uint32 type = SDLNet_Read32(&net_packet->data[11]);
		if ( auto hostility = ShopkeeperPlayerHostility.getPlayerHostility(clientnum, type) )
		{
			hostility->wantedLevel = wantedLevel;
			hostility->playerRace = (Monster)(type & 0xFF);
			hostility->sex = ((type >> 8) & 0x1) ? sex_t::MALE : sex_t::FEMALE;
			hostility->equipment = ((type >> 9) & 0x7F);
			hostility->numKills = (int)numKills;
			hostility->numAggressions = (int)numAggressions;
			hostility->numAccessories = (int)numAccessories;
			hostility->player = clientnum;
		}
		return;
	} },

	{ 'BNTY', []() {
		int player = (int)net_packet->data[4];
		if ( player >= 0 && player < MAXPLAYERS )
		{
			size_t numBounties = (size_t)net_packet->data[5];
			int index = 6;
			achievementObserver.playerAchievements[player].bountyTargets.clear();
			while ( numBounties > 0 )
			{
				Uint32 uid = SDLNet_Read32(&net_packet->data[index]);
				achievementObserver.playerAchievements[player].bountyTargets.insert(uid);
				--numBounties;
				index += 4;
			}
		}
	}},

	{ 'BNTH', []() {
		int player = (int)net_packet->data[4];
		if ( player >= 0 && player < MAXPLAYERS )
		{
			achievementObserver.playerAchievements[player].wearingBountyHat = net_packet->data[5] > 0 ? true : false;
		}
	} },

	// compendium reveal an entry
	{ 'CMPU', []() {
		int eventID = SDLNet_Read32(&net_packet->data[4]);
		if ( eventID >= Compendium_t::Events_t::kEventMonsterOffset && eventID < Compendium_t::Events_t::kEventMonsterOffset + 1000 )
		{
			auto find = Compendium_t::Events_t::monsterIDToString.find(eventID);
			if ( find != Compendium_t::Events_t::monsterIDToString.end() )
			{
				auto& unlockStatus = Compendium_t::CompendiumMonsters_t::unlocks[find->second];
				if ( unlockStatus == Compendium_t::CompendiumUnlockStatus::LOCKED_UNKNOWN )
				{
					unlockStatus = Compendium_t::CompendiumUnlockStatus::LOCKED_REVEALED_UNVISITED;
				}
			}
		}
	} },

	// compendium data update
	{ 'CMPD', []() {
		Uint8 clientSequence = net_packet->data[4];
		int sequence = net_packet->data[5];
		int numchunks = net_packet->data[6];
		if ( numchunks == 0 )
		{
			return;
		}
		char buf[512];
		stringCopy(buf, (char*)(&net_packet->data[7]), sizeof(buf), std::max(0, (int)net_packet->len - 7));
		Compendium_t::Events_t::clientReceiveData[clientSequence][sequence] = buf;
		if ( (int)Compendium_t::Events_t::clientReceiveData[clientSequence].size() == numchunks )
		{
			std::string str = "";
			for ( int i = 1; i <= numchunks; ++i )
			{
				str += Compendium_t::Events_t::clientReceiveData[clientSequence][i];
			}

			rapidjson::Document d;
			d.Parse(str.c_str());
			if ( !d.HasParseError() )
			{
				if ( d.HasMember("seq") && d.HasMember("item") )
				{
					if ( d["seq"].GetInt() == clientSequence )
					{
						for ( auto itr = d["item"].MemberBegin(); itr != d["item"].MemberEnd(); ++itr )
						{
							int id = std::stoi(itr->name.GetString());
							if ( id >= 0 && id < Compendium_t::EventTags::CPDM_EVENT_TAGS_MAX )
							{
								for ( auto itr2 = itr->value.MemberBegin(); itr2 != itr->value.MemberEnd(); ++itr2 )
								{
									int itemType = std::stoi(itr2->name.GetString());
									Sint32 value = itr2->value.GetInt();
									if ( itemType >= Compendium_t::Events_t::kEventMonsterOffset && itemType < Compendium_t::Events_t::kEventMonsterOffset + 1000 )
									{
										Compendium_t::Events_t::eventUpdateMonster(0, (Compendium_t::EventTags)id, nullptr, value, false, itemType);
										continue;
									}
									if ( itemType >= Compendium_t::Events_t::kEventWorldOffset && itemType < Compendium_t::Events_t::kEventWorldOffset + 1000 )
									{
										Compendium_t::Events_t::eventUpdateWorld(0, (Compendium_t::EventTags)id, nullptr, value, false, 
											itemType - Compendium_t::Events_t::kEventWorldOffset);
										continue;
									}
									if ( itemType >= Compendium_t::Events_t::kEventCodexOffset && itemType <= Compendium_t::Events_t::kEventCodexOffsetMax )
									{
										Compendium_t::Events_t::eventUpdateCodex(0, (Compendium_t::EventTags)id, nullptr, value, false, 
											itemType);
										continue;
									}
									if ( itemType < 0 || (itemType >= NUMITEMS && itemType < Compendium_t::Events_t::kEventSpellOffset) )
									{
										continue;
									}
									if ( itemType >= Compendium_t::Events_t::kEventSpellOffset )
									{
										Compendium_t::Events_t::eventUpdate(0, (Compendium_t::EventTags)id, SPELL_ITEM, value, false, 
											itemType - Compendium_t::Events_t::kEventSpellOffset);
									}
									else
									{
										Compendium_t::Events_t::eventUpdate(0, (Compendium_t::EventTags)id, (ItemType)itemType, value);
									}
								}
							}
						}
					}
				}
			}
			Compendium_t::Events_t::clientReceiveData.erase(clientSequence);

			Compendium_t::Events_t::writeItemsSaveData();
			Compendium_t::writeUnlocksSaveData();

			// reply got packet
			strcpy((char*)net_packet->data, "CMPD");
			net_packet->data[4] = clientnum;
			net_packet->data[5] = clientSequence;
			net_packet->address.host = net_server.host;
			net_packet->address.port = net_server.port;
			net_packet->len = 7;
			sendPacketSafe(net_sock, -1, net_packet, 0);
		}
	}},

	// character change
	{ 'ASSC', []() {
	int player = net_packet->data[8];
	if ( player >= 0 && player < MAXPLAYERS )
	{
		auto& gui = GenericGUI[player].assistShrineGUI;
		gui.savedClass = (Sint8)net_packet->data[4];
		gui.savedRace = (Sint8)net_packet->data[5];
		gui.savedSex = (Sint8)net_packet->data[6];
		gui.savedAppearance = (Sint8)net_packet->data[7];
		gui.receivedCharacterChangeOK = true;

		std::string racename = "";
		if ( gui.savedRace != RACE_HUMAN )
		{
			if ( gui.savedAppearance != 0 )
			{
				racename = Language::get(4068); // guised
				racename += ' ';
			}
		}
		racename += getMonsterLocalizedName(getMonsterFromPlayerRace(gui.savedRace)).c_str();
		camelCaseString(racename);
		std::string classname = playerClassLangEntry(gui.savedClass >= 0 ? gui.savedClass : client_classes[player], player);
		camelCaseString(classname);
		if ( player == clientnum )
		{
			if ( gui.bOpen )
			{
				gui.addNotification(Language::get(6334), Language::get(6335), "", GenericGUIMenu::AssistShrineGUI_t::AssistNotification_t::NOTIF_CHARACTER_CHANGE_OK);
			}
			messagePlayer(clientnum, MESSAGE_WORLD, Language::get(6355), racename.c_str(), classname.c_str());
		}
		else
		{
			messagePlayer(clientnum, MESSAGE_WORLD, Language::get(6336), stats[player]->name, racename.c_str(), classname.c_str());
		}
	}
	}},

	{ 'ASSU', []() {
		// server sent player current assist values
		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( player < 0 )
		{
			return;
		}
		Sint32 assistance = SDLNet_Read32(&net_packet->data[5]);
		if ( player == clientnum )
		{
			stats[player]->MISC_FLAGS[STAT_FLAG_ASSISTANCE_PLAYER_PTS]
				= std::max(assistance, stats[player]->MISC_FLAGS[STAT_FLAG_ASSISTANCE_PLAYER_PTS]);
		}
		else
		{
			stats[player]->MISC_FLAGS[STAT_FLAG_ASSISTANCE_PLAYER_PTS] = assistance;
		}
	} },

	{ 'ASSO', []() {
		// server order to open assist gui
		Uint32 uid = SDLNet_Read32(&net_packet->data[4]);
		if ( auto entity = uidToEntity(uid) )
		{
			if ( entity->behavior == &::actAssistShrine )
			{
				GenericGUI[clientnum].openGUI(GUI_TYPE_ASSIST, entity);
			}
		}
	} },

	// server order to close assist shrine
	{ 'ASCL', []() {
		if ( !net_packet || net_packet->len < 9 )
		{
			printlog(
				"[NET]: ignoring truncated ASCL packet"
			);
			return;
		}

		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( player == clientnum )
		{
			const Uint32 uid =
				SDLNet_Read32(&net_packet->data[5]);
			if ( Entity* shrine = uidToEntity(uid) )
			{
				GenericGUI[clientnum].assistShrineGUI.closeAssistShrine();
			}
		}
	} },

	{ 'CAUO', []() {
		// server order to open cauldron gui
		Uint32 uid = SDLNet_Read32(&net_packet->data[4]);
		if ( auto entity = uidToEntity(uid) )
		{
			if ( entity->behavior == &::actCauldron )
			{
				GenericGUI[clientnum].openGUI(GUI_TYPE_ALCHEMY, entity);
			}
		}
	} },

	// server order to close cauldron
	{ 'CAUC', []() {
		if ( !net_packet || net_packet->len < 9 )
		{
			printlog(
				"[NET]: ignoring truncated CAUC packet"
			);
			return;
		}

		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( player == clientnum )
		{
			const Uint32 uid =
				SDLNet_Read32(&net_packet->data[5]);
			if ( Entity* cauldron = uidToEntity(uid) )
			{
				GenericGUI[clientnum].alchemyGUI.closeAlchemyMenu();
			}
		}
	} },

	{ 'WRKO', []() {
		// server order to open workbench gui
		Uint32 uid = SDLNet_Read32(&net_packet->data[4]);
		if ( auto entity = uidToEntity(uid) )
		{
			if ( entity->behavior == &::actWorkbench )
			{
				GenericGUI[clientnum].openGUI(GUI_TYPE_TINKERING, entity);
			}
		}
	} },

	// server order to close workbench
	{ 'WRKC', []() {
		if ( !net_packet || net_packet->len < 9 )
		{
			printlog(
				"[NET]: ignoring truncated WRKC packet"
			);
			return;
		}

		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( player == clientnum )
		{
			const Uint32 uid =
				SDLNet_Read32(&net_packet->data[5]);
			if ( Entity* cauldron = uidToEntity(uid) )
			{
				GenericGUI[clientnum].tinkerGUI.closeTinkerMenu();
			}
		}
	} },

	{ 'MBXO', []() {
		// server order to open mailbox gui
		Uint32 uid = SDLNet_Read32(&net_packet->data[4]);
		if ( auto entity = uidToEntity(uid) )
		{
			if ( entity->behavior == &::actMailbox )
			{
				GenericGUI[clientnum].openGUI(GUI_TYPE_MAILBOX, entity);
			}
		}
	} },

	// server order to close mailbox
	{ 'MBXC', []() {
		if ( !net_packet || net_packet->len < 9 )
		{
			printlog(
				"[NET]: ignoring truncated MBXC packet"
			);
			return;
		}

		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( player == clientnum )
		{
			const Uint32 uid =
				SDLNet_Read32(&net_packet->data[5]);
			if ( Entity* cauldron = uidToEntity(uid) )
			{
				GenericGUI[clientnum].mailboxGUI.closeMailMenu();
			}
		}
	} },

	// server order to consume key for lock
	{ 'LKEY', []() {
		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( player < 0 )
		{
			return;
		}
		bool success = false;

		// reply got packet
		strcpy((char*)net_packet->data, "OKEY");
		net_packet->data[4] = clientnum;

		if ( player == clientnum )
		{
			Uint32 uid = SDLNet_Read32(&net_packet->data[5]);
			Entity* entity = uidToEntity(uid);
			if ( entity && entity->behavior == &actWallLock )
			{
				Item* key = players[clientnum]->inventoryUI.hasKeyForWallLock(*entity);
				if ( key )
				{
					SDLNet_Write16((Uint16)key->type, &net_packet->data[10]);
					consumeItem(key, clientnum);
					success = true;
				}
			}
		}

		net_packet->data[9] = success ? 1 : 0;
		net_packet->len = 12;
		net_packet->address.host = net_server.host;
		net_packet->address.port = net_server.port;
		sendPacketSafe(net_sock, -1, net_packet, 0);
	} },

	// server ensemble music update
	{ 'ENSM', []() {
		for ( int i = 4; i < net_packet->len; i += 4 )
		{
			Uint32 data = SDLNet_Read32(&net_packet->data[i]);
			int player = ((data & 0x7F) - 1);
			if ( player >= 0 && player < MAXPLAYERS )
			{
				players[player]->mechanics.ensembleDataUpdate = data;
			}
		}
	} },

	{ 'VOIP',[]() {
#ifdef USE_FMOD
		VoiceChat.receivePacket(net_packet);
#endif
	} },

	{ 'MAPT',[]() {
		int x = SDLNet_Read16(&net_packet->data[4]);
		int y = SDLNet_Read16(&net_packet->data[6]);
		Uint32 flagSet = SDLNet_Read32(&net_packet->data[8]);
		Uint32 flagRemove = SDLNet_Read32(&net_packet->data[12]);
		int layer = net_packet->data[16];
		if ( x >= 0 && x < map.width && y >= 0 && y < map.height && layer >= 0 && layer < MAPLAYERS )
		{
			if ( flagSet )
			{
				if ( !map.tileHasAttribute(x, y, layer, flagSet) )
				{
					map.tileAttributes[layer + (y * MAPLAYERS) + (x * MAPLAYERS * map.height)] |= flagSet;
				}
			}
			if ( flagRemove )
			{
				if ( map.tileHasAttribute(x, y, layer, flagRemove) )
				{
					map.tileAttributes[layer + (y * MAPLAYERS) + (x * MAPLAYERS * map.height)] &= ~flagRemove;
				}
			}
		}
	}},

	// command spell
	{ 'COMD',[]() {
		Uint32 uid = SDLNet_Read32(&net_packet->data[4]);
		if ( Entity* target = uidToEntity(uid) )
		{
			if ( !target->clientsHaveItsStats )
			{
				target->giveClientStats();
			}
			FollowerMenu[clientnum].followerToCommand = target;
			FollowerMenu[clientnum].initfollowerMenuGUICursor(true); // set gui_mode to follower menu
		}
	} },

	{ 'FOCI',[]() {
		Uint32 uid = SDLNet_Read32(&net_packet->data[4]);
		real_t x = SDLNet_Read16(&net_packet->data[8]) / 32.0;
		real_t y = SDLNet_Read16(&net_packet->data[10]) / 32.0;
		real_t z = SDLNet_Read16(&net_packet->data[12]) / 32.0;
		real_t dir = SDLNet_Read16(&net_packet->data[14]) / 256.0;
		int sprite = SDLNet_Read16(&net_packet->data[16]);
		Uint32 seed = SDLNet_Read32(&net_packet->data[18]);
		real_t velocityBonus = SDLNet_Read16(&net_packet->data[22]) / 256.0;
		if ( Entity* gib = spawnFociGib(x, y, z, dir, velocityBonus, uid, sprite, seed) )
		{
			gib->setUID(uid);
		}
	} },

	{ 'SANM',[]() { // player spellcast animation
		if ( !net_packet || net_packet->len < 8 )
		{
			printlog(
				"[NET]: ignoring truncated SANM packet"
			);
			return;
		}

		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( player < 0 )
		{
			return;
		}

		const int pose = net_packet->data[5];
		const int charge =
			SDLNet_Read16(&net_packet->data[6]);
		spellcastAnimationUpdateReceive(
			player,
			pose,
			charge
		);
	} },

	// update breakable counter
	{ 'GBRK', []() {
		players[clientnum]->mechanics.gremlinBreakableCounter = net_packet->data[4];
	} },
};

void clientHandlePacket()
{
    if ( !net_packet || !net_packet->data || net_packet->len < 4 )
    {
        printlog("[NET]: ignored truncated client packet");
        return;
    }
	if (handleSafePacket())
	{
		return;
	}
	if (clientDeferLateJoinMapPacket(
			net_packet->data, static_cast<std::size_t>(net_packet->len)))
	{
		return;
	}

	Uint32 packetId = SDLNet_Read32(&net_packet->data[0]);

#ifdef PACKETINFO
	char packetinfo[NET_PACKET_SIZE];
	strncpy( packetinfo, (char*)net_packet->data, net_packet->len );
	packetinfo[net_packet->len] = 0;
	printlog("info: client packet: %s\n", packetinfo);
#endif
	if ( logCheckMainLoopTimers )
	{
		char packetinfo[NET_PACKET_SIZE];
		memcpy(packetinfo, (char*)net_packet->data, net_packet->len);
		packetinfo[net_packet->len] = '\0';

		char packetHeader[5];
		memcpy(packetHeader, packetinfo, 4);
		packetHeader[4] = '\0';

		std::string tmp = packetHeader;
		unsigned long hash = djb2Hash(packetHeader);
		auto find = DebugStats.networkPackets.find(hash);
		if ( find != DebugStats.networkPackets.end() )
		{
			++DebugStats.networkPackets[hash].second;
		}
		else
		{
			DebugStats.networkPackets.insert(std::make_pair(hash, std::make_pair(tmp, 0)));
			messagePlayer(clientnum, MESSAGE_DEBUG, "%s", tmp.c_str());
		}
		if ( packetId == 'ENTU' )
		{
			int sprite = 0;
			Uint32 uidpacket = static_cast<Uint32>(SDLNet_Read32(&net_packet->data[4]));
			if ( uidToEntity(uidpacket) )
			{
				sprite = uidToEntity(uidpacket)->sprite;
				auto find = DebugStats.entityUpdatePackets.find(sprite);
				if ( find != DebugStats.entityUpdatePackets.end() )
				{
					++DebugStats.entityUpdatePackets[sprite];
				}
				else
				{
					DebugStats.entityUpdatePackets.insert(std::make_pair(sprite, 1));
				}
			}
		}
	}

    auto find = clientPacketHandlers.find(packetId);
    if (find == clientPacketHandlers.end()) {
        // error
        printlog("Got a mystery packet: %c%c%c%c",
            (char)net_packet->data[0],
            (char)net_packet->data[1],
            (char)net_packet->data[2],
            (char)net_packet->data[3]);
    } else {
        (*(find->second))(); // handle packet
    }
}

static void tryReplayClientLateJoinPackets()
{
	if (!g_clientLateJoinPacketDeferral || !g_clientLateJoinCatchupComplete
		|| !g_clientLateJoinMapIsLoaded || g_clientLateJoinReplayingPackets
		|| !net_packet || !net_packet->data)
	{
		return;
	}
	std::vector<std::vector<std::uint8_t>> livePackets;
	const std::vector<std::uint8_t> serializedLive =
		g_clientLateJoinLivePackets.serialize();
	if (serializedLive.empty()
		|| !LateJoinPacketCatchupBuffer::deserialize(
			serializedLive, livePackets))
	{
		printlog("[Late Join] Client discarded invalid deferred live packets.");
		clientResetLateJoinPacketDeferral();
		return;
	}
	const int savedLength = net_packet->len;
	const IPaddress savedAddress = net_packet->address;
	std::vector<Uint8> savedPacket(
		net_packet->data, net_packet->data + std::max(0, savedLength));
	g_clientLateJoinReplayingPackets = true;
	const auto replay = [](const std::vector<std::vector<std::uint8_t>>& packets)
	{
		for (const auto& packet : packets)
		{
			if (packet.size() > NET_PACKET_SIZE)
			{
				continue;
			}
			memcpy(net_packet->data, packet.data(), packet.size());
			net_packet->len = static_cast<int>(packet.size());
			clientHandlePacket();
		}
	};
	replay(g_clientLateJoinCatchupPackets);
	replay(livePackets);
	std::size_t resetStaticFixtures = 0;
	std::size_t rebuiltEditorLightSources = 0;
	std::size_t failedEditorLightSources = 0;
	if (map.entities)
	{
		for (node_t* node = map.entities->first; node; node = node->next)
		{
			Entity* entity = static_cast<Entity*>(node->element);
			if (entity
				&& (entity->behavior == &actTorch
					|| entity->behavior == &actCrystalShard
					|| (entity->behavior == &actCampfire
						&& entity->skill[3] > 0)))
			{
				entity->removeLightField();
				entity->light = nullptr;
				++resetStaticFixtures;
			}
			if (entity && entity->behavior == &actLightSource)
			{
				entity->removeLightField();
				entity->light = nullptr;
				entity->skill[8] = 0;
				entity->skill[9] = 0;
				// Authored Always On sources are unconditionally enabled on the
				// server. Reassert that invariant locally in case their one-time
				// ENTS update predated this client's connection or its editor UID
				// did not match during catch-up replay.
				if (entity->lightSourceAlwaysOn == 1)
				{
					entity->skill[10] = 1;
				}
				if (entity->skill[10] != 0)
				{
					entity->actLightSource();
					if (entity->light)
					{
						++rebuiltEditorLightSources;
					}
					else
					{
						++failedEditorLightSources;
					}
				}
			}
		}
	}
	if (!savedPacket.empty())
	{
		memcpy(net_packet->data, savedPacket.data(), savedPacket.size());
	}
	net_packet->len = savedLength;
	net_packet->address = savedAddress;
	const std::size_t catchupCount = g_clientLateJoinCatchupPackets.size();
	const std::size_t liveCount = livePackets.size();
	clientResetLateJoinPacketDeferral();
	printlog(
		"[Late Join] Client applied %zu catch-up and %zu deferred live packet(s); reset %zu static fixture light(s), rebuilt %zu editor light source(s), %zu rebuild failure(s).",
		catchupCount, liveCount, resetStaticFixtures,
		rebuiltEditorLightSources, failedEditorLightSources);
}

/*-------------------------------------------------------------------------------

	clientHandleMessages

	Parses messages received from the server

-------------------------------------------------------------------------------*/

void clientHandleMessages(Uint32 framerateBreakInterval)
{
	clientCheckLateJoinTimeout();
#ifdef STEAMWORKS
	if (!directConnect && !net_handler)
	{
		net_handler = new NetHandler();
		if ( !disableMultithreadedSteamNetworking )
		{
			net_handler->initializeMultithreadedPacketHandling();
		}
	}
#elif defined USE_EOS
	if ( !directConnect && !net_handler )
	{
		net_handler = new NetHandler();
	}
#endif

	if (!directConnect)
	{
#if defined(STEAMWORKS) || defined(USE_EOS)
		if ( LobbyHandler.getP2PType() == LobbyHandler_t::LobbyServiceType::LOBBY_STEAM )
		{
#ifdef STEAMWORKS
			//Steam stuff goes here.
			if ( disableMultithreadedSteamNetworking )
			{
				steamPacketThread(static_cast<void*>(net_handler));
			}
#endif
		}
		else if ( LobbyHandler.getP2PType() == LobbyHandler_t::LobbyServiceType::LOBBY_CROSSPLAY )
		{
#if defined USE_EOS
			EOSPacketThread(static_cast<void*>(net_handler));
#endif
		}
		SteamPacketWrapper* packet = nullptr;

		if ( logCheckMainLoopTimers )
		{
			DebugStats.messagesT1 = std::chrono::high_resolution_clock::now();
			DebugStats.handlePacketStartLoop = true;
		}

		while (packet = net_handler->getGamePacket())
		{
			memcpy(net_packet->data, packet->data(), packet->len());
			net_packet->len = packet->len();

			clientHandlePacket(); //Uses net_packet.

			if ( logCheckMainLoopTimers )
			{
				DebugStats.messagesT2WhileLoop = std::chrono::high_resolution_clock::now();
				DebugStats.handlePacketStartLoop = false;
			}
			delete packet;
			if ( !net_handler )
			{
				break;
			}

			if ( !disableFPSLimitOnNetworkMessages && !frameRateLimit(framerateBreakInterval, false) )
			{
				if ( logCheckMainLoopTimers )
				{
					printlog("[NETWORK]: Incoming messages exceeded given cycle time, packets remaining: %d", net_handler->game_packets.size());
				}
				break;
			}
		}
#endif
	}
	else
	{
		//Direct-connect goes here.

		// receive packets from server
		while (SDLNet_UDP_Recv(net_sock, net_packet))
		{
			// filter out broken packets
			if ( !net_packet->data[0] )
			{
				continue;
			}

			clientHandlePacket();
		}
	}
}

/*-------------------------------------------------------------------------------

	serverHandlePacket

	Called by serverHandleMessages. Does the actual handling of a packet.

-------------------------------------------------------------------------------*/

static std::unordered_map<Uint32, void(*)()> serverPacketHandlers = {
	{'JOIN', [](){
		if (!headlessLateJoinRequested || !directConnect)
		{
			memcpy(net_packet->data, "HELO", 4);
			SDLNet_Write32(MAXPLAYERS + 7, &net_packet->data[4]);
			net_packet->len = 8;
			sendPacketSafe(net_sock, -1, net_packet, 0);
			printlog("[Late Join] Rejected runtime JOIN while late join is disabled.");
			return;
		}
		if (net_packet->len < 70)
		{
			printlog("[Late Join] Rejected truncated runtime JOIN.");
			return;
		}
		const Uint8 requestedSlot = net_packet->data[56];
		const Uint32 clientSaveKey = SDLNet_Read32(&net_packet->data[61]);
		bool lockedSlots[MAXPLAYERS] = {};
		for (int player = 0; player < MAXPLAYERS; ++player)
		{
			lockedSlots[player] = MainMenu::isPlayerSlotLocked(player);
		}
		int playerIndex = MAXPLAYERS;
		bool useChunkedHelo = false;
		g_processingRuntimeJoin = true;
		const NetworkingLobbyJoinRequestResult result = lobbyPlayerJoinRequest(
			playerIndex, lockedSlots, useChunkedHelo);
		g_processingRuntimeJoin = false;
		if (result != NET_LOBBY_JOIN_DIRECTIP_SUCCESS
			|| playerIndex <= 0 || playerIndex >= MAXPLAYERS)
		{
			return;
		}

		const bool returningPlayer =
			requestedSlot != 0 && clientSaveKey != 0;
		g_lateJoinReturningPlayer[playerIndex] = returningPlayer;
		if (!g_lateJoinTransactions[playerIndex].holdForClient())
		{
			printlog(
				"[Late Join] Failed to open character selection for player %d.",
				playerIndex);
			abortServerLateJoinPlayer(playerIndex, 4);
			return;
		}
		g_lateJoinLastProgressTick[playerIndex] = ticks;
		printlog(
			"[Late Join] Player %d is connected and choosing a character.",
			playerIndex);
		logServerRosterState("runtime join");
	}},
	{'LJHI', [](){
		if (net_packet->len != 5)
		{
			printlog("[Late Join] Server rejected malformed client-ready handshake.");
			return;
		}
		const int player = decodeGameplayPacketPlayerIndex(net_packet->data[4]);
		if (player <= 0
			|| !currentPacketSenderMatchesPlayer(player)
			|| g_lateJoinTransactions[player].phase()
				!= LateJoinSnapshotTransaction::Phase::AwaitingClient)
		{
			printlog(
				"[Late Join] Server rejected character-selection handshake for player %d.",
				player);
		}
		else
		{
			g_lateJoinClientHandshake[player] = true;
			g_lateJoinLastProgressTick[player] = ticks;
			printlog(
				"[Late Join] Player %d may customize and press Ready.", player);
		}
	}},
	{'PLYR', [](){
		if (net_packet->len != 49)
		{
			printlog("[Late Join] Server rejected malformed character packet.");
			return;
		}
		const int player = decodeGameplayPacketPlayerIndex(net_packet->data[4]);
		if (player <= 0 || !currentPacketSenderMatchesPlayer(player)
			|| g_lateJoinTransactions[player].phase()
				!= LateJoinSnapshotTransaction::Phase::AwaitingClient)
		{
			printlog(
				"[Late Join] Server rejected character data for player %d.",
				player);
			return;
		}
		if (!loadingsavegame)
		{
			stats[player]->clearStats();
		}
		stringCopy(
			stats[player]->name, reinterpret_cast<char*>(&net_packet->data[5]),
			sizeof(Stat::name), 32);
		client_classes[player] = static_cast<int>(
			SDLNet_Read32(&net_packet->data[37]));
		stats[player]->sex = static_cast<sex_t>(
			static_cast<int>(SDLNet_Read32(&net_packet->data[41])));
		const Uint32 raceAndAppearance =
			SDLNet_Read32(&net_packet->data[45]);
		stats[player]->stat_appearance = (raceAndAppearance & 0xFF00) >> 8;
		stats[player]->playerRace = raceAndAppearance & 0xFF;
		if (!loadingsavegame)
		{
			initClass(player);
		}
		for (int recipient = 1; recipient < MAXPLAYERS; ++recipient)
		{
			if (recipient == player
				|| !serverPlayerCanReceiveGameplayUpdates(recipient))
			{
				continue;
			}
			memcpy(net_packet->data, "JOIN", 4);
			net_packet->data[4] = player;
			net_packet->data[5] = client_classes[player];
			net_packet->data[6] = stats[player]->sex;
			net_packet->data[7] =
				static_cast<Uint8>(stats[player]->stat_appearance);
			net_packet->data[8] =
				static_cast<Uint8>(stats[player]->playerRace);
			stringCopy(
				reinterpret_cast<char*>(&net_packet->data[9]),
				stats[player]->name,
				32,
				sizeof(Stat::name));
			net_packet->len = 41;
			net_packet->address.host = net_clients[recipient - 1].host;
			net_packet->address.port = net_clients[recipient - 1].port;
			sendPacketSafe(net_sock, -1, net_packet, recipient - 1);
		}
		g_lateJoinLastProgressTick[player] = ticks;
	}},
	{'REDY', [](){
		if (net_packet->len != 6)
		{
			printlog("[Late Join] Server rejected malformed Ready packet.");
			return;
		}
		const int player = decodeGameplayPacketPlayerIndex(net_packet->data[4]);
		if (player <= 0 || !currentPacketSenderMatchesPlayer(player)
			|| !g_lateJoinClientHandshake[player]
			|| g_lateJoinTransactions[player].phase()
				!= LateJoinSnapshotTransaction::Phase::AwaitingClient)
		{
			printlog("[Late Join] Server rejected Ready for player %d.", player);
			return;
		}
		if (net_packet->data[5] == 0)
		{
			g_lateJoinLastProgressTick[player] = ticks;
			return;
		}
		std::string placementError;
		if (!prepareAutomatiaLateJoinPlayer(
				player, g_lateJoinReturningPlayer[player], placementError)
			|| !startServerLateJoinSnapshotTransfer(player))
		{
			printlog(
				"[Late Join] Failed to prepare Ready player %d: %s",
				player,
				placementError.empty()
					? "snapshot transfer could not start"
					: placementError.c_str());
			abortServerLateJoinPlayer(player, 4);
			return;
		}
		players[player]->was_connected_to_game = true;
		g_lateJoinLastProgressTick[player] = ticks;
		printlog(
			"[Late Join] Player %d pressed Ready; snapshot transfer started.",
			player);
	}},
	{'SVFL', [](){
		if (net_packet->len != 5)
		{
			printlog("[Late Join] Rejected malformed server-flags request.");
			return;
		}
		const int player = decodeGameplayPacketPlayerIndex(net_packet->data[4]);
		if (player <= 0 || !currentPacketSenderMatchesPlayer(player))
		{
			printlog("[Late Join] Rejected unauthenticated server-flags request.");
			return;
		}
		memcpy(net_packet->data, "SVFL", 4);
		SDLNet_Write32(svFlags, &net_packet->data[4]);
		net_packet->len = 8;
		net_packet->address.host = net_clients[player - 1].host;
		net_packet->address.port = net_clients[player - 1].port;
		sendPacketSafe(net_sock, -1, net_packet, player - 1);
		printlog("[Late Join] Sent server flags to player %d.", player);
	}},
	{'CSCN', [](){
		if (net_packet->len != 5)
		{
			printlog("[Late Join] Rejected malformed scenario request.");
			return;
		}
		const int player = decodeGameplayPacketPlayerIndex(net_packet->data[4]);
		if (player <= 0 || !currentPacketSenderMatchesPlayer(player))
		{
			printlog("[Late Join] Rejected unauthenticated scenario request.");
			return;
		}
		memcpy(net_packet->data, "CSCN", 4);
		if (!gameModeManager.currentSession.challengeRun.isActive())
		{
			net_packet->data[4] = 0;
			net_packet->len = 5;
			net_packet->address.host = net_clients[player - 1].host;
			net_packet->address.port = net_clients[player - 1].port;
			sendPacketSafe(net_sock, -1, net_packet, player - 1);
			printlog("[Late Join] Sent empty custom scenario to player %d.", player);
			return;
		}
		const std::string& scenario =
			gameModeManager.currentSession.challengeRun.scenarioStr;
		constexpr std::size_t chunkBytes = 256;
		const std::size_t chunkCount = std::max<std::size_t>(
			1, (scenario.size() + chunkBytes - 1) / chunkBytes);
		if (chunkCount > 15)
		{
			printlog("[Late Join] Custom scenario is too large for player %d.", player);
			return;
		}
		for (std::size_t chunk = 0; chunk < chunkCount; ++chunk)
		{
			const std::size_t offset = chunk * chunkBytes;
			const std::size_t bytes = std::min(chunkBytes, scenario.size() - offset);
			memcpy(net_packet->data, "CSCN", 4);
			net_packet->data[4] = static_cast<Uint8>(chunk + 1)
				| static_cast<Uint8>(chunkCount << 4);
			memcpy(&net_packet->data[5], scenario.data() + offset, bytes);
			net_packet->len = static_cast<int>(5 + bytes);
			net_packet->address.host = net_clients[player - 1].host;
			net_packet->address.port = net_clients[player - 1].port;
			sendPacketSafe(net_sock, -1, net_packet, player - 1);
		}
		printlog("[Late Join] Sent custom scenario to player %d in %zu chunk(s).",
			player, chunkCount);
	}},
	{'LJRD', [](){
		LateJoinProtocol::Ready ready;
		if (!LateJoinProtocol::decodeReady(
				net_packet->data, net_packet->len, ready))
		{
			printlog("[Late Join] Server rejected malformed snapshot-ready packet.");
			return;
		}
		const int player = decodeGameplayPacketPlayerIndex(ready.playerIndex);
		if (player <= 0 || !currentPacketSenderMatchesPlayer(player)
			|| !ready.snapshotAccepted
			|| g_lateJoinTransactions[player].phase()
				!= LateJoinSnapshotTransaction::Phase::Complete
			|| g_lateJoinTransactions[player].transferId()
				!= ready.transferId
			|| g_lateJoinTransactions[player].instanceRevision()
				!= ready.instanceRevision)
		{
			printlog(
				"[Late Join] Server rejected snapshot-ready state for player %d.",
				player);
			if (player > 0)
			{
				abortServerLateJoinPlayer(player, 2);
			}
			return;
		}
		LateJoinProtocol::Authorization authorization;
		authorization.transferId = ready.transferId;
		authorization.instanceRevision = ready.instanceRevision;
		authorization.spawnAuthorized = true;
		if (!queueLateJoinRecordForPlayer(
				player, LateJoinProtocol::encodeAuthorization(authorization)))
		{
			printlog(
				"[Late Join] Server failed spawn authorization for player %d.",
				player);
			abortServerLateJoinPlayer(player, 4);
			return;
		}
		printlog(
			"[Late Join] Server sent spawn authorization to player %d transfer %u.",
			player, ready.transferId);
		g_lateJoinLastProgressTick[player] = ticks;
	}},
	{'LJGO', [](){
		LateJoinProtocol::Ready go;
		if (!LateJoinProtocol::decodeGo(
				net_packet->data, net_packet->len, go))
		{
			printlog("[Late Join] Server rejected malformed authorization ACK.");
			return;
		}
		const int player = decodeGameplayPacketPlayerIndex(go.playerIndex);
		if (player <= 0 || !currentPacketSenderMatchesPlayer(player)
			|| !go.snapshotAccepted
			|| g_lateJoinTransactions[player].phase()
				!= LateJoinSnapshotTransaction::Phase::Complete
			|| g_lateJoinTransactions[player].transferId() != go.transferId
			|| g_lateJoinTransactions[player].instanceRevision()
				!= go.instanceRevision
			|| !sendServerLateJoinCatchup(player)
			|| !sendServerLateJoinStart(player)
			|| !authorizeServerLateJoinPlayer(player))
		{
			printlog(
				"[Late Join] Server rejected authorization ACK for player %d.",
				player);
			if (player > 0)
			{
				abortServerLateJoinPlayer(player, 2);
			}
			return;
		}
		consumeAutomatiaSavedPlayerPlacement(player);
		g_lateJoinLastProgressTick[player] = 0;
		printlog(
			"[Late Join] Server opened live simulation for player %d transfer %u.",
			player, go.transferId);
	}},
	{'LJAB', [](){
		LateJoinProtocol::Abort abort;
		if (!LateJoinProtocol::decodeAbort(
				net_packet->data, net_packet->len, abort))
		{
			printlog("[Late Join] Server rejected malformed abort record.");
			return;
		}
		const int player = static_cast<int>(abort.playerIndex);
		if (!currentPacketSenderMatchesPlayer(player)
			|| (abort.transferId != 0
				&& (abort.transferId
						!= g_lateJoinTransactions[player].transferId()
					|| abort.instanceRevision
						!= g_lateJoinTransactions[player].instanceRevision())))
		{
			printlog("[Late Join] Server ignored unauthenticated abort record.");
			return;
		}
		printlog("[Late Join] Client %d aborted transfer (reason %u).",
			player, static_cast<unsigned>(abort.reason));
		abortServerLateJoinPlayer(player, abort.reason);
	}},
	// keep alive
	{'KPAL', [](){
	    const int player =
	    	decodeGameplayPacketPlayerIndex(
	    		net_packet->data[4]
	    	);
	    if ( player < 0 )
	    {
	    	return;
	    }
		client_keepalive[player] = ticks;
	}},

	// custom dialogue choice selected by a remote client
	{'CDSL', []() {
		if ( multiplayer != SERVER
			|| net_packet->len != 10 )
		{
			printlog(
				"[Custom Dialogue] Rejected malformed CDSL packet length %d.",
				net_packet->len
			);
			return;
		}

		const int player =
            decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);

		if ( player <= 0
			|| player >= MAXPLAYERS
			|| client_disconnected[player]
			|| !players[player]
			|| players[player]->isLocalPlayer() )
		{
			printlog(
				"[Custom Dialogue] Rejected CDSL packet with invalid remote player slot %d.",
				player
			);
			return;
		}

		/*
		 * For direct-IP/LAN games, verify that the claimed player slot
		 * matches the UDP endpoint assigned when that client joined.
		 */
		if ( directConnect
			&& (
				net_packet->address.host
					!= net_clients[player - 1].host
				|| net_packet->address.port
					!= net_clients[player - 1].port
			) )
		{
			printlog(
				"[Custom Dialogue] Rejected CDSL packet whose endpoint does not match player %d.",
				player
			);
			return;
		}

		const Uint32 npcUID =
			SDLNet_Read32(
				&net_packet->data[5]
			);

		const int choiceIndex =
			static_cast<int>(
				net_packet->data[9]
			);

		if ( handleCustomMonsterDialogueChoice(
				player,
				npcUID,
				choiceIndex
			) )
		{
			printlog(
				"[Custom Dialogue] Host accepted player %d choice index %d for NPC UID %u.",
				player,
				choiceIndex,
				npcUID
			);
		}
		else
		{
			printlog(
				"[Custom Dialogue] Host rejected player %d choice index %d for NPC UID %u.",
				player,
				choiceIndex,
				npcUID
			);
		}
	}},

	// ping
	{'PING', [](){
		const int j = net_packet->data[4];
		if (j <= 0 || j >= MAXPLAYERS )
		{
			return;
		}
		if ( client_disconnected[j] || !players[j] || players[j]->isLocalPlayer() )
		{
			return;
		}
		memcpy((char*)net_packet->data, "PING", 4);
		net_packet->address.host = net_clients[j - 1].host;
		net_packet->address.port = net_clients[j - 1].port;
		net_packet->len = 5;
		sendPacketSafe(net_sock, -1, net_packet, j - 1);
	}},

	// automated ping
	{'PNGU', []() {
		PingNetworkStatus_t::respond();
	}},

	// automated ping response
	{'PNGR', []() {
		PingNetworkStatus_t::receive();
	}},

	// network scan
	{'SCAN', [](){
	    handleScanPacket();
	}},

	// pause game
	{'PAUS', [](){
		messagePlayer(clientnum, MESSAGE_MISC, Language::get(1118), stats[net_packet->data[4]]->name);
		const int j =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( j < 0 )
		{
			return;
		}
		pauseGame(2, j);
	}},

	// unpause game
	{'UNPS', [](){
		messagePlayer(clientnum, MESSAGE_MISC, Language::get(1119), stats[net_packet->data[4]]->name);
		const int j =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( j < 0 )
		{
			return;
		}
		pauseGame(1, j);
	}},

	// check entity existence
	{'ENTE', [](){
		if ( net_packet->len < 9 )
		{
			return;
		}
		const int x = decodeGameplayPacketPlayerIndex(net_packet->data[4]);
		if ( x <= 0 )
		{
			return;
		}
		if ( players[x]->isLocalPlayer() )
		{
			return;
		}
		Uint32 uid = SDLNet_Read32(&net_packet->data[5]);
		Entity* entity = uidToEntity(uid);
		if ( entity )
		{
			return; // found entity.
		}
		// else reply with entity deleted.
		strcpy((char*)net_packet->data, "ENTD");
		SDLNet_Write32(uid, &net_packet->data[4]);
		net_packet->address.host = net_clients[x - 1].host;
		net_packet->address.port = net_clients[x - 1].port;
		net_packet->len = 8;
		sendPacketSafe(net_sock, -1, net_packet, x - 1);
	}},

	// client request item details.
	{'ITMU', [](){
		if ( net_packet->len < 9 )
		{
			return;
		}
		const int x = decodeGameplayPacketPlayerIndex(net_packet->data[4]);
		if ( x <= 0 )
		{
			return;
		}
		Uint32 uid = SDLNet_Read32(&net_packet->data[5]);
		Entity* entity = uidToEntity(uid);
		if ( entity )
		{
            strcpy((char*)net_packet->data, "ITMU");
            SDLNet_Write32(uid, &net_packet->data[4]);

            int transmittedType = entity->skill[10];
            std::string stableId;
#ifdef SAM_FRAMEWORK_ENABLED
            constexpr int samWorldItemTypeSentinel = 0xFFFF;
            if ( SAMItemRegistryFoundation::
                isRegisteredRuntimeItemId(entity->skill[10]) )
            {
                stableId =
                    SAMItemRegistryFoundation::
                        stableIdForRuntimeId(entity->skill[10]);
                if ( stableId.empty() )
                {
                    printlog(
                        "[S.A.M] Refusing ITMU world item runtime %d for entity %u: no stable id.\n",
                        entity->skill[10],
                        uid
                    );
                    return;
                }
                transmittedType = samWorldItemTypeSentinel;
            }
#endif

            Uint32 itemTypeAndIdentified =
                ((static_cast<Uint16>(transmittedType) & 0xFFFF) << 16);
            itemTypeAndIdentified |=
                (static_cast<Uint16>(entity->skill[15]) & 0xFFFF);

            SDLNet_Write32(itemTypeAndIdentified, &net_packet->data[8]);

			Uint32 statusBeatitudeQuantityAppearance = 0;
			statusBeatitudeQuantityAppearance |= ((static_cast<Uint8>(entity->skill[11]) & 0xFF) << 24); // status
			statusBeatitudeQuantityAppearance |= ((static_cast<Sint8>(entity->skill[12]) & 0xFF) << 16); // beatitude
			statusBeatitudeQuantityAppearance |= ((static_cast<Uint8>(entity->skill[13]) & 0xFF) << 8); // quantity
			Uint8 appearance = entity->skill[14] % items[entity->skill[10]].variations;
			if ( entity->skill[10] == TOOL_PLAYER_LOOT_BAG )
			{
				appearance = (entity->skill[14] & 0xF) % items[entity->skill[10]].variations;
			}
			else if ( entity->skill[10] == ENCHANTED_FEATHER )
			{
				appearance = entity->skill[14] % ENCHANTED_FEATHER_MAX_DURABILITY;
			}
			else if ( entity->skill[10] == MAGICSTAFF_SCEPTER )
			{
				appearance = entity->skill[14] % MAGICSTAFF_SCEPTER_CHARGE_MAX;
			}
			statusBeatitudeQuantityAppearance |= (static_cast<Uint8>(appearance) & 0xFF); // appearance
			SDLNet_Write32(statusBeatitudeQuantityAppearance, &net_packet->data[12]);

            net_packet->len = 16;
#ifdef SAM_FRAMEWORK_ENABLED
            if ( !stableId.empty() )
            {
                const int available = NET_PACKET_SIZE - 17;
                if ( available <= 0
                    || static_cast<int>(stableId.size()) > available )
                {
                    printlog(
                        "[S.A.M] Refusing ITMU world item [%s]: stable id is too long.\n",
                        stableId.c_str()
                    );
                    return;
                }

                memcpy(
                    &net_packet->data[16],
                    stableId.c_str(),
                    stableId.size()
                );
                net_packet->data[16 + stableId.size()] = '\0';
                net_packet->len =
                    17 + static_cast<int>(stableId.size());

                printlog(
                    "[S.A.M] Sending ITMU world item [%s] runtime %d for entity %u to player %d.\n",
                    stableId.c_str(),
                    entity->skill[10],
                    uid,
                    x
                );
            }
            else
#endif
            if ( entity->skill[10] >= 0 && entity->skill[10] < NUMITEMS )
            {
                if ( items[entity->skill[10]].category == TOME_SPELL )
                {
                    SDLNet_Write16(
                        entity->skill[14] % TOME_APPEARANCE_MAX,
                        &net_packet->data[16]
                    );
                    net_packet->len = 18;
                }
            }

            net_packet->address.host = net_clients[x - 1].host;
			net_packet->address.port = net_clients[x - 1].port;
			sendPacketSafe(net_sock, -1, net_packet, x - 1);
		}
	}},

	// player move
	{'PMOV', [](){
        if ( !net_packet || net_packet->len < 19 )
        {
            printlog("[NET]: ignored truncated PMOV packet");
            return;
        }
        const int player =
            decodeGameplayPacketPlayerIndex(
                net_packet->data[4]
            );
        if ( player < 0 )
		{
			return;
		}
		client_keepalive[player] = ticks;
		if (players[player] == nullptr || players[player]->entity == nullptr)
		{
			return;
		}

		// check if the info is outdated
		if ( net_packet->data[5] != currentlevel || net_packet->data[18] != secretlevel )
		{
			return;
		}

		// get info from client
		auto dx = ((Sint16)SDLNet_Read16(&net_packet->data[6])) / 32.0;
		auto dy = ((Sint16)SDLNet_Read16(&net_packet->data[8])) / 32.0;
		auto velx = ((Sint16)SDLNet_Read16(&net_packet->data[10])) / 128.0;
		auto vely = ((Sint16)SDLNet_Read16(&net_packet->data[12])) / 128.0;
		auto yaw = ((Sint16)SDLNet_Read16(&net_packet->data[14])) / 128.0;
		auto pitch = ((Sint16)SDLNet_Read16(&net_packet->data[16])) / 128.0;

		// update rotation
		players[player]->entity->yaw = yaw;
		players[player]->entity->pitch = pitch;

		// update player's internal velocity variables
		players[player]->entity->vel_x = velx; // PLAYER_VELX
		players[player]->entity->vel_y = vely; // PLAYER_VELY

		// store old coordinates
		// since this function runs more often than actPlayer runs, we need to keep track of the accumulated position in new_x/new_y
		real_t ox = players[player]->entity->x;
		real_t oy = players[player]->entity->y;
		players[player]->entity->x = players[player]->entity->new_x;
		players[player]->entity->y = players[player]->entity->new_y;

		// calculate distance
		dx -= players[player]->entity->x;
		dy -= players[player]->entity->y;
		auto dist = sqrt( dx * dx + dy * dy );

		// move player with collision detection
		real_t result = clipMove(&players[player]->entity->x, &players[player]->entity->y, dx, dy, players[player]->entity);
		if ( result < dist - .025 )
		{
			// player encountered obstacle on path
			// stop updating position on server side and send client corrected position
			const int j = net_packet->data[4];
			if ( j > 0 && j < MAXPLAYERS )
			{
				strcpy((char*)net_packet->data, "PMOV");
				SDLNet_Write16((Sint16)(players[j]->entity->x * 32), &net_packet->data[4]);
				SDLNet_Write16((Sint16)(players[j]->entity->y * 32), &net_packet->data[6]);
				net_packet->address.host = net_clients[j - 1].host;
				net_packet->address.port = net_clients[j - 1].port;
				net_packet->len = 8;
				sendPacket(net_sock, -1, net_packet, j - 1);
			}
		}

		// clipMove sent any corrections to the client, now let's save the updated coordinates.
		players[player]->entity->new_x = players[player]->entity->x;
		players[player]->entity->new_y = players[player]->entity->y;
		// return x/y to their original state as this can update more than actPlayer and causes stuttering. use new_x/new_y in actPlayer.
		players[player]->entity->x = ox;
		players[player]->entity->y = oy;

		// update the players' head and mask as these will otherwise wait until actPlayer to update their rotation. stops clipping.
		node_t* tmpNode = nullptr;
		int bodypartNum = 0;
		for ( bodypartNum = 0, tmpNode = players[player]->entity->children.first; tmpNode; tmpNode = tmpNode->next, bodypartNum++ )
		{
			if ( bodypartNum == 0 )
			{
				// hudweapon case
				continue;
			}

			Entity* limb = (Entity*)tmpNode->element;
			if ( limb )
			{
				// adjust headgear/mask yaw/pitch variations as these do not update always.
				if ( bodypartNum == 9 || bodypartNum == 10 )
				{
					limb->x = players[player]->entity->x;
					limb->y = players[player]->entity->y;
					limb->pitch = players[player]->entity->pitch;
					limb->yaw = players[player]->entity->yaw;
				}
			}
		}
	}},

	// player ghost move
	{'GMOV', []() {
        if ( !net_packet || net_packet->len < 20 )
        {
            printlog("[NET]: ignored truncated GMOV packet");
            return;
        }
        const int player =
            decodeGameplayPacketPlayerIndex(
                net_packet->data[4]
            );
        if ( player < 0 )
		{
			return;
		}
		client_keepalive[player] = ticks;
		if ( players[player] == nullptr || players[player]->ghost.my == nullptr )
		{
			return;
		}

		// check if the info is outdated
		if ( net_packet->data[5] != currentlevel || net_packet->data[18] != secretlevel )
		{
			return;
		}

		// get info from client
		auto dx = ((Sint16)SDLNet_Read16(&net_packet->data[6])) / 32.0;
		auto dy = ((Sint16)SDLNet_Read16(&net_packet->data[8])) / 32.0;
		auto velx = ((Sint16)SDLNet_Read16(&net_packet->data[10])) / 128.0;
		auto vely = ((Sint16)SDLNet_Read16(&net_packet->data[12])) / 128.0;
		auto yaw = ((Sint16)SDLNet_Read16(&net_packet->data[14])) / 128.0;
		auto pitch = ((Sint16)SDLNet_Read16(&net_packet->data[16])) / 128.0;
		bool bounce = ((int)(net_packet->data[19] & 1) == 1) ? true : false;
		int deactivated = ((int)((net_packet->data[19] >> 1) & 1) == 1) ? 1 : 0;

		// update rotation
		players[player]->ghost.my->yaw = yaw;
		players[player]->ghost.my->pitch = pitch;

		// update player's internal velocity variables
		players[player]->ghost.my->vel_x = velx; // PLAYER_VELX
		players[player]->ghost.my->vel_y = vely; // PLAYER_VELY

		// store old coordinates
		// since this function runs more often than actPlayer runs, we need to keep track of the accumulated position in new_x/new_y
		real_t ox = players[player]->ghost.my->x;
		real_t oy = players[player]->ghost.my->y;
		players[player]->ghost.my->x = players[player]->ghost.my->new_x;
		players[player]->ghost.my->y = players[player]->ghost.my->new_y;

		// calculate distance
		dx -= players[player]->ghost.my->x;
		dy -= players[player]->ghost.my->y;
		auto dist = sqrt(dx * dx + dy * dy);

		// move player with collision detection
		real_t result = clipMove(&players[player]->ghost.my->x, &players[player]->ghost.my->y, dx, dy, players[player]->ghost.my);
		if ( result < dist - .025 )
		{
			// player encountered obstacle on path
			// stop updating position on server side and send client corrected position
			const int j = net_packet->data[4];
			if ( j > 0 && j < MAXPLAYERS )
			{
				strcpy((char*)net_packet->data, "GMOV");
				SDLNet_Write16((Sint16)(players[j]->ghost.my->x * 32), &net_packet->data[4]);
				SDLNet_Write16((Sint16)(players[j]->ghost.my->y * 32), &net_packet->data[6]);
				net_packet->address.host = net_clients[j - 1].host;
				net_packet->address.port = net_clients[j - 1].port;
				net_packet->len = 8;
				sendPacket(net_sock, -1, net_packet, j - 1);
			}
		}

		// clipMove sent any corrections to the client, now let's save the updated coordinates.
		players[player]->ghost.my->new_x = players[player]->ghost.my->x;
		players[player]->ghost.my->new_y = players[player]->ghost.my->y;
		// return x/y to their original state as this can update more than actPlayer and causes stuttering. use new_x/new_y in actPlayer.
		players[player]->ghost.my->x = ox;
		players[player]->ghost.my->y = oy;

		if ( bounce )
		{
			players[player]->ghost.my->fskill[9] = Player::Ghost_t::GHOST_SQUISH_START_ANGLE / 100.f;
			playSoundEntityLocal(players[player]->ghost.my, 612 + local_rng.rand() % 3, 64);
			for ( int c = 1; c < MAXPLAYERS; ++c ) // send to other players
			{
				if ( c == player || client_disconnected[c] || !players[c] || players[c]->isLocalPlayer() )
				{
					continue;
				}
				strcpy((char*)net_packet->data, "GHFS");
				SDLNet_Write32(players[player]->ghost.my->getUID(), &net_packet->data[4]);
				net_packet->data[8] = 9;
				SDLNet_Write16(static_cast<Sint16>(players[player]->ghost.my->fskill[9] * 256), &net_packet->data[9]);
				net_packet->address.host = net_clients[c - 1].host;
				net_packet->address.port = net_clients[c - 1].port;
				net_packet->len = 11;
				sendPacketSafe(net_sock, -1, net_packet, c - 1);
			}
		}
		if ( deactivated != players[player]->ghost.my->skill[7] )
		{
			players[player]->ghost.setActive(deactivated == 0 ? true : false);
			for ( int c = 1; c < MAXPLAYERS; ++c ) // send to other players
			{
				if ( c == player || client_disconnected[c] || !players[c] || players[c]->isLocalPlayer() )
				{
					continue;
				}
				strcpy((char*)net_packet->data, "ENTS");
				SDLNet_Write32(players[player]->ghost.my->getUID(), &net_packet->data[4]);
				net_packet->data[8] = 7;
				SDLNet_Write32(players[player]->ghost.my->skill[7], &net_packet->data[9]);
				net_packet->address.host = net_clients[c - 1].host;
				net_packet->address.port = net_clients[c - 1].port;
				net_packet->len = 13;
				sendPacketSafe(net_sock, -1, net_packet, c - 1);
			}
		}
	}},

	{'REZZ', []() {
		if ( !net_packet || net_packet->len < 11 )
		{
			printlog(
				"[NET]: ignoring truncated REZZ packet"
			);
			return;
		}

		if ( net_packet->data[5] != currentlevel
			|| net_packet->data[10] != secretlevel )
		{
			return;
		}

		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( player < 0
			|| !players[player]
			|| !stats[player] )
		{
			return;
		}
		if ( players[player]->entity )
		{
			return;
		}

		const int x =
			SDLNet_Read16(&net_packet->data[6]);
		const int y =
			SDLNet_Read16(&net_packet->data[8]);

		Entity* entity = newEntity(113, 1, map.entities, nullptr); //Player entity.
		entity->x = (x * 16) + 8;
		entity->y = (y * 16) + 8;
		entity->new_x = entity->x;
		entity->new_y = entity->y;
		entity->z = -1;
		entity->flags[INVISIBLE] = false;
		entity->flags[GENIUS] = true;
		entity->behavior = &actPlayer;
		entity->skill[2] = player;
		entity->yaw = 0.0;
		entity->sizex = 4;
		entity->sizey = 4;
		entity->focalx = limbs[HUMAN][0][0]; // 0
		entity->focaly = limbs[HUMAN][0][1]; // 0
		entity->focalz = limbs[HUMAN][0][2]; // -1.5
		entity->flags[UPDATENEEDED] = true;
		entity->flags[BLOCKSIGHT] = true;
		entity->addToCreatureList(map.creatures);
		players[player]->entity = entity;
		stats[player]->HP = stats[player]->MAXHP / 2;
	}},

	// player created ghost
	{'GHOS', []() {
		if ( !net_packet || net_packet->len < 11 )
		{
			printlog(
				"[NET]: ignoring truncated GHOS packet"
			);
			return;
		}

		if ( net_packet->data[5] != currentlevel
			|| net_packet->data[10] != secretlevel )
		{
			return;
		}

		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( player < 0 || !players[player] )
		{
			return;
		}

		const int x =
			SDLNet_Read16(&net_packet->data[6]);
		const int y =
			SDLNet_Read16(&net_packet->data[8]);

		if ( players[player]->ghost.my )
		{
			list_RemoveNode(
				players[player]->ghost.my->mynode
			);
			players[player]->ghost.my = nullptr;
		}
		players[player]->ghost.reset();

		// deathcam
		int sprite = Player::Ghost_t::getSpriteForPlayer(player);
		Entity* entity = newEntity(sprite, 1, map.entities, nullptr); //Ghost entity.
		players[player]->ghost.my = entity;
		players[player]->ghost.uid = entity->getUID();
		entity->x = (x * 16) + 8;
		entity->y = (y * 16) + 8;
		entity->new_x = entity->x;
		entity->new_y = entity->y;
		entity->z = -4;
		entity->flags[PASSABLE] = true;
		entity->flags[INVISIBLE] = true;
		entity->flags[GENIUS] = true;
		entity->behavior = &actDeathGhost;
		entity->skill[2] = player;
		entity->yaw = 0.0;
		entity->pitch = PI / 16;
		entity->sizex = 2;
		entity->sizey = 2;
		entity->flags[UPDATENEEDED] = true;
		Compendium_t::Events_t::eventUpdateMonster(player, Compendium_t::CPDM_GHOST_SPAWNED, entity, 1);
	}},

	// tried to update
	{'NOUP', [](){
		if ( !net_packet || net_packet->len < 9 )
		{
			return;
		}
		const int player =
			decodeGameplayPacketPlayerIndex(net_packet->data[4]);
		if ( player <= 0 )
		{
			return;
		}
		Uint32 uid = SDLNet_Read32(&net_packet->data[5]);
		Entity* entity = uidToEntity(uid);
		if ( entity )
		{
			if ( entity->behavior == &actPlayer
				|| entity->behavior == &actDeathGhost )
			{
				printlog(
					"[World State] Rejected player %d NOUP for authoritative player entity UID %u in '%s'.",
					player,
					uid,
					players[player]
						? players[player]->worldInstance.key().c_str()
						: "<none>");
				return;
			}
			entity->flags[UPDATENEEDED] = false;
		}
	}},

	// client deleted entity
	/*{'ENTD', [](){
	    const int player =
	    	decodeGameplayPacketPlayerIndex(
	    		net_packet->data[4]
	    	);
	    if ( player < 0 )
	    {
	    	return;
	    }
		for ( auto node = entitiesToDelete[player].first; node != NULL; node = node->next )
		{
			auto deleteent = (deleteent_t*)node->element;
			if ( deleteent->uid == SDLNet_Read32(&net_packet->data[5]) )
			{
				list_RemoveNode(node);
				break;
			}
		}
	}},*/

	// clicked entity in range
	{'CKIR', [](){
        if ( !net_packet || net_packet->len < 9 )
        {
            printlog("[NET]: ignored truncated CKIR packet");
            return;
        }
	    const int player =
	    	decodeGameplayPacketPlayerIndex(
	    		net_packet->data[4]
	    	);
	    if ( player < 0 )
	    {
	    	return;
	    }
		client_keepalive[player] = ticks;
		Uint32 uid = SDLNet_Read32(&net_packet->data[5]);
		Entity* entity = uidToEntity(uid);
		const bool customPortalHint = net_packet->len >= 19
			&& net_packet->data[9] == 0xA7
			&& net_packet->data[10] == 1;
		if (customPortalHint
			&& (!entity || entity->behavior != &actCustomPortal))
		{
			const real_t hintX = static_cast<Sint16>(
				SDLNet_Read16(&net_packet->data[11])) / 32.0;
			const real_t hintY = static_cast<Sint16>(
				SDLNet_Read16(&net_packet->data[13])) / 32.0;
			Entity* resolvedPortal = nullptr;
			real_t bestFixtureDistance = 1.0;
			std::size_t serverPortalCount = 0;
			if (map.entities)
			{
				for (node_t* node = map.entities->first; node; node = node->next)
				{
					Entity* candidate = static_cast<Entity*>(node->element);
					if (!candidate || candidate->behavior != &actCustomPortal)
					{
						continue;
					}
					++serverPortalCount;
					// X/Y identify a static fixture. Z can legitimately differ
					// while a late client's render interpolation settles.
					const real_t fixtureDistance =
						std::abs(candidate->x - hintX)
						+ std::abs(candidate->y - hintY);
					if (fixtureDistance <= bestFixtureDistance)
					{
						resolvedPortal = candidate;
						bestFixtureDistance = fixtureDistance;
					}
				}
			}
			Entity* playerEntity = players[player]
				? players[player]->entity : nullptr;
			if (resolvedPortal && playerEntity
				&& entityDist(resolvedPortal, playerEntity) <= TOUCHRANGE)
			{
				printlog(
					"[Client Activity] Safely resolved player %d local UID %u to authoritative custom exit UID %u by map position.",
					player, uid, resolvedPortal->getUID());
				entity = resolvedPortal;
			}
			else
			{
				printlog(
					"[Client Activity] Rejected player %d custom-exit hint for local UID %u at (%.2f,%.2f) (server portals=%zu, fixture match=%s, server range=%s).",
					player, uid, hintX, hintY, serverPortalCount,
					resolvedPortal ? "yes" : "no",
					resolvedPortal && playerEntity
						&& entityDist(resolvedPortal, playerEntity) <= TOUCHRANGE
						? "yes" : "no");
				entity = nullptr;
			}
		}
		if ( entity )
		{
			client_selected[player] = entity;
			inrange[player] = true;
			Entity* playerEntity = players[player]
				? players[player]->entity : nullptr;
			const double distance = playerEntity
				? entityDist(entity, playerEntity) : -1.0;
			const WorldInstanceIdentity* identity =
				worldState.activeIdentity();
			printlog(
				"[Client Activity] Player %d selected UID %u in '%s' (sprite=%d, type=%s, distance=%.2f, invisible=%s, circuit=%d).",
				player, uid,
				identity ? identity->key().c_str() : "<none>",
				entity->sprite,
				entity->behavior == &actCustomPortal
					? "custom-exit" : "other",
				distance,
				entity->flags[INVISIBLE] ? "yes" : "no",
				entity->skill[28]);
		}
		else
		{
			const WorldInstanceIdentity* identity =
				worldState.activeIdentity();
			printlog(
				"[Client Activity] Player %d selected unknown UID %u in '%s'; interaction was not applied.",
				player, uid,
				identity ? identity->key().c_str() : "<none>");
		}
	}},

	// tinker salvage
	{'SALV', [](){
	    const int player =
	    	decodeGameplayPacketPlayerIndex(
	    		net_packet->data[4]
	    	);
	    if ( player < 0 )
	    {
	    	return;
	    }
		client_keepalive[player] = ticks;
		Uint32 uid = SDLNet_Read32(&net_packet->data[5]);
		Entity* entity = uidToEntity(uid);
		if ( entity )
		{
			if ( (entity->behavior == &actItem || entity->behavior == &actTorch || entity->behavior == &actCrystalShard) )
			{
				// auto salvage this item.
				if ( players[player] && players[player]->entity )
				{
					entity->itemAutoSalvageByPlayer = static_cast<Sint32>(players[player]->entity->getUID());
				}
			}
			client_selected[player] = entity;
			inrange[player] = true;
		}
	}},

	// clicked wall lock entity in range with key
	{'LKEY', []() {
		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( player < 0 )
		{
			return;
		}
		client_keepalive[player] = ticks;
		Uint32 uid = SDLNet_Read32(&net_packet->data[5]);
		Entity* entity = uidToEntity(uid);
		if ( entity && entity->behavior == &actWallLock )
		{
			if ( players[player]->entity )
			{
				client_selected[player] = entity;
				inrange[player] = true;
				if ( entity->wallLockState == Entity::WallLockStates::LOCK_NO_KEY )
				{
					if ( entity->wallLockPlayerInteracting == 0 )
					{
						entity->wallLockPlayerInteracting = players[player]->entity->getUID();
					}
					else if ( entity->wallLockPlayerInteracting == players[player]->entity->getUID() )
					{
						// client has already queued up an action, drop this interaction
						client_selected[player] = nullptr;
						inrange[player] = false;
					}
				}
			}
		}
	}},

	// clicked wall lock entity in range without key
	{'LNOK', []() {
		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( player < 0 )
		{
			return;
		}
		client_keepalive[player] = ticks;
		Uint32 uid = SDLNet_Read32(&net_packet->data[5]);
		Entity* entity = uidToEntity(uid);
		if ( entity && entity->behavior == &actWallLock )
		{
			if ( players[player]->entity )
			{
				client_selected[player] = entity;
				inrange[player] = true;
				if ( entity->wallLockState == Entity::WallLockStates::LOCK_NO_KEY )
				{
					if ( entity->wallLockPlayerInteracting == players[player]->entity->getUID() )
					{
						// client has already queued up an action, drop this interaction
						client_selected[player] = nullptr;
						inrange[player] = false;
					}
				}
			}
		}
	}},

	// client checked valid key for the lock
	{ 'OKEY', []() {
		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( player < 0 )
		{
			return;
		}
		client_keepalive[player] = ticks;
		Uint32 uid = SDLNet_Read32(&net_packet->data[5]);
		Entity* entity = uidToEntity(uid);
		if ( entity && entity->behavior == &actWallLock )
		{
			if ( entity->wallLockState == Entity::WallLockStates::LOCK_NO_KEY && net_packet->data[9] != 0 ) // success from client
			{
				Uint16 key = SDLNet_Read16(&net_packet->data[10]);
				if ( key >= WOODEN_SHIELD && key < NUMITEMS )
				{
					messagePlayer(player, MESSAGE_INTERACTION, Language::get(6378), items[key].getIdentifiedName());
					Compendium_t::Events_t::eventUpdateWorld(player, Compendium_t::CPDM_KEYLOCK_UNLOCKED_KEY, "wall locks", 1);
					Compendium_t::Events_t::eventUpdate(player, Compendium_t::CPDM_KEYLOCK_UNLOCKED_KEY, (ItemType)key, 1);
					if ( key == KEY_IRON )
					{
						Compendium_t::Events_t::eventUpdateWorld(player, Compendium_t::CPDM_KEYLOCK_UNLOCKED_KEY_IRON, "wall locks", 1);
					}
					else if ( key == KEY_SILVER )
					{
						Compendium_t::Events_t::eventUpdateWorld(player, Compendium_t::CPDM_KEYLOCK_UNLOCKED_KEY_SILVER, "wall locks", 1);
						steamStatisticUpdateClient(player, STEAM_STAT_PREMIUM_LOOTBOX, STEAM_STAT_INT, 1);
					}
					else if ( key == KEY_GOLD )
					{
						Compendium_t::Events_t::eventUpdateWorld(player, Compendium_t::CPDM_KEYLOCK_UNLOCKED_KEY_GOLD, "wall locks", 1);
						steamStatisticUpdateClient(player, STEAM_STAT_PREMIUM_LOOTBOX, STEAM_STAT_INT, 1);
					}
					else if ( key == KEY_BRONZE )
					{
						Compendium_t::Events_t::eventUpdateWorld(player, Compendium_t::CPDM_KEYLOCK_UNLOCKED_KEY_BRONZE, "wall locks", 1);
						steamStatisticUpdateClient(player, STEAM_STAT_PREMIUM_LOOTBOX, STEAM_STAT_INT, 1);
					}
				}

				entity->wallLockState = Entity::WallLockStates::LOCK_KEY_START;
				serverUpdateEntitySkill(entity, 0);
			}
			else if ( entity->wallLockState == Entity::WallLockStates::LOCK_NO_KEY && net_packet->data[9] == 0 )
			{
				messagePlayer(player, MESSAGE_INTERACTION, Language::get(6379));
				playSoundEntity(entity, 152, 64);
			}
			entity->wallLockClientInteractDelay = 0;
			entity->wallLockPlayerInteracting = 0;
		}
	}},

	// rat feed
	{'RATF', [](){
	    const int player =
	    	decodeGameplayPacketPlayerIndex(
	    		net_packet->data[4]
	    	);
	    if ( player < 0 )
	    {
	    	return;
	    }
		client_keepalive[player] = ticks;
		Uint32 uid = SDLNet_Read32(&net_packet->data[5]);
		Entity* entity = uidToEntity(uid);
		if ( entity )
		{
			achievementObserver.playerAchievements[player].rat5000secondRule.insert(uid);
			client_selected[player] = entity;
			inrange[player] = true;
		}
	}},

	// clicked entity out of range
	{'CKOR', [](){
        if ( !net_packet || net_packet->len < 9 )
        {
            printlog("[NET]: ignored truncated CKOR packet");
            return;
        }
        const int player =
            decodeGameplayPacketPlayerIndex(
                net_packet->data[4]
            );
        if ( player < 0 )
        {
            return;
        }
		Uint32 uid = SDLNet_Read32(&net_packet->data[5]);
		Entity* entity = uidToEntity(uid);
		if ( entity )
		{
            client_selected[player] = entity;
            inrange[player] = false;
		}
	}},

	// disconnect
	{'DISC', [](){
		char shortname[32];
		const int playerDisconnected =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( playerDisconnected < 0 )
		{
			return;
		}
	    if (playerDisconnected == 0) {
	        // yeah right
	        return;
	    }
		if (!currentPacketSenderMatchesPlayer(playerDisconnected))
		{
			printlog(
				"[Roster] Rejected disconnect for player %d from another endpoint.",
				playerDisconnected);
			return;
		}
		stringCopy(shortname, stats[playerDisconnected]->name, sizeof(shortname), sizeof(Stat::name));
		client_disconnected[playerDisconnected] = true;
		resetServerLateJoinPlayer(playerDisconnected);
		for ( int c = 1; c < MAXPLAYERS; c++ )
		{
			if ( client_disconnected[c] == true )
			{
				continue;
			}
			memcpy((char*)net_packet->data, "DISC", 4);
			net_packet->data[4] = playerDisconnected;
			net_packet->address.host = net_clients[c - 1].host;
			net_packet->address.port = net_clients[c - 1].port;
			net_packet->len = 5;
			sendPacketSafe(net_sock, -1, net_packet, c - 1);
			messagePlayer(c, MESSAGE_MISC, Language::get(1120), shortname);
		}
		messagePlayer(clientnum, MESSAGE_MISC, Language::get(1120), shortname);
		logServerRosterState("disconnect");
	}},

	// client callout
	{'CALL', []() {
		const int pnum =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( pnum < 0 )
		{
			return;
		}
		if ( pnum != clientnum )
		{
			Uint32 uid = SDLNet_Read32(&net_packet->data[5]);
			Entity* entity = uidToEntity(uid);
			if ( uid != 0 )
			{
				if ( !entity )
				{
					return;
				}
			}
			CalloutMenu[pnum].lockOnEntityUid = uid;
			CalloutRadialMenu::CalloutCommand cmd = (CalloutRadialMenu::CalloutCommand)net_packet->data[9];
			CalloutMenu[pnum].clientCalloutHelpFlags = SDLNet_Read32(&net_packet->data[10]);
			if ( uid == 0 )
			{
				real_t x = SDLNet_Read16(&net_packet->data[14]);
				real_t y = SDLNet_Read16(&net_packet->data[16]);
				if ( CalloutMenu[pnum].createParticleCallout(x * 16.0 + 8.0, y * 16.0 + 8.0, -4, 0, cmd) )
				{
					CalloutMenu[pnum].sendCalloutText(cmd);
				}
			}
			else
			{
				Uint32 overrideUID = 0;
				if ( entity && (entity->behavior == &actPlayer || entity->behavior == &actDeathGhost) && entity->skill[2] != pnum )
				{
					if ( cmd == CalloutRadialMenu::CALLOUT_CMD_AFFIRMATIVE
						|| cmd == CalloutRadialMenu::CALLOUT_CMD_THANKS )
					{
						entity = Player::getPlayerInteractEntity(pnum);
						overrideUID = CalloutMenu[pnum].lockOnEntityUid;
					}
					else if ( cmd == CalloutRadialMenu::CALLOUT_CMD_LOOK
						|| cmd == CalloutRadialMenu::CALLOUT_CMD_NEGATIVE )
					{
						entity = Player::getPlayerInteractEntity(pnum);
					}
					else if ( cmd == CalloutRadialMenu::CALLOUT_CMD_SOUTH
						|| cmd == CalloutRadialMenu::CALLOUT_CMD_SOUTHWEST
						|| cmd == CalloutRadialMenu::CALLOUT_CMD_SOUTHEAST )
					{
						int toPlayer = CalloutMenu[pnum].getPlayerForDirectPlayerCmd(pnum, cmd);
						if ( toPlayer >= 0 )
						{
							entity = Player::getPlayerInteractEntity(pnum);
						}
					}
				}
				if ( CalloutMenu[pnum].createParticleCallout(entity, cmd, overrideUID) )
				{
					CalloutMenu[pnum].sendCalloutText(cmd);
				}
			}
		}
	}},

	// message
	{'MSGS', [](){
		const int pnum =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( pnum < 0 )
		{
			return;
		}
		client_keepalive[pnum] = ticks;
		Uint32 color = SDLNet_Read32(&net_packet->data[5]);
		MessageType type = MESSAGE_CHAT; // the only kind of message you can get from a client.

		char shortname[32];
		stringCopy(shortname, stats[pnum]->name, sizeof(shortname), 22);

		char fmt[1024];
		const int len = snprintf(fmt, sizeof(fmt), "%s: %s", shortname, (char*)(&net_packet->data[9]));
		messagePlayerColor(clientnum, type, color, fmt);

		playSound(Message::CHAT_MESSAGE_SFX, 64);

		// relay message to all clients
		for ( int c = 1; c < MAXPLAYERS; c++ )
		{
			if ( c == pnum || client_disconnected[c] == true || !players[c] || players[c]->isLocalPlayer() )
			{
				continue;
			}
			memcpy((char*)net_packet->data, "MSGS", 4);
			SDLNet_Write32(color, &net_packet->data[4]);
			SDLNet_Write32((Uint32)type, &net_packet->data[8]);
			stringCopy((char*)(&net_packet->data[12]), fmt, len + 1, sizeof(fmt));
			net_packet->address.host = net_clients[c - 1].host;
			net_packet->address.port = net_clients[c - 1].port;
			net_packet->len = 12 + len + 1;
			sendPacketSafe(net_sock, -1, net_packet, c - 1);
		}
	}},

	// spotting (examining)
	{'SPOT', [](){
		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( player < 0 )
		{
			return;
		}
		client_keepalive[player] = ticks;
		Uint32 uid = SDLNet_Read32(&net_packet->data[5]);
		Entity* entity = uidToEntity(uid);
		if ( entity )
		{
			clickDescription(player, entity);
		}
	}},

    // Item drop. Custom items append stable_id at byte 26.
    {'DROP', [](){
        if ( net_packet->len < 26 )
        {
            printlog(
                "[NET]: ignoring malformed DROP packet with length %d.\n",
                net_packet->len
            );
            return;
        }

        const int player =
            decodeGameplayPacketPlayerIndex(net_packet->data[25]);
        if ( player < 0 || !stats[player] )
        {
            return;
        }

        int resolvedType =
            static_cast<int>(SDLNet_Read32(&net_packet->data[4]));
#ifdef SAM_FRAMEWORK_ENABLED
        if ( !resolveSAMItemTypeFromPacket(
            resolvedType,
            26,
            "DROP",
            resolvedType
        ) )
        {
            return;
        }
#endif

        client_keepalive[player] = ticks;
        auto item = newItem(
            static_cast<ItemType>(resolvedType),
            static_cast<Status>(SDLNet_Read32(&net_packet->data[8])),
            SDLNet_Read32(&net_packet->data[12]),
            SDLNet_Read32(&net_packet->data[16]),
            SDLNet_Read32(&net_packet->data[20]),
            net_packet->data[24],
            &stats[player]->inventory
        );
        if ( !item )
        {
            return;
        }
        dropItem(item, player);
    }},

    // Greasy equipment drop. Custom items append stable_id at byte 27.
    { 'GRES', []() {
        if ( net_packet->len < 27 )
        {
            printlog(
                "[NET]: ignoring malformed GRES packet with length %d.\n",
                net_packet->len
            );
            return;
        }

        const int player =
            decodeGameplayPacketPlayerIndex(net_packet->data[25]);
        if ( player < 0 || !stats[player] )
        {
            return;
        }

        int resolvedType =
            static_cast<int>(SDLNet_Read32(&net_packet->data[4]));
#ifdef SAM_FRAMEWORK_ENABLED
        if ( !resolveSAMItemTypeFromPacket(
            resolvedType,
            27,
            "GRES",
            resolvedType
        ) )
        {
            return;
        }
#endif

        client_keepalive[player] = ticks;
        auto item = newItem(
            static_cast<ItemType>(resolvedType),
            static_cast<Status>(SDLNet_Read32(&net_packet->data[8])),
            SDLNet_Read32(&net_packet->data[12]),
            SDLNet_Read32(&net_packet->data[16]),
            SDLNet_Read32(&net_packet->data[20]),
            net_packet->data[24],
            &stats[player]->inventory
        );
        if ( !item )
        {
            return;
        }
        playerGreasyDropItem(player, item);
		if ( net_packet->data[26] == 1 )
		{
			// shield
			if ( stats[player]->shield )
			{
				if ( stats[player]->shield->node )
				{
					list_RemoveNode(stats[player]->shield->node);
				}
				else
				{
					free(stats[player]->shield);
				}
				stats[player]->shield = nullptr;
			}
		}
		else if ( net_packet->data[26] == 0 )
		{
			// weapon
			if ( stats[player]->weapon )
			{
				if ( stats[player]->weapon->node )
				{
					list_RemoveNode(stats[player]->weapon->node);
				}
				else
				{
					free(stats[player]->weapon);
				}
				stats[player]->weapon = nullptr;
			}
		}
	} },

	// duck throw
	{ 'DCKA', []() {
		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[25]
			);
		if ( player < 0 )
		{
			return;
		}
		client_keepalive[player] = ticks;
		auto item = newItem(static_cast<ItemType>(SDLNet_Read32(&net_packet->data[4])),
			static_cast<Status>(SDLNet_Read32(&net_packet->data[8])),
			SDLNet_Read32(&net_packet->data[12]),
			SDLNet_Read32(&net_packet->data[16]),
			SDLNet_Read32(&net_packet->data[20]),
			net_packet->data[24],
			&stats[player]->inventory);
		playerThrowDuck(player, item, net_packet->data[26]);
		// shield
		if ( stats[player]->shield && stats[player]->shield->type == TOOL_DUCK )
		{
			if ( stats[player]->shield->node )
			{
				list_RemoveNode(stats[player]->shield->node);
			}
			else
			{
				free(stats[player]->shield);
			}
			stats[player]->shield = nullptr;
		}
	} },

    // item drop (on death)
    {'DIEI', [](){
        if ( net_packet->len < 28 )
        {
            printlog(
                "[NET]: ignoring malformed DIEI packet with length %d.\n",
                net_packet->len
            );
            return;
        }

        const int player =
            decodeGameplayPacketPlayerIndex(net_packet->data[25]);
        if ( player < 0 || !stats[player] || !stats[0] )
        {
            return;
        }

        int resolvedType =
            static_cast<int>(SDLNet_Read32(&net_packet->data[4]));

#ifdef SAM_FRAMEWORK_ENABLED
        if ( SAMItemRegistryFoundation::isSAMRuntimeItemId(resolvedType) )
        {
            if ( net_packet->len <= 28 )
            {
                printlog(
                    "[S.A.M] Refusing legacy numeric-only DIEI custom item runtime %d.\n",
                    resolvedType
                );
                return;
            }

            const int payloadLength = net_packet->len - 28;
            int stableLength = 0;
            while ( stableLength < payloadLength
                && net_packet->data[28 + stableLength] != '\0' )
            {
                ++stableLength;
            }
            if ( stableLength <= 0 || stableLength >= payloadLength )
            {
                printlog(
                    "[S.A.M] Refusing malformed DIEI stable-id payload.\n"
                );
                return;
            }

            const std::string stableId(
                reinterpret_cast<const char*>(&net_packet->data[28]),
                stableLength
            );
            resolvedType =
                SAMItemRegistryFoundation::runtimeIdForStableId(stableId);
            if ( resolvedType < 0
                || !SAMItemRegistryFoundation::isRegisteredRuntimeItemId(resolvedType) )
            {
                printlog(
                    "[S.A.M] DIEI custom item unavailable on server: [%s]. Drop rejected.\n",
                    stableId.c_str()
                );
                return;
            }
            printlog(
                "[S.A.M] Resolved DIEI custom item [%s] to server runtime %d.\n",
                stableId.c_str(),
                resolvedType
            );
        }
#endif

        auto item = newItem(
            static_cast<ItemType>(resolvedType),
            static_cast<Status>(SDLNet_Read32(&net_packet->data[8])),
            SDLNet_Read32(&net_packet->data[12]),
            SDLNet_Read32(&net_packet->data[16]),
            SDLNet_Read32(&net_packet->data[20]),
            net_packet->data[24],
            nullptr
        );
        if ( !item )
        {
            printlog(
                "[NET]: DIEI failed to construct item type %d.\n",
                resolvedType
            );
            return;
        }

        real_t x = net_packet->data[26];
        x = (x * 16) + 8;
        real_t y = net_packet->data[27];
        y = (y * 16) + 8;

        stats[0]->addItemToLootingBag(player, x, y, *item);
        if ( item->node )
        {
            list_RemoveNode(item->node);
        }
        else
        {
            free(item);
        }
    }},

	// raise/lower shield
	{'SHLD', [](){
		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( player < 0
			|| !players[player]
			|| !stats[player] )
		{
			return;
		}
		stats[player]->defending = net_packet->data[5];
        for (int c = 1; c < MAXPLAYERS; ++c) {
            // relay packet to other players
            if (client_disconnected[c] || c == player) {
                continue;
            }
            net_packet->address.host = net_clients[c - 1].host;
            net_packet->address.port = net_clients[c - 1].port;
            sendPacketSafe(net_sock, -1, net_packet, c - 1);
        }
	}},

	// sneaking
	{'SNEK', [](){
		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( player < 0
			|| !players[player]
			|| !stats[player] )
		{
			return;
		}
		stats[player]->sneaking = net_packet->data[5];
        for (int c = 1; c < MAXPLAYERS; ++c) {
            // relay packet to other players
            if (client_disconnected[c] || c == player) {
                continue;
            }
            net_packet->address.host = net_clients[c - 1].host;
            net_packet->address.port = net_clients[c - 1].port;
            sendPacketSafe(net_sock, -1, net_packet, c - 1);
        }
	}},

	// ghost sneaking
	{ 'GHOD', []() {
		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( player < 0 || !players[player] )
		{
			return;
		}
		if ( players[player]->ghost.my )
		{
			players[player]->ghost.my->skill[3] = net_packet->data[5] & (1 << 0);
			players[player]->ghost.my->skill[11] = net_packet->data[5] & (1 << 1) ? 1 : 0;
			for ( int c = 1; c < MAXPLAYERS; ++c ) {
				// relay packet to other players
				if ( client_disconnected[c] || c == player ) {
					continue;
				}
				net_packet->address.host = net_clients[c - 1].host;
				net_packet->address.port = net_clients[c - 1].port;
				sendPacketSafe(net_sock, -1, net_packet, c - 1);
			}
		}
	}},

	// close shop
	{'SHPC', [](){
		if ( net_packet->len < 9 )
		{
			return;
		}
		const int player =
			decodeGameplayPacketPlayerIndex(net_packet->data[8]);
		if ( player <= 0 )
		{
			return;
		}
		Entity* entity = uidToEntity((Uint32)SDLNet_Read32(&net_packet->data[4]));
		if ( entity )
		{
			entity->skill[0] = 0;
			monsterMoveAside(entity, uidToEntity(entity->skill[1]));
			entity->skill[1] = 0;
		}
		return;
	}},

	// buy item from shop
	{'SHPB', [](){
        if ( net_packet->len < 30 )
        {
            printlog(
                "[NET]: ignoring malformed SHPB packet with length %d.\n",
                net_packet->len
            );
            return;
        }
		Uint32 uidnum = (Uint32)SDLNet_Read32(&net_packet->data[4]);
		const int client =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[29]
			);
		if ( client < 0 )
		{
			return;
		}
		Entity* entity = uidToEntity(uidnum);
		if ( !entity )
		{
			printlog("[Shops]: warning: client %d bought item from non-existent shop! (uid=%d)\n", client, uidnum);
			return;
		}
		Stat* entitystats = entity->getStats();
		if ( !entitystats )
		{
			printlog("[Shops]: warning: client %d bought item from a \"shop\" that has no stats! (uid=%d)\n", client, uidnum);
			return;
		}
        int resolvedType = static_cast<int>(SDLNet_Read32(&net_packet->data[8]));
#ifdef SAM_FRAMEWORK_ENABLED
        if ( !resolveSAMItemTypeFromPacket(
            resolvedType,
            30,
            "SHPB",
            resolvedType
        ) )
        {
            return;
        }
#endif
        Item* item = newItem(WOODEN_SHIELD, BROKEN, 0, 1, 0, true, nullptr);
        item->type = static_cast<ItemType>(resolvedType);
		item->status = static_cast<Status>(SDLNet_Read32(&net_packet->data[12]));
		item->beatitude = SDLNet_Read16(&net_packet->data[16]);
		item->appearance = SDLNet_Read32(&net_packet->data[20]);
		item->count = SDLNet_Read32(&net_packet->data[24]);
		item->identified = false;
		if ( net_packet->data[28] & 1 )
		{
			item->identified = true;
		}
		item->playerSoldItemToShop = false;
		if ( (net_packet->data[28] >> 4) & 1 )
		{
			item->playerSoldItemToShop = true;
		}
		item->x = (Sint8)net_packet->data[18];
		item->y = (Sint8)net_packet->data[19];
		node_t* nextnode;
		for ( auto node = entitystats->inventory.first; node != NULL; node = nextnode )
		{
			nextnode = node->next;
			Item* item2 = (Item*)node->element;
			if ( !item2 )
			{
				continue;
			}
			if ( item2->playerSoldItemToShop != item->playerSoldItemToShop )
			{
				continue;
			}
			if ( item2->x != item->x || item2->y != item->y )
			{
				continue;
			}
			if (!itemCompare(item, item2, false, false))
			{
				printlog("[Shops]: client %d bought item from shop (uid=%d)\n", client, uidnum);
				if ( shopIsMysteriousShopkeeper(entity) )
				{
					buyItemFromMysteriousShopkeepConsumeOrb(client, *entity, *item2);
				}
				if ( itemTypeIsQuiver(item2->type) )
				{
					item2->count = 1; // so we consume it all up
				}
				consumeItem(item2, client);
				break;
			}
		}

		Sint32 buyValue = item->buyValue(client);
		entitystats->GOLD += buyValue;
		stats[client]->GOLD -= buyValue;
		stats[client]->GOLD = std::max(0, stats[client]->GOLD);
		if ( players[client] && players[client]->entity && !item->playerSoldItemToShop )
		{
			bool increaseSkill = false;
			if ( buyValue >= 100 )
			{
				increaseSkill = true;
			}
			else
			{
				if ( local_rng.rand() % 100 <= (std::max(10, buyValue)) ) // 20% to 100% from 1-100 gold
				{
					increaseSkill = true;
				}
			}

			if ( increaseSkill && entity )
			{
				if ( !strcmp(map.name, "Mages Guild") )
				{
					int increases = hamletShopkeeperSkillLimit[client][entity->getUID()];
					if ( increases >= hamletTradingSkillLimit )
					{
						increaseSkill = false;
						if ( local_rng.rand() % 2 )
						{
							messagePlayer(client, MESSAGE_HINT | MESSAGE_INTERACTION, Language::get(6868));
						}
					}
				}
			}

			if ( increaseSkill )
			{
				if ( buyValue <= 1 )
				{
					if ( stats[client]->getProficiency(PRO_TRADING) < SKILL_LEVEL_SKILLED )
					{
						players[client]->entity->increaseSkill(PRO_TRADING);
						if ( entity )
						{
							if ( !strcmp(map.name, "Mages Guild") )
							{
								hamletShopkeeperSkillLimit[client][entity->getUID()]++;
							}
						}
					}
				}
				else
				{
					players[client]->entity->increaseSkill(PRO_TRADING);
					if ( entity )
					{
						if ( !strcmp(map.name, "Mages Guild") )
						{
							hamletShopkeeperSkillLimit[client][entity->getUID()]++;
						}
					}
				}
			}
			//if ( local_rng.rand() % 2 )
			//{
			//	if ( item->buyValue(client) <= 1 )
			//	{
			//		// buying cheap items does not increase trading past basic
			//		if ( stats[client]->PROFICIENCIES[PRO_TRADING] < SKILL_LEVEL_SKILLED )
			//		{
			//			players[client]->entity->increaseSkill(PRO_TRADING);
			//		}
			//	}
			//	else
			//	{
			//		players[client]->entity->increaseSkill(PRO_TRADING);
			//	}
			//}
			//else if ( buyValue >= 150 )
			//{
			//	if ( buyValue >= 300 || local_rng.rand() % 2 )
			//	{
			//		players[client]->entity->increaseSkill(PRO_TRADING);
			//	}
			//}
		}
		free(item);
	}},

	//Remove a spell from the channeled spells list.
	{'UNCH', [](){
		const int client =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( client < 0 )
		{
			return;
		}
		spell_t* thespell = getSpellFromID(SDLNet_Read32(&net_packet->data[5]));
		if (spellInList(&channeledSpells[client], thespell))
		{
			node_t *node, *nextnode;
			for (node = channeledSpells[client].first; node; node = nextnode )
			{
				nextnode = node->next;
				spell_t* spell_search = (spell_t*)node->element;
				if (spell_search->ID == thespell->ID)
				{
					spell_search->sustain = false;
				}
			}
		}
	}},

	// sell item to shop
	{'SHPS', [](){
        if ( net_packet->len < 30 )
        {
            printlog(
                "[NET]: ignoring malformed SHPS packet with length %d.\n",
                net_packet->len
            );
            return;
        }
		Uint32 uidnum = (Uint32)SDLNet_Read32(&net_packet->data[4]);
		const int client =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[29]
			);
		if ( client < 0 )
		{
			return;
		}
		Entity* entity = uidToEntity(uidnum);
		if ( !entity )
		{
			printlog("[Shops]: warning: client %d sold item to non-existent shop! (uid=%d)\n", client, uidnum);
			return;
		}
		Stat* entitystats = entity->getStats();
		if ( !entitystats )
		{
			printlog("[Shops]: warning: client %d sold item to a \"shop\" that has no stats! (uid=%d)\n", client, uidnum);
			return;
		}

        int resolvedType = static_cast<int>(SDLNet_Read32(&net_packet->data[8]));
#ifdef SAM_FRAMEWORK_ENABLED
        if ( !resolveSAMItemTypeFromPacket(
            resolvedType,
            30,
            "SHPS",
            resolvedType
        ) )
        {
            return;
        }
#endif
        bool identified = net_packet->data[28] == 1;
        auto item = newItem(
            static_cast<ItemType>(resolvedType),
		    static_cast<Status>(SDLNet_Read32(&net_packet->data[12])),
			SDLNet_Read16(&net_packet->data[16]),
			SDLNet_Read32(&net_packet->data[24]),
			SDLNet_Read32(&net_packet->data[20]),
			identified, nullptr);

		if ( !item )
		{
			printlog("[Shops]: client %d sold item to shop (uid=%d) but could not create item!\n", client, uidnum);
			return;
		}

		Sint32 goldValue = item->sellValue(client);
		int xout = Player::ShopGUI_t::MAX_SHOP_X;
		int yout = Player::ShopGUI_t::MAX_SHOP_Y;
		Item* itemToStackInto = nullptr;
		getShopFreeSlot(-1, &entitystats->inventory, item, xout, yout, itemToStackInto);
		if ( itemToStackInto )
		{
			itemToStackInto->count += item->count;
			itemToStackInto->playerSoldItemToShop = true;
			free(item);
			item = nullptr;
			printlog("[Shops]: client %d sold item to shop (uid=%d), added to existing item x: %d y: %d\n", client, uidnum, itemToStackInto->x, itemToStackInto->y);
		}
		else
		{
			Item* item2 = newItem(item->type, item->status, item->beatitude, item->count, item->appearance, item->identified, &entitystats->inventory);
			item2->x = xout;
			item2->y = yout;
			item2->playerSoldItemToShop = true;
			free(item);
			item = nullptr;
			printlog("[Shops]: client %d sold item to shop (uid=%d), new item stack x: %d y: %d\n", client, uidnum, item2->x, item2->y);
		}

		stats[client]->GOLD += goldValue;
		entitystats->GOLD -= goldValue;
		//if ( players[client] && players[client]->entity )
		//{
		//	if ( local_rng.rand() % 2 )
		//	{
		//		if ( goldValue <= 1 )
		//		{
		//			// selling cheap items does not increase trading past basic
		//			if ( stats[client]->PROFICIENCIES[PRO_TRADING] < SKILL_LEVEL_SKILLED )
		//			{
		//				players[client]->entity->increaseSkill(PRO_TRADING);
		//			}
		//		}
		//		else
		//		{
		//			players[client]->entity->increaseSkill(PRO_TRADING);
		//		}
		//	}
		//}
	}},

    // Use item. Custom items append stable_id at byte 26.
    {'USEI', [](){
        if ( net_packet->len < 26 )
        {
            printlog("[NET]: refusing malformed USEI packet.\n");
            return;
        }

        const int client =
            decodeGameplayPacketPlayerIndex(
                net_packet->data[25]
            );
        if ( client < 0 )
        {
            return;
        }

        const int transmittedType = static_cast<int>(
            SDLNet_Read32(&net_packet->data[4])
        );
        int resolvedType = transmittedType;
#ifdef SAM_FRAMEWORK_ENABLED
        if ( !resolveSAMItemTypeFromPacket(
            transmittedType,
            26,
            "USEI",
            resolvedType
        ) )
        {
            return;
        }
#endif

        auto item = newItem(
            static_cast<ItemType>(resolvedType),
            static_cast<Status>(SDLNet_Read32(&net_packet->data[8])),
            SDLNet_Read32(&net_packet->data[12]),
            SDLNet_Read32(&net_packet->data[16]),
            SDLNet_Read32(&net_packet->data[20]),
            net_packet->data[24],
            &stats[client]->inventory);
        if ( !item )
        {
            printlog("[NET]: USEI failed to construct item type %d.\n", resolvedType);
            return;
        }
        useItem(item, client, nullptr, false, true);
    }},

	// use loot bag
	{ 'LOOT', []() {
		const int client =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[8]
			);
		if ( client < 0 )
		{
			return;
		}
		Uint32 appearance = SDLNet_Read32(&net_packet->data[4]);
		Stat::emptyLootingBag(client, appearance);
	} },

    // Equip item as a weapon. Custom items append stable_id at byte 28.
    {'EQUI', [](){
        if ( net_packet->len < 28 )
        {
            printlog(
                "[NET]: ignoring malformed EQUI packet with length %d.\n",
                net_packet->len
            );
            return;
        }

        const int client =
            decodeGameplayPacketPlayerIndex(net_packet->data[25]);
        if ( client < 0 || !stats[client] )
        {
            return;
        }

        int resolvedType =
            static_cast<int>(SDLNet_Read32(&net_packet->data[4]));
#ifdef SAM_FRAMEWORK_ENABLED
        if ( !resolveSAMItemTypeFromPacket(
            resolvedType,
            28,
            "EQUI",
            resolvedType
        ) )
        {
            return;
        }
#endif

        auto item = newItem(
            static_cast<ItemType>(resolvedType),
		    static_cast<Status>(SDLNet_Read32(&net_packet->data[8])),
		    SDLNet_Read32(&net_packet->data[12]),
		    SDLNet_Read32(&net_packet->data[16]),
		    SDLNet_Read32(&net_packet->data[20]),
		    net_packet->data[24],
		    &stats[client]->inventory);
		EquipItemResult res = equipItem(item, &stats[client]->weapon, client, false);
		if ( res == EQUIP_ITEM_SUCCESS_UPDATE_QTY
			|| res == EQUIP_ITEM_FAIL_CANT_UNEQUIP )
		{
			if ( item )
			{
				if ( item->node )
				{
					list_RemoveNode(item->node);
				}
				else
				{
					free(item);
				}
			}
		}
	}},

    // Equip item as a shield. Custom items append stable_id at byte 28.
    {'EQUS', [](){
        if ( net_packet->len < 28 )
        {
            printlog(
                "[NET]: ignoring malformed EQUS packet with length %d.\n",
                net_packet->len
            );
            return;
        }

        const int client =
            decodeGameplayPacketPlayerIndex(net_packet->data[25]);
        if ( client < 0 || !stats[client] )
        {
            return;
        }

        int resolvedType =
            static_cast<int>(SDLNet_Read32(&net_packet->data[4]));
#ifdef SAM_FRAMEWORK_ENABLED
        if ( !resolveSAMItemTypeFromPacket(
            resolvedType,
            28,
            "EQUS",
            resolvedType
        ) )
        {
            return;
        }
#endif

        auto item = newItem(
            static_cast<ItemType>(resolvedType),
		    static_cast<Status>(SDLNet_Read32(&net_packet->data[8])),
		    SDLNet_Read32(&net_packet->data[12]),
		    SDLNet_Read32(&net_packet->data[16]),
		    SDLNet_Read32(&net_packet->data[20]),
		    net_packet->data[24],
		    &stats[client]->inventory);
		EquipItemResult res = equipItem(item, &stats[client]->shield, client, false);
		if ( res == EQUIP_ITEM_SUCCESS_UPDATE_QTY
			|| res == EQUIP_ITEM_FAIL_CANT_UNEQUIP )
		{
			if ( item )
			{
				if ( item->node )
				{
					list_RemoveNode(item->node);
				}
				else
				{
					free(item);
				}
			}
		}
	}},

	// consume torch item shield slot
	{ 'COOK', []() {
		const int client =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[25]
			);
		if ( client < 0 )
		{
			return;
		}
		auto item = newItem(
			static_cast<ItemType>(SDLNet_Read32(&net_packet->data[4])),
			static_cast<Status>(SDLNet_Read32(&net_packet->data[8])),
			SDLNet_Read32(&net_packet->data[12]),
			SDLNet_Read32(&net_packet->data[16]),
			SDLNet_Read32(&net_packet->data[20]),
			net_packet->data[24],
			&stats[client]->inventory);
		if ( stats[client]->shield )
		{
			// deselect shield
			if ( stats[client]->shield->node )
			{
				list_RemoveNode(stats[client]->shield->node);
			}
			else
			{
				free(stats[client]->shield);
			}
			stats[client]->shield = nullptr;
		}
		if ( item->count > 0 )
		{
			bool oldIntro = intro;
			intro = true;
			equipItem(item, &stats[client]->shield, client, false);
			intro = oldIntro;
		}
		else
		{
			if ( item->node )
			{
				list_RemoveNode(item->node);
			}
			else
			{
				free(item);
			}
		}
	} },

    // Equip item in another slot. Custom items append stable_id at byte 28.
    {'EQUM', [](){
        if ( net_packet->len < 28 )
        {
            printlog(
                "[NET]: ignoring malformed EQUM packet with length %d.\n",
                net_packet->len
            );
            return;
        }

        const int client =
            decodeGameplayPacketPlayerIndex(net_packet->data[25]);
        if ( client < 0 || !stats[client] )
        {
            return;
        }

        int resolvedType =
            static_cast<int>(SDLNet_Read32(&net_packet->data[4]));
#ifdef SAM_FRAMEWORK_ENABLED
        if ( !resolveSAMItemTypeFromPacket(
            resolvedType,
            28,
            "EQUM",
            resolvedType
        ) )
        {
            return;
        }
#endif

        auto item = newItem(
            static_cast<ItemType>(resolvedType),
		    static_cast<Status>(SDLNet_Read32(&net_packet->data[8])),
		    SDLNet_Read32(&net_packet->data[12]),
		    SDLNet_Read32(&net_packet->data[16]),
		    SDLNet_Read32(&net_packet->data[20]),
		    net_packet->data[24],
		    &stats[client]->inventory);
		
		int res = -1;
		switch ( net_packet->data[27] )
		{
			case EQUIP_ITEM_SLOT_WEAPON:
				res = equipItem(item, &stats[client]->weapon, client, false);
				break;
			case EQUIP_ITEM_SLOT_SHIELD:
				res = equipItem(item, &stats[client]->shield, client, false);
				break;
			case EQUIP_ITEM_SLOT_MASK:
				res = equipItem(item, &stats[client]->mask, client, false);
				break;
			case EQUIP_ITEM_SLOT_HELM:
				res = equipItem(item, &stats[client]->helmet, client, false);
				break;
			case EQUIP_ITEM_SLOT_GLOVES:
				res = equipItem(item, &stats[client]->gloves, client, false);
				break;
			case EQUIP_ITEM_SLOT_BOOTS:
				res = equipItem(item, &stats[client]->shoes, client, false);
				break;
			case EQUIP_ITEM_SLOT_BREASTPLATE:
				res = equipItem(item, &stats[client]->breastplate, client, false);
				break;
			case EQUIP_ITEM_SLOT_CLOAK:
				res = equipItem(item, &stats[client]->cloak, client, false);
				break;
			case EQUIP_ITEM_SLOT_AMULET:
				res = equipItem(item, &stats[client]->amulet, client, false);
				break;
			case EQUIP_ITEM_SLOT_RING:
				res = equipItem(item, &stats[client]->ring, client, false);
				break;
			default:
				break;
		}

		if ( res == EQUIP_ITEM_SUCCESS_UPDATE_QTY
			|| res == EQUIP_ITEM_FAIL_CANT_UNEQUIP )
		{
			if ( item )
			{
				if ( item->node )
				{
					list_RemoveNode(item->node);
				}
				else
				{
					free(item);
				}
			}
		}
	}},

	// update appearance of item
	{ 'EQUA', []() {
		const int client =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[25]
			);
		if ( client < 0 )
		{
			return;
		}
        if ( net_packet->len < 28 )
        {
            printlog("[NET]: refusing short EQUA packet.\n");
            return;
        }
        int resolvedItemType =
            static_cast<int>(SDLNet_Read32(&net_packet->data[4]));
#ifdef SAM_FRAMEWORK_ENABLED
        if ( !resolveSAMItemTypeFromPacket(
            resolvedItemType,
            28,
            "EQUA",
            resolvedItemType
        ) )
        {
            return;
        }
#endif
        auto item = newItem(
            static_cast<ItemType>(resolvedItemType),
			static_cast<Status>(SDLNet_Read32(&net_packet->data[8])),
			SDLNet_Read32(&net_packet->data[12]),
			SDLNet_Read32(&net_packet->data[16]),
			SDLNet_Read32(&net_packet->data[20]),
			net_packet->data[24],
			nullptr);

		const bool onIdentify = net_packet->data[27];
		Item* slot = nullptr;

		switch ( net_packet->data[26] )
		{
			case ItemEquippableSlot::EQUIPPABLE_IN_SLOT_WEAPON:
				slot = stats[client]->weapon;
				break;
			case ItemEquippableSlot::EQUIPPABLE_IN_SLOT_SHIELD:
				slot = stats[client]->shield;
				break;
			case ItemEquippableSlot::EQUIPPABLE_IN_SLOT_MASK:
				slot = stats[client]->mask;
				break;
			case ItemEquippableSlot::EQUIPPABLE_IN_SLOT_HELM:
				slot = stats[client]->helmet;
				break;
			case ItemEquippableSlot::EQUIPPABLE_IN_SLOT_GLOVES:
				slot = stats[client]->gloves;
				break;
			case ItemEquippableSlot::EQUIPPABLE_IN_SLOT_BOOTS:
				slot = stats[client]->shoes;
				break;
			case ItemEquippableSlot::EQUIPPABLE_IN_SLOT_BREASTPLATE:
				slot = stats[client]->breastplate;
				break;
			case ItemEquippableSlot::EQUIPPABLE_IN_SLOT_CLOAK:
				slot = stats[client]->cloak;
				break;
			case ItemEquippableSlot::EQUIPPABLE_IN_SLOT_AMULET:
				slot = stats[client]->amulet;
				break;
			case ItemEquippableSlot::EQUIPPABLE_IN_SLOT_RING:
				slot = stats[client]->ring;
				break;
			default:
				break;
		}

		if ( slot )
		{
			if ( onIdentify )
			{
				item->identified = slot->identified;
			}
			Uint32 newAppearance = item->appearance;
			item->appearance = slot->appearance;
			if ( !itemCompare(item, slot, false, false) )
			{
				slot->appearance = newAppearance;
				if ( onIdentify )
				{
					slot->identified = true;
				}
			}
		}

		free(item);
		item = nullptr;
	} },

	// update itemType of item
    { 'EQUT', []() {
        if ( net_packet->len < 31 )
        {
            printlog("[NET]: refusing short EQUT packet.\n");
            return;
        }
        const int client =
            decodeGameplayPacketPlayerIndex(
                net_packet->data[25]
            );
        if ( client < 0 )
        {
            return;
        }

        int previousType =
            static_cast<int>(SDLNet_Read32(&net_packet->data[4]));
        int newType =
            static_cast<int>(SDLNet_Read32(&net_packet->data[27]));
#ifdef SAM_FRAMEWORK_ENABLED
        if ( net_packet->len > 31 )
        {
            if ( net_packet->len < 37 )
            {
                printlog("[S.A.M] Refusing malformed EQUT payload.\n");
                return;
            }
            const bool oldIsCustom = net_packet->data[31] != 0;
            const bool newIsCustom = net_packet->data[32] != 0;
            const int oldLength =
                static_cast<int>(SDLNet_Read16(&net_packet->data[33]));
            int offset = 35;
            if ( oldLength < 0
                || offset + oldLength + 2 > net_packet->len )
            {
                printlog("[S.A.M] Refusing malformed EQUT old stable id.\n");
                return;
            }
            std::string oldStableId;
            if ( oldLength > 0 )
            {
                oldStableId.assign(
                    reinterpret_cast<const char*>(&net_packet->data[offset]),
                    oldLength
                );
            }
            offset += oldLength;
            const int newLength =
                static_cast<int>(SDLNet_Read16(&net_packet->data[offset]));
            offset += 2;
            if ( newLength < 0
                || offset + newLength != net_packet->len )
            {
                printlog("[S.A.M] Refusing malformed EQUT new stable id.\n");
                return;
            }
            std::string newStableId;
            if ( newLength > 0 )
            {
                newStableId.assign(
                    reinterpret_cast<const char*>(&net_packet->data[offset]),
                    newLength
                );
            }
            if ( oldIsCustom )
            {
                previousType =
                    SAMItemRegistryFoundation::runtimeIdForStableId(
                        oldStableId
                    );
                if ( previousType < 0
                    || !SAMItemRegistryFoundation::
                        isRegisteredRuntimeItemId(previousType) )
                {
                    printlog(
                        "[S.A.M] EQUT old item unavailable locally: [%s].\n",
                        oldStableId.c_str()
                    );
                    return;
                }
            }
            else if ( SAMItemRegistryFoundation::
                isSAMRuntimeItemId(previousType) )
            {
                printlog(
                    "[S.A.M] Refusing numeric-only EQUT old runtime %d.\n",
                    previousType
                );
                return;
            }
            if ( newIsCustom )
            {
                newType =
                    SAMItemRegistryFoundation::runtimeIdForStableId(
                        newStableId
                    );
                if ( newType < 0
                    || !SAMItemRegistryFoundation::
                        isRegisteredRuntimeItemId(newType) )
                {
                    printlog(
                        "[S.A.M] EQUT new item unavailable locally: [%s].\n",
                        newStableId.c_str()
                    );
                    return;
                }
            }
            else if ( SAMItemRegistryFoundation::
                isSAMRuntimeItemId(newType) )
            {
                printlog(
                    "[S.A.M] Refusing numeric-only EQUT new runtime %d.\n",
                    newType
                );
                return;
            }
        }
        else if ( SAMItemRegistryFoundation::isSAMRuntimeItemId(previousType)
            || SAMItemRegistryFoundation::isSAMRuntimeItemId(newType) )
        {
            printlog("[S.A.M] Refusing numeric-only EQUT custom item.\n");
            return;
        }
#endif

        auto item = newItem(
            static_cast<ItemType>(previousType),
            static_cast<Status>(SDLNet_Read32(&net_packet->data[8])),
            SDLNet_Read32(&net_packet->data[12]),
            SDLNet_Read32(&net_packet->data[16]),
            SDLNet_Read32(&net_packet->data[20]),
            net_packet->data[24],
            nullptr);

        Item* slot = nullptr;
        switch ( net_packet->data[26] )
        {
            case ItemEquippableSlot::EQUIPPABLE_IN_SLOT_WEAPON:
                slot = stats[client]->weapon;
                break;
            case ItemEquippableSlot::EQUIPPABLE_IN_SLOT_SHIELD:
                slot = stats[client]->shield;
                break;
            case ItemEquippableSlot::EQUIPPABLE_IN_SLOT_MASK:
                slot = stats[client]->mask;
                break;
            case ItemEquippableSlot::EQUIPPABLE_IN_SLOT_HELM:
                slot = stats[client]->helmet;
                break;
            case ItemEquippableSlot::EQUIPPABLE_IN_SLOT_GLOVES:
                slot = stats[client]->gloves;
                break;
            case ItemEquippableSlot::EQUIPPABLE_IN_SLOT_BOOTS:
                slot = stats[client]->shoes;
                break;
            case ItemEquippableSlot::EQUIPPABLE_IN_SLOT_BREASTPLATE:
                slot = stats[client]->breastplate;
                break;
            case ItemEquippableSlot::EQUIPPABLE_IN_SLOT_CLOAK:
                slot = stats[client]->cloak;
                break;
            case ItemEquippableSlot::EQUIPPABLE_IN_SLOT_AMULET:
                slot = stats[client]->amulet;
                break;
            case ItemEquippableSlot::EQUIPPABLE_IN_SLOT_RING:
                slot = stats[client]->ring;
                break;
            default:
                break;
        }

        if ( slot && !itemCompare(item, slot, false, false) )
        {
            slot->type = static_cast<ItemType>(newType);
            slot->appearance = item->appearance;
        }

        free(item);
    } },

	// apply item to entity
	{'APIT', [](){
		const int client =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[25]
			);
		if ( client < 0 )
		{
			return;
		}
        if ( net_packet->len < 30 )
        {
            printlog("[NET]: refusing short APIT packet.\n");
            return;
        }
        int resolvedItemType =
            static_cast<int>(SDLNet_Read32(&net_packet->data[4]));
#ifdef SAM_FRAMEWORK_ENABLED
        if ( !resolveSAMItemTypeFromPacket(
            resolvedItemType,
            30,
            "APIT",
            resolvedItemType
        ) )
        {
            return;
        }
#endif
        auto item = newItem(
            static_cast<ItemType>(resolvedItemType),
		    static_cast<Status>(SDLNet_Read32(&net_packet->data[8])),
		    SDLNet_Read32(&net_packet->data[12]),
		    SDLNet_Read32(&net_packet->data[16]),
		    SDLNet_Read32(&net_packet->data[20]),
		    net_packet->data[24],
		    NULL);
		Entity* entity = uidToEntity(SDLNet_Read32(&net_packet->data[26]));
		if ( entity )
		{
			item->apply(client, entity);
		}
		else
		{
			printlog("warning: client applied item to entity that does not exist\n");
		}
		free(item);
	}},

	// apply item to entity
	{'APIW', [](){
		const int client =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[25]
			);
		if ( client < 0 )
		{
			return;
		}
        if ( net_packet->len < 30 )
        {
            printlog("[NET]: refusing short APIW packet.\n");
            return;
        }
        int resolvedItemType =
            static_cast<int>(SDLNet_Read32(&net_packet->data[4]));
#ifdef SAM_FRAMEWORK_ENABLED
        if ( !resolveSAMItemTypeFromPacket(
            resolvedItemType,
            30,
            "APIW",
            resolvedItemType
        ) )
        {
            return;
        }
#endif
        auto item = newItem(
            static_cast<ItemType>(resolvedItemType),
		    static_cast<Status>(SDLNet_Read32(&net_packet->data[8])),
		    SDLNet_Read32(&net_packet->data[12]),
		    SDLNet_Read32(&net_packet->data[16]),
		    SDLNet_Read32(&net_packet->data[20]),
		    net_packet->data[24],
		    NULL);
		int wallx = (SDLNet_Read16(&net_packet->data[26]));
		int wally = (SDLNet_Read16(&net_packet->data[28]));
		item->applyLockpickToWall(client, wallx, wally);
		free(item);
	}},

	// attacking
	{'ATAK', [](){
	    const int player =
	    	decodeGameplayPacketPlayerIndex(
	    		net_packet->data[4]
	    	);
	    if ( player < 0 )
	    {
	    	return;
	    }
		if (players[player] && players[player]->entity)
		{
			ItemType type = static_cast<ItemType>(SDLNet_Read32(&net_packet->data[7]));
			Uint32 appearance = static_cast<ItemType>(SDLNet_Read32(&net_packet->data[11]));
			if ( stats[player]->weapon && stats[player]->weapon->type == type )
			{
				if ( type == MAGICSTAFF_SCEPTER )
				{
					stats[player]->weapon->appearance = appearance;
				}
			}
			if ( type == TOOL_DUCK )
			{
				if ( stats[player]->weapon && stats[player]->weapon->type != TOOL_DUCK )
				{
					return;
				}
			}
			if ( type == GEM_JEWEL )
			{
				if ( stats[player]->weapon && stats[player]->weapon->type != GEM_JEWEL )
				{
					return;
				}
			}
			int pose = net_packet->data[5];
			if ( pose == PLAYER_POSE_GOLEM_SMASH )
			{
				Item* tmp = stats[player]->weapon;
				stats[player]->weapon = nullptr;
				players[player]->entity->attack(pose, net_packet->data[6], nullptr);
				stats[player]->weapon = tmp;
			}
			else
			{
				players[player]->entity->attack(pose, net_packet->data[6], nullptr);
			}
		}
	}},

	//Multiplayer chest code (server).
	{'CCLS', [](){
	    const int player =
	    	decodeGameplayPacketPlayerIndex(
	    		net_packet->data[4]
	    	);
	    if ( player < 0 )
	    {
	    	return;
	    }
		if (openedChest[player])
		{
			openedChest[player]->closeChestServer();
		}
	}},

	//Multiplayer duck code (server).
	{ 'DUCK', []() {
		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( player < 0 )
		{
			return;
		}
		const int duck = net_packet->data[5];
		players[player]->mechanics.pendingDucks.push_back(
			std::make_pair(duck, ticks + (3 + (local_rng.rand() % 30)) * TICKS_PER_SECOND));
	} },

	//The client failed some alchemy.
	{'BOOM', [](){
	    const int player =
	    	decodeGameplayPacketPlayerIndex(
	    		net_packet->data[4]
	    	);
	    if ( player < 0 )
	    {
	    	return;
	    }
		if ( players[player] && players[player]->entity )
		{
			bool protection = false;
			if ( stats[player]->mask && stats[player]->mask->type == MASK_HAZARD_GOGGLES )
			{
				bool shapeshifted = false;
				if ( stats[player]->type != HUMAN )
				{
					if ( players[player]->entity->effectShapeshift != NOTHING )
					{
						shapeshifted = true;
					}
				}
				if ( !shapeshifted )
				{
					protection = true;
					messagePlayerColor(player, MESSAGE_STATUS, makeColorRGB(0, 255, 0), Language::get(6089));
				}
			}
			spawnMagicTower(protection ? players[player]->entity : nullptr, 
				players[player]->entity->x, players[player]->entity->y, SPELL_FIREBALL, nullptr);
			players[player]->entity->setObituary(Language::get(3350));
			stats[player]->killer = KilledBy::FAILED_ALCHEMY;
		}
	}},

	//The client cast a spell.
	{'SPEL', [](){
	    const int player =
	    	decodeGameplayPacketPlayerIndex(
	    		net_packet->data[4]
	    	);
	    if ( player < 0 )
	    {
	    	return;
	    }

		spell_t* thespell = getSpellFromID(SDLNet_Read32(&net_packet->data[5]));
		if ( players[player] && players[player]->entity )
		{
			bool spellbookCast = net_packet->data[9] == 1;
			if ( net_packet->len > 10 )
			{
				CastSpellProps_t castSpellProps;
				castSpellProps.caster_x = (SDLNet_Read32(&net_packet->data[10]) / 256.0);
				castSpellProps.caster_y = (SDLNet_Read32(&net_packet->data[14]) / 256.0);
				castSpellProps.target_x = (SDLNet_Read32(&net_packet->data[18]) / 256.0);
				castSpellProps.target_y = (SDLNet_Read32(&net_packet->data[22]) / 256.0);
				castSpellProps.targetUID = (SDLNet_Read32(&net_packet->data[26]));
				castSpellProps.wallDir = net_packet->data[30];
				castSpellProps.optionalData = net_packet->data[31];
				castSpellProps.overcharge = net_packet->data[32];
				castSpell(players[player]->entity->getUID(), thespell, false, false, spellbookCast, &castSpellProps);
			}
			else
			{
				castSpell(players[player]->entity->getUID(), thespell, false, false, spellbookCast);
			}
		}
	}},

	//The client ghost cast a spell.
	{'GHSP', []() {
		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( player < 0 )
		{
			return;
		}

		spell_t* thespell = getSpellFromID(SDLNet_Read32(&net_packet->data[5]));
		if ( players[player] && players[player]->ghost.isActive() )
		{
			castSpell(players[player]->ghost.my->getUID(), thespell, false, true);
		}
	}},

	//The client added an item to the chest.
	{'CITM', [](){
        if ( net_packet->len < 28 )
        {
            printlog(
                "[NET]: ignoring malformed client CITM packet with length %d.\n",
                net_packet->len
            );
            return;
        }

	    const int player =
	    	decodeGameplayPacketPlayerIndex(
	    		net_packet->data[4]
	    	);
	    if ( player < 0 )
	    {
	    	return;
	    }
		if ( net_packet->data[27] == 0 && !openedChest[player])
		{
			return;
		}

        int resolvedType =
            static_cast<int>(SDLNet_Read32(&net_packet->data[5]));
#ifdef SAM_FRAMEWORK_ENABLED
        if ( !resolveSAMItemTypeFromPacket(
            resolvedType,
            28,
            "CITM",
            resolvedType
        ) )
        {
            return;
        }
#endif
        Item* newitem = newItem(
            static_cast<ItemType>(resolvedType),
            BROKEN,
            0,
            1,
            0,
            true,
            nullptr
        );
		newitem->status = static_cast<Status>(SDLNet_Read32(&net_packet->data[9]));
		newitem->beatitude = SDLNet_Read32(&net_packet->data[13]);
		newitem->count = SDLNet_Read32(&net_packet->data[17]);
		newitem->appearance = SDLNet_Read32(&net_packet->data[21]);
		newitem->identified = net_packet->data[25];
		bool forceNewStack = net_packet->data[26] ? true : false;
		if ( net_packet->data[27] == 0 )
		{
			Item* chestItem = openedChest[player]->addItemToChestServer(newitem, forceNewStack, nullptr);
			if ( chestItem != newitem )
			{
				free(newitem);
			}
		}
		else
		{
			Item* chestItem = Entity::addItemToVoidChestServer(player, newitem, forceNewStack, nullptr);
			if ( chestItem != newitem )
			{
				free(newitem);
			}
		}
	}},

	//The client removed an item from the chest.
	{'RCIT', [](){
        if ( net_packet->len < 28 )
        {
            printlog(
                "[NET]: ignoring malformed RCIT packet with length %d.\n",
                net_packet->len
            );
            return;
        }

	    const int player =
	    	decodeGameplayPacketPlayerIndex(
	    		net_packet->data[4]
	    	);
	    if ( player < 0 )
	    {
	    	return;
	    }
		if ( net_packet->data[27] == 0 && !openedChest[player])
		{
			return;
		}

        int resolvedType =
            static_cast<int>(SDLNet_Read32(&net_packet->data[5]));
#ifdef SAM_FRAMEWORK_ENABLED
        if ( !resolveSAMItemTypeFromPacket(
            resolvedType,
            28,
            "RCIT",
            resolvedType
        ) )
        {
            return;
        }
#endif
        Item* item = newItem(
            static_cast<ItemType>(resolvedType),
            BROKEN,
            0,
            1,
            0,
            true,
            nullptr
        );
		item->status = static_cast<Status>(SDLNet_Read32(&net_packet->data[9]));
		item->beatitude = SDLNet_Read32(&net_packet->data[13]);
		item->count = SDLNet_Read32(&net_packet->data[17]);
		item->appearance = SDLNet_Read32(&net_packet->data[21]);
		item->identified = net_packet->data[25];

		if ( net_packet->data[27] == 0 )
		{
			openedChest[player]->removeItemFromChestServer(item, item->count);
		}
		else
		{
			Entity::removeItemFromVoidChestServer(player, item, item->count);
		}
		free(item);
	}},

	// the client removed a curse on his equipment
	{'RCUR', [](){
	    Item* item;
	    const int player =
	    	decodeGameplayPacketPlayerIndex(
	    		net_packet->data[4]
	    	);
	    if ( player < 0 )
	    {
	    	return;
	    }
		switch ( net_packet->data[5] )
		{
			case 0:
				item = stats[player]->helmet;
				break;
			case 1:
				item = stats[player]->breastplate;
				break;
			case 2:
				item = stats[player]->gloves;
				break;
			case 3:
				item = stats[player]->shoes;
				break;
			case 4:
				item = stats[player]->shield;
				break;
			case 5:
				item = stats[player]->weapon;
				break;
			case 6:
				item = stats[player]->cloak;
				break;
			case 7:
				item = stats[player]->amulet;
				break;
			case 8:
				item = stats[player]->ring;
				break;
			case 9:
				item = stats[player]->mask;
				break;
			default:
				item = nullptr;
				break;
		}
		if ( item != nullptr )
		{
			item->beatitude = 0;
		}
	}},

	// the client repaired equipment or otherwise modified status of equipment.
	{'REPA', [](){
	    const int player =
	    	decodeGameplayPacketPlayerIndex(
	    		net_packet->data[4]
	    	);
	    if ( player < 0 )
	    {
	    	return;
	    }
		Item* equipment = nullptr;

		switch ( net_packet->data[5] )
		{
			case 0:
				equipment = stats[player]->weapon;
				break;
			case 1:
				equipment = stats[player]->helmet;
				break;
			case 2:
				equipment = stats[player]->breastplate;
				break;
			case 3:
				equipment = stats[player]->gloves;
				break;
			case 4:
				equipment = stats[player]->shoes;
				break;
			case 5:
				equipment = stats[player]->shield;
				break;
			case 6:
				equipment = stats[player]->cloak;
				break;
			case 7:
				equipment = stats[player]->mask;
				break;
			default:
				equipment = nullptr;
				break;
		}

		if ( !equipment )
		{
			return;
		}
		
		if ( static_cast<int>(net_packet->data[6]) > EXCELLENT )
		{
			equipment->status = EXCELLENT;
		}
		else if ( static_cast<int>(net_packet->data[6]) < BROKEN )
		{
			equipment->status = BROKEN;
		}
		equipment->status = static_cast<Status>(net_packet->data[6]);
		return;
	}},

	// the client repaired tinkering bots
	{ 'REPT', []() {
		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( player < 0 )
		{
			return;
		}
		Item* equipment = nullptr;

		switch ( net_packet->data[5] )
		{
			case 0:
				equipment = stats[player]->weapon;
				break;
			case 1:
				equipment = stats[player]->helmet;
				break;
			case 2:
				equipment = stats[player]->breastplate;
				break;
			case 3:
				equipment = stats[player]->gloves;
				break;
			case 4:
				equipment = stats[player]->shoes;
				break;
			case 5:
				equipment = stats[player]->shield;
				break;
			case 6:
				equipment = stats[player]->cloak;
				break;
			case 7:
				equipment = stats[player]->mask;
				break;
			default:
				equipment = nullptr;
				break;
		}

		if ( !equipment )
		{
			return;
		}
		if ( !(equipment->type == TOOL_SENTRYBOT
			|| equipment->type == TOOL_SPELLBOT
			|| equipment->type == TOOL_DUMMYBOT
			|| equipment->type == TOOL_GYROBOT) )
		{
			return;
		}

		if ( static_cast<int>(net_packet->data[6]) > EXCELLENT )
		{
			equipment->status = EXCELLENT;
		}
		else if ( static_cast<int>(net_packet->data[6]) < BROKEN )
		{
			equipment->status = BROKEN;
		}
		equipment->status = static_cast<Status>(net_packet->data[6]);
		equipment->appearance = static_cast<Uint32>(SDLNet_Read32(&net_packet->data[7]));
		return;
	} },

	// the client changed beatitude of equipment.
	{'BEAT', [](){
	    const int player =
	    	decodeGameplayPacketPlayerIndex(
	    		net_packet->data[4]
	    	);
	    if ( player < 0 )
	    {
	    	return;
	    }
		Item* equipment = nullptr;
		//messagePlayer(0, "client: %d, armornum: %d, status %d", player, net_packet->data[5], net_packet->data[6]);
		switch ( net_packet->data[5] )
		{
			case 0:
				equipment = stats[player]->weapon;
				break;
			case 1:
				equipment = stats[player]->helmet;
				break;
			case 2:
				equipment = stats[player]->breastplate;
				break;
			case 3:
				equipment = stats[player]->gloves;
				break;
			case 4:
				equipment = stats[player]->shoes;
				break;
			case 5:
				equipment = stats[player]->shield;
				break;
			case 6:
				equipment = stats[player]->cloak;
				break;
			case 7:
				equipment = stats[player]->mask;
				break;
			default:
				equipment = nullptr;
				break;
		}

		if ( !equipment )
		{
			return;
		}
		int itemType = SDLNet_Read16(&net_packet->data[7]);
		if ( (int)equipment->type == itemType ) // sanity check the item type is what was changed
		{
			equipment->beatitude = net_packet->data[6] - 100; // we sent the data beatitude + 100
		}
		//messagePlayer(0, "%d", equipment->beatitude);
	}},

	// client dropped gold
	{'DGLD', [](){
	    const int player =
	    	decodeGameplayPacketPlayerIndex(
	    		net_packet->data[4]
	    	);
	    if ( player < 0 )
	    {
	    	return;
	    }
		int amount = SDLNet_Read32(&net_packet->data[5]);

		if ( stats[player]->GOLD < 0 )
		{
			stats[player]->GOLD = 0;
		}
		if ( stats[player]->GOLD < amount )
		{
			amount = stats[player]->GOLD;
		}
        if ( amount <= 0 )
        {
            return;
        }
		stats[player]->GOLD -= amount;
		stats[player]->GOLD = std::max(stats[player]->GOLD, 0);
		if ( players[player] && players[player]->entity )
		{
			//Drop gold.
			playSoundEntity(players[player]->entity, 242 + local_rng.rand() % 4, 64);
			auto entity = newEntity(amount < 5 ? 1379 : 130, 0, map.entities, nullptr); // 130 = goldbag model
			entity->sizex = 4;
			entity->sizey = 4;
			entity->x = players[player]->entity->x;
			entity->y = players[player]->entity->y;
			entity->goldAmount = amount; // amount
			entity->z = 0;
			entity->vel_z = (-40 - local_rng.rand() % 5) * .01;
			entity->goldBouncing = 0;
			entity->yaw = (local_rng.rand() % 360) * PI / 180.0;
			entity->flags[PASSABLE] = true;
			entity->flags[UPDATENEEDED] = true;
			entity->behavior = &actGoldBag;
			entity->goldDroppedByPlayer = player + 1;
		}
	}},

	// client played a sound
	{'EMOT', [](){
		if ( !net_packet || net_packet->len < 8 )
		{
			printlog(
				"[NET]: ignoring truncated EMOT packet"
			);
			return;
		}

	    const int player =
	    	decodeGameplayPacketPlayerIndex(
	    		net_packet->data[4]
	    	);
	    if ( player < 0 )
	    {
	    	return;
	    }
		const int sfx = SDLNet_Read16(&net_packet->data[5]);
		const int vol = std::min(92, (int)(net_packet->data[7]));
		if ( players[player] && players[player]->entity )
		{
			playSoundEntityLocal(players[player]->entity, sfx, vol);
			for ( int c = 1; c < MAXPLAYERS; ++c )
			{
				// send to all other players
				if ( c != player
					&& !client_disconnected[c]
					&& players[c]
					&& !players[c]->isLocalPlayer() )
				{
					strcpy((char*)net_packet->data, "SNEL");
					SDLNet_Write16(sfx, &net_packet->data[4]);
					SDLNet_Write32((Uint32)players[player]->entity->getUID(), &net_packet->data[6]);
					SDLNet_Write16(vol, &net_packet->data[10]);
					net_packet->address.host = net_clients[c - 1].host;
					net_packet->address.port = net_clients[c - 1].port;
					net_packet->len = 12;
					sendPacketSafe(net_sock, -1, net_packet, c - 1);
				}
			}
		}
	}},

	// the client asked for a level up
	{'CLVL', [](){
	    const int player =
	    	decodeGameplayPacketPlayerIndex(
	    		net_packet->data[4]
	    	);
	    if ( player < 0 )
	    {
	    	return;
	    }
		if ( players[player] && players[player]->entity )
		{
			players[player]->entity->getStats()->EXP += 100;
		}
	}},

	// the client asked for a level up
	{'CSKL', [](){
	    const int player =
	    	decodeGameplayPacketPlayerIndex(
	    		net_packet->data[4]
	    	);
	    if ( player < 0 )
	    {
	    	return;
	    }
		const int skill = net_packet->data[5];
		if ( player > 0 && player < MAXPLAYERS && players[player] && players[player]->entity )
		{
			if ( skill >= 0 && skill < NUMPROFICIENCIES )
			{
				players[player]->entity->increaseSkill(skill);
			}
		}
	}},

	// the client sent a minimap ping packet.
	{'PMAP', [](){
		if ( net_packet->len < 9 )
		{
			return;
		}
		const int player =
			decodeGameplayPacketPlayerIndex(net_packet->data[4]);
		if ( player < 0 )
		{
			return;
		}
		MinimapPing newPing(ticks, player,
			net_packet->data[5], 
			net_packet->data[6],
			net_packet->data[8] ? true : false,
			(MinimapPing::PingType)net_packet->data[7]);
		sendMinimapPing(player, newPing.x, newPing.y, newPing.pingType); // relay self and to other clients.
	}},

	// the client sent a gameplayer preferences update
	{ 'GPPR', []() {
		GameplayPreferences_t::receivePacket();
	}},

	//Remove vampiric aura
	{'VAMP', [](){
	    const int player =
	    	decodeGameplayPacketPlayerIndex(
	    		net_packet->data[4]
	    	);
	    if ( player < 0 )
	    {
	    	return;
	    }
		const int spellID = SDLNet_Read32(&net_packet->data[5]);
		if ( players[player] && players[player]->entity && stats[player] )
		{
			if ( client_classes[player] == CLASS_ACCURSED &&
				stats[player]->getEffectActive(EFF_VAMPIRICAURA) && players[player]->entity->playerVampireCurse == 1 )
			{
				players[player]->entity->setEffect(EFF_VAMPIRICAURA, true, 1, true);
				messagePlayerColor(player, MESSAGE_STATUS, uint32ColorGreen, Language::get(3241));
				messagePlayerColor(player, MESSAGE_HINT, uint32ColorGreen, Language::get(3242));
				players[player]->entity->playerVampireCurse = 2; // cured.
				serverUpdateEntitySkill(players[player]->entity, 51);
				steamAchievementClient(player, "BARONY_ACH_REVERSE_THIS_CURSE");
				playSoundEntity(players[player]->entity, 402, 128);
				createParticleDropRising(players[player]->entity, 174, 1.0);
				serverSpawnMiscParticles(players[player]->entity, PARTICLE_EFFECT_RISING_DROP, 174);
			}
		}
	}},

	// the client sent a monster command.
	{'ALLY', [](){
	    const int player =
	    	decodeGameplayPacketPlayerIndex(
	    		net_packet->data[4]
	    	);
	    if ( player < 0 )
	    {
	    	return;
	    }
		const int allyCmd = net_packet->data[5];
		const Uint32 uid = SDLNet_Read32(&net_packet->data[8]);
		//messagePlayer(0, " received %d, %d, %d, %d, %d", player, allyCmd, net_packet->data[6], net_packet->data[7], uid);
		Entity* entity = uidToEntity(uid);
		if ( entity )
		{
			if ( net_packet->len > 12 )
			{
				Uint32 interactUid = SDLNet_Read32(&net_packet->data[12]);
				entity->monsterAllySendCommand(allyCmd, net_packet->data[6], net_packet->data[7], interactUid);
				//messagePlayer(0, "received UID of target: %d, applying...", uid);
				entity->monsterAllyInteractTarget = interactUid;
			}
			else
			{
				entity->monsterAllySendCommand(allyCmd, net_packet->data[6], net_packet->data[7]);
			}
		}
	}},

	{'IDIE', [](){
		if ( net_packet->len < 5 )
		{
			return;
		}
		const int playerDie =
			decodeGameplayPacketPlayerIndex(net_packet->data[4]);
		if ( playerDie >= 1 )
		{
			if ( players[playerDie] && players[playerDie]->entity )
			{
				players[playerDie]->entity->setHP(0);
			}
		}
	}},

	// use automaton food item
	{'FODA', [](){
	    const int player =
	    	decodeGameplayPacketPlayerIndex(
	    		net_packet->data[25]
	    	);
	    if ( player < 0 )
	    {
	    	return;
	    }
		auto item = newItem(
		    static_cast<ItemType>(SDLNet_Read32(&net_packet->data[4])),
		    static_cast<Status>(SDLNet_Read32(&net_packet->data[8])),
		    SDLNet_Read32(&net_packet->data[12]),
		    SDLNet_Read32(&net_packet->data[16]),
		    SDLNet_Read32(&net_packet->data[20]),
		    net_packet->data[24],
		    &stats[player]->inventory);
		item_FoodAutomaton(item, player);
	}},

	// adorcise item
	{ 'ADOR', []() {
		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[25]
			);
		if ( player < 0 )
		{
			return;
		}
		auto item = newItem(
			static_cast<ItemType>(SDLNet_Read32(&net_packet->data[4])),
			static_cast<Status>(SDLNet_Read32(&net_packet->data[8])),
			SDLNet_Read32(&net_packet->data[12]),
			SDLNet_Read32(&net_packet->data[16]),
			SDLNet_Read32(&net_packet->data[20]),
			net_packet->data[24], nullptr);
		
		real_t spawn_x = SDLNet_Read16(&net_packet->data[26]) * 16.0 + 8.0;
		real_t spawn_y = SDLNet_Read16(&net_packet->data[28]) * 16.0 + 8.0;
		bool spawned = false;
		if ( players[player]->entity )
		{
			if ( Entity* monster = spellEffectAdorcise(*players[player]->entity, spellElementMap[SPELL_ADORCISM],
				spawn_x, spawn_y, item) )
			{
				spawned = true;
			}
		}
		
		if ( !spawned )
		{
			messagePlayer(player, MESSAGE_MISC, Language::get(6578));

			// refund the item at the player, or spawn location if dead
			bool dropped = false;
			if ( players[player]->entity )
			{
				// no room to spawn!
				auto item2 = newItem(item->type,
					item->status,
					item->beatitude,
					item->count,
					item->appearance,
					item->identified,
					&stats[player]->inventory);
				dropped = dropItem(item2, player, true, true);
			}
			
			if ( !dropped )
			{
				Entity* entity = newEntity(-1, 1, map.entities, nullptr); //Item entity.
				entity->flags[INVISIBLE] = true;
				entity->flags[UPDATENEEDED] = true;
				entity->x = players[player]->player_last_x;
				entity->y = players[player]->player_last_y;
				entity->sizex = 4;
				entity->sizey = 4;
				entity->yaw = local_rng.rand() % 360 * (PI / 180.0);
				entity->vel_x = 0.0;
				entity->vel_y = 0.0;
				entity->vel_z = (-10 - local_rng.rand() % 20) * .01;
				entity->flags[PASSABLE] = true;
				entity->behavior = &actItem;
				entity->skill[10] = item->type;
				entity->skill[11] = item->status;
				entity->skill[12] = item->beatitude;
				entity->skill[13] = item->count;
				entity->skill[14] = item->appearance;
				entity->skill[15] = item->identified;
				entity->parent = 0;
				entity->itemOriginalOwner = 0;

				playSoundPos(players[player]->player_last_x, players[player]->player_last_y, 47 + local_rng.rand() % 3, 64);
			}
			messagePlayer(player, MESSAGE_MISC, Language::get(6621), item->getName());
		}

		free(item);
	} },

	// broke a mirror
	{ 'MIRR', []() {
		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( player < 0 )
		{
			return;
		}
		if ( players[player]->entity )
		{
			if ( players[player]->entity->setEffect(EFF_BLEEDING, true, TICKS_PER_SECOND * 15, true) )
			{
				messagePlayerColor(player, MESSAGE_STATUS, 
					makeColorRGB(255, 0, 0), Language::get(701)); // you're bleeding!
			}
			playSoundEntity(players[player]->entity, 162, 64);
		}
	} },

	{ 'CMPD', []() {
		// client ack received the packet
		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( player < 0 )
		{
			return;
		}
		const Uint8 clientSequence = net_packet->data[5];

		auto find = Compendium_t::Events_t::clientDataStrings[player].find(clientSequence);
		if ( find != Compendium_t::Events_t::clientDataStrings[player].end() )
		{
			Compendium_t::Events_t::clientDataStrings[player].erase(clientSequence);
		}
	}},

	// character change
	{ 'ASSC', []() {
		int player = net_packet->data[8];
		if ( player >= 0 && player < MAXPLAYERS )
		{
			auto& gui = GenericGUI[player].assistShrineGUI;
			gui.savedClass = (Sint8)net_packet->data[4];
			gui.savedRace = (Sint8)net_packet->data[5];
			gui.savedSex = (Sint8)net_packet->data[6];
			gui.savedAppearance = (Sint8)net_packet->data[7];
			gui.receivedCharacterChangeOK = true;

			std::string racename = "";
			if ( gui.savedRace != RACE_HUMAN )
			{
				if ( gui.savedAppearance != 0 )
				{
					racename = Language::get(4068); // guised
					racename += ' ';
				}
			}
			racename += getMonsterLocalizedName(getMonsterFromPlayerRace(gui.savedRace)).c_str();
			camelCaseString(racename);
			std::string classname = playerClassLangEntry(gui.savedClass >= 0 ? gui.savedClass : client_classes[player], player);
			camelCaseString(classname);

			for ( int i = 0; i < MAXPLAYERS; ++i )
			{
				if ( i != player )
				{
					messagePlayer(i, MESSAGE_WORLD, Language::get(6336), stats[player]->name, racename.c_str(), classname.c_str());
				}
			}

			if ( player > 0 )
			{
				// confirm receipt of class change to sender
				strcpy((char*)net_packet->data, "ASSC");
				net_packet->data[4] = (Sint8)gui.savedClass;
				net_packet->data[5] = (Sint8)gui.savedRace;
				net_packet->data[6] = (Sint8)gui.savedSex;
				net_packet->data[7] = (Sint8)gui.savedAppearance;
				net_packet->data[8] = player;
				net_packet->address.host = net_clients[player - 1].host;
				net_packet->address.port = net_clients[player - 1].port;
				net_packet->len = 9;
				sendPacketSafe(net_sock, -1, net_packet, player - 1);
			}
		}
	}},

	// client closed assist shrine
	{ 'ASCL', []() {
		if ( net_packet->len < 9 )
		{
			return;
		}
		const int player =
			decodeGameplayPacketPlayerIndex(net_packet->data[4]);
		if ( player >= 0 )
		{
			Uint32 uid = SDLNet_Read32(&net_packet->data[5]);
			if ( Entity* shrine = uidToEntity(uid) )
			{
				if ( achievementObserver.playerUids[player] == (Uint32)shrine->skill[0] )
				{
					shrine->skill[0] = 0;
					serverUpdateEntitySkill(shrine, 0);
				}
			}
		}
	}},

	// client closed cauldron
	{ 'CAUC', []() {
		if ( net_packet->len < 9 )
		{
			return;
		}
		const int player =
			decodeGameplayPacketPlayerIndex(net_packet->data[4]);
		if ( player >= 0 )
		{
			Uint32 uid = SDLNet_Read32(&net_packet->data[5]);
			if ( Entity* cauldron = uidToEntity(uid) )
			{
				if ( achievementObserver.playerUids[player] == (Uint32)cauldron->skill[6] )
				{
					cauldron->skill[6] = 0;
					serverUpdateEntitySkill(cauldron, 6);
				}
			}
		}
	} },

	// client closed workbench
	{ 'WRKC', []() {
		if ( net_packet->len < 9 )
		{
			return;
		}
		const int player =
			decodeGameplayPacketPlayerIndex(net_packet->data[4]);
		if ( player >= 0 )
		{
			Uint32 uid = SDLNet_Read32(&net_packet->data[5]);
			if ( Entity* workbench = uidToEntity(uid) )
			{
				if ( achievementObserver.playerUids[player] == (Uint32)workbench->skill[6] )
				{
					workbench->skill[6] = 0;
					serverUpdateEntitySkill(workbench, 6);
				}
			}
		}
	} },

	// client closed mailbox
	{ 'MBXC', []() {
		if ( net_packet->len < 9 )
		{
			return;
		}
		const int player =
			decodeGameplayPacketPlayerIndex(net_packet->data[4]);
		if ( player >= 0 )
		{
			Uint32 uid = SDLNet_Read32(&net_packet->data[5]);
			if ( Entity* mailbox = uidToEntity(uid) )
			{
				if ( achievementObserver.playerUids[player] == (Uint32)mailbox->skill[6] )
				{
					mailbox->skill[6] = 0;
					serverUpdateEntitySkill(mailbox, 6);
				}
			}
		}
	} },

	// client claimed some assist items
	{ 'ASSI', []() {
	int player = net_packet->data[4];
	if ( player >= 0 && player < MAXPLAYERS )
	{
		Sint32 claimedPts = std::max(0, (Sint32)SDLNet_Read32(&net_packet->data[5]));
		Sint32 prevPts = stats[player]->MISC_FLAGS[STAT_FLAG_ASSISTANCE_PLAYER_PTS];
		stats[player]->MISC_FLAGS[STAT_FLAG_ASSISTANCE_PLAYER_PTS] = claimedPts;

		int totalClaimed = 0;
		for ( int i = 0; i < MAXPLAYERS; ++i )
		{
			if ( !client_disconnected[i] )
			{
				totalClaimed += stats[i]->MISC_FLAGS[STAT_FLAG_ASSISTANCE_PLAYER_PTS];
				if ( i == player )
				{
					messagePlayer(i, MESSAGE_WORLD, Language::get(6356), claimedPts);
				}
				else
				{
					messagePlayer(i, MESSAGE_WORLD, Language::get(6357), stats[player]->name, claimedPts);
				}
			}
		}

		conductGameChallenges[CONDUCT_ASSISTANCE_CLAIMED] = std::max(totalClaimed, conductGameChallenges[CONDUCT_ASSISTANCE_CLAIMED]);
		for ( int i = 1; i < MAXPLAYERS; ++i )
		{
			if ( !client_disconnected[i] )
			{
				serverUpdatePlayerConduct(i, CONDUCT_ASSISTANCE_CLAIMED, conductGameChallenges[CONDUCT_ASSISTANCE_CLAIMED]);
			}
		}
		GenericGUIMenu::AssistShrineGUI_t::serverUpdateStatFlagsForClients();
	}
	} },

	{ 'VOIP',[]() {
#ifdef USE_FMOD
		VoiceChat.receivePacket(net_packet);
#endif
	} },

	{ 'FXGD',[]() {
		if ( !net_packet || net_packet->len < 15 )
		{
			printlog(
				"[NET]: ignoring truncated FXGD packet"
			);
			return;
		}

		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( player >= 1
			&& players[player]
			&& stats[player]
			&& !players[player]->isLocalPlayer() )
		{
			Sint32 goldSpent = (Sint32)SDLNet_Read32(&net_packet->data[5]);
			stats[player]->GOLD -= goldSpent;
			stats[player]->GOLD = std::max(0, stats[player]->GOLD);

			Sint32 magiccost = std::max(0, (Sint32)SDLNet_Read32(&net_packet->data[9]));
			Sint32 prevMP = stats[player]->MP;
			if ( players[player] && players[player]->entity )
			{
				if ( magiccost > stats[player]->MP )
				{
					// damage sound/effect due to overdraw.
					strcpy((char*)net_packet->data, "SHAK");
					net_packet->data[4] = 10; // turns into .1
					net_packet->data[5] = 10;
					net_packet->address.host = net_clients[player - 1].host;
					net_packet->address.port = net_clients[player - 1].port;
					net_packet->len = 6;
					sendPacketSafe(net_sock, -1, net_packet, player - 1);
					playSoundPlayer(player, 28, 92);
				}
				players[player]->entity->drainMP(magiccost);
			}

			Uint16 spellID = SDLNet_Read16(&net_packet->data[13]);
			if ( spellID != SPELL_NONE )
			{
				if ( auto spell = getSpellFromID(spellID) )
				{
					players[player]->mechanics.baseSpellIncrementMP(prevMP - stats[player]->MP, spell->skillID);
				}
			}

			strcpy((char*)net_packet->data, "GOLD");
			SDLNet_Write32(stats[player]->GOLD, &net_packet->data[4]);
			net_packet->address.host = net_clients[player - 1].host;
			net_packet->address.port = net_clients[player - 1].port;
			net_packet->len = 8;
			sendPacketSafe(net_sock, -1, net_packet, player - 1);
		}
	}},

	{ 'SANM',[]() { // player spellcast animation
		if ( !net_packet || net_packet->len < 8 )
		{
			printlog(
				"[NET]: ignoring truncated SANM packet"
			);
			return;
		}

		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( player < 0 )
		{
			return;
		}

		const int pose = net_packet->data[5];
		const int charge =
			SDLNet_Read16(&net_packet->data[6]);
		spellcastAnimationUpdateReceive(
			player,
			pose,
			charge
		);
	} },

	{ 'OVRC', []() {
		if ( !net_packet || net_packet->len < 6 )
		{
			printlog(
				"[NET]: ignoring truncated OVRC packet"
			);
			return;
		}

		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( player >= 1
			&& players[player]
			&& players[player]->entity
			&& stats[player]
			&& !players[player]->isLocalPlayer() )
		{
			cast_animation[player].overcharge_init =
				net_packet->data[5];
		}
	} },

	{ 'SPLV',[]() { // player spell level proc
		if ( !net_packet || net_packet->len < 15 )
		{
			printlog(
				"[NET]: ignoring truncated SPLV packet"
			);
			return;
		}

		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( player >= 0
			&& players[player]
			&& !players[player]->isLocalPlayer() )
		{
			if ( players[player]->entity )
			{
				int spellID = SDLNet_Read16(&net_packet->data[5]);
				Uint32 eventType = SDLNet_Read32(&net_packet->data[7]);
				int eventValue = SDLNet_Read32(&net_packet->data[11]);
					if ( spellID == SPELL_DETECT_FOOD )
					{
						players[player]->mechanics.updateSustainedSpellEvent(SPELL_DETECT_FOOD, eventValue * 10, 1.0, nullptr);
					}
					else
					{
						magicOnSpellCastEvent(players[player]->entity, players[player]->entity, nullptr, spellID, eventType, eventValue);
				}
			}
		}
	} },

	// update breakable counter
	{ 'GBRK', []() {
		if ( !net_packet || net_packet->len < 6 )
		{
			printlog(
				"[NET]: ignoring truncated GBRK packet"
			);
			return;
		}

		const int player =
			decodeGameplayPacketPlayerIndex(
				net_packet->data[4]
			);
		if ( player >= 0
			&& players[player]
			&& !players[player]->isLocalPlayer() )
		{
			if ( players[player]->entity )
			{
				int eventType = net_packet->data[5];
					if ( eventType == (int)Player::PlayerMechanics_t::BreakableEvent::GBREAK_DEGRADE )
					{
						players[player]->mechanics.incrementBreakableCounter(Player::PlayerMechanics_t::BreakableEvent::GBREAK_DEGRADE, nullptr);
				}
			}
		}
	} },
};

void serverHandlePacket()
{
    if ( !net_packet || !net_packet->data || net_packet->len < 4 )
    {
        printlog("[NET]: ignored truncated server packet");
        return;
    }
	if (handleSafePacket())
	{
		return;
	}

#ifdef PACKETINFO
	char packetinfo[NET_PACKET_SIZE];
	strncpy( packetinfo, (char*)net_packet->data, net_packet->len );
	packetinfo[net_packet->len] = 0;
	printlog("info: server packet: %s\n", packetinfo);
#endif

	Uint32 packetId = SDLNet_Read32(&net_packet->data[0]);

    auto find = serverPacketHandlers.find(packetId);
    if (find == serverPacketHandlers.end()) {
        // error
        printlog("Got a mystery packet: %c%c%c%c",
            (char)net_packet->data[0],
            (char)net_packet->data[1],
            (char)net_packet->data[2],
            (char)net_packet->data[3]);
    } else {
        (*(find->second))(); // handle packet
    }
}

/*-------------------------------------------------------------------------------

	serverHandleMessages

	Parses messages received from clients

-------------------------------------------------------------------------------*/

void serverHandleMessages(Uint32 framerateBreakInterval)
{
	const WorldInstanceIdentity* initialIdentity =
		worldState.activeIdentity();
	const std::string initialInstanceKey =
		initialIdentity ? initialIdentity->key() : std::string{};
#ifdef STEAMWORKS
	if (!directConnect && !net_handler)
	{
		net_handler = new NetHandler();
		if ( !disableMultithreadedSteamNetworking )
		{
			net_handler->initializeMultithreadedPacketHandling();
		}
	}
#elif defined USE_EOS
	if ( !directConnect && !net_handler )
	{
		net_handler = new NetHandler();
	}
#endif

	if (!directConnect)
	{
#if defined(STEAMWORKS) || defined(USE_EOS)
		if ( LobbyHandler.getP2PType() == LobbyHandler_t::LobbyServiceType::LOBBY_STEAM )
		{
#ifdef STEAMWORKS
			//Steam stuff goes here.
			if ( disableMultithreadedSteamNetworking )
			{
				steamPacketThread(static_cast<void*>(net_handler));
			}
#endif
		}
		else if ( LobbyHandler.getP2PType() == LobbyHandler_t::LobbyServiceType::LOBBY_CROSSPLAY )
		{
#if defined USE_EOS
			EOSPacketThread(static_cast<void*>(net_handler));
#endif // USE_EOS
		}
		SteamPacketWrapper* packet = nullptr;

		if ( logCheckMainLoopTimers )
		{
			DebugStats.messagesT1 = std::chrono::high_resolution_clock::now();
			DebugStats.handlePacketStartLoop = true;
		}

		while ( packet = net_handler->getGamePacket() )
		{
			g_currentPacketSenderHostIndex = packet->senderHostIndex();
			memcpy(net_packet->data, packet->data(), packet->len());
			net_packet->len = packet->len();

			serverHandlePacket(); //Uses net_packet;

			if ( logCheckMainLoopTimers )
			{
				DebugStats.messagesT2WhileLoop = std::chrono::high_resolution_clock::now();
				DebugStats.handlePacketStartLoop = false;
			}
			delete packet;
			g_currentPacketSenderHostIndex = -1;
			if ( !net_handler )
			{
				break;
			}

			if ( !disableFPSLimitOnNetworkMessages && !frameRateLimit(framerateBreakInterval, false) )
			{
				if ( logCheckMainLoopTimers )
				{
					printlog("[NETWORK]: Incoming messages exceeded given cycle time, packets remaining: %d", net_handler->game_packets.size());
				}
				break;
			}
		}
#endif
	}
	else
	{
		//Direct-connect goes here.

		while (SDLNet_UDP_Recv(net_sock, net_packet))
		{
			// filter out broken packets
			if ( !net_packet->data[0] )
			{
				continue;
			}

			serverHandlePacket(); //Uses net_packet.
		}
	}
	for (int player = 1; player < MAXPLAYERS; ++player)
	{
		const LateJoinSnapshotTransaction::Phase phase =
			g_lateJoinTransactions[player].phase();
		if (phase == LateJoinSnapshotTransaction::Phase::Failed)
		{
			printlog("[Late Join] Aborting failed transfer for player %d.", player);
			abortServerLateJoinPlayer(player, 3);
		}
		else if ((phase == LateJoinSnapshotTransaction::Phase::AwaitingClient
				|| phase == LateJoinSnapshotTransaction::Phase::Receiving
				|| phase == LateJoinSnapshotTransaction::Phase::Complete)
			&& g_lateJoinLastProgressTick[player] != 0
			&& ticks - g_lateJoinLastProgressTick[player]
				> (phase == LateJoinSnapshotTransaction::Phase::AwaitingClient
					? kLateJoinCharacterSelectionTimeoutTicks
					: kLateJoinTimeoutTicks))
		{
			if (phase == LateJoinSnapshotTransaction::Phase::AwaitingClient)
			{
				printlog(
					"[Late Join] Character selection for player %d timed out after 5 minutes.",
					player);
			}
			else
			{
				printlog(
					"[Late Join] Transfer for player %d timed out after 30 seconds.",
					player);
			}
			abortServerLateJoinPlayer(player, 1);
		}
	}
	if ( !initialInstanceKey.empty()
		&& worldState.activeIdentity()
		&& worldState.activeIdentity()->key() != initialInstanceKey )
	{
		if ( !worldState.activate(initialInstanceKey) )
		{
			printlog(
				"[NET]: unable to restore active instance '%s' after receiving packets",
				initialInstanceKey.c_str()
			);
		}
	}
}

/*-------------------------------------------------------------------------------

	handleSafePacket()

	Handles potentially safe packets. Returns true if this is not a packet to
	be handled.

-------------------------------------------------------------------------------*/

bool handleSafePacket()
{
	node_t* node;
	int c, j;

	// safe packet
	Uint32 packetId = SDLNet_Read32(&net_packet->data[0]);
	if (packetId == 'SAFE')
	{
        if ( net_packet->len < 9 )
        {
            printlog("[NET]: ignored truncated SAFE packet");
            return true;
        }
        if ( net_packet->data[4] > MAXPLAYERS )
        {
            printlog(
                "[NET]: ignored SAFE packet with invalid sender index %u",
                static_cast<unsigned>(net_packet->data[4])
            );
            return true;
        }
		if ( net_packet->data[4] != MAXPLAYERS )
		{
			int receivedPacketNum = SDLNet_Read32(&net_packet->data[5]);
			Uint8 fromClientnum = net_packet->data[4];

			if ( ticks > (60 * TICKS_PER_SECOND) && (ticks % (TICKS_PER_SECOND / 2) == 0) )
			{
				// clear old packets > 60 secs
				for ( auto it = safePacketsReceivedMap[fromClientnum].begin(); it != safePacketsReceivedMap[fromClientnum].end(); )
				{
					if ( it->second < (ticks - 60 * TICKS_PER_SECOND) )
					{
						it = safePacketsReceivedMap[fromClientnum].erase(it);
					}
					else
					{
						++it;
					}
				}
			}

			//auto tmp1 = std::chrono::high_resolution_clock::now();
			auto find = safePacketsReceivedMap[fromClientnum].find(receivedPacketNum);
			if ( find != safePacketsReceivedMap[fromClientnum].end() )
			{
				return true;
			}

			/*auto tmp2 = std::chrono::high_resolution_clock::now();
			std::chrono::duration<double> time_span =
				std::chrono::duration_cast<std::chrono::duration<double>>(tmp2 - tmp1);
			double timer = time_span.count() * 1000;*/

			safePacketsReceivedMap[fromClientnum].insert(std::make_pair(receivedPacketNum, ticks));

			// send an ack
			j = net_packet->data[4];
			net_packet->data[4] = clientnum;
			strcpy((char*)net_packet->data, "GOTP");
			if ( multiplayer == CLIENT )
			{
				net_packet->address.host = net_server.host;
				net_packet->address.port = net_server.port;
			}
			else
			{
				if ( j > 0 )
				{
					net_packet->address.host = net_clients[j - 1].host;
					net_packet->address.port = net_clients[j - 1].port;
				}
			}
			c = net_packet->len;
			net_packet->len = 9;
			if ( multiplayer == CLIENT )
			{
				sendPacket(net_sock, -1, net_packet, 0);
			}
			else
			{
				if ( j > 0 )
				{
					sendPacket(net_sock, -1, net_packet, j - 1);
				}
			}
			net_packet->len = c - 9;
			Uint8 bytedata[NET_PACKET_SIZE];
			memcpy(&bytedata, net_packet->data + 9, net_packet->len);
			memcpy(net_packet->data, &bytedata, net_packet->len);

			/*int sprite = -9999;
			char chr[5];
			chr[0] = '\0';
			strncpy(chr, (char*)net_packet->data, 4);
			chr[4] = '\0';
			if ( packetId == 'ENTU' )
			{
				Entity *entity = uidToEntity((int)SDLNet_Read32(&net_packet->data[4]));
				if ( entity )
				{
					sprite = entity->sprite;
				}
			}

			if ( packetId == 'ENTS' )
			{
				Entity *entity = uidToEntity((int)SDLNet_Read32(&net_packet->data[4]));
				if ( entity )
				{
					messagePlayer(clientnum, "%s | %d : %d - %d | %d %.8f", chr, entity->sprite, net_packet->data[8],
						SDLNet_Read32(&net_packet->data[9]), safePacketsReceivedMap[fromClientnum].size(), timer);
				}
			}
			else
			{
				messagePlayer(clientnum, "%s | %d %.8f", chr, safePacketsReceivedMap[fromClientnum].size(), timer);
			}*/
		}
	}

	// they got the safe packet
	else if (packetId == 'GOTP')
	{
        if ( net_packet->len < 9 )
        {
            printlog("[NET]: ignored truncated GOTP packet");
            return true;
        }
		for ( node = safePacketsSent.first; node != NULL; node = node->next )
		{
			packetsend_t* packet = (packetsend_t*)node->element;
			if ( packet->num == SDLNet_Read32(&net_packet->data[5]) )
			{
				list_RemoveNode(node);
				break;
			}
		}
		return true;
	}

	return false;
}

/*-------------------------------------------------------------------------------

	closeNetworkInterfaces()

	Consolidating all of the network interfaces close code into one place.

-------------------------------------------------------------------------------*/

void closeNetworkInterfaces()
{
	printlog("closing network interfaces...\n");

	receivedclientnum = false;
	g_clientLateJoinAssembler.reset();
	g_clientLateJoinBegin = LateJoinProtocol::Begin{};
	g_clientLateJoinSpawnAuthorized = false;
	for (int c = 1; c < MAXPLAYERS; ++c)
	{
		resetServerLateJoinPlayer(c);
		g_lateJoinReturningPlayer[c] = false;
	}

	if (net_handler)
	{
		delete net_handler; //Close steam multithreading and stuff.
		net_handler = nullptr;
	}

	if (net_packet != nullptr)
	{
		SDLNet_FreePacket(net_packet);
		net_packet = nullptr;
	}
	if (net_clients != nullptr)
	{
		free(net_clients);
		net_clients = nullptr;
	}
	if (net_sock != nullptr)
	{
		SDLNet_UDP_Close(net_sock);
		net_sock = nullptr;
	}
	if (net_tcpclients != nullptr)
	{
		for (int c = 0; c < MAXPLAYERS; c++)
		{
			if (net_tcpclients[c] != nullptr)
			{
				SDLNet_TCP_Close(net_tcpclients[c]);
			}
		}
		free(net_tcpclients);
		net_tcpclients = nullptr;
	}
	if (net_tcpsock != nullptr)
	{
		SDLNet_TCP_Close(net_tcpsock);
		net_tcpsock = nullptr;
	}
	if (tcpset)
	{
		SDLNet_FreeSocketSet(tcpset);
		tcpset = nullptr;
	}
#ifdef STEAMWORKS
    for (int c = 0; c < MAXPLAYERS; ++c) {
        if (steamIDRemote[c]) {
            cpp_Free_CSteamID(steamIDRemote[c]);
            steamIDRemote[c] = NULL;
        }
    }
#endif
}





/* ***** MULTITHREADED STEAM PACKET HANDLING ***** */

SteamPacketWrapper::SteamPacketWrapper(Uint8* data, int len, int senderHostIndex)
{
	_data = data;
	_len = len;
	_senderHostIndex = senderHostIndex;
}

SteamPacketWrapper::~SteamPacketWrapper()
{
	free(_data);
}

Uint8*& SteamPacketWrapper::data()
{
	return _data;
}

int& SteamPacketWrapper::len()
{
	return _len;
}

int SteamPacketWrapper::senderHostIndex() const
{
	return _senderHostIndex;
}

NetHandler::NetHandler()
{
	steam_packet_thread = nullptr;
	continue_multithreading_steam_packets = false;
	game_packets_lock = SDL_CreateMutex();
	continue_multithreading_steam_packets_lock = SDL_CreateMutex();
}

NetHandler::~NetHandler()
{
	//First, must join with the worker thread.
	printlog("Waiting for steam_packet_thread to finish...");
	stopMultithreadedPacketHandling();
	if ( steam_packet_thread )
	{
		SDL_WaitThread(steam_packet_thread, NULL); //Wait for the thread to finish.
	}
	printlog("Done.\n");

	SDL_DestroyMutex(game_packets_lock);
	game_packets_lock = nullptr;
	SDL_DestroyMutex(continue_multithreading_steam_packets_lock);
	continue_multithreading_steam_packets_lock = nullptr;

	//Free up packet memory.
	while (!game_packets.empty())
	{
		SteamPacketWrapper* packet = game_packets.front();
		delete packet;
		game_packets.pop();
	}
}

void NetHandler::toggleMultithreading(bool disableMultithreading)
{
	if ( disableMultithreading )
	{
		// stop the old thread...
		if ( steam_packet_thread )
		{
			printlog("Waiting for steam_packet_thread to finish...");
			stopMultithreadedPacketHandling();
			if ( steam_packet_thread )
			{
				SDL_WaitThread(steam_packet_thread, NULL); //Wait for the thread to finish.
			}
			printlog("Done.\n");
			SDL_DestroyMutex(game_packets_lock);
			game_packets_lock = nullptr;
			SDL_DestroyMutex(continue_multithreading_steam_packets_lock);
			continue_multithreading_steam_packets_lock = nullptr;
			steam_packet_thread = nullptr;
		}
	}
	else
	{
		// create the new thread...
		steam_packet_thread = nullptr;
		continue_multithreading_steam_packets = false;
		game_packets_lock = SDL_CreateMutex();
		continue_multithreading_steam_packets_lock = SDL_CreateMutex();
		initializeMultithreadedPacketHandling();
	}
}

void NetHandler::initializeMultithreadedPacketHandling()
{
#ifdef STEAMWORKS

	printlog("Initializing multithreaded packet handling.");

	steam_packet_thread = SDL_CreateThread(steamPacketThread, "steamPacketThread", static_cast<void* >(this));
	continue_multithreading_steam_packets = true;

#endif
}

void NetHandler::stopMultithreadedPacketHandling()
{
	SDL_LockMutex(continue_multithreading_steam_packets_lock); //NOTE: Will block.
	continue_multithreading_steam_packets = false;
	SDL_UnlockMutex(continue_multithreading_steam_packets_lock);
}

bool NetHandler::getContinueMultithreadingSteamPackets()
{
	return continue_multithreading_steam_packets;
	//SDL_UnlockMutex(continue_multithreading_steam_packets_lock);
}

void NetHandler::addGamePacket(SteamPacketWrapper* packet)
{
	if ( !disableMultithreadedSteamNetworking )
	{
		SDL_LockMutex(game_packets_lock);
		game_packets.push(packet);
		SDL_UnlockMutex(game_packets_lock);
	}
	else
	{
		game_packets.push(packet);
	}
}

SteamPacketWrapper* NetHandler::getGamePacket()
{
	SteamPacketWrapper* packet = nullptr;
	if ( !disableMultithreadedSteamNetworking )
	{
		SDL_LockMutex(game_packets_lock);
		if (!game_packets.empty())
		{
			packet = game_packets.front();
			game_packets.pop();
		}
		SDL_UnlockMutex(game_packets_lock);
	}
	else
	{
		if ( !game_packets.empty() )
		{
			packet = game_packets.front();
			game_packets.pop();
		}
	}
	return packet;
}

int EOSPacketThread(void* data)
{
#ifdef USE_EOS
	if ( !EOS.CurrentUserInfo.isValid() )
	{
		//logError("EOSPacketThread: Invalid local user Id: %s", CurrentUserInfo.getProductUserIdStr());
		return -1;
	}

	if ( !data )
	{
		return -1;    //Some generic error?
	}

	NetHandler& handler = *static_cast<NetHandler*>(data); //Basically, our this.
	EOS_ProductUserId remoteId = nullptr;
	Uint32 packetlen = 0;
	Uint32 bytes_read = 0;
	Uint8* packet = nullptr;
	bool run = true;
	std::queue<SteamPacketWrapper* > packets; //TODO: Expose to game? Use lock-free packets?

	while ( run )   //1. Check if thread is supposed to be running.
	{
		//2. Game not over. Grab/poll for packet.

		//while (handler.getContinueMultithreadingSteamPackets() && SteamNetworking()->IsP2PPacketAvailable(&packetlen)) //Burst read in a bunch of packets.
		while (EOS.HandleReceivedMessages(&remoteId) )
		{
			packetlen = std::min<uint32_t>(net_packet->len, NET_PACKET_SIZE - 1);
			//Read packets and push into queue.
			packet = static_cast<Uint8*>(malloc(packetlen));
			memcpy(packet, net_packet->data, packetlen);
			if ( !EOSFuncs::Helpers_t::isMatchingProductIds(remoteId, EOS.CurrentUserInfo.getProductUserIdHandle())
				&& net_packet->data[0] )
			{
				//Push packet into queue.
				//TODO: Use lock-free queues?
				packets.push(new SteamPacketWrapper(
					packet,
					packetlen,
					EOS.P2PConnectionInfo.getIndexFromPeerId(remoteId)
				));
				packet = nullptr;
			}
			if ( packet )
			{
				free(packet);
			}
		}


		//3. Now push our local packetstack onto the game's network stack.
		//Well, that is: analyze packet.
		//If packet is good, push into queue.
		//If packet is bad, loop back to start of function.

		while ( !packets.empty() )
		{
			//Copy over the packets read in so far, and expose them to the game.
			SteamPacketWrapper* packet = packets.front();
			packets.pop();
			handler.addGamePacket(packet);
		}

		run = false; // only run thread once if multithreading disabled.
	}
#endif // USE_EOS

	return 0;
}

int steamPacketThread(void* data)
{
#ifdef STEAMWORKS

	if (!data)
	{
		return -1;    //Some generic error?
	}

	NetHandler& handler = *static_cast<NetHandler* >(data); //Basically, our this.

	Uint32 packetlen = 0;
	Uint32 bytes_read = 0;
	CSteamID steam_id_remote;
	Uint8* packet = nullptr;
	CSteamID mySteamID = SteamUser()->GetSteamID();
	bool run = true;
	std::queue<SteamPacketWrapper* > packets; //TODO: Expose to game? Use lock-free packets?

	while (run)   //1. Check if thread is supposed to be running.
	{
		//2. Game not over. Grab/poll for packet.

		//while (handler.getContinueMultithreadingSteamPackets() && SteamNetworking()->IsP2PPacketAvailable(&packetlen)) //Burst read in a bunch of packets.
		while (SteamNetworking()->IsP2PPacketAvailable(&packetlen))
		{
			packetlen = std::min<uint32_t>(packetlen, NET_PACKET_SIZE - 1);
			//Read packets and push into queue.
			packet = static_cast<Uint8* >(malloc(packetlen));
			if (SteamNetworking()->ReadP2PPacket(packet, packetlen, &bytes_read, &steam_id_remote, 0))
			{
				if (packetlen > sizeof(uint32_t) && mySteamID.ConvertToUint64() != steam_id_remote.ConvertToUint64() && packet[0])
				{
					//Push packet into queue.
					//TODO: Use lock-free queues?
					int senderHostIndex = -1;
					for ( int host = 0; host < MAXPLAYERS; ++host )
					{
						if ( steamIDRemote[host]
							&& static_cast<CSteamID*>(steamIDRemote[host])->ConvertToUint64()
								== steam_id_remote.ConvertToUint64() )
						{
							senderHostIndex = host;
							break;
						}
					}
					packets.push(new SteamPacketWrapper(
						packet,
						packetlen,
						senderHostIndex
					));
					packet = nullptr;
				}
			}
			if (packet)
			{
				free(packet);
			}
		}


		//3. Now push our local packetstack onto the game's network stack.
		//Well, that is: analyze packet.
		//If packet is good, push into queue.
		//If packet is bad, loop back to start of function.

		while (!packets.empty())
		{
			//Copy over the packets read in so far, and expose them to the game.
			SteamPacketWrapper* packet = packets.front();
			packets.pop();
			handler.addGamePacket(packet);
		}

		if ( !disableMultithreadedSteamNetworking )
		{
			SDL_LockMutex(handler.continue_multithreading_steam_packets_lock);
			run = handler.getContinueMultithreadingSteamPackets();
			SDL_UnlockMutex(handler.continue_multithreading_steam_packets_lock);
		}
		else
		{
			run = false; // only run thread once if multithreading disabled.
		}
	}

#endif

	return 0; //If it isn't supposed to be running anymore, exit.

	//NOTE: This thread is to be created when the gameplay starts. NOT in the steam lobby.
	//If it's desired that it be created right when the network interfaces are opened, menu.c would need to be modified to support this, and the packet wrapper would need to include CSteamID.
}

/* ***** END MULTITHREADED STEAM PACKET HANDLING ***** */

void deleteMultiplayerSaveGames()
{
	if ( multiplayer != SERVER )
	{
		return;
	}

	if ( !gameModeManager.allowsSaves() )
	{
		return;
	}

	//Only delete saves if no players are left alive.
	bool lastAlive = true;

	//const int playersAtStartOfMap = numplayers;
	//int currentPlayers = 0;
	//for ( int i = 0; i < MAXPLAYERS; ++i )
	//{
	//	if ( !client_disconnected[i] )
	//	{
	//		++currentPlayers;
	//	}
	//}

	//if ( currentPlayers != playersAtStartOfMap )
	//{
	//	return;
	//}

	for ( int i = 0; i < MAXPLAYERS; ++i )
	{
		Stat* stat = nullptr;
		if ( players[i] && players[i]->entity && (stat = players[i]->entity->getStats()) && stat->HP > 0)
		{
			lastAlive = false;
		}
	}
	if ( !lastAlive )
	{
		return;
	}

	deleteSaveGame(multiplayer); // stops save scumming c:

	for ( int i = 1; i < MAXPLAYERS; ++i )
	{
		if ( client_disconnected[i] )
		{
			continue;
		}
		strcpy((char *)net_packet->data,"DSAV"); //Delete save game.
		net_packet->address.host = net_clients[i - 1].host;
		net_packet->address.port = net_clients[i - 1].port;
		net_packet->len = 4;
		sendPacketSafe(net_sock, -1, net_packet, i - 1);
	}
}

void handleScanPacket() {
    if (directConnect) {
        Uint32 hostname_len = (Uint32)strlen(MainMenu::getHostname());
        SDLNet_Write32(hostname_len, &net_packet->data[4]);
        for (int c = 0; c < hostname_len; ++c) {
            net_packet->data[8 + c] = MainMenu::getHostname()[c];
        }
        Uint32 offset = 8 + hostname_len;
        int numplayers = 0;
        for (int c = 0; c < MAXPLAYERS; ++c) {
            if (!LanDiscovery::advertisedDisconnected(
					headless, c, client_disconnected[c])) {
                ++numplayers;
            }
        }
        SDLNet_Write32(numplayers, &net_packet->data[offset]);
        net_packet->data[offset + 4] = LanDiscovery::advertisedLocked(
			!intro,
			headless && headlessLateJoinRequested);
        SDLNet_Write32(svFlags, &net_packet->data[offset + 5]);
        net_packet->len = offset + 9;
		if (headless)
		{
			const std::size_t extensionBytes = LanDiscovery::encodeExtension(
				&net_packet->data[net_packet->len],
				NET_PACKET_SIZE - static_cast<std::size_t>(net_packet->len),
				headlessServerPort,
				true,
				headlessLateJoinRequested);
			net_packet->len += static_cast<int>(extensionBytes);
		}
        sendPacket(net_sock, -1, net_packet, 0);
    }
}

PingNetworkStatus_t PingNetworkStatus[MAXPLAYERS];
bool PingNetworkStatus_t::bEnabled = false;
int PingNetworkStatus_t::pingLimitGreen = 100;
int PingNetworkStatus_t::pingLimitYellow = 150;
int PingNetworkStatus_t::pingLimitOrange = 250;
bool PingNetworkStatus_t::pingHUDDisplayGreen = false;
bool PingNetworkStatus_t::pingHUDDisplayYellow = false;
bool PingNetworkStatus_t::pingHUDDisplayOrange = true;
bool PingNetworkStatus_t::pingHUDDisplayRed = true;
bool PingNetworkStatus_t::pingHUDShowOKBriefly = true;
bool PingNetworkStatus_t::pingHUDShowNumericValue = false;
void PingNetworkStatus_t::reset()
{
	for ( int i = 0; i < MAXPLAYERS; ++i )
	{
		PingNetworkStatus[i].clear();
	}
}

void PingNetworkStatus_t::receive()
{
	if ( !net_packet || net_packet->len < 9 )
	{
		printlog(
			"[NET]: ignoring truncated PNGR packet"
		);
		return;
	}

	const int player =
		decodeGameplayPacketPlayerIndex(
			net_packet->data[4]
		);
	if ( player < 0 )
	{
		return;
	}
	auto& p = PingNetworkStatus[player];
	Uint32 seq = SDLNet_Read32(&net_packet->data[5]);

	auto find = p.pings.find(seq);
	if ( find != p.pings.end() )
	{
		if ( seq > p.lastSequence )
		{
			p.lastPingtime = SDL_GetTicks() - find->second; // update our displayed ping value
			p.lastSequence = seq;
			//messagePlayer(clientnum, MESSAGE_DEBUG, "[%d]: %4d ms", player, p.lastPingtime);
		}
		p.pings.erase(seq);

	}
	std::vector<Uint32> toErase;
	for ( auto& keypair : p.pings )
	{
		if ( keypair.first < seq )
		{
			toErase.push_back(keypair.first);
		}
	}
	for ( auto& key : toErase )
	{
		p.pings.erase(key);
	}
}

void PingNetworkStatus_t::respond()
{
	if ( !net_packet || net_packet->len < 9 )
	{
		printlog(
			"[NET]: ignoring truncated PNGU packet"
		);
		return;
	}

	const int player =
		decodeGameplayPacketPlayerIndex(
			net_packet->data[4]
		);
	if ( player < 0 )
	{
		return;
	}

	strcpy((char*)net_packet->data, "PNGR");
	net_packet->data[4] = clientnum;
	net_packet->len = 9;

	if ( multiplayer == CLIENT )
	{
		net_packet->address.host = net_server.host;
		net_packet->address.port = net_server.port;
		sendPacketSafe(net_sock, -1, net_packet, 0);
	}
	else if ( multiplayer == SERVER )
	{
		if ( player > 0 )
		{
			net_packet->address.host = net_clients[player - 1].host;
			net_packet->address.port = net_clients[player - 1].port;
			sendPacketSafe(net_sock, -1, net_packet, player - 1);
		}
	}
}

void PingNetworkStatus_t::saveDisplayMillis(bool forceUpdate)
{
	if ( oldestSequenceTicks > 0 )
	{
		displayMillisImmediate = std::max(SDL_GetTicks() - oldestSequenceTicks, lastPingtime);
	}
	else
	{
		displayMillisImmediate = lastPingtime;
	}
	if ( (ticks % (TICKS_PER_SECOND * 2) == 0) || displayMillis == 0 || forceUpdate )
	{
		displayMillis = displayMillisImmediate;
	}
}

void PingNetworkStatus_t::update()
{
	if ( !bEnabled )
	{
		reset();
		return;
	}
	if ( !(multiplayer == CLIENT || multiplayer == SERVER) )
	{
		reset();
		return;
	}
	if ( !net_packet ) { return; }

	if ( true )
	{
		bool updated = false;
		if ( multiplayer == CLIENT ) 
		{
			if ( PingNetworkStatus[0].needsUpdate || (ticks % (5 * TICKS_PER_SECOND) == 0) )
			{
				updated = true;
				PingNetworkStatus[0].needsUpdate = false;

				strcpy((char*)net_packet->data, "PNGU");
				net_packet->data[4] = clientnum;
				net_packet->len = 9;

				++PingNetworkStatus[0].sequence;
				SDLNet_Write32(PingNetworkStatus[0].sequence, &net_packet->data[5]);
				net_packet->address.host = net_server.host;
				net_packet->address.port = net_server.port;
				sendPacketSafe(net_sock, -1, net_packet, 0);

				PingNetworkStatus[0].pings[PingNetworkStatus[0].sequence] = SDL_GetTicks();
			}
		}
		else if ( multiplayer == SERVER ) 
		{
			for ( int i = 1; i < MAXPLAYERS; i++ ) 
			{
				if ( client_disconnected[i] ) 
				{
					continue;
				}

				if ( PingNetworkStatus[i].needsUpdate || (ticks % (5 * TICKS_PER_SECOND) == 0) )
				{
					updated = true;
					PingNetworkStatus[i].needsUpdate = false;

					strcpy((char*)net_packet->data, "PNGU");
					net_packet->data[4] = clientnum;
					net_packet->len = 9;

					++PingNetworkStatus[i].sequence;
					SDLNet_Write32(PingNetworkStatus[i].sequence, &net_packet->data[5]);
					net_packet->address.host = net_clients[i - 1].host;
					net_packet->address.port = net_clients[i - 1].port;
					sendPacketSafe(net_sock, -1, net_packet, i - 1);

					PingNetworkStatus[i].pings[PingNetworkStatus[i].sequence] = SDL_GetTicks();
				}
			}
		}

		if ( updated )
		{
			for ( int i = 0; i < MAXPLAYERS; ++i )
			{
				auto& p = PingNetworkStatus[i];
				while ( p.pings.size() >= 10 )
				{
					Uint32 minSequence = 0;
					for ( auto& keypairs : p.pings )
					{
						if ( minSequence == 0 )
						{
							minSequence = keypairs.first;
						}
						else if ( keypairs.first < minSequence )
						{
							minSequence = keypairs.first;
						}
					}

					if ( minSequence > 0 )
					{
						p.pings.erase(minSequence);
					}
				}
			}
		}
	}

	for ( int i = 0; i < MAXPLAYERS; ++i )
	{
		auto& p = PingNetworkStatus[i];
		if ( client_disconnected[i] )
		{
			p.clear();
			continue;
		}
		Uint32 minSequence = 0;
		for ( auto& keypairs : p.pings )
		{
			if ( minSequence == 0 )
			{
				minSequence = keypairs.first;
			}
			else if ( keypairs.first < minSequence )
			{
				minSequence = keypairs.first;
			}

		}
		if ( minSequence > 0 )
		{
			p.oldestSequenceTicks = p.pings[minSequence];
		}
		else
		{
			p.oldestSequenceTicks = 0;
		}

		p.saveDisplayMillis();
	}
}
