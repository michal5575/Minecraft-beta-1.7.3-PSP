// CraftingManager, Recipes*.java, FurnaceRecipes and TileEntityFurnace.getItemBurnTime from Beta 1.7.3.
#include "crafting.h"

#include <cstring>
#include <initializer_list>

#include "blocks.h"

namespace {

struct Ingredient {
    uint16_t id;
    int16_t damage;  // -1: any
};

struct Recipe {
    uint8_t w, h;
    bool shapeless;
    uint8_t count;  // ingredients for shapeless recipes
    Ingredient cells[9];
    ItemStack result;
};

constexpr int MAX_RECIPES = 256;
Recipe g_recipes[MAX_RECIPES];
int g_recipeCount = 0;

struct Key {
    char c;
    uint16_t id;
    int16_t damage;
};

// Blocks given directly accept any damage value (as CraftingManager.addRecipe does); items use damage 0.
Key K(char c, uint16_t id) { return {c, id, (int16_t)(id < 256 ? -1 : 0)}; }
Key K(char c, uint16_t id, int damage) { return {c, id, (int16_t)damage}; }

void shaped(ItemStack result, const char* r0, const char* r1, const char* r2, std::initializer_list<Key> keys) {
    if (g_recipeCount >= MAX_RECIPES) return;
    Recipe& r = g_recipes[g_recipeCount++];
    r = Recipe();
    const char* rows[3] = {r0, r1, r2};
    r.h = 0;
    r.w = 0;
    for (int i = 0; i < 3; ++i)
        if (rows[i]) {
            r.h = (uint8_t)(i + 1);
            int len = (int)std::strlen(rows[i]);
            if (len > r.w) r.w = (uint8_t)len;
        }
    for (int y = 0; y < r.h; ++y)
        for (int x = 0; x < r.w; ++x) {
            char ch = x < (int)std::strlen(rows[y]) ? rows[y][x] : ' ';
            Ingredient in = {0, 0};
            for (const Key& k : keys)
                if (k.c == ch) in = {k.id, k.damage};
            r.cells[y * r.w + x] = in;
        }
    r.result = result;
}

void shaped(ItemStack result, const char* r0, std::initializer_list<Key> keys) { shaped(result, r0, nullptr, nullptr, keys); }
void shaped(ItemStack result, const char* r0, const char* r1, std::initializer_list<Key> keys) {
    shaped(result, r0, r1, nullptr, keys);
}

void shapeless(ItemStack result, std::initializer_list<Ingredient> items) {
    if (g_recipeCount >= MAX_RECIPES) return;
    Recipe& r = g_recipes[g_recipeCount++];
    r = Recipe();
    r.shapeless = true;
    for (const Ingredient& i : items)
        if (r.count < 9) r.cells[r.count++] = i;
    r.result = result;
}

bool ingredientMatches(const Ingredient& in, const ItemStack& s) {
    if (in.id == 0) return s.empty();
    if (s.empty() || s.id != in.id) return false;
    return in.damage < 0 || in.damage == (int16_t)s.damage;
}

bool matchAt(const Recipe& r, const ItemStack* grid, int size, int ox, int oy, bool mirror) {
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            int rx = x - ox, ry = y - oy;
            Ingredient in = {0, 0};
            if (rx >= 0 && ry >= 0 && rx < r.w && ry < r.h) in = r.cells[ry * r.w + (mirror ? r.w - rx - 1 : rx)];
            if (!ingredientMatches(in, grid[y * size + x])) return false;
        }
    return true;
}

bool matches(const Recipe& r, const ItemStack* grid, int size) {
    if (r.shapeless) {
        bool used[9] = {};
        int n = 0;
        for (int i = 0; i < size * size; ++i) {
            const ItemStack& s = grid[i];
            if (s.empty()) continue;
            ++n;
            bool found = false;
            for (int k = 0; k < r.count; ++k)
                if (!used[k] && ingredientMatches(r.cells[k], s)) {
                    used[k] = true;
                    found = true;
                    break;
                }
            if (!found) return false;
        }
        return n == r.count;
    }
    if (r.w > size || r.h > size) return false;
    for (int oy = 0; oy <= size - r.h; ++oy)
        for (int ox = 0; ox <= size - r.w; ++ox)
            if (matchAt(r, grid, size, ox, oy, true) || matchAt(r, grid, size, ox, oy, false)) return true;
    return false;
}

}  // namespace

void initRecipes() {
    g_recipeCount = 0;
    // RecipesTools
    const uint16_t MATS[5] = {PLANKS, COBBLESTONE, I_IRON_INGOT, I_DIAMOND, I_GOLD_INGOT};
    const uint16_t PICK[5] = {I_WOOD_PICKAXE, I_STONE_PICKAXE, I_IRON_PICKAXE, I_DIAMOND_PICKAXE, I_GOLD_PICKAXE};
    const uint16_t SHOVEL[5] = {I_WOOD_SHOVEL, I_STONE_SHOVEL, I_IRON_SHOVEL, I_DIAMOND_SHOVEL, I_GOLD_SHOVEL};
    const uint16_t AXE[5] = {I_WOOD_AXE, I_STONE_AXE, I_IRON_AXE, I_DIAMOND_AXE, I_GOLD_AXE};
    const uint16_t HOE[5] = {I_WOOD_HOE, I_STONE_HOE, I_IRON_HOE, I_DIAMOND_HOE, I_GOLD_HOE};
    const uint16_t SWORD[5] = {I_WOOD_SWORD, I_STONE_SWORD, I_IRON_SWORD, I_DIAMOND_SWORD, I_GOLD_SWORD};
    for (int m = 0; m < 5; ++m) {
        Key x = K('X', MATS[m]), s = K('#', I_STICK);
        shaped(ItemStack(PICK[m]), "XXX", " # ", " # ", {x, s});
        shaped(ItemStack(SHOVEL[m]), "X", "#", "#", {x, s});
        shaped(ItemStack(AXE[m]), "XX", "X#", " #", {x, s});
        shaped(ItemStack(HOE[m]), "XX", " #", " #", {x, s});
        shaped(ItemStack(SWORD[m]), "X", "X", "#", {x, s});
    }
    shaped(ItemStack(I_SHEARS), " #", "# ", {K('#', I_IRON_INGOT)});
    // RecipesWeapons
    shaped(ItemStack(I_BOW), " #X", "# X", " #X", {K('X', I_STRING), K('#', I_STICK)});
    shaped(ItemStack(I_ARROW, 4), "X", "#", "Y", {K('Y', I_FEATHER), K('X', I_FLINT), K('#', I_STICK)});
    // RecipesArmor: leather, fire (chain), iron, diamond, gold
    const uint16_t ARMOR_MATS[5] = {I_LEATHER, 0, I_IRON_INGOT, I_DIAMOND, I_GOLD_INGOT};
    for (int m = 0; m < 5; ++m) {
        if (!ARMOR_MATS[m]) continue;  // chain armor needs fire blocks, which are not in this port
        Key x = K('X', ARMOR_MATS[m]);
        uint16_t base = (uint16_t)(I_LEATHER_HELMET + m * 4);
        shaped(ItemStack(base), "XXX", "X X", {x});
        shaped(ItemStack(base + 1), "X X", "XXX", "XXX", {x});
        shaped(ItemStack(base + 2), "XXX", "X X", "X X", {x});
        shaped(ItemStack(base + 3), "X X", "X X", {x});
    }
    // RecipesIngots
    const uint16_t BLOCKS[4] = {GOLD_BLOCK, IRON_BLOCK, DIAMOND_BLOCK, LAPIS_BLOCK};
    const ItemStack INGOTS[4] = {ItemStack(I_GOLD_INGOT, 9), ItemStack(I_IRON_INGOT, 9), ItemStack(I_DIAMOND, 9),
                                 ItemStack(I_DYE, 9, 4)};
    for (int i = 0; i < 4; ++i) {
        Key in = {'#', INGOTS[i].id, (int16_t)INGOTS[i].damage};
        shaped(ItemStack(BLOCKS[i]), "###", "###", "###", {in});
        shaped(INGOTS[i], "#", {K('#', BLOCKS[i])});
    }
    // RecipesFood
    shapeless(ItemStack(I_MUSHROOM_STEW), {{MUSHROOM_BROWN, 0}, {MUSHROOM_RED, 0}, {I_BOWL, 0}});
    shaped(ItemStack(I_COOKIE, 8), "#X#", {K('X', I_DYE, 3), K('#', I_WHEAT)});
    shaped(ItemStack(I_BREAD), "###", {K('#', I_WHEAT)});
    shaped(ItemStack(I_GOLDEN_APPLE), "###", "#X#", "###", {K('#', GOLD_BLOCK), K('X', I_APPLE)});
    // RecipesCrafting
    shaped(ItemStack(CHEST), "###", "# #", "###", {K('#', PLANKS)});
    shaped(ItemStack(FURNACE), "###", "# #", "###", {K('#', COBBLESTONE)});
    shaped(ItemStack(WORKBENCH), "##", "##", {K('#', PLANKS)});
    shaped(ItemStack(SANDSTONE), "##", "##", {K('#', SAND)});
    // RecipesDyes
    for (int d = 0; d < 16; ++d)
        shapeless(ItemStack(WOOL, 1, 15 - d), {{I_DYE, (int16_t)d}, {WOOL, 0}});
    shapeless(ItemStack(I_DYE, 2, 11), {{FLOWER_YELLOW, 0}});
    shapeless(ItemStack(I_DYE, 2, 1), {{FLOWER_RED, 0}});
    shapeless(ItemStack(I_DYE, 3, 15), {{I_BONE, 0}});
    // CraftingManager
    shaped(ItemStack(I_PAPER, 3), "###", {K('#', I_REEDS)});
    shaped(ItemStack(I_BOOK), "#", "#", "#", {K('#', I_PAPER)});
    shaped(ItemStack(FENCE, 2), "###", "###", {K('#', I_STICK)});
    shaped(ItemStack(BOOKSHELF), "###", "XXX", "###", {K('#', PLANKS), K('X', I_BOOK)});
    shaped(ItemStack(SNOW_BLOCK), "##", "##", {K('#', I_SNOWBALL)});
    shaped(ItemStack(CLAY), "##", "##", {K('#', I_CLAY_BALL)});
    shaped(ItemStack(BRICK), "##", "##", {K('#', I_BRICK)});
    shaped(ItemStack(GLOWSTONE), "##", "##", {K('#', I_GLOWSTONE_DUST)});
    shaped(ItemStack(WOOL), "##", "##", {K('#', I_STRING)});
    shaped(ItemStack(TNT), "X#X", "#X#", "X#X", {K('X', I_GUNPOWDER), K('#', SAND)});
    shaped(ItemStack(SLAB, 3, 3), "###", {K('#', COBBLESTONE)});
    shaped(ItemStack(SLAB, 3, 0), "###", {K('#', STONE)});
    shaped(ItemStack(SLAB, 3, 1), "###", {K('#', SANDSTONE)});
    shaped(ItemStack(SLAB, 3, 2), "###", {K('#', PLANKS)});
    shaped(ItemStack(LADDER, 2), "# #", "###", "# #", {K('#', I_STICK)});
    shaped(ItemStack(I_BED), "###", "XXX", {K('#', WOOL), K('X', PLANKS)});
    shaped(ItemStack(I_WOOD_DOOR), "##", "##", "##", {K('#', PLANKS)});
    shaped(ItemStack(I_IRON_DOOR), "##", "##", "##", {K('#', I_IRON_INGOT)});
    shaped(ItemStack(WOOD_STAIRS, 4), "#  ", "## ", "###", {K('#', PLANKS)});
    shaped(ItemStack(COBBLE_STAIRS, 4), "#  ", "## ", "###", {K('#', COBBLESTONE)});
    shaped(ItemStack(JACK_O_LANTERN), "A", "B", {K('A', PUMPKIN), K('B', TORCH)});
    shaped(ItemStack(I_BUCKET), "# #", " # ", {K('#', I_IRON_INGOT)});
    shapeless(ItemStack(I_FLINT_AND_STEEL), {{I_IRON_INGOT, 0}, {I_FLINT, 0}});
    shaped(ItemStack(PLANKS, 4), "#", {K('#', LOG)});
    shaped(ItemStack(I_STICK, 4), "#", "#", {K('#', PLANKS)});
    shaped(ItemStack(TORCH, 4), "X", "#", {K('X', I_COAL, 0), K('#', I_STICK)});
    shaped(ItemStack(TORCH, 4), "X", "#", {K('X', I_COAL, 1), K('#', I_STICK)});
    shaped(ItemStack(I_BOWL, 4), "# #", " # ", {K('#', PLANKS)});
    shaped(ItemStack(I_SUGAR), "#", {K('#', I_REEDS)});
    shaped(ItemStack(I_SIGN), "###", "###", " X ", {K('#', PLANKS), K('X', I_STICK)});
    shaped(ItemStack(TRAPDOOR, 2), "###", "###", {K('#', PLANKS)});
    shaped(ItemStack(JUKEBOX), "###", "#X#", "###", {K('#', PLANKS), K('X', I_DIAMOND)});
    shaped(ItemStack(I_CAKE), "AAA", "BEB", "CCC",
           {K('A', I_MILK_BUCKET), K('B', I_SUGAR), K('C', I_WHEAT), K('E', I_EGG)});
    shaped(ItemStack(I_PAINTING), "###", "#X#", "###", {K('#', I_STICK), K('X', WOOL)});
    shaped(ItemStack(I_COMPASS), " # ", "#X#", " # ", {K('#', I_IRON_INGOT), K('X', I_REDSTONE)});
    shaped(ItemStack(I_CLOCK), " # ", "#X#", " # ", {K('#', I_GOLD_INGOT), K('X', I_REDSTONE)});
    shaped(ItemStack(I_FISHING_ROD), "  #", " #X", "# X", {K('#', I_STICK), K('X', I_STRING)});
    shaped(ItemStack(NOTE_BLOCK), "###", "#X#", "###", {K('#', PLANKS), K('X', I_REDSTONE)});
    shaped(ItemStack(LEVER), "X", "#", {K('#', COBBLESTONE), K('X', I_STICK)});
    shaped(ItemStack(REDSTONE_TORCH_ON), "X", "#", {K('#', I_STICK), K('X', I_REDSTONE)});
    shaped(ItemStack(I_REPEATER), "#X#", "III",
           {K('#', REDSTONE_TORCH_ON), K('X', I_REDSTONE), K('I', STONE)});
    shaped(ItemStack(STONE_BUTTON), "#", "#", {K('#', STONE)});
    shaped(ItemStack(STONE_PLATE), "##", {K('#', STONE)});
    shaped(ItemStack(WOOD_PLATE), "##", {K('#', PLANKS)});
    shaped(ItemStack(DISPENSER), "###", "#X#", "#R#", {K('#', COBBLESTONE), K('X', I_BOW), K('R', I_REDSTONE)});
    shaped(ItemStack(PISTON), "TTT", "#X#", "#R#",
           {K('T', PLANKS), K('#', COBBLESTONE), K('X', I_IRON_INGOT), K('R', I_REDSTONE)});
    shaped(ItemStack(STICKY_PISTON), "S", "P", {K('S', I_SLIMEBALL), K('P', PISTON)});
    shaped(ItemStack(RAIL, 16), "X X", "X#X", "X X", {K('X', I_IRON_INGOT), K('#', I_STICK)});
    shaped(ItemStack(POWERED_RAIL, 6), "X X", "X#X", "XRX",
           {K('X', I_GOLD_INGOT), K('R', I_REDSTONE), K('#', I_STICK)});
    shaped(ItemStack(DETECTOR_RAIL, 6), "X X", "X#X", "XRX",
           {K('X', I_IRON_INGOT), K('R', I_REDSTONE), K('#', STONE_PLATE)});
    shaped(ItemStack(I_MINECART), "# #", "###", {K('#', I_IRON_INGOT)});
    shaped(ItemStack(I_CHEST_MINECART), "A", "B", {K('A', CHEST), K('B', I_MINECART)});
    shaped(ItemStack(I_FURNACE_MINECART), "A", "B", {K('A', FURNACE), K('B', I_MINECART)});
    shaped(ItemStack(I_BOAT), "# #", "###", {K('#', PLANKS)});
}

ItemStack craftResult(const ItemStack* grid, int size) {
    for (int i = 0; i < g_recipeCount; ++i)
        if (matches(g_recipes[i], grid, size)) return g_recipes[i].result;
    return ItemStack();
}

ItemStack smeltResult(const ItemStack& in) {
    switch (in.id) {
        case IRON_ORE: return ItemStack(I_IRON_INGOT);
        case GOLD_ORE: return ItemStack(I_GOLD_INGOT);
        case DIAMOND_ORE: return ItemStack(I_DIAMOND);
        case SAND: return ItemStack(GLASS);
        case I_PORK_RAW: return ItemStack(I_PORK_COOKED);
        case I_FISH_RAW: return ItemStack(I_FISH_COOKED);
        case COBBLESTONE: return ItemStack(STONE);
        case I_CLAY_BALL: return ItemStack(I_BRICK);
        case CACTUS: return ItemStack(I_DYE, 1, 2);
        case LOG: return ItemStack(I_COAL, 1, 1);
        default: return ItemStack();
    }
}

int fuelValue(const ItemStack& s) {
    if (s.empty()) return 0;
    if (s.id < 256 && g_blocks[s.id].material == MAT_WOOD) return 300;
    switch (s.id) {
        case I_STICK: return 100;
        case I_COAL: return 1600;
        case I_LAVA_BUCKET: return 20000;
        case SAPLING: return 100;
        default: return 0;
    }
}
