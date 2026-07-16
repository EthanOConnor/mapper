#
#    Copyright 2012, 2013, 2014 Thomas Schöps
#    Copyright 2012-2026 Kai Pastor
#
#    This file is part of OpenOrienteering.
#
#    OpenOrienteering is free software: you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation, either version 3 of the License, or
#    (at your option) any later version.
#

cmake_minimum_required(VERSION 4.4)

project(Mapper VERSION 0.9.7 LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CTest)
include(FeatureSummary)
include(GNUInstallDirs)
include(MapperBuild)

set(Mapper_COPYRIGHT "(C) 2026 The OpenOrienteering developers")
math(EXPR Mapper_VERSION_CODE
	"${Mapper_VERSION_MAJOR} * 1000000 + ${Mapper_VERSION_MINOR} * 10000 + ${Mapper_VERSION_PATCH} * 100 + 1")

if(Mapper_VERSION_DISPLAY)
	message(STATUS "Custom version display string: \"${Mapper_VERSION_DISPLAY}\"")
elseif(CMAKE_BUILD_TYPE AND NOT CMAKE_BUILD_TYPE MATCHES "Release|MinSizeRel|RelWithDebInfo")
	set(Mapper_VERSION_DISPLAY "${CMAKE_BUILD_TYPE} ${Mapper_VERSION}")
else()
	set(Mapper_VERSION_DISPLAY "${Mapper_VERSION}")
endif()
string(REGEX REPLACE "[\\/:*?| ]" "_" Mapper_VERSION_FILESYSTEM "${Mapper_VERSION_DISPLAY}")

if(PROJECT_IS_TOP_LEVEL AND CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
	message(FATAL_ERROR "In-source builds are not supported. Use a CMake preset.")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type")
	set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
		Debug Release RelWithDebInfo MinSizeRel)
endif()

option(Mapper_DEBUG_TRANSLATIONS "Debug missing translations" OFF)
option(Mapper_USE_GDAL "Enable GDAL raster and vector format support" ON)
option(Mapper_WITH_COVE "Build contour line vectorization" ON)
option(Mapper_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
	set(mapper_development_default ON)
else()
	set(mapper_development_default OFF)
endif()
option(Mapper_DEVELOPMENT_BUILD
	"Load development resources from the build tree"
	${mapper_development_default})

if(Mapper_DEVELOPMENT_BUILD)
	set(mapper_build_manual_default OFF)
else()
	set(mapper_build_manual_default ON)
endif()
option(Mapper_BUILD_MANUAL
	"Build and install the user manual"
	${mapper_build_manual_default})
option(Mapper_BUILD_API_DOCS
	"Add the developer API documentation target"
	OFF)
option(Mapper_PACKAGE_BUILD
	"Configure a redistributable package with complete dependency metadata"
	OFF)

set(Mapper_PACKAGE_NAME "openorienteering-mapper")

if(WIN32)
	# Qt snapshots this standard deployment directory when Core is found.
	# Keep runtime DLLs beside Mapper.exe so the ZIP launches independently
	# of build-machine PATH entries.
	set(CMAKE_INSTALL_BINDIR ".")
	set(MAPPER_RUNTIME_DESTINATION .)
	set(MAPPER_LIBRARY_DESTINATION .)
	set(MAPPER_DATA_DESTINATION .)
	set(MAPPER_ABOUT_DESTINATION doc)
elseif(APPLE)
	set(MAPPER_RUNTIME_DESTINATION .)
	set(MAPPER_LIBRARY_DESTINATION .)
	set(MAPPER_DATA_DESTINATION "Mapper.app/Contents/Resources")
	set(MAPPER_ABOUT_DESTINATION "Mapper.app/Contents/Resources/doc")
elseif(ANDROID)
	set(MAPPER_RUNTIME_DESTINATION "libs/${CMAKE_ANDROID_ARCH_ABI}")
	set(MAPPER_LIBRARY_DESTINATION "libs/${CMAKE_ANDROID_ARCH_ABI}")
	set(MAPPER_DATA_DESTINATION assets)
	set(MAPPER_ABOUT_DESTINATION assets/doc)
else()
	set(MAPPER_RUNTIME_DESTINATION "${CMAKE_INSTALL_BINDIR}")
	set(MAPPER_LIBRARY_DESTINATION "${CMAKE_INSTALL_LIBDIR}/${Mapper_PACKAGE_NAME}")
	set(MAPPER_DATA_DESTINATION "${CMAKE_INSTALL_DATADIR}/${Mapper_PACKAGE_NAME}")
	string(REPLACE "/${PROJECT_NAME}" "/${Mapper_PACKAGE_NAME}"
		MAPPER_ABOUT_DESTINATION "${CMAKE_INSTALL_DOCDIR}")
endif()

if(CMAKE_CROSSCOMPILING)
	message(STATUS
		"Cross-compiling from ${CMAKE_HOST_SYSTEM_NAME} to ${CMAKE_SYSTEM_NAME}")
endif()

mapper_define_build_options()

include(FetchContent)
FetchContent_Declare(corrosion
	URL https://github.com/corrosion-rs/corrosion/archive/refs/tags/v0.6.1.tar.gz
	URL_HASH SHA256=e9e95b1ee2bad52681f347993fb1a5af5cce458c5ce8a2636c9e476e4babf8e3
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
	EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(corrosion)
add_feature_info(Corrosion ON "0.6.1")

set(CLIPPER2_UTILS OFF CACHE BOOL "" FORCE)
set(CLIPPER2_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLIPPER2_TESTS OFF CACHE BOOL "" FORCE)
set(CLIPPER2_USINGZ OFF CACHE STRING "" FORCE)
FetchContent_Declare(clipper2
	URL https://github.com/AngusJohnson/Clipper2/releases/download/Clipper2_2.0.1/Clipper2_2.0.1.zip
	URL_HASH SHA256=63e893fc40c3453c9d14cbe98bc7647f16a9d5846ae25b513d8a1ed5b8421144
	SOURCE_SUBDIR CPP
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
	EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(clipper2)
if(NOT TARGET Clipper2::Clipper2)
	add_library(Clipper2::Clipper2 ALIAS Clipper2)
endif()
add_feature_info(Clipper2 ON "2.0.1")

find_package(PROJ 9.8.1 CONFIG REQUIRED)
find_package(ICU 78.3 REQUIRED COMPONENTS uc data)
if(Mapper_USE_GDAL)
	# Static GDAL exports its XZ dependency as an imported target. Resolve it
	# and its other image/database primitives explicitly so cross-builds do not
	# depend on incidental package order.
	find_package(LibLZMA REQUIRED)
	find_package(JPEG REQUIRED)
	find_package(TIFF REQUIRED)
	find_package(SQLite3 REQUIRED)
	find_package(GDAL 3.13.1 CONFIG REQUIRED)
endif()

find_path(Mapper_PROJ_DATA_DIR
	NAMES proj.db
	HINTS "${PROJ_DIR}" "${PROJ_DIR}/../../.."
	PATH_SUFFIXES . share/proj
	REQUIRED
)
file(REAL_PATH "${Mapper_PROJ_DATA_DIR}" Mapper_PROJ_DATA_DIR)
if(Mapper_USE_GDAL)
	find_path(Mapper_GDAL_DATA_DIR
		NAMES gdalvrt.xsd
		HINTS "${GDAL_DIR}" "${GDAL_DIR}/../../.."
		PATH_SUFFIXES . share/gdal
		REQUIRED
	)
	file(REAL_PATH "${Mapper_GDAL_DATA_DIR}" Mapper_GDAL_DATA_DIR)
endif()

set(mapper_qt_components
	Core
	Gui
	LinguistTools
	Network
	Positioning
	Sensors
	SerialPort
	Svg
	Widgets
)
if(Mapper_WITH_COVE)
	list(APPEND mapper_qt_components Concurrent)
endif()
if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
	list(APPEND mapper_qt_components Test)
endif()
if(NOT ANDROID)
	list(APPEND mapper_qt_components PrintSupport)
endif()
find_package(Qt6 6.11.1 REQUIRED COMPONENTS ${mapper_qt_components})
qt_standard_project_setup(REQUIRES 6.11.1)

if(NOT ANDROID)
	set(KDSingleApplication_QT6 ON CACHE BOOL "" FORCE)
	set(KDSingleApplication_STATIC ON CACHE BOOL "" FORCE)
	set(KDSingleApplication_TESTS OFF CACHE BOOL "" FORCE)
	set(KDSingleApplication_EXAMPLES OFF CACHE BOOL "" FORCE)
	set(KDSingleApplication_DOCS OFF CACHE BOOL "" FORCE)
	FetchContent_Declare(kdsingleapplication
		URL https://github.com/KDAB/KDSingleApplication/archive/refs/tags/v1.2.1.tar.gz
		URL_HASH SHA256=e3254ce9dc5ecf6d61ef83264bc61d486a307f0e3c9ed1bb2176f068cdbcbe09
		DOWNLOAD_EXTRACT_TIMESTAMP TRUE
		EXCLUDE_FROM_ALL
	)
	FetchContent_MakeAvailable(kdsingleapplication)
	set_property(TARGET kdsingleapplication PROPERTY CXX_STANDARD 23)
	add_feature_info(KDSingleApplication ON "1.2.1")
endif()

if(Mapper_BUILD_MANUAL)
	add_subdirectory(doc/manual)
endif()
add_subdirectory(examples)
add_subdirectory("symbol sets")
add_subdirectory(translations)
if(Mapper_WITH_COVE)
	add_feature_info(Mapper_WITH_COVE ON "Contour line vectorization")
	add_subdirectory(3rd-party/cove)
endif()
if(Mapper_USE_GDAL)
	add_subdirectory(src/gdal)
endif()
add_subdirectory(src/sensors)
add_subdirectory(src)
add_subdirectory(doc/licensing)
# Keep deployment last: its install script signs the complete application bundle.
add_subdirectory(packaging)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
	add_subdirectory(test)
endif()

if(Mapper_BUILD_API_DOCS)
	add_subdirectory(doc/api)
endif()

feature_summary(INCLUDE_QUIET_PACKAGES WHAT ALL)
