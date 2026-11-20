#pragma once
#include <cstdint>

// Ids below 256 are blocks; items use Beta's ids (256 + index).
enum ItemId : uint16_t {
    I_IRON_SHOVEL = 256,
    I_IRON_PICKAXE = 257,
    I_IRON_AXE = 258,
    I_FLINT_AND_STEEL = 259,
    I_APPLE = 260,
    I_BOW = 261,
    I_ARROW = 262,
    I_COAL = 263,
    I_DIAMOND = 264,
    I_IRON_INGOT = 265,
    I_GOLD_INGOT = 266,
    I_IRON_SWORD = 267,
    I_WOOD_SWORD = 268,
    I_WOOD_SHOVEL = 269,
    I_WOOD_PICKAXE = 270,
    I_WOOD_AXE = 271,
    I_STONE_SWORD = 272,
    I_STONE_SHOVEL = 273,
    I_STONE_PICKAXE = 274,
    I_STONE_AXE = 275,
    I_DIAMOND_SWORD = 276,
    I_DIAMOND_SHOVEL = 277,
    I_DIAMOND_PICKAXE = 278,
    I_DIAMOND_AXE = 279,
    I_STICK = 280,
    I_BOWL = 281,
    I_MUSHROOM_STEW = 282,
    I_GOLD_SWORD = 283,
    I_GOLD_SHOVEL = 284,
    I_GOLD_PICKAXE = 285,
    I_GOLD_AXE = 286,
    I_STRING = 287,
    I_FEATHER = 288,
    I_GUNPOWDER = 289,
    I_WOOD_HOE = 290,
    I_STONE_HOE = 291,
    I_IRON_HOE = 292,
    I_DIAMOND_HOE = 293,
    I_GOLD_HOE = 294,
    I_SEEDS = 295,
    I_WHEAT = 296,
    I_BREAD = 297,
    I_LEATHER_HELMET = 298,  // 298..317: helmet, chestplate, leggings, boots for leather, chain, iron, diamond, gold
    I_GOLD_BOOTS = 317,
    I_FLINT = 318,
    I_PORK_RAW = 319,
    I_PORK_COOKED = 320,
    I_PAINTING = 321,
    I_GOLDEN_APPLE = 322,
    I_SIGN = 323,
    I_WOOD_DOOR = 324,
    I_BUCKET = 325,
    I_WATER_BUCKET = 326,
    I_LAVA_BUCKET = 327,
    I_MINECART = 328,
    I_SADDLE = 329,
    I_IRON_DOOR = 330,
    I_REDSTONE = 331,
    I_SNOWBALL = 332,
    I_BOAT = 333,
    I_LEATHER = 334,
    I_MILK_BUCKET = 335,
    I_BRICK = 336,
    I_CLAY_BALL = 337,
    I_REEDS = 338,
    I_PAPER = 339,
    I_BOOK = 340,
    I_SLIMEBALL = 341,
    I_CHEST_MINECART = 342,
    I_FURNACE_MINECART = 343,
    I_EGG = 344,
    I_COMPASS = 345,
    I_FISHING_ROD = 346,
    I_CLOCK = 347,
    I_GLOWSTONE_DUST = 348,
    I_FISH_RAW = 349,
    I_FISH_COOKED = 350,
    I_DYE = 351,
    I_BONE = 352,
    I_SUGAR = 353,
    I_CAKE = 354,
    I_BED = 355,
    I_REPEATER = 356,
    I_COOKIE = 357,
    I_SHEARS = 359,
    I_RECORD_13 = 2256,
    I_RECORD_CAT = 2257,
    ITEM_ID_LIMIT = 2258,
};

enum ToolType : uint8_t { TOOL_NONE, TOOL_PICKAXE, TOOL_SHOVEL, TOOL_AXE, TOOL_SWORD, TOOL_HOE };
enum ToolMaterial : uint8_t { TM_WOOD, TM_STONE, TM_IRON, TM_DIAMOND, TM_GOLD };

struct ItemDef {
    const char* name = nullptr;  // null: unused id
    uint8_t icon = 0;            // index in gui/items.png
    uint8_t maxStack = 64;
    uint16_t maxDamage = 0;      // durability, 0 = none
    uint8_t tool = TOOL_NONE;
    uint8_t material = 0;
    int8_t armorSlot = -1;       // 0 helmet, 1 chestplate, 2 leggings, 3 boots
    uint8_t armorPoints = 0;
    uint8_t food = 0;            // health restored when eaten
    uint8_t places = 0;          // block placed by this item (doors, reeds, seeds), 0 = none
};

struct ItemStack {
    uint16_t id = 0;
    uint8_t count = 0;
    uint16_t damage = 0;  // durability used, or block/item metadata

    ItemStack() = default;
    ItemStack(uint16_t i, int c = 1, int d = 0) : id(i), count((uint8_t)c), damage((uint16_t)d) {}
    bool empty() const { return id == 0 || count == 0; }
    bool sameItem(const ItemStack& o) const { return id == o.id && damage == o.damage; }
    void clear() { id = 0, count = 0, damage = 0; }
};

extern ItemDef g_items[ITEM_ID_LIMIT];

void initItems();  // after initBlocks()
bool isBlockItem(uint16_t id);
bool itemExists(uint16_t id);
const char* itemName(const ItemStack& s);
int itemMaxStack(uint16_t id);
inline const ItemDef& itemDef(uint16_t id) { return g_items[id < ITEM_ID_LIMIT ? id : 0]; }
// Icon index in items.png for a non-block item (dye and similar items depend on damage).
int itemIcon(const ItemStack& s);

// Mining and combat (EnumToolMaterial, ItemTool, ItemSword).
float toolStrength(const ItemStack& held, uint8_t block);
bool canHarvest(const ItemStack& held, uint8_t block);
int attackDamage(const ItemStack& held);
int toolMaxUses(int material);

extern const ItemStack g_creativeItems[];
extern const int g_creativeItemCount;
