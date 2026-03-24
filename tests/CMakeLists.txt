add_executable(media_storage_tests
        test_integrity.cpp
        test_chunker.cpp
        test_codec.cpp
        test_crypto.cpp
        test_roundtrip.cpp
)

target_link_libraries(media_storage_tests PRIVATE
        media_storage_core
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(media_storage_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
)

include(GoogleTest)
gtest_discover_tests(media_storage_tests)
