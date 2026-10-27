// Ping in Rack's own engine: real cables, stepped by Engine::stepBlock, into destinations whose latency is known.
// Ping must report exactly that latency, whether it sends the pulse or receives the trigger that plays them.
#include <engine/Engine.hpp>
#include <context.hpp>
#include "../src/Ping.cpp"
#include <cassert>
#include <iostream>
Plugin* pluginInstance = nullptr;

// A destination: its output (on `channels` channels, the signal on channel `on`) follows its input `delay`
// samples later. A delay of 0 answers on the sample its input arrives.
struct Late : Module {
	std::vector<float> line;
	size_t head = 0;
	int channels, on;
	Late(int delay, int channels = 1, int on = 0) : line(size_t(delay) + 1), channels(channels), on(on) { config(0, 1, 1, 0); }
	void process(const ProcessArgs&) override {
		line[head] = inputs[0].getVoltage();
		head = (head + 1) % line.size();
		outputs[0].setChannels(channels);
		for (int c = 0; c < channels; c++)
			outputs[0].setVoltage(c == on ? line[head] : 0.f, c);
	}
};
// A gate source: high for 10 ms from frame `at`, or a constant level.
struct Source : Module {
	int64_t at, frame = 0;
	float constant;
	Source(int64_t at, float constant = 0.f) : at(at), constant(constant) { config(0, 0, 1, 0); }
	void process(const ProcessArgs& args) override {
		bool high = frame >= at && frame < at + int64_t(.01f * args.sampleRate);
		outputs[0].setVoltage(constant != 0.f ? constant : high ? 10.f : 0.f);
		frame++;
	}
};

static engine::Engine* E;
// Ids are given: the engine otherwise draws random ones, and nothing here seeds its generator.
static int64_t ids = 1;
static void cable(Module* from, int output, Module* to, int input) {
	auto* c = new engine::Cable;
	c->id = ids++;
	c->outputModule = from; c->outputId = output; c->inputModule = to; c->inputId = input;
	E->addCable(c);
}
template <class M> static M* add(M* m) { m->id = ids++; E->addModule(m); return m; }
static Ping* ping() {
	auto* p = new Ping;
	p->model = modelPing;
	return add(p);
}

int main() {
	Context context;
	context.engine = E = new engine::Engine;
	contextSet(&context);
	E->setSuggestedSampleRate(48000.f);
	E->stepBlock(16);
	const float sr = E->getSampleRate();

	// PULSE -> destination -> RETURN, pressed by hand.
	const int delays[] = {0, 1, 37, 1103, 4799};
	std::vector<Ping*> pulsed;
	for (int d : delays) {
		Ping* p = ping();
		Late* late = add(new Late(d));
		cable(p, Ping::PULSE_OUTPUT, late, 0);
		cable(late, 0, p, Ping::RETURN_INPUT);
		pulsed.push_back(p);
	}
	// A gate plays the destination and Ping's TRIG on the same sample.
	std::vector<Ping*> triggered;
	for (int d : delays) {
		Ping* p = ping();
		Source* gate = add(new Source(2000));
		Late* late = add(new Late(d));
		cable(gate, 0, p, Ping::TRIG_INPUT);
		cable(gate, 0, late, 0);
		cable(late, 0, p, Ping::RETURN_INPUT);
		triggered.push_back(p);
	}
	// A polyphonic return: the destination answers on its fourth channel of eight.
	Ping* poly = ping();
	Late* wide = add(new Late(250, 8, 3));
	cable(poly, Ping::PULSE_OUTPUT, wide, 0);
	cable(wide, 0, poly, Ping::RETURN_INPUT);
	// A return that is never quiet.
	Ping* noisy = ping();
	cable(add(new Source(0, .5f)), 0, noisy, Ping::RETURN_INPUT);
	// No return at all.
	Ping* deaf = ping();

	E->stepBlock(1000);
	for (Ping* p : pulsed) p->params[Ping::PING_PARAM].setValue(1.f);
	for (Ping* p : {poly, noisy, deaf}) p->params[Ping::PING_PARAM].setValue(1.f);
	E->stepBlock(10);
	for (Ping* p : pulsed) p->params[Ping::PING_PARAM].setValue(0.f);
	for (Ping* p : {poly, noisy, deaf}) p->params[Ping::PING_PARAM].setValue(0.f);
	E->stepBlock(6000);
	for (size_t i = 0; i < pulsed.size(); i++) {
		gorgon::PingStats s = pulsed[i]->stats();
		if (s.n != 1 || s.last != delays[i])
			std::cerr << "pulse, delay " << delays[i] << ": n " << s.n << " last " << s.last << "\n";
		assert(s.n == 1 && s.last == delays[i]);
		s = triggered[i]->stats();
		if (s.n != 1 || s.last != delays[i])
			std::cerr << "trigger, delay " << delays[i] << ": n " << s.n << " last " << s.last << "\n";
		assert(s.n == 1 && s.last == delays[i]);
	}
	assert(poly->stats().n == 1 && poly->stats().last == 250);
	assert(noisy->stats().n == 0 && noisy->meter.state() == gorgon::PingMeter::Status::NotQuiet);
	assert(deaf->stats().n == 0 && deaf->meter.state() == gorgon::PingMeter::Status::Waiting);
	E->stepBlock(int(2 * sr));
	assert(deaf->meter.state() == gorgon::PingMeter::Status::NoReturn);

	// AUTO every 50 ms for a second: about 20 identical results.
	Ping* repeat = pulsed[3];
	repeat->interval = .05f;
	repeat->params[Ping::AUTO_PARAM].setValue(1.f);
	E->stepBlock(int(sr));
	repeat->params[Ping::AUTO_PARAM].setValue(0.f);
	E->stepBlock(int(.2f * sr));
	gorgon::PingStats s = repeat->stats();
	assert(s.n >= 19 && s.n <= 21 && s.min == 1103 && s.max == 1103 && s.median == 1103);

	// Saved with the patch, with the rate they were measured at; restored as they were.
	json_t* saved = repeat->dataToJson();
	assert(json_array_size(json_object_get(saved, "results")) == size_t(s.n));
	assert(json_number_value(json_object_get(saved, "rate")) == sr);
	Ping restored;
	restored.dataFromJson(saved);
	json_decref(saved);
	gorgon::PingStats r = restored.stats();
	assert(r.n == s.n && r.last == 1103 && restored.resultRate.load() == sr && restored.interval.load() == .05f);
	assert(restored.summary().find("1103 samples") != std::string::npos);
	// Bad data restores nothing.
	json_t* bad = json_loads(R"({"results": [1, "x", 3], "rate": -5, "interval": 1e9})", 0, nullptr);
	restored.dataFromJson(bad);
	json_decref(bad);
	assert(restored.stats().n == 0 && restored.interval.load() == 10.f);
	// Clear from the menu takes effect on the next sample.
	repeat->clearRequested = true;
	E->stepBlock(1);
	assert(repeat->stats().n == 0);

	E->clear();
	std::cout << "ping_module_test: exact latency through Rack's engine and cables (pulse and trigger, 0-4799 samples), "
	             "polyphonic return, not quiet, no return, AUTO, saved results\n";
}
