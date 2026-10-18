# bbport for the PS5: the PC port's runtime (src/) and GPU library (gpu/), built with the PS5
# payload SDK (toolchain/prospero.cmake) and linked with the RADV of PS5_Vulkan into one title.
# ps5/tools/build.sh configures and drives this; ps5/tools/link-title.sh links and packages.
cmake_minimum_required(VERSION 3.24)
# Checks compile a static library: nothing links outside link-title.sh.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
project(bbport_ps5 C CXX)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(GPU ${ROOT}/gpu)
set(DEPS ${CMAKE_CURRENT_SOURCE_DIR}/.deps)
set(PS5_VULKAN_DIR "" CACHE PATH "PS5_Vulkan checkout (RADV archive, link recipe, packaging)")
set(BB_VULKAN_INCLUDE ${DEPS}/Vulkan-Headers/include)
list(PREPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(BUILD_SHARED_LIBS OFF)
# Frame pointers: the loader's crash reports walk them. libc++ 18 keeps std::jthread and
# std::stop_token behind -fexperimental-library (libc++experimental.a at the link).
add_compile_options(-O2 -g1 -fno-omit-frame-pointer -ffunction-sections -fdata-sections
                    $<$<COMPILE_LANGUAGE:CXX>:-fexperimental-library>)
add_compile_definitions(BBPORT_PS5=1)

# ---- third-party libraries ----
add_subdirectory(${DEPS}/fmt ${CMAKE_BINARY_DIR}/fmt EXCLUDE_FROM_ALL)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_MAN OFF CACHE BOOL "" FORCE)
set(ZYCORE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
add_subdirectory(${DEPS}/zydis ${CMAKE_BINARY_DIR}/zydis EXCLUDE_FROM_ALL)
add_library(Zydis::Zydis ALIAS Zydis)
set(SIRIT_USE_SYSTEM_SPIRV_HEADERS OFF CACHE BOOL "" FORCE)
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
set(ABSL_INTERNAL_AT_LEAST_CXX17 1 CACHE INTERNAL "")
set(ABSL_INTERNAL_AT_LEAST_CXX20 1 CACHE INTERNAL "")
set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${DEPS}/abseil-cpp ${CMAKE_BINARY_DIR}/absl EXCLUDE_FROM_ALL)
add_subdirectory(${GPU}/third_party/sirit ${CMAKE_BINARY_DIR}/sirit EXCLUDE_FROM_ALL)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory(${GPU}/third_party/miniz ${CMAKE_BINARY_DIR}/miniz EXCLUDE_FROM_ALL)
unset(CMAKE_POLICY_VERSION_MINIMUM)
find_package(Vulkan REQUIRED)
set(FFX_VK_PORTABLE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FFX_VK_PORTABLE_BUILD_PROBE OFF CACHE BOOL "" FORCE)
set(FFX_VK_PORTABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${GPU}/third_party/fsr-vulkan ${CMAKE_BINARY_DIR}/fsr-vulkan EXCLUDE_FROM_ALL)

add_library(bb_headers INTERFACE)
target_include_directories(bb_headers SYSTEM INTERFACE
    ${BB_VULKAN_INCLUDE}
    ${DEPS}/robin-map/include
    ${DEPS}/VulkanMemoryAllocator/include
    ${DEPS}/xxHash
    ${DEPS}/boost
    ${DEPS}/sdl3/include
    ${GPU}/third_party/magic_enum/include
    ${GPU}/third_party/magic_enum/include/magic_enum)
add_library(bb_xxhash STATIC ${DEPS}/xxHash/xxhash.c)

# ---- host shaders: GLSL compiled on the build machine into headers ----
find_program(GLSLANG_COMPILER NAMES glslangValidator glslang REQUIRED)
set(SHADER_SRC ${GPU}/shadps4/video_core/host_shaders)
set(SHADER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)
set(SHADER_DIR ${SHADER_INCLUDE}/video_core/host_shaders)
set(SHADER_HEADERS "")
function(add_host_shader FILENAME SHADER_NAME)
    set(OUT ${SHADER_DIR}/${SHADER_NAME}.h)
    string(TOUPPER ${SHADER_NAME} SYMBOL)
    set(DEFINES ${ARGN})
    list(TRANSFORM DEFINES PREPEND "-D")
    add_custom_command(OUTPUT ${OUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
        COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3 -o ${OUT} --vn ${SYMBOL} ${DEFINES} ${SHADER_SRC}/${FILENAME}
        COMMAND ${CMAKE_COMMAND} "-DHEADER_FILE=${OUT}" -P ${SHADER_SRC}/AddShaderHeaderInclude.cmake
        DEPENDS ${SHADER_SRC}/${FILENAME} VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${OUT} PARENT_SCOPE)
endfunction()
add_host_shader(color_to_ms_depth.frag color_to_ms_depth_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_msaa_frag SRC_MSAA=1)
add_host_shader(fault_buffer_process.comp fault_buffer_process_comp)
add_host_shader(fs_tri.vert fs_tri_vert)
add_host_shader(fsr.comp fsr_easu_comp SAMPLE_EASU=1)
add_host_shader(fsr.comp fsr_rcas_comp SAMPLE_RCAS=1)
add_host_shader(post_process.frag post_process_frag)
add_host_shader(camera_motion.comp camera_motion_comp)
add_host_shader(upscale_merge.comp upscale_merge_comp)
add_host_shader(upscale_reactive.comp upscale_reactive_comp)
add_host_shader(taa.comp taa_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_ldr_comp LDR_OUTPUT=1)
add_host_shader(buffer_multi_copy.comp buffer_multi_copy_comp)
add_host_shader(depth_resample.frag depth_resample_frag)
add_host_shader(depth_resample.frag depth_stencil_resample_frag HAS_STENCIL=1)
add_host_shader(depth_resample.frag depth_stencil_bits_frag STENCIL_BITS=1)
add_host_shader(camera_motion.comp camera_motion_debug_comp DEBUG_OVERLAY=1)
add_host_shader(frame_gen_inputs.comp frame_gen_inputs_comp)
add_host_shader(frame_gen_ui.comp frame_gen_ui_comp)
foreach(BPP IN ITEMS 8 16 32 64 96 128)
    add_host_shader(tiling.comp tiling_micro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=0)
    add_host_shader(tiling.comp tiling_macro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=1)
endforeach()
add_custom_target(host_shaders DEPENDS ${SHADER_HEADERS})

# ---- the GPU library: shadPS4's video core and shader recompiler, the port's shims ----
file(GLOB_RECURSE VIDEO_CORE CONFIGURE_DEPENDS ${GPU}/shadps4/video_core/*.cpp)
file(GLOB_RECURSE RECOMPILER CONFIGURE_DEPENDS ${GPU}/shadps4/shader_recompiler/*.cpp)
list(FILTER VIDEO_CORE EXCLUDE REGEX "renderdoc\\.cpp$")
set(COMMON
    ${GPU}/shadps4/common/decoder.cpp ${GPU}/shadps4/common/error.cpp
    ${GPU}/shadps4/common/io_file.cpp ${GPU}/shadps4/common/native_clock.cpp ${GPU}/shadps4/common/number_utils.cpp
    ${GPU}/shadps4/common/path_util.cpp ${GPU}/shadps4/common/rdtsc.cpp ${GPU}/shadps4/common/slot_vector.cpp
    ${GPU}/shadps4/common/spin_lock.cpp ${GPU}/shadps4/common/recursive_lock.cpp
    ${GPU}/shadps4/common/string_util.cpp ${GPU}/shadps4/common/thread.cpp ${GPU}/shadps4/common/signal_context.cpp
    ${GPU}/shim/common/logging/log.cpp)
# AvPlayer without its FFmpeg source and file streamer (ps5/src/ps5_stubs.cpp).
set(AVPLAYER
    ${GPU}/shadps4/core/libraries/avplayer/avplayer.cpp
    ${GPU}/shadps4/core/libraries/avplayer/avplayer_common.cpp
    ${GPU}/shadps4/core/libraries/avplayer/avplayer_impl.cpp
    ${GPU}/shadps4/core/libraries/avplayer/avplayer_state.cpp)
file(GLOB SHIM_SOURCES CONFIGURE_DEPENDS ${GPU}/shim/*.cpp ${GPU}/shim/*/*.cpp)
add_library(bbgpu STATIC ${VIDEO_CORE} ${RECOMPILER} ${COMMON} ${SHIM_SOURCES} ${AVPLAYER}
    ${GPU}/shadps4/core/libraries/gnmdriver/gnmdriver.cpp
    ${GPU}/shadps4/core/libraries/videoout/driver.cpp ${GPU}/shadps4/core/libraries/videoout/video_out.cpp
    ${GPU}/shadps4/core/libraries/kernel/equeue.cpp
    ${GPU}/third_party/imgui/imgui.cpp ${GPU}/third_party/imgui/imgui_draw.cpp
    ${GPU}/third_party/imgui/imgui_tables.cpp ${GPU}/third_party/imgui/imgui_widgets.cpp
    ${GPU}/third_party/imgui/backends/imgui_impl_vulkan.cpp
    src/ps5_stubs.cpp src/ps5_stats.cpp)
add_dependencies(bbgpu host_shaders)
target_include_directories(bbgpu PRIVATE ${GPU}/shim ${GPU}/shadps4 ${SHADER_INCLUDE}
    ${GPU}/third_party/gcn/include ${GPU}/third_party/half/include
    ${GPU}/third_party/imgui ${GPU}/third_party/imgui/backends ${GPU}/third_party/xbyak ${GPU}
    ${ROOT})
target_compile_definitions(bbgpu PRIVATE BOOST_ASIO_STANDALONE NDEBUG IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
    BB_FONT_PATH="${GPU}/third_party/fonts/DejaVuSans.ttf")
target_compile_options(bbgpu PRIVATE -Wno-deprecated-declarations -Wno-missing-field-initializers
    -Wno-unknown-warning-option "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/src/bb_ps5_compat.h")
target_link_libraries(bbgpu PUBLIC bb_headers fmt::fmt miniz Zydis sirit bb_xxhash absl::flat_hash_map absl::flat_hash_set absl::node_hash_map absl::hash
    ffx-vulkan::portable ffx-vulkan::fsr4-v07-vulkan ffx-vulkan::fsr3-vk-framegeneration-3.1.6)

# ---- the runtime (src/) and the loader (src/probe.c, entered from ps5/src/ps5_main.c) ----
file(GLOB BB_RUNTIME CONFIGURE_DEPENDS ${ROOT}/src/runtime*.c)
file(GLOB BB_ATRAC9 CONFIGURE_DEPENDS ${ROOT}/third_party/LibAtrac9/C/src/*.c)
add_library(atrac9 STATIC ${BB_ATRAC9})
target_compile_options(atrac9 PRIVATE -w)
set_target_properties(atrac9 PROPERTIES C_STANDARD 99)

# Vulkan's commands for code that calls them directly (FSR and parts of shadPS4).
set(VK_TRAMPOLINES ${CMAKE_CURRENT_BINARY_DIR}/vk_trampolines.c)
add_custom_command(OUTPUT ${VK_TRAMPOLINES}
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_vk_trampolines.py
            ${BB_VULKAN_INCLUDE}/vulkan/vulkan_core.h ${VK_TRAMPOLINES}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_vk_trampolines.py VERBATIM)

add_library(bbport OBJECT
    ${ROOT}/src/probe.c ${BB_RUNTIME}
    src/ps5_main.c src/ps5_platform.c sdl3/sdl3_ps5.c console/platform.c ${VK_TRAMPOLINES})
target_include_directories(bbport PRIVATE ${ROOT} ${ROOT}/src)
target_link_libraries(bbport PRIVATE bb_headers)
target_compile_options(bbport PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra>)
set_source_files_properties(${VK_TRAMPOLINES} PROPERTIES COMPILE_OPTIONS "-w")

# ---- link, sign and package: dist/<TITLE_ID>/ ----
# link-title.sh links the loader's objects with every static library of this build (grouped).
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/eboot.elf
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/link-title.sh ${CMAKE_BINARY_DIR} ${PS5_VULKAN_DIR} ${PS5_PAYLOAD_SDK}
            "$<TARGET_OBJECTS:bbport>"
    DEPENDS bbport bbgpu absl::flat_hash_map absl::hash atrac9 "$<TARGET_OBJECTS:bbport>" "$<TARGET_FILE:bbgpu>" fmt::fmt miniz Zydis Zycore sirit bb_xxhash
            ffx-vulkan::portable ffx-vulkan::fsr4-v07-vulkan ffx-vulkan::fsr3-vk-framegeneration-3.1.6
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/link-title.sh
    COMMAND_EXPAND_LISTS VERBATIM)
add_custom_target(title ALL DEPENDS ${CMAKE_BINARY_DIR}/eboot.elf)
