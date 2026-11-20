#pragma once

class Game;

// Audio output thread, positional effects and background music (SoundManager).
namespace sound {

bool init();  // loads sounds.pak and starts the output thread; the game runs silently if it fails
void shutdown();
void setVolumes(float effects, float music);

// Plays the sounds the game queued since the last call, heard from the given eye position and yaw.
void playQueued(Game& g, float ex, float ey, float ez, float yaw);
// Starts a random music track after the Beta delay (12000-24000 ticks between tracks).
void updateMusic(double dt);
// Starts, stops and attenuates the jukebox record (Game::record).
void updateRecord(Game& g, float ex, float ey, float ez);
void ui(const char* name);  // non-positional (menu clicks)

}  // namespace sound
