/*-------------------------------------------------------------------------------

	BARONY AUTOMATIA
	File: quest_ownership.cpp
	Desc: Durable, scope-aware custom-dialogue quest ownership.

-------------------------------------------------------------------------------*/

#include "quest_ownership.hpp"
#include "party_manager.hpp"

#include <algorithm>
#include <charconv>
#include <limits>

namespace AutomatiaQuest
{
namespace
{
std::string hexEncode(const std::string& value)
{
	static constexpr char digits[] = "0123456789abcdef";
	std::string result;
	result.reserve(value.size() * 2);
	for (const unsigned char character : value)
	{
		result.push_back(digits[character >> 4U]);
		result.push_back(digits[character & 0x0fU]);
	}
	return result;
}

bool safeQuestId(const std::string& value)
{
	if (value.empty() || value.size() > 255)
	{
		return false;
	}
	return std::none_of(
		value.begin(), value.end(),
		[](const unsigned char character)
		{
			return character < 0x20 || character == 0x7f;
		});
}
}

const char* scopeName(const Scope scope)
{
	switch (scope)
	{
		case Scope::Player: return "player";
		case Scope::Party: return "party";
		case Scope::World: return "world";
		default: return "player";
	}
}

bool scopeFromName(const std::string& name, Scope& scope)
{
	if (name == "player" || name == "personal")
	{
		scope = Scope::Player;
		return true;
	}
	if (name == "party")
	{
		scope = Scope::Party;
		return true;
	}
	if (name == "world")
	{
		scope = Scope::World;
		return true;
	}
	return false;
}

bool ObjectiveDefinition::operator==(
	const ObjectiveDefinition& other) const
{
	return id == other.id
		&& text == other.text
		&& completedText == other.completedText
		&& progressVariable == other.progressVariable
		&& stage == other.stage
		&& target == other.target
		&& defeatId == other.defeatId
		&& optional == other.optional
		&& markerMap == other.markerMap
		&& markerX == other.markerX
		&& markerY == other.markerY
		&& markerPlayableFloor == other.markerPlayableFloor
		&& markerWholeColumn == other.markerWholeColumn;
}

Scope Definition::effectiveScope() const
{
	return sharedOwnershipEnabled() ? authoredScope : Scope::Player;
}

bool Definition::sharedOwnershipEnabled() const
{
	return dialogueSchemaVersion
		>= SHARED_OWNERSHIP_DIALOGUE_SCHEMA_VERSION;
}

bool Definition::immutableMetadataMatches(const Definition& other) const
{
	return questId == other.questId
		&& title == other.title
		&& summary == other.summary
		&& objective == other.objective
		&& completedText == other.completedText
		&& failedText == other.failedText
		&& originLabel == other.originLabel
		&& originMap == other.originMap
		&& originX == other.originX
		&& originY == other.originY
		&& originPlayableFloor == other.originPlayableFloor
		&& originWholeColumn == other.originWholeColumn
		&& originTrackNpc == other.originTrackNpc
		&& originPersistentId == other.originPersistentId
		&& dialogueSchemaVersion == other.dialogueSchemaVersion
		&& authoredScope == other.authoredScope
		&& repeatable == other.repeatable
		&& objectives == other.objectives;
}

bool DefinitionRegistry::registerDefinition(
	const Definition& definition,
	std::string& error)
{
	error.clear();
	if (!safeQuestId(definition.questId)
		|| !safeQuestId(definition.dialogueId)
		|| definition.title.empty()
		|| definition.dialogueSchemaVersion
			< LEGACY_DIALOGUE_SCHEMA_VERSION
		|| definition.dialogueSchemaVersion
			> CURRENT_DIALOGUE_SCHEMA_VERSION)
	{
		error = "quest definition has invalid identity or schema metadata";
		return false;
	}

	const auto dialogue = questIdByDialogueId.find(definition.dialogueId);
	if (dialogue != questIdByDialogueId.end()
		&& dialogue->second != definition.questId)
	{
		error = "dialogue ID is already registered to another quest";
		return false;
	}

	auto found = byQuestId.find(definition.questId);
	if (found == byQuestId.end())
	{
		Entry entry;
		entry.definition = definition;
		entry.dialogueIds.push_back(definition.dialogueId);
		byQuestId.emplace(definition.questId, std::move(entry));
		questIdByDialogueId[definition.dialogueId] = definition.questId;
		return true;
	}

	if (!found->second.definition.immutableMetadataMatches(definition))
	{
		error = "quest ID is already registered with conflicting immutable metadata";
		return false;
	}

	if (std::find(found->second.dialogueIds.begin(),
		found->second.dialogueIds.end(), definition.dialogueId)
		== found->second.dialogueIds.end())
	{
		found->second.dialogueIds.push_back(definition.dialogueId);
		std::sort(found->second.dialogueIds.begin(),
			found->second.dialogueIds.end());
	}
	questIdByDialogueId[definition.dialogueId] = definition.questId;
	return true;
}

const Definition* DefinitionRegistry::findByQuestId(
	const std::string& questId) const
{
	const auto found = byQuestId.find(questId);
	return found == byQuestId.end() ? nullptr : &found->second.definition;
}

const Definition* DefinitionRegistry::findByDialogueId(
	const std::string& dialogueId) const
{
	const auto quest = questIdByDialogueId.find(dialogueId);
	return quest == questIdByDialogueId.end()
		? nullptr : findByQuestId(quest->second);
}

std::vector<std::string> DefinitionRegistry::dialogueIdsForQuest(
	const std::string& questId) const
{
	const auto found = byQuestId.find(questId);
	return found == byQuestId.end()
		? std::vector<std::string>{} : found->second.dialogueIds;
}

std::vector<std::string> DefinitionRegistry::questIds() const
{
	std::vector<std::string> result;
	result.reserve(byQuestId.size());
	for (const auto& entry : byQuestId)
	{
		result.push_back(entry.first);
	}
	std::sort(result.begin(), result.end());
	return result;
}

void DefinitionRegistry::clear()
{
	byQuestId.clear();
	questIdByDialogueId.clear();
}

DefinitionRegistry& definitionRegistry()
{
	static DefinitionRegistry registry;
	return registry;
}

bool Owner::isValid() const
{
	switch (scope)
	{
		case Scope::Player:
			return player.isValid();
		case Scope::Party:
			return partyId != AutomatiaParty::INVALID_PARTY_ID;
		case Scope::World:
			return true;
		default:
			return false;
	}
}

std::string Owner::storagePrefix() const
{
	if (!isValid())
	{
		return {};
	}
	switch (scope)
	{
		case Scope::Player:
			return std::string("player:")
				+ AutomatiaParty::durableIdentityKindName(player.kind)
				+ ":" + hexEncode(player.value) + ":";
		case Scope::Party:
			return "party:" + std::to_string(partyId) + ":";
		case Scope::World:
			return "world:";
		default:
			return {};
	}
}

Resolution resolve(
	const Definition& definition,
	const ActorContext& actor)
{
	Resolution result;
	if (!actor.authenticated || !actor.player.isValid())
	{
		result.error = "quest access requires an authenticated durable player identity";
		return result;
	}
	if (!safeQuestId(definition.questId))
	{
		result.error = "quest definition has an invalid quest ID";
		return result;
	}

	result.owner.scope = definition.effectiveScope();
	switch (result.owner.scope)
	{
		case Scope::Player:
			result.owner.player = actor.player;
			break;
		case Scope::Party:
			if (actor.partyId == AutomatiaParty::INVALID_PARTY_ID)
			{
				result.error = "authenticated player is not a member of a party";
				return result;
			}
			result.owner.partyId = actor.partyId;
			break;
		case Scope::World:
			break;
	}

	result.stateKey = makeStateKey(result.owner, definition.questId);
	result.allowed = !result.stateKey.empty();
	if (!result.allowed)
	{
		result.error = "could not construct durable quest owner key";
	}
	return result;
}

Resolution resolveAuthoritative(
	const Definition& definition,
	const AutomatiaParty::DurablePlayerIdentity& authenticatedPlayer,
	const AutomatiaParty::PartyManager& partyManager)
{
	ActorContext actor;
	actor.player = authenticatedPlayer;
	actor.partyId = partyManager.partyIdForPlayer(authenticatedPlayer);
	actor.authenticated = authenticatedPlayer.isValid();
	return resolve(definition, actor);
}

std::string makeStateKey(const Owner& owner, const std::string& questId)
{
	if (!safeQuestId(questId))
	{
		return {};
	}
	const std::string prefix = owner.storagePrefix();
	return prefix.empty() ? std::string{} : prefix + questId;
}

bool stateKeyBelongsToOwner(
	const std::string& stateKey,
	const Owner& owner,
	std::string* questId)
{
	const std::string prefix = owner.storagePrefix();
	if (prefix.empty() || stateKey.size() <= prefix.size()
		|| stateKey.compare(0, prefix.size(), prefix) != 0)
	{
		return false;
	}
	const std::string suffix = stateKey.substr(prefix.size());
	if (!safeQuestId(suffix))
	{
		return false;
	}
	if (questId)
	{
		*questId = suffix;
	}
	return true;
}

bool decodeSharedStateKey(
	const std::string& stateKey,
	Scope& scope,
	AutomatiaParty::PartyID& partyId,
	std::string& questId)
{
	partyId = AutomatiaParty::INVALID_PARTY_ID;
	questId.clear();
	if (stateKey.rfind("world:", 0) == 0)
	{
		questId = stateKey.substr(6);
		if (!safeQuestId(questId))
		{
			questId.clear();
			return false;
		}
		scope = Scope::World;
		return true;
	}
	if (stateKey.rfind("party:", 0) != 0)
	{
		return false;
	}
	const std::size_t separator = stateKey.find(':', 6);
	if (separator == std::string::npos || separator == 6)
	{
		return false;
	}
	const char* begin = stateKey.data() + 6;
	const char* end = stateKey.data() + separator;
	AutomatiaParty::PartyID parsed = 0;
	const std::from_chars_result conversion =
		std::from_chars(begin, end, parsed);
	questId = stateKey.substr(separator + 1);
	if (conversion.ec != std::errc{} || conversion.ptr != end
		|| parsed == AutomatiaParty::INVALID_PARTY_ID
		|| !safeQuestId(questId))
	{
		questId.clear();
		return false;
	}
	scope = Scope::Party;
	partyId = parsed;
	return true;
}

}
