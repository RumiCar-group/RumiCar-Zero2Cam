# Unit tests.  Each test is a stand-alone executable registered with CTest.

add_executable(test_tof_logic test_tof_logic.cpp)
target_link_libraries(test_tof_logic PRIVATE tof_drive_logic)
add_test(NAME tof_logic COMMAND test_tof_logic)

if(RUMICAR_BUILD_CORE)
  add_executable(test_pack test_pack.cpp)
  target_link_libraries(test_pack PRIVATE rumicar_core_lib)
  add_test(NAME pack COMMAND test_pack ${CMAKE_SOURCE_DIR}/packs/base $<TARGET_FILE:tof_drive>)

  # Control/telemetry servers exercised over real TCP sockets on 127.0.0.1.
  add_executable(test_network test_network.cpp)
  target_link_libraries(test_network PRIVATE rumicar_core_lib)
  add_test(NAME network COMMAND test_network ${CMAKE_SOURCE_DIR}/packs/base)

  add_executable(test_camera test_camera.cpp)
  target_link_libraries(test_camera PRIVATE rumicar_camera rumicar_core_lib)
  add_test(NAME camera COMMAND test_camera)
endif()
