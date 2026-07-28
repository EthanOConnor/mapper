# SPDX-FileCopyrightText: 2026 OpenOrienteering contributors
# SPDX-License-Identifier: GPL-3.0-or-later

if(NOT DEFINED mapper_manual_source
   OR NOT IS_DIRECTORY "${mapper_manual_source}")
	message(FATAL_ERROR "The generated Mapper manual directory is unavailable")
endif()

if("$ENV{TARGET_BUILD_DIR}" STREQUAL ""
   OR "$ENV{WRAPPER_NAME}" STREQUAL "")
	message(FATAL_ERROR "Xcode did not provide the target application bundle path")
endif()

set(mapper_manual_destination
	"$ENV{TARGET_BUILD_DIR}/$ENV{WRAPPER_NAME}/doc/manual")
file(REMOVE_RECURSE "${mapper_manual_destination}")
file(MAKE_DIRECTORY "${mapper_manual_destination}")
file(COPY "${mapper_manual_source}/"
	DESTINATION "${mapper_manual_destination}")
