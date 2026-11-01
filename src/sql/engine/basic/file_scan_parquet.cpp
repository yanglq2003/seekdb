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
#include "file_scan_parquet_codec.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <set>

namespace oceanbase { namespace sql { namespace file_sql {
namespace {
using parquet::Value;
bool hybrid(const std::string &data, size_t &position, unsigned width, size_t count, std::vector<uint32_t> &out)
{
  if (width > 32 || count > 4 * 1024 * 1024) return false;
  out.clear(); out.reserve(count);
  while (out.size() < count) {
    uint64_t header = 0;
    unsigned shift = 0;
    for (;;) {
      if (position >= data.size() || shift >= 64) return false;
      uint8_t byte = data[position++];
      if (shift == 63 && (byte & 0xfe)) return false;
      header |= uint64_t(byte & 127) << shift;
      if (!(byte & 128)) break;
      shift += 7;
    }
    if (!header) return false;
    if (!(header & 1)) {
      size_t bytes = (width + 7) / 8;
      if ((header >> 1) > count - out.size() || bytes > data.size() - position) return false;
      uint32_t value = 0;
      for (size_t i = 0; i < bytes; ++i) value |= uint32_t(uint8_t(data[position++])) << (8 * i);
      if (width < 32 && value >= (uint32_t(1) << width)) return false;
      out.insert(out.end(), header >> 1, value);
    } else {
      uint64_t groups = header >> 1;
      if (groups > (count - out.size() + 7) / 8 || groups > (data.size() - position) / std::max(1u, width)) return false;
      uint64_t values = groups * 8;
      size_t bytes = groups * width;
      if (bytes > data.size() - position) return false;
      for (uint64_t n = 0; n < values && out.size() < count; ++n) {
        uint32_t value = 0;
        for (unsigned bit = 0; bit < width; ++bit) {
          size_t offset = n * width + bit;
          value |= uint32_t((uint8_t(data[position + offset / 8]) >> (offset % 8)) & 1) << bit;
        }
        out.push_back(value);
      }
      position += bytes;
    }
  }
  return true;
}
uint64_t little64(const char *bytes)
{
  return uint64_t(parquet::little32(bytes)) | uint64_t(parquet::little32(bytes + 4)) << 32;
}
bool calendar(int64_t micros, bool date, std::string &text)
{
  int64_t seconds = micros / 1000000, remainder = micros % 1000000;
  if (remainder < 0) { --seconds; remainder += 1000000; }
  time_t time = static_cast<time_t>(seconds);
  if (static_cast<int64_t>(time) != seconds) return false;
  struct tm fields;
#ifdef _WIN32
  if (gmtime_s(&fields, &time)) return false;
#else
  if (!gmtime_r(&time, &fields)) return false;
#endif
  if (fields.tm_year < -1899 || fields.tm_year > 8099) return false;
  char buffer[64];
  if (date) snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", fields.tm_year + 1900, fields.tm_mon + 1, fields.tm_mday);
  else snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%06lld", fields.tm_year + 1900,
                fields.tm_mon + 1, fields.tm_mday, fields.tm_hour, fields.tm_min, fields.tm_sec,
                static_cast<long long>(remainder));
  text = buffer;
  return true;
}
struct Schema {
  int physical = -1;
  int converted = -1;
  int fixed = 0;
  bool optional = false;
  bool timestamp_nanos = false;
  bool null_only = false;
};
bool plain(const Schema &schema, const std::string &data, size_t &position, size_t &boolean_bit, Cell &cell)
{
  cell.is_null = false;
  size_t bytes = schema.physical == 0 ? 0 : schema.physical == 1 || schema.physical == 4 ? 4
                : schema.physical == 2 || schema.physical == 5 ? 8 : schema.physical == 3 ? 12 : 0;
  if (bytes > data.size() - position) return false;
  int64_t integer = 0;
  switch (schema.physical) {
    case 0:
      if (position + boolean_bit / 8 >= data.size()) return false;
      cell.text = (uint8_t(data[position + boolean_bit / 8]) >> (boolean_bit % 8)) & 1 ? "true" : "false";
      ++boolean_bit; return true;
    case 1:
      if (schema.converted == 13) integer = parquet::little32(data.data() + position);
      else integer = static_cast<int32_t>(parquet::little32(data.data() + position));
      break;
    case 2: integer = static_cast<int64_t>(little64(data.data() + position)); break;
    case 3: {
      uint64_t nanos = little64(data.data() + position);
      int64_t days = int64_t(parquet::little32(data.data() + position + 8)) - 2440588;
      if (nanos >= 86400000000000ULL || days < -719162 || days > 2932896) return false;
      position += 12;
      return calendar(days * 86400000000LL + nanos / 1000, false, cell.text);
    }
    case 4: case 5: {
      double number;
      if (schema.physical == 4) {
        uint32_t raw = parquet::little32(data.data() + position);
        float value; memcpy(&value, &raw, 4); number = value;
      } else { uint64_t raw = little64(data.data() + position); memcpy(&number, &raw, 8); }
      if (!std::isfinite(number)) return false;
      std::ostringstream text;
      text.imbue(std::locale::classic());
      text << std::setprecision(std::numeric_limits<double>::max_digits10) << number;
      cell.text = text.str(); position += bytes; return true;
    }
    case 6: case 7: {
      size_t length = schema.fixed;
      if (schema.physical == 6) {
        if (data.size() - position < 4) return false;
        length = parquet::little32(data.data() + position); position += 4;
      }
      if (length > 16 * 1024 * 1024 || length > data.size() - position) return false;
      cell.text.assign(data.data() + position, length); position += length; return true;
    }
    default: return false;
  }
  position += bytes;
  if (schema.converted == 6) {
    if (integer < -719162 || integer > 2932896) return false;
    return calendar(integer * 86400000000LL, true, cell.text);
  }
  if (schema.converted == 9 || schema.converted == 10 || schema.timestamp_nanos) {
    if (schema.timestamp_nanos) { int64_t remainder = integer % 1000; integer /= 1000; if (remainder < 0) --integer; }
    if (schema.converted == 9) {
      if (integer > INT64_MAX / 1000 || integer < INT64_MIN / 1000) return false;
      integer *= 1000;
    }
    return calendar(integer, false, cell.text);
  }
  if (schema.converted == 14) cell.text = std::to_string(static_cast<uint64_t>(integer));
  else cell.text = std::to_string(integer);
  return true;
}
struct Chunk {
  uint64_t offset = 0, end = 0, values = 0;
  int codec = 0;
};
struct Group {
  uint64_t rows = 0;
  std::vector<Chunk> chunks;
};
struct Cursor {
  Chunk chunk;
  uint64_t offset = 0, values = 0;
  std::string data;
  std::vector<uint32_t> definitions, indices;
  std::vector<Cell> dictionary;
  size_t row = 0, value = 0, position = 0, boolean_bit = 0;
  int encoding = 0;
};
}
struct ParquetReader::State {
  CsvReader file;
  std::string error;
  std::vector<Column> columns;
  std::vector<Schema> schema;
  std::vector<Group> groups;
  std::vector<size_t> projection;
  std::vector<Cursor> cursors;
  size_t group = 0;
  uint64_t row = 0, footer = 0;
  bool inferred = false;
  bool fail(const std::string &message) { error = "Parquet: " + message; return false; }
  bool read(uint64_t at, size_t length, std::string &out) {
    if (!file.read_at(at, length, out)) return fail(file.error());
    return true;
  }
  bool load(Cursor &cursor, const Schema &column) {
    while (cursor.offset < cursor.chunk.end) {
      std::string header;
      if (!read(cursor.offset, std::min<uint64_t>(65536, cursor.chunk.end - cursor.offset), header)) return false;
      Value page;
      parquet::Compact protocol(header);
      if (!protocol.structure(page) || page.field(1).type != 5 || page.field(2).type != 5 || page.field(3).type != 5) return fail("invalid page header");
      int64_t compressed = page.field(3).integer, uncompressed = page.field(2).integer;
      uint64_t payload = cursor.offset + protocol.position();
      if (compressed < 0 || uncompressed < 0 || compressed > 64 * 1024 * 1024 || uncompressed > 64 * 1024 * 1024
          || payload > cursor.chunk.end || uint64_t(compressed) > cursor.chunk.end - payload) return fail("invalid page size");
      cursor.offset = payload + compressed;
      int type = page.field(1).integer;
      if (type == 1) continue; // Optional index pages do not contain values.
      if (type != 0 && type != 2 && type != 3) return fail("unsupported page kind");
      std::string raw;
      if (!read(payload, compressed, raw)) return false;
      bool v2 = type == 3;
      size_t prefix = 0;
      if (v2) {
        const Value &meta = page.field(8);
        if (!page.has(8) || !meta.has(5) || !meta.has(6) || meta.field(5).integer < 0 || meta.field(6).integer != 0)
          return fail("invalid flat v2 level stream");
        prefix = meta.field(5).integer;
        if (prefix > raw.size() || prefix > uint64_t(uncompressed)) return fail("invalid v2 prefix");
      }
      bool compressed_values = !v2 || !page.field(8).has(7) || page.field(8).field(7).integer;
      if (cursor.chunk.codec == 0 || !compressed_values) {
        if (raw.size() != uint64_t(uncompressed)) return fail("uncompressed size mismatch");
        cursor.data = std::move(raw);
      } else if (cursor.chunk.codec == 1) {
        std::string decoded;
        if (!parquet::snappy(raw.substr(prefix), uncompressed - prefix, decoded)) return fail("invalid Snappy block");
        cursor.data = raw.substr(0, prefix) + decoded;
      } else return fail("unsupported compression codec");
      if (type == 2) {
        const Value &meta = page.field(7);
        if (!page.has(7) || !meta.has(1) || !meta.has(2) || meta.field(2).integer != 0
            || meta.field(1).integer < 0 || meta.field(1).integer > 1000000 || !cursor.dictionary.empty())
          return fail("invalid dictionary page");
        size_t position = 0, bit = 0, total = 0;
        for (int64_t i = 0; i < meta.field(1).integer; ++i) {
          Cell cell;
          if (!plain(column, cursor.data, position, bit, cell)) return fail("invalid dictionary value");
          total += cell.text.size();
          if (total > 16 * 1024 * 1024) return fail("dictionary memory budget exceeded");
          cursor.dictionary.push_back(std::move(cell));
        }
        continue;
      }
      const Value &meta = page.field(v2 ? 8 : 5);
      if (!page.has(v2 ? 8 : 5) || !meta.has(1) || !meta.has(v2 ? 4 : 2)
          || meta.field(1).integer <= 0 || meta.field(1).integer > 4 * 1024 * 1024) return fail("invalid data page");
      size_t count = meta.field(1).integer;
      if (count > cursor.chunk.values - cursor.values) return fail("page exceeds declared row count");
      cursor.values += count;
      cursor.encoding = meta.field(v2 ? 4 : 2).integer;
      cursor.position = 0; cursor.row = 0; cursor.value = 0; cursor.boolean_bit = 0;
      cursor.indices.clear(); cursor.definitions.clear();
      if (column.optional) {
        size_t bytes = prefix;
        if (!v2) {
          if (!meta.has(3) || meta.field(3).integer != 3 || cursor.data.size() < 4) return fail("unsupported definition encoding");
          bytes = parquet::little32(cursor.data.data()); cursor.position = 4;
        }
        if (bytes > cursor.data.size() - cursor.position) return fail("truncated definition levels");
        std::string levels = cursor.data.substr(cursor.position, bytes);
        size_t position = 0;
        if (!hybrid(levels, position, 1, count, cursor.definitions)) return fail("invalid definition levels");
        cursor.position += bytes;
      } else cursor.definitions.assign(count, 1);
      size_t values = std::count(cursor.definitions.begin(), cursor.definitions.end(), 1);
      if (v2 && (!meta.has(2) || meta.field(2).integer != static_cast<int64_t>(count - values)))
        return fail("v2 null count mismatch");
      if (cursor.encoding == 2 || cursor.encoding == 8) {
        if (values != 0) {
          if (cursor.dictionary.empty() || cursor.position >= cursor.data.size()) return fail("missing dictionary");
          unsigned width = uint8_t(cursor.data[cursor.position++]);
          if (!hybrid(cursor.data, cursor.position, width, values, cursor.indices)) return fail("invalid dictionary indices");
        }
        for (uint32_t index : cursor.indices) if (index >= cursor.dictionary.size()) return fail("dictionary index out of bounds");
      } else if (cursor.encoding == 3 && column.physical == 0) {
        if (cursor.data.size() - cursor.position < 4) return fail("truncated RLE boolean stream");
        size_t length = parquet::little32(cursor.data.data() + cursor.position);
        cursor.position += 4;
        if (length > cursor.data.size() - cursor.position) return fail("invalid RLE boolean length");
        std::string booleans = cursor.data.substr(cursor.position, length);
        size_t position = 0;
        if (!hybrid(booleans, position, 1, values, cursor.indices)) return fail("invalid RLE booleans");
        cursor.dictionary = {Cell{"false", false}, Cell{"true", false}};
      } else if (cursor.encoding != 0) return fail("unsupported data encoding");
      size_t memory = 0;
      for (const auto &current : cursors) {
        memory += current.data.capacity() + current.definitions.capacity() * sizeof(uint32_t)
                  + current.indices.capacity() * sizeof(uint32_t);
        for (const auto &cell : current.dictionary) memory += sizeof(Cell) + cell.text.capacity();
      }
      if (memory > 128 * 1024 * 1024) return fail("scan memory budget exceeded");
      return true;
    }
    return fail("column ended before row group");
  }
};
ParquetReader::ParquetReader() : state_(new State) {}
ParquetReader::~ParquetReader() { delete state_; }
const std::string &ParquetReader::identity() const { return state_->file.identity(); }
const std::vector<Column> &ParquetReader::columns() const { return state_->columns; }
const std::string &ParquetReader::error() const { return state_->error; }
bool ParquetReader::open(const std::string &path, const std::string &root)
{
  delete state_; state_ = new State;
  if (!state_->file.open(path, root, {})) return state_->fail(state_->file.error());
  return true;
}
bool ParquetReader::infer()
{
  auto &s = *state_;
  uint64_t size = 0;
  if (!s.file.size(size)) return s.fail(s.file.error());
  std::string magic, trailer, footer;
  if (size < 12 || !s.read(0, 4, magic) || magic != "PAR1" || !s.read(size - 8, 8, trailer)
      || trailer.substr(4) != "PAR1") return s.fail("invalid magic or truncated footer");
  uint32_t bytes = parquet::little32(trailer.data());
  if (bytes > 16 * 1024 * 1024 || bytes > size - 12) return s.fail("footer exceeds bounds or metadata budget");
  s.footer = size - 8 - bytes;
  if (!s.read(s.footer, bytes, footer)) return false;
  Value metadata;
  parquet::Compact protocol(footer);
  if (!protocol.structure(metadata) || protocol.position() != footer.size() || !metadata.has(2)
      || metadata.field(2).type != 9 || metadata.field(3).type != 6 || metadata.field(4).type != 9) return s.fail("invalid footer metadata");
  const auto &schema = metadata.field(2).list;
  if (schema.size() < 2 || schema.size() > 4097 || !schema[0].has(5)
      || schema[0].field(5).integer != static_cast<int64_t>(schema.size() - 1)) return s.fail("only flat schemas are supported");
  s.columns.clear(); s.schema.clear(); s.groups.clear();
  std::set<std::string> names;
  for (size_t i = 1; i < schema.size(); ++i) {
    const auto &element = schema[i];
    if (element.field(1).type != 5 || element.field(3).type != 5 || element.field(4).type != 8 || element.has(5)
        || element.field(3).integer < 0 || element.field(3).integer > 1) return s.fail("nested or repeated column");
    Schema column;
    column.physical = element.field(1).integer;
    column.optional = element.field(3).integer == 1;
    column.fixed = element.field(2).integer;
    column.converted = element.has(6) ? element.field(6).integer : -1;
    if (element.has(10)) {
      const Value &logical = element.field(10);
      if (logical.has(6)) column.converted = 6;
      if (logical.has(11)) column.null_only = true;
      if (logical.has(8)) {
        const auto &unit = logical.field(8).field(2);
        if (unit.has(1)) column.converted = 9;
        else if (unit.has(2)) column.converted = 10;
        else if (unit.has(3)) column.timestamp_nanos = true;
        else return s.fail("unsupported timestamp unit");
      }
    }
    if (column.physical < 0 || column.physical > 7 || column.converted == 1 || column.converted == 2
        || column.converted == 3 || column.converted == 5 || (column.physical == 7 && (column.fixed <= 0 || column.fixed > 16777216)))
      return s.fail("unsupported physical or logical type");
    Type type = column.physical == 0 ? Type::boolean : column.physical == 1 || column.physical == 2 ? Type::bigint
                : column.physical == 3 ? Type::datetime : column.physical == 4 || column.physical == 5 ? Type::real : Type::varchar;
    if (column.converted == 6) type = Type::date;
    if (column.converted == 9 || column.converted == 10 || column.timestamp_nanos) type = Type::datetime;
    if (column.null_only) type = Type::null_value;
    if (column.converted == 14) type = Type::varchar;
    std::string name = element.field(4).bytes, key = name;
    for (char &c : key) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (name.empty() || name.find('\0') != std::string::npos || !names.insert(key).second) return s.fail("invalid column name");
    unsigned scale = type == Type::datetime ? (column.converted == 9 ? 3 : 6) : 0;
    s.columns.push_back({name, type, column.optional, scale}); s.schema.push_back(column);
  }
  uint64_t total_rows = 0;
  size_t chunks = 0;
  for (const Value &group : metadata.field(4).list) {
    if (group.field(1).type != 9 || group.field(3).type != 6 || group.field(3).integer < 0 || group.field(1).list.size() != s.columns.size())
      return s.fail("invalid row group");
    Group parsed; parsed.rows = group.field(3).integer;
    if (parsed.rows > UINT64_MAX - total_rows) return s.fail("row count overflow");
    total_rows += parsed.rows;
    for (size_t i = 0; i < s.columns.size(); ++i) {
      if (++chunks > 100000) return s.fail("column chunk metadata budget exceeded");
      const Value &column = group.field(1).list[i];
      const Value &meta = column.field(3);
      if (column.field(3).type != 12 || (column.has(1) && !column.field(1).bytes.empty()) || meta.field(1).type != 5
          || meta.field(3).type != 9 || meta.field(4).type != 5 || meta.field(5).type != 6
          || meta.field(7).type != 6 || meta.field(9).type != 6 || (meta.has(11) && meta.field(11).type != 6)
          || meta.field(1).integer != s.schema[i].physical || meta.field(3).list.size() != 1
          || meta.field(3).list[0].bytes != s.columns[i].name || meta.field(5).integer < 0
          || uint64_t(meta.field(5).integer) != parsed.rows || meta.field(7).integer < 0)
        return s.fail("invalid or external column chunk");
      int64_t offset = meta.field(9).integer;
      if (meta.has(11)) offset = std::min(offset, meta.field(11).integer);
      if (offset < 4 || uint64_t(offset) > s.footer || uint64_t(meta.field(7).integer) > s.footer - offset)
        return s.fail("column chunk outside file bounds");
      parsed.chunks.push_back({uint64_t(offset), uint64_t(offset) + uint64_t(meta.field(7).integer), parsed.rows, int(meta.field(4).integer)});
    }
    s.groups.push_back(std::move(parsed));
  }
  if (metadata.field(3).integer < 0 || uint64_t(metadata.field(3).integer) != total_rows) return s.fail("file row count mismatch");
  s.projection.clear();
  for (size_t i = 0; i < s.columns.size(); ++i) s.projection.push_back(i);
  s.inferred = true;
  return rewind();
}
bool ParquetReader::project(const std::vector<size_t> &columns)
{
  auto &s = *state_;
  if (!s.inferred) return s.fail("schema is not inferred");
  std::set<size_t> distinct;
  for (size_t column : columns) if (column >= s.columns.size() || !distinct.insert(column).second) return s.fail("invalid projection");
  s.projection = columns;
  return rewind();
}
bool ParquetReader::rewind()
{
  auto &s = *state_;
  if (!s.file.unchanged()) return s.fail(s.file.error());
  s.group = 0; s.row = 0; s.cursors.clear();
  return true;
}
bool ParquetReader::next(std::vector<Cell> &row, bool &end)
{
  auto &s = *state_;
  if (!s.inferred || !s.file.unchanged()) return s.fail(s.file.error());
  while (s.group < s.groups.size() && s.row >= s.groups[s.group].rows) {
    for (const auto &cursor : s.cursors) {
      if (cursor.values != cursor.chunk.values || cursor.row != cursor.definitions.size())
        return s.fail("column page count differs from footer");
    }
    ++s.group; s.row = 0; s.cursors.clear();
  }
  end = s.group == s.groups.size();
  row.clear();
  if (end) return true;
  if (s.cursors.empty()) {
    s.cursors.resize(s.projection.size());
    for (size_t i = 0; i < s.projection.size(); ++i) {
      s.cursors[i].chunk = s.groups[s.group].chunks[s.projection[i]];
      s.cursors[i].offset = s.cursors[i].chunk.offset;
    }
  }
  row.assign(s.columns.size(), Cell{"", true});
  for (size_t i = 0; i < s.projection.size(); ++i) {
    size_t column = s.projection[i];
    auto &cursor = s.cursors[i];
    if (cursor.row == cursor.definitions.size() && !s.load(cursor, s.schema[column])) return false;
    if (cursor.definitions[cursor.row++]) {
      if (s.schema[column].null_only) return s.fail("non-null value for NULL logical type");
      if (cursor.encoding == 0) {
        if (!plain(s.schema[column], cursor.data, cursor.position, cursor.boolean_bit, row[column])) return s.fail("invalid plain value");
      } else {
        if (cursor.value >= cursor.indices.size()) return s.fail("missing dictionary index");
        row[column] = cursor.dictionary[cursor.indices[cursor.value++]];
      }
    }
  }
  ++s.row;
  return true;
}
}}}
