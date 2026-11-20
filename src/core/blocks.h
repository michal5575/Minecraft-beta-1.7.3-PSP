#pragma once
#include <cstdint>

enum BlockId : uint8_t {
    AIR = 0,
    STONE = 1,
    GRASS = 2,
    DIRT = 3,
    COBBLESTONE = 4,
    PLANKS = 5,
    SAPLING = 6,
    BEDROCK = 7,
    WATER_FLOWING = 8,
    WATER = 9,
    LAVA_FLOWING = 10,
    LAVA = 11,
    SAND = 12,
    GRAVEL = 13,
    GOLD_ORE = 14,
    IRON_ORE = 15,
    COAL_ORE = 16,
    LOG = 17,
    LEAVES = 18,
    SPONGE = 19,
    GLASS = 20,
    LAPIS_ORE = 21,
    LAPIS_BLOCK = 22,
    DISPENSER = 23,
    SANDSTONE = 24,
    NOTE_BLOCK = 25,
    BED = 26,
    POWERED_RAIL = 27,
    DETECTOR_RAIL = 28,
    STICKY_PISTON = 29,
    WEB = 30,
    TALL_GRASS = 31,
    DEAD_BUSH = 32,
    PISTON = 33,
    PISTON_HEAD = 34,
    WOOL = 35,
    PISTON_MOVING = 36,
    FLOWER_YELLOW = 37,
    FLOWER_RED = 38,
    MUSHROOM_BROWN = 39,
    MUSHROOM_RED = 40,
    GOLD_BLOCK = 41,
    IRON_BLOCK = 42,
    DOUBLE_SLAB = 43,
    SLAB = 44,
    BRICK = 45,
    TNT = 46,
    BOOKSHELF = 47,
    MOSSY_COBBLESTONE = 48,
    OBSIDIAN = 49,
    TORCH = 50,
    FIRE = 51,
    MOB_SPAWNER = 52,
    WOOD_STAIRS = 53,
    CHEST = 54,
    REDSTONE_WIRE = 55,
    DIAMOND_ORE = 56,
    DIAMOND_BLOCK = 57,
    WORKBENCH = 58,
    CROPS = 59,
    FARMLAND = 60,
    FURNACE = 61,
    FURNACE_LIT = 62,
    SIGN_POST = 63,
    WOOD_DOOR = 64,
    LADDER = 65,
    RAIL = 66,
    COBBLE_STAIRS = 67,
    WALL_SIGN = 68,
    LEVER = 69,
    STONE_PLATE = 70,
    IRON_DOOR = 71,
    WOOD_PLATE = 72,
    REDSTONE_ORE = 73,
    REDSTONE_ORE_LIT = 74,
    REDSTONE_TORCH_OFF = 75,
    REDSTONE_TORCH_ON = 76,
    STONE_BUTTON = 77,
    SNOW_LAYER = 78,
    ICE = 79,
    SNOW_BLOCK = 80,
    CACTUS = 81,
    CLAY = 82,
    REED = 83,
    JUKEBOX = 84,
    FENCE = 85,
    PUMPKIN = 86,
    NETHERRACK = 87,
    SOUL_SAND = 88,
    GLOWSTONE = 89,
    PORTAL = 90,
    JACK_O_LANTERN = 91,
    CAKE = 92,
    REPEATER_OFF = 93,
    REPEATER_ON = 94,
    TRAPDOOR = 96,
};

enum RenderType : uint8_t {
    RENDER_NONE,
    RENDER_CUBE,
    RENDER_CROSS,
    RENDER_LIQUID,
    RENDER_TORCH,
    RENDER_SLAB,
    RENDER_SNOW,
    RENDER_CACTUS,
    RENDER_LADDER,
    RENDER_CROPS,
    RENDER_FENCE,
    RENDER_STAIRS,
    RENDER_DOOR,
    RENDER_FARMLAND,
    RENDER_BED,
    RENDER_FIRE,
    RENDER_CAKE,
    RENDER_TRAPDOOR,
    RENDER_SIGN,  // drawn with the tile entities (TileEntitySignRenderer)
    RENDER_WIRE,
    RENDER_LEVER,
    RENDER_SHAPE,  // a single box from blockShapeBox (buttons, pressure plates)
    RENDER_REPEATER,
    RENDER_PISTON,
    RENDER_PISTON_HEAD,
    RENDER_RAIL,
    RENDER_PORTAL,
};

enum BlockFlags : uint16_t {
    BF_OPAQUE = 1 << 0,       // full opaque cube: hides neighbor faces
    BF_SOLID = 1 << 1,        // has a collision box
    BF_TRANSLUCENT = 1 << 2,  // drawn in the blended pass
    BF_SAME_CULL = 1 << 3,    // faces between two blocks of this type are hidden
    BF_LIQUID = 1 << 4,
    BF_NEEDS_FLOOR = 1 << 5,  // popped off when the block below is removed
    BF_FALLS = 1 << 6,        // sand and gravel
    BF_REPLACEABLE = 1 << 7,  // placing a block here overwrites it
    BF_MAT_SOLID = 1 << 8,    // Material.isSolid() in Beta, used by world generation
    BF_UNBREAKABLE = 1 << 9,
    BF_NORMAL_CUBE = 1 << 10, // isBlockNormalCube: full opaque, torches and ladders attach to it
};

// Beta materials that matter for gameplay (harvesting, fuel, spawning).
enum BlockMaterial : uint8_t {
    MAT_AIR,
    MAT_GROUND,
    MAT_GRASS,
    MAT_WOOD,
    MAT_ROCK,   // needs a pickaxe to drop anything
    MAT_IRON,   // needs a pickaxe
    MAT_LIQUID,
    MAT_LEAVES,
    MAT_PLANTS,
    MAT_SAND,
    MAT_GLASS,
    MAT_CLOTH,
    MAT_SNOW,   // snow layer, needs a shovel
    MAT_BUILT_SNOW,
    MAT_ICE,
    MAT_CACTUS,
    MAT_CLAY,
    MAT_PUMPKIN,
    MAT_WEB,    // only swords and shears harvest it
    MAT_CAKE,
    MAT_CIRCUIT,  // torches, ladders, redstone components
    MAT_PISTON,
    MAT_OTHER,
};

struct BlockDef {
    const char* name;
    uint8_t render;
    uint8_t opacity;   // light opacity 0..15 (15 = blocks light)
    uint8_t emission;  // light emission 0..15
    uint16_t flags;
    uint8_t tex[6];    // bottom, top, north(-z), south(+z), west(-x), east(+x)
    float hardness;    // < 0: unbreakable
    uint8_t material;
};

extern BlockDef g_blocks[256];
extern bool g_fancyLeaves;

void initBlocks();

// BlockFire.setBurnRate: chance to spread fire to air next to this block, and to catch fire itself.
int blockFireSpread(uint8_t id);
int blockFlammability(uint8_t id);
// Material.getCanBurn (lava sets fire next to these).
bool blockMatBurns(uint8_t id);

// Block.stepSound: footsteps and placing use `step`, breaking uses `broken`.
struct StepSound {
    const char* step;
    const char* broken;
    float volume, pitch;
};
const StepSound& blockStepSound(uint8_t id);

inline const BlockDef& blockDef(uint8_t id) { return g_blocks[id]; }
inline bool blockIsOpaque(uint8_t id) {
    if (id == LEAVES) return !g_fancyLeaves;
    return (g_blocks[id].flags & BF_OPAQUE) != 0;
}
inline bool blockIsLiquid(uint8_t id) { return (g_blocks[id].flags & BF_LIQUID) != 0; }
inline bool blockIsWater(uint8_t id) { return id == WATER || id == WATER_FLOWING; }
inline bool blockIsLava(uint8_t id) { return id == LAVA || id == LAVA_FLOWING; }
inline bool blockIsSolid(uint8_t id) { return (g_blocks[id].flags & BF_SOLID) != 0; }
inline bool blockMatSolid(uint8_t id) { return (g_blocks[id].flags & BF_MAT_SOLID) != 0; }
inline bool blockIsReplaceable(uint8_t id) { return (g_blocks[id].flags & BF_REPLACEABLE) != 0; }
inline bool blockIsNormalCube(uint8_t id) { return (g_blocks[id].flags & BF_NORMAL_CUBE) != 0; }
inline bool blockIsDoor(uint8_t id) { return id == WOOD_DOOR || id == IRON_DOOR; }
inline bool blockIsStairs(uint8_t id) { return id == WOOD_STAIRS || id == COBBLE_STAIRS; }
inline bool blockIsSign(uint8_t id) { return id == SIGN_POST || id == WALL_SIGN; }
inline bool blockIsRedstoneTorch(uint8_t id) { return id == REDSTONE_TORCH_OFF || id == REDSTONE_TORCH_ON; }
inline bool blockIsRepeater(uint8_t id) { return id == REPEATER_OFF || id == REPEATER_ON; }
inline bool blockIsPiston(uint8_t id) { return id == PISTON || id == STICKY_PISTON; }
inline bool blockIsPlate(uint8_t id) { return id == STONE_PLATE || id == WOOD_PLATE; }
inline bool blockIsRail(uint8_t id) { return id == RAIL || id == POWERED_RAIL || id == DETECTOR_RAIL; }

// Block.getMobilityFlag: 0 = pushed by pistons, 1 = destroyed by them, 2 = blocks them.
int blockMobility(uint8_t id);

// Texture index in terrain.png for a given side (Beta side numbering).
int blockTexture(uint8_t id, int meta, int side);

// Door helpers (BlockDoor.getState / setDoorRotation): 0..3 = which thin side the door occupies.
inline int doorState(int meta) { return (meta & 4) == 0 ? ((meta - 1) & 3) : (meta & 3); }
