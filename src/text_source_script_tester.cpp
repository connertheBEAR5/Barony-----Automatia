/*-------------------------------------------------------------------------------

	BARONY / AUTOMATIA
	Zed Text Source script tester.

	The tester deliberately does not link the gameplay world dispatcher into Zed.
	It shares the runtime parser and resource-reference resolver, then executes the
	only naturally disposable operation (@setvar) against TestSession state. Every
	live-world operation is reported and blocked.

-------------------------------------------------------------------------------*/

#include "text_source_script_tester.hpp"

#include "main.hpp"
#include "editor.hpp"
#include "entity.hpp"
#include "mod_tools.hpp"
#include "player.hpp"
#include "text_source_script.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
	constexpr int kTesterWindow = 41;
	constexpr int kResourceRows = 8;
	constexpr int kLibraryRows = 12;
	constexpr int kOutputCapacity = 512;
	constexpr int kScriptCapacity = 16384;
	constexpr std::size_t kNoSelection = static_cast<std::size_t>(-1);

	enum class TesterPage
	{
		Test,
		Library
	};

	enum class LibraryContent
	{
		Tags,
		Recipes
	};

	enum class VerificationFilter
	{
		AllCodeBacked,
		ManualVerified,
		Behavior,
		Dispatch,
		Parser,
		NeedsManual
	};

	enum class OriginFilter
	{
		All,
		Native,
		Barony,
		Automatia,
		SAMFramework,
		InstalledMods
	};

	enum class AvailabilityFilter
	{
		All,
		Available,
		Unavailable,
		NeedsReview
	};

	char resourceReference[256] = "$";
	char scriptEditBuffer[kScriptCapacity] = "";
	char librarySearch[128] = "";
	std::string loadedReference;
	std::vector<std::string> resourceNames;
	std::vector<std::string> outputLines;
	std::vector<std::string> libraryCategories;
	std::vector<std::size_t> libraryMatches;
	std::string libraryStatus;
	int resourceScroll = 0;
	int outputScroll = 0;
	int libraryScroll = 0;
	int detailScroll = 0;
	bool browsingResources = false;
	bool scriptWorkingCopyEdited = false;
	std::string contextOrigin;
	TextSourceLanguage::TestSession testSession;
	TesterPage testerPage = TesterPage::Test;
	LibraryContent libraryContent = LibraryContent::Tags;
	VerificationFilter verificationFilter = VerificationFilter::AllCodeBacked;
	OriginFilter originFilter = OriginFilter::All;
	AvailabilityFilter availabilityFilter = AvailabilityFilter::All;
	int libraryCategory = 0;
	std::uint64_t observedRegistryGeneration = 0;
	std::size_t selectedTag = kNoSelection;
	std::size_t selectedRecipe = kNoSelection;

	button_t* resourceRowButtons[kResourceRows] = {};
	button_t* resourcePreviousButton = nullptr;
	button_t* resourceNextButton = nullptr;
	button_t* libraryRowButtons[kLibraryRows] = {};
	button_t* testerTabButton = nullptr;
	button_t* libraryTabButton = nullptr;
	button_t* categoryButton = nullptr;
	button_t* verificationButton = nullptr;
	button_t* originButton = nullptr;
	button_t* availabilityButton = nullptr;
	button_t* contentButton = nullptr;
	button_t* insertTagButton = nullptr;
	button_t* insertExampleButton = nullptr;
	std::vector<button_t*> testPageButtons;
	std::vector<button_t*> libraryPageButtons;

	void appendOutput(const std::string& message);

	void clearFocusedButtons()
	{
		for ( node_t* node = button_l.first; node; )
		{
			node_t* next = node->next;
			button_t* button = static_cast<button_t*>(node->element);
			if ( button && button->focused )
			{
				list_RemoveNode(node);
			}
			node = next;
		}
	}

	button_t* addTesterButton(const char* label, int x, int y, int width,
		void (*action)(button_t*))
	{
		button_t* button = newButton();
		std::snprintf(button->label, sizeof(button->label), "%s", label);
		button->x = x;
		button->y = y;
		button->sizex = width;
		button->sizey = 18;
		button->action = action;
		button->visible = 1;
		button->focused = 1;
		return button;
	}

	std::string lowerText(std::string text)
	{
		std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		return text;
	}

	void focusInput(char* buffer, int capacity)
	{
		inputstr = buffer;
		inputlen = capacity - 1;
		cursorflash = ticks;
		if ( !SDL_IsTextInputActive() )
		{
			SDL_StartTextInput();
		}
	}

	void syncEditorScript()
	{
		testSession.setScript(scriptEditBuffer);
	}

	void setEditorScript(const std::string& script)
	{
		const std::size_t count = std::min(script.size(),
			static_cast<std::size_t>(kScriptCapacity - 1));
		std::memcpy(scriptEditBuffer, script.data(), count);
		scriptEditBuffer[count] = '\0';
		testSession.setScript(std::string(scriptEditBuffer, count));
		scriptWorkingCopyEdited = count != script.size();
		if ( count != script.size() )
		{
			appendOutput("[WARNING] Resource exceeded the 16 KiB editable tester buffer and the working copy was truncated.");
		}
	}

	std::vector<std::string> wrapText(const std::string& text, int columns)
	{
		std::vector<std::string> lines;
		size_t lineStart = 0;
		while ( lineStart <= text.size() )
		{
			const size_t newline = text.find('\n', lineStart);
			std::string logicalLine = text.substr(lineStart,
				newline == std::string::npos ? std::string::npos : newline - lineStart);
			if ( logicalLine.empty() )
			{
				lines.emplace_back();
			}
			while ( !logicalLine.empty() )
			{
				if ( static_cast<int>(logicalLine.size()) <= columns )
				{
					lines.push_back(logicalLine);
					logicalLine.clear();
					break;
				}
				size_t split = logicalLine.rfind(' ', static_cast<size_t>(columns));
				if ( split == std::string::npos || split == 0 )
				{
					split = static_cast<size_t>(columns);
				}
				lines.push_back(logicalLine.substr(0, split));
				logicalLine.erase(0, split);
				while ( !logicalLine.empty() && logicalLine.front() == ' ' )
				{
					logicalLine.erase(logicalLine.begin());
				}
			}
			if ( newline == std::string::npos )
			{
				break;
			}
			lineStart = newline + 1;
		}
		return lines;
	}

	int outputWrapColumns()
	{
		return std::max(32, (subx2 - subx1 - 44) / 8);
	}

	void scrollOutputToEnd()
	{
		const int outputTop = suby1 + 354;
		const int outputBottom = suby2 - 42;
		const int visibleRows = std::max(1, (outputBottom - outputTop - 8) / 10);
		outputScroll = std::max(0,
			static_cast<int>(outputLines.size()) - visibleRows);
	}

	void appendOutput(const std::string& message)
	{
		for ( std::string& line : wrapText(message, outputWrapColumns()) )
		{
			outputLines.push_back(std::move(line));
		}
		if ( outputLines.size() > kOutputCapacity )
		{
			outputLines.erase(outputLines.begin(),
				outputLines.begin() + (outputLines.size() - kOutputCapacity));
		}
		scrollOutputToEnd();
	}

	void appendDiagnostic(const TextSourceLanguage::Diagnostic& diagnostic)
	{
		appendOutput("[" + std::string(TextSourceLanguage::severityName(diagnostic.severity))
			+ " L" + std::to_string(diagnostic.line)
			+ ":" + std::to_string(diagnostic.column) + "] " + diagnostic.message);
	}

	bool verificationMatches(const TextSourceLanguage::VerificationEvidence& evidence)
	{
		using TextSourceLanguage::VerificationLevel;
		switch ( verificationFilter )
		{
			case VerificationFilter::AllCodeBacked:
				return true;
			case VerificationFilter::ManualVerified:
				return evidence.level == VerificationLevel::ManualGameTestVerified;
			case VerificationFilter::Behavior:
				return evidence.level == VerificationLevel::BehaviorVerified;
			case VerificationFilter::Dispatch:
				return evidence.level == VerificationLevel::DispatchVerified;
			case VerificationFilter::Parser:
				return evidence.level == VerificationLevel::ParserVerified;
			case VerificationFilter::NeedsManual:
				return evidence.manualBehaviorRequired;
		}
		return false;
	}

	bool recipeVerificationMatches(const TextSourceLanguage::ScriptRecipeDefinition& recipe)
	{
		TextSourceLanguage::VerificationEvidence evidence;
		evidence.level = recipe.verification;
		evidence.manualBehaviorRequired = recipe.manualBehaviorRequired;
		return verificationMatches(evidence);
	}

	const char* verificationFilterName()
	{
		switch ( verificationFilter )
		{
			case VerificationFilter::AllCodeBacked: return "All code";
			case VerificationFilter::ManualVerified: return "Manual L4";
			case VerificationFilter::Behavior: return "Behavior";
			case VerificationFilter::Dispatch: return "Dispatch";
			case VerificationFilter::Parser: return "Parser";
			case VerificationFilter::NeedsManual: return "Manual";
		}
		return "Unknown";
	}

	bool originMatches(const TextSourceLanguage::ScriptTagDefinition& definition)
	{
		using TextSourceLanguage::ScriptOriginKind;
		switch ( originFilter )
		{
			case OriginFilter::All:
				return true;
			case OriginFilter::Native:
				return TextSourceLanguage::isNativeScriptOrigin(definition.origin.kind);
			case OriginFilter::Barony:
				return definition.origin.kind == ScriptOriginKind::Barony;
			case OriginFilter::Automatia:
				return definition.origin.kind == ScriptOriginKind::Automatia;
			case OriginFilter::SAMFramework:
				return definition.origin.kind == ScriptOriginKind::SAMFramework;
			case OriginFilter::InstalledMods:
				return definition.origin.kind == ScriptOriginKind::SAMMod;
		}
		return false;
	}

	bool availabilityMatches(const TextSourceLanguage::ScriptTagDefinition& definition)
	{
		using TextSourceLanguage::ScriptAvailability;
		switch ( availabilityFilter )
		{
			case AvailabilityFilter::All:
				return true;
			case AvailabilityFilter::Available:
				return definition.availability == ScriptAvailability::Available
					|| definition.availability == ScriptAvailability::NeedsVerification;
			case AvailabilityFilter::Unavailable:
				return definition.availability == ScriptAvailability::UnavailableMissingMod;
			case AvailabilityFilter::NeedsReview:
				return definition.availability == ScriptAvailability::NeedsVerification
					|| definition.availability == ScriptAvailability::UnsupportedHistorical;
		}
		return false;
	}

	const char* originFilterName()
	{
		switch ( originFilter )
		{
			case OriginFilter::All: return "All";
			case OriginFilter::Native: return "Native";
			case OriginFilter::Barony: return "Barony";
			case OriginFilter::Automatia: return "Automatia";
			case OriginFilter::SAMFramework: return "S.A.M.";
			case OriginFilter::InstalledMods: return "Installed Mods";
		}
		return "Unknown";
	}

	const char* availabilityFilterName()
	{
		switch ( availabilityFilter )
		{
			case AvailabilityFilter::All: return "All";
			case AvailabilityFilter::Available: return "Available";
			case AvailabilityFilter::Unavailable: return "Unavailable";
			case AvailabilityFilter::NeedsReview: return "Needs Review";
		}
		return "Unknown";
	}

	void buildLibraryCategories()
	{
		libraryCategories.clear();
		libraryCategories.emplace_back("All");
		for ( const auto& definition : TextSourceLanguage::scriptTagDefinitions() )
		{
			const std::string category = definition.category;
			if ( std::find(libraryCategories.begin() + 1,
				libraryCategories.end(), category) == libraryCategories.end() )
			{
				libraryCategories.push_back(category);
			}
		}
		for ( const auto& recipe : TextSourceLanguage::scriptRecipeDefinitions() )
		{
			const std::string category = recipe.category;
			if ( std::find(libraryCategories.begin() + 1,
				libraryCategories.end(), category) == libraryCategories.end() )
			{
				libraryCategories.push_back(category);
			}
		}
		std::sort(libraryCategories.begin() + 1, libraryCategories.end());
		libraryCategory = std::max(0, std::min(libraryCategory,
			static_cast<int>(libraryCategories.size()) - 1));
	}

	bool recipeMatchesSearch(const TextSourceLanguage::ScriptRecipeDefinition& recipe)
	{
		const std::string needle = lowerText(librarySearch);
		if ( needle.empty() )
		{
			return true;
		}
		std::string haystack = std::string(recipe.name) + " " + recipe.category
			+ " " + recipe.script + " " + recipe.description + " " + recipe.caveats;
		return lowerText(std::move(haystack)).find(needle) != std::string::npos;
	}

	void rebuildLibraryMatches()
	{
		libraryMatches.clear();
		const std::string selectedCategory = libraryCategories.empty()
			? "All" : libraryCategories[libraryCategory];
		if ( libraryContent == LibraryContent::Tags )
		{
			const auto& definitions = TextSourceLanguage::scriptTagDefinitions();
			for ( std::size_t i = 0; i < definitions.size(); ++i )
			{
				const auto& definition = definitions[i];
				if ( selectedCategory != "All" && selectedCategory != definition.category )
				{
					continue;
				}
				if ( !verificationMatches(definition.verification)
					|| !originMatches(definition)
					|| !availabilityMatches(definition)
					|| !TextSourceLanguage::scriptTagMatchesSearch(definition, librarySearch) )
				{
					continue;
				}
				libraryMatches.push_back(i);
			}
			if ( std::find(libraryMatches.begin(), libraryMatches.end(), selectedTag)
				== libraryMatches.end() )
			{
				selectedTag = libraryMatches.empty() ? kNoSelection : libraryMatches.front();
				detailScroll = 0;
			}
		}
		else
		{
			if ( originFilter == OriginFilter::Barony
				|| originFilter == OriginFilter::Automatia
				|| originFilter == OriginFilter::SAMFramework
				|| originFilter == OriginFilter::InstalledMods
				|| availabilityFilter == AvailabilityFilter::Unavailable
				|| availabilityFilter == AvailabilityFilter::NeedsReview )
			{
				selectedRecipe = kNoSelection;
				return;
			}
			const auto& recipes = TextSourceLanguage::scriptRecipeDefinitions();
			for ( std::size_t i = 0; i < recipes.size(); ++i )
			{
				const auto& recipe = recipes[i];
				if ( selectedCategory != "All" && selectedCategory != recipe.category )
				{
					continue;
				}
				if ( !recipeVerificationMatches(recipe) || !recipeMatchesSearch(recipe) )
				{
					continue;
				}
				libraryMatches.push_back(i);
			}
			if ( std::find(libraryMatches.begin(), libraryMatches.end(), selectedRecipe)
				== libraryMatches.end() )
			{
				selectedRecipe = libraryMatches.empty() ? kNoSelection : libraryMatches.front();
				detailScroll = 0;
			}
		}
		libraryScroll = std::max(0, std::min(libraryScroll,
			std::max(0, static_cast<int>(libraryMatches.size()) - kLibraryRows)));
	}

	bool lookupResource(const std::string& key, std::string& script)
	{
		auto found = ScriptTextParser.allEntries.find(key);
		if ( found == ScriptTextParser.allEntries.end() )
		{
			return false;
		}
		script = found->second.formattedText;
		return true;
	}

	void refreshResourceNames()
	{
		resourceNames.clear();
		for ( const auto& entry : ScriptTextParser.allEntries )
		{
			if ( entry.second.objectType == ScriptTextParser_t::OBJ_SCRIPT )
			{
				resourceNames.push_back(entry.first);
			}
		}
		std::sort(resourceNames.begin(), resourceNames.end());
		resourceScroll = std::max(0, std::min(resourceScroll,
			std::max(0, static_cast<int>(resourceNames.size()) - kResourceRows)));
	}

	TextSourceLanguage::TestContext captureTestContext()
	{
		TextSourceLanguage::TestContext context;
		context.activatorPlayer = -1; // Zed has no connected runtime player.
		Entity* source = selectedEntity[0];
		if ( source && source->sprite == 132 )
		{
			context.sourceX = static_cast<int>(std::lround(source->x / 16.0));
			context.sourceY = static_cast<int>(std::lround(source->y / 16.0));
			context.playableFloor = source->playableFloor;
			context.authoredMapLayer = source->authoredMapLayer;
			context.localZ = source->z;
			context.spatialRevision = source->spatialRevision;
			contextOrigin = "selected Text Source sprite 132";
		}
		else
		{
			context.sourceX = drawx;
			context.sourceY = drawy;
			context.playableFloor = DEFAULT_PLAYABLE_FLOOR;
			context.authoredMapLayer = drawlayer;
			context.localZ = 0.0;
			context.spatialRevision = 0;
			contextOrigin = "editor cursor (playableFloor defaults to 0)";
		}
		const char* mapName = map.filename[0]
			? map.filename
			: (map.name[0] ? map.name : "untitled");
		context.mapContext = mapName;
		context.mapInstance.clear(); // MapInstance exists only in a running world.
		return context;
	}

	void updateResourceButtons();

	void closeTester(button_t*)
	{
		inputstr = nullptr;
		SDL_StopTextInput();
		scroll = 0;
		subwindow = 0;
		newwindow = 0;
		openwindow = 0;
		savewindow = 0;
		editproperty = 0;
		browsingResources = false;
	}

	void loadCurrentResource(bool reloadCatalog)
	{
		if ( reloadCatalog )
		{
			ScriptTextParser.readAllScripts();
			refreshResourceNames();
			appendOutput("[INFO] Reloaded gameplay /data/scripts resources through ScriptTextParser.");
		}
		const auto resolved = TextSourceLanguage::resolveTextSourceResource(
			resourceReference, lookupResource);
		if ( !resolved.found )
		{
			appendOutput("[ERROR] " + resolved.error
				+ "; the current editable working copy was left unchanged.");
			updateResourceButtons();
			return;
		}

		loadedReference = "$" + resolved.key;
		setEditorScript(resolved.script);
		appendOutput("[SUCCESS] Loaded " + loadedReference + " ("
			+ std::to_string(resolved.script.size())
			+ " bytes) using the gameplay resource resolver.");
		auto found = ScriptTextParser.allEntries.find(resolved.key);
		if ( found != ScriptTextParser.allEntries.end()
			&& found->second.objectType != ScriptTextParser_t::OBJ_SCRIPT )
		{
			appendOutput("[WARNING] Entry is not declared as a script, but gameplay's Text Source resolver accepts it; tester behavior matches gameplay.");
		}
		updateResourceButtons();
	}

	void loadReload(button_t*)
	{
		loadCurrentResource(true);
	}

	void validateScript(button_t*)
	{
		syncEditorScript();
		if ( testSession.script().empty() )
		{
			appendOutput("[INFO] Validate requested with no loaded script.");
		}
		const auto& parsed = testSession.validate();
		for ( const auto& diagnostic : parsed.diagnostics )
		{
			appendDiagnostic(diagnostic);
		}
		for ( const auto& token : parsed.tokens )
		{
			appendOutput("[TOKEN L" + std::to_string(token.line) + ":"
				+ std::to_string(token.column) + "] " + token.command + " ["
				+ TextSourceLanguage::safetyName(token.safety) + "]");
		}
		appendOutput(parsed.success()
			? "[SUCCESS] Shared gameplay parser accepted the script."
			: "[ERROR] Shared gameplay parser rejected the script; Run Test is disabled by validation.");
	}

	void runTest(button_t*)
	{
		syncEditorScript();
		const auto result = testSession.run();
		for ( const auto& diagnostic : result.diagnostics )
		{
			appendDiagnostic(diagnostic);
		}
		if ( result.success )
		{
			appendOutput(result.blockedCommands
				? "[SUCCESS] Sandbox run completed; unsafe commands were blocked and no authored map state changed."
				: "[SUCCESS] Sandbox run completed without live-world operations.");
		}
		else
		{
			appendOutput("[ERROR] Sandbox run did not execute; see validation/lifecycle diagnostics above.");
		}
		if ( result.variables.empty() )
		{
			appendOutput("[STATE] Disposable script variable map is empty.");
		}
		else
		{
			for ( const auto& variable : result.variables )
			{
				appendOutput("[STATE] " + variable.first + "="
					+ std::to_string(variable.second));
			}
		}
	}

	void resetContext(button_t*)
	{
		syncEditorScript();
		testSession.reset(captureTestContext());
		outputLines.clear();
		outputScroll = 0;
		appendOutput("[SUCCESS] Disposable variables cleared and test context recaptured from "
			+ contextOrigin + ".");
		appendOutput("[INFO] No entity is spawned or referenced by the tester; playableFloor, authoredMapLayer, local z, and MapInstance remain separate context axes.");
		appendOutput("[INFO] Current Text Source syntax has no S.A.M. stable-ID item/entity literal; the tester does not invent one.");
	}

	void toggleBrowse(button_t*)
	{
		if ( !browsingResources )
		{
			ScriptTextParser.readAllScripts();
			refreshResourceNames();
		}
		browsingResources = !browsingResources;
		updateResourceButtons();
	}

	void selectResource(button_t* selected)
	{
		for ( int row = 0; row < kResourceRows; ++row )
		{
			if ( resourceRowButtons[row] != selected )
			{
				continue;
			}
			const int index = resourceScroll + row;
			if ( index >= 0 && index < static_cast<int>(resourceNames.size()) )
			{
				std::snprintf(resourceReference, sizeof(resourceReference), "$%s",
					resourceNames[index].c_str());
				inputstr = resourceReference;
				inputlen = static_cast<int>(sizeof(resourceReference)) - 1;
				loadCurrentResource(false);
			}
			return;
		}
	}

	void previousResources(button_t*)
	{
		resourceScroll = std::max(0, resourceScroll - kResourceRows);
		updateResourceButtons();
	}

	void nextResources(button_t*)
	{
		resourceScroll = std::min(
			std::max(0, static_cast<int>(resourceNames.size()) - kResourceRows),
			resourceScroll + kResourceRows);
		updateResourceButtons();
	}

	void updateResourceButtons()
	{
		for ( int row = 0; row < kResourceRows; ++row )
		{
			button_t* button = resourceRowButtons[row];
			if ( !button )
			{
				continue;
			}
			const int index = resourceScroll + row;
			button->visible = testerPage == TesterPage::Test && browsingResources
				&& index >= 0 && index < static_cast<int>(resourceNames.size());
			if ( button->visible )
			{
				std::string label = "$" + resourceNames[index];
				if ( label.size() >= sizeof(button->label) )
				{
					label.resize(sizeof(button->label) - 4);
					label += "...";
				}
				std::snprintf(button->label, sizeof(button->label), "%s", label.c_str());
			}
		}
		if ( resourcePreviousButton )
		{
			resourcePreviousButton->visible = testerPage == TesterPage::Test
				&& browsingResources && resourceScroll > 0;
		}
		if ( resourceNextButton )
		{
			resourceNextButton->visible = testerPage == TesterPage::Test && browsingResources
				&& resourceScroll + kResourceRows < static_cast<int>(resourceNames.size());
		}
	}

	button_t* addPageButton(std::vector<button_t*>& pageButtons,
		const char* label, int x, int y, int width, void (*action)(button_t*))
	{
		button_t* button = addTesterButton(label, x, y, width, action);
		pageButtons.push_back(button);
		return button;
	}

	void updateLibraryButtons()
	{
		if ( observedRegistryGeneration
			!= TextSourceLanguage::scriptTagRegistryGeneration() )
		{
			observedRegistryGeneration = TextSourceLanguage::scriptTagRegistryGeneration();
			selectedTag = kNoSelection;
			libraryScroll = 0;
			detailScroll = 0;
			buildLibraryCategories();
		}
		rebuildLibraryMatches();
		for ( int row = 0; row < kLibraryRows; ++row )
		{
			button_t* button = libraryRowButtons[row];
			if ( !button )
			{
				continue;
			}
			const int match = libraryScroll + row;
			button->visible = testerPage == TesterPage::Library
				&& match >= 0 && match < static_cast<int>(libraryMatches.size());
			if ( !button->visible )
			{
				continue;
			}
			std::string label;
			if ( libraryContent == LibraryContent::Tags )
			{
				const std::size_t index = libraryMatches[match];
				label = (index == selectedTag ? "> " : "  ")
					+ std::string(TextSourceLanguage::scriptTagDefinitions()[index].canonicalTag);
			}
			else
			{
				const std::size_t index = libraryMatches[match];
				label = (index == selectedRecipe ? "> " : "  ")
					+ std::string(TextSourceLanguage::scriptRecipeDefinitions()[index].name);
			}
			if ( label.size() >= sizeof(button->label) )
			{
				label.resize(sizeof(button->label) - 4);
				label += "...";
			}
			std::snprintf(button->label, sizeof(button->label), "%s", label.c_str());
		}

		if ( contentButton )
		{
			std::snprintf(contentButton->label, sizeof(contentButton->label), "%s",
				libraryContent == LibraryContent::Tags ? "Tags" : "Recipes");
		}
		if ( categoryButton )
		{
			const std::string category = libraryCategories.empty()
				? "All" : libraryCategories[libraryCategory];
			std::snprintf(categoryButton->label, sizeof(categoryButton->label),
				"Category: %.20s", category.c_str());
		}
		if ( verificationButton )
		{
			std::snprintf(verificationButton->label, sizeof(verificationButton->label),
				"Verify: %s", verificationFilterName());
		}
		if ( originButton )
		{
			std::snprintf(originButton->label, sizeof(originButton->label),
				"Origin: %s", originFilterName());
		}
		if ( availabilityButton )
		{
			std::snprintf(availabilityButton->label, sizeof(availabilityButton->label),
				"Availability: %s", availabilityFilterName());
		}
		if ( insertTagButton )
		{
			insertTagButton->visible = testerPage == TesterPage::Library
				&& libraryContent == LibraryContent::Tags && selectedTag != kNoSelection;
		}
		if ( insertExampleButton )
		{
			insertExampleButton->visible = testerPage == TesterPage::Library
				&& (libraryContent == LibraryContent::Tags
					? selectedTag != kNoSelection : selectedRecipe != kNoSelection);
			std::snprintf(insertExampleButton->label,
				sizeof(insertExampleButton->label), "%s",
				libraryContent == LibraryContent::Tags
					? "Insert Example" : "Insert Recipe");
		}
	}

	void updatePageButtons()
	{
		for ( button_t* button : testPageButtons )
		{
			button->visible = testerPage == TesterPage::Test;
		}
		for ( button_t* button : libraryPageButtons )
		{
			button->visible = testerPage == TesterPage::Library;
		}
		if ( testerTabButton )
		{
			std::snprintf(testerTabButton->label, sizeof(testerTabButton->label), "%s",
				testerPage == TesterPage::Test ? "[ Tester ]" : "Tester");
		}
		if ( libraryTabButton )
		{
			std::snprintf(libraryTabButton->label, sizeof(libraryTabButton->label), "%s",
				testerPage == TesterPage::Library ? "[ Library ]" : "Library");
		}
		updateResourceButtons();
		updateLibraryButtons();
	}

	void showTesterPage(button_t*)
	{
		testerPage = TesterPage::Test;
		focusInput(scriptEditBuffer, kScriptCapacity);
		updatePageButtons();
	}

	void showLibraryPage(button_t*)
	{
		testerPage = TesterPage::Library;
		focusInput(librarySearch, static_cast<int>(sizeof(librarySearch)));
		updatePageButtons();
	}

	void cycleLibraryContent(button_t*)
	{
		libraryContent = libraryContent == LibraryContent::Tags
			? LibraryContent::Recipes : LibraryContent::Tags;
		libraryScroll = 0;
		detailScroll = 0;
		libraryStatus.clear();
		updateLibraryButtons();
	}

	void cycleLibraryCategory(button_t*)
	{
		if ( !libraryCategories.empty() )
		{
			libraryCategory = (libraryCategory + 1)
				% static_cast<int>(libraryCategories.size());
		}
		libraryScroll = 0;
		detailScroll = 0;
		updateLibraryButtons();
	}

	void cycleVerificationFilter(button_t*)
	{
		verificationFilter = static_cast<VerificationFilter>(
			(static_cast<int>(verificationFilter) + 1) % 6);
		libraryScroll = 0;
		detailScroll = 0;
		updateLibraryButtons();
	}

	void cycleOriginFilter(button_t*)
	{
		originFilter = static_cast<OriginFilter>(
			(static_cast<int>(originFilter) + 1) % 6);
		libraryScroll = 0;
		detailScroll = 0;
		updateLibraryButtons();
	}

	void cycleAvailabilityFilter(button_t*)
	{
		availabilityFilter = static_cast<AvailabilityFilter>(
			(static_cast<int>(availabilityFilter) + 1) % 4);
		libraryScroll = 0;
		detailScroll = 0;
		updateLibraryButtons();
	}

	void selectLibraryRow(button_t* selected)
	{
		for ( int row = 0; row < kLibraryRows; ++row )
		{
			if ( libraryRowButtons[row] != selected )
			{
				continue;
			}
			const int match = libraryScroll + row;
			if ( match >= 0 && match < static_cast<int>(libraryMatches.size()) )
			{
				if ( libraryContent == LibraryContent::Tags )
				{
					selectedTag = libraryMatches[match];
				}
				else
				{
					selectedRecipe = libraryMatches[match];
				}
				detailScroll = 0;
				libraryStatus.clear();
			}
			break;
		}
		updateLibraryButtons();
	}

	bool insertSnippet(const std::string& snippet, const std::string& label)
	{
		std::string script(scriptEditBuffer);
		std::string error;
		if ( !TextSourceLanguage::appendScriptSnippet(
			script, snippet, kScriptCapacity - 1, error) )
		{
			libraryStatus = "Insertion failed: " + error;
			appendOutput("[ERROR] " + libraryStatus);
			return false;
		}
		std::memcpy(scriptEditBuffer, script.c_str(), script.size() + 1);
		scriptWorkingCopyEdited = true;
		syncEditorScript();
		libraryStatus = "Appended " + label + " to the editable working copy.";
		appendOutput("[SUCCESS] " + libraryStatus);
		return true;
	}

	void insertSelectedTag(button_t*)
	{
		const auto& definitions = TextSourceLanguage::scriptTagDefinitions();
		if ( selectedTag >= definitions.size() )
		{
			return;
		}
		insertSnippet(definitions[selectedTag].insertionTemplate,
			definitions[selectedTag].canonicalTag);
	}

	void insertSelectedExample(button_t*)
	{
		if ( libraryContent == LibraryContent::Tags )
		{
			const auto& definitions = TextSourceLanguage::scriptTagDefinitions();
			if ( selectedTag < definitions.size() )
			{
				insertSnippet(definitions[selectedTag].minimalExample,
					std::string("verified example for ") + definitions[selectedTag].canonicalTag);
			}
		}
		else
		{
			const auto& recipes = TextSourceLanguage::scriptRecipeDefinitions();
			if ( selectedRecipe < recipes.size() )
			{
				insertSnippet(recipes[selectedRecipe].script,
					std::string("verified recipe '") + recipes[selectedRecipe].name + "'");
			}
		}
	}

	void addDetail(std::vector<std::string>& lines, const std::string& label,
		const std::string& value, int columns)
	{
		const std::string text = label + (value.empty() ? "(none)" : value);
		auto wrapped = wrapText(text, columns);
		lines.insert(lines.end(), wrapped.begin(), wrapped.end());
	}

	std::vector<std::string> selectedLibraryDetails(int columns)
	{
		std::vector<std::string> lines;
		if ( libraryContent == LibraryContent::Tags )
		{
			const auto& definitions = TextSourceLanguage::scriptTagDefinitions();
			if ( selectedTag >= definitions.size() )
			{
				lines.emplace_back("No tag matches the current filters.");
				return lines;
			}
			const auto& definition = definitions[selectedTag];
			addDetail(lines, "Name: ", definition.canonicalTag, columns);
			addDetail(lines, "Origin: ", definition.origin.displayName, columns);
			addDetail(lines, "Origin kind: ",
				TextSourceLanguage::scriptOriginKindName(definition.origin.kind), columns);
			addDetail(lines, "Stable source: ", definition.origin.stableSourceId, columns);
			addDetail(lines, "Source version: ", definition.origin.sourceVersion, columns);
			addDetail(lines, "Availability: ",
				std::string(TextSourceLanguage::scriptAvailabilityName(definition.availability))
				+ "; " + definition.availabilityDetail, columns);
			addDetail(lines, "Syntax: ", definition.syntax, columns);
			addDetail(lines, "Category: ", definition.category, columns);
			addDetail(lines, "Aliases: ", definition.aliases, columns);
			addDetail(lines, "Targets: ", definition.execution.targetTypes, columns);
			addDetail(lines, "Scope: ", definition.execution.scope, columns);
			addDetail(lines, "Attachment: ",
				TextSourceLanguage::attachmentModeName(definition.execution.attachment), columns);
			addDetail(lines, "Value: ", std::string(definition.value.valueCount)
				+ "; separators " + definition.value.separators
				+ "; " + definition.value.acceptedDomain, columns);
			addDetail(lines, "Description: ", definition.description, columns);
			addDetail(lines, "Default: ", definition.defaultBehavior, columns);
			addDetail(lines, "Verified example: ", definition.minimalExample, columns);
			addDetail(lines, "Caveats: ", definition.caveats, columns);
			addDetail(lines, "Verification: ",
				std::string(TextSourceLanguage::verificationLevelName(
					definition.verification.level))
				+ (definition.verification.manualBehaviorRequired
					? "; manual behavior check still required" : "; no manual check required"),
				columns);
			addDetail(lines, "Method: ", definition.verification.method, columns);
			addDetail(lines, "Test safety: ",
				TextSourceLanguage::safetyName(definition.execution.safety), columns);
			addDetail(lines, "Dispatch phase: ",
				TextSourceLanguage::scriptDispatchPhaseName(
					definition.execution.dispatchPhase), columns);
			addDetail(lines, "Authority/state: ",
				std::string(definition.execution.serverAuthoritative
					? "server authoritative" : "not server authoritative")
				+ (definition.execution.changesPersistentState
					? "; may affect persisted game state" : "; not marked persistent"), columns);
			addDetail(lines, "Source: ", definition.sourceLocation, columns);
		}
		else
		{
			const auto& recipes = TextSourceLanguage::scriptRecipeDefinitions();
			if ( selectedRecipe >= recipes.size() )
			{
				lines.emplace_back("No recipe matches the current filters.");
				return lines;
			}
			const auto& recipe = recipes[selectedRecipe];
			addDetail(lines, "Recipe: ", recipe.name, columns);
			addDetail(lines, "Category: ", recipe.category, columns);
			addDetail(lines, "Script: ", recipe.script, columns);
			addDetail(lines, "Description: ", recipe.description, columns);
			addDetail(lines, "Caveats: ", recipe.caveats, columns);
			addDetail(lines, "Verification: ",
				std::string(TextSourceLanguage::verificationLevelName(recipe.verification))
				+ (recipe.manualBehaviorRequired
					? "; manual behavior check still required" : "; no manual check required"),
				columns);
			addDetail(lines, "Method: ", recipe.verificationMethod, columns);
		}
		return lines;
	}

	void drawLibraryPage()
	{
		updateLibraryButtons();
		printText(font8x8_bmp, subx1 + 16, suby1 + 30,
			"Unified Verified @script Library (runtime-authoritative)");
		printText(font8x8_bmp, subx1 + 16, suby1 + 45,
			"Search name, alias, origin, source, availability, target, recipe:");
		drawDepressed(subx1 + 16, suby1 + 56, subx2 - 16, suby1 + 70);
		std::string displayedSearch = librarySearch;
		const int maximumSearch = std::max(8, (subx2 - subx1 - 48) / 8);
		if ( static_cast<int>(displayedSearch.size()) > maximumSearch )
		{
			displayedSearch = "..." + displayedSearch.substr(
				displayedSearch.size() - (maximumSearch - 3));
		}
		printText(font8x8_bmp, subx1 + 20, suby1 + 59,
			displayedSearch.empty() ? "(all code-backed entries)" : displayedSearch.c_str());
		if ( inputstr == librarySearch
			&& (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
		{
			printText(font8x8_bmp,
				subx1 + 20 + static_cast<int>(displayedSearch.size()) * 8,
				suby1 + 59, "|");
		}
		if ( mousestatus[SDL_BUTTON_LEFT]
			&& omousex >= subx1 + 16 && omousex < subx2 - 16
			&& omousey >= suby1 + 56 && omousey < suby1 + 70 )
		{
			focusInput(librarySearch, static_cast<int>(sizeof(librarySearch)));
		}

		const int listLeft = subx1 + 16;
		const int listRight = subx1 + 254;
		const int panelTop = suby1 + 134;
		const int panelBottom = suby2 - 74;
		drawDepressed(listLeft, panelTop, listRight, panelBottom);
		printTextFormatted(font8x8_bmp, listLeft + 4, panelTop + 4,
			"%d match%s", static_cast<int>(libraryMatches.size()),
			libraryMatches.size() == 1 ? "" : "es");
		if ( scroll && mousex >= listLeft && mousex < listRight
			&& mousey >= panelTop && mousey < panelBottom )
		{
			libraryScroll = std::max(0, std::min(
				std::max(0, static_cast<int>(libraryMatches.size()) - kLibraryRows),
				libraryScroll - scroll * 2));
			scroll = 0;
			updateLibraryButtons();
		}
		printTextFormatted(font8x8_bmp, listLeft + 4, panelBottom - 12,
			"Rows %d-%d", libraryMatches.empty() ? 0 : libraryScroll + 1,
			std::min(static_cast<int>(libraryMatches.size()), libraryScroll + kLibraryRows));

		const int detailLeft = subx1 + 264;
		const int detailRight = subx2 - 16;
		drawDepressed(detailLeft, panelTop, detailRight, panelBottom);
		const int detailColumns = std::max(24, (detailRight - detailLeft - 12) / 8);
		const auto detailLines = selectedLibraryDetails(detailColumns);
		const int detailRows = std::max(1, (panelBottom - panelTop - 10) / 10);
		const int maximumDetailFirst = std::max(0,
			static_cast<int>(detailLines.size()) - detailRows);
		detailScroll = std::max(0, std::min(detailScroll, maximumDetailFirst));
		if ( scroll && mousex >= detailLeft && mousex < detailRight
			&& mousey >= panelTop && mousey < panelBottom )
		{
			detailScroll = std::max(0, std::min(maximumDetailFirst,
				detailScroll - scroll * 3));
			scroll = 0;
		}
		for ( int row = 0; row < detailRows; ++row )
		{
			const int index = detailScroll + row;
			if ( index >= static_cast<int>(detailLines.size()) )
			{
				break;
			}
			printText(font8x8_bmp, detailLeft + 6, panelTop + 5 + row * 10,
				detailLines[index].c_str());
		}

		std::string status;
		if ( !libraryStatus.empty() )
		{
			status = libraryStatus;
		}
		else if ( libraryContent == LibraryContent::Tags
			&& originFilter == OriginFilter::SAMFramework && libraryMatches.empty() )
		{
			status = "Current S.A.M. source registers zero Text Source commands; Lua/JS APIs are separate.";
		}
		else if ( libraryContent == LibraryContent::Tags
			&& (originFilter == OriginFilter::Barony
				|| originFilter == OriginFilter::Automatia)
			&& libraryMatches.empty() )
		{
			status = "No command has source-verifiable exclusive provenance; use Origin: Native.";
		}
		else
		{
			status = "Insertions append to the editable tester working copy; placeholders require editing.";
		}
		if ( status.size() > static_cast<std::size_t>(std::max(12,
			(subx2 - subx1 - 32) / 8)) )
		{
			status.resize(static_cast<std::size_t>(std::max(12,
				(subx2 - subx1 - 56) / 8)));
			status += "...";
		}
		printText(font8x8_bmp, subx1 + 16, suby2 - 66, status.c_str());
	}

	void buildTesterButtons()
	{
		testPageButtons.clear();
		libraryPageButtons.clear();
		testerTabButton = addTesterButton("[ Tester ]", subx2 - 250, suby1 + 5,
			92, showTesterPage);
		libraryTabButton = addTesterButton("Library", subx2 - 150, suby1 + 5,
			112, showLibraryPage);

		addPageButton(testPageButtons, "Load / Reload", subx1 + 16, suby1 + 70,
			112, loadReload);
		addPageButton(testPageButtons, "Validate / Parse", subx1 + 136, suby1 + 70,
			136, validateScript);
		addPageButton(testPageButtons, "Run Test", subx1 + 280, suby1 + 70,
			88, runTest);
		addPageButton(testPageButtons, "Reset Test Context", subx1 + 376, suby1 + 70,
			160, resetContext);
		addPageButton(testPageButtons, "Browse", subx2 - 88, suby1 + 42,
			72, toggleBrowse);
		addTesterButton("Close", subx2 - 80, suby2 - 30, 64, closeTester);

		button_t* closeX = addTesterButton("X", subx2 - 18, suby1, 18, closeTester);
		closeX->sizey = 16;

		for ( int row = 0; row < kResourceRows; ++row )
		{
			resourceRowButtons[row] = addPageButton(testPageButtons, "", subx1 + 18,
				suby1 + 184 + row * 18, subx2 - subx1 - 164, selectResource);
			resourceRowButtons[row]->visible = 0;
		}
		resourcePreviousButton = addPageButton(testPageButtons, "< Prev", subx2 - 134,
			suby1 + 184, 56, previousResources);
		resourceNextButton = addPageButton(testPageButtons, "Next >", subx2 - 70,
			suby1 + 184, 56, nextResources);
		resourcePreviousButton->visible = 0;
		resourceNextButton->visible = 0;

		contentButton = addPageButton(libraryPageButtons, "Tags", subx1 + 16,
			suby1 + 72, 104, cycleLibraryContent);
		categoryButton = addPageButton(libraryPageButtons, "Category: All",
			subx1 + 128, suby1 + 72, 250, cycleLibraryCategory);
		verificationButton = addPageButton(libraryPageButtons,
			"Verify: All code", subx1 + 386, suby1 + 72,
			subx2 - subx1 - 402, cycleVerificationFilter);
		originButton = addPageButton(libraryPageButtons,
			"Origin: All", subx1 + 16, suby1 + 94, 250, cycleOriginFilter);
		availabilityButton = addPageButton(libraryPageButtons,
			"Availability: All", subx1 + 274, suby1 + 94,
			subx2 - subx1 - 290, cycleAvailabilityFilter);
		for ( int row = 0; row < kLibraryRows; ++row )
		{
			libraryRowButtons[row] = addPageButton(libraryPageButtons, "",
				subx1 + 18, suby1 + 146 + row * 18, 232, selectLibraryRow);
		}
		insertTagButton = addPageButton(libraryPageButtons, "Insert Tag",
			subx1 + 270, suby2 - 48, 112, insertSelectedTag);
		insertExampleButton = addPageButton(libraryPageButtons, "Insert Example",
			subx1 + 390, suby2 - 48, 136, insertSelectedExample);
		updatePageButtons();
	}
}

void openTextSourceScriptTester(button_t*)
{
	clearFocusedButtons();
	menuVisible = 0;
	subwindow = 1;
	newwindow = kTesterWindow;
	openwindow = 0;
	savewindow = 0;
	const int width = std::min(760, std::max(560, xres - 32));
	const int height = std::min(560, std::max(440, yres - 32));
	subx1 = (xres - width) / 2;
	subx2 = subx1 + width;
	suby1 = (yres - height) / 2;
	suby2 = suby1 + height;
	subtext[0] = '\0';
	browsingResources = false;
	resourceScroll = 0;
	outputLines.clear();
	loadedReference.clear();
	libraryStatus.clear();
	librarySearch[0] = '\0';
	testerPage = TesterPage::Test;
	libraryContent = LibraryContent::Tags;
	verificationFilter = VerificationFilter::AllCodeBacked;
	originFilter = OriginFilter::All;
	availabilityFilter = AvailabilityFilter::All;
	libraryCategory = 0;
	observedRegistryGeneration = TextSourceLanguage::scriptTagRegistryGeneration();
	libraryScroll = 0;
	detailScroll = 0;
	selectedTag = kNoSelection;
	selectedRecipe = kNoSelection;

	ScriptTextParser.readAllScripts();
	refreshResourceNames();
	buildLibraryCategories();
	setEditorScript("");
	testSession.reset(captureTestContext());
	buildTesterButtons();
	focusInput(resourceReference, static_cast<int>(sizeof(resourceReference)));
	appendOutput("[INFO] Text Source Script Tester opened with "
		+ std::to_string(resourceNames.size()) + " gameplay script resources.");
	appendOutput("[INFO] Enter the same $resource key stored by sprite 132, then Load / Reload.");
	appendOutput("[INFO] Sandbox policy: only @setvar writes disposable state; all live gameplay mutations are blocked and reported.");
}

void drawTextSourceScriptTester()
{
	if ( newwindow != kTesterWindow )
	{
		return;
	}

	printText(font8x8_bmp, subx1 + 12, suby1 + 9,
		"Text Source @script Tester");
	if ( testerPage == TesterPage::Library )
	{
		drawLibraryPage();
		return;
	}
	printText(font8x8_bmp, subx1 + 16, suby1 + 29,
		"Script File / Resource (gameplay convention: $entry):");
	drawDepressed(subx1 + 16, suby1 + 42, subx2 - 96, suby1 + 62);
	std::string displayedReference = resourceReference;
	const int maxReferenceChars = std::max(8, (subx2 - subx1 - 124) / 8);
	if ( static_cast<int>(displayedReference.size()) > maxReferenceChars )
	{
		displayedReference = "..." + displayedReference.substr(
			displayedReference.size() - (maxReferenceChars - 3));
	}
	printText(font8x8_bmp, subx1 + 20, suby1 + 48, displayedReference.c_str());
	if ( inputstr == resourceReference
		&& (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
	{
		printText(font8x8_bmp,
			subx1 + 20 + static_cast<int>(displayedReference.size()) * 8,
			suby1 + 48, "|");
	}
	if ( mousestatus[SDL_BUTTON_LEFT]
		&& omousex >= subx1 + 16 && omousex < subx2 - 96
		&& omousey >= suby1 + 42 && omousey < suby1 + 62 )
	{
		focusInput(resourceReference, static_cast<int>(sizeof(resourceReference)));
	}

	const auto& context = testSession.context();
	printText(font8x8_bmp, subx1 + 16, suby1 + 102, "Test Context (read-only sandbox snapshot):");
	printTextFormatted(font8x8_bmp, subx1 + 24, suby1 + 116,
		"origin=%s | activator/player=%d | source=(%d,%d)",
		contextOrigin.c_str(), context.activatorPlayer, context.sourceX, context.sourceY);
	printTextFormatted(font8x8_bmp, subx1 + 24, suby1 + 130,
		"playableFloor=%d | authoredMapLayer=%d | local Entity::z=%.2f",
		context.playableFloor, context.authoredMapLayer, context.localZ);
	std::string displayedMapContext = context.mapContext;
	if ( displayedMapContext.size() > 42 )
	{
		displayedMapContext.resize(39);
		displayedMapContext += "...";
	}
	printTextFormatted(font8x8_bmp, subx1 + 24, suby1 + 144,
		"spatialRevision=%llu | map=%s | MapInstance=%s",
		static_cast<unsigned long long>(context.spatialRevision),
		displayedMapContext.c_str(),
		context.mapInstance.empty() ? "(none in Zed)" : context.mapInstance.c_str());
	printText(font8x8_bmp, subx1 + 24, suby1 + 158,
		"No floor is inferred from local z or authoredMapLayer.");

	const int middleTop = suby1 + 180;
	const int middleBottom = suby1 + 334;
	drawDepressed(subx1 + 16, middleTop, subx2 - 16, middleBottom);
	if ( browsingResources )
	{
		printTextFormatted(font8x8_bmp, subx2 - 142, suby1 + 212,
			"%d script entries", static_cast<int>(resourceNames.size()));
		if ( resourceNames.empty() )
		{
			printText(font8x8_bmp, subx1 + 24, suby1 + 190,
				"No OBJ_SCRIPT entries were accepted by the gameplay loader.");
		}
		if ( scroll && mousex >= subx1 + 16 && mousex < subx2 - 16
			&& mousey >= middleTop && mousey < middleBottom )
		{
			resourceScroll = std::max(0, std::min(
				std::max(0, static_cast<int>(resourceNames.size()) - kResourceRows),
				resourceScroll - scroll));
			scroll = 0;
			updateResourceButtons();
		}
	}
	else
	{
		if ( testSession.script() != scriptEditBuffer )
		{
			scriptWorkingCopyEdited = true;
		}
		printTextFormatted(font8x8_bmp, subx1 + 22, suby1 + 188,
			"Working copy: %s%s | %d bytes | click below to edit",
			loadedReference.empty() ? "(new script)" : loadedReference.c_str(),
			scriptWorkingCopyEdited ? " *" : "",
			static_cast<int>(std::strlen(scriptEditBuffer)));
		auto preview = wrapText(scriptEditBuffer, outputWrapColumns());
		const int previewRows = std::max(1, (middleBottom - middleTop - 26) / 10);
		for ( int row = 0; row < previewRows
			&& row < static_cast<int>(preview.size()); ++row )
		{
			printText(font8x8_bmp, subx1 + 22, suby1 + 204 + row * 10,
				preview[row].c_str());
		}
		if ( preview.size() > static_cast<size_t>(previewRows) )
		{
			printText(font8x8_bmp, subx2 - 80, middleBottom - 12, "[more...]");
		}
		if ( inputstr == scriptEditBuffer )
		{
			printText(font8x8_bmp, subx2 - 200, middleBottom - 12,
				"[editing: append/backspace at end]");
		}
		if ( mousestatus[SDL_BUTTON_LEFT]
			&& omousex >= subx1 + 16 && omousex < subx2 - 16
			&& omousey >= middleTop && omousey < middleBottom )
		{
			focusInput(scriptEditBuffer, kScriptCapacity);
		}
	}

	const int outputTop = suby1 + 354;
	const int outputBottom = suby2 - 42;
	printText(font8x8_bmp, subx1 + 16, suby1 + 340,
		"Output / diagnostics (mouse wheel to scroll):");
	drawDepressed(subx1 + 16, outputTop, subx2 - 16, outputBottom);
	const int visibleRows = std::max(1, (outputBottom - outputTop - 8) / 10);
	const int maxFirst = std::max(0,
		static_cast<int>(outputLines.size()) - visibleRows);
	outputScroll = std::max(0, std::min(outputScroll, maxFirst));
	if ( scroll && mousex >= subx1 + 16 && mousex < subx2 - 16
		&& mousey >= outputTop && mousey < outputBottom )
	{
		outputScroll = std::max(0, std::min(maxFirst,
			outputScroll - scroll * 3));
		scroll = 0;
	}
	for ( int row = 0; row < visibleRows; ++row )
	{
		const int index = outputScroll + row;
		if ( index >= static_cast<int>(outputLines.size()) )
		{
			break;
		}
		printText(font8x8_bmp, subx1 + 22, outputTop + 5 + row * 10,
			outputLines[index].c_str());
	}
	printTextFormatted(font8x8_bmp, subx1 + 16, suby2 - 25,
		"Log lines %d-%d of %d",
		outputLines.empty() ? 0 : outputScroll + 1,
		std::min(static_cast<int>(outputLines.size()), outputScroll + visibleRows),
		static_cast<int>(outputLines.size()));
}
