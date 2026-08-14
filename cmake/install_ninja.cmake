cmake_minimum_required(VERSION 4.4.2)

if(NOT DEFINED INSTALL_DIR OR INSTALL_DIR STREQUAL "")
	message(FATAL_ERROR "Set INSTALL_DIR to the directory that will contain Ninja")
endif()

set(ninja_version 1.13.2)
set(host_processor "${CMAKE_HOST_SYSTEM_PROCESSOR}")
if(host_processor STREQUAL "" AND DEFINED ENV{RUNNER_ARCH})
	set(host_processor "$ENV{RUNNER_ARCH}")
endif()
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
	if(host_processor MATCHES "^(ARM64|arm64|aarch64)$")
		set(ninja_asset ninja-winarm64.zip)
		set(ninja_sha256 e52f0bdef9dfb1003229dbd6508a508c4073fd017247002adc66e5e806cb0391)
	else()
		set(ninja_asset ninja-win.zip)
		set(ninja_sha256 07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65)
	endif()
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
	set(ninja_asset ninja-mac.zip)
	set(ninja_sha256 c99048673aa765960a99cf10c6ddb9f1fad506099ff0a0e137ad8960a88f321b)
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
	if(host_processor MATCHES "^(ARM64|arm64|aarch64)$")
		set(ninja_asset ninja-linux-aarch64.zip)
		set(ninja_sha256 fd2cacc8050a7f12a16a2e48f9e06fca5c14fc4c2bee2babb67b58be17a607fc)
	else()
		set(ninja_asset ninja-linux.zip)
		set(ninja_sha256 5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6)
	endif()
else()
	message(FATAL_ERROR "Ninja ${ninja_version} has no configured asset for ${CMAKE_HOST_SYSTEM_NAME}")
endif()

file(MAKE_DIRECTORY "${INSTALL_DIR}")
set(ninja_archive "${INSTALL_DIR}/${ninja_asset}")
file(DOWNLOAD
	"https://github.com/ninja-build/ninja/releases/download/v${ninja_version}/${ninja_asset}"
	"${ninja_archive}"
	EXPECTED_HASH "SHA256=${ninja_sha256}"
	TLS_VERIFY ON
)
file(ARCHIVE_EXTRACT INPUT "${ninja_archive}" DESTINATION "${INSTALL_DIR}")
file(REMOVE "${ninja_archive}")

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
	set(ninja_executable "${INSTALL_DIR}/ninja.exe")
else()
	set(ninja_executable "${INSTALL_DIR}/ninja")
	file(CHMOD "${ninja_executable}"
		PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endif()

execute_process(
	COMMAND "${ninja_executable}" --version
	OUTPUT_VARIABLE installed_version
	OUTPUT_STRIP_TRAILING_WHITESPACE
	COMMAND_ERROR_IS_FATAL ANY
)
if(NOT installed_version STREQUAL "${ninja_version}")
	message(FATAL_ERROR "Expected Ninja ${ninja_version}, got ${installed_version}")
endif()
message(STATUS "Installed Ninja ${installed_version} at ${ninja_executable}")
