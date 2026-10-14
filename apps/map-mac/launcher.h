// The start-up window of the app (game folder, scenario, AI difficulty). See launcher.mm.
#pragma once

#import <Cocoa/Cocoa.h>

// Runs the launcher modally (NSApplication must exist). Returns false when the user quits.
bool run_launcher(NSString* __autoreleasing* gameDir, NSString* __autoreleasing* scenario, int* difficulty);
