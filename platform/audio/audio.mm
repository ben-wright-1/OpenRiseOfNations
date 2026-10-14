#include "audio.hpp"

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace ron::audio {

struct Audio::Impl {
	AVAudioPlayer* voice = nil;
	AVAudioPlayer* music = nil;
	AVAudioPlayer* fading = nil;  // the old track during a cross-fade
	NSMutableArray<AVAudioPlayer*>* effects = [NSMutableArray array];
	NSMutableDictionary<NSString*, NSData*>* cache = [NSMutableDictionary dictionary];  // effect files

	AVAudioPlayer* make(const std::string& path, bool cache_data) {
		NSString* p = [NSString stringWithUTF8String:path.c_str()];
		if (!p) return nil;
		NSError* err = nil;
		AVAudioPlayer* pl = nil;
		if (cache_data) {
			NSData* d = cache[p];
			if (!d) {
				d = [NSData dataWithContentsOfFile:p];
				if (!d) return nil;
				cache[p] = d;
			}
			pl = [[AVAudioPlayer alloc] initWithData:d error:&err];
		} else {
			pl = [[AVAudioPlayer alloc] initWithContentsOfURL:[NSURL fileURLWithPath:p] error:&err];
		}
		if (!pl) {
			std::fprintf(stderr, "audio: %s: %s\n", path.c_str(), err ? err.localizedDescription.UTF8String : "cannot open");
			return nil;
		}
		[pl prepareToPlay];
		return pl;
	}
};

static float env_volume(const char* name, float dflt) {
	const char* v = std::getenv(name);
	return v ? std::clamp(float(std::atof(v)), 0.0f, 1.0f) : dflt;
}

Audio::Audio() : impl_(std::make_unique<Impl>()) {
	muted_ = std::getenv("RON_MUTE") && std::atoi(std::getenv("RON_MUTE")) != 0;
	fx_vol_ = env_volume("RON_SFX_VOL", 1.0f);
	voice_vol_ = env_volume("RON_VOICE_VOL", 1.0f);
	music_vol_ = env_volume("RON_MUSIC_VOL", 0.6f);
}

Audio::~Audio() {
	[impl_->voice stop];
	[impl_->music stop];
	[impl_->fading stop];
}

void Audio::set_volumes(float effects, float voice, float music) {
	fx_vol_ = effects, voice_vol_ = voice, music_vol_ = music;
	if (impl_->voice) impl_->voice.volume = voice_vol_;
	if (impl_->music) impl_->music.volume = music_vol_;
}

bool Audio::play_voice(const std::string& abs_path) {
	stop_voice();
	if (muted_) return false;
	impl_->voice = impl_->make(abs_path, false);
	if (!impl_->voice) return false;
	impl_->voice.volume = voice_vol_;
	return [impl_->voice play];
}

void Audio::stop_voice() {
	[impl_->voice stop];
	impl_->voice = nil;
}

bool Audio::voice_playing() const { return impl_->voice && impl_->voice.playing; }

void Audio::play_effect(const std::string& abs_path, float volume) {
	if (muted_) return;
	// Drop finished players first (at most a handful are alive at once).
	NSIndexSet* done = [impl_->effects indexesOfObjectsPassingTest:^BOOL(AVAudioPlayer* p, NSUInteger, BOOL*) { return !p.playing; }];
	[impl_->effects removeObjectsAtIndexes:done];
	if (impl_->effects.count > 16) return;
	AVAudioPlayer* p = impl_->make(abs_path, true);
	if (!p) return;
	p.volume = fx_vol_ * volume;
	[p play];
	[impl_->effects addObject:p];
}

void Audio::set_music(const std::map<std::string, std::vector<std::string>>& moods, const std::string& win, const std::string& lose) {
	moods_ = moods;
	win_ = win;
	lose_ = lose;
}

void Audio::start_track(const std::string& path, double fade_ms) {
	AVAudioPlayer* next = impl_->make(path, false);
	if (!next) return;
	[impl_->fading stop];
	impl_->fading = nil;
	if (impl_->music && impl_->music.playing && fade_ms >= 10) {
		// JukeBox::play: the new track fades in while the old one fades out over min(5000 ms, its remaining time).
		impl_->fading = impl_->music;
		[impl_->fading setVolume:0 fadeDuration:fade_ms / 1000.0];
		next.volume = 0;
		[next play];
		[next setVolume:music_vol_ fadeDuration:fade_ms / 1000.0];
	} else {
		[impl_->music stop];
		next.volume = music_vol_;
		[next play];
	}
	impl_->music = next;
	track_ = path;
	track_started_ = now_;
}

// JukeBox::shuffle: a random track of the (next) mood, not the current one again when the mood has more than one.
void Audio::shuffle(bool force) {
	if (muted_ || !music_on_ || special_) return;
	auto it = moods_.find(next_mood_);
	if (it == moods_.end() || it->second.empty()) return;
	const auto& list = it->second;
	mood_ = next_mood_;
	std::string pick;
	for (int tries = 0; tries < 8; tries++) {
		rng_ = rng_ * 1103515245u + 12345u;
		pick = list[(rng_ >> 16) % list.size()];
		if (list.size() < 2 || pick != track_) break;
	}
	double fade = 0;
	if (impl_->music && impl_->music.playing) {
		fade = std::min(5000.0, (impl_->music.duration - impl_->music.currentTime) * 1000.0);
		if (!force) return;
	}
	start_track(pick, fade);
}

void Audio::music_start() {
	if (std::getenv("RON_NO_MUSIC")) return;
	music_on_ = true;
	srand(unsigned(now_));
	rng_ ^= unsigned(std::rand());
	shuffle(true);
}

void Audio::set_music_mood(const std::string& mood) {
	if (mood == next_mood_ && mood == mood_) return;
	next_mood_ = mood;
	// JukeBox::set_next_mood: switch now only when the playing track started more than 30 s ago.
	if (now_ - track_started_ > 29999) shuffle(true);
}

void Audio::music_win() {
	special_ = true;
	if (!win_.empty() && !muted_) start_track(win_, 0);
}

void Audio::music_lose() {
	special_ = true;
	if (!lose_.empty() && !muted_) start_track(lose_, 0);
}

void Audio::update(double now_ms) {
	now_ = now_ms;
	if (impl_->fading && !impl_->fading.playing) impl_->fading = nil;
	if (impl_->fading && impl_->fading.volume <= 0.001f) {
		[impl_->fading stop];
		impl_->fading = nil;
	}
	if (!music_on_ || special_ || muted_) return;
	const bool playing = impl_->music && impl_->music.playing;
	if (!playing) {
		if (track_ended_ < track_started_) track_ended_ = now_;
		if (now_ - track_ended_ >= 1000) shuffle(true);  // JukeBox::timing
	}
}

}  // namespace ron::audio
