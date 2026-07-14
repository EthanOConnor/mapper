#
#    Copyright 2012-2022, 2025, 2026 Kai Pastor
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


set(CMAKE_AUTOMOC ON)

add_definitions(
  -DQT_NO_CAST_FROM_ASCII
  -DQT_NO_CAST_TO_ASCII
  -DQT_USE_QSTRINGBUILDER
)

configure_file(test_config.h.in ${CMAKE_CURRENT_BINARY_DIR}/test_config.h @ONLY)


# This function defines a test from a list of source file names
# (without ending). For each filename, a .cpp file and .h must exist.
# The .h file is processed with Qt's moc.
# The first filename is also used as the name of the executable file and
# as the name of the test.
#
# The test executable is linked only to external libraries by default.
# It only needs to be rebuild and run when one of its components was modified.
# Additional link libraries may be added to the executable target as usual.
#
# Tests are registered directly with CTest. IDEs and CI use the same entrypoint.
function(add_test_helper testname)
	unset(TEST_${testname}_SRCS)
	set(manual_test 0)
	foreach(arg ${ARGN})
		if("${arg}" STREQUAL "MANUAL")
			set(manual_test 1)
		else()
			list(APPEND TEST_${testname}_SRCS ${arg}.cpp)
		endif()
	endforeach()
	add_executable(${testname} ${testname}.cpp ${TEST_${testname}_SRCS})
	mapper_target_defaults(${testname})
	target_link_libraries(${testname} PRIVATE
	  ${PROJ_LIBRARIES}
	  Qt6::Test
	  Qt6::Gui
	  Clipper2::Clipper2
	)
	if("${ARGN}" MATCHES "crs_template_implementation")
		target_link_libraries(${testname} PRIVATE Qt6::Widgets)
	endif()
	if(NOT manual_test)
		add_test(NAME ${testname} COMMAND $<TARGET_FILE:${testname}>)
		set_tests_properties(${testname} PROPERTIES
		  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
		)
	endif()
endfunction()


# This function defines a unit test from a list of source file names
# (without ending). For each filename, a .cpp file and .h must exist.
# The .h file is processed with Qt's moc.
# The first filename is also used as the name of the executable file and
# as the name of the test.
#
# When Mapper_DEVELOPMENT_BUILD is enabled, a unit test executable is linked
# only to external libraries by default. It only needs to be rebuild and run
# when its explicit sources are modified.
# 
# When Mapper_DEVELOPMENT_BUILD is disabled, the additional explicit sources
# are ignored, and the unit test executable is linked directly to the
# Mapper_Common library. This is a faster choice for one-time builds like CI,
# and it also avoids problems with collecting test coverage information.
#
function(add_unit_test testname)
	if(Mapper_DEVELOPMENT_BUILD)
		add_test_helper("${testname}" ${ARGN})
	elseif("${ARGN}" MATCHES "MANUAL")
		add_system_test("${testname}" MANUAL)
	else()
		add_system_test("${testname}")
	endif()
endfunction()


# This function defines a system test from a list of source file names
# (without ending). For each filename, a .cpp file and .h must exist.
# The .h file is processed with Qt's moc.
# The first filename is also used as the name of the executable file and
# as the name of the test.
#
# A system test executable is linked to the full Mapper runtime.
# That is why it will be rebuild and run very often.
#
function(add_system_test testname)
	add_test_helper("${testname}" ${ARGN})
	target_link_libraries(${testname} PRIVATE
	  Mapper_Common
	  Qt6::Widgets
	)
endfunction()


# Include generated files (moc output, build configuration)
include_directories("${PROJECT_BINARY_DIR}/src")
include_directories("${PROJECT_SOURCE_DIR}/src")
include_directories("${CMAKE_CURRENT_BINARY_DIR}")


# Unit tests
add_unit_test(tst_qglobal)
add_unit_test(autosave_t MANUAL ../src/core/autosave
	../src/settings
)
add_unit_test(encoding_t ../src/util/encoding)
target_link_libraries(encoding_t PRIVATE ICU::uc)
add_unit_test(georef_ocd_mapping_t
	../src/settings
	../src/gui/util_gui
	../src/gui/widgets/crs_param_widgets
	../src/core/georeferencing
	../src/core/crs_template
	../src/core/crs_template_implementation
	../src/fileformats/ocd_georef_fields
	../src/fileformats/file_format
	../src/util/key_value_container
	../src/util/encoding
	../src/util/xml_stream_util
	../src/core/map_coord
)
add_unit_test(georeferencing_t ../src/core/georeferencing
	../src/settings
	../src/core/crs_template
	../src/core/crs_template_implementation
	../src/core/latlon
	../src/gui/util_gui
	../src/gui/widgets/crs_param_widgets
	../src/mapper_resource
	../src/fileformats/file_format
	../src/util/key_value_container
	../src/util/encoding
	../src/util/xml_stream_util
	../src/core/map_coord
)
target_link_libraries(georef_ocd_mapping_t PRIVATE ICU::uc)
target_link_libraries(georeferencing_t PRIVATE ICU::uc)
if (Mapper_USE_GDAL)
	# cf. src/gdal/CMakeLists.txt
	target_compile_definitions(georeferencing_t PRIVATE MAPPER_TEST_GDAL)
	target_link_libraries(georeferencing_t PRIVATE GDAL::GDAL)
endif()
add_unit_test(grid_t ../src/util/util)
add_unit_test(key_value_container_t ../src/util/key_value_container)
add_unit_test(locale_t ../src/util/translation_util)
add_unit_test(map_color_t ../src/core/map_color)
add_unit_test(ocd_t ../src/fileformats/ocd_types)
add_unit_test(ocd_parameter_stream_reader_t ../src/fileformats/ocd_parameter_stream_reader)
add_unit_test(qpainter_t)
add_unit_test(util_t ../src/util/util
	../src/settings
)

# Benchmarks
add_system_test(coord_xml_t MANUAL)

# System tests
add_system_test(file_format_t)
add_system_test(duplicate_equals_t)
add_system_test(frame_pipeline_t)
if(Mapper_USE_GDAL)
	add_system_test(gdal_tiled_t)
	target_link_libraries(gdal_tiled_t PRIVATE mapper-gdal GDAL::GDAL)
endif()
add_system_test(map_t)
add_system_test(map_printer_t)
add_system_test(object_t)
add_system_test(object_query_t)
add_system_test(path_object_t)
add_system_test(raster_layer_planner_t)
add_system_test(render_ir_t)
add_system_test(vello_renderer_t)
add_system_test(style_t)
add_system_test(symbol_set_t)
add_system_test(symbol_t)
add_system_test(template_t)
add_system_test(tools_t)
add_system_test(track_t)  # Could be unit test, but needs Georeferencing
add_system_test(transform_t)
add_system_test(undo_manager_t)

# Manual parity helper used to compare exact upstream and modernization
# checkpoints without adding another product-facing export path.
add_executable(render_reference EXCLUDE_FROM_ALL render_reference.cpp)
mapper_target_defaults(render_reference)
target_link_libraries(render_reference PRIVATE Mapper_Common Qt6::Widgets)

add_executable(render_benchmark EXCLUDE_FROM_ALL render_benchmark.cpp)
mapper_target_defaults(render_benchmark)
target_link_libraries(render_benchmark PRIVATE Mapper_Common Qt6::Widgets)

add_executable(vello_benchmark EXCLUDE_FROM_ALL vello_benchmark.cpp)
mapper_target_defaults(vello_benchmark)
target_link_libraries(vello_benchmark PRIVATE Mapper_Common Qt6::Widgets)

if(Mapper_USE_GDAL)
	add_executable(raster_benchmark EXCLUDE_FROM_ALL raster_benchmark.cpp)
	mapper_target_defaults(raster_benchmark)
	target_link_libraries(raster_benchmark PRIVATE
		Mapper_Common mapper-gdal GDAL::GDAL Qt6::Widgets
	)
endif()

if(TARGET mapper-sensors)
	add_system_test(sensors_t)
	target_link_libraries(sensors_t  PRIVATE mapper-sensors)
	foreach(lib Qt6::Positioning Qt6::Sensors)
		if(TARGET ${lib})
			target_link_libraries(sensors_t  PRIVATE ${lib})
		endif()
	endforeach()
endif()

set(IOF_3_XSD_FILE "" CACHE FILEPATH "Path of the IOF Data Standard 3 XSD file")
if(IOF_3_XSD_FILE)
	find_program(XMLLINT NAMES xmllint)
	add_test(NAME validate_iof_course
		COMMAND "${XMLLINT}" --schema "${IOF_3_XSD_FILE}" "${CMAKE_CURRENT_SOURCE_DIR}/data/export/iof-3.0-course.xml"
	)
endif()
