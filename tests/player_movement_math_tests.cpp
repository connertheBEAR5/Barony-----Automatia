#include "player_movement_math.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <string>

namespace
{
bool expect(const bool condition, const char* expression, const int line)
{
	if ( !condition )
	{
		std::cerr << "FAILED line " << line << ": " << expression << '\n';
	}
	return condition;
}

#define EXPECT(expression) \
	do { if ( !expect(static_cast<bool>(expression), #expression, __LINE__) ) return false; } while ( false )

bool nearlyEqual(const double lhs, const double rhs, const double epsilon = 1e-9)
{
	return std::abs(lhs - rhs) <= epsilon;
}

std::string readSource(const char* relative)
{
	std::ifstream input(std::filesystem::path(BARONY_SOURCE_DIR) / relative,
		std::ios::binary);
	std::ostringstream contents;
	contents << input.rdbuf();
	return input ? contents.str() : std::string{};
}

bool contains(const std::string& source, const char* token)
{
	return source.find(token) != std::string::npos;
}

bool appearsInOrder(const std::string& source,
	const std::initializer_list<const char*> tokens)
{
	std::size_t position = 0;
	for ( const char* token : tokens )
	{
		position = source.find(token, position);
		if ( position == std::string::npos )
		{
			return false;
		}
		position += std::char_traits<char>::length(token);
	}
	return true;
}

bool testAxesAndAnalogMagnitudes()
{
	using PlayerMovementMath::InputVector;
	using PlayerMovementMath::normalize;

	for ( const InputVector axis : {
		InputVector{1.0, 0.0}, InputVector{-1.0, 0.0},
		InputVector{0.0, 1.0}, InputVector{0.0, -1.0} } )
	{
		const InputVector result = normalize(axis);
		EXPECT(nearlyEqual(result.strafe, axis.strafe));
		EXPECT(nearlyEqual(result.forward, axis.forward));
		EXPECT(nearlyEqual(std::hypot(result.strafe, result.forward), 1.0));
	}

	for ( const InputVector partial : {
		InputVector{0.25, 0.0}, InputVector{-0.5, 0.0},
		InputVector{0.0, 0.25}, InputVector{0.0, -0.5},
		InputVector{0.25, 0.5} } )
	{
		const InputVector result = normalize(partial);
		EXPECT(nearlyEqual(result.strafe, partial.strafe));
		EXPECT(nearlyEqual(result.forward, partial.forward));
	}
	return true;
}

bool testDiagonalNormalization()
{
	using PlayerMovementMath::InputVector;
	using PlayerMovementMath::normalize;
	const double component = 1.0 / std::sqrt(2.0);

	for ( const InputVector diagonal : {
		InputVector{1.0, 1.0}, InputVector{-1.0, 1.0},
		InputVector{1.0, -1.0}, InputVector{-1.0, -1.0} } )
	{
		const InputVector result = normalize(diagonal);
		EXPECT(nearlyEqual(std::hypot(result.strafe, result.forward), 1.0));
		EXPECT(nearlyEqual(std::abs(result.strafe), component));
		EXPECT(nearlyEqual(std::abs(result.forward), component));
		EXPECT((result.strafe < 0.0) == (diagonal.strafe < 0.0));
		EXPECT((result.forward < 0.0) == (diagonal.forward < 0.0));
	}

	const InputVector saturated = normalize({0.8, 0.8});
	EXPECT(nearlyEqual(std::hypot(saturated.strafe, saturated.forward), 1.0));
	return true;
}

bool testMovementIntegrationContract()
{
	const std::string movement = readSource("src/actplayer.cpp");
	const std::string controller = readSource("src/player.cpp");
	const std::string networking = readSource("src/net.cpp");
	const std::string flags = readSource("src/net.hpp");
	const std::string menu = readSource("src/ui/MainMenu.cpp");

	EXPECT(contains(movement, "#include \"player_movement_math.hpp\""));
	EXPECT(contains(flags, "SV_FLAG_OMNIDIRECTIONAL_MOVEMENT = 1 << 12"));
	EXPECT(contains(menu, "omnidirectional_movement_enabled"));
	EXPECT(contains(menu, "\"Omnidirectional Movement\""));
	EXPECT(contains(menu, "\"Omnidirectional\\nMovement\""));
	EXPECT(contains(menu, "gameFlagLabelWidth = 180"));
	EXPECT(contains(menu, "gameFlagButtonX = 238"));
	EXPECT(contains(menu,
		"propertyVersion(\"omnidirectional_movement_enabled\", version >= 26"));
	EXPECT(contains(movement, "const bool omnidirectionalMovement"));
	EXPECT(contains(movement,
		"double backpedalMultiplier = omnidirectionalMovement ? 1.0 : 0.25;"));
	EXPECT(contains(movement, "double lateralMultiplier = 1.0;"));
	EXPECT(contains(movement, "if ( omnidirectionalMovement )"));
	EXPECT(contains(movement, "const real_t lateralSpeed = (svFlags & SV_FLAG_OMNIDIRECTIONAL_MOVEMENT) ? .045 : .0225;"));
	EXPECT(appearsInOrder(movement, {
		"x_force *= lateralMultiplier;",
		"if ( omnidirectionalMovement )",
		"PlayerMovementMath::normalize",
		"real_t speedFactor = getSpeedFactor" }));
	EXPECT(contains(movement,
		"PLAYER_VELX += x_force * cos(my->yaw + PI / 2) * lateralSpeed"));
	EXPECT(contains(movement,
		"PLAYER_VELY += x_force * sin(my->yaw + PI / 2) * lateralSpeed"));
	EXPECT(contains(controller,
		"float GameController::getLeftXPercentForOmnidirectionalMovement(int player)"));
	EXPECT(contains(controller,
		"float GameController::getLeftYPercentForOmnidirectionalMovement(int player)"));
	EXPECT(contains(controller, "x_force / x_forceMaxForwardThreshold"));
	EXPECT(contains(controller, "x_force / x_forceMaxBackwardThreshold"));
	EXPECT(contains(controller, "y_force / y_forceMaxStrafeThreshold"));
	EXPECT(contains(movement,
		"getLeftXPercentForOmnidirectionalMovement(PLAYER_NUM)"));
	EXPECT(contains(movement,
		"getLeftYPercentForOmnidirectionalMovement(PLAYER_NUM)"));
	EXPECT(contains(movement,
		"getLeftXPercentForPlayerMovement(PLAYER_NUM)"));
	EXPECT(contains(movement,
		"getLeftYPercentForPlayerMovement(PLAYER_NUM)"));
	EXPECT(contains(movement,
		"real_t speedFactor = getSpeedFactor(weightratio"));
	EXPECT(contains(movement, "speedFactor *= speedFactorMult;"));
	EXPECT(contains(movement, "bool rooted = stats[PLAYER_NUM]->getEffectActive(EFF_ROOTED) > 0;"));
	EXPECT(contains(movement, "bool swimming = isPlayerSwimming();"));
	EXPECT(contains(movement, "my->processEntityWind();"));

	// The movement change continues to use the existing 19-byte PMOV packet and
	// server-side collision path; no new client-selected movement authority was
	// introduced.
	EXPECT(contains(movement, "net_packet->len = 19;"));
	EXPECT(contains(networking,
		"if ( !net_packet || net_packet->len < 19 )"));
	EXPECT(contains(networking, "clipMove(&players[player]->entity->x"));
	return true;
}
}

int main()
{
	if ( !testAxesAndAnalogMagnitudes()
		|| !testDiagonalNormalization()
		|| !testMovementIntegrationContract() )
	{
		return 1;
	}
	std::cout << "player movement math tests passed\n";
	return 0;
}
