// The link to HydraPy Studio: a worker thread polls GET /api/bridge, keeps the
// clock estimate, and sends CV-driven control values and transport commands.
//
// Threads: the audio thread calls poll(), setInput() and the request*()
// functions, which never block. The UI thread reads status(), controls() and
// the bindings. Everything else happens on the worker.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "BridgeJson.hpp"
#include "LoopbackHttp.hpp"

namespace gorgon {

constexpr int INPUTS = 4;

// What the audio thread follows.
struct Snapshot {
	Playhead playhead;
	Score score;
	BridgeClock clock;
};

class Bridge {
public:
	typedef std::function<double()> Now;

	// `now` is the local monotonic clock in seconds (Rack's system::getTime in
	// the module). `period` is the poll interval.
	explicit Bridge(Now now, std::string advertisement = advertisementPath(), double period = 1. / 30.)
	    : now(std::move(now)), advertisement(std::move(advertisement)), period(period) {
		for (int i = 0; i < INPUTS; i++) {
			volts[i] = 0.f;
			connected[i] = false;
		}
	}
	~Bridge() { stop(); }
	Bridge(const Bridge&) = delete;
	Bridge& operator=(const Bridge&) = delete;

	void start() {
		if (worker.joinable())
			return;
		running = true;
		worker = std::thread([this] { loop(); });
	}
	void stop() {
		{
			std::lock_guard<std::mutex> lock(wakeMutex);
			running = false;
		}
		wake.notify_all();
		if (worker.joinable())
			worker.join();
	}

	// -- audio thread ---------------------------------------------------------
	// Copies the latest snapshot if it changed since `seen`. Never blocks: if
	// the worker is mid-publish, the caller keeps its copy until next time.
	bool poll(uint64_t& seen, Snapshot& into) {
		if (version.load(std::memory_order_acquire) == seen)
			return false;
		std::unique_lock<std::mutex> lock(snapshotMutex, std::try_to_lock);
		if (!lock.owns_lock())
			return false;
		into = latest;
		seen = version.load(std::memory_order_relaxed);
		return true;
	}
	void setInput(int i, bool isConnected, float v) {
		connected[i].store(isConnected, std::memory_order_relaxed);
		volts[i].store(v, std::memory_order_relaxed);
	}
	void requestPlayToggle() { playToggle = true; wake.notify_all(); }
	void requestRestart() { restart = true; wake.notify_all(); }

	// -- UI thread ------------------------------------------------------------
	std::string status() {
		std::lock_guard<std::mutex> lock(uiMutex);
		return statusText;
	}
	bool linked() { return linkedFlag.load(); }
	std::vector<Control> controls() {
		std::lock_guard<std::mutex> lock(uiMutex);
		return controlList;
	}
	// "" = automatic (the Nth control not driven by a tracker lane), "-" = none.
	std::string binding(int i) {
		std::lock_guard<std::mutex> lock(uiMutex);
		return bindings[i];
	}
	void bind(int i, const std::string& name) {
		std::lock_guard<std::mutex> lock(uiMutex);
		bindings[i] = name;
		lastSent[i] = NAN;
	}
	// The control input `i` drives right now ("" if none).
	std::string target(int i) {
		std::lock_guard<std::mutex> lock(uiMutex);
		return resolve(i);
	}
	const std::string& advertisementFile() const { return advertisement; }

private:
	Now now;
	std::string advertisement;
	double period;
	LoopbackHttp http;
	std::thread worker;
	std::atomic<bool> running{false}, playToggle{false}, restart{false}, linkedFlag{false};
	std::mutex wakeMutex;
	std::condition_variable wake;

	std::mutex snapshotMutex;
	Snapshot latest;
	std::atomic<uint64_t> version{0};

	std::atomic<float> volts[INPUTS];
	std::atomic<bool> connected[INPUTS];

	// UI-visible, guarded by uiMutex.
	std::mutex uiMutex;
	std::string statusText = "Looking for Studio";
	std::vector<Control> controlList;
	std::vector<std::string> laneBound;   // controls the tracker's lanes are bound to
	std::string bindings[INPUTS];
	double lastSent[INPUTS] = {NAN, NAN, NAN, NAN};
	double lastSentAt[INPUTS] = {};

	// Worker-only.
	int port = 0;
	std::string token;
	int64_t scoreRevision = -1;
	double lastOk = -1e9;

	std::string resolve(int i) const {
		if (bindings[i] == "-")
			return "";
		if (!bindings[i].empty())
			return bindings[i];
		int n = 0;
		for (const Control& c : controlList) {
			bool lane = false;
			for (const std::string& b : laneBound)
				lane = lane || b == c.name;
			if (lane)
				continue;
			if (n++ == i)
				return c.name;
		}
		return "";
	}

	void setStatus(const std::string& s) {
		std::lock_guard<std::mutex> lock(uiMutex);
		statusText = s;
	}

	// Encodes and releases `j`. jansson writes numbers with '.', whatever the locale.
	static std::string dump(json_t* j) {
		char* d = json_dumps(j, JSON_COMPACT);
		std::string out = d ? d : "{}";
		std::free(d);
		json_decref(j);
		return out;
	}

	void loop() {
		Snapshot working;
		while (running) {
			double started = now();
			pollStudio(working);
			sendTransport(working);
			sendInputs();
			std::unique_lock<std::mutex> lock(wakeMutex);
			double wait = period - (now() - started);
			if (wait > 0.)
				wake.wait_for(lock, std::chrono::duration<double>(wait),
				              [this] { return !running || playToggle || restart; });
		}
	}

	void pollStudio(Snapshot& working) {
		if (port == 0 && !readAdvertisement(advertisement, port, token)) {
			linkedFlag = false;
			setStatus("No Studio running");
			return;
		}
		std::string path = "/api/bridge" + (scoreRevision >= 0 ? "?revision=" + std::to_string(scoreRevision) : std::string());
		double sent = now();
		HttpResponse r = http.request(port, path, token);
		double received = now();
		BridgeState state;
		std::string error;
		if (r.status == 200 && parseBridge(r.body, state, &error)) {
			if (state.hasScore) {
				working.score = state.score;
				scoreRevision = state.playhead.revision;
				std::vector<std::string> lanes = laneBindings(r.body);
				std::lock_guard<std::mutex> lock(uiMutex);
				laneBound = lanes;
			}
			else if (scoreRevision != state.playhead.revision) {
				scoreRevision = -1;   // ask for the score again
				return;
			}
			working.playhead = state.playhead;
			working.clock.observe(sent, received, state.serverNow);
			{
				std::lock_guard<std::mutex> lock(snapshotMutex);
				latest = working;
				version.fetch_add(1, std::memory_order_release);
			}
			{
				std::lock_guard<std::mutex> lock(uiMutex);
				controlList = state.controls;
				char text[96];
				std::snprintf(text, sizeof text, "Linked  %.0f BPM%s", working.score.bpm,
				              state.playhead.moving() ? "" : state.playhead.enabled ? "  paused" : "  auto off");
				statusText = text;
			}
			lastOk = received;
			linkedFlag = true;
			return;
		}
		// Studio restarted (new port or token) or went away: look again.
		if (r.status == 403 || !r.error.empty()) {
			port = 0;
			scoreRevision = -1;
		}
		if (received - lastOk > .5)
			linkedFlag = false;
		setStatus(r.status == 403 ? "Studio refused the token" : !error.empty() ? error : !r.error.empty() ? r.error : "Studio error");
	}

	static std::vector<std::string> laneBindings(const std::string& body) {
		std::vector<std::string> out;
		json_error_t e;
		json_t* root = json_loads(body.c_str(), 0, &e);
		json_t* b = json_object_get(json_object_get(root, "document"), "bindings");
		const char* key;
		json_t* value;
		json_object_foreach(b, key, value) {
			if (json_is_string(value) && *json_string_value(value))
				out.push_back(json_string_value(value));
		}
		json_decref(root);
		return out;
	}

	void sendTransport(const Snapshot& s) {
		if (port == 0)
			return;
		if (playToggle.exchange(false)) {
			json_t* body = json_object();
			if (s.playhead.moving())
				json_object_set_new(body, "playing", json_false());
			else {
				json_object_set_new(body, "enabled", json_true());
				json_object_set_new(body, "playing", json_true());
			}
			http.request(port, "/api/tracker_transport", token, dump(body));
		}
		if (restart.exchange(false)) {
			json_t* body = json_object();
			json_object_set_new(body, "beat", json_real(s.score.cycleStart()));
			http.request(port, "/api/tracker_transport", token, dump(body));
		}
	}

	void sendInputs() {
		if (port == 0 || !linkedFlag)
			return;
		json_t* values = json_object();
		double t = now();
		{
			std::lock_guard<std::mutex> lock(uiMutex);
			for (int i = 0; i < INPUTS; i++) {
				if (!connected[i].load(std::memory_order_relaxed))
					continue;
				std::string name = resolve(i);
				const Control* c = nullptr;
				for (const Control& candidate : controlList)
					if (candidate.name == name)
						c = &candidate;
				if (!c)
					continue;
				double value = voltsToControl(volts[i].load(std::memory_order_relaxed), c->min, c->max);
				// Send changes beyond 1/2000 of the range, and repeat once a second.
				bool changed = !(std::fabs(value - lastSent[i]) <= (c->max - c->min) / 2000.);
				if (!changed && t - lastSentAt[i] < 1.)
					continue;
				lastSent[i] = value;
				lastSentAt[i] = t;
				json_object_set_new(values, name.c_str(), json_real(value));
			}
		}
		if (json_object_size(values) == 0) {
			json_decref(values);
			return;
		}
		json_t* body = json_object();
		json_object_set_new(body, "values", values);
		http.request(port, "/api/bridge_params", token, dump(body));
	}
};

} // namespace gorgon
