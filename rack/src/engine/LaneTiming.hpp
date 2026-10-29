#pragma once
#include <algorithm>
#include <cmath>

namespace gorgon {
// Local output compensation, not a change to the score's timing contract.
inline float laneOffset(float ms) {
	return std::isfinite(ms) ? std::clamp(ms, -100.f, 100.f) : 0.f;
}
inline double lanePosition(double position, double bpm, double unitsPerBeat, float ms) {
	return position - double(laneOffset(ms)) * bpm * unitsPerBeat / 60000.;
}
}
