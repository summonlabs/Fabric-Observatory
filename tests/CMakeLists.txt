# Fabric Observatory test suite.
#
# Every test relies on its own determinism: there are no timeouts, no sleeps and
# no wall clock dependencies. A test that hangs is a defect, and the harness
# reports it as a hung process rather than masking it behind a timer.

add_library(fabric_observatory_test_support STATIC test_main.cpp)
target_link_libraries(fabric_observatory_test_support PUBLIC FabricObservatory::fabric_observatory)
target_include_directories(fabric_observatory_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
fabric_observatory_apply_warnings(fabric_observatory_test_support)
if(FABRIC_OBSERVATORY_ENABLE_ASAN)
  fabric_observatory_enable_asan(fabric_observatory_test_support)
endif()

set(FABRIC_OBSERVATORY_TESTS
  unit_identity
  unit_truth
  unit_snapshot
  unit_bounds
  unit_query_diff
  unit_persistence
  integration_ingest
  integration_restart
  property_invariants
  adversarial_input
  concurrency_runtime
  process_transport
  e2e_cli)

foreach(test_name IN LISTS FABRIC_OBSERVATORY_TESTS)
  add_executable(test_${test_name} ${test_name}.cpp)
  target_link_libraries(test_${test_name} PRIVATE fabric_observatory_test_support)
  target_include_directories(test_${test_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  fabric_observatory_apply_warnings(test_${test_name})
  if(FABRIC_OBSERVATORY_ENABLE_ASAN)
    fabric_observatory_enable_asan(test_${test_name})
  endif()
  if(TARGET fabobs)
    target_compile_definitions(test_${test_name} PRIVATE
      FABOBS_TEST_BINARY="$<TARGET_FILE:fabobs>")
  endif()
  add_test(NAME ${test_name} COMMAND test_${test_name})
  # No test timeout of any kind: a hang must be visible as a hang.
  set_tests_properties(${test_name} PROPERTIES TIMEOUT 0)
endforeach()
