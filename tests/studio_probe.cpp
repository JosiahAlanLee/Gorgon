// Runs the bridge and follower against a real HydraPy Studio, the way the
// module does, and prints what it sees. Driven by studio_e2e.py.
//
// Usage: studio_probe <seconds> <cv1 volts>
// Prints one line per 50 ms: block_time follower_beat linked moving jumps
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "../src/engine/Bridge.hpp"

using namespace gorgon;

static double now() {
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
	double seconds = argc > 1 ? std::atof(argv[1]) : 3.;
	float volts = argc > 2 ? float(std::atof(argv[2])) : 5.f;
	Bridge bridge(now);
	bridge.start();
	Snapshot snap;
	uint64_t seen = 0;
	bool have = false;
	Follower follower;
	const double sr = 48000.;
	double start = now(), nextPrint = start, frameTime = start;
	long frame = 0;
	// Render "audio" in 256-frame blocks paced to real time, like an engine.
	while (now() - start < seconds) {
		for (int i = 0; i < 256; i++, frame++) {
			if (frame % 32 == 0) {
				if (bridge.poll(seen, snap))
					have = true;
				if (have && snap.clock.ready())
					follower.correct(snap.score, snap.playhead.moving(),
					                 snap.playhead.beatAt(snap.score, snap.clock.toServer(frameTime + i / sr)));
				bridge.setInput(0, true, volts);
			}
			follower.advance(snap.score, have && snap.playhead.moving() && follower.locked, 1. / sr);
		}
		frameTime += 256. / sr;
		double t = now();
		if (t >= nextPrint) {
			// Stamped with the block's own time: the beat belongs to the block's end.
			std::printf("%.6f %.6f %d %d %ld\n", frameTime, follower.beat, int(bridge.linked()), int(snap.playhead.moving()), follower.jumps);
			std::fflush(stdout);
			nextPrint += .05;
		}
		double wait = frameTime - now();
		if (wait > 0)
			std::this_thread::sleep_for(std::chrono::duration<double>(wait));
	}
	std::printf("status %s\n", bridge.status().c_str());
	std::printf("target0 %s\n", bridge.target(0).c_str());
	bridge.stop();
	return 0;
}
