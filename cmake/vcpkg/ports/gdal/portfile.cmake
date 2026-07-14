# Temporary latest-release overlay. Delete this port as soon as the pinned
# vcpkg baseline contains GDAL 3.13.1 or newer.
set(gdal_builtin_port_dir "${VCPKG_ROOT_DIR}/ports/gdal")

vcpkg_download_distfile(gdal_archive
	URLS "https://github.com/OSGeo/gdal/releases/download/v${VERSION}/gdal-${VERSION}.tar.gz"
	FILENAME "gdal-${VERSION}.tar.gz"
	SHA512 36efa05298a4f37edd157f01d7a71121b530aa32a4c5f1e76ae24d91b95712c2b98d7d17a442eb7f8e8c82a7c4282742670385b724d1ce55f4e79a58f655cd10
)
vcpkg_extract_source_archive(
	SOURCE_PATH
	ARCHIVE "${gdal_archive}"
	PATCHES
		"${gdal_builtin_port_dir}/find-link-libraries.patch"
		"${gdal_builtin_port_dir}/iconv.diff"
		"${gdal_builtin_port_dir}/libarchive.diff"
		"${gdal_builtin_port_dir}/libkml.patch"
		"${CMAKE_CURRENT_LIST_DIR}/sqlite3-3.13.1.diff"
		"${gdal_builtin_port_dir}/target-is-valid.patch"
)

file(REMOVE "${SOURCE_PATH}/cmake/modules/packages/FindIconv.cmake")
file(REMOVE "${SOURCE_PATH}/cmake/modules/packages/FindZSTD.cmake")
file(REMOVE_RECURSE "${SOURCE_PATH}/autotest")

vcpkg_replace_string(
	"${SOURCE_PATH}/ogr/ogrsf_frmts/flatgeobuf/flatbuffers/base.h"
	[[__has_include("absl/strings/string_view.h")]]
	"(0)")

vcpkg_check_features(OUT_FEATURE_OPTIONS feature_options
	FEATURES
		curl GDAL_USE_CURL
		expat GDAL_USE_EXPAT
		geos GDAL_USE_GEOS
		core GDAL_USE_GEOTIFF
		jpeg GDAL_USE_JPEG
		core GDAL_USE_JSONC
		lerc GDAL_USE_LERC
		lzma GDAL_USE_LIBLZMA
		openjpeg GDAL_USE_OPENJPEG
		png GDAL_USE_PNG
		qhull GDAL_USE_QHULL
		core GDAL_USE_SHAPELIB_INTERNAL
		sqlite3 GDAL_USE_SQLITE3
		core GDAL_USE_TIFF
		webp GDAL_USE_WEBP
		core GDAL_USE_ZLIB
		zstd GDAL_USE_ZSTD
)

string(REPLACE "dynamic" "" qhull_target "Qhull::qhull${VCPKG_LIBRARY_LINKAGE}_r")

vcpkg_cmake_configure(
	SOURCE_PATH "${SOURCE_PATH}"
	OPTIONS
		-DVCPKG_HOST_TRIPLET=${HOST_TRIPLET}
		${feature_options}
		-DBUILD_APPS=OFF
		-DBUILD_PYTHON_BINDINGS=OFF
		-DBUILD_TESTING=OFF
		-DCMAKE_DISABLE_FIND_PACKAGE_CSharp=ON
		-DCMAKE_DISABLE_FIND_PACKAGE_Java=ON
		-DCMAKE_DISABLE_FIND_PACKAGE_JNI=ON
		-DCMAKE_DISABLE_FIND_PACKAGE_SWIG=ON
		-DGDAL_USE_INTERNAL_LIBS=OFF
		-DGDAL_USE_EXTERNAL_LIBS=OFF
		-DGDAL_BUILD_OPTIONAL_DRIVERS=ON
		-DOGR_BUILD_OPTIONAL_DRIVERS=ON
		-DFIND_PACKAGE2_KEA_ENABLED=OFF
		-DGDAL_CHECK_PACKAGE_QHULL_NAMES=Qhull
		"-DGDAL_CHECK_PACKAGE_QHULL_TARGETS=${qhull_target}"
		"-DQHULL_LIBRARY=${qhull_target}"
		"-DCMAKE_PROJECT_INCLUDE=${gdal_builtin_port_dir}/cmake-project-include.cmake"
	MAYBE_UNUSED_VARIABLES
		QHULL_LIBRARY
)
vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/gdal)

vcpkg_replace_string(
	"${CURRENT_PACKAGES_DIR}/share/gdal/GDALConfig.cmake"
	"include(CMakeFindDependencyMacro)"
	"include(CMakeFindDependencyMacro)
# GDAL needs the host pkgconf executable while resolving dependencies.
get_filename_component(vcpkg_host_prefix \"\${CMAKE_CURRENT_LIST_DIR}/../../../${HOST_TRIPLET}\" ABSOLUTE)
list(APPEND CMAKE_PROGRAM_PATH \"\${vcpkg_host_prefix}/tools/pkgconf\")")

file(REMOVE_RECURSE
	"${CURRENT_PACKAGES_DIR}/debug/include"
	"${CURRENT_PACKAGES_DIR}/debug/share")
file(REMOVE
	"${CURRENT_PACKAGES_DIR}/bin/gdal-config"
	"${CURRENT_PACKAGES_DIR}/debug/bin/gdal-config")
file(GLOB gdal_bin_files "${CURRENT_PACKAGES_DIR}/bin/*")
if(NOT gdal_bin_files)
	file(REMOVE_RECURSE
		"${CURRENT_PACKAGES_DIR}/bin"
		"${CURRENT_PACKAGES_DIR}/debug/bin")
endif()

vcpkg_replace_string(
	"${CURRENT_PACKAGES_DIR}/include/cpl_config.h"
	"#define GDAL_PREFIX \"${CURRENT_PACKAGES_DIR}\""
	"")

file(INSTALL "${gdal_builtin_port_dir}/vcpkg-cmake-wrapper.cmake"
	DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
file(INSTALL "${gdal_builtin_port_dir}/usage"
	DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.TXT")
