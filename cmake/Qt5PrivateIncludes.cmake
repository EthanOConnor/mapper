#
#    Copyright 2026 The OpenOrienteering developers
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


# Add Qt 5 private include directories to a target.
# Workaround for QTBUG-37417: some Qt 5 packages do not populate
# Qt5<Module>_PRIVATE_INCLUDE_DIRS.  Fixed in Qt 5.7+, but we still
# need the target_include_directories call itself.
function(target_qt5_private_includes target)
	foreach(module ${ARGN})
		set(qt_module Qt${module})
		set(qt5_module Qt5${module})
		if("${${qt5_module}_PRIVATE_INCLUDE_DIRS}" STREQUAL "")
			foreach(base_dir ${${qt5_module}_INCLUDE_DIRS})
				if("${base_dir}" MATCHES "/${qt_module}$")
					list(APPEND ${qt5_module}_PRIVATE_INCLUDE_DIRS
					  "${base_dir}/${${qt5_module}_VERSION}/${qt_module}")
				endif()
			endforeach()
		endif()
		target_include_directories(${target} SYSTEM PRIVATE ${${qt5_module}_PRIVATE_INCLUDE_DIRS})
	endforeach()
endfunction()
