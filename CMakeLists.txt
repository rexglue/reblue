# reblue - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(reblue VERSION 1.0.0 LANGUAGES C CXX)

include(CMakeDependentOption)

cmake_dependent_option(REBLUE_D3D12
    "Build the D3D12 renderer backend (OFF selects Vulkan)" ON "WIN32" OFF)
cmake_dependent_option(REBLUE_VULKAN_EXE
    "Build reblue_vk.exe alongside the D3D12 one" ON "WIN32;REBLUE_D3D12" OFF)
option(REBLUE_BUILD_INSTALLER "Build the built-in first-run disc installer" ON)
option(REBLUE_PSO_CAP "Compile in the PSO predictor-miss capture instrument" OFF)
option(REBLUE_PROFILING "Compile in Tracy zones (never in Release)" ON)
option(REBLUE_PCH "Precompile the hot shared headers" ON)

# reblue is the shipping executable and carries every backend-independent
# subsystem. reblue_vk is the second Windows exe, built out of the same
# configure so one build dir holds both runtimes.
set(REBLUE_TARGETS reblue)
set(REBLUE_D3D12_TARGETS "")
if(REBLUE_D3D12)
    set(REBLUE_D3D12_TARGETS reblue)
endif()
if(REBLUE_VULKAN_EXE)
    list(APPEND REBLUE_TARGETS reblue_vk)
endif()
set(REBLUE_VULKAN_TARGETS ${REBLUE_TARGETS})
if(REBLUE_D3D12)
    list(REMOVE_ITEM REBLUE_VULKAN_TARGETS reblue)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(WIN32)
    enable_language(RC)
endif()

set(REBLUE_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(REBLUE_SHADER_COMMON_H
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/XenosRecomp/XenosRecomp/shader_common.h")
cmake_path(GET REBLUE_SHADER_COMMON_H PARENT_PATH REBLUE_SHADER_COMMON_DIR)

# cmake/generated.cmake compiles the recompiled guest code, so this has to be
# known before it runs. Release is what ships, and a shipped build carries no
# profiler.
set(REBLUE_PROFILING_ON
    "$<AND:$<BOOL:${REBLUE_PROFILING}>,$<NOT:$<CONFIG:Release>>>")

include(generated/rexglue.cmake)
if(WIN32)
    find_package(directx-dxc CONFIG REQUIRED)
endif()
add_subdirectory(thirdparty)

foreach(target IN LISTS REBLUE_TARGETS)
    add_executable(${target})
    set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE ON)
endforeach()

# OBJECT and never STATIC: an archive drops the REX_HOOK symbols.
add_library(reblue_common OBJECT)
set(REBLUE_COMMON_TARGETS reblue_common)

function(reblue_add_sources target_list)
    set(files ${ARGN})
    list(TRANSFORM files PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
    foreach(target IN LISTS ${target_list})
        target_sources(${target} PRIVATE ${files})
    endforeach()
    source_group(TREE "${PROJECT_SOURCE_DIR}/src" PREFIX src FILES ${files})
endfunction()

function(reblue_sources)
    reblue_add_sources(REBLUE_COMMON_TARGETS ${ARGN})
endfunction()

# For sources that reach a backend-conditional header.
function(reblue_backend_sources)
    reblue_add_sources(REBLUE_TARGETS ${ARGN})
endfunction()

# For sources that only compile against the D3D12 backend.
function(reblue_d3d12_sources)
    reblue_add_sources(REBLUE_D3D12_TARGETS ${ARGN})
endfunction()

add_subdirectory(src)
include(cmake/generated.cmake)

if(WIN32)
    set_source_files_properties(res/windows/reblue.rc PROPERTIES COMPILE_DEFINITIONS
        "REBLUE_VER_MAJOR=${reblue_VERSION_MAJOR};REBLUE_VER_MINOR=${reblue_VERSION_MINOR};REBLUE_VER_PATCH=${reblue_VERSION_PATCH}")
endif()

# Anything listed here rebuilds the whole target when it changes, so only hot
# and stable headers belong. The COMPILE_LANGUAGE genex keeps the C sources out.
if(REBLUE_PCH)
    foreach(header IN ITEMS
            algorithm atomic chrono cstddef cstdint cstdio cstdlib cstring
            filesystem format functional memory mutex optional string
            string_view unordered_map unordered_set utility vector
            rex/cvar.h rex/hook.h rex/ppc.h rex/runtime.h rex/types.h)
        list(APPEND pch_headers "$<$<COMPILE_LANGUAGE:CXX>:<${header}$<ANGLE-R>>")
    endforeach()
    foreach(header IN ITEMS core/logging.h core/memory_helpers.h)
        list(APPEND pch_headers
            "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/src/${header}>")
    endforeach()
endif()

# The update check speaks HTTPS: WinHTTP on Windows, libcurl elsewhere. Only
# the headers are a build dependency, since http.cpp dlopens the library. A
# build with no endpoint to ask may go without them.
if(NOT WIN32)
    find_package(CURL QUIET)
    if(NOT CURL_FOUND)
        if(REBLUE_UPDATE_BASE)
            message(FATAL_ERROR
                "libcurl headers not found, but REBLUE_UPDATE_BASE is set. ")
        endif()
        message(WARNING
            "libcurl headers not found: the update and content checks compile "
            "to a stub in this build.")
    endif()
endif()

rexglue_apply_target_settings(reblue_common)
target_link_libraries(reblue_common PRIVATE rex::runtime)
# gpu/backend.h and gpu/gpu_profiling.h #error on this.
target_compile_definitions(reblue_common PRIVATE REBLUE_COMMON_TU)
# The hooks include the codegen-emitted reblue_funcs.h.
add_dependencies(reblue_common reblue_codegen)

foreach(target IN LISTS REBLUE_COMMON_TARGETS REBLUE_TARGETS)
    # shader_common.h is the one source of the SPEC_CONSTANT_* values, shared
    # with the recompiler and the host HLSL.
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" src "${REBLUE_GEN_DIR}"
        "${REBLUE_SHADER_COMMON_DIR}" "${REXGLUE_ENTRYPOINT_INCLUDE_DIR}")

    target_compile_definitions(${target} PRIVATE
        $<$<BOOL:${REBLUE_BUILD_INSTALLER}>:REBLUE_BUILD_INSTALLER>
        $<$<BOOL:${REBLUE_PSO_CAP}>:REBLUE_PSO_CAP=1>
        $<${REBLUE_PROFILING_ON}:REXGLUE_ENABLE_PROFILING>)

    # Named for their headers: the OBJECT library links nothing itself.
    target_link_libraries(${target} PRIVATE
        plume
        reblue_thirdparty
        rex::o1heap
        xxHash::xxhash
        # The SDK only links TracyClient into non-Release configs, so a Release
        # build that forces zones on has to pull it in itself.
        $<${REBLUE_PROFILING_ON}:rex::TracyClient>)

    # platform/http.cpp is shared, so this has to reach the object library too.
    if(NOT WIN32 AND CURL_FOUND)
        # Config mode sets the target, module mode the variable.
        if(TARGET CURL::libcurl)
            target_include_directories(${target} PRIVATE
                $<TARGET_PROPERTY:CURL::libcurl,INTERFACE_INCLUDE_DIRECTORIES>)
        else()
            target_include_directories(${target} PRIVATE ${CURL_INCLUDE_DIRS})
        endif()
        target_link_libraries(${target} PRIVATE ${CMAKE_DL_LIBS})
        target_compile_definitions(${target} PRIVATE REBLUE_HAVE_CURL)
    endif()

    # installer/installer_wizard.cpp is shared, so this has to reach the
    # object library too.
    if(REBLUE_BUILD_INSTALLER)
        target_link_libraries(${target} PRIVATE vdflib::vdflib)
    endif()

    if(REBLUE_PCH)
        target_precompile_headers(${target} PRIVATE ${pch_headers})
    endif()
endforeach()

foreach(target IN LISTS REBLUE_TARGETS)
    if(WIN32)
        target_sources(${target} PRIVATE res/windows/reblue.rc res/windows/reblue.manifest)
    endif()

    rexglue_setup_target(${target})

    target_link_libraries(${target} PRIVATE
        reblue_common
        reblue_generated
        ${REBLUE_RECOMP_LIB})
    if(WIN32)
        target_link_libraries(${target} PRIVATE winmm ole32 uuid winhttp shell32)
    endif()
endforeach()

# The SDK stages the same runtime DLLs next to every host exe it configures, so
# serialize the links to keep those POST_BUILD copies off each other.
if(TARGET reblue_vk AND TARGET reblue)
    add_dependencies(reblue_vk reblue)
endif()

# SDL controller mappings, reached through the hid_mappings_file cvar.
foreach(target IN LISTS REBLUE_TARGETS)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/gamecontrollerdb/gamecontrollerdb.txt"
            $<TARGET_FILE_DIR:${target}>
        VERBATIM)
endforeach()

foreach(target IN LISTS REBLUE_D3D12_TARGETS)
    target_compile_definitions(${target} PRIVATE REBLUE_D3D12)
    target_link_libraries(${target} PRIVATE d3d12 dxgi Microsoft::DirectXShaderCompiler)
    if(PLUME_D3D12_AGILITY_SDK_ENABLED)
        find_package(directx-headers CONFIG REQUIRED)
        target_link_libraries(${target} PRIVATE Microsoft::DirectX-Headers)
    endif()
    reblue_stage_dxc(${target})

    # The Agility loader (D3D12SDKPath = ".\\D3D12\\") picks D3D12Core.dll up
    # from there before the system d3d12.dll.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/D3D12Core.dll"
            "$<TARGET_FILE_DIR:${target}>/D3D12/D3D12Core.dll"
        VERBATIM)
endforeach()

foreach(target IN LISTS REBLUE_VULKAN_TARGETS)
    if(WIN32)
        # plume builds VK_KHR_win32_surface straight off the HWND and reaches
        # vulkan-1.dll through volk, so this backend needs no extra libraries. The
        # D3D12 and Vulkan exes coexist in one install, so rename this one.
        set_target_properties(${target} PROPERTIES OUTPUT_NAME reblue_vk)
    elseif(APPLE)
        # plume builds VK_EXT_metal_surface straight off the CAMetalLayer and
        # reaches libMoltenVK/libvulkan through volk, so no find_package(Vulkan)
        # or X11 here either.
        target_compile_definitions(${target} PRIVATE REBLUE_MVK)
        target_link_libraries(${target} PRIVATE
            # iconv (encoding.cpp's SjisToUtf8) is its own library on macOS, unlike
            # glibc where it is built in.
            iconv
            "-framework Cocoa"
            "-framework QuartzCore"
            "-framework Metal"
            "-framework Foundation"
            "-framework IOKit"
            "-framework IOSurface"
            "-framework CoreGraphics")
    else()
        # plume builds the surface through SDL3, which dlopen's the X11 or Wayland
        # client libraries itself, so nothing here links a window system.
        find_package(Vulkan REQUIRED)
        target_link_libraries(${target} PRIVATE Vulkan::Vulkan)
    endif()
endforeach()

# The SDK stages its shared runtime libs next to the exe on Windows only, and
# nothing populates the $ORIGIN rpath it sets on UNIX, so librexruntime and
# libTracyClient link but fail to load. These two are SHARED IMPORTED targets;
# vdflib is an in-tree shared target but lands in its own build subdirectory,
# so it needs the same treatment when the installer (and vdflib with it) is
# built in.
if(UNIX)
    foreach(target IN LISTS REBLUE_TARGETS)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:rex::runtime>
                "$<${REBLUE_PROFILING_ON}:$<TARGET_FILE:rex::TracyClient>>"
                "$<$<BOOL:${REBLUE_BUILD_INSTALLER}>:$<TARGET_FILE:vdflib::vdflib>>"
                $<TARGET_FILE_DIR:${target}>
            COMMAND_EXPAND_LISTS
            VERBATIM)
    endforeach()
endif()

# Only CMake knows the per-config runtime library names, so it renders the
# shipped-file list twice: a header the self-installer compiles in, and a list
# the packaging script reads. Neither is part of the tree that ships.
set(reblue_program_files
    "$<TARGET_FILE_NAME:reblue>"
    "$<TARGET_FILE_NAME:rex::runtime>"
    "gamecontrollerdb.txt")
if(REBLUE_VULKAN_EXE)
    list(APPEND reblue_program_files "$<TARGET_FILE_NAME:reblue_vk>")
endif()
if(REBLUE_D3D12_TARGETS)
    list(APPEND reblue_program_files dxcompiler.dll dxil.dll D3D12/D3D12Core.dll)
endif()

# Only a profiled build stages Tracy, so its entry is emitted whole or left out
# entirely rather than becoming an empty one.
set(reblue_tracy "$<TARGET_FILE_NAME:rex::TracyClient>")

set(reblue_program_files_cxx "")
foreach(f IN LISTS reblue_program_files)
    string(APPEND reblue_program_files_cxx "    \"${f}\",\n")
endforeach()
string(APPEND reblue_program_files_cxx
       "    $<${REBLUE_PROFILING_ON}:\"${reblue_tracy}\",>")

file(GENERATE OUTPUT "${REBLUE_GEN_DIR}/installer/program_files.h" CONTENT
"// Generated by CMakeLists.txt - do not edit.
#pragma once

inline constexpr const char *kProgramFiles[] = {
${reblue_program_files_cxx}
};
")

list(JOIN reblue_program_files "\n" reblue_program_files_txt)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/packaging/program_files.txt"
     CONTENT "${reblue_program_files_txt}\n$<${REBLUE_PROFILING_ON}:${reblue_tracy}\n>")
