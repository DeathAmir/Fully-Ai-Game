function(neon_download_asset file_name source_url expected_sha256)
    set(destination "${NEON_GENERATED_ASSET_DIR}/${file_name}")
    get_filename_component(destination_directory "${destination}" DIRECTORY)
    file(MAKE_DIRECTORY "${destination_directory}")
    if(EXISTS "${destination}")
        file(SHA256 "${destination}" actual_sha256)
        if(actual_sha256 STREQUAL expected_sha256)
            message(STATUS "Using cached game asset: ${file_name}")
            return()
        endif()
        file(REMOVE "${destination}")
    endif()

    message(STATUS "Downloading licensed game asset: ${file_name}")
    file(DOWNLOAD
        "${source_url}"
        "${destination}"
        EXPECTED_HASH "SHA256=${expected_sha256}"
        TLS_VERIFY ON
        STATUS download_status
        SHOW_PROGRESS
    )
    list(GET download_status 0 status_code)
    list(GET download_status 1 status_message)
    if(NOT status_code EQUAL 0)
        file(REMOVE "${destination}")
        message(FATAL_ERROR "Could not download ${file_name}: ${status_message}")
    endif()
endfunction()

function(neon_download_archive archive_name source_url expected_sha256 output_directory)
    set(download_directory "${CMAKE_BINARY_DIR}/licensed_asset_downloads")
    set(extract_directory "${CMAKE_BINARY_DIR}/licensed_asset_extract/${archive_name}")
    set(archive_path "${download_directory}/${archive_name}.zip")
    file(MAKE_DIRECTORY "${download_directory}")

    if(EXISTS "${archive_path}")
        file(SHA256 "${archive_path}" actual_sha256)
        if(NOT actual_sha256 STREQUAL expected_sha256)
            file(REMOVE "${archive_path}")
        endif()
    endif()

    if(NOT EXISTS "${archive_path}")
        message(STATUS "Downloading licensed asset pack: ${archive_name}")
        file(DOWNLOAD
            "${source_url}"
            "${archive_path}"
            EXPECTED_HASH "SHA256=${expected_sha256}"
            TLS_VERIFY ON
            STATUS download_status
            SHOW_PROGRESS
        )
        list(GET download_status 0 status_code)
        list(GET download_status 1 status_message)
        if(NOT status_code EQUAL 0)
            file(REMOVE "${archive_path}")
            message(FATAL_ERROR "Could not download ${archive_name}: ${status_message}")
        endif()
    endif()

    file(REMOVE_RECURSE "${extract_directory}")
    file(MAKE_DIRECTORY "${extract_directory}")
    file(ARCHIVE_EXTRACT INPUT "${archive_path}" DESTINATION "${extract_directory}")
    file(REMOVE_RECURSE "${NEON_GENERATED_ASSET_DIR}/${output_directory}")
    file(MAKE_DIRECTORY "${NEON_GENERATED_ASSET_DIR}/${output_directory}")

    if(archive_name STREQUAL "quaternius-ultimate-guns")
        file(GLOB pack_models "${extract_directory}/*.glb")
    else()
        file(GLOB pack_models "${extract_directory}/Models/GLB format/*.glb")
    endif()
    if(NOT pack_models)
        message(FATAL_ERROR "No GLB models found in licensed pack ${archive_name}")
    endif()
    file(COPY ${pack_models} DESTINATION "${NEON_GENERATED_ASSET_DIR}/${output_directory}")
endfunction()

function(neon_import_local_archive archive_name relative_path expected_sha256 output_directory)
    set(archive_path "${CMAKE_CURRENT_SOURCE_DIR}/${relative_path}")
    set(extract_directory "${CMAKE_BINARY_DIR}/licensed_asset_extract/${archive_name}")
    if(NOT EXISTS "${archive_path}")
        message(FATAL_ERROR "Vendored licensed archive is missing: ${relative_path}")
    endif()
    file(SHA256 "${archive_path}" actual_sha256)
    if(NOT actual_sha256 STREQUAL expected_sha256)
        message(FATAL_ERROR "Vendored licensed archive hash mismatch: ${relative_path}")
    endif()
    file(REMOVE_RECURSE "${extract_directory}")
    file(MAKE_DIRECTORY "${extract_directory}")
    file(ARCHIVE_EXTRACT INPUT "${archive_path}" DESTINATION "${extract_directory}")
    file(REMOVE_RECURSE "${NEON_GENERATED_ASSET_DIR}/${output_directory}")
    file(MAKE_DIRECTORY "${NEON_GENERATED_ASSET_DIR}/${output_directory}")
    file(GLOB pack_models "${extract_directory}/*.glb")
    if(NOT pack_models)
        message(FATAL_ERROR "No GLB models found in vendored pack ${archive_name}")
    endif()
    file(COPY ${pack_models} DESTINATION "${NEON_GENERATED_ASSET_DIR}/${output_directory}")
endfunction()

file(MAKE_DIRECTORY "${NEON_GENERATED_ASSET_DIR}")

neon_download_asset("SciFiHelmet/SciFiHelmet.gltf"
    "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/SciFiHelmet/glTF/SciFiHelmet.gltf"
    "4b9e64c5337ddaba5b44821d76786bcad3b5b78f877cf034ec6163dc8baca35a")
neon_download_asset("SciFiHelmet/SciFiHelmet.bin"
    "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/SciFiHelmet/glTF/SciFiHelmet.bin"
    "6a7af42896359f8e0d9ba65260cbb7b1a1d36aa787439d3775a48aba1f9a25ba")
neon_download_asset("SciFiHelmet/SciFiHelmet_AmbientOcclusion.png"
    "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/SciFiHelmet/glTF/SciFiHelmet_AmbientOcclusion.png"
    "95823e77b0a6e45308b53be4fdf1b7a7b6ac085c190dc1c5ef47097d0e35eb2f")
neon_download_asset("SciFiHelmet/SciFiHelmet_BaseColor.png"
    "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/SciFiHelmet/glTF/SciFiHelmet_BaseColor.png"
    "bda08dd029775ea0964d5721b779a639da091222eaa750233ba34e01e3eeace2")
neon_download_asset("SciFiHelmet/SciFiHelmet_MetallicRoughness.png"
    "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/SciFiHelmet/glTF/SciFiHelmet_MetallicRoughness.png"
    "f38f025af3201697679e2a04ba4cf01b4506e68ce4e0e0f09c268a7dfb80b2c3")
neon_download_asset("SciFiHelmet/SciFiHelmet_Normal.png"
    "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/SciFiHelmet/glTF/SciFiHelmet_Normal.png"
    "437c034b60900b503bb41be3328011ab3b7602c8454f8eebcab98929ea25fe99")

neon_download_asset(
    "WaterBottle.glb"
    "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/WaterBottle/glTF-Binary/WaterBottle.glb"
    "b337e526fd6a162013c2984aeec163f5fbb4f717252724dfc3f3458bd51df94b"
)

# Kenney publishes these complete packs under CC0 1.0. The archives are small,
# include optimized GLB files, and are pinned so upstream changes cannot silently
# alter a release build.
neon_download_archive(
    "kenney-blocky-characters"
    "https://kenney.nl/media/pages/assets/blocky-characters/8369c0cf30-1749547469/kenney_blocky-characters_20.zip"
    "5e123859aa0c1598342b600c6db197024a1d63eb9ec531398b310725f589887e"
    "Characters"
)
neon_download_archive(
    "kenney-blaster-kit"
    "https://kenney.nl/media/pages/assets/blaster-kit/261d80a716-1753959510/kenney_blaster-kit_2.1.zip"
    "91e3093e95427d59625e7e2ce2d0399b861600160fd0b4ada7714796b67cea8c"
    "Guns/Blasters"
)
neon_download_archive(
    "kenney-city-industrial"
    "https://kenney.nl/media/pages/assets/city-kit-industrial/5fcb837741-1750838303/kenney_city-kit-industrial_1.0.zip"
    "99a09ff148056678c0c3b7977ad9dbce55d2f243e06fbfc7c28642accef4dd9e"
    "Maps/Industrial"
)
neon_download_archive(
    "kenney-city-suburban"
    "https://kenney.nl/media/pages/assets/city-kit-suburban/2c871b7af2-1745479373/kenney_city-kit-suburban_20.zip"
    "5869c35cf30b1c87bdb2d197b6d325eebadd2ef08ea27f04797e8e08d77a9a39"
    "Maps/Suburban"
)
neon_download_archive(
    "kenney-city-roads"
    "https://kenney.nl/media/pages/assets/city-kit-roads/74288c9459-1741864740/kenney_city-kit-roads.zip"
    "2c1644a293a85d98837ef788b0cbc4b9d53dffb1280fbe9a4f927b644aaba4b0"
    "Maps/Roads"
)
neon_download_archive(
    "kenney-space-station"
    "https://kenney.nl/media/pages/assets/space-station-kit/6475288f2e-1712749919/kenney_space-station-kit.zip"
    "215e79bd5415cff93665183390f0343ed9acf87780306331013b78520170c6d8"
    "Maps/SpaceStation"
)

# Quaternius' Ultimate Guns Pack is public domain (CC0). The exact Poly Pizza
# archive is vendored because its CDN blocks GitHub-hosted build runners.
neon_import_local_archive(
    "quaternius-ultimate-guns"
    "third_party/assets/quaternius-ultimate-guns-glb.zip"
    "b2a37e8b30df08f5f3f239c4e1649446208b70cb111be39386cf2b1ccfa486ea"
    "Guns/Quaternius"
)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/CREDITS.md"
    "${NEON_GENERATED_ASSET_DIR}/CREDITS.md"
    COPYONLY
)
