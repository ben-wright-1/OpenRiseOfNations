// The computer opponent's data (docs/ai-spec.md): the personality, candidate city sites, the make list and the AI
// armies. Plain records with the original's fields (PDB names; offsets in the comments). The functions are in
// sim/ai.hpp; the LeaderData census fields live in Leader (sim/game.hpp).
#pragma once

#include <array>
#include <cstdint>

namespace ron::sim {

// Personality (LeaderData +0x6dd4, 24 ints; ai-spec 2.3). Values are -1..1 (arms -2..2).
struct Personality {
	int32_t rush = 0;            // +0x00 boom_vs_rush
	int32_t cities = 0;          // +0x04
	int32_t upgrades = 0;        // +0x08 (never set by the draws)
	int32_t arms = 0;            // +0x0c
	int32_t army = 0;            // +0x10
	int32_t army_size = 0;       // +0x14
	int32_t raid = 0;            // +0x18
	int32_t invade = 0;          // +0x1c
	int32_t target = 0;          // +0x20
	int32_t strategy = 0;        // +0x24
	int32_t raze = 0;            // +0x28
	int32_t spells = 0;          // +0x2c
	int32_t forts = 0;           // +0x30
	int32_t nukes = 0;           // +0x34
	int32_t air = 0;             // +0x38
	int32_t naval = 0;           // +0x3c
	int32_t market = 0;          // +0x40
	int32_t scouts = 0;          // +0x44
	int32_t civilians = 0;       // +0x48
	int32_t early_army = 0;      // +0x4c
	int32_t friendly_human = 0;  // +0x50
	int32_t alliance_human = 0;  // +0x54
	int32_t friendly_ai = 0;     // +0x58
	int32_t alliance_ai = 0;     // +0x5c
};

// Site (0x18; LeaderData +0x6e34 Sites, 10 entries; ai-spec 5).
struct Site {
	int32_t wx = 0, wy = 0, val = 0, reg = 0, dist = 0, rank = 0;
};

// MakeObject (0x28; MakeList at LeaderData +0x6ec8, 11 entries; ai-spec 6.0). MakeList::clear values.
struct MakeObject {
	int32_t t = -1, val = -1, escrow = 0, city = -1, up = 0, o = -1, num = 1, cat = 0, wx = 0, wy = 0;
};

// ArmyStatus bits (ai-spec 12.1).
namespace armystat {
constexpr int32_t kMustering = 1, kMarching = 2, kOffense = 4, kIdle = 8, kFormup = 0x10, kDefending = 0x20,
                  kNeedTransport = 0x40, kForceProcess = 0x80;
}

// ArmyData (0x98; Armies.lists[who], 16 slots per player from Armies::init; ai-spec 12.1).
struct Army {
	int16_t valid = 0, army = 0;
	int32_t status = 0, reg = 0, role = 0, num_units = 0, num_captains = 0, num_standard = 0, num_decoys = 0;
	int32_t city = 0, navy = 0, human_frame = 0, hurry = 0, target_o = 0, target_who = 0;
	int32_t x = 0, y = 0, angle = 0, rally_dist = 0, muster_x = 0, muster_y = 0;
	uint32_t muster_angle = 0;
	std::array<int32_t, 16> list{};
	int16_t who = 0, num_groups = 0;
};

}  // namespace ron::sim
