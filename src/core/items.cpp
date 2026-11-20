#include "items.h"

#include "blocks.h"

ItemDef g_items[ITEM_ID_LIMIT];

namespace {

ItemDef& item(uint16_t id, const char* name, int iconX, int iconY, int maxStack = 64) {
    ItemDef& d = g_items[id];
    d = ItemDef();
    d.name = name;
    d.icon = (uint8_t)(iconY * 16 + iconX);
    d.maxStack = (uint8_t)maxStack;
    return d;
}

const int MAX_USES[5] = {59, 131, 250, 1561, 32};
const float EFFICIENCY[5] = {2.0f, 4.0f, 6.0f, 8.0f, 12.0f};
const int HARVEST_LEVEL[5] = {0, 1, 2, 3, 0};
const int ENTITY_DAMAGE[5] = {0, 1, 2, 3, 0};

void tool(uint16_t id, const char* name, int iconX, int iconY, uint8_t type, uint8_t material) {
    ItemDef& d = item(id, name, iconX, iconY, 1);
    d.tool = type;
    d.material = material;
    d.maxDamage = (uint16_t)MAX_USES[material];
}

void food(uint16_t id, const char* name, int iconX, int iconY, int heal, int maxStack = 64) {
    item(id, name, iconX, iconY, maxStack).food = (uint8_t)heal;
}

bool inList(uint8_t block, const uint8_t* list, int n) {
    for (int i = 0; i < n; ++i)
        if (list[i] == block) return true;
    return false;
}

const uint8_t PICKAXE_BLOCKS[] = {COBBLESTONE, DOUBLE_SLAB, SLAB,      STONE,       SANDSTONE,     MOSSY_COBBLESTONE,
                                  IRON_ORE,    IRON_BLOCK,  COAL_ORE,  GOLD_BLOCK,  GOLD_ORE,      DIAMOND_ORE,
                                  DIAMOND_BLOCK, ICE,       NETHERRACK, LAPIS_ORE,  LAPIS_BLOCK};
const uint8_t SHOVEL_BLOCKS[] = {GRASS, DIRT, SAND, GRAVEL, SNOW_LAYER, SNOW_BLOCK, CLAY, FARMLAND};
const uint8_t AXE_BLOCKS[] = {PLANKS, BOOKSHELF, LOG, CHEST};

}  // namespace

void initItems() {
    for (auto& d : g_items) d = ItemDef();
    for (int id = 1; id < 256; ++id) {
        if (g_blocks[id].name && g_blocks[id].name[0] != '?') {
            g_items[id].name = g_blocks[id].name;
            g_items[id].maxStack = 64;
        }
    }
    const char* MAT_NAMES[5] = {"Wooden", "Stone", "Iron", "Diamond", "Gold"};
    static char names[5][5][24];
    const uint16_t SHOVELS[5] = {I_WOOD_SHOVEL, I_STONE_SHOVEL, I_IRON_SHOVEL, I_DIAMOND_SHOVEL, I_GOLD_SHOVEL};
    const uint16_t PICKS[5] = {I_WOOD_PICKAXE, I_STONE_PICKAXE, I_IRON_PICKAXE, I_DIAMOND_PICKAXE, I_GOLD_PICKAXE};
    const uint16_t AXES[5] = {I_WOOD_AXE, I_STONE_AXE, I_IRON_AXE, I_DIAMOND_AXE, I_GOLD_AXE};
    const uint16_t SWORDS[5] = {I_WOOD_SWORD, I_STONE_SWORD, I_IRON_SWORD, I_DIAMOND_SWORD, I_GOLD_SWORD};
    const uint16_t HOES[5] = {I_WOOD_HOE, I_STONE_HOE, I_IRON_HOE, I_DIAMOND_HOE, I_GOLD_HOE};
    const char* KINDS[5] = {"Shovel", "Pickaxe", "Axe", "Sword", "Hoe"};
    const uint16_t* IDS[5] = {SHOVELS, PICKS, AXES, SWORDS, HOES};
    const uint8_t TYPES[5] = {TOOL_SHOVEL, TOOL_PICKAXE, TOOL_AXE, TOOL_SWORD, TOOL_HOE};
    const int ICON_ROW[5] = {5, 6, 7, 4, 8};
    for (int k = 0; k < 5; ++k)
        for (int m = 0; m < 5; ++m) {
            char* n = names[k][m];
            const char* a = MAT_NAMES[m];
            int p = 0;
            while (*a) n[p++] = *a++;
            n[p++] = ' ';
            for (const char* b = KINDS[k]; *b;) n[p++] = *b++;
            n[p] = 0;
            tool(IDS[k][m], n, m, ICON_ROW[k], TYPES[k], (uint8_t)m);
        }

    item(I_FLINT_AND_STEEL, "Flint and Steel", 5, 0, 1).maxDamage = 64;
    food(I_APPLE, "Apple", 10, 0, 4, 1);
    item(I_BOW, "Bow", 5, 1, 1);
    item(I_ARROW, "Arrow", 5, 2);
    item(I_COAL, "Coal", 7, 0);
    item(I_DIAMOND, "Diamond", 7, 3);
    item(I_IRON_INGOT, "Iron Ingot", 7, 1);
    item(I_GOLD_INGOT, "Gold Ingot", 7, 2);
    item(I_STICK, "Stick", 5, 3);
    item(I_BOWL, "Bowl", 7, 4);
    food(I_MUSHROOM_STEW, "Mushroom Stew", 8, 4, 10, 1);
    item(I_STRING, "String", 8, 0);
    item(I_FEATHER, "Feather", 8, 1);
    item(I_GUNPOWDER, "Gunpowder", 8, 2);
    item(I_SEEDS, "Seeds", 9, 0).places = CROPS;
    item(I_WHEAT, "Wheat", 9, 1);
    food(I_BREAD, "Bread", 9, 2, 5, 1);

    static const char* ARMOR_NAMES[5][4] = {
        {"Leather Cap", "Leather Tunic", "Leather Pants", "Leather Boots"},
        {"Chain Helmet", "Chain Chestplate", "Chain Leggings", "Chain Boots"},
        {"Iron Helmet", "Iron Chestplate", "Iron Leggings", "Iron Boots"},
        {"Diamond Helmet", "Diamond Chestplate", "Diamond Leggings", "Diamond Boots"},
        {"Gold Helmet", "Gold Chestplate", "Gold Leggings", "Gold Boots"},
    };
    const int ARMOR_DURABILITY[4] = {11, 16, 15, 13};
    const int ARMOR_POINTS[4] = {3, 8, 6, 3};
    const int ARMOR_TIER[5] = {0, 1, 2, 3, 1};
    for (int m = 0; m < 5; ++m)
        for (int s = 0; s < 4; ++s) {
            ItemDef& d = item((uint16_t)(I_LEATHER_HELMET + m * 4 + s), ARMOR_NAMES[m][s], m, s, 1);
            d.armorSlot = (int8_t)s;
            d.armorPoints = (uint8_t)ARMOR_POINTS[s];
            d.maxDamage = (uint16_t)((ARMOR_DURABILITY[s] * 3) << ARMOR_TIER[m]);
        }

    item(I_FLINT, "Flint", 6, 0);
    food(I_PORK_RAW, "Raw Porkchop", 7, 5, 3, 1);
    food(I_PORK_COOKED, "Cooked Porkchop", 8, 5, 8, 1);
    food(I_GOLDEN_APPLE, "Golden Apple", 11, 0, 42, 1);
    item(I_WOOD_DOOR, "Wooden Door", 11, 2, 1).places = WOOD_DOOR;
    item(I_BUCKET, "Bucket", 10, 4, 1);
    item(I_WATER_BUCKET, "Water Bucket", 11, 4, 1);
    item(I_LAVA_BUCKET, "Lava Bucket", 12, 4, 1);
    item(I_MILK_BUCKET, "Milk", 13, 4, 1);
    item(I_SHEARS, "Shears", 13, 5, 1).maxDamage = 238;
    item(I_IRON_DOOR, "Iron Door", 12, 2, 1).places = IRON_DOOR;
    item(I_REDSTONE, "Redstone", 8, 3).places = REDSTONE_WIRE;
    item(I_SNOWBALL, "Snowball", 14, 0, 16);
    item(I_LEATHER, "Leather", 7, 6);
    item(I_BRICK, "Brick", 6, 1);
    item(I_CLAY_BALL, "Clay", 9, 3);
    item(I_REEDS, "Sugar Cane", 11, 1).places = REED;
    item(I_PAPER, "Paper", 10, 3);
    item(I_BOOK, "Book", 11, 3);
    item(I_EGG, "Egg", 12, 0, 16);
    item(I_GLOWSTONE_DUST, "Glowstone Dust", 9, 4);
    food(I_FISH_RAW, "Raw Fish", 9, 5, 2, 1);
    food(I_FISH_COOKED, "Cooked Fish", 10, 5, 5, 1);
    item(I_DYE, "Dye", 14, 4);
    item(I_BONE, "Bone", 12, 1);
    item(I_SUGAR, "Sugar", 13, 0);
    item(I_BED, "Bed", 13, 2, 1).places = BED;
    food(I_COOKIE, "Cookie", 12, 5, 1, 8);
    item(I_SADDLE, "Saddle", 8, 6, 1);
    item(I_MINECART, "Minecart", 7, 8, 1);
    item(I_CHEST_MINECART, "Minecart with Chest", 7, 9, 1);
    item(I_FURNACE_MINECART, "Minecart with Furnace", 7, 10, 1);
    item(I_BOAT, "Boat", 8, 8, 1);
    item(I_SLIMEBALL, "Slimeball", 14, 1);
    item(I_RECORD_13, "Music Disc", 0, 15, 1);
    item(I_RECORD_CAT, "Music Disc", 1, 15, 1);
    item(I_PAINTING, "Painting", 10, 1);
    item(I_SIGN, "Sign", 10, 2, 1);
    item(I_COMPASS, "Compass", 6, 3);
    item(I_FISHING_ROD, "Fishing Rod", 5, 4, 1).maxDamage = 64;
    item(I_CLOCK, "Clock", 6, 4);
    item(I_CAKE, "Cake", 13, 1, 1).places = CAKE;
    item(I_REPEATER, "Redstone Repeater", 6, 5).places = REPEATER_OFF;
    g_items[SIGN_POST].name = g_items[WALL_SIGN].name = g_items[CAKE].name = nullptr;
    static const uint8_t HIDDEN[] = {PISTON_HEAD, PISTON_MOVING, REDSTONE_WIRE, REDSTONE_TORCH_OFF, REPEATER_OFF,
                                     REPEATER_ON};
    for (uint8_t id : HIDDEN) g_items[id].name = nullptr;
}

bool isBlockItem(uint16_t id) { return id > 0 && id < 256; }
bool itemExists(uint16_t id) { return id > 0 && id < ITEM_ID_LIMIT && g_items[id].name; }
int itemMaxStack(uint16_t id) { return itemExists(id) ? g_items[id].maxStack : 64; }
int toolMaxUses(int material) { return MAX_USES[material]; }

int itemIcon(const ItemStack& s) {
    const ItemDef& d = itemDef(s.id);
    if (s.id == I_DYE) return d.icon + (s.damage % 8) * 16 + s.damage / 8;
    return d.icon;
}

const char* itemName(const ItemStack& s) {
    static const char* WOOL_NAMES[16] = {"White Wool", "Orange Wool", "Magenta Wool", "Light Blue Wool",
                                         "Yellow Wool", "Lime Wool", "Pink Wool", "Gray Wool",
                                         "Light Gray Wool", "Cyan Wool", "Purple Wool", "Blue Wool",
                                         "Brown Wool", "Green Wool", "Red Wool", "Black Wool"};
    static const char* DYE_NAMES[16] = {"Ink Sac", "Rose Red", "Cactus Green", "Cocoa Beans",
                                        "Lapis Lazuli", "Purple Dye", "Cyan Dye", "Light Gray Dye",
                                        "Gray Dye", "Pink Dye", "Lime Dye", "Dandelion Yellow",
                                        "Light Blue Dye", "Magenta Dye", "Orange Dye", "Bone Meal"};
    int m = s.damage;
    switch (s.id) {
        case WOOL: return WOOL_NAMES[m & 15];
        case LOG: return m == 1 ? "Spruce Wood" : m == 2 ? "Birch Wood" : "Wood";
        case LEAVES: return (m & 3) == 1 ? "Spruce Leaves" : (m & 3) == 2 ? "Birch Leaves" : "Leaves";
        case SAPLING: return (m & 3) == 1 ? "Spruce Sapling" : (m & 3) == 2 ? "Birch Sapling" : "Sapling";
        case TALL_GRASS: return m == 2 ? "Fern" : "Tall Grass";
        case SLAB:
        case DOUBLE_SLAB:
            return m == 1 ? "Sandstone Slab" : m == 2 ? "Wooden Slab" : m == 3 ? "Cobblestone Slab" : "Stone Slab";
        case I_COAL: return m == 1 ? "Charcoal" : "Coal";
        case I_DYE: return DYE_NAMES[m & 15];
        default: {
            const ItemDef& d = itemDef(s.id);
            return d.name ? d.name : "?";
        }
    }
}

float toolStrength(const ItemStack& held, uint8_t block) {
    if (held.empty()) return 1.0f;
    if (held.id == I_SHEARS) return (block == LEAVES || block == WEB) ? 15.0f : block == WOOL ? 5.0f : 1.0f;
    const ItemDef& d = itemDef(held.id);
    switch (d.tool) {
        case TOOL_PICKAXE:
            return inList(block, PICKAXE_BLOCKS, sizeof(PICKAXE_BLOCKS)) ? EFFICIENCY[d.material] : 1.0f;
        case TOOL_SHOVEL:
            return inList(block, SHOVEL_BLOCKS, sizeof(SHOVEL_BLOCKS)) ? EFFICIENCY[d.material] : 1.0f;
        case TOOL_AXE: return inList(block, AXE_BLOCKS, sizeof(AXE_BLOCKS)) ? EFFICIENCY[d.material] : 1.0f;
        case TOOL_SWORD: return block == WEB ? 15.0f : 1.5f;
        default: return 1.0f;
    }
}

bool canHarvest(const ItemStack& held, uint8_t block) {
    uint8_t m = g_blocks[block].material;
    if (m == MAT_WEB) return held.id == I_SHEARS || itemDef(held.id).tool == TOOL_SWORD;
    bool needsTool = m == MAT_ROCK || m == MAT_IRON || m == MAT_SNOW || m == MAT_BUILT_SNOW;
    if (!needsTool) return true;
    if (held.empty()) return false;
    const ItemDef& d = itemDef(held.id);
    if (d.tool == TOOL_SHOVEL) return block == SNOW_LAYER || block == SNOW_BLOCK;
    if (d.tool != TOOL_PICKAXE) return false;
    int level = HARVEST_LEVEL[d.material];
    if (block == OBSIDIAN) return level == 3;
    if (block == DIAMOND_BLOCK || block == DIAMOND_ORE || block == GOLD_BLOCK || block == GOLD_ORE ||
        block == REDSTONE_ORE || block == REDSTONE_ORE_LIT)
        return level >= 2;
    if (block == IRON_BLOCK || block == IRON_ORE || block == LAPIS_BLOCK || block == LAPIS_ORE) return level >= 1;
    return m == MAT_ROCK || m == MAT_IRON;
}

int attackDamage(const ItemStack& held) {
    if (held.empty()) return 1;
    const ItemDef& d = itemDef(held.id);
    switch (d.tool) {
        case TOOL_SWORD: return 4 + ENTITY_DAMAGE[d.material] * 2;
        case TOOL_PICKAXE: return 2 + ENTITY_DAMAGE[d.material];
        case TOOL_SHOVEL: return 1 + ENTITY_DAMAGE[d.material];
        case TOOL_AXE: return 3 + ENTITY_DAMAGE[d.material];
        default: return 1;
    }
}

const ItemStack g_creativeItems[] = {
    {STONE},          {GRASS},          {DIRT},           {COBBLESTONE},    {PLANKS},         {LOG},
    {LOG, 1, 1},      {LOG, 1, 2},      {LEAVES},         {LEAVES, 1, 1},   {LEAVES, 1, 2},   {SAND},
    {GRAVEL},         {SANDSTONE},      {GLASS},          {BRICK},          {MOSSY_COBBLESTONE}, {OBSIDIAN},
    {BEDROCK},        {COAL_ORE},       {IRON_ORE},       {GOLD_ORE},       {DIAMOND_ORE},    {REDSTONE_ORE},
    {LAPIS_ORE},      {IRON_BLOCK},     {GOLD_BLOCK},     {DIAMOND_BLOCK},  {LAPIS_BLOCK},    {SLAB},
    {SLAB, 1, 1},     {SLAB, 1, 2},     {SLAB, 1, 3},     {DOUBLE_SLAB},    {WOOD_STAIRS},    {COBBLE_STAIRS},
    {BOOKSHELF},      {WORKBENCH},      {FURNACE},        {CHEST},          {LADDER},         {FENCE},
    {I_WOOD_DOOR},    {I_IRON_DOOR},    {I_BED},    {TNT},            {SPONGE},         {TORCH},          {GLOWSTONE},
    {PUMPKIN},        {JACK_O_LANTERN}, {SNOW_BLOCK},     {SNOW_LAYER},     {ICE},            {CLAY},
    {CACTUS},         {I_REEDS},        {NETHERRACK},     {SOUL_SAND},      {SAPLING},        {SAPLING, 1, 1},
    {SAPLING, 1, 2},  {TALL_GRASS, 1, 1}, {TALL_GRASS, 1, 2}, {DEAD_BUSH},  {FLOWER_YELLOW},  {FLOWER_RED},
    {MUSHROOM_BROWN}, {MUSHROOM_RED},   {I_SEEDS},        {WATER},          {LAVA},           {WOOL},
    {WOOL, 1, 1},     {WOOL, 1, 2},     {WOOL, 1, 3},     {WOOL, 1, 4},     {WOOL, 1, 5},     {WOOL, 1, 6},
    {WOOL, 1, 7},     {WOOL, 1, 8},     {WOOL, 1, 9},     {WOOL, 1, 10},    {WOOL, 1, 11},    {WOOL, 1, 12},
    {WOOL, 1, 13},    {WOOL, 1, 14},    {WOOL, 1, 15},    {I_DIAMOND_PICKAXE}, {I_DIAMOND_SHOVEL}, {I_DIAMOND_AXE},
    {I_DIAMOND_SWORD}, {I_BOW},         {I_ARROW, 64},    {I_DYE, 1, 15},
    {I_REDSTONE},     {REDSTONE_TORCH_ON}, {I_REPEATER},  {LEVER},          {STONE_BUTTON},   {STONE_PLATE},
    {WOOD_PLATE},     {PISTON},         {STICKY_PISTON},  {DISPENSER},      {NOTE_BLOCK},     {I_SNOWBALL, 16},
    {RAIL},           {POWERED_RAIL},   {DETECTOR_RAIL},  {I_MINECART},     {I_CHEST_MINECART}, {I_FURNACE_MINECART},
    {I_BOAT},         {I_SADDLE},
};
const int g_creativeItemCount = sizeof(g_creativeItems) / sizeof(g_creativeItems[0]);
