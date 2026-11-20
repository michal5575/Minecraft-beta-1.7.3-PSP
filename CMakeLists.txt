cmake_minimum_required(VERSION 3.10)
project(MinecraftPSP CXX C ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(MINECRAFT_JAR "${CMAKE_CURRENT_SOURCE_DIR}/../jars/bin/minecraft.jar" CACHE FILEPATH "Beta 1.7.3 client jar with the textures")
set(ASSET_DIR "${CMAKE_CURRENT_BINARY_DIR}/assets")
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_custom_command(
  OUTPUT "${ASSET_DIR}/assets.S" "${ASSET_DIR}/ICON0.png"
  COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/convert_assets.py" "${MINECRAFT_JAR}" "${ASSET_DIR}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/convert_assets.py" "${MINECRAFT_JAR}"
  COMMENT "Converting Minecraft textures")
add_custom_target(assets DEPENDS "${ASSET_DIR}/assets.S")

file(GLOB CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp")

if(PSP)
  file(GLOB PSP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/psp/*.cpp")
  add_executable(minecraft ${CORE_SOURCES} ${PSP_SOURCES} "${ASSET_DIR}/assets.S")
  add_dependencies(minecraft assets)
  target_compile_options(minecraft PRIVATE -O2 -ffast-math -fno-exceptions -fno-rtti -Wall -Wno-unused-function)
  option(MC_AUTOTEST "Scripted screenshot run for PPSSPPHeadless" OFF)
  option(MC_WATERTEST "Scripted dive into water for PPSSPPHeadless" OFF)
  if(MC_AUTOTEST OR MC_WATERTEST)
    target_compile_definitions(minecraft PRIVATE MC_AUTOTEST)
  endif()
  if(MC_WATERTEST)
    target_compile_definitions(minecraft PRIVATE MC_WATERTEST MC_FAT_HEAP)
  endif()
  target_link_libraries(minecraft PRIVATE z pspgu pspge pspdisplay pspctrl psprtc psppower pspdebug psputility pspaudio)
  create_pbp_file(
    TARGET minecraft
    TITLE "Minecraft Beta 1.7.3"
    ICON_PATH "${ASSET_DIR}/ICON0.png"
    VERSION 01.00)
else()
  add_executable(mctest ${CORE_SOURCES} src/host/test_main.cpp "${ASSET_DIR}/assets.S")
  add_dependencies(mctest assets)
  target_compile_options(mctest PRIVATE -O2 -Wall)
endif()
