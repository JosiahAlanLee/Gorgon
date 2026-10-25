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
	double offset = argc > 3 ? std::atof(argv[3]) / 1000. : 0.;
	Options options;
	options.pitchZero = argc > 4 ? std::atoi(argv[4]) : 60;
	Bridge bridge(now);
	bridge.start();
	Snapshot snap;
	uint64_t seen = 0;
	bool have = false;
	Follower follower;
	OutputState outputState;
	Outputs output;
	long resetHighSamples = 0;
	int resetEdges = 0;
	bool wasReset = false;
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
					                 snap.playhead.beatAt(snap.score, snap.clock.toServer(frameTime + i / sr + offset)));
				bridge.setInput(0, true, volts);
			}
			bool moving = have && snap.playhead.moving() && follower.locked;
			output = outputState.render(snap.score, moving, follower.beat, snap.playhead.transportRevision, options, 1. / sr);
			bool reset = output.reset > 0.f;
			resetHighSamples += reset;
			resetEdges += reset && !wasReset;
			wasReset = reset;
			follower.advance(snap.score, moving, 1. / sr);
		}
		frameTime += 256. / sr;
		double t = now();
		if (t >= nextPrint) {
			// Stamped with the block's own time: the beat belongs to the block's end.
			std::printf("%.6f %.6f %d %d %ld\n", frameTime, follower.beat, int(bridge.linked()), int(snap.playhead.moving()), follower.jumps);
			std::printf("out %.6f %d %d %.6f %.6f %.6f %lld %d\n", follower.beat, int(bridge.linked()),
			            int(snap.playhead.moving()), output.gate[0], output.velocity[0], output.pitch[0],
			            static_cast<long long>(snap.playhead.transportRevision), resetEdges);
			std::fflush(stdout);
			nextPrint += .05;
		}
		double wait = frameTime - now();
		if (wait > 0)
			std::this_thread::sleep_for(std::chrono::duration<double>(wait));
	}
	std::printf("status %s\n", bridge.status().c_str());
	std::printf("target0 %s\n", bridge.target(0).c_str());
	std::printf("reset_samples %ld\n", resetHighSamples);
	bridge.stop();
	return 0;
}
