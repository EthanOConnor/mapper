#
#    Copyright 2012-2014 Thomas Schöps
#    Copyright 2012-2024, 2026 Kai Pastor
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

corrosion_import_crate(
  MANIFEST_PATH "${CMAKE_CURRENT_SOURCE_DIR}/render/vello/Cargo.toml"
  CRATES mapper-vello
  LOCKED
)
if(IOS)
  # Corrosion cannot infer the Apple mobile target from CMake, and Cargo needs
  # the same deployment floor as CMake/vcpkg to emit compatible load commands.
  corrosion_set_env_vars(mapper_vello
    "IPHONEOS_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
  )
endif()
corrosion_add_cxxbridge(mapper-vello-cxx CRATE mapper_vello FILES lib.rs)
target_compile_features(mapper-vello-cxx PRIVATE cxx_std_23)
set_target_properties(mapper-vello-cxx PROPERTIES AUTOMOC OFF AUTOUIC OFF)
add_feature_info(Vello ON "0.9.0 with wgpu 29.0.4")


# Build configuration

get_filename_component(ABSOLUTE_ABOUT_DESTINATION "${MAPPER_ABOUT_DESTINATION}" ABSOLUTE
  BASE_DIR "${CMAKE_INSTALL_PREFIX}"
)
get_filename_component(ABSOLUTE_DATA_DESTINATION "${MAPPER_DATA_DESTINATION}" ABSOLUTE
  BASE_DIR "${CMAKE_INSTALL_PREFIX}"
)
configure_file(mapper_config.h.in "${CMAKE_CURRENT_BINARY_DIR}/mapper_config.h.tmp")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different mapper_config.h.tmp mapper_config.h
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_library(mapper-config INTERFACE)
target_include_directories(mapper-config  INTERFACE "${CMAKE_CURRENT_BINARY_DIR}")

# Lowest-level C++ rendering contract. Keeping the IR in its own target makes
# the intended dependency direction ordinary CMake structure: document and
# presentation code may produce or consume IR; the IR never depends on Qt or a
# graphics backend.
add_library(mapper-render-ir STATIC
  render/render_ir.cpp
)
add_library(Mapper::RenderIR ALIAS mapper-render-ir)
mapper_target_defaults(mapper-render-ir)
target_include_directories(mapper-render-ir PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(mapper-render-ir PROPERTIES AUTOMOC OFF AUTOUIC OFF)

# The production screen backend is a replaceable consumer of immutable frame
# packets. Keep it out of the application library so a future backend can
# reuse the same planner/IR contract without introducing a runtime plugin
# system or teaching document code about GPU implementation details.
add_library(mapper-vello-backend STATIC
  render/vello_renderer.cpp
)
add_library(Mapper::VelloBackend ALIAS mapper-vello-backend)
mapper_target_defaults(mapper-vello-backend)
target_include_directories(mapper-vello-backend PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(mapper-vello-backend PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_link_libraries(mapper-vello-backend
  PUBLIC
  Mapper::RenderIR
  PRIVATE
  mapper-vello-cxx
)

# Qt owns window lifecycle and input; this target only adapts an immutable
# frame packet to the production native-surface backend.
add_library(mapper-vello-presentation STATIC
  presentation/native_surface.cpp
  presentation/vello_canvas.cpp
)
add_library(Mapper::VelloPresentation ALIAS mapper-vello-presentation)
mapper_target_defaults(mapper-vello-presentation)
target_include_directories(mapper-vello-presentation PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(mapper-vello-presentation PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_link_libraries(mapper-vello-presentation
  PUBLIC
  Mapper::VelloBackend
  Qt6::Widgets
)
if(ANDROID)
  target_link_libraries(mapper-vello-presentation PRIVATE android)
endif()


# Resources (from project root)

list(APPEND Mapper_RESOURCES "${PROJECT_SOURCE_DIR}/resources.qrc")
if(ANDROID)
	list(APPEND Mapper_RESOURCES "${PROJECT_BINARY_DIR}/examples/examples.qrc")
endif()
qt_add_resources(Mapper_RESOURCES_RCC ${Mapper_RESOURCES} OPTIONS -no-compress)


# Mapper common: static library of full runtime 
# (To be used by Mapper executable and by system tests.)

set(Mapper_Common_SRCS
  global.cpp
  mapper_resource.cpp
  settings.cpp

	collaboration/managed_map_workspace.cpp
	collaboration/map_hub_api_client.cpp
	collaboration/map_hub_credentials.cpp
	collaboration/map_hub_imagery_catalog.cpp
  
  core/autosave.cpp
  core/crs_template.cpp
  core/crs_template_implementation.cpp
  core/document_path.cpp
  core/georeferencing.cpp
  core/latlon.cpp
  core/map.cpp
  core/map_color.cpp
  core/map_coord.cpp
  core/map_grid.cpp
  core/map_information.cpp
  core/map_part.cpp
  core/map_printer.cpp
  core/map_view.cpp
  core/path_coord.cpp
  core/storage_location.cpp
  core/track.cpp
  core/virtual_coord_vector.cpp
  core/virtual_path.cpp
  
  core/objects/boolean_tool.cpp
  core/objects/object.cpp
  core/objects/object_mover.cpp
  core/objects/object_query.cpp
  core/objects/symbol_rule_set.cpp
  core/objects/text_object.cpp
  
  core/renderables/renderable.cpp
  core/renderables/renderable_implementation.cpp

  # Immutable backend-neutral render description and QPainter reference path.
  render/frame_pipeline.cpp
  render/overlay_scene.cpp
  render/render_snapshot.cpp
  render/qt_render_bridge.cpp
  render/qpainter_frame_renderer.cpp
  render/qpainter_renderer.cpp
  render/template_layer_planner.cpp
  core/symbols/area_symbol.cpp
  core/symbols/combined_symbol.cpp
  core/symbols/html_symbol_report.cpp
  core/symbols/line_symbol.cpp
  core/symbols/point_symbol.cpp
  core/symbols/symbol.cpp
  core/symbols/symbol_icon_decorator.cpp
  core/symbols/text_symbol.cpp
  
  fileformats/course_file_format.cpp
  fileformats/file_format.cpp
  fileformats/file_format_registry.cpp
  fileformats/file_import_export.cpp
  fileformats/iof_course_export.cpp
  fileformats/kml_course_export.cpp
  fileformats/ocd_file_export.cpp
  fileformats/ocd_file_format.cpp
  fileformats/ocd_file_import.cpp
  fileformats/ocd_georef_fields.cpp
  fileformats/ocd_icon.cpp
  fileformats/ocd_parameter_stream_reader.cpp
  fileformats/ocd_types.cpp
  fileformats/simple_course_export.cpp
  fileformats/xml_file_format.cpp
  
  gui/about_dialog.cpp
  gui/action_icon.cpp
  gui/autosave_dialog.cpp
  gui/color_dialog.cpp
  gui/configure_grid_dialog.cpp
  gui/file_dialog.cpp
  gui/georeferencing_dialog.cpp
  gui/home_screen_controller.cpp
  gui/main_window.cpp
  gui/main_window_controller.cpp
	gui/map_hub_dialog.cpp
  gui/modifier_key.cpp
  gui/print_progress_dialog.cpp
  gui/print_tool.cpp
  gui/print_widget.cpp
  gui/select_crs_dialog.cpp
  gui/settings_dialog.cpp
  gnss/ui/gnss_settings_page.cpp
  gnss/ui/ntrip_settings_widget.cpp
  gui/simple_course_dialog.cpp
  gui/task_dialog.cpp
  gui/text_browser_dialog.cpp
  gui/touch_cursor.cpp
  gui/util_gui.cpp

  gui/imagery/catalog_import_dialog.cpp
  gui/imagery/catalog_manager_dialog.cpp
  gui/imagery/imagery_network_permissions_dialog.cpp
  gui/imagery/imagery_source_model.cpp
  gui/imagery/online_imagery_dialog.cpp
  
  gui/map/new_map_dialog.cpp
  gui/map/map_dialog_scale.cpp
  gui/map/map_editor.cpp
  gui/map/map_editor_activity.cpp
  gui/map/map_find_feature.cpp
  gui/map/map_information_dialog.cpp
  gui/map/map_notes.cpp
  gui/map/map_widget.cpp
  gui/map/rotate_map_dialog.cpp
  gui/map/stretch_map_dialog.cpp
  
  gui/symbols/area_symbol_settings.cpp
  gui/symbols/combined_symbol_settings.cpp
  gui/symbols/icon_properties_widget.cpp
  gui/symbols/line_symbol_settings.cpp
  gui/symbols/point_symbol_editor_widget.cpp
  gui/symbols/point_symbol_settings.cpp
  gui/symbols/symbol_properties_widget.cpp
  gui/symbols/symbol_replacement.cpp
  gui/symbols/symbol_replacement_dialog.cpp
  gui/symbols/symbol_report_feature.cpp
  gui/symbols/symbol_setting_dialog.cpp
  gui/symbols/text_symbol_settings.cpp
  
  gui/widgets/action_grid_bar.cpp
  gui/widgets/color_dropdown.cpp
  gui/widgets/color_list_widget.cpp
  gui/widgets/color_wheel_widget.cpp
  gui/widgets/compass_display.cpp
  gui/widgets/crs_param_widgets.cpp
  gui/widgets/crs_selector.cpp
  gui/widgets/editor_settings_page.cpp
  gui/widgets/general_settings_page.cpp
	gui/widgets/map_hub_settings_page.cpp
  gui/widgets/home_screen_widget.cpp
  gui/widgets/key_button_bar.cpp
  gui/widgets/mapper_proxystyle.cpp
  gui/widgets/measure_widget.cpp
  gui/widgets/paint_on_template_settings_page.cpp
  gui/widgets/pie_menu.cpp
  gui/widgets/segmented_button_layout.cpp
  gui/widgets/settings_page.cpp
  gui/widgets/symbol_dropdown.cpp
  gui/widgets/symbol_render_widget.cpp
  gui/widgets/symbol_tooltip.cpp
  gui/widgets/symbol_widget.cpp
  gui/widgets/tag_select_widget.cpp
  gui/widgets/tags_widget.cpp
  gui/widgets/template_list_widget.cpp
  gui/widgets/text_browser.cpp
  gui/widgets/toast.cpp
  
  templates/paint_on_template_feature.cpp
  templates/paint_on_template_tool.cpp
  templates/online_raster_template.cpp
  templates/raster_resource_manager.cpp
  templates/template.cpp
  templates/template_adjust.cpp
  templates/template_dialog_reopen.cpp
  templates/template_image.cpp
  templates/template_image_open_dialog.cpp
  templates/template_map.cpp
  templates/template_placeholder.cpp
  templates/template_position_dock_widget.cpp
  templates/template_positioning_dialog.cpp
  templates/template_table_model.cpp
  templates/template_tool_move.cpp
  templates/template_track.cpp
  templates/world_file.cpp
  
  tools/box_zoom_tool.cpp
  tools/cut_tool.cpp
  tools/cut_hole_tool.cpp
  tools/cutout_operation.cpp
  tools/cutout_tool.cpp
  tools/distribute_points_tool.cpp
  tools/draw_line_and_area_tool.cpp
  tools/draw_point_tool.cpp
  tools/draw_point_gps_tool.cpp
  tools/draw_path_tool.cpp
  tools/draw_circle_tool.cpp
  tools/draw_rectangle_tool.cpp
  tools/draw_freehand_tool.cpp
  tools/draw_text_tool.cpp
  tools/edit_tool.cpp
  tools/edit_point_tool.cpp
  tools/edit_line_tool.cpp
  tools/fill_tool.cpp
  tools/object_selector.cpp
  tools/pan_tool.cpp
  tools/point_handles.cpp
  tools/rotate_tool.cpp
  tools/rotate_pattern_tool.cpp
  tools/scale_tool.cpp
  tools/text_object_editor_helper.cpp
  tools/tool.cpp
  tools/tool_base.cpp
  tools/tool_helpers.cpp
  
  undo/map_part_undo.cpp
  undo/object_undo.cpp
  undo/undo.cpp
  undo/undo_manager.cpp
  
  util/encoding.cpp
  util/item_delegates.cpp
  util/key_value_container.cpp
  util/mapper_service_proxy.cpp
  util/matrix.cpp
  util/overriding_shortcut.cpp
  util/recording_translator.cpp
  util/scoped_signals_blocker.cpp
  util/transformation.cpp
  util/translation_util.cpp
  util/util.cpp
  util/xml_stream_util.cpp
)
if(IOS)
  enable_language(OBJCXX)
  list(APPEND Mapper_Common_SRCS
    core/apple_document_access.mm
  )
  set_source_files_properties(core/apple_document_access.mm PROPERTIES
    COMPILE_OPTIONS "-fobjc-arc"
  )
endif()

# Extra header to be shown in the IDE or to be translated
set(Mapper_Common_HEADERS
  core/objects/object_operations.h
  core/renderables/renderable.h
  core/renderables/renderable_implementation.h
  
  fileformats/file_import_export.h  # translations
  fileformats/ocd_file_import.h     # translations
  fileformats/ocd_types.h
  fileformats/ocd_types_v8.h
  fileformats/ocd_types_v9.h
  fileformats/ocd_types_v10.h
  fileformats/ocd_types_v11.h
  fileformats/ocd_types_v12.h
  fileformats/xml_file_format_p.h
  
  gui/map/map_editor_p.h
  
  templates/world_file.h
  
)

set(CMAKE_AUTOMOC ON)
add_library(Mapper_Common STATIC
  ${Mapper_Common_SRCS}
  ${Mapper_Common_HEADERS}   # for IDE
  ${Mapper_RESOURCES_RCC}
)
mapper_target_defaults(Mapper_Common)
target_include_directories(Mapper_Common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
qt_add_resources(Mapper_Common "mapper-translations"
	OUTPUT_TARGETS Mapper_translation_resource_targets
	PREFIX "/translations"
	BASE "${PROJECT_BINARY_DIR}/translations"
	FILES ${translations_qm}
)
# Capture Qt's static-resource helper target and make it wait for lrelease.
# Xcode does not infer the producer target from generated .qm input paths, so
# a clean parallel archive can otherwise run rcc before those inputs exist.
foreach(mapper_translation_resource_target IN LISTS Mapper_translation_resource_targets)
	add_dependencies(${mapper_translation_resource_target} Mapper_translations)
endforeach()
target_link_libraries(Mapper_Common
  PUBLIC
  Clipper2::Clipper2
  Mapper::ImageryCore
  Mapper::ImageryNetwork
  Mapper::RenderIR
  ${PROJ_LIBRARIES}
  Qt6::Widgets
  Qt6::Svg
  PRIVATE
  Mapper::VelloPresentation
  ICU::data
  ICU::uc
)
foreach(lib
  cove
  mapper-config
  mapper-gdal
  mapper-gnss
  mapper-sensors
  Qt6::Network
  Qt6::PrintSupport
)
	if(TARGET ${lib})
		target_link_libraries(Mapper_Common PUBLIC ${lib})
	endif()
endforeach()
target_compile_definitions(Mapper_Common PRIVATE
  QT_NO_CAST_FROM_ASCII
  QT_NO_CAST_TO_ASCII
  QT_USE_QSTRINGBUILDER
  # MAPPER_ENABLE_COMPATIBILITY
)
target_compile_definitions(Mapper_Common INTERFACE
  MAPPER_COMMON_LIB
)

if(APPLE)
	target_link_libraries(Mapper_Common PRIVATE "-framework Security")
elseif(WIN32)
	target_link_libraries(Mapper_Common PRIVATE Advapi32)
endif()


mapper_translations_sources(${Mapper_Common_SRCS} ${Mapper_Common_HEADERS})


# Mapper executable

set(Mapper_SRCS
  main.cpp
)
 
if(WIN32)
	enable_language(RC)
	configure_file(mingw/resources.rc.in ${CMAKE_CURRENT_BINARY_DIR}/resources.rc @ONLY)
	configure_file(${PROJECT_SOURCE_DIR}/images/mapper-icon/Mapper.ico ${CMAKE_CURRENT_BINARY_DIR}/Mapper.ico COPYONLY)
	list(APPEND Mapper_SRCS ${CMAKE_CURRENT_BINARY_DIR}/resources.rc)
endif()

qt_add_executable(Mapper WIN32 MACOSX_BUNDLE ${Mapper_SRCS})

if(NOT Mapper_MOBILE)
	target_compile_definitions(Mapper PRIVATE MAPPER_USE_KDSINGLEAPPLICATION)
	target_link_libraries(Mapper PRIVATE KDAB::kdsingleapplication)
endif()

mapper_target_defaults(Mapper)
target_link_libraries(Mapper
  PRIVATE
  Mapper_Common
)
target_compile_definitions(Mapper PRIVATE
  QT_NO_CAST_FROM_ASCII
  QT_NO_CAST_TO_ASCII
  QT_USE_QSTRINGBUILDER
)
if(IOS)
	set(mapper_ios_asset_catalog
		"${PROJECT_SOURCE_DIR}/packaging/ios/Assets.xcassets")
	set(mapper_ios_privacy_manifest
		"${PROJECT_SOURCE_DIR}/packaging/ios/PrivacyInfo.xcprivacy")
	set_source_files_properties("${mapper_ios_asset_catalog}" PROPERTIES
		MACOSX_PACKAGE_LOCATION Resources
	)
	set_source_files_properties("${mapper_ios_privacy_manifest}" PROPERTIES
		MACOSX_PACKAGE_LOCATION Resources
	)
	target_sources(Mapper PRIVATE
		"${mapper_ios_asset_catalog}"
		"${mapper_ios_privacy_manifest}"
	)
	foreach(localization_marker IN LISTS Mapper_native_localizations)
		set_source_files_properties("${localization_marker}" PROPERTIES
			# Xcode recognizes the source's .lproj parent and creates the
			# localization directory itself. Including it here would produce a
			# nested <lang>.lproj/<lang>.lproj directory in the bundle.
			MACOSX_PACKAGE_LOCATION Resources)
	endforeach()
	set(mapper_ios_info_strings
		"${PROJECT_SOURCE_DIR}/packaging/ios/en.lproj/InfoPlist.strings")
	set_source_files_properties("${mapper_ios_info_strings}" PROPERTIES
		MACOSX_PACKAGE_LOCATION Resources)
	target_sources(Mapper PRIVATE
		${Mapper_native_localizations}
		"${mapper_ios_info_strings}")
	set_target_properties(Mapper PROPERTIES
		MACOSX_BUNDLE_BUNDLE_NAME "OpenOrienteering Mapper"
		MACOSX_BUNDLE_BUNDLE_VERSION "${Mapper_IOS_BUILD_NUMBER}"
		MACOSX_BUNDLE_COPYRIGHT "${Mapper_COPYRIGHT}"
		MACOSX_BUNDLE_GUI_IDENTIFIER "org.openorienteering.mapper"
		MACOSX_BUNDLE_INFO_PLIST "${PROJECT_SOURCE_DIR}/packaging/ios/Info.plist.in"
		MACOSX_BUNDLE_SHORT_VERSION_STRING "${Mapper_VERSION}"
		XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME AppIcon
		XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "org.openorienteering.mapper"
		XCODE_ATTRIBUTE_SKIP_INSTALL "NO"
	)

	function(mapper_attach_ios_resource_tree source_dir bundle_subdir)
		set(resource_patterns ${ARGN})
		if(NOT resource_patterns)
			set(resource_patterns "*")
		endif()
		set(resource_files)
		foreach(resource_pattern IN LISTS resource_patterns)
			file(GLOB_RECURSE matching_files CONFIGURE_DEPENDS
				LIST_DIRECTORIES FALSE "${source_dir}/${resource_pattern}")
			list(APPEND resource_files ${matching_files})
		endforeach()
		list(REMOVE_DUPLICATES resource_files)
		foreach(resource_file IN LISTS resource_files)
			file(RELATIVE_PATH relative_file "${source_dir}" "${resource_file}")
			get_filename_component(relative_dir "${relative_file}" DIRECTORY)
			set(bundle_location "Resources/${bundle_subdir}")
			if(relative_dir)
				string(APPEND bundle_location "/${relative_dir}")
			endif()
			set_source_files_properties("${resource_file}" PROPERTIES
				MACOSX_PACKAGE_LOCATION "${bundle_location}")
		endforeach()
		target_sources(Mapper PRIVATE ${resource_files})
	endfunction()

	# Keep C-library data on the real filesystem. Both PROJ and GDAL receive
	# these paths through MapperResource and cannot consume Qt resource URLs.
	mapper_attach_ios_resource_tree("${Mapper_PROJ_DATA_DIR}" "proj")
	if(Mapper_USE_GDAL)
		mapper_attach_ios_resource_tree("${Mapper_GDAL_DATA_DIR}" "gdal")
		set_source_files_properties(
			"${PROJECT_SOURCE_DIR}/src/gdal/mapper-osmconf.ini" PROPERTIES
			MACOSX_PACKAGE_LOCATION "Resources/gdal")
		target_sources(Mapper PRIVATE
			"${PROJECT_SOURCE_DIR}/src/gdal/mapper-osmconf.ini")
	endif()
	mapper_attach_ios_resource_tree("${PROJECT_SOURCE_DIR}/examples" "examples"
		"*.omap" "*.png")
	mapper_attach_ios_resource_tree("${PROJECT_SOURCE_DIR}/symbol sets" "symbol sets"
		"*.omap" "*.crt")
	# Restrict the generated tree to runtime notices. Build-system files appear
	# in this directory after generation and must not become app resources or
	# invalidate CONFIGURE_DEPENDS during every Xcode ZERO_CHECK.
	mapper_attach_ios_resource_tree("${PROJECT_BINARY_DIR}/doc/licensing" "doc"
		"licensing.html" "common-licenses/*" "3rd-party/*")
	if(TARGET Mapper-manual)
		add_dependencies(Mapper Mapper-manual)
		add_custom_command(TARGET Mapper POST_BUILD
			COMMAND "${CMAKE_COMMAND}"
				-Dmapper_manual_source=${PROJECT_BINARY_DIR}/doc/manual/html
				-P "${PROJECT_SOURCE_DIR}/cmake/BundleIosManual.cmake"
			COMMENT "Bundling the offline Mapper manual"
			VERBATIM
		)
	endif()
endif()
if(ANDROID)
	set(mapper_android_package_dir "${CMAKE_CURRENT_BINARY_DIR}/android-package")
	file(COPY "${PROJECT_SOURCE_DIR}/packaging/android/"
		DESTINATION "${mapper_android_package_dir}")
	file(COPY "${Mapper_PROJ_DATA_DIR}/"
		DESTINATION "${mapper_android_package_dir}/assets/proj")
	if(Mapper_USE_GDAL)
		file(COPY "${Mapper_GDAL_DATA_DIR}/"
			DESTINATION "${mapper_android_package_dir}/assets/gdal")
		configure_file(
			"${PROJECT_SOURCE_DIR}/src/gdal/mapper-osmconf.ini"
			"${mapper_android_package_dir}/assets/gdal/mapper-osmconf.ini"
			COPYONLY)
	endif()
	configure_file(
		"${PROJECT_SOURCE_DIR}/images/mapper-icon/Mapper-512.png"
		"${mapper_android_package_dir}/res/drawable/icon.png"
		COPYONLY)

	set_target_properties(Mapper PROPERTIES
		QT_ANDROID_APP_NAME "OpenOrienteering Mapper"
		QT_ANDROID_APP_ICON "@drawable/icon"
		QT_ANDROID_COMPILE_SDK_VERSION 36
		QT_ANDROID_MIN_SDK_VERSION 28
		QT_ANDROID_PACKAGE_NAME "org.openorienteering.mapper"
		QT_ANDROID_PACKAGE_SOURCE_DIR "${mapper_android_package_dir}"
		QT_ANDROID_TARGET_SDK_VERSION 36
		QT_ANDROID_VERSION_CODE "${Mapper_VERSION_CODE}"
		QT_ANDROID_VERSION_NAME "${Mapper_VERSION_DISPLAY}"
	)

	add_dependencies(Mapper Mapper_translations)
	if(TARGET Mapper-manual)
		add_dependencies(Mapper Mapper-manual)
	endif()
	add_custom_command(TARGET Mapper POST_BUILD
		COMMAND "${CMAKE_COMMAND}" -E make_directory
			"${mapper_android_package_dir}/assets/doc"
		COMMAND "${CMAKE_COMMAND}" -E copy_directory
			"${PROJECT_BINARY_DIR}/doc/manual/html"
			"${mapper_android_package_dir}/assets/doc/manual"
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different
			"${PROJECT_BINARY_DIR}/doc/licensing/licensing.html"
			"${mapper_android_package_dir}/assets/doc/licensing.html"
		COMMAND "${CMAKE_COMMAND}" -E copy_directory
			"${PROJECT_BINARY_DIR}/doc/licensing/common-licenses"
			"${mapper_android_package_dir}/assets/doc/common-licenses"
		COMMAND "${CMAKE_COMMAND}" -E copy_directory
			"${PROJECT_BINARY_DIR}/doc/licensing/3rd-party"
			"${mapper_android_package_dir}/assets/doc/3rd-party"
		COMMENT "Staging Mapper assets for Android"
		VERBATIM
	)
endif()

install(TARGETS Mapper
  RUNTIME DESTINATION "${MAPPER_RUNTIME_DESTINATION}"
  BUNDLE DESTINATION "${MAPPER_RUNTIME_DESTINATION}"   # macOS
  LIBRARY DESTINATION "${MAPPER_LIBRARY_DESTINATION}"
)

# Headless companion used by Map Hub to freeze the exact event revision into
# a token-scoped public raster. It intentionally renders map objects only: no
# authoring helpers, templates, or third-party basemap can leak into feedback.
if(NOT ANDROID AND NOT IOS)
	qt_add_executable(MapperMapRenderer collaboration/map_hub_renderer.cpp)
	mapper_target_defaults(MapperMapRenderer)
	target_link_libraries(MapperMapRenderer PRIVATE Mapper_Common)
	target_compile_definitions(MapperMapRenderer PRIVATE
		QT_NO_CAST_FROM_ASCII
		QT_NO_CAST_TO_ASCII
		QT_USE_QSTRINGBUILDER
	)
	set_target_properties(MapperMapRenderer PROPERTIES OUTPUT_NAME "mapper-map-render")
	install(TARGETS MapperMapRenderer RUNTIME DESTINATION "${MAPPER_RUNTIME_DESTINATION}")
endif()


# Workaround for "assistant" not being in PATH.
# https://github.com/OpenOrienteering/mapper/issues/2446

if(NOT CMAKE_CROSSCOMPILING AND NOT WIN32 AND NOT APPLE AND NOT ANDROID)
	find_package(Qt6Help)
	if(TARGET Qt6::qhelpgenerator)
		get_target_property(help_program Qt6::qhelpgenerator IMPORTED_LOCATION)
		if(help_program)
			get_filename_component(qt_bin_dir "${help_program}" DIRECTORY)
			set_source_files_properties("gui/util_gui.cpp" PROPERTIES COMPILE_DEFINITIONS "ASSISTANT_DIR=\"${qt_bin_dir}\"")
		endif()
	endif()
endif()
