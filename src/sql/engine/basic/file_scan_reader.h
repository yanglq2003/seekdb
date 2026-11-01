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
struct Cell { std::string text; bool is_null = false; Type declared_type = Type::null_value; };
struct Column { std::string name; Type type = Type::null_value; bool nullable = false; unsigned scale = 0; };
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
  bool read_line(std::string &line, bool &end);
  bool seek_start();
  bool read_at(uint64_t offset, size_t length, std::string &bytes);
  bool size(uint64_t &length);
  const std::string &identity() const { return identity_; }
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

class JsonLineReader final {
public:
  const std::string &identity() const { return file_.identity(); }
  bool open(const std::string &path, const std::string &secure_root);
  bool infer();
  bool next(std::vector<Cell> &row, bool &end);
  bool rewind();
  const std::vector<Column> &columns() const { return columns_; }
  const std::string &error() const { return error_; }
private:
  bool object(std::vector<std::pair<std::string, Cell>> &fields, bool &end);
  CsvReader file_;
  std::vector<Column> columns_;
  std::string error_;
  size_t line_ = 0;
  bool inferred_ = false;
};

class ParquetReader final {
public:
  const std::string &identity() const;
  ParquetReader();
  ~ParquetReader();
  ParquetReader(const ParquetReader &) = delete;
  ParquetReader &operator=(const ParquetReader &) = delete;
  bool open(const std::string &path, const std::string &secure_root);
  bool infer();
  bool project(const std::vector<size_t> &columns);
  bool next(std::vector<Cell> &row, bool &end);
  bool rewind();
  const std::vector<Column> &columns() const;
  const std::string &error() const;
private:
  struct State;
  State *state_;
};
class GlobReader final {
public:
  bool open(const std::string &pattern, const std::string &secure_root);
  bool next(std::vector<Cell> &row, bool &end);
  const std::vector<Column> &columns() const { return columns_; }
  const std::string &error() const { return error_; }
private:
  std::vector<Column> columns_{{"file", Type::varchar, false}};
  std::vector<std::string> paths_;
  std::string error_;
  size_t position_ = 0;
};
class AtomicCsvWriter final {
public:
  AtomicCsvWriter() = default;
  ~AtomicCsvWriter();
  AtomicCsvWriter(const AtomicCsvWriter &) = delete;
  AtomicCsvWriter &operator=(const AtomicCsvWriter &) = delete;
  bool open(const std::string &path, const std::string &secure_root, const CsvOptions &options,
            const std::vector<std::string> &names);
  bool append(const std::vector<Cell> &row);
  bool finish();
  const std::string &error() const { return error_; }
private:
  bool fail(const std::string &message);
  FILE *file_ = nullptr;
  int directory_ = -1;
  std::string temporary_, target_, error_;
  CsvOptions options_;
  size_t columns_ = 0;
  bool finished_ = false;
};
}}}
