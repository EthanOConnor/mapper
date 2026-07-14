include_guard(GLOBAL)

function(mapper_define_build_options)
	add_library(mapper-build-options INTERFACE)
	add_library(Mapper::BuildOptions ALIAS mapper-build-options)

	target_compile_features(mapper-build-options INTERFACE cxx_std_23)
	target_compile_definitions(mapper-build-options INTERFACE
		_USE_MATH_DEFINES
		UNICODE
		QT_DISABLE_DEPRECATED_BEFORE=0x060a00
	)
	target_compile_options(mapper-build-options INTERFACE
		"$<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>"
		"$<$<COMPILE_LANG_AND_ID:C,AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>"
	)
	# Static GDAL and PROJ exports repeat some archive paths. Apple's linker
	# rescans and de-duplicates them correctly but warns unless asked not to.
	target_link_options(mapper-build-options INTERFACE
		"$<$<LINK_LANG_AND_ID:CXX,AppleClang>:LINKER:-no_warn_duplicate_libraries>"
	)

	if(CMAKE_CXX_BYTE_ORDER STREQUAL "BIG_ENDIAN")
		target_compile_definitions(mapper-build-options INTERFACE MAPPER_BIG_ENDIAN)
	endif()

	if(Mapper_DEVELOPMENT_BUILD)
		target_compile_definitions(mapper-build-options INTERFACE MAPPER_DEVELOPMENT_BUILD)
	endif()

	if(Mapper_ENABLE_SANITIZERS)
		if(CMAKE_CXX_COMPILER_ID MATCHES "AppleClang|Clang|GNU")
			target_compile_options(mapper-build-options INTERFACE
				-fno-omit-frame-pointer
				-fsanitize=address,undefined
			)
			target_link_options(mapper-build-options INTERFACE
				-fsanitize=address,undefined
			)
		else()
			message(FATAL_ERROR
				"Mapper_ENABLE_SANITIZERS is not implemented for ${CMAKE_CXX_COMPILER_ID}")
		endif()
	endif()
endfunction()

function(mapper_target_defaults target)
	if(NOT TARGET "${target}")
		message(FATAL_ERROR "mapper_target_defaults: unknown target ${target}")
	endif()
	target_link_libraries("${target}" PRIVATE Mapper::BuildOptions)
endfunction()
