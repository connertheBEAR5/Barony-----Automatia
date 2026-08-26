#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace
{
bool expect(const bool condition, const char* expression)
{
	if (!condition)
	{
		std::cerr << "FAILED: " << expression << '\n';
	}
	return condition;
}

#define EXPECT(expression) \
	do \
	{ \
		if (!expect((expression), #expression)) \
		{ \
			return false; \
		} \
	} while (false)

std::filesystem::path sourcePath(const char* relative)
{
	return std::filesystem::path(BARONY_SOURCE_DIR) / relative;
}

std::string readFile(const char* relative)
{
	std::ifstream input(sourcePath(relative), std::ios::binary);
	std::ostringstream contents;
	contents << input.rdbuf();
	return input ? contents.str() : std::string{};
}

bool contains(const std::string& source, const std::string& value)
{
	return source.find(value) != std::string::npos;
}

std::size_t occurrences(const std::string& source, const std::string& value)
{
	std::size_t count = 0;
	std::size_t offset = 0;
	while ((offset = source.find(value, offset)) != std::string::npos)
	{
		++count;
		offset += value.size();
	}
	return count;
}

bool testExistingRuntimeActorIsReused()
{
	const std::string monsterHeader = readFile("src/monster.hpp");
	const std::string mimic = readFile("src/monster_mimic.cpp");
	const std::string actMonster = readFile("src/actmonster.cpp");
	const std::string entityShared = readFile("src/entity_shared.cpp");
	const std::string maps = readFile("src/maps.cpp");
	const std::string stats = readFile("src/stat_shared.cpp");

	EXPECT(!monsterHeader.empty());
	EXPECT(!mimic.empty());
	EXPECT(contains(monsterHeader, "MINIMIMIC"));
	EXPECT(contains(monsterHeader, "1794"));
	EXPECT(contains(mimic, "void initMiniMimic(Entity* my, Stat* myStats)"));
	EXPECT(contains(mimic, "my->initMonster(1794)"));
	EXPECT(contains(mimic, "newEntity(1795"));
	EXPECT(contains(mimic, "createMonsterEquipment(myStats, rng)"));
	EXPECT(contains(mimic, "createCustomInventory(myStats"));
	EXPECT(contains(actMonster, "case MINIMIMIC: initMiniMimic(my, myStats);"));
	EXPECT(contains(actMonster, "case MINIMIMIC: mimicAnimate(my, myStats, dist);"));

	EXPECT(contains(monsterHeader, "EDITOR_SPRITE_MINIMIMIC = 248"));
	EXPECT(contains(entityShared,
		"case EDITOR_SPRITE_MINIMIMIC: monsterType = MINIMIMIC; break;"));
	EXPECT(contains(maps, "case EDITOR_SPRITE_MINIMIMIC:"));
	EXPECT(contains(maps, "editorSpriteTypeToMonster(entity->sprite)"));
	EXPECT(contains(stats, "case EDITOR_SPRITE_MINIMIMIC:"));
	EXPECT(contains(stats, "case (1000 + MINIMIMIC):"));
	EXPECT(!std::filesystem::exists(sourcePath("src/monster_minimimic.cpp")));
	return true;
}

bool testEditorAuthoringAndThirtyTwoLayers()
{
	const std::string mainHeader = readFile("src/main.hpp");
	const std::string editor = readFile("src/editor.cpp");
	const std::string editorHeader = readFile("src/editor.hpp");
	const std::string buttons = readFile("src/buttons.cpp");
	const std::string draw = readFile("src/draw.cpp");
	const std::string entityShared = readFile("src/entity_shared.cpp");
	const std::string files = readFile("src/files.cpp");

	EXPECT(contains(mainHeader, "#define MAPLAYERS 32"));
	EXPECT(contains(entityShared, "\"MINI MIMIC\""));
	EXPECT(contains(entityShared, "case EDITOR_SPRITE_MINIMIMIC:"));
	EXPECT(contains(editor, "editorPaletteSpriteVisual"));
	EXPECT(contains(editor, "paletteIndex == EDITOR_SPRITE_MINIMIMIC"));
	EXPECT(contains(editor, "newEntity(spriteIndex, 0, map.entities"));
	EXPECT(contains(editor,
		"entity->authoredMapLayer = static_cast<Sint16>"));
	EXPECT(contains(editor, "std::min(drawlayer, MAPLAYERS - 1)"));
	EXPECT(contains(draw, "entity->sprite == EDITOR_SPRITE_MINIMIMIC"));

	// Selection, copy/paste, delete, and map save/load remain generic entity
	// operations once checkSpriteType() classifies marker 248 as a monster.
	EXPECT(contains(entityShared, "tmpStats->copyStats()"));
	EXPECT(contains(entityShared, "entityNew->authoredMapLayer"));
	EXPECT(contains(editor, "buttonDelete(NULL)"));
	EXPECT(contains(editorHeader, "MONSTER_PROPERTY_DISPOSITION = 32"));
	EXPECT(contains(buttons, "copyMonsterStatToPropertyStrings"));
	EXPECT(contains(files, "&entity->persistentID"));
	EXPECT(contains(files, "&myStats->MISC_FLAGS"));
	EXPECT(contains(files, "&myStats->customDialogueID"));
	EXPECT(contains(files, "appendPlayableZChunk(payload, \"EFLR\""));
	EXPECT(contains(files, "appendPlayableZChunk(payload, \"ELYR\""));
	return true;
}

bool testDispositionRecruitmentAndDialogueAreIndependent()
{
	const std::string statHeader = readFile("src/stat.hpp");
	const std::string entity = readFile("src/entity.cpp");
	const std::string editor = readFile("src/editor.cpp");
	const std::string buttons = readFile("src/buttons.cpp");
	const std::string actMonster = readFile("src/actmonster.cpp");
	const std::string scores = readFile("src/scores.cpp");

	EXPECT(contains(statHeader, "MONSTER_FORCE_PLAYER_ALLY"));
	EXPECT(contains(statHeader, "MONSTER_FORCE_PLAYER_ENEMY"));
	EXPECT(contains(statHeader, "MONSTER_FORCE_PLAYER_NEUTRAL"));
	EXPECT(contains(statHeader, "STAT_FLAG_MONSTER_RECRUITABLE = 30"));
	EXPECT(contains(statHeader, "bool monsterIsRecruitable() const"));
	EXPECT(occurrences(entity, "MONSTER_FORCE_PLAYER_NEUTRAL") == 4);

	EXPECT(contains(editor, "Disposition: [%s]"));
	EXPECT(contains(editor, "Recruitable: [%c]"));
	EXPECT(contains(editor, "Custom Dialogue: [%c]"));
	EXPECT(contains(editor, "Dialogue Resource:"));
	EXPECT(contains(buttons, "Stat::MONSTER_FORCE_PLAYER_ENEMY"));
	EXPECT(contains(buttons, "Stat::MONSTER_FORCE_PLAYER_NEUTRAL"));
	EXPECT(contains(buttons, "Stat::MONSTER_FORCE_PLAYER_ALLY"));
	EXPECT(contains(buttons, "STAT_FLAG_MONSTER_RECRUITABLE"));
	EXPECT(occurrences(actMonster, "myStats->monsterIsRecruitable()") >= 3);

	// Custom dialogue takes interaction priority, while recruitment remains
	// available independently and the authored resource survives recruitment.
	EXPECT(contains(actMonster, "Custom authored dialogue takes priority"));
	EXPECT(contains(actMonster, "handleCustomMonsterDialogue("));
	EXPECT(contains(actMonster, "forceFollower("));
	EXPECT(!contains(actMonster,
		"npc->getStats()->customDialogueID[0] = '\\0';"));
	EXPECT(!contains(scores,
		"monsterStats->customDialogueID[0] = '\\0';"));
	EXPECT(contains(scores, "automatia_custom_dialogue"));
	EXPECT(contains(scores, "follower->customDialogueID"));
	EXPECT(contains(scores, "stats->customDialogueID"));
	return true;
}

bool testSpatialPersistenceOwnerAndSamInventoryContracts()
{
	const std::string mimic = readFile("src/monster_mimic.cpp");
	const std::string collision = readFile("src/collision.cpp");
	const std::string files = readFile("src/files.cpp");
	const std::string game = readFile("src/game.cpp");
	const std::string scores = readFile("src/scores.cpp");

	// Entity::z remains local model elevation; structural and gameplay
	// membership are separate persisted axes.
	EXPECT(contains(mimic, "my->z = 0;"));
	EXPECT(contains(files, "entity->playableFloor"));
	EXPECT(contains(files, "entity->authoredMapLayer"));
	EXPECT(occurrences(collision, "playableFloor !=") >= 4);

	// Persistent world keys include the active MapInstance identity, so equal
	// X/Y positions on different floors or instances cannot alias.
	EXPECT(contains(game, "worldState.activeIdentity()"));
	EXPECT(contains(game, "return identity->key();"));
	EXPECT(contains(game, "savedState.playableFloor"));
	EXPECT(contains(game, "savedState.authoredMapLayer"));
	EXPECT(contains(game, "state.dynamicMonsterIDs"));
	EXPECT(contains(game, "savedState.monsterSavedType"));
	EXPECT(contains(game, "savedState.monsterSavedZ"));

	// Follower ownership is restored by the existing durable character-save
	// path, and duplicate actors are cleared before recreation.
	EXPECT(contains(scores, "characterIdentityForPlayer("));
	EXPECT(contains(scores, "pre-restore cleanup"));
	EXPECT(contains(scores, "monsterStats->leader_uid"));
	EXPECT(contains(scores, "monster->monsterAllyIndex = player"));
	EXPECT(contains(scores, "monster->inheritSpatialContextFrom"));

	// Mini Mimic inventory uses the same stable-ID resolver as every monster;
	// there is deliberately no species-specific S.A.M. persistence branch.
	EXPECT(contains(game, "persistentStableItemId"));
	EXPECT(contains(game, "SAMItemRegistryFoundation::isRegisteredRuntimeItemId"));
	EXPECT(contains(game, "itemState.stableId = persistentStableItemId"));
	EXPECT(contains(game, "resolvePersistentItemType"));
	const std::size_t itemCapture = game.find("static bool capturePersistentMonsterItem");
	const std::size_t goldCapture = game.find("static bool capturePersistentGoldBagState", itemCapture);
	EXPECT(itemCapture != std::string::npos);
	EXPECT(goldCapture != std::string::npos);
	EXPECT(!contains(game.substr(itemCapture, goldCapture - itemCapture), "MINIMIMIC"));
	return true;
}
}

int main()
{
	if (!testExistingRuntimeActorIsReused()
		|| !testEditorAuthoringAndThirtyTwoLayers()
		|| !testDispositionRecruitmentAndDialogueAreIndependent()
		|| !testSpatialPersistenceOwnerAndSamInventoryContracts())
	{
		return 1;
	}
	std::cout << "Mini Mimic authored-creature characterization passed.\n";
	return 0;
}
