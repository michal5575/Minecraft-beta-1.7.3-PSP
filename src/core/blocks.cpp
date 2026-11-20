#include "blocks.h"

#include <cstring>

BlockDef g_blocks[256];
bool g_fancyLeaves = false;

namespace {

enum : uint16_t {
    CUBE_FLAGS = BF_OPAQUE | BF_SOLID | BF_MAT_SOLID | BF_NORMAL_CUBE,
    PLANT_FLAGS = BF_NEEDS_FLOOR,
};

void def(uint8_t id, const char* name, uint8_t render, uint8_t opacity, uint8_t emission, uint16_t flags,
         int bottom, int top, int side, float hardness, uint8_t material) {
    BlockDef& d = g_blocks[id];
    d.name = name;
    d.render = render;
    d.opacity = opacity;
    d.emission = emission;
    d.flags = flags;
    d.tex[0] = (uint8_t)bottom;
    d.tex[1] = (uint8_t)top;
    for (int i = 2; i < 6; ++i) d.tex[i] = (uint8_t)side;
    d.hardness = hardness;
    d.material = material;
}

void cube(uint8_t id, const char* name, int tex, float hardness, uint8_t material) {
    def(id, name, RENDER_CUBE, 15, 0, CUBE_FLAGS, tex, tex, tex, hardness, material);
}

}  // namespace

void initBlocks() {
    std::memset(g_blocks, 0, sizeof(g_blocks));
    for (auto& b : g_blocks) b.name = "?";
    def(AIR, "Air", RENDER_NONE, 0, 0, BF_REPLACEABLE, 0, 0, 0, 0, MAT_AIR);
    cube(STONE, "Stone", 1, 1.5f, MAT_ROCK);
    def(GRASS, "Grass", RENDER_CUBE, 15, 0, CUBE_FLAGS, 2, 0, 3, 0.6f, MAT_GRASS);
    cube(DIRT, "Dirt", 2, 0.5f, MAT_GROUND);
    cube(COBBLESTONE, "Cobblestone", 16, 2.0f, MAT_ROCK);
    cube(PLANKS, "Wooden Planks", 4, 2.0f, MAT_WOOD);
    def(SAPLING, "Sapling", RENDER_CROSS, 0, 0, PLANT_FLAGS, 15, 15, 15, 0, MAT_PLANTS);
    def(BEDROCK, "Bedrock", RENDER_CUBE, 15, 0, CUBE_FLAGS | BF_UNBREAKABLE, 17, 17, 17, -1, MAT_ROCK);
    def(WATER_FLOWING, "Water", RENDER_LIQUID, 3, 0, BF_LIQUID | BF_TRANSLUCENT | BF_REPLACEABLE, 205, 205, 205, 100,
        MAT_LIQUID);
    def(WATER, "Water", RENDER_LIQUID, 3, 0, BF_LIQUID | BF_TRANSLUCENT | BF_REPLACEABLE, 205, 205, 205, 100,
        MAT_LIQUID);
    def(LAVA_FLOWING, "Lava", RENDER_LIQUID, 15, 15, BF_LIQUID | BF_REPLACEABLE, 237, 237, 237, 100, MAT_LIQUID);
    def(LAVA, "Lava", RENDER_LIQUID, 15, 15, BF_LIQUID | BF_REPLACEABLE, 237, 237, 237, 100, MAT_LIQUID);
    def(SAND, "Sand", RENDER_CUBE, 15, 0, CUBE_FLAGS | BF_FALLS, 18, 18, 18, 0.5f, MAT_SAND);
    def(GRAVEL, "Gravel", RENDER_CUBE, 15, 0, CUBE_FLAGS | BF_FALLS, 19, 19, 19, 0.6f, MAT_SAND);
    cube(GOLD_ORE, "Gold Ore", 32, 3.0f, MAT_ROCK);
    cube(IRON_ORE, "Iron Ore", 33, 3.0f, MAT_ROCK);
    cube(COAL_ORE, "Coal Ore", 34, 3.0f, MAT_ROCK);
    def(LOG, "Wood", RENDER_CUBE, 15, 0, CUBE_FLAGS, 21, 21, 20, 2.0f, MAT_WOOD);
    def(LEAVES, "Leaves", RENDER_CUBE, 1, 0, BF_SOLID | BF_MAT_SOLID | BF_OPAQUE, 52, 52, 52, 0.2f, MAT_LEAVES);
    cube(SPONGE, "Sponge", 48, 0.6f, MAT_OTHER);
    def(GLASS, "Glass", RENDER_CUBE, 0, 0, BF_SOLID | BF_MAT_SOLID | BF_SAME_CULL, 49, 49, 49, 0.3f, MAT_GLASS);
    cube(LAPIS_ORE, "Lapis Lazuli Ore", 160, 3.0f, MAT_ROCK);
    cube(LAPIS_BLOCK, "Lapis Lazuli Block", 144, 3.0f, MAT_ROCK);
    def(SANDSTONE, "Sandstone", RENDER_CUBE, 15, 0, CUBE_FLAGS, 208, 176, 192, 0.8f, MAT_ROCK);
    def(BED, "Bed", RENDER_BED, 0, 0, BF_SOLID | BF_MAT_SOLID, 134, 134, 134, 0.2f, MAT_CLOTH);
    def(TALL_GRASS, "Grass", RENDER_CROSS, 0, 0, PLANT_FLAGS | BF_REPLACEABLE, 39, 39, 39, 0, MAT_PLANTS);
    def(DEAD_BUSH, "Dead Bush", RENDER_CROSS, 0, 0, PLANT_FLAGS | BF_REPLACEABLE, 55, 55, 55, 0, MAT_PLANTS);
    cube(WOOL, "Wool", 64, 0.8f, MAT_CLOTH);
    def(FLOWER_YELLOW, "Flower", RENDER_CROSS, 0, 0, PLANT_FLAGS, 13, 13, 13, 0, MAT_PLANTS);
    def(FLOWER_RED, "Rose", RENDER_CROSS, 0, 0, PLANT_FLAGS, 12, 12, 12, 0, MAT_PLANTS);
    def(MUSHROOM_BROWN, "Mushroom", RENDER_CROSS, 0, 1, PLANT_FLAGS, 29, 29, 29, 0, MAT_PLANTS);
    def(MUSHROOM_RED, "Mushroom", RENDER_CROSS, 0, 0, PLANT_FLAGS, 28, 28, 28, 0, MAT_PLANTS);
    cube(GOLD_BLOCK, "Block of Gold", 23, 3.0f, MAT_IRON);
    cube(IRON_BLOCK, "Block of Iron", 22, 5.0f, MAT_IRON);
    def(DOUBLE_SLAB, "Stone Slab", RENDER_CUBE, 15, 0, CUBE_FLAGS, 6, 6, 5, 2.0f, MAT_ROCK);
    def(SLAB, "Stone Slab", RENDER_SLAB, 0, 0, BF_SOLID | BF_MAT_SOLID, 6, 6, 5, 2.0f, MAT_ROCK);
    cube(BRICK, "Bricks", 7, 2.0f, MAT_ROCK);
    def(TNT, "TNT", RENDER_CUBE, 15, 0, BF_OPAQUE | BF_SOLID | BF_MAT_SOLID, 10, 9, 8, 0, MAT_OTHER);
    def(BOOKSHELF, "Bookshelf", RENDER_CUBE, 15, 0, CUBE_FLAGS, 4, 4, 35, 1.5f, MAT_WOOD);
    cube(MOSSY_COBBLESTONE, "Moss Stone", 36, 2.0f, MAT_ROCK);
    cube(OBSIDIAN, "Obsidian", 37, 10.0f, MAT_ROCK);
    def(TORCH, "Torch", RENDER_TORCH, 0, 14, 0, 80, 80, 80, 0, MAT_OTHER);
    def(FIRE, "Fire", RENDER_FIRE, 0, 15, BF_REPLACEABLE, 31, 31, 31, 0, MAT_OTHER);
    def(MOB_SPAWNER, "Monster Spawner", RENDER_CUBE, 0, 0, BF_SOLID | BF_MAT_SOLID, 65, 65, 65, 5.0f, MAT_ROCK);
    def(WOOD_STAIRS, "Wooden Stairs", RENDER_STAIRS, 0, 0, BF_SOLID | BF_MAT_SOLID, 4, 4, 4, 2.0f, MAT_WOOD);
    def(CHEST, "Chest", RENDER_CUBE, 15, 0, CUBE_FLAGS, 25, 25, 26, 2.5f, MAT_WOOD);
    cube(DIAMOND_ORE, "Diamond Ore", 50, 3.0f, MAT_ROCK);
    cube(DIAMOND_BLOCK, "Block of Diamond", 24, 5.0f, MAT_IRON);
    def(WORKBENCH, "Crafting Table", RENDER_CUBE, 15, 0, CUBE_FLAGS, 4, 43, 59, 2.5f, MAT_WOOD);
    def(CROPS, "Crops", RENDER_CROPS, 0, 0, PLANT_FLAGS, 88, 88, 88, 0, MAT_PLANTS);
    def(FARMLAND, "Farmland", RENDER_FARMLAND, 15, 0, BF_SOLID | BF_MAT_SOLID | BF_OPAQUE, 2, 87, 2, 0.6f,
        MAT_GROUND);
    def(FURNACE, "Furnace", RENDER_CUBE, 15, 0, CUBE_FLAGS, 62, 62, 45, 3.5f, MAT_ROCK);
    def(FURNACE_LIT, "Furnace", RENDER_CUBE, 15, 13, CUBE_FLAGS, 62, 62, 45, 3.5f, MAT_ROCK);
    def(WOOD_DOOR, "Wooden Door", RENDER_DOOR, 0, 0, BF_SOLID | BF_MAT_SOLID, 97, 97, 97, 3.0f, MAT_WOOD);
    def(LADDER, "Ladder", RENDER_LADDER, 0, 0, BF_SOLID, 83, 83, 83, 0.4f, MAT_OTHER);
    def(COBBLE_STAIRS, "Cobblestone Stairs", RENDER_STAIRS, 0, 0, BF_SOLID | BF_MAT_SOLID, 16, 16, 16, 2.0f,
        MAT_ROCK);
    def(IRON_DOOR, "Iron Door", RENDER_DOOR, 0, 0, BF_SOLID | BF_MAT_SOLID, 98, 98, 98, 5.0f, MAT_IRON);
    cube(REDSTONE_ORE, "Redstone Ore", 51, 3.0f, MAT_ROCK);
    def(REDSTONE_ORE_LIT, "Redstone Ore", RENDER_CUBE, 15, 9, CUBE_FLAGS, 51, 51, 51, 3.0f, MAT_ROCK);
    def(SNOW_LAYER, "Snow", RENDER_SNOW, 0, 0, BF_NEEDS_FLOOR | BF_REPLACEABLE, 66, 66, 66, 0.1f, MAT_SNOW);
    def(ICE, "Ice", RENDER_CUBE, 3, 0, BF_SOLID | BF_MAT_SOLID | BF_TRANSLUCENT | BF_SAME_CULL, 67, 67, 67, 0.5f,
        MAT_ICE);
    cube(SNOW_BLOCK, "Snow", 66, 0.2f, MAT_BUILT_SNOW);
    def(CACTUS, "Cactus", RENDER_CACTUS, 0, 0, BF_SOLID | BF_MAT_SOLID | BF_NEEDS_FLOOR, 71, 69, 70, 0.4f,
        MAT_CACTUS);
    cube(CLAY, "Clay", 72, 0.6f, MAT_CLAY);
    def(REED, "Reeds", RENDER_CROSS, 0, 0, BF_NEEDS_FLOOR, 73, 73, 73, 0, MAT_PLANTS);
    def(FENCE, "Fence", RENDER_FENCE, 0, 0, BF_SOLID | BF_MAT_SOLID, 4, 4, 4, 2.0f, MAT_WOOD);
    def(PUMPKIN, "Pumpkin", RENDER_CUBE, 15, 0, CUBE_FLAGS, 102, 102, 118, 1.0f, MAT_PUMPKIN);
    cube(NETHERRACK, "Netherrack", 103, 0.4f, MAT_ROCK);
    cube(SOUL_SAND, "Soul Sand", 104, 0.5f, MAT_SAND);
    def(GLOWSTONE, "Glowstone", RENDER_CUBE, 15, 15, CUBE_FLAGS, 105, 105, 105, 0.3f, MAT_ROCK);
    def(PORTAL, "Portal", RENDER_PORTAL, 0, 11, BF_TRANSLUCENT | BF_UNBREAKABLE, 14, 14, 14, -1, MAT_OTHER);
    def(JACK_O_LANTERN, "Jack 'o' Lantern", RENDER_CUBE, 15, 15, CUBE_FLAGS, 102, 102, 118, 1.0f, MAT_PUMPKIN);
    def(WEB, "Cobweb", RENDER_CROSS, 1, 0, BF_MAT_SOLID, 11, 11, 11, 4.0f, MAT_WEB);
    def(SIGN_POST, "Sign", RENDER_SIGN, 0, 0, BF_MAT_SOLID, 4, 4, 4, 1.0f, MAT_WOOD);
    def(WALL_SIGN, "Sign", RENDER_SIGN, 0, 0, BF_MAT_SOLID, 4, 4, 4, 1.0f, MAT_WOOD);
    def(JUKEBOX, "Jukebox", RENDER_CUBE, 15, 0, CUBE_FLAGS, 74, 75, 74, 2.0f, MAT_WOOD);
    def(CAKE, "Cake", RENDER_CAKE, 0, 0, BF_SOLID | BF_MAT_SOLID | BF_NEEDS_FLOOR, 124, 121, 122, 0.5f, MAT_CAKE);
    def(TRAPDOOR, "Trapdoor", RENDER_TRAPDOOR, 0, 0, BF_SOLID | BF_MAT_SOLID, 84, 84, 84, 3.0f, MAT_WOOD);

    def(DISPENSER, "Dispenser", RENDER_CUBE, 15, 0, CUBE_FLAGS, 62, 62, 45, 3.5f, MAT_ROCK);
    cube(NOTE_BLOCK, "Note Block", 74, 0.8f, MAT_WOOD);
    def(PISTON, "Piston", RENDER_PISTON, 0, 0, BF_SOLID | BF_MAT_SOLID, 109, 107, 108, 0.5f, MAT_PISTON);
    def(STICKY_PISTON, "Sticky Piston", RENDER_PISTON, 0, 0, BF_SOLID | BF_MAT_SOLID, 109, 106, 108, 0.5f, MAT_PISTON);
    def(PISTON_HEAD, "Piston", RENDER_PISTON_HEAD, 0, 0, BF_SOLID | BF_MAT_SOLID, 107, 107, 108, 0.5f, MAT_PISTON);
    def(PISTON_MOVING, "Piston", RENDER_NONE, 0, 0, BF_SOLID | BF_MAT_SOLID | BF_UNBREAKABLE, 0, 0, 0, -1, MAT_PISTON);
    def(REDSTONE_WIRE, "Redstone Dust", RENDER_WIRE, 0, 0, 0, 164, 164, 164, 0, MAT_CIRCUIT);
    def(LEVER, "Lever", RENDER_LEVER, 0, 0, 0, 96, 96, 96, 0.5f, MAT_CIRCUIT);
    def(STONE_PLATE, "Pressure Plate", RENDER_SHAPE, 0, 0, 0, 1, 1, 1, 0.5f, MAT_ROCK);
    def(WOOD_PLATE, "Pressure Plate", RENDER_SHAPE, 0, 0, 0, 4, 4, 4, 0.5f, MAT_WOOD);
    def(REDSTONE_TORCH_OFF, "Redstone Torch", RENDER_TORCH, 0, 0, 0, 115, 115, 115, 0, MAT_CIRCUIT);
    def(REDSTONE_TORCH_ON, "Redstone Torch", RENDER_TORCH, 0, 7, 0, 99, 99, 99, 0, MAT_CIRCUIT);
    def(STONE_BUTTON, "Button", RENDER_SHAPE, 0, 0, 0, 1, 1, 1, 0.5f, MAT_CIRCUIT);
    def(REPEATER_OFF, "Redstone Repeater", RENDER_REPEATER, 0, 0, BF_SOLID, 115, 131, 5, 0, MAT_CIRCUIT);
    def(REPEATER_ON, "Redstone Repeater", RENDER_REPEATER, 0, 9, BF_SOLID, 99, 147, 5, 0, MAT_CIRCUIT);
    def(RAIL, "Rail", RENDER_RAIL, 0, 0, 0, 128, 128, 128, 0.7f, MAT_CIRCUIT);
    def(POWERED_RAIL, "Powered Rail", RENDER_RAIL, 0, 0, 0, 179, 179, 179, 0.7f, MAT_CIRCUIT);
    def(DETECTOR_RAIL, "Detector Rail", RENDER_RAIL, 0, 0, 0, 195, 195, 195, 0.7f, MAT_CIRCUIT);
    g_blocks[TORCH].material = MAT_CIRCUIT;
    g_blocks[LADDER].material = MAT_CIRCUIT;
}

int blockMobility(uint8_t id) {
    switch (id) {
        case BED: case WOOD_DOOR: case IRON_DOOR: case STONE_PLATE: case WOOD_PLATE: case FIRE: return 1;
        case PORTAL: return 2;
        default: break;
    }
    switch (g_blocks[id].material) {
        case MAT_LIQUID: case MAT_LEAVES: case MAT_PLANTS: case MAT_CIRCUIT: case MAT_SNOW: case MAT_CACTUS:
        case MAT_PUMPKIN: case MAT_CAKE: case MAT_WEB:
            return 1;
        case MAT_PISTON: return 2;
        default: return 0;
    }
}

int blockTexture(uint8_t id, int meta, int side) {
    switch (id) {
        case SAPLING:
            meta &= 3;
            return meta == 1 ? 63 : meta == 2 ? 79 : 15;
        case LOG:
            if (side <= 1) return 21;
            return meta == 1 ? 116 : meta == 2 ? 117 : 20;
        case LEAVES:
            if ((meta & 3) == 1) return g_fancyLeaves ? 132 : 133;
            return g_fancyLeaves ? 52 : 53;
        case TALL_GRASS:
            return meta == 1 ? 39 : meta == 2 ? 56 : 55;
        case WOOL: {
            if (meta == 0) return 64;
            int m = ~(meta & 15);
            return 113 + ((m & 8) >> 3) + (m & 7) * 16;
        }
        case DOUBLE_SLAB:
        case SLAB:
            if (meta == 1) return side <= 1 ? 176 : 192;
            if (meta == 2) return 4;
            if (meta == 3) return 16;
            return side <= 1 ? 6 : 5;
        case WORKBENCH:
            if (side == 1) return 43;
            if (side == 0) return 4;
            return (side == 2 || side == 4) ? 60 : 59;
        case PUMPKIN:
        case JACK_O_LANTERN: {
            if (side <= 1) return 102;
            int face = id == PUMPKIN ? 119 : 120;
            static const int faceSide[4] = {3, 4, 2, 5};
            return side == faceSide[meta & 3] ? face : 118;
        }
        case FURNACE:
        case FURNACE_LIT:
            if (side <= 1) return 62;
            return side == (meta < 2 ? 3 : meta) ? (id == FURNACE ? 44 : 61) : 45;
        case CHEST:
            if (side <= 1) return 25;
            return side == (meta < 2 ? 3 : meta) ? 27 : 26;
        case CROPS:
            return 88 + (meta & 7);
        case FARMLAND:
            if (side == 1) return meta > 0 ? 86 : 87;
            return 2;
        case BED: {
            if (side == 0) return 4;
            static const int BED_SIDE[4][6] = {
                {1, 0, 3, 2, 5, 4}, {1, 0, 5, 4, 2, 3}, {1, 0, 2, 3, 4, 5}, {1, 0, 4, 5, 3, 2}};
            int s = BED_SIDE[meta & 3][side];
            if (meta & 8) return s == 2 ? 152 : (s == 4 || s == 5) ? 151 : 135;
            return s == 3 ? 149 : (s == 4 || s == 5) ? 150 : 134;
        }
        case WOOD_DOOR:
        case IRON_DOOR:
            return (id == WOOD_DOOR ? 97 : 98) - ((meta & 8) ? 16 : 0);
        case CAKE:
            if (side == 1) return 121;
            if (side == 0) return 124;
            return (side == 4 && meta > 0) ? 123 : 122;
        case DISPENSER:
            if (side <= 1) return 62;
            return side == (meta < 2 ? 3 : meta) ? 46 : 45;
        case PISTON:
        case STICKY_PISTON: {
            static const int OPP[6] = {1, 0, 3, 2, 5, 4};
            int dir = meta & 7, front = id == PISTON ? 107 : 106;
            if (dir > 5) return front;
            if (side == dir) return (meta & 8) ? 110 : front;
            return side == OPP[dir] ? 109 : 108;
        }
        case PISTON_HEAD: {
            static const int OPP[6] = {1, 0, 3, 2, 5, 4};
            int dir = meta & 7;
            if (dir > 5) return 107;
            if (side == dir) return (meta & 8) ? 106 : 107;
            return side == OPP[dir] ? 107 : 108;
        }
        case RAIL:
            return meta >= 6 ? 112 : 128;
        case POWERED_RAIL:
            return (meta & 8) ? 179 : 163;
        default:
            return g_blocks[id].tex[side];
    }
}

int blockFireSpread(uint8_t id) {
    switch (id) {
        case PLANKS: case FENCE: case WOOD_STAIRS: case LOG: return 5;
        case LEAVES: case BOOKSHELF: case WOOL: return 30;
        case TNT: return 15;
        case TALL_GRASS: return 60;
        default: return 0;
    }
}

int blockFlammability(uint8_t id) {
    switch (id) {
        case PLANKS: case FENCE: case WOOD_STAIRS: case BOOKSHELF: return 20;
        case LOG: return 5;
        case LEAVES: case WOOL: return 60;
        case TNT: case TALL_GRASS: return 100;
        default: return 0;
    }
}

bool blockMatBurns(uint8_t id) {
    uint8_t m = g_blocks[id].material;
    return m == MAT_WOOD || m == MAT_LEAVES || m == MAT_CLOTH || id == TNT;
}

const StepSound& blockStepSound(uint8_t id) {
    static const StepSound STONE_S{"step.stone", "step.stone", 1.0f, 1.0f};
    static const StepSound WOOD_S{"step.wood", "step.wood", 1.0f, 1.0f};
    static const StepSound GRAVEL_S{"step.gravel", "step.gravel", 1.0f, 1.0f};
    static const StepSound GRASS_S{"step.grass", "step.grass", 1.0f, 1.0f};
    static const StepSound METAL_S{"step.stone", "step.stone", 1.0f, 1.5f};
    static const StepSound GLASS_S{"step.stone", "random.glass", 1.0f, 1.0f};
    static const StepSound CLOTH_S{"step.cloth", "step.cloth", 1.0f, 1.0f};
    static const StepSound SAND_S{"step.sand", "step.gravel", 1.0f, 1.0f};
    switch (id) {
        case GRASS: case SAPLING: case LEAVES: case SPONGE: case TALL_GRASS: case DEAD_BUSH: case FLOWER_YELLOW:
        case FLOWER_RED: case MUSHROOM_BROWN: case MUSHROOM_RED: case TNT: case CROPS: case REED:
            return GRASS_S;
        case DIRT: case GRAVEL: case FARMLAND: case CLAY: return GRAVEL_S;
        case PLANKS: case LOG: case BOOKSHELF: case TORCH: case FIRE: case WOOD_STAIRS: case CHEST: case WORKBENCH:
        case WOOD_DOOR: case LADDER: case FENCE: case PUMPKIN: case JACK_O_LANTERN: case SIGN_POST: case WALL_SIGN:
        case TRAPDOOR: case NOTE_BLOCK: case LEVER: case WOOD_PLATE: case REDSTONE_TORCH_OFF: case REDSTONE_TORCH_ON:
        case REPEATER_OFF: case REPEATER_ON:
            return WOOD_S;
        case GOLD_BLOCK: case IRON_BLOCK: case DIAMOND_BLOCK: case IRON_DOOR: case MOB_SPAWNER: case RAIL:
        case POWERED_RAIL: case DETECTOR_RAIL:
            return METAL_S;
        case GLASS: case ICE: case GLOWSTONE: case PORTAL: return GLASS_S;
        case WOOL: case SNOW_LAYER: case SNOW_BLOCK: case CACTUS: case CAKE: return CLOTH_S;
        case SAND: case SOUL_SAND: return SAND_S;
        default: return STONE_S;
    }
}
