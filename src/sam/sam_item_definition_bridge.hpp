/*-------------------------------------------------------------------------------

    Automatia S.A.M Integration
    File: sam_item_definition_bridge.hpp
    Stage: SAM-1K

    Installs validated S.A.M item metadata into the expanded Barony items[]
    definition storage. Item spawning remains disabled in this stage.

-------------------------------------------------------------------------------*/

#pragma once

class SAMItemDefinitionBridge
{
public:
    static void clearInstalledDefinitions();

    static int installRegisteredDefinitions();

    static int runControlledConstructionTests();

    static int installedDefinitionCount();

private:
    static int installedCount;
};
