# NEON ASSAULT

A fast, single-player 3D arena shooter written in modern C++ using SDL3 and
OpenGL 3.3. Fight increasingly aggressive bot waves, rotate between six
weapons, collect health and ammunition, and chase a high score.

## Current playable features

- Responsive first-person mouse look, WASD movement, sprint, crouch, and jump
- Six weapons: pistol, assault rifle, shotgun, SMG, DMR, and sniper rifle
- Magazine/reserve ammo, weapon switching, recoil, spread, reload timing,
  tracers, muzzle flashes, and hit markers
- Wave-based bots with chase, strafe, retreat, line-of-sight, obstacle
  avoidance, accuracy pressure, and ranged attacks
- Three selectable arenas (Neon Yard, Dust Depot, Ice Lab), collision, cover,
  pickups, scoring, death and restart flow
- Pre-match lobby controls for map, difficulty, and Potato/Balanced/Ultra
  graphics profiles; the Potato profile reduces bot and particle budgets
- OpenGL lighting, fog, particles, view-models, damage vignette, and bitmap HUD
- Procedural SDL3 audio for shots, hits, pickups, reloads, and UI feedback
- Internet-sourced glTF props with pinned hashes and explicit license credits
- Reproducible CMake build and a GitHub Actions Windows x64 Release ZIP

## Controls

| Input | Action |
|---|---|
| `W A S D` | Move |
| Mouse | Aim |
| Left mouse | Fire |
| `R` | Reload |
| `1` through `6` or wheel | Select weapon |
| `Shift` | Sprint |
| `Ctrl` | Crouch |
| `Space` | Jump |
| `Esc` | Pause / release mouse |
| `Enter` | Start / restart |
| `M` / `D` / `Q` in lobby | Change map / difficulty / graphics profile |

## Download for Windows

Open the repository's **Releases** page, download
`NeonAssault-Windows-x64.zip`, extract the entire archive, and run `PLAY.bat`.
Do not run the executable from inside the ZIP.

## Build from source

Requirements: Git, CMake 3.25+, a C++20 compiler, and internet access during
the first configure. Dependencies and licensed models are pinned and fetched by
CMake.

```bash
cmake -S . -B build
cmake --build build --config Release --parallel
```

On Windows with Visual Studio, the executable is normally written to
`build/Release/`. To create a portable directory:

```bash
cmake --install build --config Release --prefix dist/NeonAssault
```

For a source-only/offline configure, pass `-DNEON_FETCH_ASSETS=OFF`; procedural
fallback art remains playable, while the external helmet and bottle props are
omitted.

## Asset and dependency licenses

See [`assets/CREDITS.md`](assets/CREDITS.md). Downloaded asset integrity is
verified with SHA-256 before it is accepted into a build.
