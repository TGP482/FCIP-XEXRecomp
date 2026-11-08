# Far Cry Instincts Predator (Xbox 360), statically recompiled

cmake_minimum_required(VERSION 3.25)
project(fcip LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# the SDK: sdk/ here, a checkout beside this repository, else downloaded
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sdk/cmake/XexRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../Xbox360XEX-Recomp/cmake/XexRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../Xbox360XEX-Recomp)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../Xbox360XEXRecomp/cmake/XexRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../Xbox360XEXRecomp)
else()
    include(FetchContent)
    FetchContent_Declare(xex_sdk GIT_REPOSITORY https://github.com/TGP482/Xbox360XEX-Recomp.git GIT_TAG main)
    FetchContent_MakeAvailable(xex_sdk)
    set(SDK ${xex_sdk_SOURCE_DIR})
endif()
include(${SDK}/cmake/XexRecomp.cmake)
# src/d3d and src/xaudio replace the SDK's D3D HLE, renderer and XAudio HLE
list(FILTER XEX_RUNTIME_SOURCES EXCLUDE REGEX "/(xdk/d3d9|xdk/xaudio|gpu/d3d12)\\.cpp$")

file(GLOB XEX_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XEX_HOST_SOURCES EXCLUDE REGEX "/src/game/")

# partitions as xex_symbols wrote them into config/game.toml
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS config/game.toml)
file(STRINGS config/game.toml GAME_PARTITIONS REGEX "^partitions *=")
string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")

add_xex_image(FCIP default.xex game.toml ${GAME_PARTITIONS} game)
