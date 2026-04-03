#
#    Copyright 2015, 2016 Kai Pastor
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

find_package(Qt6 REQUIRED COMPONENTS Gui PrintSupport)
if(WIN32)
	find_package(Qt6 REQUIRED COMPONENTS PrintSupportPrivate)
endif()

set(PRINTSUPPORT_SRCS
  printer_properties.h
)

if(WIN32)
	list(APPEND PRINTSUPPORT_SRCS printer_properties_win.cpp)
else()
	list(APPEND PRINTSUPPORT_SRCS printer_properties.cpp)
endif()

add_library(printsupport STATIC ${PRINTSUPPORT_SRCS})

target_compile_definitions(printsupport PRIVATE
  QT_NO_CAST_FROM_ASCII
  QT_NO_CAST_TO_ASCII
  QT_USE_QSTRINGBUILDER
)
target_include_directories(printsupport
  INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(printsupport
  PUBLIC Qt6::PrintSupport Qt6::Gui
)
if(WIN32)
	target_link_libraries(printsupport PRIVATE Qt6::PrintSupportPrivate)
endif()
