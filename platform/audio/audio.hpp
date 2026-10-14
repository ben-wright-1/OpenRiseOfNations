// The sound layer of ronmap (docs/ui-port.md "Audio"): AVFoundation players for the install's WAV files, standing in
// for the original's SoundSys / SoundGlobal / JukeBox (DirectSound streams). Objective-C++ implementation, a plain C++
// interface; include from .mm files.
//
// Channels:
//   - voice: the scenario sound channel (ScenarioData::sound: play_sound / play_objective_sound / stop_sound). One sound
//     at a time; a new one stops the old one, as ScenarioData::sound is closed and reopened.
//   - effects: fire-and-forget one-shots (pings, interface clicks, unit acknowledgements).
//   - music: the JukeBox. Tracks per mood from Data/sound.xml (TRACKS/TRIBE "default"/AGE 0/MOOD: BATTLE_VICTORY =
//     "winning", BATTLE_DEFEAT = "losing", ECONOMIC = "economic"), WIN / LOSE tracks. Behaviour from JukeBox::shuffle /
//     play / set_next_mood (re/decomp/scriptui_render): a random track of the current mood, never the same one twice in
//     a row; the next one starts JukeBox::timing = 1000 ms after the last ended; a track change cross-fades over
//     min(5000 ms, what is left of the old track); set_music_mood switches at once only when the current track started
//     more than 30 s ago, else the new mood applies to the next track; the starting mood is ECONOMIC.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ron::audio {

class Audio {
public:
	Audio();
	~Audio();
	// Volumes 0..1 (the profile's sound / music sliders; RON_SFX_VOL / RON_MUSIC_VOL override, RON_MUTE=1 silences).
	void set_volumes(float effects, float voice, float music);
	bool muted() const { return muted_; }

	// The scenario channel. Returns false when the file cannot be played.
	bool play_voice(const std::string& abs_path);
	void stop_voice();
	bool voice_playing() const;

	// One-shot effect.
	void play_effect(const std::string& abs_path, float volume = 1.0f);

	// Music. `moods` maps "winning" / "losing" / "economic" to absolute track paths.
	void set_music(const std::map<std::string, std::vector<std::string>>& moods, const std::string& win, const std::string& lose);
	void music_start();
	void set_music_mood(const std::string& mood);
	void music_win();
	void music_lose();
	// Called every rendered frame with a monotonic clock in ms (track ends, cross-fades, queued moods).
	void update(double now_ms);
	const std::string& current_mood() const { return mood_; }
	const std::string& current_track() const { return track_; }

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
	bool muted_ = false;
	float fx_vol_ = 1, voice_vol_ = 1, music_vol_ = 0.6f;
	std::string mood_ = "economic", next_mood_ = "economic", track_;
	std::map<std::string, std::vector<std::string>> moods_;
	std::string win_, lose_;
	bool music_on_ = false, special_ = false;
	double now_ = 0, track_started_ = -1e9, track_ended_ = -1e9;
	unsigned rng_ = 0x62766u;
	void start_track(const std::string& path, double fade_ms);
	void shuffle(bool force);
};

}  // namespace ron::audio
