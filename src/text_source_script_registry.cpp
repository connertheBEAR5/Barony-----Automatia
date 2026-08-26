/*-------------------------------------------------------------------------------

	BARONY / AUTOMATIA
	Unified Text Source @script registry.

	Native definitions remain data-only in text_source_script_catalog.cpp. This
	file owns extension metadata deeply so S.A.M or a future S.A.M mod adapter can
	register through the exact registry consumed by parsing, runtime dispatch, the
	Zed library, and tests. No S.A.M implementation header is included here.

-------------------------------------------------------------------------------*/

#include "text_source_script.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <memory>
#include <set>
#include <utility>

namespace TextSourceLanguage
{
	namespace
	{
		const char* safeText(const char* text)
		{
			return text ? text : "";
		}

		bool startsWith(const std::string& value, const std::string& prefix)
		{
			return value.compare(0, prefix.size(), prefix) == 0;
		}

		std::vector<std::string> aliasesFor(const ScriptTagDefinition& definition)
		{
			std::vector<std::string> aliases;
			const std::string text = safeText(definition.aliases);
			std::size_t cursor = 0;
			while ( cursor < text.size() )
			{
				while ( cursor < text.size()
					&& (std::isspace(static_cast<unsigned char>(text[cursor]))
						|| text[cursor] == ',' || text[cursor] == ';') )
				{
					++cursor;
				}
				const std::size_t begin = cursor;
				while ( cursor < text.size()
					&& !std::isspace(static_cast<unsigned char>(text[cursor]))
					&& text[cursor] != ',' && text[cursor] != ';' )
				{
					++cursor;
				}
				if ( cursor > begin )
				{
					aliases.push_back(text.substr(begin, cursor - begin));
				}
			}
			return aliases;
		}

		std::vector<std::string> spellingsFor(const ScriptTagDefinition& definition)
		{
			std::vector<std::string> spellings;
			spellings.emplace_back(safeText(definition.canonicalTag));
			const auto aliases = aliasesFor(definition);
			spellings.insert(spellings.end(), aliases.begin(), aliases.end());
			return spellings;
		}

		bool spellingIsValid(const std::string& spelling)
		{
			return spelling.size() > 1 && spelling.front() == '@'
				&& std::none_of(spelling.begin(), spelling.end(), [](unsigned char c) {
					return std::isspace(c) != 0;
				});
		}

		bool spellingsOverlap(const std::string& left, const std::string& right)
		{
			// The parser is prefix-based. Rejecting both exact duplicates and prefix
			// ambiguity makes precedence explicit instead of depending on load order.
			return startsWith(left, right) || startsWith(right, left);
		}

		bool collidesWithNativePattern(const std::string& spelling)
		{
			static const char* stats[] = { "STR", "DEX", "INT", "CON", "PER", "CHR" };
			for ( const char* stat : stats )
			{
				for ( const char operation : { '=', '+' } )
				{
					const std::string concrete = std::string("@st") + stat + operation;
					if ( spellingsOverlap(spelling, concrete) )
					{
						return true;
					}
				}
			}
			for ( int proficiency = 0; proficiency < 16; ++proficiency )
			{
				for ( const char operation : { '=', '+' } )
				{
					const std::string concrete = "@pro" + std::to_string(proficiency) + operation;
					if ( spellingsOverlap(spelling, concrete) )
					{
						return true;
					}
				}
			}
			return false;
		}

		bool availabilityIsExecutable(ScriptAvailability availability)
		{
			return availability == ScriptAvailability::Available
				|| availability == ScriptAvailability::NeedsVerification;
		}

		struct OwnedScriptTagDefinition
		{
			std::string canonicalTag;
			std::string syntax;
			std::string insertionTemplate;
			std::string category;
			std::string aliases;
			std::string valueCount;
			std::string separators;
			std::string acceptedDomain;
			std::string targetTypes;
			std::string scope;
			std::string defaultBehavior;
			std::string description;
			std::string minimalExample;
			std::string invalidExample;
			std::string caveats;
			std::string sourceLocation;
			std::string verificationMethod;
			std::string dispatchEvidence;
			std::string originDisplayName;
			std::string stableSourceId;
			std::string sourceVersion;
			std::string availabilityDetail;
			ScriptTagDefinition view;

			explicit OwnedScriptTagDefinition(const ScriptTagDefinition& source)
			{
				assign(source);
			}

			void assign(const ScriptTagDefinition& source)
			{
				canonicalTag = safeText(source.canonicalTag);
				syntax = safeText(source.syntax);
				insertionTemplate = safeText(source.insertionTemplate);
				category = safeText(source.category);
				aliases = safeText(source.aliases);
				valueCount = safeText(source.value.valueCount);
				separators = safeText(source.value.separators);
				acceptedDomain = safeText(source.value.acceptedDomain);
				targetTypes = safeText(source.execution.targetTypes);
				scope = safeText(source.execution.scope);
				defaultBehavior = safeText(source.defaultBehavior);
				description = safeText(source.description);
				minimalExample = safeText(source.minimalExample);
				invalidExample = safeText(source.invalidExample);
				caveats = safeText(source.caveats);
				sourceLocation = safeText(source.sourceLocation);
				verificationMethod = safeText(source.verification.method);
				dispatchEvidence = safeText(source.verification.dispatchEvidence);
				originDisplayName = safeText(source.origin.displayName);
				stableSourceId = safeText(source.origin.stableSourceId);
				sourceVersion = safeText(source.origin.sourceVersion);
				availabilityDetail = safeText(source.availabilityDetail);

				view = source;
				refreshPointers();
			}

			void refreshPointers()
			{
				view.canonicalTag = canonicalTag.c_str();
				view.syntax = syntax.c_str();
				view.insertionTemplate = insertionTemplate.c_str();
				view.category = category.c_str();
				view.aliases = aliases.c_str();
				view.value.valueCount = valueCount.c_str();
				view.value.separators = separators.c_str();
				view.value.acceptedDomain = acceptedDomain.c_str();
				view.execution.targetTypes = targetTypes.c_str();
				view.execution.scope = scope.c_str();
				view.defaultBehavior = defaultBehavior.c_str();
				view.description = description.c_str();
				view.minimalExample = minimalExample.c_str();
				view.invalidExample = invalidExample.c_str();
				view.caveats = caveats.c_str();
				view.sourceLocation = sourceLocation.c_str();
				view.verification.method = verificationMethod.c_str();
				view.verification.dispatchEvidence = dispatchEvidence.c_str();
				view.origin.displayName = originDisplayName.c_str();
				view.origin.stableSourceId = stableSourceId.c_str();
				view.origin.sourceVersion = sourceVersion.c_str();
				view.availabilityDetail = availabilityDetail.c_str();
			}

			void markUnavailable(const std::string& detail)
			{
				view.availability = ScriptAvailability::UnavailableMissingMod;
				view.extensionValidator = nullptr;
				view.extensionDispatch = nullptr;
				availabilityDetail = detail;
				refreshPointers();
			}
		};

		std::vector<std::unique_ptr<OwnedScriptTagDefinition>> extensionDefinitions;
		std::vector<ScriptTagDefinition> unifiedDefinitions;
		std::uint64_t registryGeneration = 1;
		std::uint64_t cachedGeneration = 0;
		int registryCallbackDepth = 0;

		struct RegistryCallbackScope
		{
			RegistryCallbackScope() { ++registryCallbackDepth; }
			~RegistryCallbackScope() { --registryCallbackDepth; }
		};

		void noteRegistryMutation()
		{
			++registryGeneration;
			cachedGeneration = 0;
		}

		void sortExtensionDefinitions()
		{
			std::sort(extensionDefinitions.begin(), extensionDefinitions.end(),
				[](const auto& left, const auto& right) {
					if ( left->stableSourceId != right->stableSourceId )
					{
						return left->stableSourceId < right->stableSourceId;
					}
					return left->canonicalTag < right->canonicalTag;
				});
		}

		void rebuildUnifiedDefinitions()
		{
			if ( cachedGeneration == registryGeneration )
			{
				return;
			}
			unifiedDefinitions = nativeScriptTagDefinitions();
			unifiedDefinitions.reserve(
				unifiedDefinitions.size() + extensionDefinitions.size());
			for ( const auto& extension : extensionDefinitions )
			{
				extension->refreshPointers();
				unifiedDefinitions.push_back(extension->view);
			}
			cachedGeneration = registryGeneration;
		}

		ScriptRegistrationResult registrationFailure(
			ScriptRegistrationStatus status, std::string message)
		{
			return { status, std::move(message) };
		}
	}

	bool ScriptRegistrationResult::success() const
	{
		return status == ScriptRegistrationStatus::Registered
			|| status == ScriptRegistrationStatus::Restored;
	}

	const std::vector<ScriptTagDefinition>& scriptTagDefinitions()
	{
		rebuildUnifiedDefinitions();
		return unifiedDefinitions;
	}

	bool matchScriptTagPrefix(const std::string& token,
		const ScriptTagDefinition& definition, std::string& matchedSpelling)
	{
		matchedSpelling.clear();
		for ( const auto& spelling : spellingsFor(definition) )
		{
			if ( !spelling.empty() && startsWith(token, spelling)
				&& spelling.size() > matchedSpelling.size() )
			{
				matchedSpelling = spelling;
			}
		}
		return !matchedSpelling.empty();
	}

	ScriptRegistrationResult registerScriptTagExtension(
		const ScriptTagDefinition& definition)
	{
		if ( registryCallbackDepth > 0 )
		{
			return registrationFailure(ScriptRegistrationStatus::InvalidDefinition,
				"registry mutation is not permitted from an extension validator or dispatch callback");
		}
		const std::string canonical = safeText(definition.canonicalTag);
		const std::string sourceId = safeText(definition.origin.stableSourceId);
		if ( isNativeScriptOrigin(definition.origin.kind) )
		{
			return registrationFailure(ScriptRegistrationStatus::NativeOriginRejected,
				"extension registration requires S.A.M. or S.A.M.-mod origin metadata");
		}
		if ( definition.patternFamily || !spellingIsValid(canonical)
			|| sourceId.empty() || std::string(safeText(definition.origin.displayName)).empty()
			|| std::string(safeText(definition.syntax)).empty()
			|| std::string(safeText(definition.description)).empty() )
		{
			return registrationFailure(ScriptRegistrationStatus::InvalidDefinition,
				"extension definition requires an exact @command spelling, source identity, syntax, and description");
		}
		if ( availabilityIsExecutable(definition.availability)
			&& !definition.extensionDispatch )
		{
			return registrationFailure(ScriptRegistrationStatus::MissingDispatch,
				"available extension command '" + canonical + "' has no runtime dispatch callback");
		}
		if ( definition.value.kind == ArgumentKind::RegisteredExtension
			&& !definition.extensionValidator )
		{
			return registrationFailure(ScriptRegistrationStatus::InvalidDefinition,
				"custom argument schema for '" + canonical + "' has no validation callback");
		}

		const auto candidateSpellings = spellingsFor(definition);
		std::set<std::string> uniqueCandidateSpellings;
		for ( std::size_t index = 0; index < candidateSpellings.size(); ++index )
		{
			const std::string& spelling = candidateSpellings[index];
			if ( !spellingIsValid(spelling)
				|| !uniqueCandidateSpellings.insert(spelling).second )
			{
				return registrationFailure(index == 0
					? ScriptRegistrationStatus::InvalidDefinition
					: ScriptRegistrationStatus::AliasCollision,
					"invalid or duplicate spelling '" + spelling + "' in extension definition");
			}
		}

		OwnedScriptTagDefinition* restorable = nullptr;
		for ( const auto& extension : extensionDefinitions )
		{
			if ( extension->canonicalTag == canonical
				&& extension->stableSourceId == sourceId
				&& extension->view.availability == ScriptAvailability::UnavailableMissingMod )
			{
				restorable = extension.get();
				break;
			}
		}

		auto checkCollision = [&](const ScriptTagDefinition& existing,
			bool existingIsRestorable) -> ScriptRegistrationResult {
			if ( existingIsRestorable )
			{
				return { ScriptRegistrationStatus::Registered, "" };
			}
			const auto existingSpellings = spellingsFor(existing);
			for ( std::size_t candidateIndex = 0;
				candidateIndex < candidateSpellings.size(); ++candidateIndex )
			{
				for ( const auto& existingSpelling : existingSpellings )
				{
					if ( spellingsOverlap(candidateSpellings[candidateIndex], existingSpelling) )
					{
						return registrationFailure(candidateIndex == 0
							? ScriptRegistrationStatus::CommandCollision
							: ScriptRegistrationStatus::AliasCollision,
							"spelling '" + candidateSpellings[candidateIndex]
							+ "' conflicts with registered spelling '" + existingSpelling + "'");
					}
				}
			}
			return { ScriptRegistrationStatus::Registered, "" };
		};

		for ( const auto& native : nativeScriptTagDefinitions() )
		{
			const auto collision = checkCollision(native, false);
			if ( !collision.success() )
			{
				return collision;
			}
		}
		for ( const auto& spelling : candidateSpellings )
		{
			if ( collidesWithNativePattern(spelling) )
			{
				return registrationFailure(ScriptRegistrationStatus::CommandCollision,
					"spelling '" + spelling + "' conflicts with a native generated command family");
			}
		}
		for ( const auto& extension : extensionDefinitions )
		{
			const auto collision = checkCollision(extension->view,
				extension.get() == restorable);
			if ( !collision.success() )
			{
				return collision;
			}
		}

		if ( restorable )
		{
			restorable->assign(definition);
			noteRegistryMutation();
			return { ScriptRegistrationStatus::Restored,
				"restored extension command '" + canonical + "' for " + sourceId };
		}

		extensionDefinitions.emplace_back(
			std::make_unique<OwnedScriptTagDefinition>(definition));
		sortExtensionDefinitions();
		noteRegistryMutation();
		return { ScriptRegistrationStatus::Registered,
			"registered extension command '" + canonical + "' for " + sourceId };
	}

	bool markScriptTagSourceUnavailable(
		const std::string& stableSourceId, const std::string& detail)
	{
		if ( registryCallbackDepth > 0 )
		{
			return false;
		}
		bool changed = false;
		for ( auto& extension : extensionDefinitions )
		{
			if ( extension->stableSourceId == stableSourceId )
			{
				extension->markUnavailable(detail.empty()
					? "Unavailable - required S.A.M. mod is not currently loaded"
					: detail);
				changed = true;
			}
		}
		if ( changed )
		{
			noteRegistryMutation();
		}
		return changed;
	}

	std::size_t removeScriptTagSource(const std::string& stableSourceId)
	{
		if ( registryCallbackDepth > 0 )
		{
			return 0;
		}
		const std::size_t before = extensionDefinitions.size();
		extensionDefinitions.erase(std::remove_if(extensionDefinitions.begin(),
			extensionDefinitions.end(), [&](const auto& extension) {
				return extension->stableSourceId == stableSourceId;
			}), extensionDefinitions.end());
		const std::size_t removed = before - extensionDefinitions.size();
		if ( removed > 0 )
		{
			noteRegistryMutation();
		}
		return removed;
	}

	std::uint64_t scriptTagRegistryGeneration()
	{
		return registryGeneration;
	}

	std::size_t scriptTagCountForOrigin(ScriptOriginKind origin)
	{
		const auto& definitions = scriptTagDefinitions();
		return static_cast<std::size_t>(std::count_if(definitions.begin(),
			definitions.end(), [&](const ScriptTagDefinition& definition) {
				return definition.origin.kind == origin;
			}));
	}

	bool validateScriptTagExtensionArgument(const Token& token, std::string& error)
	{
		error.clear();
		const ScriptTagDefinition* definition = definitionForToken(token);
		if ( !definition || isNativeScriptOrigin(definition->origin.kind) )
		{
			error = "token does not resolve to a registered extension";
			return false;
		}
		if ( !definition->extensionValidator )
		{
			error = "registered extension has no argument validator";
			return false;
		}
		RegistryCallbackScope callbackScope;
		try
		{
			const bool valid = definition->extensionValidator(token, error);
			if ( !valid && error.empty() )
			{
				error = "extension validator rejected the argument";
			}
			return valid;
		}
		catch ( const std::exception& exception )
		{
			error = std::string("extension validator threw: ") + exception.what();
		}
		catch ( ... )
		{
			error = "extension validator threw an unknown exception";
		}
		return false;
	}

	bool dispatchScriptTagExtension(const Token& token, void* sourceContext,
		ScriptDispatchPhase phase, std::string& error)
	{
		error.clear();
		const ScriptTagDefinition* definition = definitionForToken(token);
		if ( !definition )
		{
			error = "extension token no longer resolves in the current registry";
			return false;
		}
		if ( isNativeScriptOrigin(definition->origin.kind) )
		{
			error = "native command must use the native Text Source dispatcher";
			return false;
		}
		if ( !availabilityIsExecutable(definition->availability) )
		{
			error = safeText(definition->availabilityDetail);
			return false;
		}
		if ( definition->execution.dispatchPhase != phase )
		{
			error = "command is registered for "
				+ std::string(scriptDispatchPhaseName(definition->execution.dispatchPhase))
				+ ", not " + scriptDispatchPhaseName(phase);
			return false;
		}
		if ( !definition->extensionDispatch )
		{
			error = "registered extension has no live dispatch callback";
			return false;
		}
		RegistryCallbackScope callbackScope;
		try
		{
			const bool dispatched = definition->extensionDispatch(
				sourceContext, token, error);
			if ( !dispatched && error.empty() )
			{
				error = "extension callback rejected dispatch";
			}
			return dispatched;
		}
		catch ( const std::exception& exception )
		{
			error = std::string("extension dispatch threw: ") + exception.what();
		}
		catch ( ... )
		{
			error = "extension dispatch threw an unknown exception";
		}
		return false;
	}

	const char* scriptOriginKindName(ScriptOriginKind origin)
	{
		switch ( origin )
		{
			case ScriptOriginKind::NativeBaronyAutomatia: return "Native Barony/Automatia";
			case ScriptOriginKind::Barony: return "Barony";
			case ScriptOriginKind::Automatia: return "Automatia";
			case ScriptOriginKind::SAMFramework: return "S.A.M. Framework";
			case ScriptOriginKind::SAMMod: return "Installed S.A.M. Mod";
		}
		return "Unknown";
	}

	const char* scriptAvailabilityName(ScriptAvailability availability)
	{
		switch ( availability )
		{
			case ScriptAvailability::Available: return "Available";
			case ScriptAvailability::UnavailableMissingMod: return "Unavailable - Missing Mod";
			case ScriptAvailability::NeedsVerification: return "Available - Needs Verification";
			case ScriptAvailability::UnsupportedHistorical: return "Unsupported / Historical";
		}
		return "Unknown";
	}

	const char* scriptDispatchPhaseName(ScriptDispatchPhase phase)
	{
		switch ( phase )
		{
			case ScriptDispatchPhase::MapGeneration: return "map-generation registration";
			case ScriptDispatchPhase::RuntimeExecution: return "runtime execution";
		}
		return "unknown phase";
	}

	bool isNativeScriptOrigin(ScriptOriginKind origin)
	{
		return origin == ScriptOriginKind::NativeBaronyAutomatia
			|| origin == ScriptOriginKind::Barony
			|| origin == ScriptOriginKind::Automatia;
	}
}
