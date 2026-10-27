// SPDX-License-Identifier: BSD-3-Clause
// A byte-bus input, read on Rack's audio thread: DSP_Emulation's BYTE_BUS_SPEC.md
// §3–5, implemented here from the specification. No Rack dependency or allocation.
#pragma once
#include <cmath>
#include <cstdint>

namespace gorgon {

struct ByteBusReader {
	static constexpr int BITS = 8, CLK = 8;
	static constexpr float V_HIGH = 1.f, V_LOW = .1f;

	bool connected = false, clocked = false;
	bool line[9] = {};
	uint8_t word = 0;   // held between CLK edges when clocked; current when unclocked

	void reset() {
		connected = clocked = false;
		for (bool& l : line)
			l = false;
		word = 0;
	}

	// One engine sample. `channels` is the cable's channel count, 0 when disconnected;
	// `volts(c)` reads channel c. Returns the word, or -1 while disconnected.
	template <class Volts> int process(int channels, Volts&& volts) {
		if (channels <= 0) {
			reset();
			return -1;
		}
		bool isClocked = channels >= 9;
		// Connecting, or switching mode, clears the lines: a CLK already high counts as an edge.
		if (!connected || isClocked != clocked) {
			for (bool& l : line)
				l = false;
			connected = true;
			clocked = isClocked;
		}
		bool wasClock = line[CLK];
		for (int c = 0; c < (clocked ? 9 : BITS); c++) {
			float v = c < channels ? float(volts(c)) : 0.f;
			if (!std::isfinite(v) || v <= V_LOW)
				line[c] = false;
			else if (v >= V_HIGH)
				line[c] = true;
		}
		uint8_t raw = 0;
		for (int b = 0; b < BITS; b++)
			raw |= uint8_t(line[b]) << b;
		if (!clocked)
			word = raw;
		else if (line[CLK] && !wasClock)
			word = raw;
		return word;
	}
};

} // namespace gorgon
