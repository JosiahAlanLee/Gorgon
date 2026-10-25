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

struct FakeStudio {
	int listener = -1, port = 0;
	std::string token = "first-token";
	std::atomic<bool> running{true};
	std::thread thread;
	std::mutex mutex;
	std::vector<std::string> requests;   // request line + headers + body
	int revision = 7;

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
				bool cached = req.find("?revision=" + std::to_string(revision) + " ") != std::string::npos;
				body = "{\"revision\":" + std::to_string(revision) + ",\"enabled\":true,\"playing\":false,"
				       "\"transport_revision\":23,\"anchor_beat\":12.5,\"anchor_time\":3.0,\"clock_time\":3.0,\"wall\":100.0,\"server_now\":100.01,"
				       "\"controls\":[{\"name\":\"Dolly Z\",\"min\":0,\"max\":1,\"value\":0.35},"
				       "{\"name\":\"Roll\",\"min\":-45,\"max\":45,\"value\":0},"
				       "{\"name\":\"Warp\",\"min\":-2,\"max\":2,\"value\":0},"
				       "{\"name\":\"Hue\",\"min\":0,\"max\":1,\"value\":0.5}]" +
				       (cached ? std::string() : ",\"document\":" + scoreJson()) + "}";
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

int main() {
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

	// Studio restarts with a new token: refused, then the new advertisement is read.
	studio.setToken("second-token");
	CHECK(waitFor([&] { return bridge.status() == "Studio refused the token"; }), "refusal status '%s'", bridge.status().c_str());
	advertise(ad, studio.port, studio.getToken());
	CHECK(waitFor([&] { return bridge.status().rfind("Linked", 0) == 0; }), "relinked: '%s'", bridge.status().c_str());

	bridge.stop();
	std::remove(ad.c_str());
	::rmdir(dir);
	if (failures) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("bridge_test: all passed\n");
	return 0;
}
