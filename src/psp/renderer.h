#pragma once
#include "../core/mat4.h"
#include "../core/mesher.h"
#include "../core/player.h"
#include "../core/world.h"

struct Camera {
    float x, y, z;
    float yaw, pitch;
    bool underwater;
    Mat4 effects = Mat4::identity();  // hurt tilt and view bobbing, applied before the view rotation
};

class WorldRenderer {
public:
    static constexpr int MAX_DISTANCE = 6;

    bool init();
    void reset();                  // drop all meshes
    void releaseChunk(Chunk* c);   // world unload hook
    // Rebuilds up to maxBuilds dirty sections, nearest first, stopping early once budgetUs microseconds
    // are used (at least one section is always built).
    int update(World& w, const Camera& cam, int maxBuilds, int budgetUs = 0);
    // Rebuilds every dirty section within `radius` chunks (loading screens).
    void buildNear(World& w, const Camera& cam, int radius);
    void render(const World& w, const Camera& cam, const RayHit* selection);

    int renderDistance = 3;  // in chunks
    int drawnSections = 0;
    int drawnVertices = 0;
    bool outOfMemory = false;

    // Set by the game every frame (day/night).
    uint32_t skyColor = 0xFFFFA77A;
    uint32_t fogColor = 0xFFFFCFB2;
    float celestialAngle = 0.0f;
    float starBrightness = 0.0f;
    float sunBrightness = 1.0f;
    int crackStage = -1;  // 0..9 while mining the selected block
    bool nether = false;  // no sky or celestial bodies, fog starts at the camera
    bool clouds = true;
    uint32_t cloudColor = 0xCCFFFFFF;
    float cloudScroll = 0;  // blocks the clouds drifted east

    // Optional hook drawn after opaque terrain with the world matrices set (entities).
    void (*drawEntities)(void* user, const Camera& cam) = nullptr;
    void* drawUser = nullptr;
    // Optional hook drawn after translucent terrain (particles, rain and snow, lightning).
    void (*drawEffects)(void* user, const Camera& cam) = nullptr;

private:
    struct SectionMesh {
        CubeVertex* cubes = nullptr;
        MeshVertex* opaque = nullptr;
        MeshVertex* trans = nullptr;
        uint16_t nCubes = 0, nOpaque = 0, nTrans = 0;
        uint8_t conn[6] = {0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F};
        // 0 never built, 1 has vertices, 2 built empty, 3 one retry queued, 4 empty for good.
        uint8_t phase = 0;
    };
    struct RenderChunk {
        SectionMesh sec[8];
    };
    static constexpr int W = MAX_DISTANCE * 2 + 1;
    static constexpr int SLOTS = W * W * 8;

    void rebuild(World& w, Chunk* c, int sy);
    void freeMesh(SectionMesh& m);
    void drawSky(const Camera& cam);
    void drawCelestial(const Camera& cam);
    void drawClouds(const Camera& cam, bool aboveTerrain);

    CubeBuffer tmpCubes_;
    MeshBuffer tmpOpaque_, tmpTrans_;
    uint16_t visible_[SLOTS];
    uint16_t queue_[SLOTS];
    uint8_t entry_[SLOTS];
    uint8_t dirs_[SLOTS];
    uint32_t visitedFrame_[SLOTS] = {};
    const Chunk* visibleChunk_[SLOTS];
    uint32_t frame_ = 0;
};
