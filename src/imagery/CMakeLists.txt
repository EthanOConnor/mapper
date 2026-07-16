#
#    Copyright 2026 Ethan O'Connor
#
#    This file is part of OpenOrienteering.
#

set(MAPPER_IMAGERY_CORE_SOURCES
  arcgis_tile_service.cpp
  arcgis_tile_service.h
  imagery_catalog_store.cpp
  imagery_catalog_store.h
  manual_imagery_source.cpp
  manual_imagery_source.h
  imagery_source.cpp
  imagery_source.h
  imagery_source_snapshot.cpp
  imagery_source_snapshot.h
  oic_catalog.cpp
  oic_catalog.h
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

set(MAPPER_IMAGERY_NETWORK_SOURCES
  imagery_catalog_repository.cpp
  imagery_catalog_repository.h
  imagery_network_permissions.cpp
  imagery_network_permissions.h
  tile_network_manager.cpp
  tile_network_manager.h
)

add_library(mapper-imagery-network STATIC ${MAPPER_IMAGERY_NETWORK_SOURCES})
add_library(Mapper::ImageryNetwork ALIAS mapper-imagery-network)
mapper_target_defaults(mapper-imagery-network)
target_include_directories(mapper-imagery-network PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(mapper-imagery-network PUBLIC
  Mapper::ImageryCore
  Qt6::Core
  Qt6::Network
)
target_compile_definitions(mapper-imagery-network PRIVATE
  QT_NO_CAST_FROM_ASCII
  QT_NO_CAST_TO_ASCII
  QT_USE_QSTRINGBUILDER
)
set_target_properties(mapper-imagery-network PROPERTIES PREFIX "")

mapper_translations_sources(${MAPPER_IMAGERY_NETWORK_SOURCES})
