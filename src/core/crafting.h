#pragma once
#include "items.h"

void initRecipes();
// grid: size*size stacks, row-major (size 2 or 3). Returns an empty stack when nothing matches.
ItemStack craftResult(const ItemStack* grid, int size);
ItemStack smeltResult(const ItemStack& in);
int fuelValue(const ItemStack& s);  // ticks of burn time, 0 if not a fuel
