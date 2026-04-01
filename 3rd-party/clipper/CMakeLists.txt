#
#    Copyright 2013-2015 Kai Pastor
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

# Vendored Clipper (libpolyclipping) 6.4.2
# Source: https://sourceforge.net/projects/polyclipping/
# License: Boost Software License (see License.txt)

set(CLIPPER_VERSION 6.4.2)
message(STATUS "Using vendored Clipper library ${CLIPPER_VERSION}")

set(CMAKE_CXX_CLANG_TIDY "")
set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE "")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	# Vendored C++ code; suppress pedantic warnings from project-wide flags.
	add_compile_options(-Wno-pedantic)
endif()

add_library(polyclipping STATIC clipper.cpp)
target_include_directories(polyclipping SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
