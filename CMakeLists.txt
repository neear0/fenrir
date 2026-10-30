cmake_minimum_required(VERSION 3.20)
project(celos_ui LANGUAGES C CXX)

# win32: Win32 window + D3D11, keeps the acrylic blur (windows only)
# glfw:  GLFW window + Vulkan, used on linux and also builds on windows
if(WIN32)
    set(celos_ui_default_platform win32)
else()
    set(celos_ui_default_platform glfw)
endif()
set(CELOSUI_PLATFORM ${celos_ui_default_platform} CACHE STRING "Window and renderer backend: win32 or glfw")
set_property(CACHE CELOSUI_PLATFORM PROPERTY STRINGS win32 glfw)
option(CELOSUI_WAYLAND "Build GLFW with native Wayland support (needs the wayland dev packages), X11 is used when both are available" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    # the c runtime is linked statically, so the only dlls left to load are windows' own
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    # control flow guard and cet shadow stack support for release builds, set here so the downloaded dependencies get them too
    add_compile_options($<$<NOT:$<CONFIG:Debug>>:/guard:cf>)
    add_link_options($<$<NOT:$<CONFIG:Debug>>:/guard:cf> $<$<NOT:$<CONFIG:Debug>>:/CETCOMPAT>)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # some distributions' compilers add these on their own, others don't, so ask for them explicitly (dependencies included)
    include(CheckCCompilerFlag)
    include(CheckPIESupported)
    check_pie_supported()
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)

    add_compile_options(-fstack-protector-strong)
    foreach(flag -fstack-clash-protection -fcf-protection)
        string(MAKE_C_IDENTIFIER "celos_ui_has${flag}" has_flag)
        check_c_compiler_flag(${flag} ${has_flag})
        if(${has_flag})
            add_compile_options(${flag})
        endif()
    endforeach()
    # fortify needs optimization, so debug builds go without it
    add_compile_options("$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=3>")
    add_link_options(-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack)
endif()

# freetype rasterizes the fonts (IMGUI_ENABLE_FREETYPE in imconfig.h). it's downloaded and only the modules CelosUI uses are
# compiled, the same list as cUI.vcxproj. external/freetype_config/ holds the ftmodule.h that registers them
include(FetchContent)
FetchContent_Declare(freetype
    URL https://github.com/freetype/freetype/archive/refs/tags/VER-2-14-3.zip
    URL_HASH SHA256=3f624983444f9e9149707853a792dc5a897f3afc9f23a9b8669c03ac564779d1
    SOURCE_SUBDIR none
)
FetchContent_MakeAvailable(freetype)
set(freetype_sources
    src/base/ftsystem.c src/base/ftinit.c src/base/ftdebug.c src/base/ftbase.c
    src/base/ftbbox.c src/base/ftglyph.c src/base/ftbitmap.c src/base/ftsynth.c src/base/ftmm.c
    src/gzip/ftgzip.c
    src/sfnt/sfnt.c src/truetype/truetype.c src/cff/cff.c
    src/psaux/psaux.c src/pshinter/pshinter.c src/psnames/psnames.c
    src/smooth/smooth.c src/autofit/autofit.c
)
list(TRANSFORM freetype_sources PREPEND ${freetype_SOURCE_DIR}/)
add_library(celos_freetype STATIC ${freetype_sources})
target_include_directories(celos_freetype PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/external/freetype_config ${freetype_SOURCE_DIR}/include)
target_compile_definitions(celos_freetype PRIVATE FT2_BUILD_LIBRARY $<$<C_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)

set(imgui_sources
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/misc/freetype/imgui_freetype.cpp
)

set(celos_ui_sources
    main.cpp
    ui/core/core.cpp
    ui/core/surfaces.cpp
    ui/debug/debug.cpp
    ui/effects/effects.cpp
    ui/functions.cpp
    ui/initialize.cpp
    ui/input.cpp
    ui/inputsystem.cpp
    ui/ui.cpp
    ui/window_drag.cpp
    ui/render/groupbox.cpp
    ui/render/main_window.cpp
    ui/render/panel.cpp
    ui/render/sidebar.cpp
    ui/render/tabs.cpp
    ui/render/text.cpp
    ui/render/titlebar.cpp
    ui/resources/font_data.cpp
    ui/resources/fonts.cpp
    ui/resources/image_data.cpp
    ui/resources/images.cpp
    ui/style/accents.cpp
    ui/style/animations.cpp
    ui/style/presets.cpp
    ui/style/tab_switch.cpp
    ui/style/theme_setup.cpp
    ui/widgets/add_rect_filled_half_rounded.cpp
    ui/widgets/button.cpp
    ui/widgets/checkbox.cpp
    ui/widgets/color_edit.cpp
    ui/widgets/combo.cpp
    ui/widgets/image.cpp
    ui/widgets/segmented.cpp
    ui/widgets/slider.cpp
    ui/widgets/swatches.cpp
    ui/widgets/tab_button.cpp
    ui/widgets/text_item.cpp
    ui/widgets/toggle.cpp
    ui/widgets/functions/render_frame_animated.cpp
    ui/widgets/functions/render_frame_border_animated.cpp
    ui/widgets/functions/render_frame_border_color.cpp
)

if(CELOSUI_PLATFORM STREQUAL "win32")
    if(NOT WIN32)
        message(FATAL_ERROR "CELOSUI_PLATFORM=win32 only builds on windows, use glfw")
    endif()

    list(APPEND celos_ui_sources
        ui/win32/d3d_create.cpp
        ui/win32/effects_d3d11.cpp
        ui/win32/platform_win32.cpp
        ui/win32/window_effects.cpp
        ui/win32/window_functions.cpp
    )
    set(backend_sources
        external/imgui/dx11/imgui_impl_dx11.cpp
    )

    # shaders are compiled at build time by fxc from the windows sdk, preferably the sdk version this build targets
    get_filename_component(windows_kits_root "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]" ABSOLUTE)
    file(GLOB windows_sdk_bin_dirs LIST_DIRECTORIES true "${windows_kits_root}/bin/10.*")
    list(SORT windows_sdk_bin_dirs COMPARE NATURAL ORDER DESCENDING)
    list(TRANSFORM windows_sdk_bin_dirs APPEND /x64)
    find_program(CELOSUI_FXC fxc HINTS "${windows_kits_root}/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64" ${windows_sdk_bin_dirs} REQUIRED)
elseif(CELOSUI_PLATFORM STREQUAL "glfw")
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_WAYLAND ${CELOSUI_WAYLAND} CACHE BOOL "" FORCE)
    FetchContent_Declare(glfw
        URL https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz
        URL_HASH SHA256=c038d34200234d071fae9345bc455e4a8f2f544ab60150765d7704e08f3dac01
        PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/external/glfw_patch/argb_visual.cmake # an alpha channel for vulkan windows on x11
    )
    # headers and volk are only downloaded, their own cmake projects aren't needed (SOURCE_SUBDIR has no CMakeLists.txt)
    FetchContent_Declare(vulkan_headers
        URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.3.296.0.tar.gz
        URL_HASH SHA256=1e872a0be3890784bbe68dcd89b7e017fed77ba95820841848718c98bda6dc33
        SOURCE_SUBDIR none
    )
    FetchContent_Declare(volk
        URL https://github.com/zeux/volk/archive/refs/tags/vulkan-sdk-1.3.296.0.tar.gz
        URL_HASH SHA256=8ffd0e81e29688f4abaa39e598937160b098228f37503903b10d481d4862ab85
        SOURCE_SUBDIR none
    )
    FetchContent_MakeAvailable(glfw vulkan_headers volk)

    # shaders are compiled at build time to spir-v by glslangValidator (glslang-tools package, or the vulkan sdk on windows)
    find_program(CELOSUI_GLSLANG glslangValidator HINTS $ENV{VULKAN_SDK}/bin REQUIRED)

    list(APPEND celos_ui_sources
        ui/glfw/blur_x11.cpp
        ui/glfw/effects_vulkan.cpp
        ui/glfw/platform_glfw.cpp
        ui/glfw/textures_vulkan.cpp
        ui/glfw/vulkan_create.cpp
    )
    set(backend_sources
        external/imgui/vulkan/imgui_impl_vulkan.cpp
        ${volk_SOURCE_DIR}/volk.c
    )
else()
    message(FATAL_ERROR "unknown CELOSUI_PLATFORM '${CELOSUI_PLATFORM}', use win32 or glfw")
endif()

add_executable(celos_ui ${celos_ui_sources} ${imgui_sources} ${backend_sources})
target_link_libraries(celos_ui PRIVATE celos_freetype)

# ui/pch.h is force-included into our sources (the vcxproj does the same), ImGui, the backends and the font data build without it
target_precompile_headers(celos_ui PRIVATE ui/pch.h)
set_source_files_properties(${imgui_sources} ${backend_sources} ui/resources/font_data.cpp ui/resources/image_data.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# compiles an hlsl shader at build time into a header holding it as an array:
# win32: shaders/<name>_dxbc.h with <name>_dxbc for d3d11 (fxc), glfw: shaders/<name>_spirv.h with <name>_spirv for vulkan (glslangValidator)
# usage: celos_ui_shader(<source> vertex|pixel [extra files it depends on])
target_include_directories(celos_ui PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
function(celos_ui_shader source stage)
    get_filename_component(name ${source} NAME_WE)
    if(CELOSUI_PLATFORM STREQUAL "win32")
        if(stage STREQUAL "vertex")
            set(profile vs_4_0)
        else()
            set(profile ps_4_0)
        endif()
        set(output ${CMAKE_CURRENT_BINARY_DIR}/shaders/${name}_dxbc.h)
        set(command ${CELOSUI_FXC} /nologo /T ${profile} /E main /Fh ${output} /Vn ${name}_dxbc ${CMAKE_CURRENT_SOURCE_DIR}/${source})
    else()
        if(stage STREQUAL "vertex")
            set(glslang_stage vert)
        else()
            set(glslang_stage frag)
        endif()
        # -D: the source is hlsl, --auto-sampled-textures: texture + sampler pairs become the combined image samplers ImGui binds
        set(output ${CMAKE_CURRENT_BINARY_DIR}/shaders/${name}_spirv.h)
        set(command ${CELOSUI_GLSLANG} -D -V -S ${glslang_stage} -e main --auto-sampled-textures --vn ${name}_spirv -o ${output} ${CMAKE_CURRENT_SOURCE_DIR}/${source})
    endif()
    add_custom_command(OUTPUT ${output}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders
        COMMAND ${command}
        DEPENDS ${source} ${ARGN}
        COMMENT "Compiling shader ${source}"
        VERBATIM
    )
    target_sources(celos_ui PRIVATE ${output})
endfunction()

foreach(effect gradient glow blur background noise)
    celos_ui_shader(ui/shaders/${effect}.hlsl pixel ui/shaders/effect.hlsli)
endforeach()

if(CELOSUI_PLATFORM STREQUAL "win32")
    celos_ui_shader(external/imgui/dx11/imgui_impl_dx11_vs.hlsl vertex)
    celos_ui_shader(external/imgui/dx11/imgui_impl_dx11_ps.hlsl pixel)
    target_compile_definitions(celos_ui PRIVATE UNICODE _UNICODE) # same character set as the vcxproj
    target_link_libraries(celos_ui PRIVATE d3d11 dwmapi)
else()
    target_include_directories(celos_ui PRIVATE ${vulkan_headers_SOURCE_DIR}/include ${volk_SOURCE_DIR})
    target_compile_definitions(celos_ui PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
    target_link_libraries(celos_ui PRIVATE glfw ${CMAKE_DL_LIBS})
    if(NOT WIN32)
        find_package(X11 REQUIRED) # blur_x11.cpp asks the x server for the compositor's blur
        target_link_libraries(celos_ui PRIVATE X11::X11)
    endif()
endif()

if(MSVC)
    target_compile_options(celos_ui PRIVATE /MP)
    # imported dlls are only looked up in system32, never next to the executable
    target_link_options(celos_ui PRIVATE $<$<NOT:$<CONFIG:Debug>>:/DEPENDENTLOADFLAG:0x800>)
endif()
