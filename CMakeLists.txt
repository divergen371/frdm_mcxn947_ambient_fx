cmake_minimum_required(VERSION 3.22.0)

include(${SdkRootDirPath}/cmake/extension/mcux.cmake)

project(frdm_mcxn947_ambient_fx LANGUAGES C CXX ASM)

include(${SdkRootDirPath}/CMakeLists.txt)

mcux_add_source(
    BASE_PATH ${CMAKE_CURRENT_LIST_DIR}
    SOURCES
        src/main.c
        src/board_audio.c
        src/audio_synth.c
        src/ambient_reverb.c
)

mcux_add_include(
    BASE_PATH ${CMAKE_CURRENT_LIST_DIR}
    INCLUDES
        src
)

mcux_remove_armgcc_configuration(
    CC "-fno-builtin"
    LD "-fno-builtin"
)

mcux_remove_armgcc_configuration(
    TARGETS debug
    CC "-O0"
)

mcux_add_armgcc_configuration(
    TARGETS debug
    CC "-Og -g3"
)

mcux_remove_armgcc_configuration(
    TARGETS release
    CC "-Os -mapcs"
    LD "-mapcs"
)

mcux_add_armgcc_configuration(
    TARGETS release
    CC "-O3 -flto=auto"
    LD "-O3 -flto=auto"
)

mcux_convert_binary(BINARY ${APPLICATION_BINARY_DIR}/${MCUX_SDK_PROJECT_NAME}.bin)
mcux_convert_binary(BINARY ${APPLICATION_BINARY_DIR}/${MCUX_SDK_PROJECT_NAME}.hex)
