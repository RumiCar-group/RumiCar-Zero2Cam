# Unit tests.  Each test is a stand-alone executable registered with CTest.

add_executable(test_tof_logic test_tof_logic.cpp)
target_link_libraries(test_tof_logic PRIVATE tof_drive_logic)
add_test(NAME tof_logic COMMAND test_tof_logic)

if(RUMICAR_BUILD_CORE)
  add_executable(test_pack test_pack.cpp)
  target_link_libraries(test_pack PRIVATE rumicar_core_lib)
  add_test(NAME pack COMMAND test_pack ${CMAKE_SOURCE_DIR}/packs/base $<TARGET_FILE:tof_drive>)
endif()
