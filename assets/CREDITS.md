# Third-party asset credits

Every downloaded file is fetched during CMake configure from the URL recorded
in `cmake/FetchGameAssets.cmake` and accepted only when its pinned SHA-256 hash
matches. The generated Windows release carries this document beside the assets.

## Kenney — Creative Commons CC0 1.0 Universal

Creator: **Kenney Vleugels / Kenney** — <https://kenney.nl/>

- **Blocky Characters 2.0** — 18 full-body modular characters and 27
  object-animation clips, including idle, walk, sprint, firearm and death
  sequences. Source: <https://kenney.nl/assets/blocky-characters>
- **Blaster Kit 2.1** — futuristic weapons, attachments, throwables and props.
  Source: <https://kenney.nl/assets/blaster-kit>
- **City Kit (Industrial) 1.0** — factory buildings, tanks and chimneys.
  Source: <https://kenney.nl/assets/city-kit-industrial>
- **City Kit (Suburban) 2.0** — houses, fencing, paths and vegetation.
  Source: <https://kenney.nl/assets/city-kit-suburban>
- **City Kit (Roads) 2.0** — roads, intersections, barriers, signs and lights.
  Source: <https://kenney.nl/assets/city-kit-roads>
- **Space Station Kit 1.0** — modular station structures, walls, doors,
  consoles, furniture and containers.
  Source: <https://kenney.nl/assets/space-station-kit>

License: <https://creativecommons.org/publicdomain/zero/1.0/>

CC0 does not require attribution, but the creator and exact packs are credited
here as a matter of respect and provenance. No endorsement is implied.

## Quaternius — Creative Commons CC0 1.0 Universal

Creator: **Quaternius** — <https://quaternius.com/>

- **Ultimate Guns Pack** — 25 GLB weapon and attachment models, including
  pistols, revolvers, shotguns, SMGs, bullpups, assault rifles and sniper
  rifles. Creator page: <https://quaternius.com/packs/ultimategun.html>
  Distribution mirror: <https://poly.pizza/bundle/Ultimate-Guns-Pack-cpgUfI4t2F>

The mirror's exact GLB archive is vendored at
`third_party/assets/quaternius-ultimate-guns-glb.zip` because its CDN rejects
requests from GitHub-hosted runners. Its SHA-256 is pinned and verified before
extraction: `b2a37e8b30df08f5f3f239c4e1649446208b70cb111be39386cf2b1ccfa486ea`.

License: <https://creativecommons.org/publicdomain/zero/1.0/>

## Khronos glTF Sample Assets — Creative Commons CC0 1.0 Universal

- **Sci-Fi Helmet** by Microsoft — source:
  <https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/SciFiHelmet>
- **Water Bottle** by Microsoft — source:
  <https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/WaterBottle>

License metadata is recorded in the upstream model READMEs.

## Runtime dependencies

- SDL 3 — zlib license
- GLEW — Modified BSD / MIT licenses
- GLM — Happy Bunny / MIT-style license
- tinygltf — MIT license

OpenGL is provided by the operating system/graphics driver. iRx's own
source is distributed under the repository's MIT license. No GPL asset or code
is currently bundled, avoiding share-alike ambiguity for the executable.
