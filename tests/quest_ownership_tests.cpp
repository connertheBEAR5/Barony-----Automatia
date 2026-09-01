#include "automatia_save.hpp"
#include "party_manager.hpp"
#include "party_persistence.hpp"
#include "quest_ownership.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
bool expect(const bool condition, const char* expression, const int line)
{
    if (!condition)
    {
        std::cerr << "FAILED line " << line << ": " << expression << '\n';
    }
    return condition;
}

#define EXPECT(expression) \
    do { if (!expect(static_cast<bool>(expression), #expression, __LINE__)) return false; } while (false)

AutomatiaParty::DurablePlayerIdentity local(const char* name)
{
    return {
        AutomatiaParty::DurableIdentityKind::LocalName,
        AutomatiaParty::normalizeLocalCharacterIdentity(name)
    };
}

AutomatiaQuest::Definition definition(
    const std::string& questId,
    const AutomatiaQuest::Scope scope,
    const int schemaVersion =
        AutomatiaQuest::SHARED_OWNERSHIP_DIALOGUE_SCHEMA_VERSION,
    const bool repeatable = false)
{
    AutomatiaQuest::Definition result;
    result.questId = questId;
    result.dialogueId = questId + "_dialogue";
    result.title = questId;
    result.summary = "summary";
    result.dialogueSchemaVersion = schemaVersion;
    result.authoredScope = scope;
    result.repeatable = repeatable;
    return result;
}

AutomatiaParty::PartyID join(
    AutomatiaParty::PartyManager& manager,
    const AutomatiaParty::DurablePlayerIdentity& leader,
    const AutomatiaParty::DurablePlayerIdentity& member,
    const std::uint64_t tick)
{
    using namespace AutomatiaParty;
    if (manager.partyIdForPlayer(leader) == INVALID_PARTY_ID)
    {
        if (!manager.createParty(leader))
        {
            return INVALID_PARTY_ID;
        }
    }
    const OperationResult invitation = manager.invitePlayer(
        leader, member, tick, 1000, MAX_PERSISTENT_PARTY_MEMBERS);
    if (!invitation
        || !manager.acceptInvitation(
            member, invitation.invitationId, tick + 1,
            MAX_PERSISTENT_PARTY_MEMBERS))
    {
        return INVALID_PARTY_ID;
    }
    return manager.partyIdForPlayer(leader);
}

struct TestQuestState
{
    int stage = 0;
    bool completed = false;
};

class TestRuntime
{
public:
    AutomatiaParty::PartyManager parties;
    std::unordered_map<std::string, TestQuestState> states;

    AutomatiaQuest::Resolution resolve(
        const AutomatiaParty::DurablePlayerIdentity& actor,
        const AutomatiaQuest::Definition& quest) const
    {
        return AutomatiaQuest::resolveAuthoritative(quest, actor, parties);
    }

    bool setStage(
        const AutomatiaParty::DurablePlayerIdentity& actor,
        const AutomatiaQuest::Definition& quest,
        const int stage)
    {
        const AutomatiaQuest::Resolution owner = resolve(actor, quest);
        if (!owner.allowed)
        {
            return false;
        }
        states[owner.stateKey].stage = stage;
        return true;
    }

    std::optional<int> stage(
        const AutomatiaParty::DurablePlayerIdentity& actor,
        const AutomatiaQuest::Definition& quest) const
    {
        const AutomatiaQuest::Resolution owner = resolve(actor, quest);
        if (!owner.allowed)
        {
            return std::nullopt;
        }
        const auto found = states.find(owner.stateKey);
        return found == states.end()
            ? std::optional<int>{} : std::optional<int>{found->second.stage};
    }

    bool reset(
        const AutomatiaParty::DurablePlayerIdentity& actor,
        const AutomatiaQuest::Definition& quest)
    {
        if (!quest.repeatable)
        {
            return false;
        }
        const AutomatiaQuest::Resolution owner = resolve(actor, quest);
        return owner.allowed && states.erase(owner.stateKey) == 1;
    }

    std::vector<std::string> projection(
        const AutomatiaParty::DurablePlayerIdentity& recipient) const
    {
        std::vector<std::string> result;
        AutomatiaQuest::Owner personal;
        personal.scope = AutomatiaQuest::Scope::Player;
        personal.player = recipient;
        AutomatiaQuest::Owner party;
        party.scope = AutomatiaQuest::Scope::Party;
        party.partyId = parties.partyIdForPlayer(recipient);
        AutomatiaQuest::Owner world;
        world.scope = AutomatiaQuest::Scope::World;
        for (const auto& entry : states)
        {
            if (AutomatiaQuest::stateKeyBelongsToOwner(entry.first, personal)
                || AutomatiaQuest::stateKeyBelongsToOwner(entry.first, party)
                || AutomatiaQuest::stateKeyBelongsToOwner(entry.first, world))
            {
                result.push_back(entry.first);
            }
        }
        std::sort(result.begin(), result.end());
        return result;
    }
};

bool testPlayerPartyMembershipAndRevocation()
{
    using namespace AutomatiaQuest;
    TestRuntime runtime;
    const auto alice = local("Alice");
    const auto bob = local("Bob");
    const auto carol = local("Carol");
    const auto dave = local("Dave");
    const Definition personal = definition("personal_quest", Scope::Player);
    const Definition partyQuest = definition("party_quest", Scope::Party);

    // 1. Personal state remains private and independent.
    EXPECT(runtime.setStage(alice, personal, 2));
    EXPECT(runtime.setStage(bob, personal, 9));
    EXPECT(runtime.stage(alice, personal) == 2);
    EXPECT(runtime.stage(bob, personal) == 9);
    EXPECT(runtime.resolve(alice, personal).stateKey
        != runtime.resolve(bob, personal).stateKey);

    // 2-4. One durable PartyID owns one shared state; a non-member cannot
    // address it, while a newly joined member immediately resolves it.
    const AutomatiaParty::PartyID partyId =
        join(runtime.parties, alice, bob, 10);
    EXPECT(partyId != AutomatiaParty::INVALID_PARTY_ID);
    EXPECT(runtime.setStage(alice, partyQuest, 4));
    EXPECT(runtime.stage(bob, partyQuest) == 4);
    EXPECT(runtime.resolve(alice, partyQuest).stateKey
        == runtime.resolve(bob, partyQuest).stateKey);
    EXPECT(!runtime.stage(carol, partyQuest).has_value());
    EXPECT(!runtime.setStage(carol, partyQuest, 99));
    EXPECT(join(runtime.parties, alice, carol, 20) == partyId);
    EXPECT(runtime.stage(carol, partyQuest) == 4);

    // 5. Leave and kick both revoke access without copying shared progress.
    EXPECT(runtime.parties.leaveParty(carol));
    EXPECT(!runtime.stage(carol, partyQuest).has_value());
    EXPECT(runtime.stage(alice, partyQuest) == 4);
    EXPECT(join(runtime.parties, alice, dave, 30) == partyId);
    EXPECT(runtime.stage(dave, partyQuest) == 4);
    EXPECT(runtime.parties.kickMember(alice, dave));
    EXPECT(!runtime.stage(dave, partyQuest).has_value());
    EXPECT(runtime.states.size() == 3); // two Personal + exactly one Party
    return true;
}

bool testPartyPersistenceRestartAndNoIdLeak()
{
    using namespace AutomatiaQuest;
    using AutomatiaSave::Json;
    TestRuntime runtime;
    const auto alice = local("Alice");
    const auto bob = local("Bob");
    const Definition partyQuest = definition("restart_party", Scope::Party);
    const AutomatiaParty::PartyID oldPartyId =
        join(runtime.parties, alice, bob, 100);
    EXPECT(oldPartyId != AutomatiaParty::INVALID_PARTY_ID);
    EXPECT(runtime.setStage(alice, partyQuest, 7));

    Json saved = AutomatiaSave::makeEmptyWorldSave("quest-scope-test");
    saved["party"] = AutomatiaParty::PartyPersistence::toPersistentJson(
        runtime.parties);
    saved["quests"]["parties"][std::to_string(oldPartyId)]
        [partyQuest.questId] = Json{
            {"started", true}, {"accepted", true},
            {"completed", false}, {"failed", false},
            {"stage", 7}, {"variables", Json::object()},
            {"flags", Json::array()},
            {"completed_objectives", Json::array()},
            {"used_choices", Json::array()}
        };
    EXPECT(AutomatiaSave::validate(saved).ok);

    const auto unique =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path()
        / ("automatia-quest-ownership-" + std::to_string(unique));
    const std::filesystem::path path = directory / "world.json";
    EXPECT(AutomatiaSave::writeAtomic(path, saved).ok);
    Json loaded;
    EXPECT(AutomatiaSave::load(path, loaded).ok);

    TestRuntime restored;
    std::string error;
    EXPECT(AutomatiaParty::PartyPersistence::loadPersistentJson(
        restored.parties, loaded["party"], error));
    EXPECT(restored.parties.partyIdForPlayer(alice) == oldPartyId);
    EXPECT(restored.parties.partyIdForPlayer(bob) == oldPartyId);
    EXPECT(loaded["quests"]["parties"][std::to_string(oldPartyId)]
        [partyQuest.questId]["stage"] == 7);
    const Resolution restoredOwner = restored.resolve(alice, partyQuest);
    EXPECT(restoredOwner.allowed);
    restored.states[restoredOwner.stateKey].stage = 7;
    EXPECT(restored.stage(bob, partyQuest) == 7);

    // 7. Dissolution archives the old key; monotonic PartyID allocation means
    // an unrelated future party can never inherit it.
    EXPECT(restored.parties.disbandParty(alice));
    EXPECT(restored.parties.createParty(alice));
    const AutomatiaParty::PartyID newPartyId =
        restored.parties.partyIdForPlayer(alice);
    EXPECT(newPartyId != oldPartyId);
    EXPECT(newPartyId > oldPartyId);
    EXPECT(!restored.stage(alice, partyQuest).has_value());
    EXPECT(restored.states.count(restoredOwner.stateKey) == 1);

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    return true;
}

bool testWorldSpatialIndependenceAndReferencedScopes()
{
    using namespace AutomatiaQuest;
    TestRuntime runtime;
    const auto alice = local("Alice");
    const auto bob = local("Bob");
    EXPECT(join(runtime.parties, alice, bob, 200)
        != AutomatiaParty::INVALID_PARTY_ID);

    const Definition worldQuest = definition("world_quest", Scope::World);
    EXPECT(runtime.setStage(alice, worldQuest, 12));
    EXPECT(runtime.stage(bob, worldQuest) == 12);

    // 9-10. Map instance, floor, layer, X/Y and Entity::z cannot influence
    // ownership because none exists in the resolver input.
    struct SpatialNoise
    {
        std::string mapInstance;
        int playableFloor = 0;
        int authoredLayer = 0;
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };
    const SpatialNoise first{"mine#west", 0, 1, 16.0, 16.0, -2.0};
    const SpatialNoise second{"mine#east", 9, 7, 16.0, 16.0, 88.0};
    (void)first;
    (void)second;
    EXPECT(runtime.resolve(alice, worldQuest).stateKey
        == runtime.resolve(bob, worldQuest).stateKey);

    // 11. A referenced quest resolves its own definition, not the current
    // dialogue quest's scope.
    const Definition current = definition("current_personal", Scope::Player);
    const Definition referencedParty =
        definition("referenced_party", Scope::Party);
    const Definition referencedWorld =
        definition("referenced_world", Scope::World);
    EXPECT(runtime.resolve(alice, current).owner.scope == Scope::Player);
    EXPECT(runtime.resolve(alice, referencedParty).owner.scope == Scope::Party);
    EXPECT(runtime.resolve(alice, referencedWorld).owner.scope == Scope::World);
    EXPECT(runtime.setStage(bob, referencedParty, 5));
    EXPECT(runtime.stage(alice, referencedParty) == 5);
    return true;
}

bool testRepeatResetLegacyRegistryAndLateJoinProjection()
{
    using namespace AutomatiaQuest;
    TestRuntime runtime;
    const auto alice = local("Alice");
    const auto bob = local("Bob");
    const auto carol = local("Carol");
    EXPECT(join(runtime.parties, alice, bob, 300)
        != AutomatiaParty::INVALID_PARTY_ID);

    const Definition personal = definition(
        "repeat_personal", Scope::Player,
        SHARED_OWNERSHIP_DIALOGUE_SCHEMA_VERSION, true);
    const Definition party = definition(
        "repeat_party", Scope::Party,
        SHARED_OWNERSHIP_DIALOGUE_SCHEMA_VERSION, true);
    const Definition world = definition(
        "repeat_world", Scope::World,
        SHARED_OWNERSHIP_DIALOGUE_SCHEMA_VERSION, true);
    EXPECT(runtime.setStage(alice, personal, 1));
    EXPECT(runtime.setStage(alice, party, 2));
    EXPECT(runtime.setStage(alice, world, 3));
    EXPECT(runtime.reset(alice, personal));
    EXPECT(!runtime.stage(alice, personal).has_value());
    EXPECT(runtime.stage(bob, party) == 2);
    EXPECT(runtime.stage(bob, world) == 3);
    EXPECT(runtime.reset(bob, party));
    EXPECT(!runtime.stage(alice, party).has_value());
    EXPECT(runtime.stage(alice, world) == 3);
    EXPECT(runtime.reset(alice, world));
    EXPECT(!runtime.stage(bob, world).has_value());

    // 14. Authored Party/World in schema 1 remain independent Personal state.
    const Definition legacyParty = definition(
        "legacy_party", Scope::Party, LEGACY_DIALOGUE_SCHEMA_VERSION);
    EXPECT(legacyParty.effectiveScope() == Scope::Player);
    EXPECT(runtime.setStage(alice, legacyParty, 4));
    EXPECT(runtime.setStage(bob, legacyParty, 8));
    EXPECT(runtime.stage(alice, legacyParty) == 4);
    EXPECT(runtime.stage(bob, legacyParty) == 8);
    EXPECT(runtime.resolve(alice, legacyParty).stateKey
        != runtime.resolve(bob, legacyParty).stateKey);

    DefinitionRegistry registry;
    std::string error;
    Definition registered = definition("registry_quest", Scope::World);
    EXPECT(registry.registerDefinition(registered, error));
    EXPECT(registry.findByQuestId("registry_quest") != nullptr);
    EXPECT(registry.findByDialogueId(registered.dialogueId) != nullptr);
    Definition conflicting = registered;
    conflicting.dialogueId = "other_dialogue";
    conflicting.title = "Conflicting immutable title";
    EXPECT(!registry.registerDefinition(conflicting, error));

    // 13. A late/reconnecting member's projection is exactly Personal + its
    // current Party + World. A non-member projection cannot contain PartyID.
    EXPECT(runtime.setStage(bob, personal, 10));
    EXPECT(runtime.setStage(bob, party, 20));
    EXPECT(runtime.setStage(bob, world, 30));
    const auto bobProjection = runtime.projection(bob);
    EXPECT(bobProjection.size() == 4);
    EXPECT(std::find(bobProjection.begin(), bobProjection.end(),
        runtime.resolve(bob, personal).stateKey) != bobProjection.end());
    EXPECT(std::find(bobProjection.begin(), bobProjection.end(),
        runtime.resolve(bob, party).stateKey) != bobProjection.end());
    EXPECT(std::find(bobProjection.begin(), bobProjection.end(),
        runtime.resolve(bob, world).stateKey) != bobProjection.end());
    EXPECT(runtime.setStage(carol, personal, 40));
    EXPECT(runtime.setStage(carol, world, 50));
    const auto carolProjection = runtime.projection(carol);
    EXPECT(carolProjection.size() == 2);
    const std::string partyPrefix = "party:";
    EXPECT(std::none_of(carolProjection.begin(), carolProjection.end(),
        [&](const std::string& key)
        {
            return key.rfind(partyPrefix, 0) == 0;
        }));
    return true;
}

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return input ? output.str() : std::string{};
}

bool testAuthorityRewardAndAcceptanceContracts()
{
    using namespace AutomatiaQuest;
    TestRuntime runtime;
    const auto alice = local("Alice");
    const auto bob = local("Bob");
    EXPECT(join(runtime.parties, alice, bob, 400)
        != AutomatiaParty::INVALID_PARTY_ID);
    const Definition shared = definition("reward_party", Scope::Party);

    // 16. Shared ownership advances one state, but the action actor alone is
    // rewarded. Ownership does not enumerate or retarget Party members.
    std::unordered_map<std::string, int> gold;
    EXPECT(runtime.setStage(alice, shared, 1));
    gold[alice.value] += 100;
    EXPECT(runtime.stage(bob, shared) == 1);
    EXPECT(gold[alice.value] == 100);
    EXPECT(gold[bob.value] == 0);

    // 17. Unauthenticated mutation has no owner. The authoritative overload
    // derives PartyID from PartyManager instead of accepting a client claim.
    ActorContext unauthenticated;
    unauthenticated.player = alice;
    unauthenticated.partyId = runtime.parties.partyIdForPlayer(alice);
    unauthenticated.authenticated = false;
    EXPECT(!resolve(shared, unauthenticated).allowed);
    const Resolution authoritative = resolveAuthoritative(
        shared, alice, runtime.parties);
    EXPECT(authoritative.allowed);
    EXPECT(authoritative.owner.partyId
        == runtime.parties.partyIdForPlayer(alice));

    const std::filesystem::path root(BARONY_SOURCE_DIR);
    const std::string game = readFile(root / "src/game.cpp");
    const std::string net = readFile(root / "src/net.cpp");
    const std::string dialogue = readFile(root / "src/actmonster.cpp");
    const std::string gameUI = readFile(root / "src/ui/GameUI.cpp");
    EXPECT(game.find("resolveAuthoritative") != std::string::npos);
    EXPECT(game.find("if ( multiplayer == CLIENT ) return false;")
        != std::string::npos);
    EXPECT(net.find("exportAutomatiaQuestRecipientState")
        != std::string::npos);
    EXPECT(net.find("serverSyncAutomatiaQuestStateForActor")
        != std::string::npos);
    EXPECT(dialogue.find("stats[player]->GOLD +=\n\t\t\tchoice.rewardGold")
        != std::string::npos);
    EXPECT(gameUI.find("ownershipName(entry.scope)")
        != std::string::npos);

    // 15. Existing character saves retain their version-1 reader and migrate
    // slot-prefixed quest state lazily onto the durable player identity.
    EXPECT(game.find("document[\"version\"].get<int>() != 1")
        != std::string::npos);
    EXPECT(game.find("makeLegacyPersistentPlayerQuestKey")
        != std::string::npos);
    EXPECT(game.find("migrateLegacyPlayerState")
        != std::string::npos);

    // 18. Exact two-client acceptance instructions are a checked artifact.
    const std::string manual = readFile(
        root / "docs/custom_dialogue_shared_quest_two_client_test.md");
    EXPECT(!manual.empty());
    EXPECT(manual.find("Client A") != std::string::npos);
    EXPECT(manual.find("Client B") != std::string::npos);
    EXPECT(manual.find("Party") != std::string::npos);
    EXPECT(manual.find("World") != std::string::npos);
    EXPECT(manual.find("leave") != std::string::npos);
    return true;
}
}

int main()
{
    const bool passed = testPlayerPartyMembershipAndRevocation()
        && testPartyPersistenceRestartAndNoIdLeak()
        && testWorldSpatialIndependenceAndReferencedScopes()
        && testRepeatResetLegacyRegistryAndLateJoinProjection()
        && testAuthorityRewardAndAcceptanceContracts();
    if (passed)
    {
        std::cout << "Quest ownership tests passed.\n";
        return 0;
    }
    return 1;
}
