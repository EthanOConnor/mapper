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

message(STATUS "Configuring ${PROJECT_NAME} API documentation")

find_package(Doxygen 1.9 REQUIRED)
configure_file(Doxyfile.in Doxyfile @ONLY)

add_custom_target(Mapper-api-docs
	COMMAND Doxygen::doxygen "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
	WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
	COMMENT "Generating Mapper API documentation"
	SOURCES
		Doxyfile.in
		extra/code_overview.h
		extra/mainpage.h
	VERBATIM
)
