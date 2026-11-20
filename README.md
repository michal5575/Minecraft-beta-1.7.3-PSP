# Minecraft beta 1.7.3. (PSP)

Minecraft beta 1.7.3. (PSP). A remake of Minecraft Beta 1.7.3 for the PSP, written from scratch. It is not based on Lamecraft.

This is a singleplayer game for the original PSP, including the 32 MB PSP-1000. The world, the player, the blocks, and the renderer are a C++ engine written for this port. It draws with the PSP graphics chip and keeps terrain, mobs, and saves inside the console's memory.

There is no multiplayer. Survival and creative are both there: infinite terrain, crafting, furnaces, chests, beds, the Beta 1.7.3 mobs, redstone, rails and minecarts, boats, the Nether, weather, clouds, and particles. Five world slots can be saved on the Memory Stick.

Textures and sounds are not in this repository. At build time they are converted from a Beta 1.7.3 `minecraft.jar` you already have.

## Screenshots

The title screen, with singleplayer only.

![Title screen](screenshots/01-title.png)

A new world, just after spawn.

![Spawn](screenshots/02-spawn.png)

Walking, mining, and the block that comes away.

![Walking](screenshots/03-walking.png)
![Mining](screenshots/04-mining.png)
![A broken block](screenshots/05-block-broken.png)

Inventory crafting and the workbench.

![Inventory crafting](screenshots/06-inventory-crafting.png)
![Workbench](screenshots/07-workbench.png)

Passive and hostile mobs share the same forest.

![Mobs](screenshots/08-mobs.png)
![More mobs](screenshots/09-more-mobs.png)

Sunset, the night sky, flying in creative, death, and the respawn.

![Sunset](screenshots/10-sunset.png)
![Night sky](screenshots/11-night-sky.png)
![Flying](screenshots/12-flying.png)
![Death](screenshots/13-death.png)
![Respawn](screenshots/14-respawn.png)

The pause menu, a furnace, a chest, and sleeping through the night.

![Pause menu](screenshots/15-pause-menu.png)
![Furnace](screenshots/16-furnace.png)
![Chest](screenshots/17-chest.png)
![Beds](screenshots/18-beds.png)
![Sleeping](screenshots/19-sleeping.png)
![Morning](screenshots/20-morning.png)

Water and lava, a sword, and fluids spreading.

![Water and lava](screenshots/21-water-and-lava.png)
![Sword](screenshots/22-sword.png)
![Sword swing](screenshots/23-sword-swing.png)
![Fluids spreading](screenshots/24-fluids-spread.png)

A dungeon, a squid, and a mob spawner.

![Dungeon](screenshots/25-dungeon.png)
![Squid](screenshots/26-squid.png)
![Spawner](screenshots/27-spawner.png)

Signs, snowballs, and a jukebox.

![Small items](screenshots/28-small-items.png)
![Snowball](screenshots/29-snowball.png)
![Sign](screenshots/30-sign.png)
![Jukebox](screenshots/31-jukebox.png)

Redstone dust, pistons, and a dispenser.

![Redstone](screenshots/32-redstone.png)
![Piston](screenshots/33-piston.png)
![Sticky piston](screenshots/34-sticky-piston.png)
![Dispenser](screenshots/35-dispenser.png)

Rails, riding a minecart, and a chest minecart.

![Rails and minecarts](screenshots/36-rails-and-minecarts.png)
![Riding a minecart](screenshots/37-riding-minecart.png)
![Chest minecart](screenshots/38-chest-minecart.png)

The Nether portal, the trip, and the way back.

![Nether portal](screenshots/39-nether-portal.png)
![Portal travel](screenshots/40-portal-travel.png)
![The Nether](screenshots/41-the-nether.png)
![Return from the Nether](screenshots/42-return-from-nether.png)

Rain, lightning, snow, clouds, and particles.

![Rain](screenshots/43-rain.png)
![Lightning](screenshots/44-lightning.png)
![Snow](screenshots/45-snow.png)
![Clouds](screenshots/46-clouds.png)
![Particles](screenshots/47-particles.png)

Five save slots, and creating a world.

![World slots](screenshots/48-world-slots.png)
![Create world](screenshots/49-create-world.png)

## Controls

The analog stick moves, and the face buttons look around. In the pause menu this can be swapped so the stick looks and the face buttons move.

| Button | In the world |
| --- | --- |
| Analog stick | Move |
| Triangle / Cross | Look up / down |
| Square / Circle | Look left / right |
| D-pad up | Jump |
| D-pad down | Drop the held item. In creative, fly down. |
| D-pad left / right | Change the hotbar slot |
| R | Mine or attack |
| L | Place or use |
| Select | Inventory in survival, item picker in creative |
| Start | Pause |

## Building

Install the [pspdev](https://github.com/pspdev/pspdev) toolchain, then from this directory:

```sh
cmake -S . -B build -DMINECRAFT_JAR=/path/to/minecraft.jar
cmake --build build
```

`minecraft.jar` has to be a Beta 1.7.3 client jar. The build converts its textures and sounds into the `EBOOT.PBP`. Copy that file to `PSP/GAME/MinecraftBeta/` on the Memory Stick.

A desktop test build uses the same jar and does not need the PSP toolchain:

```sh
cmake -S . -B build-host -DMINECRAFT_JAR=/path/to/minecraft.jar
cmake --build build-host
```
