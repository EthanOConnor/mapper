#
#    Copyright 2026 Ethan O'Connor
#
#    This file is part of OpenOrienteering.
#

set(MAPPER_IMAGERY_CORE_SOURCES
  imagery_source.cpp
  imagery_source.h
  imagery_source_snapshot.cpp
  imagery_source_snapshot.h
  tile_matrix_set.cpp
  tile_matrix_set.h
)

add_library(mapper-imagery-core STATIC ${MAPPER_IMAGERY_CORE_SOURCES})
add_library(Mapper::ImageryCore ALIAS mapper-imagery-core)
mapper_target_defaults(mapper-imagery-core)
target_include_directories(mapper-imagery-core PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(mapper-imagery-core PUBLIC Qt6::Core)
target_compile_definitions(mapper-imagery-core PRIVATE
  QT_NO_CAST_FROM_ASCII
  QT_NO_CAST_TO_ASCII
  QT_USE_QSTRINGBUILDER
)
set_target_properties(mapper-imagery-core PROPERTIES
  AUTOMOC OFF
  AUTOUIC OFF
  PREFIX ""
)

mapper_translations_sources(${MAPPER_IMAGERY_CORE_SOURCES})
