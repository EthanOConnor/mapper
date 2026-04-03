# Vendored Clipper2Lib 2.0.1
# Source: https://github.com/AngusJohnson/Clipper2 (tag Clipper2_2.0.1)
# License: Boost Software License 1.0 (see LICENSE)

set(CMAKE_CXX_CLANG_TIDY "")
set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE "")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wno-pedantic)
endif()

add_library(clipper2 STATIC src/clipper.engine.cpp)
target_include_directories(clipper2 SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_compile_features(clipper2 PUBLIC cxx_std_17)
