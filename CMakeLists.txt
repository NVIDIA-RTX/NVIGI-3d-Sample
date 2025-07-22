#
# Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.

cmake_minimum_required(VERSION 3.10)

project(NVIGISample)

# Variables
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/_bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_PRODUCTION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(DONUT_SHADERS_OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/framework")
if (MSVC)
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /D_ITERATOR_DEBUG_LEVEL=1")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W3 /MP")
endif()
set(PACKAGE_DIRECTORY "${CMAKE_SOURCE_DIR}/_package")

set(NVIGI_INSTALL_DIR  "${PACKAGE_DIRECTORY}/_bin" CACHE STRING "NVIGI Install Dir")

set (SHADERMAKE_BIN_OUTPUT_PATH "${CMAKE_SOURCE_DIR}/_build/bin" CACHE STRING "")

set(CMAKE_CONFIGURATION_TYPES "Debug;Release;Production")
set(CMAKE_EXE_LINKER_FLAGS_PRODUCTION ${CMAKE_EXE_LINKER_FLAGS_RELEASE})
set(CMAKE_CXX_FLAGS_PRODUCTION ${CMAKE_CXX_FLAGS_RELEASE})
set(CMAKE_C_FLAGS_PRODUCTION ${CMAKE_C_FLAGS_RELEASE})
set(ENV{VULKAN_SDK} ${VULKAN_SDK})

# Add libraries
# option(DONUT_WITH_VULKAN "Force VK off in donut for the NVIGI sample" OFF)
add_subdirectory(donut)


# NVIGI

# Variables
set(NVIGI_CORE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/nvigi_core" CACHE STRING "NVIGI Core Root Directory")
set(NVIGI_PLUGINS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/nvigi_plugins" CACHE STRING "NVIGI Plugins Root Directory")

find_path(NVIGI_CORE_INCLUDE_DIR nvigi.h HINTS "${NVIGI_CORE_ROOT}/include" NO_CACHE)
find_path(NVIGI_PLUGINS_INCLUDE_DIR nvigi_gpt.h HINTS "${NVIGI_PLUGINS_ROOT}/include" NO_CACHE)

find_path(NVIGI_CORE_DLL_DIR nvigi.core.framework.dll HINTS "${NVIGI_CORE_ROOT}/bin/Release_x64" NO_CACHE)
find_path(NVIGI_CORE_PRODUCTION_DLL_DIR nvigi.core.framework.dll HINTS "${NVIGI_CORE_ROOT}/bin/Production_x64" NO_CACHE)
find_path(NVIGI_PLUGIN_DLL_DIR nvigi.plugin.gpt.ggml.cuda.dll HINTS "${NVIGI_PLUGINS_ROOT}/bin/x64" NO_CACHE)

find_path(NVIGI_3RDPARTY_DLL_DIR cublas64_12.dll HINTS ${NVIGI_PLUGIN_DLL_DIR} NO_CACHE)

find_path(NVIGI_CORE_JSON_DIR nvigi.core.framework.json HINTS "${NVIGI_CORE_ROOT}/scripts" "${NVIGI_CORE_ROOT}/bin/Release_x64" NO_CACHE)

find_library(NVIGI_CORE_LIB nvigi.core.framework.lib HINTS "${NVIGI_CORE_ROOT}/lib/Release_x64" NO_CACHE)

find_file(NVIGI_CORE_DLL nvigi.core.framework.dll HINTS "${NVIGI_CORE_ROOT}/bin/Release_x64" NO_CACHE)

file(GLOB NVIGI_DLLS "${NVIGI_CORE_DLL_DIR}nvigi.core.framework.dll")
file(GLOB NVIGI_PRODUCTION_DLLS "${NVIGI_CORE_PRODUCTION_DLL_DIR}nvigi.core.framework.dll")
file(GLOB NVIGI_AGILITY_DLLS "${NVIGI_PLUGIN_DLL_DIR}/D3D12/D3D12Core.dll")
list (APPEND NVIGI_DLLS "${NVIGI_CORE_DLL_DIR}nvigi.plugin.hwi.d3d12.dll")
list (APPEND NVIGI_PRODUCTION_DLLS "${NVIGI_CORE_PRODUCTION_DLL_DIR}nvigi.plugin.hwi.d3d12.dll")
list (APPEND NVIGI_DLLS "${NVIGI_CORE_DLL_DIR}nvigi.plugin.hwi.cuda.dll")
list (APPEND NVIGI_PRODUCTION_DLLS "${NVIGI_CORE_PRODUCTION_DLL_DIR}nvigi.plugin.hwi.cuda.dll")
list (APPEND NVIGI_DLLS "${NVIGI_CORE_DLL_DIR}nvigi.plugin.hwi.common.dll")
list (APPEND NVIGI_PRODUCTION_DLLS "${NVIGI_CORE_PRODUCTION_DLL_DIR}nvigi.plugin.hwi.common.dll")
list (APPEND NVIGI_DLLS "${NVIGI_CORE_DLL_DIR}cig_scheduler_settings.dll")
list (APPEND NVIGI_PRODUCTION_DLLS "${NVIGI_CORE_PRODUCTION_DLL_DIR}cig_scheduler_settings.dll")

list (APPEND NVIGI_DLLS "${NVIGI_PLUGIN_DLL_DIR}nvigi.plugin.gpt.ggml.cuda.dll" "${NVIGI_PLUGIN_DLL_DIR}nvigi.plugin.gpt.ggml.cpu.dll")
list (APPEND NVIGI_PRODUCTION_DLLS "${NVIGI_PLUGIN_DLL_DIR}Production/nvigi.plugin.gpt.ggml.cuda.dll" "${NVIGI_PLUGIN_DLL_DIR}Production/nvigi.plugin.gpt.ggml.cpu.dll")
list (APPEND NVIGI_DLLS "${NVIGI_PLUGIN_DLL_DIR}nvigi.plugin.asr.ggml.cuda.dll" "${NVIGI_PLUGIN_DLL_DIR}nvigi.plugin.asr.ggml.cpu.dll")
list (APPEND NVIGI_PRODUCTION_DLLS "${NVIGI_PLUGIN_DLL_DIR}Production/nvigi.plugin.asr.ggml.cuda.dll" "${NVIGI_PLUGIN_DLL_DIR}Production/nvigi.plugin.asr.ggml.cpu.dll" )

list (APPEND NVIGI_DLLS "${NVIGI_PLUGIN_DLL_DIR}nvigi.plugin.gpt.ggml.d3d12.dll" "${NVIGI_PLUGIN_DLL_DIR}nvigi.plugin.gpt.ggml.vk.dll")
list (APPEND NVIGI_PRODUCTION_DLLS "${NVIGI_PLUGIN_DLL_DIR}Production/nvigi.plugin.gpt.ggml.d3d12.dll" "${NVIGI_PLUGIN_DLL_DIR}Production/nvigi.plugin.gpt.ggml.vk.dll")
list (APPEND NVIGI_DLLS "${NVIGI_PLUGIN_DLL_DIR}nvigi.plugin.asr.ggml.d3d12.dll" "${NVIGI_PLUGIN_DLL_DIR}nvigi.plugin.asr.ggml.vk.dll")
list (APPEND NVIGI_PRODUCTION_DLLS "${NVIGI_PLUGIN_DLL_DIR}Production/nvigi.plugin.asr.ggml.d3d12.dll" "${NVIGI_PLUGIN_DLL_DIR}Production/nvigi.plugin.asr.ggml.vk.dll" )

list (APPEND NVIGI_DLLS "${NVIGI_PLUGIN_DLL_DIR}nvigi.plugin.gpt.cloud.rest.dll")
list (APPEND NVIGI_PRODUCTION_DLLS "${NVIGI_PLUGIN_DLL_DIR}Production/nvigi.plugin.gpt.cloud.rest.dll")
list (APPEND NVIGI_DLLS "${NVIGI_PLUGIN_DLL_DIR}nvigi.plugin.net.dll")
list (APPEND NVIGI_PRODUCTION_DLLS "${NVIGI_PLUGIN_DLL_DIR}Production/nvigi.plugin.net.dll")
list (APPEND NVIGI_DLLS "${NVIGI_PLUGIN_DLL_DIR}nvigi.plugin.tts.asqflow-trt.dll")
list (APPEND NVIGI_PRODUCTION_DLLS "${NVIGI_PLUGIN_DLL_DIR}Production/nvigi.plugin.tts.asqflow-trt.dll")
list (APPEND NVIGI_DLLS "${NVIGI_PLUGIN_DLL_DIR}nvigi.plugin.tts.asqflow-ggml.cuda.dll")
list (APPEND NVIGI_PRODUCTION_DLLS "${NVIGI_PLUGIN_DLL_DIR}Production/nvigi.plugin.tts.asqflow-ggml.cuda.dll")
list (APPEND NVIGI_DLLS "${NVIGI_PLUGIN_DLL_DIR}nvigi.plugin.tts.asqflow-ggml.vk.dll")
list (APPEND NVIGI_PRODUCTION_DLLS "${NVIGI_PLUGIN_DLL_DIR}Production/nvigi.plugin.tts.asqflow-ggml.vk.dll")

# Glob all .bin files from the specified directory
file(GLOB NVIGI_BIN_FILES "${NVIGI_PLUGINS_ROOT}/data/nvigi.test/nvigi.tts/ASqFlow/mel_spectrograms_targets/*.bin")

list (APPEND NVIGI_DLLS ${NVIGI_BIN_FILES})
list (APPEND NVIGI_PRODUCTION_DLLS ${NVIGI_BIN_FILES})
list (APPEND NVIGI_DLLS "${NVIGI_PLUGINS_ROOT}/data/nvigi.test/nvigi.tts/ASqFlow/mel_spectrograms_targets/transcripts.json")
list (APPEND NVIGI_PRODUCTION_DLLS "${NVIGI_PLUGINS_ROOT}/data/nvigi.test/nvigi.tts/ASqFlow/mel_spectrograms_targets/transcripts.json")

# Assume if one is there, they all are...
if (NOT NVIGI_BUILD_ONLY)
    file(GLOB NVIGI_3RDPARTY_DLLS "${NVIGI_3RDPARTY_DLL_DIR}cublas64_12.dll")
    list (APPEND NVIGI_3RDPARTY_DLLS "${NVIGI_3RDPARTY_DLL_DIR}cublasLt64_12.dll")
    list (APPEND NVIGI_3RDPARTY_DLLS "${NVIGI_3RDPARTY_DLL_DIR}onnxruntime.dll")

    list (APPEND NVIGI_3RDPARTY_DLLS "${NVIGI_CORE_DLL_DIR}cudart64_12.dll")

    list (APPEND NVIGI_3RDPARTY_DLLS "${NVIGI_3RDPARTY_DLL_DIR}dxcompiler.dll")

    list (APPEND NVIGI_3RDPARTY_DLLS "${NVIGI_3RDPARTY_DLL_DIR}zlib1.dll")
	list (APPEND NVIGI_3RDPARTY_DLLS "${NVIGI_3RDPARTY_DLL_DIR}libssl-3-x64.dll")
	list (APPEND NVIGI_3RDPARTY_DLLS "${NVIGI_3RDPARTY_DLL_DIR}libcrypto-3-x64.dll")

    list (APPEND NVIGI_3RDPARTY_DLLS "${NVIGI_3RDPARTY_DLL_DIR}nvinfer_10.dll")
    list (APPEND NVIGI_3RDPARTY_DLLS "${NVIGI_3RDPARTY_DLL_DIR}RivaNormalizer.dll")
endif()

# We copy all the Jsons that may be present. 
file(GLOB NVIGI_JSONS ${NVIGI_CORE_JSON_DIR}/*.json)

#Library

add_library(nvigi_libs IMPORTED SHARED GLOBAL)
set_target_properties(nvigi_libs PROPERTIES IMPORTED_IMPLIB ${NVIGI_CORE_LIB})

target_include_directories(nvigi_libs INTERFACE ${NVIGI_CORE_ROOT} ${NVIGI_CORE_INCLUDE_DIR} ${NVIGI_PLUGINS_INCLUDE_DIR})

string(
    APPEND _copy_plugin_dlls_cmd
    "$<IF:$<CONFIG:Production>,"
        "${CMAKE_COMMAND};-E;copy_if_different;${NVIGI_PRODUCTION_DLLS};${CMAKE_RUNTIME_OUTPUT_DIRECTORY},"
        "${CMAKE_COMMAND};-E;copy_if_different;${NVIGI_DLLS};${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    ">"
)
# DLLS copy function
if (WIN32)
    add_custom_target(CopyNVIGIDLLs ALL DEPENDS ${NVIGI_DLLS})
    add_custom_command(TARGET CopyNVIGIDLLs
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
        COMMENT "Created Dir: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
    add_custom_command(TARGET CopyNVIGIDLLs
        COMMAND "${_copy_plugin_dlls_cmd}"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
    if (NOT NVIGI_BUILD_ONLY)
        add_custom_command(TARGET CopyNVIGIDLLs
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${NVIGI_3RDPARTY_DLLS} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/D3D12" 
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${NVIGI_AGILITY_DLLS} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/D3D12/"
            COMMENT Copied DLLs: ${NVIGI_3RDPARTY_DLLS}
        )
    endif()
    if (NVIGI_JSONS)
        add_custom_command(TARGET CopyNVIGIDLLs
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${NVIGI_JSONS} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
            COMMENT "Copied JSONs: ${NVIGI_JSONS}")
    endif()
    add_dependencies(nvigi_libs CopyNVIGIDLLs)

    set_target_properties(CopyNVIGIDLLs PROPERTIES FOLDER "NVIGISample")
endif ()

install(FILES ${NVIGI_DLLS} ${NVIGI_JSONS} DESTINATION "${NVIGI_INSTALL_DIR}")

# END NVIGI

get_target_property(TARGET_LIBRARIES donut_app LINK_LIBRARIES)
set_property(TARGET donut_app PROPERTY LINK_LIBRARIES  ${TARGET_LIBRARIES} )
target_compile_definitions(donut_app PUBLIC $<IF:$<CONFIG:Production>,NVIGI_PRODUCTION,NVIGI_RELEASE>)

# Collect Code
file(GLOB src_header
    "src/NVIGISample.h"
    "src/UIRenderer.h"
    "src/UIRendererLLM.h"
    "src/RenderTargets.h"
    "src/UIData.h"
    )
file(GLOB src_source
    "src/main.cpp"
    "src/NVIGISample.cpp"
    )
file(GLOB src_nvigi 
    "src/nvigi/AudioRecordingHelper.cpp"
    "src/nvigi/AudioRecordingHelper.h"
    "src/nvigi/NVIGIContext.cpp"
    "src/nvigi/NVIGIContext.h"
    )

# Create exe and link
add_executable(NVIGISample WIN32 ${src_header} ${src_source} ${src_overrides} ${src_nvigi})
source_group("Headers"              FILES ${src_header})
source_group("Sources"              FILES ${src_source})
source_group("DeviceManagerOverride"       FILES ${src_overrides})
source_group("NVIGI"       FILES ${src_nvigi})

if (WIN32)
    target_link_libraries(NVIGISample Winmm.lib dsound.lib)
endif()

# Add AGS
option(AMD_AGS "Add AMD AGS support" OFF)
if(AMD_AGS)
    set(AGS_DIR "${CMAKE_SOURCE_DIR}/amd_ags/ags_lib")
    find_library(AGS_LIB amd_ags_x64 HINTS "${AGS_DIR}/lib" NO_CACHE)
    if(AGS_LIB)
        find_path(AGS_INCLUDE_DIR "amd_ags.h" HINTS "${AGS_DIR}/inc" NO_CACHE)
        find_file(AGS_DLL "amd_ags_x64.dll" HINTS "${AGS_DIR}/lib" NO_CACHE)
        add_library(amd_ags STATIC IMPORTED GLOBAL)
        set_target_properties(amd_ags
            PROPERTIES
            IMPORTED_LOCATION "${AGS_LIB}"
            INTERFACE_INCLUDE_DIRECTORIES "${AGS_INCLUDE_DIR}")
        add_compile_definitions(AGS_ENABLE)
        install(FILES ${AGS_DLL} DESTINATION "${NVIGI_INSTALL_DIR}")
    else()
        comment("Warning: Failed to find AMD AGS")
    endif()
    target_link_libraries(NVIGISample amd_ags)

    if (WIN32)
        add_custom_target(CopyAGSdll ALL DEPENDS ${AGS_DLL})
        add_custom_command(TARGET CopyAGSdll
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${AGS_DLL} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
            COMMENT "Copied DLL: ${AGS_DLL}")
        add_dependencies(NVIGISample CopyAGSdll)
    endif ()
endif()

add_compile_definitions(USE_DX12)
add_compile_definitions(USE_VULKAN)

# NDA ONLY DLSS-G DLSS_G Release
add_compile_definitions(DLSSG_ALLOWED)
# END NDA ONLY DLSS-G DLSS_G Release
target_link_libraries(NVIGISample donut_render donut_app donut_engine nvigi_libs)
set_target_properties(NVIGISample PROPERTIES FOLDER "NVIGI Sample")
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT NVIGISample)

# Install
install(TARGETS NVIGISample DESTINATION "${PACKAGE_DIRECTORY}/_bin")
install(DIRECTORY "${CMAKE_SOURCE_DIR}/_bin/shaders" DESTINATION "${PACKAGE_DIRECTORY}/_bin")
install(FILES "${CMAKE_SOURCE_DIR}/media/sponza-plus.scene.json" DESTINATION "${PACKAGE_DIRECTORY}/media")
install(FILES "${CMAKE_SOURCE_DIR}/media/fonts/DroidSans/DroidSans-Mono.ttf" DESTINATION "${PACKAGE_DIRECTORY}/media/fonts/DroidSans")
install(DIRECTORY "${CMAKE_SOURCE_DIR}/media/glTF-Sample-Models/2.0/Sponza" DESTINATION "${PACKAGE_DIRECTORY}/media/glTF-Sample-Models/2.0")
install(DIRECTORY "${CMAKE_SOURCE_DIR}/media/glTF-Sample-Models/2.0/BrainStem" DESTINATION "${PACKAGE_DIRECTORY}/media/glTF-Sample-Models/2.0")
install(FILES "${CMAKE_SOURCE_DIR}/run.bat" DESTINATION "${PACKAGE_DIRECTORY}")
