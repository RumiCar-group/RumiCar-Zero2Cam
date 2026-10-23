# Unit tests.  Each test is a stand-alone executable registered with CTest.

add_executable(test_tof_logic test_tof_logic.cpp)
target_link_libraries(test_tof_logic PRIVATE tof_drive_logic)
add_test(NAME tof_logic COMMAND test_tof_logic)

# Vision layer against the golden data made by the Python implementation.
add_executable(test_vision test_vision.cpp)
target_link_libraries(test_vision PRIVATE cam_drive_vision nlohmann_json::nlohmann_json)
add_test(NAME vision COMMAND test_vision ${CMAKE_CURRENT_SOURCE_DIR}/data/vision)

# Traffic-light behaviour on synthetic frames, parameters, aux output.
add_executable(test_cam_logic test_cam_logic.cpp)
target_link_libraries(test_cam_logic PRIVATE cam_drive_logic nlohmann_json::nlohmann_json)
add_test(NAME cam_logic COMMAND test_cam_logic ${CMAKE_SOURCE_DIR}/packs/cam-base)

if(RUMICAR_BUILD_CORE)
  add_executable(test_pack test_pack.cpp)
  target_link_libraries(test_pack PRIVATE rumicar_core_lib)
  target_compile_definitions(test_pack PRIVATE TOF_DRIVE_VERSION="${TOF_DRIVE_VERSION}")
  add_dependencies(test_pack py_drive_manifest)
  add_test(NAME pack COMMAND test_pack ${CMAKE_SOURCE_DIR}/packs/base $<TARGET_FILE:tof_drive> ${CMAKE_SOURCE_DIR}/packs/cam-base $<TARGET_FILE:cam_drive>
           ${CMAKE_BINARY_DIR}/logic/stage/py_drive-${PY_DRIVE_VERSION} ${CMAKE_SOURCE_DIR}/packs/py-sample)
  target_compile_definitions(test_pack PRIVATE CAM_DRIVE_VERSION="${CAM_DRIVE_VERSION}")

  # Control/telemetry servers exercised over real TCP sockets on 127.0.0.1.
  add_executable(test_network test_network.cpp)
  target_link_libraries(test_network PRIVATE rumicar_core_lib)
  add_test(NAME network COMMAND test_network ${CMAKE_SOURCE_DIR}/packs/base)

  add_executable(test_camera test_camera.cpp)
  target_link_libraries(test_camera PRIVATE rumicar_camera rumicar_core_lib)
  add_test(NAME camera COMMAND test_camera)

  add_executable(test_util test_util.cpp)
  target_link_libraries(test_util PRIVATE rumicar_core_lib)
  add_test(NAME util COMMAND test_util)
endif()

# py_drive with the real runner.py (needs python3, as on the car).
add_executable(test_py_drive test_py_drive.cpp)
target_link_libraries(test_py_drive PRIVATE rumicar_api ${CMAKE_DL_LIBS})
add_dependencies(test_py_drive py_drive_manifest)
add_test(NAME py_drive COMMAND test_py_drive ${CMAKE_BINARY_DIR}/logic/stage/py_drive-${PY_DRIVE_VERSION} ${CMAKE_SOURCE_DIR}/packs/py-sample)
