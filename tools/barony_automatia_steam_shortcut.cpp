#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Entry {
	std::uint8_t type = 0;
	std::string key;
	std::string stringValue;
	std::uint32_t numberValue = 0;
	std::vector<Entry> children;
};

std::wstring utf8ToWide(const std::string& value) {
	if (value.empty()) {
		return {};
	}
	const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(),
		static_cast<int>(value.size()), nullptr, 0);
	if (length <= 0) {
		return {};
	}
	std::wstring result(static_cast<std::size_t>(length), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, value.data(),
		static_cast<int>(value.size()), result.data(), length);
	return result;
}

std::string wideToUtf8(const std::wstring& value) {
	if (value.empty()) {
		return {};
	}
	const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(),
		static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
	if (length <= 0) {
		return {};
	}
	std::string result(static_cast<std::size_t>(length), '\0');
	WideCharToMultiByte(CP_UTF8, 0, value.data(),
		static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
	return result;
}

bool readCString(const std::vector<std::uint8_t>& data, std::size_t& offset,
	std::string& result) {
	const std::size_t start = offset;
	while (offset < data.size() && data[offset] != 0) {
		++offset;
	}
	if (offset >= data.size()) {
		return false;
	}
	result.assign(reinterpret_cast<const char*>(data.data() + start), offset - start);
	++offset;
	return true;
}

bool readMap(const std::vector<std::uint8_t>& data, std::size_t& offset,
	std::vector<Entry>& result) {
	while (offset < data.size()) {
		const std::uint8_t type = data[offset++];
		if (type == 0x08) {
			return true;
		}
		if (type != 0x00 && type != 0x01 && type != 0x02) {
			return false;
		}

		Entry entry;
		entry.type = type;
		if (!readCString(data, offset, entry.key)) {
			return false;
		}
		if (type == 0x00) {
			if (!readMap(data, offset, entry.children)) {
				return false;
			}
		} else if (type == 0x01) {
			if (!readCString(data, offset, entry.stringValue)) {
				return false;
			}
		} else {
			if (offset + 4 > data.size()) {
				return false;
			}
			entry.numberValue = static_cast<std::uint32_t>(data[offset])
				| (static_cast<std::uint32_t>(data[offset + 1]) << 8)
				| (static_cast<std::uint32_t>(data[offset + 2]) << 16)
				| (static_cast<std::uint32_t>(data[offset + 3]) << 24);
			offset += 4;
		}
		result.push_back(std::move(entry));
	}
	return false;
}

void writeCString(std::vector<std::uint8_t>& data, const std::string& value) {
	data.insert(data.end(), value.begin(), value.end());
	data.push_back(0);
}

void writeMap(std::vector<std::uint8_t>& data, const std::vector<Entry>& entries) {
	for (const Entry& entry : entries) {
		data.push_back(entry.type);
		writeCString(data, entry.key);
		if (entry.type == 0x00) {
			writeMap(data, entry.children);
		} else if (entry.type == 0x01) {
			writeCString(data, entry.stringValue);
		} else {
			data.push_back(static_cast<std::uint8_t>(entry.numberValue & 0xff));
			data.push_back(static_cast<std::uint8_t>((entry.numberValue >> 8) & 0xff));
			data.push_back(static_cast<std::uint8_t>((entry.numberValue >> 16) & 0xff));
			data.push_back(static_cast<std::uint8_t>((entry.numberValue >> 24) & 0xff));
		}
	}
	data.push_back(0x08);
}

bool equalKey(const std::string& left, const char* right) {
	if (left.size() != std::char_traits<char>::length(right)) {
		return false;
	}
	for (std::size_t i = 0; i < left.size(); ++i) {
		if (static_cast<char>(std::tolower(static_cast<unsigned char>(left[i])))
			!= static_cast<char>(std::tolower(static_cast<unsigned char>(right[i])))) {
			return false;
		}
	}
	return true;
}

std::string unquote(const std::string& value) {
	if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
		return value.substr(1, value.size() - 2);
	}
	return value;
}

std::wstring normalizedPath(const std::string& value) {
	std::wstring result = utf8ToWide(unquote(value));
	for (wchar_t& character : result) {
		if (character == L'/') {
			character = L'\\';
		}
		character = static_cast<wchar_t>(std::towlower(character));
	}
	return result;
}

const Entry* findEntry(const std::vector<Entry>& entries, const char* key) {
	for (const Entry& entry : entries) {
		if (equalKey(entry.key, key)) {
			return &entry;
		}
	}
	return nullptr;
}

void setString(std::vector<Entry>& entries, const std::string& key,
	const std::string& value) {
	for (Entry& entry : entries) {
		if (entry.key == key) {
			entry.type = 0x01;
			entry.stringValue = value;
			entry.children.clear();
			return;
		}
	}
	Entry entry;
	entry.type = 0x01;
	entry.key = key;
	entry.stringValue = value;
	entries.push_back(std::move(entry));
}

void addNumber(std::vector<Entry>& entries, const std::string& key,
	std::uint32_t value) {
	Entry entry;
	entry.type = 0x02;
	entry.key = key;
	entry.numberValue = value;
	entries.push_back(std::move(entry));
}

void setNumber(std::vector<Entry>& entries, const std::string& key,
	std::uint32_t value) {
	for (Entry& entry : entries) {
		if (entry.key == key) {
			entry.type = 0x02;
			entry.numberValue = value;
			entry.stringValue.clear();
			entry.children.clear();
			return;
		}
	}
	addNumber(entries, key, value);
}

std::uint32_t crc32(const std::string& value) {
	std::uint32_t result = 0xffffffffu;
	for (const unsigned char character : value) {
		result ^= character;
		for (int bit = 0; bit < 8; ++bit) {
			result = (result >> 1) ^ ((result & 1u) ? 0xedb88320u : 0u);
		}
	}
	return ~result;
}

std::uint32_t shortcutAppId(const std::wstring& appName, const std::wstring& exe) {
	return crc32(wideToUtf8(exe) + wideToUtf8(appName)) | 0x80000000u;
}

std::vector<Entry> newShortcut(const std::wstring& appName, const std::wstring& exe,
	const std::wstring& startDir, std::uint32_t appId, const std::wstring& iconPath) {
	std::vector<Entry> result;
	setString(result, "AppName", wideToUtf8(appName));
	setString(result, "exe", "\"" + wideToUtf8(exe) + "\"");
	setString(result, "StartDir", "\"" + wideToUtf8(startDir) + "\"");
	setString(result, "icon", wideToUtf8(iconPath));
	setString(result, "ShortcutPath", "");
	setString(result, "LaunchOptions", "");
	addNumber(result, "appid", appId);
	addNumber(result, "IsHidden", 0);
	addNumber(result, "AllowDesktopConfig", 1);
	addNumber(result, "AllowOverlay", 1);
	addNumber(result, "openvr", 0);
	addNumber(result, "Devkit", 0);
	setString(result, "DevkitGameID", "");
	addNumber(result, "LastPlayTime", 0);
	Entry tags;
	tags.type = 0x00;
	tags.key = "tags";
	result.push_back(std::move(tags));
	return result;
}

bool copyArtwork(const fs::path& shortcutsPath, std::uint32_t appId,
	const fs::path& artworkDirectory) {
	const fs::path portraitSource = artworkDirectory / L"barony_automatia_portrait.png";
	const fs::path heroSource = artworkDirectory / L"barony_automatia_hero.png";
	const fs::path iconSource = artworkDirectory / L"barony_automatia_icon.png";
	std::error_code error;
	if (!fs::is_regular_file(portraitSource, error)
		|| !fs::is_regular_file(heroSource, error)
		|| !fs::is_regular_file(iconSource, error)) {
		return false;
	}
	const fs::path grid = shortcutsPath.parent_path() / L"grid";
	fs::create_directories(grid, error);
	if (error) {
		return false;
	}
	const auto copyArtworkNames = [&](const std::wstring& appIdText) {
		const fs::path portraitDestination = grid / (appIdText + L"p.png");
		const fs::path heroDestination = grid / (appIdText + L"_hero.png");
		const fs::path iconDestination = grid / (appIdText + L"_icon.png");
		const fs::path logoDestination = grid / (appIdText + L"_logo.png");
		return CopyFileW(portraitSource.c_str(), portraitDestination.c_str(), FALSE) != 0
			&& CopyFileW(heroSource.c_str(), heroDestination.c_str(), FALSE) != 0
			&& CopyFileW(iconSource.c_str(), iconDestination.c_str(), FALSE) != 0
			&& CopyFileW(iconSource.c_str(), logoDestination.c_str(), FALSE) != 0;
	};
	// Steam's shortcut record stores this as a 32-bit value. Steam clients have
	// used both the signed and unsigned text forms when looking up grid images,
	// so write both names for the same shortcut ID.
	return copyArtworkNames(std::to_wstring(appId))
		&& copyArtworkNames(std::to_wstring(static_cast<std::int32_t>(appId)));
}

fs::path findShortcutsFile(const fs::path& steamRoot) {
	const fs::path userdata = steamRoot / L"userdata";
	std::error_code error;
	fs::path best;
	fs::file_time_type bestTime{};
	bool haveBestTime = false;
	if (!fs::is_directory(userdata, error)) {
		return {};
	}

	for (const fs::directory_entry& user : fs::directory_iterator(userdata, error)) {
		if (error || !user.is_directory(error)) {
			continue;
		}
		const std::wstring name = user.path().filename().wstring();
		if (name.empty() || !std::all_of(name.begin(), name.end(),
			[](wchar_t character) { return character >= L'0' && character <= L'9'; })) {
			continue;
		}
		const fs::path config = user.path() / L"config";
		if (!fs::is_directory(config, error)) {
			continue;
		}
		const fs::path shortcuts = config / L"shortcuts.vdf";
		fs::file_time_type time{};
		if (fs::exists(shortcuts, error)) {
			time = fs::last_write_time(shortcuts, error);
		} else {
			const fs::path localConfig = config / L"localconfig.vdf";
			time = fs::last_write_time(localConfig, error);
		}
		if (!error && (!haveBestTime || time > bestTime)) {
			best = shortcuts;
			bestTime = time;
			haveBestTime = true;
		}
		error.clear();
	}
	return best;
}

bool steamIsRunning() {
	const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) {
		return false;
	}
	PROCESSENTRY32W process{};
	process.dwSize = sizeof(process);
	bool result = false;
	if (Process32FirstW(snapshot, &process)) {
		do {
			if (_wcsicmp(process.szExeFile, L"steam.exe") == 0) {
				result = true;
				break;
			}
		} while (Process32NextW(snapshot, &process));
	}
	CloseHandle(snapshot);
	return result;
}

bool readFile(const fs::path& path, std::vector<std::uint8_t>& data) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return false;
	}
	file.seekg(0, std::ios::end);
	const std::streamoff size = file.tellg();
	if (size < 0) {
		return false;
	}
	file.seekg(0, std::ios::beg);
	data.resize(static_cast<std::size_t>(size));
	return data.empty() || static_cast<bool>(file.read(
		reinterpret_cast<char*>(data.data()), size));
}

bool writeFile(const fs::path& path, const std::vector<std::uint8_t>& data) {
	const fs::path temporary = path.wstring() + L".automatia-tmp";
	{
		std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
		if (!file) {
			return false;
		}
		file.write(reinterpret_cast<const char*>(data.data()),
			static_cast<std::streamsize>(data.size()));
		if (!file) {
			return false;
		}
	}
	return MoveFileExW(temporary.c_str(), path.c_str(),
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool backupIfNeeded(const fs::path& path) {
	if (!fs::exists(path)) {
		return true;
	}
	const fs::path backup = path.wstring() + L".automatia-original";
	if (fs::exists(backup)) {
		return true;
	}
	return CopyFileW(path.c_str(), backup.c_str(), TRUE) != 0;
}

bool addShortcut(const fs::path& shortcutsPath, const std::wstring& appName,
	const std::wstring& exe, const std::wstring& startDir,
	const fs::path& artworkDirectory) {
	std::vector<std::uint8_t> data;
	std::vector<Entry> root;
	if (fs::exists(shortcutsPath)) {
		if (!readFile(shortcutsPath, data)) {
			return false;
		}
		std::size_t offset = 0;
		if (!data.empty() && !readMap(data, offset, root)) {
			root.clear();
		}
	}
	if (!backupIfNeeded(shortcutsPath)) {
		return false;
	}

	Entry* shortcuts = nullptr;
	for (Entry& entry : root) {
		if (equalKey(entry.key, "shortcuts") && entry.type == 0x00) {
			shortcuts = &entry;
			break;
		}
	}
	if (!shortcuts) {
		Entry entry;
		entry.type = 0x00;
		entry.key = "shortcuts";
		root.push_back(std::move(entry));
		shortcuts = &root.back();
	}

	const std::wstring wantedExe = normalizedPath(wideToUtf8(exe));
	const std::uint32_t appId = shortcutAppId(appName, exe);
	const fs::path gridIcon = shortcutsPath.parent_path() / L"grid"
		/ (std::to_wstring(static_cast<std::int32_t>(appId)) + L"_icon.png");
	const std::wstring gridIconPath = gridIcon.wstring();
	Entry* existing = nullptr;
	for (Entry& shortcut : shortcuts->children) {
		if (shortcut.type != 0x00) {
			continue;
		}
		const Entry* name = findEntry(shortcut.children, "AppName");
		const Entry* target = findEntry(shortcut.children, "exe");
		if ((name && utf8ToWide(name->stringValue) == appName)
			|| (target && normalizedPath(target->stringValue) == wantedExe)) {
			existing = &shortcut;
			break;
		}
	}
	if (!existing) {
		Entry shortcut;
		shortcut.type = 0x00;
		std::uint32_t nextIndex = 0;
		for (const Entry& child : shortcuts->children) {
			try {
				nextIndex = (std::max)(nextIndex,
					static_cast<std::uint32_t>(std::stoul(child.key) + 1));
			} catch (...) {
			}
		}
		shortcut.key = std::to_string(nextIndex);
		shortcut.children = newShortcut(appName, exe, startDir, appId, gridIconPath);
		shortcuts->children.push_back(std::move(shortcut));
	} else {
		setString(existing->children, "AppName", wideToUtf8(appName));
		setString(existing->children, "exe", "\"" + wideToUtf8(exe) + "\"");
		setString(existing->children, "StartDir", "\"" + wideToUtf8(startDir) + "\"");
		setString(existing->children, "icon", wideToUtf8(gridIconPath));
		setNumber(existing->children, "appid", appId);
	}

	std::vector<std::uint8_t> output;
	writeMap(output, root);
	if (!writeFile(shortcutsPath, output)) {
		return false;
	}
	if (!copyArtwork(shortcutsPath, appId, artworkDirectory)) {
		MessageBoxW(nullptr,
			L"The Automatia Steam shortcut was added, but its artwork could not be copied. "
			L"You can set custom artwork in Steam.",
			L"Barony Automatia Steam artwork", MB_OK | MB_ICONWARNING);
	}
	return true;
}

bool removeShortcut(const fs::path& shortcutsPath, const std::wstring& appName,
	const std::wstring& exe) {
	if (!fs::exists(shortcutsPath)) {
		return true;
	}
	std::vector<std::uint8_t> data;
	if (!readFile(shortcutsPath, data)) {
		return false;
	}
	std::size_t offset = 0;
	std::vector<Entry> root;
	if (!readMap(data, offset, root)) {
		return false;
	}
	const std::wstring wantedExe = normalizedPath(wideToUtf8(exe));
	bool changed = false;
	for (Entry& entry : root) {
		if (entry.type != 0x00 || !equalKey(entry.key, "shortcuts")) {
			continue;
		}
		const auto oldSize = entry.children.size();
		entry.children.erase(std::remove_if(entry.children.begin(), entry.children.end(),
			[&](const Entry& shortcut) {
				if (shortcut.type != 0x00) {
					return false;
				}
				const Entry* name = findEntry(shortcut.children, "AppName");
				const Entry* target = findEntry(shortcut.children, "exe");
				return (name && utf8ToWide(name->stringValue) == appName)
					|| (target && normalizedPath(target->stringValue) == wantedExe);
			}), entry.children.end());
		changed = changed || oldSize != entry.children.size();
	}
	if (!changed) {
		return true;
	}
	std::vector<std::uint8_t> output;
	writeMap(output, root);
	return writeFile(shortcutsPath, output);
}

int fail(const std::wstring& message) {
	MessageBoxW(nullptr, message.c_str(), L"Barony Automatia Steam shortcut",
		MB_OK | MB_ICONERROR);
	return 1;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
	const bool adding = argc == 7 && wcscmp(argv[1], L"--add") == 0;
	const bool removing = argc == 6 && wcscmp(argv[1], L"--remove") == 0;
	if (!adding && !removing) {
		return fail(L"Usage: --add <Steam folder> <name> <exe> <start folder> <artwork folder> or --remove <Steam folder> <name> <exe> <start folder>");
	}
	if (steamIsRunning()) {
		return fail(L"Please exit Steam completely, then run the Automatia installer again.");
	}
	const fs::path shortcutsPath = findShortcutsFile(argv[2]);
	if (shortcutsPath.empty()) {
		return fail(L"Could not locate a Steam user profile for the Automatia shortcut.");
	}
	const bool success = adding
		? addShortcut(shortcutsPath, argv[3], argv[4], argv[5], argv[6])
		: removeShortcut(shortcutsPath, argv[3], argv[4]);
	if (!success) {
		return fail(L"Steam's shortcut file could not be updated.");
	}
	return 0;
}
