/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "sql/engine/basic/file_scan_reader.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <unistd.h>
using namespace oceanbase::sql::file_sql;
namespace {
class FileScan : public ::testing::Test {
protected:
  std::string root;
  void SetUp() override { char path[] = "/tmp/file-sql-test-XXXXXX"; root = mkdtemp(path); }
  void TearDown() override { std::filesystem::remove_all(root); }
  void write(const std::string &data) { std::ofstream(root + "/input.csv", std::ios::binary) << data; }
};
TEST_F(FileScan, multiline_quotes_types_and_rescan)
{
  write("id,id,note,day,created\r\n1,2,\"hello,\r\nworld\",2024-02-29,2024-02-29T12:13:14\r\n3,4,\"say \"\"hi\"\"\",,\r\n");
  CsvReader reader;
  ASSERT_TRUE(reader.open(root + "/input.csv", root, {})) << reader.error();
  ASSERT_TRUE(reader.infer()) << reader.error();
  ASSERT_EQ(5u, reader.columns().size());
  EXPECT_EQ("id__2", reader.columns()[1].name);
  EXPECT_EQ(Type::bigint, reader.columns()[0].type);
  EXPECT_EQ(Type::date, reader.columns()[3].type);
  EXPECT_EQ(Type::datetime, reader.columns()[4].type);
  EXPECT_TRUE(reader.columns()[3].nullable);
  std::vector<Cell> row; bool end;
  ASSERT_TRUE(reader.next(row, end)); EXPECT_FALSE(end); EXPECT_EQ("hello,\nworld", row[2].text);
  ASSERT_TRUE(reader.next(row, end)); EXPECT_EQ("say \"hi\"", row[2].text); EXPECT_TRUE(row[3].is_null);
  ASSERT_TRUE(reader.next(row, end)); EXPECT_TRUE(end);
  ASSERT_TRUE(reader.rewind()); ASSERT_TRUE(reader.next(row, end)); EXPECT_EQ("1", row[0].text);
}
TEST_F(FileScan, malformed_and_resource_limits)
{
  for (const std::string data : {"a,b\n1\n", "a\n\"unterminated", "a\n\"quoted\"oops\n", "a\ninvalid\"quote\n", "a\n\xc0\xaf\n"}) {
    write(data); CsvReader reader;
    ASSERT_TRUE(reader.open(root + "/input.csv", root, {}));
    EXPECT_FALSE(reader.infer()); EXPECT_FALSE(reader.error().empty());
  }
  write("a\nlong-record\n"); CsvReader reader; CsvOptions options; options.max_record_bytes = 5;
  ASSERT_TRUE(reader.open(root + "/input.csv", root, options)); EXPECT_FALSE(reader.infer());
}
TEST_F(FileScan, security_and_file_changes)
{
  write("a\n1\n"); CsvReader reader;
  EXPECT_FALSE(reader.open("/etc/passwd", root, {}));
  EXPECT_FALSE(reader.open(root + "/../input.csv", root, {}));
  EXPECT_FALSE(reader.open(root + "/input.csv", "", {}));
  ASSERT_EQ(0, symlink("/etc/passwd", (root + "/link").c_str()));
  EXPECT_FALSE(reader.open(root + "/link", root, {}));
  ASSERT_TRUE(reader.open(root + "/input.csv", root, {})); ASSERT_TRUE(reader.infer());
  write("a\n12345\n"); EXPECT_FALSE(reader.unchanged()); EXPECT_FALSE(reader.rewind());
}
TEST(FileScanTypes, boundaries_and_calendar_validation)
{
  EXPECT_EQ(Type::bigint, CsvReader::infer_type("9223372036854775807"));
  EXPECT_EQ(Type::bigint, CsvReader::infer_type("-9223372036854775808"));
  EXPECT_EQ(Type::varchar, CsvReader::infer_type("9223372036854775808"));
  EXPECT_EQ(Type::varchar, CsvReader::infer_type("2023-02-29"));
  EXPECT_EQ(Type::date, CsvReader::infer_type("2000-02-29"));
  EXPECT_EQ(Type::varchar, CsvReader::infer_type("1900-02-29"));
  EXPECT_EQ(Type::varchar, CsvReader::infer_type("2024-01-01T24:00:00"));
  EXPECT_EQ(Type::varchar, CsvReader::infer_type("1e9999"));
  EXPECT_EQ(Type::real, CsvReader::widen(Type::bigint, Type::real));
  EXPECT_EQ(Type::varchar, CsvReader::widen(Type::boolean, Type::bigint));
}
}
