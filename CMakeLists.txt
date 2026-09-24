cmake_minimum_required(VERSION 3.16)
project(VRP LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(PkgConfig REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets)
pkg_check_modules(OPENXR REQUIRED openxr)
pkg_check_modules(VULKAN REQUIRED vulkan)

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

# Vendored miniaudio (playback backend)
set(VRP_HAS_MINIAUDIO ON)
message(STATUS "miniaudio playback enabled (third_party/miniaudio)")

set(VRP_SHADERS
  ${CMAKE_SOURCE_DIR}/shaders/scene.vert
  ${CMAKE_SOURCE_DIR}/shaders/scene.frag
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

add_custom_target(vrp_shaders ALL DEPENDS ${VRP_SPV_FILES})

# STATIC: the SUSE cmake macro sets BUILD_SHARED_LIBS=ON, which would
# otherwise emit an unpackaged libvrp_core.so that the player requires.
add_library(vrp_core STATIC
  src/xr/xr_vulkan_app.cpp
  src/video/video_decoder.cpp
  src/video/cuda_minimal.cpp
  src/video/cuda_nv12_texture.cpp
  src/audio/audio_player.cpp
  src/scene/scene_renderer.cpp
  src/scene/fsr1_upscaler.cpp
  src/ui/vr_menu.cpp
  src/input/gamepad.cpp
  third_party/miniaudio/miniaudio_impl.cpp
)
add_dependencies(vrp_core vrp_shaders)
target_include_directories(vrp_core PUBLIC
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party/miniaudio
  ${CMAKE_SOURCE_DIR}/third_party/fsr1
  ${OPENXR_INCLUDE_DIRS}
  ${VULKAN_INCLUDE_DIRS}
)
target_link_libraries(vrp_core PUBLIC
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

add_executable(vrp_player
  apps/vrp_player/main.cpp
  apps/vrp_player/i18n.cpp
  apps/vrp_player/i18n.hpp
  apps/vrp_player/i18n.qrc
  apps/vrp_player/host_window.cpp
  apps/vrp_player/host_window.hpp
  apps/vrp_player/hud_paint.cpp
  apps/vrp_player/hud_paint.hpp
)
set_target_properties(vrp_player PROPERTIES OUTPUT_NAME monasphere)
target_link_libraries(vrp_player PRIVATE vrp_core Qt6::Widgets)
target_include_directories(vrp_player PRIVATE ${CMAKE_SOURCE_DIR}/apps/vrp_player)
add_dependencies(vrp_player vrp_shaders)

# Copy SPIR-V next to binary for convenience
add_custom_command(TARGET vrp_player POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:vrp_player>/shaders
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${VRP_SPV_DIR} $<TARGET_FILE_DIR:vrp_player>/shaders
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:vrp_player>/translations
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/translations $<TARGET_FILE_DIR:vrp_player>/translations
)

install(TARGETS vrp_player RUNTIME DESTINATION bin)
install(FILES ${CMAKE_SOURCE_DIR}/icons/hicolor/256x256/apps/monasphere.png
  DESTINATION share/icons/hicolor/256x256/apps)
install(FILES ${CMAKE_SOURCE_DIR}/data/monasphere.desktop
  DESTINATION share/applications)
install(DIRECTORY ${VRP_SPV_DIR}/ DESTINATION share/monasphere/shaders)
install(FILES ${CMAKE_SOURCE_DIR}/THIRD_PARTY.md
  DESTINATION share/doc/monasphere
)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/translations/
  DESTINATION share/monasphere/translations
  FILES_MATCHING PATTERN "monasphere_*.json"
  PATTERN "monasphere_en.json" EXCLUDE
  PATTERN "monasphere_ja.json" EXCLUDE
)
