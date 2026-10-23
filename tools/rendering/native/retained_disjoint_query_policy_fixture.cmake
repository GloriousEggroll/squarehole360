# Standalone source-only CPU fixture wiring; include after GOW3_ROOT is set.
add_executable(gow3-native-retained-disjoint-query-policy-test
    "${CMAKE_CURRENT_LIST_DIR}/retained_disjoint_query_policy_test.cpp")
target_include_directories(gow3-native-retained-disjoint-query-policy-test PRIVATE
    "${GOW3_ROOT}/src/renderer/native")
