// SPSC channel ordering, bounded overflow, recovery and concurrent transfer.
#include "../src/engine/PcmQueue.hpp"
#include <cassert>
#include <iostream>
#include <thread>

int main() {
	gorgon::PcmQueue q;
	gorgon::AudioFrame frame{{.1f, -.2f, .3f, -.4f}}, out;
	for (uint64_t i = 0; i < q.CAPACITY; i++) assert(q.push(frame));
	assert(!q.push(frame) && q.overruns == 1);
	assert(q.pop(out) && out.samples[3] == -.4f);
	q.discard();
	assert(!q.pop(out) && q.available() == 0);
	const int total = 200000;
	std::thread producer([&] {
		for (int i = 0; i < total; i++) {
			gorgon::AudioFrame f{{float(i), float(-i), float(i + 1), float(i + 2)}};
			while (!q.push(f)) std::this_thread::yield();
		}
	});
	for (int i = 0; i < total; i++) {
		while (!q.pop(out)) std::this_thread::yield();
		assert(out.samples[0] == float(i) && out.samples[1] == float(-i));
		assert(out.samples[2] == float(i + 1) && out.samples[3] == float(i + 2));
	}
	producer.join();
	assert(q.available() == 0);
	std::cout << "pcm_test: all passed\n";
}
