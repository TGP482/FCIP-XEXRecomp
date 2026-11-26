# Far Cry Instincts Predator (Xbox 360), statically recompiled

cmake_minimum_required(VERSION 3.25)
project(fcip LANGUAGES C CXX RC)

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
# src/d3d and src/xaudio replace the SDK's D3D HLE, renderer, shader translator and XAudio HLE
list(FILTER XEX_RUNTIME_SOURCES EXCLUDE REGEX "/(xdk/d3d9|xdk/xaudio|gpu/d3d12|gpu/shaders)\\.cpp$")
# gyro aiming reads the controllers through SDL
set_source_files_properties(src/common/gyro.cpp PROPERTIES INCLUDE_DIRECTORIES "${sdl3_SOURCE_DIR}/include")
# AMD FSR 3, built from the open source AMD FSR SDK in third_party/fidelityfx with the glue in src/d3d/fsr; its
# shaders go to the FSR folder beside the exe, compiled when first used
set(FFX_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fidelityfx)
set(FFX_DEFINES FFX_BACKEND_DX12 FFX_UPSCALER FFX_FRAMEGENERATION NOMINMAX)
file(GLOB_RECURSE FFX_SOURCES CONFIGURE_DEPENDS ${FFX_DIR}/*.cpp)
add_library(fidelityfx STATIC ${FFX_SOURCES})
target_include_directories(fidelityfx PRIVATE ${FFX_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src/d3d/fsr)
target_compile_definitions(fidelityfx PRIVATE ${FFX_DEFINES} UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
# ffx_message.cpp uses FFX_UNUSED without including the header that defines it
target_compile_options(fidelityfx PRIVATE -w -include ${FFX_DIR}/api/internal/ffx_util.h)
file(GLOB FSR_GLUE_SOURCES CONFIGURE_DEPENDS src/d3d/fsr/*.cpp)
set_source_files_properties(src/d3d/fsr.cpp ${FSR_GLUE_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-isystem;${FFX_DIR};-I${CMAKE_CURRENT_SOURCE_DIR}/src/d3d/fsr" COMPILE_DEFINITIONS "${FFX_DEFINES}")
file(GLOB_RECURSE FSR_SHADERS RELATIVE ${FFX_DIR} CONFIGURE_DEPENDS
    ${FFX_DIR}/api/internal/gpu/*
    ${FFX_DIR}/upscalers/fsr3/include/gpu/*
    ${FFX_DIR}/upscalers/fsr3/internal/shaders/*
    ${FFX_DIR}/framegeneration/fsr3/include/gpu/*
    ${FFX_DIR}/framegeneration/fsr3/internal/shaders/*)
foreach(file ${FSR_SHADERS})
    configure_file(${FFX_DIR}/${file} ${CMAKE_BINARY_DIR}/FSR/shaders/${file} COPYONLY)
endforeach()
configure_file(${FFX_DIR}/license.txt ${CMAKE_BINARY_DIR}/FSR/license.txt COPYONLY)

file(GLOB XEX_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XEX_HOST_SOURCES EXCLUDE REGEX "/src/(game|editor|leveleditor)/")
list(APPEND XEX_HOST_SOURCES ${FSR_GLUE_SOURCES})

# partitions as xex_symbols wrote them into each config
foreach(image game editor)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS config/${image}.toml)
    file(STRINGS config/${image}.toml parts REGEX "^partitions *=")
    string(REGEX MATCH "[0-9]+" PARTITIONS_${image} "${parts}")
endforeach()

add_xex_image(FCIP default.xex game.toml ${PARTITIONS_game} game)
# the map editor, which the game launches, and the solo level editor on top of it, picked at launch
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/assets/ige.xex)
    add_xex_image(FCIPEditor ige.xex editor.toml ${PARTITIONS_editor} editor)
    file(GLOB LEVEL_EDITOR_SOURCES CONFIGURE_DEPENDS src/leveleditor/*.cpp)
    target_sources(FCIPEditor PRIVATE ${LEVEL_EDITOR_SOURCES})
endif()

# build_info.h for the version resource, rewritten only when the commit changes
find_package(Git QUIET)
set(BUILD_INFO ${CMAKE_CURRENT_BINARY_DIR}/build_info.h)
add_custom_target(build_info
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${BUILD_INFO} -DGIT=${GIT_EXECUTABLE}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/build_info.cmake
    BYPRODUCTS ${BUILD_INFO}
    VERBATIM)
set_source_files_properties(src/common/version.rc PROPERTIES
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_BINARY_DIR}
    OBJECT_DEPENDS "${BUILD_INFO};${CMAKE_CURRENT_SOURCE_DIR}/src/d3d/pipeline_list.bin")
foreach(target FCIP FCIPEditor)
    if(TARGET ${target})
        target_sources(${target} PRIVATE src/common/version.rc)
        add_dependencies(${target} build_info)
        target_link_libraries(${target} PRIVATE fidelityfx)
    endif()
endforeach()
