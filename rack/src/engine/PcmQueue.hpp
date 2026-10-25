// Single producer (device callback), single consumer (Rack engine).
// PCM only: no control/encoded-data stream, allocation, waiting or network I/O.
#pragma once
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace gorgon {
struct AudioFrame { float samples[4] = {}; uint64_t generation = 0; };

class PcmQueue {
public:
	static constexpr uint64_t CAPACITY = 16384;
	std::atomic<uint64_t> overruns{0};
	bool push(const AudioFrame& frame) {
		uint64_t w = written.load(std::memory_order_relaxed);
		if (w - read.load(std::memory_order_acquire) >= CAPACITY) {
			overruns.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		data[w % CAPACITY] = frame;
		written.store(w + 1, std::memory_order_release);
		return true;
	}
	bool pop(AudioFrame& frame) {
		uint64_t r = read.load(std::memory_order_relaxed);
		if (r == written.load(std::memory_order_acquire)) return false;
		frame = data[r % CAPACITY];
		read.store(r + 1, std::memory_order_release);
		return true;
	}
	uint64_t available() const {
		return written.load(std::memory_order_acquire) - read.load(std::memory_order_acquire);
	}
	// Only the consumer may discard. The producer never resets the reader's cursor.
	void discard() { read.store(written.load(std::memory_order_acquire), std::memory_order_release); }
private:
	std::array<AudioFrame, CAPACITY> data{};
	std::atomic<uint64_t> written{0}, read{0};
};
} // namespace gorgon
