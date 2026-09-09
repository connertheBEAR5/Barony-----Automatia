#pragma once

#include <array>
#include <cstdint>

// The skill-book target table mirrors the stable player proficiency IDs in
// stat.hpp.  Appraisal (the current runtime's Lore-equivalent) is intentionally
// omitted: it scales manuals but cannot be increased by one.
namespace SkillBooks
{
constexpr std::uint32_t kAppearanceMarker = 0x80000000u;
constexpr int kTargetMask = 0x0f;

constexpr std::array<int, 15> kEligibleSkills = {
	0,  // Lockpicking
	1,  // Stealth
	2,  // Trading
	4,  // Thaumaturgy
	5,  // Leadership
	6,  // Mysticism
	7,  // Sorcery
	8,  // Ranged
	9,  // Sword
	10, // Mace
	11, // Axe
	12, // Polearm
	13, // Shield
	14, // Unarmed
	15  // Alchemy
};

constexpr bool isEligibleSkill(const int skill)
{
	for (const int candidate : kEligibleSkills)
	{
		if (candidate == skill)
		{
			return true;
		}
	}
	return false;
}

constexpr std::uint32_t encodeSkill(const int skill)
{
	return isEligibleSkill(skill)
		? kAppearanceMarker | static_cast<std::uint32_t>(skill)
		: 0u;
}

constexpr int decodeSkill(const std::uint32_t appearance)
{
	if ((appearance & kAppearanceMarker) == 0
		|| (appearance & ~(kAppearanceMarker
			| static_cast<std::uint32_t>(kTargetMask))) != 0)
	{
		return -1;
	}
	const int skill = static_cast<int>(appearance & kTargetMask);
	return isEligibleSkill(skill) ? skill : -1;
}

constexpr bool hasEncodedSkill(const std::uint32_t appearance)
{
	return decodeSkill(appearance) >= 0;
}

constexpr int loreMagnitude(const int lore)
{
	const int clampedLore = lore < 0 ? 0 : (lore > 100 ? 100 : lore);
	const int rawMagnitude = 1 + clampedLore / 20;
	return rawMagnitude < 1 ? 1 : (rawMagnitude > 5 ? 5 : rawMagnitude);
}

constexpr int applyDelta(const int current, const int lore, const bool cursed)
{
	const int clampedCurrent = current < 0 ? 0 : (current > 100 ? 100 : current);
	const int signedDelta = cursed ? -loreMagnitude(lore) : loreMagnitude(lore);
	const int next = clampedCurrent + signedDelta;
	return next < 0 ? 0 : (next > 100 ? 100 : next);
}

constexpr const char* skillName(const int skill)
{
	switch (skill)
	{
		case 0: return "Lockpicking";
		case 1: return "Stealth";
		case 2: return "Trading";
		case 4: return "Thaumaturgy";
		case 5: return "Leadership";
		case 6: return "Mysticism";
		case 7: return "Sorcery";
		case 8: return "Ranged";
		case 9: return "Sword";
		case 10: return "Mace";
		case 11: return "Axe";
		case 12: return "Polearm";
		case 13: return "Shield";
		case 14: return "Unarmed";
		case 15: return "Alchemy";
		default: return "Unknown";
	}
}
} // namespace SkillBooks
