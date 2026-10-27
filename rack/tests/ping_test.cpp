// PingMeter: the count from a start to the return's first rising threshold crossing, less the cable steps.
#include "../src/engine/PingMeter.hpp"
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

using gorgon::PingMeter;
using Status = PingMeter::Status;

// Steps `m` through `n` samples of `level(t)`, starting a measurement at sample `at` with `steps`.
template <class F> static void run(PingMeter& m, int64_t& t, int64_t n, int64_t at, int steps, F level) {
	for (int64_t end = t + n; t < end; ++t) {
		if (t == at)
			m.start(steps);
		m.step(level(t));
	}
}

static int64_t last(const PingMeter& m) {
	std::array<int64_t, PingMeter::HISTORY> v;
	int n = m.copy(v);
	assert(n > 0);
	return v[size_t(n - 1)];
}

int main() {
	{   // A pulse sent at s reaches the destination at s+1; its output at s+1+D reaches Ping at s+2+D.
		for (int64_t d : {0, 1, 2, 100, 1103, 47999}) {
			PingMeter m; m.setRate(48000); m.threshold = .01f;
			int64_t t = 0, s = 1000;
			run(m, t, s + d + 10, s, PingMeter::PULSE_STEPS, [&](int64_t x) { return x >= s + 2 + d ? .5f : 0.f; });
			assert(m.state() == Status::Measured && m.total() == 1 && last(m) == d);
		}
	}
	{   // A trigger reaching Ping at s reaches the destination at s too; its output at s+D reaches Ping at s+1+D.
		for (int64_t d : {0, 7, 1103}) {
			PingMeter m; m.setRate(48000);
			int64_t t = 0, s = 500;
			run(m, t, s + d + 10, s, PingMeter::TRIGGER_STEPS, [&](int64_t x) { return x >= s + 1 + d ? -3.f : 0.f; });
			assert(last(m) == d);
		}
	}
	{   // Below the threshold is silence; the first sample at or above it is the onset.
		PingMeter m; m.setRate(48000); m.threshold = .1f;
		int64_t t = 0;
		run(m, t, 400, 300, PingMeter::PULSE_STEPS, [](int64_t x) { return x < 340 ? .099f : x < 350 ? .0999f : .1f; });
		assert(last(m) == 350 - 300 - 2);
	}
	{   // Not quiet: the return was loud within 5 ms before the start. Nothing is recorded.
		PingMeter m; m.setRate(48000);
		int64_t t = 0;
		run(m, t, 2000, 1000, PingMeter::PULSE_STEPS, [](int64_t x) { return x > 1000 - 240 && x < 900 ? 1.f : x > 1500 ? 1.f : 0.f; });
		assert(m.state() == Status::NotQuiet && m.total() == 0);
		// Quiet for exactly 5 ms and one sample is enough.
		PingMeter q; q.setRate(48000);
		t = 0;
		run(q, t, 2000, 1000, PingMeter::PULSE_STEPS, [](int64_t x) { return x == 1000 - 241 ? 1.f : x >= 1100 ? 1.f : 0.f; });
		assert(q.state() == Status::Measured && last(q) == 98);
	}
	{   // A tail that is still above the threshold at the start never counts; the rise after it does.
		PingMeter m; m.setRate(48000); m.quiet = 0;
		int64_t t = 0;
		run(m, t, 2000, 100, PingMeter::PULSE_STEPS, [](int64_t x) { return x < 150 ? 1.f : x < 600 ? 0.f : 1.f; });
		assert(last(m) == 600 - 100 - 2);
	}
	{   // No return within 2 s, a NaN return, and a start while waiting.
		PingMeter m; m.setRate(48000);
		int64_t t = 0;
		run(m, t, 100000, 10, PingMeter::PULSE_STEPS, [](int64_t) { return 0.f; });
		assert(m.state() == Status::NoReturn && m.total() == 0);
		run(m, t, 1000, t + 10, PingMeter::PULSE_STEPS, [](int64_t) { return NAN; });
		assert(m.state() == Status::Waiting);
		int64_t first = t + 10, second = t + 300;
		for (int64_t end = t + 1000; t < end; ++t) {
			if (t == first || t == second)
				m.start(PingMeter::PULSE_STEPS);
			m.step(t >= second + 50 ? 1.f : 0.f);
		}
		assert(m.total() == 1 && last(m) == 48);
	}
	{   // History keeps the last 64, oldest first; order statistics; clear; restore.
		PingMeter m; m.setRate(1000); m.quiet = 0;
		int64_t t = 0;
		for (int i = 0; i < 70; i++) {
			int64_t s = t + 1;
			run(m, t, 40, s, PingMeter::PULSE_STEPS, [&](int64_t x) { return x >= s + 2 + i % 10 ? 1.f : 0.f; });
		}
		std::array<int64_t, PingMeter::HISTORY> v;
		int n = m.copy(v);
		assert(n == 64 && m.total() == 70);
		for (int i = 0; i < 64; i++)
			assert(v[size_t(i)] == (i + 6) % 10);
		auto st = gorgon::pingStats(v, n);
		assert(st.n == 64 && st.last == 9 && st.min == 0 && st.max == 9 && st.median == 5);
		m.clear();
		assert(m.copy(v) == 0 && m.state() == Status::Idle);
		const int64_t saved[3] = {1102, 1104, 1101};
		m.restore(saved, 3);
		assert(m.copy(v) == 3);
		st = gorgon::pingStats(v, 3);
		assert(st.last == 1101 && st.min == 1101 && st.median == 1102 && st.max == 1104);
	}
	{   // A reader copying while the audio thread records sees whole, ordered histories: delays cycle 1..7.
		PingMeter m; m.setRate(48000); m.quiet = 0; m.timeout = 1 << 30;
		std::atomic<bool> done{false};
		std::thread reader([&] {
			std::array<int64_t, PingMeter::HISTORY> v;
			while (!done.load()) {
				int n = m.copy(v);
				for (int i = 1; i < n; i++)
					assert(v[size_t(i)] == v[size_t(i - 1)] % 7 + 1);
			}
		});
		for (int i = 0; i < 20000; i++) {
			m.start(PingMeter::PULSE_STEPS);    // then i % 7 + 3 quiet samples, so the count is i % 7 + 1
			for (int k = 0; k < i % 7 + 3; k++)
				m.step(0.f);
			m.step(1.f);
			m.step(0.f);
		}
		done = true;
		reader.join();
		assert(m.total() == 20000);
	}
	std::printf("ping_test: pulse/trigger cable steps, threshold onset, quiet rule, tails, timeout, restart, 64-result history, concurrent copies\n");
}
