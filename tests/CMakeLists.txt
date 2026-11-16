foreach(component IN ITEMS controls calibration simulation configuration accessories shifter presets feedback diagnostics driving_inputs)
  set(test_name chrome_axis_${component})
  set(test_target ${test_name}_test)
  add_executable(${test_target} ${component}_test.cpp)
  target_compile_options(${test_target} PRIVATE /W4 /utf-8)
  target_link_libraries(${test_target} PRIVATE chrome_axis_input)
  add_test(NAME ${test_name} COMMAND ${test_target})
endforeach()

add_executable(chrome_axis_diagnostic_recording_test diagnostic_recording_test.cpp
  ${PROJECT_SOURCE_DIR}/src/app/diagnostic_recording.cpp)
target_include_directories(chrome_axis_diagnostic_recording_test PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_compile_options(chrome_axis_diagnostic_recording_test PRIVATE /W4 /utf-8)
target_link_libraries(chrome_axis_diagnostic_recording_test PRIVATE chrome_axis_input)
add_test(NAME chrome_axis_diagnostic_recording COMMAND chrome_axis_diagnostic_recording_test)

add_executable(chrome_axis_device_inventory_test device_inventory_test.cpp
  ${PROJECT_SOURCE_DIR}/src/app/app_input.cpp
  ${PROJECT_SOURCE_DIR}/src/app/device_inventory.cpp)
target_include_directories(chrome_axis_device_inventory_test PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_compile_options(chrome_axis_device_inventory_test PRIVATE /W4 /utf-8)
target_link_libraries(chrome_axis_device_inventory_test PRIVATE chrome_axis_input)
add_test(NAME chrome_axis_device_inventory COMMAND chrome_axis_device_inventory_test)

add_executable(chrome_axis_manual_transmission_input_test manual_transmission_input_test.cpp)
target_include_directories(chrome_axis_manual_transmission_input_test PRIVATE ${PROJECT_SOURCE_DIR}/src/plugin)
target_compile_options(chrome_axis_manual_transmission_input_test PRIVATE /W4 /utf-8)
add_test(NAME chrome_axis_manual_transmission_input COMMAND chrome_axis_manual_transmission_input_test)

if(CHROME_AXIS_BUILD_PLUGIN)
  add_executable(chrome_axis_manual_transmission_backend_test manual_transmission_backend_test.cpp)
  target_include_directories(chrome_axis_manual_transmission_backend_test PRIVATE ${PROJECT_SOURCE_DIR}/src/plugin)
  target_compile_options(chrome_axis_manual_transmission_backend_test PRIVATE /W4 /utf-8)
  target_link_libraries(chrome_axis_manual_transmission_backend_test PRIVATE RED4ext::SDK)
  add_test(NAME chrome_axis_manual_transmission_backend COMMAND chrome_axis_manual_transmission_backend_test)
endif()

add_executable(chrome_axis_application_plugin_bridge_test application_plugin_bridge_test.cpp)
target_compile_options(chrome_axis_application_plugin_bridge_test PRIVATE /W4 /utf-8)
target_link_libraries(chrome_axis_application_plugin_bridge_test PRIVATE chrome_axis_input)
add_test(NAME chrome_axis_application_plugin_bridge COMMAND chrome_axis_application_plugin_bridge_test)
set_tests_properties(chrome_axis_application_plugin_bridge PROPERTIES TIMEOUT 30)

add_executable(chrome_axis_direct_input_recovery_test direct_input_recovery_test.cpp)
target_compile_options(chrome_axis_direct_input_recovery_test PRIVATE /W4 /utf-8)
target_link_libraries(chrome_axis_direct_input_recovery_test PRIVATE chrome_axis_input)
add_test(NAME chrome_axis_direct_input_recovery COMMAND chrome_axis_direct_input_recovery_test)
set_tests_properties(chrome_axis_direct_input_recovery PROPERTIES TIMEOUT 30)
