# iRx — Tactical Multiplayer

iRx is a C++20 tactical shooter built with SDL3 and OpenGL 3.3. Version 0.3 adds the first authoritative multiplayer release, connects to `irautox.ir:9832`, rebrands the client, and keeps the existing offline arena mode available when the server cannot be reached.

## Multiplayer

- Versioned UDP protocol with a stateless HMAC challenge, per-session tokens, sequence validation, packet limits, rate limits, timeouts and a 30 Hz input stream.
- Server-authoritative health, movement limits, fire rate, magazines, reloads, hit detection, grenades, teams, round timer, bomb plant/defuse state and round score.
- Terrorist and Counter-Terrorist teams, F1/F2 team selection, remote full-body operators, weapon state, team colors, health bars and a hold-Tab scoreboard.
- Bomb sites A and B, crouch-to-plant/defuse interaction, round warmup, planted-bomb timer and team win conditions.
- The dedicated Python server stores achievements, admins, bans and audit events in SQLite. The server source is delivered privately and is not included in this public client repository.
- IrAutoX AC startup screen and server-side checks for speed, teleporting, invalid input, fire-rate manipulation, replayed packets and abusive packet rates.

## Content

- Six arenas with licensed CC0 environment models, material-bearing glTF assets, collision proxies, tiled floor treatment and scalable graphics profiles.
- Ten weapon classes from Common through Mythic, licensed models, distinct damage, magazine, spread, recoil, reload and fire-rate values.
- Legendary and Mythic glow, tracer and kill-particle treatments.
- Four full-body operators with idle and sprint animation, corrected player scale and weapon placement.
- Procedural shot, reload, empty-magazine, hit, kill, pickup, grenade, plant, defuse and footstep audio.
- Offline A* tactical bots, inventory, loadouts and eight persistent local achievements.
- `.na1`, `.na2` and later chunk extensions plus an integrity-checked `.naupk` manifest for compact asset updates.
- Native Discord IPC Rich Presence with mode, team, score and party size. Set `IRX_DISCORD_APP_ID` in the environment or pass `-DIRX_DISCORD_APP_ID=...` while configuring after creating the Discord application and its `irx` art asset.

## Controls

| Input | Action |
|---|---|
| `W A S D` | Move |
| Mouse / left mouse | Aim / fire |
| Right mouse | Aim down sights |
| `R` | Reload |
| `1` through `0` or wheel | Select weapon |
| `Shift` / `Ctrl` / `Space` | Sprint / crouch / jump |
| `B` | Buy and arsenal menu |
| Hold `Tab` | Multiplayer scoreboard |
| Hold `E` while crouched | Plant or defuse at a bomb site |
| `G` | Grenade |
| `F1` / `F2` | Join Terrorist / Counter-Terrorist |
| `V` | First/third-person camera |
| `F11` | Full screen |
| `Esc` | Pause / release mouse |

## Windows download

Open [Releases](../../releases), download `iRx-Windows-x64.zip`, extract the complete archive and run `PLAY.bat`. `iRx-AssetPacks.zip` contains the verified NAUPK update chunks.

## Build

Requirements are Git, CMake 3.25+, a C++20 compiler and internet access during the first configure.

```bash
cmake -S . -B build -DNEON_FETCH_ASSETS=ON
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix dist/iRx
```

Create and verify update chunks:

```bash
python tools/pack_assets.py create dist/iRx/assets dist/iRxAssetPacks --chunk-mb 96
python tools/pack_assets.py verify dist/iRxAssetPacks/assets.naupk
```

## Licensing

All bundled art is CC0. Exact creators, source pages and license links are in [`assets/CREDITS.md`](assets/CREDITS.md). The project does not redistribute content ripped from Counter-Strike, Call of Duty or other commercial games. Gameplay code is original and uses genre-standard tactical shooter concepts rather than copied commercial source.
