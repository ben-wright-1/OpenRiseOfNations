// What the front end needs to find and remember the game, and the maps it offers (see launcher.mm). The menu itself is
// drawn in the game window with the install's own art (frontend.h).
#pragma once

#import <Cocoa/Cocoa.h>

#include <string>
#include <vector>

namespace launcher {

// NSUserDefaults keys.
extern NSString* const kDefGameDir;     // the install folder
extern NSString* const kDefDifficulty;  // AI difficulty 0..5 (Easiest .. Toughest)
extern NSString* const kDefScreen;      // the last front-end screen (main, tutorials, skirmish, scenarios)

// A folder looks like an install when it has the game's Data and scenario folders.
bool looks_like_install(NSString* dir);
// Likely install locations: the SteamCMD download (the README's route), CrossOver bottles, a Steam library. "" if none.
NSString* guess_install();
// The remembered folder when it still looks like an install, else guess_install().
NSString* saved_or_guessed_install();
void remember_install(NSString* dir);

// The first-run picker (no install found): a short native explanation, then an NSOpenPanel until the user picks a
// folder that looks like an install or gives up (nil). Call only once the app has finished launching.
NSString* ask_for_install(NSString* reason);
// "Change…": an NSOpenPanel; nil when cancelled. Shows an alert and returns nil when the folder is not an install.
NSString* choose_install(NSString* current);

struct Entry {
	std::string title;   // shown in the list
	std::string detail;  // second line
	std::string path;    // scenario path relative to the install
};
enum class Mode { Tutorials, Skirmish, Scenarios };
std::vector<Entry> entries_for(Mode m, NSString* dir);

}  // namespace launcher
