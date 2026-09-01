/*-------------------------------------------------------------------------------

	BARONY AUTOMATIA
	File: quest_ownership.hpp
	Desc: Durable, scope-aware custom-dialogue quest ownership.

-------------------------------------------------------------------------------*/

#pragma once

#include "automatia_identity.hpp"
#include "party_types.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace AutomatiaParty
{
class PartyManager;
}

namespace AutomatiaQuest
{

/*
 * Dialogue schema 1 is intentionally legacy: authored party/world values use
 * personal ownership. Schema 2 is the explicit opt-in to shared ownership.
 */
constexpr int LEGACY_DIALOGUE_SCHEMA_VERSION = 1;
constexpr int SHARED_OWNERSHIP_DIALOGUE_SCHEMA_VERSION = 2;
constexpr int CURRENT_DIALOGUE_SCHEMA_VERSION =
	SHARED_OWNERSHIP_DIALOGUE_SCHEMA_VERSION;

enum class Scope : std::uint8_t
{
	Player = 1,
	Party = 2,
	World = 3
};

const char* scopeName(Scope scope);
bool scopeFromName(const std::string& name, Scope& scope);

struct ObjectiveDefinition
{
	std::string id;
	std::string text;
	std::string completedText;
	std::string progressVariable;
	std::int32_t stage = 0;
	std::int32_t target = 1;
	std::int32_t defeatId = 0;
	bool optional = false;
	std::string markerMap;
	std::int32_t markerX = -1;
	std::int32_t markerY = -1;
	std::int32_t markerPlayableFloor = 0;
	bool markerWholeColumn = true;

	bool operator==(const ObjectiveDefinition& other) const;
};

struct Definition
{
	std::string questId;
	std::string dialogueId;
	std::string title;
	std::string summary;
	std::string objective;
	std::string completedText;
	std::string failedText;

	std::string originLabel;
	std::string originMap;
	std::int32_t originX = -1;
	std::int32_t originY = -1;
	std::int32_t originPlayableFloor = 0;
	bool originWholeColumn = true;
	bool originTrackNpc = false;
	std::int32_t originPersistentId = 0;

	int dialogueSchemaVersion = LEGACY_DIALOGUE_SCHEMA_VERSION;
	Scope authoredScope = Scope::Player;
	bool repeatable = false;
	std::vector<ObjectiveDefinition> objectives;

	Scope effectiveScope() const;
	bool sharedOwnershipEnabled() const;
	bool immutableMetadataMatches(const Definition& other) const;
};

/*
 * Immutable quest definitions are indexed both by quest_id and dialogue ID.
 * Multiple dialogue graphs may reference one quest only when their immutable
 * quest metadata agrees exactly.
 */
class DefinitionRegistry
{
public:
	bool registerDefinition(const Definition& definition, std::string& error);
	const Definition* findByQuestId(const std::string& questId) const;
	const Definition* findByDialogueId(const std::string& dialogueId) const;
	std::vector<std::string> dialogueIdsForQuest(
		const std::string& questId) const;
	std::vector<std::string> questIds() const;
	void clear();

private:
	struct Entry
	{
		Definition definition;
		std::vector<std::string> dialogueIds;
	};

	std::unordered_map<std::string, Entry> byQuestId;
	std::unordered_map<std::string, std::string> questIdByDialogueId;
};

DefinitionRegistry& definitionRegistry();

struct ActorContext
{
	AutomatiaParty::DurablePlayerIdentity player;
	AutomatiaParty::PartyID partyId = AutomatiaParty::INVALID_PARTY_ID;
	bool authenticated = false;
};

struct Owner
{
	Scope scope = Scope::Player;
	AutomatiaParty::DurablePlayerIdentity player;
	AutomatiaParty::PartyID partyId = AutomatiaParty::INVALID_PARTY_ID;

	bool isValid() const;
	std::string storagePrefix() const;
};

struct Resolution
{
	bool allowed = false;
	Owner owner;
	std::string stateKey;
	std::string error;
};

/*
 * The caller supplies only an authenticated actor context. Party/world owner
 * selection is derived from immutable definition scope; no caller-supplied
 * PartyID or spatial value participates in resolution.
 */
Resolution resolve(
	const Definition& definition,
	const ActorContext& actor);

/* Server path: PartyID is looked up from PartyManager and cannot be claimed. */
Resolution resolveAuthoritative(
	const Definition& definition,
	const AutomatiaParty::DurablePlayerIdentity& authenticatedPlayer,
	const AutomatiaParty::PartyManager& partyManager);

std::string makeStateKey(const Owner& owner, const std::string& questId);
bool stateKeyBelongsToOwner(
	const std::string& stateKey,
	const Owner& owner,
	std::string* questId = nullptr);
bool decodeSharedStateKey(
	const std::string& stateKey,
	Scope& scope,
	AutomatiaParty::PartyID& partyId,
	std::string& questId);

}
