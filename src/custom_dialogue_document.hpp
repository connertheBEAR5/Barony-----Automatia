/*-------------------------------------------------------------------------------

	BARONY AUTOMATIA
	File: custom_dialogue_document.hpp
	Desc: Lossless custom-dialogue authoring model, schema metadata, validation,
	      tutorial recipes, and sandboxed editor preview.

-------------------------------------------------------------------------------*/

#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <rapidjson/document.h>

namespace automatia
{
namespace dialogue
{

constexpr int SchemaVersion = 1;
constexpr std::size_t MaximumDocumentBytes = 64 * 1024;

enum class CapabilityOwner
{
	Root,
	Quest,
	Origin,
	Objective,
	Node,
	Choice,
	NodeCondition,
	ChoiceCondition,
	NodeAction,
	ChoiceAction,
	ItemReference
};

enum class CapabilityValue
{
	Object,
	Array,
	String,
	Integer,
	Boolean,
	Item,
	NodeReference,
	QuestReference,
	ObjectiveReference,
	EffectReference
};

struct Capability
{
	CapabilityOwner owner = CapabilityOwner::Root;
	const char* key = "";
	const char* label = "";
	CapabilityValue value = CapabilityValue::String;
	bool required = false;
	const char* defaultValue = "";
	const char* validDomain = "";
	const char* category = "";
};

const std::vector<Capability>& capabilities();
const std::vector<std::string>& choiceConditionTypes();
const std::vector<std::string>& nodeConditionTypes();
const std::vector<std::string>& choiceActionFields();
const std::vector<std::string>& nodeActionFields();
bool isChoiceConditionType(const std::string& type);
bool isNodeConditionType(const std::string& type);
bool isChoiceActionField(const std::string& field);
bool isNodeActionField(const std::string& field);
bool isSafeStableItemID(const std::string& value);

enum class Severity
{
	Error,
	Warning,
	Info
};

enum class LocationKind
{
	Document,
	Quest,
	Origin,
	Objective,
	Node,
	Choice,
	Condition,
	Action
};

struct Location
{
	LocationKind kind = LocationKind::Document;
	std::string path;
	int nodeIndex = -1;
	int nodeID = -1;
	int choiceIndex = -1;
	std::string choiceID;
	int conditionIndex = -1;
	int objectiveIndex = -1;
	std::string objectiveID;
};

struct Issue
{
	Severity severity = Severity::Error;
	std::string code;
	std::string message;
	Location location;
};

struct ValidationOptions
{
	int vanillaItemCount = 0;
	int effectCount = 160;
	bool allowStableItemIDs = true;
	std::function<bool(const std::string&)> stableItemAvailable;
};

std::vector<Issue> validate(
	const rapidjson::Value& document,
	const ValidationOptions& options = ValidationOptions{});
bool hasErrors(const std::vector<Issue>& issues);
int countIssues(const std::vector<Issue>& issues, Severity severity);
const char* severityName(Severity severity);

std::string conditionSummary(const rapidjson::Value& condition);
std::vector<std::string> actionSummaries(const rapidjson::Value& action);
std::string itemReferenceSummary(const rapidjson::Value& itemReference);

class Document
{
public:
	Document();

	bool parse(const std::string& text, std::string& error);
	bool replaceWithEdit(const std::string& text, const std::string& label,
		std::string& error);
	bool loadFile(const std::string& path, std::string& error);
	bool saveAtomic(const std::string& path, std::string& error);

	rapidjson::Document& json();
	const rapidjson::Document& json() const;
	std::string serialize(bool pretty = true) const;

	// Existing Zed code edits the RapidJSON DOM directly. Calling this after a
	// logical edit records one undo checkpoint without requiring two JSON models.
	bool recordExternalEdit(const std::string& label);
	bool undo();
	bool redo();
	bool canUndo() const;
	bool canRedo() const;
	const std::string& nextUndoLabel() const;
	const std::string& nextRedoLabel() const;

	bool dirty() const;
	void markClean();
	void reset();

private:
	struct Snapshot
	{
		std::string text;
		std::string label;
	};

	bool restore(const std::string& text);
	void trimHistory();

	rapidjson::Document document_;
	std::string currentSnapshot_;
	std::string cleanSnapshot_;
	std::vector<Snapshot> undo_;
	std::vector<Snapshot> redo_;
};

struct PreviewQuestState
{
	bool started = false;
	bool accepted = false;
	bool completed = false;
	bool failed = false;
	int stage = 0;
	std::map<std::string, bool> objectives;
	std::map<std::string, int> variables;
};

struct PreviewState
{
	int gold = 0;
	std::map<std::string, int> items;
	std::map<std::string, PreviewQuestState> quests;
	std::map<std::string, bool> worldFlags;
	std::map<std::string, bool> npcFlags;
	std::map<std::string, int> worldVariables;
	std::map<std::string, int> npcVariables;
	std::set<std::string> seenNodes;
	std::set<std::string> usedChoices;
};

struct PreviewChoice
{
	int sourceIndex = -1;
	std::string id;
	std::string text;
	int nextNode = 0;
	std::string condition;
	std::vector<std::string> actions;
};

struct PreviewFrame
{
	bool valid = false;
	int nodeID = -1;
	std::string npcText;
	std::vector<PreviewChoice> choices;
	std::vector<std::string> notices;
	std::string error;
};

class PreviewSession
{
public:
	bool begin(const rapidjson::Value& document, const PreviewState& state,
		std::string& error);
	bool choose(int visibleChoiceIndex, std::string& error);
	void reset();

	const PreviewFrame& frame() const;
	PreviewState& state();
	const PreviewState& state() const;
	const std::vector<std::string>& unavailableActions() const;

private:
	bool rebuildFrame(std::string& error);
	bool evaluateCondition(const rapidjson::Value& condition) const;
	bool applyChoiceAction(const rapidjson::Value& action, std::string& error);
	const rapidjson::Value* findNode(int nodeID) const;
	std::string rootQuestID() const;

	const rapidjson::Value* document_ = nullptr;
	PreviewState initialState_;
	PreviewState state_;
	int currentNode_ = 0;
	PreviewFrame frame_;
	std::vector<std::string> unavailableActions_;
};

struct TutorialRecipe
{
	std::string id;
	std::string title;
	std::string category;
	std::string difficulty;
	std::string goal;
	std::string playerExperience;
	std::string panelHint;
	std::vector<std::string> concepts;
	std::vector<std::string> steps;
	std::string expectedResult;
	std::string multiplayerNote;
	std::string persistenceNote;
	std::string commonMistake;
	std::string exampleJson;
	bool manualGameTestRequired = false;
};

const std::vector<TutorialRecipe>& tutorialRecipes();
const TutorialRecipe* findTutorialRecipe(const std::string& id);

// Creates a verified starter document without touching the filesystem. The
// caller owns filename collision handling and safe publication.
std::string createStarterDocument(
	const std::string& templateID,
	const std::string& dialogueID,
	const std::string& questID,
	const std::string& npcText,
	const std::string& firstChoiceText,
	const std::string& questTitle,
	const std::string& questSummary);

} // namespace dialogue
} // namespace automatia
