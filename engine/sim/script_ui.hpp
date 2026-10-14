// The presentation side of the scenario script builtins (docs/script-spec.md 7.4, docs/sim-implementation.md
// "Scripts"): what the original's MessageWin / Camera / Scene / JukeBox / SoundSys / Popup calls show, kept here as a
// read-only queue and state the renderer consumes, and the seat-local input state (selection, armed command, mouse,
// camera, interface rectangles, sound playback) that the renderer fills for the builtins that read it.
//
// Determinism: the original runs these builtins on every machine inside Game::do_frame. The local reads (selection,
// mouse, camera, keys, sound) are guarded by `semaphore byte0 & 4` (multiplayer) in most bodies, which then return -1
// (script-spec 7.4); a few (get_object_at_mouse, find_selected_id, is_move_selected, get_button_*, get_objectives_*,
// sound playback through play_sound ...) are unguarded and only appear in single-player content. The port keeps the
// same guards. Nothing in ScriptPresentation feeds back into the simulation except where the original does the same
// (the objective list, the goal list and the flags below are ScenarioData / MessageWin state the original saves).
//
// Renderer contract:
//   - read `script_presentation(game)` after each Game::do_frame; consume `events` with your own cursor on `seq`
//     (the queue keeps the newest kMaxEvents); the other fields are current state (objectives, arrows, flags,
//     highlight, locks, camera request, music);
//   - write `*script_input(game)` between frames (never during Game::do_frame) and set `attached = true`; without a
//     renderer the defaults model an idle player (nothing selected, mouse off the map, sounds end after their WAV
//     length at 15 simulation frames per second).
// Coordinates: "tile" = the scripts' tile (192 world units, ScenarioFuncSet's `t * 192 + 96` centre); "world" = sim
// world units (Object x / y).
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ron::sim {

class Game;

// OptionIndex (PDB) values the builtins compare with the armed command (Options::opt.option).
namespace scriptopt {
constexpr int32_t kBuild = 0, kBuildMilitary = 1, kBuildWonder = 2, kPatrol = 8, kMoveTo = 9, kAirstrike = 36,
                  kTrain = 47, kResearch = 48, kGather = 51;
}

// RGBA colours of the original's Color constants the builtins pass (WHITE, BLACK, RED, LIGHT_GREY).
namespace scriptcolor {
constexpr uint32_t kWhite = 0xffffffffu, kBlack = 0x000000ffu, kRed = 0xff0000ffu, kLightGrey = 0xc0c0c0ffu;
}

struct ScriptRect {  // RectTemplate<short> in screen pixels; left == right means "no such rectangle"
	int32_t left = -1, right = -1, top = -1, bottom = -1;
	bool valid() const { return left != right; }
};

// One presentation request, in the order the builtins ran (frame = Game::frame of the run).
struct ScriptEvent {
	enum Kind : uint8_t {
		kGameMessage,      // print_game_msg: MessageWin::add_message (text, color)
		kScenarioMessage,  // print_msg: MessageWin::add_scen_message (text, a = duration in ms, color); also kept as
		                   // ScriptPresentation::scenario_message
		kInstruction,      // set_instruction_msg: MessageWin::add_instructions (text)
		kFeedback,         // set_feedback_msg: MessageWin::add_feedback (text, color RED)
		kChat,             // chat / chat_all to the local player: LeaderOut::display_chat (text, who = from)
		kNotice,           // show_notice: IFaceMainBase::do_notice (text, text2 = image name, who = colour leader)
		kBubble,           // bubble_text_obj: MessageWin::add_bubble_message (text, x / y world)
		kPopup,            // popup_dialog / popup_choice: modal Popup (text, text2..text4 = buttons, a = button count)
		kSound,            // play_sound / play_objective_sound: ScenarioData::sound (text = file name as given,
		                   // text2 = game-relative path, a = length in ms or -1 unknown)
		kStopSound,        // stop_sound
		kMusicMood,        // set_music_mood (text = "winning" / "losing" / "economic", a = immediately)
		kPing,             // Scene::add_ping / Leader::receive_ping (x / y world, who = from, a = 1 flashing)
		kClearPings,       // clear_pings
		kCamera,           // camera move (see ScriptPresentation::camera; a = CameraRequest::kind)
		kClearMessages,    // clear_game_msg
		kGoalDialog,       // show_goal_dialog: ObjectivesDlg::exec
		kScreenshot,       // take_screenshot (text = file name)
		kPause,            // pause_game / popup_choice (a = 1 paused, 0 resumed)
		kVictoryScreen,    // display_results / display_aging_results / save_results (text = builtin name)
	};
	uint64_t seq = 0;
	int32_t frame = 0;
	Kind kind = kGameMessage;
	std::string text, text2, text3, text4;
	int32_t who = -1, x = -1, y = -1, a = 0;
	uint32_t color = scriptcolor::kWhite;
};

// A scenario objective (MessageWin::scen_objectives entry: id, text, sound; add_objective_text: display_check 0).
struct ScriptObjective {
	std::string id, text, sound;
	bool announce = true;     // display_check: add_objective 1, add_objective_text 0
	int32_t x = -1, y = -1;   // set_objective_loc (world), -1 none
	int32_t frame = 0;        // frame added
	int32_t completed = 0;    // objective_complete: the frame (graphic_tick in the original) it was ended, 0 open
	uint32_t color = scriptcolor::kWhite;
};

// A line drawn by draw_screen_arrow_line / draw_*_arrow (ScreenArrowSpline): two control points.
struct ScriptArrow {
	int32_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
	int32_t seconds = 0;  // the builtin's duration argument (0 = until cleared)
	bool world = false;   // false: screen pixels; true: tile coordinates (draw_arrow_* builtins)
	int32_t frame = 0;
};

// The newest camera request (Camera pan_to / set_loc / to_zoom_level writes).
struct CameraRequest {
	enum Kind : int32_t { kNone = 0, kPan = 1, kJump = 2, kZoom = 3, kPanZoom = 4 };
	uint64_t seq = 0;  // the event seq that set it
	int32_t kind = kNone;
	int32_t x = -1, y = -1;  // world target (pan / jump), -1 none
	int32_t zoom = -1;       // Camera::to_zoom_level (4 zoomed out .. 6 zoomed in), -1 unchanged
};

struct ScriptPresentation {
	static constexpr size_t kMaxEvents = 1024;
	std::deque<ScriptEvent> events;
	uint64_t next_seq = 1;
	std::vector<ScriptObjective> objectives;  // MessageWin::scen_objectives in list order
	std::string scenario_message;             // MessageWin::scenario_message (print_msg)
	int32_t scenario_message_frame = -1, scenario_message_ms = 0;
	uint32_t scenario_message_color = scriptcolor::kBlack;
	std::vector<ScriptArrow> arrows;                       // ScreenArrowSpline::arrows
	std::vector<std::array<int32_t, 2>> flags;             // add_flag (tile coordinates)
	// ScenarioData highlight state: hilite_option (OptionIndex; 0x2f train, 0x30 research, 0..2 build menus) and
	// hilite_object (TypeIndex) of highlight_*; highlight_active; highlight_object_* builtins set the object.
	int32_t hilite_option = -1, hilite_object = -1;
	bool highlight_active = false;
	int32_t highlight_who = -1, highlight_o = -1;
	bool display_bubble_text = true;           // ScenarioData::display_bubble_text
	uint32_t scroll_lock_flags = 0, zoom_lock_flags = 0;  // Camera locks (0xfff7 locked; bit 8 unset = still pannable)
	bool ui_hidden = false;                     // ui_hide / ui_show
	bool basic_menu = false;                    // use_basic_menu (player_profile prefs & 2 clear)
	bool mouse_selection_disabled = false, hotkey_selection_disabled = false;  // ScenarioData (saved)
	bool speed_control_disabled = false, pause_disabled = false;              // ScenarioData (saved)
	bool paused = false;                        // TurnControl semaphore byte0 & 1 (pause_game / unpause_game)
	std::string victory_message, defeat_message;  // ScenarioData::victory_message / defeat_message
	CameraRequest camera;
	std::string music_mood;  // JukeBox::next_mood ("winning" / "losing" / "economic")
	std::map<int32_t, float> unit_scale;  // set_unit_scale: GraphicPiece scale per graphics piece
};

// Seat-local input the renderer fills between frames (console.play's SelectGroup, Options, Console, Camera, ...).
struct ScriptInput {
	bool attached = false;  // a renderer fills this struct; false = the headless defaults
	// SelectGroup of console.play: owner (leader index, GroupData.who +0x4a) and member captains (o) in order.
	int32_t select_who = -1;
	std::vector<int32_t> selected;
	// Options: the armed command (opt.option, OptionIndex, -1 none), selecting_spot (waiting for a map click),
	// mode == BUILD_OPTIONS with mode_data (0 build menu, 1 military build menu), opt.object (the TypeIndex chosen).
	int32_t option = -1, option_object = -1, mode_data = 0;
	bool selecting_spot = false, build_options = false;
	// Console: the tile under the cursor (cursor_x / cursor_y, -1 off the map) and whether the cursor is over the
	// interface bar (graphsys->scaled_screen_ys - iface height < select_y2) while the interface is open.
	int32_t cursor_tx = -1, cursor_ty = -1;
	bool cursor_over_iface = false;
	// Screen and interface: Window::screen size, IFaceMain rectangles by lower-case name (GraphicWinXML::find_rect after
	// client_to_screen), MessageWin::scenario_message_rect (the objectives panel).
	int32_t screen_w = 0, screen_h = 0;
	std::map<std::string, ScriptRect> iface_rects;
	ScriptRect objectives_rect{0, 0, 0, 0};
	// Camera: loc_x / loc_y (world), zoom_level (6 closest .. 4 farthest; < 5 is "zoomed out"), panning.
	float camera_x = 0.f, camera_y = 0.f;
	int32_t zoom_level = 5;
	bool camera_panning = false;
	// MiscAccess::world_to_console visibility of a world point (is_on_screen); null = nothing is on screen.
	std::function<bool(int32_t x, int32_t y)> on_screen;
	std::array<uint8_t, 256> keystate{};  // Game::keystate by virtual key (bit 7 = down)
	// ScenarioData::user_warnings (saved in the original, written by the UI's issue code): bit 0 resource warning
	// (GroupOut::issue_queue_up / validate_build could not pay); resource_warning clears it.
	uint32_t user_warnings = 0;
	// ScenarioData::sound playback: -1 = model (finished once its WAV length has elapsed at 15 frames per second),
	// 0 = still playing, 1 = finished.
	int32_t sound_state = -1;
	int32_t graphic_clock = -1;  // Game::graphic_clock in ms for real_time_millis (-1: frame * 1000 / 15)
	std::string player_profile_name;
	// Modal popups (popup_dialog, popup_choice): called synchronously from inside the script run, as the original
	// blocks in BasePop::exec. Returns the chosen button (0-based); null = button 0.
	std::function<int32_t(const ScriptEvent&)> popup;
};

// The game's presentation state / input state (null when the game has no scripts).
const ScriptPresentation* script_presentation(const Game& g);
ScriptInput* script_input(Game& g);

}  // namespace ron::sim
