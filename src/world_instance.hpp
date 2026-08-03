/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: world_instance.hpp
    Desc: Stable identity primitives for divergent multiplayer map instances.

-------------------------------------------------------------------------------*/

#pragma once

#include <cstdint>
#include <algorithm>
#include <cctype>
#include <string>

struct WorldInstanceIdentity
{
    static constexpr std::size_t MAX_MAP_FILE_LENGTH = 255;
    static constexpr std::size_t MAX_INSTANCE_ID_LENGTH = 63;

    std::string mapFile = "start.lmp";
    std::string instanceId = "world";
    std::uint64_t revision = 0;

    static std::string canonicalMapFile(std::string value)
    {
        const std::size_t lastSeparator = value.find_last_of("/\\");
        if (lastSeparator != std::string::npos)
        {
            value = value.substr(lastSeparator + 1);
        }
        if (value.empty())
        {
            return "";
        }
        std::transform(
            value.begin(),
            value.end(),
            value.begin(),
            [](const unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            }
        );
        if (value.size() < 4 || value.substr(value.size() - 4) != ".lmp")
        {
            value += ".lmp";
        }
        return value;
    }

    static bool isSafeMapFile(const std::string& value)
    {
        if (value.empty() || value.size() > MAX_MAP_FILE_LENGTH)
        {
            return false;
        }
        if (value.find('\0') != std::string::npos
            || value.find('/') != std::string::npos
            || value.find('\\') != std::string::npos
            || value.find(':') != std::string::npos)
        {
            return false;
        }
        if (value.find("..") != std::string::npos)
        {
            return false;
        }
        for (const unsigned char character : value)
        {
            if (character < 0x20 || character == 0x7f)
            {
                return false;
            }
        }
        return value.size() > 4 && value.substr(value.size() - 4) == ".lmp";
    }

    static bool isSafeInstanceId(const std::string& value)
    {
        if (value.empty() || value.size() > MAX_INSTANCE_ID_LENGTH)
        {
            return false;
        }
        for (const unsigned char character : value)
        {
            const bool alphaNumeric =
                (character >= 'a' && character <= 'z')
                || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9');
            if (!alphaNumeric && character != '-' && character != '_')
            {
                return false;
            }
        }
        return true;
    }

    bool set(const std::string& newMapFile, const std::string& newInstanceId)
    {
        if (newMapFile.find('/') != std::string::npos
            || newMapFile.find('\\') != std::string::npos
            || newMapFile.find("..") != std::string::npos)
        {
            return false;
        }
        const std::string canonicalFile = canonicalMapFile(newMapFile);
        if (!isSafeMapFile(canonicalFile) || !isSafeInstanceId(newInstanceId))
        {
            return false;
        }
        if (mapFile != canonicalFile || instanceId != newInstanceId)
        {
            mapFile = canonicalFile;
            instanceId = newInstanceId;
            ++revision;
        }
        return true;
    }

    bool isValid() const
    {
        return isSafeMapFile(mapFile) && isSafeInstanceId(instanceId);
    }

    bool matches(const WorldInstanceIdentity& other) const
    {
        return mapFile == other.mapFile && instanceId == other.instanceId;
    }

    std::string key() const
    {
        return mapFile + "#" + instanceId;
    }
};
