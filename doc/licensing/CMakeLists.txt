#
#    Copyright 2014-2026 The OpenOrienteering developers
#
#    This file is part of OpenOrienteering.
#
#    OpenOrienteering is free software: you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation, either version 3 of the License, or
#    (at your option) any later version.
#

message(STATUS "Configuring ${PROJECT_NAME} dependency notices")

set(third_party_copyright)
set(common_licenses)

function(mapper_add_dependency_notice component label source filename)
	if(NOT EXISTS "${source}")
		message(FATAL_ERROR "Missing dependency notice: ${source}")
	endif()

	set(relative_path "3rd-party/${component}/${filename}")
	list(APPEND third_party_copyright
		"  <li><a href=\"${relative_path}\">${label}</a></li>")

	file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/3rd-party/${component}")
	configure_file(
		"${source}"
		"${CMAKE_CURRENT_BINARY_DIR}/${relative_path}"
		COPYONLY
	)
	install(
		FILES "${source}"
		DESTINATION "${MAPPER_ABOUT_DESTINATION}/3rd-party/${component}"
		RENAME "${filename}"
	)

	set(third_party_copyright "${third_party_copyright}" PARENT_SCOPE)
endfunction()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/common-licenses")
configure_file(
	"${PROJECT_SOURCE_DIR}/COPYING"
	"${CMAKE_CURRENT_BINARY_DIR}/common-licenses/GPL-3.txt"
	COPYONLY
)
install(
	FILES "${PROJECT_SOURCE_DIR}/COPYING"
	DESTINATION "${MAPPER_ABOUT_DESTINATION}/common-licenses"
	RENAME GPL-3.txt
)
list(APPEND common_licenses
	"  <li><a href=\"common-licenses/GPL-3.txt\">GNU General Public License version 3</a></li>")

list(APPEND third_party_copyright
	"  <li>Qt ${Qt6_VERSION} by The Qt Company and contributors: <a href=\"common-licenses/GPL-3.txt\">GNU General Public License version 3</a></li>")

mapper_add_dependency_notice(
	clipper2
	"Clipper2 2.0.1"
	"${CMAKE_CURRENT_SOURCE_DIR}/clipper2-2.0.1.txt"
	LICENSE.txt
)

if(TARGET kdsingleapplication)
	mapper_add_dependency_notice(
		kdsingleapplication
		"KDSingleApplication 1.2.1"
		"${kdsingleapplication_SOURCE_DIR}/LICENSE.txt"
		LICENSE.txt
	)
endif()

if(TARGET cove)
	mapper_add_dependency_notice(
		potrace
		"Potrace 1.16"
		"${PROJECT_SOURCE_DIR}/3rd-party/cove/potrace/copyright"
		copyright
	)
endif()

# Official Qt binary archives contain one SPDX document per installed module.
# Install those upstream-generated inventories rather than maintaining a local
# list that can drift from the actual Qt deployment closure.
file(GLOB qt_sboms CONFIGURE_DEPENDS "${QT_INSTALL_PREFIX}/sbom/*.spdx.json")
foreach(qt_sbom IN LISTS qt_sboms)
	get_filename_component(qt_sbom_name "${qt_sbom}" NAME)
	mapper_add_dependency_notice(
		qt
		"Qt module SBOM: ${qt_sbom_name}"
		"${qt_sbom}"
		"${qt_sbom_name}"
	)
endforeach()

# vcpkg installs the exact copyright file selected by every target dependency,
# including transitives, under the configured triplet's share directory.
set(vcpkg_notices)
set(vcpkg_sboms)
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
	file(GLOB vcpkg_notices CONFIGURE_DEPENDS
		"${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/*/copyright")
	foreach(vcpkg_notice IN LISTS vcpkg_notices)
		get_filename_component(component_dir "${vcpkg_notice}" DIRECTORY)
		get_filename_component(component "${component_dir}" NAME)
		mapper_add_dependency_notice(
			"vcpkg/${component}"
			"${component}"
			"${vcpkg_notice}"
			copyright
		)
	endforeach()

	file(GLOB vcpkg_sboms CONFIGURE_DEPENDS
		"${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/*/vcpkg.spdx.json")
	foreach(vcpkg_sbom IN LISTS vcpkg_sboms)
		get_filename_component(component_dir "${vcpkg_sbom}" DIRECTORY)
		get_filename_component(component "${component_dir}" NAME)
		mapper_add_dependency_notice(
			"vcpkg/${component}"
			"${component} SBOM"
			"${vcpkg_sbom}"
			vcpkg.spdx.json
		)
	endforeach()
endif()

list(LENGTH qt_sboms qt_sbom_count)
list(LENGTH vcpkg_notices vcpkg_notice_count)
list(LENGTH vcpkg_sboms vcpkg_sbom_count)
if(Mapper_PACKAGE_BUILD AND qt_sbom_count EQUAL 0)
	message(FATAL_ERROR
		"Package builds require the SPDX inventories from an official Qt binary installation")
endif()
if(Mapper_PACKAGE_BUILD AND vcpkg_notice_count EQUAL 0)
	message(FATAL_ERROR
		"Package builds require the managed vcpkg dependency notices")
endif()
if(Mapper_PACKAGE_BUILD AND vcpkg_sbom_count EQUAL 0)
	message(FATAL_ERROR
		"Package builds require the managed vcpkg SPDX inventories")
endif()

message(STATUS
	"Dependency inventory: ${qt_sbom_count} Qt SBOMs, ${vcpkg_notice_count} vcpkg notices, ${vcpkg_sbom_count} vcpkg SBOMs")

list(SORT third_party_copyright)
string(REPLACE ";" "\n" third_party_copyright "${third_party_copyright}")
string(REPLACE ";" "\n" common_licenses "${common_licenses}")
configure_file(licensing.html "${CMAKE_CURRENT_BINARY_DIR}/licensing.html" @ONLY)

install(
	FILES "${CMAKE_CURRENT_BINARY_DIR}/licensing.html"
	DESTINATION "${MAPPER_ABOUT_DESTINATION}"
)
