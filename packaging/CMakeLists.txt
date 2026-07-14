#
#    Copyright 2012-2026 The OpenOrienteering developers
#
#    This file is part of OpenOrienteering.
#
#    OpenOrienteering is free software: you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation, either version 3 of the License, or
#    (at your option) any later version.
#

message(STATUS "Configuring ${PROJECT_NAME} deployment")

# Android packages are produced directly by Qt's generated apk and aab targets.
# The package source and application metadata are configured on Mapper before
# Qt finalizes the target in src/CMakeLists.txt.
if(ANDROID)
	return()
endif()

if(APPLE)
	set_target_properties(Mapper PROPERTIES
		MACOSX_BUNDLE_BUNDLE_NAME "Mapper"
		MACOSX_BUNDLE_BUNDLE_VERSION "${Mapper_VERSION}"
		MACOSX_BUNDLE_COPYRIGHT "${Mapper_COPYRIGHT}"
		MACOSX_BUNDLE_GUI_IDENTIFIER "org.openorienteering.mapper"
		MACOSX_BUNDLE_ICON_FILE "Mapper.icns"
		MACOSX_BUNDLE_INFO_STRING "OpenOrienteering Mapper ${Mapper_VERSION_DISPLAY}"
		MACOSX_BUNDLE_SHORT_VERSION_STRING "${Mapper_VERSION}"
	)
	install(
		FILES "${PROJECT_SOURCE_DIR}/images/mapper-icon/Mapper.icns"
		DESTINATION "${MAPPER_DATA_DESTINATION}"
	)
endif()

install(
	DIRECTORY "${Mapper_PROJ_DATA_DIR}/"
	DESTINATION "${MAPPER_DATA_DESTINATION}/proj"
)
if(Mapper_USE_GDAL)
	install(
		DIRECTORY "${Mapper_GDAL_DATA_DIR}/"
		DESTINATION "${MAPPER_DATA_DESTINATION}/gdal"
	)
endif()

if(UNIX AND NOT APPLE)
	install(
		FILES "${PROJECT_SOURCE_DIR}/doc/man/Mapper.1"
		DESTINATION "${CMAKE_INSTALL_MANDIR}/man1"
	)
	install(
		FILES "${CMAKE_CURRENT_SOURCE_DIR}/linux/Mapper.desktop"
		DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
	)
	install(
		FILES "${CMAKE_CURRENT_SOURCE_DIR}/linux/openorienteering-mapper.xml"
		DESTINATION "${CMAKE_INSTALL_DATADIR}/mime/packages"
	)
	foreach(size 16 24 32 48 96 128 256 512)
		install(
			FILES "${PROJECT_SOURCE_DIR}/images/mapper-icon/Mapper-${size}.png"
			DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps"
			RENAME Mapper.png
		)
		foreach(mime_icon
			application-x-openorienteering-xmap
			application-x-openorienteering-ocd)
			install(
				FILES "${PROJECT_SOURCE_DIR}/images/mapper-icon/Mapper-${size}.png"
				DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/mimetypes"
				RENAME "${mime_icon}.png"
			)
		endforeach()
	endforeach()
endif()

qt_generate_deploy_app_script(
	TARGET Mapper
	OUTPUT_SCRIPT mapper_deploy_script
)
install(SCRIPT "${mapper_deploy_script}")

set(CPACK_PACKAGE_NAME "OpenOrienteering Mapper")
set(CPACK_PACKAGE_VENDOR "OpenOrienteering")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Map drawing software for orienteering")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://www.openorienteering.org/apps/mapper/")
set(CPACK_PACKAGE_VERSION "${Mapper_VERSION}")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/COPYING")
set(CPACK_SOURCE_GENERATOR "")
set(CPACK_STRIP_FILES ON)

if(APPLE)
	set(CPACK_GENERATOR DragNDrop)
	set(mapper_package_platform macOS)
elseif(WIN32)
	set(CPACK_GENERATOR ZIP)
	set(mapper_package_platform Windows)
else()
	set(CPACK_GENERATOR TGZ)
	set(mapper_package_platform Linux)
endif()

set(CPACK_PACKAGE_FILE_NAME
	"OpenOrienteering-Mapper-${Mapper_VERSION_FILESYSTEM}-${mapper_package_platform}-${CMAKE_SYSTEM_PROCESSOR}")

include(CPack)
