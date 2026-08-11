# Third-party asset credits

The release build downloads the following models from the Khronos glTF Sample
Assets repository and packages them for offline play:

- **Sci-Fi Helmet** by Microsoft — CC0 1.0 Universal. Source:
  `KhronosGroup/glTF-Sample-Assets/Models/SciFiHelmet`.
- **Water Bottle** by Microsoft — CC0 1.0 Universal. Source:
  `KhronosGroup/glTF-Sample-Assets/Models/WaterBottle`.

The models are not committed to this repository. Their exact upstream URLs and
SHA-256 integrity hashes are pinned in `cmake/FetchGameAssets.cmake`. This file
is copied beside the models in every packaged build.

Runtime dependencies:

- SDL 3 — zlib license
- GLEW — Modified BSD / MIT licenses
- GLM — Happy Bunny / MIT-style license
- tinygltf — MIT license
