#pragma once
#include <cstdint>

// Software mixer for IMA ADPCM effects (sounds.pak, kept in RAM) and one streamed music track.
// play/playMusic/stopMusic are called from the game thread, mix() from the audio thread.
namespace audio {

constexpr int OUTPUT_RATE = 44100;
constexpr int MAX_VOICES = 16;
constexpr int MAX_MIX_FRAMES = 1024;

bool loadBank(const char* path);
void freeBank();
int bankBytes();
// Entries of a sound group such as "step.stone": returns the first index and sets count (0 if missing).
int findGroup(const char* name, int& count);

// Gains are linear (1 = full scale); pitch scales the playback rate.
void play(int entry, float left, float right, float pitch);
void playMusic(const char* path, float volume);
void stopMusic();
void setMusicVolume(float volume);
bool musicActive();  // a track is queued or playing

void mix(int16_t* out, int frames);

}  // namespace audio
