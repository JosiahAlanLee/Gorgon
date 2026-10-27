// Runs the bridge and follower against a real HydraPy Studio, the way the
// module does, and prints what it sees. Driven by studio_e2e.py.
//
// Usage: studio_probe <seconds> <cv1 volts>
// Prints one line per 50 ms: block_time follower_beat linked moving jumps; for a
// version-3 score, an "out3" line from the version-3 follower and engine instead.
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
	v3::TickFollower ticks;
	v3::EngineV3 engine3;
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
			bool v3 = have && snap.v3();
			if (frame % 32 == 0) {
				if (bridge.poll(seen, snap))
					have = true;
				v3 = have && snap.v3();
				double server = snap.clock.toServer(frameTime + i / sr + offset);
				if (have && snap.clock.ready() && v3)
					ticks.correct(snap.score3->bpm, snap.playhead.moving(), v3::ticksAt(snap.playhead, server, snap.score3->bpm),
					              snap.playhead.transportRevision, snap.playhead.startTick);
				else if (have && snap.clock.ready())
					follower.correct(snap.score, snap.playhead.moving(), snap.playhead.beatAt(snap.score, server),
					                 snap.playhead.transportRevision);
				bridge.setInput(0, true, volts);
			}
			bool moving = have && snap.playhead.moving() && (v3 ? ticks.locked : follower.locked);
			if (v3)
				output = engine3.render(*snap.score3, snap.layer->transport(snap.playhead.startTick), moving, ticks.u,
				                        snap.playhead.transportRevision, options, sr);
			else
				output = outputState.render(snap.score, moving, follower.beat, snap.playhead.transportRevision, options, 1. / sr);
			bool reset = output.reset > 0.f;
			resetHighSamples += reset;
			resetEdges += reset && !wasReset;
			wasReset = reset;
			if (v3)
				ticks.advance(snap.score3->bpm, moving, 1. / sr);
			else
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
			if (have && snap.v3())
				std::printf("out3 %.6f %.3f %d %d %ld %.6f %.6f %.6f %lld %d\n", frameTime, ticks.u, int(bridge.linked()),
				            int(snap.playhead.moving()), ticks.jumps, output.velocity[4], output.pitch[4], output.velocity[5],
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
