#include "text_source_script.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	bool fixtureMutationWasBlocked = false;

	bool expect(bool condition, const char* expression, int line)
	{
		if ( !condition )
		{
			std::cerr << "FAILED line " << line << ": " << expression << '\n';
		}
		return condition;
	}

#define EXPECT(expression) do { if (!expect((expression), #expression, __LINE__)) return false; } while (false)

	std::string readSource(const char* relative)
	{
		std::ifstream input(std::filesystem::path(BARONY_SOURCE_DIR) / relative,
			std::ios::binary);
		std::ostringstream text;
		text << input.rdbuf();
		return input ? text.str() : std::string{};
	}

	bool contains(const std::string& source, const std::string& token)
	{
		return source.find(token) != std::string::npos;
	}

	bool hasDiagnostic(const TextSourceLanguage::ParseResult& result,
		TextSourceLanguage::Severity severity, const std::string& fragment)
	{
		return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
			[&](const TextSourceLanguage::Diagnostic& diagnostic) {
				return diagnostic.severity == severity
					&& contains(diagnostic.message, fragment);
			});
	}

	const TextSourceLanguage::Token* tokenForCanonical(
		const TextSourceLanguage::ParseResult& result, const std::string& canonical)
	{
		auto found = std::find_if(result.tokens.begin(), result.tokens.end(),
			[&](const TextSourceLanguage::Token& token) {
				return token.canonicalTag == canonical;
			});
		return found == result.tokens.end() ? nullptr : &*found;
	}

	std::string withScriptMarker(const std::string& snippet)
	{
		return snippet == "@script" ? snippet : "@script " + snippet;
	}

	std::string removeAttachedPrefix(std::string snippet)
	{
		const std::string prefix = "@attached.";
		const std::size_t found = snippet.find(prefix);
		if ( found != std::string::npos )
		{
			snippet.erase(found + 1, std::string("attached.").size());
		}
		return snippet;
	}

	std::string addAttachedPrefix(std::string snippet)
	{
		if ( contains(snippet, "@attached.") )
		{
			return snippet;
		}
		const std::size_t found = snippet.find('@');
		if ( found != std::string::npos )
		{
			snippet.insert(found + 1, "attached.");
		}
		return snippet;
	}

	std::string stripComments(const std::string& source)
	{
		std::string output;
		output.reserve(source.size());
		bool blockComment = false;
		bool lineComment = false;
		bool stringLiteral = false;
		bool characterLiteral = false;
		bool escaped = false;
		for ( std::size_t i = 0; i < source.size(); ++i )
		{
			const char c = source[i];
			const char next = i + 1 < source.size() ? source[i + 1] : '\0';
			if ( lineComment )
			{
				if ( c == '\n' )
				{
					lineComment = false;
					output.push_back(c);
				}
				continue;
			}
			if ( blockComment )
			{
				if ( c == '*' && next == '/' )
				{
					blockComment = false;
					++i;
				}
				else if ( c == '\n' )
				{
					output.push_back(c);
				}
				continue;
			}
			if ( !stringLiteral && !characterLiteral && c == '/' && next == '/' )
			{
				lineComment = true;
				++i;
				continue;
			}
			if ( !stringLiteral && !characterLiteral && c == '/' && next == '*' )
			{
				blockComment = true;
				++i;
				continue;
			}
			output.push_back(c);
			if ( escaped )
			{
				escaped = false;
				continue;
			}
			if ( (stringLiteral || characterLiteral) && c == '\\' )
			{
				escaped = true;
			}
			else if ( !characterLiteral && c == '"' )
			{
				stringLiteral = !stringLiteral;
			}
			else if ( !stringLiteral && c == '\'' )
			{
				characterLiteral = !characterLiteral;
			}
		}
		return output;
	}

	bool fixtureExtensionDispatch(void* sourceContext,
		const TextSourceLanguage::Token& token, std::string& error)
	{
		if ( !sourceContext )
		{
			error = "fixture requires a destination counter";
			return false;
		}
		try
		{
			*static_cast<int*>(sourceContext) += std::stoi(token.argument);
			fixtureMutationWasBlocked =
				TextSourceLanguage::removeScriptTagSource("fixture.mod") == 0;
			return true;
		}
		catch ( const std::exception& )
		{
			error = "fixture received a non-integer argument";
			return false;
		}
	}

	bool fixtureEvenIntegerValidator(
		const TextSourceLanguage::Token& token, std::string& error)
	{
		try
		{
			if ( std::stoi(token.argument) % 2 == 0 )
			{
				return true;
			}
		}
		catch ( const std::exception& )
		{
		}
		error = "fixture requires an even integer";
		return false;
	}

	TextSourceLanguage::ScriptTagDefinition fixtureExtensionDefinition()
	{
		using namespace TextSourceLanguage;
		ScriptTagDefinition definition;
		definition.canonicalTag = "@sam_fixture=";
		definition.syntax = "@sam_fixture=<integer>";
		definition.insertionTemplate = "@sam_fixture=<integer>";
		definition.category = "S.A.M. Test Fixture";
		definition.aliases = "@sam_fixture_alias=";
		definition.value = { ArgumentKind::Integer, true, "one", "none", "signed integer" };
		definition.execution = { AttachmentMode::Optional, "fixture source", "fixture only",
			false, false, false, true, CommandSafety::GameplayMutation,
			ScriptDispatchPhase::RuntimeExecution };
		definition.defaultBehavior = "Adds the parsed value to the fixture counter.";
		definition.description = "Synthetic registration used only by the registry characterization test.";
		definition.minimalExample = "@sam_fixture=4";
		definition.invalidExample = "@sam_fixture=bad";
		definition.caveats = "Not a real S.A.M. command; removed before the test exits.";
		definition.sourceLocation = "tests/text_source_script_tests.cpp";
		definition.verification = { VerificationLevel::DispatchVerified, true,
			"synthetic parser, alias, dispatch, unload, restore, and collision characterization",
			"fixtureExtensionDispatch" };
		definition.origin = { ScriptOriginKind::SAMMod, "S.A.M. Mod: fixture.mod",
			"fixture.mod", "1.2.3" };
		definition.availability = ScriptAvailability::Available;
		definition.availabilityDetail = "Synthetic fixture is loaded";
		definition.extensionDispatch = fixtureExtensionDispatch;
		return definition;
	}

	bool testCatalogShapeAndVerificationTotals()
	{
		using TextSourceLanguage::VerificationLevel;
		const auto& definitions = TextSourceLanguage::scriptTagDefinitions();
		EXPECT(definitions.size() == 37);

		std::set<std::string> canonicalTags;
		std::set<std::string> categories;
		int literalDefinitions = 0;
		int patternFamilies = 0;
		int aliases = 0;
		int behaviorVerified = 0;
		int dispatchVerified = 0;
		int parserVerified = 0;
		int manualGameVerified = 0;
		int manualRequired = 0;
		for ( const auto& definition : definitions )
		{
			EXPECT(definition.canonicalTag[0] == '@');
			EXPECT(definition.syntax[0] != '\0');
			EXPECT(definition.insertionTemplate[0] != '\0');
			EXPECT(definition.category[0] != '\0');
			EXPECT(definition.value.valueCount[0] != '\0');
			EXPECT(definition.value.acceptedDomain[0] != '\0');
			EXPECT(definition.execution.targetTypes[0] != '\0');
			EXPECT(definition.execution.scope[0] != '\0');
			EXPECT(definition.description[0] != '\0');
			EXPECT(definition.minimalExample[0] == '@');
			EXPECT(definition.invalidExample[0] == '@');
			EXPECT(definition.sourceLocation[0] != '\0');
			EXPECT(definition.verification.method[0] != '\0');
			EXPECT(definition.verification.dispatchEvidence[0] != '\0');
			EXPECT(canonicalTags.insert(definition.canonicalTag).second);
			categories.insert(definition.category);
			literalDefinitions += definition.patternFamily ? 0 : 1;
			patternFamilies += definition.patternFamily ? 1 : 0;
			aliases += definition.aliases[0] == '\0' ? 0 : 1;
			manualRequired += definition.verification.manualBehaviorRequired ? 1 : 0;
			switch ( definition.verification.level )
			{
				case VerificationLevel::BehaviorVerified: ++behaviorVerified; break;
				case VerificationLevel::DispatchVerified: ++dispatchVerified; break;
				case VerificationLevel::ParserVerified: ++parserVerified; break;
				case VerificationLevel::ManualGameTestVerified: ++manualGameVerified; break;
			}
			EXPECT(definition.origin.kind
				== TextSourceLanguage::ScriptOriginKind::NativeBaronyAutomatia);
			EXPECT(definition.availability
				== TextSourceLanguage::ScriptAvailability::Available);
		}
		EXPECT(literalDefinitions == 35);
		EXPECT(patternFamilies == 2);
		EXPECT(aliases == 0);
		EXPECT(behaviorVerified == 1);
		EXPECT(dispatchVerified == 36);
		EXPECT(parserVerified == 0);
		EXPECT(manualGameVerified == 0);
		EXPECT(manualRequired == 36);
		EXPECT(categories.size() >= 10);
		EXPECT(TextSourceLanguage::scriptRecipeDefinitions().size() == 6);
		return true;
	}

	bool testEveryCatalogDefinitionParsesAndRejectsItsInvalidForm()
	{
		const auto& definitions = TextSourceLanguage::scriptTagDefinitions();
		for ( std::size_t i = 0; i < definitions.size(); ++i )
		{
			const auto& definition = definitions[i];
			const auto valid = TextSourceLanguage::parseTextSourceScript(
				withScriptMarker(definition.minimalExample));
			if ( !valid.success() )
			{
				std::cerr << "Valid catalog example failed for "
					<< definition.canonicalTag << '\n';
				for ( const auto& diagnostic : valid.diagnostics )
				{
					std::cerr << "  " << diagnostic.message << '\n';
				}
				return false;
			}
			const auto* token = tokenForCanonical(valid, definition.canonicalTag);
			EXPECT(token != nullptr);
			EXPECT(token->catalogIndex == i);
			EXPECT(TextSourceLanguage::definitionForToken(*token) == &definition);
			EXPECT(token->safety == definition.execution.safety);

			const auto invalid = TextSourceLanguage::parseTextSourceScript(
				withScriptMarker(definition.invalidExample));
			if ( invalid.success() )
			{
				std::cerr << "Invalid catalog example was accepted for "
					<< definition.canonicalTag << ": "
					<< definition.invalidExample << '\n';
				return false;
			}
			EXPECT(std::any_of(invalid.tokens.begin(), invalid.tokens.end(),
				[](const TextSourceLanguage::Token& token) { return !token.valid; }));
		}
		return true;
	}

	bool testEveryAttachmentContract()
	{
		using TextSourceLanguage::AttachmentMode;
		using TextSourceLanguage::Severity;
		for ( const auto& definition : TextSourceLanguage::scriptTagDefinitions() )
		{
			std::string plain = removeAttachedPrefix(definition.minimalExample);
			std::string attached = definition.canonicalTag == "@explode="
				? "@attached.explode=0" : addAttachedPrefix(plain);
			const auto plainResult = TextSourceLanguage::parseTextSourceScript(
				withScriptMarker(plain));
			const auto attachedResult = TextSourceLanguage::parseTextSourceScript(
				withScriptMarker(attached));
			switch ( definition.execution.attachment )
			{
				case AttachmentMode::Unsupported:
					EXPECT(plainResult.success());
					EXPECT(!attachedResult.success());
					EXPECT(hasDiagnostic(attachedResult, Severity::Error,
						"not supported for map-generation directive"));
					break;
				case AttachmentMode::Optional:
					EXPECT(plainResult.success());
					EXPECT(attachedResult.success());
					EXPECT(!hasDiagnostic(plainResult, Severity::Warning, "requires @attached."));
					break;
				case AttachmentMode::Required:
					EXPECT(plainResult.success());
					EXPECT(attachedResult.success());
					EXPECT(hasDiagnostic(plainResult, Severity::Warning, "requires @attached."));
					EXPECT(!hasDiagnostic(attachedResult, Severity::Warning, "requires @attached."));
					break;
				case AttachmentMode::NotMeaningful:
					EXPECT(plainResult.success());
					EXPECT(attachedResult.success());
					EXPECT(hasDiagnostic(attachedResult, Severity::Warning,
						"has no target-specific meaning"));
					break;
			}
		}
		return true;
	}

	bool testGeneratedFamiliesCompletely()
	{
		static const char* stats[] = { "STR", "DEX", "CON", "INT", "PER", "CHR" };
		int concreteForms = 35;
		for ( const char* stat : stats )
		{
			for ( const char operation : { '=', '+' } )
			{
				const std::string spelling = std::string("@st") + stat + operation + "7";
				const auto parsed = TextSourceLanguage::parseTextSourceScript(
					"@script " + spelling);
				EXPECT(parsed.success());
				const auto* token = tokenForCanonical(parsed, "@st<STAT>{=|+}");
				EXPECT(token != nullptr);
				EXPECT(token->command == spelling.substr(0, spelling.size() - 1));
				++concreteForms;
			}
		}
		for ( int proficiency = 0; proficiency < 16; ++proficiency )
		{
			for ( const char operation : { '=', '+' } )
			{
				const std::string spelling = "@pro" + std::to_string(proficiency)
					+ operation + "7";
				const auto parsed = TextSourceLanguage::parseTextSourceScript(
					"@script " + spelling);
				EXPECT(parsed.success());
				EXPECT(tokenForCanonical(parsed, "@pro<INDEX>{=|+}") != nullptr);
				++concreteForms;
			}
		}
		EXPECT(concreteForms == 79);
		EXPECT(!TextSourceLanguage::parseTextSourceScript("@script @stLCK=1").success());
		EXPECT(!TextSourceLanguage::parseTextSourceScript("@script @pro16=1").success());
		EXPECT(!TextSourceLanguage::parseTextSourceScript("@script @pro-1=1").success());
		EXPECT(!TextSourceLanguage::parseTextSourceScript("@script @pro01=1").success());
		return true;
	}

	bool testTokenizationOrderDuplicatesAndArguments()
	{
		const auto parsed = TextSourceLanguage::parseTextSourceScript(
			"prefix @script\n@setvar=count,1 @setvar=count,2 "
			"@power=-2--1,3-4 @attached.seteffect=5-6;10-12 "
			"@attached.damage=-7 @explode=-1,-2;174");
		EXPECT(parsed.success());
		EXPECT(parsed.tokens.size() == 7);
		EXPECT(parsed.tokens[0].command == "@script");
		EXPECT(parsed.tokens[0].raw == "@script\n");
		EXPECT(parsed.tokens[1].command == "@setvar=");
		EXPECT(parsed.tokens[1].line == 2 && parsed.tokens[1].column == 1);
		EXPECT(parsed.tokens[2].command == "@setvar=");
		EXPECT(parsed.tokens[1].argument == "count,1");
		EXPECT(parsed.tokens[2].argument == "count,2");
		EXPECT(parsed.tokens[4].attached);
		EXPECT(parsed.tokens[4].argument == "5-6;10-12");
		EXPECT(parsed.tokens[5].argument == "-7");
		EXPECT(parsed.tokens[6].argument == "-1,-2;174");

		const auto tab = TextSourceLanguage::parseTextSourceScript(
			"@script\t@setvar=count,1");
		EXPECT(!tab.success());
		EXPECT(hasDiagnostic(tab, TextSourceLanguage::Severity::Error,
			"incorrect argument count"));
		EXPECT(!TextSourceLanguage::parseTextSourceScript(
			"@script @seteffect=5 @damage=1,2").success());
		EXPECT(!TextSourceLanguage::parseTextSourceScript(
			"@script @definitely_unknown=1").success());
		const auto mixed = TextSourceLanguage::parseTextSourceScript(
			"@script @damage=bad @setvar=still_valid,3");
		EXPECT(!mixed.success());
		EXPECT(mixed.tokens.size() == 3);
		EXPECT(mixed.tokens[0].valid);
		EXPECT(!mixed.tokens[1].valid);
		EXPECT(mixed.tokens[2].valid);
		EXPECT(!TextSourceLanguage::parseTextSourceScript("plain message").success());
		return true;
	}

	bool testRecipesAndComponentVerification()
	{
		using TextSourceLanguage::VerificationLevel;
		for ( const auto& recipe : TextSourceLanguage::scriptRecipeDefinitions() )
		{
			const auto parsed = TextSourceLanguage::parseTextSourceScript(recipe.script);
			if ( !parsed.success() )
			{
				std::cerr << "Recipe failed: " << recipe.name << '\n';
				return false;
			}
			EXPECT(!parsed.tokens.empty());
			EXPECT(parsed.tokens.front().canonicalTag == "@script");
			for ( const auto& token : parsed.tokens )
			{
				const auto* definition = TextSourceLanguage::definitionForToken(token);
				EXPECT(definition != nullptr);
				EXPECT(definition->verification.level != VerificationLevel::ParserVerified);
			}
			EXPECT(recipe.verification == VerificationLevel::DispatchVerified);
			EXPECT(recipe.manualBehaviorRequired);
			EXPECT(recipe.verificationMethod[0] != '\0');
		}
		return true;
	}

	bool testSearchAndInsertionHelpers()
	{
		const auto& definitions = TextSourceLanguage::scriptTagDefinitions();
		auto finds = [&](const std::string& query, const std::string& canonical) {
			return std::any_of(definitions.begin(), definitions.end(),
				[&](const TextSourceLanguage::ScriptTagDefinition& definition) {
					return canonical == definition.canonicalTag
						&& TextSourceLanguage::scriptTagMatchesSearch(definition, query);
				});
		};
		EXPECT(finds("player", "@hunger="));
		EXPECT(finds("monster", "@freezemonsters="));
		EXPECT(finds("effect", "@seteffect="));
		EXPECT(finds("power", "@power="));
		EXPECT(finds("attach", "@attachto="));
		EXPECT(finds("stat", "@st<STAT>{=|+}"));
		EXPECT(finds("inventory", "@pickupitems="));
		EXPECT(finds("mechanism", "@wire="));

		std::string script = "@script";
		std::string error;
		EXPECT(TextSourceLanguage::appendScriptSnippet(
			script, "@power=<X,Y>", 64, error));
		EXPECT(script == "@script @power=<X,Y>");
		EXPECT(error.empty());
		EXPECT(TextSourceLanguage::appendScriptSnippet(
			script, "@setvar=test,1", 64, error));
		EXPECT(!TextSourceLanguage::appendScriptSnippet(
			script, std::string(64, 'x'), 64, error));
		EXPECT(contains(error, "exceed"));
		EXPECT(!TextSourceLanguage::appendScriptSnippet(script, "", 64, error));
		return true;
	}

	bool testGameplayResourceConvention()
	{
		const std::map<std::string, std::string> resources{
			{ "sample_script", "@script @clrplayer" }
		};
		auto lookup = [&](const std::string& key, std::string& script) {
			auto found = resources.find(key);
			if ( found == resources.end() )
			{
				return false;
			}
			script = found->second;
			return true;
		};

		const auto resolved = TextSourceLanguage::resolveTextSourceResource(
			"$sample_script trailing text", lookup);
		EXPECT(resolved.isReference && resolved.found);
		EXPECT(resolved.key == "sample_script");
		EXPECT(resolved.script == "@script @clrplayer");
		EXPECT(TextSourceLanguage::resolveTextSourceResource(
			"$$sample_script", lookup).found);
		const auto lineTerminated = TextSourceLanguage::resolveTextSourceResource(
			"$sample_script\nignored", lookup);
		EXPECT(lineTerminated.found && lineTerminated.key == "sample_script");

		const auto missing = TextSourceLanguage::resolveTextSourceResource(
			"$missing_script", lookup);
		EXPECT(missing.isReference && !missing.found);
		EXPECT(contains(missing.error, "was not found"));
		const auto invalid = TextSourceLanguage::resolveTextSourceResource(
			"data/scripts/sample_script.json", lookup);
		EXPECT(!invalid.isReference && !invalid.found);
		EXPECT(contains(invalid.error, "must begin with '$'"));
		return true;
	}

	bool testDisposableBehaviorAndReset()
	{
		std::string name;
		int value = 0;
		EXPECT(TextSourceLanguage::parseSetVariableArgument("door,-7", name, value));
		EXPECT(name == "door" && value == -7);
		EXPECT(!TextSourceLanguage::parseSetVariableArgument("door", name, value));
		EXPECT(!TextSourceLanguage::parseSetVariableArgument("door,1,2", name, value));

		TextSourceLanguage::TestContext context;
		context.activatorPlayer = -1;
		context.sourceX = 12;
		context.sourceY = 34;
		context.playableFloor = 5;
		context.authoredMapLayer = 19;
		context.localZ = 1.25;
		context.spatialRevision = 77;
		context.mapContext = "upper_keep.lmp";
		context.mapInstance = "instance:test";

		TextSourceLanguage::TestSession session;
		session.reset(context);
		session.setScript("@setvar=must_not_run,1");
		const auto messageMode = session.run();
		EXPECT(!messageMode.success);
		EXPECT(messageMode.variables.empty());
		EXPECT(std::any_of(messageMode.diagnostics.begin(),
			messageMode.diagnostics.end(),
			[](const TextSourceLanguage::Diagnostic& diagnostic) {
				return contains(diagnostic.message, "message mode");
			}));

		session.setScript("@script @setvar=quest_gate,9 @setvar=quest_gate,11 "
			"@nextlevel=2 @power=1,2");
		const auto result = session.run();
		EXPECT(result.success && result.blockedCommands);
		EXPECT(result.variables.at("quest_gate") == 11);
		EXPECT(session.context().playableFloor == 5);
		EXPECT(session.context().authoredMapLayer == 19);
		EXPECT(std::fabs(session.context().localZ - 1.25) < 0.0001);
		EXPECT(session.context().spatialRevision == 77);
		EXPECT(session.context().mapContext == "upper_keep.lmp");
		EXPECT(session.context().mapInstance == "instance:test");

		bool sawTransitionBlocked = false;
		bool sawMechanismBlocked = false;
		for ( const auto& diagnostic : result.diagnostics )
		{
			sawTransitionBlocked = sawTransitionBlocked
				|| (contains(diagnostic.message, "@nextlevel=")
					&& contains(diagnostic.message, "blocked"));
			sawMechanismBlocked = sawMechanismBlocked
				|| (contains(diagnostic.message, "@power=")
					&& contains(diagnostic.message, "blocked"));
		}
		EXPECT(sawTransitionBlocked && sawMechanismBlocked);

		TextSourceLanguage::TestContext replacement = context;
		replacement.playableFloor = 2;
		replacement.authoredMapLayer = 7;
		session.reset(replacement);
		EXPECT(session.variables().empty());
		EXPECT(session.context().playableFloor == 2);
		EXPECT(session.context().authoredMapLayer == 7);
		return true;
	}

	bool testUnifiedExtensionRegistryLifecycleAndCollisions()
	{
		using namespace TextSourceLanguage;
		EXPECT(nativeScriptTagDefinitions().size() == 37);
		EXPECT(scriptTagDefinitions().size() == 37);
		EXPECT(scriptTagCountForOrigin(ScriptOriginKind::SAMFramework) == 0);
		EXPECT(scriptTagCountForOrigin(ScriptOriginKind::SAMMod) == 0);

		const std::uint64_t initialGeneration = scriptTagRegistryGeneration();
		ScriptTagDefinition fixture = fixtureExtensionDefinition();
		const auto registered = registerScriptTagExtension(fixture);
		EXPECT(registered.status == ScriptRegistrationStatus::Registered);
		EXPECT(registered.success());
		EXPECT(scriptTagRegistryGeneration() > initialGeneration);
		EXPECT(nativeScriptTagDefinitions().size() == 37);
		EXPECT(scriptTagDefinitions().size() == 38);
		EXPECT(scriptTagCountForOrigin(ScriptOriginKind::SAMMod) == 1);

		const auto canonical = parseTextSourceScript("@script @sam_fixture=7");
		EXPECT(canonical.success());
		const auto* canonicalToken = tokenForCanonical(canonical, "@sam_fixture=");
		EXPECT(canonicalToken != nullptr);
		EXPECT(canonicalToken->command == "@sam_fixture=");
		const auto* canonicalDefinition = definitionForToken(*canonicalToken);
		EXPECT(canonicalDefinition != nullptr);
		EXPECT(canonicalDefinition->origin.kind == ScriptOriginKind::SAMMod);
		EXPECT(std::string(canonicalDefinition->origin.stableSourceId) == "fixture.mod");
		EXPECT(std::string(canonicalDefinition->origin.sourceVersion) == "1.2.3");
		EXPECT(scriptTagMatchesSearch(*canonicalDefinition, "fixture.mod"));
		EXPECT(scriptTagMatchesSearch(*canonicalDefinition, "installed s.a.m. mod"));

		const auto alias = parseTextSourceScript("@script @attached.sam_fixture_alias=-2");
		EXPECT(alias.success());
		const auto* aliasToken = tokenForCanonical(alias, "@sam_fixture=");
		EXPECT(aliasToken != nullptr);
		EXPECT(aliasToken->command == "@sam_fixture_alias=");
		EXPECT(aliasToken->argument == "-2");
		EXPECT(aliasToken->attached);
		EXPECT(!parseTextSourceScript("@script @sam_fixture=bad").success());

		int counter = 3;
		std::string dispatchError;
		EXPECT(dispatchScriptTagExtension(*canonicalToken, &counter,
			ScriptDispatchPhase::RuntimeExecution, dispatchError));
		EXPECT(counter == 10);
		EXPECT(fixtureMutationWasBlocked);
		EXPECT(scriptTagDefinitions().size() == 38);
		EXPECT(dispatchError.empty());
		EXPECT(!dispatchScriptTagExtension(*canonicalToken, &counter,
			ScriptDispatchPhase::MapGeneration, dispatchError));
		EXPECT(contains(dispatchError, "not map-generation"));

		const auto duplicate = registerScriptTagExtension(fixture);
		EXPECT(!duplicate.success());
		EXPECT(duplicate.status == ScriptRegistrationStatus::CommandCollision);

		ScriptTagDefinition nativeCollision = fixture;
		nativeCollision.canonicalTag = "@power=";
		nativeCollision.aliases = "";
		const auto rejectedNative = registerScriptTagExtension(nativeCollision);
		EXPECT(!rejectedNative.success());
		EXPECT(rejectedNative.status == ScriptRegistrationStatus::CommandCollision);

		ScriptTagDefinition aliasCollision = fixture;
		aliasCollision.canonicalTag = "@sam_unique=";
		aliasCollision.aliases = "@wire=";
		const auto rejectedAlias = registerScriptTagExtension(aliasCollision);
		EXPECT(!rejectedAlias.success());
		EXPECT(rejectedAlias.status == ScriptRegistrationStatus::AliasCollision);

		ScriptTagDefinition generatedFamilyCollision = fixture;
		generatedFamilyCollision.canonicalTag = "@stSTR=";
		generatedFamilyCollision.aliases = "";
		const auto rejectedFamily = registerScriptTagExtension(generatedFamilyCollision);
		EXPECT(!rejectedFamily.success());
		EXPECT(rejectedFamily.status == ScriptRegistrationStatus::CommandCollision);

		ScriptTagDefinition missingCallback = fixture;
		missingCallback.canonicalTag = "@sam_no_dispatch=";
		missingCallback.aliases = "";
		missingCallback.extensionDispatch = nullptr;
		const auto rejectedCallback = registerScriptTagExtension(missingCallback);
		EXPECT(!rejectedCallback.success());
		EXPECT(rejectedCallback.status == ScriptRegistrationStatus::MissingDispatch);

		ScriptTagDefinition missingValidator = fixture;
		missingValidator.canonicalTag = "@sam_custom_schema=";
		missingValidator.aliases = "";
		missingValidator.value.kind = ArgumentKind::RegisteredExtension;
		const auto rejectedValidator = registerScriptTagExtension(missingValidator);
		EXPECT(!rejectedValidator.success());
		EXPECT(rejectedValidator.status == ScriptRegistrationStatus::InvalidDefinition);
		missingValidator.extensionValidator = fixtureEvenIntegerValidator;
		missingValidator.availability = ScriptAvailability::NeedsVerification;
		missingValidator.availabilityDetail = "Synthetic private schema needs behavior verification";
		const auto registeredCustomSchema = registerScriptTagExtension(missingValidator);
		EXPECT(registeredCustomSchema.success());
		const auto customSchemaAccepted =
			parseTextSourceScript("@script @sam_custom_schema=8");
		EXPECT(customSchemaAccepted.success());
		EXPECT(hasDiagnostic(customSchemaAccepted, Severity::Warning,
			"needs verification"));
		const auto rejectedCustomArgument =
			parseTextSourceScript("@script @sam_custom_schema=7");
		EXPECT(!rejectedCustomArgument.success());
		EXPECT(hasDiagnostic(rejectedCustomArgument, Severity::Error,
			"fixture requires an even integer"));
		EXPECT(scriptTagDefinitions().size() == 39);
		EXPECT(std::string(scriptTagDefinitions()[37].canonicalTag)
			== "@sam_custom_schema=");
		EXPECT(std::string(scriptTagDefinitions()[38].canonicalTag)
			== "@sam_fixture=");

		EXPECT(markScriptTagSourceUnavailable("fixture.mod",
			"Unavailable - required S.A.M. mod fixture.mod is not currently loaded"));
		EXPECT(scriptTagDefinitions().size() == 39);
		const auto unavailable = parseTextSourceScript("@script @sam_fixture=1");
		EXPECT(!unavailable.success());
		EXPECT(hasDiagnostic(unavailable, Severity::Error,
			"required S.A.M. mod fixture.mod"));
		const auto& retainedDefinitions = scriptTagDefinitions();
		const auto retainedIt = std::find_if(retainedDefinitions.begin(),
			retainedDefinitions.end(), [](const ScriptTagDefinition& definition) {
				return std::string(definition.canonicalTag) == "@sam_fixture=";
			});
		const auto* retained = retainedIt == retainedDefinitions.end()
			? nullptr : &*retainedIt;
		EXPECT(retained != nullptr);
		EXPECT(retained->availability == ScriptAvailability::UnavailableMissingMod);
		EXPECT(retained->extensionDispatch == nullptr);

		ScriptTagDefinition impostor = fixture;
		impostor.origin = { ScriptOriginKind::SAMMod, "S.A.M. Mod: impostor.mod",
			"impostor.mod", "9.9.9" };
		const auto rejectedTakeover = registerScriptTagExtension(impostor);
		EXPECT(!rejectedTakeover.success());
		EXPECT(rejectedTakeover.status == ScriptRegistrationStatus::CommandCollision);

		const auto restored = registerScriptTagExtension(fixture);
		EXPECT(restored.status == ScriptRegistrationStatus::Restored);
		EXPECT(parseTextSourceScript("@script @sam_fixture=5").success());
		EXPECT(removeScriptTagSource("fixture.mod") == 2);
		EXPECT(scriptTagDefinitions().size() == 37);
		EXPECT(scriptTagCountForOrigin(ScriptOriginKind::SAMMod) == 0);
		EXPECT(!parseTextSourceScript("@script @sam_fixture=5").success());
		EXPECT(removeScriptTagSource("fixture.mod") == 0);
		return true;
	}

	bool testCurrentSAMTextSourceRegistrationSurface()
	{
		using namespace TextSourceLanguage;
		const std::filesystem::path samRoot =
			std::filesystem::path(BARONY_SOURCE_DIR) / "src" / "sam";
		int textSourceRegistrationReferences = 0;
		int scriptTagLiterals = 0;
		for ( const auto& entry : std::filesystem::recursive_directory_iterator(samRoot) )
		{
			if ( !entry.is_regular_file() )
			{
				continue;
			}
			const std::string extension = entry.path().extension().string();
			if ( extension != ".cpp" && extension != ".hpp"
				&& extension != ".cc" && extension != ".hh"
				&& extension != ".c" && extension != ".h" )
			{
				continue;
			}
			std::ifstream input(entry.path(), std::ios::binary);
			std::ostringstream buffer;
			buffer << input.rdbuf();
			const std::string active = stripComments(buffer.str());
			textSourceRegistrationReferences += contains(active,
				"registerScriptTagExtension") ? 1 : 0;
			scriptTagLiterals += contains(active, "@script") ? 1 : 0;
		}
		EXPECT(textSourceRegistrationReferences == 0);
		EXPECT(scriptTagLiterals == 0);
		EXPECT(scriptTagCountForOrigin(ScriptOriginKind::SAMFramework) == 0);
		EXPECT(scriptTagCountForOrigin(ScriptOriginKind::SAMMod) == 0);

		const std::string manifest = readSource("src/sam/framework/sam_workshop.hpp");
		const std::string loader = readSource("src/sam/framework/sam_loader.cpp");
		const std::string lua = readSource("src/sam/framework/sam_lua_runtime.cpp");
		const std::string js = readSource("src/sam/framework/sam_js_runtime.cpp");
		EXPECT(contains(manifest, "std::vector<std::string> plugins"));
		EXPECT(!contains(manifest, "scriptCommands"));
		EXPECT(contains(loader, "on_event(event) and/or on_tick(event)"));
		EXPECT(contains(loader, "Plugin (.dll) loading is not"));
		EXPECT(contains(lua, "lua_sam_register_hook"));
		EXPECT(contains(lua, "g_customHooks.push_back(name)"));
		EXPECT(contains(js, "js_sam_register_hook"));
		EXPECT(contains(js, "g_customHooks.push_back(name)"));
		return true;
	}

	bool testRuntimeCatalogBidirectionalConsistency()
	{
		const std::string runtime = readSource("src/actgeneral.cpp");
		EXPECT(!runtime.empty());
		const std::string active = stripComments(runtime);
		const std::size_t handlerStart = active.find(
			"void TextSourceScript::handleTextSourceScript");
		const std::size_t handlerEnd = active.find(
			"void Entity::actTextSource", handlerStart);
		EXPECT(handlerStart != std::string::npos && handlerEnd != std::string::npos);
		const std::string handler = active.substr(handlerStart, handlerEnd - handlerStart);

		std::set<std::string> runtimeTags;
		const std::regex dispatchFind(
			R"re(\(\*it\)\.find\("(@[^"]+)"\))re");
		for ( std::sregex_iterator it(handler.begin(), handler.end(), dispatchFind), end;
			it != end; ++it )
		{
			const std::string tag = (*it)[1].str();
			if ( tag != "@attached." )
			{
				runtimeTags.insert(tag);
			}
		}

		const std::regex setupCall(
			R"re(textSourceProcessScriptTag\(script, "(@[^"]+)")re");
		for ( std::sregex_iterator it(active.begin(), active.end(), setupCall), end;
			it != end; ++it )
		{
			runtimeTags.insert((*it)[1].str());
		}
		runtimeTags.insert("@script");
		EXPECT(contains(handler, "for ( int i = 0; i < NUMSTATS; ++i )"));
		EXPECT(contains(handler, "for ( int i = 0; i < NUMPROFICIENCIES; ++i )"));
		runtimeTags.insert("@st<STAT>{=|+}");
		runtimeTags.insert("@pro<INDEX>{=|+}");

		std::set<std::string> catalogTags;
		for ( const auto& definition : TextSourceLanguage::scriptTagDefinitions() )
		{
			catalogTags.insert(definition.canonicalTag);
			EXPECT(contains(active, definition.verification.dispatchEvidence));
		}
		if ( runtimeTags != catalogTags )
		{
			for ( const auto& tag : runtimeTags )
			{
				if ( !catalogTags.count(tag) )
				{
					std::cerr << "Runtime-only active tag: " << tag << '\n';
				}
			}
			for ( const auto& tag : catalogTags )
			{
				if ( !runtimeTags.count(tag) )
				{
					std::cerr << "Catalog-only tag: " << tag << '\n';
				}
			}
			return false;
		}
		EXPECT(!contains(handler, "@findanytarget="));
		EXPECT(!contains(handler, "@addtomonster="));
		return true;
	}

	bool testRuntimeTesterSpatialAuthorityAndLifecycleSeams()
	{
		const std::string runtime = readSource("src/actgeneral.cpp");
		const std::string shared = readSource("src/text_source_script.cpp");
		const std::string catalog = readSource("src/text_source_script_catalog.cpp");
		const std::string registry = readSource("src/text_source_script_registry.cpp");
		const std::string tester = readSource("src/text_source_script_tester.cpp");
		const std::string editor = readSource("src/editor.cpp");
		const std::string sam = readSource("src/sam/framework/sam_logger.hpp");
		const std::string items = readSource("src/items.cpp");
		EXPECT(!runtime.empty() && !shared.empty() && !catalog.empty() && !registry.empty()
			&& !tester.empty() && !editor.empty() && !sam.empty() && !items.empty());

		EXPECT(contains(runtime, "TextSourceLanguage::parseTextSourceScript(input)"));
		EXPECT(contains(runtime, "TextSourceLanguage::resolveTextSourceResource"));
		EXPECT(contains(runtime, "TextSourceLanguage::parseSetVariableArgument"));
		EXPECT(contains(runtime, "invalid tokens will be skipped"));
		EXPECT(contains(runtime, "if ( token.valid )"));
		EXPECT(contains(runtime, "dispatchScriptTagExtension(parsedToken, &src"));
		EXPECT(contains(runtime, "ScriptDispatchPhase::RuntimeExecution"));
		EXPECT(contains(runtime, "ScriptDispatchPhase::MapGeneration"));
		EXPECT(contains(runtime, "const bool hasValidScriptMarker"));
		EXPECT(contains(runtime, "textSourceScript.handleTextSourceScript(*this, output)"));
		EXPECT(contains(registry, "spellingsOverlap"));
		EXPECT(contains(registry, "markScriptTagSourceUnavailable"));
		EXPECT(contains(registry, "nativeScriptTagDefinitions()"));
		EXPECT(contains(tester, "TextSourceLanguage::resolveTextSourceResource"));
		EXPECT(contains(tester, "TextSourceLanguage::scriptTagDefinitions()"));
		EXPECT(contains(tester, "TextSourceLanguage::appendScriptSnippet"));
		EXPECT(contains(tester, "Origin: %s"));
		EXPECT(contains(tester, "Availability: %s"));
		EXPECT(contains(tester, "OriginFilter::Barony"));
		EXPECT(contains(tester, "OriginFilter::Automatia"));
		EXPECT(contains(tester, "source-verifiable exclusive provenance"));
		EXPECT(contains(tester, "registers zero Text Source commands"));
		EXPECT(contains(tester, "ScriptTextParser.readAllScripts()"));
		EXPECT(contains(tester, "unsafe commands were blocked"));
		EXPECT(contains(editor, "Text Source Script Tester..."));
		EXPECT(contains(editor, "drawTextSourceScriptTester()"));

		EXPECT(contains(runtime, "if ( multiplayer == CLIENT )"));
		EXPECT(contains(runtime, "for ( int c = 0; c < MAXPLAYERS; ++c )"));
		EXPECT(!contains(runtime,
			"statOnlyUpdateNeeded = (entity->behavior == &actPlayer)"));
		EXPECT(contains(runtime, "worldState.playerSharesInstance(player, map)"));
		EXPECT(contains(runtime, "textSourceEntitySharesFloor"));
		EXPECT(contains(runtime, "source.playableFloor == candidate.playableFloor"));
		EXPECT(contains(runtime, "newEntityWithSpatialContext"));
		EXPECT(contains(runtime, "ScopedPlayableFloorRuntimeContext spatialScope"));
		EXPECT(contains(runtime, "src.spatialSpawnContext()"));
		EXPECT(contains(runtime,
			"textSourceEntitySharesFloor(src, *scriptEntity)"));
		EXPECT(contains(runtime, "if ( transferred )"));
		EXPECT(contains(runtime, "equippedItems.count(i)"));
		EXPECT(contains(runtime, "pickedUpItems.count(i)"));
		EXPECT(!contains(runtime, "authoredMapLayer = static_cast<int>(src.z"));

		EXPECT(contains(runtime, "textSourceDelay > 0"));
		EXPECT(contains(runtime, "textSourceVariables4W & 0xFF"));
		EXPECT(contains(runtime, "for ( int i = 4; i < 60; ++i )"));
		EXPECT(contains(runtime, "src.skill[i] >> (c * 8)"));
		EXPECT(contains(sam, "SAM_FRAMEWORK_VERSION \"2.1.0\""));
		EXPECT(contains(items, "if ( !isValidRuntimeItemType(runtimeType) )"));
		EXPECT(contains(items, "stableIdForRuntimeId(runtimeType)"));
		EXPECT(contains(items, "Refusing world-item entity"));
		EXPECT(!TextSourceLanguage::parseTextSourceScript(
			"@script @item_stable_id=sam:example").success());
		return true;
	}
}

int main()
{
	return testCatalogShapeAndVerificationTotals()
		&& testEveryCatalogDefinitionParsesAndRejectsItsInvalidForm()
		&& testEveryAttachmentContract()
		&& testGeneratedFamiliesCompletely()
		&& testTokenizationOrderDuplicatesAndArguments()
		&& testRecipesAndComponentVerification()
		&& testSearchAndInsertionHelpers()
		&& testGameplayResourceConvention()
		&& testDisposableBehaviorAndReset()
		&& testUnifiedExtensionRegistryLifecycleAndCollisions()
		&& testCurrentSAMTextSourceRegistrationSurface()
		&& testRuntimeCatalogBidirectionalConsistency()
		&& testRuntimeTesterSpatialAuthorityAndLifecycleSeams()
		? 0 : 1;
}
