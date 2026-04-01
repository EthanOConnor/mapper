find_package(Qt6 REQUIRED COMPONENTS Core Gui Test)

set(CMAKE_AUTOMOC ON)

configure_file(test_config.h.in test_config.h)
add_library(cove-test-config INTERFACE)
target_include_directories(cove-test-config INTERFACE "${CMAKE_CURRENT_BINARY_DIR}")

add_executable(cove-ParallelImageProcessingTest
  ParallelImageProcessingTest.cpp
)
add_test(
  NAME cove-ParallelImageProcessingTest
  COMMAND cove-ParallelImageProcessingTest
)
add_executable(cove-PolygonTest
  PolygonTest.cpp
)
target_link_libraries(cove-PolygonTest
  PRIVATE cove-test-config
)
add_test(
  NAME cove-PolygonTest
  COMMAND cove-PolygonTest
)

add_executable(cove-PolygonBenchmark EXCLUDE_FROM_ALL
  PolygonTest.cpp
)
target_link_libraries(cove-PolygonBenchmark
  PRIVATE cove-test-config
)
target_compile_definitions(cove-PolygonBenchmark
  PRIVATE
    COVE_BENCHMARK=QBENCHMARK
)

foreach(target
  cove-ParallelImageProcessingTest
  cove-PolygonTest
  cove-PolygonBenchmark
)
	target_link_libraries(${target}
	  PRIVATE
	    cove-vectorizer
	    Qt6::Test
	    Qt6::Gui
	)
endforeach()

