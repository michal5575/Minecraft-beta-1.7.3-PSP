#include "sound.h"

#include <dirent.h>
#include <pspaudio.h>
#include <pspkernel.h>
#include <strings.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "../core/audio.h"
#include "../core/game.h"

namespace sound {
namespace {

constexpr int FRAMES = 1024;
volatile bool g_running = false;
int g_thread = -1;
float g_effects = 1.0f, g_music = 1.0f;
JRandom g_rand(0);
double g_ticksBeforeMusic = 0;

constexpr int MAX_TRACKS = 16;
char g_tracks[MAX_TRACKS][40];
int g_trackCount = 0;

struct CachedGroup {
    const char* name;
    int first, count;
};
CachedGroup g_cache[128];
int g_cacheCount = 0;

int g_recordSerial = 0;
bool g_recordStream = false;  // the music stream is playing a jukebox record

int audioThread(SceSize, void*) {
    int ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, FRAMES, PSP_AUDIO_FORMAT_STEREO);
    if (ch < 0) return 0;
    static int16_t buf[2][FRAMES * 2];
    int cur = 0;
    while (g_running) {
        audio::mix(buf[cur], FRAMES);
        sceAudioOutputPannedBlocking(ch, PSP_AUDIO_VOLUME_MAX, PSP_AUDIO_VOLUME_MAX, buf[cur]);
        cur ^= 1;
    }
    sceAudioChRelease(ch);
    return 0;
}

void findTracks() {
    g_trackCount = 0;
    DIR* d = opendir("music");
    if (!d) {
        std::printf("sound: no music folder\n");
        return;
    }
    while (dirent* e = readdir(d)) {
        if (g_trackCount >= MAX_TRACKS) break;
        size_t n = std::strlen(e->d_name);
        if (n > 4 && n < 30 && !strcasecmp(e->d_name + n - 4, ".wav"))
            std::snprintf(g_tracks[g_trackCount++], sizeof(g_tracks[0]), "music/%s", e->d_name);
    }
    closedir(d);
}

// Picks a random variant of a sound group (SoundPool.getRandomSoundFromSoundPool).
int pick(const char* name) {
    for (int i = 0; i < g_cacheCount; ++i)
        if (g_cache[i].name == name) return g_cache[i].count ? g_cache[i].first + g_rand.nextInt(g_cache[i].count) : -1;
    int count;
    int first = audio::findGroup(name, count);
    if (g_cacheCount < 128) g_cache[g_cacheCount++] = {name, first, count};
    return count ? first + g_rand.nextInt(count) : -1;
}

}  // namespace

bool init() {
    if (!audio::loadBank("sounds.pak")) std::printf("sound: sounds.pak missing, effects disabled\n");
    findTracks();
    g_rand.setSeed((int64_t)sceKernelGetSystemTimeWide());
    g_ticksBeforeMusic = g_rand.nextInt(12000);
    g_running = true;
    g_thread = sceKernelCreateThread("audio", audioThread, 0x12, 0x10000, PSP_THREAD_ATTR_USER, nullptr);
    if (g_thread < 0 || sceKernelStartThread(g_thread, 0, nullptr) < 0) {
        g_running = false;
        return false;
    }
    std::printf("sound: %d KB of effects, %d music tracks\n", audio::bankBytes() / 1024, g_trackCount);
    return true;
}

void shutdown() {
    if (!g_running) return;
    g_running = false;
    sceKernelWaitThreadEnd(g_thread, nullptr);
    sceKernelDeleteThread(g_thread);
    audio::stopMusic();
}

void setVolumes(float effects, float music) {
    g_effects = effects;
    g_music = music;
    if (g_recordStream) return;
    audio::setMusicVolume(music);
    if (music <= 0 && audio::musicActive()) audio::stopMusic();
}

// RenderGlobal.playRecord / SoundManager.playStreaming: a positional stream with linear
// attenuation over 64 blocks that replaces the background music.
void updateRecord(Game& g, float ex, float ey, float ez) {
    const RecordState& r = g.record;
    if (r.serial != g_recordSerial) {
        g_recordSerial = r.serial;
        if (g_recordStream || r.record) audio::stopMusic();
        g_recordStream = false;
        if (r.record) {
            char path[32];
            std::snprintf(path, sizeof(path), "records/%s.wav", r.record == I_RECORD_CAT ? "cat" : "13");
            audio::playMusic(path, 0.0f);
            g_recordStream = true;
        }
    }
    if (!g_recordStream) return;
    if (!audio::musicActive()) {
        g_recordStream = false;
        audio::setMusicVolume(g_music);
        return;
    }
    float dx = r.x + 0.5f - ex, dy = r.y + 0.5f - ey, dz = r.z + 0.5f - ez;
    float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    audio::setMusicVolume(d >= 64.0f ? 0.0f : (1.0f - d / 64.0f) * g_effects);
}

void playQueued(Game& g, float ex, float ey, float ez, float yaw) {
    float rx = -std::cos(yaw * 0.017453292f), rz = -std::sin(yaw * 0.017453292f);
    for (int i = 0; i < g.soundCount; ++i) {
        const SoundEvent& s = g.sounds[i];
        if (g_effects <= 0) continue;
        int entry = pick(s.name);
        if (entry < 0) continue;
        float gain = (s.volume > 1.0f ? 1.0f : s.volume) * g_effects;
        float left = gain, right = gain;
        if (!s.global) {
            // Paulscode linear attenuation over 16 blocks (scaled by volumes above 1).
            float dx = s.x - ex, dy = s.y - ey, dz = s.z - ez;
            float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            float range = 16.0f * (s.volume > 1.0f ? s.volume : 1.0f);
            if (d >= range) continue;
            gain *= 1.0f - d / range;
            float pan = d > 0.01f ? (dx * rx + dz * rz) / d : 0.0f;
            left = gain * (pan > 0 ? 1.0f - pan * 0.7f : 1.0f);
            right = gain * (pan < 0 ? 1.0f + pan * 0.7f : 1.0f);
        }
        audio::play(entry, left, right, s.pitch);
    }
    g.soundCount = 0;
}

void updateMusic(double dt) {
    if (g_music <= 0 || !g_trackCount || g_recordStream || audio::musicActive()) return;
    g_ticksBeforeMusic -= dt * 20.0;
    if (g_ticksBeforeMusic > 0) return;
    audio::playMusic(g_tracks[g_rand.nextInt(g_trackCount)], g_music);
    g_ticksBeforeMusic = g_rand.nextInt(12000) + 12000;
}

void ui(const char* name) {
    if (g_effects <= 0) return;
    int entry = pick(name);
    if (entry >= 0) audio::play(entry, g_effects, g_effects, 1.0f);
}

}  // namespace sound
