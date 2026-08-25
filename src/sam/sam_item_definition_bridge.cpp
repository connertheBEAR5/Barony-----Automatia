/*-------------------------------------------------------------------------------

    Automatia S.A.M Integration
    File: sam_item_definition_bridge.cpp
    Stage: SAM-1K

-------------------------------------------------------------------------------*/

#include "sam_item_definition_bridge.hpp"

#include "sam_item_registry_foundation.hpp"
#include "framework/sam_logger.hpp"
#include "../items.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
    Category parseCategory(
        const std::string& value
    )
    {
        if ( value == "WEAPON" ) { return WEAPON; }
        if ( value == "ARMOR" ) { return ARMOR; }
        if ( value == "AMULET" ) { return AMULET; }
        if ( value == "POTION" ) { return POTION; }
        if ( value == "SCROLL" ) { return SCROLL; }
        if ( value == "MAGICSTAFF" ) { return MAGICSTAFF; }
        if ( value == "RING" ) { return RING; }
        if ( value == "SPELLBOOK" ) { return SPELLBOOK; }
        if ( value == "GEM" ) { return GEM; }
        if ( value == "THROWN" ) { return THROWN; }
        if ( value == "TOOL" ) { return TOOL; }
        if ( value == "FOOD" ) { return FOOD; }
        if ( value == "BOOK" ) { return BOOK; }
        if ( value == "SPELL_CAT" ) { return SPELL_CAT; }
        if ( value == "TOME_SPELL" ) { return TOME_SPELL; }

        return TOOL;
    }

    ItemEquippableSlot parseSlot(
        const std::string& value
    )
    {
        if ( value == "WEAPON" )
        {
            return EQUIPPABLE_IN_SLOT_WEAPON;
        }
        if ( value == "SHIELD" )
        {
            return EQUIPPABLE_IN_SLOT_SHIELD;
        }
        if ( value == "MASK" )
        {
            return EQUIPPABLE_IN_SLOT_MASK;
        }
        if ( value == "HELM" )
        {
            return EQUIPPABLE_IN_SLOT_HELM;
        }
        if ( value == "GLOVES" )
        {
            return EQUIPPABLE_IN_SLOT_GLOVES;
        }
        if ( value == "BOOTS" )
        {
            return EQUIPPABLE_IN_SLOT_BOOTS;
        }
        if ( value == "BREASTPLATE" )
        {
            return EQUIPPABLE_IN_SLOT_BREASTPLATE;
        }
        if ( value == "CLOAK" )
        {
            return EQUIPPABLE_IN_SLOT_CLOAK;
        }
        if ( value == "AMULET" )
        {
            return EQUIPPABLE_IN_SLOT_AMULET;
        }
        if ( value == "RING" )
        {
            return EQUIPPABLE_IN_SLOT_RING;
        }

        return NO_EQUIP;
    }

    bool isValidDefinitionSlot(
        const int runtimeId
    )
    {
        return runtimeId >= SAM_ITEM_ID_BASE
            && runtimeId < NUM_ITEM_SLOTS;
    }

    void resetDefinitionSlot(
        const int runtimeId
    )
    {
        if ( !isValidDefinitionSlot(runtimeId) )
        {
            return;
        }

        ItemGeneric& definition = items[runtimeId];

        definition.setIdentifiedName(std::string());
        definition.setUnidentifiedName(std::string());

        definition.index = 0;
        definition.indexShort = 0;
        definition.fpindex = 0;
        definition.variations = 0;
        definition.weight = 0;
        definition.gold_value = 0;
        definition.category = TOOL;
        definition.level = -1;
        definition.item_slot = NO_EQUIP;
        definition.attributes.clear();
        definition.tooltip = "tooltip_default";
        definition.samTraits = 0;
    }
}

int SAMItemDefinitionBridge::installedCount = 0;

void SAMItemDefinitionBridge::clearInstalledDefinitions()
{
    for ( const SAMFoundationItemDef& source :
        SAMItemRegistryFoundation::items() )
    {
        resetDefinitionSlot(source.runtimeId);
    }

    installedCount = 0;
}

int SAMItemDefinitionBridge::installRegisteredDefinitions()
{
    clearInstalledDefinitions();

    for ( const SAMFoundationItemDef& source :
        SAMItemRegistryFoundation::items() )
    {
        const int visualTemplateId =
            (source.category == "WEAPON"
                || source.slot == "WEAPON")
                ? BRONZE_SWORD
                : GEM_ROCK;
        const ItemGeneric& fallback =
            items[visualTemplateId];
        if ( !isValidDefinitionSlot(source.runtimeId) )
        {
            SAM_ERROR(
                "ITEMS",
                "Refusing to install S.A.M item outside expanded item storage: "
                + source.stableId
                + " -> "
                + std::to_string(source.runtimeId)
            );
            continue;
        }

        ItemGeneric& destination =
            items[source.runtimeId];

        destination.setIdentifiedName(
            source.nameIdentified
        );
        destination.setUnidentifiedName(
            source.nameUnidentified.empty()
                ? source.nameIdentified
                : source.nameUnidentified
        );

        // Use a known-safe native model until S.A.M model registration is
        // connected. This prevents zero-variation modulo operations if a
        // definition is inspected before custom model support is available.
        destination.index = fallback.index;
        destination.indexShort = fallback.indexShort;
        destination.fpindex = fallback.fpindex;
        destination.variations =
            std::max(1, fallback.variations);

        destination.weight =
            std::max(0, source.weight);
        destination.gold_value =
            std::max(0, source.goldValue);
        destination.category =
            parseCategory(source.category);
        destination.level = source.level;
        destination.item_slot =
            parseSlot(source.slot);
        destination.attributes.clear();
        destination.attributes[
            "SAM_CUSTOM_ITEM"
        ] = 1;
        destination.attributes[
            "SAM_VISUAL_TEMPLATE_ID"
        ] = visualTemplateId;
        destination.attributes[
            "no_stack"
        ] = source.stackable ? 0 : 1;
        destination.tooltip = "tooltip_default";

        ++installedCount;

        SAM_INFO(
            "ITEMS",
            "Installed live item definition ["
            + source.stableId
            + "] into items["
            + std::to_string(source.runtimeId)
            + "] using visual template "
            + std::to_string(visualTemplateId)
        );
    }

    SAM_INFO(
        "ITEMS",
        "Live S.A.M item definitions installed: "
        + std::to_string(installedCount)
    );

    return installedCount;
}

int SAMItemDefinitionBridge::runControlledConstructionTests()
{
    int passed = 0;

    for ( const SAMFoundationItemDef& source :
        SAMItemRegistryFoundation::items() )
    {
        Item* item = newItem(
            static_cast<ItemType>(source.runtimeId),
            EXCELLENT,
            0,
            1,
            0,
            true,
            nullptr
        );

        if ( !item )
        {
            SAM_ERROR(
                "ITEMS",
                "Controlled construction returned null for ["
                + source.stableId
                + "]"
            );
            continue;
        }

        bool valid = true;

        if ( static_cast<int>(item->type)
            != source.runtimeId )
        {
            SAM_ERROR(
                "ITEMS",
                "Controlled construction changed runtime id for ["
                + source.stableId
                + "]: expected "
                + std::to_string(source.runtimeId)
                + ", got "
                + std::to_string(
                    static_cast<int>(item->type)
                )
            );
            valid = false;
        }

        if ( std::strcmp(
            item->getName(),
            source.nameIdentified.c_str()
        ) != 0 )
        {
            SAM_ERROR(
                "ITEMS",
                "Controlled construction name mismatch for ["
                + source.stableId
                + "]"
            );
            valid = false;
        }

        if ( items[source.runtimeId].attributes.find(
            "SAM_CUSTOM_ITEM"
        ) == items[source.runtimeId].attributes.end() )
        {
            SAM_ERROR(
                "ITEMS",
                "Controlled construction definition marker missing for ["
                + source.stableId
                + "]"
            );
            valid = false;
        }

        if ( itemCategory(item)
            != items[source.runtimeId].category )
        {
            SAM_ERROR(
                "ITEMS",
                "Controlled construction category mismatch for ["
                + source.stableId
                + "]"
            );
            valid = false;
        }

        if ( valid )
        {
            ++passed;

            SAM_INFO(
                "ITEMS",
                "Controlled construction passed ["
                + source.stableId
                + "] runtime id "
                + std::to_string(source.runtimeId)
                + " name '"
                + item->getName()
                + "'"
            );
        }

        std::free(item);
    }

    SAM_INFO(
        "ITEMS",
        "Controlled S.A.M item constructions passed: "
        + std::to_string(passed)
        + "/"
        + std::to_string(
            SAMItemRegistryFoundation::registeredItemCount()
        )
    );

    return passed;
}

int SAMItemDefinitionBridge::installedDefinitionCount()
{
    return installedCount;
}
