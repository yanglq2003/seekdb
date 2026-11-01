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
#include "sql/engine/basic/file_scan_path.h"
#include "sql/engine/basic/file_sql_config.h"
#include "sql/engine/basic/file_scan_parquet_codec.h"
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

TEST_F(FileScan, json_union_missing_values_and_rescan)
{
  write("{\"a\":1,\"enabled\":true}\n{\"a\":2.5,\"later\":\"value\"}\n");
  JsonLineReader reader;
  ASSERT_TRUE(reader.open(root + "/input.csv", root)) << reader.error();
  ASSERT_TRUE(reader.infer()) << reader.error();
  ASSERT_EQ(3u, reader.columns().size());
  EXPECT_EQ(Type::real, reader.columns()[0].type);
  EXPECT_TRUE(reader.columns()[1].nullable);
  EXPECT_TRUE(reader.columns()[2].nullable);
  std::vector<Cell> row; bool end;
  ASSERT_TRUE(reader.next(row, end)); EXPECT_TRUE(row[2].is_null);
  ASSERT_TRUE(reader.next(row, end)); EXPECT_TRUE(row[1].is_null); EXPECT_EQ("value", row[2].text);
  ASSERT_TRUE(reader.next(row, end)); EXPECT_TRUE(end);
  ASSERT_TRUE(reader.rewind()); ASSERT_TRUE(reader.next(row, end)); EXPECT_EQ("1", row[0].text);
}
TEST_F(FileScan, json_rejects_nested_and_duplicate_fields)
{
  for (const std::string data : {"{\"a\":[]}\n", "{\"a\":{}}\n", "[]\n", "{\"a\":1,\"a\":2}\n", "{invalid}\n"}) {
    write(data); JsonLineReader reader;
    ASSERT_TRUE(reader.open(root + "/input.csv", root));
    EXPECT_FALSE(reader.infer()); EXPECT_FALSE(reader.error().empty());
  }
}

TEST_F(FileScan, json_scalar_type_conflicts_preserve_strings)
{
  write("{\"a\":1,\"b\":true}\n{\"a\":\"2\",\"b\":null}\n");
  JsonLineReader reader;
  ASSERT_TRUE(reader.open(root + "/input.csv", root));
  ASSERT_TRUE(reader.infer()) << reader.error();
  EXPECT_EQ(Type::varchar, reader.columns()[0].type);
  EXPECT_EQ(Type::boolean, reader.columns()[1].type);
  EXPECT_TRUE(reader.columns()[1].nullable);
}
TEST(FileSqlConfig, plan_descriptor_round_trip_and_validation)
{
  Config config; config.format = "read_csv"; config.path = "name\"with,escape.csv";
  config.columns.push_back({"id", Type::bigint, false});
  Config parsed;
  ASSERT_TRUE(parsed.decode(config.encode()));
  EXPECT_EQ(config.path, parsed.path);
  ASSERT_EQ(1u, parsed.columns.size());
  EXPECT_EQ(Type::bigint, parsed.columns[0].type);
  EXPECT_FALSE(parsed.decode("file_sql:v1:{}"));
  EXPECT_FALSE(parsed.decode("untrusted"));
}

TEST(FileSqlParquetCodec, compact_metadata_and_snappy_bounds)
{
  std::string encoded("\x15\x54\x18\x03" "abc\0", 8);
  parquet::Value value;
  parquet::Compact compact(encoded);
  ASSERT_TRUE(compact.structure(value));
  EXPECT_EQ(42, value.field(1).integer);
  EXPECT_EQ("abc", value.field(2).bytes);
  std::string output;
  ASSERT_TRUE(parquet::snappy(std::string("\x08\x04" "ab\x09\x02", 6), 8, output));
  EXPECT_EQ("abababab", output);
  EXPECT_FALSE(parquet::snappy(std::string("\x08\x04" "ab\x09\0", 6), 8, output));
  EXPECT_FALSE(parquet::snappy(std::string("\x08\x04" "ab\x09\x02", 6), 7, output));
}
TEST(FileSqlParquet, page_versions_dictionaries_nulls_and_row_groups)
{
  const auto base = std::filesystem::path(__FILE__).parent_path() / "file_sql_data";
  for (const char *page : {"1.0", "2.0"}) for (const char *compression : {"none", "snappy"}) for (int dictionary : {0, 1}) {
    const auto path = base / (std::string("flat-v") + page + "-" + compression + "-dict" + std::to_string(dictionary) + ".parquet");
    ParquetReader reader;
    ASSERT_TRUE(reader.open(std::filesystem::absolute(path).string(), std::filesystem::absolute(base).string())) << path;
    ASSERT_TRUE(reader.infer()) << reader.error() << ": " << path;
    ASSERT_EQ(6u, reader.columns().size());
    EXPECT_EQ(Type::date, reader.columns()[4].type);
    EXPECT_EQ(Type::datetime, reader.columns()[5].type);
    std::vector<Cell> row; bool end = false;
    int64_t sum = 0, count = 0;
    while (true) {
      ASSERT_TRUE(reader.next(row, end)) << reader.error() << ": " << path;
      if (end) break;
      ASSERT_EQ(6u, row.size());
      sum += std::stoll(row[0].text);
      if (count == 0) { EXPECT_EQ("2024-02-29", row[4].text); EXPECT_EQ("true", row[2].text); }
      if (count == 2) { EXPECT_TRUE(row[1].is_null); }
      ++count;
    }
    EXPECT_EQ(7, count); EXPECT_EQ(21, sum);
    ASSERT_TRUE(reader.project({0}));
    ASSERT_TRUE(reader.next(row, end));
    EXPECT_EQ("0", row[0].text); EXPECT_TRUE(row[1].is_null);
    ASSERT_TRUE(reader.project({}));
    count = 0;
    while (true) { ASSERT_TRUE(reader.next(row, end)); if (end) break; ++count; }
    EXPECT_EQ(7, count);
  }
}
TEST_F(FileScan, parquet_rejects_truncated_and_wrong_magic)
{
  for (const std::string data : {"PAR1", "PAR1garbagePAR1", "not a parquet file"}) {
    write(data); ParquetReader reader;
    ASSERT_TRUE(reader.open(root + "/input.csv", root));
    EXPECT_FALSE(reader.infer()); EXPECT_FALSE(reader.error().empty());
  }
}

TEST_F(FileScan, glob_discovery_sorting_recursion_and_boundary)
{
  write("id\n1\n");
  std::filesystem::create_directory(root + "/nested");
  std::ofstream(root + "/nested/other.csv") << "id\n2\n";
  ASSERT_EQ(0, symlink("/etc", (root + "/escape").c_str()));
  GlobReader reader;
  ASSERT_TRUE(reader.open(root + "/**/*.csv", root)) << reader.error();
  EXPECT_EQ("file", reader.columns()[0].name);
  std::vector<Cell> row; bool end = false;
  ASSERT_TRUE(reader.next(row, end)); EXPECT_EQ(root + "/input.csv", row[0].text);
  ASSERT_TRUE(reader.next(row, end)); EXPECT_EQ(root + "/nested/other.csv", row[0].text);
  ASSERT_TRUE(reader.next(row, end)); EXPECT_TRUE(end);
  EXPECT_FALSE(reader.open("/etc/*", root));
  EXPECT_FALSE(reader.open(root + "/../*", root));
  ASSERT_TRUE(reader.open(root + "/escape/*", root));
  ASSERT_TRUE(reader.next(row, end)); EXPECT_TRUE(end);
}
TEST_F(FileScan, csv_bom_without_header_survives_rescan)
{
  write("\xef\xbb\xbf" "1,2\n3,4\n");
  CsvReader reader; CsvOptions options; options.header = false;
  ASSERT_TRUE(reader.open(root + "/input.csv", root, options));
  ASSERT_TRUE(reader.infer()) << reader.error();
  std::vector<Cell> row; bool end;
  ASSERT_TRUE(reader.next(row, end)); EXPECT_EQ("1", row[0].text);
  ASSERT_TRUE(reader.rewind()); ASSERT_TRUE(reader.next(row, end)); EXPECT_EQ("1", row[0].text);
}

TEST_F(FileScan, csv_atomic_export_roundtrip_and_target_protection)
{
  CsvOptions options; options.null_text = "NULL";
  const std::string path = root + "/export.csv";
  {
    AtomicCsvWriter writer;
    ASSERT_TRUE(writer.open(path, root, options, {"id", "note", "missing"})) << writer.error();
    ASSERT_TRUE(writer.append({Cell{"1", false}, Cell{"say \"hi\",\nworld", false}, Cell{"", true}}));
    ASSERT_TRUE(writer.append({Cell{"2", false}, Cell{"NULL", false}, Cell{"", false}}));
    EXPECT_FALSE(std::filesystem::exists(path));
    ASSERT_TRUE(writer.finish()) << writer.error();
  }
  CsvReader reader;
  ASSERT_TRUE(reader.open(path, root, options)); ASSERT_TRUE(reader.infer()) << reader.error();
  std::vector<Cell> row; bool end;
  ASSERT_TRUE(reader.next(row, end)); EXPECT_EQ("say \"hi\",\nworld", row[1].text); EXPECT_TRUE(row[2].is_null);
  ASSERT_TRUE(reader.next(row, end)); EXPECT_EQ("NULL", row[1].text); EXPECT_FALSE(row[1].is_null); EXPECT_FALSE(row[2].is_null);
  AtomicCsvWriter duplicate;
  EXPECT_FALSE(duplicate.open(path, root, options, {"id"}));
}
TEST_F(FileScan, csv_export_failure_removes_temporary_and_cannot_replace_racing_target)
{
  {
    AtomicCsvWriter writer;
    ASSERT_TRUE(writer.open(root + "/failed.csv", root, {}, {"id"}));
    EXPECT_FALSE(writer.append({}));
    EXPECT_FALSE(writer.finish());
  }
  EXPECT_FALSE(std::filesystem::exists(root + "/failed.csv"));
  EXPECT_TRUE(std::filesystem::is_empty(root));
  {
    AtomicCsvWriter writer;
    ASSERT_TRUE(writer.open(root + "/raced.csv", root, {}, {"id"}));
    ASSERT_TRUE(writer.append({Cell{"1", false}}));
    std::ofstream(root + "/raced.csv") << "owned by another writer";
    EXPECT_FALSE(writer.finish());
  }
  size_t files = 0;
  for (const auto &entry : std::filesystem::directory_iterator(root)) {
    EXPECT_EQ("raced.csv", entry.path().filename().string()); ++files;
  }
  EXPECT_EQ(1u, files);
  std::ifstream input(root + "/raced.csv"); std::string text; std::getline(input, text);
  EXPECT_EQ("owned by another writer", text);
}

TEST_F(FileScan, json_deep_nesting_is_rejected_without_recursive_parsing)
{
  write("{\"value\":" + std::string(10000, '[') + "0" + std::string(10000, ']') + "}\n");
  JsonLineReader reader;
  ASSERT_TRUE(reader.open(root + "/input.csv", root));
  EXPECT_FALSE(reader.infer());
  write("{\"value\":18446744073709551616}\n");
  ASSERT_TRUE(reader.open(root + "/input.csv", root));
  ASSERT_TRUE(reader.infer()) << reader.error();
  EXPECT_EQ(Type::varchar, reader.columns()[0].type);
  std::vector<Cell> row; bool end;
  ASSERT_TRUE(reader.next(row, end)); EXPECT_EQ("18446744073709551616", row[0].text);
}

TEST_F(FileScan, canonical_directory_rejects_replaced_ancestor)
{
  std::filesystem::create_directories(root + "/original/child");
  int descriptor = open_canonical_directory(root + "/original/child");
  ASSERT_GE(descriptor, 0);
  close(descriptor);
  std::filesystem::rename(root + "/original", root + "/moved");
  std::filesystem::create_directory_symlink(root + "/moved", root + "/original");
  EXPECT_EQ(-1, open_canonical_directory(root + "/original/child"));
}

TEST_F(FileScan, json_union_name_budget_is_bounded_across_records)
{
  std::string input;
  for (unsigned i = 0; i < 18; ++i) {
    input += "{\"" + std::to_string(i) + std::string(65536, 'a') + "\":1}\n";
  }
  write(input);
  JsonLineReader reader;
  ASSERT_TRUE(reader.open(root + "/input.csv", root));
  EXPECT_FALSE(reader.infer());
  EXPECT_NE(std::string::npos, reader.error().find("budget"));
}
