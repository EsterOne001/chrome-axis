cmake_minimum_required(VERSION 3.24)
project(ChromeAxis VERSION 0.4.26 LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Chrome Axis requires a Windows x64 MSVC build.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
enable_language(RC)

option(CHROME_AXIS_BUILD_PLUGIN "Build the Cyberpunk 2077 RED4ext plugin" ON)

add_library(chrome_axis_input STATIC
  src/core/accessories.cpp
  src/core/calibration.cpp
  src/core/configuration.cpp
  src/core/controls.cpp
  src/core/diagnostics.cpp
  src/core/feedback.cpp
  src/core/input.cpp
  src/core/presets.cpp
  src/core/shifter.cpp
  src/core/simulation.cpp
)
target_include_directories(chrome_axis_input PUBLIC include)
target_compile_definitions(chrome_axis_input PUBLIC
  WIN32_LEAN_AND_MEAN NOMINMAX
  CHROME_AXIS_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  CHROME_AXIS_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  CHROME_AXIS_VERSION_PATCH=${PROJECT_VERSION_PATCH}
  CHROME_AXIS_VERSION_WIDE=L"${PROJECT_VERSION}"
)
target_compile_options(chrome_axis_input PRIVATE /W4 /utf-8)
target_link_libraries(chrome_axis_input PUBLIC dinput8 dxguid ole32 user32)

set(CHROME_AXIS_INSTALL_DIR red4ext/plugins/chrome_axis)
add_executable(chrome_axis_app WIN32
  src/app/main_window.cpp
  src/app/accessories_dialog.cpp
  src/app/app_text.cpp
  src/app/brake_dialog.cpp
  src/app/calibration_guide.cpp
  src/app/diagnostics_dialog.cpp
  src/app/dialog_theme.cpp
  src/app/feedback_dialog.cpp
  src/app/keyboard_dialog.cpp
  src/app/profiles_dialog.cpp
  src/app/shifter_dialog.cpp
  src/app/ui_theme.cpp
  assets/windows/accessories_dialog.rc
  assets/windows/brake_dialog.rc
  assets/windows/diagnostics_dialog.rc
  assets/windows/feedback_dialog.rc
  assets/windows/keyboard_dialog.rc
  assets/windows/product_version.rc
  assets/windows/profiles_dialog.rc
  assets/windows/theme.rc
)
set_target_properties(chrome_axis_app PROPERTIES OUTPUT_NAME ChromeAxis PDB_NAME ChromeAxis-app)
target_include_directories(chrome_axis_app PRIVATE assets)
target_compile_definitions(chrome_axis_app PRIVATE UNICODE _UNICODE CHROME_AXIS_CALIBRATION_APP)
target_compile_options(chrome_axis_app PRIVATE /W4 /utf-8)
target_link_libraries(chrome_axis_app PRIVATE chrome_axis_input advapi32 comctl32 gdi32 gdiplus shell32 shlwapi version)
install(TARGETS chrome_axis_app RUNTIME DESTINATION "${CHROME_AXIS_INSTALL_DIR}")

if(CHROME_AXIS_BUILD_PLUGIN)
  include(FetchContent)
  set(RED4EXT_HEADER_ONLY ON CACHE BOOL "Use the header-only RED4ext SDK")
  FetchContent_Declare(
    red4ext_sdk
    URL https://codeload.github.com/wopss/RED4ext.SDK/tar.gz/5246e659d783e5834cc08a90e56c8932c9ed3449
    URL_HASH SHA256=bfc42548d3e887177717dda61aaba4ccf36a01f91003c97070ad3e1b09c554f9
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(red4ext_sdk)

  add_library(ChromeAxis SHARED
    src/plugin/plugin.cpp
    assets/windows/product_version.rc
  )
  target_compile_options(ChromeAxis PRIVATE /W4 /utf-8)
  target_link_libraries(ChromeAxis PRIVATE chrome_axis_input RED4ext::SDK version)
  install(TARGETS ChromeAxis RUNTIME DESTINATION "${CHROME_AXIS_INSTALL_DIR}")
  install(FILES scripts/driving_context.reds DESTINATION "${CHROME_AXIS_INSTALL_DIR}/scripts")
  install(FILES assets/hud/chrome_axis_hud.archive DESTINATION archive/pc/mod)
endif()

install(FILES README.md README.en.md LICENSE THIRD_PARTY_NOTICES.md DESTINATION "${CHROME_AXIS_INSTALL_DIR}")
install(FILES assets/fonts/OFL.txt DESTINATION "${CHROME_AXIS_INSTALL_DIR}/licenses" RENAME Barlow-OFL.txt)
install(FILES assets/Profil/Lisez-moi.txt DESTINATION "${CHROME_AXIS_INSTALL_DIR}/Profil")

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
