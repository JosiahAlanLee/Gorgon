// The Studio link against a fake Studio on loopback (POSIX only): discovery,
// the token and Host headers, score caching by revision, CV values, transport,
// and recovery when Studio restarts with a new token.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../src/engine/Bridge.hpp"

using namespace gorgon;

static int failures = 0;
#define CHECK(cond, ...)                                                       \
	do {                                                                       \
		if (!(cond)) {                                                         \
			failures++;                                                        \
			std::printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);        \
			std::printf(__VA_ARGS__);                                          \
			std::printf("\n");                                                 \
		}                                                                      \
	} while (0)

static double now() {
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static std::string scoreJson() {
	std::string rows;
	auto lane = [](bool hits) {
		std::string s = "[";
		for (int r = 0; r < 64; r++)
			s += std::string(r ? "," : "") + (hits && r % 4 == 0 ? "{\"note\":\"C-2\",\"vel\":100}" : "null");
		return s + "]";
	};
	std::string patterns;
	for (int p = 0; p < 4; p++)
		patterns += std::string(p ? "," : "") + "{\"name\":\"P\",\"audio\":[" + lane(true) + "," + lane(false) + "," +
		            lane(false) + "," + lane(false) +
		            "],\"keys\":{\"z\":[{\"r\":0,\"v\":0.5}],\"roll\":[],\"feedback\":[]},"
		            "\"easing\":{\"z\":\"hold\",\"roll\":\"linear\",\"feedback\":\"smooth\"}}";
	return "{\"format\":\"hydrapy-tracker-1\",\"bpm\":138,\"key\":2,\"loop\":false,\"pattern\":0,\"sound\":false,"
	       "\"mutes\":[false,false,false,false],\"modes\":{\"program\":\"o0\",\"aux\":\"o1\"},"
	       "\"bindings\":{\"z\":\"Dolly Z\",\"roll\":\"Roll\",\"feedback\":\"\"},\"patterns\":[" + patterns + "]}";
}

// A version-3 document: one pattern at rate 1 with a hit on lane 6, row 0, the rest empty.
static std::string scoreJsonV3(int bpm) {
	std::string lanes;
	for (int l = 0; l < 6; l++)
		lanes += std::string(l ? "," : "") + "{\"len\":4,\"cells\":[" + (l == 5 ? "{\"note\":\"C-4\",\"vel\":90}" : "null") +
		         ",null,null,null],\"transforms\":[]}";
	std::string patterns = "{\"name\":\"A01\",\"rate\":1,\"bar\":4,\"bars\":1,\"lanes\":[" + lanes +
	                       "],\"keys\":{\"z\":[{\"r\":2,\"v\":0.5}],\"roll\":[],\"feedback\":[]},"
	                       "\"easing\":{\"z\":\"hold\",\"roll\":\"smooth\",\"feedback\":\"smooth\"},\"scenes\":[]}";
	for (int p = 1; p < 128; p++)
		patterns += ",null";
	std::string sounds, mutes;
	for (int l = 0; l < 6; l++) {
		sounds += std::string(l ? "," : "") + "{\"machine\":null,\"params\":{}}";
		mutes += std::string(l ? "," : "") + "false";
	}
	return "{\"format\":\"hydrapy-tracker-3\",\"bpm\":" + std::to_string(bpm) + ",\"key\":2,\"loop\":true,\"pattern\":0,"
	       "\"chain\":[0],\"seed\":0,\"mutes\":[" + mutes + "],\"sounds\":[" + sounds + "],\"patterns\":[" + patterns + "]}";
}

struct FakeStudio {
	int listener = -1, port = 0;
	std::string token = "first-token";
	std::atomic<bool> running{true};
	std::thread thread;
	std::mutex mutex;
	std::vector<std::string> requests;   // request line + headers + body
	std::atomic<int> revision{7};
	std::atomic<bool> v3{false};         // serve a version-3 document and playhead
	std::atomic<bool> busHeld{false};    // refuse bus windows: another module holds the layer

	FakeStudio() {
		listener = ::socket(AF_INET, SOCK_STREAM, 0);
		int one = 1;
		::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		::bind(listener, reinterpret_cast<sockaddr*>(&a), sizeof a);
		socklen_t n = sizeof a;
		::getsockname(listener, reinterpret_cast<sockaddr*>(&a), &n);
		port = ntohs(a.sin_port);
		::listen(listener, 8);
		thread = std::thread([this] { serve(); });
	}
	~FakeStudio() {
		running = false;
		::shutdown(listener, SHUT_RDWR);
		::close(listener);
		thread.join();
	}
	void setToken(const std::string& t) {
		std::lock_guard<std::mutex> lock(mutex);
		token = t;
	}
	std::string getToken() {
		std::lock_guard<std::mutex> lock(mutex);
		return token;
	}
	std::vector<std::string> take() {
		std::lock_guard<std::mutex> lock(mutex);
		std::vector<std::string> out;
		out.swap(requests);
		return out;
	}
	void serve() {
		while (running) {
			int c = ::accept(listener, nullptr, nullptr);
			if (c < 0)
				return;
			std::string req;
			char buf[4096];
			// Read headers, then Content-Length bytes of body.
			while (req.find("\r\n\r\n") == std::string::npos) {
				int k = int(::recv(c, buf, sizeof buf, 0));
				if (k <= 0)
					break;
				req.append(buf, size_t(k));
			}
			size_t split = req.find("\r\n\r\n");
			size_t length = 0;
			size_t at = req.find("Content-Length: ");
			if (at != std::string::npos && at < split)
				length = size_t(std::atol(req.c_str() + at + 16));
			while (split != std::string::npos && req.size() < split + 4 + length) {
				int k = int(::recv(c, buf, sizeof buf, 0));
				if (k <= 0)
					break;
				req.append(buf, size_t(k));
			}
			{
				std::lock_guard<std::mutex> lock(mutex);
				requests.push_back(req);
			}
			std::string body, status = "200 OK";
			bool authorized = req.find("X-Hydrapy-Token: " + getToken() + "\r\n") != std::string::npos &&
			                  req.find("Host: 127.0.0.1:" + std::to_string(port) + "\r\n") != std::string::npos;
			if (!authorized) {
				status = "403 Forbidden";
				body = "{\"error\":\"Local session token required\"}";
			}
			else if (req.compare(0, 16, "GET /api/bridge ") == 0 || req.compare(0, 16, "GET /api/bridge?") == 0) {
				int r = revision;
				bool cached = req.find("?revision=" + std::to_string(r) + " ") != std::string::npos;
				std::string mutes, playhead;
				for (int l = 0; l < 6; l++)
					mutes += std::string(l ? "," : "") + (l == 2 ? "[{\"at\":0,\"on\":false},{\"at\":960,\"on\":true}]" : "[{\"at\":0,\"on\":false}]");
				if (v3)
					playhead = "\"start\":{\"tick\":1440,\"entry\":null},\"elapsed\":96,\"entry\":null,"
					           "\"fill\":[{\"at\":0,\"on\":true}],\"bus\":{\"fill\":[{\"at\":0,\"on\":false}],\"mutes\":[" + mutes + "]},";
				body = "{\"revision\":" + std::to_string(r) + "," + playhead + "\"enabled\":true,\"playing\":false,"
				       "\"transport_revision\":23,\"anchor_beat\":12.5,\"anchor_time\":3.0,\"clock_time\":3.0,\"wall\":100.0,\"server_now\":100.01,"
				       "\"controls\":[{\"name\":\"Dolly Z\",\"min\":0,\"max\":1,\"value\":0.35},"
				       "{\"name\":\"Roll\",\"min\":-45,\"max\":45,\"value\":0},"
				       "{\"name\":\"Warp\",\"min\":-2,\"max\":2,\"value\":0},"
				       "{\"name\":\"Hue\",\"min\":0,\"max\":1,\"value\":0.5}]" +
				       (cached ? std::string() : ",\"document\":" + (v3 ? scoreJsonV3(100 + r) : scoreJson())) + "}";
			}
			else if (busHeld && req.find("\"bus\":{") != std::string::npos) {
				status = "400 Bad Request";
				body = "{\"error\":\"bus held by another module\"}";
			}
			else {
				body = "{\"applied\":[],\"ignored\":{}}";
			}
			std::string out = "HTTP/1.0 " + status + "\r\nContent-Type: application/json\r\nContent-Length: " +
			                  std::to_string(body.size()) + "\r\n\r\n" + body;
			::send(c, out.data(), out.size(), 0);
			::close(c);
		}
	}
};

static void advertise(const std::string& path, int port, const std::string& token) {
	std::ofstream(path) << "{\"url\":\"http://127.0.0.1:" << port << "\",\"port\":" << port << ",\"token\":\"" << token
	                    << "\",\"pid\":1,\"root\":\"/x\"}";
}

template <typename F>
static bool waitFor(F f, double seconds = 3.) {
	double end = now() + seconds;
	while (now() < end) {
		if (f())
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return false;
}

static bool any(const std::vector<std::string>& rs, const std::string& needle) {
	for (const std::string& r : rs)
		if (r.find(needle) != std::string::npos)
			return true;
	return false;
}

// The trace ring copied on one thread while another writes: every copied record whole.
static void traceRing() {
	v3::TraceRing ring(64);
	std::atomic<bool> done{false};
	std::thread writer([&] {
		for (int k = 0; k < 200000; k++)
			ring.write(v3::busRecord(k % 7, k, k % 2, k, k, k, -1, k));
		done = true;
	});
	long copies = 0, torn = 0;
	while (!done) {
		for (const v3::TraceRecord& r : ring.copy())
			torn += r.at != r.frame || r.received != r.frame || r.revision != r.frame || r.tick != r.frame;
		copies++;
	}
	writer.join();
	CHECK(torn == 0 && copies > 0, "%ld torn records in %ld copies", torn, copies);
}

// CHANGE overflow leaves reserved ring slots for ordered SEGMENT and DISCONNECT delivery.
static void busReservedMessages() {
	v3::BusChannel<64> channel;
	v3::BusChange change;
	for (int i = 0; i < 128; i++) channel.push(change);
	CHECK(channel.lost() == 80, "changes fill only the 48 unreserved slots");
	change.kind = v3::BusChange::SEGMENT;
	change.revision = 42;
	change.tick = 123;
	change.states = 0x7f;
	channel.push(change);
	change.kind = v3::BusChange::DISCONNECT;
	channel.push(change);
	v3::BusChange got;
	int n = 0;
	while (channel.ring.pop(got)) {
		if (n == 48)
			CHECK(got.kind == v3::BusChange::SEGMENT && got.revision == 42 && got.tick == 123 && got.states == 0x7f,
			      "SEGMENT survives overflow in order");
		if (n == 49)
			CHECK(got.kind == v3::BusChange::DISCONNECT, "DISCONNECT survives overflow in order");
		n++;
	}
	CHECK(n == 50 && channel.lost() == 80, "reserved messages were retained");
}

// BUS IN's channel with the audio thread and the worker on their own threads (#21): an
// audio-rate bus overflows a small ring while the worker drains only now and then, yet
// once the cable is pulled the worker's record is disconnected and all clear.
static void busChannelThreads() {
	v3::ScoreV3 score;   // no cycle: read ticks are the stamps
	v3::BusChannel<64> channel;
	std::atomic<bool> done{false};
	std::thread audio([&] {
		v3::BusInput bus;
		v3::BusMap map;
		v3::BusChange out[10];
		for (long s = 0; s < 200000; s++) {
			int word = s < 199990 ? int(s * 2654435761u >> 24) & 0x7f : -1;
			int n = bus.process(word, map, score, s < 100000 ? 5 : 6, 0, -1, s, out);
			channel.send(out, n, bus.state());
		}
		done = true;
	});
	v3::BusRecord record;
	while (!done) {
		channel.drain(record, &score);
		std::this_thread::sleep_for(std::chrono::microseconds(200));
	}
	audio.join();
	channel.drain(record, &score);
	bool clear = true;
	for (const auto& l : record.lists)
		clear = clear && !l.back().on;
	CHECK(channel.lost() > 0 && channel.resyncs() > 0, "the ring overflowed (%llu lost, %llu resyncs)",
	      (unsigned long long)channel.lost(), (unsigned long long)channel.resyncs());
	CHECK(record.revision == 6 && !record.connected && clear, "the record ends disconnected and clear (revision %lld)",
	      (long long)record.revision);
	record.acknowledge(record.sequence);
	CHECK(!record.active, "the lease is released");
}

int main() {
	traceRing();
	busReservedMessages();
	busChannelThreads();
	CHECK(noteMidi("C4") == 60 && noteMidi("C-4") == 60 && noteMidi("D#4") == 63, "note spellings");
	CHECK(noteMidi("C0") == 12 && noteMidi("G9") == 127, "pitch range");
	for (const char* note : {"AIR", "B#4", "E#4", "C-", "H4", "G#9", "A-10"})
		CHECK(noteMidi(note) == -1, "unpitched/invalid note %s", note);

	char dir[] = "/tmp/gorgon-bridge-XXXXXX";
	CHECK(::mkdtemp(dir) != nullptr, "mkdtemp");
	std::string ad = std::string(dir) + "/studio.json";

	{
		// No Studio advertised: a clear status, nothing published.
		Bridge idle(now, ad, .01);
		idle.start();
		std::this_thread::sleep_for(std::chrono::milliseconds(60));
		CHECK(idle.status() == "No Studio running", "status '%s'", idle.status().c_str());
		CHECK(!idle.linked(), "not linked");
	}

	FakeStudio studio;
	advertise(ad, studio.port, studio.getToken());
	Bridge bridge(now, ad, .01);
	bridge.start();
	Snapshot snap;
	uint64_t seen = 0;
	CHECK(waitFor([&] { return bridge.poll(seen, snap); }), "no snapshot");
	CHECK(snap.playhead.valid && snap.playhead.enabled && !snap.playhead.playing, "playhead flags");
	CHECK(snap.playhead.revision == 7 && snap.playhead.anchorBeat == 12.5, "playhead values");
	CHECK(snap.playhead.transportRevision == 23, "transport revision reaches the audio thread");
	CHECK(snap.score.bpm == 138. && snap.score.patterns[0].hit[0][4] && !snap.score.patterns[0].hit[1][4], "score");
	CHECK(snap.score.patterns[0].pitched[0][4] && snap.score.patterns[0].midi[0][4] == 36, "parsed note");
	CHECK(snap.score.patterns[2].lanes[LANE_Z].count == 1 && snap.score.patterns[2].lanes[LANE_ROLL].ease == EASE_LINEAR, "lanes");
	CHECK(snap.clock.ready(), "clock estimate");
	CHECK(waitFor([&] { return bridge.linked(); }), "linked");
	CHECK(bridge.status().rfind("Linked", 0) == 0, "status '%s'", bridge.status().c_str());

	// Later polls ask for the cached revision; Studio leaves the score out.
	CHECK(waitFor([&] { return any(studio.take(), "GET /api/bridge?revision=7 HTTP/1.0"); }), "cached poll");
	CHECK(waitFor([&] { return bridge.poll(seen, snap); }), "snapshot after cached poll");
	CHECK(snap.score.bpm == 138. && snap.score.patterns[0].hit[0][4], "score kept across cached polls");
	CHECK(snap.playhead.transportRevision == 23, "transport kept across cached polls");

	// Automatic bindings skip controls on tracker lanes (Dolly Z, Roll).
	CHECK(bridge.target(0) == "Warp" && bridge.target(1) == "Hue" && bridge.target(2).empty(), "auto targets");
	bridge.bind(1, "Roll");
	CHECK(bridge.target(1) == "Roll", "explicit binding");
	bridge.bind(2, "-");
	CHECK(bridge.target(2).empty(), "no binding");

	studio.take();
	bridge.setInput(0, true, 7.5f);    // Warp -2..2 → 1.0
	bridge.setInput(1, true, 0.f);     // Roll → -45
	bridge.setInput(3, false, 10.f);   // unpatched: nothing sent
	std::vector<std::string> seenRequests;
	CHECK(waitFor([&] {
		for (const std::string& r : studio.take())
			seenRequests.push_back(r);
		return any(seenRequests, "POST /api/bridge_params");
	}), "params sent");
	CHECK(any(seenRequests, "\"Warp\":1.0") && any(seenRequests, "\"Roll\":-45"), "param values");
	CHECK(!any(seenRequests, "Hue"), "unpatched input sent");
	CHECK(any(seenRequests, "Content-Type: application/json"), "json content type");
	// Unchanged values aren't resent every poll.
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	int repeats = 0;
	for (const std::string& r : studio.take())
		repeats += r.find("bridge_params") != std::string::npos;
	CHECK(repeats == 0, "%d repeated sends", repeats);

	// Transport: not moving, so PLAY starts; RESET goes to the cycle start.
	bridge.requestPlayToggle();
	bridge.requestRestart();
	seenRequests.clear();
	CHECK(waitFor([&] {
		for (const std::string& r : studio.take())
			seenRequests.push_back(r);
		return any(seenRequests, "\"beat\":0");
	}), "transport sent");
	CHECK(any(seenRequests, "{\"enabled\":true,\"playing\":true}"), "play body");

	// A version-3 score: the score and the playhead's lists arrive by pointer.
	studio.v3 = true;
	studio.revision = 8;
	CHECK(waitFor([&] { return bridge.poll(seen, snap) && snap.v3(); }), "version-3 snapshot");
	CHECK(snap.score3->bpm == 108. && snap.score3->cycle == 4 * 480 && snap.score3->patternAt(0)->lanes[5].cells[0].present,
	      "version-3 score");
	CHECK(snap.score3->patternAt(0)->autos[LANE_Z].keys.size() == 1 && snap.score3->patternAt(0)->autos[LANE_ROLL].ease == EASE_SMOOTH,
	      "version-3 automation");
	CHECK(snap.playhead.startTick == 1440 && snap.playhead.elapsed == 96 && snap.playhead.startEntry == -1, "version-3 playhead");
	CHECK(snap.layer->fill.size() == 1 && snap.layer->fill[0].on && snap.layer->busMutes[2].size() == 2 &&
	      snap.layer->busMutes[2][1].at == 960, "version-3 lists");
	CHECK(waitFor([&] { return any(studio.take(), "GET /api/bridge?revision=8 HTTP/1.0") && bridge.poll(seen, snap); }),
	      "cached version-3 poll");
	CHECK(snap.v3() && snap.score3->bpm == 108., "version-3 score kept across cached polls");
	// Unchanged lists keep their pointer; a new score replaces the old, which the worker frees
	// once the audio thread's copy has let go of it.
	const v3::Layer* layer = snap.layer.get();
	std::weak_ptr<const v3::ScoreV3> old = snap.score3;
	CHECK(waitFor([&] { return bridge.poll(seen, snap); }) && snap.layer.get() == layer, "the same lists, the same layer");
	studio.revision = 9;
	CHECK(waitFor([&] { return bridge.poll(seen, snap) && snap.score3->bpm == 109.; }), "the new version-3 score");
	CHECK(waitFor([&] { return old.expired(); }), "the old score is freed by the worker");
	bridge.requestRestart();
	seenRequests.clear();
	CHECK(waitFor([&] {
		for (const std::string& r : studio.take())
			seenRequests.push_back(r);
		return any(seenRequests, "\"beat\":0");
	}), "version-3 reset goes to the cycle's start");

	// BUS IN: the audio thread's changes reach Studio as windows of the bus layer.
	v3::BusChange segment;
	segment.kind = v3::BusChange::SEGMENT;
	segment.revision = 23;
	segment.tick = 1440;
	bridge.pushBus(segment);
	v3::BusChange mute;
	mute.revision = 23;
	mute.tick = 1440;
	mute.function = v3::BUS_MUTE1 + 2;
	mute.at = 100;
	mute.on = true;
	bridge.pushBus(mute);
	bridge.setBusFrontier(120);
	std::string id = bridge.busSender();
	CHECK(id.size() == 16 && id.find_first_not_of("0123456789abcdef") == std::string::npos, "sender id '%s'", id.c_str());
	seenRequests.clear();
	auto collect = [&] {
		for (const std::string& r : studio.take())
			seenRequests.push_back(r);
	};
	CHECK(waitFor([&] { collect(); return any(seenRequests, "\"sender\":\"" + id + "\""); }), "a bus window is sent");
	CHECK(any(seenRequests, "POST /api/tracker_transport") &&
	      any(seenRequests, "{\"segment\":{\"transport_revision\":23,\"start\":{\"tick\":1440,\"entry\":null}},\"sender\":\"" + id +
	                         "\",\"bus\":{\"from\":0,"),
	      "the window names its segment and sender, and the segment's first starts at 0");
	CHECK(any(seenRequests, "[{\"at\":0,\"on\":false},{\"at\":100,\"on\":true}]"), "lane 3's mute in the window");
	CHECK(waitFor([&] { return bridge.busOutstanding() && !bridge.busRefused(); }), "the layer is this module's");
	// Patched and quiet: a window at least once a second keeps the lease, restating from the frontier.
	seenRequests.clear();
	std::this_thread::sleep_for(std::chrono::milliseconds(1300));
	collect();
	int windows = 0;
	for (const std::string& r : seenRequests)
		windows += r.find("\"bus\":{\"from\":120,") != std::string::npos;
	CHECK(windows >= 1 && windows <= 2, "%d keep-alive windows in 1.3 s", windows);
	// Another module holds the layer: refused, then given back when it lapses.
	studio.busHeld = true;
	bridge.pushBus(mute);
	CHECK(waitFor([&] { return bridge.busRefused(); }), "the refusal reaches the audio thread");
	studio.busHeld = false;
	CHECK(waitFor([&] { return !bridge.busRefused(); }, 3.), "the layer comes back with the next accepted window");
	// And back to version 1: the version-3 score is dropped.
	studio.v3 = false;
	studio.revision = 10;
	CHECK(waitFor([&] { return bridge.poll(seen, snap) && !snap.v3() && snap.playhead.revision == 10; }), "back to version 1");
	CHECK(!snap.score3 && !snap.layer && snap.score.bpm == 138., "version-1 score again");

	// Studio restarts with a new token: refused, then the new advertisement is read.
	studio.setToken("second-token");
	CHECK(waitFor([&] { return bridge.status() == "Studio refused the token"; }), "refusal status '%s'", bridge.status().c_str());
	advertise(ad, studio.port, studio.getToken());
	CHECK(waitFor([&] { return bridge.status().rfind("Linked", 0) == 0; }), "relinked: '%s'", bridge.status().c_str());

	// Removing the module clears every bit it set: an all-clear window as the bridge stops.
	studio.take();
	bridge.stop();
	std::vector<std::string> last = studio.take();
	bool cleared = false;
	for (const std::string& r : last)
		cleared = cleared || (r.find("\"sender\":\"" + id + "\"") != std::string::npos && r.find("\"on\":true") == std::string::npos);
	CHECK(cleared, "an all-clear on stop (%zu requests)", last.size());
	std::remove(ad.c_str());
	::rmdir(dir);
	if (failures) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("bridge_test: all passed\n");
	return 0;
}
