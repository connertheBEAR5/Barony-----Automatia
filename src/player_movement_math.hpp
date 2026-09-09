#pragma once

#include <cmath>

namespace PlayerMovementMath
{
	// The two values are the signed input strengths before the player's
	// existing speed factor and acceleration are applied.  Analog input below
	// unit magnitude is intentionally left unchanged; only a combined digital
	// (or saturated analog) vector is bounded to unit length.
	struct InputVector
	{
		double strafe = 0.0;
		double forward = 0.0;
	};

	inline InputVector normalize(InputVector input)
	{
		const double magnitude = std::hypot(input.strafe, input.forward);
		if ( magnitude > 1.0 )
		{
			input.strafe /= magnitude;
			input.forward /= magnitude;
		}
		return input;
	}
}
