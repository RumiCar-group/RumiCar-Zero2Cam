# rumicar-core: control loop, hardware layer, pack/logic management.

set(RUMICAR_CORE_VERSION "${RUMICAR_FULL_VERSION}")   # reported as core_version in hello

option(RUMICAR_WITH_CAMERA "Capture images with libcamera (needs libcamera-dev and libturbojpeg0-dev)" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GPIOD REQUIRED IMPORTED_TARGET libgpiod>=2.0)
pkg_check_modules(TURBOJPEG REQUIRED IMPORTED_TARGET libturbojpeg)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)
if(RUMICAR_WITH_CAMERA)
  pkg_check_modules(LIBCAMERA REQUIRED IMPORTED_TARGET libcamera>=0.5)
endif()

# Non-hardware parts, reusable by tests and tools.
add_library(rumicar_core_lib STATIC
  src/config.cpp
  src/manifest.cpp
  src/sha256.cpp
  src/logic_loader.cpp
  src/version_store.cpp
  src/car_state.cpp
  src/request_handler.cpp
  src/wifi.cpp
  src/power.cpp
  src/upload.cpp
  src/util/process.cpp
  src/net/socket.cpp
  src/net/telemetry_frame.cpp
  src/net/telemetry_server.cpp
  src/net/control_server.cpp
)
target_include_directories(rumicar_core_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(rumicar_core_lib PUBLIC rumicar_api nlohmann_json::nlohmann_json OpenSSL::Crypto Threads::Threads ${CMAKE_DL_LIBS})

# Hardware layer (needs libgpiod >= 2 and Linux I2C/PWM headers).
add_library(rumicar_hw STATIC
  src/hw/gpio.cpp
  src/hw/pwm.cpp
  src/hw/i2c.cpp
  src/hw/vl53l0x.cpp
  src/hw/drv8835.cpp
)
target_include_directories(rumicar_hw PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(rumicar_hw PUBLIC PkgConfig::GPIOD)

# Camera layer: JPEG encoding (always) and the image source compiled in.
add_library(rumicar_camera STATIC src/camera/jpeg.cpp src/camera/camera_control.cpp)
target_include_directories(rumicar_camera PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(rumicar_camera PUBLIC PkgConfig::TURBOJPEG Threads::Threads)
if(RUMICAR_WITH_CAMERA)
  target_sources(rumicar_camera PRIVATE src/camera/libcamera_source.cpp)
  target_link_libraries(rumicar_camera PUBLIC PkgConfig::LIBCAMERA)
else()
  target_sources(rumicar_camera PRIVATE src/camera/no_camera_source.cpp)
endif()

add_executable(rumicar-core src/main.cpp)
target_compile_definitions(rumicar-core PRIVATE RUMICAR_CORE_VERSION="${RUMICAR_CORE_VERSION}")
target_link_libraries(rumicar-core PRIVATE rumicar_core_lib rumicar_hw rumicar_camera Threads::Threads)

# Replay of a recorded session through a logic and a pack (docs 4.5).  Needs only
# the core library and the JPEG decoder, so it also builds without libcamera.
add_executable(rumicar-replay src/tools/replay.cpp)
target_link_libraries(rumicar-replay PRIVATE rumicar_core_lib rumicar_camera)
install(TARGETS rumicar-replay RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/../deploy/rumicar-core.service.in ${CMAKE_CURRENT_BINARY_DIR}/rumicar-core.service @ONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/../deploy/rumicar-ap.service.in ${CMAKE_CURRENT_BINARY_DIR}/rumicar-ap.service @ONLY)
install(TARGETS rumicar-core RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/../deploy/rumicar-activate DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/rumicar-core.service DESTINATION lib/systemd/system)
install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/../deploy/rumicar-ap-ensure DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/rumicar-ap.service ${CMAKE_CURRENT_SOURCE_DIR}/../deploy/rumicar-ap.timer
        DESTINATION lib/systemd/system)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../deploy/core.json DESTINATION /etc/rumicar)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../deploy/RumiCar-AP.nmconnection DESTINATION /etc/NetworkManager/system-connections
        PERMISSIONS OWNER_READ OWNER_WRITE)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../deploy/config.txt.d/rumicar.txt DESTINATION share/rumicar/config.txt.d)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../deploy/modules-load.d/rumicar.conf DESTINATION lib/modules-load.d)
