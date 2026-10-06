cmake_minimum_required(VERSION 3.16)

# Configure with: cmake -G Ninja -S 3ds -B build3ds -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake
# (3ds/build.sh does this inside the devkitpro/devkitarm Docker image)
project(ac_3ds C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(DECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(PC_DIR "${DECOMP_ROOT}/pc")

add_compile_definitions(
    TARGET_PC
    TARGET_3DS
    VERSION=0
    F3DEX_GBI_2
    NDEBUG
    BUGFIXES
    _LANGUAGE_C
    PC_ENHANCEMENTS
)

# Same UB-neutralizing flags as the PC port's -O2 build.
# -fsigned-char: the game assumes signed plain char (as on x86 and the GC compiler); ARM defaults to unsigned.
add_compile_options(-O2 -fno-strict-aliasing -fwrapv -fsigned-char -ffunction-sections -fdata-sections)
add_compile_options($<$<COMPILE_LANGUAGE:C>:-Dnullptr=NULL>)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include   # SDL.h shim
    ${DECOMP_ROOT}/include
    ${DECOMP_ROOT}/src
    ${DECOMP_ROOT}
    ${PC_DIR}/include
    ${PC_DIR}/lib/glad/include
    ${PC_DIR}/lib/fixnes
)

# --- game sources: same selection as pc/CMakeLists.txt ---
file(GLOB_RECURSE GAME_C_SOURCES CONFIGURE_DEPENDS "${DECOMP_ROOT}/src/*.c")
file(GLOB_RECURSE GAME_CXX_SOURCES CONFIGURE_DEPENDS "${DECOMP_ROOT}/src/*.cpp")

foreach(pat
        ".*/AUS/.*"
        ".*/dolphin/(gx|os|vi|pad|card|dvd|ai|ar|dsp|exi|si|db|mtx|base|gba|amcstubs|OdemuExi2|odenotstub)/.*"
        ".*/TRK_MINNOW_DOLPHIN/.*" ".*/Runtime\\.PPCEABI\\.H/.*" ".*/MSL_C\\.PPCEABI\\.bare\\.H/.*"
        ".*/src/furniture/.*" ".*/src/data/model/obj_e_boat/.*"
        ".*/ac_npc_shop_common\\.c$" ".*/ac_animal_logo_misc\\.c$" ".*/m_item_debug\\.c$"
        ".*/actor/npc/.*_anime\\.c$" ".*/osreport\\.c$" ".*/Famicom/.*" ".*/MSL_C/.*"
        ".*/jaudio_NES/internal/dsp_cardunlock\\.c$" ".*/jaudio_NES/internal/dsp_GBAKey\\.c$"
        ".*/libc64/sprintf\\.c$" ".*/libc64/aprintf\\.c$"
        ".*/libultra/.*" ".*/ReconfigBATs\\.c$" ".*/executor\\.c$" ".*/ef_effect_lib\\.c$"
        ".*/m_card\\.c$" ".*/emu64_utility\\.c$")
    list(FILTER GAME_C_SOURCES EXCLUDE REGEX "${pat}")
endforeach()

foreach(pat
        ".*/AUS/.*" ".*/dolphin/.*" ".*/TRK_MINNOW_DOLPHIN/.*" ".*/Runtime\\.PPCEABI\\.H/.*"
        ".*/MSL_C\\.PPCEABI\\.bare\\.H/.*" ".*/Famicom/.*" ".*/emu64_print\\.cpp$"
        ".*/jsyswrapper_ext\\.cpp$" ".*/jsyswrapper_main\\.cpp$")
    list(FILTER GAME_CXX_SOURCES EXCLUDE REGEX "${pat}")
endforeach()
list(APPEND GAME_CXX_SOURCES
    ${DECOMP_ROOT}/src/static/Famicom/famicom.cpp
    ${DECOMP_ROOT}/src/static/Famicom/famicom_nesinfo.cpp
    ${DECOMP_ROOT}/src/static/Famicom/ks_nes_core.cpp
    ${DECOMP_ROOT}/src/static/Famicom/ks_nes_draw.cpp
)

set_source_files_properties(
    "${DECOMP_ROOT}/src/static/libforest/emu64/emu64.c"
    "${DECOMP_ROOT}/src/static/jaudio_NES/internal/ja_calc.c"
    "${DECOMP_ROOT}/src/static/jaudio_NES/internal/jammain_2.c"
    "${DECOMP_ROOT}/src/static/jaudio_NES/game/game64.c"
    PROPERTIES LANGUAGE CXX
)
set_source_files_properties(${GAME_C_SOURCES} ${GAME_CXX_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;-fpermissive")
set_source_files_properties("${DECOMP_ROOT}/src/main.c" PROPERTIES COMPILE_DEFINITIONS "main=ac_entry")
set_source_files_properties("${DECOMP_ROOT}/src/static/boot.c" PROPERTIES COMPILE_DEFINITIONS "main=boot_main")

# --- PC port layer, reused through the SDL/GL shims (n3ds_tev.c replaces pc_gx_tev.c) ---
set(PC_SOURCES
    ${PC_DIR}/src/pc_main.c
    ${PC_DIR}/src/pc_gx.c
    ${PC_DIR}/src/pc_gx_texture.c
    ${PC_DIR}/src/pc_gbi_runtime.c
    ${PC_DIR}/src/pc_os.c
    ${PC_DIR}/src/pc_dvd.c
    ${PC_DIR}/src/pc_pad.c
    ${PC_DIR}/src/pc_typing.c
    ${DECOMP_ROOT}/src/static/dolphin/pad/Padclamp.c
    ${PC_DIR}/src/pc_card.c
    ${PC_DIR}/src/pc_vi.c
    ${PC_DIR}/src/pc_audio.c
    ${PC_DIR}/src/pc_aram.c
    ${PC_DIR}/src/pc_misc.c
    ${PC_DIR}/src/pc_mtx.c
    ${PC_DIR}/src/pc_m_card.c
    ${PC_DIR}/src/pc_save_bswap.c
    ${PC_DIR}/src/pc_model_viewer.c
    ${PC_DIR}/src/pc_texture_pack.c
    ${PC_DIR}/src/pc_settings.c
    ${PC_DIR}/src/pc_profiler.c
    ${PC_DIR}/src/pc_pause_menu.c
    ${PC_DIR}/src/pc_settings_menu.c
    ${PC_DIR}/src/pc_menu_util.c
    ${PC_DIR}/src/pc_text_draw.c
    ${PC_DIR}/src/pc_keybindings.c
    ${PC_DIR}/src/pc_nes_fixnes.c
    ${PC_DIR}/src/pc_stubs.c
    ${PC_DIR}/src/pc_stubs_cpp.cpp
    ${PC_DIR}/src/pc_assets.c
    ${PC_DIR}/src/pc_disc.c
)
set_source_files_properties(${PC_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;$<$<COMPILE_LANGUAGE:CXX>:-fpermissive>")
set_source_files_properties("${PC_DIR}/src/pc_main.c" PROPERTIES COMPILE_DEFINITIONS "main=pc_main_entry")

set(FIXNES_DIR ${PC_DIR}/lib/fixnes)
file(GLOB FIXNES_MAPPER_SOURCES ${FIXNES_DIR}/mapper/*.c)
set(FIXNES_SOURCES
    ${FIXNES_DIR}/cpu.c ${FIXNES_DIR}/ppu.c ${FIXNES_DIR}/apu.c ${FIXNES_DIR}/mem.c
    ${FIXNES_DIR}/input.c ${FIXNES_DIR}/mapper.c ${FIXNES_DIR}/mapperList.c ${FIXNES_DIR}/vrc_irq.c
    ${FIXNES_DIR}/audio.c ${FIXNES_DIR}/fm2play.c ${FIXNES_DIR}/audio_fds.c ${FIXNES_DIR}/audio_mmc5.c
    ${FIXNES_DIR}/audio_vrc6.c ${FIXNES_DIR}/audio_vrc7.c ${FIXNES_DIR}/audio_n163.c ${FIXNES_DIR}/audio_s5b.c
    ${FIXNES_MAPPER_SOURCES}
)
set_source_files_properties(${FIXNES_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-w"
    COMPILE_DEFINITIONS "COL_TEX_BSWAP;COL_GL_BSWAP;DO_INLINE_ATTRIBS=1")

# --- 3DS platform layer ---
set(N3DS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/n3ds_sys.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/n3ds_sdl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/n3ds_gl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/n3ds_tev.c
)

# libctru exports osGetTime (os.o, pulled in by osGetMemRegionFree etc.), and so does the
# game's libultra layer. Rename the game's copy; the 3DS layer builds separately so the
# libctru headers keep the real name.
ctr_add_shader_library(gx_shader shaders/gx.v.pica)
dkp_add_embedded_binary_library(gx_shader_lib gx_shader)
# GPU self-test shaders (n3ds_gl.c, "gputest")
ctr_add_shader_library(test_min shaders/test_min.v.pica)
ctr_add_shader_library(test_flat shaders/test_flat.v.pica)
ctr_add_shader_library(gx_nolit shaders/gx_nolit.v.pica)
ctr_add_shader_library(test_out5 shaders/test_out5.v.pica)
ctr_add_shader_library(test_in4 shaders/test_in4.v.pica)
ctr_add_shader_library(test_out3 shaders/test_out3.v.pica)
ctr_add_shader_library(test_out4 shaders/test_out4.v.pica)
dkp_add_embedded_binary_library(test_shader_lib test_min test_flat gx_nolit test_out5 test_in4 test_out3 test_out4)

add_library(n3ds_platform OBJECT ${N3DS_SOURCES})
target_link_libraries(n3ds_platform gx_shader_lib test_shader_lib)
add_executable(ac_3ds $<TARGET_OBJECTS:n3ds_platform> ${PC_SOURCES} ${FIXNES_SOURCES} ${GAME_C_SOURCES} ${GAME_CXX_SOURCES})
target_compile_definitions(ac_3ds PRIVATE osGetTime=ac_osGetTime)
target_link_options(ac_3ds PRIVATE -Wl,--gc-sections)
target_link_libraries(ac_3ds gx_shader_lib test_shader_lib citro3d ctru m)

ctr_create_3dsx(ac_3ds
    SMDH "${CMAKE_CURRENT_BINARY_DIR}/ac_3ds.smdh"
)
ctr_generate_smdh("${CMAKE_CURRENT_BINARY_DIR}/ac_3ds.smdh"
    NAME "Animal Crossing"
    DESCRIPTION "GameCube decomp port"
    AUTHOR "ac-decomp / ACGC-PC-Port"
)
