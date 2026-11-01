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
#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace oceanbase { namespace sql { namespace file_sql {

enum class Type { null_value, bigint, real, boolean, varchar, date, datetime };
struct Cell { std::string text; bool is_null = false; };
struct Column { std::string name; Type type = Type::null_value; bool nullable = false; };
struct CsvOptions {
  bool header = true;
  char delimiter = ',';
  char quote = '"';
  char escape = '"';
  std::string null_text;
  size_t max_record_bytes = 16 * 1024 * 1024;
  size_t max_columns = 4096;
};

// Owns a read-only descriptor. Schema inference retains only one record at a time.
class CsvReader final {
public:
  CsvReader() = default;
  ~CsvReader();
  CsvReader(const CsvReader &) = delete;
  CsvReader &operator=(const CsvReader &) = delete;
  bool open(const std::string &path, const std::string &secure_root, const CsvOptions &options);
  bool infer();
  bool next(std::vector<Cell> &row, bool &end);
  bool rewind();
  bool unchanged();
  const std::vector<Column> &columns() const { return columns_; }
  const std::string &error() const { return error_; }
  static Type infer_type(const std::string &text);
  static Type widen(Type left, Type right);
private:
  bool record(std::vector<Cell> &row, bool &end);
  bool fail(const std::string &message);
  FILE *file_ = nullptr;
  CsvOptions options_;
  std::vector<Column> columns_;
  std::string error_;
  std::string identity_;
  size_t line_ = 1;
  bool inferred_ = false;
};
}}}
