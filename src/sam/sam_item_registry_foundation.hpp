/*-------------------------------------------------------------------------------

    Automatia S.A.M Integration
    File: sam_item_registry_foundation.hpp
    Stage: SAM-1E

    Parses and validates S.A.M item declarations without registering them into
    Barony's live items[] table yet.

-------------------------------------------------------------------------------*/

#pragma once

#include <string>
#include <vector>

struct SAMModManifest;

struct SAMFoundationItemDef
{
    std::string stableId;
    std::string modNamespace;
    std::string nameIdentified;
    std::string nameUnidentified;
    std::string description;
    std::string category;
    std::string slot;
    std::string sourcePath;

    int runtimeId = 0;
    int weight = 0;
    int goldValue = 0;
    int level = -1;
    bool stackable = false;
};

class SAMItemRegistryFoundation
{
public:
    static constexpr int RuntimeIdBase = 5000;
    static constexpr int RuntimeIdLimit = 25000;
    static constexpr int RuntimeCapacity =
        RuntimeIdLimit - RuntimeIdBase;

    static_assert(
        RuntimeCapacity == 20000,
        "S.A.M runtime item capacity must be 20,000"
    );

    static void clear();

    static void loadFromManifest(
        const SAMModManifest& manifest
    );

    static int count();

    static const SAMFoundationItemDef* getItem(
        int runtimeId
    );

    static int runtimeIdForStableId(
        const std::string& stableId
    );

    static const std::string& stableIdForRuntimeId(
        int runtimeId
    );

    static bool isSAMRuntimeItemId(
        int runtimeId
    );

    static bool isRegisteredRuntimeItemId(
        int runtimeId
    );

    static bool validateRuntimeLayout(
        int vanillaItemCount
    );

    static int registeredItemCount();

    static int runtimeIdAtIndex(
        int catalogIndex
    );

    static const std::string& stableIdAtIndex(
        int catalogIndex
    );

    static const std::string& displayNameAtIndex(
        int catalogIndex
    );

    static const std::string& categoryAtIndex(
        int catalogIndex
    );

    static const std::string& slotAtIndex(
        int catalogIndex
    );

    static int runtimeIdBase();
    static int runtimeIdLimit();
    static int runtimeCapacity();

    static const std::vector<SAMFoundationItemDef>& items();

private:
    static std::vector<SAMFoundationItemDef> registry;
};
