# HKey.exe: tray app + hooks. Built on Windows (MSVC or MinGW/GCC).
# One version for every platform: project.yml's MARKETING_VERSION, unless -DHKEY_VERSION overrides it.
unset(HKEY_VERSION CACHE)
if(NOT HKEY_VERSION)
  if(VIKEY_VERSION)
    set(HKEY_VERSION "${VIKEY_VERSION}")
  else()
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../../project.yml" _mv REGEX "MARKETING_VERSION: \"")
    string(REGEX REPLACE ".*MARKETING_VERSION: \"([^\"]+)\".*" "\\1" HKEY_VERSION "${_mv}")
  endif()
endif()
message(STATUS "HKey version ${HKEY_VERSION}")

string(REPLACE "." ";" _hkey_ver_parts "${HKEY_VERSION}")
list(LENGTH _hkey_ver_parts _hkey_ver_len)
set(HKEY_VER_MAJOR 1)
set(HKEY_VER_MINOR 0)
set(HKEY_VER_PATCH 0)
if(_hkey_ver_len GREATER 0)
  list(GET _hkey_ver_parts 0 HKEY_VER_MAJOR)
endif()
if(_hkey_ver_len GREATER 1)
  list(GET _hkey_ver_parts 1 HKEY_VER_MINOR)
endif()
if(_hkey_ver_len GREATER 2)
  list(GET _hkey_ver_parts 2 HKEY_VER_PATCH)
endif()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/HKeyVersion.h.in" "${CMAKE_CURRENT_BINARY_DIR}/HKeyVersion.h" @ONLY)

if(MSVC)
  set(HKEY_RC_SRC HKey.rc)
else()
  file(RELATIVE_PATH _hkey_rc_inc "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/HKey_rc.obj
    COMMAND windres -O coff -I . -I "${_hkey_rc_inc}" "${CMAKE_CURRENT_SOURCE_DIR}/HKey.rc" -o "${CMAKE_CURRENT_BINARY_DIR}/HKey_rc.obj"
    DEPENDS
      ${CMAKE_CURRENT_SOURCE_DIR}/HKey.rc
      ${CMAKE_CURRENT_BINARY_DIR}/HKeyVersion.h
      ${CMAKE_CURRENT_SOURCE_DIR}/resource.h
      ${CMAKE_CURRENT_SOURCE_DIR}/HKey.ico
      ${CMAKE_CURRENT_SOURCE_DIR}/HKey.manifest
      ${CMAKE_CURRENT_SOURCE_DIR}/Emoji.json
      ${CMAKE_CURRENT_SOURCE_DIR}/Sounds/SwitchToVietnamese.wav
      ${CMAKE_CURRENT_SOURCE_DIR}/Sounds/SwitchToEnglish.wav
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  )
  set(HKEY_RC_SRC ${CMAKE_CURRENT_BINARY_DIR}/HKey_rc.obj)
endif()

add_executable(hkey WIN32 main.cpp Core.cpp Macro.cpp Prefs.cpp Site.cpp Settings.cpp Emoji.cpp Updater.cpp ${HKEY_RC_SRC})
if(MSVC)
  target_link_options(hkey PRIVATE /MANIFEST:NO)
else()
  target_link_options(hkey PRIVATE -static -static-libgcc -static-libstdc++ -mwindows -municode)
endif()
set_target_properties(hkey PROPERTIES OUTPUT_NAME HKey)
target_include_directories(hkey PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(hkey PRIVATE UNICODE _UNICODE HKEY_VERSION="${HKEY_VERSION}" VIKEY_VERSION="${HKEY_VERSION}")
target_link_libraries(hkey PRIVATE vikey_engine user32 shell32 gdi32 advapi32 imm32 psapi ole32 oleaut32 comctl32 comdlg32 gdiplus winhttp winmm d2d1 dwrite dwmapi normaliz wintrust bcrypt)

# Alias vikey target to hkey for compatibility
add_custom_target(vikey DEPENDS hkey)

# Live test runner: a window with an edit control, types through SendInput while HKey runs, reads the text back.
add_executable(hkey_livetest ../Tests/livetest.cpp Prefs.cpp)
if(NOT MSVC)
  target_link_options(hkey_livetest PRIVATE -static -static-libgcc -static-libstdc++ -municode)
endif()
target_include_directories(hkey_livetest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hkey_livetest PRIVATE vikey_engine user32 advapi32 shell32)
target_compile_definitions(hkey_livetest PRIVATE UNICODE _UNICODE HKEY_VERSION="${HKEY_VERSION}")

# Macro test runner
add_executable(macro_test ../Tests/macro_test.cpp Macro.cpp Prefs.cpp)
if(NOT MSVC)
  target_link_options(macro_test PRIVATE -static -static-libgcc -static-libstdc++ -municode)
endif()
target_include_directories(macro_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(macro_test PRIVATE vikey_engine user32 advapi32 shell32)
target_compile_definitions(macro_test PRIVATE UNICODE _UNICODE HKEY_VERSION="${HKEY_VERSION}")

# Unit test runner
add_executable(unit_test ../Tests/unit_test.cpp Prefs.cpp)
if(NOT MSVC)
  target_link_options(unit_test PRIVATE -static -static-libgcc -static-libstdc++ -municode)
endif()
target_include_directories(unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../../Engine)
target_link_libraries(unit_test PRIVATE vikey_engine user32 advapi32 shell32 ole32)
target_compile_definitions(unit_test PRIVATE UNICODE _UNICODE HKEY_VERSION="${HKEY_VERSION}")
