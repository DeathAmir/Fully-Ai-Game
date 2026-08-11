# NEON ASSAULT — Arsenal Update

Neon Assault is a single-player 3D arena shooter written in C++20 with SDL3
and OpenGL 3.3. Version 0.2 expands the original prototype into a licensed
content build with animated operators, real weapon meshes, tactical bots,
multiple modes, inventory, rarity tiers and persistent achievements.

## Playable content

- **6 maps:** Neon Yard, Dust Depot, Ice Lab, Suburban Siege, Orbital Station
  and Iron Foundry. The last three use optimized CC0 environment packs from
  Kenney with independent collision proxies for reliable movement and AI.
- **10 balanced weapons:** pistol, assault rifle, shotgun, SMG, DMR, sniper,
  Nova Blaster, Dragon Core, Void Cannon and Rail Lancer.
- **Rarity system:** Common, Uncommon, Rare, Epic, Legendary and Mythic.
  Legendary and Mythic equipment has unique accent rendering; Mythic weapons
  produce radial neon kill effects.
- **Licensed model library:** 25 recognizable firearm/attachment models by
  Quaternius, 40 Kenney blaster assets, 18 operator variants and more than 200
  map GLBs. Every upstream archive has a pinned SHA-256 hash.
- **4 full-body operators:** Wraith, Viper, Nomad and Spectre. Object-based
  glTF animation supports idle, sprint, armed and shooting clips without a
  heavyweight engine runtime.
- **Tactical bots:** A* navigation grid, obstacle routing, visual memory,
  gunshot hearing, reaction delay, burst fire, target leading, dynamic cover,
  separation and four complementary roles: assault, flanker, marksman and
  rusher.
- **4 modes:** Survival, Elimination, Head Hunter and Mythic Mayhem.
- **Inventory and loadout:** select weapons in the lobby or in the in-game
  Arsenal screen, with damage, magazine and rarity information.
- **8 persistent achievements:** progress is stored in SDL's per-user
  preference directory and survives restarts.
- First-person and third-person cameras, responsive mouse aim, sprint, crouch,
  jump, recoil, reloads, ammo pickups, headshots, procedural audio, particles,
  damage feedback and scalable Potato/Balanced/Ultra profiles.

## Controls

| Input | Action |
|---|---|
| `W A S D` | Move |
| Mouse / left mouse | Aim / fire |
| `R` | Reload |
| `1` through `0` or wheel | Select one of ten weapons |
| `Shift` / `Ctrl` / `Space` | Sprint / crouch / jump |
| `Tab` | Arsenal inventory and achievements |
| `V` | Toggle first/third-person camera |
| `Esc` | Pause / release mouse |
| `Enter` | Start / restart |
| `M` in lobby | Change map |
| `G` in lobby | Change game mode |
| `C` in lobby | Change operator |
| `D` / `Q` in lobby | Change difficulty / graphics profile |

## Windows download

Open the repository's **Releases** page, download
`NeonAssault-Windows-x64.zip`, extract the entire archive, and run `PLAY.bat`.
The ZIP is portable and includes SDL3 plus the licensed runtime content. OpenGL
is supplied by the Windows graphics driver.

## Build from source

Requirements: Git, CMake 3.25+, a C++20 compiler and internet access during the
first configure. SDL3, GLM, GLEW, tinygltf and all licensed content are pinned
and fetched reproducibly.

```bash
cmake -S . -B build -DNEON_FETCH_ASSETS=ON
cmake --build build --config Release --parallel
```

Create a portable directory on Windows:

```bash
cmake --install build --config Release --prefix dist/NeonAssault
```

`-DNEON_FETCH_ASSETS=OFF` creates a source-only fallback build without the
external models. Procedural geometry remains available when an optional model
cannot be loaded.

## Licensing and provenance

All bundled art is CC0. Exact creators, pack names, source pages and license
links are documented in [`assets/CREDITS.md`](assets/CREDITS.md). Upstream
archives and individual Khronos models are checked against SHA-256 hashes in
[`cmake/FetchGameAssets.cmake`](cmake/FetchGameAssets.cmake). The project does
not redistribute ripped assets or content from commercial games.
