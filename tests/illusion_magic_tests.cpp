#include "magic/illusion_magic_model.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <string>
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

std::string readSource(const char* relative)
{
	std::ifstream input(std::filesystem::path(BARONY_SOURCE_DIR) / relative,
		std::ios::binary);
	std::ostringstream contents;
	contents << input.rdbuf();
	return input ? contents.str() : std::string{};
}

bool contains(const std::string& source, const char* token)
{
	return source.find(token) != std::string::npos;
}

bool appearsInOrder(const std::string& source,
	const std::initializer_list<const char*> tokens)
{
	std::size_t position = 0;
	for (const char* token : tokens)
	{
		position = source.find(token, position);
		if (position == std::string::npos)
		{
			return false;
		}
		position += std::char_traits<char>::length(token);
	}
	return true;
}

bool testDefinitionsAndStableIds()
{
	using namespace IllusionMagic;
	const auto& spells = definitions();
	const std::array<const char*, kSpellCount> expectedNames = {{
		"Mirror Other", "Mirror Copy", "Mirror Wall", "Mirror Reflect",
		"Mirror Mimic", "Mirror Reflect Loot", "Phantasm Path",
		"Mirror Duplicate Loot", "Mirage Wall", "Paranoia",
		"Vertical Mirage", "Shadow Step", "Store Magic"
	}};
	const std::array<int, kSpellCount> expectedMana = {{
		8, 14, 8, 24, 20, 18, 20, 100, 10, 25, 12, 12, 12
	}};
	const std::array<int, kSpellCount> expectedDifficulty = {{
		0, 40, 20, 60, 60, 60, 60, 100, 20, 60, 40, 40, 80
	}};
	EXPECT(spells.size() == 13);
	EXPECT(spells.front().id == 225);
	EXPECT(spells.back().id == 237);
	std::set<int> ids;
	std::set<std::string> names;
	for (std::size_t i = 0; i < spells.size(); ++i)
	{
		EXPECT(spells[i].id == kFirstSpellId + static_cast<int>(i));
		EXPECT(std::string(spells[i].displayName) == expectedNames[i]);
		EXPECT(spells[i].mana == expectedMana[i]);
		EXPECT(spells[i].difficulty == expectedDifficulty[i]);
		EXPECT(spells[i].mana > 0);
		EXPECT(spells[i].difficulty >= 0);
		EXPECT(ids.insert(spells[i].id).second);
		EXPECT(names.insert(spells[i].internalName).second);
		EXPECT(spellbookOffset(spells[i].id) == static_cast<int>(i));
	}
	EXPECT(definition(224) == nullptr);
	EXPECT(definition(238) == nullptr);
	EXPECT(spellbookOffset(224) == -1);
	EXPECT(spellbookOffset(238) == -1);
	EXPECT(effectiveProficiency(12, 45, 30) == 45);
	EXPECT(effectiveProficiency(-1, 140, 30) == 100);
	return true;
}

bool testTierAndVaultRules()
{
	using namespace IllusionMagic;
	EXPECT(proficiencyTier(0) == ProficiencyTier::Novice);
	EXPECT(proficiencyTier(20) == ProficiencyTier::Basic);
	EXPECT(proficiencyTier(40) == ProficiencyTier::Skilled);
	EXPECT(proficiencyTier(60) == ProficiencyTier::Expert);
	EXPECT(proficiencyTier(80) == ProficiencyTier::Master);
	EXPECT(proficiencyTier(100) == ProficiencyTier::Legendary);
	EXPECT(vaultCapacity(0) == 1);
	EXPECT(vaultCapacity(19) == 1);
	EXPECT(vaultCapacity(20) == 2);
	EXPECT(vaultCapacity(39) == 2);
	EXPECT(vaultCapacity(40) == 3);
	EXPECT(vaultCapacity(59) == 3);
	EXPECT(vaultCapacity(60) == 4);
	EXPECT(vaultCapacity(79) == 4);
	EXPECT(vaultCapacity(80) == 5);
	EXPECT(vaultCapacity(100) == 5);
	EXPECT(!vaultAllowsOldestNewest(39));
	EXPECT(vaultAllowsOldestNewest(40));
	EXPECT(!vaultAllowsExactSelection(59));
	EXPECT(vaultAllowsExactSelection(60));
	EXPECT(!vaultAllowsStudy(79));
	EXPECT(vaultAllowsStudy(80));

	StoredMagicVault vault;
	const StoredSpellSnapshot first{10, 20, 12, 50, 100, 0, true};
	const StoredSpellSnapshot second{11, 40, 20, 70, 200, 0, true};
	const StoredSpellSnapshot third{12, 60, 30, 40, 300, 0, false};
	EXPECT(vault.capture(first, 60));
	EXPECT(vault.capture(second, 60));
	EXPECT(vault.capture(third, 60));
	EXPECT(!vault.full(60));
	StoredRelease released;
	EXPECT(vault.release(VaultSelection::Exact, 1, 0, 60, 30, released));
	EXPECT(released.snapshot == second);
	EXPECT(released.releaseMana == storedReleaseMana(40, 70, 60));
	EXPECT(released.releasePower < 30);
	EXPECT(vault.size() == 2);
	EXPECT(!vault.discard(8));
	EXPECT(vault.discard(0));
	EXPECT(vault.size() == 1);

	StoredMagicVault novice;
	EXPECT(novice.capture(first, 0));
	EXPECT(novice.full(0));
	EXPECT(!novice.capture(second, 0));
	EXPECT(novice.release(VaultSelection::Exact, 99, 3, 0, 100, released));
	EXPECT(released.snapshot == first);

	StoredMagicVault randomVault;
	EXPECT(randomVault.capture(first, 20));
	EXPECT(randomVault.capture(second, 20));
	StoredMagicVault randomCopy = randomVault;
	EXPECT(randomVault.release(VaultSelection::Exact, 0, 1, 20,
		std::numeric_limits<int>::max(), released));
	EXPECT(released.snapshot == second);
	EXPECT(randomCopy.release(VaultSelection::Newest, 0, 0, 20,
		std::numeric_limits<int>::max(), released));
	EXPECT(released.snapshot == first);

	StoredMagicVault oldestNewest;
	EXPECT(oldestNewest.capture(first, 40));
	EXPECT(oldestNewest.capture(second, 40));
	EXPECT(oldestNewest.capture(third, 40));
	StoredMagicVault newest = oldestNewest;
	EXPECT(oldestNewest.release(VaultSelection::Exact, 2, 2, 40,
		std::numeric_limits<int>::max(), released));
	EXPECT(released.snapshot == first);
	EXPECT(newest.release(VaultSelection::Newest, 0, 0, 40,
		std::numeric_limits<int>::max(), released));
	EXPECT(released.snapshot == third);

	StoredMagicVault exact;
	EXPECT(exact.capture(first, 60));
	EXPECT(exact.capture(second, 60));
	EXPECT(exact.capture(third, 60));
	EXPECT(exact.release(VaultSelection::Exact, 99, 0, 60,
		std::numeric_limits<int>::max(), released));
	EXPECT(released.snapshot == third);

	StoredMagicVault maximum;
	for (int index = 0; index < 5; ++index)
	{
		StoredSpellSnapshot snapshot = first;
		snapshot.spellId += index;
		EXPECT(maximum.capture(snapshot, 80));
	}
	EXPECT(maximum.full(80));
	EXPECT(!maximum.capture(third, 80));

	StoredMagicVault study;
	EXPECT(study.capture(first, 80));
	study.accrueStudy(5U * 60U * 50U - 1U, 80);
	EXPECT(!study.readyToLearn(0, 80));
	study.accrueStudy(1, 80);
	EXPECT(study.readyToLearn(0, 80));
	StoredMagicVault noStudy;
	EXPECT(noStudy.capture(first, 60));
	noStudy.accrueStudy(5U * 60U * 50U, 60);
	EXPECT(!noStudy.readyToLearn(0, 60));
	StoredMagicVault unlearnable;
	EXPECT(unlearnable.capture(third, 80));
	unlearnable.accrueStudy(5U * 60U * 50U, 80);
	EXPECT(!unlearnable.readyToLearn(0, 80));
	return true;
}

bool testStoredReleaseScaling()
{
	using namespace IllusionMagic;
	EXPECT(storedReleaseMana(0, 0, 0) == 1);
	EXPECT(storedReleaseMana(1, 0, 0) == 1);
	EXPECT(storedReleaseMana(4, 60, 60) == 1);
	EXPECT(storedReleaseMana(5, 60, 60) == 2);
	EXPECT(storedReleaseMana(40, 60, 60) == 10);
	EXPECT(storedReleaseMana(40, 60, 40) == 11);
	EXPECT(storedReleaseMana(40, 61, 40) == 12);
	EXPECT(storedReleasePower(100, 60, 40, 1000) == 90);
	EXPECT(storedReleasePower(100, 100, 0, 1000) == 50);
	EXPECT(storedReleasePower(100, 60, 100, 1000) == 110);
	EXPECT(storedReleasePower(100, 0, 100, 1000) == 115);
	EXPECT(storedReleasePower(100, 0, 100, 80) == 79);
	EXPECT(storedReleasePower(0, 60, 60, 80) == 1);
	return true;
}

bool testCaptureAndDuplicationSafety()
{
	using namespace IllusionMagic;
	EXPECT(storeMagicEligible(10, true, true));
	EXPECT(!storeMagicEligible(10, false, true));
	EXPECT(!storeMagicEligible(198, true, false));
	EXPECT(!storeMagicEligible(MIRROR_REFLECT, true, true));
	EXPECT(!storeMagicEligible(MIRROR_DUPLICATE_LOOT, true, true));
	EXPECT(!storeMagicEligible(STORE_MAGIC, true, true));
	EXPECT(!storeMagicEligible(26, true, true));
	EXPECT(!storeMagicEligible(155, true, true));
	ItemDuplicationSafety safe;
	EXPECT(mayCreateRealDuplicate(safe));
	safe.questItem = true;
	EXPECT(!mayCreateRealDuplicate(safe));
	safe = {};
	safe.container = true;
	EXPECT(!mayCreateRealDuplicate(safe));
	EXPECT(mirroredDuplicateBeatitude(3) == -3);
	EXPECT(mirroredDuplicateBeatitude(0) == -1);
	EXPECT(mirroredDuplicateBeatitude(-2) == -2);
	return true;
}

bool testPlayableZOverlayIsolation()
{
	using namespace IllusionMagic;
	OverlayRegistry overlays;
	NavigationOverlay bridge;
	bridge.kind = OverlayKind::PhantasmPath;
	bridge.scope = {"world-a/map-1", 2};
	bridge.ownerUid = 10;
	bridge.authorizedActorUids = {11};
	bridge.expiresTick = 500;
	bridge.tiles = {{7, 9}, {8, 9}};
	EXPECT(overlays.add(bridge));
	EXPECT(overlays.supportsActor({"world-a/map-1", 2}, 7, 9, 10));
	EXPECT(overlays.supportsActor({"world-a/map-1", 2}, 7, 9, 11));
	EXPECT(!overlays.supportsActor({"world-a/map-1", 2}, 7, 9, 12));
	EXPECT(!overlays.supportsActor({"world-a/map-1", 3}, 7, 9, 10));
	EXPECT(!overlays.supportsActor({"world-a/map-2", 2}, 7, 9, 10));
	EXPECT(!overlays.supportsActor({"world-a/map-1", 2}, 8, 8, 10));

	NavigationOverlay wall;
	wall.kind = OverlayKind::FakeWall;
	wall.scope = {"world-a/map-1", 2};
	wall.ownerUid = 10;
	wall.expiresTick = 300;
	wall.tiles = {{4, 4}};
	EXPECT(overlays.add(wall));
	EXPECT(overlays.hostileAvoids({"world-a/map-1", 2}, 4, 4));
	EXPECT(!overlays.hidesWall({"world-a/map-1", 2}, 4, 4));

	NavigationOverlay hiddenWall;
	hiddenWall.kind = OverlayKind::HiddenWall;
	hiddenWall.scope = {"world-a/map-1", 2};
	hiddenWall.ownerUid = 10;
	hiddenWall.expiresTick = 400;
	hiddenWall.tiles = {{5, 4}};
	EXPECT(overlays.add(hiddenWall));
	EXPECT(overlays.hidesWall({"world-a/map-1", 2}, 5, 4));
	EXPECT(!overlays.hidesWall({"world-a/map-1", 3}, 5, 4));

	NavigationOverlay pit;
	pit.kind = OverlayKind::FakePit;
	pit.scope = {"world-a/map-1", 2};
	pit.ownerUid = 10;
	pit.expiresTick = 400;
	pit.tiles = {{6, 4}};
	EXPECT(overlays.add(pit));
	EXPECT(overlays.hidesFloor({"world-a/map-1", 2}, 6, 4));
	EXPECT(overlays.hostileAvoids({"world-a/map-1", 2}, 6, 4));
	EXPECT(!overlays.hostileAvoids({"world-a/map-1", 1}, 4, 4));
	overlays.expire(300);
	EXPECT(!overlays.hostileAvoids({"world-a/map-1", 2}, 4, 4));
	EXPECT(overlays.size() == 3);

	NavigationOverlay update = bridge;
	update.id = bridge.id = overlays.entries().front().id;
	update.tiles = {{9, 9}};
	EXPECT(overlays.add(update));
	EXPECT(overlays.size() == 3);
	EXPECT(overlays.supportsActor({"world-a/map-1", 2}, 9, 9, 10));

	NavigationOverlay invalid;
	invalid.scope = {"world-a/map-1", 2};
	EXPECT(!overlays.add(invalid));
	invalid.tiles = {{1, 1}};
	invalid.scope.mapInstance.clear();
	EXPECT(!overlays.add(invalid));

	OverlayRegistry bounded;
	NavigationOverlay oversized;
	oversized.scope = {"world-a/map-1", 2};
	oversized.ownerUid = 99;
	for (int index = 0; index < 12; ++index)
	{
		oversized.tiles.push_back({index, 1});
	}
	for (std::uint32_t uid = 1; uid <= 40; ++uid)
	{
		oversized.authorizedActorUids.push_back(uid);
	}
	EXPECT(bounded.add(oversized));
	EXPECT(bounded.entries().front().tiles.size()
		== OverlayRegistry::kMaximumTilesPerOverlay);
	EXPECT(bounded.entries().front().authorizedActorUids.size()
		== OverlayRegistry::kMaximumAuthorizedActors);
	for (std::size_t index = 1;
		index < OverlayRegistry::kMaximumOverlays; ++index)
	{
		NavigationOverlay entry;
		entry.scope = {"world-a/map-1", 2};
		entry.tiles = {{static_cast<int>(index), 2}};
		EXPECT(bounded.add(entry));
	}
	EXPECT(bounded.size() == OverlayRegistry::kMaximumOverlays);
	NavigationOverlay oneTooMany;
	oneTooMany.scope = {"world-a/map-1", 2};
	oneTooMany.tiles = {{1, 3}};
	EXPECT(!bounded.add(oneTooMany));
	return true;
}

bool testRuntimeAndSpellbookContracts()
{
	const std::string magic = readSource("src/magic/magic.hpp");
	const std::string items = readSource("src/items.hpp");
	const std::string itemNames = readSource("src/entity_shared.cpp");
	const std::string itemLoader = readSource("src/mod_tools.cpp");
	const std::string itemUse = readSource("src/items.cpp");
	const std::string game = readSource("src/game.cpp");
	const std::string net = readSource("src/net.hpp");
	const std::string netRuntime = readSource("src/net.cpp");
	const std::string casting = readSource("src/magic/castSpell.cpp");
	const std::string setup = readSource("src/magic/setupSpells.cpp");
	const std::string runtime = readSource("src/magic/illusion_magic.cpp");
	const std::string defenses = readSource("src/magic/actmagic.cpp");
	const std::string inventory = readSource("src/interface/playerinventory.cpp");
	const std::string gameUi = readSource("src/ui/GameUI.cpp");
	const std::string itemPicker = readSource("src/interface/interface.cpp");
	const std::string itemUsage = readSource("src/item_usage_funcs.cpp");
	const std::string defaultFlags = readSource("src/interface/interface.cpp");
	const std::string mainMenu = readSource("src/ui/MainMenu.cpp");
	const std::string handMagic = readSource("src/magic/act_HandMagic.cpp");
	const std::string player = readSource("src/actplayer.cpp");
	EXPECT(contains(magic, "SPELL_MIRROR_OTHER = 225"));
	EXPECT(contains(magic, "SPELL_STORE_MAGIC = 237"));
	EXPECT(contains(magic, "SPELL_HOLY_BEAM = 224"));
	EXPECT(contains(magic, "NUM_SPELLS = 238"));
	EXPECT(contains(items, "SPELLBOOK_MIRROR_OTHER"));
	EXPECT(contains(items, "SPELLBOOK_STORE_MAGIC"));
	for (const char* spellbookName : {
		"spellbook_mirror_other", "spellbook_mirror_copy",
		"spellbook_mirror_wall", "spellbook_mirror_reflect",
		"spellbook_mirror_mimic", "spellbook_mirror_reflect_loot",
		"spellbook_phantasm_path", "spellbook_mirror_duplicate_loot",
		"spellbook_mirage_wall", "spellbook_paranoia",
		"spellbook_vertical_mirage", "spellbook_shadow_step",
		"spellbook_store_magic"
	})
	{
		EXPECT(contains(itemNames, spellbookName));
	}
	for (const char* spellbookCase : {
		"case SPELLBOOK_MIRROR_OTHER:", "case SPELLBOOK_MIRROR_COPY:",
		"case SPELLBOOK_MIRROR_WALL:", "case SPELLBOOK_MIRROR_REFLECT:",
		"case SPELLBOOK_MIRROR_MIMIC:", "case SPELLBOOK_MIRROR_REFLECT_LOOT:",
		"case SPELLBOOK_PHANTASM_PATH:",
		"case SPELLBOOK_MIRROR_DUPLICATE_LOOT:",
		"case SPELLBOOK_MIRAGE_WALL:", "case SPELLBOOK_PARANOIA:",
		"case SPELLBOOK_VERTICAL_MIRAGE:", "case SPELLBOOK_SHADOW_STEP:",
		"case SPELLBOOK_STORE_MAGIC:"
	})
	{
		EXPECT(contains(itemUse, spellbookCase));
	}
	EXPECT(contains(itemLoader, "initializeIllusionSpellbookItemDefinitions"));
	EXPECT(contains(itemLoader, "spellbook_spell"));
	EXPECT(contains(itemLoader, "An unidentified spellbook must not reveal"));
	EXPECT(contains(itemLoader, "if ( item.identified )"));
	EXPECT(contains(game, "--automatian-mode"));
	EXPECT(contains(mainMenu, "automatian_mode_enabled"));
	EXPECT(contains(mainMenu, "\"Automatian Mode\""));
	EXPECT(contains(mainMenu, "propertyVersion(\"automatian_mode_enabled\""));
	EXPECT(contains(mainMenu, "SV_FLAG_AUTOMATIAN_MODE"));
	EXPECT(contains(defaultFlags, "Uint32 svFlags = 30;"));
	EXPECT(contains(net, "SV_FLAG_AUTOMATIAN_MODE = 1 << 11"));
	EXPECT(contains(casting, "IllusionMagic::cast"));
	EXPECT(contains(casting, "!IllusionMagic::enabled()"));
	EXPECT(contains(casting, "getEffectiveSpellcastingAbility"));
	EXPECT(contains(setup, "IllusionMagic::definitions()"));
	EXPECT(contains(itemUsage, "Illusion Magic is disabled"));
	EXPECT(contains(inventory, "matchesIllusion"));
	EXPECT(contains(gameUi, "Filtering: Illusion"));
	EXPECT(contains(gameUi, "STORE MAGIC - VAULT"));
	EXPECT(contains(gameUi, "MenuMiddleClick"));
	EXPECT(contains(itemPicker, "FAKE / ILLUSORY"));
	EXPECT(contains(itemPicker, "REAL DUPLICATE"));
	EXPECT(contains(itemPicker, "100 MP paid on cast"));
	EXPECT(contains(handMagic, "actPhantasmPathRangefinder"));
	EXPECT(contains(handMagic, "PhantasmPathPreviewTileState::NeedsIllusionSupport"));
	EXPECT(contains(handMagic, "PhantasmPathPreviewTileState::Invalid"));
	EXPECT(contains(handMagic, "route length and every accepted/rejected tile"));
	EXPECT(contains(player, "PhantasmPathPreview::kMaximumTiles"));
	EXPECT(contains(player, "pathPreview->setUID(-3)"));
	EXPECT(contains(runtime, "kMaximumHolograms = 32"));
	EXPECT(contains(runtime, "kMaximumFakeLootVisuals = 16"));
	EXPECT(contains(runtime, "kMaximumVaultOwners = MAXPLAYERS + 32"));
	EXPECT(contains(runtime, "activeInstanceKey"));
	EXPECT(contains(runtime, "proficiencyForSpell"));
	EXPECT(contains(runtime, "previewPhantasmPath(caster, tileX, tileY)"));
	EXPECT(contains(runtime, "No continuous unsupported path is valid in range."));
	EXPECT(contains(runtime, "state.vaults.clear()"));
	EXPECT(contains(runtime, "newItem(source->type"));
	EXPECT(contains(runtime, "itemPickup(ownerPlayer, duplicate)"));
	EXPECT(contains(runtime, "valid packet stable id"));
	EXPECT(contains(netRuntime, "'ILOV'"));
	EXPECT(contains(netRuntime, "'ILIT'"));
	EXPECT(contains(netRuntime, "'ILVA'"));
	EXPECT(contains(netRuntime, "'ILVR'"));
	EXPECT(contains(netRuntime, "decodeGameplayPacketPlayerIndex"));
	for (const char* spellCase : {
		"case MIRROR_OTHER:", "case MIRROR_COPY:", "case MIRROR_WALL:",
		"case MIRROR_REFLECT:", "case MIRROR_MIMIC:",
		"case MIRROR_REFLECT_LOOT:", "case PHANTASM_PATH:",
		"case MIRROR_DUPLICATE_LOOT:", "case MIRAGE_WALL:",
		"case PARANOIA:", "case VERTICAL_MIRAGE:",
		"case SHADOW_STEP:", "case STORE_MAGIC:"
	})
	{
		EXPECT(contains(runtime, spellCase));
	}
	EXPECT(appearsInOrder(defenses, {
		"tryStoreIncoming", "// count reflection", "shouldMirrorReflect",
		"absorbMagicEvent"
	}));
	return true;
}
}

int main()
{
	const std::array tests = {
		testDefinitionsAndStableIds,
		testTierAndVaultRules,
		testStoredReleaseScaling,
		testCaptureAndDuplicationSafety,
		testPlayableZOverlayIsolation,
		testRuntimeAndSpellbookContracts
	};
	for (const auto test : tests)
	{
		if (!test())
		{
			return 1;
		}
	}
	std::cout << "Illusion Magic tests passed\n";
	return 0;
}
