# Include only after gow3-native-live-capture-replay exists. Main gameplay and
# every other Scene target compile no census fields/hooks or query operations.
target_compile_definitions(gow3-native-live-capture-replay PRIVATE GOW3_NATIVE_FRAGMENT_CENSUS_REPLAY=1)
target_sources(gow3-native-live-capture-replay PRIVATE
    "${GOW3_ROOT}/src/renderer/native/fragment_invocation_census.cpp")
add_executable(gow3-native-fragment-invocation-ledger-test fragment_invocation_ledger_test.cpp)
target_include_directories(gow3-native-fragment-invocation-ledger-test PRIVATE "${GOW3_ROOT}/src")
