# Focused opt-in File SQL tests, using the same Linux runtime as seekdb.
option(SEEKDB_FILE_SQL_TESTS "Build File SQL parser and reader regression tests" OFF)
if(NOT SEEKDB_FILE_SQL_TESTS)
  return()
endif()

find_library(FILE_SQL_GTEST NAMES gtest HINTS "${DEP_DIR}/lib" REQUIRED)
find_library(FILE_SQL_GTEST_MAIN NAMES gtest_main HINTS "${DEP_DIR}/lib" REQUIRED)

add_executable(test_file_sql_parser EXCLUDE_FROM_ALL
  sql/parser/test_file_sql_parser.cpp)
target_link_libraries(test_file_sql_parser PRIVATE
  oceanbase ${FILE_SQL_GTEST_MAIN} ${FILE_SQL_GTEST} pthread)
add_test(NAME file_sql_parser COMMAND test_file_sql_parser)

add_executable(test_file_scan_utils EXCLUDE_FROM_ALL
  sql/engine/basic/test_file_scan_utils.cpp
  ${PROJECT_SOURCE_DIR}/src/sql/engine/basic/file_scan_reader.cpp
  ${PROJECT_SOURCE_DIR}/src/sql/engine/basic/file_scan_json.cpp
  ${PROJECT_SOURCE_DIR}/src/sql/engine/basic/file_scan_parquet.cpp
  ${PROJECT_SOURCE_DIR}/src/sql/engine/basic/file_scan_glob.cpp
  ${PROJECT_SOURCE_DIR}/src/sql/engine/basic/file_scan_csv_writer.cpp)
target_link_libraries(test_file_scan_utils PRIVATE
  ob_base ${FILE_SQL_GTEST_MAIN} ${FILE_SQL_GTEST} pthread)
add_test(NAME file_scan_utils COMMAND test_file_scan_utils)
