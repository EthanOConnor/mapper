#
#    Copyright 2016 Kai Pastor
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


include(CheckCXXCompilerFlag)

function(enable_sanitize)
	if(CMAKE_CROSSCOMPILING)
		return()
	endif()
	foreach(option ${ARGV})
		if(option STREQUAL "NO_RECOVER")
			set(check_flag "-fno-sanitize-recover=all")
			set(flags -fno-sanitize-recover=all -fno-omit-frame-pointer)
			unset(CMAKE_REQUIRED_LIBRARIES)
		else()
			set(check_flag "-fsanitize=${option}")
			set(flags "-fsanitize=${option}")
			set(CMAKE_REQUIRED_LIBRARIES "${check_flag}")
		endif()
		string(MAKE_C_IDENTIFIER "${option}" option_id)
		check_cxx_compiler_flag("${check_flag}" SANITIZE_${option_id})
		if(SANITIZE_${option_id})
			add_compile_options(${flags})
			add_link_options(${flags})
		endif()
	endforeach()
endfunction()
