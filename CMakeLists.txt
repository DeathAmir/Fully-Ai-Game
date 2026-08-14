cmake_minimum_required(VERSION 3.25)

project(iRx VERSION 0.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(IRX_FETCH_ASSETS "Download pinned, licensed glTF game assets" ON)

include(FetchContent)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(FETCHCONTENT_QUIET OFF)

set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.4.14 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(SDL3)

FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git GIT_TAG 1.0.3 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glm)

FetchContent_Declare(glew GIT_REPOSITORY https://github.com/Perlmint/glew-cmake.git GIT_TAG glew-cmake-2.2.0 GIT_SHALLOW TRUE SOURCE_SUBDIR cmake-do-not-add)
FetchContent_MakeAvailable(glew)
add_library(irx_glew STATIC "${glew_SOURCE_DIR}/src/glew.c")
target_include_directories(irx_glew PUBLIC "${glew_SOURCE_DIR}/include")
target_compile_definitions(irx_glew PUBLIC GLEW_STATIC GLEW_NO_GLU)
if(WIN32)
    target_link_libraries(irx_glew PUBLIC opengl32 ws2_32)
elseif(APPLE)
    target_link_libraries(irx_glew PUBLIC "-framework OpenGL")
else()
    find_library(IRX_OPENGL_LIBRARY NAMES GL libGL.so.1 REQUIRED)
    target_link_libraries(irx_glew PUBLIC "${IRX_OPENGL_LIBRARY}" dl)
endif()

set(TINYGLTF_BUILD_LOADER_EXAMPLE OFF CACHE BOOL "" FORCE)
set(TINYGLTF_BUILD_GL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(TINYGLTF_BUILD_VALIDATOR_EXAMPLE OFF CACHE BOOL "" FORCE)
set(TINYGLTF_BUILD_BUILDER_EXAMPLE OFF CACHE BOOL "" FORCE)
set(TINYGLTF_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(tinygltf GIT_REPOSITORY https://github.com/syoyo/tinygltf.git GIT_TAG v2.9.6 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(tinygltf)

set(IRX_GENERATED_ASSET_DIR "${CMAKE_BINARY_DIR}/game_assets")
set(NEON_GENERATED_ASSET_DIR "${IRX_GENERATED_ASSET_DIR}")
if(IRX_FETCH_ASSETS)
    include(cmake/FetchGameAssets.cmake)
else()
    file(MAKE_DIRECTORY "${IRX_GENERATED_ASSET_DIR}")
    configure_file(assets/CREDITS.md "${IRX_GENERATED_ASSET_DIR}/CREDITS.md" COPYONLY)
endif()

execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_art.py" "${IRX_GENERATED_ASSET_DIR}"
    RESULT_VARIABLE IRX_ART_RESULT
)
if(NOT IRX_ART_RESULT EQUAL 0)
    message(FATAL_ERROR "iRx generated texture pipeline failed")
endif()

set(IRX_RUNTIME_SOURCES
    src/core/bootstrap.cpp
    src/core/runtime.cpp
    src/net/client.cpp
    src/net/protocol.cpp
    src/gameplay/weapon_catalog.cpp
    src/gameplay/match_rules.cpp
    src/gameplay/bomb_system.cpp
    src/assets/asset_registry.cpp
    src/security/integrity.cpp
    src/ui/menu_model.cpp
)

add_library(irx_runtime OBJECT ${IRX_RUNTIME_SOURCES})
target_include_directories(irx_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(irx_runtime PRIVATE IRX_SERVER_HOST="irautox.ir" IRX_SERVER_PORT=9832 IRX_VERSION="${PROJECT_VERSION}")

add_executable(iRx WIN32 src/main.cpp $<TARGET_OBJECTS:irx_runtime>)
target_include_directories(iRx PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(iRx PRIVATE SDL3::SDL3 glm::glm irx_glew tinygltf)
target_compile_definitions(iRx PRIVATE GLM_FORCE_RADIANS GLM_ENABLE_EXPERIMENTAL NEON_VERSION="${PROJECT_VERSION}" IRX_VERSION="${PROJECT_VERSION}" IRX_SERVER_HOST="irautox.ir" IRX_SERVER_PORT=9832)

if(MSVC)
    target_compile_options(irx_runtime PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_compile_options(iRx PRIVATE /W4 /permissive- /EHsc /utf-8)
else()
    target_compile_options(irx_runtime PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(iRx PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_custom_command(TARGET iRx POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${IRX_GENERATED_ASSET_DIR}" "$<TARGET_FILE_DIR:iRx>/assets"
    COMMENT "Copying licensed and generated iRx assets"
)

install(TARGETS iRx RUNTIME DESTINATION .)
install(DIRECTORY "${IRX_GENERATED_ASSET_DIR}/" DESTINATION assets)
if(TARGET SDL3-shared)
    install(TARGETS SDL3-shared RUNTIME DESTINATION . LIBRARY DESTINATION .)
endif()

include(InstallRequiredSystemLibraries)
set(CPACK_PACKAGE_NAME "iRx")
set(CPACK_PACKAGE_VENDOR "IrAutoX")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_GENERATOR "ZIP")
include(CPack)
