# Development console: PE loading and bounded HLE execution, independent of the game launcher.
add_executable(dream-wince main.cpp)
target_link_libraries(dream-wince PRIVATE dream::runtime)
dream_target_defaults(dream-wince)
# With SDL3, the title's sound is heard: its CD audio from the disc image (cd_audio.cpp) and the
# AICA's output.
if(TARGET dream::audio)
  target_sources(dream-wince PRIVATE cd_audio.cpp)
  target_link_libraries(dream-wince PRIVATE dream::audio)
  target_compile_definitions(dream-wince PRIVATE DREAM_WINCE_AUDIO=1)
endif()
# With the renderer, --window shows the console live and reads the pad (live.cpp).
if(TARGET dream::render_vk)
  target_sources(dream-wince PRIVATE live.cpp)
  target_link_libraries(dream-wince PRIVATE dream::render_vk)
  target_compile_definitions(dream-wince PRIVATE DREAM_WINCE_WINDOW=1)
  if(WIN32)
    add_custom_command(TARGET dream-wince POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E
              $<IF:$<BOOL:$<TARGET_RUNTIME_DLLS:dream-wince>>,copy_if_different,true>
              $<TARGET_RUNTIME_DLLS:dream-wince> $<TARGET_FILE_DIR:dream-wince>
      COMMAND_EXPAND_LISTS)
  endif()
endif()
if(DREAM_BUILD_TESTS)
  add_test(NAME wince.help COMMAND dream-wince --help)
  if(DREAM_DEV_INTERPRETER)
    if(NOT DEFINED Python3_EXECUTABLE AND EXISTS "${DREAM_ROOT}/.venv/Scripts/python.exe")
      set(Python3_EXECUTABLE "${DREAM_ROOT}/.venv/Scripts/python.exe")
    endif()
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
      add_test(NAME wince.directx COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_directx_fixture.py" "$<TARGET_FILE:dream-wince>"
        "${CMAKE_CURRENT_BINARY_DIR}")
      set_tests_properties(wince.directx PROPERTIES TIMEOUT 60)
      add_test(NAME wince.synchronization COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_sync_fixture.py" "$<TARGET_FILE:dream-wince>"
        "${CMAKE_CURRENT_BINARY_DIR}")
      set_tests_properties(wince.synchronization PROPERTIES TIMEOUT 90)
      add_test(NAME wince.arithmetic COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_arithmetic_fixture.py" "$<TARGET_FILE:dream-wince>"
        "${CMAKE_CURRENT_BINARY_DIR}")
      set_tests_properties(wince.arithmetic PROPERTIES TIMEOUT 90)
    else()
      message(STATUS "WinCE guest media integration test requires Python 3")
    endif()
  endif()
endif()
