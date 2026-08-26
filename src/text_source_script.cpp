/*-------------------------------------------------------------------------------

	BARONY / AUTOMATIA
	Shared Text Source script front-end and disposable editor test session.

-------------------------------------------------------------------------------*/

#include "text_source_script.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <iterator>
#include <sstream>
#include <utility>

namespace TextSourceLanguage
{
	namespace
	{
		bool startsWith(const std::string& value, const std::string& prefix)
		{
			return value.compare(0, prefix.size(), prefix) == 0;
		}

		bool parseInteger(const std::string& text, int& value)
		{
			if ( text.empty() )
			{
				return false;
			}
			char* end = nullptr;
			const long parsed = std::strtol(text.c_str(), &end, 10);
			if ( end == text.c_str() || parsed < INT_MIN || parsed > INT_MAX )
			{
				return false;
			}
			while ( *end == '\n' || *end == '\r' )
			{
				++end;
			}
			if ( *end != '\0' )
			{
				return false;
			}
			value = static_cast<int>(parsed);
			return true;
		}

		bool parseIntegerOrRange(const std::string& text)
		{
			if ( text.empty() )
			{
				return false;
			}
			size_t separator = std::string::npos;
			for ( size_t i = 1; i < text.size(); ++i )
			{
				if ( text[i] == '-' )
				{
					separator = i;
					break;
				}
			}
			int ignored = 0;
			if ( separator == std::string::npos )
			{
				return parseInteger(text, ignored);
			}
			return parseInteger(text.substr(0, separator), ignored)
				&& parseInteger(text.substr(separator + 1), ignored);
		}

		bool validateMapRangeArgument(const std::string& argument)
		{
			if ( argument == "all" )
			{
				return true;
			}
			const size_t comma = argument.find(',');
			if ( comma == std::string::npos
				|| argument.find(',', comma + 1) != std::string::npos )
			{
				return false;
			}
			return parseIntegerOrRange(argument.substr(0, comma))
				&& parseIntegerOrRange(argument.substr(comma + 1));
		}

		bool validateEffectAndDurationArgument(const std::string& argument)
		{
			const size_t separator = argument.find(';');
			if ( separator == std::string::npos
				|| argument.find(';', separator + 1) != std::string::npos )
			{
				return false;
			}
			return parseIntegerOrRange(argument.substr(0, separator))
				&& parseIntegerOrRange(argument.substr(separator + 1));
		}

		bool validateExplosionArgument(const Token& token)
		{
			int ignored = 0;
			if ( token.attached )
			{
				return parseInteger(token.argument, ignored);
			}
			const size_t comma = token.argument.find(',');
			if ( comma == std::string::npos )
			{
				return false;
			}
			const size_t semicolon = token.argument.find(';', comma + 1);
			const std::string x = token.argument.substr(0, comma);
			const std::string y = token.argument.substr(comma + 1,
				semicolon == std::string::npos
					? std::string::npos : semicolon - comma - 1);
			if ( !parseInteger(x, ignored) || !parseInteger(y, ignored) )
			{
				return false;
			}
			return semicolon == std::string::npos
				|| parseInteger(token.argument.substr(semicolon + 1), ignored);
		}

		bool validVariableName(const std::string& name)
		{
			// textSourceProcessScriptTag() historically treats everything before
			// the first comma as the key (including $ACH_* achievement keys).
			return !name.empty();
		}

		void locate(const std::string& script, size_t index, int& line, int& column)
		{
			line = 1;
			column = 1;
			for ( size_t i = 0; i < index; ++i )
			{
				if ( script[i] == '\n' )
				{
					++line;
					column = 1;
				}
				else
				{
					++column;
				}
			}
		}

		bool dynamicStatCommand(const std::string& token, std::string& command,
			std::string& argument, std::string& canonicalTag)
		{
			static const char* kStatNames[] = {
				"STR", "DEX", "INT", "CON", "PER", "CHR"
			};
			if ( startsWith(token, "@st") )
			{
				for ( const char* statName : kStatNames )
				{
					const std::string prefix = std::string("@st") + statName;
					if ( token.size() > prefix.size()
						&& startsWith(token, prefix)
						&& (token[prefix.size()] == '=' || token[prefix.size()] == '+') )
					{
						command = token.substr(0, prefix.size() + 1);
						argument = token.substr(prefix.size() + 1);
						canonicalTag = "@st<STAT>{=|+}";
						return true;
					}
				}
				return false;
			}
			if ( !startsWith(token, "@pro") )
			{
				return false;
			}
			const size_t prefixLength = 4;
			size_t cursor = prefixLength;
			while ( cursor < token.size()
				&& std::isdigit(static_cast<unsigned char>(token[cursor])) )
			{
				++cursor;
			}
			if ( cursor == prefixLength || cursor >= token.size()
				|| (token[cursor] != '=' && token[cursor] != '+') )
			{
				return false;
			}
			int proficiency = 0;
			if ( !parseInteger(token.substr(prefixLength, cursor - prefixLength), proficiency)
				|| proficiency < 0 || proficiency >= 16
				|| token.substr(prefixLength, cursor - prefixLength)
					!= std::to_string(proficiency) )
			{
				return false;
			}
			command = token.substr(0, cursor + 1);
			argument = token.substr(cursor + 1);
			canonicalTag = "@pro<INDEX>{=|+}";
			return true;
		}

		bool resourceWordSeparator(char c)
		{
			// Keep this byte-for-byte convention aligned with charIsWordSeparator(),
			// which the gameplay Text Source loader historically used.
			return c == ' ' || c == '\n' || c == '\r' || c == '\0';
		}

		void addError(ParseResult& result, const Token& token, const std::string& message)
		{
			result.diagnostics.push_back(Diagnostic{
				Severity::Error, token.line, token.column, message });
		}

		void addWarning(ParseResult& result, const Token& token, const std::string& message)
		{
			result.diagnostics.push_back(Diagnostic{
				Severity::Warning, token.line, token.column, message });
		}

		bool validateArgument(ParseResult& result, const Token& token,
			const ScriptTagDefinition& definition)
		{
			int ignored = 0;
			switch ( definition.value.kind )
			{
				case ArgumentKind::None:
					if ( !std::all_of(token.argument.begin(), token.argument.end(),
						[](char c) { return c == '\n' || c == '\r'; }) )
					{
						addError(result, token, "incorrect argument count for " + token.command);
						return false;
					}
					return true;
				case ArgumentKind::Integer:
					if ( !parseInteger(token.argument, ignored) )
					{
						addError(result, token, "invalid integer for " + token.command + ": '" + token.argument + "'");
						return false;
					}
					return true;
				case ArgumentKind::MapRange:
					if ( !validateMapRangeArgument(token.argument) )
					{
						addError(result, token, "invalid coordinate/range arguments for " + token.command + ": '" + token.argument + "'");
						return false;
					}
					return true;
				case ArgumentKind::EffectAndDuration:
					if ( !validateEffectAndDurationArgument(token.argument) )
					{
						addError(result, token,
							"effect assignment requires effect-range;duration-range for "
							+ token.command + ": '" + token.argument + "'");
						return false;
					}
					return true;
				case ArgumentKind::EffectRange:
					if ( !parseIntegerOrRange(token.argument) )
					{
						addError(result, token, "invalid effect index/range for "
							+ token.command + ": '" + token.argument + "'");
						return false;
					}
					return true;
				case ArgumentKind::Explosion:
					if ( !validateExplosionArgument(token) )
					{
						addError(result, token,
							"invalid explosion coordinate/sprite arguments for "
							+ token.command + ": '" + token.argument + "'");
						return false;
					}
					return true;
				case ArgumentKind::SetVariable:
				{
					std::string name;
					if ( !parseSetVariableArgument(token.argument, name, ignored) )
					{
						addError(result, token,
							"@setvar requires name,integer with a non-empty variable name");
						return false;
					}
					return true;
				}
				case ArgumentKind::AttachmentTarget:
				case ArgumentKind::MonsterName:
					if ( token.argument.empty() )
					{
						addError(result, token, "missing argument for " + token.command);
						return false;
					}
					return true;
				case ArgumentKind::TriggerCondition:
				{
					static const char* conditions[] = {
						"always", "attached#dies", "attached#exists",
						"attached#invisible", "attached#visible",
						"attached#interacted"
					};
					const bool known = std::any_of(std::begin(conditions), std::end(conditions),
						[&](const char* condition) { return token.argument == condition; });
					if ( !known )
					{
						addError(result, token, "unknown trigger condition '" + token.argument + "'");
						return false;
					}
					return true;
				}
				case ArgumentKind::StatPattern:
				case ArgumentKind::ProficiencyPattern:
					if ( !parseInteger(token.argument, ignored) )
					{
						addError(result, token, "invalid integer for " + token.command + ": '" + token.argument + "'");
						return false;
					}
					return true;
				case ArgumentKind::RegisteredExtension:
					// The owning registration validates its private schema below.
					return true;
			}
			return false;
		}
	}

	bool ParseResult::success() const
	{
		return std::none_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& diagnostic) {
			return diagnostic.severity == Severity::Error;
		});
	}

	ParseResult parseTextSourceScript(const std::string& script)
	{
		ParseResult result;
		if ( script.empty() )
		{
			result.diagnostics.push_back({ Severity::Error, 1, 1, "script is empty" });
			return result;
		}

		size_t cursor = 0;
		while ( (cursor = script.find('@', cursor)) != std::string::npos )
		{
			const size_t start = cursor;
			++cursor;
			while ( cursor < script.size()
				&& script[cursor] != ' '
				&& script[cursor] != '@'
				&& script[cursor] != '\0' )
			{
				++cursor;
			}

			Token token;
			token.raw = script.substr(start, cursor - start);
			locate(script, start, token.line, token.column);
			std::string normalized = token.raw;
			if ( startsWith(normalized, "@attached.") )
			{
				token.attached = true;
				normalized.erase(1, std::string("attached.").size());
			}

			const auto& definitions = scriptTagDefinitions();
			const ScriptTagDefinition* definition = nullptr;
			std::size_t definitionIndex = 0;
			std::string matchedSpelling;
			for ( ; definitionIndex < definitions.size(); ++definitionIndex )
			{
				const auto& candidate = definitions[definitionIndex];
				if ( !candidate.patternFamily
					&& matchScriptTagPrefix(normalized, candidate, matchedSpelling) )
				{
					definition = &definitions[definitionIndex];
					break;
				}
			}

			if ( definition )
			{
				token.command = matchedSpelling;
				token.argument = normalized.substr(token.command.size());
				token.canonicalTag = definition->canonicalTag;
				token.catalogIndex = definitionIndex;
			}
			else if ( dynamicStatCommand(normalized, token.command,
				token.argument, token.canonicalTag) )
			{
				for ( definitionIndex = 0; definitionIndex < definitions.size(); ++definitionIndex )
				{
					if ( token.canonicalTag == definitions[definitionIndex].canonicalTag )
					{
						definition = &definitions[definitionIndex];
						token.catalogIndex = definitionIndex;
						break;
					}
				}
			}
			if ( !definition )
			{
				token.command = normalized;
				token.valid = false;
				addError(result, token, "unknown Text Source command '" + normalized + "'");
				result.tokens.push_back(std::move(token));
				continue;
			}

			token.safety = definition->execution.safety;
			token.testModeReason = definition->execution.safety == CommandSafety::GameplayMutation
				? definition->description : definition->defaultBehavior;
			if ( definition->availability == ScriptAvailability::UnavailableMissingMod
				|| definition->availability == ScriptAvailability::UnsupportedHistorical )
			{
				token.valid = false;
				addError(result, token, "unavailable Text Source command '"
					+ token.command + "': " + definition->availabilityDetail);
				result.tokens.push_back(std::move(token));
				continue;
			}
			token.valid = validateArgument(result, token, *definition);
			if ( token.valid && !isNativeScriptOrigin(definition->origin.kind)
				&& definition->extensionValidator )
			{
				std::string validationError;
				if ( !validateScriptTagExtensionArgument(token, validationError) )
				{
					token.valid = false;
					addError(result, token, "invalid extension argument for "
						+ token.command + ": " + validationError);
				}
			}
			if ( definition->availability == ScriptAvailability::NeedsVerification )
			{
				addWarning(result, token, token.command
					+ " is registered and routable but still needs verification: "
					+ definition->availabilityDetail);
			}
			if ( token.attached
				&& definition->execution.attachment == AttachmentMode::Unsupported )
			{
				token.valid = false;
				addError(result, token,
					"@attached. is not supported for map-generation directive "
					+ token.command);
			}
			else if ( token.attached
				&& definition->execution.attachment == AttachmentMode::NotMeaningful )
			{
				addWarning(result, token,
					"@attached. has no target-specific meaning for "
					+ token.command + "; current dispatch ignores the prefix");
			}
			else if ( !token.attached
				&& definition->execution.attachment == AttachmentMode::Required )
			{
				addWarning(result, token,
					token.command + " requires @attached. for the current handler to mutate a target");
			}
			result.tokens.push_back(std::move(token));
		}

		if ( result.tokens.empty() )
		{
			result.diagnostics.push_back({ Severity::Error, 1, 1,
				"no Text Source @commands were found" });
		}
		else if ( std::none_of(result.tokens.begin(), result.tokens.end(),
			[](const Token& token) { return token.command == "@script"; }) )
		{
			result.diagnostics.push_back({ Severity::Warning, 1, 1,
				"no @script marker; a newly authored Text Source remains in message mode" });
		}
		return result;
	}

	const ScriptTagDefinition* definitionForToken(const Token& token)
	{
		const auto& definitions = scriptTagDefinitions();
		if ( token.catalogIndex >= definitions.size() )
		{
			return nullptr;
		}
		const auto& definition = definitions[token.catalogIndex];
		return token.canonicalTag == definition.canonicalTag ? &definition : nullptr;
	}

	bool scriptTagMatchesSearch(
		const ScriptTagDefinition& definition, const std::string& query)
	{
		if ( query.empty() )
		{
			return true;
		}
		auto lower = [](std::string text) {
			std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
				return static_cast<char>(std::tolower(c));
			});
			return text;
		};
		const std::string needle = lower(query);
		std::string haystack;
		haystack.reserve(512);
		haystack.append(definition.canonicalTag).append(" ")
			.append(definition.syntax).append(" ")
			.append(definition.aliases).append(" ")
			.append(definition.category).append(" ")
			.append(scriptOriginKindName(definition.origin.kind)).append(" ")
			.append(definition.origin.displayName).append(" ")
			.append(definition.origin.stableSourceId).append(" ")
			.append(definition.origin.sourceVersion).append(" ")
			.append(scriptAvailabilityName(definition.availability)).append(" ")
			.append(definition.availabilityDetail).append(" ")
			.append(definition.execution.targetTypes).append(" ")
			.append(definition.execution.scope).append(" ")
			.append(definition.description).append(" ")
			.append(definition.caveats);
		return lower(std::move(haystack)).find(needle) != std::string::npos;
	}

	bool appendScriptSnippet(
		std::string& script, const std::string& snippet,
		std::size_t maximumSize, std::string& error)
	{
		error.clear();
		if ( snippet.empty() )
		{
			error = "cannot insert an empty script snippet";
			return false;
		}
		const bool needsSpace = !script.empty()
			&& !std::isspace(static_cast<unsigned char>(script.back()));
		const std::size_t required = script.size() + (needsSpace ? 1U : 0U)
			+ snippet.size();
		if ( required > maximumSize )
		{
			error = "insertion would exceed the tester script buffer";
			return false;
		}
		if ( needsSpace )
		{
			script.push_back(' ');
		}
		script += snippet;
		return true;
	}

	bool parseSetVariableArgument(
		const std::string& argument, std::string& name, int& value)
	{
		const size_t comma = argument.find(',');
		if ( comma == std::string::npos
			|| argument.find(',', comma + 1) != std::string::npos )
		{
			return false;
		}
		name = argument.substr(0, comma);
		return validVariableName(name)
			&& parseInteger(argument.substr(comma + 1), value);
	}

	ResourceResult resolveTextSourceResource(
		const std::string& source, const ResourceLookup& lookup)
	{
		ResourceResult result;
		if ( source.empty() || source.front() != '$' )
		{
			result.error = "Text Source resource references must begin with '$'";
			return result;
		}

		result.isReference = true;
		for ( size_t i = 0; i < source.size(); ++i )
		{
			if ( resourceWordSeparator(source[i]) )
			{
				break;
			}
			if ( source[i] != '$' )
			{
				result.key += source[i];
			}
		}
		if ( result.key.empty() )
		{
			result.error = "Text Source resource key is empty";
			return result;
		}
		if ( !lookup )
		{
			result.error = "Text Source resource lookup is unavailable";
			return result;
		}
		result.found = lookup(result.key, result.script);
		if ( !result.found )
		{
			result.error = "Text Source resource '$" + result.key + "' was not found";
		}
		return result;
	}

	const char* severityName(Severity severity)
	{
		switch ( severity )
		{
			case Severity::Info: return "INFO";
			case Severity::Warning: return "WARNING";
			case Severity::Error: return "ERROR";
		}
		return "UNKNOWN";
	}

	const char* safetyName(CommandSafety safety)
	{
		switch ( safety )
		{
			case CommandSafety::Directive: return "DIRECTIVE";
			case CommandSafety::DisposableState: return "DISPOSABLE";
			case CommandSafety::GameplayMutation: return "BLOCKED";
		}
		return "UNKNOWN";
	}

	const char* attachmentModeName(AttachmentMode mode)
	{
		switch ( mode )
		{
			case AttachmentMode::Unsupported: return "Unsupported";
			case AttachmentMode::NotMeaningful: return "Not meaningful";
			case AttachmentMode::Optional: return "Optional";
			case AttachmentMode::Required: return "Required";
		}
		return "Unknown";
	}

	const char* verificationLevelName(VerificationLevel level)
	{
		switch ( level )
		{
			case VerificationLevel::ParserVerified: return "Parser Verified";
			case VerificationLevel::DispatchVerified: return "Dispatch Verified";
			case VerificationLevel::BehaviorVerified: return "Behavior Verified";
			case VerificationLevel::ManualGameTestVerified: return "Manual Game Test Verified";
		}
		return "Unknown";
	}

	void TestSession::setScript(std::string script)
	{
		scriptText = std::move(script);
		lastParse = {};
	}

	const std::string& TestSession::script() const
	{
		return scriptText;
	}

	const ParseResult& TestSession::validate()
	{
		lastParse = parseTextSourceScript(scriptText);
		return lastParse;
	}

	TestResult TestSession::run()
	{
		const ParseResult& parsed = validate();
		TestResult result;
		result.diagnostics = parsed.diagnostics;
		if ( !parsed.success() )
		{
			result.variables = testVariables;
			return result;
		}
		if ( std::none_of(parsed.tokens.begin(), parsed.tokens.end(),
			[](const Token& token) {
				return token.valid && token.canonicalTag == "@script";
			}) )
		{
			result.diagnostics.push_back({ Severity::Error, 1, 1,
				"sandbox run blocked: without a valid @script marker, gameplay keeps the Text Source in message mode" });
			result.variables = testVariables;
			return result;
		}

		std::ostringstream context;
		context << "test context: player=" << testContext.activatorPlayer
			<< " source=(" << testContext.sourceX << ',' << testContext.sourceY << ')'
			<< " floor=" << testContext.playableFloor
			<< " authoredLayer=" << testContext.authoredMapLayer
			<< " localZ=" << testContext.localZ
			<< " revision=" << testContext.spatialRevision
			<< " mapContext=" << (testContext.mapContext.empty() ? "(none)" : testContext.mapContext)
			<< " mapInstance=" << (testContext.mapInstance.empty() ? "(none)" : testContext.mapInstance);
		result.diagnostics.push_back({ Severity::Info, 1, 1, context.str() });

		for ( const Token& token : parsed.tokens )
		{
			if ( token.safety == CommandSafety::GameplayMutation )
			{
				result.blockedCommands = true;
				result.diagnostics.push_back({ Severity::Warning, token.line, token.column,
					"unsafe-in-test-mode " + token.command + " blocked: " + token.testModeReason });
				continue;
			}
			if ( token.safety == CommandSafety::Directive )
			{
				result.diagnostics.push_back({ Severity::Info, token.line, token.column,
					token.command + " " + token.testModeReason });
				continue;
			}

			std::string name;
			int value = 0;
			parseSetVariableArgument(token.argument, name, value);
			testVariables[name] = value;
			result.diagnostics.push_back({ Severity::Info, token.line, token.column,
				"@setvar wrote disposable " + name + "=" + std::to_string(value) });
		}

		result.success = true;
		result.variables = testVariables;
		return result;
	}

	void TestSession::reset(const TestContext& context)
	{
		testContext = context;
		testVariables.clear();
		lastParse = {};
	}

	const TestContext& TestSession::context() const
	{
		return testContext;
	}

	const std::map<std::string, int>& TestSession::variables() const
	{
		return testVariables;
	}
}
