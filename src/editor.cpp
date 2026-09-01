/*-------------------------------------------------------------------------------

	BARONY
	File: editor.cpp
	Desc: main code for the level editor

	Copyright 2013-2016 (c) Turning Wheel LLC, all rights reserved.
	See LICENSE for details.

-------------------------------------------------------------------------------*/

#include "main.hpp"
#include "draw.hpp"
#include "light.hpp"
#include "editor.hpp"
#include "entity.hpp"
#include "items.hpp"
#include "player.hpp"
#include "interface/interface.hpp"
#include "files.hpp"
#include "init.hpp"
#include "mod_tools.hpp"
#include "text_source_script_tester.hpp"
#include "custom_dialogue_document.hpp"
#include <sys/stat.h>
#include <cmath>
#include <fstream>
#include <sstream>
#include <cctype>
#include "json.hpp"
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <functional>
#include <tuple>
#include <utility>
#ifndef EDITOR
#define EDITOR
#endif

#ifdef STEAMWORKS
#include <steam/steam_api.h>
#include "steam.hpp"
#endif // STEAMWORKS


//#include "player.hpp"

extern char monsterEffectSearchText[64];
extern char monsterEffectIdText[8];
extern int monsterEffectSelectedId;
extern void monsterEffectsUpdateSelectionFromFields();
extern const char* monsterEffectDisplayName(int effect);
extern bool monsterEffectCanSelect(int effect);
extern void monsterEffectsSetRowsVisible(bool visible);

static int monsterEffectDropdownScroll = 0;
static std::string monsterEffectDropdownLastQuery;

std::map<int, std::string> modelFileNames;
Entity* selectedEntity[MAXPLAYERS] = { nullptr };
Entity* lastSelectedEntity[MAXPLAYERS] = { nullptr };
Sint32 mousex = 0, mousey = 0;
Sint32 omousex = 0, omousey = 0;
Sint32 mousexrel = 0, mouseyrel = 0;
int itemSelect = 0;
int itemSlotSelected = -1;
char itemName[128];
real_t prev_x = 0;
real_t prev_y = 0;
bool duplicatedSprite = false;
int game = 0;

/*
 * Stage Z3.3 layer-authored stair tools. These are virtual palette entries
 * only: they reuse the existing ladder artwork while storing a dedicated
 * one-layer vertical marker on the entity. The existing Zed drawlayer is the
 * sole vertical authoring control; there is no separate playable-floor UI.
 */
static constexpr int EDITOR_VIRTUAL_Z_STAIR_UP = -10001;
static constexpr int EDITOR_VIRTUAL_Z_STAIR_DOWN = -10002;
static constexpr int EDITOR_Z_STAIR_UP_VISUAL = 11;
static constexpr int EDITOR_Z_STAIR_DOWN_VISUAL = 43;

static bool editorIsVirtualZStair(const int paletteIndex)
{
    return paletteIndex == EDITOR_VIRTUAL_Z_STAIR_UP
        || paletteIndex == EDITOR_VIRTUAL_Z_STAIR_DOWN;
}

static const char* editorVirtualSpriteName(const int paletteIndex)
{
    switch (paletteIndex)
    {
        case EDITOR_VIRTUAL_Z_STAIR_UP:
            return "Z STAIR UP (next map layer)";
        case EDITOR_VIRTUAL_Z_STAIR_DOWN:
            return "Z STAIR DOWN (previous map layer)";
        default:
            return "";
    }
}

static int editorVirtualSpriteVisual(const int paletteIndex)
{
    switch (paletteIndex)
    {
        case EDITOR_VIRTUAL_Z_STAIR_UP:
            return EDITOR_Z_STAIR_UP_VISUAL;
        case EDITOR_VIRTUAL_Z_STAIR_DOWN:
            return EDITOR_Z_STAIR_DOWN_VISUAL;
        default:
            return paletteIndex;
    }
}

static int editorPaletteSpriteVisual(const int paletteIndex)
{
	// The authored Mini Mimic marker is not a new runtime model. Use the
	// existing chest marker artwork in Zed while retaining sprite 248 in the
	// map record so assignActions() can resolve the real MINIMIMIC actor.
	if ( paletteIndex == EDITOR_SPRITE_MINIMIMIC )
	{
		return 21;
	}
	return editorVirtualSpriteVisual(paletteIndex);
}

/* Per-map fog editor fields packed into MAP_FLAG_GENBYTES5/6. */
char mapFogEnabledText[4] = "[ ]";
char mapFogDistanceText[8] = "384";
char mapFogDensityText[4] = "255";
char mapFogRedText[4] = "180";
char mapFogGreenText[4] = "180";
char mapFogBlueText[4] = "180";
char mapAmbientLightEnabledText[4] = "[ ]";
char mapAmbientLightRedText[4] = "32";
char mapAmbientLightGreenText[4] = "32";
char mapAmbientLightBlueText[4] = "32";
char mapAmbienceEnabledText[4] = "[ ]";
char mapAmbienceResourceText[256] = "";
char mapAmbienceVolumeText[4] = "100";
char mapAmbienceLoopText[4] = "[x]";
char mapAmbienceFadeInText[6] = "0";
char mapAmbienceFadeOutText[6] = "0";
// function prototypes
Uint32 timerCallback(Uint32 interval, void* param);
bool handleEvents(void);
void mainLogic(void);
std::vector<Entity*> groupedEntities;
bool moveSelectionNegativeX = false;
bool moveSelectionNegativeY = false;

/*
 * Automatia cuboid room-selection state.
 *
 * Stage:
 *   0 = click top-left
 *   1 = click bottom-right
 *   2 = area selected
 *   3 = ready to paste
 *   4 = placing room
 *   5 = room placed
 *   6 = area deleted
 */
int roomSelectBottomLayer = 0;
int roomSelectTopLayer = 0;
int roomSelectStage = 0;
bool roomClipboardReady = false;
bool editor3DModelsEnabled = true;
int roomClipboardWidth = 0;
int roomClipboardHeight = 0;
int roomClipboardDepth = 0;
int roomClipboardEntityCount = 0;

static map_t roomClipboardMap;
static list_t roomClipboardEntityList = { nullptr, nullptr };

enum RoomCopyContentMode
{
	ROOM_COPY_BOTH = 0,
	ROOM_COPY_TILES,
	ROOM_COPY_SPRITES
};

static int roomCopyContentMode = ROOM_COPY_BOTH;
static bool roomClipboardHasTiles = false;
static bool roomClipboardHasSprites = false;
static std::uint32_t roomGroupSelectedID = 0;
static int roomGroupListScroll = 0;
static char roomGroupNameText[AUTHORED_ROOM_GROUP_NAME_BYTES] = "Room Group";
static char roomGroupStatusText[160] = "";
static Uint32 roomGroupStatusUntil = 0;

static std::uint8_t roomCopyContentMask()
{
	switch ( roomCopyContentMode )
	{
		case ROOM_COPY_TILES:
			return AUTHORED_ROOM_GROUP_TILES;
		case ROOM_COPY_SPRITES:
			return AUTHORED_ROOM_GROUP_SPRITES;
		default:
			return AUTHORED_ROOM_GROUP_BOTH;
	}
}

void roomSelectResetSelection()
{
	if ( pasting )
	{
		editorRoomCancelPaste();
	}

	selectedarea_x1 = 0;
	selectedarea_y1 = 0;
	selectedarea_x2 = 0;
	selectedarea_y2 = 0;
	roomSelectBottomLayer = 0;
	roomSelectTopLayer = 0;
	roomSelectStage = 0;
	selectedarea = false;
	selectingspace = false;
	groupedEntities.clear();
}
std::vector<std::string> mapNames;
std::list<std::string> modFolderNames;
std::string physfs_saveDirectory = BASE_DATA_DIR;
std::string physfs_openDirectory = BASE_DATA_DIR;
float limbs[NUMMONSTERS][30][3]; // dummy variable for files.cpp limbs reloading in Barony.
void buttonStartSingleplayer(button_t* my) {} // dummy function for mod_tools.cpp
void steamStatisticUpdate(int statisticNum, ESteamStatTypes type, int value) {} // dummy function for mod_tools.cpp
//AchievementObserver achievementObserver; // dummy function for mod_tools.cpp
void initClass(int i) {} // dummy function for mod_tools.cpp
//void AchievementObserver::updateGlobalStat(int index, int value) {}
std::vector<std::pair<SDL_Surface**, std::string>> systemResourceImages; // dummy variable for files.cpp system resource reloading in Barony.
void initMenuOptions() {} // dummy
int textInsertCaratPosition = -1;
GenericGUIMenu GenericGUI[MAXPLAYERS];

static std::string questEditorStatusMessage;
static Uint32 questEditorStatusUntil = 0;
static bool questEditorCreateMarkers = false;


/*
 * Searchable editor palette state.
 *
 * These fields are UI-only and deliberately remain local to editor.cpp so the
 * palette update does not alter map data, editor file formats, or entity data.
 */
static char editorSpritePaletteSearch[128] = "";
static char editorTilePaletteSearch[128] = "";
static char editorMonsterItemSearch[128] = "";
static char editorOpenMapSearch[128] = "";
static char editorDirectorySearch[128] = "";
static char editorAmbienceResourceSearch[128] = "";
static std::vector<std::string> editorAmbienceResources;
static int editorAmbienceResourceFirstVisible = 0;
static bool editorAmbienceResourcesEnumerated = false;
static bool editorAmbiencePickerOpen = false;
static std::string editorMonsterItemSearchLastKey;
static std::string editorPaletteLastFilter;
static std::vector<int> editorPaletteMatches;
static int editorPaletteSelectedMatch = 0;
static int editorPaletteFirstVisible = 0;
static int editorPaletteActiveType = 0; // 1 = sprites, 2 = tiles.

static std::string editorPaletteLowercase(const std::string& text)
{
    std::string result = text;
    for ( char& character : result )
    {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return result;
}

static bool editorIsAmbienceResource(const std::string& path)
{
	const std::string lowered = editorPaletteLowercase(path);
	return lowered.size() > 4
		&& (lowered.compare(lowered.size() - 4, 4, ".ogg") == 0
			|| lowered.compare(lowered.size() - 4, 4, ".wav") == 0
			|| lowered.compare(lowered.size() - 4, 4, ".mp3") == 0);
}

static void editorCollectAmbienceResources(const std::string& directory)
{
	char** entries = PHYSFS_enumerateFiles(directory.c_str());
	if (!entries)
	{
		return;
	}
	for (char** entry = entries; *entry; ++entry)
	{
		const std::string resource = directory + "/" + *entry;
		PHYSFS_Stat stat;
		if (!PHYSFS_stat(resource.c_str(), &stat))
		{
			continue;
		}
		if (stat.filetype == PHYSFS_FILETYPE_DIRECTORY)
		{
			editorCollectAmbienceResources(resource);
		}
		else if (stat.filetype == PHYSFS_FILETYPE_REGULAR
			&& editorIsAmbienceResource(resource))
		{
			editorAmbienceResources.push_back(resource);
		}
	}
	PHYSFS_freeList(entries);
}

static void editorLoadAmbienceResources()
{
	if (editorAmbienceResourcesEnumerated)
	{
		return;
	}
	editorAmbienceResourcesEnumerated = true;
	if (PHYSFS_isInit())
	{
		editorCollectAmbienceResources("sound");
	}
	std::sort(editorAmbienceResources.begin(), editorAmbienceResources.end());
}

static bool editorPaletteTextMatches(const char* displayName, int index, const char* filter)
{
    if ( filter == nullptr || filter[0] == '\0' )
    {
        return true;
    }

    const std::string loweredFilter = editorPaletteLowercase(filter);
    const std::string loweredName = editorPaletteLowercase(displayName != nullptr ? displayName : "");
    if ( loweredName.find(loweredFilter) != std::string::npos )
    {
        return true;
    }

    const std::string indexText = std::to_string(index);
    return indexText.find(loweredFilter) != std::string::npos;
}

static void editorPaletteRebuildMatches(int paletteType)
{
    const char* filter = paletteType == 1 ? editorSpritePaletteSearch : editorTilePaletteSearch;
    const std::string filterKey = std::to_string(paletteType) + ":" + filter;
    if ( editorPaletteActiveType == paletteType && editorPaletteLastFilter == filterKey )
    {
        return;
    }

    editorPaletteMatches.clear();
    if ( paletteType == 1 )
    {
        const int count = std::min<int>(numsprites, static_cast<int>(spriteEditorNameStrings.size()));
        for ( int index = 0; index < count; ++index )
        {
            if ( editorPaletteTextMatches(spriteEditorNameStrings[index], index, filter) )
            {
                editorPaletteMatches.push_back(index);
            }
        }
        for ( const int virtualIndex : { EDITOR_VIRTUAL_Z_STAIR_UP, EDITOR_VIRTUAL_Z_STAIR_DOWN } )
        {
            if ( editorPaletteTextMatches(editorVirtualSpriteName(virtualIndex), virtualIndex, filter) )
            {
                editorPaletteMatches.push_back(virtualIndex);
            }
        }
    }
    else
    {
        const int namedTileCount = static_cast<int>(sizeof(tileEditorNameStrings) / sizeof(tileEditorNameStrings[0]));
        const int count = std::min<int>(static_cast<int>(numtiles), namedTileCount);
        for ( int index = 0; index < count; ++index )
        {
            if ( editorPaletteTextMatches(tileEditorNameStrings[index], index, filter) )
            {
                editorPaletteMatches.push_back(index);
            }
        }
    }

    editorPaletteActiveType = paletteType;
    editorPaletteLastFilter = filterKey;
    editorPaletteSelectedMatch = 0;
    editorPaletteFirstVisible = 0;
}

static void editorPaletteKeepSelectionVisible(int columns, int visibleRows)
{
    if ( editorPaletteMatches.empty() )
    {
        editorPaletteSelectedMatch = 0;
        editorPaletteFirstVisible = 0;
        return;
    }

    editorPaletteSelectedMatch = std::max(0,
        std::min(editorPaletteSelectedMatch, static_cast<int>(editorPaletteMatches.size()) - 1));

    const int pageSize = std::max(1, columns * visibleRows);
    const int selectedRow = editorPaletteSelectedMatch / columns;
    int firstRow = editorPaletteFirstVisible / columns;
    if ( selectedRow < firstRow )
    {
        firstRow = selectedRow;
    }
    else if ( selectedRow >= firstRow + visibleRows )
    {
        firstRow = selectedRow - visibleRows + 1;
    }

    const int maxFirstRow = std::max(0,
        (static_cast<int>(editorPaletteMatches.size()) - 1) / columns - visibleRows + 1);
    firstRow = std::max(0, std::min(firstRow, maxFirstRow));
    editorPaletteFirstVisible = firstRow * columns;
    editorPaletteFirstVisible = std::min(editorPaletteFirstVisible,
        std::max(0, static_cast<int>(editorPaletteMatches.size()) - pageSize));
    editorPaletteFirstVisible -= editorPaletteFirstVisible % columns;
}

static void editorPaletteHandleKeyboard(int columns, int visibleRows)
{
    if ( editorPaletteMatches.empty() )
    {
        return;
    }

    const int pageSize = std::max(1, columns * visibleRows);
    if ( keystatus[SDLK_LEFT] )
    {
        keystatus[SDLK_LEFT] = 0;
        --editorPaletteSelectedMatch;
    }
    if ( keystatus[SDLK_RIGHT] )
    {
        keystatus[SDLK_RIGHT] = 0;
        ++editorPaletteSelectedMatch;
    }
    if ( keystatus[SDLK_UP] )
    {
        keystatus[SDLK_UP] = 0;
        editorPaletteSelectedMatch -= columns;
    }
    if ( keystatus[SDLK_DOWN] )
    {
        keystatus[SDLK_DOWN] = 0;
        editorPaletteSelectedMatch += columns;
    }
    if ( keystatus[SDLK_PAGEUP] )
    {
        keystatus[SDLK_PAGEUP] = 0;
        editorPaletteSelectedMatch -= pageSize;
    }
    if ( keystatus[SDLK_PAGEDOWN] )
    {
        keystatus[SDLK_PAGEDOWN] = 0;
        editorPaletteSelectedMatch += pageSize;
    }
    if ( keystatus[SDLK_HOME] )
    {
        keystatus[SDLK_HOME] = 0;
        editorPaletteSelectedMatch = 0;
    }
    if ( keystatus[SDLK_END] )
    {
        keystatus[SDLK_END] = 0;
        editorPaletteSelectedMatch = static_cast<int>(editorPaletteMatches.size()) - 1;
    }

    editorPaletteKeepSelectionVisible(columns, visibleRows);
}

static void editorPaletteBeginTextInput(char* searchBuffer)
{
    if ( inputstr != searchBuffer || !SDL_IsTextInputActive() )
    {
        SDL_StartTextInput();
        inputstr = searchBuffer;
        inputlen = 127;
    }
}

static void editorPaletteEndTextInput()
{
    if ( SDL_IsTextInputActive() )
    {
        SDL_StopTextInput();
    }
    inputstr = nullptr;
    editorPaletteActiveType = 0;
    editorPaletteLastFilter.clear();
}


struct QuestDialogueEditorNodePreview
{
	int id = 0;
	std::string text;
	std::vector<std::string> choices;
	std::vector<int> nextNodes;
	int conditionCount = 0;
	int actionCount = 0;
};

struct QuestDialogueEditorPreview
{
	std::string filename;
	int schemaVersion = 0;
	std::string questID;
	std::string title;
	std::string summary;
	std::string scope;
	bool repeatable = false;
	bool hasOriginMarker = false;
	bool originTracksNPC = false;
	int originNPCPersistentID = 0;
	int recruitActionCount = 0;
	int objectiveCount = 0;
	int objectiveMarkerCount = 0;
	std::vector<QuestDialogueEditorNodePreview> nodes;
	std::string error;
};

struct QuestDialogueEditorFileSummary
{
	std::string dialogueID;
	std::string questID;
	std::string title;
	int errors = 0;
	int warnings = 0;
};

static std::string questEditorNormalizeID(const std::string& value);
static std::string questEditorCurrentMapFilename();

static std::vector<std::string> questDialogueEditorFiles;
static std::vector<QuestDialogueEditorFileSummary> questDialogueEditorFileSummaries;
static int questDialogueEditorSelectedFile = -1;
static int questDialogueEditorSelectedNode = -1;
static int questDialogueEditorSelectedChoice = -1;
static int questDialogueEditorSelectedObjective = -1;
static int questDialogueEditorFileScroll = 0;
static QuestDialogueEditorPreview questDialogueEditorPreview;
static automatia::dialogue::Document questDialogueEditorModel;
static rapidjson::Document& questDialogueEditorDocument =
	questDialogueEditorModel.json();
static std::string questDialogueEditorMessage;
static Uint32 questDialogueEditorMessageUntil = 0;

enum QuestDialogueEditableField
{
	QUEST_DIALOGUE_FIELD_FILE_ID = 0,
	QUEST_DIALOGUE_FIELD_QUEST_ID,
	QUEST_DIALOGUE_FIELD_QUEST_TITLE,
	QUEST_DIALOGUE_FIELD_QUEST_SUMMARY,
	QUEST_DIALOGUE_FIELD_QUEST_OBJECTIVE,
	QUEST_DIALOGUE_FIELD_QUEST_COMPLETED_TEXT,
	QUEST_DIALOGUE_FIELD_QUEST_FAILED_TEXT,
	QUEST_DIALOGUE_FIELD_LEGACY_TEXT,
	QUEST_DIALOGUE_FIELD_NODE_ID,
	QUEST_DIALOGUE_FIELD_NODE_TEXT,
	QUEST_DIALOGUE_FIELD_NODE_NEXT,
	QUEST_DIALOGUE_FIELD_CHOICE_ID,
	QUEST_DIALOGUE_FIELD_CHOICE_TEXT,
	QUEST_DIALOGUE_FIELD_CHOICE_NEXT,
	QUEST_DIALOGUE_FIELD_OBJECTIVE_ID,
	QUEST_DIALOGUE_FIELD_OBJECTIVE_TEXT,
	QUEST_DIALOGUE_FIELD_OBJECTIVE_COMPLETED_TEXT,
	QUEST_DIALOGUE_FIELD_OBJECTIVE_PROGRESS_VARIABLE,
	QUEST_DIALOGUE_FIELD_CONDITION_REFERENCE,
	QUEST_DIALOGUE_FIELD_CONDITION_QUEST,
	QUEST_DIALOGUE_FIELD_CONDITION_NUMBER,
	QUEST_DIALOGUE_FIELD_CONDITION_STABLE_ID,
	QUEST_DIALOGUE_FIELD_CONDITION_TRUE_NODE,
	QUEST_DIALOGUE_FIELD_CONDITION_FALSE_NODE,
	QUEST_DIALOGUE_FIELD_ACTION_REFERENCE,
	QUEST_DIALOGUE_FIELD_ACTION_NUMBER,
	QUEST_DIALOGUE_FIELD_ACTION_SECONDARY_NUMBER,
	QUEST_DIALOGUE_FIELD_ACTION_TERTIARY_NUMBER,
	QUEST_DIALOGUE_FIELD_ACTION_STABLE_ID,
	QUEST_DIALOGUE_FIELD_NODE_ACTION_ID,
	QUEST_DIALOGUE_FIELD_POWER_X,
	QUEST_DIALOGUE_FIELD_POWER_Y,
	QUEST_DIALOGUE_FIELD_OBJECTIVE_STAGE,
	QUEST_DIALOGUE_FIELD_OBJECTIVE_TARGET,
	QUEST_DIALOGUE_FIELD_OBJECTIVE_DEFEAT_ID,
	QUEST_DIALOGUE_FIELD_MARKER_MAP,
	QUEST_DIALOGUE_FIELD_MARKER_X,
	QUEST_DIALOGUE_FIELD_MARKER_Y,
	QUEST_DIALOGUE_FIELD_MARKER_FLOOR,
	QUEST_DIALOGUE_FIELD_ORIGIN_LABEL,
	QUEST_DIALOGUE_FIELD_ORIGIN_MAP,
	QUEST_DIALOGUE_FIELD_ORIGIN_X,
	QUEST_DIALOGUE_FIELD_ORIGIN_Y,
	QUEST_DIALOGUE_FIELD_ORIGIN_FLOOR,
	QUEST_DIALOGUE_FIELD_ORIGIN_NPC_ID,
	QUEST_DIALOGUE_FIELD_NUM_FIELDS
};

enum QuestDialogueFieldCategory
{
	QUEST_DIALOGUE_CATEGORY_FILE_QUEST = 0,
	QUEST_DIALOGUE_CATEGORY_TEXT,
	QUEST_DIALOGUE_CATEGORY_OBJECTIVE,
	QUEST_DIALOGUE_CATEGORY_CONDITION,
	QUEST_DIALOGUE_CATEGORY_ACTION,
	QUEST_DIALOGUE_CATEGORY_MARKER,
	QUEST_DIALOGUE_CATEGORY_COUNT
};

static QuestDialogueEditableField
	questDialogueEditorEditableField =
		QUEST_DIALOGUE_FIELD_QUEST_TITLE;

static QuestDialogueFieldCategory
	questDialogueEditorFieldCategory =
		QUEST_DIALOGUE_CATEGORY_FILE_QUEST;

static char questDialogueEditorEditBuffer[
	automatia::dialogue::MaximumDocumentBytes + 1] = "";
static bool questDialogueEditorEditingField = false;
static QuestDialogueEditableField questDialogueEditorLockedEditableField =
    QUEST_DIALOGUE_FIELD_QUEST_TITLE;
static QuestDialogueFieldCategory questDialogueEditorLockedFieldCategory =
    QUEST_DIALOGUE_CATEGORY_FILE_QUEST;
static bool questDialogueEditorRuleOwnerNode = false;
static bool questDialogueEditorLockedRuleOwnerNode = false;

enum QuestDialogueInspectorSelection
{
    QUEST_DIALOGUE_INSPECTOR_NONE = 0,
    QUEST_DIALOGUE_INSPECTOR_CONDITION,
    QUEST_DIALOGUE_INSPECTOR_ACTION
};

static QuestDialogueInspectorSelection
    questDialogueEditorInspectorSelection =
        QUEST_DIALOGUE_INSPECTOR_NONE;

enum QuestDialogueActionGroup
{
	QUEST_DIALOGUE_ACTION_GROUP_QUEST = 0,
	QUEST_DIALOGUE_ACTION_GROUP_REWARDS,
	QUEST_DIALOGUE_ACTION_GROUP_COSTS,
	QUEST_DIALOGUE_ACTION_GROUP_OBJECTIVES,
	QUEST_DIALOGUE_ACTION_GROUP_FLAGS,
	QUEST_DIALOGUE_ACTION_GROUP_VARIABLES,
	QUEST_DIALOGUE_ACTION_GROUP_NPC,
	QUEST_DIALOGUE_ACTION_GROUP_MECHANISMS,
	QUEST_DIALOGUE_ACTION_GROUP_STATUS,
	QUEST_DIALOGUE_ACTION_GROUP_COUNT
};

static QuestDialogueActionGroup
	questDialogueEditorActionGroup =
		QUEST_DIALOGUE_ACTION_GROUP_QUEST;

enum QuestDialogueConditionGroup
{
	QUEST_DIALOGUE_CONDITION_GROUP_ITEMS = 0,
	QUEST_DIALOGUE_CONDITION_GROUP_QUEST,
	QUEST_DIALOGUE_CONDITION_GROUP_OBJECTIVES,
	QUEST_DIALOGUE_CONDITION_GROUP_FLAGS,
	QUEST_DIALOGUE_CONDITION_GROUP_VARIABLES,
	QUEST_DIALOGUE_CONDITION_GROUP_COUNT
};

static QuestDialogueConditionGroup
	questDialogueEditorConditionGroup =
		QUEST_DIALOGUE_CONDITION_GROUP_ITEMS;

static int questDialogueEditorSelectedItemID = 0;
static int questDialogueEditorSelectedItemCount = 1;
static int questDialogueEditorSelectedConditionQuest = 0;
static int questDialogueEditorSelectedActionIndex = 0;
static int questDialogueEditorGoldAmount = 100;
static int questDialogueEditorSelectedConditionIndex = 0;
static int questDialogueEditorSelectedEffectID = 0;
static int questDialogueEditorEffectDurationSeconds = 30;
static int questDialogueEditorEffectStrength = 1;

enum QuestDialogueWorkspaceMode
{
	QUEST_DIALOGUE_WORKSPACE_FILES = 0,
	QUEST_DIALOGUE_WORKSPACE_CONVERSATION,
	QUEST_DIALOGUE_WORKSPACE_QUEST,
	QUEST_DIALOGUE_WORKSPACE_TUTORIALS,
	QUEST_DIALOGUE_WORKSPACE_JSON,
	QUEST_DIALOGUE_WORKSPACE_PREVIEW,
	QUEST_DIALOGUE_WORKSPACE_VALIDATION,
	QUEST_DIALOGUE_WORKSPACE_COUNT
};

enum QuestDialoguePendingTransition
{
	QUEST_DIALOGUE_PENDING_NONE = 0,
	QUEST_DIALOGUE_PENDING_CLOSE,
	QUEST_DIALOGUE_PENDING_SWITCH_FILE,
	QUEST_DIALOGUE_PENDING_RELOAD,
	QUEST_DIALOGUE_PENDING_OPEN_WIZARD,
	QUEST_DIALOGUE_PENDING_DUPLICATE_FILE,
	QUEST_DIALOGUE_PENDING_APPLY_TUTORIAL,
	QUEST_DIALOGUE_PENDING_DELETE_FILE
};

static QuestDialogueWorkspaceMode questDialogueEditorWorkspace =
	QUEST_DIALOGUE_WORKSPACE_CONVERSATION;
static QuestDialoguePendingTransition questDialogueEditorPendingTransition =
	QUEST_DIALOGUE_PENDING_NONE;
static int questDialogueEditorPendingFile = -1;
static int questDialogueEditorPendingTutorial = -1;
static bool questDialogueEditorUnsavedPrompt = false;
static bool questDialogueEditorDeletePrompt = false;
static std::vector<automatia::dialogue::Issue> questDialogueEditorValidationIssues;
static int questDialogueEditorValidationScroll = 0;
static int questDialogueEditorTutorialSelection = 0;
static int questDialogueEditorTutorialScroll = 0;
static int questDialogueEditorTutorialDifficulty = 0;
static int questDialogueEditorTutorialCategory = 0;
static char questDialogueEditorFileSearch[128] = "";
static char questDialogueEditorTutorialSearch[128] = "";
static bool questDialogueEditorFileSearchEditing = false;
static bool questDialogueEditorTutorialSearchEditing = false;
static char questDialogueEditorJSONBuffer[
	automatia::dialogue::MaximumDocumentBytes + 1] = "";
static bool questDialogueEditorJSONEditing = false;
static int questDialogueEditorJSONScroll = 0;
static int questDialogueEditorJSONHorizontalScroll = 0;
static std::size_t questDialogueEditorJSONCaret = 0;
static bool questDialogueEditorJSONSelectAll = false;

enum QuestDialogueMarkerPickKind
{
	QUEST_DIALOGUE_MARKER_PICK_NONE = 0,
	QUEST_DIALOGUE_MARKER_PICK_ORIGIN,
	QUEST_DIALOGUE_MARKER_PICK_OBJECTIVE
};

static QuestDialogueMarkerPickKind questDialogueEditorMarkerPick =
	QUEST_DIALOGUE_MARKER_PICK_NONE;
static int questDialogueEditorMarkerPickObjective = -1;
static int questDialogueEditorMarkerPickPlayableFloor = DEFAULT_PLAYABLE_FLOOR;
static bool questDialogueEditorMarkerPickRestore3D = false;
static bool questDialogueEditorPreserveModelOnOpen = false;
static bool questDialogueEditorBeginMarkerPick(
	QuestDialogueMarkerPickKind kind);
static automatia::dialogue::PreviewSession questDialogueEditorSandbox;
static automatia::dialogue::PreviewState questDialogueEditorSandboxInitialState;
static bool questDialogueEditorSandboxActive = false;
static int questDialogueEditorSandboxGold = 0;
static char questDialogueEditorSandboxItem[128] = "torch";
static int questDialogueEditorSandboxItemCount = 0;
static bool questDialogueEditorSandboxItemEditing = false;
enum QuestDialogueSandboxSeedKind
{
	QUEST_DIALOGUE_SANDBOX_WORLD_FLAG = 0,
	QUEST_DIALOGUE_SANDBOX_NPC_FLAG,
	QUEST_DIALOGUE_SANDBOX_WORLD_VARIABLE,
	QUEST_DIALOGUE_SANDBOX_NPC_VARIABLE,
	QUEST_DIALOGUE_SANDBOX_QUEST_STARTED,
	QUEST_DIALOGUE_SANDBOX_QUEST_ACCEPTED,
	QUEST_DIALOGUE_SANDBOX_QUEST_COMPLETED,
	QUEST_DIALOGUE_SANDBOX_QUEST_FAILED,
	QUEST_DIALOGUE_SANDBOX_QUEST_STAGE,
	QUEST_DIALOGUE_SANDBOX_OBJECTIVE_COMPLETED,
	QUEST_DIALOGUE_SANDBOX_NODE_SEEN,
	QUEST_DIALOGUE_SANDBOX_CHOICE_USED,
	QUEST_DIALOGUE_SANDBOX_SEED_COUNT
};
static QuestDialogueSandboxSeedKind questDialogueEditorSandboxSeedKind =
	QUEST_DIALOGUE_SANDBOX_WORLD_FLAG;
static char questDialogueEditorSandboxSeedKey[128] = "story_flag";
static char questDialogueEditorSandboxSeedSubkey[128] = "objective_1";
static bool questDialogueEditorSandboxSeedKeyEditing = false;
static bool questDialogueEditorSandboxSeedSubkeyEditing = false;
static int questDialogueEditorSandboxSeedValue = 1;
static bool questDialogueEditorSandboxSeedBoolean = true;
static automatia::dialogue::PreviewState questDialogueEditorSandboxConfiguredState;
static int questDialogueEditorConversationInspector = 0;
static int questDialogueEditorConversationNodeScroll = 0;
static int questDialogueEditorConversationChoiceScroll = 0;
static int questDialogueEditorConditionCatalogIndex = 0;
static int questDialogueEditorActionCatalogIndex = 0;
static int questDialogueEditorQuestPanel = 0;
static int questDialogueEditorObjectiveScroll = 0;
static char questDialogueEditorItemSearch[128] = "";
static bool questDialogueEditorItemSearchEditing = false;
static int questDialogueEditorItemSearchScroll = 0;

static bool questDialogueEditorWizardOpen = false;
static int questDialogueEditorWizardTemplate = 1;
static int questDialogueEditorWizardField = 0;
static int questDialogueEditorWizardStep = 0;
static bool questDialogueEditorWizardUseQuest = false;
static bool questDialogueEditorWizardRepeatable = false;
static int questDialogueEditorWizardScope = 0; // 0 personal, 1 party, 2 world
static int questDialogueEditorWizardOrigin = 0; // 0 none, 1 cursor, 2 selected NPC
static char questDialogueEditorWizardDialogueID[128] = "new_dialogue";
static char questDialogueEditorWizardQuestID[128] = "";
static char questDialogueEditorWizardNPCText[256] = "Hello, traveler.";
static char questDialogueEditorWizardChoiceText[256] = "Hello.";
static char questDialogueEditorWizardQuestTitle[128] = "New Quest";
static char questDialogueEditorWizardQuestSummary[256] = "";

static void questDialogueEditorRequestTransition(
	QuestDialoguePendingTransition transition,
	int argument = -1);
static void questDialogueEditorEndTransientTextInput();

static automatia::dialogue::ValidationOptions questDialogueEditorValidationOptions()
{
	automatia::dialogue::ValidationOptions options;
	options.vanillaItemCount = NUMITEMS;
	options.effectCount = NUMEFFECTS;
	options.stableItemAvailable = [](const std::string& stableID)
	{
		return editorSAMItemStableIDIsAvailable(stableID.c_str());
	};
	return options;
}

static void questDialogueEditorRefreshValidation()
{
	questDialogueEditorValidationIssues =
		automatia::dialogue::validate(questDialogueEditorDocument,
			questDialogueEditorValidationOptions());
	questDialogueEditorValidationScroll = std::max(0,
		std::min(questDialogueEditorValidationScroll,
			std::max(0, static_cast<int>(questDialogueEditorValidationIssues.size()) - 1)));
}

static void questDialogueEditorRefreshFiles()
{
	questDialogueEditorFiles.clear();
	questDialogueEditorFileSummaries.clear();

	DIR* directory = opendir("./dialogue");
	if ( !directory )
	{
		return;
	}

	for ( dirent* entry = readdir(directory);
		entry;
		entry = readdir(directory) )
	{
		const std::string name = entry->d_name;
		if ( name.size() >= 5
			&& name.substr(name.size() - 5) == ".json" )
		{
			questDialogueEditorFiles.push_back(name);
		}
	}

	closedir(directory);
	std::sort(
		questDialogueEditorFiles.begin(),
		questDialogueEditorFiles.end()
	);
	for ( const std::string& filename : questDialogueEditorFiles )
	{
		QuestDialogueEditorFileSummary summary;
		summary.dialogueID = filename.size() > 5
			? filename.substr(0, filename.size() - 5) : filename;
		automatia::dialogue::Document fileDocument;
		std::string error;
		if ( !fileDocument.loadFile("./dialogue/" + filename, error) )
		{
			summary.errors = 1;
		}
		else
		{
			const rapidjson::Value& root = fileDocument.json();
			if ( root.HasMember("quest_id") && root["quest_id"].IsString() )
				summary.questID = root["quest_id"].GetString();
			if ( root.HasMember("quest") && root["quest"].IsObject()
				&& root["quest"].HasMember("title")
				&& root["quest"]["title"].IsString() )
				summary.title = root["quest"]["title"].GetString();
			const auto issues = automatia::dialogue::validate(fileDocument.json(),
				questDialogueEditorValidationOptions());
			summary.errors = automatia::dialogue::countIssues(
				issues, automatia::dialogue::Severity::Error);
			summary.warnings = automatia::dialogue::countIssues(
				issues, automatia::dialogue::Severity::Warning);
		}
		questDialogueEditorFileSummaries.push_back(std::move(summary));
	}

	if ( questDialogueEditorSelectedFile
		>= static_cast<int>(questDialogueEditorFiles.size()) )
	{
		questDialogueEditorSelectedFile = -1;
	}
}

static int questDialogueEditorCountObjectMembers(
	const rapidjson::Value& value
)
{
	if ( !value.IsObject() )
	{
		return 0;
	}

	int count = 0;
	for ( auto member = value.MemberBegin();
		member != value.MemberEnd();
		++member )
	{
		++count;
	}
	return count;
}

static void questDialogueEditorLoadPreview(
	const std::string& filenameToLoad,
	const bool loadFromDisk = true
)
{
	const int previousNode = questDialogueEditorSelectedNode;
	const int previousChoice = questDialogueEditorSelectedChoice;
	const int previousObjective = questDialogueEditorSelectedObjective;
	questDialogueEditorPreview =
		QuestDialogueEditorPreview();

	questDialogueEditorPreview.filename =
		filenameToLoad;

	const std::string path =
		"./dialogue/" + filenameToLoad;

	if ( loadFromDisk )
	{
		std::string loadError;
		if ( !questDialogueEditorModel.loadFile(path, loadError) )
		{
			questDialogueEditorPreview.error = loadError;
			return;
		}
	}
	rapidjson::Document& document =
		questDialogueEditorDocument;

	if ( !document.IsObject() )
	{
		questDialogueEditorPreview.error =
			"Dialogue root is not a JSON object.";
		return;
	}
	if ( document.HasMember("version")
		&& document["version"].IsInt() )
	{
		questDialogueEditorPreview.schemaVersion =
			document["version"].GetInt();
	}

	if ( document.HasMember("quest_id")
		&& document["quest_id"].IsString() )
	{
		questDialogueEditorPreview.questID =
			document["quest_id"].GetString();
	}

	if ( document.HasMember("quest")
		&& document["quest"].IsObject() )
	{
		const rapidjson::Value& quest =
			document["quest"];

		if ( quest.HasMember("title")
			&& quest["title"].IsString() )
		{
			questDialogueEditorPreview.title =
				quest["title"].GetString();
		}

		if ( quest.HasMember("summary")
			&& quest["summary"].IsString() )
		{
			questDialogueEditorPreview.summary =
				quest["summary"].GetString();
		}

		if ( quest.HasMember("scope")
			&& quest["scope"].IsString() )
		{
			questDialogueEditorPreview.scope =
				quest["scope"].GetString();
		}

		if ( quest.HasMember("repeatable")
			&& quest["repeatable"].IsBool() )
		{
			questDialogueEditorPreview.repeatable =
				quest["repeatable"].GetBool();
		}

		if ( quest.HasMember("origin")
			&& quest["origin"].IsObject() )
		{
			const rapidjson::Value& origin =
				quest["origin"];

			questDialogueEditorPreview.hasOriginMarker =
				origin.HasMember("x")
				&& origin["x"].IsInt()
				&& origin.HasMember("y")
				&& origin["y"].IsInt();

			questDialogueEditorPreview.originTracksNPC =
				origin.HasMember("track_npc")
				&& origin["track_npc"].IsBool()
				&& origin["track_npc"].GetBool();

			if ( origin.HasMember("npc_persistent_id")
				&& origin["npc_persistent_id"].IsInt() )
			{
				questDialogueEditorPreview.originNPCPersistentID =
					origin["npc_persistent_id"].GetInt();
			}

			if ( questDialogueEditorPreview.originTracksNPC )
			{
				questDialogueEditorPreview.hasOriginMarker = true;
			}
		}

		if ( quest.HasMember("objectives")
			&& quest["objectives"].IsArray() )
		{
			questDialogueEditorPreview.objectiveCount =
				static_cast<int>(
					quest["objectives"].Size()
				);

			for ( const auto& objective :
				quest["objectives"].GetArray() )
			{
				if ( objective.IsObject()
					&& objective.HasMember("map_marker")
					&& objective["map_marker"].IsObject() )
				{
					++questDialogueEditorPreview
						.objectiveMarkerCount;
				}
			}
		}
	}

	if ( document.HasMember("nodes")
		&& document["nodes"].IsArray() )
	{
		for ( const auto& nodeValue :
			document["nodes"].GetArray() )
		{
			if ( !nodeValue.IsObject() )
			{
				continue;
			}

			QuestDialogueEditorNodePreview node;

			if ( nodeValue.HasMember("id")
				&& nodeValue["id"].IsInt() )
			{
				node.id =
					nodeValue["id"].GetInt();
			}

			if ( nodeValue.HasMember("text")
				&& nodeValue["text"].IsString() )
			{
				node.text =
					nodeValue["text"].GetString();
			}

			if ( nodeValue.HasMember("condition") )
			{
				node.conditionCount +=
					questDialogueEditorCountObjectMembers(
						nodeValue["condition"]
					);
			}

			if ( nodeValue.HasMember("action") )
			{
				node.actionCount +=
					questDialogueEditorCountObjectMembers(
						nodeValue["action"]
					);
			}

			if ( nodeValue.HasMember("choices")
				&& nodeValue["choices"].IsArray() )
			{
				for ( const auto& choice :
					nodeValue["choices"].GetArray() )
				{
					if ( !choice.IsObject() )
					{
						continue;
					}

					std::string choiceText =
						"(unnamed choice)";

					if ( choice.HasMember("text")
						&& choice["text"].IsString() )
					{
						choiceText =
							choice["text"].GetString();
					}

					node.choices.push_back(choiceText);

					int nextNode = -1;
					if ( choice.HasMember("next")
						&& choice["next"].IsInt() )
					{
						nextNode =
							choice["next"].GetInt();
					}
					node.nextNodes.push_back(nextNode);

					if ( choice.HasMember("condition") )
					{
						node.conditionCount +=
							questDialogueEditorCountObjectMembers(
								choice["condition"]
							);
					}

					if ( choice.HasMember("conditions")
						&& choice["conditions"].IsArray() )
					{
						for ( const auto& condition :
							choice["conditions"].GetArray() )
						{
							node.conditionCount +=
								questDialogueEditorCountObjectMembers(condition);
						}
					}

					if ( choice.HasMember("action") )
					{
						node.actionCount +=
							questDialogueEditorCountObjectMembers(
								choice["action"]
							);
					}

					if ( choice.HasMember("action")
						&& choice["action"].IsObject()
						&& choice["action"].HasMember("recruit_npc")
						&& choice["action"]["recruit_npc"].IsBool()
						&& choice["action"]["recruit_npc"].GetBool() )
					{
						++questDialogueEditorPreview.recruitActionCount;
					}
				}
			}

			questDialogueEditorPreview.nodes.push_back(node);
		}
	}

	if ( loadFromDisk )
	{
		questDialogueEditorSelectedNode =
			questDialogueEditorPreview.nodes.empty() ? -1 : 0;
		questDialogueEditorSelectedChoice = -1;
		questDialogueEditorSelectedObjective =
			questDialogueEditorPreview.objectiveCount > 0 ? 0 : -1;
	}
	else
	{
		questDialogueEditorSelectedNode = questDialogueEditorPreview.nodes.empty()
			? -1 : std::max(0, std::min(previousNode,
				static_cast<int>(questDialogueEditorPreview.nodes.size()) - 1));
		questDialogueEditorSelectedChoice = previousChoice;
		if ( questDialogueEditorSelectedNode >= 0 )
		{
			const int choiceCount = static_cast<int>(
				questDialogueEditorPreview.nodes[questDialogueEditorSelectedNode].choices.size());
			questDialogueEditorSelectedChoice = choiceCount == 0 ? -1
				: std::max(-1, std::min(previousChoice, choiceCount - 1));
		}
		questDialogueEditorSelectedObjective =
			questDialogueEditorPreview.objectiveCount == 0 ? -1
			: std::max(0, std::min(previousObjective,
				questDialogueEditorPreview.objectiveCount - 1));
	}
	questDialogueEditorRefreshValidation();
	questDialogueEditorSandboxActive = false;
}


static void questDialogueEditorSetMessage(
	const std::string& message
)
{
	questDialogueEditorMessage = message;
	questDialogueEditorMessageUntil =
		ticks + TICKS_PER_SECOND * 4;
}

static void questDialogueEditorRelinkQuestID(
	const std::string& oldID,
	const std::string& newID
)
{
	if ( oldID.empty() || oldID == newID
		|| !questDialogueEditorDocument.IsObject()
		|| !questDialogueEditorDocument.HasMember("nodes")
		|| !questDialogueEditorDocument["nodes"].IsArray() ) return;
	auto& allocator = questDialogueEditorDocument.GetAllocator();
	auto relink = [&](rapidjson::Value& condition)
	{
		if ( condition.IsObject() && condition.HasMember("quest")
			&& condition["quest"].IsString()
			&& questEditorNormalizeID(oldID)
				== questEditorNormalizeID(condition["quest"].GetString()) )
		{
			condition["quest"].SetString(newID.c_str(), allocator);
		}
	};
	for ( rapidjson::Value& node : questDialogueEditorDocument["nodes"].GetArray() )
	{
		if ( !node.IsObject() ) continue;
		if ( node.HasMember("condition") ) relink(node["condition"]);
		if ( !node.HasMember("choices") || !node["choices"].IsArray() ) continue;
		for ( rapidjson::Value& choice : node["choices"].GetArray() )
		{
			if ( !choice.IsObject() ) continue;
			if ( choice.HasMember("condition") ) relink(choice["condition"]);
			if ( choice.HasMember("conditions") && choice["conditions"].IsArray() )
				for ( rapidjson::Value& condition : choice["conditions"].GetArray() )
					relink(condition);
		}
	}
}

static void questDialogueEditorRelinkObjectiveID(
	const std::string& oldID,
	const std::string& newID
)
{
	if ( oldID.empty() || oldID == newID
		|| !questDialogueEditorDocument.IsObject()
		|| !questDialogueEditorDocument.HasMember("nodes")
		|| !questDialogueEditorDocument["nodes"].IsArray() ) return;
	const std::string questID = questDialogueEditorDocument.HasMember("quest_id")
		&& questDialogueEditorDocument["quest_id"].IsString()
		? questDialogueEditorDocument["quest_id"].GetString() : std::string{};
	auto& allocator = questDialogueEditorDocument.GetAllocator();
	auto relinkCondition = [&](rapidjson::Value& condition)
	{
		if ( !condition.IsObject() || !condition.HasMember("type")
			|| !condition["type"].IsString()
			|| !condition.HasMember("objective")
			|| !condition["objective"].IsString()
			|| questEditorNormalizeID(oldID)
				!= questEditorNormalizeID(condition["objective"].GetString()) ) return;
		const std::string type = questEditorNormalizeID(
			condition["type"].GetString());
		if ( type != "objective_completed" && type != "objective_incomplete" ) return;
		if ( condition.HasMember("quest") && condition["quest"].IsString()
			&& !questID.empty()
			&& questEditorNormalizeID(questID)
				!= questEditorNormalizeID(condition["quest"].GetString()) ) return;
		condition["objective"].SetString(newID.c_str(), allocator);
	};
	for ( rapidjson::Value& node : questDialogueEditorDocument["nodes"].GetArray() )
	{
		if ( !node.IsObject() || !node.HasMember("choices")
			|| !node["choices"].IsArray() ) continue;
		for ( rapidjson::Value& choice : node["choices"].GetArray() )
		{
			if ( !choice.IsObject() ) continue;
			if ( choice.HasMember("condition") ) relinkCondition(choice["condition"]);
			if ( choice.HasMember("conditions") && choice["conditions"].IsArray() )
				for ( rapidjson::Value& condition : choice["conditions"].GetArray() )
					relinkCondition(condition);
			if ( choice.HasMember("action") && choice["action"].IsObject() )
			{
				for ( const char* field : { "objective_complete", "objective_clear" } )
				{
					if ( choice["action"].HasMember(field)
						&& choice["action"][field].IsString()
						&& questEditorNormalizeID(oldID)
							== questEditorNormalizeID(choice["action"][field].GetString()) )
					{
						choice["action"][field].SetString(newID.c_str(), allocator);
					}
				}
			}
		}
	}
}

static bool questDialogueEditorSaveDocument()
{
	if ( questDialogueEditorSelectedFile < 0
		|| questDialogueEditorSelectedFile
			>= static_cast<int>(
				questDialogueEditorFiles.size()
			)
		|| !questDialogueEditorDocument.IsObject() )
	{
		questDialogueEditorSetMessage(
			"No valid dialogue file is selected."
		);
		return false;
	}

	questDialogueEditorModel.recordExternalEdit("Dialogue editor change");
	questDialogueEditorLoadPreview(
		questDialogueEditorFiles[questDialogueEditorSelectedFile],
		false
	);
	questDialogueEditorSandboxActive = false;
	questDialogueEditorSetMessage("Unsaved changes.");
	return true;
}

static bool questDialogueEditorWriteDocument()
{
	if ( questDialogueEditorSelectedFile < 0
		|| questDialogueEditorSelectedFile
			>= static_cast<int>(questDialogueEditorFiles.size())
		|| !questDialogueEditorDocument.IsObject() )
	{
		questDialogueEditorSetMessage("No valid dialogue file is selected.");
		return false;
	}

	const std::vector<automatia::dialogue::Issue> issues =
		automatia::dialogue::validate(
			questDialogueEditorDocument,
			questDialogueEditorValidationOptions()
		);
	for ( const auto& issue : issues )
	{
		if ( issue.severity == automatia::dialogue::Severity::Error )
		{
			questDialogueEditorSetMessage(
				"Save blocked: " + issue.location.path + " - " + issue.message
			);
			return false;
		}
	}

	const std::string path =
		"./dialogue/"
		+ questDialogueEditorFiles[
			questDialogueEditorSelectedFile
		];

	std::string saveError;
	if ( !questDialogueEditorModel.saveAtomic(path, saveError) )
	{
		questDialogueEditorSetMessage(saveError);
		return false;
	}

	questDialogueEditorSetMessage(
		"Saved "
		+ questDialogueEditorFiles[
			questDialogueEditorSelectedFile
		]
	);

	questDialogueEditorLoadPreview(
		questDialogueEditorFiles[
			questDialogueEditorSelectedFile
		],
		false
	);
	questDialogueEditorRefreshFiles();

	return true;
}

static rapidjson::Value* questDialogueEditorSelectedNodeValue()
{
	if ( !questDialogueEditorDocument.IsObject()
		|| !questDialogueEditorDocument.HasMember("nodes")
		|| !questDialogueEditorDocument["nodes"].IsArray()
		|| questDialogueEditorSelectedNode < 0 )
	{
		return nullptr;
	}

	rapidjson::Value& nodes =
		questDialogueEditorDocument["nodes"];

	if ( questDialogueEditorSelectedNode
		>= static_cast<int>(nodes.Size()) )
	{
		return nullptr;
	}

	return &nodes[
		static_cast<rapidjson::SizeType>(
			questDialogueEditorSelectedNode
		)
	];
}

static int questDialogueEditorNodeIDAt(
	const int index
)
{
	if ( !questDialogueEditorDocument.IsObject()
		|| !questDialogueEditorDocument.HasMember("nodes")
		|| !questDialogueEditorDocument["nodes"].IsArray()
		|| index < 0
		|| index >= static_cast<int>(
			questDialogueEditorDocument["nodes"].Size()
		) )
	{
		return 0;
	}

	const rapidjson::Value& node =
		questDialogueEditorDocument["nodes"][
			static_cast<rapidjson::SizeType>(index)
		];

	if ( node.IsObject()
		&& node.HasMember("id")
		&& node["id"].IsInt() )
	{
		return node["id"].GetInt();
	}

	return 0;
}

static bool questDialogueEditorCreateNewFile()
{
	questDialogueEditorRequestTransition(QUEST_DIALOGUE_PENDING_OPEN_WIZARD);
	return true;
}

static bool questDialogueEditorAddNode()
{
	if ( !questDialogueEditorDocument.IsObject() )
	{
		return false;
	}

	auto& allocator =
		questDialogueEditorDocument.GetAllocator();

	if ( !questDialogueEditorDocument.HasMember("nodes") )
	{
		rapidjson::Value nodes(
			rapidjson::kArrayType
		);
		questDialogueEditorDocument.AddMember(
			"nodes",
			nodes,
			allocator
		);
	}

	rapidjson::Value& nodes =
		questDialogueEditorDocument["nodes"];

	if ( !nodes.IsArray() )
	{
		return false;
	}

	int nextID = 0;
	for ( const auto& node : nodes.GetArray() )
	{
		if ( node.IsObject()
			&& node.HasMember("id")
			&& node["id"].IsInt() )
		{
			nextID = std::max(
				nextID,
				node["id"].GetInt() + 1
			);
		}
	}

	rapidjson::Value node(
		rapidjson::kObjectType
	);
	node.AddMember("id", nextID, allocator);

	rapidjson::Value nodeText;
	nodeText.SetString(
		"New dialogue node.",
		allocator
	);
	node.AddMember(
		"text",
		nodeText,
		allocator
	);
	node.AddMember("next", nextID, allocator);

	rapidjson::Value choices(
		rapidjson::kArrayType
	);
	node.AddMember(
		"choices",
		choices,
		allocator
	);

	nodes.PushBack(node, allocator);
	questDialogueEditorSelectedNode =
		static_cast<int>(nodes.Size()) - 1;
	questDialogueEditorSelectedChoice = -1;

	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorDeleteNode()
{
	if ( !questDialogueEditorDocument.IsObject()
		|| !questDialogueEditorDocument.HasMember("nodes")
		|| !questDialogueEditorDocument["nodes"].IsArray() )
	{
		return false;
	}

	rapidjson::Value& nodes =
		questDialogueEditorDocument["nodes"];

	if ( nodes.Size() <= 1
		|| questDialogueEditorSelectedNode < 0
		|| questDialogueEditorSelectedNode
			>= static_cast<int>(nodes.Size()) )
	{
		questDialogueEditorSetMessage(
			"A dialogue must keep at least one node."
		);
		return false;
	}

	const int deletedID =
		questDialogueEditorNodeIDAt(
			questDialogueEditorSelectedNode
		);

	for ( rapidjson::SizeType nodeIndex = 0;
		nodeIndex < nodes.Size();
		++nodeIndex )
	{
		if ( static_cast<int>(nodeIndex)
			== questDialogueEditorSelectedNode )
		{
			continue;
		}

		const rapidjson::Value& node = nodes[nodeIndex];
		if ( !node.IsObject() )
		{
			continue;
		}

		if ( node.HasMember("next")
			&& node["next"].IsInt()
			&& node["next"].GetInt() == deletedID )
		{
			questDialogueEditorSetMessage(
				"Another node still links to this node."
			);
			return false;
		}

		if ( node.HasMember("condition")
			&& node["condition"].IsObject() )
		{
			const rapidjson::Value& condition = node["condition"];
			for ( const char* branch : { "true_node", "false_node" } )
			{
				if ( condition.HasMember(branch)
					&& condition[branch].IsInt()
					&& condition[branch].GetInt() == deletedID )
				{
					questDialogueEditorSetMessage(
						"A node condition still branches to this node."
					);
					return false;
				}
			}
			if ( condition.HasMember("type")
				&& condition["type"].IsString()
				&& questEditorNormalizeID(condition["type"].GetString()) == "node_seen"
				&& condition.HasMember("node")
				&& condition["node"].IsString()
				&& questEditorNormalizeID(condition["node"].GetString())
					== "node_" + std::to_string(deletedID) )
			{
				questDialogueEditorSetMessage(
					"A node-seen condition still references this node."
				);
				return false;
			}
		}

		if ( node.HasMember("choices")
			&& node["choices"].IsArray() )
		{
			for ( const auto& choice :
				node["choices"].GetArray() )
			{
				if ( choice.IsObject()
					&& choice.HasMember("next")
					&& choice["next"].IsInt()
					&& choice["next"].GetInt()
						== deletedID )
				{
					questDialogueEditorSetMessage(
						"A choice still links to this node."
					);
					return false;
				}
			}
		}
	}

	if ( questDialogueEditorDocument.HasMember(
			"start_node"
		)
		&& questDialogueEditorDocument[
			"start_node"
		].IsInt()
		&& questDialogueEditorDocument[
			"start_node"
		].GetInt() == deletedID )
	{
		questDialogueEditorSetMessage(
			"The start node cannot be deleted."
		);
		return false;
	}

	nodes.Erase(
		nodes.Begin()
		+ questDialogueEditorSelectedNode
	);

	questDialogueEditorSelectedNode =
		std::max(
			0,
			std::min(
				questDialogueEditorSelectedNode,
				static_cast<int>(nodes.Size()) - 1
			)
		);
	questDialogueEditorSelectedChoice = -1;

	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorChoiceIDExistsGlobal(
	const std::string& candidate,
	const rapidjson::Value* ignoredChoice = nullptr
)
{
	if ( !questDialogueEditorDocument.IsObject()
		|| !questDialogueEditorDocument.HasMember("nodes")
		|| !questDialogueEditorDocument["nodes"].IsArray() )
	{
		return false;
	}
	const std::string normalizedCandidate = questEditorNormalizeID(candidate);
	for ( const rapidjson::Value& node :
		questDialogueEditorDocument["nodes"].GetArray() )
	{
		if ( !node.IsObject() || !node.HasMember("choices")
			|| !node["choices"].IsArray() ) continue;
		for ( const rapidjson::Value& choice : node["choices"].GetArray() )
		{
			if ( &choice != ignoredChoice && choice.IsObject() && choice.HasMember("id")
				&& choice["id"].IsString()
				&& questEditorNormalizeID(choice["id"].GetString())
					== normalizedCandidate )
			{
				return true;
			}
		}
	}
	return false;
}

static std::string questDialogueEditorUniqueChoiceID(
	const std::string& requestedBase
)
{
	const std::string base = requestedBase.empty() ? "choice" : requestedBase;
	std::string candidate = base;
	int suffix = 2;
	while ( questDialogueEditorChoiceIDExistsGlobal(candidate) )
	{
		candidate = base + "_" + std::to_string(suffix++);
	}
	return candidate;
}

static bool questDialogueEditorAddChoice()
{
	rapidjson::Value* node =
		questDialogueEditorSelectedNodeValue();

	if ( !node || !node->IsObject() )
	{
		return false;
	}

	auto& allocator =
		questDialogueEditorDocument.GetAllocator();

	if ( !node->HasMember("choices") )
	{
		rapidjson::Value choices(
			rapidjson::kArrayType
		);
		node->AddMember(
			"choices",
			choices,
			allocator
		);
	}

	rapidjson::Value& choices =
		(*node)["choices"];

	if ( !choices.IsArray() )
	{
		return false;
	}

	const int currentNodeID =
		node->HasMember("id")
		&& (*node)["id"].IsInt()
			? (*node)["id"].GetInt()
			: 0;

	rapidjson::Value choice(
		rapidjson::kObjectType
	);

	int choiceNumber = 1;
	std::string choiceID;
	do
	{
		choiceID = "choice_" + std::to_string(choiceNumber++);
	}
	while ( questDialogueEditorChoiceIDExistsGlobal(choiceID) );

	rapidjson::Value idValue;
	idValue.SetString(
		choiceID.c_str(),
		allocator
	);
	choice.AddMember("id", idValue, allocator);

	rapidjson::Value textValue;
	textValue.SetString(
		"New choice.",
		allocator
	);
	choice.AddMember(
		"text",
		textValue,
		allocator
	);
	choice.AddMember(
		"next",
		currentNodeID,
		allocator
	);

	choices.PushBack(choice, allocator);
	questDialogueEditorSelectedChoice =
		static_cast<int>(choices.Size()) - 1;

	return questDialogueEditorSaveDocument();
}


static bool questDialogueEditorDuplicateSelectedChoice()
{
	rapidjson::Value* node =
		questDialogueEditorSelectedNodeValue();

	if ( !node
		|| !node->IsObject()
		|| !node->HasMember("choices")
		|| !(*node)["choices"].IsArray()
		|| questDialogueEditorSelectedChoice < 0
		|| questDialogueEditorSelectedChoice
			>= static_cast<int>(
				(*node)["choices"].Size()
			) )
	{
		questDialogueEditorSetMessage(
			"Select a choice first."
		);
		return false;
	}

	rapidjson::Value& choices =
		(*node)["choices"];

	auto& allocator =
		questDialogueEditorDocument.GetAllocator();

	const rapidjson::Value& original =
		choices[
			static_cast<rapidjson::SizeType>(
				questDialogueEditorSelectedChoice
			)
		];

	rapidjson::Value duplicate(
		original,
		allocator
	);

	std::string originalID = "choice";

	if ( original.HasMember("id")
		&& original["id"].IsString() )
	{
		originalID = original["id"].GetString();
	}

	const std::string copiedID =
		questDialogueEditorUniqueChoiceID(
			(originalID.empty() ? "choice" : originalID) + "_copy"
		);

	if ( duplicate.HasMember("id") )
	{
		duplicate["id"].SetString(
			copiedID.c_str(),
			allocator
		);
	}
	else
	{
		rapidjson::Value id;
		id.SetString(
			copiedID.c_str(),
			allocator
		);
		duplicate.AddMember(
			"id",
			id,
			allocator
		);
	}

	const rapidjson::SizeType insertIndex =
		static_cast<rapidjson::SizeType>(
			questDialogueEditorSelectedChoice + 1
		);

	choices.PushBack(
		rapidjson::Value(),
		allocator
	);

	for ( rapidjson::SizeType index =
			choices.Size() - 1;
		index > insertIndex;
		--index )
	{
		choices[index].Swap(
			choices[index - 1]
		);
	}

	choices[insertIndex].Swap(duplicate);

	questDialogueEditorSelectedChoice =
		static_cast<int>(insertIndex);

	questDialogueEditorSetMessage(
		"Duplicated choice as "
		+ copiedID
		+ "."
	);

	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorMoveSelectedChoice(
	const int direction
)
{
	rapidjson::Value* node =
		questDialogueEditorSelectedNodeValue();

	if ( !node
		|| !node->IsObject()
		|| !node->HasMember("choices")
		|| !(*node)["choices"].IsArray()
		|| questDialogueEditorSelectedChoice < 0 )
	{
		questDialogueEditorSetMessage(
			"Select a choice first."
		);
		return false;
	}

	rapidjson::Value& choices =
		(*node)["choices"];

	const int destination =
		questDialogueEditorSelectedChoice
		+ direction;

	if ( destination < 0
		|| destination
			>= static_cast<int>(choices.Size()) )
	{
		questDialogueEditorSetMessage(
			"Choice is already at that edge."
		);
		return false;
	}

	choices[
		static_cast<rapidjson::SizeType>(
			questDialogueEditorSelectedChoice
		)
	].Swap(
		choices[
			static_cast<rapidjson::SizeType>(
				destination
			)
		]
	);

	questDialogueEditorSelectedChoice =
		destination;

	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorDuplicateSelectedNode()
{
	if ( !questDialogueEditorDocument.IsObject()
		|| !questDialogueEditorDocument.HasMember("nodes")
		|| !questDialogueEditorDocument["nodes"].IsArray()
		|| questDialogueEditorSelectedNode < 0
		|| questDialogueEditorSelectedNode
			>= static_cast<int>(
				questDialogueEditorDocument[
					"nodes"
				].Size()
			) )
	{
		questDialogueEditorSetMessage(
			"Select a node first."
		);
		return false;
	}

	rapidjson::Value& nodes =
		questDialogueEditorDocument["nodes"];

	int nextID = 0;

	for ( const rapidjson::Value& node :
		nodes.GetArray() )
	{
		if ( node.IsObject()
			&& node.HasMember("id")
			&& node["id"].IsInt() )
		{
			nextID =
				std::max(
					nextID,
					node["id"].GetInt() + 1
				);
		}
	}

	auto& allocator =
		questDialogueEditorDocument.GetAllocator();

	const rapidjson::Value& original =
		nodes[
			static_cast<rapidjson::SizeType>(
				questDialogueEditorSelectedNode
			)
		];

	rapidjson::Value duplicate(
		original,
		allocator
	);

	if ( duplicate.HasMember("id") )
	{
		duplicate["id"].SetInt(nextID);
	}
	else
	{
		duplicate.AddMember(
			"id",
			nextID,
			allocator
		);
	}

	if ( duplicate.HasMember("next")
		&& duplicate["next"].IsInt()
		&& original.HasMember("id")
		&& original["id"].IsInt()
		&& duplicate["next"].GetInt()
			== original["id"].GetInt() )
	{
		duplicate["next"].SetInt(nextID);
	}

	if ( duplicate.HasMember("condition") && duplicate["condition"].IsObject()
		&& original.HasMember("id") && original["id"].IsInt() )
	{
		for ( const char* branch : { "true_node", "false_node" } )
		{
			if ( duplicate["condition"].HasMember(branch)
				&& duplicate["condition"][branch].IsInt()
				&& duplicate["condition"][branch].GetInt() == original["id"].GetInt() )
			{
				duplicate["condition"][branch].SetInt(nextID);
			}
		}
	}

	if ( duplicate.HasMember("action") && duplicate["action"].IsObject() )
	{
		const std::string actionBase = "node_" + std::to_string(nextID) + "_once";
		std::string actionID = actionBase;
		int suffix = 2;
		auto actionIDExists = [&](const std::string& candidate)
		{
			for ( const rapidjson::Value& node : nodes.GetArray() )
			{
				if ( node.IsObject() && node.HasMember("action")
					&& node["action"].IsObject()
					&& node["action"].HasMember("id")
					&& node["action"]["id"].IsString()
					&& questEditorNormalizeID(node["action"]["id"].GetString())
						== questEditorNormalizeID(candidate) )
				{
					return true;
				}
			}
			return false;
		};
		while ( actionIDExists(actionID) )
		{
			actionID = actionBase + "_" + std::to_string(suffix++);
		}
		if ( duplicate["action"].HasMember("id") )
		{
			duplicate["action"]["id"].SetString(actionID.c_str(), allocator);
		}
		else
		{
			rapidjson::Value id;
			id.SetString(actionID.c_str(), allocator);
			duplicate["action"].AddMember("id", id, allocator);
		}
	}

	if ( duplicate.HasMember("choices")
		&& duplicate["choices"].IsArray() )
	{
		int copiedChoice = 1;

		for ( rapidjson::Value& choice :
			duplicate["choices"].GetArray() )
		{
			if ( !choice.IsObject() )
			{
				continue;
			}

			const std::string choiceID = questDialogueEditorUniqueChoiceID(
				"node_"
					+ std::to_string(nextID)
					+ "_choice_"
					+ std::to_string(copiedChoice++));

			if ( choice.HasMember("id") )
			{
				choice["id"].SetString(
					choiceID.c_str(),
					allocator
				);
			}
			else
			{
				rapidjson::Value id;
				id.SetString(
					choiceID.c_str(),
					allocator
				);
				choice.AddMember(
					"id",
					id,
					allocator
				);
			}

			if ( choice.HasMember("next")
				&& choice["next"].IsInt()
				&& original.HasMember("id")
				&& original["id"].IsInt()
				&& choice["next"].GetInt()
					== original["id"].GetInt() )
			{
				choice["next"].SetInt(nextID);
			}
		}
	}

	nodes.PushBack(
		duplicate,
		allocator
	);

	questDialogueEditorSelectedNode =
		static_cast<int>(nodes.Size()) - 1;
	questDialogueEditorSelectedChoice = -1;

	questDialogueEditorSetMessage(
		"Duplicated node as ID "
		+ std::to_string(nextID)
		+ "."
	);

	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorDeleteChoice()
{
	rapidjson::Value* node =
		questDialogueEditorSelectedNodeValue();

	if ( !node
		|| !node->IsObject()
		|| !node->HasMember("choices")
		|| !(*node)["choices"].IsArray()
		|| questDialogueEditorSelectedChoice < 0
		|| questDialogueEditorSelectedChoice
			>= static_cast<int>(
				(*node)["choices"].Size()
			) )
	{
		questDialogueEditorSetMessage(
			"Select a choice first."
		);
		return false;
	}

	rapidjson::Value& choices =
		(*node)["choices"];

	choices.Erase(
		choices.Begin()
		+ questDialogueEditorSelectedChoice
	);

	questDialogueEditorSelectedChoice =
		choices.Empty()
			? -1
			: std::min(
				questDialogueEditorSelectedChoice,
				static_cast<int>(choices.Size()) - 1
			);

	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorCycleChoiceNext()
{
	rapidjson::Value* node =
		questDialogueEditorSelectedNodeValue();

	if ( !node
		|| !node->IsObject()
		|| !node->HasMember("choices")
		|| !(*node)["choices"].IsArray()
		|| questDialogueEditorSelectedChoice < 0
		|| questDialogueEditorSelectedChoice
			>= static_cast<int>(
				(*node)["choices"].Size()
			)
		|| !questDialogueEditorDocument.HasMember(
			"nodes"
		)
		|| !questDialogueEditorDocument[
			"nodes"
		].IsArray()
		|| questDialogueEditorDocument[
			"nodes"
		].Empty() )
	{
		questDialogueEditorSetMessage(
			"Select a choice first."
		);
		return false;
	}

	rapidjson::Value& choice =
		(*node)["choices"][
			static_cast<rapidjson::SizeType>(
				questDialogueEditorSelectedChoice
			)
		];

	int currentID = 0;
	if ( choice.HasMember("next")
		&& choice["next"].IsInt() )
	{
		currentID = choice["next"].GetInt();
	}

	rapidjson::Value& nodes =
		questDialogueEditorDocument["nodes"];

	int nextIndex = 0;
	for ( rapidjson::SizeType index = 0;
		index < nodes.Size();
		++index )
	{
		if ( nodes[index].IsObject()
			&& nodes[index].HasMember("id")
			&& nodes[index]["id"].IsInt()
			&& nodes[index]["id"].GetInt()
				== currentID )
		{
			nextIndex =
				(static_cast<int>(index) + 1)
				% static_cast<int>(nodes.Size());
			break;
		}
	}

	const int nextID =
		questDialogueEditorNodeIDAt(nextIndex);

	if ( choice.HasMember("next") )
	{
		choice["next"].SetInt(nextID);
	}
	else
	{
		choice.AddMember(
			"next",
			nextID,
			questDialogueEditorDocument
				.GetAllocator()
		);
	}

	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorCycleChoicePrevious()
{
    rapidjson::Value* node =
        questDialogueEditorSelectedNodeValue();

    if ( !node
        || !node->IsObject()
        || !node->HasMember("choices")
        || !(*node)["choices"].IsArray()
        || questDialogueEditorSelectedChoice < 0
        || questDialogueEditorSelectedChoice
            >= static_cast<int>((*node)["choices"].Size())
        || !questDialogueEditorDocument.HasMember("nodes")
        || !questDialogueEditorDocument["nodes"].IsArray()
        || questDialogueEditorDocument["nodes"].Empty() )
    {
        questDialogueEditorSetMessage(
            "Select a choice first."
        );
        return false;
    }

    rapidjson::Value& choice =
        (*node)["choices"][
            static_cast<rapidjson::SizeType>(
                questDialogueEditorSelectedChoice
            )
        ];

    int currentID = 0;
    if ( choice.HasMember("next")
        && choice["next"].IsInt() )
    {
        currentID = choice["next"].GetInt();
    }

    rapidjson::Value& nodes =
        questDialogueEditorDocument["nodes"];

    int previousIndex =
        static_cast<int>(nodes.Size()) - 1;

    for ( rapidjson::SizeType index = 0;
        index < nodes.Size();
        ++index )
    {
        if ( nodes[index].IsObject()
            && nodes[index].HasMember("id")
            && nodes[index]["id"].IsInt()
            && nodes[index]["id"].GetInt() == currentID )
        {
            previousIndex =
                (static_cast<int>(index)
                    + static_cast<int>(nodes.Size()) - 1)
                % static_cast<int>(nodes.Size());
            break;
        }
    }

    const int previousID =
        questDialogueEditorNodeIDAt(previousIndex);

    if ( choice.HasMember("next") )
    {
        choice["next"].SetInt(previousID);
    }
    else
    {
        choice.AddMember(
            "next",
            previousID,
            questDialogueEditorDocument.GetAllocator()
        );
    }

    return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorToggleRecruit()
{
	rapidjson::Value* node =
		questDialogueEditorSelectedNodeValue();

	if ( !node
		|| !node->IsObject()
		|| !node->HasMember("choices")
		|| !(*node)["choices"].IsArray()
		|| questDialogueEditorSelectedChoice < 0
		|| questDialogueEditorSelectedChoice
			>= static_cast<int>(
				(*node)["choices"].Size()
			) )
	{
		questDialogueEditorSetMessage(
			"Select a choice first."
		);
		return false;
	}

	rapidjson::Value& choice =
		(*node)["choices"][
			static_cast<rapidjson::SizeType>(
				questDialogueEditorSelectedChoice
			)
		];

	auto& allocator =
		questDialogueEditorDocument.GetAllocator();

	if ( !choice.HasMember("action") )
	{
		rapidjson::Value action(
			rapidjson::kObjectType
		);
		choice.AddMember(
			"action",
			action,
			allocator
		);
	}

	rapidjson::Value& action =
		choice["action"];

	if ( !action.IsObject() )
	{
		return false;
	}

	bool enabled = false;
	if ( action.HasMember("recruit_npc")
		&& action["recruit_npc"].IsBool() )
	{
		enabled =
			action["recruit_npc"].GetBool();
		action["recruit_npc"].SetBool(
			!enabled
		);
	}
	else
	{
		action.AddMember(
			"recruit_npc",
			true,
			allocator
		);
	}

	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorToggleRepeatable()
{
	if ( !questDialogueEditorDocument.IsObject()
		|| !questDialogueEditorDocument.HasMember("quest")
		|| !questDialogueEditorDocument["quest"].IsObject() )
	{
		return false;
	}

	rapidjson::Value& quest =
		questDialogueEditorDocument["quest"];

	if ( quest.HasMember("repeatable")
		&& quest["repeatable"].IsBool() )
	{
		quest["repeatable"].SetBool(
			!quest["repeatable"].GetBool()
		);
	}
	else
	{
		quest.AddMember(
			"repeatable",
			true,
			questDialogueEditorDocument
				.GetAllocator()
		);
	}

	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorCycleGiverMarker()
{
	if ( !questDialogueEditorDocument.IsObject()
		|| !questDialogueEditorDocument.HasMember("quest")
		|| !questDialogueEditorDocument["quest"].IsObject() )
	{
		return false;
	}

	rapidjson::Value& quest =
		questDialogueEditorDocument["quest"];

	auto& allocator =
		questDialogueEditorDocument.GetAllocator();

	if ( !quest.HasMember("origin") )
	{
		rapidjson::Value origin(
			rapidjson::kObjectType
		);

		rapidjson::Value label;
		label.SetString(
			"Quest Giver",
			allocator
		);
		origin.AddMember(
			"label",
			label,
			allocator
		);

		rapidjson::Value mapValue;
		const std::string mapName =
			questEditorCurrentMapFilename();
		mapValue.SetString(
			mapName.c_str(),
			allocator
		);
		origin.AddMember(
			"map",
			mapValue,
			allocator
		);

		quest.AddMember(
			"origin",
			origin,
			allocator
		);
	}

	rapidjson::Value& origin =
		quest["origin"];

	const bool currentlyDynamic =
		origin.IsObject()
		&& origin.HasMember("track_npc")
		&& origin["track_npc"].IsBool()
		&& origin["track_npc"].GetBool();

	const bool currentlyStatic =
		origin.IsObject()
		&& origin.HasMember("x")
		&& origin["x"].IsInt()
		&& origin.HasMember("y")
		&& origin["y"].IsInt();

	if ( currentlyDynamic )
	{
		origin.RemoveMember("track_npc");
		origin.RemoveMember(
			"npc_persistent_id"
		);
		origin.RemoveMember("x");
		origin.RemoveMember("y");
	}
	else if ( currentlyStatic )
	{
		origin.RemoveMember("x");
		origin.RemoveMember("y");

		if ( selectedEntity[0]
			&& selectedEntity[0]->persistentID > 0 )
		{
			origin.AddMember(
				"track_npc",
				true,
				allocator
			);
			origin.AddMember(
				"npc_persistent_id",
				selectedEntity[0]->persistentID,
				allocator
			);
		}
		else
		{
			questDialogueEditorSetMessage(
				"Select an NPC with a persistent ID for Follow NPC."
			);
			return false;
		}
	}
	else
	{
		if ( !selectedEntity[0] )
		{
			quest.RemoveMember("origin");

			questDialogueEditorSetMessage(
				"No entity selected; giver marker turned off instead of using tile 0,0."
			);

			return questDialogueEditorSaveDocument();
		}

		const int markerX =
			std::max(
				0,
				static_cast<int>(
					floor(
						selectedEntity[0]->x
						/ 16.0
					)
				)
			);

		const int markerY =
			std::max(
				0,
				static_cast<int>(
					floor(
						selectedEntity[0]->y
						/ 16.0
					)
				)
			);

		origin.AddMember(
			"x",
			markerX,
			allocator
		);
		origin.AddMember(
			"y",
			markerY,
			allocator
		);
	}

	return questDialogueEditorSaveDocument();
}


static rapidjson::Value* questDialogueEditorQuestValue()
{
	if ( !questDialogueEditorDocument.IsObject()
		|| !questDialogueEditorDocument.HasMember("quest")
		|| !questDialogueEditorDocument["quest"].IsObject() )
	{
		return nullptr;
	}

	return &questDialogueEditorDocument["quest"];
}


static bool questDialogueEditorSetQuestGiverTile(
	const int tileX,
	const int tileY,
	const int playableFloor
)
{
	rapidjson::Value* quest =
		questDialogueEditorQuestValue();

	if ( !quest )
	{
		questDialogueEditorSetMessage(
			"This dialogue has no quest object."
		);
		return false;
	}

	if ( tileX < 0
		|| tileY < 0
		|| tileX >= static_cast<int>(map.width)
		|| tileY >= static_cast<int>(map.height) )
	{
		questDialogueEditorSetMessage("Selected tile is outside the map.");
		return false;
	}

	auto& allocator =
		questDialogueEditorDocument.GetAllocator();

	if ( !quest->HasMember("origin") )
	{
		rapidjson::Value origin(
			rapidjson::kObjectType
		);

		quest->AddMember(
			"origin",
			origin,
			allocator
		);
	}
	else if ( !(*quest)["origin"].IsObject() )
	{
		(*quest)["origin"].SetObject();
	}

	rapidjson::Value& origin =
		(*quest)["origin"];

	auto setString =
		[
			&allocator
		](
			rapidjson::Value& object,
			const char* member,
			const std::string& value
		)
		{
			if ( object.HasMember(member) )
			{
				object[member].SetString(
					value.c_str(),
					allocator
				);
			}
			else
			{
				rapidjson::Value key;
				key.SetString(member, allocator);

				rapidjson::Value stored;
				stored.SetString(
					value.c_str(),
					allocator
				);

				object.AddMember(
					key,
					stored,
					allocator
				);
			}
		};

	auto setInt =
		[
			&allocator
		](
			rapidjson::Value& object,
			const char* member,
			const int value
		)
		{
			if ( object.HasMember(member) )
			{
				object[member].SetInt(value);
			}
			else
			{
				rapidjson::Value key;
				key.SetString(member, allocator);

				object.AddMember(
					key,
					value,
					allocator
				);
			}
		};

	setString(
		origin,
		"label",
		"Quest Giver"
	);

	setString(
		origin,
		"map",
		questEditorCurrentMapFilename()
	);

	setInt(origin, "x", tileX);
	setInt(origin, "y", tileY);
	setInt(origin, "playable_floor", std::max(0, playableFloor));
	if ( !origin.HasMember("floor_visibility") )
	{
		setString(origin, "floor_visibility", "same_floor");
	}

	origin.RemoveMember("track_npc");
	origin.RemoveMember("npc_persistent_id");

	if ( !questDialogueEditorSaveDocument() )
	{
		return false;
	}

	questDialogueEditorSetMessage(
		"Static quest giver marker set to tile "
		+ std::to_string(tileX)
		+ ", "
		+ std::to_string(tileY)
		+ " on playable floor " + std::to_string(std::max(0, playableFloor)) + "."
	);

	return true;
}

static bool questDialogueEditorBeginMarkerPick(
	const QuestDialogueMarkerPickKind kind
)
{
	if ( kind == QUEST_DIALOGUE_MARKER_PICK_ORIGIN
		&& !questDialogueEditorQuestValue() )
	{
		questDialogueEditorSetMessage("This dialogue has no quest object.");
		return false;
	}
	if ( kind == QUEST_DIALOGUE_MARKER_PICK_OBJECTIVE
		&& questDialogueEditorSelectedObjective < 0 )
	{
		questDialogueEditorSetMessage("Select an objective first.");
		return false;
	}

	questDialogueEditorMarkerPick = kind;
	questDialogueEditorMarkerPickObjective =
		kind == QUEST_DIALOGUE_MARKER_PICK_OBJECTIVE
			? questDialogueEditorSelectedObjective : -1;
	questDialogueEditorMarkerPickRestore3D = mode3d;
	questDialogueEditorMarkerPickPlayableFloor =
		map.playableFloors.hasFloor(static_cast<PlayableFloorId>(drawlayer))
			? drawlayer : DEFAULT_PLAYABLE_FLOOR;
	mode3d = false;
	SDL_StopTextInput();
	inputstr = nullptr;
	std::snprintf(message, sizeof(message),
		"MARKER PICK floor %d: click tile; U/P changes floor; right-click/Esc cancels.",
		questDialogueEditorMarkerPickPlayableFloor);
	messagetime = 1000000;
	buttonCloseSubwindow(nullptr);
	return true;
}

static bool questDialogueEditorUseCursorTileAsQuestGiver()
{
	return questDialogueEditorBeginMarkerPick(
		QUEST_DIALOGUE_MARKER_PICK_ORIGIN);
}

static std::string questDialogueEditorGiverMarkerSummary()
{
	rapidjson::Value* quest =
		questDialogueEditorQuestValue();

	if ( !quest
		|| !quest->HasMember("origin")
		|| !(*quest)["origin"].IsObject() )
	{
		return "Giver marker: Off";
	}

	const rapidjson::Value& origin =
		(*quest)["origin"];

	if ( origin.HasMember("track_npc")
		&& origin["track_npc"].IsBool()
		&& origin["track_npc"].GetBool()
		&& origin.HasMember("npc_persistent_id")
		&& origin["npc_persistent_id"].IsInt() )
	{
		return "Giver follows NPC ID "
			+ std::to_string(
				origin["npc_persistent_id"].GetInt()
			);
	}

	if ( origin.HasMember("x")
		&& origin["x"].IsInt()
		&& origin.HasMember("y")
		&& origin["y"].IsInt() )
	{
		return "Static giver tile "
			+ std::to_string(origin["x"].GetInt())
			+ ", "
			+ std::to_string(origin["y"].GetInt());
	}

	return "Giver marker: Off";
}

static rapidjson::Value* questDialogueEditorSelectedObjectiveValue()
{
	rapidjson::Value* quest =
		questDialogueEditorQuestValue();

	if ( !quest
		|| !quest->HasMember("objectives")
		|| !(*quest)["objectives"].IsArray()
		|| questDialogueEditorSelectedObjective < 0
		|| questDialogueEditorSelectedObjective
			>= static_cast<int>(
				(*quest)["objectives"].Size()
			) )
	{
		return nullptr;
	}

	return &(*quest)["objectives"][
		static_cast<rapidjson::SizeType>(
			questDialogueEditorSelectedObjective
		)
	];
}

static bool questDialogueEditorAddObjective()
{
	rapidjson::Value* quest =
		questDialogueEditorQuestValue();

	if ( !quest )
	{
		return false;
	}

	auto& allocator =
		questDialogueEditorDocument.GetAllocator();

	if ( !quest->HasMember("objectives") )
	{
		rapidjson::Value objectives(
			rapidjson::kArrayType
		);
		quest->AddMember(
			"objectives",
			objectives,
			allocator
		);
	}

	rapidjson::Value& objectives =
		(*quest)["objectives"];

	if ( !objectives.IsArray() )
	{
		return false;
	}

	int number = static_cast<int>(objectives.Size()) + 1;
	std::string id;
	for ( ;; ++number )
	{
		id = "objective_" + std::to_string(number);
		const bool exists = std::any_of(objectives.Begin(), objectives.End(),
			[&id](const rapidjson::Value& existing)
			{
				return existing.IsObject() && existing.HasMember("id")
					&& existing["id"].IsString()
					&& questEditorNormalizeID(existing["id"].GetString())
						== questEditorNormalizeID(id);
			});
		if ( !exists ) break;
	}

	rapidjson::Value objective(
		rapidjson::kObjectType
	);

	rapidjson::Value idValue;
	idValue.SetString(
		id.c_str(),
		allocator
	);
	objective.AddMember(
		"id",
		idValue,
		allocator
	);

	rapidjson::Value textValue;
	textValue.SetString(
		"New objective.",
		allocator
	);
	objective.AddMember(
		"text",
		textValue,
		allocator
	);

	objective.AddMember(
		"stage",
		0,
		allocator
	);
	objective.AddMember(
		"optional",
		false,
		allocator
	);

	objectives.PushBack(
		objective,
		allocator
	);

	questDialogueEditorSelectedObjective =
		static_cast<int>(objectives.Size()) - 1;

	return questDialogueEditorSaveDocument();
}


static std::string questDialogueEditorUniqueObjectiveID(
	const rapidjson::Value& objectives,
	const std::string& base
)
{
	std::string candidate =
		base.empty()
			? "objective_copy"
			: base + "_copy";

	int suffix = 2;

	auto exists =
		[
			&objectives
		](
			const std::string& id
		)
		{
			for ( const rapidjson::Value& objective :
				objectives.GetArray() )
			{
				if ( objective.IsObject()
					&& objective.HasMember("id")
					&& objective["id"].IsString()
					&& questEditorNormalizeID(id)
						== questEditorNormalizeID(objective["id"].GetString()) )
				{
					return true;
				}
			}

			return false;
		};

	while ( exists(candidate) )
	{
		candidate =
			(base.empty()
				? "objective_copy"
				: base + "_copy")
			+ "_"
			+ std::to_string(suffix++);
	}

	return candidate;
}

static bool questDialogueEditorDuplicateSelectedObjective()
{
	rapidjson::Value* quest =
		questDialogueEditorQuestValue();

	if ( !quest
		|| !quest->HasMember("objectives")
		|| !(*quest)["objectives"].IsArray()
		|| questDialogueEditorSelectedObjective < 0
		|| questDialogueEditorSelectedObjective
			>= static_cast<int>(
				(*quest)["objectives"].Size()
			) )
	{
		questDialogueEditorSetMessage(
			"Select an objective first."
		);
		return false;
	}

	rapidjson::Value& objectives =
		(*quest)["objectives"];

	auto& allocator =
		questDialogueEditorDocument.GetAllocator();

	const rapidjson::Value& original =
		objectives[
			static_cast<rapidjson::SizeType>(
				questDialogueEditorSelectedObjective
			)
		];

	rapidjson::Value duplicate(
		original,
		allocator
	);

	std::string originalID = "objective";

	if ( original.HasMember("id")
		&& original["id"].IsString() )
	{
		originalID = original["id"].GetString();
	}

	const std::string copiedID =
		questDialogueEditorUniqueObjectiveID(
			objectives,
			originalID
		);

	if ( duplicate.HasMember("id") )
	{
		duplicate["id"].SetString(
			copiedID.c_str(),
			allocator
		);
	}
	else
	{
		rapidjson::Value id;
		id.SetString(
			copiedID.c_str(),
			allocator
		);
		duplicate.AddMember(
			"id",
			id,
			allocator
		);
	}

	const rapidjson::SizeType insertIndex =
		static_cast<rapidjson::SizeType>(
			questDialogueEditorSelectedObjective + 1
		);

	objectives.PushBack(
		rapidjson::Value(),
		allocator
	);

	for ( rapidjson::SizeType index =
			objectives.Size() - 1;
		index > insertIndex;
		--index )
	{
		objectives[index].Swap(
			objectives[index - 1]
		);
	}

	objectives[insertIndex].Swap(duplicate);

	questDialogueEditorSelectedObjective =
		static_cast<int>(insertIndex);

	questDialogueEditorSetMessage(
		"Duplicated objective as "
		+ copiedID
		+ "."
	);

	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorMoveSelectedObjective(
	const int direction
)
{
	rapidjson::Value* quest =
		questDialogueEditorQuestValue();

	if ( !quest
		|| !quest->HasMember("objectives")
		|| !(*quest)["objectives"].IsArray()
		|| questDialogueEditorSelectedObjective < 0 )
	{
		questDialogueEditorSetMessage(
			"Select an objective first."
		);
		return false;
	}

	rapidjson::Value& objectives =
		(*quest)["objectives"];

	const int destination =
		questDialogueEditorSelectedObjective
		+ direction;

	if ( destination < 0
		|| destination
			>= static_cast<int>(objectives.Size()) )
	{
		questDialogueEditorSetMessage(
			"Objective is already at that edge."
		);
		return false;
	}

	objectives[
		static_cast<rapidjson::SizeType>(
			questDialogueEditorSelectedObjective
		)
	].Swap(
		objectives[
			static_cast<rapidjson::SizeType>(
				destination
			)
		]
	);

	questDialogueEditorSelectedObjective =
		destination;

	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorDeleteObjective()
{
	rapidjson::Value* quest =
		questDialogueEditorQuestValue();

	if ( !quest
		|| !quest->HasMember("objectives")
		|| !(*quest)["objectives"].IsArray()
		|| questDialogueEditorSelectedObjective < 0
		|| questDialogueEditorSelectedObjective
			>= static_cast<int>(
				(*quest)["objectives"].Size()
			) )
	{
		questDialogueEditorSetMessage(
			"Select an objective first."
		);
		return false;
	}

	rapidjson::Value& objectives =
		(*quest)["objectives"];
	const rapidjson::Value& selected = objectives[
		static_cast<rapidjson::SizeType>(questDialogueEditorSelectedObjective)];
	const std::string objectiveID = selected.IsObject()
		&& selected.HasMember("id") && selected["id"].IsString()
		? selected["id"].GetString() : std::string{};
	const std::string questID = questDialogueEditorDocument.HasMember("quest_id")
		&& questDialogueEditorDocument["quest_id"].IsString()
		? questDialogueEditorDocument["quest_id"].GetString() : std::string{};

	auto conditionReferencesObjective =
		[&](const rapidjson::Value& condition)
		{
			if ( !condition.IsObject() || !condition.HasMember("type")
				|| !condition["type"].IsString()
				|| !condition.HasMember("objective")
				|| !condition["objective"].IsString()
				|| questEditorNormalizeID(objectiveID)
					!= questEditorNormalizeID(condition["objective"].GetString()) ) return false;
			const std::string type = questEditorNormalizeID(
				condition["type"].GetString());
			if ( type != "objective_completed" && type != "objective_incomplete" )
				return false;
			return questID.empty() || !condition.HasMember("quest")
				|| !condition["quest"].IsString()
				|| questEditorNormalizeID(questID)
					== questEditorNormalizeID(condition["quest"].GetString());
		};

	if ( !objectiveID.empty() && questDialogueEditorDocument.HasMember("nodes")
		&& questDialogueEditorDocument["nodes"].IsArray() )
	{
		for ( const rapidjson::Value& node :
			questDialogueEditorDocument["nodes"].GetArray() )
		{
			if ( !node.IsObject() || !node.HasMember("choices")
				|| !node["choices"].IsArray() ) continue;
			for ( const rapidjson::Value& choice : node["choices"].GetArray() )
			{
				if ( !choice.IsObject() ) continue;
				if ( choice.HasMember("condition")
					&& conditionReferencesObjective(choice["condition"]) )
				{
					questDialogueEditorSetMessage(
						"A choice condition still references this objective."
					);
					return false;
				}
				if ( choice.HasMember("conditions")
					&& choice["conditions"].IsArray() )
				{
					for ( const rapidjson::Value& condition :
						choice["conditions"].GetArray() )
					{
						if ( conditionReferencesObjective(condition) )
						{
							questDialogueEditorSetMessage(
								"A choice condition still references this objective."
							);
							return false;
						}
					}
				}
				if ( choice.HasMember("action") && choice["action"].IsObject() )
				{
					const rapidjson::Value& action = choice["action"];
					for ( const char* field : { "objective_complete", "objective_clear" } )
					{
						if ( action.HasMember(field) && action[field].IsString()
							&& questEditorNormalizeID(objectiveID)
								== questEditorNormalizeID(action[field].GetString()) )
						{
							questDialogueEditorSetMessage(
								"A choice action still references this objective."
							);
							return false;
						}
					}
				}
			}
		}
	}

	objectives.Erase(
		objectives.Begin()
		+ questDialogueEditorSelectedObjective
	);

	questDialogueEditorSelectedObjective =
		objectives.Empty()
			? -1
			: std::min(
				questDialogueEditorSelectedObjective,
				static_cast<int>(objectives.Size()) - 1
			);

	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorToggleObjectiveOptional()
{
	rapidjson::Value* objective =
		questDialogueEditorSelectedObjectiveValue();

	if ( !objective || !objective->IsObject() )
	{
		questDialogueEditorSetMessage(
			"Select an objective first."
		);
		return false;
	}

	auto& allocator =
		questDialogueEditorDocument.GetAllocator();

	if ( objective->HasMember("optional")
		&& (*objective)["optional"].IsBool() )
	{
		(*objective)["optional"].SetBool(
			!(*objective)["optional"].GetBool()
		);
	}
	else
	{
		objective->AddMember(
			"optional",
			true,
			allocator
		);
	}

	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorToggleObjectiveMarker()
{
	rapidjson::Value* objective =
		questDialogueEditorSelectedObjectiveValue();

	if ( !objective || !objective->IsObject() )
	{
		questDialogueEditorSetMessage(
			"Select an objective first."
		);
		return false;
	}

	if ( objective->HasMember("map_marker") )
	{
		objective->RemoveMember("map_marker");
		return questDialogueEditorSaveDocument();
	}

	return questDialogueEditorBeginMarkerPick(
		QUEST_DIALOGUE_MARKER_PICK_OBJECTIVE);
}

static rapidjson::Value* questDialogueEditorSelectedChoiceValue()
{
	rapidjson::Value* node =
		questDialogueEditorSelectedNodeValue();

	if ( !node
		|| !node->IsObject()
		|| !node->HasMember("choices")
		|| !(*node)["choices"].IsArray()
		|| questDialogueEditorSelectedChoice < 0
		|| questDialogueEditorSelectedChoice
			>= static_cast<int>(
				(*node)["choices"].Size()
			) )
	{
		return nullptr;
	}

	return &(*node)["choices"][
		static_cast<rapidjson::SizeType>(
			questDialogueEditorSelectedChoice
		)
	];
}

static rapidjson::Value* questDialogueEditorSelectedConditionValue()
{
    rapidjson::Value* choice = questDialogueEditorSelectedChoiceValue();
    if ( !choice || !choice->IsObject() )
    {
        return nullptr;
    }

    const bool singular = choice->HasMember("condition")
        && (*choice)["condition"].IsObject();
    const int arrayCount = choice->HasMember("conditions")
        && (*choice)["conditions"].IsArray()
        ? static_cast<int>((*choice)["conditions"].Size()) : 0;
    const int total = (singular ? 1 : 0) + arrayCount;
    if ( total == 0 ) return nullptr;
    questDialogueEditorSelectedConditionIndex = std::max(0,
        std::min(questDialogueEditorSelectedConditionIndex, total - 1));
    if ( singular && questDialogueEditorSelectedConditionIndex == 0 )
        return &(*choice)["condition"];
    const int arrayIndex = questDialogueEditorSelectedConditionIndex
        - (singular ? 1 : 0);
    return &(*choice)["conditions"][static_cast<rapidjson::SizeType>(arrayIndex)];

    return nullptr;
}

static rapidjson::Value* questDialogueEditorSelectedRuleCondition()
{
	if ( questDialogueEditorRuleOwnerNode )
	{
		rapidjson::Value* node = questDialogueEditorSelectedNodeValue();
		if ( node && node->IsObject() && node->HasMember("condition")
			&& (*node)["condition"].IsObject() )
		{
			return &(*node)["condition"];
		}
		return nullptr;
	}
	return questDialogueEditorSelectedConditionValue();
}

static rapidjson::Value& questDialogueEditorEnsureConditionArray(
    rapidjson::Value& choice
)
{
    auto& allocator = questDialogueEditorDocument.GetAllocator();
    if ( !choice.HasMember("conditions") )
    {
        rapidjson::Value conditions(rapidjson::kArrayType);
        if ( choice.HasMember("condition")
            && choice["condition"].IsObject() )
        {
            rapidjson::Value migrated;
            migrated.CopyFrom(choice["condition"], allocator);
            conditions.PushBack(migrated, allocator);
            choice.RemoveMember("condition");
        }
        choice.AddMember("conditions", conditions, allocator);
    }
    else if ( !choice["conditions"].IsArray() )
    {
        choice["conditions"].SetArray();
    }
    return choice["conditions"];
}

static void questDialogueEditorCycleSelectedCondition(const int direction)
{
    rapidjson::Value* choice = questDialogueEditorSelectedChoiceValue();
    if ( !choice || !choice->IsObject() )
    {
        questDialogueEditorSelectedConditionIndex = 0;
        return;
    }
    const int count = (choice->HasMember("condition")
        && (*choice)["condition"].IsObject() ? 1 : 0)
        + (choice->HasMember("conditions") && (*choice)["conditions"].IsArray()
            ? static_cast<int>((*choice)["conditions"].Size()) : 0);
    if ( count <= 0 )
    {
        questDialogueEditorSelectedConditionIndex = 0;
        return;
    }
    questDialogueEditorSelectedConditionIndex =
        (questDialogueEditorSelectedConditionIndex + direction + count) % count;
}

static std::string questDialogueEditorChoiceConditionName()
{
    rapidjson::Value* condition = questDialogueEditorSelectedConditionValue();
    if ( !condition || !condition->IsObject()
        || !condition->HasMember("type")
        || !(*condition)["type"].IsString() )
    {
        return "None";
    }
    return questEditorNormalizeID((*condition)["type"].GetString());
}

static std::vector<std::string> questDialogueEditorChoiceActionMembers()
{
    std::vector<std::string> members;
    rapidjson::Value* choice = questDialogueEditorSelectedChoiceValue();
    if ( !choice || !choice->IsObject()
        || !choice->HasMember("action")
        || !(*choice)["action"].IsObject() )
    {
        return members;
    }

    const rapidjson::Value& action = (*choice)["action"];
    for ( auto member = action.MemberBegin(); member != action.MemberEnd(); ++member )
    {
        members.emplace_back(member->name.GetString());
    }
    return members;
}

static rapidjson::Value* questDialogueEditorSelectedRuleAction()
{
	if ( questDialogueEditorRuleOwnerNode )
	{
		rapidjson::Value* node = questDialogueEditorSelectedNodeValue();
		if ( node && node->IsObject() && node->HasMember("action")
			&& (*node)["action"].IsObject() )
		{
			return &(*node)["action"];
		}
		return nullptr;
	}

	rapidjson::Value* choice = questDialogueEditorSelectedChoiceValue();
	if ( choice && choice->IsObject() && choice->HasMember("action")
		&& (*choice)["action"].IsObject() )
	{
		return &(*choice)["action"];
	}
	return nullptr;
}

static std::vector<std::string> questDialogueEditorRuleActionMembers()
{
	std::vector<std::string> members;
	rapidjson::Value* action = questDialogueEditorSelectedRuleAction();
	if ( !action )
	{
		return members;
	}
	for ( auto member = action->MemberBegin(); member != action->MemberEnd(); ++member )
	{
		if ( questDialogueEditorRuleOwnerNode
			&& std::string(member->name.GetString()) == "id" )
		{
			continue;
		}
		members.emplace_back(member->name.GetString());
	}
	return members;
}

static std::string questDialogueEditorSelectedRuleActionMember()
{
	const std::vector<std::string> members =
		questDialogueEditorRuleActionMembers();
	if ( members.empty() )
	{
		questDialogueEditorSelectedActionIndex = 0;
		return {};
	}
	questDialogueEditorSelectedActionIndex = std::max(0,
		std::min(questDialogueEditorSelectedActionIndex,
			static_cast<int>(members.size()) - 1));
	return members[questDialogueEditorSelectedActionIndex];
}

static std::string questDialogueEditorActionDisplayName(const std::string& member)
{
    if ( member == "recruit_npc" ) { return "Recruit NPC"; }
    if ( member == "quest_start" ) { return "Start Quest"; }
    if ( member == "quest_accept" ) { return "Accept Quest"; }
    if ( member == "quest_complete" ) { return "Complete Quest"; }
    if ( member == "quest_fail" ) { return "Fail Quest"; }
    if ( member == "quest_reset" ) { return "Reset Quest"; }
    if ( member == "quest_stage" ) { return "Set Quest Stage"; }
    if ( member == "reward_gold" ) { return "Reward Gold"; }
    if ( member == "reward_item" ) { return "Reward Item"; }
    if ( member == "remove_gold" ) { return "Remove Gold"; }
    if ( member == "remove_item" ) { return "Remove Item"; }
    if ( member == "objective_complete" ) { return "Complete Objective"; }
    if ( member == "objective_clear" ) { return "Clear Objective"; }
    if ( member == "set_world_flag" ) { return "Set World Flag"; }
    if ( member == "set_npc_flag" ) { return "Set NPC Flag"; }
    if ( member == "set_world_variable" || member == "add_world_variable" ) { return "World Variable"; }
    if ( member == "set_npc_variable" || member == "add_npc_variable" ) { return "NPC Variable"; }
    if ( member == "set_quest_variable" || member == "add_quest_variable" ) { return "Quest Variable"; }
    if ( member == "status_effect" ) { return "Status Effect"; }
    if ( member == "set_power" ) { return "Power Tile"; }
    return member;
}

static void questDialogueEditorCycleSelectedAction(const int direction)
{
    const std::vector<std::string> members = questDialogueEditorChoiceActionMembers();
    if ( members.empty() )
    {
        questDialogueEditorSelectedActionIndex = 0;
        return;
    }
    const int count = static_cast<int>(members.size());
    questDialogueEditorSelectedActionIndex =
        (questDialogueEditorSelectedActionIndex + direction + count) % count;
}

static std::string questDialogueEditorChoiceActionName()
{
    const std::vector<std::string> members = questDialogueEditorChoiceActionMembers();
    if ( members.empty() )
    {
        questDialogueEditorSelectedActionIndex = 0;
        return "None";
    }
    questDialogueEditorSelectedActionIndex = std::max(
        0,
        std::min(questDialogueEditorSelectedActionIndex,
            static_cast<int>(members.size()) - 1)
    );
    return questDialogueEditorActionDisplayName(
        members[questDialogueEditorSelectedActionIndex]
    );
}

static bool questDialogueEditorClearChoiceCondition();

static bool questDialogueEditorSetChoiceCondition(
	const int direction
)
{
	rapidjson::Value* choice =
		questDialogueEditorSelectedChoiceValue();

	if ( !choice || !choice->IsObject() )
	{
		questDialogueEditorSetMessage(
			"Select a choice first."
		);
		return false;
	}

	static const char* conditionTypes[] =
	{
		"none",
		"has_item",
		"has_gold",
		"quest_started",
		"quest_accepted",
		"quest_completed",
		"quest_failed",
		"quest_stage",
		"objective_completed",
		"objective_incomplete",
		"world_flag",
		"npc_flag",
		"world_variable",
		"npc_variable"
	};

	const int conditionCount =
		static_cast<int>(
			sizeof(conditionTypes)
			/ sizeof(conditionTypes[0])
		);

	std::string current = "none";

	if ( choice->HasMember("condition")
		&& (*choice)["condition"].IsObject()
		&& (*choice)["condition"].HasMember("type")
		&& (*choice)["condition"]["type"].IsString() )
	{
		current = questEditorNormalizeID(
			(*choice)["condition"]["type"].GetString());
	}

	int index = 0;

	for ( int i = 0; i < conditionCount; ++i )
	{
		if ( current == conditionTypes[i] )
		{
			index = i;
			break;
		}
	}

	index += direction;

	if ( index < 0 )
	{
		index = conditionCount - 1;
	}
	else if ( index >= conditionCount )
	{
		index = 0;
	}

	const std::string next =
		conditionTypes[index];

    if ( next == "none" )
    {
        return questDialogueEditorClearChoiceCondition();
    }

	auto& allocator =
		questDialogueEditorDocument.GetAllocator();

	rapidjson::Value condition(
		rapidjson::kObjectType
	);

	rapidjson::Value type;
	type.SetString(
		next.c_str(),
		allocator
	);
	condition.AddMember(
		"type",
		type,
		allocator
	);

	const std::string questID =
		questDialogueEditorPreview.questID.empty()
			? "quest_id"
			: questDialogueEditorPreview.questID;

	if ( next == "has_item" )
	{
		rapidjson::Value item;
		item.SetString(
			std::to_string(
				questDialogueEditorSelectedItemID
			).c_str(),
			allocator
		);

		condition.AddMember(
			"item",
			item,
			allocator
		);
		condition.AddMember(
			"count",
			questDialogueEditorSelectedItemCount,
			allocator
		);
	}
	else if ( next == "has_gold" )
	{
		condition.AddMember(
			"amount",
			questDialogueEditorGoldAmount,
			allocator
		);
	}
	else if ( next == "quest_started"
		|| next == "quest_accepted"
		|| next == "quest_completed"
		|| next == "quest_failed"
		|| next == "quest_stage" )
	{
		rapidjson::Value questValue;
		questValue.SetString(
			questID.c_str(),
			allocator
		);

		condition.AddMember(
			"quest",
			questValue,
			allocator
		);

		if ( next == "quest_stage" )
		{
			condition.AddMember(
				"stage",
				1,
				allocator
			);
		}
	}
	else if ( next == "objective_completed"
		|| next == "objective_incomplete" )
	{
		rapidjson::Value questValue;
		questValue.SetString(
			questID.c_str(),
			allocator
		);

		condition.AddMember(
			"quest",
			questValue,
			allocator
		);

		std::string objectiveID =
			"objective_id";

		if ( rapidjson::Value* objective =
			questDialogueEditorSelectedObjectiveValue() )
		{
			if ( objective->HasMember("id")
				&& (*objective)["id"].IsString() )
			{
				objectiveID =
					(*objective)["id"].GetString();
			}
		}

		rapidjson::Value objectiveValue;
		objectiveValue.SetString(
			objectiveID.c_str(),
			allocator
		);

		condition.AddMember(
			"objective",
			objectiveValue,
			allocator
		);
	}
	else if ( next == "world_flag"
		|| next == "npc_flag" )
	{
		rapidjson::Value id;
		id.SetString(
			next == "world_flag"
				? "world_flag_id"
				: "npc_flag_id",
			allocator
		);

		condition.AddMember(
			"id",
			id,
			allocator
		);
		condition.AddMember(
			"value",
			true,
			allocator
		);
	}
	else if ( next == "world_variable"
		|| next == "npc_variable" )
	{
		rapidjson::Value id;
		id.SetString(
			next == "world_variable"
				? "world_variable_id"
				: "npc_variable_id",
			allocator
		);

		condition.AddMember(
			"id",
			id,
			allocator
		);
		condition.AddMember(
			"value",
			1,
			allocator
		);

		rapidjson::Value comparison;
		comparison.SetString(
			"at_least",
			allocator
		);
		condition.AddMember(
			"comparison",
			comparison,
			allocator
		);
	}

    rapidjson::Value& conditions =
        questDialogueEditorEnsureConditionArray(*choice);
    conditions.PushBack(condition, allocator);
    questDialogueEditorSelectedConditionIndex =
        static_cast<int>(conditions.Size()) - 1;

	questDialogueEditorSetMessage(
		"Condition selected: "
		+ next
		+ ". Use Condition fields to edit its reference or number."
	);

	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorCycleChoiceCondition()
{
	return questDialogueEditorSetChoiceCondition(1);
}

static bool questDialogueEditorPreviousChoiceCondition()
{
	return questDialogueEditorSetChoiceCondition(-1);
}

static bool questDialogueEditorSelectChoiceConditionType(
    const char* targetType
)
{
    if ( !targetType )
    {
        return false;
    }

    static const char* conditionTypes[] =
    {
        "none", "has_item", "has_gold", "quest_started",
        "quest_accepted", "quest_completed", "quest_failed",
        "quest_stage", "objective_completed", "objective_incomplete",
        "world_flag", "npc_flag", "world_variable", "npc_variable"
    };
    const int conditionTypeCount = static_cast<int>(
        sizeof(conditionTypes) / sizeof(conditionTypes[0])
    );

    int targetIndex = -1;
    for ( int index = 0; index < conditionTypeCount; ++index )
    {
        if ( std::string(conditionTypes[index]) == targetType )
        {
            targetIndex = index;
            break;
        }
    }
    if ( targetIndex <= 0 )
    {
        return false;
    }

    rapidjson::Value* choice = questDialogueEditorSelectedChoiceValue();
    if ( !choice || !choice->IsObject() )
    {
        questDialogueEditorSetMessage("Select a choice first.");
        return false;
    }

    int originalCount = 0;
    if ( choice->HasMember("conditions")
        && (*choice)["conditions"].IsArray() )
    {
        originalCount = static_cast<int>((*choice)["conditions"].Size());
    }
    else if ( choice->HasMember("condition")
        && (*choice)["condition"].IsObject() )
    {
        originalCount = 1;
    }

    const std::string current = questDialogueEditorChoiceConditionName();
    int currentIndex = 0;
    for ( int index = 0; index < conditionTypeCount; ++index )
    {
        if ( current == conditionTypes[index] )
        {
            currentIndex = index;
            break;
        }
    }

    int steps = (targetIndex - currentIndex + conditionTypeCount)
        % conditionTypeCount;
    if ( steps == 0 )
    {
        steps = conditionTypeCount;
    }

    bool found = false;
    for ( int step = 0; step < steps; ++step )
    {
        if ( !questDialogueEditorSetChoiceCondition(1) )
        {
            return false;
        }
        if ( questDialogueEditorChoiceConditionName() == targetType )
        {
            found = true;
            break;
        }
    }

    choice = questDialogueEditorSelectedChoiceValue();
    if ( !found || !choice || !choice->IsObject()
        || !choice->HasMember("conditions")
        || !(*choice)["conditions"].IsArray() )
    {
        return false;
    }

    rapidjson::Value& conditions = (*choice)["conditions"];
    while ( static_cast<int>(conditions.Size()) > originalCount + 1 )
    {
        conditions.Erase(conditions.Begin() + originalCount);
    }
    questDialogueEditorSelectedConditionIndex =
        static_cast<int>(conditions.Size()) - 1;

    questDialogueEditorSetMessage(
        std::string("Added requirement: ") + targetType
    );
    return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorClearChoiceCondition()
{
    rapidjson::Value* choice = questDialogueEditorSelectedChoiceValue();
    if ( !choice || !choice->IsObject() )
    {
        questDialogueEditorSetMessage("Select a choice first.");
        return false;
    }

    const bool singular = choice->HasMember("condition")
        && (*choice)["condition"].IsObject();
    if ( singular && questDialogueEditorSelectedConditionIndex == 0 )
    {
        choice->RemoveMember("condition");
        questDialogueEditorSelectedConditionIndex = 0;
    }
    else if ( choice->HasMember("conditions")
        && (*choice)["conditions"].IsArray()
        && !(*choice)["conditions"].Empty() )
    {
        rapidjson::Value& conditions = (*choice)["conditions"];
        const int arrayIndex = std::max(0, std::min(
            questDialogueEditorSelectedConditionIndex - (singular ? 1 : 0),
            static_cast<int>(conditions.Size()) - 1));
        conditions.Erase(conditions.Begin()
            + arrayIndex);
        if ( conditions.Empty() )
        {
            choice->RemoveMember("conditions");
        }
        const int remaining = (singular ? 1 : 0)
            + (choice->HasMember("conditions") && (*choice)["conditions"].IsArray()
                ? static_cast<int>((*choice)["conditions"].Size()) : 0);
        questDialogueEditorSelectedConditionIndex = std::max(0,
            std::min(questDialogueEditorSelectedConditionIndex, remaining - 1));
    }
    else
    {
        choice->RemoveMember("condition");
        questDialogueEditorSelectedConditionIndex = 0;
    }

    questDialogueEditorSetMessage("Selected choice requirement removed.");
    return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorCycleChoiceAction()
{
	rapidjson::Value* choice =
		questDialogueEditorSelectedChoiceValue();

	if ( !choice || !choice->IsObject() )
	{
		questDialogueEditorSetMessage(
			"Select a choice first."
		);
		return false;
	}

	auto& allocator =
		questDialogueEditorDocument.GetAllocator();

	const std::string current =
		questDialogueEditorChoiceActionName();

	rapidjson::Value action(
		rapidjson::kObjectType
	);

	if ( current == "None" )
	{
		action.AddMember(
			"quest_accept",
			true,
			allocator
		);
	}
	else if ( current == "Accept Quest" )
	{
		action.AddMember(
			"quest_complete",
			true,
			allocator
		);
	}
	else if ( current == "Complete Quest" )
	{
		action.AddMember(
			"reward_gold",
			100,
			allocator
		);
	}
	else if ( current == "Reward Gold" )
	{
		action.AddMember(
			"recruit_npc",
			true,
			allocator
		);
	}
	else
	{
		choice->RemoveMember("action");
		return questDialogueEditorSaveDocument();
	}

	if ( choice->HasMember("action") )
	{
		(*choice)["action"] =
			std::move(action);
	}
	else
	{
		choice->AddMember(
			"action",
			action,
			allocator
		);
	}

	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorToggleChoiceOnce()
{
	rapidjson::Value* choice =
		questDialogueEditorSelectedChoiceValue();

	if ( !choice || !choice->IsObject() )
	{
		questDialogueEditorSetMessage(
			"Select a choice first."
		);
		return false;
	}

	auto& allocator =
		questDialogueEditorDocument.GetAllocator();

	if ( choice->HasMember("once")
		&& (*choice)["once"].IsBool() )
	{
		(*choice)["once"].SetBool(
			!(*choice)["once"].GetBool()
		);
	}
	else
	{
		choice->AddMember(
			"once",
			true,
			allocator
		);
	}

	return questDialogueEditorSaveDocument();
}


static const char* questDialogueEditorEditableFieldName()
{
	switch ( questDialogueEditorEditableField )
	{
		case QUEST_DIALOGUE_FIELD_FILE_ID:
			return "Dialogue/File ID";

		case QUEST_DIALOGUE_FIELD_QUEST_ID:
			return "Quest ID";

		case QUEST_DIALOGUE_FIELD_QUEST_TITLE:
			return "Quest Title";

		case QUEST_DIALOGUE_FIELD_QUEST_SUMMARY:
			return "Quest Summary";

		case QUEST_DIALOGUE_FIELD_QUEST_OBJECTIVE:
			return "General Objective";

		case QUEST_DIALOGUE_FIELD_QUEST_COMPLETED_TEXT:
			return "Quest Completion Text";

		case QUEST_DIALOGUE_FIELD_QUEST_FAILED_TEXT:
			return "Quest Failure Text";

		case QUEST_DIALOGUE_FIELD_LEGACY_TEXT:
			return "Legacy Dialogue Text";

		case QUEST_DIALOGUE_FIELD_NODE_ID:
			return "Node ID";

		case QUEST_DIALOGUE_FIELD_NODE_TEXT:
			return "Node Text";

		case QUEST_DIALOGUE_FIELD_NODE_NEXT:
			return "Node Automatic Next";

		case QUEST_DIALOGUE_FIELD_CHOICE_ID:
			return "Choice ID";

		case QUEST_DIALOGUE_FIELD_CHOICE_TEXT:
			return "Choice Text";

		case QUEST_DIALOGUE_FIELD_CHOICE_NEXT:
			return "Choice Destination";

		case QUEST_DIALOGUE_FIELD_OBJECTIVE_ID:
			return "Objective ID";

		case QUEST_DIALOGUE_FIELD_OBJECTIVE_TEXT:
			return "Objective Text";

		case QUEST_DIALOGUE_FIELD_OBJECTIVE_COMPLETED_TEXT:
			return "Objective Completed Text";

		case QUEST_DIALOGUE_FIELD_OBJECTIVE_PROGRESS_VARIABLE:
			return "Objective Progress Variable";

		case QUEST_DIALOGUE_FIELD_CONDITION_REFERENCE:
			return "Condition Reference";

		case QUEST_DIALOGUE_FIELD_CONDITION_QUEST:
			return "Required Quest";

		case QUEST_DIALOGUE_FIELD_CONDITION_NUMBER:
			return "Condition Number";

		case QUEST_DIALOGUE_FIELD_CONDITION_STABLE_ID:
			return "Condition S.A.M. Stable ID";

		case QUEST_DIALOGUE_FIELD_CONDITION_TRUE_NODE:
			return "Condition True Destination";

		case QUEST_DIALOGUE_FIELD_CONDITION_FALSE_NODE:
			return "Condition False Destination";

		case QUEST_DIALOGUE_FIELD_ACTION_REFERENCE:
			return "Action Reference";

		case QUEST_DIALOGUE_FIELD_ACTION_NUMBER:
			return "Action Number";

		case QUEST_DIALOGUE_FIELD_ACTION_SECONDARY_NUMBER:
			return "Action Secondary Number";

		case QUEST_DIALOGUE_FIELD_ACTION_TERTIARY_NUMBER:
			return "Action Tertiary Number";

		case QUEST_DIALOGUE_FIELD_ACTION_STABLE_ID:
			return "Action S.A.M. Stable ID";

		case QUEST_DIALOGUE_FIELD_NODE_ACTION_ID:
			return "One-time Node Action ID";

		case QUEST_DIALOGUE_FIELD_POWER_X:
			return "Power Tile X";

		case QUEST_DIALOGUE_FIELD_POWER_Y:
			return "Power Tile Y";

		case QUEST_DIALOGUE_FIELD_OBJECTIVE_STAGE:
			return "Objective Stage";

		case QUEST_DIALOGUE_FIELD_OBJECTIVE_TARGET:
			return "Objective Target";

		case QUEST_DIALOGUE_FIELD_OBJECTIVE_DEFEAT_ID:
			return "Objective Defeat ID";

		case QUEST_DIALOGUE_FIELD_MARKER_MAP:
			return "Marker Map";

		case QUEST_DIALOGUE_FIELD_MARKER_X:
			return "Marker X";

		case QUEST_DIALOGUE_FIELD_MARKER_Y:
			return "Marker Y";

		case QUEST_DIALOGUE_FIELD_MARKER_FLOOR:
			return "Marker Playable Floor";

		case QUEST_DIALOGUE_FIELD_ORIGIN_LABEL:
			return "Giver Label";

		case QUEST_DIALOGUE_FIELD_ORIGIN_MAP:
			return "Giver Map";

		case QUEST_DIALOGUE_FIELD_ORIGIN_X:
			return "Giver Tile X";

		case QUEST_DIALOGUE_FIELD_ORIGIN_Y:
			return "Giver Tile Y";

		case QUEST_DIALOGUE_FIELD_ORIGIN_FLOOR:
			return "Giver Playable Floor";

		case QUEST_DIALOGUE_FIELD_ORIGIN_NPC_ID:
			return "Giver Persistent NPC ID";

		default:
			return "Unknown";
	}
}

static rapidjson::Value* questDialogueEditorSelectedChoiceValueForEdit()
{
	rapidjson::Value* node =
		questDialogueEditorSelectedNodeValue();

	if ( !node
		|| !node->IsObject()
		|| !node->HasMember("choices")
		|| !(*node)["choices"].IsArray()
		|| questDialogueEditorSelectedChoice < 0
		|| questDialogueEditorSelectedChoice
			>= static_cast<int>(
				(*node)["choices"].Size()
			) )
	{
		return nullptr;
	}

	return &(*node)["choices"][
		static_cast<rapidjson::SizeType>(
			questDialogueEditorSelectedChoice
		)
	];
}

static rapidjson::Value* questDialogueEditorSelectedObjectiveValueForEdit()
{
	if ( !questDialogueEditorDocument.IsObject()
		|| !questDialogueEditorDocument.HasMember("quest")
		|| !questDialogueEditorDocument["quest"].IsObject()
		|| !questDialogueEditorDocument["quest"].HasMember(
			"objectives"
		)
		|| !questDialogueEditorDocument["quest"][
			"objectives"
		].IsArray()
		|| questDialogueEditorSelectedObjective < 0
		|| questDialogueEditorSelectedObjective
			>= static_cast<int>(
				questDialogueEditorDocument["quest"][
					"objectives"
				].Size()
			) )
	{
		return nullptr;
	}

	return &questDialogueEditorDocument["quest"][
		"objectives"
	][
		static_cast<rapidjson::SizeType>(
			questDialogueEditorSelectedObjective
		)
	];
}

static std::string questDialogueEditorReadEditableField()
{
	switch ( questDialogueEditorEditableField )
	{
		case QUEST_DIALOGUE_FIELD_FILE_ID:
			if ( questDialogueEditorSelectedFile >= 0
				&& questDialogueEditorSelectedFile
					< static_cast<int>(
						questDialogueEditorFiles.size()
					) )
			{
				const std::string& filename =
					questDialogueEditorFiles[
						questDialogueEditorSelectedFile
					];

				if ( filename.size() > 5
					&& filename.substr(
						filename.size() - 5
					) == ".json" )
				{
					return filename.substr(
						0,
						filename.size() - 5
					);
				}

				return filename;
			}
			break;

		case QUEST_DIALOGUE_FIELD_QUEST_ID:
			if ( questDialogueEditorDocument.IsObject()
				&& questDialogueEditorDocument.HasMember(
					"quest_id"
				)
				&& questDialogueEditorDocument[
					"quest_id"
				].IsString() )
			{
				return questDialogueEditorDocument[
					"quest_id"
				].GetString();
			}
			break;

		case QUEST_DIALOGUE_FIELD_QUEST_TITLE:
		case QUEST_DIALOGUE_FIELD_QUEST_SUMMARY:
		case QUEST_DIALOGUE_FIELD_QUEST_OBJECTIVE:
		case QUEST_DIALOGUE_FIELD_QUEST_COMPLETED_TEXT:
		case QUEST_DIALOGUE_FIELD_QUEST_FAILED_TEXT:
			if ( questDialogueEditorDocument.IsObject()
				&& questDialogueEditorDocument.HasMember("quest")
				&& questDialogueEditorDocument["quest"].IsObject() )
			{
				const char* member = "title";
				if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_QUEST_SUMMARY )
				{
					member = "summary";
				}
				else if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_QUEST_OBJECTIVE )
				{
					member = "objective";
				}
				else if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_QUEST_COMPLETED_TEXT )
				{
					member = "completed_text";
				}
				else if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_QUEST_FAILED_TEXT )
				{
					member = "failed_text";
				}

				if ( questDialogueEditorDocument["quest"].HasMember(
						member
					)
					&& questDialogueEditorDocument["quest"][
						member
					].IsString() )
				{
					return questDialogueEditorDocument["quest"][
						member
					].GetString();
				}
			}
			break;

		case QUEST_DIALOGUE_FIELD_LEGACY_TEXT:
			if ( questDialogueEditorDocument.IsObject()
				&& questDialogueEditorDocument.HasMember("text")
				&& questDialogueEditorDocument["text"].IsString() )
			{
				return questDialogueEditorDocument["text"].GetString();
			}
			break;

		case QUEST_DIALOGUE_FIELD_NODE_ID:
		case QUEST_DIALOGUE_FIELD_NODE_TEXT:
		case QUEST_DIALOGUE_FIELD_NODE_NEXT:
		{
			rapidjson::Value* node =
				questDialogueEditorSelectedNodeValue();

			if ( node && node->IsObject() )
			{
				if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_NODE_TEXT
					&& node->HasMember("text")
					&& (*node)["text"].IsString() )
				{
					return (*node)["text"].GetString();
				}
				const char* member = questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_NODE_ID ? "id" : "next";
				if ( node->HasMember(member) && (*node)[member].IsInt() )
				{
					return std::to_string((*node)[member].GetInt());
				}
			}
			break;
		}

		case QUEST_DIALOGUE_FIELD_CHOICE_ID:
		case QUEST_DIALOGUE_FIELD_CHOICE_TEXT:
		case QUEST_DIALOGUE_FIELD_CHOICE_NEXT:
		{
			rapidjson::Value* choice =
				questDialogueEditorSelectedChoiceValueForEdit();

			if ( choice && choice->IsObject() )
			{
				if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_CHOICE_NEXT )
				{
					if ( choice->HasMember("next")
						&& (*choice)["next"].IsInt() )
					{
						return std::to_string((*choice)["next"].GetInt());
					}
					break;
				}
				const char* member = questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_CHOICE_ID ? "id" : "text";

				if ( choice->HasMember(member)
					&& (*choice)[member].IsString() )
				{
					return (*choice)[member].GetString();
				}
			}
			break;
		}

		case QUEST_DIALOGUE_FIELD_OBJECTIVE_ID:
		case QUEST_DIALOGUE_FIELD_OBJECTIVE_TEXT:
		case QUEST_DIALOGUE_FIELD_OBJECTIVE_COMPLETED_TEXT:
		case QUEST_DIALOGUE_FIELD_OBJECTIVE_PROGRESS_VARIABLE:
		{
			rapidjson::Value* objective =
				questDialogueEditorSelectedObjectiveValueForEdit();

			if ( objective && objective->IsObject() )
			{
				const char* member = "id";
				if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_OBJECTIVE_TEXT )
				{
					member = "text";
				}
				else if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_OBJECTIVE_COMPLETED_TEXT )
				{
					member = "completed_text";
				}
				else if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_OBJECTIVE_PROGRESS_VARIABLE )
				{
					member = "progress_variable";
				}

				if ( objective->HasMember(member)
					&& (*objective)[member].IsString() )
				{
					return (*objective)[member].GetString();
				}
			}
			break;
		}

		case QUEST_DIALOGUE_FIELD_CONDITION_REFERENCE:
		case QUEST_DIALOGUE_FIELD_CONDITION_STABLE_ID:
		{
			rapidjson::Value* condition =
				questDialogueEditorSelectedRuleCondition();

			if ( condition )
			{
				if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_CONDITION_STABLE_ID )
				{
					if ( condition->HasMember("stable_id")
						&& (*condition)["stable_id"].IsString() )
					{
						return (*condition)["stable_id"].GetString();
					}
					break;
				}

				const char* candidates[] =
				{
					"item",
					"quest",
					"id",
					"objective",
					"node"
				};

				for ( const char* candidate : candidates )
				{
					if ( condition->HasMember(candidate)
						&& (*condition)[candidate].IsString() )
					{
						return (*condition)[candidate].GetString();
					}
				}
			}
			break;
		}

		case QUEST_DIALOGUE_FIELD_CONDITION_QUEST:
		{
			rapidjson::Value* condition =
				questDialogueEditorSelectedRuleCondition();

			if ( condition && condition->HasMember("quest")
				&& (*condition)["quest"].IsString() )
			{
				return (*condition)["quest"].GetString();
			}
			break;
		}

		case QUEST_DIALOGUE_FIELD_CONDITION_NUMBER:
		case QUEST_DIALOGUE_FIELD_CONDITION_TRUE_NODE:
		case QUEST_DIALOGUE_FIELD_CONDITION_FALSE_NODE:
		{
			rapidjson::Value* condition =
				questDialogueEditorSelectedRuleCondition();

			if ( condition )
			{
				if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_CONDITION_TRUE_NODE
					|| questDialogueEditorEditableField
						== QUEST_DIALOGUE_FIELD_CONDITION_FALSE_NODE )
				{
					const char* member = questDialogueEditorEditableField
						== QUEST_DIALOGUE_FIELD_CONDITION_TRUE_NODE
							? "true_node" : "false_node";
					if ( condition->HasMember(member)
						&& (*condition)[member].IsInt() )
					{
						return std::to_string((*condition)[member].GetInt());
					}
					break;
				}

				const char* candidates[] =
				{
					"count",
					"amount",
					"stage",
					"value"
				};

				for ( const char* candidate : candidates )
				{
					if ( condition->HasMember(candidate)
						&& (*condition)[candidate].IsInt() )
					{
						return std::to_string(
							(*condition)[candidate].GetInt()
						);
					}
				}
			}
			break;
		}

		case QUEST_DIALOGUE_FIELD_ACTION_REFERENCE:
		case QUEST_DIALOGUE_FIELD_ACTION_STABLE_ID:
		case QUEST_DIALOGUE_FIELD_NODE_ACTION_ID:
		{
			rapidjson::Value* action =
				questDialogueEditorSelectedRuleAction();

			if ( action )
			{
				if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_NODE_ACTION_ID )
				{
					if ( action->HasMember("id") && (*action)["id"].IsString() )
					{
						return (*action)["id"].GetString();
					}
					break;
				}

				const std::string member =
					questDialogueEditorSelectedRuleActionMember();
				if ( member.empty() || !action->HasMember(member.c_str()) )
				{
					break;
				}
				rapidjson::Value& selected = (*action)[member.c_str()];
				if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_ACTION_STABLE_ID )
				{
					if ( selected.IsObject() && selected.HasMember("stable_id")
						&& selected["stable_id"].IsString() )
					{
						return selected["stable_id"].GetString();
					}
					break;
				}
				if ( selected.IsString() )
				{
					return selected.GetString();
				}
				if ( selected.IsObject() )
				{
					const char* key = member == "reward_item"
						|| member == "remove_item"
							? "item" : "id";
					if ( selected.HasMember(key) && selected[key].IsString() )
					{
						return selected[key].GetString();
					}
				}
			}
			break;
		}

		case QUEST_DIALOGUE_FIELD_ACTION_NUMBER:
		case QUEST_DIALOGUE_FIELD_ACTION_SECONDARY_NUMBER:
		case QUEST_DIALOGUE_FIELD_ACTION_TERTIARY_NUMBER:
		{
			rapidjson::Value* action =
				questDialogueEditorSelectedRuleAction();
			const std::string member =
				questDialogueEditorSelectedRuleActionMember();

			if ( action && !member.empty() && action->HasMember(member.c_str()) )
			{
				rapidjson::Value& selected = (*action)[member.c_str()];
				if ( selected.IsInt()
					&& questDialogueEditorEditableField
						== QUEST_DIALOGUE_FIELD_ACTION_NUMBER )
				{
					return std::to_string(selected.GetInt());
				}
				if ( selected.IsObject() )
				{
					const char* key = nullptr;
					if ( questDialogueEditorEditableField
						== QUEST_DIALOGUE_FIELD_ACTION_SECONDARY_NUMBER )
					{
						key = member == "status_effect" ? "duration_seconds" : nullptr;
					}
					else if ( questDialogueEditorEditableField
						== QUEST_DIALOGUE_FIELD_ACTION_TERTIARY_NUMBER )
					{
						key = member == "status_effect" ? "strength" : nullptr;
					}
					else if ( member == "reward_item" || member == "remove_item" )
					{
						key = "count";
					}
					else if ( member == "status_effect" )
					{
						key = "effect";
					}
					else if ( member.rfind("set_", 0) == 0
						&& member.find("variable") != std::string::npos )
					{
						key = "value";
					}
					else if ( member.rfind("add_", 0) == 0
						&& member.find("variable") != std::string::npos )
					{
						key = "amount";
					}
					if ( key && selected.HasMember(key) && selected[key].IsInt() )
					{
						return std::to_string(selected[key].GetInt());
					}
				}
			}
			break;
		}

		case QUEST_DIALOGUE_FIELD_OBJECTIVE_STAGE:
		case QUEST_DIALOGUE_FIELD_OBJECTIVE_TARGET:
		case QUEST_DIALOGUE_FIELD_OBJECTIVE_DEFEAT_ID:
		{
			rapidjson::Value* objective =
				questDialogueEditorSelectedObjectiveValueForEdit();

			if ( objective && objective->IsObject() )
			{
				const char* member = "target";
				if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_OBJECTIVE_STAGE )
				{
					member = "stage";
				}
				else if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_OBJECTIVE_DEFEAT_ID )
				{
					member = "defeat_id";
				}

				if ( objective->HasMember(member)
					&& (*objective)[member].IsInt() )
				{
					return std::to_string(
						(*objective)[member].GetInt()
					);
				}

				if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_OBJECTIVE_TARGET )
				{
					return "1";
				}
				if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_OBJECTIVE_DEFEAT_ID )
				{
					return "";
				}
				return "0";
			}
			break;
		}

		case QUEST_DIALOGUE_FIELD_POWER_X:
		case QUEST_DIALOGUE_FIELD_POWER_Y:
		{
			rapidjson::Value* action =
				questDialogueEditorSelectedRuleAction();

			if ( action && action->HasMember("set_power")
				&& (*action)["set_power"].IsObject() )
			{
				rapidjson::Value& powerAction =
					(*action)["set_power"];
				const char* member =
					questDialogueEditorEditableField
						== QUEST_DIALOGUE_FIELD_POWER_X
							? "x"
							: "y";

				if ( powerAction.HasMember(member)
					&& powerAction[member].IsInt() )
				{
					return std::to_string(
						powerAction[member].GetInt()
					);
				}
			}
			break;
		}

		case QUEST_DIALOGUE_FIELD_MARKER_MAP:
		case QUEST_DIALOGUE_FIELD_MARKER_X:
		case QUEST_DIALOGUE_FIELD_MARKER_Y:
		case QUEST_DIALOGUE_FIELD_MARKER_FLOOR:
		{
			rapidjson::Value* objective =
				questDialogueEditorSelectedObjectiveValueForEdit();

			if ( objective
				&& objective->IsObject()
				&& objective->HasMember("map_marker")
				&& (*objective)["map_marker"].IsObject() )
			{
				rapidjson::Value& marker =
					(*objective)["map_marker"];
				if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_MARKER_MAP )
				{
					if ( marker.HasMember("map") && marker["map"].IsString() )
					{
						return marker["map"].GetString();
					}
					break;
				}

				const char* member = questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_MARKER_X ? "x"
					: (questDialogueEditorEditableField
						== QUEST_DIALOGUE_FIELD_MARKER_Y ? "y" : "playable_floor");

				if ( marker.HasMember(member)
					&& marker[member].IsInt() )
				{
					return std::to_string(
						marker[member].GetInt()
					);
				}
			}
			break;
		}

		case QUEST_DIALOGUE_FIELD_ORIGIN_LABEL:
		case QUEST_DIALOGUE_FIELD_ORIGIN_MAP:
		case QUEST_DIALOGUE_FIELD_ORIGIN_X:
		case QUEST_DIALOGUE_FIELD_ORIGIN_Y:
		case QUEST_DIALOGUE_FIELD_ORIGIN_FLOOR:
		case QUEST_DIALOGUE_FIELD_ORIGIN_NPC_ID:
		{
			rapidjson::Value* quest = questDialogueEditorQuestValue();
			if ( !quest || !quest->HasMember("origin")
				|| !(*quest)["origin"].IsObject() )
			{
				break;
			}
			rapidjson::Value& origin = (*quest)["origin"];
			const char* member = "label";
			if ( questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_ORIGIN_MAP )
			{
				member = "map";
			}
			else if ( questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_ORIGIN_X )
			{
				member = "x";
			}
			else if ( questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_ORIGIN_Y )
			{
				member = "y";
			}
			else if ( questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_ORIGIN_FLOOR )
			{
				member = "playable_floor";
			}
			else if ( questDialogueEditorEditableField
				== QUEST_DIALOGUE_FIELD_ORIGIN_NPC_ID )
			{
				member = "npc_persistent_id";
			}
			if ( origin.HasMember(member) )
			{
				if ( origin[member].IsString() ) return origin[member].GetString();
				if ( origin[member].IsInt() ) return std::to_string(origin[member].GetInt());
			}
			break;
		}

		default:
			break;
	}

	return "";
}

static bool questDialogueEditorWriteStringMember(
	rapidjson::Value& object,
	const char* member,
	const std::string& value
)
{
	if ( !object.IsObject() )
	{
		return false;
	}

	auto& allocator =
		questDialogueEditorDocument.GetAllocator();

	if ( object.HasMember(member) )
	{
		if ( !object[member].IsString() )
		{
			object.RemoveMember(member);
		}
		else
		{
			object[member].SetString(
				value.c_str(),
				static_cast<rapidjson::SizeType>(
					value.size()
				),
				allocator
			);
			return true;
		}
	}

	rapidjson::Value stringValue;
	stringValue.SetString(
		value.c_str(),
		static_cast<rapidjson::SizeType>(
			value.size()
		),
		allocator
	);

	object.AddMember(
		rapidjson::Value(member, allocator),
		stringValue,
		allocator
	);

	return true;
}


static bool questDialogueEditorWriteIntegerMember(
	rapidjson::Value& object,
	const char* member,
	const int value
)
{
	if ( !object.IsObject() )
	{
		return false;
	}

	auto& allocator =
		questDialogueEditorDocument.GetAllocator();

	if ( object.HasMember(member) )
	{
		if ( object[member].IsInt() )
		{
			object[member].SetInt(value);
			return true;
		}

		object.RemoveMember(member);
	}

	rapidjson::Value memberName;
	memberName.SetString(
		member,
		static_cast<rapidjson::SizeType>(
			strlen(member)
		),
		allocator
	);

	rapidjson::Value integerValue;
	integerValue.SetInt(value);

	object.AddMember(
		memberName,
		integerValue,
		allocator
	);

	return true;
}

static rapidjson::Value& questDialogueEditorSetObjectMember(
	rapidjson::Value& parent,
	const char* name
);
static void questDialogueEditorSetBoolMember(
	rapidjson::Value& object,
	const char* name,
	bool value
);

static bool questDialogueEditorParseInteger(
	const std::string& value,
	int& result
)
{
	if ( value.empty() )
	{
		return false;
	}

	char* end = nullptr;
	const long parsed =
		strtol(value.c_str(), &end, 10);

	if ( !end
		|| *end != '\0'
		|| parsed < INT_MIN
		|| parsed > INT_MAX )
	{
		return false;
	}

	result = static_cast<int>(parsed);
	return true;
}

static bool questDialogueEditorApplyEditableField()
{
	const std::string value =
		questDialogueEditorEditBuffer;

	if ( value.empty() )
	{
		rapidjson::Value* owner = nullptr;
		const char* optionalMember = nullptr;
		if ( questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_QUEST_SUMMARY
			|| questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_QUEST_OBJECTIVE
			|| questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_QUEST_COMPLETED_TEXT
			|| questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_QUEST_FAILED_TEXT )
		{
			owner = questDialogueEditorQuestValue();
			optionalMember = questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_QUEST_SUMMARY
				? "summary" : (questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_QUEST_OBJECTIVE
					? "objective" : (questDialogueEditorEditableField
						== QUEST_DIALOGUE_FIELD_QUEST_COMPLETED_TEXT ? "completed_text" : "failed_text"));
		}
		else if ( questDialogueEditorEditableField
			== QUEST_DIALOGUE_FIELD_OBJECTIVE_COMPLETED_TEXT
			|| questDialogueEditorEditableField
				== QUEST_DIALOGUE_FIELD_OBJECTIVE_PROGRESS_VARIABLE )
		{
			owner = questDialogueEditorSelectedObjectiveValueForEdit();
			optionalMember = questDialogueEditorEditableField
				== QUEST_DIALOGUE_FIELD_OBJECTIVE_COMPLETED_TEXT
					? "completed_text" : "progress_variable";
		}
		else if ( questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_OBJECTIVE_STAGE
			|| questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_OBJECTIVE_TARGET
			|| questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_OBJECTIVE_DEFEAT_ID )
		{
			owner = questDialogueEditorSelectedObjectiveValueForEdit();
			optionalMember = questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_OBJECTIVE_STAGE
				? "stage" : (questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_OBJECTIVE_TARGET
					? "target" : "defeat_id");
		}
		else if ( questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_ORIGIN_LABEL
			|| questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_ORIGIN_MAP )
		{
			rapidjson::Value* quest = questDialogueEditorQuestValue();
			if ( quest && quest->HasMember("origin") && (*quest)["origin"].IsObject() )
				owner = &(*quest)["origin"];
			optionalMember = questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_ORIGIN_LABEL
				? "label" : "map";
		}
		else if ( questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_ORIGIN_X
			|| questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_ORIGIN_Y
			|| questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_ORIGIN_NPC_ID )
		{
			rapidjson::Value* quest = questDialogueEditorQuestValue();
			if ( quest && quest->HasMember("origin") && (*quest)["origin"].IsObject() )
				owner = &(*quest)["origin"];
			optionalMember = questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_ORIGIN_X
				? "x" : (questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_ORIGIN_Y
					? "y" : "npc_persistent_id");
		}
		if ( owner && optionalMember )
		{
			owner->RemoveMember(optionalMember);
			questDialogueEditorEditingField = false;
			SDL_StopTextInput();
			if ( inputstr == questDialogueEditorEditBuffer ) inputstr = nullptr;
			questDialogueEditorSetMessage("Optional field cleared.");
			return questDialogueEditorSaveDocument();
		}
		questDialogueEditorSetMessage("This required field cannot be empty.");
		return false;
	}

	bool success = false;

	switch ( questDialogueEditorEditableField )
	{
		case QUEST_DIALOGUE_FIELD_FILE_ID:
			questDialogueEditorSetMessage(
				"Use RENAME to change the JSON filename."
			);
			return false;

		case QUEST_DIALOGUE_FIELD_QUEST_ID:
		{
			const std::string normalized = questEditorNormalizeID(value);
			if ( normalized.empty() )
			{
				questDialogueEditorSetMessage("Quest ID must contain letters or numbers.");
				return false;
			}
			const std::string oldID = questDialogueEditorDocument.HasMember("quest_id")
				&& questDialogueEditorDocument["quest_id"].IsString()
				? questDialogueEditorDocument["quest_id"].GetString() : std::string{};
			success = questDialogueEditorWriteStringMember(
				questDialogueEditorDocument, "quest_id", normalized);
			if ( success ) questDialogueEditorRelinkQuestID(oldID, normalized);
			break;
		}

		case QUEST_DIALOGUE_FIELD_QUEST_TITLE:
		case QUEST_DIALOGUE_FIELD_QUEST_SUMMARY:
		case QUEST_DIALOGUE_FIELD_QUEST_OBJECTIVE:
		case QUEST_DIALOGUE_FIELD_QUEST_COMPLETED_TEXT:
		case QUEST_DIALOGUE_FIELD_QUEST_FAILED_TEXT:
			if ( questDialogueEditorDocument.HasMember("quest")
				&& questDialogueEditorDocument["quest"].IsObject() )
			{
				const char* member = "title";
				if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_QUEST_SUMMARY ) member = "summary";
				else if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_QUEST_OBJECTIVE ) member = "objective";
				else if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_QUEST_COMPLETED_TEXT ) member = "completed_text";
				else if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_QUEST_FAILED_TEXT ) member = "failed_text";
				success =
					questDialogueEditorWriteStringMember(
						questDialogueEditorDocument["quest"],
						member,
						value
					);
			}
			break;

		case QUEST_DIALOGUE_FIELD_LEGACY_TEXT:
			if ( questDialogueEditorDocument.IsObject()
				&& questDialogueEditorDocument.HasMember("text")
				&& questDialogueEditorDocument["text"].IsString() )
			{
				success = questDialogueEditorWriteStringMember(
					questDialogueEditorDocument, "text", value);
			}
			break;

		case QUEST_DIALOGUE_FIELD_NODE_ID:
		case QUEST_DIALOGUE_FIELD_NODE_TEXT:
		case QUEST_DIALOGUE_FIELD_NODE_NEXT:
		{
			rapidjson::Value* node =
				questDialogueEditorSelectedNodeValue();

			if ( node )
			{
				if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_NODE_TEXT )
				{
					success = questDialogueEditorWriteStringMember(*node, "text", value);
				}
				else
				{
					int number = 0;
					if ( !questDialogueEditorParseInteger(value, number) )
					{
						questDialogueEditorSetMessage("Node destination/ID must be an integer.");
						return false;
					}
					if ( questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_NODE_ID )
					{
						const int oldID = node->HasMember("id") && (*node)["id"].IsInt()
							? (*node)["id"].GetInt() : number;
						rapidjson::Value& nodes = questDialogueEditorDocument["nodes"];
						for ( const rapidjson::Value& other : nodes.GetArray() )
						{
							if ( &other != node && other.IsObject() && other.HasMember("id")
								&& other["id"].IsInt() && other["id"].GetInt() == number )
							{
								questDialogueEditorSetMessage("That node ID already exists.");
								return false;
							}
						}
						(*node)["id"].SetInt(number);
						if ( questDialogueEditorDocument.HasMember("start_node")
							&& questDialogueEditorDocument["start_node"].IsInt()
							&& questDialogueEditorDocument["start_node"].GetInt() == oldID )
							questDialogueEditorDocument["start_node"].SetInt(number);
						for ( rapidjson::Value& other : nodes.GetArray() )
						{
							if ( !other.IsObject() ) continue;
							if ( other.HasMember("next") && other["next"].IsInt()
								&& other["next"].GetInt() == oldID ) other["next"].SetInt(number);
							if ( other.HasMember("condition") && other["condition"].IsObject() )
							{
								rapidjson::Value& condition = other["condition"];
								for ( const char* branch : { "true_node", "false_node" } )
									if ( condition.HasMember(branch) && condition[branch].IsInt()
										&& condition[branch].GetInt() == oldID ) condition[branch].SetInt(number);
								const std::string oldSeen = "node_" + std::to_string(oldID);
								if ( condition.HasMember("node") && condition["node"].IsString()
								&& questEditorNormalizeID(oldSeen)
									== questEditorNormalizeID(condition["node"].GetString()) )
									condition["node"].SetString(("node_" + std::to_string(number)).c_str(),
										questDialogueEditorDocument.GetAllocator());
							}
							if ( &other == node && other.HasMember("action")
								&& other["action"].IsObject()
								&& other["action"].HasMember("id")
								&& other["action"]["id"].IsString()
								&& std::string(other["action"]["id"].GetString())
									== "node_" + std::to_string(oldID) + "_once" )
							{
								other["action"]["id"].SetString(
									("node_" + std::to_string(number) + "_once").c_str(),
									questDialogueEditorDocument.GetAllocator());
							}
							if ( other.HasMember("choices") && other["choices"].IsArray() )
								for ( rapidjson::Value& linkedChoice : other["choices"].GetArray() )
									if ( linkedChoice.IsObject() && linkedChoice.HasMember("next")
										&& linkedChoice["next"].IsInt()
										&& linkedChoice["next"].GetInt() == oldID ) linkedChoice["next"].SetInt(number);
						}
						success = true;
					}
					else success = questDialogueEditorWriteIntegerMember(*node, "next", number);
				}
			}
			break;
		}

		case QUEST_DIALOGUE_FIELD_CHOICE_ID:
		case QUEST_DIALOGUE_FIELD_CHOICE_TEXT:
		case QUEST_DIALOGUE_FIELD_CHOICE_NEXT:
		{
			rapidjson::Value* choice =
				questDialogueEditorSelectedChoiceValueForEdit();

			if ( choice )
			{
				if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_CHOICE_NEXT )
				{
					int number = 0;
					if ( !questDialogueEditorParseInteger(value, number) )
					{
						questDialogueEditorSetMessage("Choice destination must be an integer.");
						return false;
					}
					success = questDialogueEditorWriteIntegerMember(*choice, "next", number);
				}
				else if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_CHOICE_ID )
				{
					const std::string normalized = questEditorNormalizeID(value);
					if ( normalized.empty() )
					{
						questDialogueEditorSetMessage("Choice ID must contain letters or numbers.");
						return false;
					}
					if ( questDialogueEditorChoiceIDExistsGlobal(normalized, choice) )
					{
						questDialogueEditorSetMessage("That stable choice ID already exists.");
						return false;
					}
					success = questDialogueEditorWriteStringMember(*choice, "id", normalized);
				}
				else success = questDialogueEditorWriteStringMember(*choice, "text", value);
			}
			break;
		}

		case QUEST_DIALOGUE_FIELD_OBJECTIVE_ID:
		case QUEST_DIALOGUE_FIELD_OBJECTIVE_TEXT:
		case QUEST_DIALOGUE_FIELD_OBJECTIVE_COMPLETED_TEXT:
		case QUEST_DIALOGUE_FIELD_OBJECTIVE_PROGRESS_VARIABLE:
		{
			rapidjson::Value* objective =
				questDialogueEditorSelectedObjectiveValueForEdit();

			if ( objective )
			{
				const char* member = "id";
				if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_OBJECTIVE_TEXT ) member = "text";
				else if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_OBJECTIVE_COMPLETED_TEXT ) member = "completed_text";
				else if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_OBJECTIVE_PROGRESS_VARIABLE ) member = "progress_variable";
					const bool normalize = questDialogueEditorEditableField
						== QUEST_DIALOGUE_FIELD_OBJECTIVE_ID
						|| questDialogueEditorEditableField
							== QUEST_DIALOGUE_FIELD_OBJECTIVE_PROGRESS_VARIABLE;
					const std::string authoredValue = normalize
						? questEditorNormalizeID(value) : value;
					if ( normalize && authoredValue.empty() )
					{
						questDialogueEditorSetMessage(
							"Objective IDs and progress variables must contain letters or numbers.");
						return false;
					}
					if ( questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_OBJECTIVE_ID )
					{
						const std::string oldID = objective->HasMember("id")
							&& (*objective)["id"].IsString()
							? (*objective)["id"].GetString() : std::string{};
						rapidjson::Value* quest = questDialogueEditorQuestValue();
						if ( quest && quest->HasMember("objectives")
							&& (*quest)["objectives"].IsArray() )
						{
							for ( const rapidjson::Value& other : (*quest)["objectives"].GetArray() )
							{
								if ( &other != objective && other.IsObject()
									&& other.HasMember("id") && other["id"].IsString()
									&& questEditorNormalizeID(other["id"].GetString()) == authoredValue )
								{
									questDialogueEditorSetMessage("That objective ID already exists.");
									return false;
								}
							}
						}
						success = questDialogueEditorWriteStringMember(*objective, member, authoredValue);
						if ( success ) questDialogueEditorRelinkObjectiveID(oldID, authoredValue);
					}
					else success = questDialogueEditorWriteStringMember(
						*objective, member, authoredValue);
			}
			break;
		}

		case QUEST_DIALOGUE_FIELD_CONDITION_REFERENCE:
		case QUEST_DIALOGUE_FIELD_CONDITION_STABLE_ID:
		{
			rapidjson::Value* condition =
				questDialogueEditorSelectedRuleCondition();

			if ( condition && condition->HasMember("type")
				&& (*condition)["type"].IsString() )
			{
				const std::string type = questEditorNormalizeID(
					(*condition)["type"].GetString());

				if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_CONDITION_STABLE_ID )
				{
					if ( type != "has_item" ) break;
					if ( !automatia::dialogue::isSafeStableItemID(value) )
					{
						questDialogueEditorSetMessage(
							"S.A.M. stable ID must be a safe namespaced ID such as mod:item.");
						return false;
					}
					success = questDialogueEditorWriteStringMember(
						*condition, "stable_id", value);
					if ( success ) condition->RemoveMember("item");
					break;
				}

				const char* member = nullptr;

				if ( type == "has_item" )
				{
					member = "item";
				}
				else if ( type == "quest_started"
					|| type == "quest_accepted"
					|| type == "quest_completed"
					|| type == "quest_failed"
					|| type == "quest_stage" )
				{
					member = "quest";
				}
				else if ( type == "objective_completed"
					|| type == "objective_incomplete" )
				{
					member = "objective";
				}
				else if ( type == "world_flag"
					|| type == "npc_flag"
					|| type == "world_variable"
					|| type == "npc_variable" )
				{
					member = "id";
				}
				else if ( type == "node_seen" )
				{
					member = "node";
				}

				if ( member )
				{
					success =
						questDialogueEditorWriteStringMember(
							*condition,
							member,
							questEditorNormalizeID(value)
						);
					if ( success && type == "has_item" )
					{
						condition->RemoveMember("stable_id");
					}
				}
			}
			break;
		}

		case QUEST_DIALOGUE_FIELD_CONDITION_QUEST:
		{
			rapidjson::Value* condition =
				questDialogueEditorSelectedRuleCondition();
			if ( condition )
			{
				success = questDialogueEditorWriteStringMember(
					*condition,
					"quest",
					questEditorNormalizeID(value)
				);
			}
			break;
		}

		case QUEST_DIALOGUE_FIELD_CONDITION_NUMBER:
		case QUEST_DIALOGUE_FIELD_CONDITION_TRUE_NODE:
		case QUEST_DIALOGUE_FIELD_CONDITION_FALSE_NODE:
		{
			int number = 0;
			if ( !questDialogueEditorParseInteger(
					value,
					number
				) )
			{
				questDialogueEditorSetMessage(
					"Condition number must be an integer."
				);
				return false;
			}

			rapidjson::Value* condition =
				questDialogueEditorSelectedRuleCondition();

			if ( condition && condition->HasMember("type")
				&& (*condition)["type"].IsString() )
			{
				const std::string type = questEditorNormalizeID(
					(*condition)["type"].GetString());

				const char* member = nullptr;
				if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_CONDITION_TRUE_NODE )
				{
					member = "true_node";
				}
				else if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_CONDITION_FALSE_NODE )
				{
					member = "false_node";
				}

				else if ( type == "has_item" )
				{
					member = "count";
					number = std::max(1, number);
				}
				else if ( type == "has_gold" )
				{
					member = "amount";
					number = std::max(0, number);
				}
				else if ( type == "quest_stage" )
				{
					member = "stage";
				}
				else if ( type == "world_variable"
					|| type == "npc_variable" )
				{
					member = "value";
				}

				if ( member )
				{
					success =
						questDialogueEditorWriteIntegerMember(
							*condition,
							member,
							number
						);
				}
			}
			break;
		}

		case QUEST_DIALOGUE_FIELD_ACTION_REFERENCE:
		case QUEST_DIALOGUE_FIELD_ACTION_STABLE_ID:
		case QUEST_DIALOGUE_FIELD_NODE_ACTION_ID:
		{
			rapidjson::Value* action = questDialogueEditorSelectedRuleAction();
			if ( !action ) break;
			if ( questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_NODE_ACTION_ID )
			{
				const std::string normalized = questEditorNormalizeID(value);
				if ( normalized.empty() )
				{
					questDialogueEditorSetMessage(
						"One-time action ID must contain letters or numbers.");
					return false;
				}
				if ( questDialogueEditorDocument.HasMember("nodes")
					&& questDialogueEditorDocument["nodes"].IsArray() )
				{
					for ( const rapidjson::Value& node :
						questDialogueEditorDocument["nodes"].GetArray() )
					{
						if ( !node.IsObject() || !node.HasMember("action")
							|| !node["action"].IsObject()
							|| &node["action"] == action
							|| !node["action"].HasMember("id")
							|| !node["action"]["id"].IsString() ) continue;
						if ( questEditorNormalizeID(node["action"]["id"].GetString())
							== normalized )
						{
							questDialogueEditorSetMessage(
								"That one-time node action ID already exists.");
							return false;
						}
					}
				}
				success = questDialogueEditorWriteStringMember(*action, "id", normalized);
				break;
			}
			const std::string member = questDialogueEditorSelectedRuleActionMember();
			if ( member.empty() || !action->HasMember(member.c_str()) ) break;
			rapidjson::Value& selected = (*action)[member.c_str()];
			if ( questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_ACTION_STABLE_ID )
			{
				if ( (member != "reward_item" && member != "remove_item")
					|| !selected.IsObject() ) break;
				if ( !automatia::dialogue::isSafeStableItemID(value) )
				{
					questDialogueEditorSetMessage(
						"S.A.M. stable ID must be a safe namespaced ID such as mod:item.");
					return false;
				}
				success = questDialogueEditorWriteStringMember(selected, "stable_id", value);
				if ( success ) selected.RemoveMember("item");
				break;
			}
			const std::string normalized = questEditorNormalizeID(value);
			if ( selected.IsString() )
			{
				success = questDialogueEditorWriteStringMember(*action, member.c_str(), normalized);
			}
			else if ( selected.IsObject() )
			{
				const bool item = member == "reward_item" || member == "remove_item";
				success = questDialogueEditorWriteStringMember(
					selected, item ? "item" : "id", normalized);
				if ( success && item ) selected.RemoveMember("stable_id");
			}
			break;
		}

		case QUEST_DIALOGUE_FIELD_ACTION_NUMBER:
		case QUEST_DIALOGUE_FIELD_ACTION_SECONDARY_NUMBER:
		case QUEST_DIALOGUE_FIELD_ACTION_TERTIARY_NUMBER:
		{
			int number = 0;
			if ( !questDialogueEditorParseInteger(
					value,
					number
				) )
			{
				questDialogueEditorSetMessage(
					"Action number must be an integer."
				);
				return false;
			}

			rapidjson::Value* action = questDialogueEditorSelectedRuleAction();
			const std::string member = questDialogueEditorSelectedRuleActionMember();
			if ( action && !member.empty() && action->HasMember(member.c_str()) )
			{
				rapidjson::Value& selected = (*action)[member.c_str()];
				if ( selected.IsInt()
					&& questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_ACTION_NUMBER )
				{
					if ( member == "reward_gold" || member == "remove_gold" )
						number = std::max(0, number);
					selected.SetInt(number);
					success = true;
				}
				else if ( selected.IsObject() )
				{
					const char* key = nullptr;
					if ( questDialogueEditorEditableField
						== QUEST_DIALOGUE_FIELD_ACTION_SECONDARY_NUMBER )
						key = member == "status_effect" ? "duration_seconds" : nullptr;
					else if ( questDialogueEditorEditableField
						== QUEST_DIALOGUE_FIELD_ACTION_TERTIARY_NUMBER )
						key = member == "status_effect" ? "strength" : nullptr;
					else if ( member == "reward_item" || member == "remove_item" ) key = "count";
					else if ( member == "status_effect" ) key = "effect";
					else if ( member.rfind("set_", 0) == 0
						&& member.find("variable") != std::string::npos ) key = "value";
					else if ( member.rfind("add_", 0) == 0
						&& member.find("variable") != std::string::npos ) key = "amount";
					if ( key )
					{
						if ( std::string(key) == "count" ) number = std::max(1, number);
						else if ( std::string(key) == "effect" ) number = std::max(0, number);
						else if ( std::string(key) == "duration_seconds" ) number = std::max(0, number);
						else if ( std::string(key) == "strength" ) number = std::max(1, std::min(255, number));
						success = questDialogueEditorWriteIntegerMember(selected, key, number);
					}
				}
			}
			break;
		}

		case QUEST_DIALOGUE_FIELD_OBJECTIVE_STAGE:
		case QUEST_DIALOGUE_FIELD_OBJECTIVE_TARGET:
		case QUEST_DIALOGUE_FIELD_OBJECTIVE_DEFEAT_ID:
		{
			int number = 0;
			if ( !questDialogueEditorParseInteger(
					value,
					number
				) )
			{
				questDialogueEditorSetMessage(
					"Objective number must be an integer."
				);
				return false;
			}

			rapidjson::Value* objective =
				questDialogueEditorSelectedObjectiveValueForEdit();

			if ( objective )
			{
				const char* member = "target";
				if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_OBJECTIVE_STAGE )
				{
					member = "stage";
				}
				else if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_OBJECTIVE_DEFEAT_ID )
				{
					member = "defeat_id";
					if ( number <= 0 )
					{
						questDialogueEditorSetMessage("Defeat entity ID must be positive.");
						return false;
					}
				}

				if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_OBJECTIVE_TARGET )
				{
					number = std::max(1, number);
				}

				success =
					questDialogueEditorWriteIntegerMember(
						*objective,
						member,
						number
					);
			}
			break;
		}

		case QUEST_DIALOGUE_FIELD_POWER_X:
		case QUEST_DIALOGUE_FIELD_POWER_Y:
		{
			int number = 0;
			if ( !questDialogueEditorParseInteger(
					value,
					number
				) )
			{
				questDialogueEditorSetMessage(
					"Power tile coordinate must be an integer."
				);
				return false;
			}

			rapidjson::Value* action = questDialogueEditorSelectedRuleAction();

			if ( action && action->HasMember("set_power")
				&& (*action)["set_power"].IsObject() )
			{
				success =
					questDialogueEditorWriteIntegerMember(
						(*action)["set_power"],
						questDialogueEditorEditableField
							== QUEST_DIALOGUE_FIELD_POWER_X
								? "x"
								: "y",
						number
					);
			}
			break;
		}

		case QUEST_DIALOGUE_FIELD_MARKER_MAP:
		case QUEST_DIALOGUE_FIELD_MARKER_X:
		case QUEST_DIALOGUE_FIELD_MARKER_Y:
		case QUEST_DIALOGUE_FIELD_MARKER_FLOOR:
		{
			rapidjson::Value* objective =
				questDialogueEditorSelectedObjectiveValueForEdit();

			if ( objective
				&& objective->IsObject()
				&& objective->HasMember("map_marker")
				&& (*objective)["map_marker"].IsObject() )
			{
				if ( questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_MARKER_MAP )
				{
					success = questDialogueEditorWriteStringMember(
						(*objective)["map_marker"], "map", value);
				}
				else
				{
					int number = 0;
					if ( !questDialogueEditorParseInteger(value, number) )
					{
						questDialogueEditorSetMessage("Marker coordinate must be an integer.");
						return false;
					}
					number = std::max(0, number);
					success = questDialogueEditorWriteIntegerMember(
						(*objective)["map_marker"],
						questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_MARKER_X
							? "x" : (questDialogueEditorEditableField
								== QUEST_DIALOGUE_FIELD_MARKER_Y ? "y" : "playable_floor"), number);
				}
			}
			break;
		}

		case QUEST_DIALOGUE_FIELD_ORIGIN_LABEL:
		case QUEST_DIALOGUE_FIELD_ORIGIN_MAP:
		case QUEST_DIALOGUE_FIELD_ORIGIN_X:
		case QUEST_DIALOGUE_FIELD_ORIGIN_Y:
		case QUEST_DIALOGUE_FIELD_ORIGIN_FLOOR:
		case QUEST_DIALOGUE_FIELD_ORIGIN_NPC_ID:
		{
			rapidjson::Value* quest = questDialogueEditorQuestValue();
			if ( !quest ) break;
			rapidjson::Value& origin = questDialogueEditorSetObjectMember(*quest, "origin");
			if ( questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_ORIGIN_LABEL
				|| questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_ORIGIN_MAP )
			{
				success = questDialogueEditorWriteStringMember(origin,
					questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_ORIGIN_LABEL
						? "label" : "map", value);
			}
			else
			{
				int number = 0;
				if ( !questDialogueEditorParseInteger(value, number) )
				{
					questDialogueEditorSetMessage("Giver coordinate/ID must be an integer.");
					return false;
				}
				if ( questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_ORIGIN_NPC_ID
					&& number <= 0 )
				{
					questDialogueEditorSetMessage("Persistent NPC ID must be positive.");
					return false;
				}
				number = std::max(0, number);
				const char* member = questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_ORIGIN_X ? "x"
					: (questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_ORIGIN_Y
						? "y" : (questDialogueEditorEditableField
							== QUEST_DIALOGUE_FIELD_ORIGIN_FLOOR
								? "playable_floor" : "npc_persistent_id"));
				success = questDialogueEditorWriteIntegerMember(origin, member, number);
				if ( success && questDialogueEditorEditableField
					== QUEST_DIALOGUE_FIELD_ORIGIN_NPC_ID )
				{
					questDialogueEditorSetBoolMember(origin, "track_npc", true);
					origin.RemoveMember("x");
					origin.RemoveMember("y");
				}
				else if ( success && questDialogueEditorEditableField
					!= QUEST_DIALOGUE_FIELD_ORIGIN_FLOOR )
				{
					questDialogueEditorSetBoolMember(origin, "track_npc", false);
					origin.RemoveMember("npc_persistent_id");
				}
			}
			break;
		}

		default:
			break;
	}

	if ( !success )
	{
		questDialogueEditorSetMessage(
			"The selected field is not available."
		);
		return false;
	}

	questDialogueEditorEditingField = false;
	SDL_StopTextInput();
	if ( inputstr == questDialogueEditorEditBuffer ) inputstr = nullptr;

	return questDialogueEditorSaveDocument();
}


static bool questDialogueEditorRenameSelectedFile()
{
	if ( questDialogueEditorSelectedFile < 0
		|| questDialogueEditorSelectedFile
			>= static_cast<int>(
				questDialogueEditorFiles.size()
			) )
	{
		questDialogueEditorSetMessage(
			"No dialogue file is selected."
		);
		return false;
	}

	if ( questDialogueEditorModel.dirty()
		&& !questDialogueEditorWriteDocument() )
	{
		questDialogueEditorSetMessage(
			"Save the valid document before renaming its file.");
		return false;
	}

	if ( questDialogueEditorEditableField
		!= QUEST_DIALOGUE_FIELD_FILE_ID )
	{
		questDialogueEditorSetMessage(
			"Select Dialogue/File ID before renaming."
		);
		return false;
	}

	const std::string normalized =
		questEditorNormalizeID(
			questDialogueEditorEditBuffer
		);

	if ( normalized.empty() )
	{
		questDialogueEditorSetMessage(
			"Dialogue/File ID cannot be empty."
		);
		return false;
	}

	const std::string oldFilename =
		questDialogueEditorFiles[
			questDialogueEditorSelectedFile
		];
	const std::string oldDialogueID = oldFilename.size() > 5
		? oldFilename.substr(0, oldFilename.size() - 5)
		: oldFilename;

	const std::string newFilename =
		normalized + ".json";

	if ( oldFilename == newFilename )
	{
		questDialogueEditorSetMessage(
			"The file already has that name."
		);
		return false;
	}

	const std::string oldPath =
		"./dialogue/" + oldFilename;

	const std::string newPath =
		"./dialogue/" + newFilename;

	if ( access(newPath.c_str(), F_OK) == 0 )
	{
		questDialogueEditorSetMessage(
			"A dialogue file with that name already exists."
		);
		return false;
	}

	if ( std::rename(
			oldPath.c_str(),
			newPath.c_str()
		) != 0 )
	{
		questDialogueEditorSetMessage(
			"Could not rename the dialogue file."
		);
		return false;
	}

	/*
	 * The NPC property window stores its currently edited dialogue ID in
	 * spriteProperties[26]. Keep that pending value synchronized.
	 */
	if ( std::string(spriteProperties[26]) == oldDialogueID )
	{
	strncpy(
		spriteProperties[26],
		normalized.c_str(),
		sizeof(spriteProperties[26]) - 1
	);
	spriteProperties[26][
		sizeof(spriteProperties[26]) - 1
	] = '\0';

	}

	/*
	 * Also update the selected live NPC immediately when it has stats.
	 * This keeps the sprite pointed at the renamed JSON even before the
	 * properties window is reopened.
	 */
	if ( selectedEntity[0] )
	{
		Stat* selectedStats =
			selectedEntity[0]->getStats();

		if ( selectedStats
			&& std::string(selectedStats->customDialogueID) == oldDialogueID )
		{
			strncpy(
				selectedStats->customDialogueID,
				normalized.c_str(),
				sizeof(
					selectedStats->customDialogueID
				) - 1
			);

			selectedStats->customDialogueID[
				sizeof(
					selectedStats->customDialogueID
				) - 1
			] = '\0';
		}
	}

	questDialogueEditorRefreshFiles();

	questDialogueEditorSelectedFile = -1;

	for ( int index = 0;
		index < static_cast<int>(
			questDialogueEditorFiles.size()
		);
		++index )
	{
		if ( questDialogueEditorFiles[index]
			== newFilename )
		{
			questDialogueEditorSelectedFile = index;
			break;
		}
	}

	if ( questDialogueEditorSelectedFile < 0 )
	{
		questDialogueEditorSetMessage(
			"File renamed, but it could not be reselected."
		);
		return false;
	}

	questDialogueEditorLoadPreview(newFilename);

	strncpy(
		questDialogueEditorEditBuffer,
		normalized.c_str(),
		sizeof(questDialogueEditorEditBuffer) - 1
	);

	questDialogueEditorEditBuffer[
		sizeof(questDialogueEditorEditBuffer) - 1
	] = '\0';

	questDialogueEditorEditingField = false;
	SDL_StopTextInput();
	if ( inputstr == questDialogueEditorEditBuffer ) inputstr = nullptr;

	questDialogueEditorSetMessage(
		"Renamed file to " + newFilename
	);

	return true;
}

static void questDialogueEditorBeginEditingField()
{
	questDialogueEditorEndTransientTextInput();
    questDialogueEditorLockedEditableField =
        questDialogueEditorEditableField;
    questDialogueEditorLockedFieldCategory =
        questDialogueEditorFieldCategory;
	questDialogueEditorLockedRuleOwnerNode =
		questDialogueEditorRuleOwnerNode;

	const std::string currentValue =
		questDialogueEditorReadEditableField();

	strncpy(
		questDialogueEditorEditBuffer,
		currentValue.c_str(),
		sizeof(questDialogueEditorEditBuffer) - 1
	);

	questDialogueEditorEditBuffer[
		sizeof(questDialogueEditorEditBuffer) - 1
	] = '\0';

	inputstr = questDialogueEditorEditBuffer;
	inputlen =
		sizeof(questDialogueEditorEditBuffer) - 1;

	questDialogueEditorEditingField = true;
	cursorflash = ticks;

	if ( !SDL_IsTextInputActive() )
	{
		SDL_StartTextInput();
	}
}

static const char* questDialogueEditorFieldCategoryName()
{
	switch ( questDialogueEditorFieldCategory )
	{
		case QUEST_DIALOGUE_CATEGORY_FILE_QUEST:
			return "File/Quest";
		case QUEST_DIALOGUE_CATEGORY_TEXT:
			return "Text";
		case QUEST_DIALOGUE_CATEGORY_OBJECTIVE:
			return "Objective";
		case QUEST_DIALOGUE_CATEGORY_CONDITION:
			return "Condition";
		case QUEST_DIALOGUE_CATEGORY_ACTION:
			return "Action";
		case QUEST_DIALOGUE_CATEGORY_MARKER:
			return "Marker";
		default:
			return "Unknown";
	}
}

static std::vector<QuestDialogueEditableField>
questDialogueEditorCategoryFields()
{
	switch ( questDialogueEditorFieldCategory )
	{
		case QUEST_DIALOGUE_CATEGORY_FILE_QUEST:
			return {
				QUEST_DIALOGUE_FIELD_FILE_ID,
				QUEST_DIALOGUE_FIELD_QUEST_ID,
				QUEST_DIALOGUE_FIELD_QUEST_TITLE,
				QUEST_DIALOGUE_FIELD_QUEST_SUMMARY,
				QUEST_DIALOGUE_FIELD_QUEST_OBJECTIVE,
				QUEST_DIALOGUE_FIELD_QUEST_COMPLETED_TEXT,
				QUEST_DIALOGUE_FIELD_QUEST_FAILED_TEXT,
				QUEST_DIALOGUE_FIELD_ORIGIN_LABEL,
				QUEST_DIALOGUE_FIELD_ORIGIN_MAP,
				QUEST_DIALOGUE_FIELD_ORIGIN_X,
				QUEST_DIALOGUE_FIELD_ORIGIN_Y,
				QUEST_DIALOGUE_FIELD_ORIGIN_FLOOR,
				QUEST_DIALOGUE_FIELD_ORIGIN_NPC_ID
			};
		case QUEST_DIALOGUE_CATEGORY_TEXT:
			return {
				QUEST_DIALOGUE_FIELD_LEGACY_TEXT,
				QUEST_DIALOGUE_FIELD_NODE_ID,
				QUEST_DIALOGUE_FIELD_NODE_TEXT,
				QUEST_DIALOGUE_FIELD_NODE_NEXT,
				QUEST_DIALOGUE_FIELD_CHOICE_ID,
				QUEST_DIALOGUE_FIELD_CHOICE_TEXT,
				QUEST_DIALOGUE_FIELD_CHOICE_NEXT
			};
		case QUEST_DIALOGUE_CATEGORY_OBJECTIVE:
			return {
				QUEST_DIALOGUE_FIELD_OBJECTIVE_ID,
				QUEST_DIALOGUE_FIELD_OBJECTIVE_TEXT,
				QUEST_DIALOGUE_FIELD_OBJECTIVE_COMPLETED_TEXT,
				QUEST_DIALOGUE_FIELD_OBJECTIVE_PROGRESS_VARIABLE,
				QUEST_DIALOGUE_FIELD_OBJECTIVE_STAGE,
				QUEST_DIALOGUE_FIELD_OBJECTIVE_TARGET,
				QUEST_DIALOGUE_FIELD_OBJECTIVE_DEFEAT_ID
			};
		case QUEST_DIALOGUE_CATEGORY_CONDITION:
			return {
				QUEST_DIALOGUE_FIELD_CONDITION_REFERENCE,
				QUEST_DIALOGUE_FIELD_CONDITION_QUEST,
				QUEST_DIALOGUE_FIELD_CONDITION_NUMBER,
				QUEST_DIALOGUE_FIELD_CONDITION_STABLE_ID,
				QUEST_DIALOGUE_FIELD_CONDITION_TRUE_NODE,
				QUEST_DIALOGUE_FIELD_CONDITION_FALSE_NODE
			};
		case QUEST_DIALOGUE_CATEGORY_ACTION:
			return {
				QUEST_DIALOGUE_FIELD_ACTION_REFERENCE,
				QUEST_DIALOGUE_FIELD_ACTION_NUMBER,
				QUEST_DIALOGUE_FIELD_ACTION_SECONDARY_NUMBER,
				QUEST_DIALOGUE_FIELD_ACTION_TERTIARY_NUMBER,
				QUEST_DIALOGUE_FIELD_ACTION_STABLE_ID,
				QUEST_DIALOGUE_FIELD_NODE_ACTION_ID,
				QUEST_DIALOGUE_FIELD_POWER_X,
				QUEST_DIALOGUE_FIELD_POWER_Y
			};
		case QUEST_DIALOGUE_CATEGORY_MARKER:
			return {
				QUEST_DIALOGUE_FIELD_MARKER_MAP,
				QUEST_DIALOGUE_FIELD_MARKER_X,
				QUEST_DIALOGUE_FIELD_MARKER_Y,
				QUEST_DIALOGUE_FIELD_MARKER_FLOOR
			};
		default:
			return { QUEST_DIALOGUE_FIELD_QUEST_TITLE };
	}
}

static void questDialogueEditorCycleEditableFieldDirection(
	const int direction
)
{
	const std::vector<QuestDialogueEditableField> fields =
		questDialogueEditorCategoryFields();

	int index = 0;
	for ( int i = 0; i < static_cast<int>(fields.size()); ++i )
	{
		if ( fields[i] == questDialogueEditorEditableField )
		{
			index = i;
			break;
		}
	}

	index += direction;
	if ( index < 0 )
	{
		index = static_cast<int>(fields.size()) - 1;
	}
	else if ( index >= static_cast<int>(fields.size()) )
	{
		index = 0;
	}

	questDialogueEditorEditableField = fields[index];

	if ( questDialogueEditorEditingField )
	{
		questDialogueEditorBeginEditingField();
	}
}

static void questDialogueEditorCycleEditableField()
{
	questDialogueEditorCycleEditableFieldDirection(1);
}

static void questDialogueEditorCycleFieldCategory(
	const int direction
)
{
	int category =
		static_cast<int>(questDialogueEditorFieldCategory)
		+ direction;

	if ( category < 0 )
	{
		category = QUEST_DIALOGUE_CATEGORY_COUNT - 1;
	}
	else if ( category >= QUEST_DIALOGUE_CATEGORY_COUNT )
	{
		category = 0;
	}

	questDialogueEditorFieldCategory =
		static_cast<QuestDialogueFieldCategory>(category);

	const std::vector<QuestDialogueEditableField> fields =
		questDialogueEditorCategoryFields();

	questDialogueEditorEditableField = fields.front();

	if ( questDialogueEditorEditingField )
	{
		questDialogueEditorBeginEditingField();
	}
}


static bool questDialogueEditorCycleScopeDirect()
{
	rapidjson::Value* quest =
		questDialogueEditorQuestValue();

	if ( !quest )
	{
		return false;
	}

	std::string scope = "player";
	if ( quest->HasMember("scope")
		&& (*quest)["scope"].IsString() )
	{
		scope = questEditorNormalizeID((*quest)["scope"].GetString());
	}

	scope = scope == "player" ? "party"
		: (scope == "party" ? "world" : "player");

	questDialogueEditorWriteStringMember(
		*quest, "scope", scope
	);
	const int schemaVersion = questDialogueEditorDocument.HasMember("version")
		&& questDialogueEditorDocument["version"].IsInt()
		? questDialogueEditorDocument["version"].GetInt() : 0;
	const std::string label = scope == "player" ? "Personal"
		: (scope == "party" ? "Party" : "World");
	questDialogueEditorSetMessage(schemaVersion
		>= automatia::dialogue::SharedQuestOwnershipSchemaVersion
		? "Scope: " + label + " (server-authoritative)."
		: "Authored scope: " + label
			+ ". Schema 1 keeps legacy Personal ownership until UPGRADE SHARING is used.");
	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorUpgradeSharedQuestOwnership()
{
	if ( !questDialogueEditorDocument.IsObject()
		|| !questDialogueEditorDocument.HasMember("version")
		|| !questDialogueEditorDocument["version"].IsInt() )
	{
		questDialogueEditorSetMessage(
			"Cannot upgrade: the document has no valid schema version.");
		return false;
	}
	if ( questDialogueEditorDocument["version"].GetInt()
		>= automatia::dialogue::SharedQuestOwnershipSchemaVersion )
	{
		questDialogueEditorSetMessage(
			"This dialogue already uses true shared quest ownership.");
		return true;
	}
	questDialogueEditorDocument["version"].SetInt(
		automatia::dialogue::SharedQuestOwnershipSchemaVersion);
	questDialogueEditorSetMessage(
		"Upgraded to schema 2. Authored Party/World scopes now use true shared ownership.");
	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorCycleComparisonDirect()
{
	rapidjson::Value* choice =
		questDialogueEditorSelectedChoiceValueForEdit();

	if ( !choice || !choice->IsObject()
		|| !choice->HasMember("condition")
		|| !(*choice)["condition"].IsObject() )
	{
		questDialogueEditorSetMessage(
			"Select a choice with a condition first."
		);
		return false;
	}

	rapidjson::Value& condition =
		(*choice)["condition"];

	std::string comparison = "equals";
	if ( condition.HasMember("comparison")
		&& condition["comparison"].IsString() )
	{
		comparison = condition["comparison"].GetString();
	}

	comparison =
		comparison == "equals" ? "not_equals"
		: comparison == "not_equals" ? "at_least"
		: comparison == "at_least" ? "at_most"
		: "equals";

	questDialogueEditorWriteStringMember(
		condition, "comparison", comparison
	);
	questDialogueEditorSetMessage(
		"Comparison: " + comparison
	);
	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorCycleConditionItemDirect()
{
	rapidjson::Value* choice =
		questDialogueEditorSelectedChoiceValueForEdit();

	if ( !choice || !choice->IsObject()
		|| !choice->HasMember("condition")
		|| !(*choice)["condition"].IsObject() )
	{
		questDialogueEditorSetMessage(
			"Select a has_item condition first."
		);
		return false;
	}

	rapidjson::Value& condition =
		(*choice)["condition"];

	if ( !condition.HasMember("type")
		|| !condition["type"].IsString()
		|| questEditorNormalizeID(condition["type"].GetString())
			!= "has_item" )
	{
		questDialogueEditorSetMessage(
			"COND ITEM requires has_item."
		);
		return false;
	}

	std::string item = "torch";
	if ( condition.HasMember("item")
		&& condition["item"].IsString() )
	{
		item = condition["item"].GetString();
	}

	item = item == "torch" ? "tool_torch" : "torch";

	questDialogueEditorWriteStringMember(
		condition, "item", item
	);
	questDialogueEditorSetMessage(
		"Condition item: " + item
	);
	return questDialogueEditorSaveDocument();
}

static rapidjson::Value* questDialogueEditorEnsureChoiceAction();

static bool questDialogueEditorCycleRewardItemDirect()
{
	rapidjson::Value* action =
		questDialogueEditorEnsureChoiceAction();

	if ( !action )
	{
		questDialogueEditorSetMessage(
			"Select a choice first."
		);
		return false;
	}

	auto& allocator =
		questDialogueEditorDocument.GetAllocator();

	std::string current;
	if ( action->HasMember("reward_item")
		&& (*action)["reward_item"].IsObject()
		&& (*action)["reward_item"].HasMember("item")
		&& (*action)["reward_item"]["item"].IsString() )
	{
		current =
			(*action)["reward_item"]["item"].GetString();
	}

	if ( current.empty() )
	{
		rapidjson::Value reward(rapidjson::kObjectType);
		rapidjson::Value item;
		item.SetString("healing_potion", allocator);
		reward.AddMember("item", item, allocator);
		reward.AddMember("count", 1, allocator);
		if ( action->HasMember("reward_item") )
		{
			(*action)["reward_item"] = std::move(reward);
		}
		else
		{
			action->AddMember("reward_item", reward, allocator);
		}
		questDialogueEditorSetMessage(
			"Reward item: healing_potion"
		);
	}
	else if ( current == "healing_potion"
		|| current == "potion_healing" )
	{
		questDialogueEditorWriteStringMember(
			(*action)["reward_item"], "item", "torch"
		);
		questDialogueEditorSetMessage(
			"Reward item: torch"
		);
	}
	else
	{
		action->RemoveMember("reward_item");
		questDialogueEditorSetMessage(
			"Reward item: Off"
		);
	}

	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorToggleRemoveItemDirect()
{
	rapidjson::Value* action =
		questDialogueEditorEnsureChoiceAction();

	if ( !action )
	{
		questDialogueEditorSetMessage(
			"Select a choice first."
		);
		return false;
	}

	auto& allocator =
		questDialogueEditorDocument.GetAllocator();

	if ( action->HasMember("remove_item") )
	{
		action->RemoveMember("remove_item");
		questDialogueEditorSetMessage(
			"Remove item: Off"
		);
	}
	else
	{
		rapidjson::Value removeItem(
			rapidjson::kObjectType
		);
		rapidjson::Value item;
		item.SetString("torch", allocator);
		removeItem.AddMember("item", item, allocator);
		removeItem.AddMember("count", 1, allocator);
		action->AddMember(
			"remove_item", removeItem, allocator
		);
		questDialogueEditorSetMessage(
			"Remove item: torch"
		);
	}

	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorToggleRemoveGoldDirect()
{
	rapidjson::Value* action =
		questDialogueEditorEnsureChoiceAction();

	if ( !action )
	{
		questDialogueEditorSetMessage(
			"Select a choice first."
		);
		return false;
	}

	auto& allocator =
		questDialogueEditorDocument.GetAllocator();

	if ( action->HasMember("remove_gold") )
	{
		action->RemoveMember("remove_gold");
		questDialogueEditorSetMessage(
			"Remove gold: Off"
		);
	}
	else
	{
		action->AddMember(
			"remove_gold", 100, allocator
		);
		questDialogueEditorSetMessage(
			"Remove gold: 100"
		);
	}

	return questDialogueEditorSaveDocument();
}



static const char* questDialogueEditorActionGroupName()
{
	switch ( questDialogueEditorActionGroup )
	{
		case QUEST_DIALOGUE_ACTION_GROUP_QUEST:
			return "Quest";
		case QUEST_DIALOGUE_ACTION_GROUP_REWARDS:
			return "Rewards";
		case QUEST_DIALOGUE_ACTION_GROUP_COSTS:
			return "Costs";
		case QUEST_DIALOGUE_ACTION_GROUP_OBJECTIVES:
			return "Objectives";
		case QUEST_DIALOGUE_ACTION_GROUP_FLAGS:
			return "Flags";
		case QUEST_DIALOGUE_ACTION_GROUP_VARIABLES:
			return "Variables";
		case QUEST_DIALOGUE_ACTION_GROUP_NPC:
			return "NPC";
		case QUEST_DIALOGUE_ACTION_GROUP_MECHANISMS:
			return "Mechanisms";
		case QUEST_DIALOGUE_ACTION_GROUP_STATUS:
			return "Status";
		default:
			return "Quest";
	}
}

static void questDialogueEditorCycleActionGroup(
	const int direction
)
{
	int group =
		static_cast<int>(
			questDialogueEditorActionGroup
		);

	group += direction;

	if ( group < 0 )
	{
		group =
			QUEST_DIALOGUE_ACTION_GROUP_COUNT - 1;
	}
	else if ( group
		>= QUEST_DIALOGUE_ACTION_GROUP_COUNT )
	{
		group = 0;
	}

	questDialogueEditorActionGroup =
		static_cast<QuestDialogueActionGroup>(
			group
		);

	questDialogueEditorSetMessage(
		std::string("Action group: ")
		+ questDialogueEditorActionGroupName()
	);
}

static rapidjson::Value* questDialogueEditorEnsureChoiceAction()
{
	rapidjson::Value* choice =
		questDialogueEditorSelectedChoiceValueForEdit();

	if ( !choice || !choice->IsObject() )
	{
		questDialogueEditorSetMessage(
			"Select a choice first."
		);
		return nullptr;
	}

	auto& allocator =
		questDialogueEditorDocument.GetAllocator();

	if ( !choice->HasMember("action") )
	{
		rapidjson::Value action(
			rapidjson::kObjectType
		);

		choice->AddMember(
			"action",
			action,
			allocator
		);
	}

	if ( !(*choice)["action"].IsObject() )
	{
		(*choice)["action"].SetObject();
	}

	return &(*choice)["action"];
}

static void questDialogueEditorClearChoiceAction()
{
	rapidjson::Value* choice =
		questDialogueEditorSelectedChoiceValueForEdit();

	if ( !choice )
	{
		questDialogueEditorSetMessage(
			"Select a choice first."
		);
		return;
	}

	if ( choice->HasMember("action") )
	{
		choice->RemoveMember("action");
		questDialogueEditorSaveDocument();
		questDialogueEditorSetMessage(
			"Choice action cleared."
		);
	}
}

static std::string questDialogueEditorDefaultObjectiveID()
{
	rapidjson::Value* objective =
		questDialogueEditorSelectedObjectiveValueForEdit();

	if ( objective
		&& objective->IsObject()
		&& objective->HasMember("id")
		&& (*objective)["id"].IsString() )
	{
		return (*objective)["id"].GetString();
	}

	return "objective_1";
}

static void questDialogueEditorSetStringMember(
	rapidjson::Value& object,
	const char* name,
	const std::string& value
)
{
	auto& allocator =
		questDialogueEditorDocument.GetAllocator();

	if ( object.HasMember(name) )
	{
		object[name].SetString(
			value.c_str(),
			allocator
		);
	}
	else
	{
		rapidjson::Value memberValue;
		memberValue.SetString(
			value.c_str(),
			allocator
		);

		rapidjson::Value memberName;
		memberName.SetString(
			name,
			allocator
		);

		object.AddMember(
			memberName,
			memberValue,
			allocator
		);
	}
}

static void questDialogueEditorSetBoolMember(
	rapidjson::Value& object,
	const char* name,
	const bool value
)
{
	auto& allocator =
		questDialogueEditorDocument.GetAllocator();

	if ( object.HasMember(name) )
	{
		object[name].SetBool(value);
	}
	else
	{
		rapidjson::Value memberName;
		memberName.SetString(
			name,
			allocator
		);

		object.AddMember(
			memberName,
			value,
			allocator
		);
	}
}

static void questDialogueEditorSetIntMember(
	rapidjson::Value& object,
	const char* name,
	const int value
)
{
	auto& allocator =
		questDialogueEditorDocument.GetAllocator();

	if ( object.HasMember(name) )
	{
		object[name].SetInt(value);
	}
	else
	{
		rapidjson::Value memberName;
		memberName.SetString(
			name,
			allocator
		);

		object.AddMember(
			memberName,
			value,
			allocator
		);
	}
}

static rapidjson::Value& questDialogueEditorSetObjectMember(
	rapidjson::Value& parent,
	const char* name
)
{
	auto& allocator =
		questDialogueEditorDocument.GetAllocator();

	if ( !parent.HasMember(name) )
	{
		rapidjson::Value memberName;
		memberName.SetString(
			name,
			allocator
		);

		rapidjson::Value object(
			rapidjson::kObjectType
		);

		parent.AddMember(
			memberName,
			object,
			allocator
		);
	}
	else if ( !parent[name].IsObject() )
	{
		parent[name].SetObject();
	}

	return parent[name];
}

static rapidjson::Value questDialogueEditorDefaultCondition(
	const std::string& type,
	const bool nodeCondition
)
{
	auto& allocator = questDialogueEditorDocument.GetAllocator();
	rapidjson::Value condition(rapidjson::kObjectType);
	questDialogueEditorSetStringMember(condition, "type", type);
	const std::string questID = questDialogueEditorPreview.questID.empty()
		? "quest_id" : questDialogueEditorPreview.questID;
	const int nodeID = questDialogueEditorNodeIDAt(
		std::max(0, questDialogueEditorSelectedNode));

	if ( type == "has_item" )
	{
		questDialogueEditorSetStringMember(condition, "item",
			std::to_string(questDialogueEditorSelectedItemID));
		questDialogueEditorSetIntMember(condition, "count",
			std::max(1, questDialogueEditorSelectedItemCount));
		if ( nodeCondition ) questDialogueEditorSetBoolMember(condition, "consume", false);
	}
	else if ( type == "has_gold" )
	{
		questDialogueEditorSetIntMember(condition, "amount",
			std::max(0, questDialogueEditorGoldAmount));
		if ( nodeCondition ) questDialogueEditorSetBoolMember(condition, "consume", false);
	}
	else if ( type == "quest_started" || type == "quest_accepted"
		|| type == "quest_completed" || type == "quest_failed"
		|| type == "quest_stage" )
	{
		questDialogueEditorSetStringMember(condition, "quest", questID);
		if ( type == "quest_stage" )
		{
			questDialogueEditorSetIntMember(condition, "stage", 1);
			questDialogueEditorSetStringMember(condition, "comparison", "equals");
		}
	}
	else if ( type == "objective_completed" || type == "objective_incomplete" )
	{
		questDialogueEditorSetStringMember(condition, "quest", questID);
		questDialogueEditorSetStringMember(condition, "objective",
			questDialogueEditorDefaultObjectiveID());
	}
	else if ( type == "node_seen" )
	{
		questDialogueEditorSetStringMember(condition, "node",
			"node_" + std::to_string(nodeID));
	}
	else if ( type == "world_flag" || type == "npc_flag" )
	{
		questDialogueEditorSetStringMember(condition, "id",
			type == "world_flag" ? "story_flag" : "npc_flag");
		questDialogueEditorSetBoolMember(condition, "value", true);
	}
	else if ( type == "world_variable" || type == "npc_variable" )
	{
		questDialogueEditorSetStringMember(condition, "id",
			type == "world_variable" ? "world_value" : "npc_value");
		questDialogueEditorSetIntMember(condition, "value", 1);
		questDialogueEditorSetStringMember(condition, "comparison", "equals");
	}

	if ( nodeCondition )
	{
		condition.AddMember("true_node", nodeID, allocator);
		condition.AddMember("false_node", nodeID, allocator);
	}
	return condition;
}

static bool questDialogueEditorReplaceRuleCondition(
	const std::string& type
)
{
	const auto& supported = questDialogueEditorRuleOwnerNode
		? automatia::dialogue::nodeConditionTypes()
		: automatia::dialogue::choiceConditionTypes();
	if ( std::find(supported.begin(), supported.end(), type) == supported.end() )
	{
		questDialogueEditorSetMessage("That condition is not supported here.");
		return false;
	}
	auto& allocator = questDialogueEditorDocument.GetAllocator();
	rapidjson::Value replacement = questDialogueEditorDefaultCondition(
		type, questDialogueEditorRuleOwnerNode);
	if ( questDialogueEditorRuleOwnerNode )
	{
		rapidjson::Value* node = questDialogueEditorSelectedNodeValue();
		if ( !node ) return false;
		if ( node->HasMember("condition") ) (*node)["condition"] = std::move(replacement);
		else node->AddMember("condition", replacement, allocator);
	}
	else
	{
		rapidjson::Value* choice = questDialogueEditorSelectedChoiceValue();
		if ( !choice ) return false;
		const bool singular = choice->HasMember("condition")
			&& (*choice)["condition"].IsObject();
		if ( singular && questDialogueEditorSelectedConditionIndex == 0 )
		{
			(*choice)["condition"] = std::move(replacement);
		}
		else if ( choice->HasMember("conditions") && (*choice)["conditions"].IsArray()
			&& !(*choice)["conditions"].Empty() )
		{
			rapidjson::Value& conditions = (*choice)["conditions"];
			const int arrayIndex = std::max(0, std::min(
				questDialogueEditorSelectedConditionIndex - (singular ? 1 : 0),
				static_cast<int>(conditions.Size()) - 1));
			conditions[static_cast<rapidjson::SizeType>(
				arrayIndex)] = std::move(replacement);
		}
		else
		{
			rapidjson::Value conditions(rapidjson::kArrayType);
			conditions.PushBack(replacement, allocator);
			choice->AddMember("conditions", conditions, allocator);
			questDialogueEditorSelectedConditionIndex = 0;
		}
	}
	questDialogueEditorSetMessage("Condition: " + type);
	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorAddChoiceCondition(
	const std::string& type
)
{
	if ( !automatia::dialogue::isChoiceConditionType(type) ) return false;
	rapidjson::Value* choice = questDialogueEditorSelectedChoiceValue();
	if ( !choice )
	{
		questDialogueEditorSetMessage("Select a choice first.");
		return false;
	}
	auto& allocator = questDialogueEditorDocument.GetAllocator();
	rapidjson::Value& conditions = questDialogueEditorEnsureConditionArray(*choice);
	conditions.PushBack(questDialogueEditorDefaultCondition(type, false), allocator);
	questDialogueEditorSelectedConditionIndex = static_cast<int>(conditions.Size()) - 1;
	questDialogueEditorSetMessage("Added requirement: " + type);
	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorRemoveRuleCondition()
{
	if ( questDialogueEditorRuleOwnerNode )
	{
		rapidjson::Value* node = questDialogueEditorSelectedNodeValue();
		if ( !node || !node->HasMember("condition") ) return false;
		node->RemoveMember("condition");
		questDialogueEditorSetMessage("Node redirect condition removed.");
		return questDialogueEditorSaveDocument();
	}
	return questDialogueEditorClearChoiceCondition();
}

static bool questDialogueEditorCycleRuleConditionType(const int direction)
{
	const auto& supported = questDialogueEditorRuleOwnerNode
		? automatia::dialogue::nodeConditionTypes()
		: automatia::dialogue::choiceConditionTypes();
	if ( supported.empty() ) return false;
	std::string current;
	if ( rapidjson::Value* condition = questDialogueEditorSelectedRuleCondition() )
	{
		if ( condition->HasMember("type") && (*condition)["type"].IsString() )
			current = questEditorNormalizeID((*condition)["type"].GetString());
	}
	int index = 0;
	const auto found = std::find(supported.begin(), supported.end(), current);
	if ( found != supported.end() ) index = static_cast<int>(found - supported.begin());
	else if ( direction < 0 ) index = static_cast<int>(supported.size()) - 1;
	else index = -1;
	index = (index + direction + static_cast<int>(supported.size()))
		% static_cast<int>(supported.size());
	return questDialogueEditorReplaceRuleCondition(supported[index]);
}

static bool questDialogueEditorCycleRuleComparison()
{
	rapidjson::Value* condition = questDialogueEditorSelectedRuleCondition();
	if ( !condition ) return false;
	const std::string type = condition->HasMember("type")
		&& (*condition)["type"].IsString()
		? questEditorNormalizeID((*condition)["type"].GetString()) : "";
	const bool nodeStage = questDialogueEditorRuleOwnerNode && type == "quest_stage";
	static const char* all[] = { "equals", "not_equals", "at_least", "at_most" };
	const int count = nodeStage ? 3 : 4;
	std::string current = condition->HasMember("comparison")
		&& (*condition)["comparison"].IsString()
		? questEditorNormalizeID((*condition)["comparison"].GetString()) : "equals";
	int index = 0;
	for ( int i = 0; i < count; ++i ) if ( current == all[nodeStage && i > 0 ? i + 1 : i] ) index = i;
	index = (index + 1) % count;
	const char* next = all[nodeStage && index > 0 ? index + 1 : index];
	questDialogueEditorSetStringMember(*condition, "comparison", next);
	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorToggleRuleConditionBoolean(const char* member)
{
	rapidjson::Value* condition = questDialogueEditorSelectedRuleCondition();
	if ( !condition || !member ) return false;
	const bool current = condition->HasMember(member) && (*condition)[member].IsBool()
		? (*condition)[member].GetBool() : false;
	questDialogueEditorSetBoolMember(*condition, member, !current);
	return questDialogueEditorSaveDocument();
}

static rapidjson::Value* questDialogueEditorEnsureRuleAction()
{
	auto& allocator = questDialogueEditorDocument.GetAllocator();
	if ( questDialogueEditorRuleOwnerNode )
	{
		rapidjson::Value* node = questDialogueEditorSelectedNodeValue();
		if ( !node ) return nullptr;
		if ( !node->HasMember("action") )
		{
			rapidjson::Value action(rapidjson::kObjectType);
			const int id = node->HasMember("id") && (*node)["id"].IsInt()
				? (*node)["id"].GetInt() : questDialogueEditorSelectedNode;
			questDialogueEditorSetStringMember(action, "id",
				"node_" + std::to_string(id) + "_once");
			node->AddMember("action", action, allocator);
		}
		else if ( !(*node)["action"].IsObject() ) (*node)["action"].SetObject();
		rapidjson::Value& action = (*node)["action"];
		if ( !action.HasMember("id") )
		{
			const int id = node->HasMember("id") && (*node)["id"].IsInt()
				? (*node)["id"].GetInt() : questDialogueEditorSelectedNode;
			questDialogueEditorSetStringMember(action, "id",
				"node_" + std::to_string(id) + "_once");
		}
		return &action;
	}
	return questDialogueEditorEnsureChoiceAction();
}

static bool questDialogueEditorAddRuleActionField(const std::string& field)
{
	if ( questDialogueEditorRuleOwnerNode
		? !automatia::dialogue::isNodeActionField(field) || field == "id"
		: !automatia::dialogue::isChoiceActionField(field) )
	{
		questDialogueEditorSetMessage("That action is not supported here.");
		return false;
	}
	rapidjson::Value* action = questDialogueEditorEnsureRuleAction();
	if ( !action ) return false;
	if ( !action->HasMember(field.c_str()) )
	{
		auto& allocator = questDialogueEditorDocument.GetAllocator();
		if ( field == "quest_start" || field == "quest_accept"
			|| field == "quest_complete" || field == "quest_fail"
			|| field == "quest_reset" || field == "recruit_npc" )
		{
			questDialogueEditorSetBoolMember(*action, field.c_str(), true);
		}
		else if ( field == "quest_stage" )
			questDialogueEditorSetIntMember(*action, field.c_str(), 1);
		else if ( field == "reward_gold" || field == "remove_gold" )
			questDialogueEditorSetIntMember(*action, field.c_str(), 100);
		else if ( field == "reward_item" || field == "remove_item" )
		{
			rapidjson::Value item(rapidjson::kObjectType);
			questDialogueEditorSetStringMember(item, "item",
				std::to_string(questDialogueEditorSelectedItemID));
			questDialogueEditorSetIntMember(item, "count",
				std::max(1, questDialogueEditorSelectedItemCount));
			action->AddMember(rapidjson::Value(field.c_str(), allocator), item, allocator);
		}
		else if ( field == "objective_complete" || field == "objective_clear" )
		{
			rapidjson::Value reference;
			const std::string id = questDialogueEditorDefaultObjectiveID();
			reference.SetString(id.c_str(), allocator);
			action->AddMember(rapidjson::Value(field.c_str(), allocator), reference, allocator);
		}
		else if ( field == "set_power" )
		{
			rapidjson::Value power(rapidjson::kObjectType);
			questDialogueEditorSetIntMember(power, "x", std::max(0, drawx));
			questDialogueEditorSetIntMember(power, "y", std::max(0, drawy));
			questDialogueEditorSetBoolMember(power, "powered", true);
			action->AddMember(rapidjson::Value(field.c_str(), allocator), power, allocator);
		}
		else if ( field == "status_effect" )
		{
			rapidjson::Value status(rapidjson::kObjectType);
			questDialogueEditorSetIntMember(status, "effect", questDialogueEditorSelectedEffectID);
			questDialogueEditorSetIntMember(status, "duration_seconds",
				questDialogueEditorEffectDurationSeconds);
			questDialogueEditorSetIntMember(status, "strength",
				questDialogueEditorEffectStrength);
			questDialogueEditorSetBoolMember(status, "enabled", true);
			action->AddMember(rapidjson::Value(field.c_str(), allocator), status, allocator);
		}
		else
		{
			rapidjson::Value object(rapidjson::kObjectType);
			const bool additive = field.rfind("add_", 0) == 0;
			if ( field.find("flag") != std::string::npos )
			{
				questDialogueEditorSetStringMember(object, "id",
					field.find("npc") != std::string::npos ? "npc_flag" : "story_flag");
				questDialogueEditorSetBoolMember(object, "value", true);
			}
			else
			{
				questDialogueEditorSetStringMember(object, "id",
					field.find("npc") != std::string::npos ? "npc_value"
					: (field.find("quest") != std::string::npos ? "quest_value" : "world_value"));
				questDialogueEditorSetIntMember(object, additive ? "amount" : "value", 1);
			}
			action->AddMember(rapidjson::Value(field.c_str(), allocator), object, allocator);
		}
	}
	const auto members = questDialogueEditorRuleActionMembers();
	const auto found = std::find(members.begin(), members.end(), field);
	if ( found != members.end() )
		questDialogueEditorSelectedActionIndex = static_cast<int>(found - members.begin());
	questDialogueEditorSetMessage("Action selected: " + field);
	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorRemoveSelectedRuleAction()
{
	rapidjson::Value* action = questDialogueEditorSelectedRuleAction();
	const std::string member = questDialogueEditorSelectedRuleActionMember();
	if ( !action || member.empty() ) return false;
	action->RemoveMember(member.c_str());
	questDialogueEditorSelectedActionIndex = std::max(0,
		questDialogueEditorSelectedActionIndex - 1);
	if ( questDialogueEditorRuleOwnerNode && action->MemberCount() == 1
		&& action->HasMember("id") )
	{
		rapidjson::Value* node = questDialogueEditorSelectedNodeValue();
		if ( node ) node->RemoveMember("action");
	}
	else if ( !questDialogueEditorRuleOwnerNode && action->ObjectEmpty() )
	{
		rapidjson::Value* choice = questDialogueEditorSelectedChoiceValue();
		if ( choice ) choice->RemoveMember("action");
	}
	questDialogueEditorSetMessage("Selected action removed.");
	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorToggleSelectedRuleActionBoolean()
{
	rapidjson::Value* action = questDialogueEditorSelectedRuleAction();
	const std::string member = questDialogueEditorSelectedRuleActionMember();
	if ( !action || member.empty() || !action->HasMember(member.c_str()) ) return false;
	rapidjson::Value& selected = (*action)[member.c_str()];
	if ( selected.IsBool() ) selected.SetBool(!selected.GetBool());
	else if ( selected.IsObject() )
	{
		const char* key = member == "set_power" ? "powered"
			: (member == "status_effect" ? "enabled"
				: (member.find("flag") != std::string::npos ? "value" : nullptr));
		if ( !key ) return false;
		const bool current = selected.HasMember(key) && selected[key].IsBool()
			? selected[key].GetBool() : false;
		questDialogueEditorSetBoolMember(selected, key, !current);
	}
	else return false;
	return questDialogueEditorSaveDocument();
}

static void questDialogueEditorCycleSelectedRuleAction(const int direction)
{
	const auto members = questDialogueEditorRuleActionMembers();
	if ( members.empty() )
	{
		questDialogueEditorSelectedActionIndex = 0;
		return;
	}
	questDialogueEditorSelectedActionIndex =
		(questDialogueEditorSelectedActionIndex + direction
			+ static_cast<int>(members.size())) % static_cast<int>(members.size());
}

static void questDialogueEditorApplyGuidedAction(
	const int slot
)
{
	rapidjson::Value* action =
		questDialogueEditorEnsureChoiceAction();

	if ( !action )
	{
		return;
	}

	const std::string objectiveID =
		questDialogueEditorDefaultObjectiveID();

	switch ( questDialogueEditorActionGroup )
	{
		case QUEST_DIALOGUE_ACTION_GROUP_QUEST:
			switch ( slot )
			{
				case 0:
					questDialogueEditorSetBoolMember(
						*action,
						"quest_start",
						true
					);
					break;
				case 1:
					questDialogueEditorSetBoolMember(
						*action,
						"quest_accept",
						true
					);
					break;
				case 2:
					questDialogueEditorSetBoolMember(
						*action,
						"quest_complete",
						true
					);
					break;
				case 3:
					questDialogueEditorSetBoolMember(
						*action,
						"quest_fail",
						true
					);
					break;
				case 4:
					questDialogueEditorSetBoolMember(
						*action,
						"quest_reset",
						true
					);
					break;
				default:
					questDialogueEditorSetIntMember(
						*action,
						"quest_stage",
						1
					);
					break;
			}
			break;

		case QUEST_DIALOGUE_ACTION_GROUP_REWARDS:
			if ( slot == 0 )
			{
				questDialogueEditorSetIntMember(
					*action,
					"reward_gold",
					questDialogueEditorGoldAmount
				);
			}
			else
			{
				rapidjson::Value& reward =
					questDialogueEditorSetObjectMember(
						*action,
						"reward_item"
					);

				questDialogueEditorSetStringMember(
					reward,
					"item",
					std::to_string(
						questDialogueEditorSelectedItemID
					)
				);
				questDialogueEditorSetIntMember(
					reward,
					"count",
					questDialogueEditorSelectedItemCount
				);
			}
			break;

		case QUEST_DIALOGUE_ACTION_GROUP_COSTS:
			if ( slot == 0 )
			{
				questDialogueEditorSetIntMember(
					*action,
					"remove_gold",
					questDialogueEditorGoldAmount
				);
			}
			else
			{
				rapidjson::Value& removal =
					questDialogueEditorSetObjectMember(
						*action,
						"remove_item"
					);

				questDialogueEditorSetStringMember(
					removal,
					"item",
					std::to_string(
						questDialogueEditorSelectedItemID
					)
				);
				questDialogueEditorSetIntMember(
					removal,
					"count",
					questDialogueEditorSelectedItemCount
				);
			}
			break;

		case QUEST_DIALOGUE_ACTION_GROUP_OBJECTIVES:
			questDialogueEditorSetStringMember(
				*action,
				slot == 0
					? "objective_complete"
					: "objective_clear",
				objectiveID
			);
			break;

		case QUEST_DIALOGUE_ACTION_GROUP_FLAGS:
		{
			const char* member =
				slot == 0
					? "set_world_flag"
					: "set_npc_flag";

			rapidjson::Value& flag =
				questDialogueEditorSetObjectMember(
					*action,
					member
				);

			questDialogueEditorSetStringMember(
				flag,
				"id",
				slot == 0
					? "story_flag"
					: "npc_flag"
			);
			questDialogueEditorSetBoolMember(
				flag,
				"value",
				true
			);
			break;
		}

		case QUEST_DIALOGUE_ACTION_GROUP_VARIABLES:
		{
			const char* member = nullptr;
			const char* identifier = nullptr;
			const char* numericMember = nullptr;

			switch ( slot )
			{
				case 0:
					member = "set_world_variable";
					identifier = "world_value";
					numericMember = "value";
					break;
				case 1:
					member = "add_world_variable";
					identifier = "world_value";
					numericMember = "amount";
					break;
				case 2:
					member = "set_npc_variable";
					identifier = "npc_value";
					numericMember = "value";
					break;
				case 3:
					member = "add_npc_variable";
					identifier = "npc_value";
					numericMember = "amount";
					break;
				case 4:
					member = "set_quest_variable";
					identifier = "quest_value";
					numericMember = "value";
					break;
				default:
					member = "add_quest_variable";
					identifier = "quest_value";
					numericMember = "amount";
					break;
			}

			rapidjson::Value& variable =
				questDialogueEditorSetObjectMember(
					*action,
					member
				);

			questDialogueEditorSetStringMember(
				variable,
				"id",
				identifier
			);
			questDialogueEditorSetIntMember(
				variable,
				numericMember,
				1
			);
			break;
		}

		case QUEST_DIALOGUE_ACTION_GROUP_NPC:
			if ( slot == 0 )
			{
				questDialogueEditorSetBoolMember(
					*action,
					"recruit_npc",
					true
				);
			}
			else
			{
				action->SetObject();
			}
			break;

		case QUEST_DIALOGUE_ACTION_GROUP_MECHANISMS:
		{
			rapidjson::Value& powerAction =
				questDialogueEditorSetObjectMember(
					*action,
					"set_power"
				);

			questDialogueEditorSetIntMember(
				powerAction,
				"x",
				0
			);
			questDialogueEditorSetIntMember(
				powerAction,
				"y",
				0
			);
			questDialogueEditorSetBoolMember(
				powerAction,
				"powered",
				slot == 0
			);
			break;
		}

		case QUEST_DIALOGUE_ACTION_GROUP_STATUS:
		{
			rapidjson::Value& status =
				questDialogueEditorSetObjectMember(
					*action,
					"status_effect"
				);

			questDialogueEditorSetIntMember(
				status,
				"effect",
				questDialogueEditorSelectedEffectID
			);
			questDialogueEditorSetIntMember(
				status,
				"duration_seconds",
				questDialogueEditorEffectDurationSeconds
			);
			questDialogueEditorSetIntMember(
				status,
				"strength",
				questDialogueEditorEffectStrength
			);
			questDialogueEditorSetBoolMember(
				status,
				"enabled",
				slot == 0
			);
			break;
		}

		default:
			break;
	}

	questDialogueEditorSaveDocument();

	questDialogueEditorSetMessage(
		std::string("Added ")
		+ questDialogueEditorChoiceActionName()
		+ " action."
	);
}


static void questDialogueEditorCycleSelectedItem(
	const int direction
)
{
	questDialogueEditorSelectedItemID += direction;

	if ( questDialogueEditorSelectedItemID < 0 )
	{
		questDialogueEditorSelectedItemID =
			NUMITEMS - 1;
	}
	else if ( questDialogueEditorSelectedItemID
		>= NUMITEMS )
	{
		questDialogueEditorSelectedItemID = 0;
	}

	questDialogueEditorSetMessage(
		"Item "
		+ std::to_string(
			questDialogueEditorSelectedItemID
		)
		+ ": "
		+ items[
			questDialogueEditorSelectedItemID
		].getIdentifiedName()
	);
}


struct QuestDialogueEditorQuestReference
{
	std::string filename;
	std::string questID;
	std::string title;
};

static std::vector<QuestDialogueEditorQuestReference>
questDialogueEditorAvailableQuestReferences()
{
	std::vector<QuestDialogueEditorQuestReference> references;

	for ( const std::string& filename :
		questDialogueEditorFiles )
	{
		const std::string path =
			"./dialogue/" + filename;

		std::ifstream input(
			path.c_str(),
			std::ios::in | std::ios::binary
		);

		if ( !input.is_open() )
		{
			continue;
		}

		const std::string jsonText(
			(std::istreambuf_iterator<char>(input)),
			std::istreambuf_iterator<char>()
		);

		rapidjson::Document document;
		document.Parse(jsonText.c_str());

		if ( document.HasParseError()
			|| !document.IsObject()
			|| !document.HasMember("quest_id")
			|| !document["quest_id"].IsString() )
		{
			continue;
		}

		QuestDialogueEditorQuestReference reference;
		reference.filename = filename;
		reference.questID =
			document["quest_id"].GetString();

		if ( document.HasMember("quest")
			&& document["quest"].IsObject()
			&& document["quest"].HasMember("title")
			&& document["quest"]["title"].IsString() )
		{
			reference.title =
				document["quest"]["title"].GetString();
		}

		if ( reference.title.empty() )
		{
			reference.title = reference.questID;
		}

		bool duplicate = false;

		for ( const QuestDialogueEditorQuestReference& existing :
			references )
		{
			if ( existing.questID == reference.questID )
			{
				duplicate = true;
				break;
			}
		}

		if ( !duplicate )
		{
			references.push_back(reference);
		}
	}

	std::sort(
		references.begin(),
		references.end(),
		[](
			const QuestDialogueEditorQuestReference& first,
			const QuestDialogueEditorQuestReference& second
		)
		{
			return first.title < second.title;
		}
	);

	return references;
}

static rapidjson::Value* questDialogueEditorSelectedChoiceCondition()
{
    return questDialogueEditorSelectedConditionValue();
}

static std::string questDialogueEditorSelectedConditionType()
{
	rapidjson::Value* condition =
		questDialogueEditorSelectedChoiceCondition();

	if ( !condition
		|| !condition->HasMember("type")
		|| !(*condition)["type"].IsString() )
	{
		return "none";
	}

	return questEditorNormalizeID((*condition)["type"].GetString());
}

static bool questDialogueEditorConditionUsesQuestReference()
{
	const std::string type =
		questDialogueEditorSelectedConditionType();

	return type == "quest_started"
		|| type == "quest_accepted"
		|| type == "quest_completed"
		|| type == "quest_failed"
		|| type == "quest_stage";
}

static bool questDialogueEditorWriteSelectedItemToCondition()
{
	rapidjson::Value* condition =
		questDialogueEditorSelectedChoiceCondition();

	if ( !condition
		|| questDialogueEditorSelectedConditionType()
			!= "has_item" )
	{
		questDialogueEditorSetMessage(
			"Select the Has Item condition first."
		);
		return false;
	}

	const std::string itemID =
		std::to_string(
			questDialogueEditorSelectedItemID
		);

	questDialogueEditorWriteStringMember(
		*condition,
		"item",
		itemID
	);

	questDialogueEditorSetIntMember(
		*condition,
		"count",
		questDialogueEditorSelectedItemCount
	);

	questDialogueEditorSetMessage(
		std::string("Required item: ")
		+ items[
			questDialogueEditorSelectedItemID
		].getIdentifiedName()
		+ " x"
		+ std::to_string(
			questDialogueEditorSelectedItemCount
		)
	);

	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorCycleConditionItem(
	const int direction
)
{
	questDialogueEditorCycleSelectedItem(direction);

	return questDialogueEditorWriteSelectedItemToCondition();
}

static bool questDialogueEditorAdjustConditionItemCount(
	const int direction
)
{
	questDialogueEditorSelectedItemCount =
		std::max(
			1,
			std::min(
				999,
				questDialogueEditorSelectedItemCount
					+ direction
			)
		);

	return questDialogueEditorWriteSelectedItemToCondition();
}

static bool questDialogueEditorCycleConditionQuest(
	const int direction
)
{
	rapidjson::Value* condition =
		questDialogueEditorSelectedChoiceCondition();

	if ( !condition
		|| !questDialogueEditorConditionUsesQuestReference() )
	{
		questDialogueEditorSetMessage(
			"Select a quest-state condition first."
		);
		return false;
	}

	const std::vector<QuestDialogueEditorQuestReference>
		references =
			questDialogueEditorAvailableQuestReferences();

	if ( references.empty() )
	{
		questDialogueEditorSetMessage(
			"No authored quest IDs were found in ./dialogue."
		);
		return false;
	}

	std::string currentQuest;

	if ( condition->HasMember("quest")
		&& (*condition)["quest"].IsString() )
	{
		currentQuest =
			(*condition)["quest"].GetString();
	}

	for ( int index = 0;
		index < static_cast<int>(references.size());
		++index )
	{
		if ( references[index].questID == currentQuest )
		{
			questDialogueEditorSelectedConditionQuest =
				index;
			break;
		}
	}

	questDialogueEditorSelectedConditionQuest +=
		direction;

	if ( questDialogueEditorSelectedConditionQuest < 0 )
	{
		questDialogueEditorSelectedConditionQuest =
			static_cast<int>(references.size()) - 1;
	}
	else if ( questDialogueEditorSelectedConditionQuest
		>= static_cast<int>(references.size()) )
	{
		questDialogueEditorSelectedConditionQuest = 0;
	}

	const QuestDialogueEditorQuestReference& selected =
		references[
			questDialogueEditorSelectedConditionQuest
		];

	questDialogueEditorWriteStringMember(
		*condition,
		"quest",
		selected.questID
	);

	questDialogueEditorSetMessage(
		"Required quest: "
		+ selected.title
		+ " ["
		+ selected.questID
		+ "]"
	);

	return questDialogueEditorSaveDocument();
}

static QuestDialogueEditorQuestReference
questDialogueEditorCurrentConditionQuestReference()
{
	QuestDialogueEditorQuestReference result;

	rapidjson::Value* condition =
		questDialogueEditorSelectedChoiceCondition();

	if ( !condition
		|| !questDialogueEditorConditionUsesQuestReference()
		|| !condition->HasMember("quest")
		|| !(*condition)["quest"].IsString() )
	{
		return result;
	}

	result.questID =
		(*condition)["quest"].GetString();
	result.title = result.questID;

	const std::vector<QuestDialogueEditorQuestReference>
		references =
			questDialogueEditorAvailableQuestReferences();

	for ( const QuestDialogueEditorQuestReference& reference :
		references )
	{
		if ( reference.questID == result.questID )
		{
			result = reference;
			break;
		}
	}

	return result;
}


static int questDialogueEditorConditionInteger(
	const char* member,
	const int fallback
)
{
	rapidjson::Value* condition =
		questDialogueEditorSelectedChoiceCondition();

	if ( !condition
		|| !condition->HasMember(member)
		|| !(*condition)[member].IsInt() )
	{
		return fallback;
	}

	return (*condition)[member].GetInt();
}

static std::string questDialogueEditorConditionString(
	const char* member,
	const std::string& fallback
)
{
	rapidjson::Value* condition =
		questDialogueEditorSelectedChoiceCondition();

	if ( !condition
		|| !condition->HasMember(member)
		|| !(*condition)[member].IsString() )
	{
		return fallback;
	}

	return (*condition)[member].GetString();
}

static bool questDialogueEditorAdjustConditionNumber(
	const int amount,
	const int minimum,
	const int maximum
)
{
	rapidjson::Value* condition =
		questDialogueEditorSelectedChoiceCondition();

	if ( !condition )
	{
		questDialogueEditorSetMessage(
			"Select a condition first."
		);
		return false;
	}

	const std::string type =
		questDialogueEditorSelectedConditionType();

	const char* member = nullptr;

	if ( type == "has_gold" )
	{
		member = "amount";
	}
	else if ( type == "quest_stage" )
	{
		member = "stage";
	}
	else if ( type == "world_variable"
		|| type == "npc_variable" )
	{
		member = "value";
	}

	if ( !member )
	{
		questDialogueEditorSetMessage(
			"This condition has no guided number."
		);
		return false;
	}

	const int current =
		questDialogueEditorConditionInteger(
			member,
			type == "has_gold" ? 100 : 1
		);

	const int next =
		std::max(
			minimum,
			std::min(
				maximum,
				current + amount
			)
		);

	questDialogueEditorWriteIntegerMember(
		*condition,
		member,
		next
	);

	questDialogueEditorSetMessage(
		std::string(member)
		+ ": "
		+ std::to_string(next)
	);

	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorCycleConditionObjective(
	const int direction
)
{
	rapidjson::Value* condition =
		questDialogueEditorSelectedChoiceCondition();

	if ( !condition )
	{
		questDialogueEditorSetMessage(
			"Select an objective condition first."
		);
		return false;
	}

	const std::string type =
		questDialogueEditorSelectedConditionType();

	if ( type != "objective_completed"
		&& type != "objective_incomplete" )
	{
		questDialogueEditorSetMessage(
			"Select Objective Complete or Objective Incomplete first."
		);
		return false;
	}

	rapidjson::Value* quest =
		questDialogueEditorQuestValue();

	if ( !quest
		|| !quest->HasMember("objectives")
		|| !(*quest)["objectives"].IsArray()
		|| (*quest)["objectives"].Empty() )
	{
		questDialogueEditorSetMessage(
			"This quest has no objectives to select."
		);
		return false;
	}

	rapidjson::Value& objectives =
		(*quest)["objectives"];

	std::string current =
		questDialogueEditorConditionString(
			"objective",
			""
		);

	int selected = 0;

	for ( rapidjson::SizeType index = 0;
		index < objectives.Size();
		++index )
	{
		if ( objectives[index].IsObject()
			&& objectives[index].HasMember("id")
			&& objectives[index]["id"].IsString()
			&& current
				== objectives[index]["id"].GetString() )
		{
			selected =
				static_cast<int>(index);
			break;
		}
	}

	selected += direction;

	if ( selected < 0 )
	{
		selected =
			static_cast<int>(
				objectives.Size()
			) - 1;
	}
	else if ( selected
		>= static_cast<int>(objectives.Size()) )
	{
		selected = 0;
	}

	rapidjson::Value& objective =
		objectives[
			static_cast<rapidjson::SizeType>(
				selected
			)
		];

	if ( !objective.IsObject()
		|| !objective.HasMember("id")
		|| !objective["id"].IsString() )
	{
		questDialogueEditorSetMessage(
			"Selected objective has no valid ID."
		);
		return false;
	}

	const std::string objectiveID =
		objective["id"].GetString();

	questDialogueEditorWriteStringMember(
		*condition,
		"objective",
		objectiveID
	);

	std::string title = objectiveID;

	if ( objective.HasMember("text")
		&& objective["text"].IsString() )
	{
		title = objective["text"].GetString();
	}

	questDialogueEditorSetMessage(
		"Required objective: "
		+ title
		+ " ["
		+ objectiveID
		+ "]"
	);

	return questDialogueEditorSaveDocument();
}

static std::string questDialogueEditorConditionObjectiveTitle()
{
	const std::string objectiveID =
		questDialogueEditorConditionString(
			"objective",
			""
		);

	if ( objectiveID.empty() )
	{
		return "";
	}

	rapidjson::Value* quest =
		questDialogueEditorQuestValue();

	if ( quest
		&& quest->HasMember("objectives")
		&& (*quest)["objectives"].IsArray() )
	{
		for ( rapidjson::Value& objective :
			(*quest)["objectives"].GetArray() )
		{
			if ( objective.IsObject()
				&& objective.HasMember("id")
				&& objective["id"].IsString()
				&& objectiveID
					== objective["id"].GetString() )
			{
				if ( objective.HasMember("text")
					&& objective["text"].IsString() )
				{
					return objective["text"].GetString();
				}
			}
		}
	}

	return objectiveID;
}

static bool questDialogueEditorToggleConditionFlagValue()
{
	rapidjson::Value* condition =
		questDialogueEditorSelectedChoiceCondition();

	if ( !condition )
	{
		questDialogueEditorSetMessage(
			"Select a flag condition first."
		);
		return false;
	}

	const std::string type =
		questDialogueEditorSelectedConditionType();

	if ( type != "world_flag"
		&& type != "npc_flag" )
	{
		questDialogueEditorSetMessage(
			"Select World Flag or NPC Flag first."
		);
		return false;
	}

	bool value = true;

	if ( condition->HasMember("value")
		&& (*condition)["value"].IsBool() )
	{
		value =
			(*condition)["value"].GetBool();
	}

	questDialogueEditorSetBoolMember(
		*condition,
		"value",
		!value
	);

	questDialogueEditorSetMessage(
		std::string("Required flag value: ")
		+ (!value ? "true" : "false")
	);

	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorOpenConditionReferenceEditor()
{
	rapidjson::Value* condition =
		questDialogueEditorSelectedChoiceCondition();

	if ( !condition )
	{
		questDialogueEditorSetMessage(
			"Select a condition first."
		);
		return false;
	}

	questDialogueEditorFieldCategory =
		QUEST_DIALOGUE_CATEGORY_CONDITION;

	questDialogueEditorEditableField =
		QUEST_DIALOGUE_FIELD_CONDITION_REFERENCE;

	questDialogueEditorBeginEditingField();

	questDialogueEditorSetMessage(
		"Type the condition name or ID, then press APPLY."
	);

	return true;
}

static bool questDialogueEditorOpenConditionNumberEditor()
{
	rapidjson::Value* condition =
		questDialogueEditorSelectedChoiceCondition();

	if ( !condition )
	{
		questDialogueEditorSetMessage(
			"Select a condition first."
		);
		return false;
	}

	questDialogueEditorFieldCategory =
		QUEST_DIALOGUE_CATEGORY_CONDITION;

	questDialogueEditorEditableField =
		QUEST_DIALOGUE_FIELD_CONDITION_NUMBER;

	questDialogueEditorBeginEditingField();

	questDialogueEditorSetMessage(
		"Type the exact condition number, then press APPLY."
	);

	return true;
}

static const char* questDialogueEditorEffectName(
	const int effectID
)
{
	static const char* names[] =
	{
		"Asleep",
		"Poisoned",
		"Stunned",
		"Confused",
		"Drunk",
		"Invisible",
		"Blind",
		"Greasy",
		"Messy",
		"Fast",
		"Paralyzed",
		"Levitating",
		"Telepathy",
		"Vomiting",
		"Bleeding",
		"Slow",
		"Magic Resistance",
		"Magic Reflection",
		"Vampiric Aura",
		"Red Shrine Buff",
		"Green Shrine Buff",
		"Blue Shrine Buff",
		"Health Regeneration",
		"Mana Regeneration",
		"Pacify",
		"Polymorph",
		"Knockback",
		"Withdrawal",
		"Strength Potion",
		"Shapeshift",
		"Webbed",
		"Fear",
		"Magic Amplification",
		"Disoriented",
		"Shadow Tagged",
		"Troll's Blood",
		"Flutter",
		"Dash",
		"Distracted Cooldown",
		"Mimic Locked",
		"Rooted",
		"Nausea Protection",
		"Constitution Bonus",
		"Power",
		"Agility",
		"Rally",
		"Marigold",
		"Ensemble Flute",
		"Ensemble Lyre",
		"Ensemble Drum",
		"Ensemble Lute",
		"Ensemble Horn",
		"Lift",
		"Guard Spirit",
		"Guard Body",
		"Divine Guard",
		"Nimbleness",
		"Greater Might",
		"Counsel",
		"Sturdiness",
		"Bless Food",
		"Pinpoint",
		"Penance",
		"Sacred Path",
		"Detect Enemy",
		"Blood Ward",
		"True Blood",
		"Divine Zeal",
		"Maximise",
		"Minimise",
		"Weakness",
		"Incoherence",
		"Overcharge",
		"Envenom Weapon",
		"Magic Grease",
		"Command",
		"Mimic Void",
		"Curse Flesh",
		"Numbing Bolt",
		"Delay Pain",
		"Seek Creature",
		"Taboo",
		"Courage",
		"Cowardice",
		"Spores",
		"Abundance",
		"Greater Abundance",
		"Preserve",
		"Mist Form",
		"Force Shield",
		"Lighten Load",
		"Attract Items",
		"Return Item",
		"Demesne Door",
		"Reflector Shield",
		"Dizzy",
		"Spin",
		"Critical Spell",
		"Magic Well",
		"Static",
		"Absorb Magic",
		"Flame Cloak",
		"Dusted",
		"Noise Visibility",
		"Spicy Ration",
		"Sour Ration",
		"Bitter Ration",
		"Hearty Ration",
		"Herbal Ration",
		"Sweet Ration",
		"Growth",
		"Thorns",
		"Blade Vines",
		"Bastion Mushroom",
		"Bastion Roots",
		"Foci: Peace",
		"Foci: Justice",
		"Foci: Providence",
		"Foci: Purity",
		"Foci: Sanctuary",
		"Stasis",
		"Health/Mana Regeneration",
		"Disrupted",
		"Frost",
		"Magician's Armor",
		"Project Spirit",
		"Defy Flesh",
		"Pinpoint Damage",
		"Salamander Heart",
		"Divine Fire",
		"Healing Word",
		"Holy Fire",
		"Sigil",
		"Sanctuary",
		"Ducked"
	};

	const int namedCount =
		static_cast<int>(
			sizeof(names) / sizeof(names[0])
		);

	if ( effectID >= 0
		&& effectID < namedCount )
	{
		return names[effectID];
	}

	if ( effectID >= namedCount
		&& effectID < NUMEFFECTS )
	{
		return "Reserved / unnamed";
	}

	return "Invalid effect";
}

static std::string questDialogueEditorConditionSummary(
	const rapidjson::Value& choice
)
{
	if ( !choice.IsObject()
		|| !choice.HasMember("condition")
		|| !choice["condition"].IsObject() )
	{
		return "None";
	}

	const rapidjson::Value& condition =
		choice["condition"];

	if ( !condition.HasMember("type")
		|| !condition["type"].IsString() )
	{
		return "Invalid condition";
	}

	const std::string type = questEditorNormalizeID(
		condition["type"].GetString());

	auto stringMember =
		[
			&condition
		](
			const char* member,
			const char* fallback
		) -> std::string
		{
			return condition.HasMember(member)
				&& condition[member].IsString()
					? condition[member].GetString()
					: fallback;
		};

	auto intMember =
		[
			&condition
		](
			const char* member,
			const int fallback
		) -> int
		{
			return condition.HasMember(member)
				&& condition[member].IsInt()
					? condition[member].GetInt()
					: fallback;
		};

	if ( type == "has_item" )
	{
		std::string item =
			stringMember("item", "?");
		std::string itemName = item;

		bool numeric = !item.empty();
		for ( const char character : item )
		{
			if ( character < '0'
				|| character > '9' )
			{
				numeric = false;
				break;
			}
		}

		if ( numeric )
		{
			const int itemID =
				std::atoi(item.c_str());

			if ( itemID >= 0
				&& itemID < NUMITEMS )
			{
				itemName =
					items[itemID]
						.getIdentifiedName();
			}
		}

		return "Requires "
			+ itemName
			+ " x"
			+ std::to_string(
				intMember("count", 1)
			);
	}

	if ( type == "has_gold" )
	{
		return "Requires "
			+ std::to_string(
				intMember("amount", 0)
			)
			+ " gold";
	}

	if ( type == "quest_started" )
	{
		return "Quest started: "
			+ stringMember("quest", "?");
	}

	if ( type == "quest_accepted" )
	{
		return "Quest accepted: "
			+ stringMember("quest", "?");
	}

	if ( type == "quest_completed" )
	{
		return "Quest completed: "
			+ stringMember("quest", "?");
	}

	if ( type == "quest_failed" )
	{
		return "Quest failed: "
			+ stringMember("quest", "?");
	}

	if ( type == "quest_stage" )
	{
		return "Quest "
			+ stringMember("quest", "?")
			+ " stage "
			+ std::to_string(
				intMember("stage", 0)
			);
	}

	if ( type == "objective_completed" )
	{
		return "Objective completed: "
			+ stringMember("objective", "?");
	}

	if ( type == "objective_incomplete" )
	{
		return "Objective incomplete: "
			+ stringMember("objective", "?");
	}

	if ( type == "world_flag" )
	{
		return "World flag: "
			+ stringMember("id", "?");
	}

	if ( type == "npc_flag" )
	{
		return "NPC flag: "
			+ stringMember("id", "?");
	}

	if ( type == "world_variable" )
	{
		return "World variable "
			+ stringMember("id", "?")
			+ " = "
			+ std::to_string(
				intMember("value", 0)
			);
	}

	if ( type == "npc_variable" )
	{
		return "NPC variable "
			+ stringMember("id", "?")
			+ " = "
			+ std::to_string(
				intMember("value", 0)
			);
	}

	return "Condition: " + type;
}

static std::string questDialogueEditorChoiceActionSummaryShort(
	const rapidjson::Value& choice
)
{
	if ( !choice.IsObject()
		|| !choice.HasMember("action")
		|| !choice["action"].IsObject() )
	{
		return "None";
	}

	const rapidjson::Value& action =
		choice["action"];

	if ( action.HasMember("reward_gold")
		&& action["reward_gold"].IsInt() )
	{
		return "Give "
			+ std::to_string(
				action["reward_gold"].GetInt()
			)
			+ " gold";
	}

	if ( action.HasMember("remove_gold")
		&& action["remove_gold"].IsInt() )
	{
		return "Take "
			+ std::to_string(
				action["remove_gold"].GetInt()
			)
			+ " gold";
	}

	if ( action.HasMember("reward_item")
		&& action["reward_item"].IsObject() )
	{
		return "Give item";
	}

	if ( action.HasMember("remove_item")
		&& action["remove_item"].IsObject() )
	{
		return "Take item";
	}

	if ( action.HasMember("status_effect")
		&& action["status_effect"].IsObject() )
	{
		const rapidjson::Value& status =
			action["status_effect"];

		const int effectID =
			status.HasMember("effect")
			&& status["effect"].IsInt()
				? status["effect"].GetInt()
				: -1;

		return std::string("Status: ")
			+ questDialogueEditorEffectName(
				effectID
			);
	}

	if ( action.HasMember("objective_complete")
		&& action["objective_complete"].IsString() )
	{
		return "Complete objective";
	}

	if ( action.HasMember("objective_clear")
		&& action["objective_clear"].IsString() )
	{
		return "Clear objective";
	}

	if ( action.HasMember("quest_start") )
	{
		return "Start quest";
	}

	if ( action.HasMember("quest_accept") )
	{
		return "Accept quest";
	}

	if ( action.HasMember("quest_complete") )
	{
		return "Complete quest";
	}

	if ( action.HasMember("quest_fail") )
	{
		return "Fail quest";
	}

	if ( action.HasMember("quest_reset") )
	{
		return "Reset quest";
	}

	if ( action.HasMember("set_power")
		&& action["set_power"].IsObject() )
	{
		const rapidjson::Value& powerAction =
			action["set_power"];
		const bool powered =
			powerAction.HasMember("powered")
			&& powerAction["powered"].IsBool()
				? powerAction["powered"].GetBool()
				: true;
		return powered ? "Power tile" : "Unpower tile";
	}

	if ( action.HasMember("recruit_npc") )
	{
		return "Recruit NPC";
	}

	return "Custom action";
}

static std::string questDialogueEditorChoiceObjectiveSummary(
	const rapidjson::Value& choice
)
{
	if ( !choice.IsObject()
		|| !choice.HasMember("action")
		|| !choice["action"].IsObject() )
	{
		return "";
	}

	const rapidjson::Value& action =
		choice["action"];

	if ( action.HasMember("objective_complete")
		&& action["objective_complete"].IsString() )
	{
		return "Completes: "
			+ std::string(
				action["objective_complete"].GetString()
			);
	}

	if ( action.HasMember("objective_clear")
		&& action["objective_clear"].IsString() )
	{
		return "Clears: "
			+ std::string(
				action["objective_clear"].GetString()
			);
	}

	return "";
}

static void questDialogueEditorCycleSelectedEffect(
	const int direction
)
{
	questDialogueEditorSelectedEffectID += direction;

	if ( questDialogueEditorSelectedEffectID < 0 )
	{
		questDialogueEditorSelectedEffectID =
			NUMEFFECTS - 1;
	}
	else if ( questDialogueEditorSelectedEffectID
		>= NUMEFFECTS )
	{
		questDialogueEditorSelectedEffectID = 0;
	}

	questDialogueEditorSetMessage(
		"Effect "
		+ std::to_string(
			questDialogueEditorSelectedEffectID
		)
		+ ": "
		+ questDialogueEditorEffectName(
			questDialogueEditorSelectedEffectID
		)
	);
}

static const char* questDialogueEditorGuidedActionLabel(
	const int slot
)
{
	switch ( questDialogueEditorActionGroup )
	{
		case QUEST_DIALOGUE_ACTION_GROUP_QUEST:
		{
			const char* labels[] =
			{
				"START QUEST",
				"ACCEPT QUEST",
				"COMPLETE",
				"FAIL QUEST",
				"RESET QUEST",
				"SET STAGE"
			};
			return labels[
				std::max(0, std::min(5, slot))
			];
		}

		case QUEST_DIALOGUE_ACTION_GROUP_REWARDS:
			return slot == 0
				? "GIVE GOLD"
				: "GIVE ITEM";

		case QUEST_DIALOGUE_ACTION_GROUP_COSTS:
			return slot == 0
				? "TAKE GOLD"
				: "TAKE ITEM";

		case QUEST_DIALOGUE_ACTION_GROUP_OBJECTIVES:
			return slot == 0
				? "FINISH OBJ"
				: "CLEAR OBJ";

		case QUEST_DIALOGUE_ACTION_GROUP_FLAGS:
			return slot == 0
				? "WORLD FLAG"
				: "NPC FLAG";

		case QUEST_DIALOGUE_ACTION_GROUP_VARIABLES:
		{
			const char* labels[] =
			{
				"SET WORLD",
				"ADD WORLD",
				"SET NPC",
				"ADD NPC",
				"SET QUEST",
				"ADD QUEST"
			};
			return labels[
				std::max(0, std::min(5, slot))
			];
		}

		case QUEST_DIALOGUE_ACTION_GROUP_NPC:
			return slot == 0
				? "RECRUIT NPC"
				: "EMPTY ACTION";

		case QUEST_DIALOGUE_ACTION_GROUP_MECHANISMS:
			return slot == 0
				? "POWER TILE"
				: "UNPOWER TILE";

		case QUEST_DIALOGUE_ACTION_GROUP_STATUS:
			return slot == 0
				? "APPLY STATUS"
				: "CLEAR STATUS";

		default:
			return "ACTION";
	}
}

static void questDialogueEditorEditChoiceTextDirect()
{
	if ( !questDialogueEditorSelectedChoiceValueForEdit() )
	{
		questDialogueEditorSetMessage(
			"Select a choice first."
		);
		return;
	}

	questDialogueEditorFieldCategory =
		QUEST_DIALOGUE_CATEGORY_TEXT;
	questDialogueEditorEditableField =
		QUEST_DIALOGUE_FIELD_CHOICE_TEXT;
	questDialogueEditorBeginEditingField();
}

static bool questDialogueEditorDuplicateSelectedFileNow()
{
	if ( questDialogueEditorSelectedFile < 0
		|| questDialogueEditorSelectedFile
			>= static_cast<int>(
				questDialogueEditorFiles.size()
			) )
	{
		questDialogueEditorSetMessage(
			"Select a file first."
		);
		return false;
	}

	const std::string sourceName =
		questDialogueEditorFiles[
			questDialogueEditorSelectedFile
		];

	const std::string baseName =
		sourceName.size() > 5
			? sourceName.substr(
				0,
				sourceName.size() - 5
			)
			: sourceName;

	int suffix = 1;
	std::string destinationName;

	do
	{
		destinationName =
			baseName
			+ "_copy_"
			+ std::to_string(suffix++)
			+ ".json";
	}
	while ( access(
		("./dialogue/" + destinationName).c_str(),
		F_OK
	) == 0 );

	automatia::dialogue::Document duplicate;
	std::string error;
	if ( !duplicate.loadFile("./dialogue/" + sourceName, error)
		|| !duplicate.saveAtomic("./dialogue/" + destinationName, error) )
	{
		questDialogueEditorSetMessage(error.empty()
			? "Could not duplicate the file." : error);
		return false;
	}

	questDialogueEditorRefreshFiles();

	for ( int index = 0;
		index < static_cast<int>(
			questDialogueEditorFiles.size()
		);
		++index )
	{
		if ( questDialogueEditorFiles[index]
			== destinationName )
		{
			questDialogueEditorSelectedFile = index;
			break;
		}
	}

	questDialogueEditorLoadPreview(destinationName);
	questDialogueEditorSetMessage(
		"Duplicated as " + destinationName
	);
	return true;
}

static bool questDialogueEditorDuplicateSelectedFile()
{
	if ( questDialogueEditorSelectedFile < 0
		|| questDialogueEditorSelectedFile
			>= static_cast<int>(questDialogueEditorFiles.size()) )
	{
		questDialogueEditorSetMessage("Select a file first.");
		return false;
	}
	questDialogueEditorRequestTransition(
		QUEST_DIALOGUE_PENDING_DUPLICATE_FILE,
		questDialogueEditorSelectedFile);
	return true;
}

static bool questDialogueEditorDeleteSelectedFileNow()
{
	if ( questDialogueEditorSelectedFile < 0
		|| questDialogueEditorSelectedFile
			>= static_cast<int>(
				questDialogueEditorFiles.size()
			) )
	{
		questDialogueEditorSetMessage(
			"Select a file first."
		);
		return false;
	}

	const std::string filename =
		questDialogueEditorFiles[
			questDialogueEditorSelectedFile
		];

	if ( std::remove(
			("./dialogue/" + filename).c_str()
		) != 0 )
	{
		questDialogueEditorSetMessage(
			"Could not delete the file."
		);
		return false;
	}

	questDialogueEditorRefreshFiles();

	if ( questDialogueEditorFiles.empty() )
	{
		questDialogueEditorSelectedFile = -1;
		questDialogueEditorModel.reset();
		questDialogueEditorPreview =
			QuestDialogueEditorPreview();
	}
	else
	{
		questDialogueEditorSelectedFile =
			std::min(
				questDialogueEditorSelectedFile,
				static_cast<int>(
					questDialogueEditorFiles.size()
				) - 1
			);

		questDialogueEditorLoadPreview(
			questDialogueEditorFiles[
				questDialogueEditorSelectedFile
			]
		);
	}

	questDialogueEditorSetMessage(
		"Deleted " + filename
	);
	return true;
}

static bool questDialogueEditorDeleteSelectedFile()
{
	if ( questDialogueEditorSelectedFile < 0
		|| questDialogueEditorSelectedFile
			>= static_cast<int>(questDialogueEditorFiles.size()) )
	{
		questDialogueEditorSetMessage("Select a file first.");
		return false;
	}
	questDialogueEditorRequestTransition(
		QUEST_DIALOGUE_PENDING_DELETE_FILE,
		questDialogueEditorSelectedFile
	);
	return true;
}

static bool questDialogueEditorValidateDocument(
	std::string& error
)
{
	questDialogueEditorRefreshValidation();
	for ( const auto& issue : questDialogueEditorValidationIssues )
	{
		if ( issue.severity == automatia::dialogue::Severity::Error )
		{
			error = issue.location.path + " - " + issue.message;
			return false;
		}
	}
	error.clear();
	return true;
#if 0
	if ( !questDialogueEditorDocument.IsObject() )
	{
		error = "Root must be a JSON object.";
		return false;
	}

	if ( !questDialogueEditorDocument.HasMember("version")
		|| !questDialogueEditorDocument["version"].IsInt()
		|| questDialogueEditorDocument["version"].GetInt()
			!= 1 )
	{
		error = "version must be integer 1.";
		return false;
	}

	if ( !questDialogueEditorDocument.HasMember("start_node")
		|| !questDialogueEditorDocument["start_node"].IsInt() )
	{
		error = "start_node must be an integer.";
		return false;
	}

	if ( !questDialogueEditorDocument.HasMember("nodes")
		|| !questDialogueEditorDocument["nodes"].IsArray()
		|| questDialogueEditorDocument["nodes"].Empty() )
	{
		error = "nodes must be a non-empty array.";
		return false;
	}

	std::unordered_set<int> nodeIDs;

	for ( const auto& node :
		questDialogueEditorDocument["nodes"].GetArray() )
	{
		if ( !node.IsObject()
			|| !node.HasMember("id")
			|| !node["id"].IsInt()
			|| !node.HasMember("text")
			|| !node["text"].IsString()
			|| std::string(
				node["text"].GetString()
			).empty() )
		{
			error =
				"Every node needs integer id and non-empty text.";
			return false;
		}

		if ( !nodeIDs.insert(
				node["id"].GetInt()
			).second )
		{
			error = "Duplicate node ID.";
			return false;
		}
	}

	if ( nodeIDs.find(
			questDialogueEditorDocument[
				"start_node"
			].GetInt()
		) == nodeIDs.end() )
	{
		error =
			"start_node points to a missing node.";
		return false;
	}

	std::unordered_set<std::string> choiceIDs;

	for ( const auto& node :
		questDialogueEditorDocument["nodes"].GetArray() )
	{
		if ( node.HasMember("choices") )
		{
			if ( !node["choices"].IsArray() )
			{
				error = "choices must be an array.";
				return false;
			}

			for ( const auto& choice :
				node["choices"].GetArray() )
			{
				if ( !choice.IsObject()
					|| !choice.HasMember("id")
					|| !choice["id"].IsString()
					|| !choice.HasMember("text")
					|| !choice["text"].IsString()
					|| std::string(
						choice["text"].GetString()
					).empty()
					|| !choice.HasMember("next")
					|| !choice["next"].IsInt() )
				{
					error =
						"Every choice needs unique id, non-empty text, and next.";
					return false;
				}

				if ( !choiceIDs.insert(
						choice["id"].GetString()
					).second )
				{
					error = "Duplicate choice ID.";
					return false;
				}

				if ( choice.HasMember("condition")
					&& !choice["condition"].IsObject() )
				{
					error = "Choice condition must be an object.";
					return false;
				}

				if ( choice.HasMember("action")
					&& !choice["action"].IsObject() )
				{
					error = "Choice action must be an object.";
					return false;
				}

				if ( nodeIDs.find(
						choice["next"].GetInt()
					) == nodeIDs.end() )
				{
					error =
						"A choice points to a missing node.";
					return false;
				}
			}
		}
	}

	if ( questDialogueEditorDocument.HasMember("quest")
		&& questDialogueEditorDocument["quest"].IsObject()
		&& questDialogueEditorDocument["quest"].HasMember("objectives") )
	{
		const rapidjson::Value& objectives =
			questDialogueEditorDocument["quest"]["objectives"];

		if ( !objectives.IsArray() )
		{
			error = "quest.objectives must be an array.";
			return false;
		}

		std::unordered_set<std::string> objectiveIDs;

		for ( const rapidjson::Value& objective :
			objectives.GetArray() )
		{
			if ( !objective.IsObject()
				|| !objective.HasMember("id")
				|| !objective["id"].IsString()
				|| std::string(
					objective["id"].GetString()
				).empty()
				|| !objective.HasMember("text")
				|| !objective["text"].IsString()
				|| std::string(
					objective["text"].GetString()
				).empty() )
			{
				error =
					"Every objective needs unique id and non-empty text.";
				return false;
			}

			if ( !objectiveIDs.insert(
					objective["id"].GetString()
				).second )
			{
				error = "Duplicate objective ID.";
				return false;
			}

			if ( objective.HasMember("map_marker")
				&& !objective["map_marker"].IsObject() )
			{
				error =
					"Objective map_marker must be an object.";
				return false;
			}
		}
	}

	error.clear();
	return true;
#endif
}



static std::vector<std::string>
questDialogueEditorAppliedActionSummary()
{
	std::vector<std::string> lines;

	rapidjson::Value* choice =
		questDialogueEditorSelectedChoiceValueForEdit();

	if ( !choice
		|| !choice->IsObject()
		|| !choice->HasMember("action")
		|| !(*choice)["action"].IsObject() )
	{
		lines.push_back("None");
		return lines;
	}

	const rapidjson::Value& action =
		(*choice)["action"];

	auto addBoolAction =
		[
			&action,
			&lines
		](
			const char* member,
			const char* label
		)
		{
			if ( action.HasMember(member)
				&& action[member].IsBool()
				&& action[member].GetBool() )
			{
				lines.push_back(label);
			}
		};

	addBoolAction("quest_start", "Start quest");
	addBoolAction("quest_accept", "Accept quest");
	addBoolAction("quest_complete", "Complete quest");
	addBoolAction("quest_fail", "Fail quest");
	addBoolAction("quest_reset", "Reset quest");
	addBoolAction("recruit_npc", "Recruit NPC");

	if ( action.HasMember("quest_stage")
		&& action["quest_stage"].IsInt() )
	{
		lines.push_back(
			"Set quest stage: "
			+ std::to_string(
				action["quest_stage"].GetInt()
			)
		);
	}

	if ( action.HasMember("reward_gold")
		&& action["reward_gold"].IsInt() )
	{
		lines.push_back(
			"Give gold: "
			+ std::to_string(
				action["reward_gold"].GetInt()
			)
		);
	}

	if ( action.HasMember("remove_gold")
		&& action["remove_gold"].IsInt() )
	{
		lines.push_back(
			"Take gold: "
			+ std::to_string(
				action["remove_gold"].GetInt()
			)
		);
	}

	auto addItemAction =
		[
			&action,
			&lines
		](
			const char* member,
			const char* label
		)
		{
			if ( !action.HasMember(member)
				|| !action[member].IsObject() )
			{
				return;
			}

			const rapidjson::Value& item =
				action[member];

			std::string itemText = "?";
			int count = 1;

			if ( item.HasMember("item")
				&& item["item"].IsString() )
			{
				itemText = item["item"].GetString();

				bool numeric = !itemText.empty();

				for ( const char character : itemText )
				{
					if ( character < '0'
						|| character > '9' )
					{
						numeric = false;
						break;
					}
				}

				if ( numeric )
				{
					const int itemID =
						std::atoi(
							itemText.c_str()
						);

					if ( itemID >= 0
						&& itemID < NUMITEMS )
					{
						itemText =
							items[itemID]
								.getIdentifiedName();
					}
				}
			}

			if ( item.HasMember("count")
				&& item["count"].IsInt() )
			{
				count = item["count"].GetInt();
			}

			lines.push_back(
				std::string(label)
				+ ": "
				+ itemText
				+ " x"
				+ std::to_string(count)
			);
		};

	addItemAction(
		"reward_item",
		"Give item"
	);

	addItemAction(
		"remove_item",
		"Take item"
	);

	auto addStringAction =
		[
			&action,
			&lines
		](
			const char* member,
			const char* label
		)
		{
			if ( action.HasMember(member)
				&& action[member].IsString() )
			{
				lines.push_back(
					std::string(label)
					+ ": "
					+ action[member].GetString()
				);
			}
		};

	addStringAction(
		"objective_complete",
		"Complete objective"
	);

	addStringAction(
		"objective_clear",
		"Clear objective"
	);

	auto addObjectIDAction =
		[
			&action,
			&lines
		](
			const char* member,
			const char* label
		)
		{
			if ( !action.HasMember(member)
				|| !action[member].IsObject() )
			{
				return;
			}

			const rapidjson::Value& object =
				action[member];

			std::string id = "?";

			if ( object.HasMember("id")
				&& object["id"].IsString() )
			{
				id = object["id"].GetString();
			}

			lines.push_back(
				std::string(label)
				+ ": "
				+ id
			);
		};

	addObjectIDAction(
		"set_world_flag",
		"Set world flag"
	);
	addObjectIDAction(
		"set_npc_flag",
		"Set NPC flag"
	);
	addObjectIDAction(
		"set_world_variable",
		"Set world variable"
	);
	addObjectIDAction(
		"add_world_variable",
		"Add world variable"
	);
	addObjectIDAction(
		"set_npc_variable",
		"Set NPC variable"
	);
	addObjectIDAction(
		"add_npc_variable",
		"Add NPC variable"
	);
	addObjectIDAction(
		"set_quest_variable",
		"Set quest variable"
	);
	addObjectIDAction(
		"add_quest_variable",
		"Add quest variable"
	);

	if ( action.HasMember("status_effect")
		&& action["status_effect"].IsObject() )
	{
		const rapidjson::Value& status =
			action["status_effect"];

		int effect = -1;
		int duration = 0;
		bool enabled = true;

		if ( status.HasMember("effect")
			&& status["effect"].IsInt() )
		{
			effect = status["effect"].GetInt();
		}

		if ( status.HasMember("duration_seconds")
			&& status["duration_seconds"].IsInt() )
		{
			duration =
				status["duration_seconds"].GetInt();
		}

		if ( status.HasMember("enabled")
			&& status["enabled"].IsBool() )
		{
			enabled =
				status["enabled"].GetBool();
		}

		lines.push_back(
			std::string(
				enabled
					? "Apply status"
					: "Clear status"
			)
			+ ": "
			+ std::to_string(effect)
			+ (
				enabled
					? " for "
						+ std::to_string(duration)
						+ "s"
					: ""
			)
		);
	}

	if ( lines.empty() )
	{
		lines.push_back(
			"Custom action fields"
		);
	}

	return lines;
}

static bool questDialogueEditorUseSelectedNPCAsQuestGiver()
{
	if ( !selectedEntity[0] )
	{
		questDialogueEditorSetMessage(
			"Select an NPC in the map first."
		);
		return false;
	}

	if ( selectedEntity[0]->persistentID <= 0 )
	{
		questDialogueEditorSetMessage(
			"Selected NPC has no persistent ID. Save the map, then select it again."
		);
		return false;
	}

	rapidjson::Value* quest =
		questDialogueEditorQuestValue();

	if ( !quest )
	{
		questDialogueEditorSetMessage(
			"This dialogue has no quest object."
		);
		return false;
	}

	auto& allocator =
		questDialogueEditorDocument.GetAllocator();

	if ( !quest->HasMember("origin") )
	{
		rapidjson::Value origin(
			rapidjson::kObjectType
		);

		quest->AddMember(
			"origin",
			origin,
			allocator
		);
	}
	else if ( !(*quest)["origin"].IsObject() )
	{
		(*quest)["origin"].SetObject();
	}

	rapidjson::Value& origin =
		(*quest)["origin"];

	const std::string mapName =
		questEditorCurrentMapFilename();

	if ( origin.HasMember("label") )
	{
		origin["label"].SetString(
			"Quest Giver",
			allocator
		);
	}
	else
	{
		rapidjson::Value label;
		label.SetString(
			"Quest Giver",
			allocator
		);

		origin.AddMember(
			"label",
			label,
			allocator
		);
	}

	if ( origin.HasMember("map") )
	{
		origin["map"].SetString(
			mapName.c_str(),
			allocator
		);
	}
	else
	{
		rapidjson::Value mapValue;
		mapValue.SetString(
			mapName.c_str(),
			allocator
		);

		origin.AddMember(
			"map",
			mapValue,
			allocator
		);
	}

	if ( origin.HasMember("track_npc") )
	{
		origin["track_npc"].SetBool(true);
	}
	else
	{
		origin.AddMember(
			"track_npc",
			true,
			allocator
		);
	}

	if ( origin.HasMember("npc_persistent_id") )
	{
		origin["npc_persistent_id"].SetInt(
			selectedEntity[0]->persistentID
		);
	}
	else
	{
		origin.AddMember(
			"npc_persistent_id",
			selectedEntity[0]->persistentID,
			allocator
		);
	}

	origin.RemoveMember("x");
	origin.RemoveMember("y");
	questDialogueEditorSetIntMember(origin, "playable_floor",
		std::max<int>(DEFAULT_PLAYABLE_FLOOR, selectedEntity[0]->playableFloor));
	if ( !origin.HasMember("floor_visibility") )
	{
		questDialogueEditorSetStringMember(origin,
			"floor_visibility", "same_floor");
	}

	if ( !questDialogueEditorSaveDocument() )
	{
		return false;
	}

	questDialogueEditorSetMessage(
		"Quest giver set to selected NPC, persistent ID "
		+ std::to_string(
			selectedEntity[0]->persistentID
		)
		+ "."
	);

	return true;
}

static bool questDialogueEditorClearQuestGiver()
{
	rapidjson::Value* quest =
		questDialogueEditorQuestValue();

	if ( !quest
		|| !quest->HasMember("origin")
		|| !(*quest)["origin"].IsObject() )
	{
		questDialogueEditorSetMessage(
			"No quest giver is currently assigned."
		);
		return false;
	}

	quest->RemoveMember("origin");

	if ( !questDialogueEditorSaveDocument() )
	{
		return false;
	}

	questDialogueEditorSetMessage(
		"Quest giver cleared."
	);

	return true;
}

static int questDialogueEditorQuestGiverPersistentID()
{
	rapidjson::Value* quest =
		questDialogueEditorQuestValue();

	if ( !quest
		|| !quest->HasMember("origin")
		|| !(*quest)["origin"].IsObject() )
	{
		return 0;
	}

	const rapidjson::Value& origin =
		(*quest)["origin"];

	if ( origin.HasMember("track_npc")
		&& origin["track_npc"].IsBool()
		&& origin["track_npc"].GetBool()
		&& origin.HasMember("npc_persistent_id")
		&& origin["npc_persistent_id"].IsInt() )
	{
		return origin["npc_persistent_id"].GetInt();
	}

	return 0;
}

static void questDialogueEditorOpenWizardNow()
{
	questDialogueEditorWizardOpen = true;
	questDialogueEditorWizardField = 0;
	questDialogueEditorWizardStep = 0;
	questDialogueEditorWizardUseQuest = questDialogueEditorWizardTemplate == 3;
	questDialogueEditorWizardRepeatable = false;
	questDialogueEditorWizardScope = 0;
	questDialogueEditorWizardOrigin = 0;
	std::snprintf(questDialogueEditorWizardDialogueID,
		sizeof(questDialogueEditorWizardDialogueID), "new_dialogue");
	questDialogueEditorWizardQuestID[0] = '\0';
	std::snprintf(questDialogueEditorWizardNPCText,
		sizeof(questDialogueEditorWizardNPCText), "Hello, traveler.");
	std::snprintf(questDialogueEditorWizardChoiceText,
		sizeof(questDialogueEditorWizardChoiceText), "Hello.");
	std::snprintf(questDialogueEditorWizardQuestTitle,
		sizeof(questDialogueEditorWizardQuestTitle), "New Quest");
	questDialogueEditorWizardQuestSummary[0] = '\0';
	inputstr = questDialogueEditorWizardDialogueID;
	inputlen = static_cast<int>(sizeof(questDialogueEditorWizardDialogueID) - 1);
	cursorflash = ticks;
	SDL_StartTextInput();
}

static void questDialogueEditorPerformTransition(
	const QuestDialoguePendingTransition transition,
	const int argument
)
{
	questDialogueEditorEndTransientTextInput();
	switch ( transition )
	{
		case QUEST_DIALOGUE_PENDING_CLOSE:
			questDialogueEditorJSONEditing = false;
			questDialogueEditorWizardOpen = false;
			SDL_StopTextInput();
			inputstr = nullptr;
			buttonCloseSubwindow(nullptr);
			break;
		case QUEST_DIALOGUE_PENDING_SWITCH_FILE:
			if ( argument >= 0
				&& argument < static_cast<int>(questDialogueEditorFiles.size()) )
			{
				questDialogueEditorSelectedFile = argument;
				questDialogueEditorLoadPreview(questDialogueEditorFiles[argument]);
			}
			break;
		case QUEST_DIALOGUE_PENDING_RELOAD:
			if ( questDialogueEditorSelectedFile >= 0
				&& questDialogueEditorSelectedFile
					< static_cast<int>(questDialogueEditorFiles.size()) )
			{
				questDialogueEditorLoadPreview(
					questDialogueEditorFiles[questDialogueEditorSelectedFile]);
				questDialogueEditorSetMessage("Reloaded selected file.");
			}
			break;
		case QUEST_DIALOGUE_PENDING_OPEN_WIZARD:
			questDialogueEditorOpenWizardNow();
			break;
		case QUEST_DIALOGUE_PENDING_DUPLICATE_FILE:
			questDialogueEditorDuplicateSelectedFileNow();
			break;
		case QUEST_DIALOGUE_PENDING_APPLY_TUTORIAL:
		{
			const auto& tutorials = automatia::dialogue::tutorialRecipes();
			if ( argument >= 0 && argument < static_cast<int>(tutorials.size()) )
			{
				std::string error;
				if ( questDialogueEditorModel.replaceWithEdit(
						tutorials[argument].exampleJson,
						"Apply tutorial: " + tutorials[argument].title,
						error) )
				{
					const std::string filename = questDialogueEditorSelectedFile >= 0
						&& questDialogueEditorSelectedFile
							< static_cast<int>(questDialogueEditorFiles.size())
						? questDialogueEditorFiles[questDialogueEditorSelectedFile]
						: std::string("tutorial.json");
					questDialogueEditorLoadPreview(filename, false);
					questDialogueEditorWorkspace = QUEST_DIALOGUE_WORKSPACE_CONVERSATION;
					questDialogueEditorSetMessage(
						"Tutorial applied in memory. Review, validate, then save.");
				}
				else questDialogueEditorSetMessage(error);
			}
			break;
		}
		case QUEST_DIALOGUE_PENDING_DELETE_FILE:
			questDialogueEditorDeletePrompt = true;
			break;
		default:
			break;
	}
}

static void questDialogueEditorRequestTransition(
	const QuestDialoguePendingTransition transition,
	const int argument
)
{
	if ( questDialogueEditorJSONEditing )
	{
		questDialogueEditorSetMessage(
			"Apply or cancel Advanced JSON before leaving this file.");
		return;
	}
	if ( questDialogueEditorEditingField )
	{
		questDialogueEditorSetMessage(
			"Apply the active field edit before leaving this file.");
		return;
	}
	if ( questDialogueEditorModel.dirty() )
	{
		questDialogueEditorPendingTransition = transition;
		questDialogueEditorPendingFile = argument;
		questDialogueEditorPendingTutorial = argument;
		questDialogueEditorUnsavedPrompt = true;
		return;
	}
	questDialogueEditorPerformTransition(transition, argument);
}

static void buttonQuestDialogueEditorClose(button_t*)
{
	questDialogueEditorRequestTransition(QUEST_DIALOGUE_PENDING_CLOSE);
}

static void questDialogueEditorUndo()
{
	if ( !questDialogueEditorModel.undo() )
	{
		questDialogueEditorSetMessage("Nothing to undo.");
		return;
	}
	const std::string filename = questDialogueEditorSelectedFile >= 0
		&& questDialogueEditorSelectedFile < static_cast<int>(questDialogueEditorFiles.size())
		? questDialogueEditorFiles[questDialogueEditorSelectedFile] : std::string{};
	questDialogueEditorLoadPreview(filename, false);
	questDialogueEditorSetMessage("Undo complete.");
}

static void questDialogueEditorRedo()
{
	if ( !questDialogueEditorModel.redo() )
	{
		questDialogueEditorSetMessage("Nothing to redo.");
		return;
	}
	const std::string filename = questDialogueEditorSelectedFile >= 0
		&& questDialogueEditorSelectedFile < static_cast<int>(questDialogueEditorFiles.size())
		? questDialogueEditorFiles[questDialogueEditorSelectedFile] : std::string{};
	questDialogueEditorLoadPreview(filename, false);
	questDialogueEditorSetMessage("Redo complete.");
}

void openQuestDialogueEditor()
{
	const bool preserveModel = questDialogueEditorPreserveModelOnOpen;
	questDialogueEditorPreserveModelOnOpen = false;
	questDialogueEditorEditingField = false;
	SDL_StopTextInput();

	/*
	 * This window can be opened directly from the monster-properties
	 * subwindow. Remove the focused buttons owned by that old subwindow
	 * before creating the dialogue editor's controls.
	 *
	 * Merely changing newwindow/subwindow is not enough because focused
	 * equipment, inventory, OK, and Cancel buttons remain in button_l.
	 */
	for ( node_t* node = button_l.first;
		node; )
	{
		node_t* nextNode = node->next;
		button_t* oldButton =
			static_cast<button_t*>(node->element);

		if ( oldButton && oldButton->focused )
		{
			list_RemoveNode(node);
		}

		node = nextNode;
	}

	menuVisible = 0;
	subwindow = 1;
	newwindow = 38;
	openwindow = 0;
	savewindow = 0;

	const int desiredHalfWidth =
		std::max(470, std::min(700, xres / 2 - 12));
	const int desiredHalfHeight =
		std::max(470, std::min(610, yres / 2 - 2));

	subx1 = std::max(
		8,
		xres / 2 - desiredHalfWidth
	);
	subx2 = std::min(
		xres - 8,
		xres / 2 + desiredHalfWidth
	);
	suby1 = std::max(
		24,
		yres / 2 - desiredHalfHeight
	);
	suby2 = std::min(
		yres - 8,
		yres / 2 + desiredHalfHeight
	);

	strcpy(subtext, "Dialogue and Quest Editor");

	questDialogueEditorRefreshFiles();

	if ( !questDialogueEditorFiles.empty() )
	{
		questDialogueEditorSelectedFile = std::max(0,
			std::min(questDialogueEditorSelectedFile,
				static_cast<int>(questDialogueEditorFiles.size()) - 1));
		questDialogueEditorLoadPreview(
			questDialogueEditorFiles[questDialogueEditorSelectedFile],
			!preserveModel
		);
	}
	else
	{
		questDialogueEditorSelectedFile = -1;
		questDialogueEditorModel.reset();
		questDialogueEditorPreview = QuestDialogueEditorPreview{};
		questDialogueEditorRefreshValidation();
	}

	button_t* closeButton = newButton();
	strcpy(closeButton->label, "Close");
	closeButton->x = subx2 - 64;
	closeButton->y = suby2 - 24;
	closeButton->sizex = 56;
	closeButton->sizey = 16;
	closeButton->action = &buttonQuestDialogueEditorClose;
	closeButton->visible = 1;
	closeButton->focused = 1;

	button_t* closeX = newButton();
	strcpy(closeX->label, "X");
	closeX->x = subx2 - 16;
	closeX->y = suby1;
	closeX->sizex = 16;
	closeX->sizey = 16;
	closeX->action = &buttonQuestDialogueEditorClose;
	closeX->visible = 1;
	closeX->focused = 1;
}


static std::string questDialogueEditorButtonTooltip(
	const std::string& label
)
{
	static const std::unordered_map<
		std::string,
		std::string
	> tooltips =
	{
		{ "NEW", "Create a new dialogue JSON with one starter node and an empty quest." },
		{ "SAVE", "Write the current dialogue and quest changes to the selected JSON file." },
		{ "RELOAD", "Discard the in-memory copy and reload the selected JSON from disk." },
		{ "RENAME", "Rename the selected dialogue file and update the selected NPC dialogue ID." },
		{ "+NODE", "Add a new dialogue node with a unique numeric node ID." },
		{ "-NODE", "Delete the selected node when nothing still links to it." },
		{ "DUPLICATE NODE", "Copy the selected node, assign a new node ID, and repair self-links to the copied node." },
		{ "+CHOICE", "Add a player response to the selected dialogue node." },
		{ "-CHOICE", "Delete the currently selected player response." },
		{ "DUPLICATE CHOICE", "Copy the selected choice, including its requirements and actions, with a unique ID." },
		{ "CHOICE UP", "Move the selected choice earlier in the displayed response order." },
		{ "CHOICE DOWN", "Move the selected choice later in the displayed response order." },
		{ "<BEFORE", "Change the selected choice destination to the previous dialogue node." },
		{ "NEXT>", "Change the selected choice destination to the next dialogue node." },
		{ "ONCE", "Allow the selected choice to be used only once by this player for this NPC." },
		{ "+OBJECT", "Add a new quest objective." },
		{ "-OBJECT", "Delete the selected quest objective." },
		{ "DUPLICATE OBJECTIVE", "Copy the selected objective with a unique objective ID." },
		{ "OBJECTIVE UP", "Move the selected objective earlier in the journal order." },
		{ "OBJECTIVE DOWN", "Move the selected objective later in the journal order." },
		{ "OPTIONAL", "Toggle whether the selected objective is optional." },
		{ "OBJ MARK", "Remove an existing marker, or temporarily return to the map to pick its tile and playable floor." },
		{ "CAT<", "Move to the previous editing category." },
		{ "CAT>", "Move to the next editing category." },
		{ "FIELD<", "Move to the previous editable field in the current category." },
		{ "FIELD>", "Move to the next editable field in the current category." },
		{ "EDIT", "Begin typing a new value for the selected field." },
		{ "APPLY", "Apply the typed field value and save it into the dialogue document." },
		{ "NEXT CONDITION", "Cycle condition templates, including checking whether another quest is completed." },
		{ "COND <", "Select the previous guided condition template." },
		{ "COND >", "Select the next guided condition template." },
		{ "CLEAR CONDITION", "Remove only the requirement from the selected choice." },
		{ "REQ ITEM <", "Choose the previous Barony item for the Has Item requirement." },
		{ "REQ ITEM >", "Choose the next Barony item for the Has Item requirement." },
		{ "REQ QTY -", "Decrease how many of the required item the player must possess." },
		{ "REQ QTY +", "Increase how many of the required item the player must possess." },
		{ "QUEST <", "Choose the previous authored quest for this quest-state requirement." },
		{ "QUEST >", "Choose the next authored quest for this quest-state requirement." },
		{ "STAGE -", "Decrease the required quest stage." },
		{ "STAGE +", "Increase the required quest stage." },
		{ "GOLD REQ -10", "Decrease the required gold amount by 10." },
		{ "GOLD REQ +10", "Increase the required gold amount by 10." },
		{ "TYPE EXACT AMOUNT", "Type an exact required gold amount in the guided condition field." },
		{ "OBJECTIVE <", "Choose the previous objective for this requirement." },
		{ "OBJECTIVE >", "Choose the next objective for this requirement." },
		{ "EDIT FLAG NAME", "Type the persistent flag name used by this condition." },
		{ "TOGGLE REQUIRED VALUE", "Require the flag to be true or false." },
		{ "EDIT VARIABLE NAME", "Type the persistent variable name used by this condition." },
		{ "VALUE -", "Decrease the required variable value." },
		{ "VALUE +", "Increase the required variable value." },
		{ "TYPE EXACT VALUE", "Type an exact required variable value." },
		{ "CLEAR ACTION", "Remove the entire action object from the selected choice." },
		{ "ACTION <", "Show the previous guided action group." },
		{ "ACTION >", "Show the next guided action group." },
		{ "COMPARE", "Cycle equals, not-equals, at-least, and at-most comparisons." },
		{ "SCOPE", "Cycle Personal, Party, and World ownership. Schema 1 keeps legacy Personal behavior; use the Quest page upgrade control to opt into schema 2 sharing." },
		{ "COND ITEM", "Cycle the item used by the selected item requirement." },
		{ "REWARD", "Cycle the selected reward-item preset." },
		{ "REMOVE ITEM", "Toggle or edit the selected choice item-removal action." },
		{ "REMOVE GOLD", "Toggle or edit the selected choice gold-removal action." },
		{ "RECRUIT", "Toggle the action that recruits the NPC." },
		{ "REPEAT", "Toggle whether the quest metadata marks the quest repeatable." },
		{ "GIVER MARKER", "Cycle marker modes: off, static at the selected entity tile, or follow a persistent NPC." },
		{ "PICK TILE ON MAP", "Temporarily close the dialogue window and click a map tile for a floor-aware marker." },
		{ "USE SELECTED NPC", "Bind the quest giver to the selected NPC's real persistent ID and current map." },
		{ "CLEAR GIVER", "Remove the dynamic quest-giver NPC binding." },
		{ "EDIT CHOICE", "Jump directly to editing the selected choice text." },
		{ "VALIDATE", "Check the current JSON structure and report the first exact problem." },
		{ "DUP FILE", "Create and select a uniquely named copy of the current dialogue file." },
		{ "DEL FILE", "Delete the selected dialogue file after confirmation." },
		{ "ITEM <", "Select the previous Barony item ID." },
		{ "ITEM >", "Select the next Barony item ID." },
		{ "QTY -", "Decrease the guided item quantity." },
		{ "QTY +", "Increase the guided item quantity." },
		{ "GOLD -10", "Decrease the guided gold amount by 10." },
		{ "GOLD +10", "Increase the guided gold amount by 10." },
		{ "EFFECT <", "Select the previous Barony status effect." },
		{ "EFFECT >", "Select the next Barony status effect." },
		{ "TIME -", "Reduce the selected status duration by five seconds." },
		{ "TIME +", "Increase the selected status duration by five seconds." },
		{ "POWER -", "Reduce the selected status strength." },
		{ "POWER +", "Increase the selected status strength." },
		{ "START QUEST", "Mark the current quest as started when this choice is used." },
		{ "ACCEPT QUEST", "Mark the current quest as accepted when this choice is used." },
		{ "COMPLETE", "Mark the current quest as completed when this choice is used." },
		{ "FAIL QUEST", "Mark the current quest as failed when this choice is used." },
		{ "RESET QUEST", "Reset the current quest when this choice is used." },
		{ "SET STAGE", "Set the current quest stage to the guided value." },
		{ "GIVE GOLD", "Give the player the displayed Gold amount." },
		{ "GIVE ITEM", "Give the player the displayed item and quantity." },
		{ "TAKE GOLD", "Remove the displayed Gold amount from the player." },
		{ "TAKE ITEM", "Remove the displayed item and quantity from the player." },
		{ "FINISH OBJ", "Mark the selected objective as completed." },
		{ "CLEAR OBJ", "Clear the selected objective's completed state." },
		{ "WORLD FLAG", "Create a persistent world flag action." },
		{ "NPC FLAG", "Create a persistent NPC-local flag action." },
		{ "SET WORLD", "Set a persistent world variable." },
		{ "ADD WORLD", "Add to a persistent world variable." },
		{ "SET NPC", "Set a persistent variable belonging to this NPC." },
		{ "ADD NPC", "Add to a persistent NPC variable." },
		{ "SET QUEST", "Set a persistent variable belonging to this quest." },
		{ "ADD QUEST", "Add to a persistent quest variable." },
		{ "RECRUIT NPC", "Recruit the dialogue NPC as a follower." },
		{ "POWER TILE", "Add a power action, then enter its tile X and Y coordinates in the Action Inspector." },
		{ "UNPOWER TILE", "Add an unpower action, then enter its tile X and Y coordinates in the Action Inspector." },
		{ "EMPTY ACTION", "Create an empty action object for advanced fields." },
		{ "APPLY STATUS", "Apply the displayed status effect, duration, and strength." },
		{ "CLEAR STATUS", "Remove the displayed status effect from the player." }
	};

	const auto found =
		tooltips.find(label);

	if ( found != tooltips.end() )
	{
		return found->second;
	}

	return "Use "
		+ label
		+ " to change the currently selected dialogue or quest element.";
}

static void drawQuestDialogueEditorWorkspace()
{
    if ( questDialogueEditorEditingField )
    {
        questDialogueEditorEditableField =
            questDialogueEditorLockedEditableField;
        questDialogueEditorFieldCategory =
            questDialogueEditorLockedFieldCategory;
		questDialogueEditorRuleOwnerNode =
			questDialogueEditorLockedRuleOwnerNode;
    }

	const int contentX1 = subx1 + 8;
	const int contentX2 = subx2 - 8;
	const int panelY1 = suby1 + 46;
	const int panelY2 = suby2 - 34;
	const int panelGap = 6;
	const int characterWidth = 8;

	int longestFilenameCharacters =
		static_cast<int>(
			strlen("Dialogue JSON Files")
		);

	for ( const std::string& filename :
		questDialogueEditorFiles )
	{
		longestFilenameCharacters =
			std::max(
				longestFilenameCharacters,
				static_cast<int>(filename.size())
			);
	}

	int longestDetailCharacters =
		static_cast<int>(
			strlen("Quest / Node Details")
		);

	const std::string detailWidthSamples[] =
	{
		"File: " + questDialogueEditorPreview.filename,
		"Quest ID: " + questDialogueEditorPreview.questID,
		"Title: " + questDialogueEditorPreview.title,
		"Scope: " + questDialogueEditorPreview.scope,
		"Giver marker: Follow NPC",
		"NPC persistent ID: "
			+ std::to_string(
				questDialogueEditorPreview.originNPCPersistentID
			)
	};

	for ( const std::string& sample :
		detailWidthSamples )
	{
		longestDetailCharacters =
			std::max(
				longestDetailCharacters,
				static_cast<int>(sample.size())
			);
	}

	const int minimumFileListWidth = 150;
	const int maximumFileListWidth = 260;
	const int minimumToolboxWidth = 236;
	const int minimumDetailWidth = 228;
	const int maximumDetailWidth = 340;
	const int minimumTreeWidth = 240;

	int fileListWidth =
		std::max(
			minimumFileListWidth,
			std::min(
				maximumFileListWidth,
				longestFilenameCharacters
					* characterWidth + 20
			)
		);

	const int toolboxWidth =
		minimumToolboxWidth;

	int detailWidth =
		std::max(
			minimumDetailWidth,
			std::min(
				maximumDetailWidth,
				longestDetailCharacters
					* characterWidth + 20
			)
		);

	const int availableWidth =
		contentX2 - contentX1;

	int requestedWidth =
		fileListWidth
		+ toolboxWidth
		+ detailWidth
		+ minimumTreeWidth
		+ panelGap * 3;

	if ( requestedWidth > availableWidth )
	{
		int overflow =
			requestedWidth - availableWidth;

		const int detailReduction =
			std::min(
				overflow,
				detailWidth - minimumDetailWidth
			);

		detailWidth -= detailReduction;
		overflow -= detailReduction;

		const int fileReduction =
			std::min(
				overflow,
				fileListWidth - minimumFileListWidth
			);

		fileListWidth -= fileReduction;
	}

	const int fileX1 = contentX1;
	const int fileX2 = fileX1 + fileListWidth;

	const int leftX1 = fileX2 + panelGap;
	const int leftX2 = leftX1 + toolboxWidth;

	const int detailX2 = contentX2;
	const int detailX1 = detailX2 - detailWidth;

	const int treeX1 = leftX2 + panelGap;
	const int treeX2 = detailX1 - panelGap;

	const int toolboxX1 = leftX1 + 6;
	const int toolboxX2 = leftX2 - 6;
	const int toolboxY1 = panelY1 + 22;
	const int toolboxButtonWidth =
		(toolboxX2 - toolboxX1 - 4) / 2;
	const int toolboxRowHeight = 19;

	const int fileListTitleY = panelY1 + 6;
	const int fileListY1 = panelY1 + 24;

	drawDepressed(leftX1, panelY1, leftX2, panelY2);
	drawDepressed(fileX1, panelY1, fileX2, panelY2);
	drawDepressed(treeX1, panelY1, treeX2, panelY2);
	drawDepressed(detailX1, panelY1, detailX2, panelY2);

	std::string hoveredDialogueEditorTooltip;

	enum QuestDialogueDeferredInspectorCommand
	{
		QUEST_DIALOGUE_DEFERRED_NONE = 0,
		QUEST_DIALOGUE_DEFERRED_CONDITION_PREVIOUS,
		QUEST_DIALOGUE_DEFERRED_CONDITION_NEXT,
		QUEST_DIALOGUE_DEFERRED_ACTION_PREVIOUS,
		QUEST_DIALOGUE_DEFERRED_ACTION_NEXT,
		QUEST_DIALOGUE_DEFERRED_REMOVE_CONDITION,
		QUEST_DIALOGUE_DEFERRED_REMOVE_ACTION
	};

	QuestDialogueDeferredInspectorCommand
		deferredInspectorCommand =
			QUEST_DIALOGUE_DEFERRED_NONE;

	auto dialogueEditorButton =
		[
			&hoveredDialogueEditorTooltip
		](
			const int x,
			const int y,
			const int width,
			const char* label
		) -> bool
		{
			const int height = 16;
			drawWindowFancy(
				x,
				y,
				x + width,
				y + height
			);

			printTextFormatted(
				font8x8_bmp,
				x + 4,
				y + 4,
				"%s",
				label
			);

			const bool hovered =
				omousex >= x
				&& omousex < x + width
				&& omousey >= y
				&& omousey < y + height;

			if ( hovered )
			{
				hoveredDialogueEditorTooltip =
					questDialogueEditorButtonTooltip(
						label
					);
			}

			if ( mousestatus[SDL_BUTTON_LEFT]
				&& hovered
				&& omousex < x + width
				&& omousey >= y
				&& omousey < y + height )
			{
				mousestatus[SDL_BUTTON_LEFT] = 0;
				return true;
			}

			return false;
		};

	auto dialogueEditorClippedText =
		[characterWidth](
			const std::string& value,
			const int availablePixels
		) -> std::string
		{
			const int maximumCharacters =
				std::max(
					0,
					availablePixels / characterWidth
				);

			if ( static_cast<int>(value.size())
				<= maximumCharacters )
			{
				return value;
			}

			if ( maximumCharacters <= 3 )
			{
				return value.substr(
					0,
					maximumCharacters
				);
			}

			return value.substr(
				0,
				maximumCharacters - 3
			) + "...";
		};

	auto dialogueEditorWrappedLines =
		[characterWidth](
			const std::string& value,
			const int availablePixels,
			const int maximumLines
		) -> std::vector<std::string>
		{
			std::vector<std::string> lines;

			const int maximumCharacters =
				std::max(
					1,
					availablePixels / characterWidth
				);

			std::string current;
			std::string word;

			auto flushWord =
				[
					&lines,
					&current,
					&word,
					maximumCharacters,
					maximumLines
				]()
				{
					if ( word.empty()
						|| static_cast<int>(lines.size())
							>= maximumLines )
					{
						word.clear();
						return;
					}

					while ( static_cast<int>(word.size())
						> maximumCharacters
						&& static_cast<int>(lines.size())
							< maximumLines )
					{
						if ( !current.empty() )
						{
							lines.push_back(current);
							current.clear();
						}

						lines.push_back(
							word.substr(
								0,
								maximumCharacters
							)
						);

						word.erase(
							0,
							maximumCharacters
						);
					}

					if ( static_cast<int>(lines.size())
						>= maximumLines )
					{
						word.clear();
						return;
					}

					if ( current.empty() )
					{
						current = word;
					}
					else if ( static_cast<int>(
						current.size()
						+ 1
						+ word.size()
					) <= maximumCharacters )
					{
						current += " " + word;
					}
					else
					{
						lines.push_back(current);
						current = word;
					}

					word.clear();
				};

			for ( const char character : value )
			{
				if ( character == '\n' )
				{
					flushWord();

					if ( !current.empty()
						&& static_cast<int>(lines.size())
							< maximumLines )
					{
						lines.push_back(current);
						current.clear();
					}

					continue;
				}

				if ( character == ' '
					|| character == '	' )
				{
					flushWord();
				}
				else
				{
					word.push_back(character);
				}
			}

			flushWord();

			if ( !current.empty()
				&& static_cast<int>(lines.size())
					< maximumLines )
			{
				lines.push_back(current);
			}

			if ( lines.empty() )
			{
				lines.push_back("");
			}

			return lines;
		};

	auto dialogueEditorDrawWrappedText =
		[
			&dialogueEditorWrappedLines
		](
			const int x,
			int& y,
			const int width,
			const std::string& value,
			const int maximumLines,
			const Uint32 color
		)
		{
			const std::vector<std::string> lines =
				dialogueEditorWrappedLines(
					value,
					width,
					maximumLines
				);

			for ( const std::string& line : lines )
			{
				printTextFormattedColor(
					font8x8_bmp,
					x,
					y,
					color,
					"%s",
					line.c_str()
				);

				y += 12;
			}
		};

	printText(
		font8x8_bmp,
		leftX1 + 6,
		panelY1 + 6,
		"Dialogue / Quest Toolbox"
	);

	auto toolboxButtonPair =
		[
			&dialogueEditorButton,
			toolboxX1,
			toolboxButtonWidth
		](
			const int y,
			const char* leftLabel,
			const char* rightLabel,
			const std::function<void()>& leftAction,
			const std::function<void()>& rightAction
		)
		{
			if ( dialogueEditorButton(
				toolboxX1,
				y,
				toolboxButtonWidth,
				leftLabel
			) )
			{
				leftAction();
			}

			if ( dialogueEditorButton(
				toolboxX1 + toolboxButtonWidth + 4,
				y,
				toolboxButtonWidth,
				rightLabel
			) )
			{
				rightAction();
			}
		};

	int toolboxY = toolboxY1;

	std::string selectionStatus = "Selected: nothing";
	if ( questDialogueEditorSelectedNode >= 0 )
	{
		selectionStatus =
			"Selected: node "
			+ std::to_string(questDialogueEditorSelectedNode + 1);

		if ( questDialogueEditorSelectedChoice >= 0 )
		{
			selectionStatus +=
				", choice "
				+ std::to_string(questDialogueEditorSelectedChoice + 1);
		}
	}

	printTextFormattedColor(
		font8x8_bmp,
		toolboxX1,
		toolboxY + 2,
		makeColorRGB(128, 255, 160),
		"%.31s",
		selectionStatus.c_str()
	);
	toolboxY += 14;

	printTextFormattedColor(
		font8x8_bmp,
		toolboxX1,
		toolboxY + 2,
		questDialogueEditorEditingField
			? makeColorRGB(255, 230, 96)
			: makeColorRGB(176, 176, 176),
		"Editing: %.21s",
		questDialogueEditorEditingField
			? questDialogueEditorEditableFieldName()
			: "click a field"
	);
	toolboxY += 17;

	toolboxButtonPair(
		toolboxY,
		"NEW",
		"SAVE",
		[]()
		{
			questDialogueEditorCreateNewFile();
		},
		[]()
		{
			questDialogueEditorWriteDocument();
		}
	);
	toolboxY += toolboxRowHeight;

	toolboxButtonPair(
		toolboxY,
		"RELOAD",
		"RENAME",
		[]()
		{
			questDialogueEditorRequestTransition(
				QUEST_DIALOGUE_PENDING_RELOAD,
				questDialogueEditorSelectedFile);
		},
		[]()
		{
			questDialogueEditorRenameSelectedFile();
		}
	);
	toolboxY += toolboxRowHeight + 3;

	toolboxButtonPair(
		toolboxY,
		"+NODE",
		"-NODE",
		[]()
		{
			questDialogueEditorAddNode();
		},
		[]()
		{
			questDialogueEditorDeleteNode();
		}
	);
	toolboxY += toolboxRowHeight;

	if ( dialogueEditorButton(
		toolboxX1,
		toolboxY,
		toolboxX2 - toolboxX1,
		"DUPLICATE NODE"
	) )
	{
		questDialogueEditorDuplicateSelectedNode();
	}
	toolboxY += toolboxRowHeight;

	toolboxButtonPair(
		toolboxY,
		"+CHOICE",
		"-CHOICE",
		[]()
		{
			questDialogueEditorAddChoice();
		},
		[]()
		{
			questDialogueEditorDeleteChoice();
		}
	);
	toolboxY += toolboxRowHeight;

	if ( dialogueEditorButton(
		toolboxX1,
		toolboxY,
		toolboxX2 - toolboxX1,
		"DUPLICATE CHOICE"
	) )
	{
		questDialogueEditorDuplicateSelectedChoice();
	}
	toolboxY += toolboxRowHeight;

	toolboxButtonPair(
		toolboxY,
		"CHOICE UP",
		"CHOICE DOWN",
		[]()
		{
			questDialogueEditorMoveSelectedChoice(-1);
		},
		[]()
		{
			questDialogueEditorMoveSelectedChoice(1);
		}
	);
	toolboxY += toolboxRowHeight;

	toolboxButtonPair(
		toolboxY,
		"<BEFORE",
		"NEXT>",
		[]()
		{
			questDialogueEditorCycleChoicePrevious();
		},
		[]()
		{
			questDialogueEditorCycleChoiceNext();
		}
	);
	toolboxY += toolboxRowHeight;

	if ( dialogueEditorButton(
		toolboxX1,
		toolboxY,
		toolboxX2 - toolboxX1,
		"ONCE"
	) )
	{
		questDialogueEditorToggleChoiceOnce();
	}
	toolboxY += toolboxRowHeight + 3;

	toolboxButtonPair(
		toolboxY,
		"+OBJECT",
		"-OBJECT",
		[]()
		{
			questDialogueEditorAddObjective();
		},
		[]()
		{
			questDialogueEditorDeleteObjective();
		}
	);
	toolboxY += toolboxRowHeight;

	if ( dialogueEditorButton(
		toolboxX1,
		toolboxY,
		toolboxX2 - toolboxX1,
		"DUPLICATE OBJECTIVE"
	) )
	{
		questDialogueEditorDuplicateSelectedObjective();
	}
	toolboxY += toolboxRowHeight;

	toolboxButtonPair(
		toolboxY,
		"OBJECTIVE UP",
		"OBJECTIVE DOWN",
		[]()
		{
			questDialogueEditorMoveSelectedObjective(-1);
		},
		[]()
		{
			questDialogueEditorMoveSelectedObjective(1);
		}
	);
	toolboxY += toolboxRowHeight;

	toolboxButtonPair(
		toolboxY,
		"OPTIONAL",
		"OBJ MARK",
		[]()
		{
			questDialogueEditorToggleObjectiveOptional();
		},
		[]()
		{
			questDialogueEditorToggleObjectiveMarker();
		}
	);
	toolboxY += toolboxRowHeight + 3;

	toolboxButtonPair(
		toolboxY,
		"CAT<",
		"CAT>",
		[]()
		{
			questDialogueEditorCycleFieldCategory(-1);
		},
		[]()
		{
			questDialogueEditorCycleFieldCategory(1);
		}
	);
	toolboxY += toolboxRowHeight;

	toolboxButtonPair(
		toolboxY,
		"FIELD<",
		"FIELD>",
		[]()
		{
			questDialogueEditorCycleEditableFieldDirection(-1);
		},
		[]()
		{
			questDialogueEditorCycleEditableFieldDirection(1);
		}
	);
	toolboxY += toolboxRowHeight;

	toolboxButtonPair(
		toolboxY,
		"EDIT",
		"APPLY",
		[]()
		{
			questDialogueEditorBeginEditingField();
		},
		[]()
		{
			questDialogueEditorApplyEditableField();
		}
	);
	toolboxY += toolboxRowHeight + 3;

	toolboxButtonPair(
		toolboxY,
		"CONDITION <",
		"CONDITION >",
		[]()
		{
			int group =
				static_cast<int>(questDialogueEditorConditionGroup) - 1;
			if ( group < 0 )
			{
				group = QUEST_DIALOGUE_CONDITION_GROUP_COUNT - 1;
			}
			questDialogueEditorConditionGroup =
				static_cast<QuestDialogueConditionGroup>(group);
		},
		[]()
		{
			int group =
				static_cast<int>(questDialogueEditorConditionGroup) + 1;
			if ( group >= QUEST_DIALOGUE_CONDITION_GROUP_COUNT )
			{
				group = 0;
			}
			questDialogueEditorConditionGroup =
				static_cast<QuestDialogueConditionGroup>(group);
		}
	);
	toolboxY += toolboxRowHeight;

	const char* conditionGroupNames[] =
	{
		"Items / Gold",
		"Quest State",
		"Objectives",
		"Flags",
		"Variables"
	};

	const char* conditionGroupTypes[][2] =
	{
		{ "has_item", "has_gold" },
		{ "quest_accepted", "quest_stage" },
		{ "objective_completed", "objective_incomplete" },
		{ "world_flag", "npc_flag" },
		{ "world_variable", "npc_variable" }
	};

	const char* conditionGroupLabels[][2] =
	{
		{ "REQUIRE ITEM", "REQUIRE GOLD" },
		{ "QUEST ACCEPTED", "QUEST STAGE" },
		{ "OBJ COMPLETE", "OBJ INCOMPLETE" },
		{ "WORLD FLAG", "NPC FLAG" },
		{ "WORLD VARIABLE", "NPC VARIABLE" }
	};

	const int conditionGroupIndex =
		static_cast<int>(questDialogueEditorConditionGroup);

	printTextFormattedColor(
		font8x8_bmp,
		toolboxX1,
		toolboxY + 4,
		makeColorRGB(128, 255, 160),
		"Condition group: %s",
		conditionGroupNames[conditionGroupIndex]
	);
	toolboxY += toolboxRowHeight;

	toolboxButtonPair(
		toolboxY,
		conditionGroupLabels[conditionGroupIndex][0],
		conditionGroupLabels[conditionGroupIndex][1],
		[conditionGroupIndex, &conditionGroupTypes]()
		{
			questDialogueEditorSelectChoiceConditionType(
				conditionGroupTypes[conditionGroupIndex][0]
			);
		},
		[conditionGroupIndex, &conditionGroupTypes]()
		{
			questDialogueEditorSelectChoiceConditionType(
				conditionGroupTypes[conditionGroupIndex][1]
			);
		}
	);
	toolboxY += toolboxRowHeight;

    toolboxButtonPair(
        toolboxY,
        "REQ <",
        "REQ >",
        []()
        {
            questDialogueEditorCycleSelectedCondition(-1);
        },
        []()
        {
            questDialogueEditorCycleSelectedCondition(1);
        }
    );
    toolboxY += toolboxRowHeight;

	toolboxButtonPair(
		toolboxY,
		"REMOVE REQ",
		"CLEAR ACTIONS",
		[]()
		{
			questDialogueEditorClearChoiceCondition();
		},
		[]()
		{
			questDialogueEditorClearChoiceAction();
		}
	);
	toolboxY += toolboxRowHeight;

	printTextFormattedColor(
		font8x8_bmp,
		toolboxX1,
		toolboxY + 4,
		makeColorRGB(128, 255, 160),
		"Requires: %.20s",
		questDialogueEditorChoiceConditionName().c_str()
	);
	toolboxY += toolboxRowHeight;

	const std::string selectedConditionType =
		questDialogueEditorSelectedConditionType();

	const bool conditionHasReference =
		selectedConditionType == "has_item"
		|| selectedConditionType == "quest_started"
		|| selectedConditionType == "quest_accepted"
		|| selectedConditionType == "quest_completed"
		|| selectedConditionType == "quest_failed"
		|| selectedConditionType == "quest_stage"
		|| selectedConditionType == "objective_completed"
		|| selectedConditionType == "objective_incomplete"
		|| selectedConditionType == "world_flag"
		|| selectedConditionType == "npc_flag"
		|| selectedConditionType == "world_variable"
		|| selectedConditionType == "npc_variable";

	const bool conditionHasNumber =
		selectedConditionType == "has_item"
		|| selectedConditionType == "has_gold"
		|| selectedConditionType == "quest_stage"
		|| selectedConditionType == "world_variable"
		|| selectedConditionType == "npc_variable";

	if ( conditionHasReference || conditionHasNumber )
	{
		printTextFormattedColor(
			font8x8_bmp,
			toolboxX1,
			toolboxY + 3,
			makeColorRGB(128, 192, 255),
			"Condition parameters (click to type)"
		);
		toolboxY += toolboxRowHeight;

		if ( conditionHasReference )
		{
			questDialogueEditorFieldCategory =
				QUEST_DIALOGUE_CATEGORY_CONDITION;
			questDialogueEditorEditableField =
				QUEST_DIALOGUE_FIELD_CONDITION_REFERENCE;

			const std::string referenceValue =
				questDialogueEditorReadEditableField();

			if ( dialogueEditorButton(
				toolboxX1,
				toolboxY,
				toolboxX2 - toolboxX1,
				("REFERENCE: "
					+ dialogueEditorClippedText(
						referenceValue.empty()
							? "(click to set)"
							: referenceValue,
						toolboxX2 - toolboxX1 - 16
					)
				).c_str()
			) )
			{
				questDialogueEditorOpenConditionReferenceEditor();
			}
			toolboxY += toolboxRowHeight;
		}

		if ( conditionHasNumber )
		{
			questDialogueEditorFieldCategory =
				QUEST_DIALOGUE_CATEGORY_CONDITION;
			questDialogueEditorEditableField =
				QUEST_DIALOGUE_FIELD_CONDITION_NUMBER;

			const std::string numberValue =
				questDialogueEditorReadEditableField();

			if ( dialogueEditorButton(
				toolboxX1,
				toolboxY,
				toolboxX2 - toolboxX1,
				("VALUE / ID: "
					+ (numberValue.empty()
						? "(click to set)"
						: numberValue)
				).c_str()
			) )
			{
				questDialogueEditorOpenConditionNumberEditor();
			}
			toolboxY += toolboxRowHeight;
		}
	}

	if ( selectedConditionType == "has_item" )
	{
		toolboxButtonPair(
			toolboxY,
			"REQ ITEM <",
			"REQ ITEM >",
			[]()
			{
				questDialogueEditorCycleConditionItem(-1);
			},
			[]()
			{
				questDialogueEditorCycleConditionItem(1);
			}
		);
		toolboxY += toolboxRowHeight;

		printTextFormatted(
			font8x8_bmp,
			toolboxX1,
			toolboxY + 4,
			"Required item ID: %d",
			questDialogueEditorSelectedItemID
		);
		toolboxY += toolboxRowHeight;

		printTextFormattedColor(
			font8x8_bmp,
			toolboxX1,
			toolboxY + 4,
			makeColorRGB(128, 255, 160),
			"Item: %.24s",
			items[
				questDialogueEditorSelectedItemID
			].getIdentifiedName()
		);
		toolboxY += toolboxRowHeight;

		toolboxButtonPair(
			toolboxY,
			"REQ QTY -",
			"REQ QTY +",
			[]()
			{
				questDialogueEditorAdjustConditionItemCount(-1);
			},
			[]()
			{
				questDialogueEditorAdjustConditionItemCount(1);
			}
		);
		toolboxY += toolboxRowHeight;

		printTextFormatted(
			font8x8_bmp,
			toolboxX1,
			toolboxY + 4,
			"Required quantity: %d",
			questDialogueEditorSelectedItemCount
		);
		toolboxY += toolboxRowHeight;
	}
	else if ( questDialogueEditorConditionUsesQuestReference() )
	{
		toolboxButtonPair(
			toolboxY,
			"QUEST <",
			"QUEST >",
			[]()
			{
				questDialogueEditorCycleConditionQuest(-1);
			},
			[]()
			{
				questDialogueEditorCycleConditionQuest(1);
			}
		);
		toolboxY += toolboxRowHeight;

		const QuestDialogueEditorQuestReference
			requiredQuest =
				questDialogueEditorCurrentConditionQuestReference();

		printTextFormattedColor(
			font8x8_bmp,
			toolboxX1,
			toolboxY + 4,
			makeColorRGB(128, 255, 160),
			"Quest ID: %.22s",
			requiredQuest.questID.empty()
				? "(choose a quest)"
				: requiredQuest.questID.c_str()
		);
		toolboxY += toolboxRowHeight;

		printTextFormatted(
			font8x8_bmp,
			toolboxX1,
			toolboxY + 4,
			"Title: %.25s",
			requiredQuest.title.empty()
				? "(not found)"
				: requiredQuest.title.c_str()
		);
		toolboxY += toolboxRowHeight;

		if ( selectedConditionType == "quest_stage" )
		{
			toolboxButtonPair(
				toolboxY,
				"STAGE -",
				"STAGE +",
				[]()
				{
					questDialogueEditorAdjustConditionNumber(
						-1,
						0,
						999
					);
				},
				[]()
				{
					questDialogueEditorAdjustConditionNumber(
						1,
						0,
						999
					);
				}
			);
			toolboxY += toolboxRowHeight;

			printTextFormatted(
				font8x8_bmp,
				toolboxX1,
				toolboxY + 4,
				"Required stage: %d",
				questDialogueEditorConditionInteger(
					"stage",
					1
				)
			);
			toolboxY += toolboxRowHeight;
		}
	}
	else if ( selectedConditionType == "has_gold" )
	{
		toolboxButtonPair(
			toolboxY,
			"GOLD REQ -10",
			"GOLD REQ +10",
			[]()
			{
				questDialogueEditorAdjustConditionNumber(
					-10,
					0,
					999999
				);
			},
			[]()
			{
				questDialogueEditorAdjustConditionNumber(
					10,
					0,
					999999
				);
			}
		);
		toolboxY += toolboxRowHeight;

		printTextFormattedColor(
			font8x8_bmp,
			toolboxX1,
			toolboxY + 4,
			makeColorRGB(128, 255, 160),
			"Required gold: %d",
			questDialogueEditorConditionInteger(
				"amount",
				100
			)
		);
		toolboxY += toolboxRowHeight;

		if ( dialogueEditorButton(
			toolboxX1,
			toolboxY,
			toolboxX2 - toolboxX1,
			"TYPE EXACT AMOUNT"
		) )
		{
			questDialogueEditorOpenConditionNumberEditor();
		}
		toolboxY += toolboxRowHeight;
	}
	else if ( selectedConditionType == "objective_completed"
		|| selectedConditionType == "objective_incomplete" )
	{
		toolboxButtonPair(
			toolboxY,
			"OBJECTIVE <",
			"OBJECTIVE >",
			[]()
			{
				questDialogueEditorCycleConditionObjective(-1);
			},
			[]()
			{
				questDialogueEditorCycleConditionObjective(1);
			}
		);
		toolboxY += toolboxRowHeight;

		printTextFormattedColor(
			font8x8_bmp,
			toolboxX1,
			toolboxY + 4,
			makeColorRGB(128, 192, 255),
			"Objective ID: %.18s",
			questDialogueEditorConditionString(
				"objective",
				"(choose objective)"
			).c_str()
		);
		toolboxY += toolboxRowHeight;

		const std::string objectiveTitle =
			questDialogueEditorConditionObjectiveTitle();

		printTextFormatted(
			font8x8_bmp,
			toolboxX1,
			toolboxY + 4,
			"Objective: %.23s",
			objectiveTitle.empty()
				? "(not found)"
				: objectiveTitle.c_str()
		);
		toolboxY += toolboxRowHeight;
	}
	else if ( selectedConditionType == "world_flag"
		|| selectedConditionType == "npc_flag" )
	{
		if ( dialogueEditorButton(
			toolboxX1,
			toolboxY,
			toolboxX2 - toolboxX1,
			"EDIT FLAG NAME"
		) )
		{
			questDialogueEditorOpenConditionReferenceEditor();
		}
		toolboxY += toolboxRowHeight;

		printTextFormattedColor(
			font8x8_bmp,
			toolboxX1,
			toolboxY + 4,
			makeColorRGB(128, 255, 160),
			"Flag: %.24s",
			questDialogueEditorConditionString(
				"id",
				"(type a flag name)"
			).c_str()
		);
		toolboxY += toolboxRowHeight;

		if ( dialogueEditorButton(
			toolboxX1,
			toolboxY,
			toolboxX2 - toolboxX1,
			"TOGGLE REQUIRED VALUE"
		) )
		{
			questDialogueEditorToggleConditionFlagValue();
		}
		toolboxY += toolboxRowHeight;

		const rapidjson::Value* flagCondition =
			questDialogueEditorSelectedChoiceCondition();

		const bool requiredFlag =
			flagCondition
			&& flagCondition->HasMember("value")
			&& (*flagCondition)["value"].IsBool()
				? (*flagCondition)["value"].GetBool()
				: true;

		printTextFormatted(
			font8x8_bmp,
			toolboxX1,
			toolboxY + 4,
			"Required value: %s",
			requiredFlag ? "true" : "false"
		);
		toolboxY += toolboxRowHeight;
	}
	else if ( selectedConditionType == "world_variable"
		|| selectedConditionType == "npc_variable" )
	{
		if ( dialogueEditorButton(
			toolboxX1,
			toolboxY,
			toolboxX2 - toolboxX1,
			"EDIT VARIABLE NAME"
		) )
		{
			questDialogueEditorOpenConditionReferenceEditor();
		}
		toolboxY += toolboxRowHeight;

		printTextFormattedColor(
			font8x8_bmp,
			toolboxX1,
			toolboxY + 4,
			makeColorRGB(128, 255, 160),
			"Variable: %.20s",
			questDialogueEditorConditionString(
				"id",
				"(type a variable name)"
			).c_str()
		);
		toolboxY += toolboxRowHeight;

		toolboxButtonPair(
			toolboxY,
			"VALUE -",
			"VALUE +",
			[]()
			{
				questDialogueEditorAdjustConditionNumber(
					-1,
					-999999,
					999999
				);
			},
			[]()
			{
				questDialogueEditorAdjustConditionNumber(
					1,
					-999999,
					999999
				);
			}
		);
		toolboxY += toolboxRowHeight;

		printTextFormatted(
			font8x8_bmp,
			toolboxX1,
			toolboxY + 4,
			"Required value: %d",
			questDialogueEditorConditionInteger(
				"value",
				1
			)
		);
		toolboxY += toolboxRowHeight;

		toolboxButtonPair(
			toolboxY,
			"COMPARE",
			"TYPE EXACT VALUE",
			[]()
			{
				questDialogueEditorCycleComparisonDirect();
			},
			[]()
			{
				questDialogueEditorOpenConditionNumberEditor();
			}
		);
		toolboxY += toolboxRowHeight;

		printTextFormattedColor(
			font8x8_bmp,
			toolboxX1,
			toolboxY + 4,
			makeColorRGB(128, 255, 160),
			"Compare: %.18s",
			questDialogueEditorConditionString(
				"comparison",
				"equals"
			).c_str()
		);
		toolboxY += toolboxRowHeight;
	}

	toolboxButtonPair(
		toolboxY,
		"ACTION <",
		"ACTION >",
		[]()
		{
			questDialogueEditorCycleActionGroup(-1);
		},
		[]()
		{
			questDialogueEditorCycleActionGroup(1);
		}
	);
	toolboxY += toolboxRowHeight;

	printTextFormattedColor(
		font8x8_bmp,
		toolboxX1,
		toolboxY + 4,
		makeColorRGB(128, 255, 160),
		"Action group: %s",
		questDialogueEditorActionGroupName()
	);
	toolboxY += toolboxRowHeight;

	toolboxButtonPair(
		toolboxY,
		questDialogueEditorGuidedActionLabel(0),
		questDialogueEditorGuidedActionLabel(1),
		[]()
		{
			questDialogueEditorApplyGuidedAction(0);
		},
		[]()
		{
			questDialogueEditorApplyGuidedAction(1);
		}
	);
	toolboxY += toolboxRowHeight;

	if ( questDialogueEditorActionGroup
		== QUEST_DIALOGUE_ACTION_GROUP_MECHANISMS )
	{
		printTextFormattedColor(
			font8x8_bmp,
			toolboxX1,
			toolboxY + 4,
			makeColorRGB(255, 230, 96),
			"Cursor tile: %d, %d",
			drawx,
			drawy
		);
		toolboxY += toolboxRowHeight;
	}

	if ( questDialogueEditorActionGroup
		== QUEST_DIALOGUE_ACTION_GROUP_REWARDS
		|| questDialogueEditorActionGroup
			== QUEST_DIALOGUE_ACTION_GROUP_COSTS )
	{
		toolboxButtonPair(
			toolboxY,
			"ITEM <",
			"ITEM >",
			[]()
			{
				questDialogueEditorCycleSelectedItem(-1);
			},
			[]()
			{
				questDialogueEditorCycleSelectedItem(1);
			}
		);
		toolboxY += toolboxRowHeight;

		printTextFormatted(
			font8x8_bmp,
			toolboxX1,
			toolboxY + 4,
			"Selected item ID: %d",
			questDialogueEditorSelectedItemID
		);
		toolboxY += toolboxRowHeight;

		printTextFormattedColor(
			font8x8_bmp,
			toolboxX1,
			toolboxY + 4,
			makeColorRGB(255, 230, 96),
			"Item: %.24s",
			items[
				questDialogueEditorSelectedItemID
			].getIdentifiedName()
		);
		toolboxY += toolboxRowHeight;

		toolboxButtonPair(
			toolboxY,
			"QTY -",
			"QTY +",
			[]()
			{
				questDialogueEditorSelectedItemCount =
					std::max(
						1,
						questDialogueEditorSelectedItemCount - 1
					);
			},
			[]()
			{
				questDialogueEditorSelectedItemCount =
					std::min(
						999,
						questDialogueEditorSelectedItemCount + 1
					);
			}
		);
		toolboxY += toolboxRowHeight;

		printTextFormatted(
			font8x8_bmp,
			toolboxX1,
			toolboxY + 4,
			"Item quantity: %d",
			questDialogueEditorSelectedItemCount
		);
		toolboxY += toolboxRowHeight;

		toolboxButtonPair(
			toolboxY,
			"GOLD -10",
			"GOLD +10",
			[]()
			{
				questDialogueEditorGoldAmount =
					std::max(
						0,
						questDialogueEditorGoldAmount - 10
					);
			},
			[]()
			{
				questDialogueEditorGoldAmount =
					std::min(
						999999,
						questDialogueEditorGoldAmount + 10
					);
			}
		);
		toolboxY += toolboxRowHeight;

		printTextFormattedColor(
			font8x8_bmp,
			toolboxX1,
			toolboxY + 4,
			makeColorRGB(255, 230, 96),
			"Gold amount: %d",
			questDialogueEditorGoldAmount
		);
		toolboxY += toolboxRowHeight;
	}

	if ( questDialogueEditorActionGroup
		== QUEST_DIALOGUE_ACTION_GROUP_STATUS )
	{
		toolboxButtonPair(
			toolboxY,
			"EFFECT <",
			"EFFECT >",
			[]()
			{
				questDialogueEditorCycleSelectedEffect(-1);
			},
			[]()
			{
				questDialogueEditorCycleSelectedEffect(1);
			}
		);
		toolboxY += toolboxRowHeight;

		toolboxButtonPair(
			toolboxY,
			"TIME -",
			"TIME +",
			[]()
			{
				questDialogueEditorEffectDurationSeconds =
					std::max(
						0,
						questDialogueEditorEffectDurationSeconds - 5
					);
			},
			[]()
			{
				questDialogueEditorEffectDurationSeconds += 5;
			}
		);
		toolboxY += toolboxRowHeight;

		toolboxButtonPair(
			toolboxY,
			"POWER -",
			"POWER +",
			[]()
			{
				questDialogueEditorEffectStrength =
					std::max(
						1,
						questDialogueEditorEffectStrength - 1
					);
			},
			[]()
			{
				questDialogueEditorEffectStrength =
					std::min(
						255,
						questDialogueEditorEffectStrength + 1
					);
			}
		);
		toolboxY += toolboxRowHeight;

		printTextFormattedColor(
			font8x8_bmp,
			toolboxX1,
			toolboxY + 4,
			makeColorRGB(255, 230, 96),
			"Effect %d: %.18s",
			questDialogueEditorSelectedEffectID,
			questDialogueEditorEffectName(
				questDialogueEditorSelectedEffectID
			)
		);
		toolboxY += toolboxRowHeight;

		printTextFormatted(
			font8x8_bmp,
			toolboxX1,
			toolboxY + 4,
			"Duration: %ds | Strength: %d",
			questDialogueEditorEffectDurationSeconds,
			questDialogueEditorEffectStrength
		);
		toolboxY += toolboxRowHeight;
	}

	if ( questDialogueEditorActionGroup
		== QUEST_DIALOGUE_ACTION_GROUP_QUEST
		|| questDialogueEditorActionGroup
			== QUEST_DIALOGUE_ACTION_GROUP_VARIABLES )
	{
		toolboxButtonPair(
			toolboxY,
			questDialogueEditorGuidedActionLabel(2),
			questDialogueEditorGuidedActionLabel(3),
			[]()
			{
				questDialogueEditorApplyGuidedAction(2);
			},
			[]()
			{
				questDialogueEditorApplyGuidedAction(3);
			}
		);
		toolboxY += toolboxRowHeight;

		toolboxButtonPair(
			toolboxY,
			questDialogueEditorGuidedActionLabel(4),
			questDialogueEditorGuidedActionLabel(5),
			[]()
			{
				questDialogueEditorApplyGuidedAction(4);
			},
			[]()
			{
				questDialogueEditorApplyGuidedAction(5);
			}
		);
		toolboxY += toolboxRowHeight;
	}

	toolboxButtonPair(
		toolboxY,
		"COMPARE",
		"SCOPE",
		[]()
		{
			questDialogueEditorCycleComparisonDirect();
		},
		[]()
		{
			questDialogueEditorCycleScopeDirect();
		}
	);
	toolboxY += toolboxRowHeight;

	toolboxButtonPair(
		toolboxY,
		"COND ITEM",
		"REWARD",
		[]()
		{
			questDialogueEditorCycleConditionItemDirect();
		},
		[]()
		{
			questDialogueEditorCycleRewardItemDirect();
		}
	);
	toolboxY += toolboxRowHeight;

	toolboxButtonPair(
		toolboxY,
		"REMOVE ITEM",
		"REMOVE GOLD",
		[]()
		{
			questDialogueEditorToggleRemoveItemDirect();
		},
		[]()
		{
			questDialogueEditorToggleRemoveGoldDirect();
		}
	);
	toolboxY += toolboxRowHeight + 3;

	toolboxButtonPair(
		toolboxY,
		"RECRUIT",
		"REPEAT",
		[]()
		{
			questDialogueEditorToggleRecruit();
		},
		[]()
		{
			questDialogueEditorToggleRepeatable();
		}
	);
	toolboxY += toolboxRowHeight;

	if ( dialogueEditorButton(
		toolboxX1,
		toolboxY,
		toolboxX2 - toolboxX1,
		"GIVER MARKER"
	) )
	{
		questDialogueEditorCycleGiverMarker();
	}

	toolboxY += toolboxRowHeight;

	toolboxButtonPair(
		toolboxY,
		"PICK TILE ON MAP",
		"USE SELECTED NPC",
		[]()
		{
			questDialogueEditorUseCursorTileAsQuestGiver();
		},
		[]()
		{
			questDialogueEditorUseSelectedNPCAsQuestGiver();
		}
	);
	toolboxY += toolboxRowHeight;

	if ( dialogueEditorButton(
		toolboxX1,
		toolboxY,
		toolboxX2 - toolboxX1,
		"CLEAR GIVER"
	) )
	{
		questDialogueEditorClearQuestGiver();
	}
	toolboxY += toolboxRowHeight;

	const std::string giverMarkerSummary =
		questDialogueEditorGiverMarkerSummary();

	printTextFormattedColor(
		font8x8_bmp,
		toolboxX1,
		toolboxY + 4,
		questDialogueEditorQuestGiverPersistentID() > 0
			? makeColorRGB(128, 255, 160)
			: makeColorRGB(128, 192, 255),
		"%.28s",
		giverMarkerSummary.c_str()
	);
	toolboxY += toolboxRowHeight + 3;

	toolboxButtonPair(
		toolboxY,
		"EDIT CHOICE",
		"VALIDATE",
		[]()
		{
			questDialogueEditorEditChoiceTextDirect();
		},
		[]()
		{
			std::string error;

			if ( questDialogueEditorValidateDocument(error) )
			{
				questDialogueEditorSetMessage(
					"Validation passed."
				);
			}
			else
			{
				questDialogueEditorSetMessage(
					"Validation: " + error
				);
			}
		}
	);
	toolboxY += toolboxRowHeight;

	toolboxButtonPair(
		toolboxY,
		"DUP FILE",
		"DEL FILE",
		[]()
		{
			questDialogueEditorDuplicateSelectedFile();
		},
		[]()
		{
			questDialogueEditorDeleteSelectedFile();
		}
	);
	toolboxY += toolboxRowHeight;

	const int fieldStatusY = toolboxY + 21;

	printTextFormattedColor(
		font8x8_bmp,
		toolboxX1,
		fieldStatusY,
		makeColorRGB(255, 230, 96),
		"[%s] %.16s",
		questDialogueEditorFieldCategoryName(),
		questDialogueEditorEditableFieldName()
	);

	drawDepressed(
		toolboxX1,
		fieldStatusY + 13,
		toolboxX2,
		fieldStatusY + 30
	);

	printTextFormatted(
		font8x8_bmp,
		toolboxX1 + 4,
		fieldStatusY + 17,
		"%.22s",
		questDialogueEditorEditingField
			? questDialogueEditorEditBuffer
			: questDialogueEditorReadEditableField().c_str()
	);

	if ( questDialogueEditorEditingField
		&& (ticks - cursorflash) % TICKS_PER_SECOND
			< TICKS_PER_SECOND / 2 )
	{
		const int cursorX =
			toolboxX1 + 4
			+ std::min<int>(
				static_cast<int>(
					strlen(
						questDialogueEditorEditBuffer
					)
				) * 8,
				toolboxX2 - toolboxX1 - 12
			);

		printText(
			font8x8_bmp,
			cursorX,
			fieldStatusY + 17,
			"_"
		);
	}

	printText(
		font8x8_bmp,
		fileX1 + 6,
		fileListTitleY,
		"Dialogue JSON Files"
	);


	printText(
		font8x8_bmp,
		treeX1 + 6,
		panelY1 + 6,
		"Decision Tree"
	);

	printText(
		font8x8_bmp,
		detailX1 + 6,
		panelY1 + 6,
		"Quest / Node Details"
	);

	const int visibleFiles =
		std::max(
			1,
			(panelY2 - fileListY1 - 8) / 16
		);

	questDialogueEditorFileScroll =
		std::max(
			0,
			std::min(
				questDialogueEditorFileScroll,
				std::max(
					0,
					static_cast<int>(
						questDialogueEditorFiles.size()
					) - visibleFiles
				)
			)
		);

	for ( int row = 0;
		row < visibleFiles;
		++row )
	{
		const int index =
			questDialogueEditorFileScroll + row;

		if ( index >= static_cast<int>(
			questDialogueEditorFiles.size()
		) )
		{
			break;
		}

		const int y = fileListY1 + row * 16;

		if ( index == questDialogueEditorSelectedFile )
		{
			drawDepressed(
				fileX1 + 4,
				y - 2,
				fileX2 - 4,
				y + 12
			);
		}

		const std::string clippedFilename =
			dialogueEditorClippedText(
				questDialogueEditorFiles[index],
				fileX2 - fileX1 - 16
			);

		printTextFormatted(
			font8x8_bmp,
			fileX1 + 8,
			y,
			"%s",
			clippedFilename.c_str()
		);

		if ( mousestatus[SDL_BUTTON_LEFT]
			&& omousex >= fileX1 + 4
			&& omousex < fileX2 - 4
			&& omousey >= y - 2
			&& omousey < y + 14 )
		{
			mousestatus[SDL_BUTTON_LEFT] = 0;
				questDialogueEditorRequestTransition(
					QUEST_DIALOGUE_PENDING_SWITCH_FILE,
					index);
		}
	}

	if ( questDialogueEditorPreview.error.empty() )
	{
		int treeY = panelY1 + 26;

		for ( int nodeIndex = 0;
			nodeIndex < static_cast<int>(
				questDialogueEditorPreview.nodes.size()
			);
			++nodeIndex )
		{
			const auto& node =
				questDialogueEditorPreview.nodes[nodeIndex];
			const int nodeStartY = treeY;

			if ( treeY + 14 >= panelY2 )
			{
				break;
			}

			if ( nodeIndex == questDialogueEditorSelectedNode )
			{
				drawDepressed(
					treeX1 + 4,
					treeY - 2,
					treeX2 - 4,
					treeY + 12
				);
			}

			const std::string nodeLabel =
				"[Node "
				+ std::to_string(node.id)
				+ "] "
				+ node.text;

			const std::vector<std::string> nodeLines =
				dialogueEditorWrappedLines(
					nodeLabel,
					treeX2 - treeX1 - 16,
					3
				);

			for ( const std::string& line :
				nodeLines )
			{
				printTextFormatted(
					font8x8_bmp,
					treeX1 + 8,
					treeY,
					"%s",
					line.c_str()
				);

				treeY += 12;
			}

			if ( mousestatus[SDL_BUTTON_LEFT]
				&& omousex >= treeX1 + 4
				&& omousex < treeX2 - 4
				&& omousey >= nodeStartY - 2
				&& omousey < treeY )
			{
				mousestatus[SDL_BUTTON_LEFT] = 0;
				questDialogueEditorSelectedNode =
					nodeIndex;
				questDialogueEditorSelectedChoice = -1;
                questDialogueEditorInspectorSelection =
                    QUEST_DIALOGUE_INSPECTOR_NONE;
				questDialogueEditorFieldCategory =
					QUEST_DIALOGUE_CATEGORY_TEXT;
				questDialogueEditorEditableField =
					QUEST_DIALOGUE_FIELD_NODE_TEXT;
			}

			treeY += 4;

			for ( size_t choiceIndex = 0;
				choiceIndex < node.choices.size();
				++choiceIndex )
			{
                const int choiceStartY = treeY;

				if ( treeY + 14 >= panelY2 )
				{
					break;
				}

				if ( nodeIndex
						== questDialogueEditorSelectedNode
					&& static_cast<int>(choiceIndex)
						== questDialogueEditorSelectedChoice )
				{
					drawDepressed(
						treeX1 + 16,
						treeY - 2,
						treeX2 - 4,
						treeY + 12
					);
				}

				const std::string choiceLabel =
					"-> "
					+ node.choices[choiceIndex]
					+ " [next "
					+ std::to_string(
						node.nextNodes[choiceIndex]
					)
					+ "]";

				const std::vector<std::string> choiceLines =
					dialogueEditorWrappedLines(
						choiceLabel,
						treeX2 - treeX1 - 28,
						3
					);

				for ( const std::string& line :
					choiceLines )
				{
					printTextFormattedColor(
						font8x8_bmp,
						treeX1 + 20,
						treeY,
						makeColorRGB(255, 230, 96),
						"%s",
						line.c_str()
					);

					treeY += 12;
				}

				if ( questDialogueEditorDocument.IsObject()
					&& questDialogueEditorDocument.HasMember("nodes")
					&& questDialogueEditorDocument["nodes"].IsArray()
					&& nodeIndex
						< static_cast<int>(
							questDialogueEditorDocument["nodes"].Size()
						) )
				{
					const rapidjson::Value& documentNode =
						questDialogueEditorDocument["nodes"][
							static_cast<rapidjson::SizeType>(
								nodeIndex
							)
						];

					if ( documentNode.IsObject()
						&& documentNode.HasMember("choices")
						&& documentNode["choices"].IsArray()
						&& choiceIndex
							< documentNode["choices"].Size() )
					{
						const rapidjson::Value& documentChoice =
							documentNode["choices"][
								static_cast<rapidjson::SizeType>(
									choiceIndex
								)
							];

						const std::string conditionSummary =
							questDialogueEditorConditionSummary(
								documentChoice
							);

						if ( conditionSummary != "None" )
						{
                            const int conditionRowY = treeY;
                            const bool conditionSelected =
                                nodeIndex == questDialogueEditorSelectedNode
                                && static_cast<int>(choiceIndex)
                                    == questDialogueEditorSelectedChoice
                                && questDialogueEditorInspectorSelection
                                    == QUEST_DIALOGUE_INSPECTOR_CONDITION;

                            if ( conditionSelected )
                            {
                                drawDepressed(
                                    treeX1 + 24,
                                    conditionRowY - 2,
                                    treeX2 - 4,
                                    conditionRowY + 10
                                );
                            }

							printTextFormattedColor(
								font8x8_bmp,
								treeX1 + 28,
								conditionRowY,
								makeColorRGB(128, 255, 160),
								"Requires: %.24s",
								conditionSummary.c_str()
							);

                            if ( mousestatus[SDL_BUTTON_LEFT]
                                && omousex >= treeX1 + 24
                                && omousex < treeX2 - 4
                                && omousey >= conditionRowY - 2
                                && omousey < conditionRowY + 11 )
                            {
                                mousestatus[SDL_BUTTON_LEFT] = 0;
                                questDialogueEditorSelectedNode = nodeIndex;
                                questDialogueEditorSelectedChoice =
                                    static_cast<int>(choiceIndex);
                                questDialogueEditorInspectorSelection =
                                    QUEST_DIALOGUE_INSPECTOR_CONDITION;
                                questDialogueEditorFieldCategory =
                                    QUEST_DIALOGUE_CATEGORY_CONDITION;
                            }

							treeY += 12;
						}

						const std::string objectiveSummary =
							questDialogueEditorChoiceObjectiveSummary(
								documentChoice
							);

						if ( !objectiveSummary.empty() )
						{
							printTextFormattedColor(
								font8x8_bmp,
								treeX1 + 28,
								treeY,
								makeColorRGB(128, 192, 255),
								"Objective: %.23s",
								objectiveSummary.c_str()
							);
							treeY += 12;
						}

						const std::string actionSummary =
							questDialogueEditorChoiceActionSummaryShort(
								documentChoice
							);

						if ( actionSummary != "None" )
						{
                            const int actionRowY = treeY;
                            const bool actionSelected =
                                nodeIndex == questDialogueEditorSelectedNode
                                && static_cast<int>(choiceIndex)
                                    == questDialogueEditorSelectedChoice
                                && questDialogueEditorInspectorSelection
                                    == QUEST_DIALOGUE_INSPECTOR_ACTION;

                            if ( actionSelected )
                            {
                                drawDepressed(
                                    treeX1 + 24,
                                    actionRowY - 2,
                                    treeX2 - 4,
                                    actionRowY + 10
                                );
                            }

							printTextFormattedColor(
								font8x8_bmp,
								treeX1 + 28,
								actionRowY,
								makeColorRGB(255, 128, 128),
								"Action: %.26s",
								actionSummary.c_str()
							);

                            if ( mousestatus[SDL_BUTTON_LEFT]
                                && omousex >= treeX1 + 24
                                && omousex < treeX2 - 4
                                && omousey >= actionRowY - 2
                                && omousey < actionRowY + 11 )
                            {
                                mousestatus[SDL_BUTTON_LEFT] = 0;
                                questDialogueEditorSelectedNode = nodeIndex;
                                questDialogueEditorSelectedChoice =
                                    static_cast<int>(choiceIndex);
                                questDialogueEditorInspectorSelection =
                                    QUEST_DIALOGUE_INSPECTOR_ACTION;
                                questDialogueEditorFieldCategory =
                                    QUEST_DIALOGUE_CATEGORY_ACTION;
                                questDialogueEditorEditableField =
                                    QUEST_DIALOGUE_FIELD_ACTION_NUMBER;
                            }

							treeY += 12;
						}
					}
				}

				if ( mousestatus[SDL_BUTTON_LEFT]
					&& omousex >= treeX1 + 16
					&& omousex < treeX2 - 4
                    && omousey >= choiceStartY - 2
                    && omousey < choiceStartY
                        + static_cast<int>(choiceLines.size()) * 12 )
				{
					mousestatus[SDL_BUTTON_LEFT] = 0;
					questDialogueEditorSelectedNode =
						nodeIndex;
					questDialogueEditorSelectedChoice =
						static_cast<int>(choiceIndex);
                    questDialogueEditorInspectorSelection =
                        QUEST_DIALOGUE_INSPECTOR_NONE;
					questDialogueEditorFieldCategory =
						QUEST_DIALOGUE_CATEGORY_TEXT;
					questDialogueEditorEditableField =
						QUEST_DIALOGUE_FIELD_CHOICE_TEXT;
				}

				treeY += 2;
			}

			treeY += 6;
		}
	}
	else
	{
		printTextFormattedColor(
			font8x8_bmp,
			treeX1 + 8,
			panelY1 + 28,
			makeColorRGB(255, 96, 96),
			"%s",
			questDialogueEditorPreview.error.c_str()
		);
	}

	int detailY = panelY1 + 26;

	printTextFormatted(
		font8x8_bmp,
		detailX1 + 8,
		detailY,
		"File: %.24s",
		questDialogueEditorPreview.filename.c_str()
	);
	detailY += 16;

	printTextFormatted(
		font8x8_bmp,
		detailX1 + 8,
		detailY,
		"Quest ID: %s",
		dialogueEditorClippedText(
			questDialogueEditorPreview.questID,
			detailX2 - detailX1 - 80
		).c_str()
	);
	detailY += 16;

	printTextFormatted(
		font8x8_bmp,
		detailX1 + 8,
		detailY,
		"Title: %.23s",
		questDialogueEditorPreview.title.c_str()
	);
	detailY += 16;

	printTextFormattedColor(
		font8x8_bmp,
		detailX1 + 8,
		detailY,
		makeColorRGB(255, 230, 96),
		"Summary:"
	);
	detailY += 12;

	dialogueEditorDrawWrappedText(
		detailX1 + 12,
		detailY,
		detailX2 - detailX1 - 24,
		questDialogueEditorPreview.summary,
		6,
		makeColorRGB(224, 224, 224)
	);
	detailY += 6;

	const std::string previewScopeLabel =
		questDialogueEditorPreview.scope == "player" ? "Personal"
		: (questDialogueEditorPreview.scope == "party" ? "Party" : "World");
	printTextFormatted(
		font8x8_bmp,
		detailX1 + 8,
		detailY,
		"Scope: %s (schema %d)",
		previewScopeLabel.c_str(),
		questDialogueEditorPreview.schemaVersion
	);
	detailY += 16;

	printTextFormatted(
		font8x8_bmp,
		detailX1 + 8,
		detailY,
		"Repeatable: %s",
		questDialogueEditorPreview.repeatable
			? "Yes"
			: "No"
	);
	detailY += 16;

	printTextFormatted(
		font8x8_bmp,
		detailX1 + 8,
		detailY,
		"Objectives: %d",
		questDialogueEditorPreview.objectiveCount
	);
	detailY += 16;

	if ( questDialogueEditorPreview.objectiveCount > 0 )
	{
		printTextFormattedColor(
			font8x8_bmp,
			detailX1 + 8,
			detailY,
			makeColorRGB(128, 255, 160),
			"Objective selected: %d",
			questDialogueEditorSelectedObjective + 1
		);
		detailY += 16;

		if ( dialogueEditorButton(
			detailX1 + 8,
			detailY,
			64,
			"NEXT OBJ"
		) )
		{
			questDialogueEditorSelectedObjective =
				(
					questDialogueEditorSelectedObjective + 1
				)
				% questDialogueEditorPreview.objectiveCount;
		}
		detailY += 22;

		rapidjson::Value* selectedObjective =
			questDialogueEditorSelectedObjectiveValue();
		if ( selectedObjective && selectedObjective->IsObject() )
		{
			printTextFormattedColor(
				font8x8_bmp,
				detailX1 + 8,
				detailY,
				makeColorRGB(128, 192, 255),
				"OBJECTIVE INSPECTOR"
			);
			detailY += 14;

			struct ObjectiveInspectorField
			{
				QuestDialogueEditableField field;
				const char* label;
			};

			const ObjectiveInspectorField objectiveFields[] =
			{
				{ QUEST_DIALOGUE_FIELD_OBJECTIVE_ID, "Objective ID" },
				{ QUEST_DIALOGUE_FIELD_OBJECTIVE_TEXT, "Objective text" },
				{ QUEST_DIALOGUE_FIELD_OBJECTIVE_STAGE, "Required stage" },
				{ QUEST_DIALOGUE_FIELD_OBJECTIVE_TARGET, "Required target" },
				{ QUEST_DIALOGUE_FIELD_OBJECTIVE_DEFEAT_ID, "Defeat ID" }
			};

			for ( const auto& objectiveField : objectiveFields )
			{
				const QuestDialogueFieldCategory previousCategory =
					questDialogueEditorFieldCategory;
				const QuestDialogueEditableField previousField =
					questDialogueEditorEditableField;

				questDialogueEditorFieldCategory =
					QUEST_DIALOGUE_CATEGORY_OBJECTIVE;
				questDialogueEditorEditableField = objectiveField.field;
				const std::string objectiveValue =
					questDialogueEditorReadEditableField();

				questDialogueEditorFieldCategory = previousCategory;
				questDialogueEditorEditableField = previousField;

				if ( dialogueEditorButton(
					detailX1 + 8,
					detailY,
					detailX2 - detailX1 - 16,
					(std::string(objectiveField.label) + ": "
						+ dialogueEditorClippedText(
							objectiveValue.empty()
								? "(click to set)"
								: objectiveValue,
							detailX2 - detailX1 - 136
						)
					).c_str()
				) )
				{
					questDialogueEditorFieldCategory =
						QUEST_DIALOGUE_CATEGORY_OBJECTIVE;
					questDialogueEditorEditableField = objectiveField.field;
					questDialogueEditorBeginEditingField();
					break;
				}
				detailY += 20;
			}

			const bool objectiveOptional =
				selectedObjective->HasMember("optional")
				&& (*selectedObjective)["optional"].IsBool()
				&& (*selectedObjective)["optional"].GetBool();

			printTextFormatted(
				font8x8_bmp,
				detailX1 + 12,
				detailY,
				"Optional: %s | Marker: %s",
				objectiveOptional ? "Yes" : "No",
				selectedObjective->HasMember("map_marker") ? "Yes" : "No"
			);
			detailY += 18;
		}
	}

	printTextFormatted(
		font8x8_bmp,
		detailX1 + 8,
		detailY,
		"Giver marker: %s",
		questDialogueEditorPreview.hasOriginMarker
			? (
				questDialogueEditorPreview.originTracksNPC
					? "Follow NPC"
					: "Static tile"
			)
			: "Optional / Off"
	);
	detailY += 16;

	if ( questDialogueEditorPreview.originTracksNPC )
	{
		printTextFormatted(
			font8x8_bmp,
			detailX1 + 8,
			detailY,
			"NPC persistent ID: %d",
			questDialogueEditorPreview.originNPCPersistentID
		);
		detailY += 16;
	}

	printTextFormatted(
		font8x8_bmp,
		detailX1 + 8,
		detailY,
		"Recruit actions: %d",
		questDialogueEditorPreview.recruitActionCount
	);
	detailY += 16;

	printTextFormatted(
		font8x8_bmp,
		detailX1 + 8,
		detailY,
		"Objective markers: %d",
		questDialogueEditorPreview.objectiveMarkerCount
	);
	detailY += 24;

	if ( questDialogueEditorSelectedNode >= 0
		&& questDialogueEditorSelectedNode
			< static_cast<int>(
				questDialogueEditorPreview.nodes.size()
			) )
	{
		const auto& node =
			questDialogueEditorPreview.nodes[
				questDialogueEditorSelectedNode
			];

		printTextFormattedColor(
			font8x8_bmp,
			detailX1 + 8,
			detailY,
			makeColorRGB(128, 255, 160),
			"Selected Node %d",
			node.id
		);
		detailY += 16;

		printTextFormattedColor(
			font8x8_bmp,
			detailX1 + 8,
			detailY,
			makeColorRGB(255, 230, 96),
			"Node text:"
		);
		detailY += 12;

		const int nodeTextY = detailY;
		const bool editingNodeText =
			questDialogueEditorEditingField
			&& questDialogueEditorEditableField
				== QUEST_DIALOGUE_FIELD_NODE_TEXT;

		const std::string nodeTextDisplay =
			editingNodeText
				? std::string(questDialogueEditorEditBuffer) + "|"
				: node.text;

		dialogueEditorDrawWrappedText(
			detailX1 + 12,
			detailY,
			detailX2 - detailX1 - 24,
			nodeTextDisplay,
			5,
			editingNodeText
				? makeColorRGB(255, 230, 96)
				: makeColorRGB(224, 224, 224)
		);

		if ( mousestatus[SDL_BUTTON_LEFT]
			&& omousex >= detailX1 + 8
			&& omousex < detailX2 - 8
			&& omousey >= nodeTextY - 2
			&& omousey < nodeTextY + 58 )
		{
			mousestatus[SDL_BUTTON_LEFT] = 0;
			questDialogueEditorFieldCategory =
				QUEST_DIALOGUE_CATEGORY_TEXT;
			questDialogueEditorEditableField =
				QUEST_DIALOGUE_FIELD_NODE_TEXT;
			questDialogueEditorBeginEditingField();
		}

		detailY += 6;

		printTextFormatted(
			font8x8_bmp,
			detailX1 + 8,
			detailY,
			"Choices: %d",
			static_cast<int>(node.choices.size())
		);
		detailY += 16;

		printTextFormatted(
			font8x8_bmp,
			detailX1 + 8,
			detailY,
			"Conditions: %d",
			node.conditionCount
		);
		detailY += 16;

		printTextFormatted(
			font8x8_bmp,
			detailX1 + 8,
			detailY,
			"Actions: %d",
			node.actionCount
		);
		detailY += 24;

		if ( questDialogueEditorSelectedChoice >= 0 )
		{
			printTextFormatted(
				font8x8_bmp,
				detailX1 + 8,
				detailY,
				"Choice condition: %.16s",
				questDialogueEditorChoiceConditionName().c_str()
			);
			detailY += 16;

			rapidjson::Value* selectedChoiceForSummary =
				questDialogueEditorSelectedChoiceValueForEdit();

			if ( selectedChoiceForSummary )
			{
				const std::string currentRequirement =
					questDialogueEditorConditionSummary(
						*selectedChoiceForSummary
					);

				printTextFormattedColor(
					font8x8_bmp,
					detailX1 + 12,
					detailY,
					makeColorRGB(128, 255, 160),
					"Requires: %.23s",
					currentRequirement.c_str()
				);
				detailY += 16;
			}

            const std::vector<std::string> visibleActions =
                questDialogueEditorChoiceActionMembers();
            printTextFormatted(
                font8x8_bmp,
                detailX1 + 8,
                detailY,
                "Choice action %d/%d: %.15s",
                visibleActions.empty() ? 0 : questDialogueEditorSelectedActionIndex + 1,
                static_cast<int>(visibleActions.size()),
                questDialogueEditorChoiceActionName().c_str()
            );
            detailY += 24;

            if ( questDialogueEditorInspectorSelection
                != QUEST_DIALOGUE_INSPECTOR_NONE )
            {
                const bool inspectCondition =
                    questDialogueEditorInspectorSelection
                        == QUEST_DIALOGUE_INSPECTOR_CONDITION;

                printTextFormattedColor(
                    font8x8_bmp,
                    detailX1 + 8,
                    detailY,
                    inspectCondition
                        ? makeColorRGB(128, 255, 160)
                        : makeColorRGB(255, 128, 128),
                    "%s INSPECTOR",
                    inspectCondition ? "CONDITION" : "ACTION"
                );
                detailY += 14;

                printTextFormatted(
                    font8x8_bmp,
                    detailX1 + 12,
                    detailY,
                    "Type: %.24s",
                    inspectCondition
                        ? questDialogueEditorChoiceConditionName().c_str()
                        : questDialogueEditorChoiceActionName().c_str()
                );
                detailY += 16;

                const int inspectorButtonWidth =
                    (detailX2 - detailX1 - 28) / 2;

                if ( dialogueEditorButton(
                    detailX1 + 8,
                    detailY,
                    inspectorButtonWidth,
                    "PREVIOUS TYPE"
                ) )
                {
                    if ( inspectCondition )
                    {
                        deferredInspectorCommand =
                            QUEST_DIALOGUE_DEFERRED_CONDITION_PREVIOUS;
                    }
                    else
                    {
                        deferredInspectorCommand =
                            QUEST_DIALOGUE_DEFERRED_ACTION_PREVIOUS;
                    }
                }

                if ( dialogueEditorButton(
                    detailX1 + 16 + inspectorButtonWidth,
                    detailY,
                    inspectorButtonWidth,
                    "NEXT TYPE"
                ) )
                {
                    if ( inspectCondition )
                    {
                        deferredInspectorCommand =
                            QUEST_DIALOGUE_DEFERRED_CONDITION_NEXT;
                    }
                    else
                    {
                        deferredInspectorCommand =
                            QUEST_DIALOGUE_DEFERRED_ACTION_NEXT;
                    }
                }
                detailY += 20;

                if ( inspectCondition )
                {
                    const std::string type =
                        questDialogueEditorSelectedConditionType();
                    const bool hasReference =
                        type == "has_item"
                        || type == "quest_started"
                        || type == "quest_accepted"
                        || type == "quest_completed"
                        || type == "quest_failed"
                        || type == "quest_stage"
                        || type == "objective_completed"
                        || type == "objective_incomplete"
                        || type == "world_flag"
                        || type == "npc_flag"
                        || type == "world_variable"
                        || type == "npc_variable";
                    const bool hasNumber =
                        type == "has_item"
                        || type == "has_gold"
                        || type == "quest_stage"
                        || type == "world_variable"
                        || type == "npc_variable";

                    if ( type == "objective_completed"
                        || type == "objective_incomplete" )
                    {
                        questDialogueEditorFieldCategory =
                            QUEST_DIALOGUE_CATEGORY_CONDITION;
                        questDialogueEditorEditableField =
                            QUEST_DIALOGUE_FIELD_CONDITION_QUEST;
                        const std::string questValue =
                            questDialogueEditorReadEditableField();
                        if ( dialogueEditorButton(
                            detailX1 + 8, detailY,
                            detailX2 - detailX1 - 16,
                            ("REQUIRED QUEST: "
                                + dialogueEditorClippedText(
                                    questValue.empty() ? "(click to set)" : questValue,
                                    detailX2 - detailX1 - 140
                                )
                            ).c_str()
                        ) )
                        {
                            questDialogueEditorBeginEditingField();
                        }
                        detailY += 20;
                    }

                    if ( hasReference )
                    {
                        questDialogueEditorFieldCategory =
                            QUEST_DIALOGUE_CATEGORY_CONDITION;
                        questDialogueEditorEditableField =
                            QUEST_DIALOGUE_FIELD_CONDITION_REFERENCE;
                        const std::string value =
                            questDialogueEditorReadEditableField();
                        const std::string referenceLabel =
                            std::string(
                                (type == "objective_completed" || type == "objective_incomplete")
                                    ? "REQUIRED OBJECTIVE: "
                                    : (type == "has_item" ? "REQUIRED ITEM: " : "REQUIRED ID: ")
                            )
                            + dialogueEditorClippedText(
                                value.empty() ? "(click to set)" : value,
                                detailX2 - detailX1 - 100
                            );
                        if ( dialogueEditorButton(
                            detailX1 + 8,
                            detailY,
                            detailX2 - detailX1 - 16,
                            referenceLabel.c_str()
                        ) )
                        {
                            questDialogueEditorOpenConditionReferenceEditor();
                        }
                        detailY += 20;
                    }

                    if ( hasNumber )
                    {
                        questDialogueEditorFieldCategory =
                            QUEST_DIALOGUE_CATEGORY_CONDITION;
                        questDialogueEditorEditableField =
                            QUEST_DIALOGUE_FIELD_CONDITION_NUMBER;
                        const std::string value =
                            questDialogueEditorReadEditableField();
                        if ( dialogueEditorButton(
                            detailX1 + 8,
                            detailY,
                            detailX2 - detailX1 - 16,
                            ("VALUE / ID: "
                                + (value.empty() ? "(click to set)" : value)
                            ).c_str()
                        ) )
                        {
                            questDialogueEditorOpenConditionNumberEditor();
                        }
                        detailY += 20;
                    }
                }
                else
                {
                    rapidjson::Value* selectedChoice =
                        questDialogueEditorSelectedChoiceValueForEdit();
                    const std::vector<std::string> selectedActionMembers =
                        questDialogueEditorChoiceActionMembers();
                    const bool selectedPowerAction =
                        questDialogueEditorSelectedActionIndex >= 0
                        && questDialogueEditorSelectedActionIndex
                            < static_cast<int>(selectedActionMembers.size())
                        && selectedActionMembers[
                            questDialogueEditorSelectedActionIndex
                        ] == "set_power";

                    if ( selectedPowerAction )
                    {
                        const QuestDialogueFieldCategory savedCategory =
                            questDialogueEditorFieldCategory;
                        const QuestDialogueEditableField savedField =
                            questDialogueEditorEditableField;

                        questDialogueEditorFieldCategory =
                            QUEST_DIALOGUE_CATEGORY_ACTION;
                        questDialogueEditorEditableField =
                            QUEST_DIALOGUE_FIELD_POWER_X;
                        const std::string powerX =
                            questDialogueEditorReadEditableField();

                        questDialogueEditorEditableField =
                            QUEST_DIALOGUE_FIELD_POWER_Y;
                        const std::string powerY =
                            questDialogueEditorReadEditableField();

                        questDialogueEditorFieldCategory = savedCategory;
                        questDialogueEditorEditableField = savedField;

                        if ( dialogueEditorButton(
                            detailX1 + 8, detailY,
                            detailX2 - detailX1 - 16,
                            ("POWER TILE X: " + powerX).c_str()
                        ) )
                        {
                            questDialogueEditorFieldCategory =
                                QUEST_DIALOGUE_CATEGORY_ACTION;
                            questDialogueEditorEditableField =
                                QUEST_DIALOGUE_FIELD_POWER_X;
                            questDialogueEditorBeginEditingField();
                        }
                        detailY += 20;

                        if ( dialogueEditorButton(
                            detailX1 + 8, detailY,
                            detailX2 - detailX1 - 16,
                            ("POWER TILE Y: " + powerY).c_str()
                        ) )
                        {
                            questDialogueEditorFieldCategory =
                                QUEST_DIALOGUE_CATEGORY_ACTION;
                            questDialogueEditorEditableField =
                                QUEST_DIALOGUE_FIELD_POWER_Y;
                            questDialogueEditorBeginEditingField();
                        }
                        detailY += 20;
                    }

                    const QuestDialogueFieldCategory previousCategory =
                        questDialogueEditorFieldCategory;
                    const QuestDialogueEditableField previousField =
                        questDialogueEditorEditableField;

                    questDialogueEditorFieldCategory =
                        QUEST_DIALOGUE_CATEGORY_ACTION;
                    questDialogueEditorEditableField =
                        QUEST_DIALOGUE_FIELD_ACTION_REFERENCE;
                    const std::string referenceValue =
                        questDialogueEditorReadEditableField();

                    questDialogueEditorFieldCategory = previousCategory;
                    questDialogueEditorEditableField = previousField;

                    if ( !referenceValue.empty()
                        && dialogueEditorButton(
                            detailX1 + 8, detailY,
                            detailX2 - detailX1 - 16,
                            ("ID / REFERENCE: "
                                + dialogueEditorClippedText(
                                    referenceValue,
                                    detailX2 - detailX1 - 132
                                )
                            ).c_str()
                        ) )
                    {
                        questDialogueEditorFieldCategory =
                            QUEST_DIALOGUE_CATEGORY_ACTION;
                        questDialogueEditorEditableField =
                            QUEST_DIALOGUE_FIELD_ACTION_REFERENCE;
                        questDialogueEditorBeginEditingField();
                    }
                    if ( !referenceValue.empty() )
                    {
                        detailY += 20;
                    }

                    questDialogueEditorFieldCategory =
                        QUEST_DIALOGUE_CATEGORY_ACTION;
                    questDialogueEditorEditableField =
                        QUEST_DIALOGUE_FIELD_ACTION_NUMBER;
                    const std::string value =
                        questDialogueEditorReadEditableField();

                    questDialogueEditorFieldCategory = previousCategory;
                    questDialogueEditorEditableField = previousField;

                    if ( !value.empty()
                        && dialogueEditorButton(
                            detailX1 + 8, detailY,
                            detailX2 - detailX1 - 16,
                            ("VALUE / COUNT: " + value).c_str()
                        ) )
                    {
                        questDialogueEditorFieldCategory =
                            QUEST_DIALOGUE_CATEGORY_ACTION;
                        questDialogueEditorEditableField =
                            QUEST_DIALOGUE_FIELD_ACTION_NUMBER;
                        questDialogueEditorBeginEditingField();
                    }
                    if ( !value.empty() )
                    {
                        detailY += 20;
                    }
                }

                if ( dialogueEditorButton(
                    detailX1 + 8,
                    detailY,
                    detailX2 - detailX1 - 16,
                    inspectCondition
                        ? "REMOVE CONDITION"
                        : "REMOVE ACTION"
                ) )
                {
                    deferredInspectorCommand =
                        inspectCondition
                            ? QUEST_DIALOGUE_DEFERRED_REMOVE_CONDITION
                            : QUEST_DIALOGUE_DEFERRED_REMOVE_ACTION;
                }
                detailY += 24;
            }

			printTextFormattedColor(
				font8x8_bmp,
				detailX1 + 8,
				detailY,
				makeColorRGB(128, 255, 160),
				"Applied actions:"
			);
			detailY += 12;

			const std::vector<std::string>
				appliedActionLines =
					questDialogueEditorAppliedActionSummary();

			for ( const std::string& line :
				appliedActionLines )
			{
				const std::string clippedAction =
					dialogueEditorClippedText(
						"- " + line,
						detailX2 - detailX1 - 24
					);

				printTextFormatted(
					font8x8_bmp,
					detailX1 + 12,
					detailY,
					"%s",
					clippedAction.c_str()
				);

				detailY += 12;

				if ( detailY > panelY2 - 72 )
				{
					printTextFormatted(
						font8x8_bmp,
						detailX1 + 12,
						detailY,
						"..."
					);
					detailY += 12;
					break;
				}
			}

			detailY += 6;

			if ( questDialogueEditorSelectedChoice
				< static_cast<int>(node.choices.size()) )
			{
				printTextFormattedColor(
					font8x8_bmp,
					detailX1 + 8,
					detailY,
					makeColorRGB(255, 230, 96),
					"Choice text:"
				);
				detailY += 12;

				const int choiceTextY = detailY;
				const bool editingChoiceText =
					questDialogueEditorEditingField
					&& questDialogueEditorEditableField
						== QUEST_DIALOGUE_FIELD_CHOICE_TEXT;

				const std::string choiceTextDisplay =
					editingChoiceText
						? std::string(questDialogueEditorEditBuffer) + "|"
						: node.choices[
							questDialogueEditorSelectedChoice
						];

				dialogueEditorDrawWrappedText(
					detailX1 + 12,
					detailY,
					detailX2 - detailX1 - 24,
					choiceTextDisplay,
					5,
					makeColorRGB(255, 230, 96)
				);

				if ( mousestatus[SDL_BUTTON_LEFT]
					&& omousex >= detailX1 + 8
					&& omousex < detailX2 - 8
					&& omousey >= choiceTextY - 2
					&& omousey < choiceTextY + 58 )
				{
					mousestatus[SDL_BUTTON_LEFT] = 0;
					questDialogueEditorEditChoiceTextDirect();
				}

				detailY += 6;
			}
		}
	}

	printTextFormattedColor(
		font8x8_bmp,
		detailX1 + 8,
		detailY,
		makeColorRGB(255, 230, 96),
		"Builder Categories"
	);
	detailY += 16;

	printTextFormattedColor(
		font8x8_bmp,
		detailX1 + 8,
		detailY,
		makeColorRGB(128, 255, 160),
		"Condition choices:"
	);
	detailY += 12;

	dialogueEditorDrawWrappedText(
		detailX1 + 12,
		detailY,
		detailX2 - detailX1 - 24,
		"Use COND < and COND >: None, Item, Gold, Quest Started/Accepted/Completed/Failed, Stage, Objective Complete/Incomplete, World/NPC Flag, World/NPC Variable",
		6,
		makeColorRGB(160, 255, 180)
	);
	detailY += 6;

	const char* categories[] =
	{
		"Item checks / consume",
		"Quest state / stage",
		"Objectives / counters",
		"Flags / variables",
		"Race / class checks",
		"Rewards / give items",
		"NPC memory",
		"Optional map markers"
	};

	for ( const char* category : categories )
	{
		if ( detailY + 12 >= panelY2 )
		{
			break;
		}

		printTextFormatted(
			font8x8_bmp,
			detailX1 + 12,
			detailY,
			"- %s",
			category
		);
		detailY += 14;
	}

	if ( deferredInspectorCommand
		!= QUEST_DIALOGUE_DEFERRED_NONE )
	{
		const int preservedNode = questDialogueEditorSelectedNode;
		const int preservedChoice = questDialogueEditorSelectedChoice;
		const int preservedObjective = questDialogueEditorSelectedObjective;
		const QuestDialogueInspectorSelection preservedInspector =
			questDialogueEditorInspectorSelection;

		switch ( deferredInspectorCommand )
		{
			case QUEST_DIALOGUE_DEFERRED_CONDITION_PREVIOUS:
				questDialogueEditorPreviousChoiceCondition();
				break;
			case QUEST_DIALOGUE_DEFERRED_CONDITION_NEXT:
				questDialogueEditorCycleChoiceCondition();
				break;
			case QUEST_DIALOGUE_DEFERRED_ACTION_PREVIOUS:
                questDialogueEditorCycleSelectedAction(-1);
                break;
            case QUEST_DIALOGUE_DEFERRED_ACTION_NEXT:
                questDialogueEditorCycleSelectedAction(1);
                break;
			case QUEST_DIALOGUE_DEFERRED_REMOVE_CONDITION:
				questDialogueEditorClearChoiceCondition();
				questDialogueEditorInspectorSelection =
					QUEST_DIALOGUE_INSPECTOR_NONE;
				break;
			case QUEST_DIALOGUE_DEFERRED_REMOVE_ACTION:
				questDialogueEditorClearChoiceAction();
				questDialogueEditorInspectorSelection =
					QUEST_DIALOGUE_INSPECTOR_NONE;
				break;
			default:
				break;
		}

		if ( !questDialogueEditorPreview.nodes.empty() )
		{
			questDialogueEditorSelectedNode = std::max(
				0,
				std::min(
					preservedNode,
					static_cast<int>(questDialogueEditorPreview.nodes.size()) - 1
				)
			);

			const auto& preservedNodePreview =
				questDialogueEditorPreview.nodes[questDialogueEditorSelectedNode];
			if ( !preservedNodePreview.choices.empty() )
			{
				questDialogueEditorSelectedChoice = std::max(
					0,
					std::min(
						preservedChoice,
						static_cast<int>(preservedNodePreview.choices.size()) - 1
					)
				);
			}
			else
			{
				questDialogueEditorSelectedChoice = -1;
			}
		}

		if ( questDialogueEditorPreview.objectiveCount > 0 )
		{
			questDialogueEditorSelectedObjective = std::max(
				0,
				std::min(
					preservedObjective,
					questDialogueEditorPreview.objectiveCount - 1
				)
			);
		}

		if ( deferredInspectorCommand
			!= QUEST_DIALOGUE_DEFERRED_REMOVE_CONDITION
			&& deferredInspectorCommand
				!= QUEST_DIALOGUE_DEFERRED_REMOVE_ACTION )
		{
			questDialogueEditorInspectorSelection = preservedInspector;
		}
	}

	if ( !questDialogueEditorMessage.empty()
		&& ticks < questDialogueEditorMessageUntil )
	{
		printTextFormattedColor(
			font8x8_bmp,
			subx1 + 10,
			suby2 - 22,
			makeColorRGB(128, 255, 160),
			"%s",
			questDialogueEditorMessage.c_str()
		);
	}
	else if ( !hoveredDialogueEditorTooltip.empty() )
	{
		const std::string footerTooltip =
			dialogueEditorClippedText(
				hoveredDialogueEditorTooltip,
				subx2 - subx1 - 20
			);

		printTextFormattedColor(
			font8x8_bmp,
			subx1 + 10,
			suby2 - 22,
			makeColorRGB(255, 230, 96),
			"%s",
			footerTooltip.c_str()
		);
	}
	else
	{
		printTextFormattedColor(
			font8x8_bmp,
			subx1 + 10,
			suby2 - 22,
			makeColorRGB(192, 192, 192),
			"Hover over any button for a guided explanation."
		);
	}
	}

static bool questDialogueEditorImmediateButton(
	const int x, const int y, const int width, const char* label,
	const bool selected = false
)
{
	const int height = 18;
	if ( selected ) drawDepressed(x, y, x + width, y + height);
	else drawWindowFancy(x, y, x + width, y + height);
	printTextFormatted(font8x8_bmp, x + 5, y + 5, "%s", label);
	const bool hovered = omousex >= x && omousex < x + width
		&& omousey >= y && omousey < y + height;
	if ( hovered && mousestatus[SDL_BUTTON_LEFT] )
	{
		mousestatus[SDL_BUTTON_LEFT] = 0;
		const bool editControl = !strcmp(label, "APPLY")
			|| !strcmp(label, "CANCEL")
			|| !strcmp(label, "APPLY RENAME")
			|| !strcmp(label, "CANCEL RENAME");
		if ( questDialogueEditorEditingField && !editControl )
		{
			questDialogueEditorSetMessage(
				"Apply or cancel the active field before using another control.");
			return false;
		}
		return true;
	}
	return false;
}

static std::string questDialogueEditorClipText(
	const std::string& text, const int pixelWidth
)
{
	const int characters = std::max(0, pixelWidth / 8);
	if ( static_cast<int>(text.size()) <= characters ) return text;
	if ( characters <= 3 ) return text.substr(0, characters);
	return text.substr(0, characters - 3) + "...";
}

static std::string questDialogueEditorLower(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(),
		[](const unsigned char character)
			{ return static_cast<char>(std::tolower(character)); });
	return value;
}

static bool questDialogueEditorMatches(
	const std::string& value, const char* filter
)
{
	if ( !filter || !filter[0] ) return true;
	return questDialogueEditorLower(value).find(
		questDialogueEditorLower(filter)) != std::string::npos;
}

static void questDialogueEditorEndTransientTextInput()
{
	const bool ownsInput = inputstr == questDialogueEditorFileSearch
		|| inputstr == questDialogueEditorTutorialSearch
		|| inputstr == questDialogueEditorItemSearch
		|| inputstr == questDialogueEditorSandboxItem
		|| inputstr == questDialogueEditorSandboxSeedKey
		|| inputstr == questDialogueEditorSandboxSeedSubkey
		|| inputstr == questDialogueEditorEditBuffer;
	questDialogueEditorFileSearchEditing = false;
	questDialogueEditorTutorialSearchEditing = false;
	questDialogueEditorItemSearchEditing = false;
	questDialogueEditorSandboxItemEditing = false;
	questDialogueEditorSandboxSeedKeyEditing = false;
	questDialogueEditorSandboxSeedSubkeyEditing = false;
	questDialogueEditorEditingField = false;
	if ( ownsInput )
	{
		SDL_StopTextInput();
		inputstr = nullptr;
	}
}

static int questDialogueEditorDrawFileRail(
	const int left, const int top, const int bottom
)
{
	const int width = 226;
	const int right = left + width;
	drawDepressed(left, top, right, bottom);
	printText(font8x8_bmp, left + 7, top + 7, "DIALOGUE FILES");
	const int searchY = top + 23;
	drawDepressed(left + 6, searchY, right - 6, searchY + 19);
	printTextFormattedColor(font8x8_bmp, left + 10, searchY + 6,
		questDialogueEditorFileSearch[0] ? makeColorRGB(255, 255, 255)
			: makeColorRGB(144, 144, 144),
		"%.24s", questDialogueEditorFileSearch[0]
			? questDialogueEditorFileSearch : "Search files...");
	if ( mousestatus[SDL_BUTTON_LEFT]
		&& omousex >= left + 6 && omousex < right - 6
		&& omousey >= searchY && omousey < searchY + 19 )
	{
		mousestatus[SDL_BUTTON_LEFT] = 0;
		questDialogueEditorEndTransientTextInput();
		questDialogueEditorFileSearchEditing = true;
		inputstr = questDialogueEditorFileSearch;
		inputlen = static_cast<int>(sizeof(questDialogueEditorFileSearch) - 1);
		cursorflash = ticks;
		SDL_StartTextInput();
	}

	std::vector<int> matches;
	for ( int index = 0; index < static_cast<int>(questDialogueEditorFiles.size()); ++index )
	{
		std::string searchable = questDialogueEditorFiles[index];
		if ( index < static_cast<int>(questDialogueEditorFileSummaries.size()) )
		{
			const auto& summary = questDialogueEditorFileSummaries[index];
			searchable += " " + summary.dialogueID + " " + summary.questID
				+ " " + summary.title;
		}
		if ( index == questDialogueEditorSelectedFile )
			searchable += " " + questDialogueEditorPreview.questID
				+ " " + questDialogueEditorPreview.title;
		if ( questDialogueEditorMatches(searchable, questDialogueEditorFileSearch) )
			matches.push_back(index);
	}
	const int listTop = searchY + 27;
	const int listBottom = bottom - 30;
	const int rowHeight = 32;
	const int visible = std::max(1, (listBottom - listTop) / rowHeight);
	const int maximumScroll = std::max(0, static_cast<int>(matches.size()) - visible);
	if ( omousex >= left && omousex < right
		&& omousey >= listTop && omousey < listBottom && scroll != 0 )
	{
		questDialogueEditorFileScroll = std::max(0,
			std::min(maximumScroll, questDialogueEditorFileScroll + scroll));
		scroll = 0;
	}
	questDialogueEditorFileScroll = std::max(0,
		std::min(maximumScroll, questDialogueEditorFileScroll));
	for ( int row = 0; row < visible; ++row )
	{
		const int filteredIndex = questDialogueEditorFileScroll + row;
		if ( filteredIndex >= static_cast<int>(matches.size()) ) break;
		const int index = matches[filteredIndex];
		const int y = listTop + row * rowHeight;
		if ( index == questDialogueEditorSelectedFile )
			drawDepressed(left + 5, y, right - 5, y + rowHeight - 2);
		const std::string marker = index == questDialogueEditorSelectedFile
			&& questDialogueEditorModel.dirty() ? "* " : "  ";
		printTextFormatted(font8x8_bmp, left + 8, y + 5, "%s%s",
			marker.c_str(), questDialogueEditorClipText(
				questDialogueEditorFiles[index], width - 74).c_str());
		int errors = 0;
		int warnings = 0;
		std::string detail;
		if ( index < static_cast<int>(questDialogueEditorFileSummaries.size()) )
		{
			const auto& summary = questDialogueEditorFileSummaries[index];
			errors = summary.errors;
			warnings = summary.warnings;
			detail = summary.questID.empty() ? "Conversation only"
				: "Quest: " + summary.questID;
			if ( !summary.title.empty() ) detail += " - " + summary.title;
		}
		if ( index == questDialogueEditorSelectedFile )
		{
			errors = automatia::dialogue::countIssues(
				questDialogueEditorValidationIssues,
				automatia::dialogue::Severity::Error);
			warnings = automatia::dialogue::countIssues(
				questDialogueEditorValidationIssues,
				automatia::dialogue::Severity::Warning);
			detail = questDialogueEditorPreview.questID.empty() ? "Conversation only"
				: "Quest: " + questDialogueEditorPreview.questID;
			if ( !questDialogueEditorPreview.title.empty() )
				detail += " - " + questDialogueEditorPreview.title;
		}
		const std::string status = errors > 0 ? "E" + std::to_string(errors)
			: (warnings > 0 ? "W" + std::to_string(warnings) : "OK");
		printTextFormattedColor(font8x8_bmp, right - 37, y + 5,
			errors > 0 ? makeColorRGB(255, 110, 100)
				: (warnings > 0 ? makeColorRGB(255, 210, 96)
					: makeColorRGB(128, 255, 160)), "%s", status.c_str());
		printTextFormattedColor(font8x8_bmp, left + 12, y + 18,
			makeColorRGB(155, 175, 195), "%s",
			questDialogueEditorClipText(detail, width - 26).c_str());
		if ( mousestatus[SDL_BUTTON_LEFT]
			&& omousex >= left + 5 && omousex < right - 5
			&& omousey >= y && omousey < y + rowHeight - 2 )
		{
			mousestatus[SDL_BUTTON_LEFT] = 0;
			if ( index != questDialogueEditorSelectedFile )
				questDialogueEditorRequestTransition(
					QUEST_DIALOGUE_PENDING_SWITCH_FILE, index);
		}
	}
	printTextFormattedColor(font8x8_bmp, left + 8, bottom - 20,
		makeColorRGB(160, 180, 200), "%d file%s%s",
		static_cast<int>(matches.size()), matches.size() == 1 ? "" : "s",
		questDialogueEditorModel.dirty() ? "  |  UNSAVED" : "");
	return right + 8;
}

static const char* questDialogueEditorWizardTemplateID()
{
	static const char* ids[] = {
		"empty_conversation", "one_choice_conversation", "two_choice_branch",
		"quest_giver", "recruitable_npc"
	};
	return ids[std::max(0, std::min(questDialogueEditorWizardTemplate, 4))];
}

static const char* questDialogueEditorWizardTemplateName()
{
	static const char* names[] = {
		"Empty Conversation", "One-Choice Conversation", "Two-Choice Branch",
		"Quest Giver", "Recruitable NPC"
	};
	return names[std::max(0, std::min(questDialogueEditorWizardTemplate, 4))];
}

static void questDialogueEditorFocusWizardField(const int field)
{
	questDialogueEditorEndTransientTextInput();
	questDialogueEditorWizardField = field;
	char* buffers[] = {
		questDialogueEditorWizardDialogueID,
		questDialogueEditorWizardQuestID,
		questDialogueEditorWizardNPCText,
		questDialogueEditorWizardChoiceText,
		questDialogueEditorWizardQuestTitle,
		questDialogueEditorWizardQuestSummary
	};
	const int lengths[] = {
		static_cast<int>(sizeof(questDialogueEditorWizardDialogueID) - 1),
		static_cast<int>(sizeof(questDialogueEditorWizardQuestID) - 1),
		static_cast<int>(sizeof(questDialogueEditorWizardNPCText) - 1),
		static_cast<int>(sizeof(questDialogueEditorWizardChoiceText) - 1),
		static_cast<int>(sizeof(questDialogueEditorWizardQuestTitle) - 1),
		static_cast<int>(sizeof(questDialogueEditorWizardQuestSummary) - 1)
	};
	questDialogueEditorWizardField = std::max(0, std::min(field, 5));
	inputstr = buffers[questDialogueEditorWizardField];
	inputlen = lengths[questDialogueEditorWizardField];
	cursorflash = ticks;
	SDL_StartTextInput();
}

static bool questDialogueEditorCreateWizardFile()
{
	const std::string dialogueID = questEditorNormalizeID(
		questDialogueEditorWizardDialogueID);
	if ( dialogueID.empty() )
	{
		questDialogueEditorSetMessage("Dialogue/File ID cannot be empty.");
		return false;
	}
	mkdir("./dialogue", 0755);
	const std::string filename = dialogueID + ".json";
	const std::string path = "./dialogue/" + filename;
	if ( access(path.c_str(), F_OK) == 0 )
	{
		questDialogueEditorSetMessage("That dialogue file already exists.");
		return false;
	}
	const bool useQuest = questDialogueEditorWizardUseQuest
		|| questDialogueEditorWizardTemplate == 3;
	const std::string questID = useQuest ? questEditorNormalizeID(
		questDialogueEditorWizardQuestID) : std::string{};
	if ( useQuest && questID.empty() )
	{
		questDialogueEditorSetMessage("A quest-enabled dialogue needs a Quest ID.");
		return false;
	}
	automatia::dialogue::Document created;
	std::string error;
	if ( !created.parse(automatia::dialogue::createStarterDocument(
			questDialogueEditorWizardTemplateID(), dialogueID, questID,
			questDialogueEditorWizardNPCText,
			questDialogueEditorWizardChoiceText,
			questDialogueEditorWizardQuestTitle,
			questDialogueEditorWizardQuestSummary), error) )
	{
		questDialogueEditorSetMessage(error);
		return false;
	}
	if ( useQuest && created.json().HasMember("quest")
		&& created.json()["quest"].IsObject() )
	{
		rapidjson::Value& quest = created.json()["quest"];
		auto& allocator = created.json().GetAllocator();
		if ( quest.HasMember("repeatable") )
			quest["repeatable"].SetBool(questDialogueEditorWizardRepeatable);
		else quest.AddMember("repeatable", questDialogueEditorWizardRepeatable, allocator);
		const char* scopeNames[] = { "player", "party", "world" };
		questDialogueEditorWizardScope = std::max(0,
			std::min(questDialogueEditorWizardScope, 2));
		if ( quest.HasMember("scope") && quest["scope"].IsString() )
		{
			quest["scope"].SetString(scopeNames[questDialogueEditorWizardScope], allocator);
		}
		else
		{
			rapidjson::Value scope;
			scope.SetString(scopeNames[questDialogueEditorWizardScope], allocator);
			quest.AddMember("scope", scope, allocator);
		}
		if ( questDialogueEditorWizardOrigin == 2 )
		{
			rapidjson::Value origin(rapidjson::kObjectType);
			rapidjson::Value label;
			label.SetString("Quest Giver", allocator);
			origin.AddMember("label", label, allocator);
			rapidjson::Value mapName;
			const std::string currentMap = questEditorCurrentMapFilename();
			mapName.SetString(currentMap.c_str(), allocator);
			origin.AddMember("map", mapName, allocator);
			if ( !selectedEntity[0] || selectedEntity[0]->persistentID <= 0 )
			{
				questDialogueEditorSetMessage(
					"Select a persistent NPC before choosing Follow Selected NPC.");
				return false;
			}
			origin.AddMember("track_npc", true, allocator);
			origin.AddMember("npc_persistent_id",
				selectedEntity[0]->persistentID, allocator);
			quest.AddMember("origin", origin, allocator);
		}
	}
	const auto issues = automatia::dialogue::validate(created.json(),
		questDialogueEditorValidationOptions());
	if ( automatia::dialogue::hasErrors(issues) )
	{
		const auto first = std::find_if(issues.begin(), issues.end(),
			[](const automatia::dialogue::Issue& issue)
				{ return issue.severity == automatia::dialogue::Severity::Error; });
		questDialogueEditorSetMessage(first == issues.end()
			? "Wizard validation failed."
			: "Wizard: " + first->location.path + " - " + first->message);
		return false;
	}
	if ( !created.saveAtomic(path, error) )
	{
		questDialogueEditorSetMessage(error);
		return false;
	}
	questDialogueEditorWizardOpen = false;
	SDL_StopTextInput();
	inputstr = nullptr;
	questDialogueEditorRefreshFiles();
	for ( int index = 0; index < static_cast<int>(questDialogueEditorFiles.size()); ++index )
	{
		if ( questDialogueEditorFiles[index] == filename )
		{
			questDialogueEditorSelectedFile = index;
			break;
		}
	}
	questDialogueEditorLoadPreview(filename);
	questDialogueEditorWorkspace = QUEST_DIALOGUE_WORKSPACE_CONVERSATION;
	questDialogueEditorSetMessage("Created " + filename + " from "
		+ questDialogueEditorWizardTemplateName() + ".");
	if ( useQuest && questDialogueEditorWizardOrigin == 1 )
	{
		questDialogueEditorWorkspace = QUEST_DIALOGUE_WORKSPACE_QUEST;
		questDialogueEditorQuestPanel = 2;
		questDialogueEditorBeginMarkerPick(QUEST_DIALOGUE_MARKER_PICK_ORIGIN);
	}
	return true;
}

static void questDialogueEditorEditField(
	const QuestDialogueEditableField field,
	const QuestDialogueFieldCategory category
)
{
	questDialogueEditorEditableField = field;
	questDialogueEditorFieldCategory = category;
	questDialogueEditorBeginEditingField();
}

static bool questDialogueEditorCycleNodeReference(
	rapidjson::Value* object,
	const char* member,
	const int direction
)
{
	if ( !object || !object->IsObject() || !member
		|| !questDialogueEditorDocument.HasMember("nodes")
		|| !questDialogueEditorDocument["nodes"].IsArray()
		|| questDialogueEditorDocument["nodes"].Empty() )
	{
		return false;
	}
	rapidjson::Value& nodes = questDialogueEditorDocument["nodes"];
	const int current = object->HasMember(member) && (*object)[member].IsInt()
		? (*object)[member].GetInt() : questDialogueEditorNodeIDAt(0);
	int index = 0;
	for ( rapidjson::SizeType i = 0; i < nodes.Size(); ++i )
	{
		if ( nodes[i].IsObject() && nodes[i].HasMember("id")
			&& nodes[i]["id"].IsInt() && nodes[i]["id"].GetInt() == current )
		{
			index = static_cast<int>(i);
			break;
		}
	}
	index = (index + direction + static_cast<int>(nodes.Size()))
		% static_cast<int>(nodes.Size());
	questDialogueEditorWriteIntegerMember(*object, member,
		questDialogueEditorNodeIDAt(index));
	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorSetSelectedStartNode()
{
	rapidjson::Value* node = questDialogueEditorSelectedNodeValue();
	if ( !node || !node->HasMember("id") || !(*node)["id"].IsInt() ) return false;
	questDialogueEditorWriteIntegerMember(questDialogueEditorDocument,
		"start_node", (*node)["id"].GetInt());
	questDialogueEditorSetMessage("Selected node is now the conversation start.");
	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorConvertLegacyToGraph()
{
	if ( !questDialogueEditorDocument.IsObject()
		|| !questDialogueEditorDocument.HasMember("text")
		|| !questDialogueEditorDocument["text"].IsString()
		|| questDialogueEditorDocument.HasMember("nodes") )
	{
		questDialogueEditorSetMessage(
			"The selected file is not a legacy one-line dialogue.");
		return false;
	}

	const std::string text = questDialogueEditorDocument["text"].GetString();
	auto& allocator = questDialogueEditorDocument.GetAllocator();
	rapidjson::Value nodes(rapidjson::kArrayType);
	rapidjson::Value node(rapidjson::kObjectType);
	node.AddMember("id", 0, allocator);
	rapidjson::Value nodeText;
	nodeText.SetString(text.c_str(), static_cast<rapidjson::SizeType>(text.size()),
		allocator);
	node.AddMember("text", nodeText, allocator);
	node.AddMember("next", 0, allocator);
	rapidjson::Value choices(rapidjson::kArrayType);
	node.AddMember("choices", choices, allocator);
	nodes.PushBack(node, allocator);
	questDialogueEditorDocument.AddMember("start_node", 0, allocator);
	questDialogueEditorDocument.AddMember("nodes", nodes, allocator);
	questDialogueEditorDocument.RemoveMember("text");
	questDialogueEditorSelectedNode = 0;
	questDialogueEditorSelectedChoice = -1;
	questDialogueEditorConversationInspector = 0;
	const bool changed = questDialogueEditorSaveDocument();
	if ( changed )
	{
		questDialogueEditorSetMessage(
			"Converted legacy text to an equivalent one-node graph; the change is unsaved.");
	}
	return changed;
}

static void questDialogueEditorJumpToNodeID(const int nodeID)
{
	if ( !questDialogueEditorDocument.HasMember("nodes")
		|| !questDialogueEditorDocument["nodes"].IsArray() ) return;
	for ( rapidjson::SizeType index = 0;
		index < questDialogueEditorDocument["nodes"].Size(); ++index )
	{
		const rapidjson::Value& node = questDialogueEditorDocument["nodes"][index];
		if ( node.IsObject() && node.HasMember("id") && node["id"].IsInt()
			&& node["id"].GetInt() == nodeID )
		{
			questDialogueEditorSelectedNode = static_cast<int>(index);
			questDialogueEditorSelectedChoice = -1;
			questDialogueEditorConversationInspector = 0;
			questDialogueEditorSetMessage("Jumped to node " + std::to_string(nodeID) + ".");
			return;
		}
	}
	questDialogueEditorSetMessage("That destination node does not exist.");
}

static void questDialogueEditorFindSelectedNodeReferences()
{
	rapidjson::Value* selected = questDialogueEditorSelectedNodeValue();
	if ( !selected || !selected->HasMember("id") || !(*selected)["id"].IsInt() ) return;
	const int target = (*selected)["id"].GetInt();
	int references = 0;
	if ( questDialogueEditorDocument.HasMember("start_node")
		&& questDialogueEditorDocument["start_node"].IsInt()
		&& questDialogueEditorDocument["start_node"].GetInt() == target ) ++references;
	for ( const rapidjson::Value& node : questDialogueEditorDocument["nodes"].GetArray() )
	{
		if ( !node.IsObject() ) continue;
		if ( node.HasMember("next") && node["next"].IsInt()
			&& node["next"].GetInt() == target ) ++references;
		if ( node.HasMember("condition") && node["condition"].IsObject() )
			for ( const char* branch : { "true_node", "false_node" } )
				if ( node["condition"].HasMember(branch)
					&& node["condition"][branch].IsInt()
					&& node["condition"][branch].GetInt() == target ) ++references;
		if ( node.HasMember("choices") && node["choices"].IsArray() )
			for ( const rapidjson::Value& choice : node["choices"].GetArray() )
				if ( choice.IsObject() && choice.HasMember("next")
					&& choice["next"].IsInt() && choice["next"].GetInt() == target ) ++references;
	}
	questDialogueEditorSetMessage("Node " + std::to_string(target) + " has "
		+ std::to_string(references) + " incoming/start reference"
		+ (references == 1 ? "." : "s."));
}

static int questDialogueEditorChoiceConditionCount(const rapidjson::Value& choice)
{
	if ( !choice.IsObject() ) return 0;
	int count = choice.HasMember("condition") && choice["condition"].IsObject() ? 1 : 0;
	if ( choice.HasMember("conditions") && choice["conditions"].IsArray() )
		count += static_cast<int>(choice["conditions"].Size());
	return count;
}

static bool questDialogueEditorDrawEditableRow(
	const int left,
	const int right,
	int& y,
	const char* label,
	const QuestDialogueEditableField field,
	const QuestDialogueFieldCategory category,
	const bool nodeRule = false
)
{
	const bool active = questDialogueEditorEditingField
		&& questDialogueEditorLockedEditableField == field
		&& questDialogueEditorLockedRuleOwnerNode == nodeRule;
	const QuestDialogueEditableField savedField = questDialogueEditorEditableField;
	const QuestDialogueFieldCategory savedCategory = questDialogueEditorFieldCategory;
	const bool savedOwner = questDialogueEditorRuleOwnerNode;
	questDialogueEditorEditableField = field;
	questDialogueEditorFieldCategory = category;
	questDialogueEditorRuleOwnerNode = nodeRule;
	const std::string value = active ? questDialogueEditorEditBuffer
		: questDialogueEditorReadEditableField();
	questDialogueEditorEditableField = savedField;
	questDialogueEditorFieldCategory = savedCategory;
	questDialogueEditorRuleOwnerNode = savedOwner;

	printTextFormatted(font8x8_bmp, left, y + 5, "%-17s %s%s", label,
		questDialogueEditorClipText(value.empty() ? "(not set)" : value,
			right - left - 142).c_str(), active ? "_" : "");
	bool applied = false;
	if ( questDialogueEditorImmediateButton(active ? right - 118 : right - 72,
		y, active ? 54 : 72, active ? "APPLY" : "EDIT") )
	{
		questDialogueEditorEditableField = field;
		questDialogueEditorFieldCategory = category;
		questDialogueEditorRuleOwnerNode = nodeRule;
		if ( active ) applied = questDialogueEditorApplyEditableField();
		else questDialogueEditorBeginEditingField();
	}
	if ( active && questDialogueEditorImmediateButton(right - 58, y, 58, "CANCEL") )
	{
		questDialogueEditorEndTransientTextInput();
		questDialogueEditorSetMessage("Field edit canceled; document was unchanged.");
	}
	y += 23;
	return applied;
}

static bool questDialogueEditorSelectVanillaItem(
	const int itemID,
	const bool conditionTarget
)
{
	if ( itemID < 0 || itemID >= NUMITEMS ) return false;
	rapidjson::Value* reference = nullptr;
	if ( conditionTarget )
	{
		reference = questDialogueEditorSelectedRuleCondition();
		if ( !reference || !reference->HasMember("type")
			|| !(*reference)["type"].IsString()
			|| questEditorNormalizeID((*reference)["type"].GetString())
				!= "has_item" ) return false;
	}
	else
	{
		rapidjson::Value* action = questDialogueEditorSelectedRuleAction();
		const std::string member = questDialogueEditorSelectedRuleActionMember();
		if ( !action || (member != "reward_item" && member != "remove_item")
			|| !action->HasMember(member.c_str())
			|| !(*action)[member.c_str()].IsObject() ) return false;
		reference = &(*action)[member.c_str()];
	}
	questDialogueEditorWriteStringMember(*reference, "item", std::to_string(itemID));
	reference->RemoveMember("stable_id");
	questDialogueEditorItemSearchEditing = false;
	if ( inputstr == questDialogueEditorItemSearch )
	{
		SDL_StopTextInput();
		inputstr = nullptr;
	}
	questDialogueEditorSelectedItemID = itemID;
	questDialogueEditorSetMessage(std::string("Vanilla item: ")
		+ items[itemID].getIdentifiedName() + " [" + std::to_string(itemID) + "]");
	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorSelectStableItem(
	const std::string& stableID,
	const bool conditionTarget
)
{
	if ( stableID.empty() || !editorSAMItemStableIDIsAvailable(stableID.c_str()) )
		return false;
	rapidjson::Value* reference = nullptr;
	if ( conditionTarget )
	{
		reference = questDialogueEditorSelectedRuleCondition();
		if ( !reference || !reference->HasMember("type")
			|| !(*reference)["type"].IsString()
			|| questEditorNormalizeID((*reference)["type"].GetString())
				!= "has_item" ) return false;
	}
	else
	{
		rapidjson::Value* action = questDialogueEditorSelectedRuleAction();
		const std::string member = questDialogueEditorSelectedRuleActionMember();
		if ( !action || (member != "reward_item" && member != "remove_item")
			|| !action->HasMember(member.c_str())
			|| !(*action)[member.c_str()].IsObject() ) return false;
		reference = &(*action)[member.c_str()];
	}
	questDialogueEditorWriteStringMember(*reference, "stable_id", stableID);
	reference->RemoveMember("item");
	questDialogueEditorItemSearchEditing = false;
	if ( inputstr == questDialogueEditorItemSearch )
	{
		SDL_StopTextInput();
		inputstr = nullptr;
	}
	questDialogueEditorSetMessage("S.A.M. stable item: " + stableID);
	return questDialogueEditorSaveDocument();
}

static void questDialogueEditorDrawItemPicker(
	const int left,
	const int right,
	int& y,
	const bool conditionTarget
)
{
	printText(font8x8_bmp, left, y + 5, "Search item catalog");
	drawDepressed(left + 142, y, right - 34, y + 19);
	printTextFormattedColor(font8x8_bmp, left + 148, y + 6,
		questDialogueEditorItemSearch[0] ? makeColorRGB(255, 255, 255)
			: makeColorRGB(145, 145, 145), "%.22s%s",
		questDialogueEditorItemSearch[0] ? questDialogueEditorItemSearch : "name or numeric ID",
		questDialogueEditorItemSearchEditing ? "_" : "");
	if ( mousestatus[SDL_BUTTON_LEFT] && omousex >= left + 142
		&& omousex < right - 34 && omousey >= y && omousey < y + 19 )
	{
		mousestatus[SDL_BUTTON_LEFT] = 0;
		questDialogueEditorEndTransientTextInput();
		questDialogueEditorItemSearchEditing = true;
		inputstr = questDialogueEditorItemSearch;
		inputlen = static_cast<int>(sizeof(questDialogueEditorItemSearch) - 1);
		cursorflash = ticks;
		SDL_StartTextInput();
	}
	if ( questDialogueEditorImmediateButton(right - 28, y, 28, "X") )
	{
		questDialogueEditorItemSearch[0] = '\0';
		questDialogueEditorItemSearchScroll = 0;
	}
	y += 23;
	struct ItemMatch
	{
		int vanillaID = -1;
		int stableIndex = -1;
	};
	std::vector<ItemMatch> matches;
	for ( int itemID = 0; itemID < NUMITEMS; ++itemID )
	{
		const std::string searchable = std::string(items[itemID].getIdentifiedName())
			+ " " + std::to_string(itemID);
		if ( questDialogueEditorMatches(searchable, questDialogueEditorItemSearch) )
			matches.push_back(ItemMatch{ itemID, -1 });
	}
	for ( int index = 0; index < editorSAMItemCatalogCount(); ++index )
	{
		const std::string stableID = editorSAMItemCatalogStableIDAt(index);
		const std::string name = editorSAMItemCatalogNameAt(index);
		if ( questDialogueEditorMatches(name + " " + stableID,
			questDialogueEditorItemSearch) )
		{
			matches.push_back(ItemMatch{ -1, index });
		}
	}
	const int visible = std::min(3, static_cast<int>(matches.size()));
	const int maximumScroll = std::max(0, static_cast<int>(matches.size()) - visible);
	if ( omousex >= left && omousex < right && omousey >= y
		&& omousey < y + visible * 19 && scroll != 0 )
	{
		questDialogueEditorItemSearchScroll = std::max(0,
			std::min(maximumScroll, questDialogueEditorItemSearchScroll + scroll));
		scroll = 0;
	}
	questDialogueEditorItemSearchScroll = std::max(0,
		std::min(maximumScroll, questDialogueEditorItemSearchScroll));
	for ( int row = 0; row < visible; ++row )
	{
		const ItemMatch& match = matches[questDialogueEditorItemSearchScroll + row];
		const std::string stableID = match.stableIndex >= 0
			? editorSAMItemCatalogStableIDAt(match.stableIndex) : std::string{};
		const std::string label = questDialogueEditorClipText(match.vanillaID >= 0
			? std::string(items[match.vanillaID].getIdentifiedName()) + "  [ID "
				+ std::to_string(match.vanillaID) + "]"
			: std::string(editorSAMItemCatalogNameAt(match.stableIndex))
				+ "  [" + stableID + "]", right - left - 12);
		if ( questDialogueEditorImmediateButton(left, y, right - left, label.c_str()) )
		{
			if ( match.vanillaID >= 0 )
				questDialogueEditorSelectVanillaItem(match.vanillaID, conditionTarget);
			else questDialogueEditorSelectStableItem(stableID, conditionTarget);
		}
		y += 20;
	}
	if ( matches.empty() )
	{
		printTextFormattedColor(font8x8_bmp, left, y,
			makeColorRGB(255, 175, 96), "No vanilla or S.A.M. item matches.");
		y += 18;
	}
}

static const char* questDialogueEditorConditionGroupName()
{
	switch ( questDialogueEditorConditionGroup )
	{
		case QUEST_DIALOGUE_CONDITION_GROUP_ITEMS: return "Items / Gold";
		case QUEST_DIALOGUE_CONDITION_GROUP_QUEST: return "Quest / History";
		case QUEST_DIALOGUE_CONDITION_GROUP_OBJECTIVES: return "Objectives";
		case QUEST_DIALOGUE_CONDITION_GROUP_FLAGS: return "Flags";
		case QUEST_DIALOGUE_CONDITION_GROUP_VARIABLES: return "Variables";
		default: return "Items / Gold";
	}
}

static bool questDialogueEditorConditionInCurrentGroup(const std::string& type)
{
	switch ( questDialogueEditorConditionGroup )
	{
		case QUEST_DIALOGUE_CONDITION_GROUP_ITEMS:
			return type == "has_item" || type == "has_gold";
		case QUEST_DIALOGUE_CONDITION_GROUP_QUEST:
			return type == "quest_started" || type == "quest_accepted"
				|| type == "quest_completed" || type == "quest_failed"
				|| type == "quest_stage" || type == "node_seen";
		case QUEST_DIALOGUE_CONDITION_GROUP_OBJECTIVES:
			return type == "objective_completed" || type == "objective_incomplete";
		case QUEST_DIALOGUE_CONDITION_GROUP_FLAGS:
			return type == "world_flag" || type == "npc_flag";
		case QUEST_DIALOGUE_CONDITION_GROUP_VARIABLES:
			return type == "world_variable" || type == "npc_variable";
		default:
			return false;
	}
}

static std::vector<std::string> questDialogueEditorConditionCatalog()
{
	std::vector<std::string> catalog;
	const auto& supported = questDialogueEditorRuleOwnerNode
		? automatia::dialogue::nodeConditionTypes()
		: automatia::dialogue::choiceConditionTypes();
	for ( const std::string& type : supported )
	{
		if ( questDialogueEditorConditionInCurrentGroup(type) )
			catalog.push_back(type);
	}
	return catalog;
}

static void questDialogueEditorCycleAvailableConditionGroup()
{
	for ( int attempt = 0; attempt < QUEST_DIALOGUE_CONDITION_GROUP_COUNT; ++attempt )
	{
		questDialogueEditorConditionGroup =
			static_cast<QuestDialogueConditionGroup>(
				(static_cast<int>(questDialogueEditorConditionGroup) + 1)
				% QUEST_DIALOGUE_CONDITION_GROUP_COUNT);
		if ( !questDialogueEditorConditionCatalog().empty() ) break;
	}
	questDialogueEditorConditionCatalogIndex = 0;
}

static bool questDialogueEditorActionInCurrentGroup(const std::string& field)
{
	switch ( questDialogueEditorActionGroup )
	{
		case QUEST_DIALOGUE_ACTION_GROUP_QUEST:
			return field.rfind("quest_", 0) == 0;
		case QUEST_DIALOGUE_ACTION_GROUP_REWARDS:
			return field.rfind("reward_", 0) == 0;
		case QUEST_DIALOGUE_ACTION_GROUP_COSTS:
			return field.rfind("remove_", 0) == 0;
		case QUEST_DIALOGUE_ACTION_GROUP_OBJECTIVES:
			return field.rfind("objective_", 0) == 0;
		case QUEST_DIALOGUE_ACTION_GROUP_FLAGS:
			return field == "set_world_flag" || field == "set_npc_flag";
		case QUEST_DIALOGUE_ACTION_GROUP_VARIABLES:
			return field.find("_variable") != std::string::npos;
		case QUEST_DIALOGUE_ACTION_GROUP_NPC:
			return field == "recruit_npc";
		case QUEST_DIALOGUE_ACTION_GROUP_MECHANISMS:
			return field == "set_power";
		case QUEST_DIALOGUE_ACTION_GROUP_STATUS:
			return field == "status_effect";
		default:
			return false;
	}
}

static std::vector<std::string> questDialogueEditorActionCatalog()
{
	std::vector<std::string> catalog;
	for ( const std::string& field : questDialogueEditorRuleOwnerNode
		? automatia::dialogue::nodeActionFields()
		: automatia::dialogue::choiceActionFields() )
	{
		if ( field != "id" && questDialogueEditorActionInCurrentGroup(field) )
			catalog.push_back(field);
	}
	return catalog;
}

static void questDialogueEditorCycleAvailableActionGroup()
{
	for ( int attempt = 0; attempt < QUEST_DIALOGUE_ACTION_GROUP_COUNT; ++attempt )
	{
		questDialogueEditorActionGroup = static_cast<QuestDialogueActionGroup>(
			(static_cast<int>(questDialogueEditorActionGroup) + 1)
			% QUEST_DIALOGUE_ACTION_GROUP_COUNT);
		if ( !questDialogueEditorActionCatalog().empty() ) break;
	}
	questDialogueEditorActionCatalogIndex = 0;
}

static void questDialogueEditorDrawConditionInspector(
	const int left,
	const int right,
	int& y,
	const int bottom
)
{
	if ( !questDialogueEditorRuleOwnerNode
		&& !questDialogueEditorSelectedChoiceValue() )
	{
		printText(font8x8_bmp, left, y, "Select a choice to author its requirements.");
		return;
	}
	if ( questDialogueEditorRuleOwnerNode && !questDialogueEditorSelectedNodeValue() )
	{
		printText(font8x8_bmp, left, y, "Select a node to author its redirect rule.");
		return;
	}

	std::vector<std::string> types = questDialogueEditorConditionCatalog();
	if ( types.empty() )
	{
		questDialogueEditorCycleAvailableConditionGroup();
		types = questDialogueEditorConditionCatalog();
	}
	questDialogueEditorConditionCatalogIndex = std::max(0,
		std::min(questDialogueEditorConditionCatalogIndex,
			static_cast<int>(types.size()) - 1));
	printTextFormattedColor(font8x8_bmp, left, y,
		makeColorRGB(128, 210, 255), "%s",
		questDialogueEditorRuleOwnerNode
			? "NODE REDIRECT RULE (one condition)"
			: "CHOICE REQUIREMENTS (all conditions are AND)");
	y += 19;
	if ( questDialogueEditorImmediateButton(left, y, 112,
		questDialogueEditorConditionGroupName()) )
	{
		questDialogueEditorCycleAvailableConditionGroup();
		types = questDialogueEditorConditionCatalog();
	}
	if ( questDialogueEditorImmediateButton(left + 118, y, 28, "<") )
		questDialogueEditorConditionCatalogIndex =
			(questDialogueEditorConditionCatalogIndex - 1
				+ static_cast<int>(types.size())) % static_cast<int>(types.size());
	drawDepressed(left + 152, y, right - 104, y + 18);
	printTextFormatted(font8x8_bmp, left + 158, y + 5, "Add/set: %s",
		types[questDialogueEditorConditionCatalogIndex].c_str());
	if ( questDialogueEditorImmediateButton(right - 98, y, 28, ">") )
		questDialogueEditorConditionCatalogIndex =
			(questDialogueEditorConditionCatalogIndex + 1) % static_cast<int>(types.size());
	if ( questDialogueEditorImmediateButton(right - 64, y, 64,
		questDialogueEditorRuleOwnerNode ? "SET" : "ADD") )
	{
		if ( questDialogueEditorRuleOwnerNode )
			questDialogueEditorReplaceRuleCondition(
				types[questDialogueEditorConditionCatalogIndex]);
		else questDialogueEditorAddChoiceCondition(
			types[questDialogueEditorConditionCatalogIndex]);
	}
	y += 27;

	rapidjson::Value* condition = questDialogueEditorSelectedRuleCondition();
	if ( !condition )
	{
		printTextFormattedColor(font8x8_bmp, left, y,
			makeColorRGB(180, 180, 180), "No condition. Choose a type above.");
		return;
	}
	if ( !questDialogueEditorRuleOwnerNode )
	{
		const int count = questDialogueEditorChoiceConditionCount(
			*questDialogueEditorSelectedChoiceValue());
		printTextFormatted(font8x8_bmp, left, y + 5, "Requirement %d of %d",
			count ? questDialogueEditorSelectedConditionIndex + 1 : 0, count);
		if ( questDialogueEditorImmediateButton(right - 84, y, 38, "PREV") )
			questDialogueEditorCycleSelectedCondition(-1);
		if ( questDialogueEditorImmediateButton(right - 40, y, 40, "NEXT") )
			questDialogueEditorCycleSelectedCondition(1);
		y += 24;
		condition = questDialogueEditorSelectedRuleCondition();
	}

	std::string type = condition && condition->HasMember("type")
		&& (*condition)["type"].IsString()
		? questEditorNormalizeID((*condition)["type"].GetString()) : "invalid";
	if ( questDialogueEditorImmediateButton(left, y, 70, "TYPE <") )
		questDialogueEditorCycleRuleConditionType(-1);
	if ( questDialogueEditorImmediateButton(left + 76, y, 70, "TYPE >") )
		questDialogueEditorCycleRuleConditionType(1);
	printTextFormatted(font8x8_bmp, left + 154, y + 5, "%s", type.c_str());
	y += 24;
	condition = questDialogueEditorSelectedRuleCondition();
	if ( !condition ) return;
	type = condition->HasMember("type") && (*condition)["type"].IsString()
		? questEditorNormalizeID((*condition)["type"].GetString()) : "invalid";
	const std::string summary = automatia::dialogue::conditionSummary(*condition);
	drawWindowFancy(left, y, right, y + 31);
	printTextFormattedColor(font8x8_bmp, left + 7, y + 7,
		makeColorRGB(128, 255, 160), "IF %s",
		questDialogueEditorClipText(summary, right - left - 20).c_str());
	y += 39;

	if ( type == "has_item" )
	{
		questDialogueEditorDrawEditableRow(left, right, y, "Vanilla item",
			QUEST_DIALOGUE_FIELD_CONDITION_REFERENCE,
			QUEST_DIALOGUE_CATEGORY_CONDITION, questDialogueEditorRuleOwnerNode);
		questDialogueEditorDrawEditableRow(left, right, y, "S.A.M. stable_id",
			QUEST_DIALOGUE_FIELD_CONDITION_STABLE_ID,
			QUEST_DIALOGUE_CATEGORY_CONDITION, questDialogueEditorRuleOwnerNode);
		questDialogueEditorDrawEditableRow(left, right, y, "Quantity",
			QUEST_DIALOGUE_FIELD_CONDITION_NUMBER,
			QUEST_DIALOGUE_CATEGORY_CONDITION, questDialogueEditorRuleOwnerNode);
		questDialogueEditorDrawItemPicker(left, right, y, true);
		printTextFormattedColor(font8x8_bmp, left, y,
			makeColorRGB(255, 210, 96),
			"Persistent custom identity uses stable_id; runtime numeric IDs are never stored.");
		y += 18;
	}
	else if ( type == "has_gold" )
	{
		questDialogueEditorDrawEditableRow(left, right, y, "Gold amount",
			QUEST_DIALOGUE_FIELD_CONDITION_NUMBER,
			QUEST_DIALOGUE_CATEGORY_CONDITION, questDialogueEditorRuleOwnerNode);
	}
	else if ( type == "quest_started" || type == "quest_accepted"
		|| type == "quest_completed" || type == "quest_failed"
		|| type == "quest_stage" )
	{
		questDialogueEditorDrawEditableRow(left, right, y, "Quest ID",
			QUEST_DIALOGUE_FIELD_CONDITION_REFERENCE,
			QUEST_DIALOGUE_CATEGORY_CONDITION, questDialogueEditorRuleOwnerNode);
		if ( type == "quest_stage" )
		{
			questDialogueEditorDrawEditableRow(left, right, y, "Stage",
				QUEST_DIALOGUE_FIELD_CONDITION_NUMBER,
				QUEST_DIALOGUE_CATEGORY_CONDITION, questDialogueEditorRuleOwnerNode);
			const std::string comparison = condition->HasMember("comparison")
				&& (*condition)["comparison"].IsString()
				? (*condition)["comparison"].GetString() : "equals";
			printTextFormatted(font8x8_bmp, left, y + 5,
				"Comparison        %s", comparison.c_str());
			if ( questDialogueEditorImmediateButton(right - 72, y, 72, "CHANGE") )
				questDialogueEditorCycleRuleComparison();
			y += 23;
		}
	}
	else if ( type == "objective_completed" || type == "objective_incomplete" )
	{
		questDialogueEditorDrawEditableRow(left, right, y, "Quest ID",
			QUEST_DIALOGUE_FIELD_CONDITION_QUEST,
			QUEST_DIALOGUE_CATEGORY_CONDITION, false);
		questDialogueEditorDrawEditableRow(left, right, y, "Objective ID",
			QUEST_DIALOGUE_FIELD_CONDITION_REFERENCE,
			QUEST_DIALOGUE_CATEGORY_CONDITION, false);
	}
	else if ( type == "node_seen" )
	{
		questDialogueEditorDrawEditableRow(left, right, y, "Seen-node key",
			QUEST_DIALOGUE_FIELD_CONDITION_REFERENCE,
			QUEST_DIALOGUE_CATEGORY_CONDITION, true);
		printText(font8x8_bmp, left, y, "Runtime keys are node_<numeric ID>, for example node_3.");
		y += 18;
	}
	else if ( type == "world_flag" || type == "npc_flag" )
	{
		questDialogueEditorDrawEditableRow(left, right, y, "Flag ID",
			QUEST_DIALOGUE_FIELD_CONDITION_REFERENCE,
			QUEST_DIALOGUE_CATEGORY_CONDITION, questDialogueEditorRuleOwnerNode);
		const bool expected = condition->HasMember("value")
			&& (*condition)["value"].IsBool() ? (*condition)["value"].GetBool() : true;
		printTextFormatted(font8x8_bmp, left, y + 5,
			"Required value     %s", expected ? "true" : "false");
		if ( questDialogueEditorImmediateButton(right - 72, y, 72, "TOGGLE") )
			questDialogueEditorToggleRuleConditionBoolean("value");
		y += 23;
	}
	else if ( type == "world_variable" || type == "npc_variable" )
	{
		questDialogueEditorDrawEditableRow(left, right, y, "Variable ID",
			QUEST_DIALOGUE_FIELD_CONDITION_REFERENCE,
			QUEST_DIALOGUE_CATEGORY_CONDITION, questDialogueEditorRuleOwnerNode);
		questDialogueEditorDrawEditableRow(left, right, y, "Compared value",
			QUEST_DIALOGUE_FIELD_CONDITION_NUMBER,
			QUEST_DIALOGUE_CATEGORY_CONDITION, questDialogueEditorRuleOwnerNode);
		const std::string comparison = condition->HasMember("comparison")
			&& (*condition)["comparison"].IsString()
			? (*condition)["comparison"].GetString() : "equals";
		printTextFormatted(font8x8_bmp, left, y + 5,
			"Comparison        %s", comparison.c_str());
		if ( questDialogueEditorImmediateButton(right - 72, y, 72, "CHANGE") )
			questDialogueEditorCycleRuleComparison();
		y += 23;
	}

	if ( questDialogueEditorRuleOwnerNode )
	{
		for ( const auto field : {
			QUEST_DIALOGUE_FIELD_CONDITION_TRUE_NODE,
			QUEST_DIALOGUE_FIELD_CONDITION_FALSE_NODE } )
		{
			const char* member = field == QUEST_DIALOGUE_FIELD_CONDITION_TRUE_NODE
				? "true_node" : "false_node";
			const char* label = field == QUEST_DIALOGUE_FIELD_CONDITION_TRUE_NODE
				? "If true -> node" : "If false -> node";
			printTextFormatted(font8x8_bmp, left, y + 5, "%-17s %d", label,
				condition->HasMember(member) && (*condition)[member].IsInt()
					? (*condition)[member].GetInt() : 0);
			if ( questDialogueEditorImmediateButton(right - 110, y, 32, "<") )
				questDialogueEditorCycleNodeReference(condition, member, -1);
			if ( questDialogueEditorImmediateButton(right - 72, y, 32, ">") )
				questDialogueEditorCycleNodeReference(condition, member, 1);
			if ( questDialogueEditorImmediateButton(right - 34, y, 34, "EDIT") )
			{
				questDialogueEditorRuleOwnerNode = true;
				questDialogueEditorEditField(field, QUEST_DIALOGUE_CATEGORY_CONDITION);
			}
			y += 23;
		}
		if ( type == "has_item" || type == "has_gold" )
		{
			const bool consume = condition->HasMember("consume")
				&& (*condition)["consume"].IsBool() && (*condition)["consume"].GetBool();
			printTextFormatted(font8x8_bmp, left, y + 5,
				"Consume on true  %s", consume ? "yes" : "no");
			if ( questDialogueEditorImmediateButton(right - 72, y, 72, "TOGGLE") )
				questDialogueEditorToggleRuleConditionBoolean("consume");
			y += 23;
		}
	}
	if ( y + 22 < bottom && questDialogueEditorImmediateButton(
		left, y, right - left, "REMOVE SELECTED CONDITION") )
		questDialogueEditorRemoveRuleCondition();
}

static void questDialogueEditorDrawActionInspector(
	const int left,
	const int right,
	int& y,
	const int bottom
)
{
	if ( !questDialogueEditorRuleOwnerNode
		&& !questDialogueEditorSelectedChoiceValue() )
	{
		printText(font8x8_bmp, left, y, "Select a choice to author its action stack.");
		return;
	}
	if ( questDialogueEditorRuleOwnerNode && !questDialogueEditorSelectedNodeValue() )
	{
		printText(font8x8_bmp, left, y, "Select a node to author its one-time action.");
		return;
	}

	std::vector<std::string> catalog = questDialogueEditorActionCatalog();
	if ( catalog.empty() )
	{
		questDialogueEditorCycleAvailableActionGroup();
		catalog = questDialogueEditorActionCatalog();
	}
	questDialogueEditorActionCatalogIndex = std::max(0,
		std::min(questDialogueEditorActionCatalogIndex,
			static_cast<int>(catalog.size()) - 1));
	printTextFormattedColor(font8x8_bmp, left, y,
		makeColorRGB(128, 210, 255), "%s",
		questDialogueEditorRuleOwnerNode
			? "NODE ACTIONS (run once per action ID)"
			: "CHOICE ACTIONS (all fields execute together)");
	y += 19;
	if ( questDialogueEditorImmediateButton(left, y, 112,
		questDialogueEditorActionGroupName()) )
	{
		questDialogueEditorCycleAvailableActionGroup();
		catalog = questDialogueEditorActionCatalog();
	}
	if ( questDialogueEditorImmediateButton(left + 118, y, 28, "<") )
		questDialogueEditorActionCatalogIndex =
			(questDialogueEditorActionCatalogIndex - 1
				+ static_cast<int>(catalog.size())) % static_cast<int>(catalog.size());
	drawDepressed(left + 152, y, right - 104, y + 18);
	printTextFormatted(font8x8_bmp, left + 158, y + 5, "Add: %s",
		questDialogueEditorActionDisplayName(
			catalog[questDialogueEditorActionCatalogIndex]).c_str());
	if ( questDialogueEditorImmediateButton(right - 98, y, 28, ">") )
		questDialogueEditorActionCatalogIndex =
			(questDialogueEditorActionCatalogIndex + 1) % static_cast<int>(catalog.size());
	if ( questDialogueEditorImmediateButton(right - 64, y, 64, "ADD") )
		questDialogueEditorAddRuleActionField(
			catalog[questDialogueEditorActionCatalogIndex]);
	y += 27;

	rapidjson::Value* action = questDialogueEditorSelectedRuleAction();
	if ( !action )
	{
		printTextFormattedColor(font8x8_bmp, left, y,
			makeColorRGB(180, 180, 180), "No actions. Choose one above.");
		return;
	}
	if ( questDialogueEditorRuleOwnerNode )
	{
		questDialogueEditorDrawEditableRow(left, right, y, "One-time ID",
			QUEST_DIALOGUE_FIELD_NODE_ACTION_ID,
			QUEST_DIALOGUE_CATEGORY_ACTION, true);
		printText(font8x8_bmp, left, y,
			"This stable ID prevents a node reward/action from repeating.");
		y += 18;
	}

	const auto members = questDialogueEditorRuleActionMembers();
	if ( members.empty() )
	{
		printText(font8x8_bmp, left, y, "No executable action fields.");
		return;
	}
	questDialogueEditorSelectedActionIndex = std::max(0,
		std::min(questDialogueEditorSelectedActionIndex,
			static_cast<int>(members.size()) - 1));
	printTextFormatted(font8x8_bmp, left, y + 5, "Action %d of %d",
		questDialogueEditorSelectedActionIndex + 1,
		static_cast<int>(members.size()));
	if ( questDialogueEditorImmediateButton(right - 84, y, 38, "PREV") )
		questDialogueEditorCycleSelectedRuleAction(-1);
	if ( questDialogueEditorImmediateButton(right - 40, y, 40, "NEXT") )
		questDialogueEditorCycleSelectedRuleAction(1);
	y += 24;
	const std::string member = questDialogueEditorSelectedRuleActionMember();
	const auto summaries = automatia::dialogue::actionSummaries(*action);
	int summaryIndex = 0;
	for ( auto iterator = action->MemberBegin(); iterator != action->MemberEnd();
		++iterator, ++summaryIndex )
	{
		if ( member == iterator->name.GetString() ) break;
	}
	const std::string summary = summaryIndex >= 0
		&& summaryIndex < static_cast<int>(summaries.size())
		? summaries[summaryIndex] : questDialogueEditorActionDisplayName(member);
	drawWindowFancy(left, y, right, y + 31);
	printTextFormattedColor(font8x8_bmp, left + 7, y + 7,
		makeColorRGB(255, 190, 110), "%s",
		questDialogueEditorClipText(summary, right - left - 20).c_str());
	y += 39;

	if ( member == "quest_start" || member == "quest_accept"
		|| member == "quest_complete" || member == "quest_fail"
		|| member == "quest_reset" || member == "recruit_npc" )
	{
		const bool enabled = action->HasMember(member.c_str())
			&& (*action)[member.c_str()].IsBool()
			&& (*action)[member.c_str()].GetBool();
		printTextFormatted(font8x8_bmp, left, y + 5,
			"Enabled           %s", enabled ? "true" : "false");
		if ( questDialogueEditorImmediateButton(right - 72, y, 72, "TOGGLE") )
			questDialogueEditorToggleSelectedRuleActionBoolean();
		y += 23;
	}
	else if ( member == "quest_stage" || member == "reward_gold"
		|| member == "remove_gold" )
	{
		questDialogueEditorDrawEditableRow(left, right, y,
			member == "quest_stage" ? "Quest stage" : "Gold amount",
			QUEST_DIALOGUE_FIELD_ACTION_NUMBER,
			QUEST_DIALOGUE_CATEGORY_ACTION, questDialogueEditorRuleOwnerNode);
	}
	else if ( member == "reward_item" || member == "remove_item" )
	{
		questDialogueEditorDrawEditableRow(left, right, y, "Vanilla item",
			QUEST_DIALOGUE_FIELD_ACTION_REFERENCE,
			QUEST_DIALOGUE_CATEGORY_ACTION, questDialogueEditorRuleOwnerNode);
		questDialogueEditorDrawEditableRow(left, right, y, "S.A.M. stable_id",
			QUEST_DIALOGUE_FIELD_ACTION_STABLE_ID,
			QUEST_DIALOGUE_CATEGORY_ACTION, questDialogueEditorRuleOwnerNode);
		questDialogueEditorDrawEditableRow(left, right, y, "Quantity",
			QUEST_DIALOGUE_FIELD_ACTION_NUMBER,
			QUEST_DIALOGUE_CATEGORY_ACTION, questDialogueEditorRuleOwnerNode);
		questDialogueEditorDrawItemPicker(left, right, y, false);
		printTextFormattedColor(font8x8_bmp, left, y,
			makeColorRGB(255, 210, 96),
			"Missing stable IDs remain authored; unavailable rewards are rejected atomically.");
		y += 18;
	}
	else if ( member == "objective_complete" || member == "objective_clear" )
	{
		questDialogueEditorDrawEditableRow(left, right, y, "Objective ID",
			QUEST_DIALOGUE_FIELD_ACTION_REFERENCE,
			QUEST_DIALOGUE_CATEGORY_ACTION, false);
	}
	else if ( member == "set_world_flag" || member == "set_npc_flag" )
	{
		questDialogueEditorDrawEditableRow(left, right, y, "Flag ID",
			QUEST_DIALOGUE_FIELD_ACTION_REFERENCE,
			QUEST_DIALOGUE_CATEGORY_ACTION, questDialogueEditorRuleOwnerNode);
		rapidjson::Value& selected = (*action)[member.c_str()];
		const bool enabled = selected.IsObject() && selected.HasMember("value")
			&& selected["value"].IsBool() && selected["value"].GetBool();
		printTextFormatted(font8x8_bmp, left, y + 5,
			"Set value         %s", enabled ? "true" : "false");
		if ( questDialogueEditorImmediateButton(right - 72, y, 72, "TOGGLE") )
			questDialogueEditorToggleSelectedRuleActionBoolean();
		y += 23;
	}
	else if ( member.find("_variable") != std::string::npos )
	{
		questDialogueEditorDrawEditableRow(left, right, y, "Variable ID",
			QUEST_DIALOGUE_FIELD_ACTION_REFERENCE,
			QUEST_DIALOGUE_CATEGORY_ACTION, questDialogueEditorRuleOwnerNode);
		questDialogueEditorDrawEditableRow(left, right, y,
			member.rfind("add_", 0) == 0 ? "Add amount" : "Set value",
			QUEST_DIALOGUE_FIELD_ACTION_NUMBER,
			QUEST_DIALOGUE_CATEGORY_ACTION, questDialogueEditorRuleOwnerNode);
	}
	else if ( member == "set_power" )
	{
		questDialogueEditorDrawEditableRow(left, right, y, "Tile X",
			QUEST_DIALOGUE_FIELD_POWER_X,
			QUEST_DIALOGUE_CATEGORY_ACTION, false);
		questDialogueEditorDrawEditableRow(left, right, y, "Tile Y",
			QUEST_DIALOGUE_FIELD_POWER_Y,
			QUEST_DIALOGUE_CATEGORY_ACTION, false);
		rapidjson::Value& selected = (*action)[member.c_str()];
		const bool powered = selected.IsObject() && selected.HasMember("powered")
			&& selected["powered"].IsBool() && selected["powered"].GetBool();
		printTextFormatted(font8x8_bmp, left, y + 5,
			"Power state       %s", powered ? "powered" : "unpowered");
		if ( questDialogueEditorImmediateButton(right - 72, y, 72, "TOGGLE") )
			questDialogueEditorToggleSelectedRuleActionBoolean();
		y += 23;
		if ( questDialogueEditorImmediateButton(left, y, right - left,
			"USE EDITOR CURSOR TILE") )
		{
			questDialogueEditorSetIntMember(selected, "x", std::max(0, drawx));
			questDialogueEditorSetIntMember(selected, "y", std::max(0, drawy));
			questDialogueEditorSaveDocument();
		}
		y += 23;
	}
	else if ( member == "status_effect" )
	{
		questDialogueEditorDrawEditableRow(left, right, y, "Effect ID",
			QUEST_DIALOGUE_FIELD_ACTION_NUMBER,
			QUEST_DIALOGUE_CATEGORY_ACTION, false);
		rapidjson::Value& selected = (*action)[member.c_str()];
		int effectID = selected.IsObject() && selected.HasMember("effect")
			&& selected["effect"].IsInt() ? selected["effect"].GetInt() : 0;
		printTextFormatted(font8x8_bmp, left, y + 5, "Effect name       %s",
			questDialogueEditorEffectName(effectID));
		if ( questDialogueEditorImmediateButton(right - 72, y, 32, "<") )
		{
			effectID = (effectID - 1 + NUMEFFECTS) % NUMEFFECTS;
			questDialogueEditorSetIntMember(selected, "effect", effectID);
			questDialogueEditorSaveDocument();
		}
		if ( questDialogueEditorImmediateButton(right - 34, y, 34, ">") )
		{
			effectID = (effectID + 1) % NUMEFFECTS;
			questDialogueEditorSetIntMember(selected, "effect", effectID);
			questDialogueEditorSaveDocument();
		}
		y += 23;
		questDialogueEditorDrawEditableRow(left, right, y, "Duration seconds",
			QUEST_DIALOGUE_FIELD_ACTION_SECONDARY_NUMBER,
			QUEST_DIALOGUE_CATEGORY_ACTION, false);
		questDialogueEditorDrawEditableRow(left, right, y, "Strength 1-255",
			QUEST_DIALOGUE_FIELD_ACTION_TERTIARY_NUMBER,
			QUEST_DIALOGUE_CATEGORY_ACTION, false);
		const bool enabled = selected.IsObject() && (!selected.HasMember("enabled")
			|| (selected["enabled"].IsBool() && selected["enabled"].GetBool()));
		printTextFormatted(font8x8_bmp, left, y + 5,
			"Operation         %s", enabled ? "apply" : "clear");
		if ( questDialogueEditorImmediateButton(right - 72, y, 72, "TOGGLE") )
			questDialogueEditorToggleSelectedRuleActionBoolean();
		y += 23;
	}

	if ( y + 22 < bottom && questDialogueEditorImmediateButton(
		left, y, right - left, "REMOVE SELECTED ACTION") )
		questDialogueEditorRemoveSelectedRuleAction();
}

static void questDialogueEditorDrawConversationPage()
{
	const int top = suby1 + 48;
	const int bottom = suby2 - 34;
	const int mainLeft = questDialogueEditorDrawFileRail(subx1 + 8, top, bottom);
	const int right = subx2 - 8;
	const int available = right - mainLeft;
	const int graphRight = mainLeft + std::max(304, available * 45 / 100);
	const int inspectorLeft = graphRight + 8;
	drawDepressed(mainLeft, top, graphRight, bottom);
	drawDepressed(inspectorLeft, top, right, bottom);

	printTextFormattedColor(font8x8_bmp, mainLeft + 9, top + 8,
		makeColorRGB(128, 210, 255), "CONVERSATION FLOW");
	const bool legacy = questDialogueEditorDocument.IsObject()
		&& questDialogueEditorDocument.HasMember("text")
		&& questDialogueEditorDocument["text"].IsString()
		&& !questDialogueEditorDocument.HasMember("nodes");
	if ( legacy )
	{
		printTextFormattedColor(font8x8_bmp, mainLeft + 14, top + 40,
			makeColorRGB(255, 230, 96), "LEGACY ONE-LINE DIALOGUE");
		printText(font8x8_bmp, mainLeft + 14, top + 60,
			"One terminal NPC line; no responses or automatic progression.");
		int legacyY = top + 92;
		questDialogueEditorDrawEditableRow(mainLeft + 14, graphRight - 14,
			legacyY, "NPC text", QUEST_DIALOGUE_FIELD_LEGACY_TEXT,
			QUEST_DIALOGUE_CATEGORY_TEXT);
		if ( questDialogueEditorImmediateButton(mainLeft + 14, legacyY + 10,
			graphRight - mainLeft - 28, "CONVERT TO EDITABLE NODE GRAPH") )
		{
			questDialogueEditorConvertLegacyToGraph();
		}
		printText(font8x8_bmp, inspectorLeft + 14, top + 42,
			"Conversion preserves the line as node 0, then enables nodes,");
		printText(font8x8_bmp, inspectorLeft + 14, top + 58,
			"responses, conditions, and actions. Undo restores legacy format.");
		return;
	}
	if ( questDialogueEditorImmediateButton(graphRight - 216, top + 4, 62, "+ NODE") )
		questDialogueEditorAddNode();
	if ( questDialogueEditorImmediateButton(graphRight - 148, top + 4, 64, "DUP NODE") )
		questDialogueEditorDuplicateSelectedNode();
	if ( questDialogueEditorImmediateButton(graphRight - 78, top + 4, 70, "DELETE") )
		questDialogueEditorDeleteNode();

	const int split = top + std::max(205, (bottom - top) * 48 / 100);
	const int nodeListTop = top + 31;
	const int nodeListBottom = split - 8;
	const int nodeRowHeight = 42;
	const int nodeVisible = std::max(1, (nodeListBottom - nodeListTop) / nodeRowHeight);
	const int nodeCount = static_cast<int>(questDialogueEditorPreview.nodes.size());
	const int nodeMaxScroll = std::max(0, nodeCount - nodeVisible);
	if ( omousex >= mainLeft && omousex < graphRight
		&& omousey >= nodeListTop && omousey < nodeListBottom && scroll != 0 )
	{
		questDialogueEditorConversationNodeScroll = std::max(0,
			std::min(nodeMaxScroll, questDialogueEditorConversationNodeScroll + scroll));
		scroll = 0;
	}
	questDialogueEditorConversationNodeScroll = std::max(0,
		std::min(nodeMaxScroll, questDialogueEditorConversationNodeScroll));
	const int startNode = questDialogueEditorDocument.IsObject()
		&& questDialogueEditorDocument.HasMember("start_node")
		&& questDialogueEditorDocument["start_node"].IsInt()
		? questDialogueEditorDocument["start_node"].GetInt() : INT_MIN;
	for ( int row = 0; row < nodeVisible; ++row )
	{
		const int index = questDialogueEditorConversationNodeScroll + row;
		if ( index >= nodeCount ) break;
		const int y = nodeListTop + row * nodeRowHeight;
		const auto& previewNode = questDialogueEditorPreview.nodes[index];
		if ( index == questDialogueEditorSelectedNode )
			drawDepressed(mainLeft + 7, y, graphRight - 7, y + nodeRowHeight - 4);
		else drawWindowFancy(mainLeft + 7, y, graphRight - 7, y + nodeRowHeight - 4);
		printTextFormattedColor(font8x8_bmp, mainLeft + 14, y + 6,
			previewNode.id == startNode ? makeColorRGB(128, 255, 160)
				: makeColorRGB(255, 230, 96),
			"%s NODE %d", previewNode.id == startNode ? "START" : "     ", previewNode.id);
		printTextFormatted(font8x8_bmp, mainLeft + 14, y + 21, "%s",
			questDialogueEditorClipText(previewNode.text, graphRight - mainLeft - 116).c_str());
		printTextFormattedColor(font8x8_bmp, graphRight - 91, y + 21,
			makeColorRGB(150, 185, 220), "%d choice%s",
			static_cast<int>(previewNode.choices.size()),
			previewNode.choices.size() == 1 ? "" : "s");
		if ( mousestatus[SDL_BUTTON_LEFT] && omousex >= mainLeft + 7
			&& omousex < graphRight - 7 && omousey >= y
			&& omousey < y + nodeRowHeight - 4 )
		{
			mousestatus[SDL_BUTTON_LEFT] = 0;
			questDialogueEditorSelectedNode = index;
			questDialogueEditorSelectedChoice = -1;
			questDialogueEditorConversationInspector = 0;
			questDialogueEditorRuleOwnerNode = true;
		}
	}

	printTextFormattedColor(font8x8_bmp, mainLeft + 9, split,
		makeColorRGB(128, 210, 255), "RESPONSES ON SELECTED NODE");
	int buttonX = graphRight - 282;
	if ( questDialogueEditorImmediateButton(buttonX, split - 4, 48, "+ ADD") )
		questDialogueEditorAddChoice();
	buttonX += 52;
	if ( questDialogueEditorImmediateButton(buttonX, split - 4, 48, "DUP") )
		questDialogueEditorDuplicateSelectedChoice();
	buttonX += 52;
	if ( questDialogueEditorImmediateButton(buttonX, split - 4, 48, "DEL") )
		questDialogueEditorDeleteChoice();
	buttonX += 52;
	if ( questDialogueEditorImmediateButton(buttonX, split - 4, 48, "UP") )
		questDialogueEditorMoveSelectedChoice(-1);
	buttonX += 52;
	if ( questDialogueEditorImmediateButton(buttonX, split - 4, 48, "DOWN") )
		questDialogueEditorMoveSelectedChoice(1);

	const int choiceListTop = split + 24;
	const int choiceRowHeight = 40;
	rapidjson::Value* selectedNode = questDialogueEditorSelectedNodeValue();
	const int choiceCount = selectedNode && selectedNode->HasMember("choices")
		&& (*selectedNode)["choices"].IsArray()
		? static_cast<int>((*selectedNode)["choices"].Size()) : 0;
	const int choiceVisible = std::max(1, (bottom - choiceListTop - 6) / choiceRowHeight);
	const int choiceMaxScroll = std::max(0, choiceCount - choiceVisible);
	if ( omousex >= mainLeft && omousex < graphRight
		&& omousey >= choiceListTop && omousey < bottom && scroll != 0 )
	{
		questDialogueEditorConversationChoiceScroll = std::max(0,
			std::min(choiceMaxScroll, questDialogueEditorConversationChoiceScroll + scroll));
		scroll = 0;
	}
	questDialogueEditorConversationChoiceScroll = std::max(0,
		std::min(choiceMaxScroll, questDialogueEditorConversationChoiceScroll));
	for ( int row = 0; row < choiceVisible; ++row )
	{
		const int index = questDialogueEditorConversationChoiceScroll + row;
		if ( index >= choiceCount ) break;
		const int y = choiceListTop + row * choiceRowHeight;
		const rapidjson::Value& choice = (*selectedNode)["choices"]
			[static_cast<rapidjson::SizeType>(index)];
		if ( index == questDialogueEditorSelectedChoice )
			drawDepressed(mainLeft + 7, y, graphRight - 7, y + choiceRowHeight - 4);
		else drawWindowFancy(mainLeft + 7, y, graphRight - 7, y + choiceRowHeight - 4);
		const std::string id = choice.IsObject() && choice.HasMember("id")
			&& choice["id"].IsString() ? choice["id"].GetString() : "(invalid)";
		const std::string textValue = choice.IsObject() && choice.HasMember("text")
			&& choice["text"].IsString() ? choice["text"].GetString() : "(invalid)";
		const int destination = choice.IsObject() && choice.HasMember("next")
			&& choice["next"].IsInt() ? choice["next"].GetInt() : -1;
		printTextFormattedColor(font8x8_bmp, mainLeft + 14, y + 5,
			makeColorRGB(255, 230, 96), "%s -> NODE %d", id.c_str(), destination);
		printTextFormatted(font8x8_bmp, mainLeft + 14, y + 20, "%s",
			questDialogueEditorClipText(textValue, graphRight - mainLeft - 138).c_str());
		const int conditions = questDialogueEditorChoiceConditionCount(choice);
		const int actions = choice.IsObject() && choice.HasMember("action")
			&& choice["action"].IsObject() ? static_cast<int>(choice["action"].MemberCount()) : 0;
		printTextFormattedColor(font8x8_bmp, graphRight - 120, y + 20,
			makeColorRGB(150, 185, 220), "R%d / A%d", conditions, actions);
		if ( mousestatus[SDL_BUTTON_LEFT] && omousex >= mainLeft + 7
			&& omousex < graphRight - 7 && omousey >= y
			&& omousey < y + choiceRowHeight - 4 )
		{
			mousestatus[SDL_BUTTON_LEFT] = 0;
			questDialogueEditorSelectedChoice = index;
			questDialogueEditorConversationInspector = 1;
			questDialogueEditorRuleOwnerNode = false;
			questDialogueEditorSelectedConditionIndex = 0;
			questDialogueEditorSelectedActionIndex = 0;
		}
	}

	const char* inspectorTabs[] = { "NODE", "CHOICE", "REQUIREMENTS", "ACTIONS" };
	const int widths[] = { 52, 60, 104, 64 };
	int tabX = inspectorLeft + 8;
	for ( int tab = 0; tab < 4; ++tab )
	{
		if ( questDialogueEditorImmediateButton(tabX, top + 5, widths[tab],
			inspectorTabs[tab], questDialogueEditorConversationInspector == tab) )
		{
			questDialogueEditorConversationInspector = tab;
			if ( tab == 0 ) questDialogueEditorRuleOwnerNode = true;
			if ( tab == 1 ) questDialogueEditorRuleOwnerNode = false;
		}
		tabX += widths[tab] + 4;
	}
	int y = top + 34;
	const int fieldLeft = inspectorLeft + 12;
	const int fieldRight = right - 12;

	if ( questDialogueEditorConversationInspector == 0 )
	{
		questDialogueEditorRuleOwnerNode = true;
		if ( !selectedNode )
		{
			printText(font8x8_bmp, fieldLeft, y, "Select a node from the flow list.");
			return;
		}
		questDialogueEditorDrawEditableRow(fieldLeft, fieldRight, y, "Node ID",
			QUEST_DIALOGUE_FIELD_NODE_ID, QUEST_DIALOGUE_CATEGORY_TEXT);
		questDialogueEditorDrawEditableRow(fieldLeft, fieldRight, y, "NPC text",
			QUEST_DIALOGUE_FIELD_NODE_TEXT, QUEST_DIALOGUE_CATEGORY_TEXT);
		questDialogueEditorDrawEditableRow(fieldLeft, fieldRight, y, "Automatic next",
			QUEST_DIALOGUE_FIELD_NODE_NEXT, QUEST_DIALOGUE_CATEGORY_TEXT);
		if ( questDialogueEditorImmediateButton(fieldLeft, y, 72, "NEXT <") )
			questDialogueEditorCycleNodeReference(selectedNode, "next", -1);
		if ( questDialogueEditorImmediateButton(fieldLeft + 78, y, 72, "NEXT >") )
			questDialogueEditorCycleNodeReference(selectedNode, "next", 1);
		if ( questDialogueEditorImmediateButton(fieldLeft + 156, y, 92, "JUMP TO NEXT") )
		{
			const int destination = selectedNode->HasMember("next")
				&& (*selectedNode)["next"].IsInt() ? (*selectedNode)["next"].GetInt() : -1;
			questDialogueEditorJumpToNodeID(destination);
		}
		y += 25;
		if ( questDialogueEditorImmediateButton(fieldLeft, y, 112, "SET AS START") )
			questDialogueEditorSetSelectedStartNode();
		if ( questDialogueEditorImmediateButton(fieldLeft + 120, y, 128, "FIND REFERENCES") )
			questDialogueEditorFindSelectedNodeReferences();
		y += 30;
		const bool hasCondition = selectedNode->HasMember("condition")
			&& (*selectedNode)["condition"].IsObject();
		const bool hasAction = selectedNode->HasMember("action")
			&& (*selectedNode)["action"].IsObject();
		printTextFormatted(font8x8_bmp, fieldLeft, y,
			"Redirect rule: %s | One-time action: %s",
			hasCondition ? "configured" : "none", hasAction ? "configured" : "none");
		y += 20;
		if ( questDialogueEditorImmediateButton(fieldLeft, y, 146, "EDIT NODE RULE") )
		{
			questDialogueEditorRuleOwnerNode = true;
			questDialogueEditorConversationInspector = 2;
		}
		if ( questDialogueEditorImmediateButton(fieldLeft + 154, y, 154,
			"EDIT NODE ACTIONS") )
		{
			questDialogueEditorRuleOwnerNode = true;
			questDialogueEditorConversationInspector = 3;
		}
	}
	else if ( questDialogueEditorConversationInspector == 1 )
	{
		questDialogueEditorRuleOwnerNode = false;
		rapidjson::Value* choice = questDialogueEditorSelectedChoiceValue();
		if ( !choice )
		{
			printText(font8x8_bmp, fieldLeft, y, "Select a response from the flow list.");
			return;
		}
		questDialogueEditorDrawEditableRow(fieldLeft, fieldRight, y, "Choice ID",
			QUEST_DIALOGUE_FIELD_CHOICE_ID, QUEST_DIALOGUE_CATEGORY_TEXT);
		questDialogueEditorDrawEditableRow(fieldLeft, fieldRight, y, "Player response",
			QUEST_DIALOGUE_FIELD_CHOICE_TEXT, QUEST_DIALOGUE_CATEGORY_TEXT);
		questDialogueEditorDrawEditableRow(fieldLeft, fieldRight, y, "Destination",
			QUEST_DIALOGUE_FIELD_CHOICE_NEXT, QUEST_DIALOGUE_CATEGORY_TEXT);
		if ( questDialogueEditorImmediateButton(fieldLeft, y, 62, "DEST <") )
			questDialogueEditorCycleNodeReference(choice, "next", -1);
		if ( questDialogueEditorImmediateButton(fieldLeft + 68, y, 62, "DEST >") )
			questDialogueEditorCycleNodeReference(choice, "next", 1);
		if ( questDialogueEditorImmediateButton(fieldLeft + 136, y, 92, "JUMP TO DEST") )
		{
			const int destination = choice->HasMember("next") && (*choice)["next"].IsInt()
				? (*choice)["next"].GetInt() : -1;
			questDialogueEditorJumpToNodeID(destination);
		}
		y += 25;
		const bool once = choice->HasMember("once") && (*choice)["once"].IsBool()
			&& (*choice)["once"].GetBool();
		printTextFormatted(font8x8_bmp, fieldLeft, y + 5,
			"Once-only response %s", once ? "YES" : "NO");
		if ( questDialogueEditorImmediateButton(fieldRight - 72, y, 72, "TOGGLE") )
			questDialogueEditorToggleChoiceOnce();
		y += 27;
		if ( questDialogueEditorImmediateButton(fieldLeft, y, 146, "EDIT REQUIREMENTS") )
		{
			questDialogueEditorRuleOwnerNode = false;
			questDialogueEditorConversationInspector = 2;
		}
		if ( questDialogueEditorImmediateButton(fieldLeft + 154, y, 146,
			"EDIT ACTION STACK") )
		{
			questDialogueEditorRuleOwnerNode = false;
			questDialogueEditorConversationInspector = 3;
		}
	}
	else
	{
		if ( questDialogueEditorEditingField )
			questDialogueEditorRuleOwnerNode = questDialogueEditorLockedRuleOwnerNode;
		printText(font8x8_bmp, fieldLeft, y + 5, "Edit rules on:");
		if ( questDialogueEditorImmediateButton(fieldLeft + 104, y, 80, "NODE",
			questDialogueEditorRuleOwnerNode) ) questDialogueEditorRuleOwnerNode = true;
		if ( questDialogueEditorImmediateButton(fieldLeft + 190, y, 84, "CHOICE",
			!questDialogueEditorRuleOwnerNode) )
		{
			if ( questDialogueEditorSelectedChoiceValue() ) questDialogueEditorRuleOwnerNode = false;
			else questDialogueEditorSetMessage("Select a choice first.");
		}
		y += 29;
		if ( questDialogueEditorConversationInspector == 2 )
			questDialogueEditorDrawConditionInspector(fieldLeft, fieldRight, y, bottom - 8);
		else questDialogueEditorDrawActionInspector(fieldLeft, fieldRight, y, bottom - 8);
	}
}

static void questDialogueEditorDrawFilesPage()
{
	const int top = suby1 + 48;
	const int bottom = suby2 - 34;
	const int mainLeft = questDialogueEditorDrawFileRail(subx1 + 8, top, bottom);
	const int right = subx2 - 8;
	drawDepressed(mainLeft, top, right, bottom);
	printTextFormattedColor(font8x8_bmp, mainLeft + 12, top + 10,
		makeColorRGB(128, 210, 255), "FILES AND RESOURCE IDENTITY");
	printText(font8x8_bmp, mainLeft + 12, top + 27,
		"The filename selects the dialogue resource. quest_id is separate persistent story identity.");

	int x = mainLeft + 12;
	const int y = top + 52;
	if ( questDialogueEditorImmediateButton(x, y, 92, "NEW..." ) )
		questDialogueEditorCreateNewFile();
	x += 100;
	if ( questDialogueEditorImmediateButton(x, y, 92, "DUPLICATE") )
		questDialogueEditorDuplicateSelectedFile();
	x += 100;
	if ( questDialogueEditorImmediateButton(x, y, 92, "RELOAD") )
		questDialogueEditorRequestTransition(
			QUEST_DIALOGUE_PENDING_RELOAD, questDialogueEditorSelectedFile);
	x += 100;
	if ( questDialogueEditorImmediateButton(x, y, 92, "DELETE...") )
		questDialogueEditorDeleteSelectedFile();

	const std::string filename = questDialogueEditorSelectedFile >= 0
		&& questDialogueEditorSelectedFile < static_cast<int>(questDialogueEditorFiles.size())
		? questDialogueEditorFiles[questDialogueEditorSelectedFile] : "(none)";
	int detailY = top + 88;
	printTextFormatted(font8x8_bmp, mainLeft + 16, detailY,
		"Selected file: %s%s", filename.c_str(),
		questDialogueEditorModel.dirty() ? "  * unsaved" : "");
	detailY += 22;
	printTextFormatted(font8x8_bmp, mainLeft + 16, detailY,
		"Dialogue/File ID: %s", filename.size() > 5
			? filename.substr(0, filename.size() - 5).c_str() : filename.c_str());
	if ( questDialogueEditorImmediateButton(right - 126, detailY - 6, 110,
		questDialogueEditorEditingField
			&& questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_FILE_ID
			? "APPLY RENAME" : "RENAME...") )
	{
		if ( questDialogueEditorEditingField
			&& questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_FILE_ID )
			questDialogueEditorRenameSelectedFile();
		else questDialogueEditorEditField(
			QUEST_DIALOGUE_FIELD_FILE_ID, QUEST_DIALOGUE_CATEGORY_FILE_QUEST);
	}
	if ( questDialogueEditorEditingField
		&& questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_FILE_ID
		&& questDialogueEditorImmediateButton(right - 246, detailY - 6, 112,
			"CANCEL RENAME") )
	{
		questDialogueEditorEndTransientTextInput();
		questDialogueEditorSetMessage("Rename canceled; file was unchanged.");
	}
	detailY += 26;
	if ( questDialogueEditorEditingField
		&& questDialogueEditorEditableField == QUEST_DIALOGUE_FIELD_FILE_ID )
	{
		drawDepressed(mainLeft + 16, detailY, right - 16, detailY + 21);
		printTextFormatted(font8x8_bmp, mainLeft + 21, detailY + 6,
			"%s_", questDialogueEditorClipText(questDialogueEditorEditBuffer,
				right - mainLeft - 52).c_str());
		detailY += 34;
	}
	printTextFormatted(font8x8_bmp, mainLeft + 16, detailY,
		"Quest ID: %s", questDialogueEditorPreview.questID.empty()
			? "(no quest)" : questDialogueEditorPreview.questID.c_str());
	detailY += 18;
	printTextFormatted(font8x8_bmp, mainLeft + 16, detailY,
		"Graph: %d node%s, %d objective%s, %d validation issue%s",
		static_cast<int>(questDialogueEditorPreview.nodes.size()),
		questDialogueEditorPreview.nodes.size() == 1 ? "" : "s",
		questDialogueEditorPreview.objectiveCount,
		questDialogueEditorPreview.objectiveCount == 1 ? "" : "s",
		static_cast<int>(questDialogueEditorValidationIssues.size()),
		questDialogueEditorValidationIssues.size() == 1 ? "" : "s");
	detailY += 30;
	printTextFormattedColor(font8x8_bmp, mainLeft + 16, detailY,
		makeColorRGB(255, 230, 96), "SAFE FILE WORKFLOW");
	detailY += 18;
	const char* notes[] = {
		"- Edits stay in memory until SAVE.",
		"- SAVE validates, writes a sibling temporary file, then atomically replaces the JSON.",
		"- Switch, reload, close, tutorial replace, and delete prompt on unsaved changes.",
		"- Unknown extension fields and array order survive visual edits and saves.",
		"- Renaming changes the selected NPC only when it was bound to the old file ID."
	};
	for ( const char* note : notes )
	{
		printText(font8x8_bmp, mainLeft + 20, detailY, note);
		detailY += 17;
	}
}

static bool questDialogueEditorAddQuestMetadata()
{
	if ( !questDialogueEditorDocument.IsObject() ) return false;
	auto& allocator = questDialogueEditorDocument.GetAllocator();
	if ( !questDialogueEditorDocument.HasMember("quest_id") )
		questDialogueEditorSetStringMember(questDialogueEditorDocument,
			"quest_id", "new_quest");
	if ( !questDialogueEditorDocument.HasMember("quest") )
	{
		rapidjson::Value quest(rapidjson::kObjectType);
		questDialogueEditorSetStringMember(quest, "title", "New Quest");
		questDialogueEditorSetStringMember(quest, "summary", "");
		questDialogueEditorSetStringMember(quest, "scope", "player");
		questDialogueEditorSetBoolMember(quest, "repeatable", false);
		rapidjson::Value objectives(rapidjson::kArrayType);
		quest.AddMember("objectives", objectives, allocator);
		questDialogueEditorDocument.AddMember("quest", quest, allocator);
	}
	questDialogueEditorSetMessage("Quest metadata added. Set a unique Quest ID next.");
	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorClearObjectiveMarker()
{
	rapidjson::Value* objective = questDialogueEditorSelectedObjectiveValueForEdit();
	if ( !objective || !objective->HasMember("map_marker") ) return false;
	objective->RemoveMember("map_marker");
	questDialogueEditorSetMessage("Objective marker cleared.");
	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorSetObjectiveMarkerTile(
	const int tileX,
	const int tileY,
	const int playableFloor
)
{
	rapidjson::Value* objective = questDialogueEditorSelectedObjectiveValueForEdit();
	if ( !objective ) return false;
	if ( tileX < 0 || tileY < 0 || tileX >= static_cast<int>(map.width)
		|| tileY >= static_cast<int>(map.height) )
	{
		questDialogueEditorSetMessage("Selected tile is outside the map.");
		return false;
	}
	rapidjson::Value& marker = questDialogueEditorSetObjectMember(
		*objective, "map_marker");
	questDialogueEditorSetStringMember(marker, "map",
		questEditorCurrentMapFilename());
	questDialogueEditorSetIntMember(marker, "x", tileX);
	questDialogueEditorSetIntMember(marker, "y", tileY);
	questDialogueEditorSetIntMember(marker, "playable_floor",
		std::max(0, playableFloor));
	if ( !marker.HasMember("floor_visibility") )
	{
		questDialogueEditorSetStringMember(marker, "floor_visibility", "same_floor");
	}
	questDialogueEditorSetMessage("Objective marker set to tile "
		+ std::to_string(tileX) + ", " + std::to_string(tileY)
		+ " on playable floor " + std::to_string(std::max(0, playableFloor)) + ".");
	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorSetObjectiveMarkerFromCursor()
{
	return questDialogueEditorBeginMarkerPick(
		QUEST_DIALOGUE_MARKER_PICK_OBJECTIVE);
}

static int questDialogueEditorDefaultMarkerFloor()
{
	return map.playableFloors.hasFloor(static_cast<PlayableFloorId>(drawlayer))
		? drawlayer : DEFAULT_PLAYABLE_FLOOR;
}

static bool questDialogueEditorBeginManualOriginCoordinates()
{
	rapidjson::Value* quest = questDialogueEditorQuestValue();
	if ( !quest ) return false;
	if ( !quest->HasMember("origin") || !(*quest)["origin"].IsObject() )
	{
		if ( !questDialogueEditorSetQuestGiverTile(
				0, 0, questDialogueEditorDefaultMarkerFloor()) )
		{
			return false;
		}
	}
	questDialogueEditorEditableField = QUEST_DIALOGUE_FIELD_ORIGIN_X;
	questDialogueEditorFieldCategory = QUEST_DIALOGUE_CATEGORY_FILE_QUEST;
	questDialogueEditorBeginEditingField();
	questDialogueEditorSetMessage(
		"Enter tile X, press Apply, then edit tile Y and playable floor.");
	return true;
}

static bool questDialogueEditorBeginManualObjectiveCoordinates()
{
	rapidjson::Value* objective = questDialogueEditorSelectedObjectiveValueForEdit();
	if ( !objective ) return false;
	if ( !objective->HasMember("map_marker")
		|| !(*objective)["map_marker"].IsObject() )
	{
		if ( !questDialogueEditorSetObjectiveMarkerTile(
				0, 0, questDialogueEditorDefaultMarkerFloor()) )
		{
			return false;
		}
	}
	questDialogueEditorEditableField = QUEST_DIALOGUE_FIELD_MARKER_X;
	questDialogueEditorFieldCategory = QUEST_DIALOGUE_CATEGORY_MARKER;
	questDialogueEditorBeginEditingField();
	questDialogueEditorSetMessage(
		"Enter tile X, press Apply, then edit tile Y and playable floor.");
	return true;
}

static const char* questDialogueEditorMarkerVisibility(
	const rapidjson::Value& marker
)
{
	return marker.IsObject() && marker.HasMember("floor_visibility")
		&& marker["floor_visibility"].IsString()
		&& std::string(marker["floor_visibility"].GetString()) == "same_floor"
			? "same_floor" : "column";
}

static bool questDialogueEditorToggleOriginMarkerVisibility()
{
	rapidjson::Value* quest = questDialogueEditorQuestValue();
	if ( !quest || !quest->HasMember("origin")
		|| !(*quest)["origin"].IsObject() ) return false;
	rapidjson::Value& origin = (*quest)["origin"];
	const bool currentlySame = std::string(
		questDialogueEditorMarkerVisibility(origin)) == "same_floor";
	questDialogueEditorSetStringMember(origin, "floor_visibility",
		currentlySame ? "column" : "same_floor");
	if ( !currentlySame && (!origin.HasMember("playable_floor")
		|| !origin["playable_floor"].IsInt()) )
	{
		questDialogueEditorSetIntMember(origin, "playable_floor",
			questDialogueEditorDefaultMarkerFloor());
	}
	questDialogueEditorSetMessage(currentlySame
		? "Origin marker now appears throughout the vertical column."
		: "Origin marker now appears only on its selected playable floor.");
	return questDialogueEditorSaveDocument();
}

static bool questDialogueEditorToggleObjectiveMarkerVisibility()
{
	rapidjson::Value* objective = questDialogueEditorSelectedObjectiveValueForEdit();
	if ( !objective || !objective->HasMember("map_marker")
		|| !(*objective)["map_marker"].IsObject() ) return false;
	rapidjson::Value& marker = (*objective)["map_marker"];
	const bool currentlySame = std::string(
		questDialogueEditorMarkerVisibility(marker)) == "same_floor";
	questDialogueEditorSetStringMember(marker, "floor_visibility",
		currentlySame ? "column" : "same_floor");
	if ( !currentlySame && (!marker.HasMember("playable_floor")
		|| !marker["playable_floor"].IsInt()) )
	{
		questDialogueEditorSetIntMember(marker, "playable_floor",
			questDialogueEditorDefaultMarkerFloor());
	}
	questDialogueEditorSetMessage(currentlySame
		? "Objective marker now appears throughout the vertical column."
		: "Objective marker now appears only on its selected playable floor.");
	return questDialogueEditorSaveDocument();
}

static void questDialogueEditorDrawQuestPage()
{
	const int top = suby1 + 48;
	const int bottom = suby2 - 34;
	const int mainLeft = questDialogueEditorDrawFileRail(subx1 + 8, top, bottom);
	const int right = subx2 - 8;
	drawDepressed(mainLeft, top, right, bottom);
	const char* tabs[] = { "OVERVIEW", "OBJECTIVES", "QUEST GIVER" };
	const int widths[] = { 82, 92, 96 };
	int tabX = mainLeft + 10;
	for ( int tab = 0; tab < 3; ++tab )
	{
		if ( questDialogueEditorImmediateButton(tabX, top + 5, widths[tab], tabs[tab],
			questDialogueEditorQuestPanel == tab) ) questDialogueEditorQuestPanel = tab;
		tabX += widths[tab] + 5;
	}

	rapidjson::Value* quest = questDialogueEditorQuestValue();
	if ( !quest )
	{
		printTextFormattedColor(font8x8_bmp, mainLeft + 18, top + 48,
			makeColorRGB(255, 210, 96), "THIS DIALOGUE HAS NO QUEST METADATA");
		printText(font8x8_bmp, mainLeft + 18, top + 70,
			"Conversation-only files are valid. Add quest metadata only when this dialogue owns a quest.");
		if ( questDialogueEditorImmediateButton(mainLeft + 18, top + 96, 154,
			"ADD QUEST METADATA") ) questDialogueEditorAddQuestMetadata();
		return;
	}

	const int left = mainLeft + 16;
	const int fieldRight = right - 16;
	int y = top + 39;
	if ( questDialogueEditorQuestPanel == 0 )
	{
		printTextFormattedColor(font8x8_bmp, left, y,
			makeColorRGB(128, 210, 255), "QUEST JOURNAL OVERVIEW");
		y += 21;
		questDialogueEditorDrawEditableRow(left, fieldRight, y, "Quest ID",
			QUEST_DIALOGUE_FIELD_QUEST_ID, QUEST_DIALOGUE_CATEGORY_FILE_QUEST);
		questDialogueEditorDrawEditableRow(left, fieldRight, y, "Title",
			QUEST_DIALOGUE_FIELD_QUEST_TITLE, QUEST_DIALOGUE_CATEGORY_FILE_QUEST);
		questDialogueEditorDrawEditableRow(left, fieldRight, y, "Summary",
			QUEST_DIALOGUE_FIELD_QUEST_SUMMARY, QUEST_DIALOGUE_CATEGORY_FILE_QUEST);
		questDialogueEditorDrawEditableRow(left, fieldRight, y, "General objective",
			QUEST_DIALOGUE_FIELD_QUEST_OBJECTIVE, QUEST_DIALOGUE_CATEGORY_FILE_QUEST);
		questDialogueEditorDrawEditableRow(left, fieldRight, y, "Completion text",
			QUEST_DIALOGUE_FIELD_QUEST_COMPLETED_TEXT, QUEST_DIALOGUE_CATEGORY_FILE_QUEST);
		questDialogueEditorDrawEditableRow(left, fieldRight, y, "Failure text",
			QUEST_DIALOGUE_FIELD_QUEST_FAILED_TEXT, QUEST_DIALOGUE_CATEGORY_FILE_QUEST);
		const std::string scope = quest->HasMember("scope") && (*quest)["scope"].IsString()
			? questEditorNormalizeID((*quest)["scope"].GetString()) : "player";
		const std::string scopeLabel = scope == "player" ? "Personal"
			: (scope == "party" ? "Party" : "World");
		const int schemaVersion = questDialogueEditorDocument.HasMember("version")
			&& questDialogueEditorDocument["version"].IsInt()
			? questDialogueEditorDocument["version"].GetInt() : 0;
		const bool sharedOwnership = schemaVersion
			>= automatia::dialogue::SharedQuestOwnershipSchemaVersion;
		printTextFormatted(font8x8_bmp, left, y + 5, "%-17s %s",
			"Ownership", scopeLabel.c_str());
		if ( questDialogueEditorImmediateButton(fieldRight - 72, y, 72, "CHANGE") )
			questDialogueEditorCycleScopeDirect();
		y += 23;
		const bool repeatable = quest->HasMember("repeatable")
			&& (*quest)["repeatable"].IsBool() && (*quest)["repeatable"].GetBool();
		printTextFormatted(font8x8_bmp, left, y + 5, "%-17s %s",
			"Repeatable", repeatable ? "yes" : "no");
		if ( questDialogueEditorImmediateButton(fieldRight - 72, y, 72, "TOGGLE") )
			questDialogueEditorToggleRepeatable();
		y += 27;
		printTextFormatted(font8x8_bmp, left, y + 5,
			"Schema %d ownership semantics", schemaVersion);
		if ( !sharedOwnership
			&& questDialogueEditorImmediateButton(
				fieldRight - 128, y, 128, "UPGRADE SHARING") )
		{
			questDialogueEditorUpgradeSharedQuestOwnership();
		}
		y += 25;
		if ( !sharedOwnership && scope != "player" )
		{
			printTextFormattedColor(font8x8_bmp, left, y,
				makeColorRGB(255, 210, 96),
				"Legacy schema: authored %s currently executes as Personal.",
				scopeLabel.c_str());
		}
		else if ( scope == "player" )
		{
			printTextFormattedColor(font8x8_bmp, left, y,
				makeColorRGB(128, 255, 160),
				"Personal: private state owned by this durable player identity.");
		}
		else if ( scope == "party" )
		{
			printTextFormattedColor(font8x8_bmp, left, y,
				makeColorRGB(128, 255, 160),
				"Party: one state shared by the server's persistent PartyID.");
		}
		else
		{
			printTextFormattedColor(font8x8_bmp, left, y,
				makeColorRGB(128, 255, 160),
				"World: one state shared by every player in this world save.");
		}
		y += 17;
		if ( sharedOwnership && scope == "party" )
		{
			printText(font8x8_bmp, left, y,
				"Members synchronize progress; leaving immediately revokes the shared view.");
			y += 17;
		}
		else if ( sharedOwnership && scope == "world" )
		{
			printText(font8x8_bmp, left, y,
				"Map instances, playable floors, authored layers, and entity Z do not divide it.");
			y += 17;
		}
		printText(font8x8_bmp, left, y,
			"Quest state persists in Automatia story saves; this editor does not mutate live quest state.");
	}
	else if ( questDialogueEditorQuestPanel == 2 )
	{
		printTextFormattedColor(font8x8_bmp, left, y,
			makeColorRGB(128, 210, 255), "QUEST GIVER / JOURNAL ORIGIN");
		y += 21;
		printTextFormattedColor(font8x8_bmp, left, y,
			makeColorRGB(255, 230, 96), "%s",
			questDialogueEditorGiverMarkerSummary().c_str());
		y += 22;
		if ( questDialogueEditorImmediateButton(left, y, 116, "PICK TILE ON MAP") )
			questDialogueEditorUseCursorTileAsQuestGiver();
		if ( questDialogueEditorImmediateButton(left + 124, y, 112,
			"MANUAL COORDS") ) questDialogueEditorBeginManualOriginCoordinates();
		if ( questDialogueEditorImmediateButton(left + 244, y, 144,
			"FOLLOW SELECTED NPC") ) questDialogueEditorUseSelectedNPCAsQuestGiver();
		if ( questDialogueEditorImmediateButton(left + 396, y, 96,
			"CLEAR ORIGIN") ) questDialogueEditorClearQuestGiver();
		y += 31;
		if ( !quest->HasMember("origin") || !(*quest)["origin"].IsObject() )
		{
			printText(font8x8_bmp, left, y,
				"No origin. Pick a map tile, enter coordinates, or select a persistent NPC.");
			return;
		}
		questDialogueEditorDrawEditableRow(left, fieldRight, y, "Label",
			QUEST_DIALOGUE_FIELD_ORIGIN_LABEL, QUEST_DIALOGUE_CATEGORY_FILE_QUEST);
		questDialogueEditorDrawEditableRow(left, fieldRight, y, "Map",
			QUEST_DIALOGUE_FIELD_ORIGIN_MAP, QUEST_DIALOGUE_CATEGORY_FILE_QUEST);
		questDialogueEditorDrawEditableRow(left, fieldRight, y, "Static tile X",
			QUEST_DIALOGUE_FIELD_ORIGIN_X, QUEST_DIALOGUE_CATEGORY_FILE_QUEST);
		questDialogueEditorDrawEditableRow(left, fieldRight, y, "Static tile Y",
			QUEST_DIALOGUE_FIELD_ORIGIN_Y, QUEST_DIALOGUE_CATEGORY_FILE_QUEST);
		questDialogueEditorDrawEditableRow(left, fieldRight, y, "Playable floor",
			QUEST_DIALOGUE_FIELD_ORIGIN_FLOOR, QUEST_DIALOGUE_CATEGORY_FILE_QUEST);
		questDialogueEditorDrawEditableRow(left, fieldRight, y, "Persistent NPC ID",
			QUEST_DIALOGUE_FIELD_ORIGIN_NPC_ID, QUEST_DIALOGUE_CATEGORY_FILE_QUEST);
		const rapidjson::Value& origin = (*quest)["origin"];
		const bool tracking = origin.HasMember("track_npc")
			&& origin["track_npc"].IsBool() && origin["track_npc"].GetBool();
		printTextFormatted(font8x8_bmp, left, y + 5,
			"Track NPC         %s", tracking ? "yes" : "no (static marker)");
		if ( questDialogueEditorImmediateButton(fieldRight - 128, y, 128,
			"USE MODE BUTTONS") )
			questDialogueEditorSetMessage(
				"Use cursor tile for static mode or Follow Selected NPC for tracking mode.");
		y += 28;
		const bool sameFloor = std::string(
			questDialogueEditorMarkerVisibility(origin)) == "same_floor";
		printTextFormatted(font8x8_bmp, left, y + 5,
			"Floor visibility  %s", sameFloor ? "same floor only" : "whole column");
		if ( questDialogueEditorImmediateButton(fieldRight - 136, y, 136,
			sameFloor ? "USE WHOLE COLUMN" : "USE SAME FLOOR") )
			questDialogueEditorToggleOriginMarkerVisibility();
		y += 27;
		printText(font8x8_bmp, left, y,
			"Same-floor compares PlayableFloorID; column ignores floor but still stays on this map/tile.");
		y += 17;
		printText(font8x8_bmp, left, y,
			"Persistent NPC IDs are map-scoped. Follow mode requires a saved, persistent map entity.");
	}
	else
	{
		const int listWidth = std::min(300,
			std::max(292, (right - mainLeft) * 40 / 100));
		const int listRight = mainLeft + listWidth;
		drawDepressed(mainLeft + 8, y - 5, listRight, bottom - 8);
		printTextFormattedColor(font8x8_bmp, mainLeft + 16, y + 3,
			makeColorRGB(128, 210, 255), "OBJECTIVES (%d)",
			questDialogueEditorPreview.objectiveCount);
		if ( questDialogueEditorImmediateButton(mainLeft + 16, y + 20, 58, "+ ADD") )
			questDialogueEditorAddObjective();
		if ( questDialogueEditorImmediateButton(mainLeft + 80, y + 20, 58, "DUP") )
			questDialogueEditorDuplicateSelectedObjective();
		if ( questDialogueEditorImmediateButton(mainLeft + 144, y + 20, 52, "DEL") )
			questDialogueEditorDeleteObjective();
		if ( questDialogueEditorImmediateButton(mainLeft + 202, y + 20, 32, "UP") )
			questDialogueEditorMoveSelectedObjective(-1);
		if ( questDialogueEditorImmediateButton(mainLeft + 240, y + 20, 36, "DOWN") )
			questDialogueEditorMoveSelectedObjective(1);
		const int listTop = y + 47;
		const int rowHeight = 42;
		const int visible = std::max(1, (bottom - listTop - 12) / rowHeight);
		const int count = quest->HasMember("objectives") && (*quest)["objectives"].IsArray()
			? static_cast<int>((*quest)["objectives"].Size()) : 0;
		const int maximumScroll = std::max(0, count - visible);
		if ( omousex >= mainLeft + 8 && omousex < listRight
			&& omousey >= listTop && omousey < bottom && scroll != 0 )
		{
			questDialogueEditorObjectiveScroll = std::max(0,
				std::min(maximumScroll, questDialogueEditorObjectiveScroll + scroll));
			scroll = 0;
		}
		questDialogueEditorObjectiveScroll = std::max(0,
			std::min(maximumScroll, questDialogueEditorObjectiveScroll));
		if ( count > 0 )
		{
			rapidjson::Value& objectives = (*quest)["objectives"];
			for ( int row = 0; row < visible; ++row )
			{
				const int index = questDialogueEditorObjectiveScroll + row;
				if ( index >= count ) break;
				const int rowY = listTop + row * rowHeight;
				const rapidjson::Value& objective = objectives[
					static_cast<rapidjson::SizeType>(index)];
				if ( index == questDialogueEditorSelectedObjective )
					drawDepressed(mainLeft + 14, rowY, listRight - 6, rowY + 37);
				else drawWindowFancy(mainLeft + 14, rowY, listRight - 6, rowY + 37);
				const std::string id = objective.IsObject() && objective.HasMember("id")
					&& objective["id"].IsString() ? objective["id"].GetString() : "(invalid)";
				const std::string textValue = objective.IsObject() && objective.HasMember("text")
					&& objective["text"].IsString() ? objective["text"].GetString() : "(invalid)";
				printTextFormatted(font8x8_bmp, mainLeft + 21, rowY + 5,
					"%d. %s", index + 1, id.c_str());
				printTextFormattedColor(font8x8_bmp, mainLeft + 21, rowY + 20,
					makeColorRGB(180, 200, 220), "%s",
					questDialogueEditorClipText(textValue, listWidth - 42).c_str());
				if ( mousestatus[SDL_BUTTON_LEFT] && omousex >= mainLeft + 14
					&& omousex < listRight - 6 && omousey >= rowY
					&& omousey < rowY + 37 )
				{
					mousestatus[SDL_BUTTON_LEFT] = 0;
					questDialogueEditorSelectedObjective = index;
				}
			}
		}

		const int inspectorLeft = listRight + 12;
		int inspectorY = y;
		printTextFormattedColor(font8x8_bmp, inspectorLeft, inspectorY,
			makeColorRGB(128, 210, 255), "SELECTED OBJECTIVE");
		inspectorY += 21;
		rapidjson::Value* objective = questDialogueEditorSelectedObjectiveValueForEdit();
		if ( !objective )
		{
			printText(font8x8_bmp, inspectorLeft, inspectorY,
				"Add or select an objective to edit it.");
			return;
		}
		questDialogueEditorDrawEditableRow(inspectorLeft, fieldRight, inspectorY,
			"Objective ID", QUEST_DIALOGUE_FIELD_OBJECTIVE_ID,
			QUEST_DIALOGUE_CATEGORY_OBJECTIVE);
		questDialogueEditorDrawEditableRow(inspectorLeft, fieldRight, inspectorY,
			"Active text", QUEST_DIALOGUE_FIELD_OBJECTIVE_TEXT,
			QUEST_DIALOGUE_CATEGORY_OBJECTIVE);
		questDialogueEditorDrawEditableRow(inspectorLeft, fieldRight, inspectorY,
			"Completed text", QUEST_DIALOGUE_FIELD_OBJECTIVE_COMPLETED_TEXT,
			QUEST_DIALOGUE_CATEGORY_OBJECTIVE);
		questDialogueEditorDrawEditableRow(inspectorLeft, fieldRight, inspectorY,
			"Stage", QUEST_DIALOGUE_FIELD_OBJECTIVE_STAGE,
			QUEST_DIALOGUE_CATEGORY_OBJECTIVE);
		const bool optional = objective->HasMember("optional")
			&& (*objective)["optional"].IsBool() && (*objective)["optional"].GetBool();
		printTextFormatted(font8x8_bmp, inspectorLeft, inspectorY + 5,
			"%-17s %s", "Optional", optional ? "yes" : "no");
		if ( questDialogueEditorImmediateButton(fieldRight - 72, inspectorY, 72, "TOGGLE") )
			questDialogueEditorToggleObjectiveOptional();
		inspectorY += 23;
		questDialogueEditorDrawEditableRow(inspectorLeft, fieldRight, inspectorY,
			"Progress variable", QUEST_DIALOGUE_FIELD_OBJECTIVE_PROGRESS_VARIABLE,
			QUEST_DIALOGUE_CATEGORY_OBJECTIVE);
		questDialogueEditorDrawEditableRow(inspectorLeft, fieldRight, inspectorY,
			"Progress target", QUEST_DIALOGUE_FIELD_OBJECTIVE_TARGET,
			QUEST_DIALOGUE_CATEGORY_OBJECTIVE);
		questDialogueEditorDrawEditableRow(inspectorLeft, fieldRight, inspectorY,
			"Defeat entity ID", QUEST_DIALOGUE_FIELD_OBJECTIVE_DEFEAT_ID,
			QUEST_DIALOGUE_CATEGORY_OBJECTIVE);
		const bool hasMarker = objective->HasMember("map_marker")
			&& (*objective)["map_marker"].IsObject();
		printTextFormattedColor(font8x8_bmp, inspectorLeft, inspectorY,
			makeColorRGB(255, 230, 96), "MAP MARKER: %s",
			hasMarker ? "configured" : "none");
		inspectorY += 18;
		if ( questDialogueEditorImmediateButton(inspectorLeft, inspectorY, 112,
			"PICK TILE ON MAP") ) questDialogueEditorSetObjectiveMarkerFromCursor();
		if ( questDialogueEditorImmediateButton(inspectorLeft + 120, inspectorY, 108,
			"MANUAL COORDS") ) questDialogueEditorBeginManualObjectiveCoordinates();
		if ( questDialogueEditorImmediateButton(inspectorLeft + 236, inspectorY, 110,
			"CLEAR MARKER") ) questDialogueEditorClearObjectiveMarker();
		inspectorY += 26;
		if ( hasMarker )
		{
			questDialogueEditorDrawEditableRow(inspectorLeft, fieldRight, inspectorY,
				"Marker map", QUEST_DIALOGUE_FIELD_MARKER_MAP,
				QUEST_DIALOGUE_CATEGORY_MARKER);
			questDialogueEditorDrawEditableRow(inspectorLeft, fieldRight, inspectorY,
				"Marker tile X", QUEST_DIALOGUE_FIELD_MARKER_X,
				QUEST_DIALOGUE_CATEGORY_MARKER);
			questDialogueEditorDrawEditableRow(inspectorLeft, fieldRight, inspectorY,
				"Marker tile Y", QUEST_DIALOGUE_FIELD_MARKER_Y,
				QUEST_DIALOGUE_CATEGORY_MARKER);
			questDialogueEditorDrawEditableRow(inspectorLeft, fieldRight, inspectorY,
				"Playable floor", QUEST_DIALOGUE_FIELD_MARKER_FLOOR,
				QUEST_DIALOGUE_CATEGORY_MARKER);
			const rapidjson::Value& marker = (*objective)["map_marker"];
			const bool sameFloor = std::string(
				questDialogueEditorMarkerVisibility(marker)) == "same_floor";
			printTextFormatted(font8x8_bmp, inspectorLeft, inspectorY + 5,
				"%-17s %s", "Floor visibility",
				sameFloor ? "same floor only" : "whole column");
			if ( questDialogueEditorImmediateButton(fieldRight - 136, inspectorY, 136,
				sameFloor ? "WHOLE COLUMN" : "SAME FLOOR") )
				questDialogueEditorToggleObjectiveMarkerVisibility();
			inspectorY += 23;
		}
	}
}

static void questDialogueEditorDrawTutorialsPage()
{
	const int top = suby1 + 48;
	const int bottom = suby2 - 34;
	const int listLeft = subx1 + 8;
	const int listRight = listLeft + 330;
	const int detailLeft = listRight + 8;
	const int right = subx2 - 8;
	drawDepressed(listLeft, top, listRight, bottom);
	drawDepressed(detailLeft, top, right, bottom);
	printText(font8x8_bmp, listLeft + 8, top + 8, "VERIFIED TUTORIAL LIBRARY");
	drawDepressed(listLeft + 7, top + 25, listRight - 7, top + 45);
	printTextFormattedColor(font8x8_bmp, listLeft + 12, top + 31,
		questDialogueEditorTutorialSearch[0] ? makeColorRGB(255, 255, 255)
			: makeColorRGB(144, 144, 144), "%.35s",
		questDialogueEditorTutorialSearch[0]
			? questDialogueEditorTutorialSearch : "Search title, category, or difficulty...");
	if ( mousestatus[SDL_BUTTON_LEFT]
		&& omousex >= listLeft + 7 && omousex < listRight - 7
		&& omousey >= top + 25 && omousey < top + 45 )
	{
		mousestatus[SDL_BUTTON_LEFT] = 0;
		questDialogueEditorEndTransientTextInput();
		questDialogueEditorTutorialSearchEditing = true;
		inputstr = questDialogueEditorTutorialSearch;
		inputlen = static_cast<int>(sizeof(questDialogueEditorTutorialSearch) - 1);
		cursorflash = ticks;
		SDL_StartTextInput();
	}
	const auto& tutorials = automatia::dialogue::tutorialRecipes();
	static const char* difficulties[] = {
		"All", "Beginner", "Intermediate", "Advanced"
	};
	static const std::vector<std::string> categories = []
	{
		std::vector<std::string> values{ "All" };
		for ( const auto& tutorial : automatia::dialogue::tutorialRecipes() )
		{
			if ( std::find(values.begin(), values.end(), tutorial.category)
				== values.end() ) values.push_back(tutorial.category);
		}
		std::sort(values.begin() + 1, values.end());
		return values;
	}();
	questDialogueEditorTutorialDifficulty = std::max(0,
		std::min(questDialogueEditorTutorialDifficulty, 3));
	questDialogueEditorTutorialCategory = std::max(0,
		std::min(questDialogueEditorTutorialCategory,
			static_cast<int>(categories.size()) - 1));
	const std::string difficultyLabel = std::string("DIFF: ")
		+ difficulties[questDialogueEditorTutorialDifficulty] + " >";
	const std::string categoryLabel = "CAT: "
		+ categories[questDialogueEditorTutorialCategory] + " >";
	if ( questDialogueEditorImmediateButton(listLeft + 7, top + 50, 124,
		difficultyLabel.c_str()) )
	{
		questDialogueEditorTutorialDifficulty =
			(questDialogueEditorTutorialDifficulty + 1) % 4;
		questDialogueEditorTutorialScroll = 0;
	}
	if ( questDialogueEditorImmediateButton(listLeft + 137, top + 50, 186,
		questDialogueEditorClipText(categoryLabel, 174).c_str()) )
	{
		questDialogueEditorTutorialCategory =
			(questDialogueEditorTutorialCategory + 1)
			% static_cast<int>(categories.size());
		questDialogueEditorTutorialScroll = 0;
	}
	std::vector<int> matches;
	for ( int index = 0; index < static_cast<int>(tutorials.size()); ++index )
	{
		if ( questDialogueEditorTutorialDifficulty > 0
			&& tutorials[index].difficulty
				!= difficulties[questDialogueEditorTutorialDifficulty] ) continue;
		if ( questDialogueEditorTutorialCategory > 0
			&& tutorials[index].category
				!= categories[questDialogueEditorTutorialCategory] ) continue;
		const std::string searchable = tutorials[index].title + " "
			+ tutorials[index].category + " " + tutorials[index].difficulty;
		if ( questDialogueEditorMatches(searchable, questDialogueEditorTutorialSearch) )
			matches.push_back(index);
	}
	const int listTop = top + 76;
	const int visible = std::max(1, (bottom - listTop - 8) / 32);
	const int maximumScroll = std::max(0, static_cast<int>(matches.size()) - visible);
	if ( omousex >= listLeft && omousex < listRight
		&& omousey >= listTop && omousey < bottom && scroll != 0 )
	{
		questDialogueEditorTutorialScroll = std::max(0,
			std::min(maximumScroll, questDialogueEditorTutorialScroll + scroll));
		scroll = 0;
	}
	questDialogueEditorTutorialScroll = std::max(0,
		std::min(maximumScroll, questDialogueEditorTutorialScroll));
	for ( int row = 0; row < visible; ++row )
	{
		const int filtered = questDialogueEditorTutorialScroll + row;
		if ( filtered >= static_cast<int>(matches.size()) ) break;
		const int index = matches[filtered];
		const int y = listTop + row * 32;
		if ( index == questDialogueEditorTutorialSelection )
			drawDepressed(listLeft + 6, y, listRight - 6, y + 29);
		printTextFormatted(font8x8_bmp, listLeft + 11, y + 5, "%.36s",
			tutorials[index].title.c_str());
		printTextFormattedColor(font8x8_bmp, listLeft + 11, y + 18,
			makeColorRGB(150, 185, 220), "%.16s | %.12s",
			tutorials[index].category.c_str(), tutorials[index].difficulty.c_str());
		if ( mousestatus[SDL_BUTTON_LEFT]
			&& omousex >= listLeft + 6 && omousex < listRight - 6
			&& omousey >= y && omousey < y + 29 )
		{
			mousestatus[SDL_BUTTON_LEFT] = 0;
			questDialogueEditorTutorialSelection = index;
		}
	}
	if ( tutorials.empty() ) return;
	if ( matches.empty() )
	{
		printText(font8x8_bmp, listLeft + 11, listTop + 8,
			"No recipes match these filters.");
		printText(font8x8_bmp, detailLeft + 12, top + 12,
			"Clear the search or cycle the difficulty/category filters.");
		return;
	}
	if ( !matches.empty()
		&& std::find(matches.begin(), matches.end(),
			questDialogueEditorTutorialSelection) == matches.end() )
	{
		questDialogueEditorTutorialSelection = matches.front();
	}
	questDialogueEditorTutorialSelection = std::max(0,
		std::min(questDialogueEditorTutorialSelection,
			static_cast<int>(tutorials.size()) - 1));
	const auto& tutorial = tutorials[questDialogueEditorTutorialSelection];
	int y = top + 10;
	printTextFormattedColor(font8x8_bmp, detailLeft + 12, y,
		makeColorRGB(255, 230, 96), "%s", tutorial.title.c_str());
	y += 18;
	printTextFormatted(font8x8_bmp, detailLeft + 12, y,
		"%s | %s | ID: %s%s", tutorial.category.c_str(),
		tutorial.difficulty.c_str(), tutorial.id.c_str(),
		tutorial.manualGameTestRequired ? " | MANUAL GAME TEST" : "");
	y += 22;
	printTextFormatted(font8x8_bmp, detailLeft + 12, y,
		"Goal: %.75s", tutorial.goal.c_str());
	y += 18;
	printTextFormatted(font8x8_bmp, detailLeft + 12, y,
		"Player: %.73s", tutorial.playerExperience.c_str());
	y += 18;
	printTextFormattedColor(font8x8_bmp, detailLeft + 12, y,
		makeColorRGB(150, 185, 220), "Panel: %.74s", tutorial.panelHint.c_str());
	y += 18;
	std::string concepts;
	for ( std::size_t index = 0; index < tutorial.concepts.size(); ++index )
	{
		if ( index ) concepts += ", ";
		concepts += tutorial.concepts[index];
	}
	printTextFormattedColor(font8x8_bmp, detailLeft + 12, y,
		makeColorRGB(150, 185, 220), "Concepts: %.70s", concepts.c_str());
	y += 22;
	printTextFormattedColor(font8x8_bmp, detailLeft + 12, y,
		makeColorRGB(128, 210, 255), "STEPS");
	y += 16;
	for ( std::size_t index = 0; index < tutorial.steps.size() && y + 16 < bottom; ++index )
	{
		printTextFormatted(font8x8_bmp, detailLeft + 17, y, "%d. %.76s",
			static_cast<int>(index) + 1, tutorial.steps[index].c_str());
		y += 17;
	}
	y += 6;
	printTextFormattedColor(font8x8_bmp, detailLeft + 12, y,
		makeColorRGB(128, 255, 160), "Expected: %.70s", tutorial.expectedResult.c_str());
	y += 18;
	printTextFormatted(font8x8_bmp, detailLeft + 12, y,
		"Multiplayer: %.66s", tutorial.multiplayerNote.c_str());
	y += 18;
	printTextFormatted(font8x8_bmp, detailLeft + 12, y,
		"Persistence: %.66s", tutorial.persistenceNote.c_str());
	y += 18;
	printTextFormattedColor(font8x8_bmp, detailLeft + 12, y,
		makeColorRGB(255, 175, 96), "Common mistake: %.64s", tutorial.commonMistake.c_str());
	if ( questDialogueEditorImmediateButton(right - 182, bottom - 30, 166,
		"APPLY VERIFIED EXAMPLE") )
	{
		if ( questDialogueEditorSelectedFile < 0 )
			questDialogueEditorSetMessage("Create or select a destination file first.");
		else questDialogueEditorRequestTransition(
			QUEST_DIALOGUE_PENDING_APPLY_TUTORIAL,
			questDialogueEditorTutorialSelection);
	}
}

static bool questDialogueEditorJSONOwnsTextInput()
{
	return newwindow == 38 && questDialogueEditorJSONEditing
		&& inputstr == questDialogueEditorJSONBuffer;
}

static void questDialogueEditorJSONStore(const std::string& value)
{
	const std::size_t size = std::min(value.size(),
		automatia::dialogue::MaximumDocumentBytes);
	std::memcpy(questDialogueEditorJSONBuffer, value.data(), size);
	questDialogueEditorJSONBuffer[size] = '\0';
	questDialogueEditorJSONCaret = std::min(questDialogueEditorJSONCaret, size);
	cursorflash = ticks;
}

static void questDialogueEditorJSONInsert(const std::string& inserted)
{
	std::string source = questDialogueEditorJSONSelectAll
		? std::string{} : std::string(questDialogueEditorJSONBuffer);
	if ( questDialogueEditorJSONSelectAll ) questDialogueEditorJSONCaret = 0;
	questDialogueEditorJSONSelectAll = false;
	questDialogueEditorJSONCaret = std::min(
		questDialogueEditorJSONCaret, source.size());
	const std::size_t available = automatia::dialogue::MaximumDocumentBytes
		- source.size();
	const std::size_t count = std::min(available, inserted.size());
	source.insert(questDialogueEditorJSONCaret, inserted, 0, count);
	questDialogueEditorJSONCaret += count;
	questDialogueEditorJSONStore(source);
}

static std::pair<std::size_t, std::size_t>
questDialogueEditorJSONLineBounds(
	const std::string& source,
	const std::size_t caret
)
{
	const std::size_t clamped = std::min(caret, source.size());
	const std::size_t previous = clamped == 0
		? std::string::npos : source.rfind('\n', clamped - 1);
	const std::size_t start = previous == std::string::npos ? 0 : previous + 1;
	const std::size_t next = source.find('\n', clamped);
	return { start, next == std::string::npos ? source.size() : next };
}

static void questDialogueEditorJSONMoveVertical(const int direction)
{
	const std::string source = questDialogueEditorJSONBuffer;
	const auto bounds = questDialogueEditorJSONLineBounds(
		source, questDialogueEditorJSONCaret);
	const std::size_t column = questDialogueEditorJSONCaret - bounds.first;
	if ( direction < 0 )
	{
		if ( bounds.first == 0 ) return;
		const auto destination = questDialogueEditorJSONLineBounds(
			source, bounds.first - 1);
		questDialogueEditorJSONCaret = destination.first
			+ std::min(column, destination.second - destination.first);
	}
	else
	{
		if ( bounds.second >= source.size() ) return;
		const auto destination = questDialogueEditorJSONLineBounds(
			source, bounds.second + 1);
		questDialogueEditorJSONCaret = destination.first
			+ std::min(column, destination.second - destination.first);
	}
	questDialogueEditorJSONSelectAll = false;
	cursorflash = ticks;
}

static bool questDialogueEditorJSONHandleKey(const SDL_KeyboardEvent& key)
{
	const bool control = (key.keysym.mod & KMOD_CTRL) != 0;
	std::string source = questDialogueEditorJSONBuffer;
	if ( control && key.keysym.sym == SDLK_a )
	{
		questDialogueEditorJSONSelectAll = true;
		questDialogueEditorJSONCaret = source.size();
		return true;
	}
	if ( control && key.keysym.sym == SDLK_c )
	{
		SDL_SetClipboardText(questDialogueEditorJSONBuffer);
		return true;
	}
	if ( control && key.keysym.sym == SDLK_x )
	{
		SDL_SetClipboardText(questDialogueEditorJSONBuffer);
		questDialogueEditorJSONCaret = 0;
		questDialogueEditorJSONSelectAll = false;
		questDialogueEditorJSONStore("");
		return true;
	}
	if ( control && key.keysym.sym == SDLK_v )
	{
		char* clipboard = SDL_GetClipboardText();
		if ( clipboard )
		{
			questDialogueEditorJSONInsert(clipboard);
			SDL_free(clipboard);
		}
		return true;
	}

	if ( key.keysym.sym == SDLK_BACKSPACE )
	{
		if ( questDialogueEditorJSONSelectAll )
		{
			questDialogueEditorJSONCaret = 0;
			questDialogueEditorJSONSelectAll = false;
			questDialogueEditorJSONStore("");
		}
		else if ( questDialogueEditorJSONCaret > 0 )
		{
			source.erase(questDialogueEditorJSONCaret - 1, 1);
			--questDialogueEditorJSONCaret;
			questDialogueEditorJSONStore(source);
		}
		return true;
	}
	if ( key.keysym.sym == SDLK_DELETE )
	{
		if ( questDialogueEditorJSONSelectAll )
		{
			questDialogueEditorJSONCaret = 0;
			questDialogueEditorJSONSelectAll = false;
			questDialogueEditorJSONStore("");
		}
		else if ( questDialogueEditorJSONCaret < source.size() )
		{
			source.erase(questDialogueEditorJSONCaret, 1);
			questDialogueEditorJSONStore(source);
		}
		return true;
	}
	if ( key.keysym.sym == SDLK_RETURN || key.keysym.sym == SDLK_KP_ENTER )
	{
		questDialogueEditorJSONInsert("\n");
		return true;
	}
	if ( key.keysym.sym == SDLK_TAB )
	{
		questDialogueEditorJSONInsert("  ");
		return true;
	}
	if ( key.keysym.sym == SDLK_LEFT || key.keysym.sym == SDLK_RIGHT )
	{
		if ( questDialogueEditorJSONSelectAll )
		{
			questDialogueEditorJSONCaret = key.keysym.sym == SDLK_LEFT
				? 0 : source.size();
		}
		else if ( key.keysym.sym == SDLK_LEFT && questDialogueEditorJSONCaret > 0 )
		{
			--questDialogueEditorJSONCaret;
		}
		else if ( key.keysym.sym == SDLK_RIGHT
			&& questDialogueEditorJSONCaret < source.size() )
		{
			++questDialogueEditorJSONCaret;
		}
		questDialogueEditorJSONSelectAll = false;
		cursorflash = ticks;
		return true;
	}
	if ( key.keysym.sym == SDLK_UP || key.keysym.sym == SDLK_DOWN )
	{
		questDialogueEditorJSONMoveVertical(
			key.keysym.sym == SDLK_UP ? -1 : 1);
		return true;
	}
	if ( key.keysym.sym == SDLK_HOME || key.keysym.sym == SDLK_END )
	{
		const auto bounds = questDialogueEditorJSONLineBounds(
			source, questDialogueEditorJSONCaret);
		questDialogueEditorJSONCaret = key.keysym.sym == SDLK_HOME
			? bounds.first : bounds.second;
		questDialogueEditorJSONSelectAll = false;
		cursorflash = ticks;
		return true;
	}
	if ( key.keysym.sym == SDLK_ESCAPE )
	{
		questDialogueEditorJSONEditing = false;
		questDialogueEditorJSONSelectAll = false;
		SDL_StopTextInput();
		inputstr = nullptr;
		questDialogueEditorSetMessage("Advanced JSON edit canceled.");
		return true;
	}
	return false;
}

static void questDialogueEditorLoadJSONBuffer()
{
	const std::string pretty = questDialogueEditorModel.serialize(true);
	const std::string json = pretty.size() <= automatia::dialogue::MaximumDocumentBytes
		? pretty : questDialogueEditorModel.serialize(false);
	std::snprintf(questDialogueEditorJSONBuffer,
		sizeof(questDialogueEditorJSONBuffer), "%s", json.c_str());
	questDialogueEditorJSONCaret = 0;
	questDialogueEditorJSONSelectAll = false;
	questDialogueEditorJSONHorizontalScroll = 0;
}

static bool questDialogueEditorApplyJSONBuffer(const bool saveToDisk)
{
	std::string error;
	if ( !questDialogueEditorModel.replaceWithEdit(
			questDialogueEditorJSONBuffer, "Apply Advanced JSON", error) )
	{
		questDialogueEditorSetMessage(error);
		return false;
	}
	questDialogueEditorJSONEditing = false;
	questDialogueEditorJSONSelectAll = false;
	SDL_StopTextInput();
	inputstr = nullptr;
	const std::string filename = questDialogueEditorSelectedFile >= 0
		? questDialogueEditorFiles[questDialogueEditorSelectedFile] : std::string{};
	questDialogueEditorLoadPreview(filename, false);
	if ( saveToDisk )
	{
		if ( !questDialogueEditorWriteDocument() ) return false;
		questDialogueEditorSetMessage("Advanced JSON validated and saved to disk.");
	}
	else
	{
		questDialogueEditorSetMessage(
			"Advanced JSON applied in memory. Press SAVE to write the file.");
	}
	return true;
}

static void questDialogueEditorDrawJSONPage()
{
	const int top = suby1 + 48;
	const int bottom = suby2 - 34;
	const int mainLeft = questDialogueEditorDrawFileRail(subx1 + 8, top, bottom);
	const int right = subx2 - 8;
	drawDepressed(mainLeft, top, right, bottom);
	printTextFormattedColor(font8x8_bmp, mainLeft + 12, top + 9,
		makeColorRGB(128, 210, 255), "ADVANCED JSON - EDITABLE LOSSLESS SOURCE");
	printTextFormatted(font8x8_bmp, mainLeft + 12, top + 25,
		"%zu / %zu bytes. Click text; arrows/Home/End, Enter, Tab, Ctrl+A/C/X/V work.",
		questDialogueEditorJSONEditing
			? std::strlen(questDialogueEditorJSONBuffer)
			: questDialogueEditorModel.serialize(false).size(),
		automatia::dialogue::MaximumDocumentBytes);
	if ( !questDialogueEditorJSONEditing )
	{
		if ( questDialogueEditorImmediateButton(right - 106, top + 7, 90, "EDIT JSON") )
		{
			questDialogueEditorEndTransientTextInput();
			questDialogueEditorLoadJSONBuffer();
			questDialogueEditorJSONEditing = true;
			inputstr = questDialogueEditorJSONBuffer;
			inputlen = static_cast<int>(sizeof(questDialogueEditorJSONBuffer) - 1);
			cursorflash = ticks;
			SDL_StartTextInput();
		}
	}
	else
	{
		if ( questDialogueEditorImmediateButton(right - 286, top + 7, 70, "APPLY") )
		{
			questDialogueEditorApplyJSONBuffer(false);
		}
		if ( questDialogueEditorImmediateButton(right - 210, top + 7, 106,
			"APPLY & SAVE") )
		{
			questDialogueEditorApplyJSONBuffer(true);
		}
		if ( questDialogueEditorImmediateButton(right - 98, top + 7, 82, "CANCEL") )
		{
			questDialogueEditorJSONEditing = false;
			SDL_StopTextInput();
			inputstr = nullptr;
		}
	}

	const std::string source = questDialogueEditorJSONEditing
		? std::string(questDialogueEditorJSONBuffer)
		: questDialogueEditorModel.serialize(true);
	std::vector<std::string> lines(1);
	std::vector<std::size_t> lineStarts(1, 0);
	for ( const char character : source )
	{
		if ( character == '\n' )
		{
			lines.emplace_back();
			lineStarts.push_back(lineStarts.back() + lines[lines.size() - 2].size() + 1);
		}
		else lines.back().push_back(character);
	}
	const int codeTop = top + 50;
	const int visible = std::max(1, (bottom - codeTop - 8) / 13);
	const int maximumScroll = std::max(0, static_cast<int>(lines.size()) - visible);
	if ( omousex >= mainLeft && omousex < right
		&& omousey >= codeTop && omousey < bottom && scroll != 0 )
	{
		questDialogueEditorJSONScroll = std::max(0,
			std::min(maximumScroll, questDialogueEditorJSONScroll + scroll));
		scroll = 0;
	}
	questDialogueEditorJSONScroll = std::max(0,
		std::min(maximumScroll, questDialogueEditorJSONScroll));
	int caretLine = 0;
	int caretColumn = 0;
	if ( questDialogueEditorJSONEditing )
	{
		questDialogueEditorJSONCaret = std::min(
			questDialogueEditorJSONCaret, source.size());
		auto line = std::upper_bound(lineStarts.begin(), lineStarts.end(),
			questDialogueEditorJSONCaret);
		caretLine = std::max(0, static_cast<int>(line - lineStarts.begin()) - 1);
		caretColumn = static_cast<int>(questDialogueEditorJSONCaret
			- lineStarts[caretLine]);
		if ( caretLine < questDialogueEditorJSONScroll )
			questDialogueEditorJSONScroll = caretLine;
		else if ( caretLine >= questDialogueEditorJSONScroll + visible )
			questDialogueEditorJSONScroll = caretLine - visible + 1;
		const int visibleColumns = std::max(1, (right - mainLeft - 66) / 8);
		if ( caretColumn < questDialogueEditorJSONHorizontalScroll )
			questDialogueEditorJSONHorizontalScroll = caretColumn;
		else if ( caretColumn >= questDialogueEditorJSONHorizontalScroll + visibleColumns )
			questDialogueEditorJSONHorizontalScroll = caretColumn - visibleColumns + 1;
		questDialogueEditorJSONHorizontalScroll = std::max(0,
			questDialogueEditorJSONHorizontalScroll);

		if ( mousestatus[SDL_BUTTON_LEFT]
			&& omousex >= mainLeft + 50 && omousex < right
			&& omousey >= codeTop && omousey < bottom )
		{
			mousestatus[SDL_BUTTON_LEFT] = 0;
			const int clickedLine = questDialogueEditorJSONScroll
				+ (omousey - codeTop) / 13;
			if ( clickedLine >= 0 && clickedLine < static_cast<int>(lines.size()) )
			{
				const int clickedColumn = questDialogueEditorJSONHorizontalScroll
					+ std::max(0, (omousex - (mainLeft + 54)) / 8);
				questDialogueEditorJSONCaret = lineStarts[clickedLine]
					+ std::min<std::size_t>(clickedColumn, lines[clickedLine].size());
				questDialogueEditorJSONSelectAll = false;
				cursorflash = ticks;
			}
		}
	}
	for ( int row = 0; row < visible; ++row )
	{
		const int index = questDialogueEditorJSONScroll + row;
		if ( index >= static_cast<int>(lines.size()) ) break;
		printTextFormattedColor(font8x8_bmp, mainLeft + 12, codeTop + row * 13,
			makeColorRGB(155, 175, 195), "%4d", index + 1);
		const std::string visibleLine = lines[index].size()
			> static_cast<std::size_t>(questDialogueEditorJSONHorizontalScroll)
			? lines[index].substr(questDialogueEditorJSONHorizontalScroll)
			: std::string{};
		printTextFormatted(font8x8_bmp, mainLeft + 54, codeTop + row * 13,
			"%s", questDialogueEditorClipText(
				visibleLine, right - mainLeft - 66).c_str());
		if ( questDialogueEditorJSONEditing && index == caretLine
			&& !questDialogueEditorJSONSelectAll
			&& (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
		{
			const int visibleCaretColumn = caretColumn
				- questDialogueEditorJSONHorizontalScroll;
			if ( visibleCaretColumn >= 0 )
			{
				printTextFormattedColor(font8x8_bmp,
					mainLeft + 54 + visibleCaretColumn * 8,
					codeTop + row * 13,
					makeColorRGB(255, 230, 96), "|");
			}
		}
	}
	if ( questDialogueEditorJSONEditing )
	{
		printTextFormattedColor(font8x8_bmp, right - 230, bottom - 18,
			questDialogueEditorJSONSelectAll
				? makeColorRGB(255, 230, 96) : makeColorRGB(155, 175, 195),
			questDialogueEditorJSONSelectAll ? "ALL TEXT SELECTED" : "Line %d, column %d",
			caretLine + 1, caretColumn + 1);
	}
}

static const char* questDialogueEditorSandboxSeedKindName()
{
	switch ( questDialogueEditorSandboxSeedKind )
	{
		case QUEST_DIALOGUE_SANDBOX_WORLD_FLAG: return "World flag";
		case QUEST_DIALOGUE_SANDBOX_NPC_FLAG: return "NPC flag";
		case QUEST_DIALOGUE_SANDBOX_WORLD_VARIABLE: return "World variable";
		case QUEST_DIALOGUE_SANDBOX_NPC_VARIABLE: return "NPC variable";
		case QUEST_DIALOGUE_SANDBOX_QUEST_STARTED: return "Quest started";
		case QUEST_DIALOGUE_SANDBOX_QUEST_ACCEPTED: return "Quest accepted";
		case QUEST_DIALOGUE_SANDBOX_QUEST_COMPLETED: return "Quest completed";
		case QUEST_DIALOGUE_SANDBOX_QUEST_FAILED: return "Quest failed";
		case QUEST_DIALOGUE_SANDBOX_QUEST_STAGE: return "Quest stage";
		case QUEST_DIALOGUE_SANDBOX_OBJECTIVE_COMPLETED: return "Objective";
		case QUEST_DIALOGUE_SANDBOX_NODE_SEEN: return "Node seen";
		case QUEST_DIALOGUE_SANDBOX_CHOICE_USED: return "Choice used";
		default: return "World flag";
	}
}

static bool questDialogueEditorSandboxSeedUsesBoolean()
{
	return questDialogueEditorSandboxSeedKind != QUEST_DIALOGUE_SANDBOX_WORLD_VARIABLE
		&& questDialogueEditorSandboxSeedKind != QUEST_DIALOGUE_SANDBOX_NPC_VARIABLE
		&& questDialogueEditorSandboxSeedKind != QUEST_DIALOGUE_SANDBOX_QUEST_STAGE;
}

static bool questDialogueEditorApplySandboxSeed()
{
	questDialogueEditorEndTransientTextInput();
	const std::string key = questEditorNormalizeID(
		questDialogueEditorSandboxSeedKey);
	const std::string subkey = questEditorNormalizeID(
		questDialogueEditorSandboxSeedSubkey);
	if ( key.empty() )
	{
		questDialogueEditorSetMessage("Simulated-state key must contain letters or numbers.");
		return false;
	}
	auto& state = questDialogueEditorSandboxConfiguredState;
	switch ( questDialogueEditorSandboxSeedKind )
	{
		case QUEST_DIALOGUE_SANDBOX_WORLD_FLAG:
			state.worldFlags[key] = questDialogueEditorSandboxSeedBoolean;
			break;
		case QUEST_DIALOGUE_SANDBOX_NPC_FLAG:
			state.npcFlags[key] = questDialogueEditorSandboxSeedBoolean;
			break;
		case QUEST_DIALOGUE_SANDBOX_WORLD_VARIABLE:
			state.worldVariables[key] = questDialogueEditorSandboxSeedValue;
			break;
		case QUEST_DIALOGUE_SANDBOX_NPC_VARIABLE:
			state.npcVariables[key] = questDialogueEditorSandboxSeedValue;
			break;
		case QUEST_DIALOGUE_SANDBOX_QUEST_STARTED:
			state.quests[key].started = questDialogueEditorSandboxSeedBoolean;
			break;
		case QUEST_DIALOGUE_SANDBOX_QUEST_ACCEPTED:
			state.quests[key].accepted = questDialogueEditorSandboxSeedBoolean;
			break;
		case QUEST_DIALOGUE_SANDBOX_QUEST_COMPLETED:
			state.quests[key].completed = questDialogueEditorSandboxSeedBoolean;
			break;
		case QUEST_DIALOGUE_SANDBOX_QUEST_FAILED:
			state.quests[key].failed = questDialogueEditorSandboxSeedBoolean;
			break;
		case QUEST_DIALOGUE_SANDBOX_QUEST_STAGE:
			state.quests[key].stage = questDialogueEditorSandboxSeedValue;
			break;
		case QUEST_DIALOGUE_SANDBOX_OBJECTIVE_COMPLETED:
			if ( subkey.empty() )
			{
				questDialogueEditorSetMessage(
					"Objective simulation also needs an objective ID.");
				return false;
			}
			state.quests[key].objectives[subkey] =
				questDialogueEditorSandboxSeedBoolean;
			break;
		case QUEST_DIALOGUE_SANDBOX_NODE_SEEN:
			if ( questDialogueEditorSandboxSeedBoolean ) state.seenNodes.insert(key);
			else state.seenNodes.erase(key);
			break;
		case QUEST_DIALOGUE_SANDBOX_CHOICE_USED:
			if ( questDialogueEditorSandboxSeedBoolean ) state.usedChoices.insert(key);
			else state.usedChoices.erase(key);
			break;
		default:
			return false;
	}
	questDialogueEditorSandboxActive = false;
	questDialogueEditorSetMessage(std::string("Configured preview state: ")
		+ questDialogueEditorSandboxSeedKindName() + " " + key + ".");
	return true;
}

static void questDialogueEditorDrawPreviewPage()
{
	const int top = suby1 + 48;
	const int bottom = suby2 - 34;
	const int mainLeft = questDialogueEditorDrawFileRail(subx1 + 8, top, bottom);
	const int right = subx2 - 8;
	drawDepressed(mainLeft, top, right, bottom);
	printTextFormattedColor(font8x8_bmp, mainLeft + 12, top + 9,
		makeColorRGB(128, 210, 255), "SANDBOX PREVIEW - NO LIVE WORLD MUTATION");
	printText(font8x8_bmp, mainLeft + 12, top + 25,
		"Quest, flag, variable, gold, item, once-choice, and node-seen changes stay in this session only.");
	int controlsY = top + 47;
	printTextFormatted(font8x8_bmp, mainLeft + 16, controlsY + 5,
		"Gold: %d", questDialogueEditorSandboxGold);
	if ( questDialogueEditorImmediateButton(mainLeft + 104, controlsY, 30, "-") )
		questDialogueEditorSandboxGold = std::max(0, questDialogueEditorSandboxGold - 10);
	if ( questDialogueEditorImmediateButton(mainLeft + 140, controlsY, 30, "+") )
		questDialogueEditorSandboxGold += 10;
	printText(font8x8_bmp, mainLeft + 188, controlsY + 5, "Item");
	drawDepressed(mainLeft + 226, controlsY, mainLeft + 356, controlsY + 19);
	printTextFormatted(font8x8_bmp, mainLeft + 232, controlsY + 6,
		"%.10s x%d%s", questDialogueEditorSandboxItem,
		questDialogueEditorSandboxItemCount,
		questDialogueEditorSandboxItemEditing ? "_" : "");
	if ( mousestatus[SDL_BUTTON_LEFT]
		&& omousex >= mainLeft + 226 && omousex < mainLeft + 356
		&& omousey >= controlsY && omousey < controlsY + 19 )
	{
		mousestatus[SDL_BUTTON_LEFT] = 0;
		questDialogueEditorEndTransientTextInput();
		questDialogueEditorSandboxItemEditing = true;
		inputstr = questDialogueEditorSandboxItem;
		inputlen = static_cast<int>(sizeof(questDialogueEditorSandboxItem) - 1);
		cursorflash = ticks;
		SDL_StartTextInput();
	}
	if ( questDialogueEditorImmediateButton(mainLeft + 366, controlsY, 30, "-") )
		questDialogueEditorSandboxItemCount = std::max(0,
			questDialogueEditorSandboxItemCount - 1);
	if ( questDialogueEditorImmediateButton(mainLeft + 402, controlsY, 30, "+") )
		++questDialogueEditorSandboxItemCount;
	if ( questDialogueEditorImmediateButton(right - 214, controlsY, 94, "START / RESET") )
	{
		questDialogueEditorEndTransientTextInput();
		questDialogueEditorRefreshValidation();
		if ( automatia::dialogue::hasErrors(questDialogueEditorValidationIssues) )
		{
			questDialogueEditorSandboxActive = false;
			questDialogueEditorSetMessage(
				"Preview blocked until validation errors are fixed.");
			questDialogueEditorWorkspace = QUEST_DIALOGUE_WORKSPACE_VALIDATION;
			return;
		}
		questDialogueEditorSandboxInitialState =
			questDialogueEditorSandboxConfiguredState;
		questDialogueEditorSandboxInitialState.gold = questDialogueEditorSandboxGold;
		if ( questDialogueEditorSandboxItem[0] && questDialogueEditorSandboxItemCount > 0 )
			questDialogueEditorSandboxInitialState.items[questDialogueEditorSandboxItem]
				= questDialogueEditorSandboxItemCount;
		std::string error;
		questDialogueEditorSandboxActive = questDialogueEditorSandbox.begin(
			questDialogueEditorDocument, questDialogueEditorSandboxInitialState, error);
		if ( !questDialogueEditorSandboxActive ) questDialogueEditorSetMessage(error);
	}
	if ( questDialogueEditorImmediateButton(right - 112, controlsY, 96, "VALIDATE FIRST") )
		questDialogueEditorWorkspace = QUEST_DIALOGUE_WORKSPACE_VALIDATION;

	const int seedY = controlsY + 27;
	if ( questDialogueEditorImmediateButton(mainLeft + 16, seedY, 126,
		questDialogueEditorSandboxSeedKindName()) )
	{
		questDialogueEditorEndTransientTextInput();
		questDialogueEditorSandboxSeedKind =
			static_cast<QuestDialogueSandboxSeedKind>(
				(static_cast<int>(questDialogueEditorSandboxSeedKind) + 1)
				% QUEST_DIALOGUE_SANDBOX_SEED_COUNT);
	}
	drawDepressed(mainLeft + 148, seedY, mainLeft + 292, seedY + 19);
	printTextFormatted(font8x8_bmp, mainLeft + 154, seedY + 6, "%.15s%s",
		questDialogueEditorSandboxSeedKey,
		questDialogueEditorSandboxSeedKeyEditing ? "_" : "");
	if ( mousestatus[SDL_BUTTON_LEFT]
		&& omousex >= mainLeft + 148 && omousex < mainLeft + 292
		&& omousey >= seedY && omousey < seedY + 19 )
	{
		mousestatus[SDL_BUTTON_LEFT] = 0;
		questDialogueEditorEndTransientTextInput();
		questDialogueEditorSandboxSeedKeyEditing = true;
		inputstr = questDialogueEditorSandboxSeedKey;
		inputlen = static_cast<int>(sizeof(questDialogueEditorSandboxSeedKey) - 1);
		cursorflash = ticks;
		SDL_StartTextInput();
	}
	const bool objectiveSeed = questDialogueEditorSandboxSeedKind
		== QUEST_DIALOGUE_SANDBOX_OBJECTIVE_COMPLETED;
	if ( objectiveSeed )
	{
		drawDepressed(mainLeft + 298, seedY, mainLeft + 420, seedY + 19);
		printTextFormatted(font8x8_bmp, mainLeft + 304, seedY + 6, "%.12s%s",
			questDialogueEditorSandboxSeedSubkey,
			questDialogueEditorSandboxSeedSubkeyEditing ? "_" : "");
		if ( mousestatus[SDL_BUTTON_LEFT]
			&& omousex >= mainLeft + 298 && omousex < mainLeft + 420
			&& omousey >= seedY && omousey < seedY + 19 )
		{
			mousestatus[SDL_BUTTON_LEFT] = 0;
			questDialogueEditorEndTransientTextInput();
			questDialogueEditorSandboxSeedSubkeyEditing = true;
			inputstr = questDialogueEditorSandboxSeedSubkey;
			inputlen = static_cast<int>(sizeof(questDialogueEditorSandboxSeedSubkey) - 1);
			cursorflash = ticks;
			SDL_StartTextInput();
		}
	}
	else
	{
		printTextFormattedColor(font8x8_bmp, mainLeft + 302, seedY + 6,
			makeColorRGB(150, 185, 220), "%zu configured",
			questDialogueEditorSandboxConfiguredState.worldFlags.size()
				+ questDialogueEditorSandboxConfiguredState.npcFlags.size()
				+ questDialogueEditorSandboxConfiguredState.worldVariables.size()
				+ questDialogueEditorSandboxConfiguredState.npcVariables.size()
				+ questDialogueEditorSandboxConfiguredState.quests.size()
				+ questDialogueEditorSandboxConfiguredState.seenNodes.size()
				+ questDialogueEditorSandboxConfiguredState.usedChoices.size());
	}
	if ( questDialogueEditorSandboxSeedUsesBoolean() )
	{
		if ( questDialogueEditorImmediateButton(mainLeft + 426, seedY, 58,
			questDialogueEditorSandboxSeedBoolean ? "TRUE" : "FALSE") )
			questDialogueEditorSandboxSeedBoolean =
				!questDialogueEditorSandboxSeedBoolean;
	}
	else
	{
		if ( questDialogueEditorImmediateButton(mainLeft + 426, seedY, 24, "-") )
			--questDialogueEditorSandboxSeedValue;
		printTextFormatted(font8x8_bmp, mainLeft + 454, seedY + 6, "%d",
			questDialogueEditorSandboxSeedValue);
		if ( questDialogueEditorImmediateButton(mainLeft + 482, seedY, 24, "+") )
			++questDialogueEditorSandboxSeedValue;
	}
	if ( questDialogueEditorImmediateButton(mainLeft + 512, seedY, 52, "SET") )
		questDialogueEditorApplySandboxSeed();
	if ( questDialogueEditorImmediateButton(mainLeft + 570, seedY, 82, "CLEAR ALL") )
	{
		questDialogueEditorEndTransientTextInput();
		questDialogueEditorSandboxConfiguredState =
			automatia::dialogue::PreviewState{};
		questDialogueEditorSandboxActive = false;
		questDialogueEditorSetMessage("Cleared configured preview state.");
	}

	if ( !questDialogueEditorSandboxActive )
	{
		printTextFormattedColor(font8x8_bmp, mainLeft + 18, controlsY + 74,
			makeColorRGB(180, 180, 180),
			"Seed any required state above, then START / RESET.");
		return;
	}
	const auto& frame = questDialogueEditorSandbox.frame();
	const auto& simulated = questDialogueEditorSandbox.state();
	int y = controlsY + 66;
	drawWindowFancy(mainLeft + 14, y, right - 14, y + 80);
	printTextFormattedColor(font8x8_bmp, mainLeft + 23, y + 8,
		makeColorRGB(255, 230, 96), "NPC - NODE %d", frame.nodeID);
	printTextFormatted(font8x8_bmp, mainLeft + 23, y + 26, "%.90s", frame.npcText.c_str());
	int itemCount = 0;
	for ( const auto& item : simulated.items ) itemCount += std::max(0, item.second);
	printTextFormattedColor(font8x8_bmp, mainLeft + 23, y + 44,
		makeColorRGB(128, 210, 255),
		"SIMULATED: %d gold | %d item%s | %zu world flag%s | %zu used response%s",
		simulated.gold, itemCount, itemCount == 1 ? "" : "s",
		simulated.worldFlags.size(), simulated.worldFlags.size() == 1 ? "" : "s",
		simulated.usedChoices.size(), simulated.usedChoices.size() == 1 ? "" : "s");
	const auto questState = simulated.quests.find(questDialogueEditorPreview.questID);
	if ( questState != simulated.quests.end() )
		printTextFormattedColor(font8x8_bmp, mainLeft + 23, y + 59,
			makeColorRGB(170, 230, 170), "QUEST %s: stage %d | %s%s%s",
			questDialogueEditorPreview.questID.c_str(), questState->second.stage,
			questState->second.accepted ? "accepted " : "",
			questState->second.completed ? "completed " : "",
			questState->second.failed ? "failed" : "");
	y += 90;
	for ( int index = 0; index < static_cast<int>(frame.choices.size())
		&& y + 61 < bottom; ++index )
	{
		const auto& choice = frame.choices[index];
		drawWindowFancy(mainLeft + 30, y, right - 30, y + 56);
		printTextFormatted(font8x8_bmp, mainLeft + 39, y + 7,
			"%d. %.76s  -> node %d", index + 1, choice.text.c_str(), choice.nextNode);
		if ( !choice.condition.empty() )
			printTextFormattedColor(font8x8_bmp, mainLeft + 39, y + 23,
				makeColorRGB(150, 200, 255), "Requires: %.70s", choice.condition.c_str());
		if ( !choice.actions.empty() )
			printTextFormattedColor(font8x8_bmp, mainLeft + 39, y + 38,
				makeColorRGB(255, 190, 110), "Actions: %.72s", choice.actions.front().c_str());
		if ( mousestatus[SDL_BUTTON_LEFT]
			&& omousex >= mainLeft + 30 && omousex < right - 30
			&& omousey >= y && omousey < y + 56 )
		{
			mousestatus[SDL_BUTTON_LEFT] = 0;
			std::string error;
			if ( !questDialogueEditorSandbox.choose(index, error) )
				questDialogueEditorSetMessage(error);
		}
		y += 62;
	}
	for ( const std::string& notice : frame.notices )
	{
		if ( y + 13 >= bottom ) break;
		printTextFormattedColor(font8x8_bmp, mainLeft + 22, y,
			makeColorRGB(255, 175, 96), "NOTICE: %.78s", notice.c_str());
		y += 14;
	}
}

static void questDialogueEditorNavigateToIssue(
	const automatia::dialogue::Issue& issue
)
{
	if ( issue.location.nodeIndex >= 0 )
		questDialogueEditorSelectedNode = issue.location.nodeIndex;
	if ( issue.location.choiceIndex >= 0 )
		questDialogueEditorSelectedChoice = issue.location.choiceIndex;
	if ( issue.location.objectiveIndex >= 0 )
		questDialogueEditorSelectedObjective = issue.location.objectiveIndex;
	if ( issue.location.kind == automatia::dialogue::LocationKind::Quest
		|| issue.location.kind == automatia::dialogue::LocationKind::Origin
		|| issue.location.kind == automatia::dialogue::LocationKind::Objective )
		questDialogueEditorWorkspace = QUEST_DIALOGUE_WORKSPACE_QUEST;
	else if ( issue.location.nodeIndex >= 0 )
		questDialogueEditorWorkspace = QUEST_DIALOGUE_WORKSPACE_CONVERSATION;
	else questDialogueEditorWorkspace = QUEST_DIALOGUE_WORKSPACE_JSON;
	questDialogueEditorSetMessage("Navigated to " + issue.location.path + ".");
}

static void questDialogueEditorDrawValidationPage()
{
	const int top = suby1 + 48;
	const int bottom = suby2 - 34;
	const int mainLeft = questDialogueEditorDrawFileRail(subx1 + 8, top, bottom);
	const int right = subx2 - 8;
	drawDepressed(mainLeft, top, right, bottom);
	const int errors = automatia::dialogue::countIssues(
		questDialogueEditorValidationIssues, automatia::dialogue::Severity::Error);
	const int warnings = automatia::dialogue::countIssues(
		questDialogueEditorValidationIssues, automatia::dialogue::Severity::Warning);
	const int infos = automatia::dialogue::countIssues(
		questDialogueEditorValidationIssues, automatia::dialogue::Severity::Info);
	printTextFormattedColor(font8x8_bmp, mainLeft + 12, top + 9,
		errors ? makeColorRGB(255, 110, 100) : makeColorRGB(128, 255, 160),
		"VALIDATION - %d ERROR%s, %d WARNING%s, %d INFO",
		errors, errors == 1 ? "" : "S", warnings, warnings == 1 ? "" : "S", infos);
	if ( questDialogueEditorImmediateButton(right - 108, top + 5, 92, "RUN AGAIN") )
		questDialogueEditorRefreshValidation();
	const int listTop = top + 37;
	const int rowHeight = 43;
	const int visible = std::max(1, (bottom - listTop - 8) / rowHeight);
	const int maximumScroll = std::max(0,
		static_cast<int>(questDialogueEditorValidationIssues.size()) - visible);
	if ( omousex >= mainLeft && omousex < right
		&& omousey >= listTop && omousey < bottom && scroll != 0 )
	{
		questDialogueEditorValidationScroll = std::max(0,
			std::min(maximumScroll, questDialogueEditorValidationScroll + scroll));
		scroll = 0;
	}
	questDialogueEditorValidationScroll = std::max(0,
		std::min(maximumScroll, questDialogueEditorValidationScroll));
	if ( questDialogueEditorValidationIssues.empty() )
	{
		printTextFormattedColor(font8x8_bmp, mainLeft + 20, listTop + 12,
			makeColorRGB(128, 255, 160), "No validation issues. This document is ready to save.");
		return;
	}
	for ( int row = 0; row < visible; ++row )
	{
		const int index = questDialogueEditorValidationScroll + row;
		if ( index >= static_cast<int>(questDialogueEditorValidationIssues.size()) ) break;
		const auto& issue = questDialogueEditorValidationIssues[index];
		const int y = listTop + row * rowHeight;
		drawWindowFancy(mainLeft + 10, y, right - 10, y + rowHeight - 4);
		const Uint32 color = issue.severity == automatia::dialogue::Severity::Error
			? makeColorRGB(255, 110, 100)
			: (issue.severity == automatia::dialogue::Severity::Warning
				? makeColorRGB(255, 210, 96) : makeColorRGB(130, 190, 255));
		printTextFormattedColor(font8x8_bmp, mainLeft + 18, y + 6, color,
			"%s  %s", automatia::dialogue::severityName(issue.severity),
			issue.location.path.c_str());
		printTextFormatted(font8x8_bmp, mainLeft + 18, y + 21, "%.91s",
			issue.message.c_str());
		if ( mousestatus[SDL_BUTTON_LEFT]
			&& omousex >= mainLeft + 10 && omousex < right - 10
			&& omousey >= y && omousey < y + rowHeight - 4 )
		{
			mousestatus[SDL_BUTTON_LEFT] = 0;
			questDialogueEditorNavigateToIssue(issue);
		}
	}
}

static void questDialogueEditorDrawWizard()
{
	const int width = std::min(760, subx2 - subx1 - 60);
	const int height = 410;
	const int left = (subx1 + subx2 - width) / 2;
	const int right = left + width;
	const int top = (suby1 + suby2 - height) / 2;
	const int bottom = top + height;
	drawWindowFancy(left, top, right, bottom);
	printTextFormattedColor(font8x8_bmp, left + 16, top + 14,
		makeColorRGB(128, 210, 255), "NEW DIALOGUE WIZARD - STEP %d OF 4",
		questDialogueEditorWizardStep + 1);
	const char* stepNames[] = { "IDENTITY", "CONVERSATION", "QUEST", "FINISH" };
	int stepX = left + 18;
	for ( int step = 0; step < 4; ++step )
	{
		if ( step == questDialogueEditorWizardStep )
			drawDepressed(stepX, top + 34, stepX + 154, top + 53);
		printTextFormattedColor(font8x8_bmp, stepX + 7, top + 40,
			step == questDialogueEditorWizardStep ? makeColorRGB(255, 230, 96)
				: makeColorRGB(145, 145, 145), "%d  %s", step + 1, stepNames[step]);
		stepX += 164;
	}
	int y = top + 70;
	const char* labels[] = {
		"Dialogue/File ID", "Quest ID", "First NPC line",
		"First response", "Quest title", "Quest summary"
	};
	char* values[] = {
		questDialogueEditorWizardDialogueID, questDialogueEditorWizardQuestID,
		questDialogueEditorWizardNPCText, questDialogueEditorWizardChoiceText,
		questDialogueEditorWizardQuestTitle, questDialogueEditorWizardQuestSummary
	};
	auto drawField = [&](const int field)
	{
		printTextFormatted(font8x8_bmp, left + 18, y + 6, "%s", labels[field]);
		const int fieldLeft = left + 218;
		const bool active = field == questDialogueEditorWizardField
			&& inputstr == values[field] && SDL_IsTextInputActive();
		if ( active )
			drawWindowFancy(fieldLeft, y, right - 24, y + 21);
		else drawDepressed(fieldLeft, y, right - 24, y + 21);
		printTextFormatted(font8x8_bmp, fieldLeft + 6, y + 7, "%.58s%s",
			values[field], active ? "_" : "");
		if ( mousestatus[SDL_BUTTON_LEFT]
			&& omousex >= fieldLeft && omousex < right - 24
			&& omousey >= y && omousey < y + 21 )
		{
			mousestatus[SDL_BUTTON_LEFT] = 0;
			questDialogueEditorFocusWizardField(field);
		}
		y += 34;
	};

	if ( questDialogueEditorWizardStep == 0 )
	{
		printText(font8x8_bmp, left + 18, y + 5, "Starting template");
		if ( questDialogueEditorImmediateButton(left + 180, y, 28, "<") )
		{
			questDialogueEditorWizardTemplate =
				(questDialogueEditorWizardTemplate + 4) % 5;
			if ( questDialogueEditorWizardTemplate == 3 )
				questDialogueEditorWizardUseQuest = true;
		}
		drawDepressed(left + 214, y, right - 64, y + 19);
		printTextFormatted(font8x8_bmp, left + 221, y + 6, "%s",
			questDialogueEditorWizardTemplateName());
		if ( questDialogueEditorImmediateButton(right - 56, y, 28, ">") )
		{
			questDialogueEditorWizardTemplate =
				(questDialogueEditorWizardTemplate + 1) % 5;
			if ( questDialogueEditorWizardTemplate == 3 )
				questDialogueEditorWizardUseQuest = true;
		}
		y += 36;
		drawField(0);
		const bool questRequired = questDialogueEditorWizardTemplate == 3;
		printText(font8x8_bmp, left + 18, y + 5, "Include a quest?");
		if ( questDialogueEditorImmediateButton(left + 218, y, 64, "NO",
			!questDialogueEditorWizardUseQuest) )
		{
			if ( questRequired ) questDialogueEditorSetMessage(
				"Quest Giver template requires quest metadata.");
			else questDialogueEditorWizardUseQuest = false;
		}
		if ( questDialogueEditorImmediateButton(left + 290, y, 64, "YES",
			questDialogueEditorWizardUseQuest) ) questDialogueEditorWizardUseQuest = true;
		y += 34;
		if ( questDialogueEditorWizardUseQuest || questRequired ) drawField(1);
		printTextFormattedColor(font8x8_bmp, left + 18, y + 7,
			makeColorRGB(170, 190, 210),
			"Creates: dialogue/%s.json",
			questEditorNormalizeID(questDialogueEditorWizardDialogueID).c_str());
		y += 21;
		printText(font8x8_bmp, left + 18, y,
			"The filename selects the resource. Quest ID is separate saved story identity.");
	}
	else if ( questDialogueEditorWizardStep == 1 )
	{
		printText(font8x8_bmp, left + 18, y,
			"Write the first exchange. The graph editor handles every later branch.");
		y += 28;
		drawField(2);
		if ( questDialogueEditorWizardTemplate != 0 ) drawField(3);
		else
		{
			printTextFormattedColor(font8x8_bmp, left + 18, y,
				makeColorRGB(170, 190, 210),
				"Empty Conversation starts with one node and no player response.");
			y += 24;
		}
		if ( questDialogueEditorWizardTemplate == 2
			|| questDialogueEditorWizardTemplate == 3
			|| questDialogueEditorWizardTemplate == 4 )
			printText(font8x8_bmp, left + 18, y,
				"This template also creates a second response and follow-up node.");
	}
	else if ( questDialogueEditorWizardStep == 2 )
	{
		const bool questRequired = questDialogueEditorWizardTemplate == 3;
		if ( !questDialogueEditorWizardUseQuest && !questRequired )
		{
			printTextFormattedColor(font8x8_bmp, left + 18, y,
				makeColorRGB(170, 190, 210), "CONVERSATION-ONLY RESOURCE");
			y += 24;
			printText(font8x8_bmp, left + 18, y,
				"No quest state or journal metadata will be created.");
			y += 30;
			if ( questDialogueEditorImmediateButton(left + 18, y, 148,
				"ENABLE QUEST") ) questDialogueEditorWizardUseQuest = true;
		}
		else
		{
			drawField(1);
			drawField(4);
			drawField(5);
			printText(font8x8_bmp, left + 18, y + 5, "Ownership scope");
			const char* scopeLabels[] = { "PERSONAL", "PARTY", "WORLD" };
			int scopeX = left + 218;
			for ( int scope = 0; scope < 3; ++scope )
			{
				if ( questDialogueEditorImmediateButton(scopeX, y, 76,
					scopeLabels[scope], questDialogueEditorWizardScope == scope) )
				{
					questDialogueEditorWizardScope = scope;
				}
				scopeX += 82;
			}
			y += 30;
			printText(font8x8_bmp, left + 18, y + 5, "Repeatable");
			if ( questDialogueEditorImmediateButton(left + 218, y, 82,
				questDialogueEditorWizardRepeatable ? "YES" : "NO",
				questDialogueEditorWizardRepeatable) )
				questDialogueEditorWizardRepeatable = !questDialogueEditorWizardRepeatable;
			y += 30;
			printText(font8x8_bmp, left + 18, y + 5, "Quest giver");
			const char* origins[] = { "NONE", "PICK AFTER CREATE", "SELECTED NPC" };
			int originX = left + 218;
			for ( int origin = 0; origin < 3; ++origin )
			{
				if ( questDialogueEditorImmediateButton(originX, y,
					origin == 0 ? 66 : 110, origins[origin],
					questDialogueEditorWizardOrigin == origin) )
					questDialogueEditorWizardOrigin = origin;
				originX += origin == 0 ? 74 : 118;
			}
		}
	}
	else
	{
		const bool useQuest = questDialogueEditorWizardUseQuest
			|| questDialogueEditorWizardTemplate == 3;
		printTextFormattedColor(font8x8_bmp, left + 18, y,
			makeColorRGB(128, 255, 160), "READY TO CREATE");
		y += 28;
		printTextFormatted(font8x8_bmp, left + 28, y,
			"File: dialogue/%s.json",
			questEditorNormalizeID(questDialogueEditorWizardDialogueID).c_str());
		y += 22;
		printTextFormatted(font8x8_bmp, left + 28, y, "Template: %s",
			questDialogueEditorWizardTemplateName());
		y += 22;
		printTextFormatted(font8x8_bmp, left + 28, y, "Quest: %s",
			useQuest ? questEditorNormalizeID(questDialogueEditorWizardQuestID).c_str()
				: "none (conversation only)");
		y += 22;
		if ( useQuest )
		{
			const char* scopeNames[] = { "Personal", "Party", "World" };
			printTextFormatted(font8x8_bmp, left + 28, y, "Ownership: %s (schema 2)",
				scopeNames[std::max(0, std::min(questDialogueEditorWizardScope, 2))]);
			y += 22;
		}
		const char* originNames[] = { "none", "pick a map tile after creation", "selected persistent NPC" };
		printTextFormatted(font8x8_bmp, left + 28, y, "Quest giver: %s",
			useQuest ? originNames[questDialogueEditorWizardOrigin] : "not applicable");
		y += 31;
		printText(font8x8_bmp, left + 18, y,
			"Create & Open validates the generated document, then publishes it atomically.");
	}

	printTextFormattedColor(font8x8_bmp, left + 18, bottom - 74,
		makeColorRGB(255, 210, 96),
		"Quest Giver adds quest actions; Recruitable NPC adds generic recruit_npc.");
	if ( questDialogueEditorWizardStep > 0
		&& questDialogueEditorImmediateButton(left + 18, bottom - 44, 82, "BACK") )
	{
		--questDialogueEditorWizardStep;
		SDL_StopTextInput();
		inputstr = nullptr;
	}
	if ( questDialogueEditorWizardStep < 3
		&& questDialogueEditorImmediateButton(right - 222, bottom - 44, 94, "NEXT") )
	{
		bool valid = true;
		if ( questDialogueEditorWizardStep == 0 )
		{
			const std::string dialogueID = questEditorNormalizeID(
				questDialogueEditorWizardDialogueID);
			const bool useQuest = questDialogueEditorWizardUseQuest
				|| questDialogueEditorWizardTemplate == 3;
			if ( dialogueID.empty() )
			{
				questDialogueEditorSetMessage("Dialogue/File ID cannot be empty.");
				valid = false;
			}
			else if ( access(("./dialogue/" + dialogueID + ".json").c_str(), F_OK) == 0 )
			{
				questDialogueEditorSetMessage("That dialogue file already exists.");
				valid = false;
			}
			else if ( useQuest
				&& questEditorNormalizeID(questDialogueEditorWizardQuestID).empty() )
			{
				questDialogueEditorSetMessage("Quest ID is required when quest mode is enabled.");
				valid = false;
			}
		}
		else if ( questDialogueEditorWizardStep == 1
			&& questDialogueEditorWizardNPCText[0] == '\0' )
		{
			questDialogueEditorSetMessage("The first NPC line cannot be empty.");
			valid = false;
		}
		else if ( questDialogueEditorWizardStep == 2
			&& (questDialogueEditorWizardUseQuest
				|| questDialogueEditorWizardTemplate == 3)
			&& questEditorNormalizeID(questDialogueEditorWizardQuestID).empty() )
		{
			questDialogueEditorSetMessage("Quest ID cannot be empty.");
			valid = false;
		}
		if ( valid )
		{
			++questDialogueEditorWizardStep;
			SDL_StopTextInput();
			inputstr = nullptr;
		}
	}
	if ( questDialogueEditorWizardStep == 3
		&& questDialogueEditorImmediateButton(right - 250, bottom - 44, 122,
			"CREATE & OPEN") ) questDialogueEditorCreateWizardFile();
	if ( questDialogueEditorImmediateButton(right - 116, bottom - 44, 88, "CANCEL") )
	{
		questDialogueEditorWizardOpen = false;
		SDL_StopTextInput();
		inputstr = nullptr;
	}
}

static void questDialogueEditorDrawUnsavedPrompt()
{
	const int width = 520;
	const int height = 154;
	const int left = (subx1 + subx2 - width) / 2;
	const int right = left + width;
	const int top = (suby1 + suby2 - height) / 2;
	const int bottom = top + height;
	drawWindowFancy(left, top, right, bottom);
	printTextFormattedColor(font8x8_bmp, left + 18, top + 16,
		makeColorRGB(255, 210, 96), "UNSAVED DIALOGUE CHANGES");
	printText(font8x8_bmp, left + 18, top + 40,
		"Save the current file before continuing, discard the in-memory edits, or cancel.");
	const int y = bottom - 43;
	if ( questDialogueEditorImmediateButton(left + 96, y, 92, "SAVE") )
	{
		const auto transition = questDialogueEditorPendingTransition;
		const int argument = transition == QUEST_DIALOGUE_PENDING_APPLY_TUTORIAL
			? questDialogueEditorPendingTutorial : questDialogueEditorPendingFile;
		if ( questDialogueEditorWriteDocument() )
		{
			questDialogueEditorUnsavedPrompt = false;
			questDialogueEditorPendingTransition = QUEST_DIALOGUE_PENDING_NONE;
			questDialogueEditorPerformTransition(transition, argument);
		}
	}
	if ( questDialogueEditorImmediateButton(left + 206, y, 92, "DISCARD") )
	{
		const auto transition = questDialogueEditorPendingTransition;
		const int argument = transition == QUEST_DIALOGUE_PENDING_APPLY_TUTORIAL
			? questDialogueEditorPendingTutorial : questDialogueEditorPendingFile;
		if ( transition != QUEST_DIALOGUE_PENDING_CLOSE
			&& questDialogueEditorSelectedFile >= 0
			&& questDialogueEditorSelectedFile
				< static_cast<int>(questDialogueEditorFiles.size()) )
		{
			questDialogueEditorLoadPreview(
				questDialogueEditorFiles[questDialogueEditorSelectedFile]);
			if ( !questDialogueEditorPreview.error.empty() )
			{
				questDialogueEditorSetMessage(
					"Could not reload the on-disk file; discard was canceled.");
				return;
			}
		}
		else questDialogueEditorModel.markClean();
		questDialogueEditorUnsavedPrompt = false;
		questDialogueEditorPendingTransition = QUEST_DIALOGUE_PENDING_NONE;
		questDialogueEditorPerformTransition(transition, argument);
	}
	if ( questDialogueEditorImmediateButton(left + 316, y, 92, "CANCEL") )
	{
		questDialogueEditorUnsavedPrompt = false;
		questDialogueEditorPendingTransition = QUEST_DIALOGUE_PENDING_NONE;
	}
}

static void questDialogueEditorDrawDeletePrompt()
{
	const int width = 520;
	const int height = 160;
	const int left = (subx1 + subx2 - width) / 2;
	const int right = left + width;
	const int top = (suby1 + suby2 - height) / 2;
	const int bottom = top + height;
	drawWindowFancy(left, top, right, bottom);
	const std::string filename = questDialogueEditorSelectedFile >= 0
		&& questDialogueEditorSelectedFile < static_cast<int>(questDialogueEditorFiles.size())
		? questDialogueEditorFiles[questDialogueEditorSelectedFile] : "(none)";
	printTextFormattedColor(font8x8_bmp, left + 18, top + 16,
		makeColorRGB(255, 110, 100), "DELETE DIALOGUE FILE?");
	printTextFormatted(font8x8_bmp, left + 18, top + 42,
		"This permanently removes ./dialogue/%s", filename.c_str());
	printText(font8x8_bmp, left + 18, top + 61,
		"NPC references are not rewritten. This operation cannot be undone here.");
	const int y = bottom - 43;
	if ( questDialogueEditorImmediateButton(left + 145, y, 106, "DELETE FILE") )
	{
		questDialogueEditorDeletePrompt = false;
		questDialogueEditorDeleteSelectedFileNow();
	}
	if ( questDialogueEditorImmediateButton(left + 269, y, 106, "CANCEL") )
		questDialogueEditorDeletePrompt = false;
}

static void drawQuestDialogueEditor()
{
	const bool blockedAtStart = questDialogueEditorUnsavedPrompt
		|| questDialogueEditorDeletePrompt || questDialogueEditorWizardOpen;
	const int pendingMouse = mousestatus[SDL_BUTTON_LEFT];
	if ( blockedAtStart ) mousestatus[SDL_BUTTON_LEFT] = 0;
	if ( !blockedAtStart && questDialogueEditorEditingField
		&& keystatus[SDLK_ESCAPE] )
	{
		keystatus[SDLK_ESCAPE] = 0;
		questDialogueEditorEndTransientTextInput();
		questDialogueEditorSetMessage("Field edit canceled; document was unchanged.");
	}
	else if ( !blockedAtStart && questDialogueEditorEditingField
		&& keystatus[SDLK_RETURN] )
	{
		keystatus[SDLK_RETURN] = 0;
		if ( questDialogueEditorLockedEditableField
			== QUEST_DIALOGUE_FIELD_FILE_ID )
			questDialogueEditorRenameSelectedFile();
		else questDialogueEditorApplyEditableField();
	}

	const int tabY = suby1 + 23;
	const char* tabLabels[] = {
		"FILES", "CONVERSATION", "QUEST", "TUTORIALS", "JSON", "PREVIEW", "VALIDATION"
	};
	const int tabWidths[] = { 58, 104, 54, 84, 48, 68, 84 };
	int tabX = subx1 + 8;
	for ( int mode = 0; mode < QUEST_DIALOGUE_WORKSPACE_COUNT; ++mode )
	{
		if ( questDialogueEditorImmediateButton(tabX, tabY, tabWidths[mode],
			tabLabels[mode], static_cast<int>(questDialogueEditorWorkspace) == mode) )
		{
			if ( questDialogueEditorJSONEditing
				&& questDialogueEditorWorkspace == QUEST_DIALOGUE_WORKSPACE_JSON
				&& mode != QUEST_DIALOGUE_WORKSPACE_JSON )
			{
				questDialogueEditorSetMessage(
					"Apply or cancel Advanced JSON before changing tabs.");
			}
			else if ( questDialogueEditorEditingField )
			{
				questDialogueEditorSetMessage(
					"Apply or cancel the active field before changing tabs.");
			}
			else
			{
				questDialogueEditorEndTransientTextInput();
				questDialogueEditorWorkspace = static_cast<QuestDialogueWorkspaceMode>(mode);
				if ( questDialogueEditorWorkspace == QUEST_DIALOGUE_WORKSPACE_QUEST )
					questDialogueEditorFieldCategory = QUEST_DIALOGUE_CATEGORY_OBJECTIVE;
				if ( questDialogueEditorWorkspace == QUEST_DIALOGUE_WORKSPACE_VALIDATION )
					questDialogueEditorRefreshValidation();
			}
		}
		tabX += tabWidths[mode] + 4;
	}

	int actionX = subx2 - 306;
	if ( questDialogueEditorImmediateButton(actionX, tabY, 56, "SAVE") )
	{
		if ( questDialogueEditorJSONEditing )
			questDialogueEditorSetMessage("Apply or cancel Advanced JSON before saving.");
		else if ( questDialogueEditorEditingField )
			questDialogueEditorSetMessage("Apply or cancel the active field before saving.");
		else questDialogueEditorWriteDocument();
	}
	actionX += 62;
	if ( questDialogueEditorImmediateButton(actionX, tabY, 56, "UNDO") )
	{
		if ( questDialogueEditorEditingField || questDialogueEditorJSONEditing )
			questDialogueEditorSetMessage("Apply or cancel the active edit before undo.");
		else questDialogueEditorUndo();
	}
	actionX += 62;
	if ( questDialogueEditorImmediateButton(actionX, tabY, 56, "REDO") )
	{
		if ( questDialogueEditorEditingField || questDialogueEditorJSONEditing )
			questDialogueEditorSetMessage("Apply or cancel the active edit before redo.");
		else questDialogueEditorRedo();
	}
	actionX += 62;
	if ( questDialogueEditorImmediateButton(actionX, tabY, 104, "RUN VALIDATION") )
	{
		if ( questDialogueEditorJSONEditing )
			questDialogueEditorSetMessage(
				"Apply or cancel Advanced JSON before running validation.");
		else
		{
			questDialogueEditorEndTransientTextInput();
			questDialogueEditorRefreshValidation();
			questDialogueEditorWorkspace = QUEST_DIALOGUE_WORKSPACE_VALIDATION;
		}
	}

	if ( !blockedAtStart )
	{
		const bool control = (SDL_GetModState() & KMOD_CTRL) != 0;
		if ( control && keystatus[SDLK_s] )
		{
			keystatus[SDLK_s] = 0;
			if ( questDialogueEditorEditingField || questDialogueEditorJSONEditing )
				questDialogueEditorSetMessage("Apply or cancel the active edit before saving.");
			else questDialogueEditorWriteDocument();
		}
		if ( control && keystatus[SDLK_z] )
		{
			keystatus[SDLK_z] = 0;
			if ( questDialogueEditorEditingField || questDialogueEditorJSONEditing )
				questDialogueEditorSetMessage("Apply or cancel the active edit before undo.");
			else questDialogueEditorUndo();
		}
		if ( control && keystatus[SDLK_y] )
		{
			keystatus[SDLK_y] = 0;
			if ( questDialogueEditorEditingField || questDialogueEditorJSONEditing )
				questDialogueEditorSetMessage("Apply or cancel the active edit before redo.");
			else questDialogueEditorRedo();
		}
	}

	switch ( questDialogueEditorWorkspace )
	{
		case QUEST_DIALOGUE_WORKSPACE_FILES:
			questDialogueEditorDrawFilesPage();
			break;
		case QUEST_DIALOGUE_WORKSPACE_CONVERSATION:
			questDialogueEditorDrawConversationPage();
			break;
		case QUEST_DIALOGUE_WORKSPACE_QUEST:
			questDialogueEditorDrawQuestPage();
			break;
		case QUEST_DIALOGUE_WORKSPACE_TUTORIALS:
			questDialogueEditorDrawTutorialsPage();
			break;
		case QUEST_DIALOGUE_WORKSPACE_JSON:
			questDialogueEditorDrawJSONPage();
			break;
		case QUEST_DIALOGUE_WORKSPACE_PREVIEW:
			questDialogueEditorDrawPreviewPage();
			break;
		case QUEST_DIALOGUE_WORKSPACE_VALIDATION:
			questDialogueEditorDrawValidationPage();
			break;
		default:
			break;
	}

	printTextFormattedColor(font8x8_bmp, subx2 - 292, suby2 - 21,
		questDialogueEditorModel.dirty()
			? makeColorRGB(255, 210, 96) : makeColorRGB(128, 255, 160),
		"%s | Ctrl+S save | Ctrl+Z/Y undo/redo",
		questDialogueEditorModel.dirty() ? "UNSAVED" : "SAVED");

	if ( blockedAtStart ) mousestatus[SDL_BUTTON_LEFT] = pendingMouse;
	if ( questDialogueEditorUnsavedPrompt ) questDialogueEditorDrawUnsavedPrompt();
	else if ( questDialogueEditorDeletePrompt ) questDialogueEditorDrawDeletePrompt();
	else if ( questDialogueEditorWizardOpen ) questDialogueEditorDrawWizard();
}

static std::string questEditorJsonEscape(const std::string& value)
{
	std::string escaped;
	escaped.reserve(value.size() + 16);

	for ( const char character : value )
	{
		switch ( character )
		{
			case '"':
				escaped += "\\\"";
				break;
			case '\\':
				escaped += "\\\\";
				break;
			case '\n':
				escaped += "\\n";
				break;
			case '\r':
				escaped += "\\r";
				break;
			default:
				escaped += character;
				break;
		}
	}

	return escaped;
}

static std::string questEditorNormalizeID(const std::string& value)
{
	std::string result;
	bool previousWasSeparator = false;

	for ( const unsigned char character : value )
	{
		if ( std::isalnum(character) )
		{
			result += static_cast<char>(std::tolower(character));
			previousWasSeparator = false;
		}
		else if ( !previousWasSeparator && !result.empty() )
		{
			result += '_';
			previousWasSeparator = true;
		}
	}

	while ( !result.empty() && result.back() == '_' )
	{
		result.pop_back();
	}

	return result;
}

static std::string questEditorCurrentMapFilename()
{
	std::string mapFilename = map.filename;

	if ( mapFilename.empty() )
	{
		mapFilename = map.name;
	}

	const size_t slash = mapFilename.find_last_of("/\\");
	if ( slash != std::string::npos )
	{
		mapFilename = mapFilename.substr(slash + 1);
	}

	if ( mapFilename.empty() )
	{
		mapFilename = "unnamed.lmp";
	}
	else if ( mapFilename.find('.') == std::string::npos )
	{
		mapFilename += ".lmp";
	}

	return mapFilename;
}

static bool questEditorWriteStarterJSON(
	const char* dialogueIDText,
	const char* giverLabelText,
	const Entity* giverEntity,
	const bool createMarkers
)
{
	if ( !dialogueIDText || !giverEntity )
	{
		questEditorStatusMessage = "No NPC or dialogue ID.";
		questEditorStatusUntil = ticks + TICKS_PER_SECOND * 4;
		return false;
	}

	const std::string dialogueID =
		questEditorNormalizeID(dialogueIDText);

	if ( dialogueID.empty() )
	{
		questEditorStatusMessage = "Enter a Custom Dialogue ID first.";
		questEditorStatusUntil = ticks + TICKS_PER_SECOND * 4;
		return false;
	}

	std::string giverLabel =
		giverLabelText ? giverLabelText : "";

	if ( giverLabel.empty() )
	{
		giverLabel = "Quest Giver";
	}

	const std::string mapFilename =
		questEditorCurrentMapFilename();

	const int giverX =
		std::max(0, static_cast<int>(floor(giverEntity->x / 16.0)));

	const int giverY =
		std::max(0, static_cast<int>(floor(giverEntity->y / 16.0)));

	const std::string objectiveID =
		dialogueID + "_objective";

	const std::string path =
		"./dialogue/" + dialogueID + ".json";

	mkdir("./dialogue", 0755);
	if ( access(path.c_str(), F_OK) == 0 )
	{
		questEditorStatusMessage = path + " already exists; existing data was not replaced.";
		questEditorStatusUntil = ticks + TICKS_PER_SECOND * 5;
		return false;
	}

	std::ostringstream output;

	output
		<< "{\n"
		<< "  \"version\": "
		<< automatia::dialogue::SchemaVersion << ",\n"
		<< "  \"quest_id\": \""
		<< questEditorJsonEscape(dialogueID)
		<< "\",\n"
		<< "  \"quest\": {\n"
		<< "    \"title\": \"New Quest\",\n"
		<< "    \"summary\": \"Describe the quest here.\",\n"
		<< "    \"objective\": \"Complete the objective.\",\n"
		<< "    \"scope\": \"player\",\n"
		<< "    \"repeatable\": false,\n"
		<< "    \"origin\": {\n"
		<< "      \"label\": \""
		<< questEditorJsonEscape(giverLabel)
		<< "\",\n"
		<< "      \"map\": \""
		<< questEditorJsonEscape(mapFilename)
		<< "\"";

	if ( createMarkers )
	{
		output
			<< ",\n"
			<< "      \"x\": " << giverX << ",\n"
			<< "      \"y\": " << giverY << "\n";
	}
	else
	{
		output << "\n";
	}

	output
		<< "    },\n"
		<< "    \"objectives\": [\n"
		<< "      {\n"
		<< "        \"id\": \""
		<< questEditorJsonEscape(objectiveID)
		<< "\",\n"
		<< "        \"text\": \"Complete the objective.\",\n"
		<< "        \"stage\": 0,\n"
		<< "        \"optional\": false";

	if ( createMarkers )
	{
		output
			<< ",\n"
			<< "        \"map_marker\": {\n"
			<< "          \"map\": \""
			<< questEditorJsonEscape(mapFilename)
			<< "\",\n"
			<< "          \"x\": " << giverX << ",\n"
			<< "          \"y\": " << giverY << "\n"
			<< "        }\n";
	}
	else
	{
		output << "\n";
	}

	output
		<< "      }\n"
		<< "    ]\n"
		<< "  },\n"
		<< "  \"start_node\": 0,\n"
		<< "  \"nodes\": [\n"
		<< "    {\n"
		<< "      \"id\": 0,\n"
		<< "      \"text\": \"Will you help me?\",\n"
		<< "      \"choices\": [\n"
		<< "        {\n"
		<< "          \"id\": \"accept\",\n"
		<< "          \"text\": \"I'll help.\",\n"
		<< "          \"next\": 1,\n"
		<< "          \"once\": true,\n"
		<< "          \"action\": {\n"
		<< "            \"quest_start\": true,\n"
		<< "            \"quest_accept\": true\n"
		<< "          }\n"
		<< "        },\n"
		<< "        {\n"
		<< "          \"id\": \"decline\",\n"
		<< "          \"text\": \"Not right now.\",\n"
		<< "          \"next\": 0\n"
		<< "        }\n"
		<< "      ]\n"
		<< "    },\n"
		<< "    {\n"
		<< "      \"id\": 1,\n"
		<< "      \"text\": \"Thank you. Check your journal and map.\",\n"
		<< "      \"next\": 1\n"
		<< "    }\n"
		<< "  ]\n"
		<< "}\n";

	automatia::dialogue::Document created;
	std::string error;
	if ( !created.parse(output.str(), error)
		|| !created.saveAtomic(path, error) )
	{
		questEditorStatusMessage = error.empty()
			? "Could not write " + path : error;
		questEditorStatusUntil = ticks + TICKS_PER_SECOND * 5;
		return false;
	}

	questEditorStatusMessage =
		"Wrote " + path;

	questEditorStatusUntil =
		ticks + TICKS_PER_SECOND * 5;

	printlog(
		"[Quest Editor] Wrote starter quest JSON '%s'.",
		path.c_str()
	);

	return true;
}
// Structural ownership is explicit as of V4.9/ELYR. Editor selection/copy
// must not infer a sprite layer from model-height Z because decoration offsets
// and runtime-local Z are independent of the authored layer.
static int entityAuthoredSpriteLayer(const Entity* entity)
{
	if ( !entity )
	{
		return 0;
	}
	return std::clamp(
		static_cast<int>(entity->authoredMapLayer), 0, MAPLAYERS - 1);
}

static void clearRoomClipboard()
{
	if ( roomClipboardMap.tiles )
	{
		free(roomClipboardMap.tiles);
		roomClipboardMap.tiles = nullptr;
	}

	list_FreeAll(&roomClipboardEntityList);
	roomClipboardEntityList.first = nullptr;
	roomClipboardEntityList.last = nullptr;

	roomClipboardMap.entities = &roomClipboardEntityList;
	roomClipboardMap.width = 0;
	roomClipboardMap.height = 0;
	roomClipboardMap.numLayers = 0;
	authoredRoomGroupsReset(roomClipboardMap.roomGroups);

	roomClipboardReady = false;
	roomClipboardHasTiles = false;
	roomClipboardHasSprites = false;
	roomClipboardWidth = 0;
	roomClipboardHeight = 0;
	roomClipboardDepth = 0;
	roomClipboardEntityCount = 0;
}

static void normalizeRoomSelection()
{
	const int oldX1 = selectedarea_x1;
	const int oldY1 = selectedarea_y1;

	selectedarea_x1 =
		std::max(
			0,
			std::min(
				std::min(oldX1, selectedarea_x2),
				static_cast<int>(map.width) - 1
			)
		);
	selectedarea_x2 =
		std::max(
			0,
			std::min(
				std::max(oldX1, selectedarea_x2),
				static_cast<int>(map.width) - 1
			)
		);

	selectedarea_y1 =
		std::max(
			0,
			std::min(
				std::min(oldY1, selectedarea_y2),
				static_cast<int>(map.height) - 1
			)
		);
	selectedarea_y2 =
		std::max(
			0,
			std::min(
				std::max(oldY1, selectedarea_y2),
				static_cast<int>(map.height) - 1
			)
		);

	const int oldBottom = roomSelectBottomLayer;
	roomSelectBottomLayer =
		std::max(
			0,
			std::min(
				std::min(oldBottom, roomSelectTopLayer),
				std::max(
					0,
					static_cast<int>(map.numLayers) - 1
				)
			)
		);
	roomSelectTopLayer =
		std::max(
			roomSelectBottomLayer,
			std::min(
				std::max(oldBottom, roomSelectTopLayer),
				std::max(
					0,
					static_cast<int>(map.numLayers) - 1
				)
			)
		);
}

void editorRoomCopySelection()
{
	if ( !selectedarea || pasting )
	{
		return;
	}

	normalizeRoomSelection();
	clearRoomClipboard();

	roomClipboardWidth =
		selectedarea_x2 - selectedarea_x1 + 1;
	roomClipboardHeight =
		selectedarea_y2 - selectedarea_y1 + 1;
	roomClipboardDepth =
		roomSelectTopLayer - roomSelectBottomLayer + 1;

	roomClipboardMap.width = roomClipboardWidth;
	roomClipboardMap.height = roomClipboardHeight;
	roomClipboardMap.numLayers = roomClipboardDepth;
	roomClipboardMap.entities = &roomClipboardEntityList;
	authoredRoomGroupsReset(roomClipboardMap.roomGroups);

	roomClipboardHasTiles =
		roomCopyContentMode == ROOM_COPY_BOTH
		|| roomCopyContentMode == ROOM_COPY_TILES;
	roomClipboardHasSprites =
		roomCopyContentMode == ROOM_COPY_BOTH
		|| roomCopyContentMode == ROOM_COPY_SPRITES;

	if ( roomClipboardHasTiles )
	{
		const size_t tileCount =
			static_cast<size_t>(roomClipboardWidth)
			* static_cast<size_t>(roomClipboardHeight)
			* MAPLAYERS;

		roomClipboardMap.tiles =
			static_cast<Sint32*>(
				malloc(sizeof(Sint32) * tileCount)
			);

		if ( !roomClipboardMap.tiles )
		{
			clearRoomClipboard();
			return;
		}

		memset(
			roomClipboardMap.tiles,
			0,
			sizeof(Sint32) * tileCount
		);

		for ( int localX = 0; localX < roomClipboardWidth; ++localX )
		{
			for ( int localY = 0; localY < roomClipboardHeight; ++localY )
			{
				for ( int localLayer = 0; localLayer < roomClipboardDepth; ++localLayer )
				{
					const int sourceLayer =
						roomSelectBottomLayer + localLayer;

					const int sourceIndex =
						sourceLayer
						+ (selectedarea_y1 + localY) * MAPLAYERS
						+ (selectedarea_x1 + localX)
							* MAPLAYERS * map.height;

					const int destinationIndex =
						localLayer
						+ localY * MAPLAYERS
						+ localX
							* MAPLAYERS * roomClipboardHeight;

					/*
					 * Air is copied deliberately so empty interior
					 * spaces and openings carve the destination.
					 */
					roomClipboardMap.tiles[destinationIndex] =
						map.tiles[sourceIndex];
				}
			}
		}
	}

	roomClipboardEntityCount = 0;

	if ( roomClipboardHasSprites )
	{
		for ( node_t* node = map.entities->first;
			node;
			node = node->next )
		{
			Entity* source =
				static_cast<Entity*>(node->element);

			if ( !source )
			{
				continue;
			}

			const int sourceLayer =
				entityAuthoredSpriteLayer(source);
			const int sourceTileX =
				static_cast<int>(source->x / 16);
			const int sourceTileY =
				static_cast<int>(source->y / 16);

			if ( sourceTileX < selectedarea_x1
				|| sourceTileX > selectedarea_x2
				|| sourceTileY < selectedarea_y1
				|| sourceTileY > selectedarea_y2
				|| sourceLayer < roomSelectBottomLayer
				|| sourceLayer > roomSelectTopLayer )
			{
				continue;
			}

			Entity* snapshot =
				newEntity(
					source->sprite,
					0,
					&roomClipboardEntityList,
					nullptr
				);

			if ( !snapshot )
			{
				continue;
			}

			setSpriteAttributes(
				snapshot,
				source,
				source
			);

			snapshot->x =
				source->x - selectedarea_x1 * 16;
			snapshot->y =
				source->y - selectedarea_y1 * 16;
			const int clipboardLocalLayer =
				sourceLayer - roomSelectBottomLayer;
			snapshot->authoredMapLayer =
				static_cast<Sint16>(clipboardLocalLayer);
			/*
			 * Normalize gameplay membership along with the clipboard-relative
			 * authored layer. Otherwise an upper ordinary sprite pasted back to
			 * layer 0 can retain stale EFLR membership from its source layer.
			 * Preserve explicit legacy FLOR membership only for genuine authored
			 * layer-0 content.
			 */
			if ( sourceLayer > 0
				|| source->verticalLayerTransitionDelta != 0 )
			{
				snapshot->playableFloor =
					source->verticalLayerTransitionDelta != 0
						? static_cast<PlayableFloorId>(
							std::max(0, clipboardLocalLayer - 1))
						: static_cast<PlayableFloorId>(clipboardLocalLayer);
			}
			snapshot->persistentID = 0;

			++roomClipboardEntityCount;
		}
	}

	/*
	 * Named groups are clipboard metadata. Only fully-contained groups are
	 * copied so a partial cuboid never silently changes a room definition.
	 * The clipboard stores X/Y/authored-layer-relative bounds and never encodes
	 * Entity::z or playableFloor; the existing sprite clipboard path remains
	 * responsible for relocating those entity properties.
	 */
	const std::uint8_t copiedContentMask = roomCopyContentMask();
	for ( std::uint32_t i = 0; i < map.roomGroups.count; ++i )
	{
		const AuthoredRoomGroup& sourceGroup = map.roomGroups.entries[i];
		if ( !authoredRoomGroupFullyInside(sourceGroup,
			selectedarea_x1, selectedarea_y1,
			selectedarea_x2, selectedarea_y2,
			static_cast<std::int16_t>(roomSelectBottomLayer),
			static_cast<std::int16_t>(roomSelectTopLayer)) )
		{
			continue;
		}
		AuthoredRoomGroup relative = authoredRoomGroupTranslated(sourceGroup,
			-selectedarea_x1, -selectedarea_y1,
			static_cast<std::int16_t>(-roomSelectBottomLayer));
		relative.contentMask &= copiedContentMask;
		if ( relative.contentMask != 0 )
		{
			authoredRoomGroupAdd(roomClipboardMap.roomGroups, relative.name,
				relative.x1, relative.y1, relative.x2, relative.y2,
				relative.bottomLayer, relative.topLayer,
				relative.contentMask);
		}
	}

	roomClipboardReady =
		roomClipboardHasTiles
		|| roomClipboardHasSprites;
	roomSelectStage = 3;
}

void editorRoomBeginPaste()
{
	if ( !roomClipboardReady )
	{
		return;
	}

	pasting = true;
	selectedarea = false;
	selectingspace = false;
	roomSelectStage = 4;
}

void editorRoomCancelPaste()
{
	pasting = false;
	roomSelectStage =
		roomClipboardReady ? 3 : 0;
}

void editorRoomPlaceClipboard(
	int destinationX,
	int destinationY,
	int destinationBottomLayer
)
{
	if ( !roomClipboardReady )
	{
		return;
	}
	if ( map.roomGroups.count + roomClipboardMap.roomGroups.count
		> AUTHORED_ROOM_GROUP_MAX_COUNT )
	{
		std::snprintf(message, sizeof(message),
			"Paste needs %u free Room Group slot(s).",
			roomClipboardMap.roomGroups.count);
		messagetime = 60;
		return;
	}
	for ( std::uint32_t i = 0; i < roomClipboardMap.roomGroups.count; ++i )
	{
		const AuthoredRoomGroup placed = authoredRoomGroupTranslated(
			roomClipboardMap.roomGroups.entries[i], destinationX, destinationY,
			static_cast<std::int16_t>(destinationBottomLayer));
		if ( placed.x1 < 0 || placed.y1 < 0
			|| placed.x2 >= static_cast<std::int32_t>(map.width)
			|| placed.y2 >= static_cast<std::int32_t>(map.height)
			|| placed.bottomLayer < 0
			|| placed.topLayer >= static_cast<std::int16_t>(map.numLayers) )
		{
			std::snprintf(message, sizeof(message),
				"Named Room Groups must fit completely inside the map.");
			messagetime = 60;
			return;
		}
	}

	makeUndo();

	if ( roomClipboardHasTiles
		&& roomClipboardMap.tiles )
	{
		for ( int localX = 0; localX < roomClipboardWidth; ++localX )
		{
			for ( int localY = 0; localY < roomClipboardHeight; ++localY )
			{
				const int destinationTileX =
					destinationX + localX;
				const int destinationTileY =
					destinationY + localY;

				if ( destinationTileX < 0
					|| destinationTileX >= static_cast<int>(map.width)
					|| destinationTileY < 0
					|| destinationTileY >= static_cast<int>(map.height) )
				{
					continue;
				}

				for ( int localLayer = 0;
					localLayer < roomClipboardDepth;
					++localLayer )
				{
					const int destinationLayer =
						destinationBottomLayer + localLayer;

					if ( destinationLayer < 0
						|| destinationLayer >= MAPLAYERS
						|| destinationLayer
							>= static_cast<int>(map.numLayers) )
					{
						continue;
					}

					const int sourceIndex =
						localLayer
						+ localY * MAPLAYERS
						+ localX
							* MAPLAYERS * roomClipboardHeight;

					const int destinationIndex =
						destinationLayer
						+ destinationTileY * MAPLAYERS
						+ destinationTileX
							* MAPLAYERS * map.height;

					map.tiles[destinationIndex] =
						roomClipboardMap.tiles[sourceIndex];
				}
			}
		}
	}

	if ( roomClipboardHasSprites )
	{
		for ( node_t* node =
				roomClipboardEntityList.first;
			node;
			node = node->next )
		{
			Entity* snapshot =
				static_cast<Entity*>(node->element);

			if ( !snapshot )
			{
				continue;
			}

			const int localLayer =
				entityAuthoredSpriteLayer(snapshot);
			const int destinationLayer =
				destinationBottomLayer + localLayer;
			const int destinationTileX =
				destinationX
					+ static_cast<int>(snapshot->x / 16);
			const int destinationTileY =
				destinationY
					+ static_cast<int>(snapshot->y / 16);

			if ( destinationTileX < 0
				|| destinationTileX >= static_cast<int>(map.width)
				|| destinationTileY < 0
				|| destinationTileY >= static_cast<int>(map.height)
				|| destinationLayer < 0
				|| destinationLayer
					>= static_cast<int>(map.numLayers) )
			{
				continue;
			}

			Entity* pastedEntity =
				newEntity(
					snapshot->sprite,
					0,
					map.entities,
					nullptr
				);

			if ( !pastedEntity )
			{
				continue;
			}

			setSpriteAttributes(
				pastedEntity,
				snapshot,
				snapshot
			);

			pastedEntity->x =
				destinationX * 16 + snapshot->x;
			pastedEntity->y =
				destinationY * 16 + snapshot->y;
			pastedEntity->authoredMapLayer = static_cast<Sint16>(destinationLayer);
			if ( destinationLayer > 0
				|| pastedEntity->verticalLayerTransitionDelta != 0 )
			{
				pastedEntity->playableFloor =
					pastedEntity->verticalLayerTransitionDelta != 0
						? static_cast<PlayableFloorId>(
							std::max(0, destinationLayer - 1))
						: static_cast<PlayableFloorId>(destinationLayer);
			}
			pastedEntity->persistentID = 0;
		}
	}

	for ( std::uint32_t i = 0; i < roomClipboardMap.roomGroups.count; ++i )
	{
		const AuthoredRoomGroup placed = authoredRoomGroupTranslated(
			roomClipboardMap.roomGroups.entries[i], destinationX, destinationY,
			static_cast<std::int16_t>(destinationBottomLayer));
		if ( placed.x1 < 0 || placed.y1 < 0
			|| placed.x2 >= static_cast<std::int32_t>(map.width)
			|| placed.y2 >= static_cast<std::int32_t>(map.height)
			|| placed.bottomLayer < 0
			|| placed.topLayer >= static_cast<std::int16_t>(map.numLayers) )
		{
			continue;
		}
		authoredRoomGroupAdd(map.roomGroups, placed.name,
			placed.x1, placed.y1, placed.x2, placed.y2,
			placed.bottomLayer, placed.topLayer, placed.contentMask);
	}

	pasting = false;
	roomSelectStage = 5;

	selectedarea_x1 = destinationX;
	selectedarea_y1 = destinationY;
	selectedarea_x2 =
		destinationX + roomClipboardWidth - 1;
	selectedarea_y2 =
		destinationY + roomClipboardHeight - 1;
	roomSelectBottomLayer =
		destinationBottomLayer;
	roomSelectTopLayer =
		std::min(
			MAPLAYERS - 1,
			destinationBottomLayer
				+ roomClipboardDepth - 1
		);
	selectedarea = true;
	reselectEntityGroup();
}

void editorRoomDeleteSelection()
{
	if ( !selectedarea )
	{
		return;
	}

	normalizeRoomSelection();
	makeUndo();

	const bool deleteTiles =
		roomCopyContentMode == ROOM_COPY_BOTH
		|| roomCopyContentMode == ROOM_COPY_TILES;
	const bool deleteSprites =
		roomCopyContentMode == ROOM_COPY_BOTH
		|| roomCopyContentMode == ROOM_COPY_SPRITES;
	const std::uint8_t deletedContentMask = roomCopyContentMask();

	if ( deleteTiles )
	{
		for ( int x = selectedarea_x1;
			x <= selectedarea_x2;
			++x )
		{
			for ( int y = selectedarea_y1;
				y <= selectedarea_y2;
				++y )
			{
				for ( int layer = roomSelectBottomLayer;
					layer <= roomSelectTopLayer;
					++layer )
				{
					map.tiles[
						layer
						+ y * MAPLAYERS
						+ x * MAPLAYERS * map.height
					] = 0;
				}
			}
		}
	}

	if ( deleteSprites )
	{
		node_t* next = nullptr;

		for ( node_t* node = map.entities->first;
			node;
			node = next )
		{
			next = node->next;

			Entity* entity =
				static_cast<Entity*>(node->element);

			if ( !entity )
			{
				continue;
			}

			const int entityLayer =
				entityAuthoredSpriteLayer(entity);
			const int entityX =
				static_cast<int>(entity->x / 16);
			const int entityY =
				static_cast<int>(entity->y / 16);

			if ( entityX >= selectedarea_x1
				&& entityX <= selectedarea_x2
				&& entityY >= selectedarea_y1
				&& entityY <= selectedarea_y2
				&& entityLayer >= roomSelectBottomLayer
				&& entityLayer <= roomSelectTopLayer )
			{
				if ( selectedEntity[0] == entity )
				{
					selectedEntity[0] = nullptr;
					lastSelectedEntity[0] = nullptr;
				}

				list_RemoveNode(node);
			}
		}
	}

	for ( std::uint32_t i = 0; i < map.roomGroups.count; )
	{
		AuthoredRoomGroup& group = map.roomGroups.entries[i];
		if ( !authoredRoomGroupFullyInside(group,
			selectedarea_x1, selectedarea_y1,
			selectedarea_x2, selectedarea_y2,
			static_cast<std::int16_t>(roomSelectBottomLayer),
			static_cast<std::int16_t>(roomSelectTopLayer)) )
		{
			++i;
			continue;
		}
		group.contentMask &= static_cast<std::uint8_t>(~deletedContentMask);
		if ( group.contentMask == 0 )
		{
			authoredRoomGroupRemove(map.roomGroups, group.id);
		}
		else
		{
			++i;
		}
	}

	groupedEntities.clear();
	selectedarea = false;
	roomSelectStage = 6;
}

static const char* roomSelectStageText()
{
	switch ( roomSelectStage )
	{
		case 0:
			return "Click and Drag";
		case 1:
			return "Dragging Area";
		case 2:
			return "Area Selected";
		case 3:
			return "Ready to Paste";
		case 4:
			return "Click to Place";
		case 5:
			return "Room Placed";
		case 6:
			return "Area Deleted";
		default:
			return "Select Area";
	}
}

static int selectedRoomGroupIndex()
{
	return authoredRoomGroupFindByID(map.roomGroups, roomGroupSelectedID);
}

static void roomGroupSetStatus(const char* text)
{
	std::snprintf(roomGroupStatusText, sizeof(roomGroupStatusText), "%s",
		text ? text : "");
	roomGroupStatusUntil = ticks + TICKS_PER_SECOND * 4;
}

static void roomGroupLoadEditorFields(const AuthoredRoomGroup& group)
{
	roomGroupSelectedID = group.id;
	std::snprintf(roomGroupNameText, sizeof(roomGroupNameText), "%s", group.name);
	if ( group.contentMask == AUTHORED_ROOM_GROUP_TILES )
	{
		roomCopyContentMode = ROOM_COPY_TILES;
	}
	else if ( group.contentMask == AUTHORED_ROOM_GROUP_SPRITES )
	{
		roomCopyContentMode = ROOM_COPY_SPRITES;
	}
	else
	{
		roomCopyContentMode = ROOM_COPY_BOTH;
	}
	inputstr = roomGroupNameText;
	inputlen = static_cast<int>(sizeof(roomGroupNameText) - 1);
	cursorflash = ticks;
}

static bool roomGroupApplyBoundsToSelection()
{
	const int index = selectedRoomGroupIndex();
	if ( index < 0 )
	{
		roomGroupSetStatus("Choose a Room Group first.");
		return false;
	}
	const AuthoredRoomGroup& group = map.roomGroups.entries[index];
	selectedTool = 3;
	selectedarea_x1 = group.x1;
	selectedarea_y1 = group.y1;
	selectedarea_x2 = group.x2;
	selectedarea_y2 = group.y2;
	roomSelectBottomLayer = group.bottomLayer;
	roomSelectTopLayer = group.topLayer;
	selectedarea = true;
	selectingspace = false;
	roomSelectStage = 2;
	roomGroupLoadEditorFields(group);
	reselectEntityGroup();
	return true;
}

static void openRoomGroupManager()
{
	menuVisible = 0;
	subwindow = 1;
	newwindow = 42;
	openwindow = 0;
	savewindow = 0;
	subx1 = std::max(16, xres / 2 - 330);
	subx2 = std::min(xres - 16, xres / 2 + 330);
	suby1 = std::max(24, yres / 2 - 230);
	suby2 = std::min(yres - 16, yres / 2 + 230);
	std::snprintf(subtext, sizeof(subtext), "Room Groups:");

	const int selectedIndex = selectedRoomGroupIndex();
	if ( selectedIndex >= 0 )
	{
		roomGroupLoadEditorFields(map.roomGroups.entries[selectedIndex]);
	}
	else
	{
		roomGroupSelectedID = 0;
		std::snprintf(roomGroupNameText, sizeof(roomGroupNameText), "Room Group");
		inputstr = roomGroupNameText;
		inputlen = static_cast<int>(sizeof(roomGroupNameText) - 1);
	}
	roomGroupListScroll = std::clamp(roomGroupListScroll, 0,
		std::max(0, static_cast<int>(map.roomGroups.count) - 1));
	roomGroupStatusText[0] = '\0';
	cursorflash = ticks;
	SDL_StartTextInput();

	button_t* closeButton = newButton();
	std::snprintf(closeButton->label, sizeof(closeButton->label), "Close");
	closeButton->x = subx2 - 64;
	closeButton->y = suby2 - 24;
	closeButton->sizex = 56;
	closeButton->sizey = 16;
	closeButton->action = &buttonCloseSubwindow;
	closeButton->visible = 1;
	closeButton->focused = 1;

	button_t* closeX = newButton();
	std::snprintf(closeX->label, sizeof(closeX->label), "X");
	closeX->x = subx2 - 16;
	closeX->y = suby1;
	closeX->sizex = 16;
	closeX->sizey = 16;
	closeX->action = &buttonCloseSubwindow;
	closeX->visible = 1;
	closeX->focused = 1;
}

static void drawRoomGroupManager()
{
	const int left = subx1 + 16;
	const int right = subx2 - 16;
	printText(font8x8_bmp, left, suby1 + 28,
		"Named cuboids retain X/Y and authored layers; sprite local Z is unchanged.");
	printText(font8x8_bmp, left, suby1 + 44, "Name:");
	drawDepressed(left + 48, suby1 + 40, right - 170, suby1 + 58);
	printText(font8x8_bmp, left + 52, suby1 + 45, roomGroupNameText);

	const char* contentLabel = roomCopyContentMode == ROOM_COPY_TILES
		? "CONTENT: TILES"
		: (roomCopyContentMode == ROOM_COPY_SPRITES
			? "CONTENT: SPRITES" : "CONTENT: BOTH");
	drawWindowFancy(right - 158, suby1 + 40, right, suby1 + 58);
	printText(font8x8_bmp, right - 152, suby1 + 45, contentLabel);

	auto clicked = [](const int x1, const int y1, const int x2, const int y2)
	{
		if ( mousestatus[SDL_BUTTON_LEFT]
			&& omousex >= x1 && omousex < x2
			&& omousey >= y1 && omousey < y2 )
		{
			mousestatus[SDL_BUTTON_LEFT] = 0;
			return true;
		}
		return false;
	};

	if ( clicked(left + 48, suby1 + 40, right - 170, suby1 + 58) )
	{
		inputstr = roomGroupNameText;
		inputlen = static_cast<int>(sizeof(roomGroupNameText) - 1);
		cursorflash = ticks;
		SDL_StartTextInput();
	}
	else if ( clicked(right - 158, suby1 + 40, right, suby1 + 58) )
	{
		roomCopyContentMode = (roomCopyContentMode + 1) % 3;
	}

	const int listTop = suby1 + 72;
	const int listBottom = suby2 - 112;
	const int rowHeight = 20;
	const int visibleRows = std::max(1, (listBottom - listTop) / rowHeight);
	const int maximumScroll = std::max(0,
		static_cast<int>(map.roomGroups.count) - visibleRows);
	if ( omousex >= left && omousex < right
		&& omousey >= listTop && omousey < listBottom && scroll != 0 )
	{
		roomGroupListScroll = std::clamp(roomGroupListScroll + scroll,
			0, maximumScroll);
		scroll = 0;
	}
	roomGroupListScroll = std::clamp(roomGroupListScroll, 0, maximumScroll);
	for ( int row = 0; row < visibleRows; ++row )
	{
		const int index = roomGroupListScroll + row;
		const int rowY = listTop + row * rowHeight;
		if ( index >= static_cast<int>(map.roomGroups.count) )
		{
			break;
		}
		const AuthoredRoomGroup& group = map.roomGroups.entries[index];
		if ( group.id == roomGroupSelectedID )
		{
			drawDepressed(left, rowY, right, rowY + 18);
		}
		else
		{
			drawWindowFancy(left, rowY, right, rowY + 18);
		}
		printTextFormatted(font8x8_bmp, left + 5, rowY + 5,
			"%c %-31.31s  (%d,%d)-(%d,%d)  L%d-%d  %s",
			group.id == roomGroupSelectedID ? '>' : ' ', group.name,
			group.x1, group.y1, group.x2, group.y2,
			group.bottomLayer, group.topLayer,
			group.contentMask == AUTHORED_ROOM_GROUP_TILES ? "tiles"
				: (group.contentMask == AUTHORED_ROOM_GROUP_SPRITES
					? "sprites" : "both"));
		if ( clicked(left, rowY, right, rowY + 18) )
		{
			roomGroupLoadEditorFields(group);
		}
	}

	if ( map.roomGroups.count == 0 )
	{
		printText(font8x8_bmp, left + 8, listTop + 8,
			"No named Room Groups. Select a cuboid, name it, then create one.");
	}
	printTextFormatted(font8x8_bmp, left, listBottom + 4,
		"%u / %zu groups", map.roomGroups.count, AUTHORED_ROOM_GROUP_MAX_COUNT);
	if ( roomGroupStatusText[0] && ticks < roomGroupStatusUntil )
	{
		printText(font8x8_bmp, left + 120, listBottom + 4, roomGroupStatusText);
	}

	auto actionButton = [&](const int x, const int y, const int width,
		const char* label)
	{
		drawWindowFancy(x, y, x + width, y + 18);
		printText(font8x8_bmp, x + 5, y + 5, label);
		return clicked(x, y, x + width, y + 18);
	};
	const int firstActionY = suby2 - 82;
	if ( actionButton(left, firstActionY, 152, "NEW FROM SELECTION") )
	{
		if ( !selectedarea )
		{
			roomGroupSetStatus("Select a cuboid first.");
		}
		else
		{
			normalizeRoomSelection();
			makeUndo();
			const int index = authoredRoomGroupAdd(map.roomGroups,
				roomGroupNameText, selectedarea_x1, selectedarea_y1,
				selectedarea_x2, selectedarea_y2,
				static_cast<std::int16_t>(roomSelectBottomLayer),
				static_cast<std::int16_t>(roomSelectTopLayer),
				roomCopyContentMask());
			if ( index >= 0 )
			{
				roomGroupLoadEditorFields(map.roomGroups.entries[index]);
				roomGroupSetStatus("Room Group created.");
			}
			else
			{
				roomGroupSetStatus("Could not create Room Group (limit reached).");
			}
		}
	}
	if ( actionButton(left + 160, firstActionY, 136, "UPDATE SELECTED") )
	{
		const int index = selectedRoomGroupIndex();
		if ( index < 0 || !selectedarea )
		{
			roomGroupSetStatus("Choose a group and select a cuboid.");
		}
		else
		{
			normalizeRoomSelection();
			makeUndo();
			if ( authoredRoomGroupUpdate(map.roomGroups, roomGroupSelectedID,
				roomGroupNameText, selectedarea_x1, selectedarea_y1,
				selectedarea_x2, selectedarea_y2,
				static_cast<std::int16_t>(roomSelectBottomLayer),
				static_cast<std::int16_t>(roomSelectTopLayer),
				roomCopyContentMask()) )
			{
				roomGroupLoadEditorFields(
					map.roomGroups.entries[selectedRoomGroupIndex()]);
				roomGroupSetStatus("Room Group updated.");
			}
		}
	}
	if ( actionButton(left + 304, firstActionY, 112, "DELETE GROUP") )
	{
		if ( selectedRoomGroupIndex() < 0 )
		{
			roomGroupSetStatus("Choose a Room Group first.");
		}
		else
		{
			makeUndo();
			authoredRoomGroupRemove(map.roomGroups, roomGroupSelectedID);
			roomGroupSelectedID = 0;
			std::snprintf(roomGroupNameText, sizeof(roomGroupNameText), "Room Group");
			roomGroupSetStatus("Room Group deleted; room contents were not changed.");
		}
	}

	const int secondActionY = suby2 - 54;
	if ( actionButton(left, secondActionY, 120, "SELECT BOUNDS") )
	{
		if ( roomGroupApplyBoundsToSelection() )
		{
			buttonCloseSubwindow(nullptr);
			reselectEntityGroup();
		}
	}
	if ( actionButton(left + 128, secondActionY, 104, "COPY GROUP") )
	{
		if ( roomGroupApplyBoundsToSelection() )
		{
			buttonCloseSubwindow(nullptr);
			editorRoomCopySelection();
		}
	}
}
void actGib(Entity* my) {} // dummy for draw.cpp
void actHudArm(Entity* my) {} // dummy for draw.cpp
void actHudWeapon(Entity* my) {} // dummy for draw.cpp
void actHUDMagicParticle(Entity* my) {} // dummy for draw.cpp
void actHUDMagicParticleCircling(Entity* my) {} // dummy for draw.cpp
void actHudShield(Entity* my) {} // dummy for draw.cpp
void actHudAdditional(Entity* my) {} // dummy for draw.cpp
void actHudAdditional2(Entity* my) {} // dummy for draw.cpp
void actHudArrowModel(Entity* my) {} // dummy for draw.cpp
void actLeftHandMagic(Entity* my) {} // dummy for draw.cpp
void actRightHandMagic(Entity* my) {} // dummy for draw.cpp
void actMagicRangefinder(Entity* my) {} // dummy for draw.cpp
void actTouchCastThirdPersonParticle(Entity* my) {} // dummy for draw.cpp
void actSprite(Entity* my) {} // dummy for draw.cpp
bool messagePlayer(int player, Uint32 type, char const * const message, ...) {return true;} // dummy
bool itemTypeIsFoci(const ItemType type) { return false; } // dummy

map_t copymap;

int errorMessage = 0;
int errorArr[32] =
{
	0
};

/*
 * Authored enemy squad and named-elite fields.
 *
 * These values deliberately use currently serialized Stat::MISC_FLAGS
 * slots so the current LMPV4.6 monster record does not change size.
 */
static constexpr int STAT_FLAG_AUTHORED_SQUAD_ID = 12;
static constexpr int STAT_FLAG_AUTHORED_SQUAD_OPTIONS = 13;
static constexpr int STAT_FLAG_AUTHORED_ELITE_PRESET = 14;
static constexpr int STAT_FLAG_AUTHORED_SQUAD_DEFEAT_ID = 15;

enum AuthoredSquadOptions : int
{
	AUTHORED_SQUAD_ROLE_MASK = 0x3,
	AUTHORED_SQUAD_ROLE_NONE = 0,
	AUTHORED_SQUAD_ROLE_LEADER = 1,
	AUTHORED_SQUAD_ROLE_MEMBER = 2,
	AUTHORED_SQUAD_FOLLOW_LEADER = 1 << 2,
	AUTHORED_SQUAD_ASSIST = 1 << 3,
	AUTHORED_SQUAD_SHARED_ALERT = 1 << 4,
	AUTHORED_SQUAD_WAKE_TOGETHER = 1 << 5
};

char monsterPropertyNames[14][16] = 
{
	"Name:",
	"MAX HP:",
	"HP:",
	"MAX MP:",
	"MP:",
	"LEVEL:",
	"GOLD:",
	"STR:",
	"DEX:",
	"CON:",
	"INT:",
	"PER:",
	"CHR:",
	"Is NPC:"
};

char chestPropertyNames[4][40] =
{
	"Orientation: (0-3)",
	"Chest Type: (0-7)",
	"Locked Chance: (0-100%)",
	"Mimic Chance: (0-100%)"
};

char summonTrapPropertyNames[7][44] =
{
	"Monster To Spawn: (-1 to 32)",
	"Quantity Per Spawn: (1-9)",
	"Time Between Spawns: (1-999s)",
	"Amount of Spawn Instances: (1-99)",
	"Requires Power to Disable: (0-1)",
	"Chance to Stop Working Each Spawn: (0-100%)",
	"Autospawn Next To Player (0-16)"
};

char itemPropertyNames[6][36] =
{
	"Item ID: (1-255)",
	"Status: (0-5)",
	"Blessing: (-9 to +9)",
	"Quantity: (1-99)",
	"Identified: (0-2)",
	"Category: (0-16, if random_item)"
};

char itemCategoryNames[17][32] =
{
	"random",
	"weapon",
	"armor",
	"amulet",
	"potion",
	"scroll",
	"magicstaff",
	"ring",
	"spellbook",
	"gem",
	"thrown",
	"tool",
	"food",
	"book",
	"equipment",
	"jewelry",
	"magical"
};

char powerCrystalPropertyNames[5][42] =
{
    "Orientation: (0-3)",
    "Powered Distance (0-99)",
    "Rotation Direction: (0-1)",
    "Require Unlock Spell to Activate (0-1)",
    "Require Circuit Power to Activate (0-1)"
};

char monsterItemPropertyNames[7][36] =
{
	"Item ID: (0-255)",
	"Status: (0-5)",
	"Blessing: (-9 to +9)",
	"Quantity: (1-99)",
	"Identified: (0-2)",
	"Chance (1-100)",
	"Category: (0-16, if default_random)"
};

char leverTimerPropertyNames[1][26] =
{
	"Powered Duration (1-999s)"
};

char boulderTrapPropertyNames[3][42] =
{
	"Amount of times to re-fire (-1 - 99)",
	"Delay between re-fire (2-999s)",
	"Pre-delay for first time trigger (0-999s)"
};

char pedestalPropertyNames[5][35] =
{
	"Orb Type (0-3)",
	"Pre-loaded with Orb (0-1)",
	"Inverted power generation (0-1)",
	"Pedestal start beneath ground(0-1)",
	"Lock orb when placed(0-1)"
};

char teleporterPropertyNames[3][25] =
{
	"X Coordinate to teleport",
	"Y Coordinate to teleport",
	"Type of sprite (0-2)"
};

char ceilingTilePropertyNames[4][30] =
{
	"Model texture to use (0-9999)",
	"Direction (0-3)",
	"Allow boulder trap gen (0-2)",
	"Allow minotaur break (0-1)"
};

char spellTrapPropertyNames[5][38] =
{
	"Spell Type: (-1 - 9)",
	"Amount of times to refire (-1 - 99)",
	"Power once to continuously fire (0-1)",
	"Ceiling model to use (0-9999)",
	"Trap refire rate (1-999s)",
};

char shrineTeleportPropertyNames[4][48] =
{
	"Direction (-1 - 3)",
	"Height Offset (Qtrs of a voxel, +ive is higher)",
	"Destination X Offset",
	"Destination Y Offset"
};

char furniturePropertyNames[1][19] =
{
	"Direction (-1 - 7)"
};

char windPropertyNames[1][19] =
{
	"Direction (-1 - 7)"
};

char floorDecorationPropertyNames[10][59] =
{
	"Model texture to use (0-9999)",
	"Direction (-1 - 7)",
	"Height Offset (Qtrs of a voxel, +ive is higher)",
	"X Offset (L/R, Qtrs of a voxel, +ive is right)",
	"Y Offset (U/D, Qtrs of a voxel, +ive is down)",
	"Destroy if no wall (-1 - 8)",
	"Interact Text",
	"",
	"",
	""
};

char colliderDecorationPropertyNames[11][59] =
{
	"Model texture to use (0-9999)",
	"Direction (-1 - 7)",
	"Height Offset (Qtrs of a voxel, +ive is higher)",
	"X Offset (L/R, Qtrs of a voxel, +ive is right)",
	"Y Offset (U/D, Qtrs of a voxel, +ive is down)",
	"Has Collision (0 - no collision, 1 - collision",
	"Collision X (8 - full width, 4 - half width)",
	"Collision Y (8 - full width, 4 - half width)",
	"HP (0 - not damageable)",
	"Diggable (0-1)",
	"Damage Types"
};

char soundSourcePropertyNames[5][59] =
{
	"Sound source line number to play from sounds.txt (0-999)",
	"Volume (0-255)",
	"Play once only (0-1)",
	"Activation delay (0-9999 ticks, 50 ticks / sec)",
	"Sound origin (0 = this entity, 1 = global)"
};

char lightSourcePropertyNames[10][48] =
{
	"Light always on (0-1)",
	"Brightness (0-255)",
	"Invert power (0-1)",
	"Light/unlight once only (0-1)",
	"Tile radius of light source (0-64)",
	"Light flicker enable (0-1)",
	"Activation delay (0-9999 ticks, 50 ticks / sec)",
	"Color (R, G, B) (0-255):",
	"",
	""
};

char textSourcePropertyNames[10][45] =
{
	"Color (R, G, B) (0-255):",
	"",
	"",
	"Text:",
	"",
	"",
	"",
	"",
	"Message delay (0-9999 ticks, 50 ticks / sec)",
	"Send message once only (0 - 1)"
};

char customPortalPropertyNames[13][59] =
{
	"Model texture to use (0-9999)",
	"Animation frames (0-9)",
	"Model Height Offset (Qtrs of a voxel, +ive is higher)",
	"Levels to advance (-99 - 99)",
	"Level name to jump to (Can be used with above option)",
	"Requires power to be visible (0-1)",
	"Exit toggle between secret levels file (0-1)",
	"Requirement Mode: (0-4)",
	"Required Race: (-1 = Any)",
	"Required Class: (-1 = Any)",
	"Activate when powered (0-1)",
	"Tunnel End ID: (0 = Disabled)",
    "Destination Tunnel ID: (0 = Player Start)"
};

char signalTimerPropertyNames[6][55] =
{
	"Input signal direction (0 - 3)",
	"Output activation delay (0-9999 ticks, 50 ticks / sec)",
	"Output pulse time (0 - 9999 ticks, 50 ticks / sec)",
	"Output repeat count (0 - 9999)",
	"Latch input and keep powered (0 - 1)",
	"Invert Output (0 - 1)"
};

char ANDGatePropertyNames[6][55] =
{
	"Output signal direction (0 - 3)",
	"Output activation delay (0-9999 ticks, 50 ticks / sec)",
	"Output pulse time (0 - 9999 ticks, 50 ticks / sec)",
	"Output repeat count (0 - 9999)",
	"Latch input and keep powered (0 - 1)",
	"Invert Output (0 - 1)"
};

char pressurePlatePropertyNames[4][48] =
{
	"Trigger Type: (0-7)",
	"Requirement Mode: (0-4)",
	"Required Race: (-1 = Any)",
	"Required Class: (-1 = Any)"
};

char tablePropertyNames[3][34] =
{
	"Direction (-1 - 7)",
	"Spawn chairs (-1 - 4)",
	"Spawn item on table % (-1 - 100)",
};

char readableBookPropertyNames[4][32] =
{
	"Status: (0-5)",
	"Blessing: (-9 to +9)",
	"Identified: (0-2)",
	"Book Title:"
};

char doorPropertyNames[3][42] =
{
	"Force Door Locked/Unlocked (0-2)",
	"Disable unlocking with lockpick (0-1)",
	"Disable unlocking with spell (0-1)"
};

char doorIronPropertyNames[4][42] =
{
	"Unlock When Powered (0-1)",
	"Disable unlocking with lockpick (0-1)",
	"Disable unlocking with spell (0-1)",
	"Force Door Locked/Unlocked (0-2)"
};

char gatePropertyNames[1][35] =
{
	"Disable unlocking with spell (0-1)"
};

char playerSpawnPropertyNames[1][35] =
{
	"Spawn Facing Direction (-1 - 7)"
};

char statuePropertyNames[2][16] =
{
	"Direction (0-3)",
	"Statue ID",
};

char wallLockPropertyNames[6][32] =
{
	"Material (0 - 7)",
	"Invert Power (0 - 1)",
	"Key Turnable (0 - 1)",
	"Lockpickable (-1 - 100)",
	"Skeleton Key Usable (0 - 1)",
	"Auto Gen Matching Key (0 - 1)"
};

char wallButtonPropertyNames[2][49] =
{
	"Invert Power (0 - 1)",
	"Deactivation Time (0-9999 ticks)"
};

const char* playerClassLangEntry(int classnum, int playernum)
{
	if ( classnum >= CLASS_BARBARIAN && classnum <= CLASS_JOKER )
	{
		return Language::get(1900 + classnum);
	}
	else if ( classnum >= CLASS_CONJURER )
	{
		return Language::get(3223 + classnum - CLASS_CONJURER);
	}
	else if ( classnum >= CLASS_SEXTON && classnum <= CLASS_MONK )
	{
		return Language::get(2550 + classnum - CLASS_SEXTON);
	}
	else
	{
		return "undefined classname";
	}
}
static const char* requirementRaceName(int race)
{
	switch ( race )
	{
		case -1:
			return "Any Race";

		case RACE_HUMAN:
			return "Human";

		case RACE_SKELETON:
			return "Skeleton";

		case RACE_VAMPIRE:
			return "Vampire";

		case RACE_SUCCUBUS:
			return "Succubus";

		case RACE_GOATMAN:
			return "Goatman";

		case RACE_AUTOMATON:
			return "Automaton";

		case RACE_INCUBUS:
			return "Incubus";

		case RACE_GOBLIN:
			return "Goblin";

		case RACE_INSECTOID:
			return "Insectoid";

		case RACE_RAT:
			return "Rat";

		case RACE_TROLL:
			return "Troll";

		case RACE_SPIDER:
			return "Spider";

		case RACE_IMP:
			return "Imp";

		case RACE_DRYAD:
			return "Dryad";

		case RACE_MYCONID:
			return "Myconid";

		case RACE_GREMLIN:
			return "Gremlin";

		case RACE_SALAMANDER:
			return "Salamander";

		case RACE_GNOME:
			return "Gnome";

		default:
			return nullptr;
	}
}

static const char* requirementClassName(int classnum)
{
	if ( classnum == -1 )
	{
		return "Any Class";
	}

	if ( classnum < 0 || classnum >= NUMCLASSES )
	{
		return nullptr;
	}

	return playerClassLangEntry(classnum, 0);
}
/*-------------------------------------------------------------------------------

mouseInBounds

Returns true if the mouse is within the rectangle specified, otherwise
returns false

-------------------------------------------------------------------------------*/

bool mouseInBounds(int x1, int x2, int y1, int y2)
{
	if ( omousey >= y1 && omousey < y2 )
		if ( omousex >= x1 && omousex < x2 )
		{
			return true;
		}

	return false;
}

int recentUsedTiles[9][9] = { 0 };
int recentUsedTilePalette = 0;
int lockTilePalette[9] = { 0 };
int lastPaletteTileSelected = 0;

void closeNetworkInterfaces()
{
	//Because Dennis.
}

/*-------------------------------------------------------------------------------

	mainLogic

	handles time dependent procedures

-------------------------------------------------------------------------------*/

view_t camera_vel;
view_t camera;

static void questDialogueEditorFinishMarkerPick(
	const bool canceled,
	const bool applied
)
{
	const bool restore3D = questDialogueEditorMarkerPickRestore3D;
	questDialogueEditorMarkerPick = QUEST_DIALOGUE_MARKER_PICK_NONE;
	questDialogueEditorMarkerPickObjective = -1;
	questDialogueEditorMarkerPickRestore3D = false;
	messagetime = 0;
	mode3d = restore3D;
	questDialogueEditorPreserveModelOnOpen = true;
	openQuestDialogueEditor();
	if ( canceled )
	{
		questDialogueEditorSetMessage("Tile picking canceled; marker was unchanged.");
	}
	else if ( !applied )
	{
		questDialogueEditorSetMessage("The selected marker target is no longer valid.");
	}
}

static void questDialogueEditorCycleMarkerPickFloor(const int direction)
{
	std::vector<int> floorIDs;
	floorIDs.reserve(map.playableFloors.floors.size());
	for ( const PlayableFloorData& floor : map.playableFloors.floors )
	{
		floorIDs.push_back(floor.id);
	}
	if ( floorIDs.empty() )
	{
		floorIDs.push_back(DEFAULT_PLAYABLE_FLOOR);
	}
	std::sort(floorIDs.begin(), floorIDs.end());
	floorIDs.erase(std::unique(floorIDs.begin(), floorIDs.end()), floorIDs.end());
	auto found = std::find(floorIDs.begin(), floorIDs.end(),
		questDialogueEditorMarkerPickPlayableFloor);
	int index = found == floorIDs.end()
		? 0 : static_cast<int>(std::distance(floorIDs.begin(), found));
	index = std::max(0, std::min(static_cast<int>(floorIDs.size()) - 1,
		index + direction));
	questDialogueEditorMarkerPickPlayableFloor = floorIDs[index];
	std::snprintf(message, sizeof(message),
		"MARKER PICK floor %d: click tile; U/P changes floor; right-click/Esc cancels.",
		questDialogueEditorMarkerPickPlayableFloor);
	messagetime = 1000000;
}

static bool questDialogueEditorHandleMarkerPick()
{
	if ( questDialogueEditorMarkerPick == QUEST_DIALOGUE_MARKER_PICK_NONE )
	{
		return false;
	}
	camx += (keystatus[SDLK_RIGHT] - keystatus[SDLK_LEFT]) * TEXTURESIZE;
	camy += (keystatus[SDLK_DOWN] - keystatus[SDLK_UP]) * TEXTURESIZE;
	camx = std::max<long>(-xres / 2,
		std::min<long>(((long)map.width << TEXTUREPOWER) - xres / 2, camx));
	camy = std::max<long>(-yres / 2,
		std::min<long>(((long)map.height << TEXTUREPOWER) - yres / 2, camy));

	if ( keystatus[SDLK_ESCAPE] || mousestatus[SDL_BUTTON_RIGHT] )
	{
		keystatus[SDLK_ESCAPE] = 0;
		mousestatus[SDL_BUTTON_RIGHT] = 0;
		questDialogueEditorFinishMarkerPick(true, false);
		return true;
	}
	if ( keystatus[SDLK_u] )
	{
		keystatus[SDLK_u] = 0;
		questDialogueEditorCycleMarkerPickFloor(1);
	}
	if ( keystatus[SDLK_p] )
	{
		keystatus[SDLK_p] = 0;
		questDialogueEditorCycleMarkerPickFloor(-1);
	}

	if ( mousestatus[SDL_BUTTON_LEFT] )
	{
		mousestatus[SDL_BUTTON_LEFT] = 0;
		const int tileX = (mousex + camx) >> TEXTUREPOWER;
		const int tileY = (mousey + camy) >> TEXTUREPOWER;
		if ( mousey < 16 || mousey >= yres - 16
			|| tileX < 0 || tileY < 0
			|| tileX >= static_cast<int>(map.width)
			|| tileY >= static_cast<int>(map.height) )
		{
			std::snprintf(message, sizeof(message),
				"Pick a tile inside the map (floor %d); right-click/Esc cancels.",
				questDialogueEditorMarkerPickPlayableFloor);
			messagetime = 1000000;
			return true;
		}

		bool applied = false;
		if ( questDialogueEditorMarkerPick == QUEST_DIALOGUE_MARKER_PICK_ORIGIN )
		{
			applied = questDialogueEditorSetQuestGiverTile(
				tileX, tileY, questDialogueEditorMarkerPickPlayableFloor);
		}
		else
		{
			questDialogueEditorSelectedObjective =
				questDialogueEditorMarkerPickObjective;
			applied = questDialogueEditorSetObjectiveMarkerTile(
				tileX, tileY, questDialogueEditorMarkerPickPlayableFloor);
		}
		questDialogueEditorFinishMarkerPick(false, applied);
	}
	return true;
}

void mainLogic(void)
{
	// messages
	if ( messagetime > 0 )
	{
		messagetime--;
	}

	if ( errorMessage > 0 )
	{
		errorMessage--;
		if ( errorMessage == 0 )
		{
			for ( int i = 0; i < sizeof(errorArr) / sizeof(int); i++ )
				errorArr[i] = 0;
		}
	}

	if ( questDialogueEditorHandleMarkerPick() )
	{
		return;
	}
	

	// basic editing functions are not available under these cases
	if ( subwindow || tilepalette || spritepalette )
	{
		return;
	}

	// scroll camera on minimap
	if ( mousestatus[SDL_BUTTON_LEFT] && toolbox )
	{
		if ( omousex >= xres - 120 && omousex < xres - 8 )
		{
			if ( omousey >= 24 && omousey < 136 )
			{
				camx = ((long)map.width << TEXTUREPOWER) * (real_t)(mousex - xres + 120) / 112 - xres / 2;
				camy = ((long)map.height << TEXTUREPOWER) * (real_t)(mousey - 24) / 112 - yres / 2;
			}
		}
	}
	camx -= camx % TEXTURESIZE; // make sure the camera is a multiple of 32 for hover text to work.
	camy -= camy % TEXTURESIZE; // make sure the camera is a multiple of 32 for hover text to work.

	// basic editor functions
	if ( mode3d == false )
	{
		camx += (keystatus[SDLK_RIGHT] - keystatus[SDLK_LEFT]) * TEXTURESIZE;
		camy += (keystatus[SDLK_DOWN] - keystatus[SDLK_UP]) * TEXTURESIZE;
	}
	else
	{
		const bool editor3DFastCamera =
			keystatus[SDLK_LSHIFT]
			|| keystatus[SDLK_RSHIFT];
		const real_t editor3DMoveSpeed =
			editor3DFastCamera
				? 0.12
				: 0.05;
		const real_t editor3DVerticalSpeed =
			editor3DFastCamera
				? 0.6
				: 0.25;

		// camera velocity
		camera_vel.x +=
			cos(camera.ang)
			* (keystatus[SDLK_UP] - keystatus[SDLK_DOWN])
			* editor3DMoveSpeed;
		camera_vel.y +=
			sin(camera.ang)
			* (keystatus[SDLK_UP] - keystatus[SDLK_DOWN])
			* editor3DMoveSpeed;

		const int editor3DMoveDown =
			keystatus[SDLK_PAGEDOWN]
			|| keystatus[SDLK_e];
		const int editor3DMoveUp =
			keystatus[SDLK_PAGEUP]
			|| keystatus[SDLK_q];

		camera_vel.z +=
			(editor3DMoveDown - editor3DMoveUp)
			* editor3DVerticalSpeed;
		camera_vel.ang +=
			(keystatus[SDLK_RIGHT] - keystatus[SDLK_LEFT])
			* .04;

		if ( keystatus[SDLK_HOME] )
		{
			keystatus[SDLK_HOME] = 0;
			camera.z = 0;
			camera_vel.z = 0;
		}

		// camera position
		camera.x += camera_vel.x;
		camera.y += camera_vel.y;
		camera.z += camera_vel.z;

		const real_t editor3DHighestCamera =
			-std::max(
				64.0,
				static_cast<real_t>(
					MAPLAYERS * 16
				)
			);
		const real_t editor3DLowestCamera = 64.0;

		camera.z = std::min(
			editor3DLowestCamera,
			std::max(
				editor3DHighestCamera,
				camera.z
			)
		);

		camera.ang += camera_vel.ang;
		while ( camera.ang >= PI * 2 )
		{
			camera.ang -= PI * 2;
		}
		while ( camera.ang < 0 )
		{
			camera.ang += PI * 2;
		}

		// friction
		camera_vel.x *= .65;
		camera_vel.y *= .65;
		camera_vel.z *= .65;
		camera_vel.ang *= .5;
	}
	if ( camx < -xres / 2 )
	{
		camx = -xres / 2;
	}
	if ( camx > ((long)map.width << TEXTUREPOWER) - ((long)xres / 2) )
	{
		camx = ((long)map.width << TEXTUREPOWER) - ((long)xres / 2);
	}
	if ( camy < -yres / 2 )
	{
		camy = -yres / 2;
	}
	if ( camy > ((long)map.height << TEXTUREPOWER) - ((long)yres / 2) )
	{
		camy = ((long)map.height << TEXTUREPOWER) - ((long)yres / 2);
	}

	if (scroll < 0 )   // mousewheel up
	{
		if ( keystatus[SDLK_LCTRL] || keystatus[SDLK_RCTRL] )
		{
			recentUsedTilePalette++; //scroll through palettes 1-9
			if ( recentUsedTilePalette == 9 )
			{
				recentUsedTilePalette = 0;
			}
		}
		else if ( keystatus[SDLK_LSHIFT] || keystatus[SDLK_RSHIFT] )
			{
				drawlayer = std::min(drawlayer + 1, MAPLAYERS - 1);

				if ( selectedEntity[0]
					&& entityAuthoredSpriteLayer(selectedEntity[0]) != drawlayer )
				{
					selectedEntity[0] = nullptr;
				}
			}
		else
		{
			lastPaletteTileSelected++; //scroll through tiles 1-9
			if ( lastPaletteTileSelected == 9 )
			{
				lastPaletteTileSelected = 0;
			}
			selectedTile = selectedTile = recentUsedTiles[recentUsedTilePalette][lastPaletteTileSelected];
		}
		scroll = 0;
	}
	if (scroll > 0 )   // mousewheel down
	{
		if ( keystatus[SDLK_LCTRL] || keystatus[SDLK_RCTRL] )
		{
			recentUsedTilePalette--; //scroll through palettes 1-9
			if ( recentUsedTilePalette == -1 )
			{
				recentUsedTilePalette = 8;
			}
		}
		else if ( keystatus[SDLK_LSHIFT] || keystatus[SDLK_RSHIFT] )
		{
			drawlayer = std::max(drawlayer - 1, 0);
		}
		else
		{
			lastPaletteTileSelected--; //scroll through tiles 1-9
			if ( lastPaletteTileSelected == -1 )
			{
				lastPaletteTileSelected = 8;
			}
			selectedTile = selectedTile = recentUsedTiles[recentUsedTilePalette][lastPaletteTileSelected];

		}
		scroll = 0;
	}

	switch ( drawlayer )
	{
		case 0:
			strcpy(layerstatus, "FLOOR");
			break;
		case 1:
			strcpy(layerstatus, "WALLS");
			break;
		case 2:
			strcpy(layerstatus, "CEILING");
			break;
		default:
			strcpy(layerstatus, "LAYER");
			break;
	}
}

/*-------------------------------------------------------------------------------

	handleButtons

	Draws buttons and processes clicks

-------------------------------------------------------------------------------*/

void handleButtons(void)
{
	node_t* node;
	node_t* nextnode;
	button_t* button;
	int w, h;

	// handle buttons
	for ( node = button_l.first; node != NULL; node = nextnode )
	{
		nextnode = node->next;
		button = (button_t*)node->element;
		if ( !subwindow && button->focused )
		{
			list_RemoveNode(button->node);
			continue;
		}
		if ( button->visible == 0 )
		{
			continue;    // invisible buttons are not processed
		}
		w = strlen(button->label) * 8;
		h = 8;
		if ( subwindow && !button->focused )
		{
			// unfocused buttons do not work when a subwindow is active
			drawWindow(button->x, button->y, button->x + button->sizex, button->y + button->sizey);
			printText(font8x8_bmp, button->x + (button->sizex - w) / 2, button->y + (button->sizey - h) / 2, button->label);
		}
		else
		{
			if ( omousex >= button->x && omousex < button->x + button->sizex )
			{
				if ( omousey >= button->y && omousey < button->y + button->sizey )
				{
					if ( button == butFile && menuVisible )
					{
						menuVisible = 1;
					}
					if ( button == butEdit && menuVisible )
					{
						menuVisible = 2;
					}
					if ( button == butView && menuVisible )
					{
						menuVisible = 3;
					}
					if ( button == butMap && menuVisible )
					{
						menuVisible = 4;
					}
					if ( button == butDialogue && menuVisible )
					{
						menuVisible = 5;
					}
					if ( button == butScripts && menuVisible )
					{
						menuVisible = 7;
					}
					if ( button == butHelp && menuVisible )
					{
						menuVisible = 6;
					}
					if ( mousestatus[SDL_BUTTON_LEFT] )
					{
						button->pressed = true;
					}
				}
			}
			if ( button->pressed )
			{
				if ( omousex >= button->x && omousex < button->x + button->sizex && mousex >= button->x && mousex < button->x + button->sizex )
				{
					if ( omousey >= button->y && omousey < button->y + button->sizey && mousey >= button->y && mousey < button->y + button->sizey )
					{
						drawDepressed(button->x, button->y, button->x + button->sizex, button->y + button->sizey);
						printText(font8x8_bmp, button->x + (button->sizex - w) / 2, button->y + (button->sizey - h) / 2, button->label);
						if ( !mousestatus[SDL_BUTTON_LEFT] )   // releasing the mouse over the button
						{
							button->pressed = false;
							if ( button->action != NULL )
							{
								(*button->action)(button); // run the button's assigned action
								if ( !subwindow && button->focused )
								{
									list_RemoveNode(button->node);
								}
							}
						}
					}
					else
					{
						drawWindow(button->x, button->y, button->x + button->sizex, button->y + button->sizey);
						printText(font8x8_bmp, button->x + (button->sizex - w) / 2, button->y + (button->sizey - h) / 2, button->label);
						if ( !mousestatus[SDL_BUTTON_LEFT] )   // releasing the mouse over nothing
						{
							button->pressed = false;
						}
					}
				}
				else
				{
					drawWindow(button->x, button->y, button->x + button->sizex, button->y + button->sizey);
					printText(font8x8_bmp, button->x + (button->sizex - w) / 2, button->y + (button->sizey - h) / 2, button->label);
					if ( !mousestatus[SDL_BUTTON_LEFT] )   // releasing the mouse over nothing
					{
						button->pressed = false;
					}
				}
			}
			else
			{
				if ( (button != butFile || menuVisible != 1) && (button != butEdit || menuVisible != 2) && (button != butView || menuVisible != 3) && (button != butMap || menuVisible != 4) && (button != butDialogue || menuVisible != 5) && (button != butScripts || menuVisible != 7) && (button != butHelp || menuVisible != 6) )
				{
					drawWindow(button->x, button->y, button->x + button->sizex, button->y + button->sizey);
					printText(font8x8_bmp, button->x + (button->sizex - w) / 2, button->y + (button->sizey - h) / 2, button->label);
				}
				else
				{
					drawDepressed(button->x, button->y, button->x + button->sizex, button->y + button->sizey);
					printText(font8x8_bmp, button->x + (button->sizex - w) / 2, button->y + (button->sizey - h) / 2, button->label);
				}
			}
		}
	}
}

/*-------------------------------------------------------------------------------

	handleEvents

	Handles all SDL events; receives input, updates gamestate, etc.

-------------------------------------------------------------------------------*/

bool handleEvents(void)
{
	real_t d;
	int j;


	// calculate app rate
	t = SDL_GetTicks();
	real_t timesync = t - ot;
	ot = t;

	// do timer
	time_diff += timesync;
	constexpr real_t frame = (real_t)1000 / (real_t)TICKS_PER_SECOND;
	while (time_diff >= frame) {
		time_diff -= frame;
		timerCallback(0, NULL);
	}

	// calculate fps
	if ( timesync != 0 )
	{
		frameval[cycles & (AVERAGEFRAMES - 1)] = 1.0 / timesync;
	}
	else
	{
		frameval[cycles & (AVERAGEFRAMES - 1)] = 1.0;
	}
	d = frameval[0];
	for (j = 1; j < AVERAGEFRAMES; j++)
	{
		d += frameval[j];
	}
	fps = d / AVERAGEFRAMES * 1000;

	while ( SDL_PollEvent(&event) )   // poll SDL events
	{
		// Global events
		switch ( event.type )
		{
			case SDL_QUIT: // if SDL receives the shutdown signal
				buttonExit(NULL);
				break;
			case SDL_KEYDOWN: // if a key is pressed...
				if ( questDialogueEditorJSONOwnsTextInput()
					&& questDialogueEditorJSONHandleKey(event.key) )
				{
					lastkeypressed = event.key.keysym.sym;
					break;
				}
				if ( SDL_IsTextInputActive() )
				{
#ifdef APPLE
					if ( (event.key.keysym.sym == SDLK_DELETE || event.key.keysym.sym == SDLK_BACKSPACE) && strlen(inputstr) > 0 )
					{
						inputstr[strlen(inputstr) - 1] = 0;
						cursorflash = ticks;
					}
#else
					if ( event.key.keysym.sym == SDLK_BACKSPACE && strlen(inputstr) > 0 )
					{
						if ( textInsertCaratPosition >= 0 && newwindow == 20 && textInsertCaratPosition != strlen(inputstr) )
						{
							if ( textInsertCaratPosition != 0 )
							{
								std::string tmp = inputstr;
								tmp.erase(textInsertCaratPosition - 1, 1);
								strcpy(inputstr, tmp.c_str());
								textInsertCaratPosition = std::max(textInsertCaratPosition - 1, 0);
							}
						}
						else
						{
							inputstr[strlen(inputstr) - 1] = 0;
						}
						cursorflash = ticks;
					}
					else if ( event.key.keysym.sym == SDLK_BACKQUOTE && strlen(inputstr) > 0 )
					{
						if ( textInsertCaratPosition >= 0 && newwindow == 20 && textInsertCaratPosition != strlen(inputstr) )
						{
							if ( textInsertCaratPosition != 0 )
							{
								std::string tmp = inputstr;
								tmp.erase(textInsertCaratPosition - 1, 1);
								strcpy(inputstr, tmp.c_str());
								textInsertCaratPosition = std::max(textInsertCaratPosition - 1, 0);
							}
						}
						else
						{
							inputstr[strlen(inputstr) - 1] = 0;
						}
						cursorflash = ticks;
					}
#endif
					else if ( event.key.keysym.sym == SDLK_c && SDL_GetModState()&KMOD_CTRL )
					{
						SDL_SetClipboardText(inputstr);
						cursorflash = ticks;
					}
					else if ( event.key.keysym.sym == SDLK_v && SDL_GetModState()&KMOD_CTRL )
					{
						char* clipboardText = SDL_GetClipboardText();
						if ( clipboardText )
						{
							strncpy(inputstr, clipboardText, inputlen);
							inputstr[inputlen] = '\0';
							SDL_free(clipboardText);
						}
						cursorflash = ticks;
					}
				}
				lastkeypressed = event.key.keysym.sym;
				keystatus[event.key.keysym.sym] = 1; // set this key's index to 1
				break;
			case SDL_KEYUP: // if a key is unpressed...
				keystatus[event.key.keysym.sym] = 0; // set this key's index to 0
				break;
			case SDL_TEXTINPUT:
				if ( questDialogueEditorJSONOwnsTextInput() )
				{
					questDialogueEditorJSONInsert(event.text.text);
					break;
				}
				if ( (event.text.text[0] != 'c' && event.text.text[0] != 'C') || !(SDL_GetModState()&KMOD_CTRL) )
				{
					if ( (event.text.text[0] != 'v' && event.text.text[0] != 'V') || !(SDL_GetModState()&KMOD_CTRL) )
					{
						if ( event.text.text[0] != '`' )
						{
							if ( textInsertCaratPosition >= 0 && newwindow == 20 )
							{
								if ( textInsertCaratPosition == strlen(inputstr) )
								{
									strncat(inputstr, event.text.text, std::max<size_t>(0, inputlen - strlen(inputstr)));
									textInsertCaratPosition = std::min((int)strlen(inputstr), textInsertCaratPosition + 1);
								}
								else if ( inputlen - ((int)strlen(inputstr) + 1) >= 0)
								{
									std::string tmp = inputstr;
									tmp.insert(textInsertCaratPosition, event.text.text);
									strcpy(inputstr, tmp.c_str());
									textInsertCaratPosition = std::min((int)strlen(inputstr), textInsertCaratPosition + 1);
								}
							}
							else
							{
								strncat(inputstr, event.text.text, std::max<size_t>(0, inputlen - strlen(inputstr)));
							}
							cursorflash = ticks;
						}
					}
				}
				break;
			case SDL_MOUSEMOTION: // if the mouse is moved...
                float factorX;
                float factorY;
                {
                    int w1, w2, h1, h2;
                    SDL_GL_GetDrawableSize(screen, &w1, &h1);
                    SDL_GetWindowSize(screen, &w2, &h2);
                    factorX = (float)w1 / w2;
                    factorY = (float)h1 / h2;
                }
                mousex = event.motion.x * factorX;
                mousey = event.motion.y * factorY;
				mousexrel = event.motion.xrel;
				mouseyrel = event.motion.yrel;
				break;
			case SDL_MOUSEBUTTONDOWN: // if a mouse button is pressed...
				mousestatus[event.button.button] = 1; // set this mouse button to 1
				break;
			case SDL_MOUSEBUTTONUP: // if a mouse button is released...
				mousestatus[event.button.button] = 0; // set this mouse button to 0
				break;
			case SDL_MOUSEWHEEL:
				if ( event.wheel.y > 0 )
				{
					mousestatus[SDL_BUTTON_WHEELUP] = 1;
				}
				else if ( event.wheel.y < 0 )
				{
					mousestatus[SDL_BUTTON_WHEELDOWN] = 1;
				}
				if (mousestatus[4])
				{
					scroll = 1;
				}
				else if (mousestatus[5])
				{
					scroll = -1;
				}
				mousestatus[SDL_BUTTON_WHEELUP] = 0;
				mousestatus[SDL_BUTTON_WHEELDOWN] = 0;
				break;
			case SDL_USEREVENT: // if the game timer elapses
				mainLogic();
				mousexrel = 0;
				mouseyrel = 0;
				break;
			case SDL_WINDOWEVENT: // if the window is resized
				if ( event.window.event == SDL_WINDOWEVENT_RESIZED )
				{
					if (fullscreen || ticks == 0)
					{
						break;
					}
                    float factorX, factorY;
                    {
                        int w1, w2, h1, h2;
                        SDL_GL_GetDrawableSize(screen, &w1, &h1);
                        SDL_GetWindowSize(screen, &w2, &h2);
                        factorX = (float)w1 / w2;
                        factorY = (float)h1 / h2;
                    }
                    const int x = event.window.data1 * factorX;
                    const int y = event.window.data2 * factorY;
					if ( !resizeWindow(x, y) )
					{
						printlog("critical error! Attempting to abort safely...\n");
						mainloop = 0;
					}
					if (palette != NULL)
					{
						free(palette);
					}
					palette = (int*) malloc(sizeof(unsigned int) * xres * yres);
				}
				break;
		}
	}
	if (!mousestatus[SDL_BUTTON_LEFT])
	{
		omousex = mousex;
		omousey = mousey;
		ocamx = camx;
		ocamy = camy;
	}

	return true;
}

/*-------------------------------------------------------------------------------

	timerCallback

	A callback function for the game timer which pushes an SDL event

-------------------------------------------------------------------------------*/

#include "ui/LoadingScreen.hpp"

Uint32 timerCallback(Uint32 interval, void* param)
{
	SDL_Event event;
	SDL_UserEvent userevent;

	userevent.type = SDL_USEREVENT;
	userevent.code = 0;
	userevent.data1 = NULL;
	userevent.data2 = NULL;

	event.type = SDL_USEREVENT;
	event.user = userevent;

	ticks++;
	loadingticks++;
	SDL_PushEvent(&event);
	return (interval);
}

/*-------------------------------------------------------------------------------

	editFill

	Fills a region of the map with a certain tile

-------------------------------------------------------------------------------*/

void editFill(int x, int y, int layer, int type)
{
	int repeat = 1;
	int fillspot;

	if ( type == map.tiles[layer + y * MAPLAYERS + x * MAPLAYERS * map.height] )
	{
		return;
	}

	fillspot = map.tiles[layer + y * MAPLAYERS + x * MAPLAYERS * map.height];
	map.tiles[layer + y * MAPLAYERS + x * MAPLAYERS * map.height] = type + numtiles;

	while ( repeat )
	{
		repeat = 0;
		for ( x = 0; x < map.width; x++ )
		{
			for ( y = 0; y < map.height; y++ )
			{
				if ( map.tiles[layer + y * MAPLAYERS + x * MAPLAYERS * map.height] == type + numtiles )
				{
					if ( x < map.width - 1 )
					{
						if ( map.tiles[layer + y * MAPLAYERS + (x + 1)*MAPLAYERS * map.height] == fillspot )
						{
							map.tiles[layer + y * MAPLAYERS + (x + 1)*MAPLAYERS * map.height] = type + numtiles;
							repeat = 1;
						}
					}
					if ( x > 0 )
					{
						if ( map.tiles[layer + y * MAPLAYERS + (x - 1)*MAPLAYERS * map.height] == fillspot )
						{
							map.tiles[layer + y * MAPLAYERS + (x - 1)*MAPLAYERS * map.height] = type + numtiles;
							repeat = 1;
						}
					}
					if ( y < map.height - 1 )
					{
						if ( map.tiles[layer + (y + 1)*MAPLAYERS + x * MAPLAYERS * map.height] == fillspot )
						{
							map.tiles[layer + (y + 1)*MAPLAYERS + x * MAPLAYERS * map.height] = type + numtiles;
							repeat = 1;
						}
					}
					if ( y > 0 )
					{
						if ( map.tiles[layer + (y - 1)*MAPLAYERS + x * MAPLAYERS * map.height] == fillspot )
						{
							map.tiles[layer + (y - 1)*MAPLAYERS + x * MAPLAYERS * map.height] = type + numtiles;
							repeat = 1;
						}
					}
				}
			}
		}
	}

	for ( x = 0; x < map.width; x++ )
	{
		for ( y = 0; y < map.height; y++ )
		{
			if ( map.tiles[layer + y * MAPLAYERS + x * MAPLAYERS * map.height] == type + numtiles )
			{
				map.tiles[layer + y * MAPLAYERS + x * MAPLAYERS * map.height] = type;
			}
		}
	}
}

/*-------------------------------------------------------------------------------

	makeUndo

	adds an undomap to the undolist

-------------------------------------------------------------------------------*/

#define MAXUNDOS 10

node_t* undospot = nullptr;
node_t* redospot = nullptr;
list_t undolist;
void makeUndo()
{
	node_t* node, *nextnode;

	// eliminate any undo nodes beyond the one we are currently on
	if ( undospot != nullptr )
	{
		for ( node = undospot->next; node != nullptr; node = nextnode )
		{
			nextnode = node->next;
			list_RemoveNode(node);
		}
	}
	else
	{
		if ( redospot )
		{
			list_FreeAll(&undolist);
		}
	}

	// copy all the current map data
	map_t* undomap = (map_t*) malloc(sizeof(map_t));
	strcpy(undomap->author, map.author);
	strcpy(undomap->name, map.name);
	undomap->skybox = map.skybox;
	undomap->width = map.width;
	undomap->height = map.height;
	undomap->numLayers = map.numLayers;
	undomap->roomGroups = map.roomGroups;
	for ( int c = 0; c < MAPFLAGS; c++ )
	{
		undomap->flags[c] = map.flags[c];
	}
	undomap->tiles = (Sint32*) malloc(sizeof(Sint32) * undomap->width * undomap->height * MAPLAYERS);
	memcpy(undomap->tiles, map.tiles, sizeof(Sint32)*undomap->width * undomap->height * MAPLAYERS);
	undomap->entities = (list_t*) malloc(sizeof(list_t));
	undomap->entities->first = nullptr;
	undomap->entities->last = nullptr;
	undomap->creatures = nullptr;
	undomap->worldUI = nullptr;
	undomap->trapexcludelocations = nullptr;
	undomap->monsterexcludelocations = nullptr;
	undomap->lootexcludelocations = nullptr;
	for ( node = map.entities->first; node != nullptr; node = node->next )
	{
		Entity* entity = newEntity(((Entity*)node->element)->sprite, 1, undomap->entities, nullptr);

		setSpriteAttributes(entity, (Entity*)node->element, (Entity*)node->element);
	}

	// add the new node to the undo list
	node = list_AddNodeLast(&undolist);
	node->element = undomap;
	node->deconstructor = &mapDeconstructor;
	if ( list_Size(&undolist) > MAXUNDOS + 1 )
	{
		list_RemoveNode(undolist.first);
	}
	undospot = node;
	redospot = nullptr;
}

void clearUndos()
{
	list_FreeAll(&undolist);
	undospot = nullptr;
	redospot = nullptr;
}

/*-------------------------------------------------------------------------------

	undo() / redo()

	self explanatory

-------------------------------------------------------------------------------*/

void undo()
{
	node_t* node;
	if ( undospot == NULL )
	{
		return;
	}
	selectedEntity[0] = NULL;
	if ( undospot == undolist.last )
	{
		node_t* tempnode = undospot;
		makeUndo();
		undospot = tempnode;
	}
	free(map.tiles);
	free(camera.vismap);
	map_t* undomap = (map_t*)undospot->element;
	map.width = undomap->width;
	map.height = undomap->height;
	map.numLayers = undomap->numLayers;
	map.roomGroups = undomap->roomGroups;
	map.tiles = (Sint32*) malloc(sizeof(Sint32) * map.width * map.height * MAPLAYERS);
	camera.vismap = (bool*) malloc(sizeof(bool) * map.height * map.width);
    memset(camera.vismap, 0, sizeof(bool) * map.height * map.width);
	memcpy(map.tiles, undomap->tiles, sizeof(Sint32)*undomap->width * undomap->height * MAPLAYERS);
	list_FreeAll(map.entities);
	for ( node = undomap->entities->first; node != NULL; node = node->next )
	{
		Entity* entity = newEntity(((Entity*)node->element)->sprite, 1, map.entities, nullptr);

		setSpriteAttributes(entity, (Entity*)node->element, (Entity*)node->element);
	}
	roomSelectResetSelection();
	if ( redospot != NULL )
	{
		redospot = redospot->prev;
	}
	else
	{
		redospot = undospot->next;
	}
	undospot = undospot->prev;
}

void redo()
{
	node_t* node;

	if ( redospot == NULL )
	{
		return;
	}
	selectedEntity[0] = NULL;
	free(map.tiles);
	free(camera.vismap);
	map_t* undomap = (map_t*)redospot->element;
	map.width = undomap->width;
	map.height = undomap->height;
	map.numLayers = undomap->numLayers;
	map.roomGroups = undomap->roomGroups;
	map.tiles = (Sint32*) malloc(sizeof(Sint32) * map.width * map.height * MAPLAYERS);
	camera.vismap = (bool*) malloc(sizeof(bool) * map.height * map.width);
    memset(camera.vismap, 0, sizeof(bool) * map.height * map.width);
	memcpy(map.tiles, undomap->tiles, sizeof(Sint32)*undomap->width * undomap->height * MAPLAYERS);
	list_FreeAll(map.entities);
	for ( node = undomap->entities->first; node != NULL; node = node->next )
	{
		Entity* entity = newEntity(((Entity*)node->element)->sprite, 1, map.entities, nullptr);

		setSpriteAttributes(entity, (Entity*)node->element, (Entity*)node->element);
	}
	roomSelectResetSelection();
	if ( undospot != NULL )
	{
		undospot = undospot->next;
	}
	else
	{
		undospot = redospot->prev;
	}
	redospot = redospot->next;
}

void processCommandLine(int argc, char** argv)
{
	int c = 0;
	size_t datadirsz = std::min(sizeof(datadir) - 1, strlen(BASE_DATA_DIR));
	strncpy(datadir, BASE_DATA_DIR, datadirsz);
	datadir[datadirsz] = '\0';
	if ( argc > 1 )
	{
		for ( c = 1; c < argc; c++ )
		{
			if ( argv[c] != nullptr )
			{
				if ( !strcmp(argv[c], "--nosteam") )
				{
#ifdef STEAMWORKS
					steamRuntimeDisableByCommandLine();
#endif
				}
				else if ( !strncmp(argv[c], "-map=", 5) )
				{
					strcpy(maptoload, argv[c] + 5);
					loadingmap = true;
				}
				else if (!strncmp(argv[c], "-datadir=", 9))
				{
					datadirsz = std::min(sizeof(datadir) - 1, strlen(argv[c] + 9));
					strncpy(datadir, argv[c] + 9, datadirsz);
					datadir[datadirsz] = '\0';
				}
				else if ( !strncmp(argv[c], "-xres=", 6) )
				{
					char buf[32];
					size_t len = std::min(sizeof(buf), strlen(argv[c] + 6));
					strncpy(buf, argv[c] + 6, len);
					buf[len] = '\0';
					xres = atoi(buf);
				}
				else if ( !strncmp(argv[c], "-yres=", 6) )
				{
					char buf[32];
					size_t len = std::min(sizeof(buf), strlen(argv[c] + 6));
					strncpy(buf, argv[c] + 6, len);
					buf[len] = '\0';
					yres = atoi(buf);
				}
			}
		}
	}
	printlog("Data path is %s", datadir);
}

/*-------------------------------------------------------------------------------

loadTilePalettes

loads the tile palette file for the editor.

-------------------------------------------------------------------------------*/

int loadTilePalettes()
{
	char filename[128] = { 0 };
	File* fp;
	int c;

	// open log file
	if ( !logfile )
	{
		openLogFile();
	}

	// compose filename
	strcpy(filename, "editor/tilepalettes.txt");

	// check if palette file is valid
	if ( !dataPathExists(filename) )
	{
		// palette file doesn't exist
		printlog("error: unable to locate tile palette file: '%s'", filename);
		return 1;
	}

	// open palette file
	if ( (fp = openDataFile(filename, "rb")) == NULL )
	{
		printlog("error: unable to load tile palette file: '%s'", filename);
		return 1;
	}

	// read file
	int paletteNumber = 0;
	int paletteTile = 0;
	bool lockValueEntry = 0;
	for (; !(fp->eof()); )
	{
		//printlog( "loading line %d...\n", line);
		char data[1024];

		// read line from file
		int i;
		bool fileEnd = false;
		for ( i = 0; ; i++ )
		{
			data[i] = fp->getc();
			if ( fp->eof() )
			{
				fileEnd = true;
				break;
			}

			// blank or comment lines stop reading at a newline
			if ( data[i] == '\n' )
			{
				break;
			}
		}

		if ( fileEnd )
		{
			break;
		}

		// skip blank and comment lines
		if ( data[0] == '\n' || data[0] == '#' )
		{
			continue;
		}

		// process line
		if ( !lockValueEntry )
		{
			recentUsedTiles[paletteNumber][paletteTile] = atoi(data);
			//printlog("read tile number '%d', pattern '%d', data '%d' \n", paletteTile, paletteNumber, atoi(data));
			++paletteTile;
			if ( paletteTile == 9 )
			{
				paletteTile = 0;
				paletteNumber++;
				lockValueEntry = true;
			}
		}
		else
		{
			lockTilePalette[paletteNumber - 1] = atoi(data);
			//printlog("read lock value for palette '%d', data '%d' \n", paletteNumber - 1, atoi(data));
			lockValueEntry = false;
		}
	}

	// close file
	FileIO::close(fp);
	printlog("successfully loaded tile palette file '%s'\n", filename);
	return 0;
}

/*-------------------------------------------------------------------------------

saveTilePalettes()

saves the tile palette file for the editor.

-------------------------------------------------------------------------------*/

int saveTilePalettes()
{
	char filename[128] = { 0 };
	File* fp;
	int c;

	// open log file
	if ( !logfile )
	{
		openLogFile();
	}

	// compose filename
	strcpy(filename, "editor/tilepalettes.txt");

	// check if palette file is valid
	if ( !dataPathExists(filename) )
	{
		// palette file doesn't exist
		printlog("error: unable to locate existing tile palette file: '%s'...\ncreating...", filename);
	}

	// open/create palette file
	if ( (fp = openDataFile(filename, "w")) == NULL )
	{
		printlog("error: unable to save or create tile palette file: '%s'", filename);
		return 1;
	}

	// write file
	Uint32 line;
	int paletteNumber = 0;
	int paletteTile = 0;
	bool lockValueEntry = 0;
	char data[128];

	fp->puts("# Tile palette file\n");
	fp->puts("# lines beginning with pound character are a comment\n");
	fp->puts("# blank lines are ignored\n");
	fp->puts("");

	for ( paletteNumber = 0; paletteNumber < 9; paletteNumber++ )
	{
		paletteTile = 0;
		snprintf(data, sizeof(data), "# palette %d tiles\n", paletteNumber + 1);
		fp->puts(data);
		fp->puts("\n");
		for ( paletteTile = 0; paletteTile < 9; paletteTile++ )
		{
			if ( paletteTile == 3 || paletteTile == 6 )
			{
				fp->puts("\n");
			}
			snprintf(data, sizeof(data), "%d\n", recentUsedTiles[paletteNumber][paletteTile]);
			fp->puts(data);
		}
		fp->puts("\n");
		snprintf(data, sizeof(data), "# palette %d locked (1) or unlocked (0)\n", paletteNumber + 1);
		fp->puts(data);
		fp->puts("\n");
		snprintf(data, sizeof(data), "%d\n", lockTilePalette[paletteNumber]);
		fp->puts(data);
		fp->puts("\n");
	}

	fp->puts("# end\n");

	// close file
	FileIO::close(fp);
	printlog("saved tile palette file '%s'\n", filename);
	return 0;
}

/*-------------------------------------------------------------------------------

updateRecentTileList

Updates the tile palette in the editor if not locked, takes tile as input and either 
inserts into an empty slot, or shifts the palette to accomodate. 

-------------------------------------------------------------------------------*/

void updateRecentTileList(int tile)
{
	int checkEmpty = -1;

	for ( int i = 0; i < 9; i++ )
	{
		if ( recentUsedTiles[recentUsedTilePalette][i] == tile )
		{
			lastPaletteTileSelected = i;
			return; // tile exists in recent list.
		}

		if ( recentUsedTiles[recentUsedTilePalette][i] == 0 && checkEmpty == -1 )
		{
			checkEmpty = i; // index of next empty tile.
			lastPaletteTileSelected = checkEmpty;
		}
	}

	if ( lockTilePalette[recentUsedTilePalette] == 1 )
	{
		return; // palette locked, don't change.
	}

	if ( checkEmpty == -1 )
	{
		for ( int j = 8; j > 0; j-- )
		{
			recentUsedTiles[recentUsedTilePalette][j] = recentUsedTiles[recentUsedTilePalette][j - 1]; // shift array by 1 to insert new tile as the array is full.
		}
		recentUsedTiles[recentUsedTilePalette][0] = tile; // insert tile into array.
		lastPaletteTileSelected = 0;
	}
	else
	{
		recentUsedTiles[recentUsedTilePalette][checkEmpty] = tile; // insert tile into array.
	}

	return;
}

/*-------------------------------------------------------------------------------

	main

	Initializes program resources, harbors main loop, and cleans up
	afterwords

-------------------------------------------------------------------------------*/

bool selectingspace = false;
int selectedarea_x1, selectedarea_x2;
int selectedarea_y1, selectedarea_y2;
bool selectedarea = false;
bool pasting = false;

#ifdef APPLE
#include <mach-o/dyld.h> //For _NSGetExecutablePath()
#endif

int main(int argc, char** argv)
{
#ifdef APPLE
	uint32_t buffsize = 4096;
	char binarypath[buffsize];
	int result = _NSGetExecutablePath(binarypath, &buffsize);
	if (result == 0)   //It worked.
	{
		printlog( "Binary path: %s\n", binarypath);
		char* last = strrchr(binarypath, '/');
		*last = '\0';
		char execpath[buffsize];
		strcpy(execpath, binarypath);
		//char* last = strrchr(execpath, '/');
		//strcat(execpath, '/');
		//strcat(execpath, "/../../../");
		printlog( "Chrooting to directory: %s\n", execpath);
		chdir(execpath);
		///Users/ciprian/barony/barony-sdl2-take2/barony.app/Contents/MacOS/barony
		chdir("..");
		chdir("..");
		chdir("..");
		chdir("barony.app/Contents/Resources");
		//chdir("..");
	}
	else
	{
		printlog( "Failed to get binary path. Program may not work correctly!\n");
	}
#endif

#ifdef LINUX
	(void)chdir(BASE_DATA_DIR); // fixes a lot of headaches...
#endif

	button_t* button;
	node_t* node;
	node_t* nextnode;
	Entity* entity;
	SDL_Rect pos;
	int c;
	int x, y, z;
	int x2, y2;
	//char action[32];
	int oslidery = 0;
	light_t* light = nullptr;
	bool savedundo = false;
	smoothlighting = true;

	Stat* spriteStats = nullptr;

	processCommandLine(argc, argv);

#ifdef WINDOWS
	strcpy(outputdir, "./");
#else
	char *basepath = getenv("HOME");
	snprintf(outputdir, sizeof(outputdir), "%s/.barony", basepath);
	if ( access(outputdir, F_OK) == -1 )
		mkdir(outputdir, 0777);
#endif

	// load default language file (english)
	if ( Language::loadLanguage("en", true) )
	{
		exit(1);
	}
    
    // init sdl
    Uint32 init_flags = SDL_INIT_VIDEO | SDL_INIT_EVENTS;
    init_flags |= SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC;
    if (SDL_Init(init_flags) == -1)
    {
        printlog("failed to initialize SDL: %s\n", SDL_GetError());
        return 1;
    }

	// initialize
	verticalSync = true;
	if ( (x = initApp("Barony Editor", fullscreen)) )
	{
		printlog("Critical error: %d\n", x);
#ifdef STEAMWORKS
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Uh oh",
								"Barony has encountered a critical error and cannot start.\n\n"
								"Please check the log.txt file in the game directory for additional info\n"
								"and verify Steam is running. Alternatively, contact us through our website\n"
								"at http://www.baronygame.com/ for support.",
								screen);
#else
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Uh oh",
								"Barony has encountered a critical error and cannot start.\n\n"
								"Please check the log.txt file in the game directory for additional info,\n"
								"or contact us through our website at http://www.baronygame.com/ for support.",
								screen);
#endif
		deinitApp();
		exit(x);
	}
	
#ifdef STEAMWORKS
	if ( steamRuntimeAvailable() && g_SteamStatistics )
	{
		g_SteamStatistics->RequestStats();
	}
#endif // STEAMWORKS


	copymap.tiles = nullptr;
	copymap.entities = nullptr;
	copymap.creatures = nullptr;
	copymap.worldUI = nullptr;
	undolist.first = nullptr;
	undolist.last = nullptr;

	// Load Cursors
	cursorArrow = SDL_GetCursor();
	cursorPencil = newCursor(cursor_pencil);
	cursorPoint = newCursor(cursor_point);
	cursorBrush = newCursor(cursor_brush);
	cursorSelect = cursorArrow;
	cursorFill = newCursor(cursor_fill);

	// create an empty map
	map.width = 32;
	map.height = 24;
	map.numLayers = MAPLAYERS;
	authoredRoomGroupsReset(map.roomGroups);
	map.entities = (list_t*) malloc(sizeof(list_t));
	map.creatures = nullptr;
	map.worldUI = nullptr;
	map.entities->first = nullptr;
	map.entities->last = nullptr;
	map.tiles = (int*) malloc(sizeof(int) * map.width * map.height * MAPLAYERS);
	camera.vismap = (bool*) malloc(sizeof(bool) * map.height * map.width);
    memset(camera.vismap, 0, sizeof(bool) * map.height * map.width);
	strcpy(map.name, "");
	strcpy(map.author, "");
	map.skybox = 0;
	for ( c = 0; c < MAPFLAGS; c++ )
	{
		map.flags[c] = 0;
	}
	for ( z = 0; z < MAPLAYERS; z++ )
	{
		for ( y = 0; y < map.height; y++ )
		{
			for ( x = 0; x < map.width; x++ )
			{
				if (z == OBSTACLELAYER)
				{
					if (x == 0 || y == 0 || x == map.width - 1 || y == map.height - 1)
					{
						map.tiles[z + y * MAPLAYERS + x * MAPLAYERS * map.height] = 2;
					}
					else
					{
						map.tiles[z + y * MAPLAYERS + x * MAPLAYERS * map.height] = 0;
					}
				}
				else if (z >= 3)
				{
					// Higher layers default to air (0)
					map.tiles[z + y * MAPLAYERS + x * MAPLAYERS * map.height] = 0;
				}
				else
				{
					map.tiles[z + y * MAPLAYERS + x * MAPLAYERS * map.height] = 1;
				}
			}
		}
	}
    clearAdditionalPlayableFloorLightmaps();
    for (int c = 0; c < MAXPLAYERS + 1; ++c) {
        lightmaps[c].clear();
        lightmaps[c].resize(
			lightmapSize3D(
				map.width,
				map.height
			)
		);
        lightmapsSmoothed[c].clear();

		lightmapsSmoothed[c].resize(
			lightmapSmoothedSize3D(
				map.width,
				map.height
			)
		);
    }

	// initialize camera position
	camera.x = 4;
	camera.y = 4;
	camera.z = 0;
	camera.ang = 0;
	camera.vang = 0;

	// initialize editor settings
	strcpy(layerstatus, "BACKGROUND");
	palette = (int*) malloc(sizeof(unsigned int) * xres * yres);

	// main interface
	button = butFile = newButton();
	strcpy(button->label, "File");
	button->x = 0;
	button->y = 0;
	button->sizex = 40;
	button->sizey = 16;
	button->action = &buttonFile;

	button = butEdit = newButton();
	strcpy(button->label, "Edit");
	button->x = 40;
	button->y = 0;
	button->sizex = 40;
	button->sizey = 16;
	button->action = &buttonEdit;

	button = butView = newButton();
	strcpy(button->label, "View");
	button->x = 80;
	button->y = 0;
	button->sizex = 40;
	button->sizey = 16;
	button->action = &buttonView;

	button = butMap = newButton();
	strcpy(button->label, "Map");
	button->x = 120;
	button->y = 0;
	button->sizex = 32;
	button->sizey = 16;
	button->action = &buttonMap;

	button = butDialogue = newButton();
	strcpy(button->label, "Dialogue");
	button->x = 152;
	button->y = 0;
	button->sizex = 64;
	button->sizey = 16;
	button->action = &buttonDialogue;

	button = butScripts = newButton();
	strcpy(button->label, "Scripts");
	button->x = 216;
	button->y = 0;
	button->sizex = 56;
	button->sizey = 16;
	button->action = &buttonScripts;

	button = butHelp = newButton();
	strcpy(button->label, "Help");
	button->x = 272;
	button->y = 0;
	button->sizex = 40;
	button->sizey = 16;
	button->action = &buttonHelp;

	button = butX = newButton();
	strcpy(button->label, "X");
	button->x = xres - 16;
	button->y = 0;
	button->sizex = 16;
	button->sizey = 16;
	button->action = &buttonExit;
	button->visible = 0;

	button = but_ = newButton();
	strcpy(button->label, "_");
	button->x = xres - 32;
	button->y = 0;
	button->sizex = 16;
	button->sizey = 16;
	button->action = &buttonIconify;
	button->visible = 0;

	// toolbox
	button = butTilePalette = newButton();
	strcpy(button->label, "Palette ...");
	button->x = xres - 112;
	button->y = 152;
	button->sizex = 96;
	button->sizey = 16;
	button->action = &buttonTilePalette;

	button = butSprite = newButton();
	strcpy(button->label, "Sprite  ...");
	button->x = xres - 112;
	button->y = 168;
	button->sizex = 96;
	button->sizey = 16;
	button->action = &buttonSprite;

	// Pencil Tool Button
	button = butPencil = newButton();
	strcpy(button->label, "Pencil");
	button->x = xres - 96;
	button->y = 204;
	button->sizex = 64;
	button->sizey = 16;
	button->action = &buttonPencil;

	// Point Tool Button
	button = butPoint = newButton();
	strcpy(button->label, "Point");
	button->x = xres - 96;
	button->y = 220;
	button->sizex = 64;
	button->sizey = 16;
	button->action = &buttonPoint;

	// Brush Tool Button
	button = butBrush = newButton();
	strcpy(button->label, "Brush");
	button->x = xres - 96;
	button->y = 236;
	button->sizex = 64;
	button->sizey = 16;
	button->action = &buttonBrush;

	// Select Tool Button
	button = butSelect = newButton();
	strcpy(button->label, "Select");
	button->x = xres - 96;
	button->y = 252;
	button->sizex = 64;
	button->sizey = 16;
	button->action = &buttonSelect;

	// Fill Tool Button
	button = butFill = newButton();
	strcpy(button->label, "Fill");
	button->x = xres - 96;
	button->y = 268;
	button->sizex = 64;
	button->sizey = 16;
	button->action = &buttonFill;

	// file menu
	butNew = button = newButton();
	strcpy(button->label, "New          Ctrl+N");
	button->x = 16;
	button->y = 16;
	button->sizex = 160;
	button->sizey = 16;
	button->action = &buttonNew;
	button->visible = 0;

	butOpen = button = newButton();
	strcpy(button->label, "Open ...     Ctrl+O");
	button->x = 16;
	button->y = 32;
	button->sizex = 160;
	button->sizey = 16;
	button->action = &buttonOpen;
	button->visible = 0;

	butDir = button = newButton();
	strcpy(button->label, "Directory... Ctrl+D");
	button->x = 16;
	button->y = 48;
	button->sizex = 160;
	button->sizey = 16;
	button->action = &buttonOpenDirectory;
	button->visible = 0;

	butSave = button = newButton();
	strcpy(button->label, "Save         Ctrl+S");
	button->x = 16;
	button->y = 64;
	button->sizex = 160;
	button->sizey = 16;
	button->action = &buttonSave;
	button->visible = 0;

	butSaveAs = button = newButton();
	strcpy(button->label, "Save As ...        ");
	button->x = 16;
	button->y = 80;
	button->sizex = 160;
	button->sizey = 16;
	button->action = &buttonSaveAs;
	button->visible = 0;

	butExit = button = newButton();
	strcpy(button->label, "Exit         Alt+F4");
	button->x = 16;
	button->y = 96;
	button->sizex = 160;
	button->sizey = 16;
	button->action = &buttonExit;
	button->visible = 0;

	// edit menu
	butCut = button = newButton();
	strcpy(button->label, "Cut         Ctrl+X");
	button->x = 56;
	button->y = 16;
	button->sizex = 152;
	button->sizey = 16;
	button->action = &buttonCut;
	button->visible = 0;

	butCopy = button = newButton();
	strcpy(button->label, "Copy        Ctrl+C");
	button->x = 56;
	button->y = 32;
	button->sizex = 152;
	button->sizey = 16;
	button->action = &buttonCopy;
	button->visible = 0;

	butPaste = button = newButton();
	strcpy(button->label, "Paste       Ctrl+V");
	button->x = 56;
	button->y = 48;
	button->sizex = 152;
	button->sizey = 16;
	button->action = &buttonPaste;
	button->visible = 0;

	butDelete = button = newButton();
	strcpy(button->label, "Delete      Del   ");
	button->x = 56;
	button->y = 64;
	button->sizex = 152;
	button->sizey = 16;
	button->action = &buttonDelete;
	button->visible = 0;

	butSelectAll = button = newButton();
	strcpy(button->label, "Select All  Ctrl+A");
	button->x = 56;
	button->y = 80;
	button->sizex = 152;
	button->sizey = 16;
	button->action = &buttonSelectAll;
	button->visible = 0;

	butUndo = button = newButton();
	strcpy(button->label, "Undo        Ctrl+Z");
	button->x = 56;
	button->y = 96;
	button->sizex = 152;
	button->sizey = 16;
	button->action = &buttonUndo;
	button->visible = 0;

	butRedo = button = newButton();
	strcpy(button->label, "Redo        Ctrl+Y");
	button->x = 56;
	button->y = 112;
	button->sizex = 152;
	button->sizey = 16;
	button->action = &buttonRedo;
	button->visible = 0;

	// view menu
	butStatusBar = button = newButton();
	strcpy(button->label, "Statusbar   Ctrl+I");
	button->x = 96;
	button->y = 16;
	button->sizex = 152;
	button->sizey = 16;
	button->action = &buttonStatusBar;
	button->visible = 0;

	butToolbox = button = newButton();
	strcpy(button->label, "Toolbox     Ctrl+T");
	button->x = 96;
	button->y = 32;
	button->sizex = 152;
	button->sizey = 16;
	button->action = &buttonToolbox;
	button->visible = 0;

	butAllLayers = button = newButton();
	strcpy(button->label, "All Layers  Ctrl+L");
	button->x = 96;
	button->y = 48;
	button->sizex = 152;
	button->sizey = 16;
	button->action = &buttonAllLayers;
	button->visible = 0;

	butViewSprites = button = newButton();
	strcpy(button->label, "Sprites     Ctrl+E");
	button->x = 96;
	button->y = 64;
	button->sizex = 152;
	button->sizey = 16;
	button->action = &buttonViewSprites;
	button->visible = 0;

	butGrid = button = newButton();
	strcpy(button->label, "Grid        Ctrl+G");
	button->x = 96;
	button->y = 80;
	button->sizex = 152;
	button->sizey = 16;
	button->action = &buttonGrid;
	button->visible = 0;

	but3DMode = button = newButton();
	strcpy(button->label, "3D Mode     Ctrl+F");
	button->x = 96;
	button->y = 96;
	button->sizex = 152;
	button->sizey = 16;
	button->action = &button3DMode;
	button->visible = 0;

	but3DModels = button = newButton();
	strcpy(button->label, "3D Models");
	button->x = 96;
	button->y = 112;
	button->sizex = 152;
	button->sizey = 16;
	button->action = &button3DModels;
	button->visible = 0;

	butHoverText = button = newButton();
	strcpy(button->label, "Hover Text  Ctrl+H");
	button->x = 96;
	button->y = 128;
	button->sizex = 152;
	button->sizey = 16;
	button->action = &buttonHoverText;
	button->visible = 0;

	// map menu
	butAttributes = button = newButton();
	strcpy(button->label, "Attributes ...  Ctrl+M      ");
	button->x = 136;
	button->y = 16;
	button->sizex = 232;
	button->sizey = 16;
	button->action = &buttonAttributes;
	button->visible = 0;

	butClearMap = button = newButton();
	strcpy(button->label, "Clear Map       Ctrl+Shift+N");
	button->x = 136;
	button->y = 32;
	button->sizex = 232;
	button->sizey = 16;
	button->action = &buttonClearMap;
	button->visible = 0;

	// dialogue menu
	butDialogueEditor = button = newButton();
	strcpy(button->label, "Edit Custom Dialogue ...");
	button->x = 168;
	button->y = 16;
	button->sizex = 208;
	button->sizey = 16;
	button->action = &buttonDialogueEditor;
	button->visible = 0;

	// scripts menu
	butTextSourceScriptTester = button = newButton();
	strcpy(button->label, "Text Source Script Tester...");
	button->x = 232;
	button->y = 16;
	button->sizex = 240;
	button->sizey = 16;
	button->action = &openTextSourceScriptTester;
	button->visible = 0;

	// help menu
	butAbout = button = newButton();
	strcpy(button->label, "About            F1");
	button->x = 288;
	button->y = 16;
	button->sizex = 160;
	button->sizey = 16;
	button->action = &buttonAbout;
	button->visible = 0;

	// controls menu
	butEditorControls = button = newButton();
	strcpy(button->label, "Editor Help       H");
	button->x = 288;
	button->y = 32;
	button->sizex = 160;
	button->sizey = 16;
	button->action = &buttonEditorControls;
	button->visible = 0;

	if ( loadingmap )
	{
		if ( loadMap(physfsFormatMapName(maptoload).c_str(), &map, map.entities, map.creatures) == -1 )
		{
			strcpy(message, "Failed to open ");
			strcat(message, maptoload);
		}
		else
		{
			strcpy(filename, maptoload);
		}
	}

	ItemTooltips.readItemsFromFile();
	ItemTooltips.readItemLocalizationsFromFile();
	ItemTooltips.readBookLocalizationsFromFile();
	for ( c = 0; c < NUMITEMS; c++ )
	{
		items[c].surfaces.first = nullptr;
		items[c].surfaces.last = nullptr;
		for ( x = 0; x < list_Size(&items[c].images); x++ )
		{
			auto** surface = static_cast<SDL_Surface**>(malloc(sizeof(SDL_Surface*)));
			node_t* node = list_AddNodeLast(&items[c].surfaces);
			node->element = surface;
			node->deconstructor = &defaultDeconstructor;
			node->size = sizeof(SDL_Surface*);

			node_t* node2 = list_Node(&items[c].images, x);
			auto* string = static_cast<string_t*>(node2->element);
			*surface = loadImage(string->data);
		}
	}
	loadTilePalettes();
	EditorEntityData_t::readFromFile();

	/*
	 * Sprite 42 is the reverse-ladder editor object, but it should use the
	 * same 2D editor image as the normal ladder (sprite 11). Keep a separate
	 * SDL surface so normal sprite cleanup remains safe.
	 */
	if ( numsprites > 42 && sprites[11] != nullptr )
	{
		SDL_Surface* ladderReverseEditorImage =
			SDL_ConvertSurface(sprites[11], sprites[11]->format, 0);
		if ( ladderReverseEditorImage != nullptr )
		{
			if ( sprites[42] != nullptr )
			{
				SDL_FreeSurface(sprites[42]);
			}
			sprites[42] = ladderReverseEditorImage;
		}
	}

	bool achievementCartographer = false;

	// main loop
	printlog( "running main loop.\n");
	while (mainloop)
	{
		// game logic
		(void)handleEvents();

#ifdef STEAMWORKS
		if ( steamRuntimeAvailable() )
		{
			SteamAPI_RunCallbacks();
			if ( SteamUser() && SteamUserStats()
				&& SteamUser()->BLoggedOn() && !achievementCartographer )
			{
				SteamUserStats()->SetAchievement("BARONY_ACH_CARTOGRAPHER");
				achievementCartographer = true;
				SteamUserStats()->StoreStats();
				//printlog("STEAM ACHIEVEMENT\n");
			}
		}
#endif

		// move buttons
		/*if( !fullscreen ) {
			butX->visible = 0;
			but_->visible = 0;
		} else {
			butX->visible = 1;
			but_->visible = 1;
			butX->x = xres-16;
			but_->x = xres-32;
		}*/
		butTilePalette->x = xres - 112;
		butSprite->x = xres - 112;
		butPencil->x = xres - 96;
		butPoint->x = xres - 96;
		butBrush->x = xres - 96;
		butSelect->x = xres - 96;
		butFill->x = xres - 96;

		bool wasTextEditable = SDL_IsTextInputActive();
		if ( !wasTextEditable )
		{
			textInsertCaratPosition = -1;
		}

		GL_CHECK_ERR(glViewport(0, 0, xres, yres)); // fix for resizing editor with hdr enabled.

		if ( !spritepalette && !tilepalette )
		{
			allowediting = 1;
			if ( (omousex >= xres - 128 && toolbox) || omousey < 16 || (omousey >= yres - 16 && statusbar) || subwindow || menuVisible )
			{
				allowediting = 0;
			}
			if ( mode3d )
			{
				allowediting = 0;
			}
			if ( menuVisible == 1 )
			{
				if ((omousex > 16 + butNew->sizex || omousey > 112 || (omousey < 16 && omousex > 192)) && mousestatus[SDL_BUTTON_LEFT])
				{
					menuVisible = 0;
					menuDisappear = 1;
				}
			}
			else if ( menuVisible == 2 )
			{
				if ((omousex > 56 + butCut->sizex || omousex < 40 || omousey > 128 || (omousey < 16 && omousex > 192)) && mousestatus[SDL_BUTTON_LEFT])
				{
					menuVisible = 0;
					menuDisappear = 1;
				}
			}
			else if ( menuVisible == 3 )
			{
				if ((omousex > 96 + butToolbox->sizex || omousex < 80 || omousey > 128 || (omousey < 16 && omousex > 192)) && mousestatus[SDL_BUTTON_LEFT])
				{
					menuVisible = 0;
					menuDisappear = 1;
				}
			}
			else if ( menuVisible == 4 )
			{
				if ((omousex > 136 + butClearMap->sizex || omousex < 120 || omousey > 48 || (omousey < 16 && omousex > 192)) && mousestatus[SDL_BUTTON_LEFT])
				{
					menuVisible = 0;
					menuDisappear = 1;
				}
			}
			else if ( menuVisible == 5 )
			{
				if ((omousex > 168 + butDialogueEditor->sizex || omousex < 152 || omousey > 32 || (omousey < 16 && omousex > 216)) && mousestatus[SDL_BUTTON_LEFT])
				{
					menuVisible = 0;
					menuDisappear = 1;
				}
			}
			else if ( menuVisible == 7 )
			{
				if ((omousex > 232 + butTextSourceScriptTester->sizex || omousex < 216 || omousey > 32 || (omousey < 16 && omousex > 272)) && mousestatus[SDL_BUTTON_LEFT])
				{
					menuVisible = 0;
					menuDisappear = 1;
				}
			}
			else if ( menuVisible == 6 )
			{
				if ((omousex > 288 + butAbout->sizex || omousex < 272 || omousey > 48 || (omousey < 16 && omousex > 312)) && mousestatus[SDL_BUTTON_LEFT])
				{
					menuVisible = 0;
					menuDisappear = 1;
				}
			}
			if ( !mousestatus[SDL_BUTTON_LEFT] )
			{
				menuDisappear = 0;
			}

			if ( allowediting && !menuDisappear )
			{
				// MAIN LEVEL EDITING
				drawx = (mousex + camx) >> TEXTUREPOWER;
				drawy = (mousey + camy) >> TEXTUREPOWER;
				odrawx = (omousex + ocamx) >> TEXTUREPOWER;
				odrawy = (omousey + ocamy) >> TEXTUREPOWER;

				// Set the Cursor to the corresponding tool
				switch ( selectedTool )
				{
					case 0: // Pencil
						SDL_SetCursor(cursorPencil);
						break;
					case 1: // Point
						SDL_SetCursor(cursorPoint);
						break;
					case 2: // Brush
						SDL_SetCursor(cursorBrush);
						break;
					case 3: // Select
						SDL_SetCursor(cursorSelect);
						break;
					case 4: // Fill
						SDL_SetCursor(cursorFill);
						break;
					default:
						SDL_SetCursor(cursorArrow);
						break;
				}

				// Move Entities
				if ( map.entities->first != NULL && viewsprites && allowediting )
				{
					for ( node = map.entities->first; node != NULL; node = nextnode )
					{
						nextnode = node->next;
						entity = (Entity*)node->element;
							if ( entityAuthoredSpriteLayer(entity) != drawlayer )
							{
								continue;
							}
						if ( entity == selectedEntity[0] )
						{
							if ( mousestatus[SDL_BUTTON_LEFT] )
							{
								if ( newwindow == 0 )
								{
									// if the entity moved from where it was picked up, or if the sprite was right click duplicated, store an undo.
									if ( selectedEntity[0]->x / 16 != prev_x || selectedEntity[0]->y / 16 != prev_y || duplicatedSprite )
									{
										duplicatedSprite = false;
										makeUndo();
									}
								}
								mousestatus[SDL_BUTTON_LEFT] = 0;
								selectedEntity[0] = NULL;
								break;
							}
							else if ( mousestatus[SDL_BUTTON_RIGHT] )
							{
								if ( newwindow == 0 )
								{
									// if previous sprite was duplicated and another right click is registered, store an undo.
									if ( duplicatedSprite )
									{
										makeUndo();
									}
									duplicatedSprite = true;
								}
								selectedEntity[0] = newEntity(entity->sprite, 0, map.entities, nullptr);
								
								setSpriteAttributes(selectedEntity[0], entity, lastSelectedEntity[0]);

								lastSelectedEntity[0] = selectedEntity[0];

								mousestatus[SDL_BUTTON_RIGHT] = 0;
								break;
							}
							entity->x = (long)(drawx << 4);
							entity->y = (long)(drawy << 4);
							
						}
						else
						{
							if ( (omousex + camx) >> TEXTUREPOWER == entity->x / 16 && (omousey + camy) >> TEXTUREPOWER == entity->y / 16 )
							{
								if ( mousestatus[SDL_BUTTON_LEFT] && selectedTool == 1 )
								{
									// select sprite
									selectedEntity[0] = entity;
									lastSelectedEntity[0] = selectedEntity[0];
									prev_x = entity->x / 16;
									prev_y = entity->y / 16;
									mousestatus[SDL_BUTTON_LEFT] = 0;
									if ( newwindow == 0 && selectedEntity[0] != NULL )
									{
										makeUndo();
									}
								}
								else if ( mousestatus[SDL_BUTTON_RIGHT] && selectedTool == 1 )
								{
									// duplicate sprite
									duplicatedSprite = true;
									if ( newwindow == 0 )
									{
										makeUndo();
									}
									selectedEntity[0] = newEntity(entity->sprite, 0, map.entities, nullptr);
									lastSelectedEntity[0] = selectedEntity[0];

									setSpriteAttributes(selectedEntity[0], entity, entity);

									mousestatus[SDL_BUTTON_RIGHT] = 0;
								}
							}
						}
					}
				}

				// Modify World
				if ( mousestatus[SDL_BUTTON_LEFT] && selectedEntity[0] == NULL )
				{
					if ( allowediting )
					{
						if ( !savedundo )
						{
							savedundo = true;
							makeUndo();
						}
						if ( !pasting )   // Not Pasting, Normal Editing Mode
						{
							if ( selectedTool == 0 )		// Process Pencil Tool functionality
							{
								if ( drawx >= 0 && drawx < map.width && drawy >= 0 && drawy < map.height )
								{
									map.tiles[drawlayer + drawy * MAPLAYERS + drawx * MAPLAYERS * map.height] = selectedTile;
								}
							}
							else if ( selectedTool == 1 )	// Process Point Tool functionality
							{
								// All functionality of the Point Tool is encapsulated above in the "Move Entities" section
							}
							else if ( selectedTool == 2 )	// Process Brush Tool functionality
							{
								for ( x = drawx - 1; x <= drawx + 1; x++ )
								{
									for ( y = drawy - 1; y <= drawy + 1; y++ )
									{
										if ( (x != drawx - 1 || y != drawy - 1) && (x != drawx + 1 || y != drawy - 1) && (x != drawx - 1 || y != drawy + 1) && (x != drawx + 1 || y != drawy + 1) )
										{
											if ( x >= 0 && x < map.width && y >= 0 && y < map.height )
											{
												map.tiles[drawlayer + y * MAPLAYERS + x * MAPLAYERS * map.height] = selectedTile;
											}
										}
									}
								}
							}
							else if ( selectedTool == 3 )	// Process Select Tool functionality
							{
									if ( !selectingspace )
									{
										if ( drawx >= 0
											&& drawy >= 0
											&& drawx < static_cast<int>(map.width)
											&& drawy < static_cast<int>(map.height) )
										{
											selectingspace = true;
											selectedarea_x1 = drawx;
											selectedarea_x2 = drawx;
											selectedarea_y1 = drawy;
											selectedarea_y2 = drawy;
											roomSelectBottomLayer = drawlayer;
											roomSelectTopLayer = drawlayer;
											selectedarea = true;
											roomSelectStage = 1;
										}
										else
										{
											selectedarea = false;
										}
									}
									else
									{
										selectedarea_x1 = std::max(
											0,
											std::min(
												std::min(odrawx, drawx),
												static_cast<int>(map.width) - 1
											)
										);
										selectedarea_x2 = std::max(
											0,
											std::min(
												std::max(odrawx, drawx),
												static_cast<int>(map.width) - 1
											)
										);
										selectedarea_y1 = std::max(
											0,
											std::min(
												std::min(odrawy, drawy),
												static_cast<int>(map.height) - 1
											)
										);
										selectedarea_y2 = std::max(
											0,
											std::min(
												std::max(odrawy, drawy),
												static_cast<int>(map.height) - 1
											)
										);

										roomSelectBottomLayer = drawlayer;
										roomSelectTopLayer = drawlayer;
										roomSelectStage = 1;

										if ( map.entities->first
											&& viewsprites
											&& allowediting )
										{
											reselectEntityGroup();
											moveSelectionNegativeX = false;
											moveSelectionNegativeY = false;
										}
									}
							}
							else if ( selectedTool == 4 )	// Process Fill Tool functionality
							{
								if ( drawx >= 0 && drawx < map.width && drawy >= 0 && drawy < map.height )
								{
									editFill(drawx, drawy, drawlayer, selectedTile);
								}
							}
						}
						else
						{
							mousestatus[SDL_BUTTON_LEFT] = 0;
							editorRoomPlaceClipboard(
								drawx,
								drawy,
								drawlayer
							);
						}
					}
				}
				else if ( !mousestatus[SDL_BUTTON_LEFT] )
				{
					if ( selectedTool == 3
						&& selectingspace
						&& selectedarea )
					{
						normalizeRoomSelection();
						roomSelectStage = 2;
						reselectEntityGroup();
					}

					selectingspace = false;
					savedundo = false;
				}
				if ( mousestatus[SDL_BUTTON_RIGHT] && selectedEntity[0] == NULL )
				{
					if ( pasting )
					{
						mousestatus[SDL_BUTTON_RIGHT] = 0;
						editorRoomCancelPaste();
					}
					else if ( selectedTool != 3 )
					{
						if ( drawx >= 0 && drawx < map.width && drawy >= 0 && drawy < map.height )
						{
							selectedTile = map.tiles[drawlayer + drawy * MAPLAYERS + drawx * MAPLAYERS * map.height];
							updateRecentTileList(selectedTile);
						}
					}
					else
					{
						selectedarea = false;
					}
				}
			}
			else
			{
				SDL_SetCursor(cursorArrow);
			}

			// main drawing
			drawClearBuffers();
			if ( mode3d == false )
			{
				if ( alllayers )
					for (c = 0; c <= drawlayer; c++)
					{
						drawLayer(camx, camy, c, &map);
					}
				else
				{
					drawLayer(camx, camy, drawlayer, &map);
				}
				if ( pasting
					&& roomClipboardReady
					&& roomClipboardHasTiles
					&& roomClipboardMap.tiles )
				{
					for ( int previewLayer = 0;
						previewLayer < roomClipboardDepth;
						++previewLayer )
					{
						drawLayer(
							camx - (drawx << TEXTUREPOWER),
							camy - (drawy << TEXTUREPOWER),
							previewLayer,
							&roomClipboardMap
						);
					}

					SDL_Rect previewBounds;
					previewBounds.x =
						(drawx << TEXTUREPOWER) - camx;
					previewBounds.y =
						(drawy << TEXTUREPOWER) - camy;
					previewBounds.w =
						roomClipboardWidth << TEXTUREPOWER;
					previewBounds.h =
						roomClipboardHeight << TEXTUREPOWER;

					drawRect(
						&previewBounds,
						makeColorRGB(0, 255, 255),
						160
					);
				}
				if ( selectedarea )
				{
					pos.x = (selectedarea_x1 << TEXTUREPOWER) - camx;
					pos.y = (selectedarea_y1 << TEXTUREPOWER) - camy;
					pos.w = (selectedarea_x2 - selectedarea_x1 + 1) << TEXTUREPOWER;
					pos.h = (selectedarea_y2 - selectedarea_y1 + 1) << TEXTUREPOWER;
					drawRect(&pos, makeColorRGB(255, 255, 255), 127);
				}
				if ( viewsprites )
				{
					drawEntities2D(camx, camy);
				}
				if ( showgrid )
				{
					drawGrid(camx, camy);
				}
			}
			else
			{
				camera.winx = 0;
				camera.winy = 16;
				camera.winw = xres - 128;
				camera.winh = yres - 32;
				/*
				 * Keep the editor preview light local to the camera's current
				 * structural slice. Spawning one light on every map layer makes
				 * the contributions overlap in a derived Playable-Z light volume
				 * and badly overexposes the preview. The ordinary editor light
				 * radius is sufficient; real geometry and structural attenuation
				 * decide how much reaches adjacent layers.
				 *
				 * This is a runtime-only editor light. It is removed immediately
				 * after drawing and is never stored in the
				 * map entity list or written to the .lmp file.
				 */
				const int editor3DCameraLightLayer =
					entityZToLightmapLayer(camera.z);
				light_t* editor3DCameraLight =
					addLight(
						static_cast<Sint32>(camera.x),
						static_cast<Sint32>(camera.y),
						editor3DCameraLightLayer,
						"editor"
					);

				using Editor3DPreviewState =
					std::tuple<
						Entity*,
						bool,
						int,
						real_t,
						real_t,
						real_t,
						real_t
					>;

				std::vector<Editor3DPreviewState>
					editor3DEntityPreviewStates;
				editor3DEntityPreviewStates.reserve(
					map.entities
						? list_Size(map.entities)
						: 0
				);

				int editor3DModelCount = 0;
				int editor3DSpriteCount = 0;

				for ( node = map.entities->first;
					node != NULL;
					node = node->next )
				{
					entity =
						static_cast<Entity*>(
							node->element
						);

					if ( !entity )
					{
						continue;
					}

					editor3DEntityPreviewStates.emplace_back(
						entity,
						entity->flags[SPRITE],
						entity->sprite,
						entity->x,
						entity->y,
						entity->z,
						entity->yaw
					);

					const int editorSpriteType =
						checkSpriteType(
							entity->sprite
						);

					/*
					 * Runtime model-backed decorations use voxel models in the
					 * editor's 3D preview. Other editor entities remain flat
					 * sprites.
					 *
					 * 10 = legacy/modern ceiling tile model
					 * 13 = floor decoration
					 * 27 = collider decoration
					 */
					const bool isCeilingTile =
						editorSpriteType == 10;
					const bool isFloorDecoration =
						editorSpriteType == 13;
					const bool isColliderDecoration =
						editorSpriteType == 27;
					const bool isVerticalLayerStair =
						entity->verticalLayerTransitionDelta != 0;
					const bool isAuthoredMiniMimic =
						entity->sprite == EDITOR_SPRITE_MINIMIMIC;

					int editor3DModelIndex =
						entity->sprite;

					if ( isCeilingTile )
					{
						editor3DModelIndex =
							entity->ceilingTileModel != 0
								? entity->ceilingTileModel
								: 621;
					}
					else if ( isFloorDecoration )
					{
						editor3DModelIndex =
							entity->floorDecorationModel;
					}
					else if ( isColliderDecoration )
					{
						editor3DModelIndex =
							entity->colliderDecorationModel;
					}
					else if ( isVerticalLayerStair )
					{
						// Older ZLDR records and an in-progress editor placement can
						// legitimately carry model 0. Model 0 is system/null.vox, not a
						// stair, so mirror the runtime default before drawing the preview.
						editor3DModelIndex = entity->verticalLayerTransitionModel > 0
							? entity->verticalLayerTransitionModel
							: (entity->verticalLayerTransitionDelta > 0 ? 161 : 253);
					}

					const bool hasEditorPreviewModel =
						editor3DModelsEnabled
						&& (isCeilingTile
							|| isFloorDecoration
							|| isColliderDecoration
							|| isVerticalLayerStair)
						&& editor3DModelIndex >= 0
						&& static_cast<Uint32>(
							editor3DModelIndex
						) < nummodels
						&& modelFileNames.find(
							editor3DModelIndex
						) != modelFileNames.end();

					entity->flags[SPRITE] =
						!hasEditorPreviewModel;

					if ( isAuthoredMiniMimic
						&& !hasEditorPreviewModel )
					{
						// Match the searchable 2D palette marker in the 3D view.
						// Runtime keeps the existing Mini Mimic root and resolves the
						// authored Baby/Scaled Mimic appearance after assignActions().
						entity->sprite = 21;
					}

					if ( hasEditorPreviewModel )
					{
						entity->sprite =
							editor3DModelIndex;

						if ( isCeilingTile )
						{
							// Match assignActions(): -24 is the ceiling model's
							// local height. worldRenderZ() adds authoredMapLayer
							// exactly once for a modern Playable-Z placement.
							entity->z = -24;
							entity->yaw = entity->ceilingTileDir * (PI / 2);
						}
						else if ( isFloorDecoration
							|| isColliderDecoration
							|| isVerticalLayerStair )
						{
							const Sint32 heightOffset = isVerticalLayerStair
								? entity->floorDecorationHeightOffset
								: (isFloorDecoration ? entity->floorDecorationHeightOffset
									: entity->colliderDecorationHeightOffset);
							const Sint32 xOffset = isVerticalLayerStair
								? entity->floorDecorationXOffset
								: (isFloorDecoration ? entity->floorDecorationXOffset
									: entity->colliderDecorationXOffset);
							const Sint32 yOffset = isVerticalLayerStair
								? entity->floorDecorationYOffset
								: (isFloorDecoration ? entity->floorDecorationYOffset
									: entity->colliderDecorationYOffset);
							int decorationRotation = isVerticalLayerStair
								? entity->verticalLayerTransitionRotation
								: (isFloorDecoration ? entity->floorDecorationRotation
									: entity->colliderDecorationRotation);

							entity->z += 7.5 - heightOffset * 0.25;
							entity->x += xOffset * 0.25;
							entity->y += yOffset * 0.25;
							if ( decorationRotation < 0 )
							{
								decorationRotation = 0;
							}
							entity->yaw = decorationRotation * (PI / 4);
						}

						++editor3DModelCount;
					}
					else
					{
						++editor3DSpriteCount;
					}

					entity->x += 8;
					entity->y += 8;
				}

				occlusionCulling(map, camera);
                beginGraphics();
				glBeginCamera(&camera, false, map);
				glDrawWorld(&camera, REALCOLORS);
				//drawFloors(&camera);
				drawEntities3D(&camera, REALCOLORS);
				glEndCamera(&camera, false, map);

				printTextFormatted(
					font8x8_bmp,
					8,
					yres - 104,
					"x = %3.3f\n"
					"y = %3.3f\n"
					"z = %3.3f\n"
					"ang = %3.3f\n"
					"fps = %3.1f\n"
					"decor models = %d  sprites = %d\n"
					"camera light layer = %d\n"
					"Up/Down move  Left/Right turn\n"
					"Q/E or PgUp/PgDn height  Home reset",
					camera.x,
					camera.y,
					camera.z,
					camera.ang,
					fps,
					editor3DModelCount,
					editor3DSpriteCount,
					editor3DCameraLightLayer
				);

				if ( editor3DCameraLight
					&& editor3DCameraLight->node )
				{
					list_RemoveNode(
						editor3DCameraLight->node
					);
				}

				for ( auto& previewState :
					editor3DEntityPreviewStates )
				{
					Entity* editorEntity =
						std::get<0>(previewState);

					if ( !editorEntity )
					{
						continue;
					}

					editorEntity->flags[SPRITE] =
						std::get<1>(previewState);
					editorEntity->sprite =
						std::get<2>(previewState);
					editorEntity->x =
						std::get<3>(previewState);
					editorEntity->y =
						std::get<4>(previewState);
					editorEntity->z =
						std::get<5>(previewState);
					editorEntity->yaw =
						std::get<6>(previewState);
				}
			}

			// primary interface
			drawWindowFancy(0, 0, xres, 16);
			if ( toolbox )
			{
				if ( statusbar )
				{
					drawWindowFancy(xres - 128, 16, xres, yres - 16);
				}
				else
				{
					drawWindowFancy(xres - 128, 16, xres, yres);
				}
				drawEditormap(camx, camy);

				if ( selectedTool == 3 )
				{
					const int panelX = xres - 122;
					const int valueX = xres - 56;
					int panelY = 310;

					printText(
						font8x8_bmp,
						xres - 104,
						292,
						"SELECT AREA"
					);

					auto drawValueField =
						[
							&panelY,
							panelX,
							valueX
						](
							const char* label,
							const int value
						)
						{
							printText(
								font8x8_bmp,
								panelX,
								panelY + 4,
								label
							);
							drawDepressed(
								valueX,
								panelY,
								xres - 8,
								panelY + 16
							);
							printTextFormatted(
								font8x8_bmp,
								valueX + 4,
								panelY + 4,
								"%d",
								value
							);
							panelY += 20;
						};

					drawValueField("TL X", selectedarea_x1);
					drawValueField("TL Y", selectedarea_y1);
					drawValueField("BR X", selectedarea_x2);
					drawValueField("BR Y", selectedarea_y2);

					auto drawLayerField =
						[
							&panelY,
							panelX
						](
							const char* label,
							int& value
						)
						{
							printText(
								font8x8_bmp,
								panelX,
								panelY + 4,
								label
							);

							const int minusX = xres - 68;
							const int valueX = xres - 50;
							const int plusX = xres - 24;

							drawWindowFancy(
								minusX,
								panelY,
								minusX + 16,
								panelY + 16
							);
							printText(
								font8x8_bmp,
								minusX + 5,
								panelY + 4,
								"-"
							);

							drawDepressed(
								valueX,
								panelY,
								valueX + 24,
								panelY + 16
							);
							printTextFormatted(
								font8x8_bmp,
								valueX + 4,
								panelY + 4,
								"%d",
								value
							);

							drawWindowFancy(
								plusX,
								panelY,
								plusX + 16,
								panelY + 16
							);
							printText(
								font8x8_bmp,
								plusX + 5,
								panelY + 4,
								"+"
							);

							if ( mousestatus[SDL_BUTTON_LEFT]
								&& omousey >= panelY
								&& omousey < panelY + 16 )
							{
								if ( omousex >= minusX
									&& omousex < minusX + 16 )
								{
									mousestatus[SDL_BUTTON_LEFT] = 0;
									value = std::max(0, value - 1);
									roomSelectStage = 2;
								}
								else if ( omousex >= plusX
									&& omousex < plusX + 16 )
								{
									mousestatus[SDL_BUTTON_LEFT] = 0;
									value = std::min(
										std::max(
											0,
											static_cast<int>(
												map.numLayers
											) - 1
										),
										value + 1
									);
									roomSelectStage = 2;
								}
							}

							panelY += 20;
						};

					drawLayerField(
						"Bottom",
						roomSelectBottomLayer
					);
					drawLayerField(
						"Top",
						roomSelectTopLayer
					);

					if ( roomSelectBottomLayer
						> roomSelectTopLayer )
					{
						std::swap(
							roomSelectBottomLayer,
							roomSelectTopLayer
						);
					}

					auto roomPanelButton =
						[
							&panelY
						](
							const int x,
							const int width,
							const char* label
						) -> bool
						{
							drawWindowFancy(
								x,
								panelY,
								x + width,
								panelY + 18
							);

							printText(
								font8x8_bmp,
								x + 4,
								panelY + 5,
								label
							);

							if ( mousestatus[SDL_BUTTON_LEFT]
								&& omousex >= x
								&& omousex < x + width
								&& omousey >= panelY
								&& omousey < panelY + 18 )
							{
								mousestatus[SDL_BUTTON_LEFT] = 0;
								return true;
							}

							return false;
						};

					const int buttonX = xres - 120;
					const int buttonWidth = 108;

					const char* copyModeLabel =
						roomCopyContentMode == ROOM_COPY_TILES
							? "COPY: TILES"
							: (
								roomCopyContentMode == ROOM_COPY_SPRITES
									? "COPY: SPRITES"
									: "COPY: BOTH"
							);

					if ( roomPanelButton(
							buttonX,
							buttonWidth,
							copyModeLabel
						) )
					{
						roomCopyContentMode =
							(roomCopyContentMode + 1) % 3;
					}

					panelY += 23;

					if ( roomPanelButton(
							buttonX,
							buttonWidth,
							"COPY"
						) )
					{
						editorRoomCopySelection();
					}

					panelY += 23;

					if ( roomPanelButton(
							buttonX,
							buttonWidth,
							"PASTE"
						) )
					{
						editorRoomBeginPaste();
					}

					panelY += 23;

					if ( roomPanelButton(
							buttonX,
							buttonWidth,
							"DELETE"
						) )
					{
						editorRoomDeleteSelection();
					}

					panelY += 23;

					if ( roomPanelButton(
							buttonX,
							buttonWidth,
							"ROOM GROUPS"
						) )
					{
						openRoomGroupManager();
					}

					panelY += 26;

					printText(
						font8x8_bmp,
						panelX,
						panelY,
						"Stage:"
					);
					printText(
						font8x8_bmp,
						panelX,
						panelY + 12,
						roomSelectStageText()
					);

					if ( roomClipboardReady )
					{
						printTextFormatted(
							font8x8_bmp,
							panelX,
							panelY + 32,
							"%dx%dx%d",
							roomClipboardWidth,
							roomClipboardHeight,
							roomClipboardDepth
						);
						printTextFormatted(
							font8x8_bmp,
							panelX,
							panelY + 44,
							"Sprites: %d",
							roomClipboardEntityCount
						);
					}
				}
				else
				{
				// draw selected tile / hovering tile
				pos.x = xres - 48;
				pos.y = 320;
				pos.w = 32;
				pos.h = 32;
				if ( selectedTile >= 0 && selectedTile < numtiles )
				{
					if ( tiles[selectedTile] != NULL )
					{
						drawImage(tiles[selectedTile], NULL, &pos);
					}
					else
					{
						drawImage(sprites[0], NULL, &pos);
					}
				}
				else
				{
					drawImage(sprites[0], NULL, &pos);
				}
				pos.x = xres - 48;
				pos.y = 360;
				pos.w = 32;
				pos.h = 32;
				if ( drawx >= 0 && drawx < map.width && drawy >= 0 && drawy < map.height )
				{
					c = map.tiles[drawlayer + drawy * MAPLAYERS + drawx * MAPLAYERS * map.height];
					if ( c >= 0 && c < numtiles )
					{
						if ( tiles[c] != NULL )
						{
							drawImage(tiles[c], NULL, &pos);
						}
						else
						{
							drawImage(sprites[0], NULL, &pos);
						}
					}
					else
					{
						drawImage(sprites[0], NULL, &pos);
					}
				}
				else
				{
					drawImage(sprites[0], NULL, &pos);
				}
				printTextFormatted(font8x8_bmp, xres - 124, 324, "Selected:\n\n%9d",
					(selectedTile >= 0 && selectedTile < numtiles) ? selectedTile : 0);
				printTextFormatted(font8x8_bmp, xres - 124, 364, " Hovered:\n\n%9d",
					(drawx >= 0 && drawx < map.width&& drawy >= 0 && drawy < map.height) ? c : 0);

				// Print the name of the selected tool below the Tool Buttons
				switch ( selectedTool )
				{
					case 0: // Pencil
						printText(font8x8_bmp, xres - 84, 292, "PENCIL");
						break;
					case 1: // Point
						printText(font8x8_bmp, xres - 84, 292, "POINT");
						break;
					case 2: // Brush
						printText(font8x8_bmp, xres - 84, 292, "BRUSH");
						break;
					case 3: // Select
						printText(font8x8_bmp, xres - 88, 292, "SELECT");
						break;
					case 4: // Fill
						printText(font8x8_bmp, xres - 80, 292, "FILL");
						break;
				}

				int recentTileStartx = xres - 114;
				int recentTileStarty = 420;
				int recentIndex = 0;
				int pad_x = 34;
				int pad_y = 34;
				char tmpStr[32] = "PALETTE: ";
				char tmpStr2[2] = "";
				pos.x = recentTileStartx;
				pos.y = recentTileStarty;
				pos.w = 32;
				pos.h = 32;
				SDL_Rect boxPos;
				snprintf(tmpStr2, sizeof(tmpStr2), "%d", recentUsedTilePalette + 1); //reset
				strcat(tmpStr, tmpStr2);
				printText(font8x8_bmp, xres - 110, recentTileStarty - 16, tmpStr);

				for ( recentIndex = 0; recentIndex < 9; recentIndex++ )
				{
					if ( recentIndex == 3 || recentIndex == 6 )
					{
						pos.x = recentTileStartx;
						pos.y += pad_y;
					}
					

					if ( mousestatus[SDL_BUTTON_LEFT] )
					{
						if ( omousex >= pos.x && omousex < pos.x + 32 && omousey >= pos.y && omousey < pos.y + 32 )
						{
							selectedTile = recentUsedTiles[recentUsedTilePalette][recentIndex];
							lastPaletteTileSelected = recentIndex;
						}
					}
					if ( mousestatus[SDL_BUTTON_RIGHT] )
					{
						if ( omousex >= pos.x && omousex < pos.x + 32 && omousey >= pos.y && omousey < pos.y + 32 )
						{
							if ( lockTilePalette[recentUsedTilePalette] != 1 )
							{
								recentUsedTiles[recentUsedTilePalette][recentIndex] = 0;
							}
						}
					}

					boxPos.x = pos.x - 2;
					boxPos.y = pos.y - 2;
					boxPos.w = pos.w + 4;
					boxPos.h = pos.h + 4;

					if ( lastPaletteTileSelected == recentIndex )
					{
						drawRect(&boxPos, makeColorRGB(255, 0, 0), 255);
					}
					drawImage(tiles[recentUsedTiles[recentUsedTilePalette][recentIndex]], NULL, &pos);
					pos.x += pad_x;
				}

				if ( lockTilePalette[recentUsedTilePalette] == 1 )
				{
					printText(font8x8_bmp, xres - 100, pos.y + 40, "LOCKED");
				}
				else
				{
					printText(font8x8_bmp, xres - 100, pos.y + 40, "UNLOCKED");
				}
				}
			}
			if ( statusbar )
			{
				drawWindowFancy(0, yres - 16, xres, yres);
				printTextFormatted(font8x8_bmp, 4, yres - 12, "X: %4d Y: %4d Z: %d %s", drawx, drawy, drawlayer + 1, layerstatus);
				if ( messagetime )
				{
					printText(font8x8_bmp, xres - 8 * (strlen(message)) - 12, yres - 12, message);
				}
			}

			// handle main menus
			if ( menuVisible == 1 )
			{
				drawWindowFancy(0, 16, 16, 112);
				butNew->visible = 1;
				butOpen->visible = 1;
				butDir->visible = 1;
				butSave->visible = 1;
				butSaveAs->visible = 1;
				butExit->visible = 1;
			}
			else
			{
				butNew->visible = 0;
				butOpen->visible = 0;
				butDir->visible = 0;
				butSave->visible = 0;
				butSaveAs->visible = 0;
				butExit->visible = 0;
			}
			if ( menuVisible == 2 )
			{
				drawWindowFancy(40, 16, 56, 128);
				butPaste->visible = 1;
				butCut->visible = 1;
				butCopy->visible = 1;
				butDelete->visible = 1;
				butSelectAll->visible = 1;
				butUndo->visible = 1;
				butRedo->visible = 1;
			}
			else
			{
				butPaste->visible = 0;
				butCut->visible = 0;
				butCopy->visible = 0;
				butDelete->visible = 0;
				butSelectAll->visible = 0;
				butUndo->visible = 0;
				butRedo->visible = 0;
			}
			if ( menuVisible == 3 )
			{
				drawWindowFancy(80, 16, 96, 144);
				butToolbox->visible = 1;
				butStatusBar->visible = 1;
				butAllLayers->visible = 1;
				butHoverText->visible = 1;
				butViewSprites->visible = 1;
				butGrid->visible = 1;
				but3DMode->visible = 1;
				but3DModels->visible = 1;
				if ( statusbar )
				{
					printText(font8x8_bmp, 84, 20, "x");
				}
				if ( toolbox )
				{
					printText(font8x8_bmp, 84, 36, "x");
				}
				if ( alllayers )
				{
					printText(font8x8_bmp, 84, 52, "x");
				}
				if ( viewsprites )
				{
					printText(font8x8_bmp, 84, 68, "x");
				}
				if ( showgrid )
				{
					printText(font8x8_bmp, 84, 84, "x");
				}
				if ( mode3d )
				{
					printText(font8x8_bmp, 84, 100, "x");
				}
				if ( editor3DModelsEnabled )
				{
					printText(font8x8_bmp, 84, 116, "x");
				}
				if ( hovertext )
				{
					printText(font8x8_bmp, 84, 132, "x");
				}
			}
			else
			{
				butToolbox->visible = 0;
				butStatusBar->visible = 0;
				butAllLayers->visible = 0;
				butHoverText->visible = 0;
				butViewSprites->visible = 0;
				butGrid->visible = 0;
				but3DMode->visible = 0;
				but3DModels->visible = 0;
			}
			if ( menuVisible == 4 )
			{
				drawWindowFancy(120, 16, 136, 48);
				butAttributes->visible = 1;
				butClearMap->visible = 1;
			}
			else
			{
				butAttributes->visible = 0;
				butClearMap->visible = 0;
			}
			if ( menuVisible == 5 )
			{
				drawWindowFancy(152, 16, 168, 32);
				butDialogueEditor->visible = 1;
			}
			else
			{
				butDialogueEditor->visible = 0;
			}

			if ( menuVisible == 7 )
			{
				drawWindowFancy(216, 16, 232, 32);
				butTextSourceScriptTester->visible = 1;
			}
			else
			{
				butTextSourceScriptTester->visible = 0;
			}

			if ( menuVisible == 6 )
			{
				drawWindowFancy(272, 16, 288, 48);
				butAbout->visible = 1;
				butEditorControls->visible = 1;
			}
			else
			{
				butAbout->visible = 0;
				butEditorControls->visible = 0;
			}

			// subwindows
			if ( subwindow )
			{
				drawWindowFancy(subx1, suby1, subx2, suby2);
				if ( subtext[0] != '\0' )
				{
					printText(font8x8_bmp, subx1 + 8, suby1 + 8, subtext);
				}

				// Open/save map browser with live search filtering.
				if ( (openwindow == 1 || savewindow) )
				{
					const int searchLabelX = subx1 + 8;
					const int searchY = suby1 + 28;
					const int searchBoxX1 = subx1 + 64;
					const int searchBoxX2 = subx2 - 8;
					const int listTop = suby1 + 44;
					const int listBottom = suby2 - 52;
					const int visibleRows = std::max(1, (listBottom - listTop - 8) / 8);

					printText(font8x8_bmp, searchLabelX, searchY, "Search:");
					drawDepressed(searchBoxX1, searchY - 4, searchBoxX2, searchY + 12);
					printText(font8x8_bmp, searchBoxX1 + 4, searchY, editorOpenMapSearch);
					if ( inputstr == editorOpenMapSearch
						&& (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
					{
						printText(font8x8_bmp, searchBoxX1 + 4 + strlen(editorOpenMapSearch) * 8, searchY, "|");
					}

					std::vector<const std::string*> filteredMapNames;
					const std::string loweredSearch = editorPaletteLowercase(editorOpenMapSearch);
					for ( const std::string& mapName : mapNames )
					{
						if ( loweredSearch.empty()
							|| editorPaletteLowercase(mapName).find(loweredSearch) != std::string::npos )
						{
							filteredMapNames.push_back(&mapName);
						}
					}

					drawDepressed(subx1 + 4, listTop, subx2 - 20, listBottom);
					drawDepressed(subx2 - 20, listTop, subx2 - 4, listBottom);

					if ( filteredMapNames.empty() )
					{
						selectedFile = 0;
						y2 = 0;
						printText(font8x8_bmp, subx1 + 8, listTop + 4, "No matching maps.");
					}
					else
					{
						selectedFile = std::max(0, std::min(selectedFile,
							static_cast<int>(filteredMapNames.size()) - 1));
						const int maxFirst = std::max(0,
							static_cast<int>(filteredMapNames.size()) - visibleRows);
						y2 = std::max(0, std::min(y2, maxFirst));

						if ( selectedFile < y2 )
						{
							y2 = selectedFile;
						}
						else if ( selectedFile >= y2 + visibleRows )
						{
							y2 = selectedFile - visibleRows + 1;
						}

						if ( scroll && mousex >= subx1 + 4 && mousex < subx2 - 4
							&& mousey >= listTop && mousey < listBottom )
						{
							y2 = std::max(0, std::min(maxFirst, y2 - scroll));
							selectedFile = std::max(y2,
								std::min(selectedFile, y2 + visibleRows - 1));
							scroll = 0;
						}

						const int listHeight = listBottom - listTop - 2;
						slidersize = filteredMapNames.size() <= static_cast<size_t>(visibleRows)
							? listHeight
							: std::max(8, listHeight * visibleRows
								/ static_cast<int>(filteredMapNames.size()));
						const int sliderTravel = std::max(0, listHeight - slidersize);
						slidery = listTop + 1 + (maxFirst > 0 ? sliderTravel * y2 / maxFirst : 0);
						drawWindowFancy(subx2 - 19, slidery, subx2 - 5, slidery + slidersize);

						if ( mousestatus[SDL_BUTTON_LEFT]
							&& omousex >= subx2 - 20 && omousex < subx2 - 4
							&& omousey >= listTop && omousey < listBottom )
						{
							const int requestedSlider = std::max(listTop + 1,
								std::min(listBottom - 1 - slidersize,
								oslidery + mousey - omousey));
							y2 = sliderTravel > 0
								? (requestedSlider - listTop - 1) * maxFirst / sliderTravel
								: 0;
							selectedFile = std::max(y2,
								std::min(selectedFile, y2 + visibleRows - 1));
							mclick = 1;
						}
						else
						{
							oslidery = slidery;
						}

						if ( mousestatus[SDL_BUTTON_LEFT]
							&& omousex >= subx1 + 8 && omousex < subx2 - 24
							&& omousey >= listTop + 4 && omousey < listBottom - 4 )
						{
							const int clicked = y2 + ((omousey - listTop - 4) >> 3);
							if ( clicked >= 0 && clicked < static_cast<int>(filteredMapNames.size()) )
							{
								selectedFile = clicked;
								snprintf(filename, sizeof(filename), "%s",
									filteredMapNames[selectedFile]->c_str());
								inputstr = filename;
								cursorflash = ticks;
							}
						}

						pos.x = subx1 + 8;
						pos.y = listTop + 4 + (selectedFile - y2) * 8;
						pos.w = subx2 - subx1 - 32;
						pos.h = 8;
						if ( selectedFile >= y2 && selectedFile < y2 + visibleRows )
						{
							drawRect(&pos, makeColorRGB(64, 64, 64), 255);
						}

						x = subx1 + 8;
						y = listTop + 4;
						const int lastVisible = std::min(static_cast<int>(filteredMapNames.size()),
							y2 + visibleRows);
						for ( int index = y2; index < lastVisible; ++index )
						{
							printText(font8x8_bmp, x, y, filteredMapNames[index]->c_str());
							y += 8;
						}
					}

					// Filename field remains separate from the search filter.
					drawDepressed(subx1 + 4, suby2 - 48, subx2 - 68, suby2 - 32);
					printText(font8x8_bmp, subx1 + 8, suby2 - 44, filename);
					if ( inputstr == filename
						&& (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
					{
						printText(font8x8_bmp, subx1 + 8 + strlen(filename) * 8, suby2 - 44, "|");
					}

					if ( mousestatus[SDL_BUTTON_LEFT] )
					{
						if ( omousex >= searchBoxX1 && omousex < searchBoxX2
							&& omousey >= searchY - 4 && omousey < searchY + 12 )
						{
							inputstr = editorOpenMapSearch;
							inputlen = 127;
							cursorflash = ticks;
							selectedFile = 0;
							y2 = 0;
						}
						else if ( omousex >= subx1 + 4 && omousex < subx2 - 68
							&& omousey >= suby2 - 48 && omousey < suby2 - 32 )
						{
							inputstr = filename;
							inputlen = 28;
							cursorflash = ticks;
						}
					}

					if ( !SDL_IsTextInputActive() )
					{
						SDL_StartTextInput();
					}
					if ( inputstr != editorOpenMapSearch && inputstr != filename )
					{
						inputstr = filename;
					}
					inputlen = inputstr == editorOpenMapSearch ? 127 : 28;
				}
				else if ( openwindow == 2 )
				{
					const int searchLabelX = subx1 + 8;
					const int searchY = suby1 + 28;
					const int searchBoxX1 = subx1 + 64;
					const int searchBoxX2 = subx2 - 8;
					const int listTop = suby1 + 44;
					const int listBottom = suby2 - 112;
					const int visibleRows = std::max(1, (listBottom - listTop - 8) / 8);

					printText(font8x8_bmp, searchLabelX, searchY, "Search:");
					drawDepressed(searchBoxX1, searchY - 4, searchBoxX2, searchY + 12);
					printText(font8x8_bmp, searchBoxX1 + 4, searchY, editorDirectorySearch);
					if ( inputstr == editorDirectorySearch
						&& (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
					{
						printText(font8x8_bmp, searchBoxX1 + 4 + strlen(editorDirectorySearch) * 8, searchY, "|");
					}

					std::vector<std::string> filteredFolders;
					const std::string loweredSearch = editorPaletteLowercase(editorDirectorySearch);
					for ( const std::string& folder : modFolderNames )
					{
						if ( loweredSearch.empty()
							|| editorPaletteLowercase(folder).find(loweredSearch) != std::string::npos )
						{
							filteredFolders.push_back(folder);
						}
					}

					drawDepressed(subx1 + 4, listTop, subx2 - 20, listBottom);
					drawDepressed(subx2 - 20, listTop, subx2 - 4, listBottom);
					if ( filteredFolders.empty() )
					{
						selectedFile = 0;
						y2 = 0;
						printText(font8x8_bmp, subx1 + 8, listTop + 4, "No matching directories.");
					}
					else
					{
						selectedFile = std::max(0, std::min(selectedFile,
							static_cast<int>(filteredFolders.size()) - 1));
						const int maxFirst = std::max(0,
							static_cast<int>(filteredFolders.size()) - visibleRows);
						y2 = std::max(0, std::min(y2, maxFirst));
						if ( scroll && mousex >= subx1 + 4 && mousex < subx2 - 4
							&& mousey >= listTop && mousey < listBottom )
						{
							y2 = std::max(0, std::min(maxFirst, y2 - scroll));
							selectedFile = std::max(y2,
								std::min(selectedFile, y2 + visibleRows - 1));
							scroll = 0;
						}

						const int listHeight = listBottom - listTop - 2;
						slidersize = filteredFolders.size() <= static_cast<size_t>(visibleRows)
							? listHeight
							: std::max(8, listHeight * visibleRows
								/ static_cast<int>(filteredFolders.size()));
						const int sliderTravel = std::max(0, listHeight - slidersize);
						slidery = listTop + 1 + (maxFirst > 0 ? sliderTravel * y2 / maxFirst : 0);
						drawWindowFancy(subx2 - 19, slidery, subx2 - 5, slidery + slidersize);

						if ( mousestatus[SDL_BUTTON_LEFT]
							&& omousex >= subx2 - 20 && omousex < subx2 - 4
							&& omousey >= listTop && omousey < listBottom )
						{
							const int requestedSlider = std::max(listTop + 1,
								std::min(listBottom - 1 - slidersize,
								oslidery + mousey - omousey));
							y2 = sliderTravel > 0
								? (requestedSlider - listTop - 1) * maxFirst / sliderTravel
								: 0;
							selectedFile = std::max(y2,
								std::min(selectedFile, y2 + visibleRows - 1));
							mclick = 1;
						}
						else
						{
							oslidery = slidery;
						}

						if ( mousestatus[SDL_BUTTON_LEFT]
							&& omousex >= subx1 + 8 && omousex < subx2 - 24
							&& omousey >= listTop + 4 && omousey < listBottom - 4 )
						{
							const int clicked = y2 + ((omousey - listTop - 4) >> 3);
							if ( clicked >= 0 && clicked < static_cast<int>(filteredFolders.size()) )
							{
								selectedFile = clicked;
								snprintf(foldername, sizeof(foldername), "%s",
									filteredFolders[selectedFile].c_str());
								inputstr = foldername;
								cursorflash = ticks;
							}
						}

						pos.x = subx1 + 8;
						pos.y = listTop + 4 + (selectedFile - y2) * 8;
						pos.w = subx2 - subx1 - 32;
						pos.h = 8;
						if ( selectedFile >= y2 && selectedFile < y2 + visibleRows )
						{
							drawRect(&pos, makeColorRGB(64, 64, 64), 255);
						}
						x = subx1 + 8;
						y = listTop + 4;
						const int lastVisible = std::min(static_cast<int>(filteredFolders.size()),
							y2 + visibleRows);
						for ( int index = y2; index < lastVisible; ++index )
						{
							printText(font8x8_bmp, x, y, filteredFolders[index].c_str());
							y += 8;
						}
					}

					drawDepressed(subx1 + 4, suby2 - 108, subx2 - 4, suby2 - 92);
					printText(font8x8_bmp, subx1 + 8, suby2 - 104, foldername);
					if ( inputstr == foldername
						&& (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
					{
						printText(font8x8_bmp, subx1 + 8 + strlen(foldername) * 8,
							suby2 - 104, "|");
					}

					printTextFormatted(font8x8_bmp, subx1 + 8, suby2 - 32,
						"Save Dir: %smaps/", physfs_saveDirectory.c_str());
					printTextFormatted(font8x8_bmp, subx1 + 8, suby2 - 16,
						"Load Dir: %smaps/", physfs_openDirectory.c_str());

					if ( mousestatus[SDL_BUTTON_LEFT] )
					{
						if ( omousex >= searchBoxX1 && omousex < searchBoxX2
							&& omousey >= searchY - 4 && omousey < searchY + 12 )
						{
							inputstr = editorDirectorySearch;
							inputlen = 127;
							cursorflash = ticks;
							selectedFile = 0;
							y2 = 0;
						}
						else if ( omousex >= subx1 + 4 && omousex < subx2 - 4
							&& omousey >= suby2 - 108 && omousey < suby2 - 92 )
						{
							inputstr = foldername;
							inputlen = 28;
							cursorflash = ticks;
						}
					}

					if ( !SDL_IsTextInputActive() )
					{
						SDL_StartTextInput();
					}
					if ( inputstr != editorDirectorySearch && inputstr != foldername )
					{
						inputstr = foldername;
					}
					inputlen = inputstr == editorDirectorySearch ? 127 : 28;
				}

				// new map and attributes windows
				if ( newwindow == 1 )
				{
					int pad_y1 = 0;
					int start_y = suby1 + 28;
					int rowheight = 16;

					printText(font8x8_bmp, subx1 + 8, start_y + pad_y1, "Map Name:");
					drawDepressed(subx1 + 4, suby1 + 40, subx2 - 4, suby1 + 56);
					printText(font8x8_bmp, subx1 + 8, start_y + 16, nametext);
					pad_y1 += 24;
					printText(font8x8_bmp, subx1 + 8, start_y + pad_y1 + 12, "Author Name:");
					drawDepressed(subx1 + 4, suby1 + 76, subx2 - 4, suby1 + 92);
					printText(font8x8_bmp, subx1 + 8, start_y + 16 + 36, authortext);

					start_y = suby1 + 104;
					pad_y1 = 0;
					int start_x2 = subx1 + 180;
					int start_x3 = subx2 - 32;
					printText(font8x8_bmp, subx1 + 8, start_y + pad_y1, "Map Skybox:");
					drawDepressed(subx1 + 104, start_y + pad_y1 - 4, subx1 + 168, start_y + pad_y1 + rowheight - 4);
					printText(font8x8_bmp, subx1 + 108, start_y + pad_y1, skyboxtext);

					printText(font8x8_bmp, start_x2, start_y + pad_y1, "Disable Traps:");
					printText(font8x8_bmp, start_x3, start_y + pad_y1, mapflagtext[MAP_FLAG_DISABLETRAPS]);
					pad_y1 += 24;
					printText(font8x8_bmp, subx1 + 8, start_y + pad_y1, "Map Ceiling:");
					drawDepressed(subx1 + 104, start_y + pad_y1 - 4, subx1 + 168, start_y + pad_y1 + rowheight - 4);
					printText(font8x8_bmp, subx1 + 108, start_y + pad_y1, mapflagtext[MAP_FLAG_CEILINGTILE]);

					printText(font8x8_bmp, start_x2, start_y + pad_y1, "Disable Monster Spawns:");
					printText(font8x8_bmp, start_x3, start_y + pad_y1, mapflagtext[MAP_FLAG_DISABLEMONSTERS]);
					pad_y1 += 24;
					printText(font8x8_bmp, subx1 + 8, start_y + pad_y1, "Entity Qty:");
					drawDepressed(subx1 + 104, start_y + pad_y1 - 4, subx1 + 128, start_y + pad_y1 + rowheight - 4);
					printText(font8x8_bmp, subx1 + 108, start_y + pad_y1, mapflagtext[MAP_FLAG_GENTOTALMIN]);
					printText(font8x8_bmp, subx1 + 132, start_y + pad_y1, "-");
					drawDepressed(subx1 + 144, start_y + pad_y1 - 4, subx1 + 168, start_y + pad_y1 + rowheight - 4);
					printText(font8x8_bmp, subx1 + 144 + 4, start_y + pad_y1, mapflagtext[MAP_FLAG_GENTOTALMAX]);

					printText(font8x8_bmp, start_x2, start_y + pad_y1, "Disable Loot Spawns:");
					printText(font8x8_bmp, start_x3, start_y + pad_y1, mapflagtext[MAP_FLAG_DISABLELOOT]);
			
					pad_y1 += 24;
					printText(font8x8_bmp, subx1 + 8, start_y + pad_y1, "Monster Qty:");
					drawDepressed(subx1 + 104, start_y + pad_y1 - 4, subx1 + 128, start_y + pad_y1 + rowheight - 4);
					printText(font8x8_bmp, subx1 + 108, start_y + pad_y1, mapflagtext[MAP_FLAG_GENMONSTERMIN]);
					printText(font8x8_bmp, subx1 + 132, start_y + pad_y1, "-");
					drawDepressed(subx1 + 144, start_y + pad_y1 - 4, subx1 + 168, start_y + pad_y1 + rowheight - 4);
					printText(font8x8_bmp, subx1 + 144 + 4, start_y + pad_y1, mapflagtext[MAP_FLAG_GENMONSTERMAX]);

					printText(font8x8_bmp, start_x2, start_y + pad_y1, "Disable Digging:");
					printText(font8x8_bmp, start_x3, start_y + pad_y1, mapflagtext[MAP_FLAG_DISABLEDIGGING]);

					pad_y1 += 24;
					printText(font8x8_bmp, subx1 + 8, start_y + pad_y1, "Item Qty:");
					drawDepressed(subx1 + 104, start_y + pad_y1 - 4, subx1 + 128, start_y + pad_y1 + rowheight - 4);
					printText(font8x8_bmp, subx1 + 108, start_y + pad_y1, mapflagtext[MAP_FLAG_GENLOOTMIN]);
					printText(font8x8_bmp, subx1 + 132, start_y + pad_y1, "-");
					drawDepressed(subx1 + 144, start_y + pad_y1 - 4, subx1 + 168, start_y + pad_y1 + rowheight - 4);
					printText(font8x8_bmp, subx1 + 144 + 4, start_y + pad_y1, mapflagtext[MAP_FLAG_GENLOOTMAX]);

					printText(font8x8_bmp, start_x2, start_y + pad_y1, "Disable Teleportation:");
					printText(font8x8_bmp, start_x3, start_y + pad_y1, mapflagtext[MAP_FLAG_DISABLETELEPORT]);

					pad_y1 += 24;
					printText(font8x8_bmp, subx1 + 8, start_y + pad_y1, "Deco Qty:");
					drawDepressed(subx1 + 104, start_y + pad_y1 - 4, subx1 + 128, start_y + pad_y1 + rowheight - 4);
					printText(font8x8_bmp, subx1 + 108, start_y + pad_y1, mapflagtext[MAP_FLAG_GENDECORATIONMIN]);
					printText(font8x8_bmp, subx1 + 132, start_y + pad_y1, "-");
					drawDepressed(subx1 + 144, start_y + pad_y1 - 4, subx1 + 168, start_y + pad_y1 + rowheight - 4);
					printText(font8x8_bmp, subx1 + 144 + 4, start_y + pad_y1, mapflagtext[MAP_FLAG_GENDECORATIONMAX]);

					printText(font8x8_bmp, start_x2, start_y + pad_y1, "Disable Levitation:");
					printText(font8x8_bmp, start_x3, start_y + pad_y1, mapflagtext[MAP_FLAG_DISABLELEVITATION]);

					pad_y1 += 24;
					printText(font8x8_bmp, subx1 + 8, start_y + pad_y1, "Gen Border:");
					drawDepressed(subx1 + 104, start_y + pad_y1 - 4, subx1 + 168, start_y + pad_y1 + rowheight - 4);
					printText(font8x8_bmp, subx1 + 108, start_y + pad_y1, mapflagtext[MAP_FLAG_PERIMETER_GAP]);

					printText(font8x8_bmp, start_x2, start_y + pad_y1, "Gen Adjacent Rooms:");
					printText(font8x8_bmp, start_x3, start_y + pad_y1, mapflagtext[MAP_FLAG_GENADJACENTROOMS]);

					pad_y1 += 24;
					printText(font8x8_bmp, start_x2, start_y + pad_y1, "Disable Opening Spell:");
					printText(font8x8_bmp, start_x3, start_y + pad_y1, mapflagtext[MAP_FLAG_DISABLEOPENING]);

					pad_y1 += 24;
					printText(font8x8_bmp, start_x2, start_y + pad_y1, "Disable Herx Messages:");
					printText(font8x8_bmp, start_x3, start_y + pad_y1, mapflagtext[MAP_FLAG_DISABLEMESSAGES]);

					pad_y1 += 24;
					printText(font8x8_bmp, start_x2, start_y + pad_y1, "Disable Hunger Loss:");
					printText(font8x8_bmp, start_x3, start_y + pad_y1, mapflagtext[MAP_FLAG_DISABLEHUNGER]);

					const int fogPanelX = subx1 + 8;
					const int fogPanelY = suby1 + 356;
					const int fogFieldX = fogPanelX + 104;

					printTextFormattedColor(
						font8x8_bmp,
						fogPanelX,
						fogPanelY,
						makeColorRGB(120, 220, 255),
						"Custom Map Fog"
					);
					printText(font8x8_bmp, fogPanelX, fogPanelY + 18, "Enabled:");
					printText(font8x8_bmp, fogFieldX, fogPanelY + 18, mapFogEnabledText);

					const char* fogLabels[5] =
					{
						"Distance:",
						"Density:",
						"Red:",
						"Green:",
						"Blue:"
					};
					char* fogValues[5] =
					{
						mapFogDistanceText,
						mapFogDensityText,
						mapFogRedText,
						mapFogGreenText,
						mapFogBlueText
					};

					for ( int fogIndex = 0; fogIndex < 5; ++fogIndex )
					{
						const int fogY = fogPanelY + 36 + fogIndex * 18;
						printText(font8x8_bmp, fogPanelX, fogY, fogLabels[fogIndex]);
						drawDepressed(fogFieldX - 4, fogY - 4, fogFieldX + 60, fogY + 12);
						printText(font8x8_bmp, fogFieldX, fogY, fogValues[fogIndex]);
					}

					const int ambientLightPanelX = fogPanelX + 220;
					const int ambientLightPanelY = fogPanelY;
					const int ambientLightFieldX = ambientLightPanelX + 112;
					printTextFormattedColor(
						font8x8_bmp,
						ambientLightPanelX,
						ambientLightPanelY,
						makeColorRGB(255, 210, 120),
						"Map Ambient Light"
					);
					printText(font8x8_bmp, ambientLightPanelX, ambientLightPanelY + 18, "Enabled:");
					printText(font8x8_bmp, ambientLightFieldX, ambientLightPanelY + 18,
						mapAmbientLightEnabledText);
					const char* ambientLightLabels[3] = {"Red:", "Green:", "Blue:"};
					char* ambientLightValues[3] = {
						mapAmbientLightRedText, mapAmbientLightGreenText, mapAmbientLightBlueText};
					for ( int lightIndex = 0; lightIndex < 3; ++lightIndex )
					{
						const int lightY = ambientLightPanelY + 36 + lightIndex * 18;
						printText(font8x8_bmp, ambientLightPanelX, lightY, ambientLightLabels[lightIndex]);
						drawDepressed(ambientLightFieldX - 4, lightY - 4,
							ambientLightFieldX + 60, lightY + 12);
						printText(font8x8_bmp, ambientLightFieldX, lightY, ambientLightValues[lightIndex]);
					}
					printTextFormattedColor(font8x8_bmp, ambientLightPanelX, ambientLightPanelY + 96,
						makeColorRGB(
							std::clamp(atoi(mapAmbientLightRedText), 0, 255),
							std::clamp(atoi(mapAmbientLightGreenText), 0, 255),
							std::clamp(atoi(mapAmbientLightBlueText), 0, 255)),
						"Light color preview");

					const int ambiencePanelX = fogPanelX + 440;
					const int ambiencePanelY = fogPanelY;
					const int ambienceFieldX = ambiencePanelX + 132;
					printTextFormattedColor(
						font8x8_bmp,
						ambiencePanelX,
						ambiencePanelY,
						makeColorRGB(120, 220, 255),
						"Map Ambient Audio"
					);
					printText(font8x8_bmp, ambiencePanelX, ambiencePanelY + 18, "Enabled:");
					printText(font8x8_bmp, ambienceFieldX, ambiencePanelY + 18,
						mapAmbienceEnabledText);
					const char* ambienceLabels[5] = {
						"Sound:", "Volume:", "Loop:", "Fade In (ms):", "Fade Out (ms):"};
					char* ambienceValues[5] = {
						mapAmbienceResourceText, mapAmbienceVolumeText, mapAmbienceLoopText,
						mapAmbienceFadeInText, mapAmbienceFadeOutText};
					for ( int ambienceIndex = 0; ambienceIndex < 5; ++ambienceIndex )
					{
						const int ambienceY = ambiencePanelY + 36 + ambienceIndex * 18;
						printText(font8x8_bmp, ambiencePanelX, ambienceY, ambienceLabels[ambienceIndex]);
						const int ambienceWidth = ambienceIndex == 0 ? 220 : 60;
						drawDepressed(ambienceFieldX - 4, ambienceY - 4,
							ambienceFieldX + ambienceWidth, ambienceY + 12);
						if ( ambienceIndex == 0 && mapAmbienceResourceText[0] == '\0' )
						{
							printTextFormattedColor(font8x8_bmp, ambienceFieldX, ambienceY,
								makeColorRGB(180, 180, 180), "Select sound...");
						}
						else
						{
							printText(font8x8_bmp, ambienceFieldX, ambienceY, ambienceValues[ambienceIndex]);
						}
					}
					if ( editorAmbiencePickerOpen )
					{
						const int pickerX = ambiencePanelX;
						const int pickerY = ambiencePanelY + 52;
						const int pickerWidth = 340;
						const int pickerBottom = std::max(pickerY + 24, suby2 - 60);
						SDL_Rect pickerRect = {pickerX, pickerY, pickerWidth, pickerBottom - pickerY};
						drawRect(&pickerRect, makeColorRGB(32, 32, 44), 255);
						drawDepressed(pickerX + 4, pickerY + 4, pickerX + pickerWidth - 4, pickerY + 20);
						printText(font8x8_bmp, pickerX + 8, pickerY + 8, editorAmbienceResourceSearch);
						if ( inputstr == editorAmbienceResourceSearch
							&& (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
						{
							printText(font8x8_bmp, pickerX + 8
								+ strlen(editorAmbienceResourceSearch) * 8, pickerY + 8, "|");
						}
						const std::string filter = editorPaletteLowercase(editorAmbienceResourceSearch);
						std::vector<const std::string*> matches;
						for ( const std::string& resource : editorAmbienceResources )
						{
							if ( filter.empty() || editorPaletteLowercase(resource).find(filter) != std::string::npos )
							{
								matches.push_back(&resource);
							}
						}
						const int visibleRows = std::max(0, (pickerBottom - (pickerY + 28)) / 12);
						editorAmbienceResourceFirstVisible = std::max(0,
							std::min(editorAmbienceResourceFirstVisible,
								std::max(0, static_cast<int>(matches.size()) - visibleRows)));
						if ( matches.empty() )
						{
							printTextFormattedColor(font8x8_bmp, pickerX + 8, pickerY + 32,
								makeColorRGB(255, 210, 120), "No VFS audio resources match this search.");
						}
						for ( int row = 0; row < visibleRows
							&& editorAmbienceResourceFirstVisible + row < static_cast<int>(matches.size()); ++row )
						{
							printText(font8x8_bmp, pickerX + 8, pickerY + 28 + row * 12,
								matches[editorAmbienceResourceFirstVisible + row]->c_str());
						}
					}
					const char* mapPropertiesHelp = nullptr;
					if ( mousex >= fogFieldX - 4 && mousex < fogFieldX + 60
						&& mousey >= fogPanelY + 32 && mousey < fogPanelY + 126 )
					{
						const int fogRow = (mousey - (fogPanelY + 32)) / 18;
						if ( fogRow == 0 ) { mapPropertiesHelp = "Fog distance is 16-4080 world units; lower values hide more distant geometry."; }
						else if ( fogRow == 1 ) { mapPropertiesHelp = "Fog density controls opacity: 0 is transparent and 255 is fully opaque at the fog limit."; }
						else if ( fogRow >= 2 && fogRow <= 4 ) { mapPropertiesHelp = "Fog RGB uses 0-255 per channel and is independent from map ambient light."; }
					}
					if ( mousex >= ambientLightFieldX - 4 && mousex < ambientLightFieldX + 60 )
					{
						if ( mousey >= ambientLightPanelY + 14 && mousey < ambientLightPanelY + 32 )
						{
							mapPropertiesHelp = "Enable a map-wide RGB light base, like Hell's authored ambient lighting.";
						}
						else if ( mousey >= ambientLightPanelY + 32 && mousey < ambientLightPanelY + 90 )
						{
							mapPropertiesHelp = "Ambient-light RGB is 0-255 per channel. Hell's neutral base is approximately 32, 32, 32.";
						}
					}
					if ( mousex >= ambienceFieldX - 4 && mousex < ambienceFieldX + 220 )
					{
						const int audioRow = (mousey - (ambiencePanelY + 32)) / 18;
						if ( mousey >= ambiencePanelY + 14 && mousey < ambiencePanelY + 32 )
						{
							mapPropertiesHelp = "Enable environmental audio for this map. It is separate from music and visual ambient light.";
						}
						else if ( audioRow == 0 ) { mapPropertiesHelp = "Choose a sound from the mounted VFS. The search menu only lists supported audio resources."; }
						else if ( audioRow == 1 ) { mapPropertiesHelp = "Volume is 0-100 and affects only this map's environmental audio loop."; }
						else if ( audioRow == 2 ) { mapPropertiesHelp = "Loop repeats the selected environmental sound until this MapInstance is left."; }
						else if ( audioRow == 3 ) { mapPropertiesHelp = "Fade In is milliseconds from silence after this MapInstance becomes active."; }
						else if ( audioRow == 4 ) { mapPropertiesHelp = "Fade Out is milliseconds used when changing to another MapInstance."; }
					}
					if ( mapPropertiesHelp )
					{
						printTextFormattedColor(font8x8_bmp, subx1 + 8, suby2 - 20,
							makeColorRGB(255, 230, 96), "%s", mapPropertiesHelp);
					}

					start_y = suby2 - 72;
					pad_y1 = 0;
					printText(font8x8_bmp, subx1 + 8, start_y + pad_y1, "Map Width:");
					drawDepressed(subx1 + 104, start_y + pad_y1 - 4, subx1 + 168, start_y + pad_y1 + rowheight - 4);
					printText(font8x8_bmp, subx1 + 108, start_y + pad_y1, widthtext);
					pad_y1 += 24;
					printText(font8x8_bmp, subx1 + 8, start_y + pad_y1, "Map Height:");
					drawDepressed(subx1 + 104, start_y + pad_y1 - 4, subx1 + 168, start_y + pad_y1 + rowheight - 4);
					printText(font8x8_bmp, subx1 + 108, start_y + pad_y1, heighttext);

					if ( keystatus[SDLK_TAB] )
					{
						keystatus[SDLK_TAB] = 0;
						cursorflash = ticks;
						editproperty++;
						if ( editproperty == 15 )
						{
							editproperty = 0;
						}
						switch ( editproperty )
						{
							case 0:
								inputstr = nametext;
								break;
							case 1:
								inputstr = authortext;
								break;
							case 2:
								inputstr = skyboxtext;
								break;
							case 3:
								inputstr = mapflagtext[MAP_FLAG_CEILINGTILE];
								break;
							case 4:
								inputstr = mapflagtext[MAP_FLAG_GENTOTALMIN];
								break;
							case 5:
								inputstr = mapflagtext[MAP_FLAG_GENTOTALMAX];
								break;
							case 6:
								inputstr = mapflagtext[MAP_FLAG_GENMONSTERMIN];
								break;
							case 7:
								inputstr = mapflagtext[MAP_FLAG_GENMONSTERMAX];
								break;
							case 8:
								inputstr = mapflagtext[MAP_FLAG_GENLOOTMIN];
								break;
							case 9:
								inputstr = mapflagtext[MAP_FLAG_GENLOOTMAX];
								break;
							case 10:
								inputstr = mapflagtext[MAP_FLAG_GENDECORATIONMIN];
								break;
							case 11:
								inputstr = mapflagtext[MAP_FLAG_GENDECORATIONMAX];
								break;
							case 12:
								inputstr = mapflagtext[MAP_FLAG_PERIMETER_GAP];
								break;
							case 13:
								inputstr = widthtext;
								break;
							case 14:
								inputstr = heighttext;
								break;
							default:
								break;
						}
					}

					// select a textbox
					if ( mousestatus[SDL_BUTTON_LEFT] )
					{
						if ( omousex >= start_x3 && omousey >= suby1 + 100 && omousex < start_x3 + 24 && omousey < suby1 + 116 )
						{
							if ( !strncmp(mapflagtext[MAP_FLAG_DISABLETRAPS], "[x]", 3) )
							{
								strcpy(mapflagtext[MAP_FLAG_DISABLETRAPS], "[ ]");
							}
							else
							{
								strcpy(mapflagtext[MAP_FLAG_DISABLETRAPS], "[x]");
							}
							mousestatus[SDL_BUTTON_LEFT] = 0;
						}
						if ( omousex >= start_x3 && omousey >= suby1 + 124 && omousex < start_x3 + 24 && omousey < suby1 + 140 )
						{
							if ( !strncmp(mapflagtext[MAP_FLAG_DISABLEMONSTERS], "[x]", 3) )
							{
								strcpy(mapflagtext[MAP_FLAG_DISABLEMONSTERS], "[ ]");
							}
							else
							{
								strcpy(mapflagtext[MAP_FLAG_DISABLEMONSTERS], "[x]");
							}
							mousestatus[SDL_BUTTON_LEFT] = 0;
						}
						if ( omousex >= start_x3 && omousey >= suby1 + 148 && omousex < start_x3 + 24 && omousey < suby1 + 164 )
						{
							if ( !strncmp(mapflagtext[MAP_FLAG_DISABLELOOT], "[x]", 3) )
							{
								strcpy(mapflagtext[MAP_FLAG_DISABLELOOT], "[ ]");
							}
							else
							{
								strcpy(mapflagtext[MAP_FLAG_DISABLELOOT], "[x]");
							}
							mousestatus[SDL_BUTTON_LEFT] = 0;
						}
						if ( omousex >= start_x3 && omousey >= suby1 + 172 && omousex < start_x3 + 24 && omousey < suby1 + 188 )
						{
							if ( !strncmp(mapflagtext[MAP_FLAG_DISABLEDIGGING], "[x]", 3) )
							{
								strcpy(mapflagtext[MAP_FLAG_DISABLEDIGGING], "[ ]");
							}
							else
							{
								strcpy(mapflagtext[MAP_FLAG_DISABLEDIGGING], "[x]");
							}
							mousestatus[SDL_BUTTON_LEFT] = 0;
						}
						if ( omousex >= start_x3 && omousey >= suby1 + 196 && omousex < start_x3 + 24 && omousey < suby1 + 212 )
						{
							if ( !strncmp(mapflagtext[MAP_FLAG_DISABLETELEPORT], "[x]", 3) )
							{
								strcpy(mapflagtext[MAP_FLAG_DISABLETELEPORT], "[ ]");
							}
							else
							{
								strcpy(mapflagtext[MAP_FLAG_DISABLETELEPORT], "[x]");
							}
							mousestatus[SDL_BUTTON_LEFT] = 0;
						}
						if ( omousex >= start_x3 && omousey >= suby1 + 220 && omousex < start_x3 + 24 && omousey < suby1 + 236 )
						{
							if ( !strncmp(mapflagtext[MAP_FLAG_DISABLELEVITATION], "[x]", 3) )
							{
								strcpy(mapflagtext[MAP_FLAG_DISABLELEVITATION], "[ ]");
							}
							else
							{
								strcpy(mapflagtext[MAP_FLAG_DISABLELEVITATION], "[x]");
							}
							mousestatus[SDL_BUTTON_LEFT] = 0;
						}
						if ( omousex >= start_x3 && omousey >= suby1 + 244 && omousex < start_x3 + 24 && omousey < suby1 + 260 )
						{
							if ( !strncmp(mapflagtext[MAP_FLAG_GENADJACENTROOMS], "[x]", 3) )
							{
								strcpy(mapflagtext[MAP_FLAG_GENADJACENTROOMS], "[ ]");
							}
							else
							{
								strcpy(mapflagtext[MAP_FLAG_GENADJACENTROOMS], "[x]");
							}
							mousestatus[SDL_BUTTON_LEFT] = 0;
						}
						if ( omousex >= start_x3 && omousey >= suby1 + 268 && omousex < start_x3 + 24 && omousey < suby1 + 284 )
						{
							if ( !strncmp(mapflagtext[MAP_FLAG_DISABLEOPENING], "[x]", 3) )
							{
								strcpy(mapflagtext[MAP_FLAG_DISABLEOPENING], "[ ]");
							}
							else
							{
								strcpy(mapflagtext[MAP_FLAG_DISABLEOPENING], "[x]");
							}
							mousestatus[SDL_BUTTON_LEFT] = 0;
						}
						if ( omousex >= start_x3 && omousey >= suby1 + 292 && omousex < start_x3 + 24 && omousey < suby1 + 308 )
						{
							if ( !strncmp(mapflagtext[MAP_FLAG_DISABLEMESSAGES], "[x]", 3) )
							{
								strcpy(mapflagtext[MAP_FLAG_DISABLEMESSAGES], "[ ]");
							}
							else
							{
								strcpy(mapflagtext[MAP_FLAG_DISABLEMESSAGES], "[x]");
							}
							mousestatus[SDL_BUTTON_LEFT] = 0;
						}
						if ( omousex >= start_x3 && omousey >= suby1 + 316 && omousex < start_x3 + 24 && omousey < suby1 + 332 )
						{
							if ( !strncmp(mapflagtext[MAP_FLAG_DISABLEHUNGER], "[x]", 3) )
							{
								strcpy(mapflagtext[MAP_FLAG_DISABLEHUNGER], "[ ]");
							}
							else
							{
								strcpy(mapflagtext[MAP_FLAG_DISABLEHUNGER], "[x]");
							}
							mousestatus[SDL_BUTTON_LEFT] = 0;
						}

						start_y = suby1 + 40;
						pad_y1 = 0;
						if ( omousex >= subx1 + 4 && omousey >= start_y + pad_y1 && omousex < subx2 - 4 && omousey < start_y + pad_y1 + 16 )
						{
							inputstr = nametext;
							editproperty = 0;
							cursorflash = ticks;
						}
						pad_y1 += 36;
						if ( omousex >= subx1 + 4 && omousey >= start_y + pad_y1 && omousex < subx2 - 4 && omousey < start_y + pad_y1 + 16 )
						{
							inputstr = authortext;
							editproperty = 1;
							cursorflash = ticks;
						}
						pad_y1 += 24;
						if ( omousex >= subx1 + 104 && omousey >= start_y + pad_y1 && omousex < subx1 + 104 + 64 && omousey < start_y + pad_y1 + 16 )
						{
							inputstr = skyboxtext;
							editproperty = 2;
							cursorflash = ticks;
						}
						pad_y1 += 24;
						if ( omousex >= subx1 + 104 && omousey >= start_y + pad_y1 && omousex < subx1 + 104 + 64 && omousey < start_y + pad_y1 + 16 )
						{
							inputstr = mapflagtext[MAP_FLAG_CEILINGTILE];
							editproperty = 3;
							cursorflash = ticks;
						}
						pad_y1 += 24;
						if ( omousex >= subx1 + 104 && omousey >= start_y + pad_y1 && omousex < subx1 + 104 + 24 && omousey < start_y + pad_y1 + 16 )
						{
							inputstr = mapflagtext[MAP_FLAG_GENTOTALMIN];
							editproperty = 4;
							cursorflash = ticks;
						}
						if ( omousex >= subx1 + 144 && omousey >= start_y + pad_y1 && omousex < subx1 + 144 + 24 && omousey < start_y + pad_y1 + 16 )
						{
							inputstr = mapflagtext[MAP_FLAG_GENTOTALMAX];
							editproperty = 5;
							cursorflash = ticks;
						}
						pad_y1 += 24;
						if ( omousex >= subx1 + 104 && omousey >= start_y + pad_y1 && omousex < subx1 + 104 + 24 && omousey < start_y + pad_y1 + 16 )
						{
							inputstr = mapflagtext[MAP_FLAG_GENMONSTERMIN];
							editproperty = 6;
							cursorflash = ticks;
						}
						if ( omousex >= subx1 + 144 && omousey >= start_y + pad_y1 && omousex < subx1 + 144 + 24 && omousey < start_y + pad_y1 + 16 )
						{
							inputstr = mapflagtext[MAP_FLAG_GENMONSTERMAX];
							editproperty = 7;
							cursorflash = ticks;
						}
						pad_y1 += 24;
						if ( omousex >= subx1 + 104 && omousey >= start_y + pad_y1 && omousex < subx1 + 104 + 24 && omousey < start_y + pad_y1 + 16 )
						{
							inputstr = mapflagtext[MAP_FLAG_GENLOOTMIN];
							editproperty = 8;
							cursorflash = ticks;
						}
						if ( omousex >= subx1 + 144 && omousey >= start_y + pad_y1 && omousex < subx1 + 144 + 24 && omousey < start_y + pad_y1 + 16 )
						{
							inputstr = mapflagtext[MAP_FLAG_GENLOOTMAX];
							editproperty = 9;
							cursorflash = ticks;
						}
						pad_y1 += 24;
						if ( omousex >= subx1 + 104 && omousey >= start_y + pad_y1 && omousex < subx1 + 104 + 24 && omousey < start_y + pad_y1 + 16 )
						{
							inputstr = mapflagtext[MAP_FLAG_GENDECORATIONMIN];
							editproperty = 10;
							cursorflash = ticks;
						}
						if ( omousex >= subx1 + 144 && omousey >= start_y + pad_y1 && omousex < subx1 + 144 + 24 && omousey < start_y + pad_y1 + 16 )
						{
							inputstr = mapflagtext[MAP_FLAG_GENDECORATIONMAX];
							editproperty = 11;
							cursorflash = ticks;
						}
						pad_y1 += 24;
						if ( omousex >= subx1 + 104 && omousey >= start_y + pad_y1 && omousex < subx1 + 104 + 64 && omousey < start_y + pad_y1 + 16 )
						{
							inputstr = mapflagtext[MAP_FLAG_PERIMETER_GAP];
							editproperty = 12;
							cursorflash = ticks;
						}

						const int fogClickX = subx1 + 112;
						const int fogClickY = suby1 + 356;
						if ( omousex >= fogClickX
							&& omousex < fogClickX + 32
							&& omousey >= fogClickY + 14
							&& omousey < fogClickY + 32 )
						{
							if ( !strncmp(mapFogEnabledText, "[x]", 3) )
							{
								strcpy(mapFogEnabledText, "[ ]");
							}
							else
							{
								strcpy(mapFogEnabledText, "[x]");
							}
							mousestatus[SDL_BUTTON_LEFT] = 0;
						}

						char* fogClickValues[5] =
						{
							mapFogDistanceText,
							mapFogDensityText,
							mapFogRedText,
							mapFogGreenText,
							mapFogBlueText
						};
						for ( int fogIndex = 0; fogIndex < 5; ++fogIndex )
						{
							const int fogY = fogClickY + 36 + fogIndex * 18;
							if ( omousex >= fogClickX - 4
								&& omousex < fogClickX + 60
								&& omousey >= fogY - 4
								&& omousey < fogY + 12 )
							{
								inputstr = fogClickValues[fogIndex];
								editproperty = 15 + fogIndex;
								cursorflash = ticks;
							}
						}

						const int ambientLightClickX = subx1 + 340;
						const int ambientLightClickY = suby1 + 356;
						if ( omousex >= ambientLightClickX
							&& omousex < ambientLightClickX + 32
							&& omousey >= ambientLightClickY + 14
							&& omousey < ambientLightClickY + 32 )
						{
							strcpy(mapAmbientLightEnabledText,
								!strncmp(mapAmbientLightEnabledText, "[x]", 3) ? "[ ]" : "[x]");
							mousestatus[SDL_BUTTON_LEFT] = 0;
						}
						char* ambientLightClickValues[3] =
						{
							mapAmbientLightRedText,
							mapAmbientLightGreenText,
							mapAmbientLightBlueText
						};
						for ( int lightIndex = 0; lightIndex < 3; ++lightIndex )
						{
							const int lightY = ambientLightClickY + 36 + lightIndex * 18;
							if ( omousex >= ambientLightClickX - 4
								&& omousex < ambientLightClickX + 60
								&& omousey >= lightY - 4 && omousey < lightY + 12 )
							{
								inputstr = ambientLightClickValues[lightIndex];
								editproperty = 20 + lightIndex;
								cursorflash = ticks;
							}
						}

						const int ambienceClickX = subx1 + 580;
						const int ambienceClickY = suby1 + 356;
						if ( omousex >= ambienceClickX
							&& omousex < ambienceClickX + 32
							&& omousey >= ambienceClickY + 14
							&& omousey < ambienceClickY + 32 )
						{
							strcpy(mapAmbienceEnabledText,
								!strncmp(mapAmbienceEnabledText, "[x]", 3) ? "[ ]" : "[x]");
							mousestatus[SDL_BUTTON_LEFT] = 0;
						}
						const int ambienceResourceY = ambienceClickY + 36;
						if ( omousex >= ambienceClickX - 4 && omousex < ambienceClickX + 220
							&& omousey >= ambienceResourceY - 4 && omousey < ambienceResourceY + 12 )
						{
							editorLoadAmbienceResources();
							editorAmbiencePickerOpen = !editorAmbiencePickerOpen;
							editorAmbienceResourceFirstVisible = 0;
							inputstr = editorAmbienceResourceSearch;
							inputlen = 127;
							editproperty = -1;
							cursorflash = ticks;
							mousestatus[SDL_BUTTON_LEFT] = 0;
						}
						const int ambienceVolumeY = ambienceClickY + 54;
						if ( !editorAmbiencePickerOpen
							&& omousex >= ambienceClickX - 4 && omousex < ambienceClickX + 60
							&& omousey >= ambienceVolumeY - 4 && omousey < ambienceVolumeY + 12 )
						{
							inputstr = mapAmbienceVolumeText;
							editproperty = 23;
							cursorflash = ticks;
						}
						if ( !editorAmbiencePickerOpen
							&& omousex >= ambienceClickX && omousex < ambienceClickX + 32
							&& omousey >= ambienceClickY + 68 && omousey < ambienceClickY + 86 )
						{
							strcpy(mapAmbienceLoopText,
								!strncmp(mapAmbienceLoopText, "[x]", 3) ? "[ ]" : "[x]");
							mousestatus[SDL_BUTTON_LEFT] = 0;
						}
						for ( int fadeIndex = 0; fadeIndex < 2; ++fadeIndex )
						{
							const int fadeY = ambienceClickY + 90 + fadeIndex * 18;
							if ( !editorAmbiencePickerOpen
								&& omousex >= ambienceClickX - 4 && omousex < ambienceClickX + 60
								&& omousey >= fadeY - 4 && omousey < fadeY + 12 )
							{
								inputstr = fadeIndex == 0 ? mapAmbienceFadeInText : mapAmbienceFadeOutText;
								editproperty = 24 + fadeIndex;
								cursorflash = ticks;
							}
						}
						if ( editorAmbiencePickerOpen )
						{
							const int pickerX = subx1 + 448;
							const int pickerY = suby1 + 408;
							const int pickerWidth = 340;
							const int pickerBottom = std::max(pickerY + 24, suby2 - 60);
							const int listTop = pickerY + 28;
							const int visibleRows = std::max(0, (pickerBottom - listTop) / 12);
							const std::string filter = editorPaletteLowercase(editorAmbienceResourceSearch);
							std::vector<const std::string*> matches;
							for ( const std::string& resource : editorAmbienceResources )
							{
								if ( filter.empty() || editorPaletteLowercase(resource).find(filter) != std::string::npos )
								{
									matches.push_back(&resource);
								}
							}
							if ( omousex >= pickerX + 4 && omousex < pickerX + pickerWidth - 4
								&& omousey >= pickerY + 4 && omousey < pickerY + 20 )
							{
								inputstr = editorAmbienceResourceSearch;
								inputlen = 127;
								editproperty = -1;
								cursorflash = ticks;
							}
							else if ( omousex >= pickerX + 4 && omousex < pickerX + pickerWidth - 4
								&& omousey >= listTop && omousey < pickerBottom )
							{
								const int clickedRow = (omousey - listTop) / 12;
								const int selected = editorAmbienceResourceFirstVisible + clickedRow;
								if ( clickedRow < visibleRows && selected >= 0
									&& selected < static_cast<int>(matches.size()) )
								{
									snprintf(mapAmbienceResourceText, sizeof(mapAmbienceResourceText), "%s",
										matches[selected]->c_str());
									editorAmbiencePickerOpen = false;
									inputstr = mapAmbienceResourceText;
									mousestatus[SDL_BUTTON_LEFT] = 0;
								}
							}
						}

						if ( omousex >= subx1 + 104 && omousey >= suby2 - 76 && omousex < subx1 + 168 && omousey < suby2 - 60 )
						{
							inputstr = widthtext;
							editproperty = 13;
							cursorflash = ticks;
						}
						if ( omousex >= subx1 + 104 && omousey >= suby2 - 52 && omousex < subx1 + 168 && omousey < suby2 - 36 )
						{
							inputstr = heighttext;
							editproperty = 14;
							cursorflash = ticks;
						}
					}

					start_y = suby1 + 44;
					pad_y1 = 0;

					if ( editproperty == 0 )   // edit map name
					{
						if ( !SDL_IsTextInputActive() )
						{
							SDL_StartTextInput();
							inputstr = nametext;
						}
						//strncpy(nametext,inputstr,31);
						inputlen = 31;
						if ( (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
						{
							printText(font8x8_bmp, subx1 + 8 + strlen(nametext) * 8, start_y + pad_y1, "\26");
						}
					}
					pad_y1 += 36;
					if ( editproperty == 1 )   // edit author name
					{
						if ( !SDL_IsTextInputActive() )
						{
							SDL_StartTextInput();
							inputstr = authortext;
						}
						//strncpy(authortext,inputstr,31);
						inputlen = 31;
						if ( (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
						{
							printText(font8x8_bmp, subx1 + 8 + strlen(authortext) * 8, start_y + pad_y1, "\26");
						}
					}
					pad_y1 += 24;
					if ( editproperty == 2 )   // edit map skybox
					{
						if ( !SDL_IsTextInputActive() )
						{
							SDL_StartTextInput();
							inputstr = skyboxtext;
						}
						//strncpy(widthtext,inputstr,3);
						inputlen = 3;
						if ( (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
						{
							printText(font8x8_bmp, subx1 + 108 + strlen(skyboxtext) * 8, start_y + pad_y1, "\26");
						}
					}
					pad_y1 += 24;
					if ( editproperty == 3 )   // edit map ceiling tiles
					{
						if ( !SDL_IsTextInputActive() )
						{
							SDL_StartTextInput();
							inputstr = mapflagtext[MAP_FLAG_CEILINGTILE];
						}
						//strncpy(widthtext,inputstr,3);
						inputlen = 3;
						if ( (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
						{
							printText(font8x8_bmp, subx1 + 108 + strlen(mapflagtext[MAP_FLAG_CEILINGTILE]) * 8, start_y + pad_y1, "\26");
						}
					}
					if ( editproperty == 13 )   // edit map width
					{
						if ( !SDL_IsTextInputActive() )
						{
							SDL_StartTextInput();
							inputstr = widthtext;
						}
						//strncpy(widthtext,inputstr,3);
						inputlen = 3;
						if ( (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
						{
							printText(font8x8_bmp, subx1 + 108 + strlen(widthtext) * 8, suby2 - 72, "\26");
						}
					}
					if ( editproperty == 14 )   // edit map height
					{
						if ( !SDL_IsTextInputActive() )
						{
							SDL_StartTextInput();
							inputstr = heighttext;
						}
						//strncpy(heighttext,inputstr,3);
						inputlen = 3;
						if ( (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
						{
							printText(font8x8_bmp, subx1 + 108 + strlen(heighttext) * 8, suby2 - 48, "\26");
						}
					}
					pad_y1 += 24;
					if ( editproperty == 4 )   // edit min entity gen
					{
						if ( !SDL_IsTextInputActive() )
						{
							SDL_StartTextInput();
							inputstr = mapflagtext[MAP_FLAG_GENTOTALMIN];
						}
						inputlen = 2;
						if ( (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
						{
							printText(font8x8_bmp, subx1 + 108 + strlen(mapflagtext[MAP_FLAG_GENTOTALMIN]) * 8, start_y + pad_y1, "\26");
						}
					}
					if ( editproperty == 5 )   // edit max entity gen
					{
						if ( !SDL_IsTextInputActive() )
						{
							SDL_StartTextInput();
							inputstr = mapflagtext[MAP_FLAG_GENTOTALMAX];
						}
						inputlen = 2;
						if ( (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
						{
							printText(font8x8_bmp, subx1 + 148 + strlen(mapflagtext[MAP_FLAG_GENTOTALMAX]) * 8, start_y + pad_y1, "\26");
						}
					}
					pad_y1 += 24;
					if ( editproperty == 6 )   // edit min monster gen
					{
						if ( !SDL_IsTextInputActive() )
						{
							SDL_StartTextInput();
							inputstr = mapflagtext[MAP_FLAG_GENMONSTERMIN];
						}
						inputlen = 2;
						if ( (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
						{
							printText(font8x8_bmp, subx1 + 108 + strlen(mapflagtext[MAP_FLAG_GENMONSTERMIN]) * 8, start_y + pad_y1, "\26");
						}
					}
					if ( editproperty == 7 )   // edit max monster gen
					{
						if ( !SDL_IsTextInputActive() )
						{
							SDL_StartTextInput();
							inputstr = mapflagtext[MAP_FLAG_GENMONSTERMAX];
						}
						inputlen = 2;
						if ( (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
						{
							printText(font8x8_bmp, subx1 + 148 + strlen(mapflagtext[MAP_FLAG_GENMONSTERMAX]) * 8, start_y + pad_y1, "\26");
						}
					}
					pad_y1 += 24;
					if ( editproperty == 8 )   // edit min monster gen
					{
						if ( !SDL_IsTextInputActive() )
						{
							SDL_StartTextInput();
							inputstr = mapflagtext[MAP_FLAG_GENLOOTMIN];
						}
						inputlen = 2;
						if ( (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
						{
							printText(font8x8_bmp, subx1 + 108 + strlen(mapflagtext[MAP_FLAG_GENLOOTMIN]) * 8, start_y + pad_y1, "\26");
						}
					}
					if ( editproperty == 9 )   // edit max monster gen
					{
						if ( !SDL_IsTextInputActive() )
						{
							SDL_StartTextInput();
							inputstr = mapflagtext[MAP_FLAG_GENLOOTMAX];
						}
						inputlen = 2;
						if ( (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
						{
							printText(font8x8_bmp, subx1 + 148 + strlen(mapflagtext[MAP_FLAG_GENLOOTMAX]) * 8, start_y + pad_y1, "\26");
						}
					}
					pad_y1 += 24;
					if ( editproperty == 10 )   // edit min decoration gen
					{
						if ( !SDL_IsTextInputActive() )
						{
							SDL_StartTextInput();
							inputstr = mapflagtext[MAP_FLAG_GENDECORATIONMIN];
						}
						inputlen = 2;
						if ( (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
						{
							printText(font8x8_bmp, subx1 + 108 + strlen(mapflagtext[MAP_FLAG_GENDECORATIONMIN]) * 8, start_y + pad_y1, "\26");
						}
					}
					if ( editproperty == 11 )   // edit max decoration gen
					{
						if ( !SDL_IsTextInputActive() )
						{
							SDL_StartTextInput();
							inputstr = mapflagtext[MAP_FLAG_GENDECORATIONMAX];
						}
						inputlen = 2;
						if ( (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
						{
							printText(font8x8_bmp, subx1 + 148 + strlen(mapflagtext[MAP_FLAG_GENDECORATIONMAX]) * 8, start_y + pad_y1, "\26");
						}
					}
					pad_y1 += 24;
					if ( editproperty == 12 )   // edit perimeter gap
					{
						if ( !SDL_IsTextInputActive() )
						{
							SDL_StartTextInput();
							inputstr = mapflagtext[MAP_FLAG_PERIMETER_GAP];
						}
						inputlen = 3;
						if ( (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
						{
							printText(font8x8_bmp, subx1 + 108 + strlen(mapflagtext[MAP_FLAG_PERIMETER_GAP]) * 8, start_y + pad_y1, "\26");
						}
					}
					if ( editproperty >= 15 && editproperty <= 19 )
					{
						char* fogEditValues[5] =
						{
							mapFogDistanceText,
							mapFogDensityText,
							mapFogRedText,
							mapFogGreenText,
							mapFogBlueText
						};
						const int fogIndex = editproperty - 15;
						if ( !SDL_IsTextInputActive() )
						{
							SDL_StartTextInput();
							inputstr = fogEditValues[fogIndex];
						}
						inputlen = fogIndex == 0 ? 4 : 3;
						if ( (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
						{
							const int fogCursorY = suby1 + 392 + fogIndex * 18;
							printText(
								font8x8_bmp,
								subx1 + 112 + strlen(fogEditValues[fogIndex]) * 8,
								fogCursorY,
								"\26"
							);
						}
					}
					if ( editproperty >= 20 && editproperty <= 22 )
					{
						char* ambientLightEditValues[3] = {
							mapAmbientLightRedText,
							mapAmbientLightGreenText,
							mapAmbientLightBlueText};
						const int lightIndex = editproperty - 20;
						if ( !SDL_IsTextInputActive() )
						{
							SDL_StartTextInput();
							inputstr = ambientLightEditValues[lightIndex];
						}
						inputlen = 3;
						if ( (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
						{
							printText(font8x8_bmp, subx1 + 340
								+ strlen(ambientLightEditValues[lightIndex]) * 8,
								suby1 + 392 + lightIndex * 18, "\26");
						}
					}
					if ( editproperty >= 23 && editproperty <= 25 )
					{
						char* ambienceEditValues[3] = {
							mapAmbienceVolumeText,
							mapAmbienceFadeInText,
							mapAmbienceFadeOutText};
						const int ambienceIndex = editproperty - 23;
						if ( !SDL_IsTextInputActive() )
						{
							SDL_StartTextInput();
							inputstr = ambienceEditValues[ambienceIndex];
						}
						inputlen = ambienceIndex == 0 ? 3 : 5;
						if ( (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
						{
							const int ambienceCursorY = suby1 + 410 + ambienceIndex * 36;
							printText(font8x8_bmp, subx1 + 580
								+ strlen(ambienceEditValues[ambienceIndex]) * 8,
								ambienceCursorY, "\26");
						}
					}
				}
				else if ( newwindow == 2 ) 
				{
					if ( selectedEntity[0] != NULL )
					{
						spriteStats = selectedEntity[0]->getStats();
						if ( spriteStats != nullptr )
						{
							int numProperties = sizeof(monsterPropertyNames) / sizeof(monsterPropertyNames[0]); //find number of entries in property list
							const int lenProperties = sizeof(monsterPropertyNames[0]) / sizeof(char); //find length of entry in property list
							int spacing = 20; // px between each item in the list.
							int pad_y1 = suby1 + 28; // 28 px spacing from subwindow start.
							int pad_x1 = subx1 + 8; // 8px spacing from subwindow start.
							int pad_x2 = 64;
							int pad_y2 = suby1 + 28 + 2 * spacing;
							int pad_x3 = 44; //property field width
							int pad_x4;
							Uint32 color = makeColorRGB(255, 255, 255);
							char tmpPropertyName[lenProperties] = "";
							for ( int i = 0; i < numProperties; i++ )
							{
								strcpy(tmpPropertyName, monsterPropertyNames[i]);
								pad_y1 = suby1 + 28 + i * spacing;

								// value of 0 is name field, should be longer
								if ( i == 0 )
								{
									drawDepressed(pad_x1 - 4, pad_y1 + 16 - 4, subx2 - 4, pad_y1 + 28);
									// print values on top of boxes
									printTextFormattedColor(font8x8_bmp, pad_x1, pad_y1, color, tmpPropertyName);
									printText(font8x8_bmp, pad_x1, pad_y1 + 16, spriteProperties[i]);
								}
								else
								{
									pad_y1 += spacing;
									if ( i < 7 )
									{
										if ( i < 3 ) //hp
										{
											color = makeColorRGB(0, 255, 0);
										}
										else if ( i < 5 ) //mp
										{
											color = makeColorRGB(0, 255, 228);
										}
										else if ( i == 5 ) //level
										{
											color = makeColorRGB(255, 192, 0);
										}
										else if ( i == 6 ) //gold
										{
											color = makeColorRGB(255, 192, 0);
										}
										drawDepressed(pad_x1 + pad_x2 - 4, pad_y1 - 4, pad_x1 + pad_x2 + pad_x3 - 4, pad_y1 + 16 - 4);
										// draw another box side by side, spaced by pad_x3 + 16
										drawDepressed(pad_x1 + pad_x2 - 4 + (pad_x3 + 16), pad_y1 - 4, pad_x1 + pad_x2 + pad_x3 - 4 + (pad_x3 + 16), pad_y1 + 16 - 4);
										// print values on top of boxes
										// print property name
										printTextFormattedColor(font8x8_bmp, pad_x1, pad_y1, color, tmpPropertyName);
										// print dash between boxes
										printTextFormattedColor(font8x8_bmp, pad_x1 + pad_x2 - 4 + (pad_x3 + 4), pad_y1, color, "-");
										// print left text
										printText(font8x8_bmp, pad_x1 + pad_x2, pad_y1, spriteProperties[i]);
										// print right text
										printText(font8x8_bmp, pad_x1 + pad_x2 + (pad_x3 + 16), pad_y1, spriteProperties[i + 12]);
									}
									else if ( i < 13 )
									{
										color = makeColorRGB(255, 255, 255);
										pad_y1 += spacing + 10;
										drawDepressed(pad_x1 + pad_x2 - 4, pad_y1 - 4, pad_x1 + pad_x2 + pad_x3 - 4, pad_y1 + 16 - 4);
										// draw another box side by side, spaced by pad_x3 + 16
										drawDepressed(pad_x1 + pad_x2 - 4 + (pad_x3 + 16), pad_y1 - 4, pad_x1 + pad_x2 + pad_x3 - 4 + (pad_x3 + 16), pad_y1 + 16 - 4);
										// print values on top of boxes
										// print property name
										printTextFormattedColor(font8x8_bmp, pad_x1, pad_y1, color, tmpPropertyName);
										// print dash between boxes
										printTextFormattedColor(font8x8_bmp, pad_x1 + pad_x2 - 4 + (pad_x3 + 4), pad_y1, color, "-");
										// print left text
										printText(font8x8_bmp, pad_x1 + pad_x2, pad_y1, spriteProperties[i]);
										// print right text
										printText(font8x8_bmp, pad_x1 + pad_x2 + (pad_x3 + 16), pad_y1, spriteProperties[i + 12]);
									}
									else if ( i >= 13 )
									{
										if ( i == 13 )
										{
											pad_y1 += 10;
										}
										color = makeColorRGB(255, 255, 255);
										pad_y1 += spacing + 10;
										drawDepressed(pad_x1 + pad_x2 - 4, pad_y1 - 4, pad_x1 + pad_x2 + pad_x3 - 4, pad_y1 + 16 - 4);
										// print property name
										printTextFormattedColor(font8x8_bmp, pad_x1, pad_y1, color, tmpPropertyName);
										// print left text
										printText(font8x8_bmp, pad_x1 + pad_x2, pad_y1, spriteProperties[i + 12]);
										if ( i == 13 && spriteStats->type == SHOPKEEPER )
										{
											char shopTypeText[32] = "";
											color = makeColorRGB(0, 255, 0);
											switch ( atoi(spriteProperties[25]) )
											{
												case 1:
													strcpy(shopTypeText, "Arms and Armor");
													break;
												case 2:
													strcpy(shopTypeText, "Hats and Helmets");
													break;
												case 3:
													strcpy(shopTypeText, "Jewelry");
													break;
												case 4:
													strcpy(shopTypeText, "Bookstore");
													break;
												case 5:
													strcpy(shopTypeText, "Apothecary");
													break;
												case 6:
													strcpy(shopTypeText, "Magistaffs");
													break;
												case 7:
													strcpy(shopTypeText, "Food Store");
													break;
												case 8:
													strcpy(shopTypeText, "Hardware Store");
													break;
												case 9:
													strcpy(shopTypeText, "Hunting Store");
													break;
												case 10:
													strcpy(shopTypeText, "General Store");
													break;
												default:
													strcpy(shopTypeText, "Default Random Store");
													color = makeColorRGB(255, 255, 255);
													break;
											}
											printTextFormattedColor(font8x8_bmp, pad_x1 + pad_x2 + pad_x3 + 8, pad_y1, color, shopTypeText);
											color = makeColorRGB(255, 255, 255);
										}
									}
								}
							}
							
							// Cycle properties with TAB.
							if ( keystatus[SDLK_TAB] )
							{
								keystatus[SDLK_TAB] = 0;
								cursorflash = ticks;
								editproperty++;
								/*
								* The original monster fields use indices 0 through 25.
								* Index 26 is the custom dialogue ID.
								*/
								if ( editproperty > 26 )
								{
									editproperty = 0;
								}
								
								inputstr = spriteProperties[editproperty];
							}
							// select a textbox
							if ( mousestatus[SDL_BUTTON_LEFT] )
							{
								for ( int i = 0; i < numProperties; i++ )
								{
									pad_y1 = suby1 + 28 + i * spacing;
									if ( i == 0 )
									{
										if ( omousex >= pad_x1 - 4 && omousey >= pad_y1 + 16 - 4 && omousex < subx2 - 4 && omousey < pad_y1 + 32 - 4 )
										{
											inputstr = spriteProperties[i];
											editproperty = i;
											cursorflash = ticks;
										}
									}
									else
									{
										pad_y1 += spacing;
										if ( i < 7 )
										{
											// check if mouse is in left property box
											if ( omousex >= pad_x1 + pad_x2 - 4 && omousey >= pad_y1 - 4 && omousex < pad_x1 + pad_x2 + pad_x3 - 4 && omousey < pad_y1 + 16 - 4 )
											{
												inputstr = spriteProperties[i];
												editproperty = i;
												cursorflash = ticks;
											}
											// check if mouse is in right property box (offset from above by pad_x3 + 16)
											else if ( omousex >= pad_x1 + pad_x2 - 4 + (pad_x3 + 16) && omousey >= pad_y1 - 4 && omousex < pad_x1 + pad_x2 + pad_x3 - 4 + (pad_x3 + 16) && omousey < pad_y1 + 16 - 4 )
											{
												inputstr = spriteProperties[i + 12];
												editproperty = i + 12;
												cursorflash = ticks;
											}
										}
										else if ( i < 13 )
										{
											pad_y1 += spacing + 10;
											// check if mouse is in left property box
											if ( omousex >= pad_x1 + pad_x2 - 4 && omousey >= pad_y1 - 4 && omousex < pad_x1 + pad_x2 + pad_x3 - 4 && omousey < pad_y1 + 16 - 4 )
											{
												inputstr = spriteProperties[i];
												editproperty = i;
												cursorflash = ticks;
											}
											// check if mouse is in right property box (offset from above by pad_x3 + 16)
											else if ( omousex >= pad_x1 + pad_x2 - 4 + (pad_x3 + 16) && omousey >= pad_y1 - 4 && omousex < pad_x1 + pad_x2 + pad_x3 - 4 + (pad_x3 + 16) && omousey < pad_y1 + 16 - 4 )
											{
												inputstr = spriteProperties[i + 12];
												editproperty = i + 12;
												cursorflash = ticks;
											}
										}
										else if ( i >= 13 )
										{
											if ( i == 13 )
											{
												pad_y1 += 10;
											}
											pad_y1 += spacing + 10;
											// check if mouse is in left property box
											if ( omousex >= pad_x1 + pad_x2 - 4 && omousey >= pad_y1 - 4 && omousex < pad_x1 + pad_x2 + pad_x3 - 4 && omousey < pad_y1 + 16 - 4 )
											{
												inputstr = spriteProperties[i + 12];
												editproperty = i + 12;
												cursorflash = ticks;
											}
										}
									}
								}
							}
							const bool showMiniMimicNPCControls =
								spriteStats->type == MINIMIMIC;
							if ( showMiniMimicNPCControls )
							{
								const int controlsY =
									suby1 + MINIMIMIC_NPC_CONTROLS_Y_OFFSET;
								const int dispositionX =
									subx1 + MINIMIMIC_DISPOSITION_X_OFFSET;
								const int recruitableX =
									subx1 + MINIMIMIC_RECRUITABLE_X_OFFSET;
								const int customDialogueX =
									subx1 + MINIMIMIC_DIALOGUE_TOGGLE_X_OFFSET;
								const int disposition = std::max(0, std::min(2,
									atoi(spriteProperties[MONSTER_PROPERTY_DISPOSITION])));
								const char* dispositionName = disposition == 1
									? "Passive"
									: (disposition == 2 ? "Friendly" : "Hostile");
								const bool recruitable =
									atoi(spriteProperties[MONSTER_PROPERTY_RECRUITABLE]) != 0;
								const bool dialogueEnabled =
									atoi(spriteProperties[MONSTER_PROPERTY_DIALOGUE_ENABLED]) != 0;
								const bool scaledMimicAppearance =
									atoi(spriteProperties[
										MONSTER_PROPERTY_MINIMIMIC_APPEARANCE]) != 0;

								printTextFormattedColor(
									font8x8_bmp,
									dispositionX,
									controlsY,
									makeColorRGB(255, 220, 128),
									"Disposition: [%s]",
									dispositionName
								);
								printTextFormattedColor(
									font8x8_bmp,
									recruitableX,
									controlsY,
									recruitable
										? makeColorRGB(96, 255, 96)
										: makeColorRGB(220, 220, 220),
									"Recruitable: [%c]",
									recruitable ? 'x' : ' '
								);
								printTextFormattedColor(
									font8x8_bmp,
									customDialogueX,
									controlsY,
									dialogueEnabled
										? makeColorRGB(96, 255, 96)
										: makeColorRGB(220, 220, 220),
									"Custom Dialogue: [%c]",
									dialogueEnabled ? 'x' : ' '
								);

								if ( mousestatus[SDL_BUTTON_LEFT]
									&& omousey >= controlsY - 3
									&& omousey < controlsY + 12 )
								{
									if ( omousex >= dispositionX
										&& omousex < recruitableX - 8 )
									{
										mousestatus[SDL_BUTTON_LEFT] = 0;
										snprintf(
											spriteProperties[MONSTER_PROPERTY_DISPOSITION],
											sizeof(spriteProperties[MONSTER_PROPERTY_DISPOSITION]),
											"%d",
											(disposition + 1) % 3
										);
									}
									else if ( omousex >= recruitableX
										&& omousex < customDialogueX - 8 )
									{
										mousestatus[SDL_BUTTON_LEFT] = 0;
										snprintf(
											spriteProperties[MONSTER_PROPERTY_RECRUITABLE],
											sizeof(spriteProperties[MONSTER_PROPERTY_RECRUITABLE]),
											"%d",
											recruitable ? 0 : 1
										);
									}
									else if ( omousex >= customDialogueX
										&& omousex < subx2 - 8 )
									{
										mousestatus[SDL_BUTTON_LEFT] = 0;
										snprintf(
											spriteProperties[MONSTER_PROPERTY_DIALOGUE_ENABLED],
											sizeof(spriteProperties[MONSTER_PROPERTY_DIALOGUE_ENABLED]),
											"%d",
											dialogueEnabled ? 0 : 1
										);
									}
								}

								const int appearanceY = controlsY
									+ MINIMIMIC_APPEARANCE_ROW_OFFSET;
								printTextFormattedColor(
									font8x8_bmp,
									subx1 + 8,
									appearanceY,
									makeColorRGB(160, 220, 255),
									"Appearance: [%s]",
									scaledMimicAppearance
										? "Scaled Mimic"
										: "Baby"
								);
								printTextFormattedColor(
									font8x8_bmp,
									subx1 + 240,
									appearanceY,
									makeColorRGB(180, 180, 180),
									scaledMimicAppearance
										? "full mimic, fitted to Mini size"
										: "Mini shell + mimic insides"
								);
								if ( mousestatus[SDL_BUTTON_LEFT]
									&& omousex >= subx1 + 8
									&& omousex < subx2 - 8
									&& omousey >= appearanceY - 3
									&& omousey < appearanceY + 12 )
								{
									mousestatus[SDL_BUTTON_LEFT] = 0;
									snprintf(
										spriteProperties[
											MONSTER_PROPERTY_MINIMIMIC_APPEARANCE],
										sizeof(spriteProperties[
											MONSTER_PROPERTY_MINIMIMIC_APPEARANCE]),
										"%d",
										scaledMimicAppearance
											? MINIMIMIC_APPEARANCE_BABY
											: MINIMIMIC_APPEARANCE_SCALED_MIMIC
									);
								}
							}

							/*
							* Custom dialogue assignment.
							*
							* The full visual node editor will be opened from this area in a later
							* stage. For now, this field establishes a stable graph ID.
							*/
							const int dialogueLabelX =
								subx1 + 8;

							const int dialogueLabelY =
								suby1 + MINIMIMIC_NPC_CONTROLS_Y_OFFSET
								+ (showMiniMimicNPCControls
									? MINIMIMIC_DIALOGUE_LABEL_ROW_OFFSET
									: 0);

							const int dialogueFieldX1 =
								subx1 + 8;

							const int dialogueFieldY1 =
								dialogueLabelY + 14;

							const int dialogueFieldX2 =
								subx2 - 80;

							const int dialogueFieldY2 =
								dialogueFieldY1 + 16;

							printTextFormattedColor(
								font8x8_bmp,
								dialogueLabelX,
								dialogueLabelY,
								makeColorRGB(255, 255, 255),
								"Dialogue Resource:"
							);

							drawDepressed(
								dialogueFieldX1 - 4,
								dialogueFieldY1 - 4,
								dialogueFieldX2,
								dialogueFieldY2
							);

							/*
							* Only display the portion that fits in the current fixed editor field.
							* The full value remains in spriteProperties[26].
							*/
							char visibleDialogueID[48] = "";

							strncpy(
								visibleDialogueID,
								spriteProperties[26],
								sizeof(visibleDialogueID) - 1
							);

							visibleDialogueID[
								sizeof(visibleDialogueID) - 1
							] = '\0';

							printText(
								font8x8_bmp,
								dialogueFieldX1,
								dialogueFieldY1,
								visibleDialogueID
							);

							if ( mousestatus[SDL_BUTTON_LEFT]
								&& omousex >= dialogueFieldX1 - 4
								&& omousex < dialogueFieldX2
								&& omousey >= dialogueFieldY1 - 4
								&& omousey < dialogueFieldY2 )
							{
								mousestatus[SDL_BUTTON_LEFT] = 0;
								if ( showMiniMimicNPCControls )
								{
									strcpy(
										spriteProperties[MONSTER_PROPERTY_DIALOGUE_ENABLED],
										"1"
									);
								}

								inputstr = spriteProperties[26];
								editproperty = 26;
								cursorflash = ticks;
							}

							const int dialogueEditorButtonX1 =
								dialogueFieldX2 + 4;

							const int dialogueEditorButtonY1 =
								dialogueFieldY1 - 4;

							const int dialogueEditorButtonX2 =
								subx2 - 8;

							const int dialogueEditorButtonY2 =
								dialogueFieldY2;

							drawWindowFancy(
								dialogueEditorButtonX1,
								dialogueEditorButtonY1,
								dialogueEditorButtonX2,
								dialogueEditorButtonY2
							);

							printTextFormattedColor(
								font8x8_bmp,
								dialogueEditorButtonX1 + 4,
								dialogueFieldY1,
								makeColorRGB(255, 230, 96),
								"EDIT..."
							);

							if ( mousestatus[SDL_BUTTON_LEFT]
								&& omousex >= dialogueEditorButtonX1
								&& omousex < dialogueEditorButtonX2
								&& omousey >= dialogueEditorButtonY1
								&& omousey < dialogueEditorButtonY2 )
							{
								mousestatus[SDL_BUTTON_LEFT] = 0;
								if ( showMiniMimicNPCControls )
								{
									strcpy(
										spriteProperties[MONSTER_PROPERTY_DIALOGUE_ENABLED],
										"1"
									);
								}
								openQuestDialogueEditor();
							}

							const char* authoredMonsterLabels[4] =
							{
								"Squad ID:",
								"Squad Options:",
								"Elite Preset:",
								"Defeat ID:"
							};

							const char* authoredMonsterHelp[4] =
							{
								"0 none",
								"role 1/2 + bits 4,8,16,32",
								"0 normal, 1-7 named",
								"numeric quest/squad tag"
							};

							const int authoredFieldX1 =
								subx1 + 8;
							const int authoredFieldX2 =
								subx1 + 120;
							const int authoredHelpX =
								subx1 + 132;
							const int authoredStartY =
								dialogueFieldY2 + 18;

							// Keep the miniboss toggle with the authored monster fields
							// instead of below the inventory template buttons.
							const int disableMinibossX = subx2 - 176;
							const int disableMinibossY = authoredStartY;
							printTextFormattedColor(
								font8x8_bmp,
								disableMinibossX,
								disableMinibossY,
								color,
								!strcmp(spriteProperties[31], "disable")
									? "Disable Miniboss: [x]"
									: "Disable Miniboss: [ ]"
							);
							if ( mousestatus[SDL_BUTTON_LEFT] )
							{
								const int checkboxX1 = disableMinibossX
									+ static_cast<int>(strlen("Disable Miniboss: ")) * 8;
								const int checkboxX2 = checkboxX1 + 3 * 8;
								if ( omousex >= checkboxX1
									&& omousex < checkboxX2
									&& omousey >= disableMinibossY
									&& omousey < disableMinibossY + 8 )
								{
									mousestatus[SDL_BUTTON_LEFT] = 0;
									if ( !strcmp(spriteProperties[31], "disable") )
									{
										strcpy(spriteProperties[31], "");
									}
									else
									{
										strcpy(spriteProperties[31], "disable");
									}
								}
							}

							for ( int authoredIndex = 0;
								authoredIndex < 4;
								++authoredIndex )
							{
								const int propertyIndex =
									27 + authoredIndex;
								const int fieldY =
									authoredStartY
									+ authoredIndex * 22;

								printTextFormattedColor(
									font8x8_bmp,
									authoredFieldX1,
									fieldY,
									makeColorRGB(255, 255, 255),
									authoredMonsterLabels[
										authoredIndex
									]
								);

								drawDepressed(
									authoredFieldX2 - 4,
									fieldY - 4,
									authoredFieldX2 + 64,
									fieldY + 12
								);

								printText(
									font8x8_bmp,
									authoredFieldX2,
									fieldY,
									spriteProperties[propertyIndex]
								);

								printTextFormattedColor(
									font8x8_bmp,
									authoredHelpX + 60,
									fieldY,
									makeColorRGB(160, 200, 255),
									authoredMonsterHelp[
										authoredIndex
									]
								);

								if ( mousestatus[SDL_BUTTON_LEFT]
									&& omousex
										>= authoredFieldX2 - 4
									&& omousex
										< authoredFieldX2 + 64
									&& omousey >= fieldY - 4
									&& omousey < fieldY + 12 )
								{
									mousestatus[
										SDL_BUTTON_LEFT
									] = 0;
									inputstr =
										spriteProperties[
											propertyIndex
										];
									editproperty =
										propertyIndex;
									cursorflash = ticks;
								}
							}

							const int currentElitePreset =
								std::max(
									0,
									atoi(
										spriteProperties[29]
									)
								);

							const char* currentElitePresetName =
								"Normal monster";
							switch ( currentElitePreset )
							{
								case 1:
									currentElitePresetName =
										"Algernon";
									break;
								case 2:
									currentElitePresetName =
										"Funny Bones";
									break;
								case 3:
									currentElitePresetName =
										"Potato King";
									break;
								case 4:
									currentElitePresetName =
										"Thumpus";
									break;
								case 5:
									currentElitePresetName =
										"Bram Kindly";
									break;
								case 6:
									currentElitePresetName =
										"Johann";
									break;
								case 7:
									currentElitePresetName =
										"Merlin";
									break;
								default:
									break;
							}

							const int authoredInfoY =
								authoredStartY + 4 * 22 + 10;

							int squadOptionsValue =
								std::max(
									0,
									atoi(
										spriteProperties[28]
									)
								);
							const int squadRoleBits =
								squadOptionsValue & 3;
							const bool squadLeaderEnabled =
								squadRoleBits == 1;
							const bool squadFollowEnabled =
								(squadOptionsValue & 4) != 0;
							const bool squadAssistEnabled =
								(squadOptionsValue & 8) != 0;
							const bool squadShareTargetEnabled =
								(squadOptionsValue & 16) != 0;
							const bool squadWakeTogetherEnabled =
								(squadOptionsValue & 32) != 0;

							const char* squadRoleName = "None";
							if ( squadRoleBits == 1 )
							{
								squadRoleName = "Leader";
							}
							else if ( squadRoleBits == 2 )
							{
								squadRoleName = "Member";
							}

							printTextFormattedColor(
								font8x8_bmp,
								authoredFieldX1,
								authoredInfoY,
								makeColorRGB(96, 255, 96),
								"Current elite preset: %s",
								currentElitePresetName
							);

							printTextFormattedColor(
								font8x8_bmp,
								authoredFieldX1,
								authoredInfoY + 16,
								makeColorRGB(96, 220, 255),
								"Current squad role: %s",
								squadRoleName
							);

							const char* squadToggleLabels[5] =
							{
								"Leader",
								"Follows leader",
								"Assists squadmates",
								"Shares target",
								"Wakes together"
							};
							const bool squadToggleValues[5] =
							{
								squadLeaderEnabled,
								squadFollowEnabled,
								squadAssistEnabled,
								squadShareTargetEnabled,
								squadWakeTogetherEnabled
							};
							const int squadToggleBits[5] =
							{
								1,
								4,
								8,
								16,
								32
							};

							for ( int squadToggleIndex = 0;
								squadToggleIndex < 5;
								++squadToggleIndex )
							{
								const int toggleY =
									authoredInfoY + 30 + squadToggleIndex * 12;
								const int boxX1 = authoredFieldX1;
								const int boxY1 = toggleY - 2;
								const int boxX2 = authoredFieldX1 + 10;
								const int boxY2 = toggleY + 10;
								const int clickX2 = authoredFieldX1 + 120;

								drawDepressed(
									boxX1,
									boxY1,
									boxX2,
									boxY2
								);

								if ( squadToggleValues[squadToggleIndex] )
								{
									printTextFormattedColor(
										font8x8_bmp,
										boxX1 + 2,
										toggleY,
										makeColorRGB(96, 255, 96),
										"X"
									);
								}

								printTextFormattedColor(
									font8x8_bmp,
									authoredFieldX1 + 16,
									toggleY,
									squadToggleValues[squadToggleIndex]
										? makeColorRGB(96, 255, 96)
										: makeColorRGB(220, 220, 220),
									squadToggleLabels[squadToggleIndex]
								);

								if ( mousestatus[SDL_BUTTON_LEFT]
									&& omousex >= boxX1
									&& omousex < clickX2
									&& omousey >= boxY1
									&& omousey < boxY2 )
								{
									mousestatus[SDL_BUTTON_LEFT] = 0;
									int newSquadOptionsValue =
										squadOptionsValue;

									if ( squadToggleIndex == 0 )
									{
										if ( (newSquadOptionsValue & 3) == 1 )
										{
											newSquadOptionsValue &= ~3;
										}
										else
										{
											newSquadOptionsValue =
												(newSquadOptionsValue & ~3) | 1;
										}
									}
									else
									{
										newSquadOptionsValue ^=
											squadToggleBits[
												squadToggleIndex
											];
									}

									if ( (newSquadOptionsValue & 3) != 1 )
									{
										if ( newSquadOptionsValue
											& (4 | 8 | 16 | 32) )
										{
											newSquadOptionsValue =
												(newSquadOptionsValue & ~3) | 2;
										}
										else
										{
											newSquadOptionsValue &= ~3;
										}
									}

									snprintf(
										spriteProperties[28],
										sizeof(spriteProperties[28]),
										"%d",
										newSquadOptionsValue
									);
								}
							}

							printTextFormattedColor(
								font8x8_bmp,
								authoredFieldX1,
								authoredInfoY + 92,
								makeColorRGB(220, 220, 220),
								"Field guide:"
							);
							printTextFormattedColor(
								font8x8_bmp,
								authoredFieldX1,
								authoredInfoY + 104,
								makeColorRGB(180, 180, 180),
								"Squad ID: same number = same squad."
							);
							printTextFormattedColor(
								font8x8_bmp,
								authoredFieldX1,
								authoredInfoY + 116,
								makeColorRGB(180, 180, 180),
								"Leader off + squad bits on = member."
							);
							printTextFormattedColor(
								font8x8_bmp,
								authoredFieldX1,
								authoredInfoY + 128,
								makeColorRGB(180, 180, 180),
								"Elite IDs: 0 normal, 1-7 named elites."
							);
							printTextFormattedColor(
								font8x8_bmp,
								authoredFieldX1,
								authoredInfoY + 140,
								makeColorRGB(180, 180, 180),
								"Defeat ID: numeric tag for later checks."
							);

							//items for monster
							pad_y2 = suby1 + 28 + 2 * spacing;
							pad_x3 = 40;
							pad_x4 = subx2 - 112;
							printTextFormattedColor(font8x8_bmp, pad_x4 - 8, pad_y2, color, " Helm");
							
							//pad_y2 += spacing * 2 - 16;
							pad_y2 += spacing * 2;
							printTextFormattedColor(font8x8_bmp, pad_x4 - 8, pad_y2, color, "Amulet");

							//pad_x4 += 64 * 2;
							pad_y2 += spacing * 2;
							printTextFormattedColor(font8x8_bmp, pad_x4 - 4, pad_y2, color, "Armor");

							//pad_x4 -= 64;
							//pad_y2 += spacing * 2 - 16;
							pad_y2 += spacing * 2;
							printTextFormattedColor(font8x8_bmp, pad_x4 - 8 - 4, pad_y2, color, " Boots");

							pad_y2 = suby1 + 28 + 2 * spacing;
							pad_y2 += 16;
							pad_x4 -= 64;
							printTextFormattedColor(font8x8_bmp, pad_x4 - 8 - 4, pad_y2, color, " Cloak");

							pad_x4 += 64 * 2;
							printTextFormattedColor(font8x8_bmp, pad_x4 - 8, pad_y2, color, " Mask");

							pad_x4 -= 64 * 2;
							pad_y2 += spacing * 2;
							printTextFormattedColor(font8x8_bmp, pad_x4 - 8, pad_y2, color, "Weapon");

							pad_x4 += 64 * 2;
							printTextFormattedColor(font8x8_bmp, pad_x4 - 8, pad_y2, color, "Shield");

							pad_x4 -= 64 * 2;
							pad_y2 += spacing * 2;
							printTextFormattedColor(font8x8_bmp, pad_x4 - 8, pad_y2, color, " Ring");

							pad_x4 += 64 * 2;
							printTextFormattedColor(font8x8_bmp, pad_x4 - 8, pad_y2, color, "Gloves");

							pad_x4 -= 64 * 2;

							pad_y2 += 32 + spacing * 2;
							int activeInventorySlots = 6;
							Stat* inventoryStats = selectedEntity[0] != nullptr ? selectedEntity[0]->getStats() : nullptr;
							if ( inventoryStats != nullptr )
							{
								activeInventorySlots = inventoryStats->MISC_FLAGS[31];
								if ( activeInventorySlots < 1 || activeInventorySlots > ITEM_SLOT_INVENTORY_COUNT )
								{
									activeInventorySlots = 6;
								}
							}
							const int inventoryPageCount = (activeInventorySlots + 5) / 6;
							const int inventoryFirstSlot = monsterInventoryPage * 6 + 1;
							const int inventoryLastSlot = std::min(activeInventorySlots, inventoryFirstSlot + 5);
							printTextFormattedColor(font8x8_bmp, pad_x4 - 8, pad_y2, color,
								"Inventory Page %d of %d", monsterInventoryPage + 1, inventoryPageCount);
							printTextFormattedColor(font8x8_bmp, pad_x4 - 8, pad_y2 + 12, color,
								"Slots %d-%d of %d active", inventoryFirstSlot, inventoryLastSlot, activeInventorySlots);


							if ( editproperty <= 30 )
							{
								// limit of properties is twice the vertical count
								if ( !SDL_IsTextInputActive() )
								{
									SDL_StartTextInput();
									inputstr = spriteProperties[0];
								}
								//strncpy(nametext,inputstr,31);

								// value of 0 is the name field, else the input is a number
								if ( editproperty == 0 )
								{
									inputlen = 31;
								}
								else if ( editproperty == 26 )
								{
									/*
									* Reserve one byte for null termination in Stat::customDialogueID.
									*/
									inputlen = 63;
								}
								else if ( editproperty >= 27
									&& editproperty <= 30 )
								{
									inputlen = 9;
								}
								else
								{
									inputlen = 4;
								}
								if ( (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
								{
									pad_y1 = suby1 + 28 + editproperty * spacing;

									if ( editproperty >= 27
										&& editproperty <= 30 )
									{
										const int authoredIndex =
											editproperty - 27;
										const int authoredCursorX =
											subx1
											+ 120
											+ static_cast<int>(
												strlen(
													spriteProperties[
														editproperty
													]
												)
											) * 8;
										const int authoredCursorY =
											authoredStartY
											+ authoredIndex * 22;

										printText(
											font8x8_bmp,
											authoredCursorX,
											authoredCursorY,
											"\26"
										);
									}
									else if ( editproperty == 26 )
									{
										const int dialogueCursorX =
											subx1
											+ 8
											+ static_cast<int>(
												strlen(spriteProperties[26])
											) * 8;

										const int dialogueCursorY =
											dialogueFieldY1;

										/*
										* Keep the cursor inside the visible field even when the stored ID
										* is longer than the currently visible portion.
										*/
										printText(
											font8x8_bmp,
											std::min(
												dialogueCursorX,
												subx2 - 88
											),
											dialogueCursorY,
											"\26"
										);
									}
									else if ( editproperty == 0 )
									{
										printText(font8x8_bmp, pad_x1 + strlen(spriteProperties[editproperty]) * 8, pad_y1 + 16, "\26");
									}
									else if ( editproperty < 7 )
									{
										pad_y1 += spacing;
										// left box
										printText(font8x8_bmp, pad_x1 + pad_x2 + strlen(spriteProperties[editproperty]) * 8, pad_y1, "\26");
									}
									else if ( editproperty < 13 )
									{
										pad_y1 += spacing;
										pad_y1 += spacing + 10;
										// left box
										printText(font8x8_bmp, pad_x1 + pad_x2 + strlen(spriteProperties[editproperty]) * 8, pad_y1, "\26");
									}
									else if ( editproperty < 19 )
									{
										pad_y1 = suby1 + 28 + (editproperty - 12) * spacing;
										pad_y1 += spacing;
										// right box
										printText(font8x8_bmp, pad_x1 + pad_x2 + (pad_x3 + 20) + strlen(spriteProperties[editproperty]) * 8, pad_y1, "\26");
									}
									else if ( editproperty < 25 )
									{
										pad_y1 = suby1 + 28 + (editproperty - 12) * spacing;
										pad_y1 += spacing;
										pad_y1 += spacing + 10;
										// right box
										printText(font8x8_bmp, pad_x1 + pad_x2 + (pad_x3 + 20) + strlen(spriteProperties[editproperty]) * 8, pad_y1, "\26");
									}
									else if ( editproperty >= 25 )
									{
										pad_y1 = suby1 + 28 + (editproperty - 12) * spacing;
										pad_y1 += spacing;
										pad_y1 += spacing + 20;
										// left box
										printText(font8x8_bmp, pad_x1 + pad_x2 + strlen(spriteProperties[editproperty]) * 8, pad_y1, "\26");
									}
								}
							}
					}
				}
				}
				else if ( newwindow == 3 )
				{
					if ( selectedEntity[0] != NULL )
					{
						int numProperties = sizeof(chestPropertyNames) / sizeof(chestPropertyNames[0]); //find number of entries in property list
						const int lenProperties = sizeof(chestPropertyNames[0]) / sizeof(char); //find length of entry in property list
						int spacing = 40; // Keep labels, fields, and feedback on distinct rows.
						int pad_y1 = suby1 + 28; // 28 px spacing from subwindow start.
						int pad_x1 = subx1 + 8; // 8px spacing from subwindow start.
						int pad_x2 = 64;
						int pad_x3 = pad_x1 + pad_x2 + 8;
						int pad_y2 = 0;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);

						for ( int i = 0; i < numProperties; i++ )
						{
							int propertyInt = atoi(spriteProperties[i]);

							strcpy(tmpPropertyName, chestPropertyNames[i]);
							pad_y1 = suby1 + 28 + i * spacing;
							pad_y2 = suby1 + 44 + i * spacing;
							// box outlines then text
							drawDepressed(pad_x1 - 4, suby1 + 40 + i * spacing, pad_x1 - 4 + pad_x2, suby1 + 56 + i * spacing);
							// print values on top of boxes
							printText(font8x8_bmp, pad_x1, suby1 + 44 + i * spacing, spriteProperties[i]);
							printText(font8x8_bmp, pad_x1, pad_y1, tmpPropertyName);

							if ( errorArr[i] != 1 )
							{
								if ( i == 0 )
								{
									if ( propertyInt > 3 || propertyInt < 0 )
									{
										errorMessage = 60;
										errorArr[i] = 1;
										snprintf(spriteProperties[i], sizeof(spriteProperties[i]), "%d", 1); //reset
									}
									else
									{
										color = makeColorRGB(0, 255, 0);
										char tmpStr[32] = "";
										if ( propertyInt == 0 )
										{
											strcpy(tmpStr, "EAST");
										}
										else if ( propertyInt == 1 )
										{
											strcpy(tmpStr, "SOUTH");
										}
										else if ( propertyInt == 2 )
										{
											strcpy(tmpStr, "WEST");
										}
										else if ( propertyInt == 3 )
										{
											strcpy(tmpStr, "NORTH");
										}
										printTextFormattedColor(font8x8_bmp, pad_x3, pad_y2, color, tmpStr);
									}
								} 
								else if ( i == 1 )
								{
									if ( propertyInt > 8 || propertyInt < 0 )
									{
										errorMessage = 60;
										errorArr[i] = 1;
										snprintf(spriteProperties[i], sizeof(spriteProperties[i]), "%d", 0); //reset
									}
								}
								else if ( i == 2 )
								{
									if ( propertyInt > 100 || propertyInt < -1 )
									{
										errorMessage = 60;
										errorArr[i] = 1;
										snprintf(spriteProperties[i], sizeof(spriteProperties[i]), "%d", -1); //reset
									}
									else if ( propertyInt == -1 )
									{
										printTextFormattedColor(font8x8_bmp, pad_x3, pad_y2, colorRandom, "Default 10%");
									}
									else
									{
										color = makeColorRGB(0, 255, 0);
										char tmpStr[32] = "";
										strcpy(tmpStr, spriteProperties[i]); //reset
										strcat(tmpStr, " %%");
										printTextFormattedColor(font8x8_bmp, pad_x3, pad_y2, color, tmpStr);
									}
								}
								else if ( i == 3 )
								{
									if ( propertyInt > 100 || propertyInt < -1 )
									{
										errorMessage = 60;
										errorArr[i] = 1;
										snprintf(spriteProperties[i], sizeof(spriteProperties[i]), "%d", -1); //reset
									}
									else if ( propertyInt == -1 )
									{
										printTextFormattedColor(font8x8_bmp, pad_x3, pad_y2, colorRandom, "Default Rand %");
									}
									else
									{
										color = makeColorRGB(0, 255, 0);
										char tmpStr[32] = "";
										strcpy(tmpStr, spriteProperties[i]); //reset
										strcat(tmpStr, " %%");
										printTextFormattedColor(font8x8_bmp, pad_x3, pad_y2, color, tmpStr);
									}
								}
							}

							if ( errorMessage )
							{
								color = makeColorRGB(255, 0, 0);
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, pad_x3, pad_y2, color, "Invalid ID!");
								}
							}

							pad_x1 = subx1 + 8;
						}

						pad_y1 = suby1 + 28 + 4 * spacing + 18;
						pad_x1 += 18;
						spacing = 18;
						//printText(font8x8_bmp, pad_x1 + 16, pad_y1, "Chest Facing");
						pad_y1 = suby1 + 28 + 9 * spacing;
						printText(font8x8_bmp, pad_x1 + 32, pad_y1, "NORTH(3)");
						pad_y1 = suby1 + 28 + 10 * spacing;
						printText(font8x8_bmp, pad_x1, pad_y1, "WEST(2)");
						printText(font8x8_bmp, pad_x1 + 96 - 16, pad_y1, "EAST(0)");
						pad_y1 = suby1 + 28 + 11 * spacing;
						printText(font8x8_bmp, pad_x1 + 32, pad_y1, "SOUTH(1)");

						spacing = 14;
						pad_y1 = suby1 + 14 + 14 * spacing;
						pad_x1 = subx1 + 8 + 192;
						pad_y1 = suby1 + 4 + 1 * spacing;
						printText(font8x8_bmp, pad_x1, pad_y1, "Chest Types");
						pad_y1 += spacing;
						printText(font8x8_bmp, pad_x1, pad_y1, "0 - Random");
						pad_y1 += spacing;
						printText(font8x8_bmp, pad_x1, pad_y1, "1 - Garbage");
						pad_y1 += spacing;
						printText(font8x8_bmp, pad_x1, pad_y1, "2 - Food");
						pad_y1 += spacing;
						printText(font8x8_bmp, pad_x1, pad_y1, "3 - Jewelry");
						pad_y1 += spacing;
						printText(font8x8_bmp, pad_x1, pad_y1, "4 - Equipment");
						pad_y1 += spacing;
						printText(font8x8_bmp, pad_x1, pad_y1, "5 - Tools");
						pad_y1 += spacing;
						printText(font8x8_bmp, pad_x1, pad_y1, "6 - Magical");
						pad_y1 += spacing;
						printText(font8x8_bmp, pad_x1, pad_y1, "7 - Potions");
						pad_y1 += spacing;
						printText(font8x8_bmp, pad_x1, pad_y1, "8 - Empty");

						pad_x1 = subx1 + 8;
						pad_y1 = suby1 + 28;
						spacing = 36;

						// Cycle properties with TAB.
						if ( keystatus[SDLK_TAB] )
						{
							keystatus[SDLK_TAB] = 0;
							cursorflash = ticks;
							editproperty++;
							if ( editproperty == numProperties )
							{
								editproperty = 0;
							}

							inputstr = spriteProperties[editproperty];
						}

						// select a textbox
						if ( mousestatus[SDL_BUTTON_LEFT] )
						{
							for ( int i = 0; i < numProperties; i++ )
							{
								if ( omousex >= pad_x1 - 4 && omousey >= suby1 + 40 + i * spacing && omousex < pad_x1 - 4 + pad_x2 && omousey < suby1 + 56 + i * spacing )
								{
									inputstr = spriteProperties[i];
									editproperty = i;
									cursorflash = ticks;
								}
								pad_x1 = subx1 + 8;
							}
						}
						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}
							if ( editproperty == 2 || editproperty == 3 )
							{
								inputlen = 3;
							}
							else
							{
								inputlen = 1;
							}
							if ( (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
							{
								printText(font8x8_bmp, subx1 + 8 + strlen(spriteProperties[editproperty]) * 8, suby1 + 44 + editproperty * spacing, "\26");
							}
						}
					}
				}
				else if ( newwindow == 4 || newwindow == 5 )
				{
					if ( newwindow == 5
						&& (itemSlotSelected < 0 || itemSlotSelected >= 16) )
					{
						itemSlotSelected = -1;
						buttonCloseSpriteSubwindow(nullptr);
					}
					else if ( selectedEntity[0] != NULL )
					{
						int numProperties;

						if ( newwindow == 4 )
						{
							numProperties = sizeof(itemPropertyNames) / sizeof(itemPropertyNames[0]); //find number of entries in property list
						}
						else if ( newwindow == 5 )
						{
							numProperties = sizeof(monsterItemPropertyNames) / sizeof(monsterItemPropertyNames[0]); //find number of entries in property list
						}
						/*
						 * Some monster inventory property labels are longer than 31
						 * characters. The previous 32-byte temporary buffer overflowed
						 * when copying "Category: (0-16, if default_random)", causing
						 * glibc's fortified strcpy check to abort the editor.
						 */
						const int lenProperties = 64;

						int spacing = 36; // 36 px between each item in the list.
						int verticalOffset = 0;
						int pad_x1 = subx1 + 8 + 96 + 80; // 104 px spacing from subwindow start. handles right side item list
						int pad_y1 = suby1 + 40 + verticalOffset; // right-side item list starts directly under its heading.
						int pad_x2 = 64; // handles right side item list

						int pad_y2 = (suby2 - 52 - 36) + verticalOffset - 4; //handles right side item list
						int pad_x3 = subx1 + 8; //handles left side menu
						int pad_y3 = suby1 + 28; // 28 px spacing from subwindow start, handles left side menu
						int pad_x4 = 64; //handles left side menu-end
						int pad_y4; //handles left side menu-end
						const int totalNumItems =
							static_cast<int>(sizeof(itemNameStrings) / sizeof(itemNameStrings[0]));
						int editorNumItems = totalNumItems;
						if ( newwindow == 5
							&& itemSlotSelected >= 0
							&& itemSlotSelected < 10 )
						{
							const int typedCapacity =
								static_cast<int>(sizeof(itemStringsByType[itemSlotSelected])
								/ sizeof(itemStringsByType[itemSlotSelected][0]));
							editorNumItems = 0;
							while ( editorNumItems < typedCapacity
								&& itemStringsByType[itemSlotSelected][editorNumItems][0] != '\0' )
							{
								++editorNumItems;
							}
						}
						editorNumItems = std::max(1, editorNumItems);
						const std::string itemSearchKey = std::to_string(newwindow)
							+ ":" + std::to_string(itemSlotSelected)
							+ ":" + editorMonsterItemSearch;
						if ( itemSearchKey != editorMonsterItemSearchLastKey )
						{
							editorMonsterItemSearchLastKey = itemSearchKey;
							const std::string loweredFilter = editorPaletteLowercase(editorMonsterItemSearch);
							if ( !loweredFilter.empty() )
							{
								for ( int searchIndex = 0; searchIndex < editorNumItems; ++searchIndex )
								{
									const char* searchName = nullptr;
									if ( newwindow == 5 && searchIndex == 1 )
									{
										searchName = "default_random";
									}
									else if ( newwindow == 5 && itemSlotSelected >= 0 && itemSlotSelected < 10 )
									{
										searchName = itemStringsByType[itemSlotSelected][searchIndex];
									}
									else
									{
										searchName = itemNameStrings[searchIndex];
									}
									const std::string loweredName = editorPaletteLowercase(searchName != nullptr ? searchName : "");
									if ( loweredName.find(loweredFilter) != std::string::npos
										|| std::to_string(searchIndex).find(loweredFilter) != std::string::npos )
									{
										itemSelect = searchIndex;
										break;
									}
								}
							}
						}
						auto safeGlobalItemIndex = [totalNumItems](int value)
						{
							return std::max(0, std::min(value, totalNumItems - 1));
						};
						int propertyInt = 0;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);
						for ( int i = 0; i < numProperties; i++ )
						{
							if ( newwindow == 4 )
							{
								snprintf(tmpPropertyName, sizeof(tmpPropertyName), "%s", itemPropertyNames[i]);

							}
							else if ( newwindow == 5 )
							{
								snprintf(tmpPropertyName, sizeof(tmpPropertyName), "%s", monsterItemPropertyNames[i]);
							}
							pad_y3 = suby1 + 40 + spacing + i * spacing;
							pad_y4 = suby1 + 44 + 12 + spacing + i * spacing;
							// box outlines then text
							drawDepressed(pad_x3 - 4, pad_y3, pad_x3 - 4 + pad_x2, pad_y4);
							// print values on top of boxes
							printText(font8x8_bmp, pad_x3, pad_y3 - 12, tmpPropertyName);
							printText(font8x8_bmp, pad_x3, pad_y3 + 4, spriteProperties[i]);

							propertyInt = atoi(spriteProperties[i]);

							if ( errorArr[i] != 1 )
							{
								if ( i == 0 )
								{
									Sint32 previousEditorItemValue = 0;
									const char* currentStableID = "";
									if ( newwindow == 4 && selectedEntity[0] )
									{
										previousEditorItemValue = selectedEntity[0]->skill[10];
										currentStableID =
											selectedEntity[0]->authoredItemStableID.c_str();
									}
									else if ( newwindow == 5 && selectedEntity[0]
										&& selectedEntity[0]->getStats() )
									{
										previousEditorItemValue = selectedEntity[0]->getStats()
											->EDITOR_ITEMS[itemSlotSelected * ITEM_SLOT_NUMPROPERTIES];
										currentStableID = editorGetMonsterSlotStableID(
											selectedEntity[0]->getStats(), itemSlotSelected);
									}
									const bool validSAMItemProperty =
										editorSAMItemPropertyValueIsValid(propertyInt,
											previousEditorItemValue, currentStableID);
									if ( newwindow == 4 )
									{
										if ( (propertyInt > totalNumItems - 2 || propertyInt < 0)
											&& !validSAMItemProperty )
										{
											errorMessage = 60;
											errorArr[i] = 1;
											if ( propertyInt < 1 )
											{
												snprintf(spriteProperties[i], sizeof(spriteProperties[i]), "%d", 1);
											}
											else
											{
												snprintf(spriteProperties[i], sizeof(spriteProperties[i]), "%d", editorNumItems - 2);
											}
										}
										else if ( propertyInt == 0 )
										{
											errorMessage = 60;
											errorArr[i] = 1;
										}
									}
									else if ( newwindow == 5 )
									{
										if ( (propertyInt > totalNumItems - 2 || propertyInt < 0)
											&& !validSAMItemProperty )
										{
											errorMessage = 60;
											errorArr[i] = 1;
											if ( propertyInt < 0 )
											{
												snprintf(spriteProperties[i], sizeof(spriteProperties[i]), "%d", 0);
											}
											else
											{
												snprintf(spriteProperties[i], sizeof(spriteProperties[i]), "%d", editorNumItems - 2);
											}
										}
									}
								}
								else if ( i == 1 )
								{
									if ( propertyInt == 2 )
									{
										color = makeColorRGB(200, 128, 0);
										printTextFormattedColor(font8x8_bmp, pad_x3 + pad_x4 + 8, pad_y3 + 4, color, "Decrepit");
									}
									else if ( propertyInt == 3 )
									{
										color = makeColorRGB(255, 255, 0);
										printTextFormattedColor(font8x8_bmp, pad_x3 + pad_x4 + 8, pad_y3 + 4, color, "Worn");
									}
									else if ( propertyInt == 4 )
									{
										color = makeColorRGB(128, 200, 0);
										printTextFormattedColor(font8x8_bmp, pad_x3 + pad_x4 + 8, pad_y3 + 4, color, "Servicable");
									}
									else if ( propertyInt == 5 )
									{
										color = makeColorRGB(0, 255, 0);
										printTextFormattedColor(font8x8_bmp, pad_x3 + pad_x4 + 8, pad_y3 + 4, color, "Excellent");
									}
									else if ( propertyInt == 1 )
									{
										color = makeColorRGB(255, 0, 0);
										printTextFormattedColor(font8x8_bmp, pad_x3 + pad_x4 + 8, pad_y3 + 4, color, "Broken");
									}
									else if ( propertyInt == 0 )
									{
										printTextFormattedColor(font8x8_bmp, pad_x3 + pad_x4 + 8, pad_y3 + 4, colorRandom, "Random");
									}
									else if ( propertyInt < 0 || propertyInt > 5 )
									{
										errorMessage = 60;
										errorArr[i] = 1;
										snprintf(spriteProperties[i], sizeof(spriteProperties[i]), "%d", 0); //reset
									}
								}
								else if ( i == 2 )
								{
									if ( strcmp(spriteProperties[i], "00") == 0 )
									{
										printTextFormattedColor(font8x8_bmp, pad_x3 + pad_x4 + 8, pad_y3 + 4, colorRandom, "Random");
									}
									else if ( propertyInt > 9 || propertyInt < -9 )
									{
										errorMessage = 60;
										errorArr[i] = 1;
										snprintf(spriteProperties[i], sizeof(spriteProperties[i]), "%d", 0); //reset
									}
								}
								else if ( i == 3 )
								{
									if ( propertyInt > 99 || propertyInt < 0 )
									{
										errorMessage = 60;
										errorArr[i] = 1;
										snprintf(spriteProperties[i], sizeof(spriteProperties[i]), "%d", 1); //reset
									}
									else if ( propertyInt == 0 )
									{
										color = makeColorRGB(255, 0, 0);
										printTextFormattedColor(font8x8_bmp, pad_x3 + pad_x4 + 8, pad_y3 + 4, color, "Invalid ID!");
									}
								}
								else if ( i == 4 )
								{
									if ( propertyInt == 1 )
									{
										color = makeColorRGB(0, 255, 0);
										printTextFormattedColor(font8x8_bmp, pad_x3 + pad_x4 + 8, pad_y3 + 4, color, "Identified");
									}
									else if ( propertyInt == 0 )
									{
										color = makeColorRGB(255, 255, 0);
										printTextFormattedColor(font8x8_bmp, pad_x3 + pad_x4 + 8, pad_y3 + 4, color, "Unidentified");
									}
									else if ( propertyInt == 2 )
									{
										printTextFormattedColor(font8x8_bmp, pad_x3 + pad_x4 + 8, pad_y3 + 4, colorRandom, "Random");
									}
									else
									{
										errorMessage = 60;
										errorArr[i] = 1;
										snprintf(spriteProperties[i], sizeof(spriteProperties[i]), "%d", 0);
									}
								}
								else if ( i == 5 && newwindow == 5)
								{
									if ( propertyInt > 100 || propertyInt < 0 )
									{
										errorMessage = 60;
										errorArr[i] = 1;
										snprintf(spriteProperties[i], sizeof(spriteProperties[i]), "%d", 100); //reset
									}
									else if ( propertyInt == 0 )
									{
										color = makeColorRGB(255, 0, 0);
										printTextFormattedColor(font8x8_bmp, pad_x3 + pad_x4 + 8, pad_y3 + 4, color, "Invalid ID!");
									}
									else
									{
										color = makeColorRGB(0, 255, 0);
										char tmpStr[32] = "";
										strcpy(tmpStr, spriteProperties[i]); //reset
										strcat(tmpStr, " %%");
										printTextFormattedColor(font8x8_bmp, pad_x3 + pad_x4 + 8, pad_y3 + 4, color, tmpStr);
									}
								}
								else if ( (i == 5 && newwindow == 4) || (i == 6 && newwindow == 5) )
								{
									if ( propertyInt > 16 || propertyInt < 0 )
									{
										errorMessage = 60;
										errorArr[i] = 1;
										snprintf(spriteProperties[i], sizeof(spriteProperties[i]), "%d", 0); //reset
									}
									else if ( propertyInt >= 0 && propertyInt <= (static_cast<int>(sizeof(itemCategoryNames[propertyInt])) / static_cast<int>(sizeof(itemCategoryNames[propertyInt][0]))) )
									{
										if ( propertyInt == 0 )
										{
											color = colorRandom;
										}
										else
										{
											color = makeColorRGB(200, 64, 220);
										}

										printTextFormattedColor(font8x8_bmp, pad_x3 + pad_x4 + 8, pad_y3 + 4, color, itemCategoryNames[propertyInt]);
									}
								}
							}

							if ( errorMessage )
							{
								color = makeColorRGB(255, 0, 0);
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, pad_x3 + pad_x4 + 8, pad_y3 + 4, color, "Invalid ID!");
								}
							}

						}



						drawDepressed(pad_x1, pad_y1, subx2 - 20, pad_y2);
						drawDepressed(subx2 - 20, pad_y1, subx2 - 4, pad_y2);
						const int visibleItemRows = std::max(1, (pad_y2 - (pad_y1 + 4)) / 8);
						slidersize = std::min<int>(((pad_y2 - 1) - (pad_y1 + 1)), ((pad_y2 - 1) - (pad_y1 + 1)) / ((real_t)(editorNumItems + 1) / visibleItemRows));
						slidery = std::min(std::max(pad_y1, slidery), pad_y2 - 1 - slidersize);
						drawWindowFancy(subx2 - 19, slidery, subx2 - 5, slidery + slidersize);

						// directory list offset from slider
						y2 = ((real_t)(slidery - (pad_y1)) / (pad_y2 - (pad_y1))) * editorNumItems;
						y2 = std::max(0, std::min(y2, editorNumItems - 1));
						itemSelect = std::max(0, std::min(itemSelect, editorNumItems - 1));
						if ( scroll )
						{
							slidery -= 8 * scroll;
							slidery = std::min(std::max(pad_y1, slidery), pad_y2 - 1 - slidersize);
							y2 = ((real_t)(slidery - (pad_y1)) / ((pad_y2) - (pad_y1))) * editorNumItems;
							y2 = std::max(0, std::min(y2, editorNumItems - 1));
							itemSelect = std::max(y2, std::min(itemSelect, std::min(editorNumItems - 1, y2 + visibleItemRows - 1)));
							scroll = 0;
						}
						if ( mousestatus[SDL_BUTTON_LEFT] && omousex >= subx2 - 20 && omousex < subx2 - 4 && omousey >= (pad_y1) && omousey < pad_y2 )
						{
							slidery = oslidery + mousey - omousey;
							slidery = std::min(std::max(pad_y1, slidery), pad_y2 - 1 - slidersize);
							y2 = ((real_t)(slidery - (pad_y1)) / ((pad_y2) - (pad_y1))) * editorNumItems;
							y2 = std::max(0, std::min(y2, editorNumItems - 1));
							mclick = 1;
							itemSelect = std::max(y2, std::min(itemSelect, std::min(editorNumItems - 1, y2 + visibleItemRows - 1)));
						}
						else
						{
							oslidery = slidery;
						}

						pos.x = pad_x1 ;
						pos.y = pad_y1 + 4 + (itemSelect - y2) * 8;
						pos.w = subx2 - pad_x1 - 24;
						pos.h = 8;
						drawRect(&pos, makeColorRGB(64, 64, 64), 255);

						// print all the items
						x = pad_x1;
						y = pad_y1 + 4;
						c = std::min(editorNumItems, y2 + visibleItemRows);
						for ( z = y2; z < c; z++ )
						{
							if ( newwindow == 5 && z == 1 )
							{
								printText(font8x8_bmp, x, y, "default_random");
							}
							else
							{
								itemSelect = std::max(0, std::min(itemSelect, editorNumItems - 1));
								switch ( itemSlotSelected )
								{
									case -1:
										printText(font8x8_bmp, x, y, itemNameStrings[z]);
										break;
									default:
										if ( itemSlotSelected < 10 )
										{
											printText(font8x8_bmp, x, y, itemStringsByType[itemSlotSelected][z]);
										}
										else
										{
											printText(font8x8_bmp, x, y, itemNameStrings[z]);
										}
										break;
								}
									
							}
							y += 8;
						}

						// Search field underneath the selectable item list.
						const int itemSearchY = pad_y2 + 16;
						printText(font8x8_bmp, pad_x1, itemSearchY, "Search:");
						drawDepressed(pad_x1 + 56, itemSearchY - 4, subx2 - 24, itemSearchY + 12);
						printText(font8x8_bmp, pad_x1 + 60, itemSearchY, editorMonsterItemSearch);
						if ( inputstr == editorMonsterItemSearch
							&& (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
						{
							printText(font8x8_bmp,
								pad_x1 + 60 + strlen(editorMonsterItemSearch) * 8,
								itemSearchY, "\26");
						}

						// item selection box
						pad_y3 = suby1 + 40;
						pad_y4 = suby1 + 44 + 12;
						// box outlines then text
						drawDepressed(pad_x3 - 4, pad_y3, pad_x3 - 4 + pad_x2 + 112, pad_y4);
						// print values on top of boxes
						printText(font8x8_bmp, pad_x3, pad_y3 - 12, "Item Name");
						printText(font8x8_bmp, pad_x1, pad_y1 - 12, "Click to select item");
						printText(font8x8_bmp, pad_x3, pad_y3 + 4, itemName);
                        {
                            const Sint32 displayedItemID = static_cast<Sint32>(strtoll(spriteProperties[0], nullptr, 10));
                            char samName[128] = "";
                            char samDetail[256] = "";
                            const char* slotStableID = newwindow == 4
                                ? (selectedEntity[0]
                                    ? selectedEntity[0]->authoredItemStableID.c_str() : "")
                                : editorGetMonsterSlotStableID(
                                    selectedEntity[0] == nullptr
                                        ? nullptr : selectedEntity[0]->getStats(),
                                    itemSlotSelected);
                            if ( editorDescribeSAMItem(displayedItemID, slotStableID,
                                samName, sizeof(samName), samDetail, sizeof(samDetail))
                                && samDetail[0] != '\0' )
                            {
                                printText(font8x8_bmp, pad_x3, pad_y3 + 20, samDetail);
                            }
                        }
						//drawDepressed(pad_x1, suby2 - 48, subx2 - 4, suby2 - 32);
						//printText(font8x8_bmp, pad_x1, suby2 - 44, itemName);

						// select a file
						if ( mousestatus[SDL_BUTTON_LEFT] )
						{
							if ( omousex >= pad_x1 + 56 && omousex < subx2 - 24
								&& omousey >= itemSearchY - 4 && omousey < itemSearchY + 12 )
							{
								if ( !SDL_IsTextInputActive() )
								{
									SDL_StartTextInput();
								}
								inputstr = editorMonsterItemSearch;
								inputlen = 127;
								editproperty = numProperties;
								cursorflash = ticks;
							}
							else if ( omousex >= pad_x1 && omousex < subx2 - 24 && omousey >= pad_y1 + 4 && omousey < pad_y2 - 4 )
							{
								itemSelect = y2 + ((omousey - (pad_y1 + 4)) >> 3);
								if ( newwindow == 4 )
								{
									itemSelect = std::max(y2, std::min(itemSelect, std::min(std::max(0, editorNumItems - 2), y2 + visibleItemRows - 1)));
								}
								else
								{
									itemSelect = std::max(y2, std::min(itemSelect, std::min(std::max(0, editorNumItems - 2), y2 + visibleItemRows - 1)));
								}
								switch ( itemSlotSelected )
								{
									case -1:
										strcpy(itemName, itemNameStrings[itemSelect]);
										break;
									default:
										if ( itemSlotSelected < 10 )
										{
											strcpy(itemName, itemStringsByType[itemSlotSelected][itemSelect]);
										}
										else
										{
											strcpy(itemName, itemNameStrings[itemSelect]);
										}
										break;
								}
								//inputstr = itemName;
								editorNumItems = (sizeof(itemNameStrings) / sizeof(itemNameStrings[0]));
								for ( z = 0; z < editorNumItems; z++ )
								{
									if ( strcmp(itemName, itemNameStrings[z]) == 0 )
									{
										char tmpStr[5];
										snprintf(tmpStr, sizeof(tmpStr), "%d", z);
										strcpy(spriteProperties[0], tmpStr);
										
										z = editorNumItems;
									}
								}
							}

							for ( int i = 0; i < numProperties; i++ )
							{
								if ( omousex >= pad_x3 - 4 && omousey >= suby1 + 40 + spacing + i * spacing && omousex < pad_x3 - 4 + pad_x4 && omousey < suby1 + 56 + spacing + i * spacing )
								{
									inputstr = spriteProperties[i];
									editproperty = i;
									cursorflash = ticks;
								}
							}
						}

						// Cycle properties with TAB.
						if ( keystatus[SDLK_TAB] )
						{
							keystatus[SDLK_TAB] = 0;
							cursorflash = ticks;
							if ( inputstr == editorMonsterItemSearch || editproperty >= numProperties )
							{
								editproperty = 0;
							}
							else
							{
								editproperty++;
								if ( editproperty == numProperties )
								{
									editproperty = 0;
								}
							}

							inputstr = spriteProperties[editproperty];
						}
						
						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}

							if ( editproperty == 0 )
							{
                                inputlen = 10;
								//update the item name when the ID changes.
                                const Sint32 displayedItemID = static_cast<Sint32>(strtoll(spriteProperties[0], nullptr, 10));
                                char samName[128] = "";
                                char samDetail[256] = "";
                                const char* slotStableID = newwindow == 4
                                    ? (selectedEntity[0]
                                        ? selectedEntity[0]->authoredItemStableID.c_str() : "")
                                    : editorGetMonsterSlotStableID(
                                        selectedEntity[0] == nullptr
                                            ? nullptr : selectedEntity[0]->getStats(),
                                        itemSlotSelected);
                                if ( editorDescribeSAMItem(displayedItemID, slotStableID,
                                    samName, sizeof(samName), samDetail, sizeof(samDetail)) )
                                {
                                    snprintf(itemName, sizeof(itemName), "%s", samName);
                                }
                                else if ( newwindow == 5 && displayedItemID == 1 )
                                {
                                    strcpy(itemName, "default_random");
                                }
                                else
                                {
                                    strcpy(itemName, itemNameStrings[safeGlobalItemIndex(displayedItemID)]);
                                }
							}
							else if( editproperty == 2 || editproperty == 3 )
							{
								inputlen = 2;
							}
							else if ( editproperty == 5 )
							{
								inputlen = 3;
							}
							else if ( editproperty == 6 && newwindow == 5 )
							{
								inputlen = 3;
							}
							else
							{
								inputlen = 1;
							}

							if ( (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
							{
								printText(font8x8_bmp, pad_x3 + strlen(spriteProperties[editproperty]) * 8, suby1 + 44 + spacing + editproperty * spacing, "\26");
							}
						}
						
					}
				}
				else if ( newwindow == 6 )
				{
					if ( selectedEntity[0] != NULL )
					{
						int numProperties = sizeof(summonTrapPropertyNames) / sizeof(summonTrapPropertyNames[0]); //find number of entries in property list
						const int lenProperties = sizeof(summonTrapPropertyNames[0]) / sizeof(char); //find length of entry in property list
						int spacing = 36; // 36 px between each item in the list.
						int pad_y1 = suby1 + 28; // 28 px spacing from subwindow start.
						int pad_x1 = subx1 + 8; // 8px spacing from subwindow start.
						int pad_x2 = 64;
						int pad_x3 = pad_x1 + pad_x2 + 8;
						int pad_y2 = 0;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 color2 = makeColorRGB(255, 255, 0);
						Uint32 colorBad = makeColorRGB(255, 0, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);

						for ( int i = 0; i < numProperties; i++ )
						{
							int propertyInt = atoi(spriteProperties[i]);

							strcpy(tmpPropertyName, summonTrapPropertyNames[i]);
							pad_y1 = suby1 + 28 + i * spacing;
							pad_y2 = suby1 + 44 + i * spacing;
							// box outlines then text
							drawDepressed(pad_x1 - 4, suby1 + 40 + i * spacing, pad_x1 - 4 + pad_x2, suby1 + 56 + i * spacing);
							// print values on top of boxes
							printText(font8x8_bmp, pad_x1, suby1 + 44 + i * spacing, spriteProperties[i]);
							printText(font8x8_bmp, pad_x1, pad_y1, tmpPropertyName);

							if ( errorArr[i] != 1 )
							{
								if ( i == 0 ) //check input for valid entries, correct or notify the user if out of bounds.
								{
									if ( propertyInt > 32 || propertyInt < -1 )
									{
										errorMessage = 60;
										errorArr[i] = 1;
										snprintf(spriteProperties[i], sizeof(spriteProperties[i]), "%d", 0); //reset
									}
									else if ( propertyInt == 0 )
									{
										printTextFormattedColor(font8x8_bmp, pad_x3, pad_y2, colorRandom, "Random monster to match level curve");
									}
									else if ( propertyInt == -1 )
									{
										printTextFormattedColor(font8x8_bmp, pad_x3, pad_y2, colorRandom, "Completely random monster");
									}
									else if ( propertyInt == 6 || propertyInt == 12 )
									{
										printTextFormattedColor(font8x8_bmp, pad_x3, pad_y2, colorBad, "Error: Unused monster ID, will reset to 0");
									}
									else
									{
										color = makeColorRGB(0, 255, 0);
										char tmpStr[32] = "";
										strcpy(tmpStr, monsterEditorNameStrings[atoi(spriteProperties[i])]);
										printTextFormattedColor(font8x8_bmp, pad_x3, pad_y2, color, tmpStr);
									}
								}
								else if ( i == 1 )
								{
									if ( propertyInt > 9 || propertyInt < 0 )
									{
										errorMessage = 60;
										errorArr[i] = 1;
										snprintf(spriteProperties[i], sizeof(spriteProperties[i]), "%d", 1); //reset
									}
									else if ( propertyInt == 0 )
									{
										printTextFormattedColor(font8x8_bmp, pad_x3, pad_y2, colorBad, "Error: Must be > 0");
									}
								}
								else if ( i == 2 )
								{
									if ( propertyInt > 999 || propertyInt < 0 )
									{
										errorMessage = 60;
										errorArr[i] = 1;
										snprintf(spriteProperties[i], sizeof(spriteProperties[i]), "%d", 1); //reset
									}
									else if ( propertyInt == 0 )
									{
										printTextFormattedColor(font8x8_bmp, pad_x3, pad_y2, colorBad, "Error: Must be > 0");
									}
									else
									{
										printTextFormattedColor(font8x8_bmp, pad_x3, pad_y2, color, "%d seconds", atoi(spriteProperties[i]));
									}
								}
								else if ( i == 3 )
								{
									if ( propertyInt > 99 || propertyInt < 0 )
									{
										errorMessage = 60;
										errorArr[i] = 1;
										snprintf(spriteProperties[i], sizeof(spriteProperties[i]), "%d", 1); //reset
									}
									else if ( propertyInt == 0 )
									{
										printTextFormattedColor(font8x8_bmp, pad_x3, pad_y2, colorBad, "Error: Must be > 0");
									}
									else
									{
										printTextFormattedColor(font8x8_bmp, pad_x3, pad_y2, color, "%d instances", atoi(spriteProperties[i]));
									}
								}
								else if ( i == 4 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										errorMessage = 60;
										errorArr[i] = 1;
										snprintf(spriteProperties[i], sizeof(spriteProperties[i]), "%d", 0); //reset
									}
									else if ( propertyInt == 0 )
									{
										printTextFormattedColor(font8x8_bmp, pad_x3, pad_y2, color2, "No - power to enable");
									}
									else if ( propertyInt == 1 )
									{
										printTextFormattedColor(font8x8_bmp, pad_x3, pad_y2, color, "Yes - power to disable");
									}
								}
								else if ( i == 5 )
								{
									if ( propertyInt > 100 || propertyInt < 0 )
									{
										errorMessage = 60;
										errorArr[i] = 1;
										snprintf(spriteProperties[i], sizeof(spriteProperties[i]), "%d", 0); //reset
									}
									else
									{
										color = makeColorRGB(0, 255, 0);
										char tmpStr[32] = "";
										strcpy(tmpStr, spriteProperties[i]); //reset
										strcat(tmpStr, " %%");
										printTextFormatted(font8x8_bmp, pad_x3, pad_y2, tmpStr);
									}
								}
								else if ( i == 6 )
								{
									if ( propertyInt > 16 || propertyInt < 0 )
									{
										errorMessage = 60;
										errorArr[i] = 1;
										snprintf(spriteProperties[i], sizeof(spriteProperties[i]), "%d", 0); //reset
									}
									else if ( propertyInt == 0 )
									{
										printTextFormattedColor(font8x8_bmp, pad_x3, pad_y2, color2, "");
									}
									else if ( propertyInt > 0 )
									{
										printTextFormattedColor(font8x8_bmp, pad_x3, pad_y2, color, "Auto activate near player (x tiles)");
									}
								}
							}

							if ( errorMessage )
							{
								color = makeColorRGB(255, 0, 0);
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, pad_x3, pad_y2, color, "Invalid ID!");
								}
							}

							pad_x1 = subx1 + 8;
						}
						// Cycle properties with TAB.
						if ( keystatus[SDLK_TAB] )
						{
							keystatus[SDLK_TAB] = 0;
							cursorflash = ticks;
							editproperty++;
							if ( editproperty == numProperties )
							{
								editproperty = 0;
							}

							inputstr = spriteProperties[editproperty];
						}

						// select a textbox
						if ( mousestatus[SDL_BUTTON_LEFT] )
						{
							for ( int i = 0; i < numProperties; i++ )
							{
								if ( omousex >= pad_x1 - 4 && omousey >= suby1 + 40 + i * spacing && omousex < pad_x1 - 4 + pad_x2 && omousey < suby1 + 56 + i * spacing )
								{
									inputstr = spriteProperties[i];
									editproperty = i;
									cursorflash = ticks;
								}
								pad_x1 = subx1 + 8;
							}
						}
						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}
							if ( editproperty == 0 || editproperty == 3) //length of text field allowed to enter
							{
								inputlen = 2;
							}
							else if ( editproperty == 2 || editproperty == 5 || editproperty == 6 )
							{
								inputlen = 3;
							}
							else
							{
								inputlen = 1;
							}
							if ( (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
							{
								printText(font8x8_bmp, subx1 + 8 + strlen(spriteProperties[editproperty]) * 8, suby1 + 44 + editproperty * spacing, "\26");
							}
						}
					}
				}
				else if ( newwindow == 7 )
				{
					if ( selectedEntity[0] != NULL )
					{
						int numProperties = sizeof(powerCrystalPropertyNames) / sizeof(powerCrystalPropertyNames[0]); //find number of entries in property list
						const int lenProperties = sizeof(powerCrystalPropertyNames[0]) / sizeof(char); //find length of entry in property list
						int spacing = 36; // 36 px between each item in the list.
						int pad_y1 = suby1 + 28; // 28 px spacing from subwindow start.
						int pad_x1 = subx1 + 8; // 8px spacing from subwindow start.
						int pad_x2 = 64;
						int pad_x3 = pad_x1 + pad_x2 + 8;
						int pad_y2 = 0;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);

						for ( int i = 0; i < numProperties; i++ )
						{
							int propertyInt = atoi(spriteProperties[i]);

							strcpy(tmpPropertyName, powerCrystalPropertyNames[i]);
							pad_y1 = suby1 + 28 + i * spacing;
							pad_y2 = suby1 + 44 + i * spacing;
							// box outlines then text
							drawDepressed(pad_x1 - 4, suby1 + 40 + i * spacing, pad_x1 - 4 + pad_x2, suby1 + 56 + i * spacing);
							// print values on top of boxes
							printText(font8x8_bmp, pad_x1, suby1 + 44 + i * spacing, spriteProperties[i]);
							printText(font8x8_bmp, pad_x1, pad_y1, tmpPropertyName);

							if ( errorArr[i] != 1 )
							{
								if ( i == 0 )
								{
									if ( propertyInt > 3 || propertyInt < 0 )
									{
										errorMessage = 60;
										errorArr[i] = 1;
										snprintf(spriteProperties[i], sizeof(spriteProperties[i]), "%d", 0); //reset
									}
									else
									{
										color = makeColorRGB(0, 255, 0);
										char tmpStr[32] = "";
										if ( propertyInt == 0 )
										{
											strcpy(tmpStr, "EAST");
										}
										else if ( propertyInt == 1 )
										{
											strcpy(tmpStr, "SOUTH");
										}
										else if ( propertyInt == 2 )
										{
											strcpy(tmpStr, "WEST");
										}
										else if ( propertyInt == 3 )
										{
											strcpy(tmpStr, "NORTH");
										}
										printTextFormattedColor(font8x8_bmp, pad_x3, pad_y2, color, tmpStr);
									}
								}
								else if ( i == 1 )
								{
									if ( propertyInt > 99 || propertyInt < 0 )
									{
										errorMessage = 60;
										errorArr[i] = 1;
										snprintf(spriteProperties[i], sizeof(spriteProperties[i]), "%d", 1); //reset
									}
									else
									{
										color = makeColorRGB(0, 255, 0);
										// 32 is WAY TOO SMALL for this, wtf?
										// spriteProperties is a string table, each entry 128 bytes long
										// " Tiles to power in facing direction" = 35 bytes
										// do not forget the null terminator
										char tmpStr[128 + 35 + 1] = "";
										strcpy(tmpStr, spriteProperties[i]); //reset
										strcat(tmpStr, " Tiles to power in facing direction");
										printTextFormattedColor(font8x8_bmp, pad_x3, pad_y2, color, tmpStr);
									}
								}
								else if ( i == 2 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										errorMessage = 60;
										errorArr[i] = 1;
										snprintf(spriteProperties[i], sizeof(spriteProperties[i]), "%d", 0); //reset
									}
									else if ( propertyInt == 0 )
									{
										printTextFormattedColor(font8x8_bmp, pad_x3, pad_y2, colorRandom, "Clockwise");
									}
									else if ( propertyInt == 1 )
									{
										printTextFormattedColor(font8x8_bmp, pad_x3, pad_y2, color, "Counter-Clockwise");
									}
								}
								else if ( i == 3 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										errorMessage = 60;
										errorArr[i] = 1;
										snprintf(spriteProperties[i], sizeof(spriteProperties[i]), "%d", 0); //reset
									}
									else if ( propertyInt == 0 )
									{
										printTextFormattedColor(font8x8_bmp, pad_x3, pad_y2, colorRandom, "Always on");
									}
									else if ( propertyInt == 1 )
									{
										printTextFormattedColor(font8x8_bmp, pad_x3, pad_y2, color, "Requires spell to activate");
									}
								}
								else if ( i == 4 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										errorMessage = 60;
										errorArr[i] = 1;

										snprintf(
											spriteProperties[i],
											sizeof(spriteProperties[i]),
											"%d",
											0
										);
									}
									else if ( propertyInt == 0 )
									{
										printTextFormattedColor(
											font8x8_bmp,
											pad_x3,
											pad_y2,
											colorRandom,
											"Does not require circuit power"
										);
									}
									else
									{
										printTextFormattedColor(
											font8x8_bmp,
											pad_x3,
											pad_y2,
											color,
											"Activates when powered"
										);
									}
								}
							}

							if ( errorMessage )
							{
								color = makeColorRGB(255, 0, 0);
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, pad_x3, pad_y2, color, "Invalid ID!");
								}
							}

							pad_x1 = subx1 + 8;
						}

						// Cycle properties with TAB.
						if ( keystatus[SDLK_TAB] )
						{
							keystatus[SDLK_TAB] = 0;
							cursorflash = ticks;
							editproperty++;
							if ( editproperty == numProperties )
							{
								editproperty = 0;
							}

							inputstr = spriteProperties[editproperty];
						}

						// select a textbox
						if ( mousestatus[SDL_BUTTON_LEFT] )
						{
							for ( int i = 0; i < numProperties; i++ )
							{
								pad_x1 = subx1 + 8;
								if ( omousex >= pad_x1 - 4 && omousey >= suby1 + 40 + i * spacing && omousex < pad_x1 - 4 + pad_x2 && omousey < suby1 + 56 + i * spacing )
								{
									inputstr = spriteProperties[i];
									editproperty = i;
									cursorflash = ticks;
								}
							}
						}
						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}
							if ( editproperty == 1 )
							{
								inputlen = 2;
							}
							else
							{
								inputlen = 1;
							}
							if ( (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
							{
								printText(font8x8_bmp, subx1 + 8 + strlen(spriteProperties[editproperty]) * 8, suby1 + 44 + editproperty * spacing, "\26");
							}
						}
					}
				}
				else if ( newwindow == 8 )
				{
					if ( selectedEntity[0] != NULL )
					{
						int numProperties = sizeof(leverTimerPropertyNames) / sizeof(leverTimerPropertyNames[0]); //find number of entries in property list
						const int lenProperties = sizeof(leverTimerPropertyNames[0]) / sizeof(char); //find length of entry in property list
						int spacing = 36; // 36 px between each item in the list.
						int inputFieldHeader_y = suby1 + 28; // 28 px spacing from subwindow start.
						int inputField_x = subx1 + 8; // 8px spacing from subwindow start.
						int inputField_y = inputFieldHeader_y + 16;
						int inputFieldWidth = 64; // width of the text field
						int inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);
						Uint32 colorError = makeColorRGB(255, 0, 0);

						for ( int i = 0; i < numProperties; i++ )
						{
							int propertyInt = atoi(spriteProperties[i]);

							strcpy(tmpPropertyName, leverTimerPropertyNames[i]);
							inputFieldHeader_y = suby1 + 28 + i * spacing;
							inputField_y = inputFieldHeader_y + 16;
							// box outlines then text
							drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
							// print values on top of boxes
							printText(font8x8_bmp, inputField_x, suby1 + 44 + i * spacing, spriteProperties[i]);
							printText(font8x8_bmp, inputField_x, inputFieldHeader_y, tmpPropertyName);

							if ( errorArr[i] != 1 )
							{
								if ( i == 0 )
								{
									if ( propertyInt > 999 || propertyInt < 0 )
									{
										propertyPageError(i, 5); // reset to default 5 seconds.
									}
									else
									{
										char tmpStr[32] = "";
										if ( propertyInt == 0 )
										{
											strcpy(tmpStr, "Value must be > 0!");
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, tmpStr);
										}
										else
										{
											if ( propertyInt == 1 )
											{
												strcpy(tmpStr, "second");
											}
											else
											{
												strcpy(tmpStr, "seconds");
											}
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
										}
									}
								}
								else
								{
									// enter other row entries here
								}
							}

							if ( errorMessage )
							{
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, "Invalid ID!");
								}
							}
						}

						propertyPageTextAndInput(numProperties, inputFieldWidth);

						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}

							// set the maximum length allowed for user input
							if ( editproperty == 0 )
							{
								inputlen = 4;
							}
							else
							{
								inputlen = 4;
							}
							propertyPageCursorFlash(spacing);
						}
					}
				}
				else if ( newwindow == 9 )
				{
					if ( selectedEntity[0] != NULL )
					{
						int numProperties = sizeof(boulderTrapPropertyNames) / sizeof(boulderTrapPropertyNames[0]); //find number of entries in property list
						const int lenProperties = sizeof(boulderTrapPropertyNames[0]) / sizeof(char); //find length of entry in property list
						int spacing = 36; // 36 px between each item in the list.
						int inputFieldHeader_y = suby1 + 28; // 28 px spacing from subwindow start.
						int inputField_x = subx1 + 8; // 8px spacing from subwindow start.
						int inputField_y = inputFieldHeader_y + 16;
						int inputFieldWidth = 64; // width of the text field
						int inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);
						Uint32 colorError = makeColorRGB(255, 0, 0);

						for ( int i = 0; i < numProperties; i++ )
						{
							int propertyInt = atoi(spriteProperties[i]);

							strcpy(tmpPropertyName, boulderTrapPropertyNames[i]);
							inputFieldHeader_y = suby1 + 28 + i * spacing;
							inputField_y = inputFieldHeader_y + 16;
							// box outlines then text
							drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
							// print values on top of boxes
							printText(font8x8_bmp, inputField_x, suby1 + 44 + i * spacing, spriteProperties[i]);
							printText(font8x8_bmp, inputField_x, inputFieldHeader_y, tmpPropertyName);

							if ( errorArr[i] != 1 )
							{
								if ( i == 0 )
								{
									if ( propertyInt > 99 || propertyInt < -1 )
									{
										propertyPageError(i, 0); // reset to default 0 re-fire.
									}
									else
									{
										char tmpStr[32] = "";
										if ( propertyInt == 1 )
										{
											strcpy(tmpStr, "time");
										}
										else if ( propertyInt == -1 )
										{
											strcpy(tmpStr, "infinite reload");
										}
										else
										{
											strcpy(tmpStr, "times");
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else if ( i == 1 )
								{
									if ( propertyInt > 999 || propertyInt < 0 )
									{
										propertyPageError(i, 1); // reset to default 1 seconds.
									}
									else
									{
										char tmpStr[32] = "";
										if ( propertyInt < 2 )
										{
											strcpy(tmpStr, "Value must be > 1!");
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, tmpStr);
										}
										else
										{
											if ( propertyInt == 1 )
											{
												strcpy(tmpStr, "second");
											}
											else
											{
												strcpy(tmpStr, "seconds");
											}
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
										}
									}
								}
								else if ( i == 2 )
								{
									if ( propertyInt > 999 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 1 seconds.
									}
									else
									{
										char tmpStr[32] = "";
										if ( propertyInt == 1 )
										{
											strcpy(tmpStr, "second");
										}
										else
										{
											strcpy(tmpStr, "seconds");
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else
								{
									// enter other row entries here
								}
							}

							if ( errorMessage )
							{
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, "Invalid ID!");
								}
							}
						}

						propertyPageTextAndInput(numProperties, inputFieldWidth);

						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}

							// set the maximum length allowed for user input
							if ( editproperty == 0 )
							{
								inputlen = 4;
							}
							else
							{
								inputlen = 4;
							}
							propertyPageCursorFlash(spacing);
						}
					}
				}
				else if ( newwindow == 39 )
				{
                    Stat* stats = selectedEntity[0] ? selectedEntity[0]->getStats() : nullptr;
                    monsterEffectsUpdateSelectionFromFields();
                    printText(font8x8_bmp, subx1 + 16, suby1 + 28, "Choose an effect below, then click Add Selected Effect.");
                    printText(font8x8_bmp, subx1 + 16, suby1 + 40, "Type in Search to filter the effect list, then click a result.");
                    printText(font8x8_bmp, subx1 + 16, suby1 + 54, "Search:");
                    drawDepressed(subx1 + 72, suby1 + 50, subx1 + 286, suby1 + 66);
                    printText(font8x8_bmp, subx1 + 76, suby1 + 54, monsterEffectSearchText);
                    printText(font8x8_bmp, subx1 + 300, suby1 + 54, "Effect ID:");
                    drawDepressed(subx1 + 380, suby1 + 50, subx1 + 428, suby1 + 66);
                    printText(font8x8_bmp, subx1 + 384, suby1 + 54, monsterEffectIdText);

                    const bool showEffectDropdown = inputstr == monsterEffectSearchText;
                    monsterEffectsSetRowsVisible(!showEffectDropdown);
                    std::vector<int> effectSearchMatches;
                    const int effectDropdownVisibleRows = 8;
                    std::string effectQuery = monsterEffectSearchText;
                    std::transform(effectQuery.begin(), effectQuery.end(), effectQuery.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if ( showEffectDropdown )
                    {
                        for ( int effect = 0; effect < 135; ++effect )
                        {
                            if ( !monsterEffectCanSelect(effect) )
                            {
                                continue;
                            }
                            std::string effectName = monsterEffectDisplayName(effect);
                            std::transform(effectName.begin(), effectName.end(), effectName.begin(),
                                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                            if ( effectQuery.empty() || effectName.find(effectQuery) != std::string::npos
                                || std::to_string(effect).find(effectQuery) != std::string::npos )
                            {
                                effectSearchMatches.push_back(effect);
                            }
                        }
                        if ( effectQuery != monsterEffectDropdownLastQuery )
                        {
                            monsterEffectDropdownLastQuery = effectQuery;
                            monsterEffectDropdownScroll = 0;
                        }
                        const int maxDropdownScroll = std::max(0,
                            static_cast<int>(effectSearchMatches.size()) - effectDropdownVisibleRows);
                        monsterEffectDropdownScroll = std::max(0,
                            std::min(monsterEffectDropdownScroll, maxDropdownScroll));
                        const int dropdownX1 = subx1 + 72;
                        const int dropdownX2 = subx1 + 286;
                        const int dropdownY1 = suby1 + 68;
                        const int dropdownY2 = dropdownY1 + effectDropdownVisibleRows * 16;
                        const bool mouseOverDropdown = omousex >= dropdownX1 && omousex < dropdownX2
                            && omousey >= dropdownY1 && omousey < dropdownY2;
                        if ( mouseOverDropdown && scroll != 0 )
                        {
                            monsterEffectDropdownScroll += scroll;
                            monsterEffectDropdownScroll = std::max(0,
                                std::min(monsterEffectDropdownScroll, maxDropdownScroll));
                            scroll = 0;
                        }
                    }

                    printTextFormatted(font8x8_bmp, subx1 + 16, suby1 + 70, "Selected: %s (ID %d)", monsterEffectDisplayName(monsterEffectSelectedId), monsterEffectSelectedId);
                    if ( monsterEffectsShowAll )
                    {
                        const Uint32 cautionColor = makeColorRGB(255, 192, 64);
                        printTextFormattedColor(font8x8_bmp, subx1 + 172, suby2 - 42, cautionColor,
                            "CAUTION: Not all effects are safe.");
                    }
                    printText(font8x8_bmp, subx1 + 288, suby1 + 88, "Strength");
                    printText(font8x8_bmp, subx1 + 390, suby1 + 88, "Duration");
                    if ( mousestatus[SDL_BUTTON_LEFT] )
                    {
                        const int dropdownX1 = subx1 + 72;
                        const int dropdownX2 = subx1 + 286;
                        const int dropdownY1 = suby1 + 68;
                        const int dropdownRowHeight = 16;
                        const int dropdownY2 = dropdownY1
                            + effectDropdownVisibleRows * dropdownRowHeight;
                        const bool clickInsideSearch = omousex >= subx1 + 72 && omousex < subx1 + 286
                            && omousey >= suby1 + 50 && omousey < suby1 + 66;
                        const bool clickInsideDropdown = showEffectDropdown
                            && omousex >= dropdownX1 && omousex < dropdownX2
                            && omousey >= dropdownY1 && omousey < dropdownY2;
                        if ( showEffectDropdown && !clickInsideSearch && !clickInsideDropdown )
                        {
                            inputstr = monsterEffectIdText;
                            inputlen = 3;
                            editproperty = 1001;
                        }
                        if ( showEffectDropdown )
                        {
                            const int dropdownX1 = subx1 + 72;
                            const int dropdownX2 = subx1 + 286;
                            const int dropdownY1 = suby1 + 68;
                            const int dropdownRowHeight = 16;
                            const int visibleMatchCount = std::min(effectDropdownVisibleRows,
                                static_cast<int>(effectSearchMatches.size()) - monsterEffectDropdownScroll);
                            for ( int matchRow = 0; matchRow < visibleMatchCount; ++matchRow )
                            {
                                const int rowY = dropdownY1 + matchRow * dropdownRowHeight;
                                if ( omousex >= dropdownX1 && omousex < dropdownX2
                                    && omousey >= rowY && omousey < rowY + dropdownRowHeight )
                                {
                                    monsterEffectSelectedId = effectSearchMatches[monsterEffectDropdownScroll + matchRow];
                                    snprintf(monsterEffectIdText, sizeof(monsterEffectIdText), "%d", monsterEffectSelectedId);
                                    snprintf(monsterEffectSearchText, sizeof(monsterEffectSearchText), "%s",
                                        monsterEffectDisplayName(monsterEffectSelectedId));
                                    inputstr = monsterEffectIdText;
                                    inputlen = 3;
                                    editproperty = 1001;
                                    cursorflash = ticks;
                                    mousestatus[SDL_BUTTON_LEFT] = 0;
                                    break;
                                }
                            }
                        }
                        if ( mousestatus[SDL_BUTTON_LEFT]
                            && omousex >= subx1 + 72 && omousex < subx1 + 286
                            && omousey >= suby1 + 50 && omousey < suby1 + 66 )
                        {
                            inputstr = monsterEffectSearchText;
                            inputlen = 63;
                            editproperty = 1000;
                            cursorflash = ticks;
                            if ( !SDL_IsTextInputActive() )
                            {
                                SDL_StartTextInput();
                            }
                        }
                        else if ( mousestatus[SDL_BUTTON_LEFT]
                            && omousex >= subx1 + 380 && omousex < subx1 + 428
                            && omousey >= suby1 + 50 && omousey < suby1 + 66 )
                        {
                            inputstr = monsterEffectIdText;
                            inputlen = 3;
                            editproperty = 1001;
                            cursorflash = ticks;
                            if ( !SDL_IsTextInputActive() )
                            {
                                SDL_StartTextInput();
                            }
                        }
                    }
                    if ( stats )
                    {
                        int row = 0;
                        for ( int effect = 0; effect < 135 && row < 8; ++effect )
                        {
                            if ( !stats->getEffectActive(effect) ) continue;
                            const int y = suby1 + 100 + row * 38;
                            printTextFormatted(font8x8_bmp, subx1 + 286, y + 18, "Value: %d", (int)stats->getEffectActive(effect));
                            if ( stats->EFFECTS_TIMERS[effect] < 0 ) printText(font8x8_bmp, subx1 + 370, y + 18, "Duration: permanent");
                            else printTextFormatted(font8x8_bmp, subx1 + 370, y + 18, "Duration: %d sec", stats->EFFECTS_TIMERS[effect] / TICKS_PER_SECOND);
                            ++row;
                        }
                        if ( row == 0 ) printText(font8x8_bmp, subx1 + 18, suby1 + 116, "No starting effects. Choose one above and click Add Selected Effect.");
                    }

                    // Draw the search results last so the dropdown stays above all labels and effect rows.
                    if ( showEffectDropdown )
                    {
                        const int dropdownX1 = subx1 + 72;
                        const int dropdownX2 = subx1 + 286;
                        const int dropdownY1 = suby1 + 68;
                        const int dropdownRowHeight = 16;
                        const int dropdownY2 = dropdownY1
                            + effectDropdownVisibleRows * dropdownRowHeight;
                        drawDepressed(dropdownX1, dropdownY1, dropdownX2, dropdownY2);
                        if ( effectSearchMatches.empty() )
                        {
                            printText(font8x8_bmp, dropdownX1 + 4, dropdownY1 + 4, "No matching effects");
                        }
                        else
                        {
                            const int visibleMatchCount = std::min(effectDropdownVisibleRows,
                                static_cast<int>(effectSearchMatches.size()) - monsterEffectDropdownScroll);
                            for ( int matchRow = 0; matchRow < visibleMatchCount; ++matchRow )
                            {
                                const int effect = effectSearchMatches[monsterEffectDropdownScroll + matchRow];
                                const int rowY = dropdownY1 + matchRow * dropdownRowHeight;
                                if ( omousex >= dropdownX1 && omousex < dropdownX2
                                    && omousey >= rowY && omousey < rowY + dropdownRowHeight )
                                {
                                    SDL_Rect highlight = { dropdownX1 + 2, rowY + 2,
                                        dropdownX2 - dropdownX1 - 4, dropdownRowHeight - 2 };
                                    drawRect(&highlight, makeColorRGB(64, 64, 64), 255);
                                }
                                printTextFormatted(font8x8_bmp, dropdownX1 + 4, rowY + 4, "%s (ID %d)",
                                    monsterEffectDisplayName(effect), effect);
                            }
                            if ( static_cast<int>(effectSearchMatches.size()) > effectDropdownVisibleRows )
                            {
                                printTextFormatted(font8x8_bmp, dropdownX2 - 42, dropdownY2 - 12, "%d/%d",
                                    monsterEffectDropdownScroll + 1,
                                    std::max(1, static_cast<int>(effectSearchMatches.size()) - effectDropdownVisibleRows + 1));
                            }
                        }
                    }
				}
				else if ( newwindow == 10 )
				{
					if ( selectedEntity[0] != NULL )
					{
						int numProperties = sizeof(pedestalPropertyNames) / sizeof(pedestalPropertyNames[0]); //find number of entries in property list
						const int lenProperties = sizeof(pedestalPropertyNames[0]) / sizeof(char); //find length of entry in property list
						int spacing = 36; // 36 px between each item in the list.
						int inputFieldHeader_y = suby1 + 28; // 28 px spacing from subwindow start.
						int inputField_x = subx1 + 8; // 8px spacing from subwindow start.
						int inputField_y = inputFieldHeader_y + 16;
						int inputFieldWidth = 64; // width of the text field
						int inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);
						Uint32 colorError = makeColorRGB(255, 0, 0);

						for ( int i = 0; i < numProperties; i++ )
						{
							int propertyInt = atoi(spriteProperties[i]);

							strcpy(tmpPropertyName, pedestalPropertyNames[i]);
							inputFieldHeader_y = suby1 + 28 + i * spacing;
							inputField_y = inputFieldHeader_y + 16;
							// box outlines then text
							drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
							// print values on top of boxes
							printText(font8x8_bmp, inputField_x, suby1 + 44 + i * spacing, spriteProperties[i]);
							printText(font8x8_bmp, inputField_x, inputFieldHeader_y, tmpPropertyName);

							if ( errorArr[i] != 1 )
							{
								if ( i == 0 )
								{
									if ( propertyInt > 3 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0 blue.
									}
									else
									{
										char tmpStr[32] = "";
										if ( propertyInt == 0 )
										{
											strcpy(tmpStr, "blue");
										}
										else if ( propertyInt == 1 )
										{
											strcpy(tmpStr, "red");
										}
										else if ( propertyInt == 2 )
										{
											strcpy(tmpStr, "purple");
										}
										else if ( propertyInt == 3 )
										{
											strcpy(tmpStr, "green");
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else if ( i == 1 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0 (no orb)
									}
									else
									{
										char tmpStr[32] = "";
										if ( propertyInt == 1 )
										{
											strcpy(tmpStr, "pre-load with orb");
										}
										else
										{
											strcpy(tmpStr, "no orb pre-loaded");
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else if ( i == 2 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0 non-inverted.
									}
									else
									{
										char tmpStr[32] = "";
										if ( propertyInt == 1 )
										{
											strcpy(tmpStr, "inverted (orb to de-power)");
										}
										else
										{
											strcpy(tmpStr, "non-inverted (orb to power)");
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else if ( i == 3 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0 normal height.
									}
									else
									{
										char tmpStr[32] = "";
										if ( propertyInt == 1 )
										{
											strcpy(tmpStr, "true");
										}
										else
										{
											strcpy(tmpStr, "false");
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else if ( i == 4 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0 no lock
									}
									else
									{
										char tmpStr[32] = "";
										if ( propertyInt == 0 )
										{
											strcpy(tmpStr, "able to retreive");
										}
										else
										{
											strcpy(tmpStr, "locked when placed");
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else
								{
									// enter other row entries here
								}
							}

							if ( errorMessage )
							{
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, "Invalid ID!");
								}
							}
						}

						propertyPageTextAndInput(numProperties, inputFieldWidth);

						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}

							// set the maximum length allowed for user input
							inputlen = 2;
							propertyPageCursorFlash(spacing);
						}
					}
				}
				else if ( newwindow == 11 )
				{
					if ( selectedEntity[0] != NULL )
					{
						int numProperties = sizeof(teleporterPropertyNames) / sizeof(teleporterPropertyNames[0]); //find number of entries in property list
						const int lenProperties = sizeof(teleporterPropertyNames[0]) / sizeof(char); //find length of entry in property list
						int spacing = 36; // 36 px between each item in the list.
						int inputFieldHeader_y = suby1 + 28; // 28 px spacing from subwindow start.
						int inputField_x = subx1 + 8; // 8px spacing from subwindow start.
						int inputField_y = inputFieldHeader_y + 16;
						int inputFieldWidth = 64; // width of the text field
						int inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);
						Uint32 colorError = makeColorRGB(255, 0, 0);

						for ( int i = 0; i < numProperties; i++ )
						{
							int propertyInt = atoi(spriteProperties[i]);

							strcpy(tmpPropertyName, teleporterPropertyNames[i]);
							inputFieldHeader_y = suby1 + 28 + i * spacing;
							inputField_y = inputFieldHeader_y + 16;
							// box outlines then text
							drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
							// print values on top of boxes
							printText(font8x8_bmp, inputField_x, suby1 + 44 + i * spacing, spriteProperties[i]);
							printText(font8x8_bmp, inputField_x, inputFieldHeader_y, tmpPropertyName);

							if ( errorArr[i] != 1 )
							{
								if ( i == 0 )
								{
									if ( propertyInt > 999 || propertyInt < 0 )
									{
										propertyPageError(i, 1); // reset to default 1.
									}
								}
								else if ( i == 1 )
								{
									if ( propertyInt > 999 || propertyInt < 0 )
									{
										propertyPageError(i, 1); // reset to default 1.
									}
								}
								else if ( i == 2 )
								{
									if ( propertyInt > 2 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0 up.
									}
									else
									{
										char tmpStr[32] = "";
										if ( propertyInt == 2 )
										{
											strcpy(tmpStr, "portal");
										}
										else if ( propertyInt == 1 )
										{
											strcpy(tmpStr, "ladder down");
										}
										else
										{
											strcpy(tmpStr, "ladder up");
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else
								{
									// enter other row entries here
								}
							}

							if ( errorMessage )
							{
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, "Invalid ID!");
								}
							}
						}

						propertyPageTextAndInput(numProperties, inputFieldWidth);

						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}

							// set the maximum length allowed for user input
							if ( editproperty == 2 )
							{
								inputlen = 2;
							}
							else
							{
								inputlen = 4;
							}
							propertyPageCursorFlash(spacing);
						}
					}
				}
				else if ( newwindow == 12 )
				{
					if ( selectedEntity[0] != nullptr )
					{
						int numProperties = sizeof(ceilingTilePropertyNames) / sizeof(ceilingTilePropertyNames[0]); //find number of entries in property list
						const int lenProperties = sizeof(ceilingTilePropertyNames[0]) / sizeof(char); //find length of entry in property list
						int spacing = 36; // 36 px between each item in the list.
						int inputFieldHeader_y = suby1 + 28; // 28 px spacing from subwindow start.
						int inputField_x = subx1 + 8; // 8px spacing from subwindow start.
						int inputField_y = inputFieldHeader_y + 16;
						int inputFieldWidth = 64; // width of the text field
						int inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);
						Uint32 colorError = makeColorRGB(255, 0, 0);

						for ( int i = 0; i < numProperties; i++ )
						{
							int propertyInt = atoi(spriteProperties[i]);

							strcpy(tmpPropertyName, ceilingTilePropertyNames[i]);
							inputFieldHeader_y = suby1 + 28 + i * spacing;
							inputField_y = inputFieldHeader_y + 16;
							// box outlines then text
							drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
							// print values on top of boxes
							printText(font8x8_bmp, inputField_x, suby1 + 44 + i * spacing, spriteProperties[i]);
							printText(font8x8_bmp, inputField_x, inputFieldHeader_y, tmpPropertyName);

							if ( errorArr[i] != 1 )
							{
								if ( i == 0 )
								{
									if ( propertyInt > 9999 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										if ( modelFileNames.find(propertyInt) != modelFileNames.end() )
										{
											std::string tmpStr = modelFileNames[propertyInt];
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr.c_str());
										}
										else
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, makeColorRGB(255, 0, 0), "Unknown Model!");
										}
									}
								}
								else if ( i == 1 )
								{
									if ( propertyInt > 3 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										char tmpStr[32] = "";
										switch ( propertyInt )
										{
											case 0:
												strcpy(tmpStr, "East");
												break;
											case 1:
												strcpy(tmpStr, "South");
												break;
											case 2:
												strcpy(tmpStr, "West");
												break;
											case 3:
												strcpy(tmpStr, "North");
												break;
											default:
												break;
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else if ( i == 2 )
								{
									if ( propertyInt > 2 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										char tmpStr[32] = "";
										switch ( propertyInt )
										{
											case 0:
												strcpy(tmpStr, "No trap spawn");
												printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
												break;
											case 1:
												strcpy(tmpStr, "Allow spawn and destroy tile");
												printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, tmpStr);
												break;
											case 2:
												strcpy(tmpStr, "Allow spawn and keep tile");
												printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorRandom, tmpStr);
												break;
											default:
												break;
										}
									}
								}
								else if ( i == 3 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										char tmpStr[32] = "";
										if ( propertyInt == 0 )
										{
											strcpy(tmpStr, "Indestructible");
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
										}
										else if ( propertyInt == 1 )
										{
											strcpy(tmpStr, "Minotaur breaks");
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, tmpStr);
										}
									}
								}
								else
								{
									// enter other row entries here
								}
							}

							if ( errorMessage )
							{
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, "Invalid ID!");
								}
							}
						}

						propertyPageTextAndInput(numProperties, inputFieldWidth);

						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}

							// set the maximum length allowed for user input
							if ( editproperty == 0 )
							{
								inputlen = 4;
							}
							else
							{
								inputlen = 3;
							}
							propertyPageCursorFlash(spacing);
						}
					}
				}
				else if ( newwindow == 13 )
				{
					if ( selectedEntity[0] != nullptr )
					{
						int numProperties = sizeof(spellTrapPropertyNames) / sizeof(spellTrapPropertyNames[0]); //find number of entries in property list
						const int lenProperties = sizeof(spellTrapPropertyNames[0]) / sizeof(char); //find length of entry in property list
						int spacing = 36; // 36 px between each item in the list.
						int inputFieldHeader_y = suby1 + 28; // 28 px spacing from subwindow start.
						int inputField_x = subx1 + 8; // 8px spacing from subwindow start.
						int inputField_y = inputFieldHeader_y + 16;
						int inputFieldWidth = 64; // width of the text field
						int inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);
						Uint32 colorError = makeColorRGB(255, 0, 0);

						for ( int i = 0; i < numProperties; i++ )
						{
							int propertyInt = atoi(spriteProperties[i]);

							strcpy(tmpPropertyName, spellTrapPropertyNames[i]);
							inputFieldHeader_y = suby1 + 28 + i * spacing;
							inputField_y = inputFieldHeader_y + 16;
							// box outlines then text
							drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
							// print values on top of boxes
							printText(font8x8_bmp, inputField_x, suby1 + 44 + i * spacing, spriteProperties[i]);
							printText(font8x8_bmp, inputField_x, inputFieldHeader_y, tmpPropertyName);

							if ( errorArr[i] != 1 )
							{
								if ( i == 0 )
								{
									if ( propertyInt > 99 || propertyInt < -1 )
									{
										propertyPageError(i, -1); // reset to default -1.
									}
								}
								else if ( i == 1 )
								{
									if ( propertyInt > 99 || propertyInt < -1 )
									{
										propertyPageError(i, -1); // reset to default -1.
									}
								}
								else if ( i == 2 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0 continuous.
									}
									else
									{
										char tmpStr[32] = "";
										if ( propertyInt == 0 )
										{
											strcpy(tmpStr, "must re-trigger power to fire");
										}
										else if ( propertyInt == 1 )
										{
											strcpy(tmpStr, "continous fire first power up");
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else if ( i == 3 )
								{
									if ( propertyInt > 9999 || propertyInt < 0 )
									{
										propertyPageError(i, 1); // reset to default 1.
									}
								}
								else if ( i == 4 )
								{
									if ( propertyInt > 999 || propertyInt < 0 )
									{
										propertyPageError(i, 1); // reset to default 1.
									}
									else
									{
										char tmpStr[32] = "";
										if ( propertyInt == 1 )
										{
											strcpy(tmpStr, "second");
										}
										else if ( propertyInt > 1 )
										{
											strcpy(tmpStr, "seconds");
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else
								{
									// enter other row entries here
								}
							}

							if ( errorMessage )
							{
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, "Invalid ID!");
								}
							}
						}

						propertyPageTextAndInput(numProperties, inputFieldWidth);

						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}

							// set the maximum length allowed for user input
							if ( editproperty == 0 || editproperty == 1 )
							{
								inputlen = 3;
							}
							else if ( editproperty == 2 )
							{
								inputlen = 2;
							}
							else
							{
								inputlen = 4;
							}
							propertyPageCursorFlash(spacing);
						}
					}
				}
				else if ( newwindow == 14 )
				{
					if ( selectedEntity[0] != nullptr )
					{
						int numProperties = sizeof(furniturePropertyNames) / sizeof(furniturePropertyNames[0]); //find number of entries in property list
						const int lenProperties = sizeof(furniturePropertyNames[0]) / sizeof(char); //find length of entry in property list
						int spacing = 36; // 36 px between each item in the list.
						int inputFieldHeader_y = suby1 + 28; // 28 px spacing from subwindow start.
						int inputField_x = subx1 + 8; // 8px spacing from subwindow start.
						int inputField_y = inputFieldHeader_y + 16;
						int inputFieldWidth = 64; // width of the text field
						int inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);
						Uint32 colorError = makeColorRGB(255, 0, 0);

						for ( int i = 0; i < numProperties; i++ )
						{
							int propertyInt = atoi(spriteProperties[i]);

							strcpy(tmpPropertyName, furniturePropertyNames[i]);
							inputFieldHeader_y = suby1 + 28 + i * spacing;
							inputField_y = inputFieldHeader_y + 16;
							// box outlines then text
							drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
							// print values on top of boxes
							printText(font8x8_bmp, inputField_x, suby1 + 44 + i * spacing, spriteProperties[i]);
							printText(font8x8_bmp, inputField_x, inputFieldHeader_y, tmpPropertyName);

							if ( errorArr[i] != 1 )
							{
								if ( i == 0 )
								{
									if ( propertyInt > 9 || propertyInt < -1 )
									{
										propertyPageError(i, -1); // reset to default -1.
									}
									else
									{
										char tmpStr[32] = "";
										switch ( propertyInt )
										{
											case -1:
												strcpy(tmpStr, "random");
												break;
											case 0:
												strcpy(tmpStr, "East");
												break;
											case 1:
												strcpy(tmpStr, "Southeast");
												break;
											case 2:
												strcpy(tmpStr, "South");
												break;
											case 3:
												strcpy(tmpStr, "Southwest");
												break;
											case 4:
												strcpy(tmpStr, "West");
												break;
											case 5:
												strcpy(tmpStr, "Northwest");
												break;
											case 6:
												strcpy(tmpStr, "North");
												break;
											case 7:
												strcpy(tmpStr, "Northeast");
												break;
											default:
												break;
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else
								{
									// enter other row entries here
								}
							}

							if ( errorMessage )
							{
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, "Invalid ID!");
								}
							}
						}

						propertyPageTextAndInput(numProperties, inputFieldWidth);

						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}

							// set the maximum length allowed for user input
							inputlen = 2;
							propertyPageCursorFlash(spacing);
						}
					}
				}
				else if ( newwindow == 15 )
				{
					if ( selectedEntity[0] != nullptr )
					{
						int numProperties = sizeof(floorDecorationPropertyNames) / sizeof(floorDecorationPropertyNames[0]); //find number of entries in property list
						const int lenProperties = sizeof(floorDecorationPropertyNames[0]) / sizeof(char); //find length of entry in property list
						int spacing = 36; // 36 px between each item in the list.
						int inputFieldHeader_y = suby1 + 28; // 28 px spacing from subwindow start.
						int inputField_x = subx1 + 8; // 8px spacing from subwindow start.
						int inputField_y = inputFieldHeader_y + 16;
						int inputFieldWidth = 64; // width of the text field
						int inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);
						Uint32 colorError = makeColorRGB(255, 0, 0);
						std::string rotationStr = "";
						for ( int i = 0; i < numProperties; i++ )
						{
							int propertyInt = atoi(spriteProperties[i]);

							if ( i >= 6 && i <= 9 )
							{
								inputFieldWidth = subx2 - inputField_x; // width of the text field
								if ( i > 6 )
								{
									spacing = 18;
								}
								else
								{
									spacing = 36;
								}
							}
							else
							{
								inputFieldWidth = 64; // width of the text field
								spacing = 36;
							}

							inputFieldHeader_y = suby1 + 28 + i * spacing;
							inputField_y = inputFieldHeader_y + 16;

							if ( i > 6 && i <= 9 )
							{
								int offsetBoxes = 3 * 36;
								inputFieldHeader_y = suby1 + 28 + i * spacing + offsetBoxes;
								inputField_y = inputFieldHeader_y + 16;
								// box outlines then text
								drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
								// print values on top of boxes
								printText(font8x8_bmp, inputField_x, suby1 + 44 + i * spacing + offsetBoxes, spriteProperties[i]);
							}
							else
							{
								// box outlines then text
								drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
								// print values on top of boxes
								printText(font8x8_bmp, inputField_x, suby1 + 44 + i * spacing, spriteProperties[i]);
								strcpy(tmpPropertyName, floorDecorationPropertyNames[i]);
								printText(font8x8_bmp, inputField_x, inputFieldHeader_y, tmpPropertyName);
							}

							if ( errorArr[i] != 1 )
							{
								if ( i == 0 )
								{
									if ( propertyInt > 9999 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										if ( modelFileNames.find(propertyInt) != modelFileNames.end() )
										{
											std::string tmpStr = modelFileNames[propertyInt];
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr.c_str());
										}
										else
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, makeColorRGB(255, 0, 0), "Unknown Model!");
										}
									}
								}
								else if ( i == 1 )
								{
									if ( propertyInt > 7 || propertyInt < -1 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										char tmpStr[32] = "";
										switch ( propertyInt )
										{
											case -1:
												strcpy(tmpStr, "random");
												break;
											case 0:
												strcpy(tmpStr, "East");
												break;
											case 1:
												strcpy(tmpStr, "Southeast");
												break;
											case 2:
												strcpy(tmpStr, "South");
												break;
											case 3:
												strcpy(tmpStr, "Southwest");
												break;
											case 4:
												strcpy(tmpStr, "West");
												break;
											case 5:
												strcpy(tmpStr, "Northwest");
												break;
											case 6:
												strcpy(tmpStr, "North");
												break;
											case 7:
												strcpy(tmpStr, "Northeast");
												break;
											default:
												break;
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
										rotationStr = tmpStr;
									}
								}
								else if ( i == 2 || i == 3 || i == 4 )
								{
									if ( propertyInt > 999 || propertyInt < -999 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
								}
								else if ( i == 5 )
								{
									if ( propertyInt > 8 || propertyInt < -1 )
									{
										propertyPageError(i, -1); // reset to default -1
									}
									else
									{
										char tmpStr[32] = "";
										switch ( propertyInt )
										{
										case -1:
											strcpy(tmpStr, "n/a");
											break;
										case 0:
											strcpy(tmpStr, "East");
											break;
										case 1:
											strcpy(tmpStr, "Southeast");
											break;
										case 2:
											strcpy(tmpStr, "South");
											break;
										case 3:
											strcpy(tmpStr, "Southwest");
											break;
										case 4:
											strcpy(tmpStr, "West");
											break;
										case 5:
											strcpy(tmpStr, "Northwest");
											break;
										case 6:
											strcpy(tmpStr, "North");
											break;
										case 7:
											strcpy(tmpStr, "Northeast");
											break;
										case 8:
											snprintf(tmpStr, sizeof(tmpStr), "Opposite To Rotation");
											break;
										default:
											break;
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else
								{
									// enter other row entries here
								}
							}

							if ( errorMessage )
							{
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, "Invalid ID!");
								}
							}
						}

						spacing = 36;

						// Cycle properties with TAB.
						if ( keystatus[SDLK_TAB] )
						{
							keystatus[SDLK_TAB] = 0;
							cursorflash = ticks;
							editproperty++;
							if ( editproperty == numProperties )
							{
								editproperty = 0;
							}

							inputstr = spriteProperties[editproperty];
						}

						// select a textbox
						if ( mousestatus[SDL_BUTTON_LEFT] )
						{
							for ( int i = 0; i < numProperties; i++ )
							{
								inputField_x = subx1 + 8;
								if ( i > 6 && i <= 9 )
								{
									spacing = 18;
									int offsetBoxes = 3 * 36;
									inputFieldWidth = subx2 - inputField_x; // width of the text field
									if ( mouseInBounds(inputField_x - 4, inputField_x - 4 + inputFieldWidth,
										suby1 + 40 + i * spacing + offsetBoxes, suby1 + 56 + i * spacing + offsetBoxes) )
									{
										inputstr = spriteProperties[i];
										editproperty = i;
										cursorflash = ticks;
									}
								}
								else
								{
									if ( i == 6 )
									{
										inputFieldWidth = subx2 - inputField_x; // width of the text field
									}
									else
									{
										inputFieldWidth = 64; // width of the text field
									}
									spacing = 36;

									if ( mouseInBounds(inputField_x - 4, inputField_x - 4 + inputFieldWidth,
										suby1 + 40 + i * spacing, suby1 + 56 + i * spacing) )
									{
										inputstr = spriteProperties[i];
										editproperty = i;
										cursorflash = ticks;
									}
								}
							}
						}

						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}

							// set the maximum length allowed for user input
							if ( editproperty >= 6 && editproperty <= 9 )
							{
								inputlen = 48;
							}
							else
							{
								inputlen = 4;
							}
							if ( (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
							{
								if ( editproperty > 6 && editproperty <= 9 )
								{
									spacing = 18;
									int offsetBoxes = 3 * 36;
									if ( (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
									{
										printText(font8x8_bmp, subx1 + 8 + strlen(spriteProperties[editproperty]) * 8, suby1 + 44 + editproperty * spacing + offsetBoxes, "\26");
									}
								}
								else
								{
									spacing = 36;
									propertyPageCursorFlash(spacing);
								}
							}
						}
					}
				}
				else if ( newwindow == 18 )
				{
					if ( selectedEntity[0] != nullptr )
					{
						int numProperties = sizeof(soundSourcePropertyNames) / sizeof(soundSourcePropertyNames[0]); //find number of entries in property list
						const int lenProperties = sizeof(soundSourcePropertyNames[0]) / sizeof(char); //find length of entry in property list
						int spacing = 36; // 36 px between each item in the list.
						int inputFieldHeader_y = suby1 + 28; // 28 px spacing from subwindow start.
						int inputField_x = subx1 + 8; // 8px spacing from subwindow start.
						int inputField_y = inputFieldHeader_y + 16;
						int inputFieldWidth = 64; // width of the text field
						int inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);
						Uint32 colorError = makeColorRGB(255, 0, 0);

						for ( int i = 0; i < numProperties; i++ )
						{
							int propertyInt = atoi(spriteProperties[i]);

							strcpy(tmpPropertyName, soundSourcePropertyNames[i]);
							inputFieldHeader_y = suby1 + 28 + i * spacing;
							inputField_y = inputFieldHeader_y + 16;
							// box outlines then text
							drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
							// print values on top of boxes
							printText(font8x8_bmp, inputField_x, suby1 + 44 + i * spacing, spriteProperties[i]);
							printText(font8x8_bmp, inputField_x, inputFieldHeader_y, tmpPropertyName);

							if ( errorArr[i] != 1 )
							{
								if ( i == 4 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else if ( propertyInt == 0 )
									{
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "Play sound on this entity");
									}
									else
									{
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "Play sound global");
									}
								}
								else if ( i == 3 )
								{
									if ( propertyInt > 9999 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
								}
								if ( i == 2 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
								}
								else if ( i == 1 )
								{
									if ( propertyInt > 255 || propertyInt < -1 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
								}
								else if ( i == 0 )
								{
									if ( propertyInt > 999 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
								}
								else
								{
									// enter other row entries here
								}
							}

							if ( errorMessage )
							{
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, "Invalid ID!");
								}
							}
						}

						propertyPageTextAndInput(numProperties, inputFieldWidth);

						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}

							// set the maximum length allowed for user input
							if ( editproperty <= 1 )
							{
								inputlen = 3;
							}
							else if ( editproperty == 2 )
							{
								inputlen = 1;
							}
							else
							{
								inputlen = 4;
							}
							propertyPageCursorFlash(spacing);
						}
					}
				}
				else if ( newwindow == 19 )
				{
					if ( selectedEntity[0] != nullptr )
					{
						int numProperties = sizeof(lightSourcePropertyNames) / sizeof(lightSourcePropertyNames[0]); //find number of entries in property list
						const int lenProperties = sizeof(lightSourcePropertyNames[0]) / sizeof(char); //find length of entry in property list
						int spacing = 36; // 36 px between each item in the list.
						int inputFieldHeader_y = suby1 + 28; // 28 px spacing from subwindow start.
						int inputField_x = subx1 + 8; // 8px spacing from subwindow start.
						int inputField_y = inputFieldHeader_y + 16;
						int inputFieldWidth = 64; // width of the text field
						int inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);
						Uint32 colorError = makeColorRGB(255, 0, 0);

						for ( int i = 0; i < numProperties; i++ )
						{
							inputField_x = subx1 + 8;
							int propertyInt = atoi(spriteProperties[i]);

							if ( i >= 7 && i <= 9 )
							{
								inputFieldFeedback_x = inputField_x + (inputFieldWidth + 8) * 4 - 4;
							}

							if ( i == 8 || i == 9 )
							{
								// no header
								inputFieldHeader_y = suby1 + 28 + 7 * spacing;
								inputField_y = inputFieldHeader_y + 16;
								inputField_x = subx1 + 8 + (inputFieldWidth + 8) * (i - 7);

								// box outlines then text
								drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
								// print values on top of boxes
								printText(font8x8_bmp, inputField_x, suby1 + 44 + 7 * spacing, spriteProperties[i]);

								if ( i == 9 )
								{
									Uint32 colorPreview = makeColorRGB((Uint32)atoi(spriteProperties[7]),
										(Uint32)atoi(spriteProperties[8]), (Uint32)atoi(spriteProperties[9]));
									SDL_Rect src;
									src.x = subx1 + 8 + (inputFieldWidth + 8) * 3;
									src.h = 16;
									src.w = 32;
									src.y = inputField_y - 4;
									drawRect(&src, colorPreview, 255);
								}
							}
							else
							{
								
								strcpy(tmpPropertyName, lightSourcePropertyNames[i]);
								inputFieldHeader_y = suby1 + 28 + i * spacing;

								inputField_y = inputFieldHeader_y + 16;
								// box outlines then text
								drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
								// print values on top of boxes
								printText(font8x8_bmp, inputField_x, suby1 + 44 + i * spacing, spriteProperties[i]);
								printText(font8x8_bmp, inputField_x, inputFieldHeader_y, tmpPropertyName);
							}


							if ( errorArr[i] != 1 )
							{
								if ( i == 0 || i == 2 || i == 3 || i == 5 )
								{
									if ( propertyInt > 2 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
								}
								else if ( i == 4 )
								{
									if ( propertyInt > 64 || propertyInt < -1 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
								}
								else if ( i == 6 )
								{
									if ( propertyInt > 9999 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
								}
								else if ( i == 1 || (i >= 7 && i <= 9) )
								{
									if ( propertyInt > 255 || propertyInt < -1 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
								}
								else
								{
									// enter other row entries here
								}
							}

							if ( errorMessage )
							{
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, "Invalid ID!");
								}
							}
						}

						//propertyPageTextAndInput(numProperties, inputFieldWidth);
						{
							int pad_x1 = subx1 + 8;
							int spacing = 36;
							int pad_x2 = inputFieldWidth;

							// Cycle properties with TAB.
							if ( keystatus[SDLK_TAB] )
							{
								keystatus[SDLK_TAB] = 0;
								cursorflash = ticks;
								editproperty++;
								if ( editproperty == numProperties )
								{
									editproperty = 0;
								}

								inputstr = spriteProperties[editproperty];
							}

							// select a textbox
							if ( mousestatus[SDL_BUTTON_LEFT] )
							{
								for ( int i = 0; i < numProperties; i++ )
								{
									if ( i == 8 || i == 9 )
									{
										inputFieldWidth = 64;
										inputField_x = subx1 + 8 + (inputFieldWidth + 8) * (i - 7);
										if ( omousex >= inputField_x - 4 
											&& omousex < inputField_x - 4 + inputFieldWidth 
											&& omousey >= suby1 + 40 + 7 * spacing 
											&& omousey < suby1 + 56 + 7 * spacing )
										{
											inputstr = spriteProperties[i];
											editproperty = i;
											cursorflash = ticks;
										}
									}
									else
									{
										if ( omousex >= pad_x1 - 4 && omousey >= suby1 + 40 + i * spacing && omousex < pad_x1 - 4 + pad_x2 && omousey < suby1 + 56 + i * spacing )
										{
											inputstr = spriteProperties[i];
											editproperty = i;
											cursorflash = ticks;
										}
									}
								}
							}
						}

						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}

							// set the maximum length allowed for user input
							if ( editproperty == 1 )
							{
								inputlen = 3;
							}
							else if ( editproperty == 0 || editproperty == 2 || editproperty == 3 || editproperty == 5 )
							{
								inputlen = 1;
							}
							else if ( editproperty == 4 )
							{
								inputlen = 2;
							}
							else
							{
								inputlen = 4;
							}
							//propertyPageCursorFlash(spacing);
							if ( (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
							{
								if ( editproperty == 8 || editproperty == 9 )
								{
									printText(font8x8_bmp, (subx1 + 8 + (inputFieldWidth + 8) * (editproperty - 7)) + strlen(spriteProperties[editproperty]) * 8, suby1 + 44 + 7 * spacing, "\26");
								}
								else
								{
									printText(font8x8_bmp, subx1 + 8 + strlen(spriteProperties[editproperty]) * 8, suby1 + 44 + editproperty * spacing, "\26");
								}
							}
						}
					}
				}
				else if ( newwindow == 20 )
				{
					if ( selectedEntity[0] != nullptr )
					{
						int numProperties = sizeof(textSourcePropertyNames) / sizeof(textSourcePropertyNames[0]); //find number of entries in property list
						const int lenProperties = sizeof(textSourcePropertyNames[0]) / sizeof(char); //find length of entry in property list
						int spacing = 36; // 36 px between each item in the list.
						int inputFieldHeader_y = suby1 + 28; // 28 px spacing from subwindow start.
						int inputField_x = subx1 + 8; // 8px spacing from subwindow start.
						int inputField_y = inputFieldHeader_y + 16;
						int inputFieldWidth = 64; // width of the text field
						int inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);
						Uint32 colorError = makeColorRGB(255, 0, 0);
						bool showTextSourceTooltip = false;

						for ( int i = 0; i < numProperties; i++ )
						{
							inputField_x = subx1 + 8;
							int propertyInt = atoi(spriteProperties[i]);
							if ( i >= 3 && i < 8 )
							{
								inputFieldWidth = subx2 - inputField_x; // width of the text field
								inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
								if ( i > 3 )
								{
									spacing = 18;
								}
								else
								{
									spacing = 36;
								}
							}
							else
							{
								inputFieldWidth = 64; // width of the text field
								inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
								spacing = 36;
							}

							if ( i <= 2 )
							{
								inputFieldFeedback_x = inputField_x + (inputFieldWidth + 8) * 4 - 4;
							}
							if ( i == 1 || i == 2 )
							{
								inputFieldHeader_y = suby1 + 28;
								inputField_y = inputFieldHeader_y + 16;
								inputField_x = subx1 + 8 + (inputFieldWidth + 8) * i;
								if ( i == 2 )
								{
									Uint32 colorPreview = makeColorRGB((Uint32)atoi(spriteProperties[0]),
										(Uint32)atoi(spriteProperties[1]), (Uint32)atoi(spriteProperties[2]));
									SDL_Rect src;
									src.x = subx1 + 8 + (inputFieldWidth + 8) * 3;
									src.h = 16;
									src.w = 32;
									src.y = inputField_y - 4;
									drawRect(&src, colorPreview, 255);

									printText(font8x8_bmp, inputField_x + (inputFieldWidth + 8), inputFieldHeader_y, "Hover for help");
									if ( mouseInBounds(inputField_x + (inputFieldWidth + 8), inputField_x + (inputFieldWidth + 8) + strlen("Hover for help") * 8,
										inputFieldHeader_y, inputFieldHeader_y + 16) )
									{
										showTextSourceTooltip = true;
									}
								}
							}
							else if ( i > 3 && i < 8 )
							{
								inputFieldHeader_y = suby1 + 28 + (i - 1) * spacing;
								inputField_y = inputFieldHeader_y + 16;
							}
							else
							{
								// header print
								if ( i == 3 )
								{
									inputFieldHeader_y = suby1 + 28 + (i - 2) * spacing;
								}
								else if ( i >= 8 )
								{
									inputFieldHeader_y = suby1 + 28 + (i - 4) * spacing;
								}
								else
								{
									inputFieldHeader_y = suby1 + 28;
								}
								inputField_y = inputFieldHeader_y + 16;
								strcpy(tmpPropertyName, textSourcePropertyNames[i]);
								printText(font8x8_bmp, inputField_x, inputFieldHeader_y, tmpPropertyName);
							}
							// box outlines then text
							// print values on top of boxes
							if ( i == 1 || i == 2 )
							{
								drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
								printText(font8x8_bmp, inputField_x, inputField_y, spriteProperties[i]);
							}
							else
							{
								drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
								printText(font8x8_bmp, inputField_x, inputField_y, spriteProperties[i]);
							}

							if ( errorArr[i] != 1 )
							{
								if ( i >= 0 && i <= 2 )
								{
									if ( propertyInt > 255 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
								}
								else if ( i == 8 )
								{
									if ( propertyInt > 9999 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
								}
								else if ( i == 9 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
								}
								else
								{
									// enter other row entries here
								}
							}

							if ( errorMessage )
							{
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, "Invalid ID!");
								}
							}
						}

						spacing = 36;

						// Cycle properties with TAB.
						if ( keystatus[SDLK_TAB] )
						{
							keystatus[SDLK_TAB] = 0;
							cursorflash = ticks;
							editproperty++;
							if ( editproperty == numProperties )
							{
								editproperty = 0;
							}

							inputstr = spriteProperties[editproperty];
						}

						// select a textbox
						if ( mousestatus[SDL_BUTTON_LEFT] )
						{
							for ( int i = 0; i < numProperties; i++ )
							{
								inputField_x = subx1 + 8;
								if ( i > 3 && i < 8 )
								{
									spacing = 18;
									inputFieldWidth = subx2 - inputField_x; // width of the text field
									if ( mouseInBounds(inputField_x - 4, inputField_x - 4 + inputFieldWidth,
										suby1 + 40 + (i - 1) * spacing, suby1 + 56 + (i - 1) * spacing) )
									{
										inputstr = spriteProperties[i];
										editproperty = i;
										cursorflash = ticks;
									}
								}
								else
								{
									if ( i == 3 )
									{
										inputFieldWidth = subx2 - inputField_x; // width of the text field
									}
									else
									{
										inputFieldWidth = 64; // width of the text field
									}
									spacing = 36;
									if ( i == 1 || i == 2 )
									{
										inputField_x = subx1 + 8 + (inputFieldWidth + 8) * i;
										spacing = 0;
									}
									else if ( i == 3 )
									{
										spacing = 12;
									}
									
									if ( i >= 8 )
									{
										if ( mouseInBounds(inputField_x - 4, inputField_x - 4 + inputFieldWidth,
											suby1 + 40 + (i - 4) * spacing, suby1 + 56 + (i - 4) * spacing) )
										{
											inputstr = spriteProperties[i];
											editproperty = i;
											cursorflash = ticks;
										}
									}
									else
									{
										if ( mouseInBounds(inputField_x - 4, inputField_x - 4 + inputFieldWidth, 
											suby1 + 40 + i * spacing, suby1 + 56 + i * spacing) )
										{
											inputstr = spriteProperties[i];
											editproperty = i;
											cursorflash = ticks;
										}
									}
								}
							}
						}

						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}
							if ( SDL_IsTextInputActive() )
							{
								if ( textInsertCaratPosition >= 0 )
								{
									textInsertCaratPosition = std::min(textInsertCaratPosition, (int)strlen(inputstr));
									if ( editproperty < 3 || editproperty > 8 )
									{
										textInsertCaratPosition = -1;
									}
								}
								if ( keystatus[SDLK_LEFT] )
								{
									keystatus[SDLK_LEFT] = 0;
									if ( textInsertCaratPosition > 0 )
									{
										--textInsertCaratPosition;
									}
									else if ( textInsertCaratPosition == 0 )
									{
										// do nothing
									}
									else
									{
										textInsertCaratPosition = strlen(inputstr);
									}
									cursorflash = ticks;
								}
								else if ( keystatus[SDLK_RIGHT] )
								{
									keystatus[SDLK_RIGHT] = 0;
									if ( textInsertCaratPosition == -1 )
									{
										textInsertCaratPosition = strlen(inputstr);
									}
									else
									{
										++textInsertCaratPosition;
										textInsertCaratPosition = std::min((int)strlen(inputstr), textInsertCaratPosition);
									}
									cursorflash = ticks;
								}
								if ( keystatus[SDLK_RETURN] )
								{
									if ( textInsertCaratPosition >= 0 )
									{
										textInsertCaratPosition = -1;
									}
									else
									{
										textInsertCaratPosition = strlen(inputstr);
									}
									keystatus[SDLK_RETURN] = 0;
								}
							}

							// set the maximum length allowed for user input
							if ( editproperty >= 3 && editproperty < 8 )
							{
								if ( editproperty == 7 )
								{
									inputlen = 32;
								}
								else
								{
									inputlen = 48;
								}
							}
							else
							{
								if ( editproperty == 9 )
								{
									inputlen = 1;
								}
								else
								{
									inputlen = 4;
								}
							}
							if ( (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
							{
								if ( editproperty > 3 && editproperty < 8 )
								{
									spacing = 18;
									if ( textInsertCaratPosition >= 0 )
									{
										printTextFormattedColor(font8x8_bmp, subx1 + 8 + textInsertCaratPosition * 8, suby1 + 44 + (editproperty - 1) * spacing, color, "\26");
									}
									else
									{
										printText(font8x8_bmp, subx1 + 8 + strlen(spriteProperties[editproperty]) * 8, suby1 + 44 + (editproperty - 1) * spacing, "\26");
									}
								}
								else
								{
									spacing = 36;
									if ( editproperty == 1 || editproperty == 2 )
									{
										spacing = 0;
										inputFieldWidth = 64;
										printText(font8x8_bmp, subx1 + 8 + (inputFieldWidth + 8) * editproperty + strlen(spriteProperties[editproperty]) * 8, suby1 + 44, "\26");
									}
									else if ( editproperty == 3 )
									{
										if ( textInsertCaratPosition >= 0 )
										{
											printTextFormattedColor(font8x8_bmp, subx1 + 8 + textInsertCaratPosition * 8, suby1 + 44 + spacing, color, "\26");
										}
										else
										{
											printText(font8x8_bmp, subx1 + 8 + strlen(spriteProperties[editproperty]) * 8, suby1 + 44 + spacing, "\26");
										}
									}
									else
									{
										if ( editproperty >= 8 )
										{
											printText(font8x8_bmp, subx1 + 8 + strlen(spriteProperties[editproperty]) * 8, suby1 + 44 + (editproperty - 4) * spacing, "\26");
										}
										else
										{
											printText(font8x8_bmp, subx1 + 8 + strlen(spriteProperties[editproperty]) * 8, suby1 + 44 + editproperty * spacing, "\26");
										}
									}
								}
							}
						}

						if ( showTextSourceTooltip )
						{
							SDL_Rect src;
							src.w = 74 * 8 + 4;
							src.h = 240;
							src.x = omousex - src.w / 2;
							src.y = omousey + 16 - src.h / 2;
							drawTooltip(&src);
							printText(font8x8_bmp, src.x + 4, src.y + 4, "This sprite sends a message to all players in the specified color.");
							printText(font8x8_bmp, src.x + 4, src.y + 16, "Text will appear as one line unless a new line symbol is entered.");
							printText(font8x8_bmp, src.x + 4, src.y + 28, "To insert text onto a new line, enter \\n in the text field.");
							printText(font8x8_bmp, src.x + 4, src.y + 40, "To address the player's name in the text, enter @p in the text field.");
							printText(font8x8_bmp, src.x + 4, src.y + 52, "E.g \"Hello, \\n@p\"");
							printText(font8x8_bmp, src.x + 4, src.y + 64, "Adding @script to the text allows some basic scripting instead of text.");
							printText(font8x8_bmp, src.x + 4, src.y + 76, "Separate all tags with a space. Some tags allow a range of values.");
							printText(font8x8_bmp, src.x + 4, src.y + 88, "@clrplayer clears player data. @class=1 sets class to warrior");
							printText(font8x8_bmp, src.x + 4, src.y + 100, "@clrstats resets stats and HP/MP. @hunger=250 sets player hungry.");
							printText(font8x8_bmp, src.x + 4, src.y + 112, "@nextlevel=2 triggers a 2 level change.");
							printText(font8x8_bmp, src.x + 4, src.y + 124, "@copyNPC=2 player stats set to monster named \"scriptNPC\" that is NPC: 2");
							printText(font8x8_bmp, src.x + 4, src.y + 136, "");
							printText(font8x8_bmp, src.x + 4, src.y + 148, "Tags that take a map reference or range: (i.e tag=1-3,4-6 tag=8,10)");
							printText(font8x8_bmp, src.x + 4, src.y + 160, "@power=11,15 powers tile x=11,y=15");
							printText(font8x8_bmp, src.x + 4, src.y + 172, "@power=11-13,15-17 powers 3x3 grid from x=11,y=15 to x=13,y=17.");
							printText(font8x8_bmp, src.x + 4, src.y + 184, "@unpower= unpower tile(s). @freezemonsters= freeze monster on tile(s)");
							printText(font8x8_bmp, src.x + 4, src.y + 196, "@unfreezemonsters= unfreeze on tile(s). @killall= kill npcs on tile(s)");
							printText(font8x8_bmp, src.x + 4, src.y + 208, "@killenemies= kill non- ally npcs on tile(s).");
							printText(font8x8_bmp, src.x + 4, src.y + 220, "@nodropitems= npc will be marked to not drop items on death within tile(s)");
						}
					}
				}
				else if ( newwindow == 21 )
				{
					if ( selectedEntity[0] != nullptr )
					{
						int numProperties = sizeof(signalTimerPropertyNames) / sizeof(signalTimerPropertyNames[0]); //find number of entries in property list
						const int lenProperties = sizeof(signalTimerPropertyNames[0]) / sizeof(char); //find length of entry in property list
						int spacing = 36; // 36 px between each item in the list.
						int inputFieldHeader_y = suby1 + 28; // 28 px spacing from subwindow start.
						int inputField_x = subx1 + 8; // 8px spacing from subwindow start.
						int inputField_y = inputFieldHeader_y + 16;
						int inputFieldWidth = 64; // width of the text field
						int inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);
						Uint32 colorError = makeColorRGB(255, 0, 0);

						for ( int i = 0; i < numProperties; i++ )
						{
							int propertyInt = atoi(spriteProperties[i]);

							strcpy(tmpPropertyName, signalTimerPropertyNames[i]);
							inputFieldHeader_y = suby1 + 28 + i * spacing;
							inputField_y = inputFieldHeader_y + 16;
							// box outlines then text
							drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
							// print values on top of boxes
							printText(font8x8_bmp, inputField_x, suby1 + 44 + i * spacing, spriteProperties[i]);
							printText(font8x8_bmp, inputField_x, inputFieldHeader_y, tmpPropertyName);

							if ( errorArr[i] != 1 )
							{
								if ( i == 0 )
								{
									if ( propertyInt > 3 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										char tmpStr[8] = "";
										switch ( propertyInt )
										{
											case 0:
												strcpy(tmpStr, "West");
												break;
											case 1:
												strcpy(tmpStr, "South");
												break;
											case 2:
												strcpy(tmpStr, "East");
												break;
											case 3:
												strcpy(tmpStr, "North");
												break;
											default:
												break;
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else if ( i == 1 )
								{
									if ( propertyInt > 9999 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
								}
								else if ( i == 2 )
								{
									if ( propertyInt > 9999 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										if ( propertyInt == 0 )
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "output without on/off toggling");
										}
									}
								}
								else if ( i == 3 )
								{
									if ( propertyInt > 9999 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										if ( propertyInt == 0 )
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "repeat infinite");
										}
									}
								}
								else if ( i == 4 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										if ( propertyInt == 0 )
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "turn off without input signal");
										}
										else if ( propertyInt == 1 )
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "stay on without input signal");
										}
									}
								}
								else if ( i == 5 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										if ( propertyInt == 0 )
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "non-inverted");
										}
										else if ( propertyInt == 1 )
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "output inverted");
										}
									}
								}
								else
								{
									// enter other row entries here
								}
							}

							if ( errorMessage )
							{
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, "Invalid ID!");
								}
							}
						}

						propertyPageTextAndInput(numProperties, inputFieldWidth);

						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}

							// set the maximum length allowed for user input
							inputlen = 4;
							propertyPageCursorFlash(spacing);
						}
					}
				}
				else if ( newwindow == 22 )
				{
					if ( selectedEntity[0] != nullptr )
					{
						int numProperties = sizeof(customPortalPropertyNames) / sizeof(customPortalPropertyNames[0]); //find number of entries in property list
						const int lenProperties = 128; // Some custom-exit labels can fill the original 59-byte row width.
						int spacing = 36; // 36 px between each item in the list.
						int inputFieldHeader_y = suby1 + 28; // 28 px spacing from subwindow start.
						int inputField_x = subx1 + 8; // 8px spacing from subwindow start.
						int inputField_y = inputFieldHeader_y + 16;
						int inputFieldWidth = 64; // width of the text field
						int inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);
						Uint32 colorError = makeColorRGB(255, 0, 0);

						for ( int i = 0; i < numProperties; i++ )
						{
							int propertyInt = (i == 4) ? 0 : atoi(spriteProperties[i]);

							snprintf(tmpPropertyName, sizeof(tmpPropertyName), "%s", customPortalPropertyNames[i]);
							inputFieldHeader_y = suby1 + 28 + i * spacing;
							inputField_y = inputFieldHeader_y + 16;
							// box outlines then text
							if ( i == 4 )
							{
								inputFieldWidth = 280; // width of the text field
							}
							else
							{
								inputFieldWidth = 64; // width of the text field
							}
							drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
							// print values on top of boxes
							printText(font8x8_bmp, inputField_x, suby1 + 44 + i * spacing, spriteProperties[i]);
							printText(font8x8_bmp, inputField_x, inputFieldHeader_y, tmpPropertyName);

							if ( errorArr[i] != 1 )
							{
								if ( i == 0 )
								{
									if ( propertyInt > 9999 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										if ( modelFileNames.find(propertyInt) != modelFileNames.end() )
										{
											std::string tmpStr = modelFileNames[propertyInt];
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr.c_str());
										}
										else
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, makeColorRGB(255, 0, 0), "Unknown Model!");
										}
									}
								}
								else if ( i == 1 )
								{
									if ( propertyInt > 9 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										if ( propertyInt != 0 )
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, 
												"using model textures %d-%d for animation", 
												atoi(spriteProperties[0]), atoi(spriteProperties[0]) + propertyInt - 1);
										}
									}
								}
								else if ( i == 2 )
								{
									if ( propertyInt > 999 || propertyInt < -999 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
								}
								else if ( i == 3 )
								{
									if ( propertyInt > 99 || propertyInt < -99 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										if ( spriteProperties[4][0] != 0 && propertyInt != 0 )
										{
											char shortName[16] = "";
											strncpy(shortName, spriteProperties[4], 11);
											if ( strlen(spriteProperties[4]) > 9 )
											{
												strcat(shortName, "..");
											}
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "move to instance %d of map name %s", propertyInt, shortName);
										}
										else if ( spriteProperties[4][0] != 0 && propertyInt == 0 )
										{
											char shortName[16] = "";
											strncpy(shortName, spriteProperties[4], 11);
											if ( strlen(spriteProperties[4]) > 9 )
											{
												strcat(shortName, "..");
											}
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "move to first instance of map name %s", shortName);
										}
										else
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "jump %d levels", propertyInt);
										}
									}
								}
								else if ( i == 4 )
								{

								}
								else if ( i == 5 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										if ( propertyInt == 0 )
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "always visible");
										}
										else if ( propertyInt == 1 )
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "requires power to be visible");
										}
									}
								}
								else if ( i == 6 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										if ( propertyInt == 0 )
										{
											if ( spriteProperties[4][0] != 0 )
											{
												printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "searching for map in normal levels");
											}
											else
											{
												printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "no toggle");
											}
										}
										else if ( propertyInt == 1 )
										{
											if ( spriteProperties[4][0] != 0 )
											{
												printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "searching for map in secret levels");
											}
											else
											{
												printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "will toggle secret levels file");
											}
										}
									}
								}
								else if ( i == 7 )
								{
									if ( propertyInt < 0 || propertyInt > 4 )
									{
										propertyPageError(i, 0);
									}
									else
									{
										switch ( propertyInt )
										{
											case 0:
												printTextFormattedColor(
													font8x8_bmp,
													inputFieldFeedback_x,
													inputField_y,
													color,
													"No restriction"
												);
												break;

											case 1:
												printTextFormattedColor(
													font8x8_bmp,
													inputFieldFeedback_x,
													inputField_y,
													color,
													"Race must match"
												);
												break;

											case 2:
												printTextFormattedColor(
													font8x8_bmp,
													inputFieldFeedback_x,
													inputField_y,
													color,
													"Class must match"
												);
												break;

											case 3:
												printTextFormattedColor(
													font8x8_bmp,
													inputFieldFeedback_x,
													inputField_y,
													color,
													"Race AND class"
												);
												break;

											case 4:
												printTextFormattedColor(
													font8x8_bmp,
													inputFieldFeedback_x,
													inputField_y,
													color,
													"Race OR class"
												);
												break;
										}
									}
								}
								else if ( i == 8 )
								{
									const char* raceName = requirementRaceName(propertyInt);

									if ( raceName == nullptr )
									{
										propertyPageError(i, -1);
									}
									else
									{
										printTextFormattedColor(
											font8x8_bmp,
											inputFieldFeedback_x,
											inputField_y,
											color,
											raceName
										);
									}
								}
								else if ( i == 9 )
								{
									const char* className = requirementClassName(propertyInt);

									if ( className == nullptr )
									{
										propertyPageError(i, -1);
									}
									else
									{
										printTextFormattedColor(
											font8x8_bmp,
											inputFieldFeedback_x,
											inputField_y,
											color,
											className
										);
									}
								}
								else if ( i == 10 )
								{
									if ( propertyInt < 0 || propertyInt > 1 )
									{
										propertyPageError(i, 0);
									}
									else if ( propertyInt == 0 )
									{
										printTextFormattedColor(
											font8x8_bmp,
											inputFieldFeedback_x,
											inputField_y,
											color,
											"Power will not activate exit"
										);
									}
									else
									{
										printTextFormattedColor(
											font8x8_bmp,
											inputFieldFeedback_x,
											inputField_y,
											color,
											"Power activates exit"
										);
									}
								}
								else if ( i == 11 )
								{
									if ( propertyInt < 0 || propertyInt > 9999999 )
									{
										propertyPageError(i, 0);
									}
									else if ( propertyInt == 0 )
									{
										printTextFormattedColor(
											font8x8_bmp,
											inputFieldFeedback_x,
											inputField_y,
											colorRandom,
											"Cannot be targeted as a tunnel end"
										);
									}
									else
									{
										printTextFormattedColor(
											font8x8_bmp,
											inputFieldFeedback_x,
											inputField_y,
											color,
											"This tunnel end ID: %d",
											propertyInt
										);
									}
								}
								else if ( i == 12 )
								{
									if ( propertyInt < 0 || propertyInt > 9999999 )
									{
										propertyPageError(i, 0);
									}
									else if ( propertyInt == 0 )
									{
										printTextFormattedColor(
											font8x8_bmp,
											inputFieldFeedback_x,
											inputField_y,
											colorRandom,
											"Destination uses normal Player Start"
										);
									}
									else
									{
										printTextFormattedColor(
											font8x8_bmp,
											inputFieldFeedback_x,
											inputField_y,
											color,
											"Search destination map for tunnel ID %d",
											propertyInt
										);
									}
								}
								else
								{
									// enter other row entries here
								}
							}

							if ( errorMessage )
							{
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, "Invalid ID!");
								}
							}
						}

						propertyPageTextAndInput(numProperties, inputFieldWidth);

						if ( editproperty >= 0 && editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}

							// set the maximum length allowed for user input

						if ( editproperty == 4 )
						{
							inputlen = 32;
						}
						else if ( editproperty == 11 || editproperty == 12 )
						{
							inputlen = 7;
						}
						else
						{
							inputlen = 4;
						}
							propertyPageCursorFlash(spacing);
						}
					}
				}
				else if ( newwindow == 23 )
				{
					if ( selectedEntity[0] != nullptr )
					{
						int numProperties = sizeof(tablePropertyNames) / sizeof(tablePropertyNames[0]); //find number of entries in property list
						const int lenProperties = sizeof(tablePropertyNames[0]) / sizeof(char); //find length of entry in property list
						int spacing = 36; // 36 px between each item in the list.
						int inputFieldHeader_y = suby1 + 28; // 28 px spacing from subwindow start.
						int inputField_x = subx1 + 8; // 8px spacing from subwindow start.
						int inputField_y = inputFieldHeader_y + 16;
						int inputFieldWidth = 64; // width of the text field
						int inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);
						Uint32 colorError = makeColorRGB(255, 0, 0);

						for ( int i = 0; i < numProperties; i++ )
						{
							int propertyInt = atoi(spriteProperties[i]);

							strcpy(tmpPropertyName, tablePropertyNames[i]);
							inputFieldHeader_y = suby1 + 28 + i * spacing;
							inputField_y = inputFieldHeader_y + 16;
							// box outlines then text
							drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
							// print values on top of boxes
							printText(font8x8_bmp, inputField_x, suby1 + 44 + i * spacing, spriteProperties[i]);
							printText(font8x8_bmp, inputField_x, inputFieldHeader_y, tmpPropertyName);

							if ( errorArr[i] != 1 )
							{
								if ( i == 0 )
								{
									if ( propertyInt > 7 || propertyInt < -1 )
									{
										propertyPageError(i, -1); // reset to default -1.
									}
									else
									{
										char tmpStr[32] = "";
										switch ( propertyInt )
										{
											case -1:
												strcpy(tmpStr, "default");
												break;
											case 0:
												strcpy(tmpStr, "East");
												break;
											case 1:
												strcpy(tmpStr, "Southeast");
												break;
											case 2:
												strcpy(tmpStr, "South");
												break;
											case 3:
												strcpy(tmpStr, "Southwest");
												break;
											case 4:
												strcpy(tmpStr, "West");
												break;
											case 5:
												strcpy(tmpStr, "Northwest");
												break;
											case 6:
												strcpy(tmpStr, "North");
												break;
											case 7:
												strcpy(tmpStr, "Northeast");
												break;
											default:
												break;
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else if ( i == 1 )
								{
									if ( propertyInt > 4 || propertyInt < -1 )
									{
										propertyPageError(i, -1); // reset to default -1.
									}
									else
									{
										if ( propertyInt == -1 )
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "random chairs", propertyInt);
										}
										else
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "%d chairs", propertyInt);
										}
									}
								}
								else if ( i == 2 )
								{
									if ( propertyInt > 100 || propertyInt < -1 )
									{
										propertyPageError(i, -1); // reset to default -1.
									}
									else
									{
										if ( propertyInt == -1 )
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "default random item chance");
										}
										else
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "%d%% chance", propertyInt);
										}
									}
								}
								else
								{
									// enter other row entries here
								}
							}

							if ( errorMessage )
							{
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, "Invalid ID!");
								}
							}
						}

						propertyPageTextAndInput(numProperties, inputFieldWidth);

						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}

							// set the maximum length allowed for user input
							if ( editproperty == 2 )
							{
								inputlen = 4;
							}
							else
							{
								inputlen = 3;
							}
							propertyPageCursorFlash(spacing);
						}
					}
				}
				else if ( newwindow == 24 )
				{
						if ( selectedEntity[0] != nullptr )
					{
						int numProperties = sizeof(readableBookPropertyNames) / sizeof(readableBookPropertyNames[0]); //find number of entries in property list
						const int lenProperties = sizeof(readableBookPropertyNames[0]) / sizeof(char); //find length of entry in property list
						int spacing = 36; // 36 px between each item in the list.
						int inputFieldHeader_y = suby1 + 28; // 28 px spacing from subwindow start.
						int inputField_x = subx1 + 8; // 8px spacing from subwindow start.
						int inputField_y = inputFieldHeader_y + 16;
						int inputFieldWidth = 64; // width of the text field
						int inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);
						Uint32 colorError = makeColorRGB(255, 0, 0);

						for ( int i = 0; i < numProperties; i++ )
						{
							int propertyInt = atoi(spriteProperties[i]);

							strcpy(tmpPropertyName, readableBookPropertyNames[i]);
							inputFieldHeader_y = suby1 + 28 + i * spacing;
							inputField_y = inputFieldHeader_y + 16;
							// box outlines then text
							if ( i == 3 )
							{
								inputFieldWidth = subx2 - inputField_x; // width of the text field
							}
							else
							{
								inputFieldWidth = 64; // width of the text field
							}
							drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
							// print values on top of boxes
							printText(font8x8_bmp, inputField_x, suby1 + 44 + i * spacing, spriteProperties[i]);
							printText(font8x8_bmp, inputField_x, inputFieldHeader_y, tmpPropertyName);

							if ( errorArr[i] != 1 )
							{
								if ( i == 0 )
								{
									if ( propertyInt == 2 )
									{
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, makeColorRGB(200, 128, 0), "Decrepit");
									}
									else if ( propertyInt == 3 )
									{
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, makeColorRGB(255, 255, 0), "Worn");
									}
									else if ( propertyInt == 4 )
									{
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, makeColorRGB(128, 200, 0), "Servicable");
									}
									else if ( propertyInt == 5 )
									{
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, makeColorRGB(0, 255, 0), "Excellent");
									}
									else if ( propertyInt == 1 )
									{
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, makeColorRGB(255, 0, 0), "Broken");
									}
									else if ( propertyInt == 0 )
									{
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorRandom, "Random");
									}
									else if ( propertyInt < 0 || propertyInt > 5 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
								}
								else if ( i == 1 )
								{
									if ( strcmp(spriteProperties[i], "00") == 0 )
									{
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorRandom, "Random");
									}
									else if ( propertyInt > 9 || propertyInt < -9 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
								}
								else if ( i == 2 )
								{
									if ( propertyInt == 1 )
									{
										color = makeColorRGB(0, 255, 0);
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "Identified");
									}
									else if ( propertyInt == 0 )
									{
										color = makeColorRGB(255, 255, 0);
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "Unidentified");
									}
									else if ( propertyInt == 2 )
									{
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorRandom, "Random");
									}
									else
									{
										propertyPageError(i, 0); // reset to default 0.
									}
								}
								else
								{
									// enter other row entries here
								}
							}

							if ( errorMessage )
							{
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, "Invalid ID!");
								}
							}
						}

						propertyPageTextAndInput(numProperties, inputFieldWidth);

						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}

							// set the maximum length allowed for user input

							if ( editproperty == 3 )
							{
								inputlen = 48;
							}
							else
							{
								inputlen = 4;
							}
							propertyPageCursorFlash(spacing);
						}
					}
				}
				else if ( newwindow == 26 )
				{
					if ( selectedEntity[0] != NULL )
					{
						int numProperties = sizeof(doorPropertyNames) / sizeof(doorPropertyNames[0]); //find number of entries in property list
						const int lenProperties = sizeof(doorPropertyNames[0]) / sizeof(char); //find length of entry in property list
						int spacing = 36; // 36 px between each item in the list.
						int inputFieldHeader_y = suby1 + 28; // 28 px spacing from subwindow start.
						int inputField_x = subx1 + 8; // 8px spacing from subwindow start.
						int inputField_y = inputFieldHeader_y + 16;
						int inputFieldWidth = 64; // width of the text field
						int inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);
						Uint32 colorError = makeColorRGB(255, 0, 0);

						for ( int i = 0; i < numProperties; i++ )
						{
							int propertyInt = atoi(spriteProperties[i]);

							strcpy(tmpPropertyName, doorPropertyNames[i]);
							inputFieldHeader_y = suby1 + 28 + i * spacing;
							inputField_y = inputFieldHeader_y + 16;
							// box outlines then text
							drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
							// print values on top of boxes
							printText(font8x8_bmp, inputField_x, suby1 + 44 + i * spacing, spriteProperties[i]);
							printText(font8x8_bmp, inputField_x, inputFieldHeader_y, tmpPropertyName);

							if ( errorArr[i] != 1 )
							{
								if ( i == 0 )
								{
									if ( propertyInt > 2 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0 random.
									}
									else
									{
										char tmpStr[32] = "";
										if ( propertyInt == 1 )
										{
											strcpy(tmpStr, "force locked");
										}
										else if ( propertyInt == 2 )
										{
											strcpy(tmpStr, "force unlocked");
										}
										else
										{
											strcpy(tmpStr, "default random");
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else if ( i == 1 || i == 2 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default no disable
									}
									else
									{
										char tmpStr[32] = "";
										if ( propertyInt == 1 )
										{
											strcpy(tmpStr, "disabled");
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else
								{
									// enter other row entries here
								}
							}

							if ( errorMessage )
							{
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, "Invalid ID!");
								}
							}
						}

						propertyPageTextAndInput(numProperties, inputFieldWidth);

						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}

							// set the maximum length allowed for user input
							inputlen = 2;
							propertyPageCursorFlash(spacing);
						}
					}
				}
				else if ( newwindow == 36 )
				{
					if ( selectedEntity[0] != NULL )
					{
						int numProperties = sizeof(doorIronPropertyNames) / sizeof(doorIronPropertyNames[0]); //find number of entries in property list
						const int lenProperties = sizeof(doorIronPropertyNames[0]) / sizeof(char); //find length of entry in property list
						int spacing = 36; // 36 px between each item in the list.
						int inputFieldHeader_y = suby1 + 28; // 28 px spacing from subwindow start.
						int inputField_x = subx1 + 8; // 8px spacing from subwindow start.
						int inputField_y = inputFieldHeader_y + 16;
						int inputFieldWidth = 64; // width of the text field
						int inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);
						Uint32 colorError = makeColorRGB(255, 0, 0);

						for ( int i = 0; i < numProperties; i++ )
						{
							int propertyInt = atoi(spriteProperties[i]);

							strcpy(tmpPropertyName, doorIronPropertyNames[i]);
							inputFieldHeader_y = suby1 + 28 + i * spacing;
							inputField_y = inputFieldHeader_y + 16;
							// box outlines then text
							drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
							// print values on top of boxes
							printText(font8x8_bmp, inputField_x, suby1 + 44 + i * spacing, spriteProperties[i]);
							printText(font8x8_bmp, inputField_x, inputFieldHeader_y, tmpPropertyName);

							if ( errorArr[i] != 1 )
							{
								if ( i == 3 )
								{
									if ( propertyInt > 2 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0 random.
									}
									else
									{
										char tmpStr[32] = "";
										if ( propertyInt == 1 )
										{
											strcpy(tmpStr, "force locked");
										}
										else if ( propertyInt == 2 )
										{
											strcpy(tmpStr, "force unlocked");
										}
										else
										{
											strcpy(tmpStr, "default random");
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else if ( i == 1 || i == 2 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default no disable
									}
									else
									{
										char tmpStr[32] = "";
										if ( propertyInt == 1 )
										{
											strcpy(tmpStr, "disabled");
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else if ( i == 0 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										propertyPageError(i, 1); // reset to default power to unlock
									}
									else
									{
										char tmpStr[32] = "";
										if ( propertyInt == 1 )
										{
											strcpy(tmpStr, "unlock when powered");
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else
								{
									// enter other row entries here
								}
							}

							if ( errorMessage )
							{
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, "Invalid ID!");
								}
							}
						}

						propertyPageTextAndInput(numProperties, inputFieldWidth);

						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}

							// set the maximum length allowed for user input
							inputlen = 2;
							propertyPageCursorFlash(spacing);
						}
					}
				}
				else if ( newwindow == 27 )
				{
					if ( selectedEntity[0] != NULL )
					{
						int numProperties = sizeof(gatePropertyNames) / sizeof(gatePropertyNames[0]); //find number of entries in property list
						const int lenProperties = sizeof(gatePropertyNames[0]) / sizeof(char); //find length of entry in property list
						int spacing = 36; // 36 px between each item in the list.
						int inputFieldHeader_y = suby1 + 28; // 28 px spacing from subwindow start.
						int inputField_x = subx1 + 8; // 8px spacing from subwindow start.
						int inputField_y = inputFieldHeader_y + 16;
						int inputFieldWidth = 64; // width of the text field
						int inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);
						Uint32 colorError = makeColorRGB(255, 0, 0);

						for ( int i = 0; i < numProperties; i++ )
						{
							int propertyInt = atoi(spriteProperties[i]);

							strcpy(tmpPropertyName, gatePropertyNames[i]);
							inputFieldHeader_y = suby1 + 28 + i * spacing;
							inputField_y = inputFieldHeader_y + 16;
							// box outlines then text
							drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
							// print values on top of boxes
							printText(font8x8_bmp, inputField_x, suby1 + 44 + i * spacing, spriteProperties[i]);
							printText(font8x8_bmp, inputField_x, inputFieldHeader_y, tmpPropertyName);

							if ( errorArr[i] != 1 )
							{
								if ( i == 0 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default no disable
									}
									else
									{
										char tmpStr[32] = "";
										if ( propertyInt == 1 )
										{
											strcpy(tmpStr, "disabled");
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else
								{
									// enter other row entries here
								}
							}

							if ( errorMessage )
							{
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, "Invalid ID!");
								}
							}
						}

						propertyPageTextAndInput(numProperties, inputFieldWidth);

						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}

							// set the maximum length allowed for user input
							inputlen = 2;
							propertyPageCursorFlash(spacing);
						}
					}
				}
				else if ( newwindow == 28 )
				{
					if ( selectedEntity[0] != nullptr )
					{
						int numProperties = sizeof(playerSpawnPropertyNames) / sizeof(playerSpawnPropertyNames[0]); //find number of entries in property list
						const int lenProperties = sizeof(playerSpawnPropertyNames[0]) / sizeof(char); //find length of entry in property list
						int spacing = 36; // 36 px between each item in the list.
						int inputFieldHeader_y = suby1 + 28; // 28 px spacing from subwindow start.
						int inputField_x = subx1 + 8; // 8px spacing from subwindow start.
						int inputField_y = inputFieldHeader_y + 16;
						int inputFieldWidth = 64; // width of the text field
						int inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);
						Uint32 colorError = makeColorRGB(255, 0, 0);

						for ( int i = 0; i < numProperties; i++ )
						{
							int propertyInt = atoi(spriteProperties[i]);

							strcpy(tmpPropertyName, playerSpawnPropertyNames[i]);
							inputFieldHeader_y = suby1 + 28 + i * spacing;
							inputField_y = inputFieldHeader_y + 16;
							// box outlines then text
							drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
							// print values on top of boxes
							printText(font8x8_bmp, inputField_x, suby1 + 44 + i * spacing, spriteProperties[i]);
							printText(font8x8_bmp, inputField_x, inputFieldHeader_y, tmpPropertyName);

							if ( errorArr[i] != 1 )
							{
								if ( i == 0 )
								{
									if ( propertyInt > 7 || propertyInt < -1 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										char tmpStr[32] = "";
										switch ( propertyInt )
										{
											case -1:
												strcpy(tmpStr, "random");
												break;
											case 0:
												strcpy(tmpStr, "East");
												break;
											case 1:
												strcpy(tmpStr, "Southeast");
												break;
											case 2:
												strcpy(tmpStr, "South");
												break;
											case 3:
												strcpy(tmpStr, "Southwest");
												break;
											case 4:
												strcpy(tmpStr, "West");
												break;
											case 5:
												strcpy(tmpStr, "Northwest");
												break;
											case 6:
												strcpy(tmpStr, "North");
												break;
											case 7:
												strcpy(tmpStr, "Northeast");
												break;
											default:
												break;
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else
								{
									// enter other row entries here
								}
							}

							if ( errorMessage )
							{
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, "Invalid ID!");
								}
							}
						}

						propertyPageTextAndInput(numProperties, inputFieldWidth);

						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}

							// set the maximum length allowed for user input
							inputlen = 2;
							propertyPageCursorFlash(spacing);
						}
					}
				}
				else if ( newwindow == 29 )
				{
					if ( selectedEntity[0] != nullptr )
					{
						int numProperties = sizeof(statuePropertyNames) / sizeof(statuePropertyNames[0]); //find number of entries in property list
						const int lenProperties = sizeof(statuePropertyNames[0]) / sizeof(char); //find length of entry in property list
						int spacing = 36; // 36 px between each item in the list.
						int inputFieldHeader_y = suby1 + 28; // 28 px spacing from subwindow start.
						int inputField_x = subx1 + 8; // 8px spacing from subwindow start.
						int inputField_y = inputFieldHeader_y + 16;
						int inputFieldWidth = 64; // width of the text field
						int inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);
						Uint32 colorError = makeColorRGB(255, 0, 0);

						for ( int i = 0; i < numProperties; i++ )
						{
							int propertyInt = atoi(spriteProperties[i]);

							strcpy(tmpPropertyName, statuePropertyNames[i]);
							inputFieldHeader_y = suby1 + 28 + i * spacing;
							inputField_y = inputFieldHeader_y + 16;
							// box outlines then text
							if ( i == 1 )
							{
								inputFieldWidth = 80; // width of the text field
							}
							else
							{
								inputFieldWidth = 64; // width of the text field
							}
							drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
							// print values on top of boxes
							printText(font8x8_bmp, inputField_x, suby1 + 44 + i * spacing, spriteProperties[i]);
							printText(font8x8_bmp, inputField_x, inputFieldHeader_y, tmpPropertyName);

							if ( errorArr[i] != 1 )
							{
								if ( i == 0 )
								{
									if ( propertyInt > 3 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										char tmpStr[32] = "";
										switch ( propertyInt )
										{
											case 0:
												strcpy(tmpStr, "East");
												break;
											case 1:
												strcpy(tmpStr, "South");
												break;
											case 2:
												strcpy(tmpStr, "West");
												break;
											case 3:
												strcpy(tmpStr, "North");
												break;
											default:
												break;
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else if ( i == 1 )
								{
									if ( propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
								}
								else
								{
									// enter other row entries here
								}
							}

							if ( errorMessage )
							{
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, "Invalid ID!");
								}
							}
						}

						propertyPageTextAndInput(numProperties, inputFieldWidth);

						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}

							// set the maximum length allowed for user input
							inputlen = 10;
							propertyPageCursorFlash(spacing);
						}
					}
				}
				else if ( newwindow == 30 )
				{
					if ( selectedEntity[0] != nullptr )
					{
						int numProperties = sizeof(shrineTeleportPropertyNames) / sizeof(shrineTeleportPropertyNames[0]); //find number of entries in property list
						const int lenProperties = sizeof(shrineTeleportPropertyNames[0]) / sizeof(char); //find length of entry in property list
						int spacing = 36; // 36 px between each item in the list.
						int inputFieldHeader_y = suby1 + 28; // 28 px spacing from subwindow start.
						int inputField_x = subx1 + 8; // 8px spacing from subwindow start.
						int inputField_y = inputFieldHeader_y + 16;
						int inputFieldWidth = 64; // width of the text field
						int inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);
						Uint32 colorError = makeColorRGB(255, 0, 0);

						for ( int i = 0; i < numProperties; i++ )
						{
							int propertyInt = atoi(spriteProperties[i]);

							strcpy(tmpPropertyName, shrineTeleportPropertyNames[i]);
							inputFieldHeader_y = suby1 + 28 + i * spacing;
							inputField_y = inputFieldHeader_y + 16;
							// box outlines then text
							drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
							// print values on top of boxes
							printText(font8x8_bmp, inputField_x, suby1 + 44 + i * spacing, spriteProperties[i]);
							printText(font8x8_bmp, inputField_x, inputFieldHeader_y, tmpPropertyName);

							if ( errorArr[i] != 1 )
							{
								if ( i == 0 )
								{
									if ( propertyInt > 3 || propertyInt < -1 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										char tmpStr[32] = "";
										switch ( propertyInt )
										{
											case -1:
												strcpy(tmpStr, "random");
												break;
											case 0:
												strcpy(tmpStr, "East");
												break;
											case 1:
												strcpy(tmpStr, "South");
												break;
											case 2:
												strcpy(tmpStr, "West");
												break;
											case 3:
												strcpy(tmpStr, "North");
												break;
											default:
												break;
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else if ( i == 1 || i == 2 || i == 3 )
								{
									if ( propertyInt > 999 || propertyInt < -999 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
								}
								else
								{
									// enter other row entries here
								}
							}

							if ( errorMessage )
							{
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, "Invalid ID!");
								}
							}
						}

						propertyPageTextAndInput(numProperties, inputFieldWidth);

						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}

							// set the maximum length allowed for user input
							inputlen = 4;
							propertyPageCursorFlash(spacing);
						}
					}
				}
				else if ( newwindow == 40 )
				{
					drawMonsterTemplateBrowser();
				}
				else if ( newwindow == 41 )
				{
					drawTextSourceScriptTester();
				}
				else if ( newwindow == 42 )
				{
					drawRoomGroupManager();
				}
				else if ( newwindow == 25 )
				{
					//if ( selectedEntity[0] != nullptr )
					//{
					//	int numProperties = sizeof(playerClassSetterPropertyNames) / sizeof(playerClassSetterPropertyNames[0]); //find number of entries in property list
					//	const int lenProperties = sizeof(playerClassSetterPropertyNames[0]) / sizeof(char); //find length of entry in property list
					//	int spacing = 36; // 36 px between each item in the list.
					//	int inputFieldHeader_y = suby1 + 28; // 28 px spacing from subwindow start.
					//	int inputField_x = subx1 + 8; // 8px spacing from subwindow start.
					//	int inputField_y = inputFieldHeader_y + 16;
					//	int inputFieldWidth = 64; // width of the text field
					//	int inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
					//	char tmpPropertyName[lenProperties] = "";
					//	Uint32 color = makeColorRGB(0, 255, 0);
					//	Uint32 colorRandom = makeColorRGB(0, 168, 255);
					//	Uint32 colorError = makeColorRGB(255, 0, 0);

					//	for ( int i = 0; i < numProperties; i++ )
					//	{
					//		int propertyInt = atoi(spriteProperties[i]);

					//		strcpy(tmpPropertyName, playerClassSetterPropertyNames[i]);
					//		inputFieldHeader_y = suby1 + 28 + i * spacing;
					//		inputField_y = inputFieldHeader_y + 16;
					//		// box outlines then text
					//		inputFieldWidth = 64; // width of the text field
					//		drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
					//		// print values on top of boxes
					//		printText(font8x8_bmp, inputField_x, suby1 + 44 + i * spacing, spriteProperties[i]);
					//		printText(font8x8_bmp, inputField_x, inputFieldHeader_y, tmpPropertyName);

					//		if ( errorArr[i] != 1 )
					//		{
					//			if ( i == 0 )
					//			{
					//				if ( propertyInt < 0 || propertyInt > NUMCLASSES - 1 )
					//				{
					//					propertyPageError(i, 0); // reset to default 0.
					//				}
					//				else
					//				{
					//					printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, playerClassLangEntry(propertyInt, 0));
					//				}
					//			}
					//			else if ( i == 1 )
					//			{
					//				if ( propertyInt > 1 || propertyInt < 0 )
					//				{
					//					propertyPageError(i, 0); // reset to default 0.
					//				}
					//				else if ( propertyInt == 1 )
					//				{
					//					printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "Trigger once only");
					//				}
					//				else if ( propertyInt == 0 )
					//				{
					//					printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "Can re-trigger");
					//				}
					//			}
					//			else
					//			{
					//				// enter other row entries here
					//			}
					//		}

					//		if ( errorMessage )
					//		{
					//			if ( errorArr[i] == 1 )
					//			{
					//				printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, "Invalid ID!");
					//			}
					//		}
					//	}

					//	propertyPageTextAndInput(numProperties, inputFieldWidth);

					//	if ( editproperty < numProperties )   // edit
					//	{
					//		if ( !SDL_IsTextInputActive() )
					//		{
					//			SDL_StartTextInput();
					//			inputstr = spriteProperties[0];
					//		}

					//		// set the maximum length allowed for user input
					//		inputlen = 3;
					//		propertyPageCursorFlash(spacing);
					//	}
					//}
				}
				else if ( newwindow == 31 )
				{
					if ( selectedEntity[0] != nullptr )
					{
						int numProperties = sizeof(colliderDecorationPropertyNames) / sizeof(colliderDecorationPropertyNames[0]); //find number of entries in property list
						const int lenProperties = sizeof(colliderDecorationPropertyNames[0]) / sizeof(char); //find length of entry in property list
						int spacing = 36; // 36 px between each item in the list.
						int inputFieldHeader_y = suby1 + 28; // 28 px spacing from subwindow start.
						int inputField_x = subx1 + 8; // 8px spacing from subwindow start.
						int inputField_y = inputFieldHeader_y + 16;
						int inputFieldWidth = 64; // width of the text field
						int inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);
						Uint32 colorError = makeColorRGB(255, 0, 0);

						for ( int i = 0; i < numProperties; i++ )
						{
							int propertyInt = atoi(spriteProperties[i]);
							inputFieldWidth = 64; // width of the text field
							spacing = 36;

							inputFieldHeader_y = suby1 + 28 + i * spacing;
							inputField_y = inputFieldHeader_y + 16;

							// box outlines then text
							drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
							// print values on top of boxes
							printText(font8x8_bmp, inputField_x, suby1 + 44 + i * spacing, spriteProperties[i]);
							strcpy(tmpPropertyName, colliderDecorationPropertyNames[i]);
							printText(font8x8_bmp, inputField_x, inputFieldHeader_y, tmpPropertyName);

							if ( errorArr[i] != 1 )
							{
								if ( i == 0 || i == 8 )
								{
									if ( propertyInt > 9999 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else if ( i == 0 )
									{
										if ( modelFileNames.find(propertyInt) != modelFileNames.end() )
										{
											std::string tmpStr = modelFileNames[propertyInt];
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr.c_str());
										}
										else
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, makeColorRGB(255, 0, 0), "Unknown Model!");
										}
									}
								}
								else if ( i == 1 )
								{
									if ( propertyInt > 7 || propertyInt < -1 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										char tmpStr[32] = "";
										switch ( propertyInt )
										{
											case -1:
												strcpy(tmpStr, "random");
												break;
											case 0:
												strcpy(tmpStr, "East");
												break;
											case 1:
												strcpy(tmpStr, "Southeast");
												break;
											case 2:
												strcpy(tmpStr, "South");
												break;
											case 3:
												strcpy(tmpStr, "Southwest");
												break;
											case 4:
												strcpy(tmpStr, "West");
												break;
											case 5:
												strcpy(tmpStr, "Northwest");
												break;
											case 6:
												strcpy(tmpStr, "North");
												break;
											case 7:
												strcpy(tmpStr, "Northeast");
												break;
											default:
												break;
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else if ( i == 2 || i == 3 || i == 4 )
								{
									if ( propertyInt > 999 || propertyInt < -999 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
								}
								else if ( i == 5 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										char tmpStr[32] = "";
										switch ( propertyInt )
										{
											case 0:
												strcpy(tmpStr, "No collision");
												break;
											case 1:
												strcpy(tmpStr, "Has collision");
												break;
											default:
												break;
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else if ( i == 6 || i == 7 )
								{
									if ( propertyInt > 8 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
								}
								else if ( i == 8 )
								{
									if ( propertyInt > 999 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										char tmpStr[32] = "";
										switch ( propertyInt )
										{
											case 0:
												strcpy(tmpStr, "Unable to damage");
												break;
											case 1:
												strcpy(tmpStr, "Breakable");
												break;
											default:
												break;
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else if ( i == 9 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										char tmpStr[32] = "";
										switch ( propertyInt )
										{
											case 0:
												strcpy(tmpStr, "Not diggable");
												break;
											case 1:
												strcpy(tmpStr, "Diggable");
												break;
											default:
												break;
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else if ( i == 10 )
								{
									if ( EditorEntityData_t::colliderData.find(propertyInt) ==
										EditorEntityData_t::colliderData.end() )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										char tmpStr[64] = "";
										auto& colliderData = EditorEntityData_t::colliderData[propertyInt];
										snprintf(tmpStr, sizeof(tmpStr), "%s", colliderData.name.c_str());
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else
								{
									// enter other row entries here
								}
							}

							if ( errorMessage )
							{
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, "Invalid ID!");
								}
							}
						}

						propertyPageTextAndInput(numProperties, inputFieldWidth);

						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}

							// set the maximum length allowed for user input
							inputlen = 5;
							propertyPageCursorFlash(spacing);
						}
					}
				}
				else if ( newwindow == 32 )
				{
					if ( selectedEntity[0] != nullptr )
					{
						int numProperties = sizeof(ANDGatePropertyNames) / sizeof(ANDGatePropertyNames[0]); //find number of entries in property list
						const int lenProperties = sizeof(ANDGatePropertyNames[0]) / sizeof(char); //find length of entry in property list
						int spacing = 36; // 36 px between each item in the list.
						int inputFieldHeader_y = suby1 + 28; // 28 px spacing from subwindow start.
						int inputField_x = subx1 + 8; // 8px spacing from subwindow start.
						int inputField_y = inputFieldHeader_y + 16;
						int inputFieldWidth = 64; // width of the text field
						int inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);
						Uint32 colorError = makeColorRGB(255, 0, 0);

						for ( int i = 0; i < numProperties; i++ )
						{
							int propertyInt = atoi(spriteProperties[i]);

							strcpy(tmpPropertyName, ANDGatePropertyNames[i]);
							inputFieldHeader_y = suby1 + 28 + i * spacing;
							inputField_y = inputFieldHeader_y + 16;
							// box outlines then text
							drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
							// print values on top of boxes
							printText(font8x8_bmp, inputField_x, suby1 + 44 + i * spacing, spriteProperties[i]);
							printText(font8x8_bmp, inputField_x, inputFieldHeader_y, tmpPropertyName);

							if ( errorArr[i] != 1 )
							{
								if ( i == 0 )
								{
									if ( propertyInt > 3 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										char tmpStr[8] = "";
										switch ( propertyInt )
										{
										case 0:
											strcpy(tmpStr, "East");
											break;
										case 1:
											strcpy(tmpStr, "South");
											break;
										case 2:
											strcpy(tmpStr, "West");
											break;
										case 3:
											strcpy(tmpStr, "North");
											break;
										default:
											break;
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else if ( i == 1 )
								{
									if ( propertyInt > 9999 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
								}
								else if ( i == 2 )
								{
									if ( propertyInt > 9999 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										if ( propertyInt == 0 )
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "output without on/off toggling");
										}
									}
								}
								else if ( i == 3 )
								{
									if ( propertyInt > 9999 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										if ( propertyInt == 0 )
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "repeat infinite");
										}
									}
								}
								else if ( i == 4 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										if ( propertyInt == 0 )
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "turn off without input signal");
										}
										else if ( propertyInt == 1 )
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "stay on without input signal");
										}
									}
								}
								else if ( i == 5 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										if ( propertyInt == 0 )
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "non-inverted");
										}
										else if ( propertyInt == 1 )
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "output inverted");
										}
									}
								}
								else
								{
									// enter other row entries here
								}
							}

							if ( errorMessage )
							{
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, "Invalid ID!");
								}
							}
						}

						propertyPageTextAndInput(numProperties, inputFieldWidth);

						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}

							// set the maximum length allowed for user input
							inputlen = 4;
							propertyPageCursorFlash(spacing);
						}
					}
				}
				else if ( newwindow == 33 )
				{
					if ( selectedEntity[0] != nullptr )
					{
						int numProperties = sizeof(pressurePlatePropertyNames) / sizeof(pressurePlatePropertyNames[0]); //find number of entries in property list
						const int lenProperties = sizeof(pressurePlatePropertyNames[0]) / sizeof(char); //find length of entry in property list
						int spacing = 36; // 36 px between each item in the list.
						int inputFieldHeader_y = suby1 + 28; // 28 px spacing from subwindow start.
						int inputField_x = subx1 + 8; // 8px spacing from subwindow start.
						int inputField_y = inputFieldHeader_y + 16;
						int inputFieldWidth = 64; // width of the text field
						int inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);
						Uint32 colorError = makeColorRGB(255, 0, 0);

						for ( int i = 0; i < numProperties; i++ )
						{
							int propertyInt = atoi(spriteProperties[i]);

							strcpy(tmpPropertyName, pressurePlatePropertyNames[i]);
							inputFieldHeader_y = suby1 + 28 + i * spacing;
							inputField_y = inputFieldHeader_y + 16;
							// box outlines then text
							drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
							// print values on top of boxes
							printText(font8x8_bmp, inputField_x, suby1 + 44 + i * spacing, spriteProperties[i]);
							printText(font8x8_bmp, inputField_x, inputFieldHeader_y, tmpPropertyName);

							if ( errorArr[i] != 1 )
							{
								if ( i == 0 )
								{
									if ( propertyInt >= Entity::PRESSURE_PLATE_ENUM_END || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										char tmpStr[32] = "";
										switch ( propertyInt )
										{
										case Entity::PRESSURE_PLATE_DEFAULT_ALL:
											strcpy(tmpStr, "Default All");
											break;
										case Entity::PRESSURE_PLATE_PLAYERS:
											strcpy(tmpStr, "Player");
											break;
										case Entity::PRESSURE_PLATE_MONSTERS:
											strcpy(tmpStr, "Any Monster");
											break;
										case Entity::PRESSURE_PLATE_ITEMS:
											strcpy(tmpStr, "Items");
											break;
										case Entity::PRESSURE_PLATE_BOULDERS:
											strcpy(tmpStr, "Boulders");
											break;
										case Entity::PRESSURE_PLATE_PLAYERS_OR_MONSTERS:
											strcpy(tmpStr, "Players/Any Monster");
											break;
										case Entity::PRESSURE_PLATE_PLAYERS_OR_ALLIES:
											strcpy(tmpStr, "Players/Ally Monster");
											break;
										case Entity::PRESSURE_PLATE_MONSTERS_NON_ALLY:
											strcpy(tmpStr, "Non-Ally Monster");
											break;
										default:
											break;
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
																}
								else if ( i == 1 )
								{
									if ( propertyInt < 0 || propertyInt > 4 )
									{
										propertyPageError(i, 0);
									}
									else
									{
										switch ( propertyInt )
										{
											case 0:
												printTextFormattedColor(
													font8x8_bmp,
													inputFieldFeedback_x,
													inputField_y,
													color,
													"No restriction"
												);
												break;

											case 1:
												printTextFormattedColor(
													font8x8_bmp,
													inputFieldFeedback_x,
													inputField_y,
													color,
													"Race must match"
												);
												break;

											case 2:
												printTextFormattedColor(
													font8x8_bmp,
													inputFieldFeedback_x,
													inputField_y,
													color,
													"Class must match"
												);
												break;

											case 3:
												printTextFormattedColor(
													font8x8_bmp,
													inputFieldFeedback_x,
													inputField_y,
													color,
													"Race AND class"
												);
												break;

											case 4:
												printTextFormattedColor(
													font8x8_bmp,
													inputFieldFeedback_x,
													inputField_y,
													color,
													"Race OR class"
												);
												break;

											default:
												break;
										}
									}
								}
									else if ( i == 2 )
									{
										const char* raceName = requirementRaceName(propertyInt);

										if ( raceName == nullptr )
										{
											propertyPageError(i, -1);
										}
										else
										{
											printTextFormattedColor(
												font8x8_bmp,
												inputFieldFeedback_x,
												inputField_y,
												color,
												raceName
											);
										}
									}
									else if ( i == 3 )
									{
										const char* className = requirementClassName(propertyInt);

										if ( className == nullptr )
										{
											propertyPageError(i, -1);
										}
										else
										{
											printTextFormattedColor(
												font8x8_bmp,
												inputFieldFeedback_x,
												inputField_y,
												color,
												className
											);
										}
									}
									else
									{
										// enter other row entries here
									}
							}

							if ( errorMessage )
							{
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, "Invalid ID!");
								}
							}
						}

						propertyPageTextAndInput(numProperties, inputFieldWidth);

						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}

							// set the maximum length allowed for user input
							inputlen = 4;
							propertyPageCursorFlash(spacing);
						}
					}
				}
				else if ( newwindow == 34 )
				{
					if ( selectedEntity[0] != nullptr )
					{
						int numProperties = sizeof(wallLockPropertyNames) / sizeof(wallLockPropertyNames[0]); //find number of entries in property list
						const int lenProperties = sizeof(wallLockPropertyNames[0]) / sizeof(char); //find length of entry in property list
						int spacing = 36; // 36 px between each item in the list.
						int inputFieldHeader_y = suby1 + 28; // 28 px spacing from subwindow start.
						int inputField_x = subx1 + 8; // 8px spacing from subwindow start.
						int inputField_y = inputFieldHeader_y + 16;
						int inputFieldWidth = 64; // width of the text field
						int inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);
						Uint32 colorError = makeColorRGB(255, 0, 0);

						for ( int i = 0; i < numProperties; i++ )
						{
							int propertyInt = atoi(spriteProperties[i]);

							strcpy(tmpPropertyName, wallLockPropertyNames[i]);
							inputFieldHeader_y = suby1 + 28 + i * spacing;
							inputField_y = inputFieldHeader_y + 16;
							// box outlines then text
							drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
							// print values on top of boxes
							printText(font8x8_bmp, inputField_x, suby1 + 44 + i * spacing, spriteProperties[i]);
							printText(font8x8_bmp, inputField_x, inputFieldHeader_y, tmpPropertyName);

							if ( errorArr[i] != 1 )
							{
								if ( i == 0 )
								{
									if ( propertyInt > 7 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										switch ( propertyInt )
										{
										case 0:
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "stone");
											break;
										case 1:
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "bone");
											break;
										case 2:
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "bronze");
											break;
										case 3:
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "iron");
											break;
										case 4:
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "silver");
											break;
										case 5:
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "gold");
											break;
										case 6:
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "crystal");
											break;
										case 7:
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "machine");
											break;
										default:
											break;
										}
									}
								}
								else if ( i == 1 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										if ( propertyInt == 1 )
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "output without key");
										}
									}
								}
								else if ( i == 2 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										if ( propertyInt == 1 )
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "toggleable with key");
										}
									}
								}
								else if ( i == 3 )
								{
									if ( propertyInt > 100 || propertyInt < -1 )
									{
										propertyPageError(i, -1); // reset to default -1
									}
									else
									{
										if ( propertyInt >= 0 )
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "%d skill required", propertyInt);
										}
									}
								}
								else if ( i == 4 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										if ( propertyInt == 1 )
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "skeleton key usable");
										}
									}
								}
								else if ( i == 5 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										if ( propertyInt == 1 )
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "auto gen key");
										}
									}
								}
								else
								{
									// enter other row entries here
								}
							}

							if ( errorMessage )
							{
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, "Invalid ID!");
								}
							}
						}

						propertyPageTextAndInput(numProperties, inputFieldWidth);

						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}

							// set the maximum length allowed for user input
							inputlen = 4;
							propertyPageCursorFlash(spacing);
						}
					}
				}
				else if ( newwindow == 35 )
				{
					if ( selectedEntity[0] != nullptr )
					{
						int numProperties = sizeof(wallButtonPropertyNames) / sizeof(wallButtonPropertyNames[0]); //find number of entries in property list
						const int lenProperties = sizeof(wallButtonPropertyNames[0]) / sizeof(char); //find length of entry in property list
						int spacing = 36; // 36 px between each item in the list.
						int inputFieldHeader_y = suby1 + 28; // 28 px spacing from subwindow start.
						int inputField_x = subx1 + 8; // 8px spacing from subwindow start.
						int inputField_y = inputFieldHeader_y + 16;
						int inputFieldWidth = 64; // width of the text field
						int inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);
						Uint32 colorError = makeColorRGB(255, 0, 0);

						for ( int i = 0; i < numProperties; i++ )
						{
							int propertyInt = atoi(spriteProperties[i]);

							strcpy(tmpPropertyName, wallButtonPropertyNames[i]);
							inputFieldHeader_y = suby1 + 28 + i * spacing;
							inputField_y = inputFieldHeader_y + 16;
							// box outlines then text
							drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
							// print values on top of boxes
							printText(font8x8_bmp, inputField_x, suby1 + 44 + i * spacing, spriteProperties[i]);
							printText(font8x8_bmp, inputField_x, inputFieldHeader_y, tmpPropertyName);

							if ( errorArr[i] != 1 )
							{
								if ( i == 0 )
								{
									if ( propertyInt > 1 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										if ( propertyInt == 1 )
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "powered when unpressed");
										}
									}
								}
								else if ( i == 1 )
								{
									if ( propertyInt > 9999 || propertyInt < 0 )
									{
										propertyPageError(i, 0); // reset to default 0.
									}
									else
									{
										if ( propertyInt == 0 )
										{
											printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, "no reset");
										}
									}
								}
								else
								{
									// enter other row entries here
								}
							}

							if ( errorMessage )
							{
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, "Invalid ID!");
								}
							}
						}

						propertyPageTextAndInput(numProperties, inputFieldWidth);

						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}

							// set the maximum length allowed for user input
							inputlen = 4;
							propertyPageCursorFlash(spacing);
						}
					}
				}
				else if ( newwindow == 38 )
				{
					drawQuestDialogueEditor();
				}
				else if ( newwindow == 37 )
				{
					if ( selectedEntity[0] != nullptr )
					{
						int numProperties = sizeof(windPropertyNames) / sizeof(windPropertyNames[0]); //find number of entries in property list
						const int lenProperties = sizeof(windPropertyNames[0]) / sizeof(char); //find length of entry in property list
						int spacing = 36; // 36 px between each item in the list.
						int inputFieldHeader_y = suby1 + 28; // 28 px spacing from subwindow start.
						int inputField_x = subx1 + 8; // 8px spacing from subwindow start.
						int inputField_y = inputFieldHeader_y + 16;
						int inputFieldWidth = 64; // width of the text field
						int inputFieldFeedback_x = inputField_x + inputFieldWidth + 8;
						char tmpPropertyName[lenProperties] = "";
						Uint32 color = makeColorRGB(0, 255, 0);
						Uint32 colorRandom = makeColorRGB(0, 168, 255);
						Uint32 colorError = makeColorRGB(255, 0, 0);

						for ( int i = 0; i < numProperties; i++ )
						{
							int propertyInt = atoi(spriteProperties[i]);

							strcpy(tmpPropertyName, windPropertyNames[i]);
							inputFieldHeader_y = suby1 + 28 + i * spacing;
							inputField_y = inputFieldHeader_y + 16;
							// box outlines then text
							drawDepressed(inputField_x - 4, inputField_y - 4, inputField_x - 4 + inputFieldWidth, inputField_y + 16 - 4);
							// print values on top of boxes
							printText(font8x8_bmp, inputField_x, suby1 + 44 + i * spacing, spriteProperties[i]);
							printText(font8x8_bmp, inputField_x, inputFieldHeader_y, tmpPropertyName);

							if ( errorArr[i] != 1 )
							{
								if ( i == 0 )
								{
									if ( propertyInt > 9 || propertyInt < -1 )
									{
										propertyPageError(i, -1); // reset to default -1.
									}
									else
									{
										char tmpStr[32] = "";
										switch ( propertyInt )
										{
										case -1:
											strcpy(tmpStr, "random");
											break;
										case 0:
											strcpy(tmpStr, "East");
											break;
										case 1:
											strcpy(tmpStr, "Southeast");
											break;
										case 2:
											strcpy(tmpStr, "South");
											break;
										case 3:
											strcpy(tmpStr, "Southwest");
											break;
										case 4:
											strcpy(tmpStr, "West");
											break;
										case 5:
											strcpy(tmpStr, "Northwest");
											break;
										case 6:
											strcpy(tmpStr, "North");
											break;
										case 7:
											strcpy(tmpStr, "Northeast");
											break;
										default:
											break;
										}
										printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, color, tmpStr);
									}
								}
								else
								{
									// enter other row entries here
								}
							}

							if ( errorMessage )
							{
								if ( errorArr[i] == 1 )
								{
									printTextFormattedColor(font8x8_bmp, inputFieldFeedback_x, inputField_y, colorError, "Invalid ID!");
								}
							}
						}

						propertyPageTextAndInput(numProperties, inputFieldWidth);

						if ( editproperty < numProperties )   // edit
						{
							if ( !SDL_IsTextInputActive() )
							{
								SDL_StartTextInput();
								inputstr = spriteProperties[0];
							}

							// set the maximum length allowed for user input
							inputlen = 2;
							propertyPageCursorFlash(spacing);
						}
					}
				}
				else if ( newwindow == 16 || newwindow == 17 )
				{
					int textColumnLeft = subx1 + 16;
					int textColumnRight = (subx2 - subx1) / 2 + 300;
					int pady = suby1 + 16;
					int spacing = 0;
					Uint32 colorHeader = makeColorRGB(0, 255, 0);
					char helptext[128];

					if ( newwindow == 16 )
					{
						printTextFormattedColor(font8x8_bmp, textColumnLeft, pady + spacing, colorHeader, "Editor File Shortcuts:");
						spacing += 12;
						strcpy(helptext, "New Map:                            CTRL + N");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "Open:                               CTRL + O");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "Save:                               CTRL + S");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "Change Load/Save Directory:         CTRL + D");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "Close Window/Dialogue:              CTRL + M");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "Delete Text:                        Backspace or Grave (`)");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);

						spacing += 16;

						printTextFormattedColor(font8x8_bmp, textColumnLeft, pady + spacing, colorHeader, "Editor Functions:");
						spacing += 12;
						strcpy(helptext, "Open Sprite Window:                 S");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "Open Tile Window:                   T");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "Sprite Properties:                  F2");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "Map Properties:                     CTRL + M");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "Delete Selected Sprite:             DEL");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "Cycle Stacked Sprites:              C");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);

						spacing += 16;

						printTextFormattedColor(font8x8_bmp, textColumnLeft, pady + spacing, colorHeader, "Navigation:");
						spacing += 12;
						strcpy(helptext, "Move Camera/View:                   Arrow Keys");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "Change Current Wall Layer:          SHIFT + Scrollwheel");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "Change Current Wall Layer:          CTRL + U, CTRL + P");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "Toggle First Person Camera:         F");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);

						spacing += 16;

						printTextFormattedColor(font8x8_bmp, textColumnLeft, pady + spacing, colorHeader, "Tile Palette (Last Used Tiles):");
						spacing += 12;
						strcpy(helptext, "Cycle Through Current Tile Palette: Scrollwheel");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "Cycle Through All Palettes:         CTRL + Scrollwheel");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "Choose Specific Tile In Palette:    Numpad 0-9");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "Choose Specific Tile In Palette:    Left Click Tile");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "Lock Changes to Current Palette:    Numpad *");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "Go To Next Palette:                 Numpad +");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "Go To Previous Palette:             Numpad -");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "Clear Tile in Palette:              Right Click Tile");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
					}
					else if ( newwindow == 17 )
					{
						printTextFormattedColor(font8x8_bmp, textColumnLeft, pady + spacing, colorHeader, "Editing Tools:");
						spacing += 20;

						printTextFormattedColor(font8x8_bmp, textColumnLeft, pady + spacing, colorHeader, "Pencil:");
						strcpy(helptext, "        Draws currently selected tile on current wall layer.");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "   Does not select sprites. Right click sets the selected tile");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "   under the cursor to selected.");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 20;
						printTextFormattedColor(font8x8_bmp, textColumnLeft, pady + spacing, colorHeader, "Point:");
						strcpy(helptext, "       Selects sprites only. Sprites can be moved or deleted once");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "   placed and selected with this tool. Left click selects, right");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "   clicking duplicates a sprite and places it the cursor.");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 20;
						strcpy(helptext, "   When sprites are stacked, only the lowest listed sprite is");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "   selected. Hovering over multiple sprites and cycling with C");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "   allows you to change the order that sprites are drawn in");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "   the editor.");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 20;
						strcpy(helptext, "   Certain sprites like monsters, chests, boulder traps, and most");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "   Blessed Addition sprites (sprite 75 and onwards) have extra");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "   customisable properties when F2 is pressed while the sprite");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "   is selected using this tool. If no sprite is selected, F2");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "   will show properties of the last sprite selected.");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);

						spacing += 20;
						printTextFormattedColor(font8x8_bmp, textColumnLeft, pady + spacing, colorHeader, "Brush:");
						strcpy(helptext, "       Same as pencil, but draws a larger area at once.");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);

						spacing += 20;
						printTextFormattedColor(font8x8_bmp, textColumnLeft, pady + spacing, colorHeader, "Select:");
						strcpy(helptext, "        Selects area of tiles or sprites. Tiles can be copied/");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "   pasted/deleted in groups. Sprites can be moved in groups");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "   with ALT + Arrow Keys. Selection can be moved with CTRL + ");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
						strcpy(helptext, "   Arrow Keys, and resized with SHIFT + Arrow Keys.\n");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);

						spacing += 20;
						printTextFormattedColor(font8x8_bmp, textColumnLeft, pady + spacing, colorHeader, "Fill:");
						strcpy(helptext, "      Fills in left-clicked area with currently selected tile.");
						printTextFormatted(font8x8_bmp, textColumnLeft, pady + spacing, helptext);
						spacing += 12;
					}
				}

				if ( keystatus[SDLK_ESCAPE] )
				{
					keystatus[SDLK_ESCAPE] = 0;
					if ( newwindow > 1 )
					{
						//buttonCloseSpriteSubwindow(NULL);
					}
					else if ( openwindow > 0 || savewindow == 1 )
					{
						buttonCloseSubwindow(NULL);
					}
					if ( newwindow == 16 || newwindow == 17
						|| newwindow == 42 )
					{
						buttonCloseSubwindow(NULL);
					}
				}
				if ( keystatus[SDLK_RETURN] )
				{
					keystatus[SDLK_RETURN] = 0;
					if ( newwindow > 1 )
					{
						//buttonSpritePropertiesConfirm(NULL);
					}
					else if ( openwindow == 1 )
					{
						buttonOpenConfirm(NULL);
					}
					else if ( savewindow == 1 )
					{
						//buttonSaveConfirm(NULL);
					}
					if ( newwindow == 16 )
					{
						buttonEditorToolsHelp(nullptr);
					}
					else if ( newwindow == 17 )
					{
						buttonCloseSubwindow(nullptr);
					}
				}
			}
			else
			{
				if ( SDL_IsTextInputActive() )
				{
					SDL_StopTextInput();
				}

				// handle hotkeys
				if ( keystatus[SDLK_LCTRL] || keystatus[SDLK_RCTRL] )
				{
					if ( keystatus[SDLK_n] && !keystatus[SDLK_LSHIFT] && !keystatus[SDLK_RSHIFT] )
					{
						keystatus[SDLK_n] = 0;
						buttonNew(NULL);
						groupedEntities.clear();
					}
					if ( keystatus[SDLK_s] )
					{
						keystatus[SDLK_s] = 0;
						buttonSave(NULL);
					}
					if ( keystatus[SDLK_o] )
					{
						keystatus[SDLK_o] = 0;
						buttonOpen(NULL);
						groupedEntities.clear();
					}
					if ( keystatus[SDLK_x] )
					{
						keystatus[SDLK_x] = 0;
						buttonCut(NULL);
					}
					if ( keystatus[SDLK_c] )
					{
						keystatus[SDLK_c] = 0;
						buttonCopy(NULL);
						groupedEntities.clear();
					}
					if ( keystatus[SDLK_v] )
					{
						keystatus[SDLK_v] = 0;
						buttonPaste(NULL);
						groupedEntities.clear();
					}
					if ( keystatus[SDLK_a] )
					{
						keystatus[SDLK_a] = 0;
						buttonSelectAll(NULL);
						reselectEntityGroup();
					}
					if ( keystatus[SDLK_z] )
					{
						keystatus[SDLK_z] = 0;
						buttonUndo(NULL);
						groupedEntities.clear();
					}
					if ( keystatus[SDLK_y] )
					{
						keystatus[SDLK_y] = 0;
						buttonRedo(NULL);
						groupedEntities.clear();
					}
					if ( keystatus[SDLK_g] )
					{
						keystatus[SDLK_g] = 0;
						buttonGrid(NULL);
					}
					if ( keystatus[SDLK_d] )
					{
						keystatus[SDLK_d] = 0;
						buttonOpenDirectory(NULL);
					}
					if ( keystatus[SDLK_t] )
					{
						keystatus[SDLK_t] = 0;
						buttonToolbox(NULL);
					}
					if ( keystatus[SDLK_e] )
					{
						keystatus[SDLK_e] = 0;
						buttonViewSprites(NULL);
					}
					if ( keystatus[SDLK_l] )
					{
						keystatus[SDLK_l] = 0;
						buttonAllLayers(NULL);
					}
					if ( keystatus[SDLK_h] )
					{
						keystatus[SDLK_h] = 0;
						buttonHoverText(NULL);
					}
					if ( keystatus[SDLK_i] )
					{
						keystatus[SDLK_i] = 0;
						buttonStatusBar(NULL);
					}
					if ( keystatus[SDLK_m] )
					{
						keystatus[SDLK_m] = 0;
						buttonAttributes(NULL);
					}
					// Cycle layer up.
					if ( keystatus[SDLK_u] )
					{
						keystatus[SDLK_u] = 0;

						const int oldLayer = drawlayer;
						drawlayer = std::min(drawlayer + 1, MAPLAYERS - 1);

						if ( drawlayer != oldLayer )
						{
							// Do not keep a single sprite selected after leaving its layer.
							if ( selectedEntity[0]
								&& entityAuthoredSpriteLayer(selectedEntity[0]) != drawlayer )
							{
								selectedEntity[0] = nullptr;
							}

							// Rebuild group selection so it only contains sprites
							// on the newly viewed layer.
							reselectEntityGroup();
						}
					}

					// Cycle layer down.
					if ( keystatus[SDLK_p] )
					{
						keystatus[SDLK_p] = 0;

						const int oldLayer = drawlayer;
						drawlayer = std::max(drawlayer - 1, 0);

						if ( drawlayer != oldLayer )
						{
							if ( selectedEntity[0]
								&& entityAuthoredSpriteLayer(selectedEntity[0]) != drawlayer )
							{
								selectedEntity[0] = nullptr;
							}

							reselectEntityGroup();
						}
					}
					if ( keystatus[SDLK_LSHIFT] || keystatus[SDLK_RSHIFT] )
					{
						if ( keystatus[SDLK_n] )
						{
							keystatus[SDLK_n] = 0;
							buttonClearMap(NULL);
							groupedEntities.clear();
						}
					}
					if ( keystatus[SDLK_DOWN] )
					{
						keystatus[SDLK_DOWN] = 0;
						// move selection
						if ( selectedarea_y2 < map.height - 1 )
						{
							selectedarea_y2 += 1;
							if ( selectedarea_y1 < map.height - 1 )
							{
								selectedarea_y1 += 1;
							}
							reselectEntityGroup();
						}
					}
					else if ( keystatus[SDLK_UP] )
					{
						keystatus[SDLK_UP] = 0;
						// move selection
						if ( selectedarea_y1 > 0 )
						{
							selectedarea_y1 -= 1;
							if ( selectedarea_y2 > 0 )
							{
								selectedarea_y2 -= 1;
							}
							reselectEntityGroup();
						}
					}
					else if ( keystatus[SDLK_LEFT] )
					{
						keystatus[SDLK_LEFT] = 0;
						// move selection
						if ( selectedarea_x1 > 0 )
						{
							selectedarea_x1 -= 1;
							if ( selectedarea_x2 > 0 )
							{
								selectedarea_x2 -= 1;
							}
							reselectEntityGroup();
						}
					}
					else if ( keystatus[SDLK_RIGHT] )
					{
						keystatus[SDLK_RIGHT] = 0;
						// move selection
						if ( selectedarea_x2 < map.width - 1 )
						{
							selectedarea_x2 += 1;
							if ( selectedarea_x1 < map.width - 1 )
							{
								selectedarea_x1 += 1;
							}
							reselectEntityGroup();
						}
					}
				}
				else
				{
					if ( keystatus[SDLK_LSHIFT] || keystatus[SDLK_RSHIFT] )
					{
						if ( keystatus[SDLK_DOWN] )
						{
							keystatus[SDLK_DOWN] = 0;
							// resize selection
							if ( selectedarea_y2 < map.height - 1 && !moveSelectionNegativeY )
							{
								selectedarea_y2 += 1;
								reselectEntityGroup();
							}
							else if ( selectedarea_y1 < selectedarea_y2
								&& selectedarea_y1 < map.height - 1 && moveSelectionNegativeY )
							{
								selectedarea_y1 += 1;
								reselectEntityGroup();
							}
							else if ( selectedarea_y1 == selectedarea_y2 )
							{
								moveSelectionNegativeY = false;
								if ( selectedarea_y2 < map.height - 1 )
								{
									selectedarea_y2 += 1;
									reselectEntityGroup();
								}
							}
						}
						else if ( keystatus[SDLK_UP] )
						{
							keystatus[SDLK_UP] = 0;
							// resize selection
							if ( selectedarea_y2 > selectedarea_y1 && !moveSelectionNegativeY )
							{
								selectedarea_y2 -= 1;
								reselectEntityGroup();
							}
							else if ( selectedarea_y1 < selectedarea_y2 
								&& selectedarea_y1 > 0 && moveSelectionNegativeY )
							{
								selectedarea_y1 -= 1;
								reselectEntityGroup();
							}
							else if ( selectedarea_y1 == selectedarea_y2 )
							{
								moveSelectionNegativeY = true;
								if ( selectedarea_y1 > 0 )
								{
									selectedarea_y1 -= 1;
									reselectEntityGroup();
								}
							}
						}
						else if ( keystatus[SDLK_LEFT] )
						{
							keystatus[SDLK_LEFT] = 0;
							// resize selection
							if ( selectedarea_x2 > selectedarea_x1 && !moveSelectionNegativeX )
							{
								selectedarea_x2 -= 1;
								reselectEntityGroup();
							}
							else if ( selectedarea_x1 < selectedarea_x2
								&& selectedarea_x1 > 0 && moveSelectionNegativeX )
							{
								selectedarea_x1 -= 1;
								reselectEntityGroup();
							}
							else if ( selectedarea_x1 == selectedarea_x2 )
							{
								moveSelectionNegativeX = true;
								if ( selectedarea_x1 > 0 )
								{
									selectedarea_x1 -= 1;
									reselectEntityGroup();
								}
							}
						}
						else if ( keystatus[SDLK_RIGHT] )
						{
							keystatus[SDLK_RIGHT] = 0;
							// resize selection
							if ( selectedarea_x2 < map.width - 1 && !moveSelectionNegativeX)
							{
								selectedarea_x2 += 1;
								reselectEntityGroup();
							}
							else if ( selectedarea_x1 < selectedarea_x2
								&& selectedarea_x1 < map.width - 1 && moveSelectionNegativeX )
							{
								selectedarea_x1 += 1;
								reselectEntityGroup();
							}
							else if ( selectedarea_x1 == selectedarea_x2 )
							{
								moveSelectionNegativeX = false;
								if ( selectedarea_x2 < map.width - 1 )
								{
									selectedarea_x2 += 1;
									reselectEntityGroup();
								}
							}
						}
					}
					if ( keystatus[SDLK_s] )
					{
						keystatus[SDLK_s] = 0;
						spritepalette = 1;
					}
					if ( keystatus[SDLK_t] )
					{
						keystatus[SDLK_t] = 0;
						tilepalette = 1;
					}
					if ( keystatus[SDLK_f] )
					{
						keystatus[SDLK_f] = 0;
						button3DMode(NULL);
					}
				}
				if ( pasting && keystatus[SDLK_ESCAPE] )
				{
					keystatus[SDLK_ESCAPE] = 0;
					editorRoomCancelPaste();
				}

				if ( keystatus[SDLK_LALT] || keystatus[SDLK_RALT] )
				{
					if ( keystatus[SDLK_f] )
					{
						keystatus[SDLK_f] = 0;
						menuVisible = 1;
					}
					if ( keystatus[SDLK_e] )
					{
						keystatus[SDLK_e] = 0;
						menuVisible = 2;
					}
					if ( keystatus[SDLK_v] )
					{
						keystatus[SDLK_v] = 0;
						menuVisible = 3;
					}
					if ( keystatus[SDLK_m] )
					{
						keystatus[SDLK_m] = 0;
						menuVisible = 4;
					}
					if ( keystatus[SDLK_h] )
					{
						keystatus[SDLK_h] = 0;
						menuVisible = 5;
					}
					if ( keystatus[SDLK_F4] )
					{
						keystatus[SDLK_F4] = 0;
						buttonExit(NULL);
					}
					if ( keystatus[SDLK_DOWN] )
					{
						keystatus[SDLK_DOWN] = 0;
						// move entities
						makeUndo();
						if ( selectedarea_y2 < map.height - 1 )
						{
							for ( std::vector<Entity*>::iterator it = groupedEntities.begin(); it != groupedEntities.end(); ++it )
							{
								Entity* tmpEntity = *it;
								tmpEntity->y += 16;
							}
							selectedarea_y2 += 1;
							if ( selectedarea_y1 < map.height - 1 )
							{
								selectedarea_y1 += 1;
							}
						}
					}
					else if ( keystatus[SDLK_UP] )
					{
						keystatus[SDLK_UP] = 0;
						// move entities
						makeUndo();
						if ( selectedarea_y1 > 0 )
						{
							for ( std::vector<Entity*>::iterator it = groupedEntities.begin(); it != groupedEntities.end(); ++it )
							{
								Entity* tmpEntity = *it;
								tmpEntity->y -= 16;
							}
							selectedarea_y1 -= 1;
							if ( selectedarea_y2 > 0 )
							{
								selectedarea_y2 -= 1;
							}
						}
					}
					else if ( keystatus[SDLK_LEFT] )
					{
						keystatus[SDLK_LEFT] = 0;
						// move entities
						makeUndo();
						if ( selectedarea_x1 > 0 )
						{
							for ( std::vector<Entity*>::iterator it = groupedEntities.begin(); it != groupedEntities.end(); ++it )
							{
								Entity* tmpEntity = *it;
								tmpEntity->x -= 16;
							}
							selectedarea_x1 -= 1;
							if ( selectedarea_x2 > 0 )
							{
								selectedarea_x2 -= 1;
							}
						}
					}
					else if ( keystatus[SDLK_RIGHT] )
					{
						keystatus[SDLK_RIGHT] = 0;
						// move entities
						makeUndo();
						if ( selectedarea_x2 < map.width - 1 )
						{
							for ( std::vector<Entity*>::iterator it = groupedEntities.begin(); it != groupedEntities.end(); ++it )
							{
								Entity* tmpEntity = *it;
								tmpEntity->x += 16;
							}
							selectedarea_x2 += 1;
							if ( selectedarea_x1 < map.width - 1 )
							{
								selectedarea_x1 += 1;
							}
						}
					}
				}
				if ( keystatus[SDLK_DELETE] )
				{
					keystatus[SDLK_DELETE] = 0;
					buttonDelete(NULL);
					groupedEntities.clear();
				}
				if ( keystatus[SDLK_c] )
				{
					keystatus[SDLK_c] = 0;
					buttonCycleSprites(NULL);
				}
				if ( keystatus[SDLK_F1] )
				{
					keystatus[SDLK_F1] = 0;
					buttonAbout(NULL);
				}
				if ( keystatus[SDLK_h] )
				{
					keystatus[SDLK_h] = 0;
					buttonEditorControls(NULL);
				}
				if ( keystatus[SDLK_1] ) // Switch to Pencil Tool
				{
					keystatus[SDLK_1] = 0;
					selectedTool = 0;
					selectedarea = false;
				}
				if ( keystatus[SDLK_2] ) // Switch to Point Tool
				{
					keystatus[SDLK_2] = 0;
					selectedTool = 1;
					selectedarea = false;
				}
				if ( keystatus[SDLK_3] ) // Switch to Brush Tool
				{
					keystatus[SDLK_3] = 0;
					selectedTool = 2;
					selectedarea = false;
				}
				if ( keystatus[SDLK_4] ) // Switch to Select Tool
				{
					keystatus[SDLK_4] = 0;
					selectedTool = 3;
					selectedarea = false;
				}
				if ( keystatus[SDLK_5] ) // Switch to Fill Tool
				{
					keystatus[SDLK_5] = 0;
					selectedTool = 4;
					selectedarea = false;
				}
				if ( keystatus[SDLK_F2] )
				{
					keystatus[SDLK_F2] = 0;
					makeUndo();
					buttonSpriteProperties(NULL);
				}
				if ( keystatus[SDLK_KP_7] )
				{
					keystatus[SDLK_KP_7] = 0;
					selectedTile = recentUsedTiles[recentUsedTilePalette][0];
				}
				if ( keystatus[SDLK_KP_8] )
				{
					keystatus[SDLK_KP_8] = 0;
					selectedTile = recentUsedTiles[recentUsedTilePalette][1];
				}
				if ( keystatus[SDLK_KP_9] )
				{
					keystatus[SDLK_KP_9] = 0;
					selectedTile = recentUsedTiles[recentUsedTilePalette][2];
				}
				if ( keystatus[SDLK_KP_4] )
				{
					keystatus[SDLK_KP_4] = 0;
					selectedTile = recentUsedTiles[recentUsedTilePalette][3];
				}
				if ( keystatus[SDLK_KP_5] )
				{
					keystatus[SDLK_KP_5] = 0;
					selectedTile = recentUsedTiles[recentUsedTilePalette][4];
				}
				if ( keystatus[SDLK_KP_6] )
				{
					keystatus[SDLK_KP_6] = 0;
					selectedTile = recentUsedTiles[recentUsedTilePalette][5];
				}
				if ( keystatus[SDLK_KP_1] )
				{
					keystatus[SDLK_KP_1] = 0;
					selectedTile = recentUsedTiles[recentUsedTilePalette][6];
				}
				if ( keystatus[SDLK_KP_2] )
				{
					keystatus[SDLK_KP_2] = 0;
					selectedTile = recentUsedTiles[recentUsedTilePalette][7];
				}
				if ( keystatus[SDLK_KP_3] )
				{
					keystatus[SDLK_KP_3] = 0;
					selectedTile = recentUsedTiles[recentUsedTilePalette][8];
				}
				if ( keystatus[SDLK_KP_PLUS] )
				{
					keystatus[SDLK_KP_PLUS] = 0;
					recentUsedTilePalette++; //scroll through palettes 1-9
					if ( recentUsedTilePalette == 9 )
					{
						recentUsedTilePalette = 0;
					}
				}
				if ( keystatus[SDLK_KP_MINUS] )
				{
					keystatus[SDLK_KP_MINUS] = 0;
					recentUsedTilePalette--; //scroll through palettes 1-9
					if ( recentUsedTilePalette == -1 )
					{
						recentUsedTilePalette = 8;
					}
				}
				if ( keystatus[SDLK_KP_MULTIPLY] )
				{
					keystatus[SDLK_KP_MULTIPLY] = 0;
					lockTilePalette[recentUsedTilePalette] = !lockTilePalette[recentUsedTilePalette]; // toggle lock/unlock
				}
				if ( keystatus[SDLK_F5] )
				{
					keystatus[SDLK_F5] = 0;
					buttonOpenPrevMap(nullptr);
				}
				if ( keystatus[SDLK_F8] )
				{
					keystatus[SDLK_F8] = 0;
					buttonOpenNextMap(nullptr);
				}
			}
			// process and draw buttons
			handleButtons();
		}

        if ( spritepalette || tilepalette )
        {
            const int paletteType = spritepalette ? 1 : 2;
            char* searchBuffer = paletteType == 1 ? editorSpritePaletteSearch : editorTilePaletteSearch;
            editorPaletteBeginTextInput(searchBuffer);
            editorPaletteRebuildMatches(paletteType);

            const int headerHeight = 32;
            const int footerHeight = 24;
            const int cellSize = 64;
            const int columns = std::max(1, xres / cellSize);
            const int visibleRows = std::max(1, (yres - headerHeight - footerHeight) / cellSize);
            const int pageSize = columns * visibleRows;

            editorPaletteHandleKeyboard(columns, visibleRows);
            if ( scroll != 0 )
            {
                const int firstRow = editorPaletteFirstVisible / columns;
                const int rowCount = editorPaletteMatches.empty()
                    ? 0 : (static_cast<int>(editorPaletteMatches.size()) - 1) / columns + 1;
                const int maxFirstRow = std::max(0, rowCount - visibleRows);
                const int newFirstRow = std::max(0, std::min(firstRow - scroll, maxFirstRow));
                editorPaletteFirstVisible = newFirstRow * columns;
                editorPaletteSelectedMatch = std::max(editorPaletteFirstVisible,
                    std::min(editorPaletteSelectedMatch, editorPaletteFirstVisible + pageSize - 1));
                scroll = 0;
            }
            editorPaletteKeepSelectionVisible(columns, visibleRows);

            drawRect(nullptr, makeColorRGB(0, 0, 0), 255);
            drawWindowFancy(4, 4, xres - 4, 28);
            printText(font8x8_bmp, 10, 12, "Search:");
            drawDepressed(66, 8, xres - 154, 24);
            printText(font8x8_bmp, 70, 12, searchBuffer);
            if ( (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
            {
                const int cursorX = std::min(xres - 162, 70 + static_cast<int>(strlen(searchBuffer)) * 8);
                printText(font8x8_bmp, cursorX, 12, "\26");
            }
            printTextFormatted(font8x8_bmp, xres - 146, 12, "%d match%s",
                static_cast<int>(editorPaletteMatches.size()), editorPaletteMatches.size() == 1 ? "" : "es");

            if ( keystatus[SDLK_DELETE] && SDL_GetModState() & KMOD_CTRL )
            {
                keystatus[SDLK_DELETE] = 0;
                searchBuffer[0] = '\0';
                editorPaletteLastFilter.clear();
                editorPaletteRebuildMatches(paletteType);
            }

            int hoveredMatch = -1;
            const int visibleEnd = std::min(static_cast<int>(editorPaletteMatches.size()),
                editorPaletteFirstVisible + pageSize);
            for ( int matchIndex = editorPaletteFirstVisible; matchIndex < visibleEnd; ++matchIndex )
            {
                const int localIndex = matchIndex - editorPaletteFirstVisible;
                const int column = localIndex % columns;
                const int row = localIndex / columns;
                const int cellX = column * cellSize;
                const int cellY = headerHeight + row * cellSize;
                const int objectIndex = editorPaletteMatches[matchIndex];

                SDL_Rect cellRect;
                cellRect.x = cellX;
                cellRect.y = cellY;
                cellRect.w = cellSize;
                cellRect.h = cellSize;
                if ( matchIndex == editorPaletteSelectedMatch )
                {
                    drawRect(&cellRect, makeColorRGB(72, 72, 72), 255);
                }

                SDL_Surface* image = nullptr;
                if ( paletteType == 1 )
                {
                    const int visualIndex =
                        editorPaletteSpriteVisual(objectIndex);
                    if ( visualIndex >= 0 && visualIndex < numsprites )
                    {
                        image = sprites[visualIndex] != nullptr ? sprites[visualIndex] : sprites[0];
                    }
                }
                else if ( objectIndex >= 0 && objectIndex < numtiles )
                {
                    image = tiles[objectIndex] != nullptr ? tiles[objectIndex] : sprites[0];
                }

                if ( image != nullptr )
                {
                    const int maxImageSize = cellSize - 8;
                    const real_t scaleX = static_cast<real_t>(maxImageSize) / std::max(1, image->w);
                    const real_t scaleY = static_cast<real_t>(maxImageSize) / std::max(1, image->h);
                    const real_t imageScale = std::min<real_t>(1.0, std::min(scaleX, scaleY));
                    pos.w = std::max(1, static_cast<int>(image->w * imageScale));
                    pos.h = std::max(1, static_cast<int>(image->h * imageScale));
                    pos.x = cellX + (cellSize - pos.w) / 2;
                    pos.y = cellY + (cellSize - pos.h) / 2;
                    drawImageScaled(image, nullptr, &pos);
                }

                if ( mousex >= cellX && mousex < cellX + cellSize
                    && mousey >= cellY && mousey < cellY + cellSize )
                {
                    hoveredMatch = matchIndex;
                    editorPaletteSelectedMatch = matchIndex;
                }
            }

            bool chooseCurrent = false;
            if ( mousestatus[SDL_BUTTON_LEFT] )
            {
                mclick = 1;
            }
            if ( !mousestatus[SDL_BUTTON_LEFT] && mclick )
            {
                mclick = 0;
                if ( hoveredMatch >= 0 )
                {
                    editorPaletteSelectedMatch = hoveredMatch;
                    chooseCurrent = true;
                }
            }
            if ( keystatus[SDLK_RETURN] || keystatus[SDLK_KP_ENTER] )
            {
                keystatus[SDLK_RETURN] = 0;
                keystatus[SDLK_KP_ENTER] = 0;
                chooseCurrent = !editorPaletteMatches.empty();
            }

            if ( chooseCurrent && !editorPaletteMatches.empty() )
            {
                const int objectIndex = editorPaletteMatches[editorPaletteSelectedMatch];
                if ( paletteType == 1 )
                {
                    /*
                     * Virtual Z stairs behave like ordinary sprite palette
                     * entries: selecting one always gives the editor a held
                     * sprite. If the current authored layer cannot host that
                     * stair direction, move to the nearest valid layer rather
                     * than rejecting the palette selection.
                     */
                    const int originalDrawLayer = drawlayer;
                    if ( objectIndex == EDITOR_VIRTUAL_Z_STAIR_UP )
                    {
                        drawlayer = std::max(
                            OBSTACLELAYER,
                            std::min(drawlayer, MAPLAYERS - 3));
                    }
                    else if ( objectIndex == EDITOR_VIRTUAL_Z_STAIR_DOWN )
                    {
                        drawlayer = std::max(
                            OBSTACLELAYER + 1,
                            std::min(drawlayer, MAPLAYERS - 1));
                    }
                    if ( drawlayer != originalDrawLayer )
                    {
                        reselectEntityGroup();
                    }

                    const int spriteIndex = editorIsVirtualZStair(objectIndex)
                        ? editorVirtualSpriteVisual(objectIndex)
                        : objectIndex;
                    entity = newEntity(spriteIndex, 0, map.entities, nullptr);
                    selectedEntity[0] = entity;
                    lastSelectedEntity[0] = entity;
                    setSpriteAttributes(entity, nullptr, nullptr);
                    entity->authoredMapLayer = static_cast<Sint16>(
                        std::max(0, std::min(drawlayer, MAPLAYERS - 1)));
                    if ( objectIndex == EDITOR_VIRTUAL_Z_STAIR_UP )
                    {
                        entity->verticalLayerTransitionDelta = 1;
                        entity->verticalLayerTransitionModel = 161;
                        entity->verticalLayerTransitionRotation = 0;
                        entity->floorDecorationHeightOffset = 0;
                        entity->floorDecorationXOffset = 0;
                        entity->floorDecorationYOffset = 0;
                        entity->floorDecorationDestroyIfNoWall = -1;
                        entity->playableFloorTransitionEnabled = false;
                        entity->playableFloorTransitionTargetPersistentID = 0;
                        if ( drawlayer != originalDrawLayer )
                        {
                            snprintf(message, sizeof(message),
                                "Z STAIR UP selected; switched map layer %d -> %d for valid placement.",
                                originalDrawLayer, drawlayer);
                        }
                        else
                        {
                            snprintf(message, sizeof(message),
                                "Z STAIR UP selected on map layer %d -> %d.",
                                drawlayer, drawlayer + 1);
                        }
                        messagetime = 100;
                    }
                    else if ( objectIndex == EDITOR_VIRTUAL_Z_STAIR_DOWN )
                    {
                        entity->verticalLayerTransitionDelta = -1;
                        entity->verticalLayerTransitionModel = 253;
                        entity->verticalLayerTransitionRotation = 0;
                        entity->floorDecorationHeightOffset = 0;
                        entity->floorDecorationXOffset = 0;
                        entity->floorDecorationYOffset = 0;
                        entity->floorDecorationDestroyIfNoWall = -1;
                        entity->playableFloorTransitionEnabled = false;
                        entity->playableFloorTransitionTargetPersistentID = 0;
                        if ( drawlayer != originalDrawLayer )
                        {
                            snprintf(message, sizeof(message),
                                "Z STAIR DOWN selected; switched map layer %d -> %d for valid placement.",
                                originalDrawLayer, drawlayer);
                        }
                        else
                        {
                            snprintf(message, sizeof(message),
                                "Z STAIR DOWN selected on map layer %d -> %d.",
                                drawlayer, drawlayer - 1);
                        }
                        messagetime = 100;
                    }
                    spritepalette = 0;
                }
                else
                {
                    selectedTile = objectIndex;
                    updateRecentTileList(selectedTile);
                    tilepalette = 0;
                }
                editorPaletteEndTextInput();
            }

            if ( keystatus[SDLK_ESCAPE] )
            {
                keystatus[SDLK_ESCAPE] = 0;
                mclick = 0;
                spritepalette = 0;
                tilepalette = 0;
                editorPaletteEndTextInput();
            }

            const int infoIndex = hoveredMatch >= 0 ? hoveredMatch : editorPaletteSelectedMatch;
            if ( !editorPaletteMatches.empty() && infoIndex >= 0
                && infoIndex < static_cast<int>(editorPaletteMatches.size()) )
            {
                const int objectIndex = editorPaletteMatches[infoIndex];
                const char* displayName = nullptr;
                if ( paletteType == 1 )
                {
                    displayName = editorIsVirtualZStair(objectIndex)
                        ? editorVirtualSpriteName(objectIndex)
                        : spriteEditorNameStrings[objectIndex];
                }
                else
                {
                    displayName = tileEditorNameStrings[objectIndex];
                }
                if ( paletteType == 1 && editorIsVirtualZStair(objectIndex) )
                {
                    printTextFormatted(font8x8_bmp, 4, yres - 20, "Layer stair: %s", displayName);
                }
                else
                {
                    printTextFormatted(font8x8_bmp, 4, yres - 20, "%s %d: %s",
                        paletteType == 1 ? "Sprite" : "Tile", objectIndex, displayName);
                }
            }
            else
            {
                printText(font8x8_bmp, 4, yres - 20, "No matching entries. Backspace to edit; Ctrl+Delete clears search.");
            }
            printText(font8x8_bmp, 4, yres - 10,
                "Arrows navigate | Wheel/Page Up/Page Down scroll | Home/End jump | Enter selects | Esc closes");
        }


		// flip screen
		GO_SwapBuffers(screen);
		cycles++;
	}

	// deinit
	SDL_SetCursor(cursorArrow);
	SDL_FreeCursor(cursorPencil);
	SDL_FreeCursor(cursorPoint);
	SDL_FreeCursor(cursorBrush);
	SDL_FreeCursor(cursorFill);
	if ( palette != NULL )
	{
		free(palette);
	}
	if ( copymap.tiles != NULL )
	{
		free(copymap.tiles);
	}
	list_FreeAll(&undolist);
	saveTilePalettes();
    for (int c = 0; c < sizeof(view_t::fb) / sizeof(view_t::fb[0]); ++c) {
        camera.fb[c].destroy();
    }
	return deinitApp();
}

void propertyPageTextAndInput(int numProperties, int width)
{
	int pad_x1 = subx1 + 8;
	int spacing = 36;
	int pad_x2 = width;

	if ( numProperties <= 0 )
	{
		return;
	}
	if ( editproperty < 0 || editproperty >= numProperties )
	{
		editproperty = 0;
		inputstr = spriteProperties[0];
	}

	// Cycle properties with TAB.
	if ( keystatus[SDLK_TAB] )
	{
		keystatus[SDLK_TAB] = 0;
		cursorflash = ticks;
		editproperty++;
		if ( editproperty == numProperties )
		{
			editproperty = 0;
		}

		inputstr = spriteProperties[editproperty];
	}

	// select a textbox
	if ( mousestatus[SDL_BUTTON_LEFT] )
	{
		for ( int i = 0; i < numProperties; i++ )
		{
			if ( omousex >= pad_x1 - 4 && omousey >= suby1 + 40 + i * spacing && omousex < pad_x1 - 4 + pad_x2 && omousey < suby1 + 56 + i * spacing )
			{
				inputstr = spriteProperties[i];
				editproperty = i;
				cursorflash = ticks;
			}
		}
	}
}

void propertyPageError(int rowIndex, int resetValue)
{
	errorMessage = 60;
	errorArr[rowIndex] = 1;
	snprintf(spriteProperties[rowIndex], sizeof(spriteProperties[rowIndex]), "%d", resetValue); //reset
}

void propertyPageCursorFlash(int rowSpacing)
{
	if ( editproperty < 0 || editproperty >= 32 )
	{
		return;
	}
	if ( (ticks - cursorflash) % TICKS_PER_SECOND < TICKS_PER_SECOND / 2 )
	{
		printText(font8x8_bmp, subx1 + 8 + strlen(spriteProperties[editproperty]) * 8, suby1 + 44 + editproperty * rowSpacing, "\26");
	}
}

void reselectEntityGroup()
{
	groupedEntities.clear();

	node_t* nextnode = nullptr;

	for ( node_t* node = map.entities->first; node != nullptr; node = nextnode )
	{
		nextnode = node->next;
		Entity* entity = static_cast<Entity*>(node->element);

		if ( entity == nullptr )
		{
			continue;
		}

		const int entityLayer =
			entityAuthoredSpriteLayer(entity);

		if ( selectedTool == 3 )
		{
			if ( entityLayer < roomSelectBottomLayer
				|| entityLayer > roomSelectTopLayer )
			{
				continue;
			}
		}
		else if ( entityLayer != drawlayer )
		{
			continue;
		}

		if ( entity->x / 16 >= selectedarea_x1
			&& entity->x / 16 <= selectedarea_x2
			&& entity->y / 16 >= selectedarea_y1
			&& entity->y / 16 <= selectedarea_y2 )
		{
			groupedEntities.push_back(entity);
		}
	}
}

int generateDungeon(char* levelset, Uint32 seed, std::tuple<int, int, int, int> mapParameters)
{
	return 0; // dummy function
}
