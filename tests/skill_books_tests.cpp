#include "skill_books.hpp"

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
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

#define EXPECT(expression) \
	do { if ( !expect(static_cast<bool>(expression), #expression, __LINE__) ) return false; } while ( false )

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
	for ( const char* token : tokens )
	{
		position = source.find(token, position);
		if ( position == std::string::npos )
		{
			return false;
		}
		position += std::char_traits<char>::length(token);
	}
	return true;
}

bool testLoreBucketsAndMutation()
{
	using namespace SkillBooks;
	EXPECT(loreMagnitude(-1) == 1);
	EXPECT(loreMagnitude(0) == 1);
	EXPECT(loreMagnitude(19) == 1);
	EXPECT(loreMagnitude(20) == 2);
	EXPECT(loreMagnitude(39) == 2);
	EXPECT(loreMagnitude(40) == 3);
	EXPECT(loreMagnitude(59) == 3);
	EXPECT(loreMagnitude(60) == 4);
	EXPECT(loreMagnitude(79) == 4);
	EXPECT(loreMagnitude(80) == 5);
	EXPECT(loreMagnitude(100) == 5);
	EXPECT(loreMagnitude(500) == 5);
	EXPECT(applyDelta(10, 0, false) == 11);
	EXPECT(applyDelta(10, 40, false) == 13);
	EXPECT(applyDelta(10, 80, true) == 5);
	EXPECT(applyDelta(2, 100, true) == 0);
	EXPECT(applyDelta(99, 100, false) == 100);
	EXPECT(applyDelta(-10, 0, false) == 1);
	EXPECT(applyDelta(150, 0, true) == 99);
	return true;
}

bool testTargetsAndEncoding()
{
	using namespace SkillBooks;
	EXPECT(kEligibleSkills.size() == 15);
	EXPECT(!isEligibleSkill(3)); // Appraisal/Lore is the scaling source.
	EXPECT(!isEligibleSkill(-1));
	EXPECT(!isEligibleSkill(16));
	for ( const int skill : kEligibleSkills )
	{
		const std::uint32_t encoded = encodeSkill(skill);
		EXPECT(hasEncodedSkill(encoded));
		EXPECT(decodeSkill(encoded) == skill);
		EXPECT((encoded & kAppearanceMarker) != 0);
	}
	EXPECT(encodeSkill(3) == 0);
	EXPECT(decodeSkill(0) == -1);
	EXPECT(decodeSkill(kAppearanceMarker | 3u) == -1);
	EXPECT(decodeSkill(kAppearanceMarker | 31u) == -1);
	return true;
}

bool testRuntimeContracts()
{
	const std::string itemUsage = readSource("src/item_usage_funcs.cpp");
	const std::string items = readSource("src/items.cpp");
	const std::string network = readSource("src/net.cpp");
	const std::string definitions = readSource("src/mod_tools.cpp");
	const std::string flags = readSource("src/net.hpp");
	const std::string menu = readSource("src/ui/MainMenu.cpp");
	const std::string maps = readSource("src/maps.cpp");
	const std::string magic = readSource("src/magic/magic.cpp");
	const std::string game = readSource("src/game.cpp");

	EXPECT(contains(flags, "SV_FLAG_AUTOMATIAN_MODE = 1 << 11"));
	EXPECT(contains(menu, "automatian_mode_enabled"));
	EXPECT(contains(menu, "\"Automatian Mode\""));
	EXPECT(contains(menu, "legacyIllusionMagicEnabled || legacySkillBooksEnabled"));
	EXPECT(contains(definitions, "initializeSkillManualItemDefinitions"));
	EXPECT(contains(items, "itemIsSkillManual"));
	EXPECT(contains(items, "SKILL_BOOK"));
	EXPECT(contains(definitions, "Unknown skill book"));
	EXPECT(contains(itemUsage, "void item_SkillManual"));
	EXPECT(contains(itemUsage, "stats[player]->getModifiedProficiency(PRO_APPRAISAL)"));
	EXPECT(contains(itemUsage, "players[player]->entity->applySkillDelta"));
	EXPECT(contains(network, "resolvedType == SKILL_BOOK || resolvedType == SKILL_SCROLL"));
	EXPECT(contains(network, "itemIsSkillManual(candidate)"));
	EXPECT(contains(network, "Rejected invalid or disabled Grimoire spell request"));
	EXPECT(contains(maps, "map_rng.rand() % 75 == 0"));
	EXPECT(contains(maps, "&& automatianModeEnabled()"));
	EXPECT(contains(magic, "!automatianModeEnabled() || !stats || !grimoire"));
	EXPECT(contains(game, "if ( !automatianModeEnabled() || automatiaMagicGrimoireMerchantUnlocked )"));
	EXPECT(appearsInOrder(itemUsage, {
		"if (!automatianModeEnabled())",
		"const int lore = stats[player]->getModifiedProficiency(PRO_APPRAISAL)",
		"if (multiplayer == CLIENT)",
		"players[player]->entity->applySkillDelta" }));
	return true;
}
}

int main()
{
	if ( !testLoreBucketsAndMutation()
		|| !testTargetsAndEncoding()
		|| !testRuntimeContracts() )
	{
		return 1;
	}
	std::cout << "skill books tests passed\n";
	return 0;
}
