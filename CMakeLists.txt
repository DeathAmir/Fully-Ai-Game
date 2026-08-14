cmake_minimum_required(VERSION 3.25)

project(iRx VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(NEON_FETCH_ASSETS "Download pinned, licensed glTF game assets" ON)
set(IRX_DISCORD_APP_ID "" CACHE STRING "Discord application id used for Rich Presence")

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.14
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SDL3)

FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glm)

FetchContent_Declare(
    glew
    GIT_REPOSITORY https://github.com/Perlmint/glew-cmake.git
    GIT_TAG glew-cmake-2.2.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR cmake-do-not-add
)
FetchContent_MakeAvailable(glew)
add_library(neon_glew STATIC "${glew_SOURCE_DIR}/src/glew.c")
target_include_directories(neon_glew PUBLIC "${glew_SOURCE_DIR}/include")
target_compile_definitions(neon_glew PUBLIC GLEW_STATIC GLEW_NO_GLU)
if(WIN32)
    target_link_libraries(neon_glew PUBLIC opengl32)
elseif(APPLE)
    target_link_libraries(neon_glew PUBLIC "-framework OpenGL")
else()
    find_library(NEON_OPENGL_LIBRARY NAMES GL libGL.so.1 REQUIRED)
    target_link_libraries(neon_glew PUBLIC "${NEON_OPENGL_LIBRARY}" dl)
endif()

set(TINYGLTF_BUILD_LOADER_EXAMPLE OFF CACHE BOOL "" FORCE)
set(TINYGLTF_BUILD_GL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(TINYGLTF_BUILD_VALIDATOR_EXAMPLE OFF CACHE BOOL "" FORCE)
set(TINYGLTF_BUILD_BUILDER_EXAMPLE OFF CACHE BOOL "" FORCE)
set(TINYGLTF_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    tinygltf
    GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
    GIT_TAG v2.9.6
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(tinygltf)

set(NEON_GENERATED_ASSET_DIR "${CMAKE_BINARY_DIR}/game_assets")
if(NEON_FETCH_ASSETS)
    include(cmake/FetchGameAssets.cmake)
else()
    file(MAKE_DIRECTORY "${NEON_GENERATED_ASSET_DIR}")
    configure_file(assets/CREDITS.md "${NEON_GENERATED_ASSET_DIR}/CREDITS.md" COPYONLY)
endif()

add_executable(iRx WIN32
    src/main.cpp
    src/net_client.cpp
    src/discord_rpc.cpp
)

target_link_libraries(iRx PRIVATE
    SDL3::SDL3
    glm::glm
    neon_glew
    tinygltf
)

target_compile_definitions(iRx PRIVATE
    GLM_FORCE_RADIANS
    GLM_ENABLE_EXPERIMENTAL
    NEON_VERSION="${PROJECT_VERSION}"
    IRX_DISCORD_APP_ID="${IRX_DISCORD_APP_ID}"
)

if(MSVC)
    target_compile_options(iRx PRIVATE /W4 /permissive- /EHsc /utf-8)
else()
    target_compile_options(iRx PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_link_libraries(iRx PRIVATE ws2_32)
endif()

add_custom_command(TARGET iRx POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${NEON_GENERATED_ASSET_DIR}"
        "$<TARGET_FILE_DIR:iRx>/assets"
    COMMENT "Copying game assets"
)

install(TARGETS iRx RUNTIME DESTINATION .)
install(DIRECTORY "${NEON_GENERATED_ASSET_DIR}/" DESTINATION assets)
if(TARGET SDL3-shared)
    install(TARGETS SDL3-shared RUNTIME DESTINATION . LIBRARY DESTINATION .)
endif()

include(InstallRequiredSystemLibraries)
set(CPACK_PACKAGE_NAME "iRx")
set(CPACK_PACKAGE_VENDOR "IrAutoX")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_GENERATOR "ZIP")
include(CPack)
