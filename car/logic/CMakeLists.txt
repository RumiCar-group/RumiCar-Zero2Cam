# Driving-logic plug-ins.  Each plug-in is a shared library exporting
# rumi_logic_get_api() (see include/rumicar/logic_api.h).

# TOF_DRIVE_VERSION is defined in the top-level CMakeLists.txt (shared with packaging).

# Pure computation part, reusable by tests and tools (no I/O).
add_library(tof_drive_logic STATIC
  tof_drive/tof_logic.cpp
  tof_drive/params_json.cpp
)
target_include_directories(tof_drive_logic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tof_drive)
target_link_libraries(tof_drive_logic PUBLIC rumicar_api nlohmann_json::nlohmann_json)

# The plug-in loaded by rumicar-core.
add_library(tof_drive MODULE tof_drive/plugin.cpp)
target_link_libraries(tof_drive PRIVATE tof_drive_logic)
target_compile_definitions(tof_drive PRIVATE TOF_DRIVE_VERSION="${TOF_DRIVE_VERSION}")
set_target_properties(tof_drive PROPERTIES
  PREFIX "lib"
  OUTPUT_NAME "tof_drive"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

# Staged logic directory with its manifest (what gets installed and shipped).
set(TOF_DRIVE_STAGE ${CMAKE_CURRENT_BINARY_DIR}/stage/tof_drive-${TOF_DRIVE_VERSION})
add_custom_command(
  OUTPUT ${TOF_DRIVE_STAGE}/manifest.json
  COMMAND ${CMAKE_COMMAND} -E make_directory ${TOF_DRIVE_STAGE}
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:tof_drive> ${TOF_DRIVE_STAGE}/libtof_drive.so
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/make_manifest.py logic ${TOF_DRIVE_STAGE}
          --name tof_drive --version ${TOF_DRIVE_VERSION} --entry libtof_drive.so --abi 2
          --description "ToF driving logic: avoids walls and obstacles with the three distance sensors; the camera image is not used"
  DEPENDS tof_drive ${CMAKE_SOURCE_DIR}/tools/make_manifest.py
  COMMENT "Writing manifest.json for tof_drive ${TOF_DRIVE_VERSION}"
  VERBATIM)
add_custom_target(tof_drive_manifest ALL DEPENDS ${TOF_DRIVE_STAGE}/manifest.json)
install(DIRECTORY ${TOF_DRIVE_STAGE}/ DESTINATION lib/rumicar/logic/tof_drive-${TOF_DRIVE_VERSION})

# --- cam_drive: camera-assisted driving on top of tof_drive -------------------
# Vision layer (features, pack format, detector evaluation): pure computation,
# reusable by tests and tools.  Floating-point contraction is off so the scores
# match the Python implementation to the last few bits on every target.
add_library(cam_drive_vision STATIC
  cam_drive/vision_features.cpp
  cam_drive/vision_pack.cpp
  cam_drive/vision_eval.cpp
)
target_include_directories(cam_drive_vision PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/cam_drive)
target_compile_options(cam_drive_vision PRIVATE -ffp-contract=off)

# Behaviour (parameters + traffic-light logic on top of tof_drive_logic).
add_library(cam_drive_logic STATIC
  cam_drive/cam_params.cpp
  cam_drive/cam_logic.cpp
)
target_include_directories(cam_drive_logic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/cam_drive)
target_link_libraries(cam_drive_logic PUBLIC cam_drive_vision tof_drive_logic)

# The plug-in loaded by rumicar-core.
add_library(cam_drive MODULE cam_drive/plugin.cpp)
target_link_libraries(cam_drive PRIVATE cam_drive_logic)
target_compile_definitions(cam_drive PRIVATE CAM_DRIVE_VERSION="${CAM_DRIVE_VERSION}")
set_target_properties(cam_drive PROPERTIES
  PREFIX "lib"
  OUTPUT_NAME "cam_drive"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

set(CAM_DRIVE_STAGE ${CMAKE_CURRENT_BINARY_DIR}/stage/cam_drive-${CAM_DRIVE_VERSION})
add_custom_command(
  OUTPUT ${CAM_DRIVE_STAGE}/manifest.json
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CAM_DRIVE_STAGE}
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:cam_drive> ${CAM_DRIVE_STAGE}/libcam_drive.so
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/make_manifest.py logic ${CAM_DRIVE_STAGE}
          --name cam_drive --version ${CAM_DRIVE_VERSION} --entry libcam_drive.so --abi 2
          --description "Camera driving logic: the ToF logic plus traffic lights learned from recordings (stops at red, goes on green); packs may carry vision.bin"
  DEPENDS cam_drive ${CMAKE_SOURCE_DIR}/tools/make_manifest.py
  COMMENT "Writing manifest.json for cam_drive ${CAM_DRIVE_VERSION}"
  VERBATIM)
add_custom_target(cam_drive_manifest ALL DEPENDS ${CAM_DRIVE_STAGE}/manifest.json)
install(DIRECTORY ${CAM_DRIVE_STAGE}/ DESTINATION lib/rumicar/logic/cam_drive-${CAM_DRIVE_VERSION})
