#
#    Copyright 2026 Ethan O'Connor
#
#    This file is part of OpenOrienteering.
#
#    OpenOrienteering is free software: you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation, either version 3 of the License, or
#    (at your option) any later version.
#
#    OpenOrienteering is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
#
#    You should have received a copy of the GNU General Public License
#    along with OpenOrienteering.  If not, see <http://www.gnu.org/licenses/>.

find_package(Qt6 REQUIRED COMPONENTS Core Network Widgets)
find_package(Qt6 COMPONENTS Bluetooth)
if(NOT ANDROID AND NOT IOS)
	find_package(Qt6 COMPONENTS SerialPort)
endif()

set(CMAKE_AUTOMOC ON)


# GNSS protocol parsers, transport, and session management

set(MAPPER_GNSS_DEFINITIONS
  PRIVATE
    QT_NO_CAST_FROM_ASCII
    QT_NO_CAST_TO_ASCII
    QT_USE_QSTRINGBUILDER
)

# minmea (vendored NMEA parser, WTFPL license)
add_library(minmea STATIC 3rdparty/minmea/minmea.c)
target_include_directories(minmea PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/minmea")
set_target_properties(minmea PROPERTIES C_STANDARD 99)
# minmea is C code; suppress C++ warnings
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
	target_compile_options(minmea PRIVATE -w)
endif()


set(MAPPER_GNSS_SOURCES
  # Protocol layer
  protocol/ubx_parser.cpp
  protocol/nmea_parser.cpp
  protocol/protocol_detector.cpp
  protocol/rtcm_framer.cpp
  # Transport layer
  transport/gnss_transport.cpp
  transport/tcp_transport.cpp
  transport/ble_device_model.cpp
  # Corrections
  correction/ntrip_client.cpp
  gga_generator.cpp
  # Protocol - configuration
  protocol/ubx_config.cpp
  # Session & integration
  gnss_session.cpp
  gnss_position_source.cpp
  gnss_raw_logger.cpp
  # UI
  ui/gnss_status_overlay.cpp
  ui/gnss_detail_panel.cpp
  ui/gnss_device_dialog.cpp
  ui/ntrip_settings_widget.cpp
  ui/gnss_settings_page.cpp
)

# BLE transport — platform-specific implementation
if(IOS)
	# iOS: Native CoreBluetooth (no Qt Bluetooth dependency)
	enable_language(OBJCXX)
	list(APPEND MAPPER_GNSS_SOURCES
	  transport/ble_transport_corebluetooth.mm
	  transport/ble_scanner_corebluetooth.mm
	  transport/ble_scanner_transport_adapter.cpp
	)
	list(APPEND MAPPER_GNSS_DEFINITIONS
	  PUBLIC MAPPER_GNSS_BLE_COREBLUETOOTH
	)
elseif(TARGET Qt6::Bluetooth)
	# Android/desktop: Qt Bluetooth
	list(APPEND MAPPER_GNSS_SOURCES
	  transport/ble_transport.cpp
	  transport/spp_transport.cpp
	)
	list(APPEND MAPPER_GNSS_DEFINITIONS
	  PUBLIC MAPPER_GNSS_BLE
	  PUBLIC MAPPER_GNSS_SPP
	)
endif()

# Serial transport (desktop only, requires Qt SerialPort)
if(TARGET Qt6::SerialPort)
	list(APPEND MAPPER_GNSS_SOURCES
	  transport/serial_transport.cpp
	)
	list(APPEND MAPPER_GNSS_DEFINITIONS
	  PUBLIC MAPPER_GNSS_SERIAL
	)
endif()

add_library(mapper-gnss STATIC ${MAPPER_GNSS_SOURCES})
target_compile_definitions(mapper-gnss ${MAPPER_GNSS_DEFINITIONS})
target_include_directories(mapper-gnss
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
  PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(mapper-gnss
  PUBLIC Qt6::Core Qt6::Widgets
  PRIVATE Qt6::Network minmea
)
if(IOS)
	target_compile_options(mapper-gnss PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
	find_library(COREBLUETOOTH_FRAMEWORK CoreBluetooth)
	if(COREBLUETOOTH_FRAMEWORK)
		target_link_libraries(mapper-gnss PRIVATE "${COREBLUETOOTH_FRAMEWORK}")
	endif()
elseif(TARGET Qt6::Bluetooth)
	target_link_libraries(mapper-gnss PUBLIC Qt6::Bluetooth)
endif()
if(TARGET Qt6::SerialPort)
	target_link_libraries(mapper-gnss PRIVATE Qt6::SerialPort)
endif()
