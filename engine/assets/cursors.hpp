// Mouse cursors (docs/ui-spec.md section 7): Data/cursors.xml rows; the row index is the id for Cursors::set (id -1
// shows row 0). A negative x / y selects a cell of a strip of (size + 1)-pixel cells: x = 1 - (xs + 1) * x
// (Cursors::init @007d0270). Reference: re/tools/cursors.mjs.
#pragma once

#include <string>
#include <vector>

#include "assets/files.hpp"

namespace ron {

struct CursorRow {
	std::string name, file;  // file: game-relative TGA path
	int x = 0, y = 0, xs = 0, ys = 0, hot_x = 0, hot_y = 0;
	int gdi_id = 0, bw_id = 0;
};

std::vector<CursorRow> load_cursors(const GameFiles& files, const std::string& xml = "Data/cursors.xml");

// Cursor ids used by the in-game console (cursors.xml rows).
enum CursorId : int {
	kCursorMain = 0,
	kCursorGarrison = 7,
	kCursorRepair = 9,
	kCursorAttack = 14,
	kCursorBuild = 16,
	kCursorNoAttack = 27,
	kCursorAttackTo = 35,
	kCursorMoveTo = 36,
	kCursorWait = 67,
};

}  // namespace ron
