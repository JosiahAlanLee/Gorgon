// SPDX-License-Identifier: BSD-3-Clause
// Rack-side ByteBus sender. Contract: spec/bytebus-output.md.
// No Rack dependency, allocation, or clock independent of the Gorgon follower.
#pragma once
#include "GorgonEngine.hpp"

namespace gorgon {

enum class BusSource { Z, Roll, Feedback, HitMask };

inline BusSource busSourceFromInt(int source) {
	return source >= int(BusSource::Z) && source <= int(BusSource::HitMask)
		? BusSource(source) : BusSource::Z;
}

// `v3`: the score is version 3, whose hit mask has six lanes and FILL (bytebus-output.md#version-3).
inline const char* busSourceName(BusSource source, bool v3 = false) {
	switch (source) {
		case BusSource::Roll: return "Roll";
		case BusSource::Feedback: return "Feedback";
		case BusSource::HitMask: return v3 ? "Hit mask (lanes 1–6, FILL)" : "Hit mask (lanes 1–4)";
		default: return "Z";
	}
}

inline uint8_t busWord(const Outputs& output, BusSource source) {
	if (source == BusSource::HitMask) {
		// Versions 1 and 2 leave lanes 5 and 6 and FILL clear, so their word is unchanged.
		uint8_t word = 0;
		for (int lane = 0; lane < OUTPUT_LANES; ++lane)
			if (output.present[lane]) word |= uint8_t(1u << lane);
		if (output.fill) word |= uint8_t(1u << 6);
		return word;
	}
	int lane = int(busSourceFromInt(int(source)));
	double value = output.lane[lane];
	if (!std::isfinite(value)) return 0;
	// The engine supplies float samples. Use their float endpoints too, so
	// Feedback's 0.45f midpoint is exactly halfway between 0 and 0.9f.
	double low = float(LANE_MIN[lane]), high = float(LANE_MAX[lane]);
	double unit = (value - low) / (high - low);
	unit = std::max(0., std::min(1., unit));
	return uint8_t(std::floor(255. * unit + .5));
}

struct BusFrame {
	uint8_t word = 0;
	bool strobe = false;

	template <class Output> void write(Output& output) const {
		output.setChannels(9);
		for (int bit = 0; bit < 8; ++bit)
			output.setVoltage(word & (1u << bit) ? 10.f : 0.f, bit);
		output.setVoltage(strobe ? 10.f : 0.f, 8);
	}
};

struct ByteBusOutput {
	BusFrame held;
	bool clockHigh = false, wasMoving = false, pending = false;
	int64_t transportRevision = -1;

	BusFrame render(const Outputs& output, bool moving, int64_t transport, BusSource source) {
		bool high = moving && output.clock > 0.f;
		bool edge = high && !clockHigh;
		bool publishTransport = transport != transportRevision || (moving && !wasMoving);
		clockHigh = high;
		wasMoving = moving;
		transportRevision = transport;
		if (!moving) {
			held.strobe = pending = false;
			return held;
		}
		pending |= publishTransport;
		if (held.strobe) {
			held.strobe = false; // Mandatory low sample; preserve pending transport.
			return held;
		}
		if (pending || edge) {
			held.word = busWord(output, source);
			held.strobe = true;
			pending = false;
		}
		return held;
	}
};

} // namespace gorgon
