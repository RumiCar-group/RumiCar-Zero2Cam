# rumicar-core: control loop, hardware layer, pack/logic management.

set(RUMICAR_CORE_VERSION "0.1.0")

find_package(PkgConfig REQUIRED)
pkg_check_modules(GPIOD REQUIRED IMPORTED_TARGET libgpiod>=2.0)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)

# Non-hardware parts, reusable by tests and tools.
add_library(rumicar_core_lib STATIC
  src/config.cpp
  src/manifest.cpp
  src/sha256.cpp
  src/logic_loader.cpp
  src/version_store.cpp
)
target_include_directories(rumicar_core_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(rumicar_core_lib PUBLIC rumicar_api nlohmann_json::nlohmann_json OpenSSL::Crypto ${CMAKE_DL_LIBS})

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

add_executable(rumicar-core src/main.cpp)
target_compile_definitions(rumicar-core PRIVATE RUMICAR_CORE_VERSION="${RUMICAR_CORE_VERSION}")
target_link_libraries(rumicar-core PRIVATE rumicar_core_lib rumicar_hw Threads::Threads)

install(TARGETS rumicar-core RUNTIME DESTINATION bin)
install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/../deploy/rumicar-activate DESTINATION bin)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../deploy/rumicar-core.service DESTINATION lib/systemd/system)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../deploy/core.json DESTINATION /etc/rumicar)
