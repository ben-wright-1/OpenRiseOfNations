// The start-up menu shown when the app is opened without a scenario argument (double-clicked from Finder): Tutorials,
// Skirmish, Scenarios and (not yet available) Multiplayer, each opening a list of maps; the game folder (the user's
// own Rise of Nations: Extended Edition install) is chosen at the bottom. Choices are remembered in NSUserDefaults.
#import "launcher.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

NSString* const kDefGameDir = @"GameDirectory";
NSString* const kDefDifficulty = @"Difficulty";

// A folder looks like an install when it has the game's data and scenarios.
BOOL looks_like_install(NSString* dir) {
	if (dir.length == 0) return NO;
	NSFileManager* fm = NSFileManager.defaultManager;
	BOOL isDir = NO;
	if (![fm fileExistsAtPath:dir isDirectory:&isDir] || !isDir) return NO;
	BOOL data = NO, scen = NO;
	for (NSString* e in [fm contentsOfDirectoryAtPath:dir error:nil]) {
		if ([e caseInsensitiveCompare:@"Data"] == NSOrderedSame) data = YES;
		if ([e caseInsensitiveCompare:@"scenario"] == NSOrderedSame) scen = YES;
	}
	return data && scen;
}

// Likely install locations: SteamCMD download (the README's route), CrossOver / Wine bottles, a Steam library.
NSString* guess_install() {
	NSString* home = NSHomeDirectory();
	NSMutableArray<NSString*>* candidates = [NSMutableArray arrayWithObjects:
	    [home stringByAppendingPathComponent:@"Games/RiseOfNations"],
	    [home stringByAppendingPathComponent:@"Games/Rise of Nations"],
	    [home stringByAppendingPathComponent:@"Library/Application Support/Steam/steamapps/common/Rise of Nations"], nil];
	NSString* bottles = [home stringByAppendingPathComponent:@"Library/Application Support/CrossOver/Bottles"];
	for (NSString* b in [NSFileManager.defaultManager contentsOfDirectoryAtPath:bottles error:nil] ?: @[]) {
		[candidates addObject:[NSString pathWithComponents:@[ bottles, b, @"drive_c/Program Files (x86)/Steam/steamapps/common/Rise of Nations" ]]];
	}
	for (NSString* c in candidates) {
		if (looks_like_install(c)) return c;
	}
	return @"";
}

// The path of `rel` inside the install, matching folder and file names case-insensitively; nil if missing.
NSString* find_in_install(NSString* dir, NSString* rel) {
	NSString* cur = dir;
	for (NSString* part in [rel componentsSeparatedByString:@"/"]) {
		NSString* match = nil;
		for (NSString* e in [NSFileManager.defaultManager contentsOfDirectoryAtPath:cur error:nil]) {
			if ([e caseInsensitiveCompare:part] == NSOrderedSame) {
				match = e;
				break;
			}
		}
		if (!match) return nil;
		cur = [cur stringByAppendingPathComponent:match];
	}
	return [cur substringFromIndex:dir.length + 1];
}

struct Entry {
	NSString* title;   // shown in the list
	NSString* detail;  // second line
	NSString* path;    // scenario path relative to the install
};

enum class Mode { Tutorials, Skirmish, Scenarios };

std::vector<Entry> entries_for(Mode m, NSString* dir) {
	std::vector<Entry> out;
	auto add = [&](NSString* title, NSString* detail, NSString* rel) {
		if (NSString* p = find_in_install(dir, rel)) out.push_back({title, detail, p});
	};
	if (m == Mode::Tutorials) {
		add(@"Tutorial 1: Getting around", @"Select and move units, scroll and zoom the map.", @"scenario/Tutorial1/tut1.scx");
		add(@"Tutorial 2: Building", @"Citizens, military buildings and building in groups.", @"scenario/Tutorial2/tut2.scx");
		add(@"Tutorial 3: Combat", @"Lead your army against a city and its defenders.", @"scenario/Tutorial3/tut3.scx");
		add(@"Tutorial 4: Cities", @"The alarm, the library and founding new cities.", @"scenario/Tutorial4/tut4.scx");
		add(@"Tutorial 5: Growing your nation", @"Gather resources, build farms and advance an age.", @"scenario/Tutorial5/tut5.scx");
		add(@"Tutorial 6: Diplomacy", @"War, peace and alliances.", @"scenario/Tutorial6/tut6.scx");
	} else if (m == Mode::Skirmish) {
		add(@"Skirmish", @"Ready-made map with computer opponents.", @"scenario/Custom/skirmish/skirmish.scx");
		add(@"Caribbean", @"Islands and coasts.", @"scenario/Custom/caribbean/caribbean.scx");
		add(@"Europe", @"Eight nations across Europe.", @"scenario/Custom/europe/europe.scx");
		add(@"Italy", @"A crowded peninsula.", @"scenario/Custom/italy_mp/italy_mp.scx");
		add(@"Capture", @"Take the enemy's cities.", @"scenario/Custom/capture/capture.scx");
		add(@"Missile", @"Modern-age warfare.", @"scenario/Custom/missile/missile.scx");
	} else {
		struct { NSString *file, *title; } chess[] = {
		    {@"ceaging1", @"Chess Exercise: Ages 1"}, {@"ceaging2", @"Chess Exercise: Ages 2"},
		    {@"ceaging3", @"Chess Exercise: Ages 3"}, {@"cegeneral3", @"Chess Exercise: General"},
		    {@"cehotkey1", @"Chess Exercise: Hotkeys"}, {@"cemicrowars", @"Chess Exercise: Micro Wars"},
		    {@"ceraiding1", @"Chess Exercise: Raiding"}, {@"cewonder", @"Chess Exercise: Wonder"}};
		for (const auto& c : chess)
			add(c.title, @"A short challenge.", [NSString stringWithFormat:@"scenario/Chess Exercise/%@.scx", c.file]);
		// Conquer the World battle maps, played on their own (the campaign map is not supported yet).
		NSString* conquest = find_in_install(dir, @"conquest");
		if (conquest) {
			NSString* root = [dir stringByAppendingPathComponent:conquest];
			NSMutableArray<NSString*>* maps = [NSMutableArray array];
			for (NSString* rel in [NSFileManager.defaultManager enumeratorAtPath:root]) {
				if ([rel.pathExtension caseInsensitiveCompare:@"scx"] == NSOrderedSame) [maps addObject:rel];
			}
			[maps sortUsingSelector:@selector(caseInsensitiveCompare:)];
			for (NSString* rel in maps) {
				NSString* campaign = rel.pathComponents.firstObject;
				out.push_back({[NSString stringWithFormat:@"%@: %@", campaign, rel.lastPathComponent.stringByDeletingPathExtension],
				               @"Campaign battle (experimental: some end early without the campaign map).",
				               [conquest stringByAppendingPathComponent:rel]});
			}
		}
	}
	return out;
}

}  // namespace

@interface RONLauncher : NSObject <NSWindowDelegate, NSTableViewDataSource, NSTableViewDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) NSView* menuPage;
@property(nonatomic, strong) NSView* listPage;
@property(nonatomic, strong) NSTextField* pathField;
@property(nonatomic, strong) NSTextField* status;
@property(nonatomic, strong) NSMutableArray<NSButton*>* modeButtons;
@property(nonatomic, strong) NSTextField* listTitle;
@property(nonatomic, strong) NSTextField* listNote;
@property(nonatomic, strong) NSTableView* table;
@property(nonatomic, strong) NSPopUpButton* difficultyMenu;
@property(nonatomic, strong) NSTextField* difficultyLabel;
@property(nonatomic, strong) NSButton* playButton;
@property(nonatomic) BOOL accepted;
@end

@implementation RONLauncher {
	std::vector<Entry> _entries;
	Mode _mode;
}

static const CGFloat kW = 640, kH = 460;

- (NSTextField*)label:(NSString*)text in:(NSView*)v frame:(NSRect)r size:(CGFloat)size bold:(BOOL)bold {
	NSTextField* t = [NSTextField labelWithString:text];
	t.frame = r;
	t.font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
	t.lineBreakMode = NSLineBreakByWordWrapping;
	t.maximumNumberOfLines = 0;
	[v addSubview:t];
	return t;
}

- (NSButton*)modeButton:(NSString*)title note:(NSString*)note y:(CGFloat)y tag:(NSInteger)tag enabled:(BOOL)enabled {
	NSButton* b = [NSButton buttonWithTitle:title target:self action:@selector(openMode:)];
	b.frame = NSMakeRect(170, y, 300, 44);
	b.font = [NSFont boldSystemFontOfSize:16];
	b.bezelStyle = NSBezelStyleRegularSquare;
	b.tag = tag;
	b.enabled = enabled;
	[self.menuPage addSubview:b];
	NSTextField* n = [self label:note in:self.menuPage frame:NSMakeRect(170, y - 18, 300, 16) size:11 bold:NO];
	n.alignment = NSTextAlignmentCenter;
	n.textColor = NSColor.secondaryLabelColor;
	[self.modeButtons addObject:b];
	return b;
}

- (instancetype)init {
	if (!(self = [super init])) return nil;
	self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, kW, kH)
	                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
	                                            backing:NSBackingStoreBuffered
	                                              defer:NO];
	self.window.title = @"OpenRiseOfNations";
	self.window.delegate = self;
	self.window.releasedWhenClosed = NO;
	self.modeButtons = [NSMutableArray array];

	// ---- main menu
	self.menuPage = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, kW, kH)];
	NSTextField* title = [self label:@"OpenRiseOfNations" in:self.menuPage frame:NSMakeRect(20, 396, kW - 40, 36) size:28 bold:YES];
	title.alignment = NSTextAlignmentCenter;
	NSTextField* sub = [self label:@"An open-source engine for Rise of Nations: Extended Edition"
	                            in:self.menuPage
	                         frame:NSMakeRect(20, 372, kW - 40, 20)
	                          size:13
	                          bold:NO];
	sub.alignment = NSTextAlignmentCenter;
	sub.textColor = NSColor.secondaryLabelColor;
	[self modeButton:@"Tutorials" note:@"Learn to play, step by step." y:312 tag:int(Mode::Tutorials) enabled:YES];
	[self modeButton:@"Skirmish" note:@"Battle computer opponents on a ready-made map." y:242 tag:int(Mode::Skirmish) enabled:YES];
	[self modeButton:@"Scenarios" note:@"Chess exercises and campaign battles." y:172 tag:int(Mode::Scenarios) enabled:YES];
	[self modeButton:@"Multiplayer" note:@"Not available yet." y:102 tag:-1 enabled:NO];

	[self label:@"Game folder" in:self.menuPage frame:NSMakeRect(20, 54, 90, 18) size:11 bold:YES];
	self.pathField = [NSTextField textFieldWithString:@""];
	self.pathField.frame = NSMakeRect(110, 50, 400, 22);
	self.pathField.editable = NO;
	self.pathField.selectable = YES;
	self.pathField.font = [NSFont systemFontOfSize:11];
	[self.menuPage addSubview:self.pathField];
	NSButton* choose = [NSButton buttonWithTitle:@"Change…" target:self action:@selector(choose:)];
	choose.frame = NSMakeRect(516, 46, 104, 28);
	[self.menuPage addSubview:choose];
	self.status = [self label:@"" in:self.menuPage frame:NSMakeRect(20, 12, 500, 32) size:11 bold:NO];
	NSButton* quit = [NSButton buttonWithTitle:@"Quit" target:self action:@selector(quit:)];
	quit.frame = NSMakeRect(536, 10, 84, 28);
	quit.keyEquivalent = @"q";
	quit.keyEquivalentModifierMask = NSEventModifierFlagCommand;
	[self.menuPage addSubview:quit];

	// ---- map list
	self.listPage = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, kW, kH)];
	self.listTitle = [self label:@"" in:self.listPage frame:NSMakeRect(20, 412, kW - 40, 30) size:22 bold:YES];
	self.listNote = [self label:@"" in:self.listPage frame:NSMakeRect(20, 380, kW - 40, 30) size:11 bold:NO];
	self.listNote.textColor = NSColor.secondaryLabelColor;
	NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(20, 64, kW - 40, 310)];
	scroll.hasVerticalScroller = YES;
	scroll.borderType = NSBezelBorder;
	self.table = [[NSTableView alloc] initWithFrame:scroll.bounds];
	NSTableColumn* col = [[NSTableColumn alloc] initWithIdentifier:@"map"];
	col.width = kW - 60;
	[self.table addTableColumn:col];
	self.table.headerView = nil;
	self.table.rowHeight = 38;
	self.table.dataSource = self;
	self.table.delegate = self;
	self.table.target = self;
	self.table.doubleAction = @selector(play:);
	scroll.documentView = self.table;
	[self.listPage addSubview:scroll];
	self.difficultyLabel = [self label:@"AI difficulty" in:self.listPage frame:NSMakeRect(20, 26, 90, 18) size:12 bold:YES];
	self.difficultyMenu = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(110, 20, 160, 26) pullsDown:NO];
	[self.difficultyMenu addItemsWithTitles:@[ @"Easiest", @"Easy", @"Moderate", @"Tough", @"Tougher", @"Toughest" ]];
	[self.listPage addSubview:self.difficultyMenu];
	NSButton* back = [NSButton buttonWithTitle:@"Back" target:self action:@selector(back:)];
	back.frame = NSMakeRect(420, 18, 96, 32);
	back.keyEquivalent = @"\e";
	[self.listPage addSubview:back];
	self.playButton = [NSButton buttonWithTitle:@"Play" target:self action:@selector(play:)];
	self.playButton.frame = NSMakeRect(524, 18, 96, 32);
	self.playButton.keyEquivalent = @"\r";
	[self.listPage addSubview:self.playButton];

	NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
	NSString* dir = [d stringForKey:kDefGameDir];
	if (!looks_like_install(dir)) dir = guess_install();
	[self setGameDir:dir];
	NSInteger diff = [d objectForKey:kDefDifficulty] ? [d integerForKey:kDefDifficulty] : 2;
	[self.difficultyMenu selectItemAtIndex:std::clamp<NSInteger>(diff, 0, 5)];
	self.window.contentView = self.menuPage;
	[self.window center];
	return self;
}

- (void)setGameDir:(NSString*)dir {
	self.pathField.stringValue = dir ?: @"";
	const BOOL ok = looks_like_install(dir);
	for (NSButton* b in self.modeButtons) b.enabled = ok && b.tag >= 0;
	if (ok) {
		self.status.textColor = NSColor.secondaryLabelColor;
		self.status.stringValue = @"Your game files are ready.";
	} else {
		self.status.textColor = NSColor.systemRedColor;
		self.status.stringValue = dir.length ? @"This folder is not a Rise of Nations: Extended Edition install (it needs the "
		                                       @"Data and scenario folders)."
		                                     : @"Choose your Rise of Nations: Extended Edition folder (see the README for how "
		                                       @"to get the game files onto a Mac).";
	}
}

- (void)choose:(id)sender {
	NSOpenPanel* p = [NSOpenPanel openPanel];
	p.canChooseDirectories = YES;
	p.canChooseFiles = NO;
	p.message = @"Choose your Rise of Nations: Extended Edition folder (the one with Data and scenario in it).";
	if ([p runModal] == NSModalResponseOK) {
		[self setGameDir:p.URL.path];
		if (looks_like_install(p.URL.path)) [NSUserDefaults.standardUserDefaults setObject:p.URL.path forKey:kDefGameDir];
	}
}

- (void)openMode:(NSButton*)sender {
	_mode = Mode(sender.tag);
	_entries = entries_for(_mode, self.pathField.stringValue);
	switch (_mode) {
		case Mode::Tutorials:
			self.listTitle.stringValue = @"Tutorials";
			self.listNote.stringValue = @"Start with Tutorial 1. The tutorials guide you with voice-overs, arrows and objectives.";
			break;
		case Mode::Skirmish:
			self.listTitle.stringValue = @"Skirmish";
			self.listNote.stringValue = @"You play the map's human player against computer opponents. Random maps and the full "
			                            @"skirmish setup (nations, teams, map size) are not supported yet.";
			break;
		case Mode::Scenarios:
			self.listTitle.stringValue = @"Scenarios";
			self.listNote.stringValue = @"Chess exercises and the battle maps of the Conquer the World campaigns.";
			break;
	}
	const BOOL ai = _mode != Mode::Tutorials;
	self.difficultyMenu.hidden = !ai;
	self.difficultyLabel.hidden = !ai;
	[self.table reloadData];
	if (!_entries.empty()) [self.table selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
	self.playButton.enabled = !_entries.empty();
	self.window.contentView = self.listPage;
	[self.window makeFirstResponder:self.table];
}

- (void)back:(id)sender {
	self.window.contentView = self.menuPage;
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView {
	return NSInteger(_entries.size());
}

- (NSView*)tableView:(NSTableView*)tableView viewForTableColumn:(NSTableColumn*)column row:(NSInteger)row {
	NSTableCellView* cell = [tableView makeViewWithIdentifier:@"cell" owner:self];
	if (!cell) {
		cell = [[NSTableCellView alloc] initWithFrame:NSMakeRect(0, 0, column.width, 38)];
		cell.identifier = @"cell";
		NSTextField* t = [NSTextField labelWithString:@""];
		t.frame = NSMakeRect(8, 18, column.width - 16, 18);
		t.font = [NSFont boldSystemFontOfSize:13];
		t.tag = 1;
		[cell addSubview:t];
		NSTextField* d = [NSTextField labelWithString:@""];
		d.frame = NSMakeRect(8, 2, column.width - 16, 16);
		d.font = [NSFont systemFontOfSize:11];
		d.textColor = NSColor.secondaryLabelColor;
		d.tag = 2;
		[cell addSubview:d];
	}
	const Entry& e = _entries[size_t(row)];
	[(NSTextField*)[cell viewWithTag:1] setStringValue:e.title];
	[(NSTextField*)[cell viewWithTag:2] setStringValue:e.detail];
	return cell;
}

- (void)play:(id)sender {
	const NSInteger row = self.table.selectedRow;
	if (row < 0 || size_t(row) >= _entries.size()) return;
	self.accepted = YES;
	NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
	[d setObject:self.pathField.stringValue forKey:kDefGameDir];
	if (!self.difficultyMenu.hidden) [d setInteger:self.difficultyMenu.indexOfSelectedItem forKey:kDefDifficulty];
	[NSApp stopModal];
	[self.window orderOut:nil];
}

- (NSString*)chosenScenario {
	const NSInteger row = self.table.selectedRow;
	return row >= 0 && size_t(row) < _entries.size() ? _entries[size_t(row)].path : nil;
}

- (int)chosenDifficulty {
	return self.difficultyMenu.hidden ? -1 : int(self.difficultyMenu.indexOfSelectedItem);
}

- (void)quit:(id)sender {
	self.accepted = NO;
	[NSApp stopModal];
}

- (void)windowWillClose:(NSNotification*)n {
	if (!self.accepted) [NSApp stopModal];
}

@end

// RON_LAUNCHER_SNAPSHOT=out.png[:MODE] renders the menu (or the list of MODE 0 tutorials, 1 skirmish, 2 scenarios)
// offscreen to a PNG and exits: a check that needs no screen capture.
static bool snapshot_launcher(RONLauncher* l, const char* spec) {
	std::string s = spec;
	const size_t colon = s.rfind(':');
	int mode = -1;
	if (colon != std::string::npos && colon + 2 == s.size()) {
		mode = s[colon + 1] - '0';
		s.resize(colon);
	}
	if (mode >= 0) {
		NSButton* b = [NSButton new];
		b.tag = mode;
		[l openMode:b];
	}
	l.window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];  // an opaque, readable capture
	NSView* v = l.window.contentView;
	v.wantsLayer = YES;
	v.layer.backgroundColor = NSColor.windowBackgroundColor.CGColor;
	[l.table reloadData];
	[l.table tile];
	[v layoutSubtreeIfNeeded];
	[l.table enumerateAvailableRowViewsUsingBlock:^(NSTableRowView*, NSInteger) {}];
	for (NSInteger r = 0; r < l.table.numberOfRows; r++) [l.table viewAtColumn:0 row:r makeIfNecessary:YES];
	[v displayIfNeeded];
	NSBitmapImageRep* rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds];
	[v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
	return [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(s.c_str()) atomically:YES];
}

bool run_launcher(NSString* __autoreleasing* gameDir, NSString* __autoreleasing* scenario, int* difficulty) {
	RONLauncher* l = [RONLauncher new];
	if (const char* snap = getenv("RON_LAUNCHER_SNAPSHOT")) {
		std::printf("%s\n", snapshot_launcher(l, snap) ? "launcher snapshot written" : "launcher snapshot failed");
		return false;
	}
	[l.window makeKeyAndOrderFront:nil];
	[NSApp activateIgnoringOtherApps:YES];
	[NSApp runModalForWindow:l.window];
	if (!l.accepted) return false;
	*gameDir = l.pathField.stringValue;
	*scenario = [l chosenScenario];
	const int d = [l chosenDifficulty];
	if (d >= 0) *difficulty = d;
	return true;
}
