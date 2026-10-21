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
          --name tof_drive --version ${TOF_DRIVE_VERSION} --entry libtof_drive.so
          --description "ToF driving logic: avoids walls and obstacles with the three distance sensors; the camera image is not used"
  DEPENDS tof_drive ${CMAKE_SOURCE_DIR}/tools/make_manifest.py
  COMMENT "Writing manifest.json for tof_drive ${TOF_DRIVE_VERSION}"
  VERBATIM)
add_custom_target(tof_drive_manifest ALL DEPENDS ${TOF_DRIVE_STAGE}/manifest.json)
install(DIRECTORY ${TOF_DRIVE_STAGE}/ DESTINATION lib/rumicar/logic/tof_drive-${TOF_DRIVE_VERSION})
