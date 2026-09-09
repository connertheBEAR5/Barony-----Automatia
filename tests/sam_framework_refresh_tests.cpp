#include "sam/framework/sam_workshop.hpp"
#include "sam/sam_class_registry_foundation.hpp"
#include "sam/sam_item_registry_foundation.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{
bool expect(const bool condition, const char* expression)
{
	if ( !condition )
	{
		std::cerr << "FAILED: " << expression << '\n';
	}
	return condition;
}

#define EXPECT(expression) do { if (!expect((expression), #expression)) return false; } while (false)

bool writeFile(const std::filesystem::path& path, const std::string& contents)
{
	std::error_code error;
	std::filesystem::create_directories(path.parent_path(), error);
	if ( error )
	{
		return false;
	}
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	output << contents;
	return output.good();
}

std::filesystem::path fixtureRoot()
{
	return std::filesystem::temp_directory_path()
		/ ("barony_sam_framework_refresh_"
			+ std::to_string(std::chrono::high_resolution_clock::now()
				.time_since_epoch().count()));
}

bool testWorkshopDigestAndDuplicateNamespace()
{
	const std::filesystem::path root = fixtureRoot();
	const std::filesystem::path alpha = root / "alpha";
	const std::filesystem::path duplicate = root / "duplicate";
	const std::filesystem::path malformed = root / "malformed";
	std::error_code error;

	const std::string alphaManifest = R"json({
  "namespace": "alpha",
  "name": "Alpha",
  "version": "1.0.0",
  "framework_min_version": "v2.0.0",
  "items": ["items/knife.json"]
})json";
	const std::string duplicateManifest = R"json({
  "namespace": "alpha",
  "name": "Duplicate Alpha",
  "version": "1.0.0",
  "framework_min_version": "2.0.0",
  "items": ["items/other.json"]
})json";

	EXPECT(writeFile(alpha / "mod.json", alphaManifest));
	EXPECT(writeFile(alpha / "items/knife.json",
		R"json({"id":"alpha:knife","name_identified":"Knife","category":"WEAPON"})json"));
	EXPECT(writeFile(duplicate / "mod.json", duplicateManifest));
	EXPECT(writeFile(duplicate / "items/other.json",
		R"json({"id":"alpha:other","name_identified":"Other","category":"WEAPON"})json"));

	std::vector<SAMModManifest> manifests = SAMWorkshop::scan({
		{alpha.string(), "Alpha"},
		{duplicate.string(), "Duplicate"}
	});
	EXPECT(manifests.size() == 1);
	EXPECT(manifests[0].ns == "alpha");
	EXPECT(manifests[0].frameworkMinVersion == "2.0.0");
	EXPECT(manifests[0].contentDigest.size() == 16);
	const std::string originalDigest = manifests[0].contentDigest;

	// The declared item bytes affect the per-mod fingerprint even when the
	// namespace and manifest version stay identical.
	EXPECT(writeFile(alpha / "items/knife.json",
		R"json({"id":"alpha:knife","name_identified":"Sharper Knife","category":"WEAPON"})json"));
	manifests = SAMWorkshop::scan({{alpha.string(), "Alpha"}});
	EXPECT(manifests.size() == 1);
	EXPECT(manifests[0].contentDigest != originalDigest);
	const std::string payloadDigest = manifests[0].contentDigest;

	// Companion scripts are discovered by SAMLoader, so they must participate
	// in compatibility even though the manifest only declares the JSON base.
	EXPECT(writeFile(alpha / "items/knife.lua", "function on_event() end\n"));
	manifests = SAMWorkshop::scan({{alpha.string(), "Alpha"}});
	EXPECT(manifests.size() == 1);
	EXPECT(manifests[0].contentDigest != payloadDigest);
	const std::string scriptDigest = manifests[0].contentDigest;
	EXPECT(writeFile(alpha / "items/knife.lua", "function on_event() return 1 end\n"));
	manifests = SAMWorkshop::scan({{alpha.string(), "Alpha"}});
	EXPECT(manifests.size() == 1);
	EXPECT(manifests[0].contentDigest != scriptDigest);

	EXPECT(writeFile(malformed / "mod.json", R"json({
  "namespace": "malformed",
  "name": "Malformed",
  "version": "1.0.0",
  "framework_min_version": "2.0.0",
  "items": "items/not-an-array.json",
  "rooms": { "mine": ["../outside.lmp", 9] },
  "typoed_filed": true
})json"));
	manifests = SAMWorkshop::scan({{malformed.string(), "Malformed"}});
	EXPECT(manifests.size() == 1);
	EXPECT(manifests[0].items.empty());
	EXPECT(manifests[0].rooms.empty());

	SAMWorkshop::clear();
	std::filesystem::remove_all(root, error);
	return true;
}

bool testFoundationRegistriesRejectBadTypesAndReuseSlots()
{
	const std::filesystem::path root = fixtureRoot();
	std::error_code error;
	EXPECT(writeFile(root / "items/bad_required.json",
		R"json({"id":7,"name_identified":"Bad","category":"WEAPON"})json"));
	EXPECT(writeFile(root / "items/first.json",
		R"json({"id":"alpha:first","name_identified":"First","category":"WEAPON","weight":3})json"));
	EXPECT(writeFile(root / "items/defaulted.json",
		R"json({"id":"alpha:defaulted","name_identified":"Defaulted","category":"TOOL","name_unidentified":false,"slot":7,"weight":"heavy","gold_value":999999999999999999999999,"level":1.5,"stackable":"yes"})json"));
	EXPECT(writeFile(root / "items/second.json",
		R"json({"id":"alpha:second","name_identified":"Second","category":"TOOL"})json"));
	EXPECT(writeFile(root / "classes/bad_required.json",
		R"json({"id":3,"name":"Bad Class"})json"));
	EXPECT(writeFile(root / "classes/guardian.json",
		R"json({"id":"alpha:guardian","name":"Guardian","description":42,"stats":{"STR":"too high","DEX":3,"GOLD":12}})json"));

	SAMModManifest manifest;
	manifest.ns = "alpha";
	manifest.modPath = root.string();
	manifest.items = {
		"items/bad_required.json", "items/first.json",
		"items/defaulted.json", "items/second.json"
	};
	manifest.classes = {"classes/bad_required.json", "classes/guardian.json"};

	SAMItemRegistryFoundation::clear();
	SAMClassRegistryFoundation::clear();
	EXPECT(SAMItemRegistryFoundation::registerFrameworkBuiltin(
		"sam:reserved_low", 5000, "reserved low", "TOOL"));
	EXPECT(SAMItemRegistryFoundation::registerFrameworkBuiltin(
		"sam:hunters_workbench", 6000, "hunter's workbench", "TOOL"));
	SAMItemRegistryFoundation::loadFromManifest(manifest);
	EXPECT(SAMItemRegistryFoundation::registeredItemCount() == 5);
	EXPECT(SAMItemRegistryFoundation::runtimeIdForStableId("alpha:first") == 5001);
	EXPECT(SAMItemRegistryFoundation::runtimeIdForStableId("alpha:defaulted") == 5002);
	EXPECT(SAMItemRegistryFoundation::runtimeIdForStableId("alpha:second") == 5003);
	const SAMFoundationItemDef* defaulted =
		SAMItemRegistryFoundation::getItem(5002);
	EXPECT(defaulted != nullptr);
	EXPECT(defaulted->nameUnidentified.empty());
	EXPECT(defaulted->slot == "NO_EQUIP");
	EXPECT(defaulted->weight == 0);
	EXPECT(defaulted->goldValue == 0);
	EXPECT(defaulted->level == -1);
	EXPECT(!defaulted->stackable);

	SAMClassRegistryFoundation::loadFromManifest(manifest);
	EXPECT(SAMClassRegistryFoundation::count() == 1);
	const SAMFoundationClassDef* guardian =
		SAMClassRegistryFoundation::getClass(1000);
	EXPECT(guardian != nullptr);
	EXPECT(guardian->description.empty());
	EXPECT(guardian->str == 0);
	EXPECT(guardian->dex == 3);
	EXPECT(guardian->gold == 12);

	// A fresh scan must not retain maps/cursors from the previous set of mods.
	SAMItemRegistryFoundation::clear();
	EXPECT(SAMItemRegistryFoundation::runtimeIdForStableId("alpha:first") == -1);
	manifest.items = {"items/second.json"};
	SAMItemRegistryFoundation::loadFromManifest(manifest);
	EXPECT(SAMItemRegistryFoundation::runtimeIdForStableId("alpha:second") == 5000);

	SAMItemRegistryFoundation::clear();
	SAMClassRegistryFoundation::clear();
	std::filesystem::remove_all(root, error);
	return true;
}
}

int main()
{
	return testWorkshopDigestAndDuplicateNamespace()
		&& testFoundationRegistriesRejectBadTypesAndReuseSlots()
		? 0 : 1;
}
