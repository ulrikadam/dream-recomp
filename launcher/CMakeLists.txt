# The game library window (WP3.6). The library half reads configuration files only, so it is built
# and tested on every runner; the window needs SDL3 and is built with the renderer.
add_library(dream_launcher_lib STATIC library.cpp config_edit.cpp importer.cpp cards.cpp discs.cpp)
target_link_libraries(dream_launcher_lib PRIVATE dream::toml PUBLIC dream::runtime)
dream_target_defaults(dream_launcher_lib)

if(DREAM_BUILD_TESTS)
  add_executable(dream_launcher_tests tests/test_library.cpp tests/test_config_edit.cpp tests/test_importer.cpp tests/test_cards.cpp tests/test_discs.cpp ${DREAM_ROOT}/runtime/tests/test_main.cpp)
  target_link_libraries(dream_launcher_tests PRIVATE dream_launcher_lib dream::toml)
  target_include_directories(dream_launcher_tests SYSTEM PRIVATE ${DREAM_ROOT}/third_party/doctest)
  dream_target_defaults(dream_launcher_tests)
  add_test(NAME launcher.unit COMMAND dream_launcher_tests)
endif()

# The window is the games' own (dream::render_vk), so it exists only where they can open one.
if(NOT TARGET dream_render_vk)
  return()
endif()

add_executable(dream_launcher main.cpp)
target_link_libraries(dream_launcher PRIVATE dream_launcher_lib dream::render_vk)
# Where games/ is when no --root is given: the checkout the launcher was built from.
target_compile_definitions(dream_launcher PRIVATE DREAM_LAUNCHER_ROOT="${DREAM_ROOT}")
dream_target_defaults(dream_launcher)
if(WIN32)
  add_custom_command(TARGET dream_launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:dream_launcher>
            $<TARGET_FILE_DIR:dream_launcher>
    COMMAND_EXPAND_LISTS)
endif()
