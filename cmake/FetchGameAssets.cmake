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

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/CREDITS.md"
    "${NEON_GENERATED_ASSET_DIR}/CREDITS.md"
    COPYONLY
)
