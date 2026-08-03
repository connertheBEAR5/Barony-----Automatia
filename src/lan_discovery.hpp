/*-------------------------------------------------------------------------------

	BARONY AUTOMATIA
	File: lan_discovery.hpp
	Desc: Backward-compatible LAN discovery metadata and dedicated-host policy.

-------------------------------------------------------------------------------*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace LanDiscovery
{
	constexpr std::size_t extensionSize = 7;
	constexpr std::uint8_t dedicatedFlag = 0x01;
	constexpr std::uint8_t lateJoinFlag = 0x02;

	struct Extension
	{
		bool present = false;
		bool dedicated = false;
		bool lateJoin = false;
		std::uint16_t gamePort = 0;
	};

	inline bool isDedicatedHostSlot(bool dedicated, int playerIndex)
	{
		return dedicated && playerIndex == 0;
	}

	inline bool advertisedDisconnected(
		bool dedicated,
		int playerIndex,
		bool disconnected)
	{
		return isDedicatedHostSlot(dedicated, playerIndex) || disconnected;
	}

	inline bool advertisedLocked(bool gameStarted, bool lateJoinEnabled)
	{
		return gameStarted && !lateJoinEnabled;
	}

	inline bool browserShouldInclude(int playerCount, bool dedicated)
	{
		return playerCount > 0 || dedicated;
	}

	inline std::size_t encodeExtension(
		std::uint8_t* destination,
		std::size_t capacity,
		std::uint16_t gamePort,
		bool dedicated,
		bool lateJoin)
	{
		if (!destination || capacity < extensionSize || gamePort == 0)
		{
			return 0;
		}
		std::memcpy(destination, "AUT1", 4);
		destination[4] = static_cast<std::uint8_t>(gamePort >> 8);
		destination[5] = static_cast<std::uint8_t>(gamePort & 0xff);
		destination[6] =
			(dedicated ? dedicatedFlag : 0)
			| (lateJoin ? lateJoinFlag : 0);
		return extensionSize;
	}

	inline Extension decodeExtension(
		const std::uint8_t* source,
		std::size_t size)
	{
		Extension result;
		if (!source || size < extensionSize
			|| std::memcmp(source, "AUT1", 4) != 0)
		{
			return result;
		}
		result.gamePort = static_cast<std::uint16_t>(
			(static_cast<std::uint16_t>(source[4]) << 8)
			| source[5]);
		if (result.gamePort == 0)
		{
			return Extension{};
		}
		result.present = true;
		result.dedicated = (source[6] & dedicatedFlag) != 0;
		result.lateJoin = (source[6] & lateJoinFlag) != 0;
		return result;
	}
}
