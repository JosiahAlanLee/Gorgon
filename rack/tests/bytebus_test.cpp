#include "../src/engine/ByteBusOutput.hpp"
#include <array>
#include <cassert>
#include <iostream>
#include <limits>

using namespace gorgon;

struct Port {
	int channels = 0;
	std::array<float, 16> voltage{};
	void setChannels(int count) { channels = count; }
	void setVoltage(float value, int channel) { voltage[channel] = value; }
};

static void codesAndWire() {
	for (int lane = 0; lane < AUTO_LANES; ++lane) {
		Outputs output;
		for (int word = 0; word < 256; ++word) {
			output.lane[lane] = float(LANE_MIN[lane] + word / 255. * (LANE_MAX[lane] - LANE_MIN[lane]));
			assert(busWord(output, BusSource(lane)) == word);
		}
		output.lane[lane] = float(LANE_MIN[lane] - 1.);
		assert(busWord(output, BusSource(lane)) == 0);
		output.lane[lane] = float(LANE_MAX[lane] + 1.);
		assert(busWord(output, BusSource(lane)) == 255);
		output.lane[lane] = float((LANE_MIN[lane] + LANE_MAX[lane]) / 2.);
		assert(busWord(output, BusSource(lane)) == 128);
		for (float invalid : {std::numeric_limits<float>::infinity(),
			-std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
			output.lane[lane] = invalid;
			assert(busWord(output, BusSource(lane)) == 0);
		}
	}
	for (int word = 0; word < 256; ++word) {
		for (bool strobe : {false, true}) {
			Port port;
			BusFrame{uint8_t(word), strobe}.write(port);
			assert(port.channels == 9);
			unsigned decoded = 0;
			for (int bit = 0; bit < 8; ++bit) {
				assert(port.voltage[bit] == 0.f || port.voltage[bit] == 10.f);
				if (port.voltage[bit] == 10.f) decoded |= 1u << bit;
			}
			assert(decoded == unsigned(word));
			assert(port.voltage[8] == (strobe ? 10.f : 0.f));
		}
	}
	assert(busSourceFromInt(-1) == BusSource::Z && busSourceFromInt(4) == BusSource::Z);
}

static void masks() {
	for (unsigned mask = 0; mask < 16; ++mask) {
		Outputs output;
		for (int lane = 0; lane < 4; ++lane) output.present[lane] = mask & (1u << lane);
		assert(busWord(output, BusSource::HitMask) == mask);
	}
	Score score;
	for (int lane = 0; lane < 4; ++lane) {
		score.patterns[0].hit[lane][0] = true;
		score.patterns[0].velocity[lane][0] = 127;
	}
	score.mutes[1] = true;
	score.patterns[0].velocity[2][0] = 0;
	// All hits are AIR (pitched=false). Sample after a short gate has closed.
	Options options;
	options.gate = .01;
	auto output = evaluate(score, true, .2, options);
	for (float gate : output.gate) assert(gate == 0.f);
	assert(busWord(output, BusSource::HitMask) == 9);
	assert(busWord(evaluate(score, true, .25, options), BusSource::HitMask) == 0);
}

static void transportAndHold() {
	ByteBusOutput bus;
	Outputs output;
	output.lane[LANE_Z] = .5f;
	assert(bus.render(output, false, 0, BusSource::Z).word == 0);
	// A late start, even in CLOCK's low phase, publishes immediately.
	auto first = bus.render(output, true, 0, BusSource::Z);
	assert(first.strobe && first.word == 128);
	output.lane[LANE_Z] = 1.f;
	assert(!bus.render(output, true, 0, BusSource::Z).strobe);
	for (int i = 0; i < 20; ++i) {
		auto held = bus.render(output, true, 0, BusSource::Z);
		assert(!held.strobe && held.word == 128);
	}
	output.clock = 10.f;
	assert(bus.render(output, true, 0, BusSource::Z).word == 255);
	for (int i = 0; i < 20; ++i) assert(!bus.render(output, true, 0, BusSource::Z).strobe);
	output.clock = 0.f;
	bus.render(output, true, 0, BusSource::Z);
	output.clock = 10.f;
	assert(bus.render(output, true, 0, BusSource::Z).strobe); // repeated FF still clocks
	// A seek immediately after that strobe inserts a low gap, coalescing to
	// the latest playhead's value before publishing. High CLOCK adds no pulse.
	output.lane[LANE_Z] = 0.f;
	auto gap = bus.render(output, true, 1, BusSource::Z);
	assert(!gap.strobe && gap.word == 255);
	output.lane[LANE_Z] = .5f;
	auto seek = bus.render(output, true, 2, BusSource::Z);
	assert(seek.strobe && seek.word == 128);
	for (int i = 0; i < 20; ++i) assert(!bus.render(output, true, 2, BusSource::Z).strobe);
	// Source changes alone do not publish; the next clock acquires Roll.
	output.lane[LANE_ROLL] = -45.f;
	auto changed = bus.render(output, true, 2, BusSource::Roll);
	assert(!changed.strobe && changed.word == 128);
	output.clock = 0.f;
	bus.render(output, true, 2, BusSource::Roll);
	output.clock = 5.f;
	auto next = bus.render(output, true, 2, BusSource::Roll);
	assert(next.strobe && next.word == 0);
	// A pending seek is cancelled by stopping, with no fallback clock.
	bus.render(output, true, 3, BusSource::Z);
	for (int i = 0; i < 200; ++i) {
		auto stopped = bus.render(output, false, 3, BusSource::Z);
		assert(!stopped.strobe && stopped.word == 0);
	}
	auto resume = bus.render(output, true, 3, BusSource::Z);
	assert(resume.strobe && resume.word == 128);
	assert(!bus.render(output, true, 3, BusSource::Z).strobe);
	assert(!bus.render(output, true, 3, BusSource::Z).strobe);
}

static void sampleClocks() {
	for (double rate : {44100., 48000., 96000.}) {
		for (double high : {5., 10.}) {
			Score score;
			auto& lane = score.patterns[0].lanes[LANE_Z];
			lane.count = 2; lane.ease = EASE_LINEAR;
			lane.keys[0] = {0, 0.}; lane.keys[1] = {8, 1.};
			Options options;
			options.high = high;
			ByteBusOutput bus;
			bool lastClock = false, lastStrobe = false;
			uint8_t held = 0;
			int publications = 0;
			for (int sample = 0; sample < int(rate); ++sample) {
				auto output = evaluate(score, true, 2. * sample / rate, options);
				bool clock = output.clock > 0.f;
				auto frame = bus.render(output, true, 1, BusSource::Z);
				assert(frame.strobe == (clock && !lastClock));
				assert(!(frame.strobe && lastStrobe));
				if (frame.strobe) {
					++publications;
					int row = int(std::floor(8. * sample / rate + 1e-9));
					assert(std::abs(int(frame.word) - int(std::floor(row * 255. / 8 + .5))) <= 1);
					held = frame.word;
				} else assert(frame.word == held);
				lastClock = clock; lastStrobe = frame.strobe;
			}
			assert(publications == 8);
		}
		std::cout << int(rate) << " Hz: clock timing, holds and both CV profiles passed\n";
	}
	Score loop;
	loop.loop = true; loop.pattern = 3;
	ByteBusOutput bus;
	bus.render(evaluate(loop, true, 63.9, Options()), true, 1, BusSource::Z);
	bus.render(evaluate(loop, true, 63.99, Options()), true, 1, BusSource::Z);
	assert(bus.render(evaluate(loop, true, 48., Options()), true, 1, BusSource::Z).strobe);
}

int main() {
	codesAndWire(); masks(); transportAndHold(); sampleClocks();
	std::cout << "bytebus_test: all passed\n";
}
