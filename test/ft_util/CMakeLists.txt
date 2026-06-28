add_executable(FtUtilTest FtUtilTest.cpp)

target_include_directories(FtUtilTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(FtUtilTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FtUtilTest)
