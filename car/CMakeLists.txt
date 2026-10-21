cmake_minimum_required(VERSION 3.25)
# Version of the car software (package, core, image).  CI passes the release
# tag here (vX.Y.Z -> X.Y.Z) so every artifact of a release carries its number.
set(RUMICAR_VERSION "0.1.0" CACHE STRING "Version of rumicar-core and the rumicar-zero2cam package (X.Y.Z)")
if(NOT RUMICAR_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "RUMICAR_VERSION must be X.Y.Z, got '${RUMICAR_VERSION}'")
endif()
project(RumiCarZero2Cam VERSION ${RUMICAR_VERSION} LANGUAGES CXX)

include(GNUInstallDirs)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Install layout (relative to CMAKE_INSTALL_PREFIX; the .deb uses /usr):
#   bin/rumicar-core, bin/rumicar-activate
#   lib/rumicar/logic/<name>-<version>/     shipped logic (manifest.json + .so)
#   share/rumicar/packs/<id>-<version>/     shipped pack
#   lib/systemd/system/rumicar-core.service
#   /etc/rumicar/core.json, /etc/NetworkManager/system-connections/RumiCar-AP.nmconnection
set(RUMICAR_LOGIC_INSTALL_DIR ${CMAKE_INSTALL_PREFIX}/lib/rumicar/logic)
set(RUMICAR_PACKS_INSTALL_DIR ${CMAKE_INSTALL_PREFIX}/share/rumicar/packs)
set(RUMICAR_BINDIR ${CMAKE_INSTALL_FULL_BINDIR})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(RUMICAR_BUILD_CORE "Build rumicar-core (needs libgpiod >= 2, OpenSSL)" ON)
option(RUMICAR_BUILD_TESTS "Build unit tests" ON)

add_compile_options(-Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion)

set(TOF_DRIVE_VERSION "0.1.0")   # version of the shipped driving logic (logic/tof_drive)

find_package(nlohmann_json 3.11 REQUIRED)

# Shared contract header (C ABI between core and logic plug-ins).
add_library(rumicar_api INTERFACE)
target_include_directories(rumicar_api INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_subdirectory(logic)

if(RUMICAR_BUILD_CORE)
  add_subdirectory(core)
endif()

if(RUMICAR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# The shipped pack is installed under its <id>-<version> directory name so it
# can be copied into /var/lib/rumicar/packs as is.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/packs/base/manifest.json RUMICAR_BASE_PACK_MANIFEST)
string(JSON RUMICAR_BASE_PACK_ID GET "${RUMICAR_BASE_PACK_MANIFEST}" id)
string(JSON RUMICAR_BASE_PACK_VERSION GET "${RUMICAR_BASE_PACK_MANIFEST}" version)
set(RUMICAR_SHIPPED_PACK ${RUMICAR_BASE_PACK_ID}-${RUMICAR_BASE_PACK_VERSION})
set(RUMICAR_SHIPPED_LOGIC tof_drive-${TOF_DRIVE_VERSION})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/packs/base/ DESTINATION share/rumicar/packs/${RUMICAR_SHIPPED_PACK})

# --- Debian package (cpack -G DEB in the build directory) ----------------------
set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME rumicar-zero2cam)
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_CONTACT "RumiCar-group <https://github.com/RumiCar-group/RumiCar-Zero2Cam>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "RumiCar Zero2Cam car software: control loop, ToF driving logic, access point, PC link")
set(CPACK_SET_DESTDIR ON)   # install() rules use CMAKE_INSTALL_PREFIX (/usr) and absolute /etc paths
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION misc)
set(CPACK_DEBIAN_PACKAGE_PRIORITY optional)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)       # library dependencies from the linked binaries (dpkg-shlibdeps)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "network-manager, dnsmasq-base, python3, iw")
if(RUMICAR_WITH_CAMERA)
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "${CPACK_DEBIAN_PACKAGE_DEPENDS}, libcamera-ipa")
endif()
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/deploy/debian/postinst.in ${CMAKE_BINARY_DIR}/debian/postinst @ONLY
  FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
  ${CMAKE_BINARY_DIR}/debian/postinst
  ${CMAKE_CURRENT_SOURCE_DIR}/deploy/debian/prerm
  ${CMAKE_CURRENT_SOURCE_DIR}/deploy/debian/postrm
  ${CMAKE_CURRENT_SOURCE_DIR}/deploy/debian/conffiles)
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)
include(CPack)
