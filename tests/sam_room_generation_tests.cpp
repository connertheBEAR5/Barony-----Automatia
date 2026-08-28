#include "sam/framework/sam_rooms.hpp"
#include "sam/framework/sam_workshop.hpp"

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

bool writeFixture(const std::filesystem::path& path, const std::string& bytes)
{
	std::filesystem::create_directories(path.parent_path());
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	return output.good();
}

std::string readFixture(const std::filesystem::path& path)
{
	std::ifstream input(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(input),
		std::istreambuf_iterator<char>());
}

SAMModManifest manifest(const std::string& name,
	const std::filesystem::path& root, const std::string& room)
{
	SAMModManifest result;
	result.ns = name;
	result.version = "1.0.0";
	result.modPath = root.string();
	result.rooms.push_back({"Mine", {room, room}});
	return result;
}

bool testCanonicalOrderingDedupAndByteFingerprint()
{
	const std::filesystem::path fixture =
		std::filesystem::temp_directory_path()
		/ ("barony_sam_room_generation_"
			+ std::to_string(std::chrono::high_resolution_clock::now()
				.time_since_epoch().count()));
	std::error_code error;
	const std::filesystem::path alphaRoot = fixture / "alpha";
	const std::filesystem::path betaRoot = fixture / "beta";
	EXPECT(writeFixture(alphaRoot / "rooms/a.lmp", "room-a-v1"));
	EXPECT(writeFixture(betaRoot / "rooms/b.lmp", "room-b-v1"));

	const SAMModManifest alpha = manifest("alpha", alphaRoot, "rooms/a.lmp");
	const SAMModManifest beta = manifest("beta", betaRoot, "rooms/b.lmp");
	std::vector<std::string> entries =
		SAMRooms::contentFingerprintEntries({beta, alpha});
	EXPECT(entries.size() == 2);
	EXPECT(entries[0].find("room:mine:alpha/rooms/a.lmp@") == 0);
	EXPECT(entries[1].find("room:mine:beta/rooms/b.lmp@") == 0);

	SAMRooms::applyAll({beta, alpha});
	const std::vector<std::string>& pool = SAMRooms::roomsFor("MINE");
	EXPECT(pool.size() == 2);
	EXPECT(pool[0] == (alphaRoot / "rooms/a.lmp").string());
	EXPECT(pool[1] == (betaRoot / "rooms/b.lmp").string());

	const std::vector<std::string> before = entries;
	EXPECT(writeFixture(alphaRoot / "rooms/a.lmp", "room-a-v2"));
	entries = SAMRooms::contentFingerprintEntries({alpha, beta});
	EXPECT(entries.size() == before.size());
	EXPECT(entries[0] != before[0]);
	EXPECT(entries[1] == before[1]);

	// Duplicate effective keys choose the same content in both the generated
	// pool and byte fingerprint catalog, independent of installation paths.
	const SAMModManifest duplicateAlpha = manifest(
		"alpha", betaRoot, "rooms/a.lmp");
	EXPECT(writeFixture(betaRoot / "rooms/a.lmp", "duplicate-alpha"));
	SAMRooms::applyAll({duplicateAlpha, alpha});
	EXPECT(SAMRooms::roomsFor("mine").size() == 1);
	const std::vector<std::string> duplicateEntries =
		SAMRooms::contentFingerprintEntries({duplicateAlpha, alpha});
	EXPECT(duplicateEntries.size() == 1);
	const std::filesystem::path selectedAlpha =
		SAMRooms::roomsFor("mine")[0];
	const std::vector<std::string> selectedOnlyEntries =
		SAMRooms::contentFingerprintEntries({manifest(
			"alpha", selectedAlpha.parent_path().parent_path(), "rooms/a.lmp")});
	EXPECT(duplicateEntries == selectedOnlyEntries);

	// Model a second machine whose absolute roots order the same two files in
	// the opposite direction. Both the fingerprint and selected bytes remain
	// identical; an absolute-path tie-break would fail this characterization.
	const std::filesystem::path mirroredLowRoot = fixture / "mirror/a_root";
	const std::filesystem::path mirroredHighRoot = fixture / "mirror/z_root";
	EXPECT(writeFixture(mirroredLowRoot / "rooms/a.lmp", "duplicate-alpha"));
	EXPECT(writeFixture(mirroredHighRoot / "rooms/a.lmp", "room-a-v2"));
	const SAMModManifest mirroredLow = manifest(
		"alpha", mirroredLowRoot, "rooms/a.lmp");
	const SAMModManifest mirroredHigh = manifest(
		"alpha", mirroredHighRoot, "rooms/a.lmp");
	EXPECT(SAMRooms::contentFingerprintEntries({mirroredLow, mirroredHigh})
		== duplicateEntries);
	SAMRooms::applyAll({mirroredLow, mirroredHigh});
	EXPECT(SAMRooms::roomsFor("mine").size() == 1);
	EXPECT(readFixture(SAMRooms::roomsFor("mine")[0])
		== readFixture(selectedAlpha));

	SAMModManifest invalid = manifest("invalid", fixture / "missing",
		"rooms/not_there.lmp");
	invalid.rooms.push_back({"mine", {"../escape.lmp"}});
	EXPECT(SAMRooms::contentFingerprintEntries({invalid}).empty());

	SAMRooms::clear();
	std::filesystem::remove_all(fixture, error);
	return true;
}
}

int main()
{
	if ( !testCanonicalOrderingDedupAndByteFingerprint() )
	{
		return 1;
	}
	std::cout << "S.A.M room ordering and content fingerprint tests passed.\n";
	return 0;
}
