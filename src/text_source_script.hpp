/*-------------------------------------------------------------------------------

	BARONY / AUTOMATIA
	Shared Text Source script front-end and disposable editor test session.

	The gameplay dispatcher remains TextSourceScript::handleTextSourceScript(). Both
	gameplay and Zed call parseTextSourceScript(), so the tester cannot drift into a
	second grammar or a second interpreter.

-------------------------------------------------------------------------------*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace TextSourceLanguage
{
	enum class Severity
	{
		Info,
		Warning,
		Error
	};

	enum class CommandSafety
	{
		Directive,
		DisposableState,
		GameplayMutation
	};

	// These contracts describe the grammar the legacy gameplay dispatcher
	// actually consumes.  They intentionally live in this lightweight shared
	// header so Zed, the parser, and characterization tests use one catalog.
	enum class ArgumentKind
	{
		None,
		Integer,
		MapRange,
		EffectAndDuration,
		EffectRange,
		Explosion,
		SetVariable,
		AttachmentTarget,
		TriggerCondition,
		MonsterName,
		StatPattern,
		ProficiencyPattern,
		RegisteredExtension
	};

	enum class AttachmentMode
	{
		Unsupported,
		NotMeaningful,
		Optional,
		Required
	};

	enum class VerificationLevel
	{
		ParserVerified,
		DispatchVerified,
		BehaviorVerified,
		ManualGameTestVerified
	};

	// Origin and availability are independent axes.  NativeBaronyAutomatia is
	// intentionally conservative: the current source proves that a command is
	// native to this build, but does not always prove whether it first shipped in
	// upstream Barony or in Automatia.
	enum class ScriptOriginKind
	{
		NativeBaronyAutomatia,
		Barony,
		Automatia,
		SAMFramework,
		SAMMod
	};

	enum class ScriptAvailability
	{
		Available,
		UnavailableMissingMod,
		NeedsVerification,
		UnsupportedHistorical
	};

	enum class ScriptDispatchPhase
	{
		MapGeneration,
		RuntimeExecution
	};

	struct ValueContract
	{
		ArgumentKind kind = ArgumentKind::None;
		bool required = false;
		const char* valueCount = "none";
		const char* separators = "none";
		const char* acceptedDomain = "none";
	};

	struct ExecutionContract
	{
		AttachmentMode attachment = AttachmentMode::NotMeaningful;
		const char* targetTypes = "global";
		const char* scope = "global";
		bool mapRangeSupported = false;
		bool allSupported = false;
		bool changesPersistentState = false;
		bool serverAuthoritative = true;
		CommandSafety safety = CommandSafety::GameplayMutation;
		ScriptDispatchPhase dispatchPhase = ScriptDispatchPhase::RuntimeExecution;
	};

	struct VerificationEvidence
	{
		VerificationLevel level = VerificationLevel::ParserVerified;
		bool manualBehaviorRequired = true;
		const char* method = "parser characterization";
		// A deliberately precise source fragment used by the consistency test.
		// It is evidence, not an alternate dispatcher.
		const char* dispatchEvidence = "";
	};

	struct ScriptOrigin
	{
		ScriptOriginKind kind = ScriptOriginKind::NativeBaronyAutomatia;
		const char* displayName = "Native Barony/Automatia";
		const char* stableSourceId = "native.barony-automatia";
		const char* sourceVersion = "";
	};

	struct Token;
	using ScriptExtensionValidator = bool (*)(
		const Token& token, std::string& error);
	using ScriptExtensionDispatch = bool (*)(
		void* sourceContext, const Token& token, std::string& error);

	struct ScriptTagDefinition
	{
		const char* canonicalTag = "";
		const char* syntax = "";
		const char* insertionTemplate = "";
		const char* category = "Miscellaneous";
		// Exact accepted spellings separated by whitespace, comma, or semicolon.
		const char* aliases = "";
		bool patternFamily = false;
		ValueContract value;
		ExecutionContract execution;
		const char* defaultBehavior = "";
		const char* description = "";
		const char* minimalExample = "";
		const char* invalidExample = "";
		const char* caveats = "";
		const char* sourceLocation = "";
		VerificationEvidence verification;
		ScriptOrigin origin;
		ScriptAvailability availability = ScriptAvailability::Available;
		const char* availabilityDetail = "Available in the current runtime";
		// Optional for shared ArgumentKind contracts; required for
		// ArgumentKind::RegisteredExtension.
		ScriptExtensionValidator extensionValidator = nullptr;
		// Native commands continue through the legacy C++ dispatcher.  Registered
		// extensions must provide this callback when executable (Available or
		// NeedsVerification).
		ScriptExtensionDispatch extensionDispatch = nullptr;
	};

	enum class ScriptRegistrationStatus
	{
		Registered,
		Restored,
		InvalidDefinition,
		MissingDispatch,
		NativeOriginRejected,
		CommandCollision,
		AliasCollision
	};

	struct ScriptRegistrationResult
	{
		ScriptRegistrationStatus status = ScriptRegistrationStatus::InvalidDefinition;
		std::string message;

		bool success() const;
	};

	struct ScriptRecipeDefinition
	{
		const char* name = "";
		const char* category = "";
		const char* script = "";
		const char* description = "";
		const char* caveats = "";
		VerificationLevel verification = VerificationLevel::ParserVerified;
		bool manualBehaviorRequired = true;
		const char* verificationMethod = "";
	};

	struct Diagnostic
	{
		Severity severity = Severity::Info;
		int line = 1;
		int column = 1;
		std::string message;
	};

	struct Token
	{
		std::string raw;
		std::string command;
		std::string argument;
		std::string canonicalTag;
		std::size_t catalogIndex = static_cast<std::size_t>(-1);
		int line = 1;
		int column = 1;
		bool attached = false;
		bool valid = true;
		CommandSafety safety = CommandSafety::GameplayMutation;
		std::string testModeReason;
	};

	struct ParseResult
	{
		std::vector<Token> tokens;
		std::vector<Diagnostic> diagnostics;

		bool success() const;
	};

	struct ResourceResult
	{
		bool isReference = false;
		bool found = false;
		std::string key;
		std::string script;
		std::string error;
	};

	using ResourceLookup = std::function<bool(
		const std::string& key, std::string& script)>;

	struct TestContext
	{
		int activatorPlayer = -1;
		int sourceX = 0;
		int sourceY = 0;
		int playableFloor = 0;
		int authoredMapLayer = 0;
		double localZ = 0.0;
		std::uint64_t spatialRevision = 0;
		std::string mapContext;
		std::string mapInstance;
	};

	struct TestResult
	{
		bool success = false;
		bool blockedCommands = false;
		std::vector<Diagnostic> diagnostics;
		std::map<std::string, int> variables;
	};

	ParseResult parseTextSourceScript(const std::string& script);
	// The unified view is the authoritative native catalog plus live/retained
	// extension registrations. Registration and unload are main-thread lifecycle
	// operations; callers must not retain element pointers across a mutation.
	const std::vector<ScriptTagDefinition>& scriptTagDefinitions();
	const std::vector<ScriptTagDefinition>& nativeScriptTagDefinitions();
	const std::vector<ScriptRecipeDefinition>& scriptRecipeDefinitions();
	ScriptRegistrationResult registerScriptTagExtension(
		const ScriptTagDefinition& definition);
	bool markScriptTagSourceUnavailable(
		const std::string& stableSourceId, const std::string& detail);
	std::size_t removeScriptTagSource(const std::string& stableSourceId);
	std::uint64_t scriptTagRegistryGeneration();
	std::size_t scriptTagCountForOrigin(ScriptOriginKind origin);
	bool matchScriptTagPrefix(const std::string& token,
		const ScriptTagDefinition& definition, std::string& matchedSpelling);
	bool validateScriptTagExtensionArgument(
		const Token& token, std::string& error);
	bool dispatchScriptTagExtension(
		const Token& token, void* sourceContext, ScriptDispatchPhase phase,
		std::string& error);
	const ScriptTagDefinition* definitionForToken(const Token& token);
	bool scriptTagMatchesSearch(
		const ScriptTagDefinition& definition, const std::string& query);
	bool appendScriptSnippet(
		std::string& script, const std::string& snippet,
		std::size_t maximumSize, std::string& error);
	bool parseSetVariableArgument(
		const std::string& argument, std::string& name, int& value);
	ResourceResult resolveTextSourceResource(
		const std::string& source, const ResourceLookup& lookup);
	const char* severityName(Severity severity);
	const char* safetyName(CommandSafety safety);
	const char* attachmentModeName(AttachmentMode mode);
	const char* verificationLevelName(VerificationLevel level);
	const char* scriptOriginKindName(ScriptOriginKind origin);
	const char* scriptAvailabilityName(ScriptAvailability availability);
	const char* scriptDispatchPhaseName(ScriptDispatchPhase phase);
	bool isNativeScriptOrigin(ScriptOriginKind origin);

	class TestSession
	{
	public:
		void setScript(std::string script);
		const std::string& script() const;
		const ParseResult& validate();
		TestResult run();
		void reset(const TestContext& context);
		const TestContext& context() const;
		const std::map<std::string, int>& variables() const;

	private:
		std::string scriptText;
		ParseResult lastParse;
		TestContext testContext;
		std::map<std::string, int> testVariables;
	};
}
