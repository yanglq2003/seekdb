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
#include <algorithm>
#include <cstdlib>
#include <climits>
#include <cerrno>
#include <set>
#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace oceanbase { namespace sql { namespace file_sql {
#ifndef _WIN32
namespace {
class Discovery {
public:
  std::vector<std::string> parts;
  std::set<std::string> matches;
  size_t entries = 0, bytes = 0;
  std::string error;
  bool fail(const std::string &message) { error = "glob: " + message; return false; }
  bool walk(int descriptor, const std::string &path, size_t part, unsigned depth)
  {
    if (depth > 32) return fail("directory depth limit exceeded");
    if (parts[part] == "**" && part + 1 < parts.size() && !walk(descriptor, path, part + 1, depth + 1)) return false;
    int copy = openat(descriptor, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (copy < 0) return fail("cannot reopen authorized directory");
    DIR *directory = fdopendir(copy);
    if (!directory) { close(copy); return fail("cannot enumerate directory"); }
    bool valid = true;
    struct dirent *entry;
    errno = 0;
    while (valid && (entry = readdir(directory))) {
      std::string name(entry->d_name);
      if (name == "." || name == "..") continue;
      if (++entries > 100000) { valid = fail("entry budget exceeded"); break; }
      const std::string &pattern = parts[part];
      if (pattern != "**" && fnmatch(pattern.c_str(), name.c_str(), FNM_PERIOD)) continue;
      struct stat info;
      if (fstatat(descriptor, name.c_str(), &info, AT_SYMLINK_NOFOLLOW)) { valid = fail("directory changed during discovery"); break; }
      std::string child = path == "/" ? path + name : path + "/" + name;
      if (child.size() >= PATH_MAX) { valid = fail("path length limit exceeded"); break; }
      if (S_ISREG(info.st_mode) && part + 1 == parts.size()) {
        if (matches.insert(child).second) bytes += child.size();
        if (bytes > 16 * 1024 * 1024) valid = fail("discovery result budget exceeded");
      } else if (S_ISDIR(info.st_mode) && (part + 1 < parts.size() || pattern == "**")) {
        int nested = openat(descriptor, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (nested < 0) { valid = fail("directory changed during discovery"); break; }
        valid = walk(nested, child, pattern == "**" ? part : part + 1, depth + 1);
        close(nested);
      }
      errno = 0;
    }
    if (valid && errno) valid = fail("directory enumeration failed");
    closedir(directory);
    return valid;
  }
};
}
#endif
bool GlobReader::open(const std::string &pattern, const std::string &root)
{
  paths_.clear(); position_ = 0; error_.clear();
#ifdef _WIN32
  error_ = "glob requires POSIX descriptor support";
  return false;
#else
  auto fail = [&](const std::string &message) { error_ = "glob: " + message; return false; };
  if (root.empty() || root == "NULL" || root == "null" || root.find('\0') != std::string::npos
      || pattern.find('\0') != std::string::npos || pattern.empty()) return fail("secure_file_priv denies access");
  char canonical[PATH_MAX];
  if (!realpath(root.c_str(), canonical)) return fail("cannot resolve secure_file_priv");
  std::string base(canonical), relative(pattern);
  if (pattern[0] == '/') {
    std::string prefix = base == "/" ? "/" : base + "/";
    if (pattern.compare(0, prefix.size(), prefix)) return fail("pattern is outside secure_file_priv");
    relative = pattern.substr(prefix.size());
  }
  Discovery discovery;
  size_t start = 0;
  for (;;) {
    size_t slash = relative.find('/', start);
    std::string part = relative.substr(start, slash == std::string::npos ? slash : slash - start);
    if (part.empty() || part == "." || part == "..") return fail("invalid path component");
    discovery.parts.push_back(part);
    if (discovery.parts.size() > 32) return fail("pattern depth limit exceeded");
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  int descriptor = open_canonical_directory(base);
  if (descriptor < 0) return fail("cannot open secure directory");
  bool valid = discovery.walk(descriptor, base, 0, 0);
  close(descriptor);
  if (!valid) { error_ = discovery.error; return false; }
  paths_.assign(discovery.matches.begin(), discovery.matches.end());
  return true;
#endif
}
bool GlobReader::next(std::vector<Cell> &row, bool &end)
{
  row.clear(); end = position_ == paths_.size();
  if (!end) row.push_back({paths_[position_++], false});
  return true;
}
}}}
