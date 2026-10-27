// Ping's measurement: a start (a pulse Ping sends, or a trigger it receives) begins a count, and the first
// rising crossing of a threshold on the return ends it. No Rack dependency, no allocation.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace gorgon {

struct PingMeter {
	enum class Status { Idle, Waiting, Measured, NoReturn, NotQuiet };
	// A Rack cable takes one sample. A pulse Ping sends reaches the destination a sample later, and the
	// destination's output reaches Ping a sample after it leaves, so a pulse's count drops two samples. A
	// trigger Ping receives reaches the destination on the same sample, so its count drops one. What remains
	// is the destination's own latency: from the sample its input sees the start to the sample its output
	// crosses the threshold.
	static constexpr int PULSE_STEPS = 2, TRIGGER_STEPS = 1;
	static constexpr int HISTORY = 64;

	float threshold = .01f;   // volts, on the loudest return channel
	int64_t quiet = 240;      // the return must stay under the threshold this long before a start
	int64_t timeout = 96000;  // samples to wait for a return

	void setRate(float rate) {
		quiet = std::max<int64_t>(1, std::llround(.005 * rate));
		timeout = std::max<int64_t>(1, std::llround(2. * rate));
	}

	// A start on this sample, before step(). A start while waiting abandons that wait.
	void start(int steps) {
		if (now - lastLoud <= quiet) {
			waiting = false;
			status.store(Status::NotQuiet, std::memory_order_relaxed);
			return;
		}
		waiting = true;
		begun = now;
		correction = steps;
		status.store(Status::Waiting, std::memory_order_relaxed);
	}

	// Every sample: the return, as the loudest channel's level (its sign doesn't matter).
	void step(float level) {
		bool loud = std::isfinite(level) && std::fabs(level) >= threshold;
		if (waiting && now > begun) {
			if (loud && !above) {
				record(now - begun - correction);
				waiting = false;
				status.store(Status::Measured, std::memory_order_relaxed);
			}
			else if (now - begun >= timeout) {
				waiting = false;
				status.store(Status::NoReturn, std::memory_order_relaxed);
			}
		}
		if (loud)
			lastLoud = now;
		above = loud;
		++now;
	}

	// Audio thread: forget every result.
	void clear() {
		count.store(0, std::memory_order_release);
		waiting = false;
		status.store(Status::Idle, std::memory_order_relaxed);
	}

	// Any thread: the results so far, oldest first, up to HISTORY of them. Returns how many were copied.
	// The ring has one slot more than it shows, so the slot being written is never one being read; a count
	// that moved during the copy means a slot may have been reused, and the copy starts again.
	int copy(std::array<int64_t, HISTORY>& out) const {
		for (;;) {
			int total = count.load(std::memory_order_acquire);
			int n = std::min(total, HISTORY);
			for (int i = 0; i < n; ++i)
				out[size_t(i)] = results[size_t((total - n + i) % RING)].load(std::memory_order_relaxed);
			if (count.load(std::memory_order_acquire) == total)
				return n;
		}
	}
	int total() const { return count.load(std::memory_order_acquire); }
	Status state() const { return status.load(std::memory_order_relaxed); }

	// Restoring saved results (not while the audio thread steps).
	void restore(const int64_t* values, int n) {
		n = std::min(n, HISTORY);
		for (int i = 0; i < n; ++i)
			results[size_t(i)].store(values[i], std::memory_order_relaxed);   // slots 0..n-1 for counts 0..n-1
		count.store(n, std::memory_order_release);
		status.store(n ? Status::Measured : Status::Idle, std::memory_order_relaxed);
	}

private:
	void record(int64_t samples) {
		int total = count.load(std::memory_order_relaxed);
		results[size_t(total % RING)].store(samples, std::memory_order_relaxed);
		count.store(total + 1, std::memory_order_release);
	}

	static constexpr int RING = HISTORY + 1;
	std::array<std::atomic<int64_t>, RING> results{};
	std::atomic<int> count{0};
	std::atomic<Status> status{Status::Idle};
	int64_t now = 0, begun = 0, lastLoud = INT64_MIN / 2;
	int correction = 0;
	bool waiting = false, above = false;
};

// Order statistics of a copy of the results.
struct PingStats {
	int n = 0;
	int64_t last = 0, min = 0, median = 0, max = 0;
};
inline PingStats pingStats(std::array<int64_t, PingMeter::HISTORY> values, int n) {
	PingStats s;
	s.n = n;
	if (n <= 0)
		return s;
	s.last = values[size_t(n - 1)];
	std::sort(values.begin(), values.begin() + n);
	s.min = values[0];
	s.max = values[size_t(n - 1)];
	s.median = values[size_t(n / 2)];
	return s;
}

} // namespace gorgon
