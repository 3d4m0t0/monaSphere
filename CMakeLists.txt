cmake_minimum_required(VERSION 3.16)
project(VRP LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(PkgConfig REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network DBus)
# Wayland single-instance focus uses a Qt private type. Skip it when the
# private component or that type is missing, so older distro Qt still builds.
set(VRP_HAS_QT_GUI_PRIVATE OFF)
find_package(Qt6 QUIET COMPONENTS GuiPrivate)
if(Qt6GuiPrivate_FOUND)
  set(_qt_probe_dir ${CMAKE_BINARY_DIR}/cmake_probe)
  file(MAKE_DIRECTORY ${_qt_probe_dir})
  file(WRITE ${_qt_probe_dir}/qt_wayland_activate.cpp
[=[
#include <QtGui/qpa/qplatformwindow_p.h>
#include <QWindow>
int main() {
  QWindow* w = nullptr;
  auto* native = w->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
  return native ? 1 : 0;
}
]=])
  try_compile(VRP_HAS_QT_GUI_PRIVATE
    ${_qt_probe_dir}/build
    ${_qt_probe_dir}/qt_wayland_activate.cpp
    CMAKE_FLAGS -DCMAKE_CXX_STANDARD=17
    LINK_LIBRARIES Qt6::Gui Qt6::GuiPrivate
  )
endif()
if(VRP_HAS_QT_GUI_PRIVATE)
  message(STATUS "Qt6 GuiPrivate usable — Wayland activation token enabled")
else()
  message(STATUS "Qt6 GuiPrivate unavailable — building without Wayland activation token")
endif()
pkg_check_modules(OPENXR REQUIRED openxr)
pkg_check_modules(VULKAN REQUIRED vulkan)
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
pkg_check_modules(XCB_RANDR REQUIRED xcb xcb-randr)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)

set(VRP_HAS_FFMPEG OFF)
pkg_check_modules(FFMPEG libavcodec libavformat libavutil libswscale libswresample)
if(FFMPEG_FOUND)
  set(VRP_HAS_FFMPEG ON)
  message(STATUS "FFmpeg found — video playback enabled")
else()
  message(WARNING "FFmpeg not found — build placeholder-only video path. Install ffmpeg devel packages and reconfigure.")
endif()

set(VRP_HAS_SDL2 OFF)
pkg_check_modules(SDL2 sdl2)
if(SDL2_FOUND)
  set(VRP_HAS_SDL2 ON)
  message(STATUS "SDL2 found")
endif()

set(VRP_HAS_PULSE OFF)
pkg_check_modules(PULSE libpulse libpulse-simple)
if(PULSE_FOUND)
  set(VRP_HAS_PULSE ON)
  message(STATUS "PulseAudio found — sink enumeration / HMD HDMI profile switch")
else()
  message(WARNING "PulseAudio not found — audio device list limited to default")
endif()

set(VRP_HAS_PIPEWIRE OFF)
pkg_check_modules(PIPEWIRE libpipewire-0.3)
pkg_check_modules(LIBDRM libdrm)
if(PIPEWIRE_FOUND)
  set(VRP_HAS_PIPEWIRE ON)
  message(STATUS "PipeWire found — live ScreenCast capture enabled")
  if(NOT LIBDRM_FOUND)
    message(WARNING "libdrm not found — DMA-BUF fourcc headers may be missing")
  endif()
else()
  message(WARNING "PipeWire not found — live capture disabled")
endif()

# Vendored miniaudio (playback backend)
set(VRP_HAS_MINIAUDIO ON)
message(STATUS "miniaudio playback enabled (third_party/miniaudio)")

# OpenAL Soft — multichannel → HRTF binaural (headers vendored; runtime .so.1)
set(VRP_HAS_OPENAL OFF)
find_library(OPENAL_LIBRARY NAMES openal openal.so.1 libopenal.so.1
  PATHS /usr/lib64 /usr/lib /usr/local/lib64 /usr/local/lib)
if(OPENAL_LIBRARY)
  set(VRP_HAS_OPENAL ON)
  message(STATUS "OpenAL Soft HRTF enabled (${OPENAL_LIBRARY})")
else()
  message(WARNING "libopenal.so.1 not found — multichannel stays stereo downmix")
endif()

set(VRP_SHADERS
  ${CMAKE_SOURCE_DIR}/shaders/scene.vert
  ${CMAKE_SOURCE_DIR}/shaders/scene.frag
  ${CMAKE_SOURCE_DIR}/shaders/grid.vert
  ${CMAKE_SOURCE_DIR}/shaders/grid.frag
  ${CMAKE_SOURCE_DIR}/shaders/star.vert
  ${CMAKE_SOURCE_DIR}/shaders/star.frag
  ${CMAKE_SOURCE_DIR}/shaders/nebula.vert
  ${CMAKE_SOURCE_DIR}/shaders/nebula.frag
  ${CMAKE_SOURCE_DIR}/shaders/milkyway.vert
  ${CMAKE_SOURCE_DIR}/shaders/milkyway.frag
  ${CMAKE_SOURCE_DIR}/shaders/moon.vert
  ${CMAKE_SOURCE_DIR}/shaders/moon.frag
  ${CMAKE_SOURCE_DIR}/shaders/sun.vert
  ${CMAKE_SOURCE_DIR}/shaders/sun.frag
)
set(VRP_SPV_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${VRP_SPV_DIR})

foreach(SRC ${VRP_SHADERS})
  get_filename_component(NAME ${SRC} NAME)
  set(SPV ${VRP_SPV_DIR}/${NAME}.spv)
  add_custom_command(
    OUTPUT ${SPV}
    COMMAND ${GLSLANG_VALIDATOR} -V ${SRC} -o ${SPV}
    DEPENDS ${SRC}
    COMMENT "Compiling ${NAME}"
  )
  list(APPEND VRP_SPV_FILES ${SPV})
endforeach()

# FSR1 compute (EASU / RCAS) — AMD FidelityFX MIT headers
set(FSR_PASS ${CMAKE_SOURCE_DIR}/shaders/fsr_pass.comp)
set(FSR_INCLUDE ${CMAKE_SOURCE_DIR}/third_party/fsr1)
foreach(PASS easu rcas)
  if(PASS STREQUAL "easu")
    set(PASS_DEFS -DSAMPLE_EASU=1 -DSAMPLE_RCAS=0 -DSAMPLE_BILINEAR=0)
  else()
    set(PASS_DEFS -DSAMPLE_EASU=0 -DSAMPLE_RCAS=1 -DSAMPLE_BILINEAR=0)
  endif()
  set(SPV ${VRP_SPV_DIR}/fsr_${PASS}.comp.spv)
  add_custom_command(
    OUTPUT ${SPV}
    COMMAND ${GLSLANG_VALIDATOR} -V -S comp ${PASS_DEFS}
            -I${FSR_INCLUDE} ${FSR_PASS} -o ${SPV}
    DEPENDS ${FSR_PASS} ${FSR_INCLUDE}/ffx_a.h ${FSR_INCLUDE}/ffx_fsr1.h
    COMMENT "Compiling fsr_${PASS}.comp"
  )
  list(APPEND VRP_SPV_FILES ${SPV})
endforeach()

set(SPV ${VRP_SPV_DIR}/fsr_src_blit.comp.spv)
add_custom_command(
  OUTPUT ${SPV}
  COMMAND ${GLSLANG_VALIDATOR} -V ${CMAKE_SOURCE_DIR}/shaders/fsr_src_blit.comp -o ${SPV}
  DEPENDS ${CMAKE_SOURCE_DIR}/shaders/fsr_src_blit.comp
  COMMENT "Compiling fsr_src_blit.comp"
)
list(APPEND VRP_SPV_FILES ${SPV})

set(SPV ${VRP_SPV_DIR}/bc7_mode6.comp.spv)
add_custom_command(
  OUTPUT ${SPV}
  COMMAND ${GLSLANG_VALIDATOR} -V ${CMAKE_SOURCE_DIR}/shaders/bc7_mode6.comp -o ${SPV}
  DEPENDS ${CMAKE_SOURCE_DIR}/shaders/bc7_mode6.comp
  COMMENT "Compiling bc7_mode6.comp"
)
list(APPEND VRP_SPV_FILES ${SPV})

set(SPV ${VRP_SPV_DIR}/bc1.comp.spv)
add_custom_command(
  OUTPUT ${SPV}
  COMMAND ${GLSLANG_VALIDATOR} -V ${CMAKE_SOURCE_DIR}/shaders/bc1.comp -o ${SPV}
  DEPENDS ${CMAKE_SOURCE_DIR}/shaders/bc1.comp
  COMMENT "Compiling bc1.comp"
)
list(APPEND VRP_SPV_FILES ${SPV})

add_custom_target(vrp_shaders ALL DEPENDS ${VRP_SPV_FILES})

# Gamepad / HUD control icons — Qt Gui only, reusable outside the player.
add_library(vrp_gamepad_icons STATIC
  src/ui/gamepad_icons.cpp
)
target_include_directories(vrp_gamepad_icons PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(vrp_gamepad_icons PUBLIC Qt6::Gui)

# All-sky star catalog + mesh — stdlib only, reusable outside the player.
add_library(vrp_starfield STATIC
  src/starfield/starfield.cpp
)
set_target_properties(vrp_starfield PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
target_include_directories(vrp_starfield PUBLIC ${CMAKE_SOURCE_DIR}/src)

# STATIC: the SUSE cmake macro sets BUILD_SHARED_LIBS=ON, which would
# otherwise emit an unpackaged libvrp_core.so that the player requires.
add_library(vrp_core STATIC
  src/xr/xr_vulkan_app.cpp
  src/video/video_decoder.cpp
  src/video/cuda_minimal.cpp
  src/video/cuda_nv12_texture.cpp
  src/video/bc7_thumbs.cpp
  src/video/vaapi_nv12_texture.cpp
  src/audio/audio_player.cpp
  src/audio/openal_spatial.cpp
  src/audio/stereo_surround.cpp
  src/audio/pulse_monitor.cpp
  src/scene/scene_renderer.cpp
  src/scene/fsr1_upscaler.cpp
  src/ui/vr_menu.cpp
  src/net/dlna_client.cpp
  src/input/gamepad.cpp
  src/input/pad_filter.cpp
  src/input/pad_hook_bridge.cpp
  third_party/miniaudio/miniaudio_impl.cpp
)
if(VRP_HAS_PIPEWIRE)
  target_sources(vrp_core PRIVATE
    src/capture/pipewire_capture.cpp
    src/video/pipewire_dma_texture.cpp
  )
endif()
add_dependencies(vrp_core vrp_shaders)
target_include_directories(vrp_core PUBLIC
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party/miniaudio
  ${CMAKE_SOURCE_DIR}/third_party/openal
  ${CMAKE_SOURCE_DIR}/third_party/fsr1
  ${OPENXR_INCLUDE_DIRS}
  ${VULKAN_INCLUDE_DIRS}
)
target_link_libraries(vrp_core PUBLIC
  vrp_starfield
  ${OPENXR_LIBRARIES}
  ${VULKAN_LIBRARIES}
  ${CMAKE_DL_LIBS}
)
target_compile_options(vrp_core PUBLIC ${OPENXR_CFLAGS_OTHER} ${VULKAN_CFLAGS_OTHER})

if(VRP_HAS_FFMPEG)
  target_compile_definitions(vrp_core PUBLIC VRP_HAS_FFMPEG=1)
  target_include_directories(vrp_core PUBLIC ${FFMPEG_INCLUDE_DIRS})
  target_link_libraries(vrp_core PUBLIC ${FFMPEG_LIBRARIES})
  target_compile_options(vrp_core PUBLIC ${FFMPEG_CFLAGS_OTHER})
endif()

if(VRP_HAS_SDL2)
  target_compile_definitions(vrp_core PUBLIC VRP_HAS_SDL2=1)
  target_include_directories(vrp_core PUBLIC ${SDL2_INCLUDE_DIRS})
  target_link_libraries(vrp_core PUBLIC ${SDL2_LIBRARIES})
  target_compile_options(vrp_core PUBLIC ${SDL2_CFLAGS_OTHER})
endif()

if(VRP_HAS_PULSE)
  target_compile_definitions(vrp_core PUBLIC VRP_HAS_PULSE=1)
  target_include_directories(vrp_core PUBLIC ${PULSE_INCLUDE_DIRS})
  target_link_libraries(vrp_core PUBLIC ${PULSE_LIBRARIES})
  target_compile_options(vrp_core PUBLIC ${PULSE_CFLAGS_OTHER})
endif()

if(VRP_HAS_MINIAUDIO)
  target_compile_definitions(vrp_core PUBLIC VRP_HAS_MINIAUDIO=1)
  # miniaudio on Linux needs pthread; Pulse/ALSA backends come via dlopen.
  find_package(Threads REQUIRED)
  target_link_libraries(vrp_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

if(VRP_HAS_OPENAL)
  target_compile_definitions(vrp_core PUBLIC VRP_HAS_OPENAL=1)
  target_link_libraries(vrp_core PUBLIC ${OPENAL_LIBRARY})
endif()

if(VRP_HAS_PIPEWIRE)
  target_compile_definitions(vrp_core PUBLIC VRP_HAS_PIPEWIRE=1)
  target_include_directories(vrp_core PUBLIC ${PIPEWIRE_INCLUDE_DIRS} ${LIBDRM_INCLUDE_DIRS})
  target_link_libraries(vrp_core PUBLIC ${PIPEWIRE_LIBRARIES} ${LIBDRM_LIBRARIES})
  target_compile_options(vrp_core PUBLIC ${PIPEWIRE_CFLAGS_OTHER} ${LIBDRM_CFLAGS_OTHER})
endif()

set(DRM_LEASE_XML ${CMAKE_SOURCE_DIR}/third_party/wayland/drm-lease-v1.xml)
set(DRM_LEASE_HEADER ${CMAKE_BINARY_DIR}/drm-lease-v1-client-protocol.h)
set(DRM_LEASE_CODE ${CMAKE_BINARY_DIR}/drm-lease-v1-protocol.c)
add_custom_command(
  OUTPUT ${DRM_LEASE_HEADER}
  COMMAND ${WAYLAND_SCANNER} client-header ${DRM_LEASE_XML} ${DRM_LEASE_HEADER}
  DEPENDS ${DRM_LEASE_XML}
  VERBATIM
)
add_custom_command(
  OUTPUT ${DRM_LEASE_CODE}
  COMMAND ${WAYLAND_SCANNER} private-code ${DRM_LEASE_XML} ${DRM_LEASE_CODE}
  DEPENDS ${DRM_LEASE_XML}
  VERBATIM
)
add_executable(vrp_player
  apps/vrp_player/main.cpp
  apps/vrp_player/desktop_ui.cpp
  apps/vrp_player/desktop_ui.hpp
  apps/vrp_player/i18n.cpp
  apps/vrp_player/i18n.hpp
  apps/vrp_player/i18n.qrc
  apps/vrp_player/host_window.cpp
  apps/vrp_player/host_window.hpp
  apps/vrp_player/youtube_session.cpp
  apps/vrp_player/youtube_session.hpp
  apps/vrp_player/drm_lease_probe.cpp
  apps/vrp_player/drm_lease_probe.hpp
  apps/vrp_player/x11_direct_probe.cpp
  apps/vrp_player/x11_direct_probe.hpp
  apps/vrp_player/hud_paint.cpp
  apps/vrp_player/hud_paint.hpp
  apps/vrp_player/hud_cache.cpp
  apps/vrp_player/hud_cache.hpp
  src/capture/screencast_portal.cpp
  src/capture/screencast_portal.hpp
  src/capture/portal_request.hpp
  src/capture/chromium_yt.cpp
  src/capture/chromium_yt.hpp
  src/capture/chromium_yt_x11.cpp
  src/capture/chromium_yt_x11.hpp
  src/capture/eis_pointer.cpp
  src/capture/eis_pointer.hpp
  src/capture/kde_eis_pointer.cpp
  src/capture/kde_eis_pointer.hpp
  src/capture/desktop_env.hpp
  src/capture/kde_virtual_display.cpp
  src/capture/kde_virtual_display.hpp
  src/capture/virtual_display_manager.cpp
  src/capture/virtual_display_manager.hpp
  src/capture/kwin_window_placer.cpp
  src/capture/kwin_window_placer.hpp
  ${DRM_LEASE_HEADER}
  ${DRM_LEASE_CODE}
)
set_target_properties(vrp_player PROPERTIES OUTPUT_NAME monasphere)

# LD_PRELOAD pad filter for [アプリ] guests (no /dev/uinput required).
add_library(monasphere_padhook SHARED src/input/pad_hook_preload.cpp)
target_include_directories(monasphere_padhook PRIVATE ${CMAKE_SOURCE_DIR}/src)
find_package(Threads REQUIRED)
target_link_libraries(monasphere_padhook PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
set_target_properties(monasphere_padhook PROPERTIES OUTPUT_NAME monasphere_padhook)
add_dependencies(vrp_player monasphere_padhook)

find_package(X11 REQUIRED)
find_library(XTST_LIBRARY Xtst REQUIRED)
find_library(XRANDR_LIBRARY Xrandr REQUIRED)
# libei-devel optional; fall back to SONAME when only the runtime package is present.
find_library(LIBEI_LIBRARY NAMES ei ei.so.1 PATHS /usr/lib64 /usr/lib)
if(NOT LIBEI_LIBRARY)
  set(LIBEI_LIBRARY /usr/lib64/libei.so.1)
endif()
target_link_libraries(vrp_player PRIVATE vrp_core vrp_gamepad_icons Qt6::Widgets Qt6::Network Qt6::DBus
                      ${WAYLAND_CLIENT_LIBRARIES} ${XCB_RANDR_LIBRARIES} ${CMAKE_DL_LIBS}
                      X11::X11 ${XTST_LIBRARY} ${XRANDR_LIBRARY} ${LIBEI_LIBRARY})
target_include_directories(vrp_player PRIVATE ${CMAKE_BINARY_DIR} ${WAYLAND_CLIENT_INCLUDE_DIRS}
                           ${XCB_RANDR_INCLUDE_DIRS} ${X11_INCLUDE_DIR}
                           ${CMAKE_SOURCE_DIR}/third_party/libei)
target_compile_options(vrp_player PRIVATE ${WAYLAND_CLIENT_CFLAGS_OTHER} ${XCB_RANDR_CFLAGS_OTHER})
if(VRP_HAS_QT_GUI_PRIVATE)
  target_compile_definitions(vrp_player PRIVATE VRP_HAS_QT_GUI_PRIVATE=1)
  target_link_libraries(vrp_player PRIVATE Qt6::GuiPrivate)
endif()
target_include_directories(vrp_player PRIVATE ${CMAKE_SOURCE_DIR}/apps/vrp_player)
add_dependencies(vrp_player vrp_shaders)

# Copy SPIR-V next to binary for convenience
add_custom_command(TARGET vrp_player POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:vrp_player>/shaders
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${VRP_SPV_DIR} $<TARGET_FILE_DIR:vrp_player>/shaders
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/data/stars_wikidata.bin
          $<TARGET_FILE_DIR:vrp_player>/shaders/stars_wikidata.bin
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/data/nebulae_wikidata.bin
          $<TARGET_FILE_DIR:vrp_player>/shaders/nebulae_wikidata.bin
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/data/gaia_sky_edr3_equirect.jpg
          $<TARGET_FILE_DIR:vrp_player>/shaders/gaia_sky_edr3_equirect.jpg
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:vrp_player>/translations
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/translations $<TARGET_FILE_DIR:vrp_player>/translations
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:monasphere_padhook>
          $<TARGET_FILE_DIR:vrp_player>/libmonasphere_padhook.so
)

install(TARGETS vrp_player RUNTIME DESTINATION bin)
install(TARGETS monasphere_padhook LIBRARY DESTINATION lib)
install(TARGETS vrp_gamepad_icons vrp_starfield ARCHIVE DESTINATION lib)
install(FILES ${CMAKE_SOURCE_DIR}/src/ui/gamepad_icons.hpp DESTINATION include/vrp/ui)
install(FILES ${CMAKE_SOURCE_DIR}/src/starfield/starfield.hpp DESTINATION include/vrp/starfield)
install(FILES
  ${CMAKE_SOURCE_DIR}/shaders/star.vert
  ${CMAKE_SOURCE_DIR}/shaders/star.frag
  ${CMAKE_SOURCE_DIR}/shaders/nebula.vert
  ${CMAKE_SOURCE_DIR}/shaders/nebula.frag
  ${CMAKE_SOURCE_DIR}/shaders/milkyway.vert
  ${CMAKE_SOURCE_DIR}/shaders/milkyway.frag
  ${CMAKE_SOURCE_DIR}/shaders/moon.vert
  ${CMAKE_SOURCE_DIR}/shaders/moon.frag
  ${CMAKE_SOURCE_DIR}/shaders/sun.vert
  ${CMAKE_SOURCE_DIR}/shaders/sun.frag
  ${CMAKE_SOURCE_DIR}/data/LICENSE
  ${CMAKE_SOURCE_DIR}/data/README_stars.md
  ${CMAKE_SOURCE_DIR}/data/README_nebulae.md
  ${CMAKE_SOURCE_DIR}/data/README_milkyway.md
  ${CMAKE_SOURCE_DIR}/data/README_live.md
  DESTINATION share/vrp/starfield)
install(FILES ${CMAKE_SOURCE_DIR}/src/starfield/README.md DESTINATION share/doc/vrp_starfield)
install(FILES ${CMAKE_SOURCE_DIR}/icons/hicolor/256x256/apps/monasphere.png
  DESTINATION share/icons/hicolor/256x256/apps)
install(FILES ${CMAKE_SOURCE_DIR}/data/monasphere.desktop
  DESTINATION share/applications)
install(DIRECTORY ${VRP_SPV_DIR}/ DESTINATION share/monasphere/shaders)
install(FILES ${CMAKE_SOURCE_DIR}/data/stars_wikidata.bin
  DESTINATION share/monasphere/shaders)
install(FILES ${CMAKE_SOURCE_DIR}/data/nebulae_wikidata.bin
  DESTINATION share/monasphere/shaders)
install(FILES ${CMAKE_SOURCE_DIR}/data/gaia_sky_edr3_equirect.jpg
  DESTINATION share/monasphere/shaders)
install(FILES
  ${CMAKE_SOURCE_DIR}/data/LICENSE
  ${CMAKE_SOURCE_DIR}/data/README_stars.md
  ${CMAKE_SOURCE_DIR}/data/README_nebulae.md
  ${CMAKE_SOURCE_DIR}/data/README_milkyway.md
  ${CMAKE_SOURCE_DIR}/data/README_live.md
  DESTINATION share/monasphere/shaders)
install(FILES ${CMAKE_SOURCE_DIR}/THIRD_PARTY.md
              ${CMAKE_SOURCE_DIR}/data/README_live.md
  DESTINATION share/doc/monasphere
)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/translations/
  DESTINATION share/monasphere/translations
  FILES_MATCHING PATTERN "monasphere_*.json"
  PATTERN "monasphere_en.json" EXCLUDE
  PATTERN "monasphere_ja.json" EXCLUDE
)
