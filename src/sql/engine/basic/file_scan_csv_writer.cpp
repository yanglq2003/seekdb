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
#include "file_scan_path.h"
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdlib>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace oceanbase { namespace sql { namespace file_sql {
AtomicCsvWriter::~AtomicCsvWriter()
{
  if (file_) fclose(file_);
#ifndef _WIN32
  if (directory_ >= 0) {
    if (!temporary_.empty()) unlinkat(directory_, temporary_.c_str(), 0);
    close(directory_);
  }
#endif
}
bool AtomicCsvWriter::fail(const std::string &message)
{
  error_ = "COPY CSV: " + message;
  return false;
}
bool AtomicCsvWriter::open(const std::string &path, const std::string &root, const CsvOptions &options,
                           const std::vector<std::string> &names)
{
  if (directory_ >= 0 || file_ || finished_) return fail("writer is already opened");
  if (root.empty() || root == "NULL" || root == "null" || root.find('\0') != std::string::npos
      || path.empty() || path.find('\0') != std::string::npos) return fail("secure_file_priv denies access");
  if (names.empty() || names.size() > options.max_columns || !options.delimiter || !options.quote || !options.escape
      || options.delimiter == options.quote || options.delimiter == '\n' || options.delimiter == '\r')
    return fail("invalid CSV options");
  if (options.null_text.find_first_of(std::string() + options.delimiter + options.quote + '\n' + '\r') != std::string::npos
      || options.null_text.find('\0') != std::string::npos) return fail("NULL text cannot contain CSV syntax");
#ifdef _WIN32
  return fail("secure CSV writer requires POSIX descriptor support");
#else
  char canonical[PATH_MAX];
  if (!realpath(root.c_str(), canonical)) return fail("cannot resolve secure_file_priv");
  std::string base(canonical), relative(path);
  if (path[0] == '/') {
    std::string prefix = base == "/" ? "/" : base + "/";
    if (path.compare(0, prefix.size(), prefix)) return fail("output is outside secure_file_priv");
    relative = path.substr(prefix.size());
  }
  directory_ = open_canonical_directory(base);
  if (directory_ < 0) return fail("cannot open secure directory");
  size_t start = 0;
  for (;;) {
    size_t slash = relative.find('/', start);
    std::string part = relative.substr(start, slash == std::string::npos ? slash : slash - start);
    if (part.empty() || part == "." || part == "..") return fail("invalid output path component");
    if (slash == std::string::npos) { target_ = part; break; }
    int next = openat(directory_, part.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (next < 0) return fail("cannot open output directory or symbolic link denied");
    close(directory_); directory_ = next; start = slash + 1;
  }
  struct stat info;
  if (!fstatat(directory_, target_.c_str(), &info, AT_SYMLINK_NOFOLLOW) || errno != ENOENT)
    return fail("target exists or cannot be inspected");
  static std::atomic<uint64_t> next_id{0};
  int descriptor = -1;
  for (unsigned attempt = 0; attempt < 1024; ++attempt) {
    temporary_ = ".file-sql-" + std::to_string(getpid()) + "-" + std::to_string(++next_id);
    descriptor = openat(directory_, temporary_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor >= 0 || errno != EEXIST) break;
  }
  if (descriptor < 0) { temporary_.clear(); return fail("cannot create exclusive temporary output"); }
  file_ = fdopen(descriptor, "wb");
  if (!file_) { close(descriptor); return fail("cannot open output stream"); }
  options_ = options; columns_ = names.size();
  if (options.header) {
    std::vector<Cell> header;
    for (const auto &name : names) header.push_back({name, false});
    if (!append(header)) return false;
  }
  return true;
#endif
}
bool AtomicCsvWriter::append(const std::vector<Cell> &row)
{
  if (!file_ || finished_ || !error_.empty()) return fail("output stream is unavailable");
  if (row.size() != columns_) return fail("output column count mismatch");
  std::string line;
  auto push = [&](char c) {
    if (line.size() >= options_.max_record_bytes) return false;
    line.push_back(c); return true;
  };
  for (size_t i = 0; i < row.size(); ++i) {
    if (i && !push(options_.delimiter)) return fail("record budget exceeded");
    const Cell &cell = row[i];
    const std::string &text = cell.is_null ? options_.null_text : cell.text;
    bool quote = !cell.is_null && (text == options_.null_text
        || text.find_first_of(std::string() + options_.delimiter + options_.quote + '\n' + '\r') != std::string::npos);
    if (quote && !push(options_.quote)) return fail("record budget exceeded");
    for (char c : text) {
      if (c == '\0') return fail("NUL byte is not supported in CSV text");
      if (quote && (c == options_.quote || c == options_.escape) && !push(options_.escape)) return fail("record budget exceeded");
      if (!push(c)) return fail("record budget exceeded");
    }
    if (quote && !push(options_.quote)) return fail("record budget exceeded");
  }
  if (!push('\n')) return fail("record budget exceeded");
  if (fwrite(line.data(), 1, line.size(), file_) != line.size()) return fail("output write failed");
  return true;
}
bool AtomicCsvWriter::finish()
{
  if (finished_) return true;
  if (!file_ || !error_.empty()) return fail("cannot publish a failed output");
#ifdef _WIN32
  return fail("secure CSV writer requires POSIX descriptor support");
#else
  if (fflush(file_) || ferror(file_) || fsync(fileno(file_))) return fail("cannot flush output");
  int status = fclose(file_); file_ = nullptr;
  if (status) return fail("cannot close output");
  // linkat creates the final name exclusively, including if a target appeared during the query.
  if (linkat(directory_, temporary_.c_str(), directory_, target_.c_str(), 0)) return fail("target exists or publication failed");
  finished_ = true;
  if (!unlinkat(directory_, temporary_.c_str(), 0)) temporary_.clear();
  return true;
#endif
}
}}}
