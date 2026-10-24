# Cross-compiles LocalSend for Mac OS 9 (Carbon) with Retro68. From the repository root:
#   mkdir -p build/carbon && cd build/carbon
#   cmake ../../os9 -DCMAKE_TOOLCHAIN_FILE=$RETRO68/powerpc-apple-macos/cmake/retrocarbon.toolchain.cmake
#   make
# or run os9/build.sh. The result is LocalSend.APPL, LocalSend.bin (MacBinary) and LocalSend.dsk (disk image).
cmake_minimum_required(VERSION 3.10)
project(LocalSendClassic C)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_application(LocalSend
    CREATOR "LSPC"
    app_main.c plat_carbon.c
    ${ROOT}/src/ls.c ${ROOT}/src/ls_json.c ${ROOT}/src/ls_compat.c ${ROOT}/src/ls_tls.c
    icon.r size.r)
target_include_directories(LocalSend PRIVATE ${ROOT}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(LocalSend PRIVATE -std=gnu99 -Os -DLS_NO_VSNPRINTF -DLS_OS9)
