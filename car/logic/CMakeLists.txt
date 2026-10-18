# Driving-logic plug-ins.  Each plug-in is a shared library exporting
# rumi_logic_get_api() (see include/rumicar/logic_api.h).

set(TOF_DRIVE_VERSION "0.1.0")

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

install(TARGETS tof_drive LIBRARY DESTINATION lib/rumicar/logic/tof_drive-${TOF_DRIVE_VERSION})
