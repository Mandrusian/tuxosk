cmake_minimum_required(VERSION 3.16)
project(tuxosk CXX)
set(CMAKE_CXX_STANDARD 17)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(APPLE)
  set(_tuxosk_default_wayland OFF)
else()
  set(_tuxosk_default_wayland ON)
endif()
option(BUILD_WAYLAND "Build the Wayland tuxosk executable" ${_tuxosk_default_wayland})

find_package(PkgConfig REQUIRED)
pkg_check_modules(CAIRO REQUIRED cairo)

add_library(oskcore STATIC src/lexicon.cpp src/swipe.cpp src/layout.cpp src/engine.cpp src/ui.cpp)
target_include_directories(oskcore PUBLIC ${CAIRO_INCLUDE_DIRS})
target_link_directories(oskcore PUBLIC ${CAIRO_LIBRARY_DIRS})
target_link_libraries(oskcore PUBLIC ${CAIRO_LIBRARIES})
target_compile_options(oskcore PRIVATE -O2)

if(BUILD_WAYLAND)
  enable_language(C)
  pkg_check_modules(WAYLAND_DEPS REQUIRED glib-2.0 gio-2.0 wayland-client xkbcommon)
  find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

  set(GEN ${CMAKE_BINARY_DIR}/gen)
  file(MAKE_DIRECTORY ${GEN})
  set(GEN_SRCS)
  foreach(p input-method-unstable-v2 virtual-keyboard-unstable-v1 wlr-layer-shell-unstable-v1 xdg-shell)
    add_custom_command(OUTPUT ${GEN}/${p}-client-protocol.h ${GEN}/${p}-protocol.c
      COMMAND ${WAYLAND_SCANNER} client-header ${CMAKE_SOURCE_DIR}/protocols/${p}.xml ${GEN}/${p}-client-protocol.h
      COMMAND sed -i "s/namespace/name_space/g" ${GEN}/${p}-client-protocol.h
      COMMAND ${WAYLAND_SCANNER} private-code ${CMAKE_SOURCE_DIR}/protocols/${p}.xml ${GEN}/${p}-protocol.c
      DEPENDS ${CMAKE_SOURCE_DIR}/protocols/${p}.xml)
    list(APPEND GEN_SRCS ${GEN}/${p}-protocol.c ${GEN}/${p}-client-protocol.h)
  endforeach()

  add_executable(tuxosk src/main.cpp ${GEN_SRCS})
  set_source_files_properties(${GEN}/input-method-unstable-v2-protocol.c ${GEN}/virtual-keyboard-unstable-v1-protocol.c
    ${GEN}/wlr-layer-shell-unstable-v1-protocol.c ${GEN}/xdg-shell-protocol.c PROPERTIES LANGUAGE C)
  set_target_properties(tuxosk PROPERTIES LINKER_LANGUAGE CXX)
  target_include_directories(tuxosk PRIVATE ${GEN} ${WAYLAND_DEPS_INCLUDE_DIRS})
  target_link_directories(tuxosk PRIVATE ${WAYLAND_DEPS_LIBRARY_DIRS})
  target_link_libraries(tuxosk oskcore ${WAYLAND_DEPS_LIBRARIES})
  install(TARGETS tuxosk RUNTIME DESTINATION bin)
endif()

add_executable(osk-test tests/test_core.cpp)
target_link_libraries(osk-test oskcore)

add_executable(osk-snapshot tests/snapshot.cpp)
target_link_libraries(osk-snapshot oskcore)

install(FILES data/en.txt data/blocklist.txt DESTINATION share/tuxosk)
install(PROGRAMS scripts/osk-select DESTINATION bin)
install(FILES data/osk-select.desktop DESTINATION share/applications)

enable_testing()
add_test(NAME core COMMAND osk-test ${CMAKE_SOURCE_DIR}/data/en.txt)
add_test(NAME snapshot COMMAND osk-snapshot ${CMAKE_SOURCE_DIR}/data/en.txt ${CMAKE_BINARY_DIR}/snapshot_test)
