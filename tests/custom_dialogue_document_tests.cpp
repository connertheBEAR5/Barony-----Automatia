#include "custom_dialogue_document.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>

namespace
{
	bool expect(const bool condition, const char* expression, const int line)
	{
		if ( !condition )
		{
			std::cerr << "FAILED line " << line << ": " << expression << '\n';
		}
		return condition;
	}

#define EXPECT(expression) do { if (!expect((expression), #expression, __LINE__)) return false; } while (false)

	using automatia::dialogue::Document;
	using automatia::dialogue::Issue;
	using automatia::dialogue::Severity;
	using automatia::dialogue::ValidationOptions;

	bool hasCode(const std::vector<Issue>& issues, const std::string& code)
	{
		return std::any_of(issues.begin(), issues.end(), [&](const Issue& issue)
			{ return issue.code == code; });
	}

	std::string readFile(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		std::ostringstream text;
		text << input.rdbuf();
		return input ? text.str() : std::string{};
	}

	std::string conditionFixture(const std::string& type, const bool node)
	{
		std::string fields;
		if ( type == "has_item" )
			fields = ",\"item\":\"torch\",\"count\":1" + std::string(node ? ",\"consume\":false" : "");
		else if ( type == "has_gold" )
			fields = ",\"amount\":0" + std::string(node ? ",\"consume\":false" : "");
		else if ( type == "quest_started" || type == "quest_accepted"
			|| type == "quest_completed" || type == "quest_failed" )
			fields = ",\"quest\":\"quest_a\"";
		else if ( type == "quest_stage" )
			fields = ",\"quest\":\"quest_a\",\"stage\":1,\"comparison\":\"equals\"";
		else if ( type == "objective_completed" || type == "objective_incomplete" )
			fields = ",\"quest\":\"quest_a\",\"objective\":\"objective_1\"";
		else if ( type == "node_seen" )
			fields = ",\"node\":\"node_0\"";
		else if ( type == "world_flag" || type == "npc_flag" )
			fields = ",\"id\":\"flag\",\"value\":true";
		else if ( type == "world_variable" || type == "npc_variable" )
			fields = ",\"id\":\"variable\",\"value\":1,\"comparison\":\"equals\"";
		return "{\"type\":\"" + type + "\"" + fields
			+ (node ? ",\"true_node\":0,\"false_node\":0" : "") + "}";
	}

	std::string actionFieldFixture(const std::string& field)
	{
		if ( field == "quest_start" || field == "quest_accept"
			|| field == "quest_complete" || field == "quest_fail"
			|| field == "quest_reset" || field == "recruit_npc" )
			return "\"" + field + "\":true";
		if ( field == "quest_stage" || field == "reward_gold" || field == "remove_gold" )
			return "\"" + field + "\":1";
		if ( field == "reward_item" || field == "remove_item" )
			return "\"" + field + "\":{\"item\":\"torch\",\"count\":1}";
		if ( field == "objective_complete" || field == "objective_clear" )
			return "\"" + field + "\":\"objective_1\"";
		if ( field == "set_world_flag" || field == "set_npc_flag" )
			return "\"" + field + "\":{\"id\":\"flag\",\"value\":true}";
		if ( field == "set_world_variable" || field == "set_npc_variable"
			|| field == "set_quest_variable" )
			return "\"" + field + "\":{\"id\":\"variable\",\"value\":1}";
		if ( field == "add_world_variable" || field == "add_npc_variable"
			|| field == "add_quest_variable" )
			return "\"" + field + "\":{\"id\":\"variable\",\"amount\":1}";
		if ( field == "set_power" )
			return "\"set_power\":{\"x\":1,\"y\":2,\"powered\":true}";
		if ( field == "status_effect" )
			return "\"status_effect\":{\"effect\":0,\"duration_seconds\":1,"
				"\"strength\":1,\"enabled\":true}";
		return {};
	}

	std::string fixtureRoot(const std::string& nodeMembers)
	{
		return "{\"version\":1,\"quest_id\":\"quest_a\",\"quest\":{"
			"\"title\":\"Quest A\",\"scope\":\"player\",\"repeatable\":true,"
			"\"objectives\":[{\"id\":\"objective_1\",\"text\":\"Objective\"}]},"
			"\"start_node\":0,\"nodes\":[{\"id\":0,\"text\":\"Node\",\"next\":0"
			+ nodeMembers + "}]}";
	}

	bool testTutorialLibraryIsVerified()
	{
		using namespace automatia::dialogue;
		const auto& tutorials = tutorialRecipes();
		EXPECT(tutorials.size() == 39);
		std::set<std::string> ids;
		ValidationOptions options;
		options.vanillaItemCount = 400;
		options.effectCount = 256;
		options.stableItemAvailable = [](const std::string&) { return true; };
		for ( const TutorialRecipe& tutorial : tutorials )
		{
			EXPECT(!tutorial.id.empty());
			EXPECT(!tutorial.title.empty());
			EXPECT(!tutorial.playerExperience.empty());
			EXPECT(!tutorial.panelHint.empty());
			EXPECT(!tutorial.steps.empty());
			EXPECT(ids.insert(tutorial.id).second);
			Document document;
			std::string error;
			if ( !document.parse(tutorial.exampleJson, error) )
			{
				std::cerr << "Tutorial '" << tutorial.id << "' failed to parse: "
					<< error << '\n';
				return false;
			}
			const auto issues = validate(document.json(), options);
			if ( hasErrors(issues) )
			{
				std::cerr << "Tutorial '" << tutorial.id << "' has validation errors:\n";
				for ( const Issue& issue : issues )
				{
					if ( issue.severity == Severity::Error )
						std::cerr << "  " << issue.location.path << ": " << issue.message << '\n';
				}
				return false;
			}
			const std::string canonical = document.serialize(false);
			Document roundTripped;
			EXPECT(roundTripped.parse(canonical, error));
			EXPECT(roundTripped.serialize(false) == canonical);
		}
		EXPECT(findTutorialRecipe("sam_stable_item") != nullptr);
		EXPECT(findTutorialRecipe("party_shared_quest") != nullptr);
		EXPECT(findTutorialRecipe("world_shared_quest") != nullptr);
		EXPECT(findTutorialRecipe("floor_aware_markers") != nullptr);
		EXPECT(findTutorialRecipe("enemy_group_defeat") != nullptr);
		EXPECT(findTutorialRecipe("complex_quest_interaction") != nullptr);
		EXPECT(findTutorialRecipe("not_a_recipe") == nullptr);
		return true;
	}

	bool testLosslessRoundTripDirtyHistoryAndAtomicSave()
	{
		const std::string source =
			"{\"version\":1,\"vendor_extension\":{\"keep\":[3,2,1]},"
			"\"start_node\":0,\"nodes\":[{\"id\":0,\"text\":\"Original\","
			"\"next\":0,\"choices\":[{\"id\":\"keep\",\"text\":\"Keep\","
			"\"next\":0,\"custom_choice\":true,\"condition\":{\"type\":\"has_item\","
			"\"item\":\"torch\",\"condition_extension\":{\"enabled\":true}},"
			"\"conditions\":[{\"type\":\"has_gold\",\"amount\":0,"
			"\"array_extension\":[\"keep\"]}],\"action\":{\"reward_item\":{"
			"\"item\":\"torch\",\"count\":1,\"item_extension\":17},"
			"\"custom_action\":{\"x\":7}}}]}]}";
		Document document;
		std::string error;
		EXPECT(document.parse(source, error));
		EXPECT(!document.dirty());
		const std::string serialized = document.serialize(false);
		EXPECT(serialized.find("vendor_extension") != std::string::npos);
		EXPECT(serialized.find("custom_choice") != std::string::npos);
		EXPECT(serialized.find("custom_action") != std::string::npos);

		auto& json = document.json();
		auto& text = json["nodes"][0]["text"];
		text.SetString("Changed", json.GetAllocator());
		EXPECT(document.recordExternalEdit("Edit NPC text"));
		EXPECT(document.dirty());
		EXPECT(document.canUndo());
		EXPECT(document.nextUndoLabel() == "Edit NPC text");
		EXPECT(document.undo());
		EXPECT(std::string(document.json()["nodes"][0]["text"].GetString()) == "Original");
		EXPECT(!document.dirty());
		EXPECT(document.redo());
		EXPECT(std::string(document.json()["nodes"][0]["text"].GetString()) == "Changed");
		EXPECT(document.replaceWithEdit(
			"{\"version\":1,\"start_node\":4,\"nodes\":[{\"id\":4,\"text\":\"Raw\",\"next\":4}]}",
			"Apply Advanced JSON", error));
		EXPECT(document.json()["start_node"].GetInt() == 4);
		EXPECT(document.undo());
		EXPECT(std::string(document.json()["nodes"][0]["text"].GetString()) == "Changed");
		EXPECT(document.redo());
		EXPECT(document.json()["start_node"].GetInt() == 4);
		EXPECT(document.undo());
		EXPECT(std::string(document.json()["nodes"][0]["text"].GetString()) == "Changed");

		const std::filesystem::path path = std::filesystem::temp_directory_path()
			/ "automatia_custom_dialogue_document_test.json";
		const std::filesystem::path temporary = path.string() + ".automatia-dialogue.tmp";
		std::error_code ignored;
		std::filesystem::remove(path, ignored);
		std::filesystem::remove(temporary, ignored);
		EXPECT(document.saveAtomic(path.string(), error));
		EXPECT(!document.dirty());
		EXPECT(std::filesystem::is_regular_file(path));
		EXPECT(!std::filesystem::exists(temporary));
		const std::string saved = readFile(path);
		EXPECT(saved.find("vendor_extension") != std::string::npos);
		EXPECT(saved.find("\"Changed\"") != std::string::npos);
		Document reloaded;
		EXPECT(reloaded.loadFile(path.string(), error));
		EXPECT(reloaded.json()["vendor_extension"]["keep"].Size() == 3);
		const auto& reloadedChoice = reloaded.json()["nodes"][0]["choices"][0];
		EXPECT(reloadedChoice.HasMember("custom_choice"));
		EXPECT(reloadedChoice["condition"].HasMember("condition_extension"));
		EXPECT(reloadedChoice["conditions"][0].HasMember("array_extension"));
		EXPECT(reloadedChoice["action"]["reward_item"].HasMember("item_extension"));
		EXPECT(reloadedChoice["action"].HasMember("custom_action"));
		std::filesystem::remove(path, ignored);

		Document oversized;
		EXPECT(oversized.parse(
			"{\"version\":1,\"start_node\":0,\"nodes\":[{\"id\":0,"
			"\"text\":\"Small\",\"next\":0}]}", error));
		const std::string largeText(automatia::dialogue::MaximumDocumentBytes, 'x');
		oversized.json()["nodes"][0]["text"].SetString(largeText.c_str(),
			static_cast<rapidjson::SizeType>(largeText.size()),
			oversized.json().GetAllocator());
		EXPECT(oversized.recordExternalEdit("Grow past runtime limit"));
		EXPECT(!oversized.saveAtomic(path.string(), error));
		EXPECT(error.find("64 KiB") != std::string::npos);
		EXPECT(!std::filesystem::exists(path));
		EXPECT(!std::filesystem::exists(temporary));
		return true;
	}

	bool testValidationAndLocations()
	{
		Document document;
		std::string error;
		EXPECT(document.parse(
			"{\"version\":1,\"start_node\":0,\"nodes\":["
			"{\"id\":0,\"text\":\"A\",\"next\":99,\"choices\":["
			"{\"id\":\"same\",\"text\":\"A\",\"next\":99},"
			"{\"id\":\"same\",\"text\":\"B\",\"next\":0}]},"
			"{\"id\":2,\"text\":\"Unused\",\"next\":2}]}", error));
		const auto issues = automatia::dialogue::validate(document.json());
		EXPECT(automatia::dialogue::hasErrors(issues));
		EXPECT(hasCode(issues, "duplicate_choice_id"));
		EXPECT(hasCode(issues, "broken_node_link"));
		EXPECT(hasCode(issues, "unreachable_node"));
		const auto duplicate = std::find_if(issues.begin(), issues.end(), [](const Issue& issue)
			{ return issue.code == "duplicate_choice_id"; });
		EXPECT(duplicate != issues.end());
		EXPECT(duplicate->location.nodeID == 0);
		EXPECT(duplicate->location.choiceIndex == 1);
		EXPECT(duplicate->location.path == "$.nodes[0].choices[1]");

		Document stable;
		EXPECT(stable.parse(
			"{\"version\":1,\"start_node\":0,\"nodes\":[{\"id\":0,"
			"\"text\":\"Stable\",\"next\":0,\"choices\":[{\"id\":\"item\","
			"\"text\":\"Item\",\"next\":0,\"condition\":{\"type\":\"has_item\","
			"\"stable_id\":\"example:crystal\",\"count\":1}}]}]}", error));
		ValidationOptions options;
		options.stableItemAvailable = [](const std::string&) { return false; };
		const auto stableIssues = automatia::dialogue::validate(stable.json(), options);
		EXPECT(!automatia::dialogue::hasErrors(stableIssues));
		EXPECT(hasCode(stableIssues, "missing_stable_item"));
		EXPECT(!hasCode(stableIssues, "unknown_field"));

		Document references;
		EXPECT(references.parse(
			"{\"version\":1,\"quest_id\":\"quest_a\",\"quest\":{\"title\":\"Quest\","
			"\"scope\":\"player\",\"objectives\":[{\"id\":\"present\",\"text\":\"Present\"}]},"
			"\"start_node\":0,\"nodes\":[{\"id\":0,\"text\":\"A\",\"next\":1,"
			"\"action\":{\"id\":\"shared_action\"},\"choices\":[{\"id\":\"missing_ref\","
			"\"text\":\"Check\",\"next\":1,\"condition\":{\"type\":\"objective_completed\","
			"\"quest\":\"quest_a\",\"objective\":\"absent\"},\"action\":{"
			"\"objective_complete\":\"also_absent\"}}]},{\"id\":1,\"text\":\"B\","
			"\"next\":1,\"action\":{\"id\":\"shared_action\"}}]}", error));
		const auto referenceIssues = automatia::dialogue::validate(references.json());
		EXPECT(hasCode(referenceIssues, "duplicate_node_action_id"));
		EXPECT(hasCode(referenceIssues, "missing_objective_reference"));

		Document externalObjective;
		EXPECT(externalObjective.parse(
			"{\"version\":1,\"start_node\":0,\"nodes\":[{\"id\":0,"
			"\"text\":\"External\",\"next\":0,\"choices\":[{\"id\":\"check\","
			"\"text\":\"Check\",\"next\":0,\"condition\":{"
			"\"type\":\"objective_completed\",\"quest\":\"another_quest\","
			"\"objective\":\"external_objective\"}}]}]}", error));
		const auto externalIssues = automatia::dialogue::validate(
			externalObjective.json());
		EXPECT(!hasCode(externalIssues, "missing_objective_reference"));
		EXPECT(!automatia::dialogue::hasErrors(externalIssues));

		Document invalidGlobalComparison;
		EXPECT(invalidGlobalComparison.parse(
			"{\"version\":1,\"start_node\":0,\"nodes\":[{\"id\":0,"
			"\"text\":\"Compare\",\"next\":0,\"choices\":[{\"id\":\"item\","
			"\"text\":\"Item\",\"next\":0,\"condition\":{\"type\":\"has_item\","
			"\"item\":\"torch\",\"comparison\":17}}]}]}", error));
		EXPECT(hasCode(automatia::dialogue::validate(
			invalidGlobalComparison.json()), "invalid_comparison"));
		return true;
	}

	bool testSandboxPreview()
	{
		using namespace automatia::dialogue;
		Document document;
		std::string error;
		EXPECT(document.parse(
			"{\"version\":1,\"quest_id\":\"preview_quest\","
			"\"quest\":{\"title\":\"Preview\",\"scope\":\"player\"},"
			"\"start_node\":0,\"nodes\":[{\"id\":0,\"text\":\"Pay?\",\"next\":0,"
			"\"choices\":[{\"id\":\"pay\",\"text\":\"Pay\",\"next\":1,\"once\":true,"
			"\"conditions\":[{\"type\":\"has_gold\",\"amount\":5},{\"type\":"
			"\"world_flag\",\"id\":\"open\",\"value\":false}],\"action\":{"
			"\"remove_gold\":5,\"reward_item\":{\"stable_id\":\"example:token\",\"count\":2},"
			"\"quest_accept\":true,\"set_world_flag\":{\"id\":\"open\",\"value\":true},"
			"\"set_power\":{\"x\":1,\"y\":2,\"powered\":true}}}]},"
			"{\"id\":1,\"text\":\"Done\",\"next\":1}]}", error));
		PreviewState state;
		state.gold = 10;
		PreviewSession preview;
		EXPECT(preview.begin(document.json(), state, error));
		EXPECT(preview.frame().valid);
		EXPECT(preview.frame().choices.size() == 1);
		EXPECT(preview.frame().choices[0].condition.find(" AND ") != std::string::npos);
		EXPECT(preview.choose(0, error));
		EXPECT(preview.frame().nodeID == 1);
		EXPECT(preview.state().gold == 5);
		EXPECT(preview.state().items.at("example:token") == 2);
		EXPECT(preview.state().worldFlags.at("open"));
		EXPECT(preview.state().quests.at("preview_quest").accepted);
		EXPECT(preview.unavailableActions().size() == 1);
		preview.reset();
		EXPECT(preview.state().gold == 10);
		EXPECT(preview.state().items.empty());
		return true;
	}

	bool testLegacyDialogueVisualModelAndPreview()
	{
		using namespace automatia::dialogue;
		Document document;
		std::string error;
		EXPECT(document.parse(
			"{\"version\":1,\"text\":\"A legacy greeting.\","
			"\"legacy_extension\":{\"keep\":true}}", error));
		const auto issues = validate(document.json());
		EXPECT(!hasErrors(issues));
		EXPECT(hasCode(issues, "legacy_document"));
		EXPECT(hasCode(issues, "unknown_field"));

		PreviewSession preview;
		PreviewState state;
		EXPECT(preview.begin(document.json(), state, error));
		EXPECT(preview.frame().valid);
		EXPECT(preview.frame().nodeID == 0);
		EXPECT(preview.frame().npcText == "A legacy greeting.");
		EXPECT(preview.frame().choices.empty());
		EXPECT(!preview.frame().notices.empty());

		document.json()["text"].SetString(
			"Edited legacy greeting.", document.json().GetAllocator());
		EXPECT(document.recordExternalEdit("Edit legacy text"));
		EXPECT(document.serialize(false).find("legacy_extension") != std::string::npos);
		EXPECT(document.undo());
		EXPECT(std::string(document.json()["text"].GetString())
			== "A legacy greeting.");
		return true;
	}

	bool testSandboxEveryConditionFamily()
	{
		using namespace automatia::dialogue;
		std::string choices;
		int choiceIndex = 0;
		for ( const std::string& type : choiceConditionTypes() )
		{
			std::string condition = conditionFixture(type, false);
			if ( type == "objective_incomplete" )
			{
				const std::string oldReference = "\"objective\":\"objective_1\"";
				condition.replace(condition.find(oldReference), oldReference.size(),
					"\"objective\":\"objective_2\"");
			}
			if ( !choices.empty() ) choices += ",";
			choices += "{\"id\":\"condition_" + std::to_string(choiceIndex)
				+ "\",\"text\":\"Visible\",\"next\":1,\"condition\":"
				+ condition + "}";
			++choiceIndex;
		}
		Document document;
		std::string error;
		EXPECT(document.parse(
			"{\"version\":1,\"start_node\":0,\"nodes\":["
			"{\"id\":0,\"text\":\"Conditions\",\"next\":0,\"choices\":["
			+ choices + "]},{\"id\":1,\"text\":\"Done\",\"next\":1}]}", error));
		PreviewState state;
		state.gold = 1;
		state.items["torch"] = 1;
		auto& quest = state.quests["quest_a"];
		quest.started = true;
		quest.accepted = true;
		quest.completed = true;
		quest.failed = true;
		quest.stage = 1;
		quest.objectives["objective_1"] = true;
		quest.objectives["objective_2"] = false;
		state.worldFlags["flag"] = true;
		state.npcFlags["flag"] = true;
		state.worldVariables["variable"] = 1;
		state.npcVariables["variable"] = 1;
		PreviewSession preview;
		EXPECT(preview.begin(document.json(), state, error));
		EXPECT(preview.frame().choices.size() == choiceConditionTypes().size());

		for ( const std::string& type : nodeConditionTypes() )
		{
			std::string condition = conditionFixture(type, true);
			const std::string branches = "\"true_node\":0,\"false_node\":0";
			const std::size_t branchOffset = condition.find(branches);
			EXPECT(branchOffset != std::string::npos);
			condition.replace(branchOffset, branches.size(),
				"\"true_node\":1,\"false_node\":2");
			Document redirect;
			EXPECT(redirect.parse(
				"{\"version\":1,\"start_node\":0,\"nodes\":["
				"{\"id\":0,\"text\":\"Check\",\"condition\":" + condition + "},"
				"{\"id\":1,\"text\":\"True\",\"next\":1},"
				"{\"id\":2,\"text\":\"False\",\"next\":2}]}", error));
			PreviewState redirectState = state;
			redirectState.seenNodes.insert("node_0");
			PreviewSession redirectPreview;
			EXPECT(redirectPreview.begin(redirect.json(), redirectState, error));
			EXPECT(redirectPreview.frame().nodeID == 1);
		}
		return true;
	}

	bool testCapabilityAndRuntimeParityContract()
	{
		using namespace automatia::dialogue;
		EXPECT(isSafeStableItemID("example:crystal"));
		EXPECT(!isSafeStableItemID(":crystal"));
		EXPECT(!isSafeStableItemID("example:"));
		EXPECT(!isSafeStableItemID("example:item/path"));
		EXPECT(capabilities().size() >= 45);
		const std::set<std::string> expectedChoiceConditions = {
			"has_item", "has_gold", "quest_started", "quest_accepted",
			"quest_completed", "quest_failed", "quest_stage", "objective_completed",
			"objective_incomplete", "world_flag", "npc_flag", "world_variable",
			"npc_variable"
		};
		EXPECT(std::set<std::string>(choiceConditionTypes().begin(),
			choiceConditionTypes().end()) == expectedChoiceConditions);
		const std::set<std::string> expectedNodeConditions = {
			"has_item", "has_gold", "quest_started", "quest_accepted",
			"quest_completed", "quest_failed", "quest_stage", "node_seen",
			"world_flag", "npc_flag", "world_variable", "npc_variable"
		};
		EXPECT(std::set<std::string>(nodeConditionTypes().begin(),
			nodeConditionTypes().end()) == expectedNodeConditions);
		const std::set<std::string> expectedChoiceActions = {
			"quest_start", "quest_accept", "quest_complete", "quest_fail",
			"quest_stage", "reward_gold", "reward_item", "status_effect",
			"remove_gold", "remove_item", "objective_complete", "objective_clear",
			"quest_reset", "set_power", "recruit_npc", "set_world_flag",
			"set_npc_flag", "set_quest_variable", "add_quest_variable",
			"set_world_variable", "add_world_variable", "set_npc_variable",
			"add_npc_variable"
		};
		EXPECT(std::set<std::string>(choiceActionFields().begin(),
			choiceActionFields().end()) == expectedChoiceActions);
		const std::set<std::string> expectedNodeActions = {
			"id", "quest_accept", "quest_complete", "quest_stage", "reward_gold",
			"reward_item", "set_world_flag", "set_world_variable",
			"add_world_variable", "set_npc_flag", "set_npc_variable"
		};
		EXPECT(std::set<std::string>(nodeActionFields().begin(),
			nodeActionFields().end()) == expectedNodeActions);

		const std::string runtime = readFile(
			std::filesystem::path(BARONY_SOURCE_DIR) / "src/actmonster.cpp");
		EXPECT(!runtime.empty());
		for ( const std::string& condition : choiceConditionTypes() )
			EXPECT(runtime.find("\"" + condition + "\"") != std::string::npos);
		for ( const std::string& action : choiceActionFields() )
			EXPECT(runtime.find("\"" + action + "\"") != std::string::npos);
		for ( const std::string& condition : nodeConditionTypes() )
			EXPECT(runtime.find("\"" + condition + "\"") != std::string::npos);
		for ( const std::string& action : nodeActionFields() )
			EXPECT(runtime.find("\"" + action + "\"") != std::string::npos);
		EXPECT(runtime.find("if ( multiplayer == CLIENT )") != std::string::npos);
		EXPECT(runtime.find("\"CDSL\"") != std::string::npos);
		EXPECT(runtime.find("pendingCustomDialogueChoices") != std::string::npos);
		EXPECT(runtime.find("serverSyncAutomatiaQuestStateForActor") != std::string::npos);
		return true;
	}

	bool testEveryConditionAndActionFixture()
	{
		using namespace automatia::dialogue;
		ValidationOptions options;
		options.vanillaItemCount = 400;
		options.effectCount = 256;
		std::string error;
		for ( const std::string& type : choiceConditionTypes() )
		{
			Document document;
			const std::string choice = ",\"choices\":[{\"id\":\"choice\","
				"\"text\":\"Choice\",\"next\":0,\"condition\":"
				+ conditionFixture(type, false) + "}]";
			EXPECT(document.parse(fixtureRoot(choice), error));
			if ( hasErrors(validate(document.json(), options)) )
			{
				std::cerr << "Choice condition fixture failed: " << type << '\n';
				return false;
			}
		}
		for ( const std::string& type : nodeConditionTypes() )
		{
			Document document;
			EXPECT(document.parse(fixtureRoot(",\"condition\":"
				+ conditionFixture(type, true)), error));
			if ( hasErrors(validate(document.json(), options)) )
			{
				std::cerr << "Node condition fixture failed: " << type << '\n';
				return false;
			}
		}
		for ( const std::string& field : choiceActionFields() )
		{
			Document document;
			const std::string choice = ",\"choices\":[{\"id\":\"choice\","
				"\"text\":\"Choice\",\"next\":0,\"action\":{"
				+ actionFieldFixture(field) + "}}]";
			EXPECT(document.parse(fixtureRoot(choice), error));
			if ( hasErrors(validate(document.json(), options)) )
			{
				std::cerr << "Choice action fixture failed: " << field << '\n';
				return false;
			}
		}
		for ( const std::string& field : nodeActionFields() )
		{
			Document document;
			const std::string member = field == "id" ? std::string{}
				: "," + actionFieldFixture(field);
			EXPECT(document.parse(fixtureRoot(",\"action\":{\"id\":\"once_action\""
				+ member + "}"), error));
			if ( hasErrors(validate(document.json(), options)) )
			{
				std::cerr << "Node action fixture failed: " << field << '\n';
				return false;
			}
		}
		return true;
	}

	bool testEditorAndIntegrationSourceContracts()
	{
		const std::string editor = readFile(
			std::filesystem::path(BARONY_SOURCE_DIR) / "src/editor.cpp");
		const std::string buttons = readFile(
			std::filesystem::path(BARONY_SOURCE_DIR) / "src/buttons.cpp");
		EXPECT(!editor.empty());
		EXPECT(!buttons.empty());
		for ( const std::string& token : {
			"questDialogueEditorAddNode", "questDialogueEditorDuplicateSelectedNode",
			"questDialogueEditorDeleteNode", "questDialogueEditorAddChoice",
			"questDialogueEditorDuplicateSelectedChoice",
			"questDialogueEditorMoveSelectedChoice", "questDialogueEditorDeleteChoice",
			"questDialogueEditorAddObjective",
			"questDialogueEditorDuplicateSelectedObjective",
			"questDialogueEditorMoveSelectedObjective",
			"questDialogueEditorDeleteObjective",
			"questDialogueEditorRenameSelectedFile",
			"questDialogueEditorDuplicateSelectedFileNow",
			"questDialogueEditorDeleteSelectedFileNow",
			"questDialogueEditorDrawWizard", "questDialogueEditorDrawPreviewPage",
			"questDialogueEditorDrawJSONPage", "questDialogueEditorDrawValidationPage",
			"questDialogueEditorJSONHandleKey", "APPLY & SAVE",
			"questDialogueEditorBeginMarkerPick", "MANUAL COORDS",
			"USE WHOLE COLUMN", "USE SAME FLOOR",
			"questDialogueEditorUpgradeSharedQuestOwnership", "UPGRADE SHARING",
			"questDialogueEditorSelectStableItem", "editorSAMItemCatalogCount",
			"replaceWithEdit", "saveAtomic" } )
		{
			EXPECT(editor.find(token) != std::string::npos);
		}
		EXPECT(buttons.find("editorSAMItemStableIDIsAvailable") != std::string::npos);
		EXPECT(buttons.find("sam_item_catalog.json") != std::string::npos);
		return true;
	}

	bool testFloorAwareMarkerValidation()
	{
		using namespace automatia::dialogue;
		Document valid;
		std::string error;
		EXPECT(valid.parse(
			"{\"version\":2,\"quest_id\":\"tower\",\"quest\":"
			"{"
			"\"title\":\"Tower\",\"origin\":{\"map\":\"tower.lmp\",\"x\":1,\"y\":2,"
			"\"playable_floor\":3,\"floor_visibility\":\"same_floor\"},"
			"\"objectives\":[{\"id\":\"signal\",\"text\":\"Find it.\","
			"\"map_marker\":{\"map\":\"tower.lmp\",\"x\":4,\"y\":5,"
			"\"floor_visibility\":\"column\"}}]},\"start_node\":0,"
			"\"nodes\":[{\"id\":0,\"text\":\"Go.\",\"next\":0}]}", error));
		EXPECT(!hasErrors(validate(valid.json())));

		Document missingFloor;
		EXPECT(missingFloor.parse(
			"{\"version\":2,\"quest_id\":\"tower\",\"quest\":"
			"{"
			"\"title\":\"Tower\",\"origin\":{\"map\":\"tower.lmp\",\"x\":1,\"y\":2,"
			"\"floor_visibility\":\"same_floor\"}},\"start_node\":0,"
			"\"nodes\":[{\"id\":0,\"text\":\"Go.\",\"next\":0}]}", error));
		EXPECT(hasCode(validate(missingFloor.json()), "missing_origin_floor"));

		Document invalidVisibility;
		EXPECT(invalidVisibility.parse(
			"{\"version\":2,\"quest_id\":\"tower\",\"quest\":"
			"{"
			"\"title\":\"Tower\",\"objectives\":[{\"id\":\"signal\",\"text\":\"Find it.\","
			"\"map_marker\":{\"map\":\"tower.lmp\",\"x\":4,\"y\":5,"
			"\"floor_visibility\":\"everywhere\"}}]},\"start_node\":0,"
			"\"nodes\":[{\"id\":0,\"text\":\"Go.\",\"next\":0}]}", error));
		EXPECT(hasCode(validate(invalidVisibility.json()),
			"invalid_marker_floor_visibility"));
		return true;
	}

	bool testStarterTemplates()
	{
		using namespace automatia::dialogue;
		for ( const std::string& id : { "empty_conversation", "one_choice_conversation",
			"two_choice_branch", "quest_giver", "recruitable_npc" } )
		{
			Document document;
			std::string error;
			EXPECT(document.parse(createStarterDocument(id, "file_id", "My Quest",
				"Hello", "Continue", "A Quest", "Summary"), error));
			EXPECT(document.json()["version"].GetInt() == SchemaVersion);
			EXPECT(!hasErrors(validate(document.json())));
		}
		const std::string identities = createStarterDocument(
			"quest_giver", "dialogue_file_identity", "persistent_quest_identity",
			"Hello", "Accept", "Quest", "Summary");
		EXPECT(identities.find("dialogue_file_identity") == std::string::npos);
		EXPECT(identities.find("\"quest_id\": \"persistent_quest_identity\"")
			!= std::string::npos);
		return true;
	}

	bool testSharedOwnershipSchemaMigration()
	{
		using namespace automatia::dialogue;
		Document legacy;
		std::string error;
		EXPECT(legacy.parse(
			"{\"version\":1,\"quest_id\":\"legacy_party\",\"quest\":{"
			"\"title\":\"Legacy\",\"scope\":\"party\"},\"start_node\":0,"
			"\"nodes\":[{\"id\":0,\"text\":\"Legacy\",\"next\":0}]}", error));
		const auto legacyIssues = validate(legacy.json());
		EXPECT(!hasErrors(legacyIssues));
		EXPECT(hasCode(legacyIssues, "legacy_scope_falls_back_to_player"));

		Document shared;
		EXPECT(shared.parse(
			"{\"version\":2,\"quest_id\":\"shared_party\",\"quest\":{"
			"\"title\":\"Shared\",\"scope\":\"party\"},\"start_node\":0,"
			"\"nodes\":[{\"id\":0,\"text\":\"Shared\",\"next\":0}]}", error));
		const auto sharedIssues = validate(shared.json());
		EXPECT(!hasErrors(sharedIssues));
		EXPECT(!hasCode(sharedIssues, "legacy_scope_falls_back_to_player"));

		Document invalid;
		EXPECT(invalid.parse(
			"{\"version\":3,\"start_node\":0,\"nodes\":[{\"id\":0,"
			"\"text\":\"Invalid\",\"next\":0}]}", error));
		EXPECT(hasCode(validate(invalid.json()), "invalid_version"));
		return true;
	}
}

int main()
{
	const bool passed = testTutorialLibraryIsVerified()
		&& testLosslessRoundTripDirtyHistoryAndAtomicSave()
		&& testValidationAndLocations()
		&& testSandboxPreview()
		&& testLegacyDialogueVisualModelAndPreview()
		&& testSandboxEveryConditionFamily()
		&& testCapabilityAndRuntimeParityContract()
		&& testEveryConditionAndActionFixture()
		&& testEditorAndIntegrationSourceContracts()
		&& testFloorAwareMarkerValidation()
		&& testStarterTemplates()
		&& testSharedOwnershipSchemaMigration();
	if ( passed )
	{
		std::cout << "Custom dialogue document tests passed.\n";
		return 0;
	}
	return 1;
}
