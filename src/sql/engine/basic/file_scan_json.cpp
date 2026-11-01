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
#include "file_scan_reader.h"
#include <rapidjson/reader.h>
#include <rapidjson/memorystream.h>
#include <set>
#include <algorithm>
#include <cctype>

namespace oceanbase { namespace sql { namespace file_sql {
namespace {
std::string folded(std::string text)
{
  for (char &c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return text;
}
}
bool JsonLineReader::open(const std::string &path, const std::string &root)
{
  columns_.clear(); error_.clear(); line_ = 0; inferred_ = false;
  if (!file_.open(path, root, {})) { error_ = file_.error(); return false; }
  return true;
}
bool JsonLineReader::object(std::vector<std::pair<std::string, Cell>> &fields, bool &end)
{
  fields.clear();
  std::string line;
  do {
    if (!file_.read_line(line, end)) { error_ = file_.error(); return false; }
    if (end) return true;
    ++line_;
    if (line_ == 1 && line.compare(0, 3, "\xef\xbb\xbf") == 0) line.erase(0, 3);
  } while (line.find_first_not_of(" \t\r") == std::string::npos);
  struct FlatObject : rapidjson::BaseReaderHandler<rapidjson::UTF8<>, FlatObject> {
    std::vector<std::pair<std::string, Cell>> &fields;
    std::set<std::string> names;
    std::string key;
    bool started = false, closed = false, pending = false;
    explicit FlatObject(std::vector<std::pair<std::string, Cell>> &output) : fields(output) {}
    bool StartObject() { if (started) return false; started = true; return true; }
    bool EndObject(rapidjson::SizeType) { if (!started || pending || closed) return false; closed = true; return true; }
    bool StartArray() { return false; }
    bool EndArray(rapidjson::SizeType) { return false; }
    bool Key(const char *text, rapidjson::SizeType length, bool) {
      if (!started || closed || pending || fields.size() >= 4096) return false;
      key.assign(text, length);
      if (key.empty() || key.find('\0') != std::string::npos || !names.insert(folded(key)).second) return false;
      pending = true; return true;
    }
    bool store(Cell cell) {
      if (!started || closed || !pending) return false;
      fields.emplace_back(key, std::move(cell)); pending = false; return true;
    }
    bool Null() { return store(Cell{"", true}); }
    bool Bool(bool value) { return store(Cell{value ? "true" : "false", false, Type::boolean}); }
    bool String(const char *text, rapidjson::SizeType length, bool) {
      Cell cell; cell.text.assign(text, length);
      Type type = CsvReader::infer_type(cell.text);
      cell.declared_type = type == Type::date || type == Type::datetime ? type : Type::varchar;
      return store(std::move(cell));
    }
    bool RawNumber(const char *text, rapidjson::SizeType length, bool) {
      Cell cell; cell.text.assign(text, length); cell.declared_type = CsvReader::infer_type(cell.text);
      return store(std::move(cell));
    }
  } handler(fields);
  rapidjson::MemoryStream input(line.data(), line.size());
  rapidjson::Reader reader;
  if (!reader.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseNumbersAsStringsFlag>(input, handler)
      || !handler.closed) {
    error_ = "JSONL line " + std::to_string(line_) + ": invalid or non-flat object at byte "
           + std::to_string(reader.GetErrorOffset());
    return false;
  }
  return true;
}
bool JsonLineReader::infer()
{
  if (!rewind()) return false;
  columns_.clear(); inferred_ = false;
  size_t count = 0;
  size_t schema_bytes = 0;
  std::vector<std::pair<std::string, Cell>> fields;
  bool end = false;
  while (object(fields, end) && !end) {
    std::set<std::string> present;
    for (const auto &field : fields) {
      const std::string key = folded(field.first);
      present.insert(key);
      auto column = std::find_if(columns_.begin(), columns_.end(), [&](const Column &c) { return folded(c.name) == key; });
      if (column == columns_.end()) {
        if (columns_.size() >= 4096) { error_ = "JSONL union schema exceeds column limit"; return false; }
        if (field.first.size() > 1024 * 1024 - schema_bytes) {
          error_ = "JSONL union schema exceeds name budget"; return false;
        }
        schema_bytes += field.first.size();
        columns_.push_back({field.first, Type::null_value, count != 0});
        column = columns_.end() - 1;
      }
      column->nullable |= field.second.is_null;
      column->type = CsvReader::widen(column->type, field.second.is_null ? Type::null_value : field.second.declared_type);
      if (field.second.declared_type == Type::datetime && field.second.text.size() > 20)
        column->scale = std::max<unsigned>(column->scale, field.second.text.size() - 20);
    }
    for (auto &column : columns_) if (!present.count(folded(column.name))) column.nullable = true;
    ++count;
  }
  if (!error_.empty()) return false;
  if (columns_.empty()) { error_ = "JSONL input has no columns"; return false; }
  inferred_ = true;
  return rewind();
}
bool JsonLineReader::rewind()
{
  line_ = 0;
  if (!file_.seek_start()) { error_ = file_.error(); return false; }
  return true;
}
bool JsonLineReader::next(std::vector<Cell> &row, bool &end)
{
  if (!inferred_) { error_ = "JSONL schema must be inferred before scanning"; return false; }
  std::vector<std::pair<std::string, Cell>> fields;
  if (!object(fields, end)) return false;
  row.clear();
  if (end) return true;
  for (const auto &column : columns_) {
    auto field = std::find_if(fields.begin(), fields.end(), [&](const auto &f) { return folded(f.first) == folded(column.name); });
    row.push_back(field == fields.end() ? Cell{"", true} : field->second);
  }
  return true;
}
}}}
