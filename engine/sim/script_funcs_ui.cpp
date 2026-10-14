// The presentation builtins of ScenarioFuncSet (docs/script-spec.md 7.3 category d): objectives and goals, messages,
// sounds and music, pings, the local selection, camera moves, interface queries, highlights, arrows and flags,
// warnings and the mouse. Each body follows its ScenarioFuncSet decompile (re/decomp/script_funcs, MessageWin and
// ScreenArrowSpline in re/decomp/scen_builtins; gitignored): return values, validity checks, the multiplayer guard
// and every piece of saved state are as the original's; what the original draws, plays or moves is posted to the
// presentation queue (sim/script_ui.hpp), and what it reads from the local UI comes from ScriptInput.
#include <cctype>
#include <cmath>
#include <cstring>

#include "script/bhs_stdlib.hpp"
#include "sim/ai.hpp"
#include "sim/combat.hpp"
#include "sim/fog.hpp"
#include "sim/production.hpp"
#include "sim/script_funcs_internal.hpp"
#include "sim/trig.hpp"
#include "sim/placement.hpp"

namespace ron::sim {
namespace sf {

namespace {

std::string u8(const U16& s) { return bhs::to_utf8(s); }
std::string lower(std::string s) {
	for (char& c : s) c = char(std::tolower(uint8_t(c)));
	return s;
}

// GameAccess::console->who / ->play (the local leader).
int32_t console(const Game& g) { return g.local_play; }

// Scene::add_ping (from = the pinging leader, flash = the add_ping flag). The original adds the ping only on the
// machine whose console matches the target leader (Leader +8 == console->who); the queue keeps every ping with its
// target in x2 so a renderer can filter by seat.
void add_ping(Game& g, int32_t x, int32_t y, int32_t from, int32_t flash, int32_t to) {
	ScriptEvent& e = push_event(g, ScriptEvent::kPing);
	e.x = x;
	e.y = y;
	e.who = from;
	e.a = flash;
	e.text = std::to_string(to);
}

void camera_event(Game& g, int32_t kind, int32_t x, int32_t y, int32_t zoom) {
	ScriptPresentation& p = pres(g);
	ScriptEvent& e = push_event(g, ScriptEvent::kCamera);
	e.a = kind;
	e.x = x;
	e.y = y;
	e.who = zoom;
	p.camera.seq = e.seq;
	p.camera.kind = kind;
	if (x >= 0) {
		p.camera.x = x;
		p.camera.y = y;
	}
	if (zoom >= 0) p.camera.zoom = zoom;
}

// Camera pan (panning = 1, pan_to) unless scrolling is locked (scroll_lock_flags & 8).
void pan_to(Game& g, int32_t x, int32_t y) {
	if (pres(g).scroll_lock_flags & 8) return;
	camera_event(g, CameraRequest::kPan, x, y, -1);
}

// The to_zoom_level write of zoom_in / zoom_in_on (6) and zoom_out (4): only when the current level differs.
void zoom_to(Game& g, int32_t level) {
	if (input(g).zoom_level != level) camera_event(g, CameraRequest::kZoom, -1, -1, level);
}

// A popup posted to the queue and answered by the renderer's modal handler (BasePop::exec).
int32_t popup(Game& g, const U16& text, std::initializer_list<U16> buttons) {
	ScriptEvent& e = push_event(g, ScriptEvent::kPopup);
	e.text = u8(text);
	std::string* slot[3] = {&e.text2, &e.text3, &e.text4};
	int k = 0;
	for (const U16& b : buttons) {
		if (k < 3) *slot[k] = u8(b);
		k++;
	}
	e.a = k;
	const ScriptEvent copy = e;
	const ScriptInput& in = input(g);
	return in.popup ? in.popup(copy) : 0;
}

void set_pause(Game& g, bool on) {
	pres(g).paused = on;
	push_event(g, ScriptEvent::kPause).a = on ? 1 : 0;
}

// The cursor-over-interface test of the mouse builtins (iface opened and scaled_screen_ys - iface height < select_y2).
bool cursor_blocked(Game& g) { return !pres(g).ui_hidden && input(g).cursor_over_iface; }

// ScenarioFuncSet::get_iface_rect @9e27e0 (the renderer's rectangles; unknown -> all -1).
ScriptRect iface_rect(Game& g, const U16& name) {
	const auto& m = input(g).iface_rects;
	auto it = m.find(lower(u8(name)));
	return it == m.end() ? ScriptRect{} : it->second;
}

// The highlight of highlight_build_option / _train_option / _tech / highlight_option.
void highlight(Game& g, int32_t option, int32_t object) {
	ScriptPresentation& p = pres(g);
	p.hilite_option = option;
	p.hilite_object = object;
	p.highlight_active = true;
}

// ScreenArrowSpline::add_arrow @910ac0: appends an arrow (end_time = tick + seconds, 0 = forever) and returns the count.
int32_t add_arrow(Game& g, const ScriptArrow& a) {
	ScriptPresentation& p = pres(g);
	p.arrows.push_back(a);
	return int32_t(p.arrows.size());
}

}  // namespace

void install_ui_funcs(ScriptHost& h) {
	Game& g = h.game;
	bhs::Runtime& rt = h.rt;
	auto def = [&rt](const char* name, Fn f, int overload = 0) { rt.set_native(name, std::move(f), overload); };

	// ---- objectives (MessageWin scenario objectives; add_objective @9e34d0 ... clear_objectives @9e3650)
	auto add_obj = [&g](NativeCall& c, bool announce) {
		// MessageWin::add_scen_objective (when MessageWin::tell; text must be non-empty), then set_scen_objective_sound.
		const U16 text = c.arg_str(0), id = c.arg_str(1), sound = c.arg_str(2);
		ScriptPresentation& p = pres(g);
		if (!text.empty()) {
			ScriptObjective o;
			o.id = u8(id);
			o.text = u8(text);
			o.announce = announce;
			o.frame = g.frame;
			o.color = scriptcolor::kWhite;  // next_message_color (reset to LIGHT_GREY after each entry)
			p.objectives.push_back(std::move(o));
		}
		for (ScriptObjective& o : p.objectives) {
			if (id.empty() || bhs::ieq(bhs::to_u16(o.id), id)) o.sound = u8(sound);
		}
		return make_int(1);
	};
	def("add_objective", [add_obj](NativeCall& c) { return add_obj(c, true); });
	def("add_objective_text", [add_obj](NativeCall& c) { return add_obj(c, false); });
	def("ctw_add_objective_text", [add_obj](NativeCall& c) { return add_obj(c, false); });  // add_ctw_objective
	def("change_objective_text", [&g](NativeCall& c) {
		const U16 id = c.arg_str(0);
		for (ScriptObjective& o : pres(g).objectives) {
			if (bhs::ieq(bhs::to_u16(o.id), id)) o.text = u8(c.arg_str(1));
		}
		return make_int(1);
	});
	def("set_objective_sound", [&g](NativeCall& c) {
		const U16 id = c.arg_str(0);
		for (ScriptObjective& o : pres(g).objectives) {
			if (id.empty() || bhs::ieq(bhs::to_u16(o.id), id)) o.sound = u8(c.arg_str(1));
		}
		return make_int(1);
	});
	def("set_objective_loc", [&g](NativeCall& c) {
		const U16 id = c.arg_str(0);
		for (ScriptObjective& o : pres(g).objectives) {
			if (id.empty() || bhs::ieq(bhs::to_u16(o.id), id)) {
				o.x = tile_centre(c.arg_int(1));
				o.y = tile_centre(c.arg_int(2));
			}
		}
		return make_int(1);
	});
	def("play_objective_sound", [&h, &g](NativeCall& c) {
		// MessageWin::play_scen_objective_sound @7e8450: the first entry matching the id ("" = the first entry) plays its
		// sound through ScenarioFuncSet::play_sound.
		const U16 id = c.arg_str(0);
		for (const ScriptObjective& o : pres(g).objectives) {
			if (id.empty() || bhs::ieq(bhs::to_u16(o.id), id)) {
				h.play_sound(bhs::to_u16(o.sound));
				break;
			}
		}
		return make_int(1);
	});
	def("objective_complete", [&g](NativeCall& c) {
		// MessageWin::end_scen_objective @7e8690: every match ("" = all) gets the completion stamp.
		const U16 id = c.arg_str(0);
		for (ScriptObjective& o : pres(g).objectives) {
			if (id.empty() || bhs::ieq(bhs::to_u16(o.id), id)) o.completed = g.frame > 0 ? g.frame : 1;
		}
		return make_int(1);
	});
	def("remove_objective", [&g](NativeCall& c) {
		const U16 id = c.arg_str(0);
		auto& v = pres(g).objectives;
		v.erase(std::remove_if(v.begin(), v.end(), [&](const ScriptObjective& o) { return bhs::ieq(bhs::to_u16(o.id), id); }),
		        v.end());
		return make_int(1);
	});
	def("clear_objectives", [&g](NativeCall&) {
		pres(g).objectives.clear();
		return make_int(1);
	});

	// ---- goals (ScenarioData::objectives[who], saved simulation state)
	def("add_goal", [&g](NativeCall& c) {  // @9e3670: returns the new count
		const int32_t w = c.arg_int(0) - 1;
		if (!valid_leader(g, w)) return make_int(-1);
		auto& v = g.scen.goals[size_t(w)];
		v.push_back(ScenarioData::Goal{c.arg_str(1), c.arg_str(2), 0});
		return make_int(int32_t(v.size()));
	});
	auto goal_set = [&g](NativeCall& c, int32_t done) {  // complete_goal @9e3780 / reset_goal @9e3880
		const int32_t w = c.arg_int(0) - 1;
		if (!valid_leader(g, w)) return make_int(-1);
		for (auto& o : g.scen.goals[size_t(w)]) {
			if (bhs::ieq(o.id, c.arg_str(1))) {
				o.completed = done;
				return make_int(1);
			}
		}
		return make_int(0);
	};
	def("complete_goal", [goal_set](NativeCall& c) { return goal_set(c, 1); });
	def("reset_goal", [goal_set](NativeCall& c) { return goal_set(c, 0); });
	def("remove_goal", [&g](NativeCall& c) {  // @9e3980
		const int32_t w = c.arg_int(0) - 1;
		if (!valid_leader(g, w)) return make_int(-1);
		auto& v = g.scen.goals[size_t(w)];
		for (size_t k = 0; k < v.size(); k++) {
			if (bhs::ieq(v[k].id, c.arg_str(1))) {
				v.erase(v.begin() + long(k));
				return make_int(1);
			}
		}
		return make_int(0);
	});
	def("clear_goals", [&g](NativeCall& c) {  // @9e3a50
		const int32_t w = c.arg_int(0) - 1;
		if (!valid_leader(g, w)) return make_int(-1);
		g.scen.goals[size_t(w)].clear();
		return make_int(1);
	});
	def("goal_dialog_shown", [&g](NativeCall&) { return make_int(g.scen.objective_dlg_viewed ? 1 : 0); });
	def("show_goal_dialog", [&g](NativeCall&) {  // @9e3ae0 (ObjectivesDlg::exec)
		if (mp(g)) return make_int(-1);
		push_event(g, ScriptEvent::kGoalDialog);
		return make_int(1);
	});

	// ---- messages
	def("set_instruction_msg", [&g](NativeCall& c) {  // @9e3b00
		if (mp(g)) return make_int(-1);
		push_event(g, ScriptEvent::kInstruction).text = u8(c.arg_str(0));
		return make_int(1);
	});
	def("set_feedback_msg", [&g](NativeCall& c) {  // @9e3b80 (colour RED)
		if (mp(g)) return make_int(-1);
		ScriptEvent& e = push_event(g, ScriptEvent::kFeedback);
		e.text = u8(c.arg_str(0));
		e.color = scriptcolor::kRed;
		return make_int(1);
	});
	def("print_game_msg", [&g](NativeCall& c) {  // @9fecd0: MessageWin::add_message in game_msg_color
		ScriptEvent& e = push_event(g, ScriptEvent::kGameMessage);
		e.text = u8(c.arg_str(0));
		e.color = g.scen.game_msg_color;
		return VP();
	});
	def("print_msg", [&g](NativeCall& c) {  // @9feb80: MessageWin::add_scen_message (msg_time, msg_color)
		ScriptPresentation& p = pres(g);
		ScriptEvent& e = push_event(g, ScriptEvent::kScenarioMessage);
		e.text = u8(c.arg_str(0));
		e.a = g.scen.msg_time;
		e.color = g.scen.msg_color;
		p.scenario_message = e.text;
		p.scenario_message_frame = g.frame;
		p.scenario_message_ms = g.scen.msg_time;
		p.scenario_message_color = g.scen.msg_color;
		return VP();
	});
	def("clear_game_msg", [&g](NativeCall&) {  // @9fed30: MessageWin::clear_messages
		push_event(g, ScriptEvent::kClearMessages);
		return VP();
	});
	def("chat", [&g](NativeCall& c) {  // @9e3d80
		const int32_t to = c.arg_int(0) - 1, from = c.arg_int(1) - 1;
		if (!live_leader(g, to) || !valid_leader(g, from)) return make_int(-1);
		if (to == console(g)) {
			ScriptEvent& e = push_event(g, ScriptEvent::kChat);
			e.text = u8(c.arg_str(2));
			e.who = from;
		}
		return make_int(1);
	});
	def("chat_all", [&g](NativeCall& c) {  // @9e3e00
		const int32_t from = c.arg_int(0) - 1;
		if (!valid_leader(g, from)) return make_int(-1);
		if (live_leader(g, console(g))) {
			ScriptEvent& e = push_event(g, ScriptEvent::kChat);
			e.text = u8(c.arg_str(1));
			e.who = from;
		}
		return make_int(1);
	});
	def("show_notice", [&g](NativeCall& c) {  // @a02740 (IFaceMainBase::do_notice; -1 when the image is missing)
		const int32_t w = c.arg_int(2) - 1;
		if (!valid_leader(g, w)) return make_int(-1);
		ScriptEvent& e = push_event(g, ScriptEvent::kNotice);
		e.text = u8(c.arg_str(0));
		e.text2 = u8(c.arg_str(1));
		e.who = w;
		return make_int(1);
	});
	def("bubble_text_obj", [&g](NativeCall& c) {  // @9ff550: MessageWin::add_bubble_message at the object
		const int32_t w = c.arg_int(1) - 1, o = c.arg_int(2);
		Object* ob = valid_object(g, w, o);
		if (!ob) return make_int(-1);
		ScriptEvent& e = push_event(g, ScriptEvent::kBubble);
		e.text = u8(c.arg_str(0));
		e.x = ob->x;
		e.y = ob->y;
		e.who = console(g);
		return make_int(1);
	});
	def("bubble_text", [&g](NativeCall& c) {  // @9ff4f0: a bubble at a tile
		const int32_t x = c.arg_int(1), y = c.arg_int(2);
		if (!tile_ok(g, x, y)) return make_int(-1);
		ScriptEvent& e = push_event(g, ScriptEvent::kBubble);
		e.text = u8(c.arg_str(0));
		e.x = tile_centre(x);
		e.y = tile_centre(y);
		e.who = console(g);
		return make_int(1);
	});
	def("bubble_text_hide", [&g](NativeCall&) {  // @9ff4e0
		pres(g).display_bubble_text = false;
		return make_int(1);
	});
	def("popup_dialog", [&g](NativeCall& c) {  // @9fedd0 (modal; returns 1)
		popup(g, c.arg_str(0), {});
		return make_int(1);
	});
	def("popup_choice", [&g](NativeCall& c) {  // @9fee70: pauses (outside the CTW window), 1 = the accept button
		set_pause(g, true);
		const int32_t r = popup(g, c.arg_str(0), {c.arg_str(1), c.arg_str(2)});
		set_pause(g, false);
		return make_int(r == 0 ? 1 : 0);
	});
	def("popup_choice", [&g](NativeCall& c) {  // @9fef90: the chosen button 0..2
		set_pause(g, true);
		const int32_t r = popup(g, c.arg_str(0), {c.arg_str(1), c.arg_str(2), c.arg_str(3)});
		set_pause(g, false);
		return make_int(r);
	}, 1);
	def("use_basic_menu", [&g](NativeCall&) {
		if (!mp(g)) pres(g).basic_menu = true;
		return VP();
	});
	def("use_advanced_menu", [&g](NativeCall&) {
		if (!mp(g)) pres(g).basic_menu = false;
		return VP();
	});
	def("set_victory_message", [&g](NativeCall& c) {
		g.scen.victory_message = c.arg_str(0);
		pres(g).victory_message = u8(c.arg_str(0));
		return make_int(1);
	});
	def("set_defeat_message", [&g](NativeCall& c) {
		g.scen.defeat_message = c.arg_str(0);
		pres(g).defeat_message = u8(c.arg_str(0));
		return make_int(1);
	});

	// ---- sound and music
	def("play_sound", [&h](NativeCall& c) { return make_int(h.play_sound(c.arg_str(0))); });  // @a02f50
	def("play_sound_to", [](NativeCall&) { return make_int(-1); });                            // @9e3c50
	def("stop_sound", [&h, &g](NativeCall&) {                                                   // @9e3c60
		if (h.sound.active) {
			h.sound.active = false;
			push_event(g, ScriptEvent::kStopSound);
		}
		return VP();
	});
	def("sound_finished", [&h, &g](NativeCall&) {  // @9e3c80
		if (mp(g)) return make_int(-1);
		return make_int(h.sound_done() ? 1 : 0);
	});
	def("set_music_volume", [](NativeCall&) { return VP(); });  // @9e3c40: empty body
	def("set_music_mood", [&g](NativeCall& c) {                 // @9e3cc0
		const U16 m = c.arg_str(0);
		const char* mood = bhs::ieq(m, u"winning") ? "winning" : bhs::ieq(m, u"losing") ? "losing"
		                                                       : bhs::ieq(m, u"economic") ? "economic" : nullptr;
		if (!mood) return make_int(-1);
		ScriptPresentation& p = pres(g);
		if (p.music_mood != mood) {
			p.music_mood = mood;
			ScriptEvent& e = push_event(g, ScriptEvent::kMusicMood);
			e.text = mood;
			e.a = c.arg_int(1);
		}
		return make_int(1);
	});

	// ---- pings
	def("ping", [&g](NativeCall& c) {  // @9e3e70: Leader::receive_ping on the target's machine
		const int32_t to = c.arg_int(0) - 1, from = c.arg_int(1) - 1;
		const int32_t x = c.arg_int(2), y = c.arg_int(3);
		if (!live_leader(g, to) || !live_leader(g, from) || !tile_ok(g, x, y)) return make_int(-1);
		if (to == console(g)) add_ping(g, tile_centre(x), tile_centre(y), from, 1, to);
		return make_int(1);
	});
	def("ping_all", [&g](NativeCall& c) {  // @9e3f30 (who_to is the pinging leader)
		const int32_t w = c.arg_int(0) - 1;
		const int32_t x = c.arg_int(1), y = c.arg_int(2);
		if (!live_leader(g, w) || !tile_ok(g, x, y)) return make_int(-1);
		add_ping(g, tile_centre(x), tile_centre(y), w, 1, -1);
		return make_int(1);
	});
	def("ping_object", [&g](NativeCall& c) {  // @9e3fd0: only on the target's machine
		const int32_t to = c.arg_int(0) - 1, from = c.arg_int(1) - 1, whose = c.arg_int(2) - 1;
		if (!live_leader(g, from) || to != console(g)) return make_int(-1);
		Object* ob = valid_object(g, whose, c.arg_int(3));
		if (!ob) return make_int(-1);
		add_ping(g, ob->x, ob->y, from, 1, to);
		return make_int(1);
	});
	def("ping_object_all", [&g](NativeCall& c) {  // @9e4090
		const int32_t from = c.arg_int(0) - 1, whose = c.arg_int(1) - 1;
		if (!live_leader(g, from)) return make_int(-1);
		Object* ob = valid_object(g, whose, c.arg_int(2));
		if (!ob) return make_int(-1);
		add_ping(g, ob->x, ob->y, from, 1, -1);
		return make_int(1);
	});
	auto ping_named = [&g](const U16& name, int32_t from, bool only_owner_console) {  // @9e4140 / @9e42b0
		const int32_t gi = g.scen.find_group(name);
		if (gi < 0) return -1;
		const ScenarioData::Group& gr = g.scen.groups[size_t(gi)];
		if (!live_leader(g, gr.who) || !live_leader(g, from)) return -1;
		if (only_owner_console && gr.who != console(g)) return 1;
		for (int32_t id : gr.ids) {
			if (Object* ob = valid_object(g, gr.who, id)) add_ping(g, ob->x, ob->y, from, 1, gr.who);
		}
		return 1;
	};
	auto unnamed = [](int32_t who) { return U16(u"_Unnamed") + bhs::itow_text(who); };  // default_group @4ce990
	def("ping_group", [ping_named, unnamed](NativeCall& c) {  // @4cea20
		return make_int(ping_named(unnamed(c.arg_int(1)), c.arg_int(0) - 1, true));
	});
	def("ping_group", [ping_named](NativeCall& c) { return make_int(ping_named(c.arg_str(0), c.arg_int(1) - 1, true)); }, 1);
	def("ping_group_all", [ping_named, unnamed](NativeCall& c) {  // @4ce920
		return make_int(ping_named(unnamed(c.arg_int(1)), c.arg_int(0) - 1, false));
	});
	def("ping_group_all", [ping_named](NativeCall& c) {
		return make_int(ping_named(c.arg_str(0), c.arg_int(1) - 1, false));
	}, 1);
	auto ping_ug = [&g](NativeCall& c, bool only_owner_console) {  // @9fa9c0 / @9faae0 (UnitGroup {nation, units})
		bhs::Value* s = c.args.empty() ? nullptr : c.args[0].get();
		if (!s || s->elems.size() < 2 || !s->elems[0] || !s->elems[1]) return make_int(-1);
		const int32_t who = bhs::value_int(s->elems[0].get());
		const int32_t from = c.arg_int(1) - 1;
		if (!live_leader(g, who) || !live_leader(g, from)) return make_int(-1);
		if (only_owner_console && who != console(g)) return make_int(1);
		for (const VP& v : s->elems[1]->elems) {
			if (Object* ob = valid_object(g, who, v ? bhs::value_int(v.get()) : -1)) add_ping(g, ob->x, ob->y, from, 1, who);
		}
		return make_int(1);
	};
	def("ping_group", [ping_ug](NativeCall& c) { return ping_ug(c, true); }, 2);
	def("ping_group_all", [ping_ug](NativeCall& c) { return ping_ug(c, false); }, 2);
	def("ping_constant", [&g](NativeCall& c) {  // @9e43f0 (no flash)
		const int32_t from = c.arg_int(0) - 1;
		const int32_t x = c.arg_int(1), y = c.arg_int(2);
		if (!live_leader(g, from) || !tile_ok(g, x, y)) return make_int(-1);
		add_ping(g, tile_centre(x), tile_centre(y), from, 0, -1);
		return make_int(1);
	});
	def("clear_pings", [&g](NativeCall&) {
		push_event(g, ScriptEvent::kClearPings);
		return make_int(1);
	});

	// ---- the local selection (console.play's SelectGroup)
	def("object_selected", [&g](NativeCall& c) {  // @9f0270
		const int32_t w = c.arg_int(0) - 1;
		int32_t o = c.arg_int(1);
		if (!live_leader(g, w) || mp(g)) return make_int(-1);
		if (!valid_object(g, w, o)) return make_int(-1);
		const ScriptInput& in = input(g);
		if (in.select_who != w) return make_int(0);
		o = captain_of(g, w, o);
		return make_int(std::find(in.selected.begin(), in.selected.end(), o) != in.selected.end() ? 1 : 0);
	});
	def("object_type_selected", [&g](NativeCall& c) {  // @9f04e0
		const int32_t w = c.arg_int(0) - 1;
		const int32_t t = type_index(g, c.arg_str(1));
		if (t < 0 || mp(g) || !valid_leader(g, w)) return make_int(-1);
		const ScriptInput& in = input(g);
		if (in.select_who != w) return make_int(0);
		for (int32_t o : in.selected) {
			Object* ob = object_at(g, w, o);
			if (!ob || !ob->ptype || !type_is(*ob->ptype, t)) continue;
			// vfuncs +0x20 (is a building) / +0x4c (WallData::is_active): an unfinished building does not count.
			if (!ob->is_unit && !(ob->flags & objflag::kActive)) return make_int(0);
			return make_int(1);
		}
		return make_int(0);
	});
	def("num_objects_selected", [&g](NativeCall& c) {  // @9f06f0: GroupData::get_num_cap_const
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w) || mp(g)) return make_int(-1);
		const ScriptInput& in = input(g);
		if (in.select_who != w) return make_int(0);
		int32_t n = 0;
		for (int32_t o : in.selected) {
			const Object* ob = object_at(g, w, o);
			if (ob && (ob->flags & objflag::kValid)) n++;
		}
		return make_int(n);
	});
	def("any_object_selected", [&g](NativeCall& c) {  // @9f0680
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w) || mp(g)) return make_int(-1);
		const ScriptInput& in = input(g);
		if (in.select_who != w) return make_int(0);
		return make_int(in.selected.empty() ? 0 : 1);
	});
	def("find_selected_id", [&g](NativeCall&) {  // @9f0900 (no guard)
		const ScriptInput& in = input(g);
		return make_int(in.selected.empty() ? -1 : in.selected.front());
	});
	def("clear_selection", [&g](NativeCall&) {  // @9f0930: SelectGroup::clear (the renderer applies it)
		ScriptInput& in = input(g);
		in.selected.clear();
		push_event(g, ScriptEvent::kCamera).a = CameraRequest::kNone;  // a no-op camera event marks the change
		pres(g).events.back().text = "clear_selection";
		return make_int(1);
	});
	auto sel_flag = [&g](bool* flag, bool v) {  // @9f0960..9f0a20 (single player scripted games only)
		if (mp(g) || !scripted(g)) return make_int(-1);
		*flag = v;
		return make_int(1);
	};
	def("mouse_selection_enable", [&g, sel_flag](NativeCall&) { return sel_flag(&pres(g).mouse_selection_disabled, false); });
	def("mouse_selection_disable", [&g, sel_flag](NativeCall&) { return sel_flag(&pres(g).mouse_selection_disabled, true); });
	def("hotkey_selection_enable", [&g, sel_flag](NativeCall&) { return sel_flag(&pres(g).hotkey_selection_disabled, false); });
	def("hotkey_selection_disable", [&g, sel_flag](NativeCall&) { return sel_flag(&pres(g).hotkey_selection_disabled, true); });

	// ---- camera
	auto centre_on = [&g](NativeCall& c, bool zoom) {  // center_camera_on @9fdc80 / zoom_in_on @9fdd60
		if (mp(g)) return make_int(-1);
		const int32_t w = c.arg_int(0) - 1;
		int32_t o = c.arg_int(1);
		if (active_unit_slot(g, w, o)) o = get_captain(g, w, o);
		Object* ob = valid_object(g, w, o);
		if (!ob) return make_int(-1);
		if (zoom) zoom_to(g, 6);
		pan_to(g, ob->x, ob->y);
		return make_int(1);
	};
	def("center_camera_on", [centre_on](NativeCall& c) { return centre_on(c, false); });
	def("zoom_in_on", [centre_on](NativeCall& c) { return centre_on(c, true); });
	def("move_camera", [&g](NativeCall& c) {  // @9fdb50
		if (mp(g)) return make_int(-1);
		const int32_t x = c.arg_int(0), y = c.arg_int(1);
		if (!tile_ok(g, x, y)) return make_int(-1);
		pan_to(g, tile_centre(x), tile_centre(y));
		return make_int(1);
	});
	def("jump_camera", [&g](NativeCall& c) {  // @9fdc00: Camera::set_loc
		if (mp(g)) return make_int(-1);
		const int32_t x = c.arg_int(0), y = c.arg_int(1);
		if (!tile_ok(g, x, y)) return make_int(-1);
		if (!(pres(g).scroll_lock_flags & 8)) camera_event(g, CameraRequest::kJump, tile_centre(x), tile_centre(y), -1);
		return make_int(1);
	});
	auto zoom_at = [&g](NativeCall& c, int32_t level) {  // zoom_in @9fd8d0 (6) / zoom_out @9fda10 (4)
		if (mp(g)) return make_int(-1);
		const int32_t x = c.arg_int(0), y = c.arg_int(1);
		if (x < 0) {
			zoom_to(g, level);
			return make_int(1);
		}
		if (y < 0 || x >= g.world.tile_xs || y >= g.world.tile_ys) return make_int(-1);
		zoom_to(g, level);
		pan_to(g, tile_centre(x), tile_centre(y));
		return make_int(1);
	};
	def("zoom_in", [zoom_at](NativeCall& c) { return zoom_at(c, 6); });
	def("zoom_out", [zoom_at](NativeCall& c) { return zoom_at(c, 4); });
	def("get_camera_x", [&g](NativeCall&) {  // @9fdfb0: (int)(loc_x / 192.0)
		if (mp(g)) return make_int(-1);
		return make_int(int32_t(double(input(g).camera_x) / 192.0));
	});
	def("get_camera_y", [&g](NativeCall&) {
		if (mp(g)) return make_int(-1);
		return make_int(int32_t(double(input(g).camera_y) / 192.0));
	});
	def("get_current_zoom", [&g](NativeCall&) { return make_int(mp(g) ? -1 : input(g).zoom_level); });
	def("is_zoomed_out", [&g](NativeCall&) { return make_int(mp(g) ? -1 : (input(g).zoom_level < 5 ? 1 : 0)); });
	def("is_camera_panning", [&g](NativeCall&) { return make_int(input(g).camera_panning ? 1 : 0); });
	def("scrolling_lock", [&g](NativeCall&) {
		pres(g).scroll_lock_flags = 0xfff7;
		return VP();
	});
	def("scrolling_unlock", [&g](NativeCall&) {
		pres(g).scroll_lock_flags = 0;
		return VP();
	});
	def("scrolling_unlock_minimap", [&g](NativeCall&) {
		pres(g).scroll_lock_flags &= ~4u;
		return VP();
	});
	def("zooming_lock", [&g](NativeCall&) {
		pres(g).zoom_lock_flags = 0xfff7;
		return VP();
	});
	def("zooming_unlock", [&g](NativeCall&) {
		pres(g).zoom_lock_flags = 0;
		return VP();
	});
	def("is_on_screen", [&g](NativeCall& c) {  // @9f3b50: world_to_console of the tile centre (at terrain height)
		if (mp(g)) return make_int(-1);
		const int32_t x = c.arg_int(0), y = c.arg_int(1);
		if (!tile_ok(g, x, y)) return make_int(-1);
		const ScriptInput& in = input(g);
		return make_int(in.on_screen && in.on_screen(tile_centre(x), tile_centre(y)) ? 1 : 0);
	});

	// ---- interface
	auto button = [&g](NativeCall& c, int k) {  // get_button_* @9f3c70.. (rect_valid: left != right)
		const ScriptRect r = iface_rect(g, c.arg_str(0));
		if (!r.valid()) return make_int(-1);
		switch (k) {
		case 0: return make_int(r.top);
		case 1: return make_int(r.left);
		case 2: return make_int(r.right);
		case 3: return make_int((r.right + r.left) / 2);
		case 4: return make_int(r.bottom);
		default: return make_int((r.bottom + r.top) / 2);
		}
	};
	def("get_button_top", [button](NativeCall& c) { return button(c, 0); });
	def("get_button_left", [button](NativeCall& c) { return button(c, 1); });
	def("get_button_right", [button](NativeCall& c) { return button(c, 2); });
	def("get_button_center_x", [button](NativeCall& c) { return button(c, 3); });
	def("get_button_bottom", [button](NativeCall& c) { return button(c, 4); });
	def("get_button_center_y", [button](NativeCall& c) { return button(c, 5); });
	def("get_screen_width", [&g](NativeCall&) { return make_int(input(g).screen_w); });
	def("get_screen_height", [&g](NativeCall&) { return make_int(input(g).screen_h); });
	auto objectives_rect = [&g](int k) {  // get_objectives_* @9f3e10.. (an all-zero rect is -1)
		const ScriptRect& r = input(g).objectives_rect;
		if (r.left == 0 && r.right == 0 && r.top == 0 && r.bottom == 0) return make_int(-1);
		return make_int(k == 0 ? r.top : k == 1 ? r.left : k == 2 ? r.right : r.bottom);
	};
	def("get_objectives_top", [objectives_rect](NativeCall&) { return objectives_rect(0); });
	def("get_objectives_left", [objectives_rect](NativeCall&) { return objectives_rect(1); });
	def("get_objectives_right", [objectives_rect](NativeCall&) { return objectives_rect(2); });
	def("get_objectives_bottom", [objectives_rect](NativeCall&) { return objectives_rect(3); });
	def("ui_hide", [&g](NativeCall&) {
		pres(g).ui_hidden = true;
		return make_int(1);
	});
	def("ui_show", [&g](NativeCall&) {
		pres(g).ui_hidden = false;
		return make_int(1);
	});
	auto armed = [&g](int32_t opt) {  // is_*_selected @9f3be0..: opt.option and selecting_spot (no guard)
		const ScriptInput& in = input(g);
		return make_int(in.option == opt && in.selecting_spot ? 1 : 0);
	};
	def("is_move_selected", [armed](NativeCall&) { return armed(scriptopt::kMoveTo); });
	def("is_rally_selected", [armed](NativeCall&) { return armed(scriptopt::kGather); });
	def("is_patrol_selected", [armed](NativeCall&) { return armed(scriptopt::kPatrol); });
	def("is_airstrike_selected", [armed](NativeCall&) { return armed(scriptopt::kAirstrike); });
	auto build_menu = [&g](int32_t data) {  // build_menu_active @9f3a00 / build_military_menu_active @9f3a30
		if (mp(g)) return make_int(-1);
		const ScriptInput& in = input(g);
		return make_int(in.build_options && in.mode_data == data ? 1 : 0);
	};
	def("build_menu_active", [build_menu](NativeCall&) { return build_menu(0); });
	def("build_military_menu_active", [build_menu](NativeCall&) { return build_menu(1); });
	def("build_option_selected", [&g](NativeCall& c) {  // @9f3a90
		if (mp(g)) return make_int(-1);
		const int32_t t = type_index(g, c.arg_str(0));
		if (t < 0) return make_int(-1);
		const ScriptInput& in = input(g);
		const bool build = in.option == scriptopt::kBuild || in.option == scriptopt::kBuildMilitary ||
		                   in.option == scriptopt::kBuildWonder;
		return make_int(build && in.build_options && in.option_object == t ? 1 : 0);
	});
	// Options::mode DIPLO_NEGOTIATE_OPTIONS / DIPLO_OPTIONS: the renderer reports them through `option` as -2 (contact
	// panel, mode_data = the contacted leader) and -3 (diplomacy panel).
	def("contact_open", [&g](NativeCall&) {  // @9fcd40
		const ScriptInput& in = input(g);
		return make_int(in.option == -2 ? in.mode_data + 1 : 0);
	});
	def("diplomacy_open", [&g](NativeCall&) { return make_int(input(g).option == -3 ? 1 : 0); });  // @9fcd30

	// ---- highlights, arrows, flags
	def("clear_highlight", [&g](NativeCall&) {  // @9ff400
		if (mp(g)) return make_int(-1);
		ScriptPresentation& p = pres(g);
		p.hilite_option = -1;
		p.hilite_object = -1;
		p.highlight_active = false;
		return make_int(1);
	});
	def("highlight_build_option", [&g](NativeCall& c) {  // @9ff100: the build menu of the type's category
		const int32_t t = type_index(g, c.arg_str(0));
		if (mp(g) || t < 0 || !is_build_type(t)) return make_int(-1);
		const int32_t cat = g.type(t).cat;
		highlight(g, cat >= 0 && cat <= 2 ? cat : -1, t);
		return make_int(1);
	});
	def("highlight_train_option", [&g](NativeCall& c) {  // @9ff210 (OPTION_TRAIN 0x2f)
		const int32_t t = type_index(g, c.arg_str(0));
		if (mp(g) || t < 0) return make_int(-1);
		highlight(g, scriptopt::kTrain, t);
		return make_int(1);
	});
	def("highlight_tech", [&g](NativeCall& c) {  // @9ff2a0 (OPTION_RESEARCH 0x30; no guard; always 1)
		highlight(g, scriptopt::kResearch, type_index(g, c.arg_str(0)));
		return make_int(1);
	});
	def("highlight_option", [&g](NativeCall& c) {  // @9ff310
		// option_info's English names (the 330 command options) are interface data the port does not load: the body's
		// fallback is taken (hilite_object = the type of that name, hilite_option -1, returns -1) and the name is kept
		// for the renderer in the event.
		if (mp(g)) return make_int(-1);
		highlight(g, -1, type_index(g, c.arg_str(0)));
		ScriptEvent& e = push_event(g, ScriptEvent::kCamera);
		e.a = CameraRequest::kNone;
		e.text = "highlight_option";
		e.text2 = u8(c.arg_str(0));
		return make_int(-1);
	});
	def("clear_all_arrows", [&g](NativeCall&) {  // @9ffd30
		pres(g).arrows.clear();
		return VP();
	});
	def("draw_screen_arrow_line", [&g](NativeCall& c) {  // @9ffcc0
		ScriptArrow a;
		a.x1 = c.arg_int(0);
		a.y1 = c.arg_int(1);
		a.x2 = c.arg_int(2);
		a.y2 = c.arg_int(3);
		a.seconds = c.arg_int(4);
		a.frame = g.frame;
		return make_int(add_arrow(g, a));
	});
	def("add_flag", [&g](NativeCall& c) {  // @9fd790: ScenarioData::add_component
		if (!mp(g)) {
			const int32_t x = c.arg_int(0), y = c.arg_int(1);
			if (!tile_ok(g, x, y)) return make_int(-1);
			pres(g).flags.push_back({x, y});
		}
		return make_int(1);
	});
	def("clear_flags", [&g](NativeCall&) {  // @9fd7e0
		pres(g).flags.clear();
		return make_int(1);
	});

	// ---- warnings (single player)
	def("resource_warning", [&g](NativeCall&) {  // @9f3560: consumes ScenarioData::user_warnings bit 0
		if (mp(g)) return make_int(-1);
		ScriptInput& in = input(g);
		if (in.user_warnings & 1) {
			in.user_warnings &= ~1u;
			return make_int(1);
		}
		return make_int(0);
	});
	def("pop_cap_warning", [&g](NativeCall& c) {  // @9f3620: pop_cap <= control
		const int32_t w = c.arg_int(0) - 1;
		if (mp(g) || !live_leader(g, w)) return make_int(-1);
		const Leader& l = g.leaders[size_t(w)];
		return make_int(l.pop_cap <= l.control ? 1 : 0);
	});
	def("commerce_cap_warning", [&g](NativeCall& c) {  // @9f36b0: any good with over_cap != 0
		const int32_t w = c.arg_int(0) - 1;
		if (mp(g) || !live_leader(g, w)) return make_int(-1);
		for (int32_t k = 0; k < 6; k++) {
			if (g.leaders[size_t(w)].over_cap[size_t(k)] != 0) return make_int(1);
		}
		return make_int(0);
	});

	// ---- the mouse (console cursor tile)
	def("get_resource_at_mouse", [&g](NativeCall&) {  // @9f1ac0
		if (cursor_blocked(g)) return make_int(0);
		const ScriptInput& in = input(g);
		const int32_t x = in.cursor_tx, y = in.cursor_ty;
		if (!tile_ok(g, x, y) || !(g.world.t(x, y) & tmask::kResource)) return make_int(-1);
		// ObjectsData::find_good_at(w, -1, 0, 0): the good terminating the wcoord's object list.
		const WData& d = g.world.w(x >> 2, y >> 2);
		if (d.down == -2 && d.down_who >= 0 && size_t(d.down_who) < g.goods.size()) {
			const GoodObj& gd = g.goods[size_t(d.down_who)];
			if (gd.alive()) return make_int(gd.type);
		}
		return make_int(-1);
	});
	def("get_object_at_mouse", [&g](NativeCall& c) {  // @9f1620: ObjectsData::find(cursor centre, all, who, 0xc0)
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		if (cursor_blocked(g)) return make_int(-1);
		const ScriptInput& in = input(g);
		const int32_t x = tile_centre(in.cursor_tx), y = tile_centre(in.cursor_ty);
		if (!tile_ok(g, in.cursor_tx, in.cursor_ty)) return make_int(-1);
		const int32_t u = objects_find_unit(g, x, y, search::kAll, w, 0xc0, 0, filter::kAll, 0, 0, filter::kAll, 0, 0);
		if (u >= 0 && g.find_who == w) return make_int(u);
		return make_int(objects_find_building(g, x, y, search::kAll, w, 0xc0, 0, filter::kAll, 0, 0));
	});
	def("get_object_type_at_mouse", [&g](NativeCall& c) {  // @9f16e0
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		const int32_t t = type_index(g, c.arg_str(1));
		if (cursor_blocked(g) || t < 0) return make_int(-1);
		const ScriptInput& in = input(g);
		const int32_t x = tile_centre(in.cursor_tx), y = tile_centre(in.cursor_ty);
		if (is_unit_type(t))
			return make_int(objects_find_unit(g, x, y, search::kAll, 0x60, 0xc0, 0, filter::kType, t, 0, filter::kAll, 0, 0));
		if (is_build_type(t))
			return make_int(objects_find_building(g, x, y, search::kAll, 0x60, 0xc0, 0, filter::kType, t, 0));
		return make_int(-1);
	});
	def("is_game_mouse_near", [&g](NativeCall& c) {  // @9f38c0: vector_dist(cursor tile, tile) < r
		if (mp(g)) return make_int(-1);
		const int32_t x = c.arg_int(0), y = c.arg_int(1), r = c.arg_int(2);
		if (!tile_ok(g, x, y)) return make_int(-1);
		if (r < 0) return make_int(-1);
		const ScriptInput& in = input(g);
		return make_int(vector_dist(in.cursor_tx - x, in.cursor_ty - y) < r ? 1 : 0);
	});
	def("key_down", [&g](NativeCall& c) {  // @9f3950: keystate[VkKeyScanW(key[0])] bit 7
		if (mp(g)) return make_int(-1);
		const U16 k = c.arg_str(0);
		if (k.empty()) return make_int(0);
		const char16_t ch = k[0];
		const int32_t vk = (ch >= u'a' && ch <= u'z') ? ch - 32 : ch;
		return make_int(vk >= 0 && vk < 256 ? input(g).keystate[size_t(vk)] >> 7 : 0);
	});
	def("key_released", [&g](NativeCall&) {  // @9f39a0: needs prevFrameKeystate (the renderer's previous frame)
		if (mp(g)) return make_int(-1);
		return make_int(0);
	});

	// ---- player-profile XML files (open_file @9fe080 .. file_sort_entries @9fe500)
	auto close_xml = [&h]() {  // close_file @a034e0
		h.xml_elements.clear();
		h.xml_curr_entry.clear();
		h.xml_curr_index = 0;
		h.xml_curr.reset();
		h.xml_parent.reset();
		h.xml_root.reset();
	};
	auto xml_valid = [&h]() { return h.xml_root && h.xml_curr; };  // check_xml_valid
	def("close_file", [close_xml](NativeCall&) {
		close_xml();
		return make_int(0);
	});
	def("open_file", [&h, close_xml](NativeCall& c) {
		close_xml();
		const std::string key = lower(u8(c.arg_str(0)));
		auto& doc = h.xml_files[key];
		if (!doc) doc = std::make_shared<ScriptHost::XmlNode>();  // XML::init: a new document when the file is absent
		h.xml_root = doc;
		h.xml_parent = doc;
		h.xml_curr = doc;
		return make_int(1);
	});
	def("file_set_category", [&h, xml_valid](NativeCall& c) {
		if (!xml_valid()) return make_int(-1);
		const U16 name = c.arg_str(0);
		if (name.empty()) return make_int(-1);
		std::shared_ptr<ScriptHost::XmlNode> e;
		const std::string n8 = u8(name);
		if (c.arg_int(1) == 0) {  // XMLNode::add_element
			e = std::make_shared<ScriptHost::XmlNode>();
			e->name = n8;
			h.xml_curr->children.push_back(e);
		} else if (bhs::ieq(name, h.xml_curr_entry) && h.xml_curr_index < int32_t(h.xml_elements.size())) {
			e = h.xml_elements[size_t(h.xml_curr_index)];
		} else {  // XMLNode::get_element(name, create)
			for (auto& ch : h.xml_curr->children) {
				if (bhs::ieq(bhs::to_u16(ch->name), name)) {
					e = ch;
					break;
				}
			}
			if (!e) {
				e = std::make_shared<ScriptHost::XmlNode>();
				e->name = n8;
				h.xml_curr->children.push_back(e);
			}
		}
		h.xml_parent = h.xml_curr;
		h.xml_curr = e;
		h.xml_elements.clear();
		h.xml_curr_entry.clear();
		h.xml_curr_index = 0;
		return make_int(1);
	});
	def("file_write_attrib", [&h, xml_valid](NativeCall& c) {
		if (!xml_valid()) return make_int(-1);
		const U16 a = c.arg_str(0), v = c.arg_str(1);
		for (auto& kv : h.xml_curr->attribs) {
			if (bhs::ieq(kv.first, a)) {
				kv.second = v;
				return make_int(1);
			}
		}
		h.xml_curr->attribs.push_back({a, v});
		return make_int(1);
	});
	def("file_read_attrib", [&h, xml_valid](NativeCall& c) {  // from the current entry of the element list
		const U16 a = c.arg_str(0), dflt = c.arg_str(1);
		if (!xml_valid() || h.xml_elements.empty() || h.xml_curr_index >= int32_t(h.xml_elements.size()))
			return make_string(dflt);
		for (const auto& kv : h.xml_elements[size_t(h.xml_curr_index)]->attribs) {
			if (bhs::ieq(kv.first, a)) return make_string(kv.second);
		}
		return make_string(dflt);
	});
	def("file_next_entry", [&h, xml_valid](NativeCall&) {
		if (xml_valid() && !h.xml_elements.empty() && h.xml_curr_index + 1 < int32_t(h.xml_elements.size())) {
			h.xml_curr_index++;
			return make_int(1);
		}
		return make_int(-1);
	});
	def("file_sort_entries", [&h, xml_valid](NativeCall& c) {
		// sort_element_list @a02bb0: the children named `elem` re-appended in the order of get_attrib_num(attrib, -1)
		// (ordered_insert: equal keys keep their order; descending walks the list from the tail).
		if (!xml_valid()) return make_int(-1);
		const U16 elem = c.arg_str(0), attrib = c.arg_str(1);
		const bool desc = c.arg_int(2) != 0;
		auto& ch = h.xml_curr->children;
		std::vector<std::pair<int32_t, std::shared_ptr<ScriptHost::XmlNode>>> sel;
		std::vector<std::shared_ptr<ScriptHost::XmlNode>> rest;
		for (auto& e : ch) {
			if (!bhs::ieq(bhs::to_u16(e->name), elem)) {
				rest.push_back(e);
				continue;
			}
			int32_t key = -1;
			for (const auto& kv : e->attribs) {
				if (bhs::ieq(kv.first, attrib)) key = bhs::convert_int(kv.second);
			}
			size_t at = 0;
			while (at < sel.size() && sel[at].first <= key) at++;
			sel.insert(sel.begin() + long(at), {key, e});
		}
		if (desc) std::reverse(sel.begin(), sel.end());
		ch = rest;
		for (auto& s : sel) ch.push_back(s.second);
		h.xml_curr_entry = elem;
		h.xml_curr_index = 0;
		h.xml_elements.clear();
		for (auto& e : ch) {
			if (bhs::ieq(bhs::to_u16(e->name), elem)) h.xml_elements.push_back(e);
		}
		return make_int(1);
	});

	// ---- time, profile, game control
	def("real_time_millis", [&g](NativeCall&) {  // @9ead50: Game::graphic_clock
		if (mp(g)) return make_int(-1);
		const ScriptInput& in = input(g);
		return make_int(in.attached && in.graphic_clock >= 0 ? in.graphic_clock : int32_t(int64_t(g.frame) * 1000 / 15));
	});
	def("get_player_profile_name", [&g](NativeCall&) { return make_string(bhs::to_u16(input(g).player_profile_name)); });
	def("speed_control_disable", [&g](NativeCall&) {
		pres(g).speed_control_disabled = true;
		return make_int(1);
	});
	def("pause_disable", [&g](NativeCall&) {
		pres(g).pause_disabled = true;
		return make_int(1);
	});
	def("pause_game", [&g](NativeCall&) {  // @9e5b70 (TurnControl pause; the renderer stops stepping)
		if (!mp(g)) set_pause(g, true);
		return VP();
	});
	def("unpause_game", [&g](NativeCall&) {
		if (!mp(g)) set_pause(g, false);
		return VP();
	});
	def("take_screenshot", [&g](NativeCall& c) {
		push_event(g, ScriptEvent::kScreenshot).text = u8(c.arg_str(0));
		return make_int(1);
	});
	def("set_unit_scale", [&g](NativeCall& c) {  // @9eabe0: the unit's graphics piece scale
		const int32_t w = c.arg_int(0) - 1, o = c.arg_int(1);
		const float s = c.arg_float(2);
		if (!(s > 0.f) || !valid_unit_slot(g, w, o)) return make_int(-1);
		const Unit* u = unit_at(g, w, o);
		const int32_t piece = u && !u->guys.empty() ? u->guys[0].gpiece : -1;
		if (piece < 0) return make_int(-1);
		pres(g).unit_scale[piece] = s;
		return make_int(1);
	});
	def("get_unit_scale", [&g](NativeCall& c) {  // @9eac70 (1.0 for a piece never scaled)
		const int32_t w = c.arg_int(0) - 1, o = c.arg_int(1);
		if (!valid_unit_slot(g, w, o)) return make_float(-1.f);
		const Unit* u = unit_at(g, w, o);
		const int32_t piece = u && !u->guys.empty() ? u->guys[0].gpiece : -1;
		if (piece < 0) return make_float(-1.f);
		auto it = pres(g).unit_scale.find(piece);
		return make_float(it == pres(g).unit_scale.end() ? 1.f : it->second);
	});
	// MessageWin's CTW objective list (add_ctw_objective with display 1, then set_ctw_objective_sound; end_ctw_objective):
	// the renderer gets them in the same ScriptPresentation::objectives list.
	def("ctw_add_objective", [add_obj](NativeCall& c) { return add_obj(c, true); });  // @a019e0
	def("ctw_objective_complete", [&g](NativeCall& c) {  // @a01b00
		const U16 id = c.arg_str(0);
		for (ScriptObjective& o : pres(g).objectives) {
			if (id.empty() || bhs::ieq(bhs::to_u16(o.id), id)) o.completed = g.frame > 0 ? g.frame : 1;
		}
		return make_int(1);
	});
}

}  // namespace sf
}  // namespace ron::sim
