/*-------------------------------------------------------------------------------

    Automatia S.A.M Integration
    File: sam_class_registry_foundation.hpp
    Stage: SAM-1C

    Loads and validates S.A.M class declarations without applying them to
    Barony gameplay or character creation yet.

-------------------------------------------------------------------------------*/

#pragma once

#include <string>
#include <vector>

struct SAMModManifest;

struct SAMFoundationClassDef
{
    std::string stableId;
    std::string modNamespace;
    std::string name;
    std::string description;
    std::string sourcePath;
    int runtimeId = 0;

    int str = 0;
    int dex = 0;
    int con = 0;
    int intel = 0;
    int per = 0;
    int chr = 0;
    int hp = 0;
    int mp = 0;
    int gold = 0;
};

class Stat;

class SAMClassRegistryFoundation
{
public:
    static void clear();

    static void loadFromManifest(
        const SAMModManifest& manifest
    );

    static int count();

    static const SAMFoundationClassDef* getClass(
        int runtimeId
    );

    static int runtimeIdForStableId(
        const std::string& stableId
    );

    static void applyStats(
        int runtimeId,
        Stat* stats
    );

    static const std::vector<SAMFoundationClassDef>& classes();

private:
    static std::vector<SAMFoundationClassDef> registry;
};
