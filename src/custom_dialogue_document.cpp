/*-------------------------------------------------------------------------------

	BARONY AUTOMATIA
	File: custom_dialogue_document.cpp
	Desc: Lossless custom-dialogue authoring model and shared schema services.

-------------------------------------------------------------------------------*/

#include "custom_dialogue_document.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace automatia
{
namespace dialogue
{
namespace
{

using rapidjson::Value;

bool contains(const std::vector<std::string>& values, const std::string& value)
{
	return std::find(values.begin(), values.end(), value) != values.end();
}

std::string normalizeID(const std::string& value)
{
	std::string result;
	result.reserve(value.size());
	bool separator = false;
	for ( const unsigned char character : value )
	{
		if ( std::isalnum(character) || character == '_' || character == '-'
			|| character == '.' )
		{
			result.push_back(static_cast<char>(std::tolower(character)));
			separator = false;
		}
		else if ( !result.empty() && !separator )
		{
			result.push_back('_');
			separator = true;
		}
	}
	while ( !result.empty() && result.back() == '_' )
	{
		result.pop_back();
	}
	return result;
}

bool isSafeStableID(const std::string& value)
{
	const std::size_t separator = value.find(':');
	if ( value.empty() || value.size() > 127 || separator == std::string::npos
		|| separator == 0 || separator + 1 >= value.size() )
	{
		return false;
	}
	return std::none_of(value.begin(), value.end(), [](const unsigned char c)
	{
		return std::isspace(c) || std::iscntrl(c) || c == '/' || c == '\\';
	});
}

bool parseNonnegativeIntegerString(const std::string& value, int& number)
{
	if ( value.empty() || !std::all_of(value.begin(), value.end(), [](const unsigned char c)
		{ return std::isdigit(c); }) )
	{
		return false;
	}
	char* end = nullptr;
	const long parsed = std::strtol(value.c_str(), &end, 10);
	if ( !end || *end != '\0' || parsed < 0
		|| parsed > std::numeric_limits<int>::max() )
	{
		return false;
	}
	number = static_cast<int>(parsed);
	return true;
}

Location atDocument(const std::string& path)
{
	Location location;
	location.path = path;
	return location;
}

void addIssue(std::vector<Issue>& issues, const Severity severity,
	const std::string& code, const std::string& message, const Location& location)
{
	issues.push_back(Issue{ severity, code, message, location });
}

bool memberIsString(const Value& object, const char* key)
{
	return object.IsObject() && object.HasMember(key) && object[key].IsString();
}

bool memberIsInt(const Value& object, const char* key)
{
	return object.IsObject() && object.HasMember(key) && object[key].IsInt();
}

bool memberIsBool(const Value& object, const char* key)
{
	return object.IsObject() && object.HasMember(key) && object[key].IsBool();
}

bool nonemptyString(const Value& object, const char* key)
{
	return memberIsString(object, key) && object[key].GetStringLength() > 0;
}

void reportUnknownMembers(std::vector<Issue>& issues, const Value& object,
	const std::unordered_set<std::string>& known, const Location& location,
	const std::string& owner)
{
	if ( !object.IsObject() )
	{
		return;
	}
	for ( auto member = object.MemberBegin(); member != object.MemberEnd(); ++member )
	{
		const std::string key = member->name.GetString();
		if ( known.find(key) == known.end() )
		{
			Location fieldLocation = location;
			fieldLocation.path += "." + key;
			addIssue(issues, Severity::Info, "unknown_field",
				owner + " extension field '" + key
				+ "' is preserved in Advanced JSON.", fieldLocation);
		}
	}
}

bool isComparison(const std::string& comparison, const bool allowNotEquals)
{
	return comparison == "equals" || comparison == "at_least"
		|| comparison == "at_most"
		|| (allowNotEquals && comparison == "not_equals");
}

void validateIdentifier(std::vector<Issue>& issues, const Value& object,
	const char* key, const Location& location, const bool required)
{
	if ( !object.HasMember(key) )
	{
		if ( required )
		{
			addIssue(issues, Severity::Error, "missing_id",
				std::string("Missing required '") + key + "'.", location);
		}
		return;
	}
	if ( !object[key].IsString() || object[key].GetStringLength() == 0 )
	{
		addIssue(issues, Severity::Error, "invalid_id",
			std::string("'") + key + "' must be a non-empty string.", location);
		return;
	}
	const std::string raw = object[key].GetString();
	if ( normalizeID(raw).empty() )
	{
		addIssue(issues, Severity::Error, "invalid_id",
			std::string("'") + key + "' normalizes to an empty ID.", location);
	}
	else if ( normalizeID(raw) != raw )
	{
		addIssue(issues, Severity::Warning, "normalized_id",
			std::string("'") + key + "' is normalized by the runtime to '"
			+ normalizeID(raw) + "'.", location);
	}
}

void validateItemReference(std::vector<Issue>& issues, const Value& reference,
	const ValidationOptions& options, const Location& location)
{
	if ( !reference.IsObject() )
	{
		addIssue(issues, Severity::Error, "invalid_item_reference",
			"Item reference must be an object.", location);
		return;
	}

	const bool hasItem = reference.HasMember("item");
	const bool hasStableID = reference.HasMember("stable_id");
	if ( !hasItem && !hasStableID )
	{
		addIssue(issues, Severity::Error, "missing_item",
			"Item reference needs 'item' or persistent 'stable_id'.", location);
	}
	if ( hasItem && !reference["item"].IsString() )
	{
		addIssue(issues, Severity::Error, "invalid_item",
			"Item reference 'item' must be a string alias or vanilla numeric ID.", location);
	}

	if ( hasStableID )
	{
		if ( !options.allowStableItemIDs )
		{
			addIssue(issues, Severity::Error, "stable_item_unsupported",
				"This runtime does not allow S.A.M. stable item references.", location);
		}
		else if ( !reference["stable_id"].IsString()
			|| !isSafeStableID(reference["stable_id"].GetString()) )
		{
			addIssue(issues, Severity::Error, "invalid_stable_item_id",
				"stable_id must be a safe namespaced ID such as mod:item.", location);
		}
		else if ( options.stableItemAvailable
			&& !options.stableItemAvailable(reference["stable_id"].GetString()) )
		{
			addIssue(issues, Severity::Warning, "missing_stable_item",
				"S.A.M. item '" + std::string(reference["stable_id"].GetString())
				+ "' is unavailable; its stable reference is preserved.", location);
		}
	}
	else if ( hasItem && reference["item"].IsString() )
	{
		const std::string item = normalizeID(reference["item"].GetString());
		int numeric = -1;
		const bool knownAlias = item == "torch" || item == "tool_torch"
			|| item == "healing_potion" || item == "potion_healing";
		if ( parseNonnegativeIntegerString(item, numeric) )
		{
			if ( options.vanillaItemCount > 0 && numeric >= options.vanillaItemCount )
			{
				addIssue(issues, Severity::Error, "session_item_id",
					"Custom runtime item IDs are session-local; use stable_id instead.", location);
			}
		}
		else if ( !knownAlias )
		{
			addIssue(issues, Severity::Error, "unsupported_item_alias",
				"Unsupported dialogue item alias '" + item + "'.", location);
		}
	}

	if ( reference.HasMember("count")
		&& (!reference["count"].IsInt() || reference["count"].GetInt() <= 0) )
	{
		addIssue(issues, Severity::Error, "invalid_item_count",
			"Item count must be a positive integer.", location);
	}
}

void validateCondition(std::vector<Issue>& issues, const Value& condition,
	const ValidationOptions& options, const Location& location, const bool nodeCondition)
{
	if ( !condition.IsObject() || !memberIsString(condition, "type") )
	{
		addIssue(issues, Severity::Error, "invalid_condition",
			"Condition must be an object with a string 'type'.", location);
		return;
	}

	const std::string type = normalizeID(condition["type"].GetString());
	const bool known = nodeCondition ? isNodeConditionType(type)
		: isChoiceConditionType(type);
	if ( !known )
	{
		addIssue(issues, Severity::Error, "unsupported_condition",
			"Condition type '" + type + "' is not supported on this "
			+ (nodeCondition ? "node." : "choice."), location);
		return;
	}

	if ( nodeCondition
		&& (!memberIsInt(condition, "true_node") || !memberIsInt(condition, "false_node")) )
	{
		addIssue(issues, Severity::Error, "missing_condition_branch",
			"Node conditions require integer true_node and false_node destinations.", location);
	}

	if ( type == "has_item" )
	{
		validateItemReference(issues, condition, options, location);
		if ( nodeCondition && condition.HasMember("consume")
			&& !condition["consume"].IsBool() )
		{
			addIssue(issues, Severity::Error, "invalid_consume",
				"Node item consume must be Boolean.", location);
		}
	}
	else if ( type == "has_gold" )
	{
		if ( !memberIsInt(condition, "amount") || condition["amount"].GetInt() < 0 )
		{
			addIssue(issues, Severity::Error, "invalid_gold_amount",
				"has_gold requires a non-negative integer amount.", location);
		}
		if ( nodeCondition && condition.HasMember("consume")
			&& !condition["consume"].IsBool() )
		{
			addIssue(issues, Severity::Error, "invalid_consume",
				"Node gold consume must be Boolean.", location);
		}
	}
	else if ( type == "quest_started" || type == "quest_accepted"
		|| type == "quest_completed" || type == "quest_failed" )
	{
		validateIdentifier(issues, condition, "quest", location, true);
	}
	else if ( type == "quest_stage" )
	{
		validateIdentifier(issues, condition, "quest", location, true);
		if ( !memberIsInt(condition, "stage") )
		{
			addIssue(issues, Severity::Error, "invalid_quest_stage",
				"quest_stage requires an integer stage.", location);
		}
		const std::string comparison = memberIsString(condition, "comparison")
			? normalizeID(condition["comparison"].GetString()) : "equals";
		if ( !isComparison(comparison, !nodeCondition) )
		{
			addIssue(issues, Severity::Error, "invalid_comparison",
				"Unsupported quest-stage comparison '" + comparison + "'.", location);
		}
	}
	else if ( type == "objective_completed" || type == "objective_incomplete" )
	{
		validateIdentifier(issues, condition, "quest", location, true);
		validateIdentifier(issues, condition, "objective", location, true);
	}
	else if ( type == "node_seen" )
	{
		validateIdentifier(issues, condition, "node", location, true);
	}
	else if ( type == "world_flag" || type == "npc_flag" )
	{
		validateIdentifier(issues, condition, "id", location, true);
		if ( condition.HasMember("value") && !condition["value"].IsBool() )
		{
			addIssue(issues, Severity::Error, "invalid_flag_value",
				"Flag condition value must be Boolean.", location);
		}
	}
	else if ( type == "world_variable" || type == "npc_variable" )
	{
		validateIdentifier(issues, condition, "id", location, true);
		if ( !memberIsInt(condition, "value") )
		{
			addIssue(issues, Severity::Error, "invalid_variable_value",
				"Variable condition requires an integer value.", location);
		}
		const std::string comparison = memberIsString(condition, "comparison")
			? normalizeID(condition["comparison"].GetString()) : "equals";
		if ( !isComparison(comparison, true) )
		{
			addIssue(issues, Severity::Error, "invalid_comparison",
				"Unsupported variable comparison '" + comparison + "'.", location);
		}
	}
	if ( !nodeCondition && type != "quest_stage"
		&& type != "world_variable" && type != "npc_variable"
		&& condition.HasMember("comparison") )
	{
		if ( !condition["comparison"].IsString()
			|| !isComparison(normalizeID(condition["comparison"].GetString()), true) )
		{
			addIssue(issues, Severity::Error, "invalid_comparison",
				"Choice comparison must be equals, not_equals, at_least, or at_most.",
				location);
		}
	}

	std::unordered_set<std::string> knownMembers{ "type" };
	if ( type == "has_item" )
	{
		knownMembers.insert("item");
		knownMembers.insert("stable_id");
		knownMembers.insert("count");
	}
	else if ( type == "has_gold" ) knownMembers.insert("amount");
	else if ( type == "quest_started" || type == "quest_accepted"
		|| type == "quest_completed" || type == "quest_failed" ) knownMembers.insert("quest");
	else if ( type == "quest_stage" )
	{
		knownMembers.insert("quest");
		knownMembers.insert("stage");
		knownMembers.insert("comparison");
	}
	else if ( type == "objective_completed" || type == "objective_incomplete" )
	{
		knownMembers.insert("quest");
		knownMembers.insert("objective");
	}
	else if ( type == "node_seen" ) knownMembers.insert("node");
	else if ( type == "world_flag" || type == "npc_flag" )
	{
		knownMembers.insert("id");
		knownMembers.insert("value");
	}
	else if ( type == "world_variable" || type == "npc_variable" )
	{
		knownMembers.insert("id");
		knownMembers.insert("value");
		knownMembers.insert("comparison");
	}
	if ( nodeCondition )
	{
		knownMembers.insert("true_node");
		knownMembers.insert("false_node");
		if ( type == "has_item" || type == "has_gold" ) knownMembers.insert("consume");
	}
	else if ( condition.HasMember("comparison") )
	{
		// The choice parser accepts this token globally even where the evaluator
		// ignores it, so retain and validate it as a recognized compatibility field.
		knownMembers.insert("comparison");
	}
	reportUnknownMembers(issues, condition, knownMembers, location,
		nodeCondition ? "Node condition" : "Choice condition");
}

void validateFlagAction(std::vector<Issue>& issues, const Value& value,
	const Location& location, const std::string& field)
{
	if ( !value.IsObject() || !nonemptyString(value, "id") || !memberIsBool(value, "value") )
	{
		addIssue(issues, Severity::Error, "invalid_action",
			field + " requires string id and Boolean value.", location);
	}
}

void validateVariableAction(std::vector<Issue>& issues, const Value& value,
	const Location& location, const std::string& field, const bool additive)
{
	const char* number = additive ? "amount" : "value";
	if ( !value.IsObject() || !nonemptyString(value, "id") || !memberIsInt(value, number) )
	{
		addIssue(issues, Severity::Error, "invalid_action",
			field + " requires string id and integer " + number + ".", location);
	}
}

void validateAction(std::vector<Issue>& issues, const Value& action,
	const ValidationOptions& options, const Location& location,
	const bool nodeAction, const bool hasQuest, const bool repeatable)
{
	if ( !action.IsObject() )
	{
		addIssue(issues, Severity::Error, "invalid_action",
			"Action must be an object.", location);
		return;
	}
	if ( nodeAction )
	{
		validateIdentifier(issues, action, "id", location, true);
	}

	for ( auto member = action.MemberBegin(); member != action.MemberEnd(); ++member )
	{
		const std::string field = member->name.GetString();
		if ( nodeAction ? !isNodeActionField(field) : !isChoiceActionField(field) )
		{
			Location fieldLocation = location;
			fieldLocation.path += "." + field;
			addIssue(issues, Severity::Info, "unknown_action_field",
				"Unknown action extension '" + field + "' is preserved but not executed.",
				fieldLocation);
			continue;
		}
		const Value& value = member->value;
		if ( field == "id" )
		{
			continue;
		}
		if ( field == "quest_start" || field == "quest_accept"
			|| field == "quest_complete" || field == "quest_fail"
			|| field == "quest_reset" || field == "recruit_npc" )
		{
			if ( !value.IsBool() )
			{
				addIssue(issues, Severity::Error, "invalid_action",
					field + " must be Boolean.", location);
			}
			if ( field == "quest_reset" && (!repeatable || !hasQuest) )
			{
				addIssue(issues, Severity::Error, "invalid_quest_reset",
					"quest_reset requires a repeatable player-scoped quest.", location);
			}
		}
		else if ( field == "quest_stage" )
		{
			if ( !value.IsInt() )
			{
				addIssue(issues, Severity::Error, "invalid_action",
					"quest_stage must be an integer.", location);
			}
		}
		else if ( field == "reward_gold" || field == "remove_gold" )
		{
			if ( !value.IsInt() || value.GetInt() < 0 )
			{
				addIssue(issues, Severity::Error, "invalid_action",
					field + " must be a non-negative integer.", location);
			}
		}
		else if ( field == "reward_item" || field == "remove_item" )
		{
			Location itemLocation = location;
			itemLocation.path += "." + field;
			validateItemReference(issues, value, options, itemLocation);
			reportUnknownMembers(issues, value,
				{ "item", "count", "stable_id" }, itemLocation, "Item reference");
		}
		else if ( field == "objective_complete" || field == "objective_clear" )
		{
			if ( !value.IsString() || value.GetStringLength() == 0 )
			{
				addIssue(issues, Severity::Error, "invalid_action",
					field + " must be a non-empty objective ID.", location);
			}
		}
		else if ( field == "set_world_flag" || field == "set_npc_flag" )
		{
			validateFlagAction(issues, value, location, field);
			Location fieldLocation = location;
			fieldLocation.path += "." + field;
			reportUnknownMembers(issues, value, { "id", "value" },
				fieldLocation, field);
		}
		else if ( field == "set_world_variable" || field == "set_npc_variable"
			|| field == "set_quest_variable" )
		{
			validateVariableAction(issues, value, location, field, false);
			Location fieldLocation = location;
			fieldLocation.path += "." + field;
			reportUnknownMembers(issues, value, { "id", "value" },
				fieldLocation, field);
		}
		else if ( field == "add_world_variable" || field == "add_npc_variable"
			|| field == "add_quest_variable" )
		{
			validateVariableAction(issues, value, location, field, true);
			Location fieldLocation = location;
			fieldLocation.path += "." + field;
			reportUnknownMembers(issues, value, { "id", "amount" },
				fieldLocation, field);
		}
		else if ( field == "set_power" )
		{
			if ( !value.IsObject() || !memberIsInt(value, "x")
				|| !memberIsInt(value, "y") || !memberIsBool(value, "powered") )
			{
				addIssue(issues, Severity::Error, "invalid_action",
					"set_power requires integer x/y and Boolean powered.", location);
			}
			Location fieldLocation = location;
			fieldLocation.path += ".set_power";
			reportUnknownMembers(issues, value, { "x", "y", "powered" },
				fieldLocation, "set_power");
		}
		else if ( field == "status_effect" )
		{
			if ( !value.IsObject() || !memberIsInt(value, "effect") )
			{
				addIssue(issues, Severity::Error, "invalid_status_effect",
					"status_effect requires an integer effect ID.", location);
			}
			else if ( value["effect"].GetInt() < 0
				|| (options.effectCount > 0 && value["effect"].GetInt() >= options.effectCount) )
			{
				addIssue(issues, Severity::Error, "invalid_status_effect",
					"Status effect ID is outside the runtime range.", location);
			}
			if ( value.IsObject() && value.HasMember("duration_seconds")
				&& (!value["duration_seconds"].IsInt()
					|| value["duration_seconds"].GetInt() < 0) )
			{
				addIssue(issues, Severity::Error, "invalid_status_duration",
					"Status duration must be a non-negative integer.", location);
			}
			if ( value.IsObject() && value.HasMember("strength")
				&& (!value["strength"].IsInt() || value["strength"].GetInt() < 1
					|| value["strength"].GetInt() > 255) )
			{
				addIssue(issues, Severity::Error, "invalid_status_strength",
					"Status strength must be between 1 and 255.", location);
			}
			if ( value.IsObject() && value.HasMember("enabled")
				&& !value["enabled"].IsBool() )
			{
				addIssue(issues, Severity::Error, "invalid_status_enabled",
					"Status enabled must be Boolean.", location);
			}
			Location fieldLocation = location;
			fieldLocation.path += ".status_effect";
			reportUnknownMembers(issues, value,
				{ "effect", "duration_seconds", "strength", "enabled" },
				fieldLocation, "status_effect");
		}
	}

	const bool usesQuest = action.HasMember("quest_start")
		|| action.HasMember("quest_accept") || action.HasMember("quest_complete")
		|| action.HasMember("quest_fail") || action.HasMember("quest_reset")
		|| action.HasMember("quest_stage") || action.HasMember("objective_complete")
		|| action.HasMember("objective_clear") || action.HasMember("set_quest_variable")
		|| action.HasMember("add_quest_variable");
	if ( usesQuest && !hasQuest )
	{
		addIssue(issues, Severity::Error, "quest_action_without_quest",
			"Quest/objective action requires a root quest_id.", location);
	}
}

std::string jsonString(const Value& object, const char* key,
	const std::string& fallback = std::string{})
{
	return memberIsString(object, key) ? object[key].GetString() : fallback;
}

int jsonInt(const Value& object, const char* key, const int fallback = 0)
{
	return memberIsInt(object, key) ? object[key].GetInt() : fallback;
}

bool jsonBool(const Value& object, const char* key, const bool fallback = false)
{
	return memberIsBool(object, key) ? object[key].GetBool() : fallback;
}

std::string comparisonPhrase(const std::string& value)
{
	if ( value == "not_equals" ) return "is not";
	if ( value == "at_least" ) return "is at least";
	if ( value == "at_most" ) return "is at most";
	return "is";
}

} // namespace

const std::vector<std::string>& choiceConditionTypes()
{
	static const std::vector<std::string> values = {
		"has_item", "has_gold", "quest_started", "quest_accepted",
		"quest_completed", "quest_failed", "quest_stage",
		"objective_completed", "objective_incomplete", "world_flag",
		"npc_flag", "world_variable", "npc_variable"
	};
	return values;
}

const std::vector<std::string>& nodeConditionTypes()
{
	static const std::vector<std::string> values = {
		"has_item", "has_gold", "quest_started", "quest_accepted",
		"quest_completed", "quest_failed", "quest_stage", "node_seen",
		"world_flag", "npc_flag", "world_variable", "npc_variable"
	};
	return values;
}

const std::vector<std::string>& choiceActionFields()
{
	static const std::vector<std::string> values = {
		"quest_start", "quest_accept", "quest_complete", "quest_fail",
		"quest_stage", "reward_gold", "reward_item", "status_effect",
		"remove_gold", "remove_item", "objective_complete", "objective_clear",
		"quest_reset", "set_power", "recruit_npc", "set_world_flag",
		"set_npc_flag", "set_quest_variable", "add_quest_variable",
		"set_world_variable", "add_world_variable", "set_npc_variable",
		"add_npc_variable"
	};
	return values;
}

const std::vector<std::string>& nodeActionFields()
{
	static const std::vector<std::string> values = {
		"id", "quest_accept", "quest_complete", "quest_stage", "reward_gold",
		"reward_item", "set_world_flag", "set_world_variable",
		"add_world_variable", "set_npc_flag", "set_npc_variable"
	};
	return values;
}

bool isChoiceConditionType(const std::string& type)
{
	return contains(choiceConditionTypes(), type);
}

bool isNodeConditionType(const std::string& type)
{
	return contains(nodeConditionTypes(), type);
}

bool isChoiceActionField(const std::string& field)
{
	return contains(choiceActionFields(), field);
}

bool isNodeActionField(const std::string& field)
{
	return contains(nodeActionFields(), field);
}

bool isSafeStableItemID(const std::string& value)
{
	return isSafeStableID(value);
}

const std::vector<Capability>& capabilities()
{
	using O = CapabilityOwner;
	using V = CapabilityValue;
	static const std::vector<Capability> values = {
		{ O::Root, "version", "Schema Version", V::Integer, true, "2", "1|2", "File" },
		{ O::Root, "quest_id", "Quest ID", V::String, false, "", "normalized ID", "File" },
		{ O::Root, "quest", "Quest Metadata", V::Object, false, "", "object", "Quest" },
		{ O::Root, "start_node", "Start Node", V::NodeReference, true, "", "existing node", "Conversation" },
		{ O::Root, "nodes", "Nodes", V::Array, true, "", "non-empty array", "Conversation" },
		{ O::Root, "text", "Legacy Dialogue Text", V::String, false, "", "non-empty", "Legacy" },
		{ O::Quest, "title", "Title", V::String, true, "", "non-empty", "Overview" },
		{ O::Quest, "summary", "Summary", V::String, false, "", "string", "Overview" },
		{ O::Quest, "objective", "General Objective", V::String, false, "", "string", "Overview" },
		{ O::Quest, "completed_text", "Completion Text", V::String, false, "", "string", "Overview" },
		{ O::Quest, "failed_text", "Failure Text", V::String, false, "", "string", "Overview" },
		{ O::Quest, "scope", "Scope", V::String, false, "player", "player|party|world", "Overview" },
		{ O::Quest, "repeatable", "Repeatable", V::Boolean, false, "false", "Boolean", "Overview" },
		{ O::Quest, "origin", "Quest Giver", V::Object, false, "", "object", "Giver" },
		{ O::Quest, "objectives", "Objectives", V::Array, false, "[]", "array", "Objectives" },
		{ O::Origin, "label", "Label", V::String, false, "", "string", "Giver" },
		{ O::Origin, "map", "Map", V::String, false, "", "map ID", "Giver" },
		{ O::Origin, "x", "Tile X", V::Integer, false, "", ">= 0", "Giver" },
		{ O::Origin, "y", "Tile Y", V::Integer, false, "", ">= 0", "Giver" },
		{ O::Origin, "playable_floor", "Playable Floor", V::Integer, false, "0", ">= 0", "Giver" },
		{ O::Origin, "floor_visibility", "Floor Visibility", V::String, false, "column", "same_floor|column", "Giver" },
		{ O::Origin, "track_npc", "Track NPC", V::Boolean, false, "false", "Boolean", "Giver" },
		{ O::Origin, "npc_persistent_id", "Persistent NPC ID", V::Integer, false, "", "> 0", "Giver" },
		{ O::Objective, "id", "Objective ID", V::String, true, "", "unique ID", "Objectives" },
		{ O::Objective, "text", "Active Text", V::String, true, "", "non-empty", "Objectives" },
		{ O::Objective, "completed_text", "Completed Text", V::String, false, "", "string", "Objectives" },
		{ O::Objective, "stage", "Stage", V::Integer, false, "0", "integer", "Objectives" },
		{ O::Objective, "optional", "Optional", V::Boolean, false, "false", "Boolean", "Objectives" },
		{ O::Objective, "progress_variable", "Progress Variable", V::String, false, "", "normalized ID", "Objectives" },
		{ O::Objective, "target", "Target", V::Integer, false, "1", "> 0", "Objectives" },
		{ O::Objective, "defeat_id", "Defeat ID", V::Integer, false, "", "> 0", "Objectives" },
		{ O::Objective, "map_marker", "Map Marker", V::Object, false, "", "map/x/y + optional floor policy", "Objectives" },
		{ O::Node, "id", "Node ID", V::Integer, true, "", "unique integer", "Node" },
		{ O::Node, "text", "NPC Text", V::String, true, "", "non-empty", "Node" },
		{ O::Node, "next", "Automatic Next", V::NodeReference, false, "self", "existing node", "Node" },
		{ O::Node, "condition", "Redirect Condition", V::Object, false, "", "node condition", "Node" },
		{ O::Node, "action", "One-time Node Action", V::Object, false, "", "node action", "Node" },
		{ O::Node, "choices", "Choices", V::Array, false, "[]", "array", "Node" },
		{ O::Choice, "id", "Choice ID", V::String, true, "", "unique stable ID", "Choice" },
		{ O::Choice, "text", "Player Response", V::String, true, "", "non-empty", "Choice" },
		{ O::Choice, "next", "Destination", V::NodeReference, true, "", "existing node", "Choice" },
		{ O::Choice, "once", "Once Only", V::Boolean, false, "false", "Boolean", "Choice" },
		{ O::Choice, "condition", "Single Condition", V::Object, false, "", "choice condition", "Condition" },
		{ O::Choice, "conditions", "All Conditions", V::Array, false, "[]", "logical AND", "Condition" },
		{ O::Choice, "action", "Actions", V::Object, false, "", "choice action fields", "Action" },
		{ O::ItemReference, "item", "Vanilla Item", V::Item, false, "", "alias or vanilla numeric string", "Items" },
		{ O::ItemReference, "stable_id", "S.A.M. Stable ID", V::Item, false, "", "namespaced persistent ID", "Items" },
		{ O::ItemReference, "count", "Quantity", V::Integer, false, "1", "> 0", "Items" }
	};
	return values;
}

const char* severityName(const Severity severity)
{
	switch ( severity )
	{
		case Severity::Error: return "ERROR";
		case Severity::Warning: return "WARNING";
		default: return "INFO";
	}
}

bool hasErrors(const std::vector<Issue>& issues)
{
	return std::any_of(issues.begin(), issues.end(), [](const Issue& issue)
		{ return issue.severity == Severity::Error; });
}

int countIssues(const std::vector<Issue>& issues, const Severity severity)
{
	return static_cast<int>(std::count_if(issues.begin(), issues.end(),
		[severity](const Issue& issue) { return issue.severity == severity; }));
}

std::vector<Issue> validate(const Value& document, const ValidationOptions& options)
{
	std::vector<Issue> issues;
	if ( !document.IsObject() )
	{
		addIssue(issues, Severity::Error, "root_not_object",
			"Dialogue root must be a JSON object.", atDocument("$"));
		return issues;
	}

	reportUnknownMembers(issues, document,
		{ "version", "quest_id", "quest", "start_node", "nodes", "text" },
		atDocument("$"), "Root");

	const int schemaVersion = memberIsInt(document, "version")
		? document["version"].GetInt() : 0;
	if ( schemaVersion < LegacySchemaVersion
		|| schemaVersion > SchemaVersion )
	{
		addIssue(issues, Severity::Error, "invalid_version",
			"version must be integer 1 or 2.", atDocument("$.version"));
	}
	validateIdentifier(issues, document, "quest_id", atDocument("$.quest_id"), false);

	const bool hasQuestID = nonemptyString(document, "quest_id");
	bool repeatable = false;
	std::unordered_set<std::string> objectiveIDs;
	if ( document.HasMember("quest") )
	{
		Location questLocation = atDocument("$.quest");
		questLocation.kind = LocationKind::Quest;
		if ( !document["quest"].IsObject() )
		{
			addIssue(issues, Severity::Error, "invalid_quest",
				"quest must be an object.", questLocation);
		}
		else
		{
			const Value& quest = document["quest"];
			reportUnknownMembers(issues, quest,
				{ "title", "summary", "objective", "completed_text", "failed_text",
				  "origin", "objectives", "scope", "repeatable" },
				questLocation, "Quest");
			if ( !hasQuestID )
			{
				addIssue(issues, Severity::Error, "quest_without_id",
					"Quest metadata requires a root quest_id.", questLocation);
			}
			if ( !nonemptyString(quest, "title") )
			{
				addIssue(issues, Severity::Error, "missing_quest_title",
					"Quest metadata requires a non-empty title.", questLocation);
			}
			for ( const char* field : { "summary", "objective", "completed_text", "failed_text" } )
			{
				if ( quest.HasMember(field) && !quest[field].IsString() )
				{
					addIssue(issues, Severity::Error, "invalid_quest_text",
						std::string("quest.") + field + " must be a string.", questLocation);
				}
			}

			std::string scope = "player";
			if ( quest.HasMember("scope") )
			{
				if ( !quest["scope"].IsString() )
				{
					addIssue(issues, Severity::Error, "invalid_scope",
						"Quest scope must be player, party, or world.", questLocation);
				}
				else
				{
					scope = normalizeID(quest["scope"].GetString());
					if ( scope != "player" && scope != "party" && scope != "world" )
					{
						addIssue(issues, Severity::Error, "invalid_scope",
							"Quest scope must be player, party, or world.", questLocation);
					}
					else if ( scope != "player"
						&& schemaVersion < SharedQuestOwnershipSchemaVersion )
					{
						addIssue(issues, Severity::Warning, "legacy_scope_falls_back_to_player",
							"Schema 1 retains legacy Personal ownership for authored '"
							+ scope + "' scope. Explicitly upgrade this file to schema 2"
							+ " to enable shared ownership.",
							questLocation);
					}
				}
			}
			if ( quest.HasMember("repeatable") )
			{
				if ( !quest["repeatable"].IsBool() )
				{
					addIssue(issues, Severity::Error, "invalid_repeatable",
						"Quest repeatable must be Boolean.", questLocation);
				}
				else
				{
					repeatable = quest["repeatable"].GetBool();
				}
			}

			if ( quest.HasMember("origin") )
			{
				Location originLocation = atDocument("$.quest.origin");
				originLocation.kind = LocationKind::Origin;
				const Value& origin = quest["origin"];
				if ( !origin.IsObject() )
				{
					addIssue(issues, Severity::Error, "invalid_origin",
						"Quest origin must be an object.", originLocation);
				}
				else
				{
					reportUnknownMembers(issues, origin,
						{ "label", "map", "x", "y", "playable_floor", "floor_visibility",
						  "track_npc", "npc_persistent_id" },
						originLocation, "Quest origin");
					for ( const char* field : { "label", "map" } )
					{
						if ( origin.HasMember(field) && !origin[field].IsString() )
						{
							addIssue(issues, Severity::Error, "invalid_origin_text",
								std::string("Origin ") + field + " must be a string.", originLocation);
						}
					}
					const bool hasX = origin.HasMember("x");
					const bool hasY = origin.HasMember("y");
					if ( hasX != hasY || (hasX && (!origin["x"].IsInt()
						|| !origin["y"].IsInt() || origin["x"].GetInt() < 0
						|| origin["y"].GetInt() < 0)) )
					{
						addIssue(issues, Severity::Error, "invalid_origin_marker",
							"Static origin requires non-negative integer x and y.", originLocation);
					}
					if ( origin.HasMember("track_npc") && !origin["track_npc"].IsBool() )
					{
						addIssue(issues, Severity::Error, "invalid_track_npc",
							"Origin track_npc must be Boolean.", originLocation);
					}
					if ( origin.HasMember("playable_floor")
						&& (!origin["playable_floor"].IsInt()
							|| origin["playable_floor"].GetInt() < 0) )
					{
						addIssue(issues, Severity::Error, "invalid_origin_floor",
							"Origin playable_floor must be a non-negative integer.", originLocation);
					}
					if ( origin.HasMember("floor_visibility") )
					{
						if ( !origin["floor_visibility"].IsString()
							|| (std::string(origin["floor_visibility"].GetString()) != "same_floor"
								&& std::string(origin["floor_visibility"].GetString()) != "column") )
						{
							addIssue(issues, Severity::Error, "invalid_origin_floor_visibility",
								"Origin floor_visibility must be 'same_floor' or 'column'.", originLocation);
						}
						else if ( std::string(origin["floor_visibility"].GetString()) == "same_floor"
							&& !origin.HasMember("playable_floor") )
						{
							addIssue(issues, Severity::Error, "missing_origin_floor",
								"A same-floor origin requires playable_floor.", originLocation);
						}
					}
					if ( jsonBool(origin, "track_npc")
						&& (!memberIsInt(origin, "npc_persistent_id")
							|| origin["npc_persistent_id"].GetInt() <= 0) )
					{
						addIssue(issues, Severity::Error, "invalid_giver_id",
							"Tracked quest giver requires a positive persistent NPC ID.", originLocation);
					}
				}
			}

			if ( quest.HasMember("objectives") )
			{
				if ( !quest["objectives"].IsArray() )
				{
					addIssue(issues, Severity::Error, "invalid_objectives",
						"quest.objectives must be an array.", questLocation);
				}
				else
				{
					const Value& objectives = quest["objectives"];
					for ( rapidjson::SizeType i = 0; i < objectives.Size(); ++i )
					{
						Location objectiveLocation = atDocument(
							"$.quest.objectives[" + std::to_string(i) + "]");
						objectiveLocation.kind = LocationKind::Objective;
						objectiveLocation.objectiveIndex = static_cast<int>(i);
						const Value& objective = objectives[i];
						if ( !objective.IsObject() )
						{
							addIssue(issues, Severity::Error, "invalid_objective",
								"Objective must be an object.", objectiveLocation);
							continue;
						}
						reportUnknownMembers(issues, objective,
							{ "id", "text", "completed_text", "stage", "optional",
							  "progress_variable", "target", "defeat_id", "map_marker" },
							objectiveLocation, "Objective");
						if ( !nonemptyString(objective, "id") )
						{
							addIssue(issues, Severity::Error, "invalid_objective_id",
								"Objective needs a non-empty string ID.", objectiveLocation);
						}
						else
						{
							objectiveLocation.objectiveID = objective["id"].GetString();
							if ( !objectiveIDs.insert(normalizeID(objective["id"].GetString())).second )
							{
								addIssue(issues, Severity::Error, "duplicate_objective_id",
									"Duplicate objective ID '" + objectiveLocation.objectiveID + "'.",
									objectiveLocation);
							}
						}
						if ( !nonemptyString(objective, "text") )
						{
							addIssue(issues, Severity::Error, "invalid_objective_text",
								"Objective needs non-empty active text.", objectiveLocation);
						}
						if ( objective.HasMember("completed_text")
							&& !objective["completed_text"].IsString() )
						{
							addIssue(issues, Severity::Error, "invalid_objective_text",
								"Objective completed_text must be a string.", objectiveLocation);
						}
						if ( objective.HasMember("stage") && !objective["stage"].IsInt() )
						{
							addIssue(issues, Severity::Error, "invalid_objective_stage",
								"Objective stage must be an integer.", objectiveLocation);
						}
						if ( objective.HasMember("optional") && !objective["optional"].IsBool() )
						{
							addIssue(issues, Severity::Error, "invalid_objective_optional",
								"Objective optional must be Boolean.", objectiveLocation);
						}
						if ( objective.HasMember("progress_variable")
							&& !objective["progress_variable"].IsString() )
						{
							addIssue(issues, Severity::Error, "invalid_progress_variable",
								"Objective progress_variable must be a string.", objectiveLocation);
						}
						if ( objective.HasMember("target")
							&& (!objective["target"].IsInt() || objective["target"].GetInt() <= 0) )
						{
							addIssue(issues, Severity::Error, "invalid_objective_target",
								"Objective target must be a positive integer.", objectiveLocation);
						}
						if ( objective.HasMember("defeat_id")
							&& (!objective["defeat_id"].IsInt() || objective["defeat_id"].GetInt() <= 0) )
						{
							addIssue(issues, Severity::Error, "invalid_defeat_id",
								"Objective defeat_id must be a positive integer.", objectiveLocation);
						}
						if ( objective.HasMember("map_marker") )
						{
							const Value& marker = objective["map_marker"];
							if ( !marker.IsObject() || !nonemptyString(marker, "map")
								|| !memberIsInt(marker, "x") || !memberIsInt(marker, "y")
								|| marker["x"].GetInt() < 0 || marker["y"].GetInt() < 0 )
							{
								addIssue(issues, Severity::Error, "invalid_objective_marker",
									"Objective marker requires map and non-negative integer x/y.",
									objectiveLocation);
							}
							else
							{
								Location markerLocation = objectiveLocation;
								markerLocation.path += ".map_marker";
								reportUnknownMembers(issues, marker,
									{ "map", "x", "y", "playable_floor", "floor_visibility" },
									markerLocation, "Objective marker");
								if ( marker.HasMember("playable_floor")
									&& (!marker["playable_floor"].IsInt()
										|| marker["playable_floor"].GetInt() < 0) )
								{
									addIssue(issues, Severity::Error, "invalid_marker_floor",
										"Marker playable_floor must be a non-negative integer.", markerLocation);
								}
								if ( marker.HasMember("floor_visibility") )
								{
									if ( !marker["floor_visibility"].IsString()
										|| (std::string(marker["floor_visibility"].GetString()) != "same_floor"
											&& std::string(marker["floor_visibility"].GetString()) != "column") )
									{
										addIssue(issues, Severity::Error, "invalid_marker_floor_visibility",
											"Marker floor_visibility must be 'same_floor' or 'column'.", markerLocation);
									}
									else if ( std::string(marker["floor_visibility"].GetString()) == "same_floor"
										&& !marker.HasMember("playable_floor") )
									{
										addIssue(issues, Severity::Error, "missing_marker_floor",
											"A same-floor marker requires playable_floor.", markerLocation);
									}
								}
							}
						}
					}
				}
			}
		}
	}

	// Version-1 legacy one-line documents are still accepted by the runtime.
	const bool legacy = document.HasMember("text") && document["text"].IsString()
		&& !document.HasMember("nodes");
	if ( legacy )
	{
		if ( document["text"].GetStringLength() == 0 )
		{
			addIssue(issues, Severity::Error, "empty_legacy_text",
				"Legacy dialogue text cannot be empty.", atDocument("$.text"));
		}
		addIssue(issues, Severity::Info, "legacy_document",
			"Legacy one-line format is supported and preserved.", atDocument("$.text"));
		return issues;
	}

	if ( !memberIsInt(document, "start_node") )
	{
		addIssue(issues, Severity::Error, "missing_start_node",
			"start_node must be an integer.", atDocument("$.start_node"));
	}
	if ( !document.HasMember("nodes") || !document["nodes"].IsArray()
		|| document["nodes"].Empty() )
	{
		addIssue(issues, Severity::Error, "invalid_nodes",
			"nodes must be a non-empty array.", atDocument("$.nodes"));
		return issues;
	}

	const Value& nodes = document["nodes"];
	std::unordered_map<int, int> nodeIndexByID;
	std::unordered_set<std::string> choiceIDs;
	std::unordered_set<std::string> nodeActionIDs;
	std::unordered_map<int, std::set<int>> edges;
	auto validateObjectiveConditionReference = [&](const Value& condition,
		const Location& location)
	{
		if ( !condition.IsObject() || !memberIsString(condition, "type")
			|| !memberIsString(condition, "objective") ) return;
		const std::string type = normalizeID(condition["type"].GetString());
		if ( type != "objective_completed" && type != "objective_incomplete" ) return;
		// Only the root quest's objectives are declared in this document. A
		// conversation-only file, or a reference to another quest, is resolved by
		// the persistent story registry at runtime and cannot be checked locally.
		if ( !hasQuestID || !memberIsString(condition, "quest")
			|| normalizeID(condition["quest"].GetString())
				!= normalizeID(document["quest_id"].GetString()) ) return;
		if ( objectiveIDs.find(normalizeID(condition["objective"].GetString()))
			== objectiveIDs.end() )
		{
			addIssue(issues, Severity::Error, "missing_objective_reference",
				"Condition references missing objective '"
				+ std::string(condition["objective"].GetString()) + "'.", location);
		}
	};
	auto validateObjectiveActionReferences = [&](const Value& action,
		const Location& location)
	{
		if ( !action.IsObject() ) return;
		for ( const char* field : { "objective_complete", "objective_clear" } )
		{
			if ( memberIsString(action, field)
				&& objectiveIDs.find(normalizeID(action[field].GetString()))
					== objectiveIDs.end() )
			{
				Location fieldLocation = location;
				fieldLocation.path += "." + std::string(field);
				addIssue(issues, Severity::Error, "missing_objective_reference",
					std::string(field) + " references missing objective '"
					+ action[field].GetString() + "'.", fieldLocation);
			}
		}
	};

	for ( rapidjson::SizeType nodeIndex = 0; nodeIndex < nodes.Size(); ++nodeIndex )
	{
		Location nodeLocation = atDocument("$.nodes[" + std::to_string(nodeIndex) + "]");
		nodeLocation.kind = LocationKind::Node;
		nodeLocation.nodeIndex = static_cast<int>(nodeIndex);
		const Value& node = nodes[nodeIndex];
		if ( !node.IsObject() )
		{
			addIssue(issues, Severity::Error, "invalid_node", "Node must be an object.",
				nodeLocation);
			continue;
		}
		reportUnknownMembers(issues, node,
			{ "id", "text", "next", "condition", "action", "choices" },
			nodeLocation, "Node");
		if ( !memberIsInt(node, "id") )
		{
			addIssue(issues, Severity::Error, "invalid_node_id",
				"Node ID must be an integer.", nodeLocation);
		}
		else
		{
			nodeLocation.nodeID = node["id"].GetInt();
			if ( nodeIndexByID.find(nodeLocation.nodeID) != nodeIndexByID.end() )
			{
				addIssue(issues, Severity::Error, "duplicate_node_id",
					"Duplicate node ID " + std::to_string(nodeLocation.nodeID) + ".",
					nodeLocation);
			}
			else
			{
				nodeIndexByID[nodeLocation.nodeID] = static_cast<int>(nodeIndex);
			}
		}
		if ( !nonemptyString(node, "text") )
		{
			addIssue(issues, Severity::Error, "empty_node_text",
				"Node text must be a non-empty string.", nodeLocation);
		}
		if ( node.HasMember("next") && !node["next"].IsInt() )
		{
			addIssue(issues, Severity::Error, "invalid_node_next",
				"Node next must be an integer destination.", nodeLocation);
		}
		if ( nodeLocation.nodeID >= 0 )
		{
			edges[nodeLocation.nodeID].insert(memberIsInt(node, "next")
				? node["next"].GetInt() : nodeLocation.nodeID);
		}
		if ( node.HasMember("condition") )
		{
			Location conditionLocation = nodeLocation;
			conditionLocation.kind = LocationKind::Condition;
			conditionLocation.path += ".condition";
			validateCondition(issues, node["condition"], options, conditionLocation, true);
			if ( node["condition"].IsObject() && nodeLocation.nodeID >= 0 )
			{
				if ( memberIsInt(node["condition"], "true_node") )
					edges[nodeLocation.nodeID].insert(node["condition"]["true_node"].GetInt());
				if ( memberIsInt(node["condition"], "false_node") )
					edges[nodeLocation.nodeID].insert(node["condition"]["false_node"].GetInt());
			}
		}
		if ( node.HasMember("action") )
		{
			Location actionLocation = nodeLocation;
			actionLocation.kind = LocationKind::Action;
			actionLocation.path += ".action";
			validateAction(issues, node["action"], options, actionLocation, true,
				hasQuestID, repeatable);
			if ( node["action"].IsObject() && nonemptyString(node["action"], "id")
				&& !nodeActionIDs.insert(
					normalizeID(node["action"]["id"].GetString())).second )
			{
				addIssue(issues, Severity::Error, "duplicate_node_action_id",
					"Duplicate one-time node action ID '"
					+ std::string(node["action"]["id"].GetString()) + "'.",
					actionLocation);
			}
		}
		if ( node.HasMember("choices") && !node["choices"].IsArray() )
		{
			addIssue(issues, Severity::Error, "invalid_choices",
				"Node choices must be an array.", nodeLocation);
		}
		if ( !node.HasMember("choices") || !node["choices"].IsArray() )
		{
			continue;
		}

		const Value& choices = node["choices"];
		for ( rapidjson::SizeType choiceIndex = 0; choiceIndex < choices.Size(); ++choiceIndex )
		{
			Location choiceLocation = nodeLocation;
			choiceLocation.kind = LocationKind::Choice;
			choiceLocation.choiceIndex = static_cast<int>(choiceIndex);
			choiceLocation.path += ".choices[" + std::to_string(choiceIndex) + "]";
			const Value& choice = choices[choiceIndex];
			if ( !choice.IsObject() )
			{
				addIssue(issues, Severity::Error, "invalid_choice",
					"Choice must be an object.", choiceLocation);
				continue;
			}
			reportUnknownMembers(issues, choice,
				{ "id", "text", "next", "once", "condition", "conditions", "action" },
				choiceLocation, "Choice");
			if ( !nonemptyString(choice, "id") )
			{
				addIssue(issues, Severity::Error, "invalid_choice_id",
					"Choice ID must be a non-empty string.", choiceLocation);
			}
			else
			{
				choiceLocation.choiceID = choice["id"].GetString();
				const std::string normalized = normalizeID(choiceLocation.choiceID);
				if ( !choiceIDs.insert(normalized).second )
				{
					addIssue(issues, Severity::Error, "duplicate_choice_id",
						"Duplicate stable choice ID '" + choiceLocation.choiceID + "'.",
						choiceLocation);
				}
			}
			if ( !nonemptyString(choice, "text") )
			{
				addIssue(issues, Severity::Error, "empty_choice_text",
					"Choice response text must be non-empty.", choiceLocation);
			}
			if ( !memberIsInt(choice, "next") )
			{
				addIssue(issues, Severity::Error, "invalid_choice_next",
					"Choice destination must be an integer node ID.", choiceLocation);
			}
			else if ( nodeLocation.nodeID >= 0 )
			{
				edges[nodeLocation.nodeID].insert(choice["next"].GetInt());
			}
			if ( choice.HasMember("once") && !choice["once"].IsBool() )
			{
				addIssue(issues, Severity::Error, "invalid_once",
					"Choice once must be Boolean.", choiceLocation);
			}
			if ( choice.HasMember("condition") )
			{
				Location conditionLocation = choiceLocation;
				conditionLocation.kind = LocationKind::Condition;
				conditionLocation.path += ".condition";
				conditionLocation.conditionIndex = 0;
				validateCondition(issues, choice["condition"], options,
					conditionLocation, false);
				validateObjectiveConditionReference(choice["condition"], conditionLocation);
			}
			if ( choice.HasMember("conditions") )
			{
				if ( !choice["conditions"].IsArray() )
				{
					addIssue(issues, Severity::Error, "invalid_conditions",
						"Choice conditions must be an array (logical AND).", choiceLocation);
				}
				else
				{
					const Value& conditions = choice["conditions"];
					for ( rapidjson::SizeType conditionIndex = 0;
						conditionIndex < conditions.Size(); ++conditionIndex )
					{
						Location conditionLocation = choiceLocation;
						conditionLocation.kind = LocationKind::Condition;
						conditionLocation.conditionIndex = static_cast<int>(conditionIndex);
						conditionLocation.path += ".conditions["
							+ std::to_string(conditionIndex) + "]";
					validateCondition(issues, conditions[conditionIndex], options,
						conditionLocation, false);
					validateObjectiveConditionReference(
						conditions[conditionIndex], conditionLocation);
					}
				}
			}
			if ( choice.HasMember("action") )
			{
				Location actionLocation = choiceLocation;
				actionLocation.kind = LocationKind::Action;
				actionLocation.path += ".action";
				validateAction(issues, choice["action"], options, actionLocation, false,
					hasQuestID, repeatable);
				validateObjectiveActionReferences(choice["action"], actionLocation);
			}
		}
	}

	const int startNode = memberIsInt(document, "start_node")
		? document["start_node"].GetInt() : std::numeric_limits<int>::min();
	if ( memberIsInt(document, "start_node")
		&& nodeIndexByID.find(startNode) == nodeIndexByID.end() )
	{
		addIssue(issues, Severity::Error, "broken_start_node",
			"start_node " + std::to_string(startNode) + " does not exist.",
			atDocument("$.start_node"));
	}
	for ( const auto& source : edges )
	{
		for ( const int destination : source.second )
		{
			if ( nodeIndexByID.find(destination) == nodeIndexByID.end() )
			{
				Location location = atDocument("$.nodes["
					+ std::to_string(nodeIndexByID[source.first]) + "]");
				location.kind = LocationKind::Node;
				location.nodeID = source.first;
				location.nodeIndex = nodeIndexByID[source.first];
				addIssue(issues, Severity::Error, "broken_node_link",
					"Node " + std::to_string(source.first) + " links to missing node "
					+ std::to_string(destination) + ".", location);
			}
		}
	}

	if ( nodeIndexByID.find(startNode) != nodeIndexByID.end() )
	{
		std::set<int> reachable;
		std::vector<int> pending{ startNode };
		while ( !pending.empty() )
		{
			const int current = pending.back();
			pending.pop_back();
			if ( !reachable.insert(current).second ) continue;
			const auto found = edges.find(current);
			if ( found == edges.end() ) continue;
			for ( const int next : found->second )
				if ( nodeIndexByID.find(next) != nodeIndexByID.end() ) pending.push_back(next);
		}
		for ( const auto& node : nodeIndexByID )
		{
			if ( reachable.find(node.first) == reachable.end() )
			{
				Location location = atDocument("$.nodes[" + std::to_string(node.second) + "]");
				location.kind = LocationKind::Node;
				location.nodeID = node.first;
				location.nodeIndex = node.second;
				addIssue(issues, Severity::Warning, "unreachable_node",
					"Node " + std::to_string(node.first) + " is unreachable from start_node.",
					location);
			}
		}
	}

	return issues;
}

std::string itemReferenceSummary(const Value& reference)
{
	if ( !reference.IsObject() ) return "invalid item reference";
	const int count = jsonInt(reference, "count", 1);
	if ( memberIsString(reference, "stable_id") )
	{
		return std::string(reference["stable_id"].GetString()) + " x"
			+ std::to_string(count) + " (stable)";
	}
	return jsonString(reference, "item", "?") + " x" + std::to_string(count);
}

std::string conditionSummary(const Value& condition)
{
	if ( !condition.IsObject() || !memberIsString(condition, "type") )
		return "Invalid condition";
	const std::string type = normalizeID(condition["type"].GetString());
	if ( type == "has_item" ) return "Player has " + itemReferenceSummary(condition);
	if ( type == "has_gold" ) return "Player has at least "
		+ std::to_string(jsonInt(condition, "amount")) + " gold";
	if ( type == "quest_started" || type == "quest_accepted"
		|| type == "quest_completed" || type == "quest_failed" )
		return "Quest " + jsonString(condition, "quest", "?") + " is "
			+ type.substr(std::string("quest_").size());
	if ( type == "quest_stage" ) return "Quest " + jsonString(condition, "quest", "?")
		+ " stage " + comparisonPhrase(jsonString(condition, "comparison", "equals"))
		+ " " + std::to_string(jsonInt(condition, "stage"));
	if ( type == "objective_completed" || type == "objective_incomplete" )
		return "Objective " + jsonString(condition, "objective", "?") + " is "
			+ (type == "objective_completed" ? "complete" : "incomplete");
	if ( type == "node_seen" ) return "Node " + jsonString(condition, "node", "?")
		+ " was seen";
	if ( type == "world_flag" || type == "npc_flag" )
		return (type == "world_flag" ? "World" : "NPC") + std::string(" flag ")
			+ jsonString(condition, "id", "?") + " is "
			+ (jsonBool(condition, "value", true) ? "true" : "false");
	if ( type == "world_variable" || type == "npc_variable" )
		return (type == "world_variable" ? "World" : "NPC") + std::string(" variable ")
			+ jsonString(condition, "id", "?") + " "
			+ comparisonPhrase(jsonString(condition, "comparison", "equals")) + " "
			+ std::to_string(jsonInt(condition, "value"));
	return "Condition: " + type;
}

std::vector<std::string> actionSummaries(const Value& action)
{
	std::vector<std::string> lines;
	if ( !action.IsObject() ) return { "Invalid action" };
	for ( auto member = action.MemberBegin(); member != action.MemberEnd(); ++member )
	{
		const std::string field = member->name.GetString();
		const Value& value = member->value;
		if ( field == "id" ) lines.push_back("One-time action ID: " + jsonString(action, "id", "?"));
		else if ( field == "quest_start" && value.IsBool() && value.GetBool() ) lines.push_back("Start quest");
		else if ( field == "quest_accept" && value.IsBool() && value.GetBool() ) lines.push_back("Accept quest");
		else if ( field == "quest_complete" && value.IsBool() && value.GetBool() ) lines.push_back("Complete quest");
		else if ( field == "quest_fail" && value.IsBool() && value.GetBool() ) lines.push_back("Fail quest");
		else if ( field == "quest_reset" && value.IsBool() && value.GetBool() ) lines.push_back("Reset quest");
		else if ( field == "quest_stage" && value.IsInt() ) lines.push_back("Set quest stage to " + std::to_string(value.GetInt()));
		else if ( field == "reward_gold" && value.IsInt() ) lines.push_back("Give " + std::to_string(value.GetInt()) + " gold");
		else if ( field == "remove_gold" && value.IsInt() ) lines.push_back("Take " + std::to_string(value.GetInt()) + " gold");
		else if ( field == "reward_item" ) lines.push_back("Give " + itemReferenceSummary(value));
		else if ( field == "remove_item" ) lines.push_back("Take " + itemReferenceSummary(value));
		else if ( field == "objective_complete" && value.IsString() ) lines.push_back("Complete objective " + std::string(value.GetString()));
		else if ( field == "objective_clear" && value.IsString() ) lines.push_back("Clear objective " + std::string(value.GetString()));
		else if ( field == "recruit_npc" && value.IsBool() && value.GetBool() ) lines.push_back("Recruit this NPC");
		else if ( field == "status_effect" && value.IsObject() ) lines.push_back(
			(jsonBool(value, "enabled", true) ? "Apply" : "Clear") + std::string(" status effect ")
			+ std::to_string(jsonInt(value, "effect", -1)));
		else if ( field == "set_power" && value.IsObject() ) lines.push_back(
			(jsonBool(value, "powered", true) ? "Power" : "Unpower") + std::string(" tile ")
			+ std::to_string(jsonInt(value, "x")) + "," + std::to_string(jsonInt(value, "y")));
		else if ( field == "set_world_flag" || field == "set_npc_flag" ) lines.push_back(
			(field == "set_world_flag" ? "Set world flag " : "Set NPC flag ")
			+ jsonString(value, "id", "?"));
		else if ( field.find("_variable") != std::string::npos ) lines.push_back(
			field + ": " + jsonString(value, "id", "?"));
		else if ( isChoiceActionField(field) || isNodeActionField(field) ) lines.push_back(field);
		else lines.push_back("Preserved custom action: " + field);
	}
	if ( lines.empty() ) lines.push_back("No actions");
	return lines;
}

Document::Document()
{
	reset();
}

void Document::reset()
{
	document_.SetObject();
	currentSnapshot_ = serialize(false);
	cleanSnapshot_ = currentSnapshot_;
	undo_.clear();
	redo_.clear();
}

bool Document::parse(const std::string& text, std::string& error)
{
	if ( text.size() > MaximumDocumentBytes )
	{
		error = "Dialogue exceeds the runtime 64 KiB limit.";
		return false;
	}
	rapidjson::Document parsed;
	parsed.Parse(text.c_str(), text.size());
	if ( parsed.HasParseError() )
	{
		error = "JSON parse error at byte " + std::to_string(parsed.GetErrorOffset()) + ".";
		return false;
	}
	if ( !parsed.IsObject() )
	{
		error = "Dialogue root must be an object.";
		return false;
	}
	document_.Swap(parsed);
	currentSnapshot_ = serialize(false);
	cleanSnapshot_ = currentSnapshot_;
	undo_.clear();
	redo_.clear();
	error.clear();
	return true;
}

bool Document::replaceWithEdit(const std::string& text, const std::string& label,
	std::string& error)
{
	if ( text.size() > MaximumDocumentBytes )
	{
		error = "Dialogue exceeds the runtime 64 KiB limit.";
		return false;
	}
	rapidjson::Document parsed;
	parsed.Parse(text.c_str(), text.size());
	if ( parsed.HasParseError() )
	{
		error = "JSON parse error at byte " + std::to_string(parsed.GetErrorOffset()) + ".";
		return false;
	}
	if ( !parsed.IsObject() )
	{
		error = "Dialogue root must be an object.";
		return false;
	}
	const std::string next = [&parsed]()
	{
		rapidjson::StringBuffer buffer;
		rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
		parsed.Accept(writer);
		return std::string(buffer.GetString());
	}();
	if ( next == currentSnapshot_ )
	{
		error.clear();
		return true;
	}
	undo_.push_back(Snapshot{ currentSnapshot_, label });
	redo_.clear();
	document_.Swap(parsed);
	currentSnapshot_ = next;
	trimHistory();
	error.clear();
	return true;
}

bool Document::loadFile(const std::string& path, std::string& error)
{
	std::ifstream input(path, std::ios::binary);
	if ( !input.is_open() )
	{
		error = "Could not open " + path + ".";
		return false;
	}
	const std::string text((std::istreambuf_iterator<char>(input)),
		std::istreambuf_iterator<char>());
	if ( !input.good() && !input.eof() )
	{
		error = "Could not read " + path + ".";
		return false;
	}
	return parse(text, error);
}

bool Document::saveAtomic(const std::string& path, std::string& error)
{
	const std::string compact = serialize(false);
	if ( compact.size() + 1 > MaximumDocumentBytes )
	{
		error = "Dialogue exceeds the runtime 64 KiB limit.";
		return false;
	}
	const std::string pretty = serialize(true);
	const std::string& saved = pretty.size() + 1 <= MaximumDocumentBytes
		? pretty : compact;
	const std::string temporary = path + ".automatia-dialogue.tmp";
	{
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		if ( !output.is_open() )
		{
			error = "Could not open temporary save file for " + path + ".";
			return false;
		}
		output << saved << '\n';
		output.flush();
		if ( !output.good() )
		{
			output.close();
			std::remove(temporary.c_str());
			error = "Could not finish writing " + path + ".";
			return false;
		}
	}
	if ( std::rename(temporary.c_str(), path.c_str()) != 0 )
	{
		std::remove(temporary.c_str());
		error = "Could not atomically replace " + path + ".";
		return false;
	}
	markClean();
	error.clear();
	return true;
}

rapidjson::Document& Document::json() { return document_; }
const rapidjson::Document& Document::json() const { return document_; }

std::string Document::serialize(const bool pretty) const
{
	rapidjson::StringBuffer buffer;
	if ( pretty )
	{
		rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
		document_.Accept(writer);
	}
	else
	{
		rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
		document_.Accept(writer);
	}
	return buffer.GetString();
}

void Document::trimHistory()
{
	constexpr std::size_t maximumSnapshots = 64;
	if ( undo_.size() > maximumSnapshots )
		undo_.erase(undo_.begin(), undo_.begin() + (undo_.size() - maximumSnapshots));
	if ( redo_.size() > maximumSnapshots )
		redo_.erase(redo_.begin(), redo_.begin() + (redo_.size() - maximumSnapshots));
}

bool Document::recordExternalEdit(const std::string& label)
{
	const std::string next = serialize(false);
	if ( next == currentSnapshot_ ) return false;
	undo_.push_back(Snapshot{ currentSnapshot_, label });
	redo_.clear();
	currentSnapshot_ = next;
	trimHistory();
	return true;
}

bool Document::restore(const std::string& text)
{
	rapidjson::Document parsed;
	parsed.Parse(text.c_str(), text.size());
	if ( parsed.HasParseError() || !parsed.IsObject() ) return false;
	document_.Swap(parsed);
	currentSnapshot_ = text;
	return true;
}

bool Document::undo()
{
	if ( undo_.empty() ) return false;
	const Snapshot snapshot = undo_.back();
	undo_.pop_back();
	redo_.push_back(Snapshot{ currentSnapshot_, snapshot.label });
	return restore(snapshot.text);
}

bool Document::redo()
{
	if ( redo_.empty() ) return false;
	const Snapshot snapshot = redo_.back();
	redo_.pop_back();
	undo_.push_back(Snapshot{ currentSnapshot_, snapshot.label });
	return restore(snapshot.text);
}

bool Document::canUndo() const { return !undo_.empty(); }
bool Document::canRedo() const { return !redo_.empty(); }
const std::string& Document::nextUndoLabel() const
{
	static const std::string empty;
	return undo_.empty() ? empty : undo_.back().label;
}
const std::string& Document::nextRedoLabel() const
{
	static const std::string empty;
	return redo_.empty() ? empty : redo_.back().label;
}
bool Document::dirty() const { return currentSnapshot_ != cleanSnapshot_; }
void Document::markClean()
{
	currentSnapshot_ = serialize(false);
	cleanSnapshot_ = currentSnapshot_;
}

namespace
{
std::string previewItemKey(const Value& reference)
{
	if ( memberIsString(reference, "stable_id") ) return reference["stable_id"].GetString();
	return jsonString(reference, "item", "?");
}

bool comparePreviewValue(const int actual, const int expected,
	const std::string& comparison)
{
	if ( comparison == "not_equals" ) return actual != expected;
	if ( comparison == "at_least" ) return actual >= expected;
	if ( comparison == "at_most" ) return actual <= expected;
	return actual == expected;
}
}

bool PreviewSession::begin(const Value& document, const PreviewState& state,
	std::string& error)
{
	const bool legacy = document.IsObject() && memberIsString(document, "text")
		&& !document.HasMember("nodes");
	if ( !legacy && (!document.IsObject() || !memberIsInt(document, "start_node")
		|| !document.HasMember("nodes") || !document["nodes"].IsArray()) )
	{
		error = "Preview requires a validated graph or legacy text document.";
		return false;
	}
	document_ = &document;
	initialState_ = state;
	state_ = state;
	currentNode_ = document["start_node"].GetInt();
	unavailableActions_.clear();
	return rebuildFrame(error);
}

void PreviewSession::reset()
{
	state_ = initialState_;
	if ( document_ && memberIsInt(*document_, "start_node") )
	{
		currentNode_ = (*document_)["start_node"].GetInt();
	}
	unavailableActions_.clear();
	std::string ignored;
	rebuildFrame(ignored);
}

const PreviewFrame& PreviewSession::frame() const { return frame_; }
PreviewState& PreviewSession::state() { return state_; }
const PreviewState& PreviewSession::state() const { return state_; }
const std::vector<std::string>& PreviewSession::unavailableActions() const
{
	return unavailableActions_;
}

const Value* PreviewSession::findNode(const int nodeID) const
{
	if ( !document_ || !document_->IsObject() || !document_->HasMember("nodes")
		|| !(*document_)["nodes"].IsArray() ) return nullptr;
	for ( const Value& node : (*document_)["nodes"].GetArray() )
	{
		if ( node.IsObject() && memberIsInt(node, "id") && node["id"].GetInt() == nodeID )
			return &node;
	}
	return nullptr;
}

std::string PreviewSession::rootQuestID() const
{
	return document_ && memberIsString(*document_, "quest_id")
		? (*document_)["quest_id"].GetString() : std::string{};
}

bool PreviewSession::evaluateCondition(const Value& condition) const
{
	if ( !condition.IsObject() || !memberIsString(condition, "type") ) return false;
	const std::string type = normalizeID(condition["type"].GetString());
	if ( type == "has_item" )
	{
		const auto found = state_.items.find(previewItemKey(condition));
		return found != state_.items.end() && found->second >= jsonInt(condition, "count", 1);
	}
	if ( type == "has_gold" ) return state_.gold >= jsonInt(condition, "amount");
	if ( type == "quest_started" || type == "quest_accepted"
		|| type == "quest_completed" || type == "quest_failed" )
	{
		const auto found = state_.quests.find(jsonString(condition, "quest"));
		if ( found == state_.quests.end() ) return false;
		if ( type == "quest_started" ) return found->second.started;
		if ( type == "quest_accepted" ) return found->second.accepted;
		if ( type == "quest_completed" ) return found->second.completed;
		return found->second.failed;
	}
	if ( type == "quest_stage" )
	{
		const auto found = state_.quests.find(jsonString(condition, "quest"));
		const int actual = found == state_.quests.end() ? 0 : found->second.stage;
		return comparePreviewValue(actual, jsonInt(condition, "stage"),
			jsonString(condition, "comparison", "equals"));
	}
	if ( type == "objective_completed" || type == "objective_incomplete" )
	{
		bool completed = false;
		const auto quest = state_.quests.find(jsonString(condition, "quest"));
		if ( quest != state_.quests.end() )
		{
			const auto objective = quest->second.objectives.find(
				jsonString(condition, "objective"));
			completed = objective != quest->second.objectives.end() && objective->second;
		}
		return type == "objective_completed" ? completed : !completed;
	}
	if ( type == "node_seen" )
		return state_.seenNodes.find(jsonString(condition, "node")) != state_.seenNodes.end();
	if ( type == "world_flag" || type == "npc_flag" )
	{
		const auto& flags = type == "world_flag" ? state_.worldFlags : state_.npcFlags;
		const auto found = flags.find(jsonString(condition, "id"));
		const bool value = found != flags.end() && found->second;
		return value == jsonBool(condition, "value", true);
	}
	if ( type == "world_variable" || type == "npc_variable" )
	{
		const auto& variables = type == "world_variable"
			? state_.worldVariables : state_.npcVariables;
		const auto found = variables.find(jsonString(condition, "id"));
		const int value = found == variables.end() ? 0 : found->second;
		return comparePreviewValue(value, jsonInt(condition, "value"),
			jsonString(condition, "comparison", "equals"));
	}
	return false;
}

bool PreviewSession::applyChoiceAction(const Value& action, std::string& error)
{
	if ( !action.IsObject() ) return true;
	const std::string questID = rootQuestID();
	PreviewQuestState* quest = questID.empty() ? nullptr : &state_.quests[questID];

	if ( memberIsInt(action, "remove_gold") && state_.gold < action["remove_gold"].GetInt() )
	{
		error = "Simulated player cannot afford this choice.";
		return false;
	}
	if ( action.HasMember("remove_item") && action["remove_item"].IsObject() )
	{
		const Value& item = action["remove_item"];
		const std::string key = previewItemKey(item);
		const int count = jsonInt(item, "count", 1);
		if ( state_.items[key] < count )
		{
			error = "Simulated player lacks " + itemReferenceSummary(item) + ".";
			return false;
		}
	}

	if ( action.HasMember("quest_reset") && jsonBool(action, "quest_reset") && quest )
		*quest = PreviewQuestState{};
	if ( action.HasMember("quest_start") && jsonBool(action, "quest_start") && quest ) quest->started = true;
	if ( action.HasMember("quest_accept") && jsonBool(action, "quest_accept") && quest )
	{
		quest->started = true;
		quest->accepted = true;
	}
	if ( memberIsInt(action, "quest_stage") && quest ) quest->stage = action["quest_stage"].GetInt();
	if ( action.HasMember("quest_complete") && jsonBool(action, "quest_complete") && quest ) quest->completed = true;
	if ( action.HasMember("quest_fail") && jsonBool(action, "quest_fail") && quest ) quest->failed = true;
	if ( memberIsString(action, "objective_complete") && quest )
		quest->objectives[action["objective_complete"].GetString()] = true;
	if ( memberIsString(action, "objective_clear") && quest )
		quest->objectives[action["objective_clear"].GetString()] = false;

	if ( memberIsInt(action, "remove_gold") ) state_.gold -= action["remove_gold"].GetInt();
	if ( memberIsInt(action, "reward_gold") ) state_.gold += action["reward_gold"].GetInt();
	if ( action.HasMember("remove_item") && action["remove_item"].IsObject() )
		state_.items[previewItemKey(action["remove_item"])] -= jsonInt(action["remove_item"], "count", 1);
	if ( action.HasMember("reward_item") && action["reward_item"].IsObject() )
		state_.items[previewItemKey(action["reward_item"])] += jsonInt(action["reward_item"], "count", 1);

	auto applyFlag = [&](const char* field, std::map<std::string, bool>& flags)
	{
		if ( action.HasMember(field) && action[field].IsObject() )
			flags[jsonString(action[field], "id")] = jsonBool(action[field], "value");
	};
	applyFlag("set_world_flag", state_.worldFlags);
	applyFlag("set_npc_flag", state_.npcFlags);
	auto applyVariable = [&](const char* field, std::map<std::string, int>& variables,
		const bool additive)
	{
		if ( !action.HasMember(field) || !action[field].IsObject() ) return;
		const std::string id = jsonString(action[field], "id");
		if ( additive ) variables[id] += jsonInt(action[field], "amount");
		else variables[id] = jsonInt(action[field], "value");
	};
	applyVariable("set_world_variable", state_.worldVariables, false);
	applyVariable("add_world_variable", state_.worldVariables, true);
	applyVariable("set_npc_variable", state_.npcVariables, false);
	applyVariable("add_npc_variable", state_.npcVariables, true);
	if ( quest )
	{
		applyVariable("set_quest_variable", quest->variables, false);
		applyVariable("add_quest_variable", quest->variables, true);
	}

	for ( const char* unsafe : { "status_effect", "set_power", "recruit_npc" } )
	{
		if ( action.HasMember(unsafe) )
		{
			const std::string notice = std::string("Preview unavailable for ") + unsafe
				+ "; no live world state was changed.";
			if ( std::find(unavailableActions_.begin(), unavailableActions_.end(), notice)
				== unavailableActions_.end() ) unavailableActions_.push_back(notice);
		}
	}
	return true;
}

bool PreviewSession::rebuildFrame(std::string& error)
{
	frame_ = PreviewFrame{};
	if ( !document_ )
	{
		error = "No preview document.";
		frame_.error = error;
		return false;
	}
	if ( memberIsString(*document_, "text") && !document_->HasMember("nodes") )
	{
		frame_.valid = true;
		frame_.nodeID = 0;
		frame_.npcText = (*document_)["text"].GetString();
		frame_.notices.push_back(
			"Legacy one-line dialogue; convert it in Conversation to add flow.");
		state_.seenNodes.insert("node_0");
		error.clear();
		return true;
	}

	for ( int redirects = 0; redirects < 64; ++redirects )
	{
		const Value* node = findNode(currentNode_);
		if ( !node )
		{
			error = "Preview destination node " + std::to_string(currentNode_) + " is missing.";
			frame_.error = error;
			return false;
		}
		if ( !node->HasMember("condition") ) break;
		const Value& condition = (*node)["condition"];
		if ( !condition.IsObject() ) break;
		const bool passed = evaluateCondition(condition);
		if ( passed && jsonBool(condition, "consume") )
		{
			const std::string type = jsonString(condition, "type");
			if ( type == "has_gold" ) state_.gold -= jsonInt(condition, "amount");
			if ( type == "has_item" ) state_.items[previewItemKey(condition)] -= jsonInt(condition, "count", 1);
		}
		currentNode_ = jsonInt(condition, passed ? "true_node" : "false_node", currentNode_);
		if ( redirects == 63 )
		{
			error = "Condition redirects exceeded the preview safety limit.";
			frame_.error = error;
			return false;
		}
	}

	const Value* node = findNode(currentNode_);
	if ( !node ) return false;
	frame_.valid = true;
	frame_.nodeID = currentNode_;
	frame_.npcText = jsonString(*node, "text");
	state_.seenNodes.insert("node_" + std::to_string(currentNode_));

	if ( node->HasMember("action") && (*node)["action"].IsObject() )
	{
		const Value& action = (*node)["action"];
		const std::string key = "node_action:" + jsonString(action, "id");
		if ( !jsonString(action, "id").empty() && state_.usedChoices.insert(key).second )
			applyChoiceAction(action, error);
	}

	if ( node->HasMember("choices") && (*node)["choices"].IsArray() )
	{
		const Value& choices = (*node)["choices"];
		for ( rapidjson::SizeType index = 0; index < choices.Size(); ++index )
		{
			const Value& choice = choices[index];
			if ( !choice.IsObject() ) continue;
			const std::string id = jsonString(choice, "id");
			if ( jsonBool(choice, "once") && state_.usedChoices.find(id) != state_.usedChoices.end() )
				continue;
			bool visible = true;
			std::vector<std::string> summaries;
			if ( choice.HasMember("condition") )
			{
				visible = evaluateCondition(choice["condition"]);
				summaries.push_back(conditionSummary(choice["condition"]));
			}
			if ( choice.HasMember("conditions") && choice["conditions"].IsArray() )
			{
				for ( const Value& condition : choice["conditions"].GetArray() )
				{
					visible = visible && evaluateCondition(condition);
					summaries.push_back(conditionSummary(condition));
				}
			}
			if ( !visible ) continue;
			PreviewChoice preview;
			preview.sourceIndex = static_cast<int>(index);
			preview.id = id;
			preview.text = jsonString(choice, "text");
			preview.nextNode = jsonInt(choice, "next", currentNode_);
			for ( std::size_t i = 0; i < summaries.size(); ++i )
			{
				if ( i ) preview.condition += " AND ";
				preview.condition += summaries[i];
			}
			if ( choice.HasMember("action") ) preview.actions = actionSummaries(choice["action"]);
			frame_.choices.push_back(std::move(preview));
		}
	}
	if ( frame_.choices.empty() )
	{
		const int next = jsonInt(*node, "next", currentNode_);
		if ( next != currentNode_ )
		{
			PreviewChoice continuation;
			continuation.sourceIndex = -2;
			continuation.id = "__continue";
			continuation.text = "Continue";
			continuation.nextNode = next;
			frame_.choices.push_back(std::move(continuation));
		}
		else frame_.notices.push_back("Terminal node");
	}
	for ( const std::string& notice : unavailableActions_ ) frame_.notices.push_back(notice);
	error.clear();
	return true;
}

bool PreviewSession::choose(const int visibleChoiceIndex, std::string& error)
{
	if ( !frame_.valid || visibleChoiceIndex < 0
		|| visibleChoiceIndex >= static_cast<int>(frame_.choices.size()) )
	{
		error = "Preview choice index is invalid.";
		return false;
	}
	const PreviewChoice selected = frame_.choices[visibleChoiceIndex];
	if ( selected.sourceIndex >= 0 )
	{
		const Value* node = findNode(frame_.nodeID);
		if ( !node || !node->HasMember("choices") || !(*node)["choices"].IsArray()
			|| selected.sourceIndex >= static_cast<int>((*node)["choices"].Size()) )
		{
			error = "Preview source choice is no longer available.";
			return false;
		}
		const Value& choice = (*node)["choices"][selected.sourceIndex];
		if ( choice.HasMember("action") && !applyChoiceAction(choice["action"], error) )
			return false;
		state_.usedChoices.insert(selected.id);
	}
	currentNode_ = selected.nextNode;
	return rebuildFrame(error);
}

namespace
{
std::string escapeJSON(const std::string& value)
{
	std::string result;
	result.reserve(value.size() + 8);
	for ( const unsigned char character : value )
	{
		switch ( character )
		{
			case '"': result += "\\\""; break;
			case '\\': result += "\\\\"; break;
			case '\n': result += "\\n"; break;
			case '\r': result += "\\r"; break;
			case '\t': result += "\\t"; break;
			default:
				if ( character >= 0x20 ) result.push_back(static_cast<char>(character));
				break;
		}
	}
	return result;
}

std::string questBlock(const bool repeatable = false,
	const std::string& extra = std::string{}, const std::string& scope = "player")
{
	return "\n  \"quest_id\": \"tutorial_quest\","
		"\n  \"quest\": {"
		"\n    \"title\": \"Tutorial Quest\","
		"\n    \"summary\": \"A verified editor tutorial.\","
		"\n    \"objective\": \"Complete the tutorial objective.\","
		"\n    \"completed_text\": \"Tutorial complete.\","
		"\n    \"failed_text\": \"Try the tutorial again.\","
		"\n    \"scope\": \"" + scope + "\","
		"\n    \"repeatable\": " + std::string(repeatable ? "true" : "false") + ","
		"\n    \"objectives\": ["
		"\n      {\"id\": \"objective_1\", \"text\": \"Complete the objective.\","
		" \"completed_text\": \"Objective complete.\", \"stage\": 0,"
		" \"optional\": false, \"progress_variable\": \"progress\", \"target\": 1}"
		"\n    ]" + extra + "\n  },";
}

std::string choiceExample(const std::string& conditionMember,
	const std::string& actionMember, const bool once = false,
	const std::string& rootQuest = std::string{})
{
	return "{\n  \"version\": 1," + rootQuest
		+ "\n  \"start_node\": 0,"
		"\n  \"nodes\": ["
		"\n    {"
		"\n      \"id\": 0,"
		"\n      \"text\": \"Choose a response.\","
		"\n      \"next\": 0,"
		"\n      \"choices\": ["
		"\n        {"
		"\n          \"id\": \"tutorial_choice\","
		"\n          \"text\": \"Use the tutorial response.\","
		"\n          \"next\": 1"
		+ (once ? ",\n          \"once\": true" : std::string{})
		+ (conditionMember.empty() ? std::string{} : ",\n          " + conditionMember)
		+ (actionMember.empty() ? std::string{} : ",\n          \"action\": {" + actionMember + "\n          }")
		+ "\n        },"
		"\n        {\"id\": \"decline\", \"text\": \"Not now.\", \"next\": 0}"
		"\n      ]"
		"\n    },"
		"\n    {\"id\": 1, \"text\": \"The tutorial action ran.\", \"next\": 1}"
		"\n  ]\n}";
}

TutorialRecipe recipe(const std::string& id, const std::string& title,
	const std::string& category, const std::string& difficulty,
	const std::string& goal, const std::string& example,
	const bool manual = false)
{
	TutorialRecipe result;
	result.id = id;
	result.title = title;
	result.category = category;
	result.difficulty = difficulty;
	result.goal = goal;
	result.playerExperience = "The player experiences this authored result: " + goal;
	if ( category == "Getting Started" )
		result.panelHint = "Use Conversation cards for nodes, responses, and direct destinations.";
	else if ( category == "Quests" || category == "Objectives"
		|| category == "Quest Giver" )
		result.panelHint = "Use the Quest tab, then return to the response Action inspector.";
	else if ( category == "Items" || category == "Rewards" || category == "Costs" )
		result.panelHint = "Use the searchable item catalog in the Condition or Action inspector.";
	else
		result.panelHint = "Use the semantic Condition or Action inspector on the selected response.";
	result.concepts = { category, "Conversation cards", "Semantic inspector",
		"Sandbox preview", "Validation" };
	result.steps = {
		"Apply the verified example to a selected dialogue file, or recreate it in Conversation.",
		"Select the node or player-response card that owns this behavior.",
		result.panelHint,
		"Configure the named IDs, destinations, values, or item in the labeled inspector fields.",
		"Read the semantic summary, validate to zero errors, run the sandbox preview, then save."
	};
	result.expectedResult = goal + " The generated path uses only the current runtime schema.";
	result.multiplayerNote = "Choices and consequences are revalidated and applied by the host.";
	result.persistenceNote = "Quest, once-choice, flag, variable, and node-seen state use the existing story registry.";
	if ( id.find("item") != std::string::npos || id == "sam_stable_item" )
		result.commonMistake = "Use a vanilla ItemType string or stable_id; never persist a S.A.M. session runtime number.";
	else if ( id.find("quest") != std::string::npos || category == "Objectives" )
		result.commonMistake = "Dialogue filename and quest_id are separate; objective IDs must exist in the referenced quest.";
	else if ( id == "node_redirect" || id == "node_seen" || id == "node_action" )
		result.commonMistake = "Node redirects need valid true/false destinations, and node action IDs must be unique.";
	else
		result.commonMistake = "Keep response IDs unique and validate every destination before saving.";
	result.exampleJson = example;
	result.manualGameTestRequired = manual;
	return result;
}
}

const std::vector<TutorialRecipe>& tutorialRecipes()
{
	static const std::vector<TutorialRecipe> recipes = []
	{
		std::vector<TutorialRecipe> values;
		values.push_back(recipe("first_conversation", "Your First Conversation", "Getting Started", "Beginner",
			"Make an NPC say hello.", choiceExample("", "")));
		values.push_back(recipe("linear_conversation", "A Linear Conversation", "Getting Started", "Beginner",
			"Link several nodes in order.",
			"{\"version\":1,\"start_node\":0,\"nodes\":["
			"{\"id\":0,\"text\":\"First line.\",\"next\":1},"
			"{\"id\":1,\"text\":\"Second line.\",\"next\":2},"
			"{\"id\":2,\"text\":\"Final line.\",\"next\":2}]}"));
		values.push_back(recipe("branching_conversation", "A Two-Choice Branch", "Getting Started", "Beginner",
			"Send two responses to different destinations.",
			"{\"version\":1,\"start_node\":0,\"nodes\":["
			"{\"id\":0,\"text\":\"Choose.\",\"next\":0,\"choices\":["
			"{\"id\":\"yes\",\"text\":\"Yes.\",\"next\":1},"
			"{\"id\":\"no\",\"text\":\"No.\",\"next\":2}]},"
			"{\"id\":1,\"text\":\"You agreed.\",\"next\":1},"
			"{\"id\":2,\"text\":\"You declined.\",\"next\":2}]}"));
		values.push_back(recipe("once_response", "Once-Only Response", "Getting Started", "Beginner",
			"Hide a response after that player uses it.", choiceExample("", "", true)));

		values.push_back(recipe("item_requirement", "Require an Item", "Conditions", "Beginner",
			"Show a response only while the player has a torch.",
			choiceExample("\"condition\": {\"type\":\"has_item\",\"item\":\"torch\",\"count\":1}", "")));
		values.push_back(recipe("give_item", "Give an Item", "Rewards", "Beginner",
			"Give a healing potion.", choiceExample("", "\n            \"reward_item\": {\"item\":\"healing_potion\",\"count\":1}")));
		values.push_back(recipe("take_item", "Take an Item", "Costs", "Beginner",
			"Take one torch after host validation.", choiceExample(
			"\"condition\": {\"type\":\"has_item\",\"item\":\"torch\",\"count\":1}",
			"\n            \"remove_item\": {\"item\":\"torch\",\"count\":1}")));
		values.push_back(recipe("gold_requirement", "Require Gold", "Conditions", "Beginner",
			"Gate a response behind 100 gold.", choiceExample(
			"\"condition\": {\"type\":\"has_gold\",\"amount\":100}", "")));
		values.push_back(recipe("gold_reward_cost", "Give and Take Gold", "Rewards", "Intermediate",
			"Charge 25 gold and reward 100 gold.", choiceExample(
			"\"condition\": {\"type\":\"has_gold\",\"amount\":25}",
			"\n            \"remove_gold\": 25,\n            \"reward_gold\": 100")));

		const std::string quest = questBlock();
		values.push_back(recipe("quest_start_accept", "Start and Accept a Quest", "Quests", "Beginner",
			"Create the player-scoped quest journal state.", choiceExample("",
			"\n            \"quest_start\": true,\n            \"quest_accept\": true", false, quest)));
		values.push_back(recipe("quest_stage", "Change Quest Stage", "Quests", "Intermediate",
			"Set the current quest stage.", choiceExample("",
			"\n            \"quest_stage\": 2", false, quest)));
		values.push_back(recipe("quest_complete", "Complete a Quest", "Quests", "Intermediate",
			"Complete the current quest.", choiceExample("",
			"\n            \"quest_complete\": true", false, quest)));
		values.push_back(recipe("quest_fail", "Fail a Quest", "Quests", "Intermediate",
			"Fail the current quest.", choiceExample("",
			"\n            \"quest_fail\": true", false, quest)));
		values.push_back(recipe("quest_reset", "Reset a Repeatable Quest", "Quests", "Advanced",
			"Reset a repeatable player quest.", choiceExample("",
			"\n            \"quest_reset\": true", false, questBlock(true))));
		values.push_back(recipe("objective_state", "Complete and Clear Objectives", "Objectives", "Intermediate",
			"Update an objective state.", choiceExample("",
			"\n            \"objective_complete\": \"objective_1\","
			"\n            \"objective_clear\": \"objective_1\"", false, quest)));
		values.push_back(recipe("objective_marker", "Objective Map Marker", "Objectives", "Intermediate",
			"Add a static journal marker.",
			"{\"version\":1," + questBlock(false,
				",\n    \"origin\": {\"label\":\"Guide\",\"map\":\"tutorial.lmp\",\"x\":4,\"y\":5}")
			+ "\n  \"start_node\":0,\"nodes\":[{\"id\":0,\"text\":\"Track it.\",\"next\":0}]}"));
		values.back().exampleJson =
			"{\"version\":1,\"quest_id\":\"tutorial_quest\",\"quest\":{"
			"\"title\":\"Marked Objective\",\"scope\":\"player\",\"repeatable\":false,"
			"\"objectives\":[{\"id\":\"objective_1\",\"text\":\"Visit the tile.\","
			"\"map_marker\":{\"map\":\"tutorial.lmp\",\"x\":8,\"y\":9}}]},"
			"\"start_node\":0,\"nodes\":[{\"id\":0,\"text\":\"Track it.\",\"next\":0}]}";
		values.push_back(recipe("static_giver", "Static Quest Giver Marker", "Quest Giver", "Intermediate",
			"Place the giver marker on a map tile.",
			"{\"version\":1,\"quest_id\":\"tutorial_quest\",\"quest\":{"
			"\"title\":\"Static Giver\",\"origin\":{\"label\":\"Guide\",\"map\":\"tutorial.lmp\",\"x\":4,\"y\":5}},"
			"\"start_node\":0,\"nodes\":[{\"id\":0,\"text\":\"Hello.\",\"next\":0}]}"));
		values.push_back(recipe("tracked_giver", "Follow the Selected NPC", "Quest Giver", "Intermediate",
			"Track a persistent NPC as quest origin.",
			"{\"version\":1,\"quest_id\":\"tutorial_quest\",\"quest\":{"
			"\"title\":\"Tracked Giver\",\"origin\":{\"label\":\"Guide\",\"map\":\"tutorial.lmp\","
			"\"track_npc\":true,\"npc_persistent_id\":1842}},"
			"\"start_node\":0,\"nodes\":[{\"id\":0,\"text\":\"Follow me.\",\"next\":0}]}"));

		values.push_back(recipe("other_quest_state", "Check Another Quest", "Conditions", "Intermediate",
			"Show a response after another quest completes.", choiceExample(
			"\"condition\": {\"type\":\"quest_completed\",\"quest\":\"other_quest\"}", "")));
		values.push_back(recipe("world_flag", "World Flag", "Flags", "Intermediate",
			"Require and set a persistent world flag.", choiceExample(
			"\"condition\": {\"type\":\"world_flag\",\"id\":\"gate_open\",\"value\":false}",
			"\n            \"set_world_flag\": {\"id\":\"gate_open\",\"value\":true}")));
		values.push_back(recipe("npc_flag", "NPC Flag", "Flags", "Intermediate",
			"Require and set per-player NPC memory.", choiceExample(
			"\"condition\": {\"type\":\"npc_flag\",\"id\":\"met\",\"value\":false}",
			"\n            \"set_npc_flag\": {\"id\":\"met\",\"value\":true}")));
		values.push_back(recipe("world_variable", "World Variable", "Variables", "Intermediate",
			"Compare, set, and add a world variable.", choiceExample(
			"\"condition\": {\"type\":\"world_variable\",\"id\":\"gate_progress\",\"value\":2,\"comparison\":\"at_least\"}",
			"\n            \"set_world_variable\": {\"id\":\"gate_progress\",\"value\":2},"
			"\n            \"add_world_variable\": {\"id\":\"gate_progress\",\"amount\":1}")));
		values.push_back(recipe("npc_variable", "NPC Variable", "Variables", "Intermediate",
			"Compare, set, and add NPC-local state.", choiceExample(
			"\"condition\": {\"type\":\"npc_variable\",\"id\":\"trust\",\"value\":1,\"comparison\":\"equals\"}",
			"\n            \"set_npc_variable\": {\"id\":\"trust\",\"value\":1},"
			"\n            \"add_npc_variable\": {\"id\":\"trust\",\"amount\":1}")));
		values.push_back(recipe("quest_variable", "Quest Variables", "Variables", "Advanced",
			"Set and add player quest variables (there is no quest-variable condition type).",
			choiceExample("", "\n            \"set_quest_variable\": {\"id\":\"progress\",\"value\":1},"
			"\n            \"add_quest_variable\": {\"id\":\"progress\",\"amount\":1}", false, quest)));
		values.push_back(recipe("comparison_operators", "Comparison Operators and AND", "Conditions", "Advanced",
			"Require every condition in the runtime-supported conditions array.", choiceExample(
			"\"conditions\": [{\"type\":\"quest_stage\",\"quest\":\"other_quest\",\"stage\":2,\"comparison\":\"at_least\"},"
			"{\"type\":\"world_variable\",\"id\":\"attempts\",\"value\":0,\"comparison\":\"not_equals\"}]", "")));

		values.push_back(recipe("status_effect", "Status Effect", "Player Status", "Advanced",
			"Apply a named runtime effect.", choiceExample("",
			"\n            \"status_effect\": {\"effect\":22,\"duration_seconds\":30,\"strength\":1,\"enabled\":true}"), true));
		values.push_back(recipe("power_tile", "Power a Mechanism Tile", "Mechanisms", "Advanced",
			"Send host-authoritative power to a tile.", choiceExample("",
			"\n            \"set_power\": {\"x\":12,\"y\":8,\"powered\":true}"), true));
		values.push_back(recipe("recruit_npc", "Recruit an NPC", "NPC", "Intermediate",
			"Recruit through the generic follower path.", choiceExample("",
			"\n            \"recruit_npc\": true"), true));
		values.push_back(recipe("mini_mimic_recruit", "Dialogue and Recruitable Mini Mimic", "NPC", "Advanced",
			"Keep Mini Mimic dialogue, recruitment, disposition, and appearance independent.",
			choiceExample("", "\n            \"recruit_npc\": true"), true));
		values.push_back(recipe("player_scope_multiplayer", "Private Player Quest State", "Multiplayer", "Intermediate",
			"Keep two players' progress private even when they share a party.",
			R"json({"version":2,"quest_id":"private_trial","quest":{"title":"Private Trial","scope":"player","repeatable":false,"objectives":[{"id":"finish","text":"Finish your own trial."}]},"start_node":0,"nodes":[{"id":0,"text":"This trial belongs only to you.","next":0,"choices":[{"id":"accept","text":"Begin my trial.","next":0,"action":{"quest_start":true,"quest_accept":true}}]}]})json", true));
		values.back().steps = {
			"Use schema version 2 and select Personal on the Quest Overview panel.",
			"Give the quest a stable quest_id and add its objectives.",
			"Add quest_start and quest_accept to the accepting response.",
			"Validate, save, then accept with two clients and advance only one client.",
			"Confirm the other player remains independent even when both are in one party."
		};
		values.back().multiplayerNote = "The server resolves the owner from each authenticated durable player identity.";
		values.back().commonMistake = "Never use player slot, floor, map layer, or Entity::z as persistent player identity.";

		values.push_back(recipe("party_shared_quest", "One Quest Shared by a Party", "Multiplayer", "Advanced",
			"Share exactly one quest state across the current persistent PartyID.",
			R"json({"version":2,"quest_id":"party_relic","quest":{"title":"The Party Relic","summary":"Recover it together.","scope":"party","repeatable":false,"objectives":[{"id":"recover","text":"Recover the relic.","stage":0}]},"start_node":0,"nodes":[{"id":0,"text":"Your party may pursue this together.","next":0,"choices":[{"id":"accept","text":"We will recover it.","next":0,"action":{"quest_start":true,"quest_accept":true}}]}]})json", true));
		values.back().steps = {
			"Use schema version 2; schema 1 intentionally keeps legacy Personal semantics.",
			"Select Party in Quest Overview and author normal objectives/actions.",
			"Accept with one party member and verify another current member receives it.",
			"Add a member after progress and verify the existing state synchronizes.",
			"Kick or leave with one client and verify that client immediately loses the party view."
		};
		values.back().multiplayerNote = "The server derives the persistent PartyID; no client chooses or submits an owner ID.";
		values.back().persistenceNote = "Party quest state stays with that durable PartyID and is never copied into personal state.";
		values.back().commonMistake = "Shared ownership does not multiply reward_gold or reward_item; those still target the actor.";

		values.push_back(recipe("world_shared_quest", "One Quest Shared by the World", "Multiplayer", "Advanced",
			"Share progress across every player and map instance in the same persistent world save.",
			R"json({"version":2,"quest_id":"world_beacon","quest":{"title":"Light the World Beacon","scope":"world","repeatable":true,"objectives":[{"id":"ignite","text":"Ignite the beacon.","stage":0}]},"start_node":0,"nodes":[{"id":0,"text":"Every explorer will see this progress.","next":0,"choices":[{"id":"ignite","text":"Ignite it.","next":0,"action":{"quest_start":true,"quest_accept":true,"objective_complete":"ignite","quest_complete":true}}]}]})json", true));
		values.back().steps = {
			"Use schema version 2 and select World in Quest Overview.",
			"Author the quest normally; do not encode a map instance or floor into its identity.",
			"Advance it with one client while another occupies a different MapInstance or floor.",
			"Reconnect a client and verify the current world state arrives during late-join sync.",
			"Test quest_reset separately because this example is repeatable."
		};
		values.back().multiplayerNote = "One state belongs to the persistent WorldState/world save, not to MapInstance.";
		values.back().commonMistake = "Same X/Y, playableFloor, authoredMapLayer, and Entity::z never determine world quest ownership.";

		values.push_back(recipe("floor_aware_markers", "Floor-Aware and Column Markers", "Objectives", "Advanced",
			"Choose whether each marker appears on one playable floor or through its entire vertical column.",
			R"json({"version":2,"quest_id":"tower_search","quest":{"title":"Search the Tower","scope":"player","origin":{"label":"Archivist","map":"tower.lmp","x":4,"y":5,"playable_floor":0,"floor_visibility":"same_floor"},"objectives":[{"id":"lower_key","text":"Find the lower key.","map_marker":{"map":"tower.lmp","x":8,"y":9,"playable_floor":0,"floor_visibility":"same_floor"}},{"id":"column_signal","text":"Investigate the signal column.","map_marker":{"map":"tower.lmp","x":12,"y":6,"floor_visibility":"column"}}]},"start_node":0,"nodes":[{"id":0,"text":"Search every floor carefully.","next":0}]})json", true));
		values.back().steps = {
			"On Quest Giver or Objectives, choose Pick Tile on Map or Manual Coords.",
			"During tile pick, use U/P to select an existing PlayableFloorID, then click a tile.",
			"Leave Same Floor enabled for a marker tied to one floor.",
			"Toggle Whole Column when the same X/Y should be marked on every playable floor.",
			"Test the minimap while moving between floors; map identity and X/Y still must match."
		};
		values.back().commonMistake = "Playable floor is a separate axis; do not substitute authoredMapLayer or local Entity::z in JSON.";

		values.push_back(recipe("enemy_group_defeat", "Detect an Authored Enemy Group's Deaths", "Objectives", "Advanced",
			"Advance a quest objective whenever monsters with one Squad Defeat ID die.",
			R"json({"version":2,"quest_id":"clear_raiders","quest":{"title":"Clear the Raiders","scope":"party","objectives":[{"id":"raider_squad","text":"Defeat the marked raiders.","completed_text":"The raiders are defeated.","stage":0,"target":3,"progress_variable":"raiders_defeated","defeat_id":42}]},"start_node":0,"nodes":[{"id":0,"text":"Clear the raider squad.","next":0,"choices":[{"id":"accept","text":"We will handle them.","next":0,"action":{"quest_start":true,"quest_accept":true}}]}]})json", true));
		values.back().steps = {
			"In each target monster's editor properties, set Defeat ID (Squad Defeat ID) to the same value, such as 42.",
			"Do not confuse Defeat ID with Squad ID: Squad ID controls AI squad behavior only.",
			"On the quest objective, set defeat_id to that value and set a target count.",
			"Optionally set progress_variable for a readable persistent counter; otherwise one is derived.",
			"Accept the quest and kill marked monsters; the server credits each matching death.",
			"Validate with multiple clients and save/reload to confirm scoped progress persists."
		};
		values.back().expectedResult = "Each death whose authored Squad Defeat ID is 42 advances the one Party-owned counter; ordinary Squad ID does not.";
		values.back().multiplayerNote = "Deaths are credited server-side to the quest's authored scope; reward actions still target only their actor.";
		values.back().persistenceNote = "Pending Defeat ID credit is reconciled even when kills occur before that definition is first opened.";
		values.back().commonMistake = "Setting only Squad ID will coordinate the monsters but will not advance a defeat_id objective.";

		values.push_back(recipe("complex_quest_interaction", "Multi-Stage Branch, Flag, Objective, and Reward", "Advanced", "Advanced",
			"Build a staged quest whose later response requires both quest progress and a world consequence.",
			R"json({"version":2,"quest_id":"sealed_vault","quest":{"title":"The Sealed Vault","scope":"player","objectives":[{"id":"earn_trust","text":"Earn the keeper's trust.","stage":0},{"id":"open_vault","text":"Open the vault.","stage":1}]},"start_node":0,"nodes":[{"id":0,"text":"Will you prove yourself?","next":0,"choices":[{"id":"accept","text":"I will.","next":1,"once":true,"action":{"quest_start":true,"quest_accept":true,"objective_complete":"earn_trust","quest_stage":1,"set_world_flag":{"id":"keeper_trust","value":true}}}]},{"id":1,"text":"The vault awaits.","next":1,"choices":[{"id":"open","text":"Open the vault.","next":2,"conditions":[{"type":"quest_stage","quest":"sealed_vault","stage":1,"comparison":"at_least"},{"type":"world_flag","id":"keeper_trust","value":true},{"type":"objective_completed","quest":"sealed_vault","objective":"earn_trust"}],"action":{"objective_complete":"open_vault","quest_complete":true,"reward_gold":100}},{"id":"wait","text":"Not yet.","next":1}]},{"id":2,"text":"The vault opens and you take your reward.","next":2}]})json", true));
		values.back().steps = {
			"Create two staged objectives and a once-only acceptance response.",
			"In one action, accept the quest, complete stage-zero work, advance stage, and set the consequence flag.",
			"On the later response, use the Conditions array so quest stage, flag, and objective must all pass.",
			"Complete the final objective and quest in the same validated server action, then reward the actor.",
			"Use Sandbox Preview to seed each missing condition and test both hidden and available paths."
		};
		values.back().multiplayerNote = "The host revalidates all three conditions; the 100 gold reward goes once to the actor, not every shared observer.";
		values.back().commonMistake = "A Conditions array is logical AND; every referenced quest/objective ID must be stable and correctly scoped.";
		values.push_back(recipe("sam_stable_item", "S.A.M. Stable Item Reference", "Items", "Advanced",
			"Persist a custom item by stable ID, never by its session runtime number.", choiceExample(
			"\"condition\": {\"type\":\"has_item\",\"stable_id\":\"tutorial:custom_item\",\"count\":1}",
			"\n            \"reward_item\": {\"stable_id\":\"tutorial:custom_item\",\"count\":1}"), true));

		values.push_back(recipe("node_redirect", "Node Redirect Condition", "Advanced", "Advanced",
			"Branch before displaying a node and optionally consume the requirement.",
			"{\"version\":1,\"start_node\":0,\"nodes\":["
			"{\"id\":0,\"text\":\"Checking...\",\"next\":0,\"condition\":{\"type\":\"has_gold\",\"amount\":10,\"consume\":false,\"true_node\":1,\"false_node\":2}},"
			"{\"id\":1,\"text\":\"You have enough.\",\"next\":1},"
			"{\"id\":2,\"text\":\"You need more.\",\"next\":2}]}"));
		values.push_back(recipe("node_seen", "Remember a Seen Node", "Advanced", "Advanced",
			"Redirect using persistent node-seen memory.",
			"{\"version\":1,\"start_node\":0,\"nodes\":["
			"{\"id\":0,\"text\":\"Checking memory...\",\"next\":0,\"condition\":{\"type\":\"node_seen\",\"node\":\"node_1\",\"true_node\":2,\"false_node\":1}},"
			"{\"id\":1,\"text\":\"First meeting.\",\"next\":0},"
			"{\"id\":2,\"text\":\"Welcome back.\",\"next\":0}]}"));
		values.push_back(recipe("node_action", "One-Time Node Action", "Advanced", "Advanced",
			"Apply the node-only action subset once per player/NPC action ID.",
			"{\"version\":1," + questBlock() + "\n  \"start_node\":0,\"nodes\":["
			"{\"id\":0,\"text\":\"A node reward.\",\"next\":0,\"action\":{\"id\":\"node_reward\",\"quest_accept\":true,\"reward_gold\":10}}]}"));
		return values;
	}();
	return recipes;
}

const TutorialRecipe* findTutorialRecipe(const std::string& id)
{
	const auto& recipes = tutorialRecipes();
	const auto found = std::find_if(recipes.begin(), recipes.end(),
		[&id](const TutorialRecipe& value) { return value.id == id; });
	return found == recipes.end() ? nullptr : &*found;
}

std::string createStarterDocument(const std::string& templateID,
	const std::string& dialogueID, const std::string& questID,
	const std::string& npcText, const std::string& firstChoiceText,
	const std::string& questTitle, const std::string& questSummary)
{
	(void)dialogueID; // Resource identity lives in the filename by contract.
	const std::string cleanQuestID = normalizeID(questID);
	const bool hasQuest = !cleanQuestID.empty()
		|| templateID == "quest_giver";
	const std::string effectiveQuestID = cleanQuestID.empty()
		? "new_quest" : cleanQuestID;
	const bool branching = templateID == "two_choice_branch"
		|| templateID == "quest_giver" || templateID == "recruitable_npc";
	const bool empty = templateID == "empty_conversation";
	const bool recruit = templateID == "recruitable_npc";

	std::ostringstream json;
	json << "{\n  \"version\": " << SchemaVersion;
	if ( hasQuest )
	{
		json << ",\n  \"quest_id\": \"" << escapeJSON(effectiveQuestID) << "\","
			<< "\n  \"quest\": {"
			<< "\n    \"title\": \"" << escapeJSON(questTitle.empty() ? "New Quest" : questTitle) << "\","
			<< "\n    \"summary\": \"" << escapeJSON(questSummary) << "\","
			<< "\n    \"scope\": \"player\","
			<< "\n    \"repeatable\": false,"
			<< "\n    \"objectives\": []"
			<< "\n  }";
	}
	json << ",\n  \"start_node\": 0,\n  \"nodes\": [\n    {"
		<< "\n      \"id\": 0,"
		<< "\n      \"text\": \"" << escapeJSON(npcText.empty()
			? (empty ? "New dialogue node." : "Hello, traveler.") : npcText) << "\","
		<< "\n      \"next\": 0,"
		<< "\n      \"choices\": [";
	if ( !empty )
	{
		json << "\n        {\"id\": \"response_1\", \"text\": \""
			<< escapeJSON(firstChoiceText.empty() ? "Hello." : firstChoiceText)
			<< "\", \"next\": " << (branching ? 1 : 0);
		if ( hasQuest || recruit )
		{
			json << ", \"action\": {";
			bool comma = false;
			if ( hasQuest )
			{
				json << "\"quest_start\": true, \"quest_accept\": true";
				comma = true;
			}
			if ( recruit ) json << (comma ? ", " : "") << "\"recruit_npc\": true";
			json << "}";
		}
		json << "}";
		if ( branching )
			json << ",\n        {\"id\": \"response_2\", \"text\": \"Not right now.\", \"next\": 0}";
		json << '\n';
	}
	json << "      ]\n    }";
	if ( branching )
		json << ",\n    {\"id\": 1, \"text\": \"Thank you.\", \"next\": 1}";
	json << "\n  ]\n}";
	return json.str();
}

} // namespace dialogue
} // namespace automatia
