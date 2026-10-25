// Director: a tutorial-only Rack module that lets an outside program film Rack.
//
// It never touches audio. Once any Director module is in a patch, it installs one
// DirectorAgent widget in Rack's scene. The agent outlives patch loads, so a single
// Rack session can be driven through many patches.
//
// The agent, on Rack's UI thread:
//   * writes <dir>/layout.json a few times a second: window geometry, view zoom and
//     offset, every module's screen box, every port's and param's screen centre, and
//     the items of any open context menu, all in X screen pixels;
//   * reads new lines appended to <dir>/commands.jsonl and acts on them (see README);
//   * draws highlight rings, captions and the sync flash above the rack.
//
// <dir> is $GTUT_PROBE_DIR, or <Rack user folder>/gtut-probe.
#include "plugin.hpp"
#include <patch.hpp>
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::string probeDir() {
	const char* env = std::getenv("GTUT_PROBE_DIR");
	std::string dir = env && *env ? std::string(env) : asset::user("gtut-probe");
	system::createDirectories(dir);
	return dir;
}

// Rack widget coordinates to X screen pixels.
struct ScreenMap {
	math::Vec windowPos;
	float scale = 1.f;  // pixelRatio / windowRatio
	math::Vec toScreen(math::Vec v) const { return windowPos.plus(v.mult(scale)); }
	static ScreenMap current() {
		ScreenMap m;
		int x = 0, y = 0;
		if (APP->window && APP->window->win)
			glfwGetWindowPos(APP->window->win, &x, &y);
		m.windowPos = math::Vec(x, y);
		if (APP->window && APP->window->windowRatio > 0.f)
			m.scale = APP->window->pixelRatio / APP->window->windowRatio;
		return m;
	}
};

json_t* vecJ(math::Vec v) {
	return json_pack("[ff]", (double) v.x, (double) v.y);
}

json_t* rectJ(const ScreenMap& m, widget::Widget* w) {
	math::Vec a = m.toScreen(w->getAbsoluteOffset(math::Vec()));
	math::Vec b = m.toScreen(w->getAbsoluteOffset(w->box.size));
	return json_pack("[ffff]", (double) a.x, (double) a.y, (double) (b.x - a.x), (double) (b.y - a.y));
}

math::Vec centreOf(const ScreenMap& m, widget::Widget* w) {
	return m.toScreen(w->getAbsoluteOffset(w->box.size.div(2.f)));
}

double smoothstep(double t) {
	t = std::min(1.0, std::max(0.0, t));
	return t * t * (3.0 - 2.0 * t);
}

void collectMenuItems(widget::Widget* w, const ScreenMap& m, json_t* out) {
	for (widget::Widget* child : w->children) {
		if (!child->isVisible())
			continue;
		if (auto* item = dynamic_cast<ui::MenuItem*>(child)) {
			json_t* itemJ = json_object();
			json_object_set_new(itemJ, "text", json_string(item->text.c_str()));
			json_object_set_new(itemJ, "rightText", json_string(item->rightText.c_str()));
			json_object_set_new(itemJ, "disabled", json_boolean(item->disabled));
			json_object_set_new(itemJ, "box", rectJ(m, item));
			json_object_set_new(itemJ, "centre", vecJ(centreOf(m, item)));
			json_array_append_new(out, itemJ);
		}
		else if (auto* label = dynamic_cast<ui::MenuLabel*>(child)) {
			json_t* itemJ = json_object();
			json_object_set_new(itemJ, "label", json_string(label->text.c_str()));
			json_object_set_new(itemJ, "box", rectJ(m, label));
			json_array_append_new(out, itemJ);
		}
		collectMenuItems(child, m, out);
	}
}

app::ModuleWidget* moduleById(int64_t id) {
	return APP->scene->rack->getModule(id);
}

// A module may be named by numeric id or by model slug ("Gorgon", "Vastish").
app::ModuleWidget* findModule(json_t* moduleJ) {
	if (json_is_integer(moduleJ))
		return moduleById(json_integer_value(moduleJ));
	if (json_is_string(moduleJ)) {
		std::string want = json_string_value(moduleJ);
		for (app::ModuleWidget* mw : APP->scene->rack->getModules())
			if (mw->model && (mw->model->slug == want || mw->model->name == want))
				return mw;
	}
	return NULL;
}

struct Highlight {
	int64_t moduleId = -1;
	std::string portType;  // "input", "output", "param" or "" for the whole module
	int index = -1;
	std::string label;
	double until = 0.0;
};

struct ViewAnim {
	bool active = false;
	double start = 0.0, seconds = 0.0;
	math::Vec fromOffset, toOffset;
	float fromZoom = 1.f, toZoom = 1.f;
};

struct ParamAnim {
	int64_t moduleId = -1;
	int paramId = -1;
	float from = 0.f, to = 0.f;
	double start = 0.0, seconds = 0.0;
};

struct DirectorAgent : widget::Widget {
	std::string dir = probeDir();
	std::string commandsPath = dir + "/commands.jsonl";
	std::string layoutPath = dir + "/layout.json";
	std::streamoff commandOffset = 0;
	double lastWrite = 0.0;
	double interval = 0.2;
	int64_t frame = 0;
	int64_t lastCommand = 0;
	std::vector<Highlight> highlights;
	std::vector<ParamAnim> params;
	ViewAnim view;
	double flashUntil = 0.0;
	std::string pendingLoad;
	std::string pendingSave;
	std::string pendingCatalog;
	std::vector<std::string> log;

	DirectorAgent() {
		// Skip commands written before this Rack started; the harness truncates
		// the file at stage start, so this normally starts at zero.
		std::ifstream f(commandsPath, std::ios::binary | std::ios::ate);
		if (f)
			commandOffset = f.tellg();
	}

	double now() { return system::getTime(); }

	void note(const std::string& s) {
		log.push_back(s);
		if (log.size() > 20)
			log.erase(log.begin());
	}

	void step() override {
		Widget::step();
		box = APP->scene->box.zeroPos();
		frame++;
		runDeferred();
		readCommands();
		animate();
		double t = now();
		if (t - lastWrite >= interval) {
			lastWrite = t;
			writeLayout();
		}
		// Keep the agent drawn above the rack after patch loads add widgets.
		// Menus and the module browser are added later and stay above it.
	}

	void runDeferred() {
		if (!pendingLoad.empty()) {
			std::string path = pendingLoad;
			pendingLoad.clear();
			highlights.clear();
			params.clear();
			view.active = false;
			APP->patch->load(path);
			APP->patch->path = path;
			note("loaded " + path);
		}
		if (!pendingSave.empty()) {
			std::string path = pendingSave;
			pendingSave.clear();
			APP->patch->save(path);
			note("saved " + path);
		}
		if (!pendingCatalog.empty()) {
			std::string path = pendingCatalog;
			pendingCatalog.clear();
			writeCatalog(path);
		}
	}

	void readCommands() {
		std::ifstream f(commandsPath, std::ios::binary);
		if (!f)
			return;
		f.seekg(0, std::ios::end);
		std::streamoff size = f.tellg();
		if (size < commandOffset)
			commandOffset = 0;  // truncated by the harness
		if (size == commandOffset)
			return;
		f.seekg(commandOffset);
		std::string line;
		while (std::getline(f, line)) {
			if (f.eof())
				break;  // incomplete last line; read it next frame
			commandOffset = f.tellg();
			if (!line.empty())
				runCommand(line);
		}
	}

	void runCommand(const std::string& line) {
		json_error_t err;
		json_t* cmdJ = json_loads(line.c_str(), 0, &err);
		if (!cmdJ) {
			note(std::string("bad json: ") + err.text);
			return;
		}
		json_t* seqJ = json_object_get(cmdJ, "seq");
		if (json_is_integer(seqJ))
			lastCommand = json_integer_value(seqJ);
		std::string op = json_string_value(json_object_get(cmdJ, "op")) ?: "";
		double seconds = json_number_value(json_object_get(cmdJ, "seconds"));
		if (op == "view") {
			startView(cmdJ, seconds);
		}
		else if (op == "highlight") {
			addHighlight(cmdJ, seconds);
		}
		else if (op == "clear") {
			highlights.clear();
		}
		else if (op == "param") {
			startParam(cmdJ, seconds);
		}
		else if (op == "flash") {
			flashUntil = now() + (seconds > 0 ? seconds : 0.1);
		}
		else if (op == "load") {
			const char* p = json_string_value(json_object_get(cmdJ, "path"));
			if (p) pendingLoad = p;
		}
		else if (op == "save") {
			const char* p = json_string_value(json_object_get(cmdJ, "path"));
			if (p) pendingSave = p;
		}
		else if (op == "catalog") {
			const char* p = json_string_value(json_object_get(cmdJ, "path"));
			pendingCatalog = p ? p : dir + "/catalog.json";
		}
		else if (op == "interval") {
			interval = std::min(5.0, std::max(0.02, seconds));
		}
		else if (op == "dump") {
			lastWrite = 0.0;
		}
		else {
			note("unknown op " + op);
		}
		json_decref(cmdJ);
	}

	// View: fit modules (all by default) with a margin, optionally at a fixed zoom.
	void startView(json_t* cmdJ, double seconds) {
		app::RackScrollWidget* rs = APP->scene->rackScroll;
		math::Rect bound;
		bool any = false;
		auto include = [&](app::ModuleWidget* mw) {
			if (!mw) return;
			bound = any ? bound.expand(mw->box) : mw->box;
			any = true;
		};
		json_t* modulesJ = json_object_get(cmdJ, "modules");
		json_t* moduleJ = json_object_get(cmdJ, "module");
		if (json_is_array(modulesJ)) {
			size_t i; json_t* m;
			json_array_foreach(modulesJ, i, m) include(findModule(m));
		}
		else if (moduleJ) {
			include(findModule(moduleJ));
		}
		else {
			// Everything but the Director itself, which sits out at the right edge.
			for (app::ModuleWidget* mw : APP->scene->rack->getModules())
				if (!(mw->model && mw->model->slug == "Director"))
					include(mw);
		}
		if (!any) {
			note("view: no module");
			return;
		}
		double margin = json_number_value(json_object_get(cmdJ, "margin"));
		bound = bound.grow(math::Vec(margin > 0 ? margin : 24, margin > 0 ? margin : 24));
		math::Vec size = rs->getSize();
		float zoom = std::min(size.x / bound.size.x, size.y / bound.size.y);
		json_t* zoomJ = json_object_get(cmdJ, "zoom");
		if (json_is_number(zoomJ))
			zoom = json_number_value(zoomJ);
		view.fromOffset = rs->offset;
		view.fromZoom = rs->zoomWidget->zoom;
		view.toZoom = zoom;
		view.toOffset = bound.getCenter().mult(zoom).minus(size.div(2.f));
		view.start = now();
		view.seconds = seconds;
		view.active = true;
	}

	void addHighlight(json_t* cmdJ, double seconds) {
		app::ModuleWidget* mw = findModule(json_object_get(cmdJ, "module"));
		if (!mw) {
			note("highlight: no module");
			return;
		}
		Highlight h;
		h.moduleId = mw->module ? mw->module->id : -1;
		const char* type = json_string_value(json_object_get(cmdJ, "type"));
		h.portType = type ? type : "";
		json_t* indexJ = json_object_get(cmdJ, "index");
		h.index = json_is_integer(indexJ) ? (int) json_integer_value(indexJ) : -1;
		const char* label = json_string_value(json_object_get(cmdJ, "label"));
		h.label = label ? label : "";
		h.until = now() + (seconds > 0 ? seconds : 3.0);
		highlights.push_back(h);
	}

	void startParam(json_t* cmdJ, double seconds) {
		app::ModuleWidget* mw = findModule(json_object_get(cmdJ, "module"));
		if (!mw || !mw->module) {
			note("param: no module");
			return;
		}
		ParamAnim a;
		a.moduleId = mw->module->id;
		a.paramId = (int) json_integer_value(json_object_get(cmdJ, "index"));
		if (a.paramId < 0 || a.paramId >= (int) mw->module->paramQuantities.size()) {
			note("param: bad index");
			return;
		}
		engine::ParamQuantity* pq = mw->module->paramQuantities[a.paramId];
		a.from = pq->getValue();
		json_t* valueJ = json_object_get(cmdJ, "value");      // native units
		json_t* scaledJ = json_object_get(cmdJ, "scaled");    // 0..1 of the range
		if (json_is_number(valueJ))
			a.to = json_number_value(valueJ);
		else if (json_is_number(scaledJ))
			a.to = pq->getMinValue() + json_number_value(scaledJ) * (pq->getMaxValue() - pq->getMinValue());
		else {
			note("param: no value");
			return;
		}
		a.start = now();
		a.seconds = seconds;
		params.push_back(a);
	}

	void animate() {
		double t = now();
		app::RackScrollWidget* rs = APP->scene->rackScroll;
		if (view.active) {
			double k = view.seconds > 0 ? smoothstep((t - view.start) / view.seconds) : 1.0;
			// Interpolate zoom geometrically so the move feels even.
			float zoom = view.fromZoom * std::pow(view.toZoom / view.fromZoom, (float) k);
			rs->zoomWidget->setZoom(zoom);
			rs->offset = view.fromOffset.plus(view.toOffset.minus(view.fromOffset).mult((float) k));
			if (k >= 1.0)
				view.active = false;
		}
		for (auto it = params.begin(); it != params.end();) {
			app::ModuleWidget* mw = moduleById(it->moduleId);
			if (!mw || !mw->module) {
				it = params.erase(it);
				continue;
			}
			double k = it->seconds > 0 ? smoothstep((t - it->start) / it->seconds) : 1.0;
			mw->module->paramQuantities[it->paramId]->setValue(it->from + (it->to - it->from) * (float) k);
			if (k >= 1.0) it = params.erase(it);
			else ++it;
		}
		highlights.erase(std::remove_if(highlights.begin(), highlights.end(),
			[&](const Highlight& h) { return h.until < t; }), highlights.end());
	}

	widget::Widget* highlightTarget(const Highlight& h) {
		app::ModuleWidget* mw = moduleById(h.moduleId);
		if (!mw) return NULL;
		if (h.portType == "input") return mw->getInput(h.index);
		if (h.portType == "output") return mw->getOutput(h.index);
		if (h.portType == "param") return mw->getParam(h.index);
		return mw;
	}

	void draw(const DrawArgs& args) override {
		double t = now();
		for (const Highlight& h : highlights) {
			widget::Widget* w = highlightTarget(h);
			if (!w) continue;
			math::Vec a = w->getAbsoluteOffset(math::Vec());
			math::Vec b = w->getAbsoluteOffset(w->box.size);
			math::Vec c = a.plus(b).div(2.f);
			float pulse = 0.5f + 0.5f * std::sin((float) t * 6.f);
			nvgBeginPath(args.vg);
			if (h.portType.empty()) {
				nvgRoundedRect(args.vg, a.x - 4, a.y - 4, b.x - a.x + 8, b.y - a.y + 8, 6);
			}
			else {
				float r = std::max(b.x - a.x, b.y - a.y) * 0.5f + 7.f;
				nvgCircle(args.vg, c.x, c.y, r);
			}
			nvgStrokeWidth(args.vg, 3.f + 2.f * pulse);
			nvgStrokeColor(args.vg, nvgRGBAf(1.f, 0.78f, 0.2f, 0.55f + 0.45f * pulse));
			nvgStroke(args.vg);
			if (!h.label.empty()) {
				std::shared_ptr<window::Font> font = APP->window->uiFont;
				if (font && font->handle >= 0) {
					nvgFontFaceId(args.vg, font->handle);
					nvgFontSize(args.vg, 18.f);
					nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
					float tb[4];
					float tx = b.x + 14.f, ty = c.y;
					nvgTextBounds(args.vg, tx, ty, h.label.c_str(), NULL, tb);
					nvgBeginPath(args.vg);
					nvgRoundedRect(args.vg, tb[0] - 6, tb[1] - 4, tb[2] - tb[0] + 12, tb[3] - tb[1] + 8, 4);
					nvgFillColor(args.vg, nvgRGBAf(0.f, 0.f, 0.f, 0.8f));
					nvgFill(args.vg);
					nvgFillColor(args.vg, nvgRGBf(1.f, 0.85f, 0.4f));
					nvgText(args.vg, tx, ty, h.label.c_str(), NULL);
				}
			}
		}
		if (t < flashUntil) {
			nvgBeginPath(args.vg);
			nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
			nvgFillColor(args.vg, nvgRGBf(1.f, 1.f, 1.f));
			nvgFill(args.vg);
		}
	}

	json_t* moduleJson(app::ModuleWidget* mw, const ScreenMap& m) {
		json_t* modJ = json_object();
		json_object_set_new(modJ, "id", json_integer(mw->module ? mw->module->id : -1));
		if (mw->model) {
			json_object_set_new(modJ, "plugin", json_string(mw->model->plugin->slug.c_str()));
			json_object_set_new(modJ, "model", json_string(mw->model->slug.c_str()));
			json_object_set_new(modJ, "name", json_string(mw->model->name.c_str()));
		}
		json_object_set_new(modJ, "box", rectJ(m, mw));
		json_object_set_new(modJ, "grid", vecJ(mw->box.pos));
		json_t* portsJ = json_array();
		for (app::PortWidget* pw : mw->getPorts()) {
			json_t* pJ = json_object();
			json_object_set_new(pJ, "type", json_string(pw->type == engine::Port::INPUT ? "input" : "output"));
			json_object_set_new(pJ, "index", json_integer(pw->portId));
			engine::PortInfo* info = pw->getPortInfo();
			json_object_set_new(pJ, "name", json_string(info ? info->getName().c_str() : ""));
			json_object_set_new(pJ, "centre", vecJ(centreOf(m, pw)));
			json_object_set_new(pJ, "cables", json_integer((json_int_t) APP->scene->rack->getCablesOnPort(pw).size()));
			json_array_append_new(portsJ, pJ);
		}
		json_object_set_new(modJ, "ports", portsJ);
		json_t* paramsJ = json_array();
		for (app::ParamWidget* pw : mw->getParams()) {
			json_t* pJ = json_object();
			json_object_set_new(pJ, "index", json_integer(pw->paramId));
			engine::ParamQuantity* pq = pw->getParamQuantity();
			if (pq) {
				json_object_set_new(pJ, "name", json_string(pq->getLabel().c_str()));
				json_object_set_new(pJ, "value", json_real(pq->getValue()));
				json_object_set_new(pJ, "min", json_real(pq->getMinValue()));
				json_object_set_new(pJ, "max", json_real(pq->getMaxValue()));
				json_object_set_new(pJ, "display", json_string(pq->getDisplayValueString().c_str()));
			}
			json_object_set_new(pJ, "box", rectJ(m, pw));
			json_object_set_new(pJ, "centre", vecJ(centreOf(m, pw)));
			json_array_append_new(paramsJ, pJ);
		}
		json_object_set_new(modJ, "params", paramsJ);
		return modJ;
	}

	void writeLayout() {
		ScreenMap m = ScreenMap::current();
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "time", json_real(now()));
		json_object_set_new(rootJ, "frame", json_integer(frame));
		json_object_set_new(rootJ, "seq", json_integer(lastCommand));
		double fd = APP->window->getLastFrameDuration();
		json_object_set_new(rootJ, "fps", json_real(fd > 0 ? 1.0 / fd : 0.0));
		json_object_set_new(rootJ, "patch", json_string(APP->patch->path.c_str()));
		json_t* winJ = json_object();
		json_object_set_new(winJ, "pos", vecJ(m.windowPos));
		json_object_set_new(winJ, "size", vecJ(APP->scene->box.size.mult(m.scale)));
		json_object_set_new(winJ, "pixelRatio", json_real(APP->window->pixelRatio));
		json_object_set_new(winJ, "windowRatio", json_real(APP->window->windowRatio));
		json_object_set_new(rootJ, "window", winJ);
		app::RackScrollWidget* rs = APP->scene->rackScroll;
		json_object_set_new(rootJ, "zoom", json_real(rs->zoomWidget->zoom));
		json_object_set_new(rootJ, "viewAnimating", json_boolean(view.active || !params.empty()));
		json_t* modulesJ = json_array();
		for (app::ModuleWidget* mw : APP->scene->rack->getModules())
			json_array_append_new(modulesJ, moduleJson(mw, m));
		json_object_set_new(rootJ, "modules", modulesJ);
		json_t* menuJ = json_array();
		for (widget::Widget* child : APP->scene->children)
			if (dynamic_cast<ui::MenuOverlay*>(child))
				collectMenuItems(child, m, menuJ);
		json_object_set_new(rootJ, "menu", menuJ);
		json_t* logJ = json_array();
		for (const std::string& s : log)
			json_array_append_new(logJ, json_string(s.c_str()));
		json_object_set_new(rootJ, "log", logJ);
		std::string tmp = layoutPath + ".tmp";
		if (json_dump_file(rootJ, tmp.c_str(), JSON_INDENT(1) | JSON_REAL_PRECISION(7)) == 0)
			std::rename(tmp.c_str(), layoutPath.c_str());
		json_decref(rootJ);
	}

	// Port and param names and ranges for every installed model, so patches can be
	// written by name. Instantiates each module once, off the engine.
	void writeCatalog(const std::string& path) {
		json_t* rootJ = json_object();
		for (plugin::Plugin* p : rack::plugin::plugins) {
			json_t* pluginJ = json_object();
			json_object_set_new(pluginJ, "version", json_string(p->version.c_str()));
			json_t* modelsJ = json_object();
			for (plugin::Model* model : p->models) {
				engine::Module* module = NULL;
				try {
					module = model->createModule();
				}
				catch (...) {
					module = NULL;
				}
				if (!module) continue;
				json_t* modelJ = json_object();
				json_object_set_new(modelJ, "name", json_string(model->name.c_str()));
				// Panel width in HP, from a preview widget (no module), as the module browser makes.
				try {
					app::ModuleWidget* preview = model->createModuleWidget(NULL);
					json_object_set_new(modelJ, "hp", json_integer((json_int_t) std::round(preview->box.size.x / RACK_GRID_WIDTH)));
					delete preview;
				}
				catch (...) {
				}
				json_t* paramsJ = json_array();
				for (size_t i = 0; i < module->paramQuantities.size(); i++) {
					engine::ParamQuantity* pq = module->paramQuantities[i];
					json_array_append_new(paramsJ, json_pack("{s:i,s:s,s:f,s:f,s:f,s:b}",
						"index", (int) i, "name", pq->getLabel().c_str(),
						"min", (double) pq->getMinValue(), "max", (double) pq->getMaxValue(),
						"default", (double) pq->getDefaultValue(), "snap", pq->snapEnabled));
				}
				json_object_set_new(modelJ, "params", paramsJ);
				auto ports = [](const std::vector<engine::PortInfo*>& infos) {
					json_t* a = json_array();
					for (size_t i = 0; i < infos.size(); i++)
						json_array_append_new(a, json_pack("{s:i,s:s}", "index", (int) i, "name", infos[i]->getName().c_str()));
					return a;
				};
				json_object_set_new(modelJ, "inputs", ports(module->inputInfos));
				json_object_set_new(modelJ, "outputs", ports(module->outputInfos));
				json_object_set_new(modelsJ, model->slug.c_str(), modelJ);
				delete module;
			}
			json_object_set_new(pluginJ, "models", modelsJ);
			json_object_set_new(rootJ, p->slug.c_str(), pluginJ);
		}
		std::string tmp = path + ".tmp";
		if (json_dump_file(rootJ, tmp.c_str(), JSON_INDENT(1)) == 0)
			std::rename(tmp.c_str(), path.c_str());
		json_decref(rootJ);
		note("catalog " + path);
	}
};

DirectorAgent* agent = NULL;

}  // namespace

struct Director : engine::Module {
	Director() {
		config(0, 0, 0, 0);
	}
};

struct DirectorWidget : app::ModuleWidget {
	DirectorWidget(Director* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Director.svg")));
	}

	void step() override {
		ModuleWidget::step();
		// Only a real instance in the rack installs the agent, not the browser preview.
		if (module && !agent && APP->scene) {
			agent = new DirectorAgent;
			APP->scene->addChild(agent);
		}
	}
};

Model* modelDirector = createModel<Director, DirectorWidget>("Director");
