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
#include <string>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace oceanbase { namespace sql { namespace file_sql {
// realpath alone is insufficient if a component is replaced before open().
// Pin each component without following links, including the authorized root.
inline int open_canonical_directory(const std::string &path)
{
#ifdef _WIN32
  return -1;
#else
  if (path.empty() || path[0] != '/' || path.find('\0') != std::string::npos) return -1;
  int descriptor = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  for (size_t start = 1; descriptor >= 0 && start < path.size();) {
    size_t slash = path.find('/', start);
    std::string part = path.substr(start, slash == std::string::npos ? slash : slash - start);
    if (part.empty() || part == "." || part == "..") { close(descriptor); return -1; }
    int next = openat(descriptor, part.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    close(descriptor); descriptor = next;
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return descriptor;
#endif
}
}}}
