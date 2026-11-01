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
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <climits>
#include <set>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace oceanbase { namespace sql { namespace file_sql {
namespace {
std::string lower(std::string text)
{
  for (char &c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return text;
}
bool utf8(const std::string &s)
{
  for (size_t i = 0; i < s.size();) {
    unsigned char c = s[i++];
    if (c < 128) { if (c == 0) return false; continue; }
    unsigned count = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
    if (!count || i + count > s.size()) return false;
    unsigned value = c & ((1u << (6 - count)) - 1);
    for (unsigned n = 0; n < count; ++n) {
      unsigned char next = s[i++];
      if ((next & 0xc0) != 0x80) return false;
      value = (value << 6) | (next & 63);
    }
    if ((count == 1 && value < 128) || (count == 2 && value < 2048)
        || (count == 3 && value < 65536) || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
  }
  return true;
}
#ifndef _WIN32
std::string identity(FILE *file)
{
  struct stat s;
  if (fstat(fileno(file), &s) || !S_ISREG(s.st_mode)) return {};
  return std::to_string(s.st_dev) + ":" + std::to_string(s.st_ino) + ":" + std::to_string(s.st_size)
       + ":" + std::to_string(s.st_mtim.tv_sec) + ":" + std::to_string(s.st_mtim.tv_nsec)
       + ":" + std::to_string(s.st_ctim.tv_sec) + ":" + std::to_string(s.st_ctim.tv_nsec);
}
#endif
}
CsvReader::~CsvReader() { if (file_) fclose(file_); }
bool CsvReader::fail(const std::string &message)
{
  error_ = "CSV line " + std::to_string(line_) + ": " + message;
  return false;
}
bool CsvReader::open(const std::string &path, const std::string &root, const CsvOptions &options)
{
  if (file_) { fclose(file_); file_ = nullptr; }
  inferred_ = false; columns_.clear(); error_.clear(); line_ = 1; options_ = options;
  if (path.find('\0') != std::string::npos || root.find('\0') != std::string::npos || root.empty()
      || lower(root) == "null") return fail("secure_file_priv denies access");
  if (!options.delimiter || !options.quote || !options.escape || options.delimiter == options.quote
      || options.delimiter == '\n' || options.delimiter == '\r' || !options.max_columns
      || !options.max_record_bytes) return fail("invalid CSV options");
#ifdef _WIN32
  return fail("secure file scanner requires POSIX descriptor support");
#else
  char canonical_root[PATH_MAX];
  if (!realpath(root.c_str(), canonical_root)) return fail("cannot resolve secure_file_priv");
  std::string base(canonical_root), relative(path);
  if (!path.empty() && path[0] == '/') {
    std::string prefix = base == "/" ? "/" : base + "/";
    if (path.compare(0, prefix.size(), prefix)) return fail("path is outside secure_file_priv");
    relative = path.substr(prefix.size());
  }
  int descriptor = ::open(base.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (descriptor < 0) return fail("cannot open secure directory");
  size_t start = 0;
  while (descriptor >= 0) {
    size_t slash = relative.find('/', start);
    std::string part = relative.substr(start, slash == std::string::npos ? slash : slash - start);
    if (part.empty() || part == ".." || part == ".") { close(descriptor); return fail("invalid path component"); }
    int next = openat(descriptor, part.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK
                      | (slash == std::string::npos ? 0 : O_DIRECTORY));
    close(descriptor); descriptor = next;
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  if (descriptor < 0) return fail("cannot open file or symbolic link denied");
  file_ = fdopen(descriptor, "rb");
  if (!file_) { close(descriptor); return fail("cannot create file stream"); }
  identity_ = identity(file_);
  if (identity_.empty()) { fclose(file_); file_ = nullptr; return fail("input must be a regular file"); }
  return true;
#endif
}
bool CsvReader::unchanged()
{
#ifdef _WIN32
  return false;
#else
  return file_ && identity(file_) == identity_ ? true : fail("file changed during query");
#endif
}
bool CsvReader::record(std::vector<Cell> &row, bool &end)
{
  row.clear(); end = false;
  std::string field;
  bool quoted = false, inside = false, closed = false, seen = false;
  size_t bytes = 0;
  auto append = [&]() {
    if (!utf8(field)) return fail("invalid UTF-8");
    if (row.size() >= options_.max_columns) return fail("column limit exceeded");
    row.push_back({field, !quoted && field == options_.null_text});
    field.clear(); quoted = false; closed = false;
    return true;
  };
  for (;;) {
    int c = fgetc(file_);
    if (c == EOF) {
      if (ferror(file_)) return fail("read failed");
      if (inside) return fail("unterminated quoted field");
      if (!seen) { end = true; return unchanged(); }
      return append() && unchanged();
    }
    seen = true;
    if (++bytes > options_.max_record_bytes) return fail("record size limit exceeded");
    if (inside) {
      if (c == options_.escape && options_.escape != options_.quote) {
        int escaped = fgetc(file_);
        if (escaped == EOF || ++bytes > options_.max_record_bytes) return fail("invalid escape");
        if (escaped == '\n') ++line_;
        field.push_back(static_cast<char>(escaped));
      } else if (c == options_.quote) {
        int following = fgetc(file_);
        if (following == options_.quote && options_.escape == options_.quote) {
          field.push_back(options_.quote);
          if (++bytes > options_.max_record_bytes) return fail("record size limit exceeded");
        } else { if (following != EOF) ungetc(following, file_); inside = false; closed = true; }
      } else {
        if (c == '\n') ++line_;
        // Normalize CRLF inside quoted fields to the same newline used outside quotes.
        if (c == '\r') { int n = fgetc(file_); if (n != '\n' && n != EOF) ungetc(n, file_); c = '\n'; }
        field.push_back(static_cast<char>(c));
      }
    } else if (c == options_.delimiter) {
      if (!append()) return false;
    } else if (c == '\n' || c == '\r') {
      if (c == '\r') { int n = fgetc(file_); if (n != '\n' && n != EOF) ungetc(n, file_); }
      ++line_; return append();
    } else if (c == options_.quote && field.empty() && !closed) {
      quoted = true; inside = true;
    } else if (closed || c == options_.quote) return fail("unexpected character after quote or quote in unquoted field");
    else field.push_back(static_cast<char>(c));
  }
}
Type CsvReader::infer_type(const std::string &s)
{
  if (lower(s) == "true" || lower(s) == "false") return Type::boolean;
  int64_t integer = 0;
  auto parsed = std::from_chars(s.data(), s.data() + s.size(), integer);
  if (parsed.ec == std::errc() && parsed.ptr == s.data() + s.size()) return Type::bigint;
  // Overflowing integer literals remain strings instead of losing integer precision.
  if (s.find_first_of(".eE") != std::string::npos && !s.empty() && s.find_first_not_of("0123456789+-.eE") == std::string::npos) {
    char *end = nullptr;
    double value = strtod(s.c_str(), &end);
    if (end == s.c_str() + s.size() && std::isfinite(value)) return Type::real;
  }
  auto digits = [&](size_t at, size_t count) {
    int value = 0;
    for (size_t i = at; i < at + count; ++i) {
      if (i >= s.size() || s[i] < '0' || s[i] > '9') return -1;
      value = value * 10 + s[i] - '0';
    }
    return value;
  };
  if (s.size() == 10 || s.size() == 19) {
    int year = digits(0, 4), month = digits(5, 2), day = digits(8, 2);
    const int days[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (s[4] == '-' && s[7] == '-' && year > 0 && month >= 1 && month <= 12
        && day >= 1 && day <= days[month] + (month == 2 && leap)) {
      if (s.size() == 10) return Type::date;
      int hour = digits(11, 2), minute = digits(14, 2), second = digits(17, 2);
      if ((s[10] == ' ' || s[10] == 'T') && s[13] == ':' && s[16] == ':'
          && hour >= 0 && hour < 24 && minute >= 0 && minute < 60 && second >= 0 && second < 60)
        return Type::datetime;
    }
  }
  return Type::varchar;
}
Type CsvReader::widen(Type a, Type b)
{
  if (a == Type::null_value) return b;
  if (b == Type::null_value || a == b) return a;
  if ((a == Type::bigint && b == Type::real) || (b == Type::bigint && a == Type::real)) return Type::real;
  if ((a == Type::date && b == Type::datetime) || (b == Type::date && a == Type::datetime)) return Type::datetime;
  return Type::varchar;
}
bool CsvReader::infer()
{
  if (!file_ || !unchanged()) return false;
  if (fseek(file_, 0, SEEK_SET)) return fail("cannot rewind");
  columns_.clear(); line_ = 1; inferred_ = false;
  std::vector<Cell> row; bool end = false;
  if (!record(row, end)) return false;
  if (end) return fail("empty file has no schema");
  if (row[0].text.compare(0, 3, "\xef\xbb\xbf") == 0) row[0].text.erase(0, 3);
  std::set<std::string> names;
  for (size_t i = 0; i < row.size(); ++i) {
    std::string name = options_.header && !row[i].text.empty() ? row[i].text : "column" + std::to_string(i + 1);
    std::string unique = name;
    unsigned suffix = 2;
    while (!names.insert(lower(unique)).second) unique = name + "__" + std::to_string(suffix++);
    columns_.push_back({unique, Type::null_value, false});
  }
  if (!options_.header) {
    for (size_t i = 0; i < row.size(); ++i) {
      columns_[i].nullable = row[i].is_null;
      columns_[i].type = row[i].is_null ? Type::null_value : infer_type(row[i].text);
    }
  }
  while (record(row, end) && !end) {
    if (row.size() != columns_.size()) return fail("field count differs from schema");
    for (size_t i = 0; i < row.size(); ++i) {
      columns_[i].nullable |= row[i].is_null;
      columns_[i].type = widen(columns_[i].type, row[i].is_null ? Type::null_value : infer_type(row[i].text));
    }
  }
  if (!error_.empty()) return false;
  inferred_ = true;
  return rewind();
}
bool CsvReader::rewind()
{
  if (!file_ || !unchanged() || fseek(file_, 0, SEEK_SET)) return fail("cannot rescan changed file");
  line_ = 1;
  if (options_.header) { std::vector<Cell> header; bool end; return record(header, end); }
  return true;
}
bool CsvReader::next(std::vector<Cell> &row, bool &end)
{
  if (!inferred_) return fail("schema must be inferred before scanning");
  if (!unchanged() || !record(row, end)) return false;
  return end || row.size() == columns_.size() ? true : fail("field count differs from schema");
}
}}}
