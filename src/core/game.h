#pragma once
#include "crafting.h"
#include "items.h"
#include "jrandom.h"
#include "particles.h"
#include "player.h"
#include "world.h"

enum GameMode : uint8_t { MODE_SURVIVAL = 0, MODE_CREATIVE = 1 };

enum EntityType : uint8_t {
    ENT_NONE,
    ENT_ITEM,
    ENT_ARROW,
    ENT_TNT,
    ENT_ZOMBIE,
    ENT_SKELETON,
    ENT_CREEPER,
    ENT_SPIDER,
    ENT_PIG,
    ENT_COW,
    ENT_SHEEP,
    ENT_CHICKEN,
    ENT_SLIME,
    ENT_SQUID,
    ENT_WOLF,
    ENT_SNOWBALL,
    ENT_EGG,
    ENT_FISH,      // fishing bobber (EntityFish)
    ENT_PAINTING,
    ENT_MINECART,
    ENT_BOAT,
    ENT_GHAST,
    ENT_PIGZOMBIE,
    ENT_FIREBALL,
    ENT_TYPE_COUNT
};

inline bool entityIsMob(uint8_t t) { return (t >= ENT_ZOMBIE && t <= ENT_WOLF) || t == ENT_GHAST || t == ENT_PIGZOMBIE; }
inline bool entityIsVehicle(uint8_t t) { return t == ENT_MINECART || t == ENT_BOAT; }
constexpr int16_t RIDER_PLAYER = -2;  // Entity::rider when the player sits in it
// EntityMinecart.func_514_g / func_515_a: the point on the track under a minecart's (x, posY, z), optionally
// moved `offset` along the track; false off rails.
bool minecartRailPos(const World& w, float& x, float& y, float& z);
bool minecartRailPosAhead(const World& w, float x, float y, float z, float offset, float& ox, float& oy, float& oz);
inline bool entityIsThrowable(uint8_t t) { return t == ENT_SNOWBALL || t == ENT_EGG; }

// EnumArt: title, size in pixels and offset in art/kz.png.
struct PaintingArt {
    const char* title;
    int w, h, u, v;
};
extern const PaintingArt g_paintingArt[];
extern const int g_paintingArtCount;
inline bool entityIsHostile(uint8_t t) {
    return (t >= ENT_ZOMBIE && t <= ENT_SPIDER) || t == ENT_SLIME || t == ENT_GHAST || t == ENT_PIGZOMBIE;
}
// BiomeGenBase.getEnableSnow / canSpawnLightningBolt (weather.cpp); biome -1 (unloaded) neither snows nor rains.
bool biomeSnows(int biome);
bool biomeRains(int biome);
inline bool entityIsAnimal(uint8_t t) { return (t >= ENT_PIG && t <= ENT_CHICKEN) || t == ENT_WOLF; }

struct Entity : Body {
    uint8_t type = ENT_NONE;
    bool dead = false;
    int16_t health = 0, lastDamage = 0;
    int16_t hurtTime = 0, hurtResist = 0, deathTime = 0, attackTime = 0;
    int age = 0;
    int16_t fire = 0;
    float prevYaw = 0;
    // dropped items
    ItemStack item;
    int16_t pickupDelay = 0;
    float spin = 0;
    // AI
    bool targeting = false;   // chasing the player
    int16_t target = -1;      // index of the entity being chased (arrows: the shooter)
    int16_t lookTicks = 0;    // EntityLiving.numTicksToChaseTarget while watching the player
    float moveForward = 0, moveStrafe = 0;
    bool jumping = false;
    int16_t wanderTicks = 0;
    float wanderX = 0, wanderZ = 0;
    int16_t fuse = 0, prevFuse = 0;  // creeper / primed TNT
    uint8_t woolColor = 0;
    bool sheared = false;
    int eggTimer = 0;
    // arrows, throwables and the fishing bobber
    bool inGround = false, fromPlayer = false;
    int16_t groundTicks = 0, airTicks = 0;
    int16_t hooked = -1;                 // bobber: hooked entity index
    int16_t biteTimer = 0, bitingTicks = 0;
    // paintings: art index (woolColor), facing 0..3 (fuse) and the hanging block
    int tileX = 0, tileY = 0, tileZ = 0;
    // animation
    float limbSwing = 0, limbAmount = 0, prevLimbAmount = 0;
    int16_t livingSoundTime = 0;
    // slime
    uint8_t size = 1;
    int16_t jumpDelay = 0;
    float squish = 0, prevSquish = 0;
    bool splits = false;  // died with exactly 0 health (EntitySlime.setEntityDead)
    // squid
    float bodyPitch = 0, prevBodyPitch = 0, bodySpin = 0, prevBodySpin = 0;
    float tentacle = 0, prevTentacle = 0, phase = 0, phaseSpeed = 0;
    float thrust = 0, spinSpeed = 0, swimX = 0, swimY = 0, swimZ = 0;
    // wolf
    bool tamed = false, sitting = false, angry = false;
    bool begging = false, wet = false, shaking = false;
    float beg = 0, prevBeg = 0, shake = 0, prevShake = 0;
    // riding: the entity sitting in this one (index or RIDER_PLAYER) and the one this mob sits in
    int16_t rider = -1, vehicle = -1;
    bool saddled = false;  // pigs
    bool powered = false;  // creepers struck by lightning
    // minecarts (EntityMinecart: type 0 plain, 1 chest, 2 furnace) and boats
    uint8_t cartType = 0;
    bool cartFlipped = false;
    int16_t rollTime = 0, rollDamage = 0, rollDir = 1;  // hurt wobble (timeSinceHit, damageTaken, forwardDirection)
    int fuel = 0;
    float pushX = 0, pushZ = 0;
    ItemStack cartItems[27];
};

struct Inventory {
    ItemStack main[36];   // 0..8 hotbar
    ItemStack armor[4];   // helmet, chestplate, leggings, boots
    int selected = 0;

    ItemStack& held() { return main[selected]; }
    // Merges into matching stacks, then empty slots; leaves the remainder in s.
    bool add(ItemStack& s);
    int armorValue() const;
    void damageArmor(int damage);
    int count(uint16_t id) const;
    bool consume(uint16_t id);
    void clear();
};

// A sound for the platform to play this frame. `global` sounds ignore the position (GUI, music).
struct SoundEvent {
    const char* name;
    float x, y, z;
    float volume, pitch;
    bool global;
};

// What the UI should open after a use action.
enum class UseResult { None, Workbench, Furnace, Chest, Sign, Dispenser, CartChest };

// The record playing in a jukebox (World.playRecord). `serial` changes on every start or stop.
struct RecordState {
    uint16_t record = 0;  // 0: silent
    int x = 0, y = 0, z = 0;
    int serial = 0;
};

class Game {
public:
    static constexpr int MAX_ENTITIES = 128;
    static constexpr int MAX_HEALTH = 20;
    static constexpr int MAX_AIR = 300;

    Game();
    bool init();

    bool createWorld(const char* dir, int64_t seed, int mode);
    bool loadWorld(const char* dir);
    bool saveLevel();
    void closeWorld();  // saves level.dat and all chunks
    bool worldOpen() const { return world.isOpen(); }
    static bool levelExists(const char* dir);

    // Streams chunks; `budget` seconds of generation work on the main thread.
    void stream(int renderDistance, double budget);
    bool spawnReady(int radius) const;  // chunks around the player are lit
    void placeAtSpawn();                // after the spawn area is loaded

    void tick();  // 20 Hz

    // Input for the next tick.
    bool attackHeld = false;
    bool attackPressed = false;
    bool usePressed = false;

    // Targeting (updated by updateTarget, also used for the selection box).
    void updateTarget(float eyeX, float eyeY, float eyeZ);
    bool hasBlockTarget = false;
    RayHit blockTarget{};
    Entity* entityTarget = nullptr;

    UseResult lastUse = UseResult::None;
    TileEntity* openTile = nullptr;  // also the sign being edited
    RecordState record;
    void jukeboxStop(int x, int y, int z);
    int fishHook = -1;  // index of the player's bobber
    int riding = -1;    // index of the vehicle or pig the player sits in
    int openCart = -1;  // chest minecart whose inventory is open
    Entity* ridingEntity() { return riding >= 0 ? &ents[riding] : nullptr; }
    void mount(int index);  // Entity.mountEntity for the player; the same index again dismounts
    // Collision boxes of entities for Body::move (boats for everyone, every body for vehicles).
    int entityBoxes(const Body* self, const AABB& area, AABB* out, int max) const;
    // Compass and clock needle state (TextureCompassFX / TextureWatchFX), updated every tick.
    float compassAngle = 0, compassVel = 0, clockAngle = 0, clockVel = 0;

    int crackStage() const { return mining_ && mineProgress_ > 0 ? (int)(mineProgress_ * 10.0f) : -1; }
    // EntityLiving.getSwingProgress (0..1 over 8 ticks).
    float swingProgress(float partial) const {
        float d = swing_ - prevSwing_;
        if (d < 0) d += 1.0f;
        return prevSwing_ + d * partial;
    }
    void swingItem();
    int handResets = 0;        // bumped when an item is used in the air (ItemRenderer lowers and raises it)
    float attackedAtYaw = 0;   // direction of the last hit relative to the view, for the hurt camera tilt

    void respawn();
    void dropItem(const ItemStack& s, float x, float y, float z, bool fromPlayer);
    void dropHeld(bool all);
    Entity* spawn(uint8_t type, float x, float y, float z);
    void explode(float x, float y, float z, float power, bool flaming = false);
    bool hurtPlayer(int damage, float fromX, float fromZ, bool knock, Entity* attacker = nullptr);
    int biomeAt(int x, int z) const;
    bool slimeChunk(int cx, int cz) const;
    static float wolfTailAngle(const Entity& e);

    // Day/night (World.getCelestialAngle, getSkyColor, getFogColor), darkened by rain and thunder.
    float celestialAngle() const;
    uint32_t skyColor(float partial = 1.0f) const;  // 0xRRGGBB
    uint32_t fogColor(float partial = 1.0f) const;
    uint32_t cloudColor(float partial = 1.0f) const;
    float starBrightness() const;
    int computeSkySubtracted() const;
    bool isDaytime() const { return world.skySubtracted < 4; }

    // Weather (weather.cpp). The timers count down to the next change like WorldInfo's rainTime and
    // thunderTime; the strengths fade by 0.01 per tick.
    int rainTime = 0, thunderTime = 0;
    bool raining = false, thundering = false;
    float rain = 0, prevRain = 0, thunder = 0, prevThunder = 0;
    int lightningFlash = 0;  // World.lightningFlash: sky flash ticks
    bool fancyWeather = false;  // more rain splashes (fancy graphics)
    float rainStrength(float partial) const;
    float thunderStrength(float partial) const;  // already multiplied by the rain strength
    bool isRaining() const { return rainStrength(1.0f) > 0.2f; }
    bool isThundering() const { return thunderStrength(1.0f) > 0.9f; }
    bool rainingAt(int x, int y, int z) const;  // World.canLightningStrikeAt
    void clearWeather();                        // after sleeping through the night
    // EntityLightningBolt
    struct Bolt {
        float x, y, z;
        int64_t seed;  // RenderLightningBolt shape
        int life, flashes;
    };
    static constexpr int MAX_BOLTS = 4;
    Bolt bolts[MAX_BOLTS];
    int boltCount = 0;
    void strikeLightning(float x, float y, float z);

    Particles particles;

    // Shown in the world list (saved in level.dat); the platform keeps lastPlayed current.
    char worldName[32] = "New World";
    int64_t lastPlayed = 0;
    struct LevelInfo {
        char name[32];
        int64_t seed, lastPlayed;
        int mode;
    };
    static bool readLevelInfo(const char* dir, LevelInfo& out);
    static bool renameLevel(const char* dir, const char* name);
    static void deleteLevel(const char* dir);

    bool survival() const { return mode == MODE_SURVIVAL; }

    // The Nether (nether.cpp). Dimension 0 is the overworld, -1 the Nether (saved in DIM-1).
    int dimension = 0;
    float portalTime = 0, prevPortalTime = 0;  // EntityPlayerSP.timeInPortal: overlay and nausea
    int portalCooldown = 0;                    // timeUntilPortal
    bool portalPending = false;                // set when the portal timer runs out; the platform travels
    // Minecraft.usePortal: saves and closes this dimension, then opens the other one with the position
    // scaled by 8. The platform streams the destination and then calls placeInPortal.
    bool travelDimension();
    // Teleporter.placeInPortal. Returns false after moving the player to a remembered portal outside the
    // loaded area: stream the chunks there and call it again.
    bool placeInPortal();
    bool tryCreatePortal(int x, int y, int z);  // BlockPortal.tryToCreatePortal

    World world;
    Player player;
    Inventory inv;
    uint8_t mode = MODE_SURVIVAL;
    int health = MAX_HEALTH, prevHealth = MAX_HEALTH, air = MAX_AIR;
    int hurtTime = 0, hurtResist = 0, lastDamage = 0, fire = 0;
    bool dead = false;
    int deathTime = 0;
    const char* deathMessage = "";
    int spawnX = 0, spawnY = 64, spawnZ = 0;
    bool spawnFound = false;

    // Beds (EntityPlayer.sleepInBedAt / wakeUpPlayer). The bed position is the head block.
    bool sleeping = false;
    int sleepTimer = 0;  // 0..100 while falling asleep, 100..110 fading back in after the night
    int sleepX = 0, sleepY = 0, sleepZ = 0;
    bool hasBed = false;
    int bedX = 0, bedY = 0, bedZ = 0;
    const char* message = nullptr;  // one-shot status text for the HUD
    void wakeUp(bool setSpawn);
    float sleepYaw() const;
    Entity ents[MAX_ENTITIES];
    JRandom rand;
    int renderDistance = 3;
    int skyChanges = 0;  // incremented when the sky light level changes (all meshes rebuilt)

    // Sounds since the platform last drained them (World.playSoundEffect / playSoundAtEntity).
    static constexpr int MAX_SOUNDS = 48;
    SoundEvent sounds[MAX_SOUNDS];
    int soundCount = 0;
    void playSound(const char* name, float x, float y, float z, float volume, float pitch);
    void playSoundAt(const Body& b, const char* name, float volume, float pitch) {
        playSound(name, b.x, b.y, b.z, volume, pitch);
    }
    void playUiSound(const char* name, float volume, float pitch);
    JRandom soundRand;  // pitch and variant randomness, kept apart from gameplay randomness

    // Called for every chunk leaving the world (renderer meshes).
    void (*onChunkRelease)(Chunk*, void*) = nullptr;
    void* releaseUser = nullptr;

private:
    char baseDir_[64] = {};
    bool inPortal_ = false;
    // Portals seen in either dimension (x, y, z, dimension), so travel finds them beyond the loaded chunks
    // like Teleporter's 128-block search.
    static constexpr int MAX_KNOWN_PORTALS = 32;
    int32_t knownPortals_[MAX_KNOWN_PORTALS][4] = {};
    int knownPortalCount_ = 0;
    void rememberPortal(int x, int y, int z);
    void dimensionDir(char* out, int size, int dim) const;
    void tickPortal();
    void portalNeighborChange(int x, int y, int z);
    bool findPortal();
    void createPortal();
    void tickWeather();
    void weatherChunk(Chunk* c);
    void rollLightning();
    void lightningFire(int x, int y, int z);
    void tickBolts();
    void struckByLightning(Entity& e);
    void rainEffects();
    void fireSmoke(int x, int y, int z);
    void portalParticles(int x, int y, int z);
    void blockParticles(int x, int y, int z, uint8_t id);
    void deathPoof(const Entity& e);
    void heartsOrSmoke(const Entity& e, bool hearts);
    int rainTicks_ = 0, rainSoundTimer_ = 0;
    void tickGhast(Entity& e);
    bool ghastCourseClear(const Entity& e, float x, float y, float z, float dist) const;
    void tickFireball(Entity& e);
    void shootFireball(Entity& shooter, float dx, float dy, float dz);
    void angerPigZombies(const Entity& victim);

    static void hookLoaded(World& w, Chunk* c, void* user);
    static void hookUnload(World& w, Chunk* c, void* user);
    void onChunkLoaded(Chunk* c);
    void onChunkUnload(Chunk* c);
    void onChunkPopulated(Chunk* c);
    void findSpawn();

    float mobPitch();
    void displayTicks();
    void caveSounds();
    void bodySounds(Body& b);
    void blockSound(int x, int y, int z, uint8_t id, bool broken);

    void tickPlayer();
    void tickMining();
    void tickUse();
    void tickEntities();
    void tickEntity(Entity& e);
    void tickItem(Entity& e);
    void tickArrow(Entity& e);
    void tickTnt(Entity& e);
    void tickThrowable(Entity& e);
    void tickFish(Entity& e);
    void tickPainting(Entity& e);
    bool paintingFits(const Entity& e) const;
    void setPaintingDir(Entity& e, int dir);
    bool placePainting(const RayHit& hit);
    void throwItem(uint8_t type);
    int retrieveFish();
    void tickDials();
    bool eatCake(int x, int y, int z);
    void tickMob(Entity& e);
    void mobAI(Entity& e);
    void slimeAI(Entity& e);
    void squidTick(Entity& e);
    void wolfAI(Entity& e);
    void wolfUpdate(Entity& e);
    bool useOnWolf(Entity& e);
    void alertWolves(Entity& victim, bool playerAttacked);
    Entity* targetOf(const Entity& e);
    void faceTowards(Entity& e, float x, float z, float maxTurn);
    bool canSee(const Entity& e, float x, float y, float z) const;
    void killMob(Entity& e, Entity* killer = nullptr);
    void removeMob(Entity& e);
    bool hurtEntity(Entity& e, int damage, float fromX, float fromZ, bool byPlayer, Entity* attacker = nullptr);
    void spawnMobs();
    bool monsterLightOk(int x, int y, int z);
    void randomTicks();
    void randomTick(int x, int y, int z, uint8_t id);
    void tickTiles();
    void tickFurnace(TileEntity& t);
    void tickSpawner(TileEntity& t);
    void updateSky();

    bool breakBlock(int x, int y, int z, bool drops);
    void dropBlockItems(int x, int y, int z, uint8_t id, int meta);
    bool placeHeld(const RayHit& hit);
    UseResult useOnBlock(const RayHit& hit);
    bool useItemInAir();
    void shootArrow(Entity* shooter, float x, float y, float z, float dx, float dy, float dz, float speed,
                    float spread, bool fromPlayer);
    void damageHeld(int amount);
    void popUnsupported(int x, int y, int z);
    void settleFalling(int x, int y, int z);
    bool supportsPlant(uint8_t id, uint8_t below, int x, int y, int z) const;
    void markLeavesForDecay(int x, int y, int z);
    bool canSpawnAt(int x, int y, int z) const;
    bool bedSpawnSpot(int x, int y, int z, int& ox, int& oy, int& oz) const;
    void tickSleep();

    // Block updates (blocklogic.cpp): World.setBlockWithNotify, onNeighborBlockChange, onBlockAdded,
    // scheduled ticks, liquids and fire.
public:
    void setBlockNotify(int x, int y, int z, uint8_t id, int meta = 0);
    void notifyNeighbors(int x, int y, int z, uint8_t changed);
    void onBlockAdded(int x, int y, int z, uint8_t id);
    // Redstone (redstone.cpp). Sides use Beta numbering; `side` is the face of the queried block that the
    // asking block touches, seen from the asker (0: the queried block is below it).
    bool powerWeak(int x, int y, int z, int side) const;    // Block.isPoweringTo
    bool powerStrong(int x, int y, int z, int side) const;  // Block.isIndirectlyPoweringTo
    bool blockPowered(int x, int y, int z) const;           // World.isBlockGettingPowered
    bool powerTo(int x, int y, int z, int side) const;      // World.isBlockIndirectlyProvidingPowerTo
    bool indirectlyPowered(int x, int y, int z) const;      // World.isBlockIndirectlyGettingPowered
    void tickPistons();
    // Entities standing on pressure plates (Block.onEntityCollidedWithBlock).
    void touchPlates(const AABB& box);

private:
    bool blockCanStay(int x, int y, int z, uint8_t id, int meta) const;
    void onBlockRemoved(int x, int y, int z, uint8_t old, int oldMeta);
    bool canProvidePower(uint8_t id) const;
    bool wireConnects(int x, int y, int z, int dir) const;
    bool wireWeak(int x, int y, int z, int side) const;
    void wireUpdate(int x, int y, int z);
    void wireCalc(int x, int y, int z, int sx, int sy, int sz);
    void wireNotifyIfWire(int x, int y, int z);
    void notifyAround(int x, int y, int z, uint8_t id);
    bool redstoneTorchPowered(int x, int y, int z, int meta) const;
    void redstoneTorchTick(int x, int y, int z, uint8_t id);
    bool repeaterPowered(int x, int y, int z, int meta) const;
    void notifyAttached(int x, int y, int z, uint8_t id, int meta);
    void toggleLever(int x, int y, int z);
    void pressButton(int x, int y, int z);
    void plateUpdate(int x, int y, int z);
    void notePlay(int x, int y, int z);
    void dispense(int x, int y, int z);
    void doorPowered(int x, int y, int z, bool powered);
    bool pistonPowered(int x, int y, int z, int dir) const;
    bool pistonCanPush(uint8_t id, int x, int y, int z, bool destroy) const;
    bool pistonCanExtend(int x, int y, int z, int dir) const;
    bool pistonTryExtend(int x, int y, int z, int dir);
    void pistonUpdate(int x, int y, int z);
    void pistonEvent(int x, int y, int z, int event, int dir);
    TileEntity* pistonTile(int x, int y, int z, uint8_t id, int meta, int dir, bool extending, bool source);
    void pistonFinish(int x, int y, int z);
    void pistonPush(const TileEntity& t, float progress, float amount);
    bool redstoneNeighborChange(int x, int y, int z, uint8_t id, uint8_t changed);
    void redstoneAdded(int x, int y, int z, uint8_t id);
    void redstoneRemoved(int x, int y, int z, uint8_t id, int meta);
    bool redstoneTick(int x, int y, int z, uint8_t id);
    bool redstoneUse(int x, int y, int z, uint8_t id);
    bool redstoneClick(int x, int y, int z, uint8_t id);
    void redstonePlaced(int x, int y, int z, uint8_t id);
    // Rails, minecarts, boats and riding (vehicles.cpp).
    void railRefresh(int x, int y, int z, bool powered, bool force);
    void railNeighborChange(int x, int y, int z, uint8_t id, uint8_t changed);
    bool poweredRailSearch(int x, int y, int z, int meta, bool forward, int depth) const;
    bool poweredRailAt(int x, int y, int z, bool forward, int depth, int axis) const;
    void detectorUpdate(int x, int y, int z, int meta);
    void tickMinecart(Entity& e);
    void tickBoat(Entity& e);
    void tickRider(Entity& v);
    void cartCollide(Entity& cart, Body& other, Entity* otherEnt);
    void vehicleCollisions();
    bool hurtVehicle(Entity& e, int damage);
    bool useOnEntity(Entity& e);
    bool placeCart(const RayHit& hit);
    bool placeBoat();
    float riderYawDelta_ = 0;  // Entity.entityRiderYawDelta
    float riderMX_ = 0, riderMZ_ = 0;
    int boatCount_ = 0;  // live boats; when zero only vehicles need entity collision boxes
    static int entityBoxesThunk(void* user, const Body* self, const AABB& area, AABB* out, int max) {
        return static_cast<const Game*>(user)->entityBoxes(self, area, out, max);
    }
    bool editingBlocks_ = false;  // World.editingBlocks: neighbor notifications are suppressed
    bool wirePower_ = true;       // BlockRedstoneWire.wiresProvidePower
    bool pistonBusy_ = false;     // BlockPistonBase.ignoreUpdates
    void onNeighborChange(int x, int y, int z, uint8_t changed);
    void tickScheduled();
    void scheduledTick(int x, int y, int z, uint8_t id);
    int tickRate(uint8_t id) const;
    void liquidTick(int x, int y, int z, uint8_t id);
    void liquidFlowInto(int x, int y, int z, uint8_t id, int meta);
    bool liquidCanDisplace(int x, int y, int z, bool water) const;
    void liquidOptimalFlow(int x, int y, int z, bool water, bool out[4]) const;
    int liquidFlowCost(int x, int y, int z, int depth, int from, bool water) const;
    void liquidHarden(int x, int y, int z);
    void lavaIgnite(int x, int y, int z);
    bool fireCanStay(int x, int y, int z) const;
    bool fireNeighborBurns(int x, int y, int z) const;
    int fireEncouragement(int x, int y, int z) const;
    void fireTick(int x, int y, int z);
    void fireTryCatch(int x, int y, int z, int chance, int age);

    float mineProgress_ = 0;
    int mineX_ = 0, mineY_ = 0, mineZ_ = 0;
    bool mining_ = false;
    int mineSoundTicks_ = 0;
    int caveSoundTimer_ = 6000;
    int mineDelay_ = 0;
    int useDelay_ = 0;
    int swingInt_ = 0;
    bool swinging_ = false;
    float swing_ = 0, prevSwing_ = 0;
    int spawnTimer_ = 0;
    int levelSaveTimer_ = 0;
    bool autosaving_ = false;
};
