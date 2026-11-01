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
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace oceanbase { namespace sql { namespace file_sql { namespace parquet {
// Bounded Thrift Compact values used for footer and page metadata only.
// Wire definitions: apache/parquet-format parquet.thrift and Apache Thrift Compact protocol.
struct Value {
  unsigned type = 0;
  int64_t integer = 0;
  std::string bytes;
  std::vector<Value> list;
  std::map<int, Value> fields;
  const Value &field(int id) const {
    static const Value empty;
    auto it = fields.find(id);
    return it == fields.end() ? empty : it->second;
  }
  bool has(int id) const { return fields.find(id) != fields.end(); }
};
class Compact {
public:
  explicit Compact(const std::string &input) : input_(input) {}
  bool structure(Value &value, unsigned depth = 0) {
    if (depth > 32 || ++values_ > 100000) return false;
    value.type = 12;
    int previous = 0;
    while (position_ < input_.size()) {
      uint8_t header = byte();
      if (!header) return true;
      int id = previous + (header >> 4);
      if (!(header >> 4)) {
        uint64_t encoded;
        if (!varint(encoded)) return false;
        int64_t decoded = unzigzag(encoded);
        if (decoded <= 0 || decoded > 32767) return false;
        id = decoded;
      }
      if (id <= 0 || id > 32767 || value.fields.count(id)) return false;
      previous = id;
      Value child;
      if (!read(header & 15, child, depth + 1, true)) return false;
      value.fields.emplace(id, std::move(child));
    }
    return false;
  }
  size_t position() const { return position_; }
  bool varint(uint64_t &result) {
    result = 0;
    for (unsigned shift = 0; shift < 64 && position_ < input_.size(); shift += 7) {
      uint8_t part = byte();
      if (shift == 63 && (part & 0xfe)) return false;
      result |= uint64_t(part & 127) << shift;
      if (!(part & 128)) return true;
    }
    return false;
  }
private:
  static int64_t unzigzag(uint64_t value) { return static_cast<int64_t>(value >> 1) ^ -static_cast<int64_t>(value & 1); }
  uint8_t byte() { return static_cast<uint8_t>(input_[position_++]); }
  bool read(unsigned type, Value &value, unsigned depth, bool field = false) {
    if (depth > 32 || ++values_ > 100000) return false;
    value.type = type;
    uint64_t number = 0;
    switch (type) {
      case 1: case 2:
        if (!field) {
          if (position_ >= input_.size()) return false;
          unsigned boolean = byte();
          if (boolean != 1 && boolean != 2) return false;
          value.integer = boolean == 1;
        } else value.integer = type == 1;
        return true;
      case 3:
        if (position_ >= input_.size()) return false;
        value.integer = static_cast<int8_t>(byte()); return true;
      case 4: case 5: case 6:
        if (!varint(number)) return false;
        value.integer = unzigzag(number); return true;
      case 7:
        if (input_.size() - position_ < 8) return false;
        position_ += 8; return true;
      case 8:
        if (!varint(number) || number > input_.size() - position_) return false;
        value.bytes.assign(input_.data() + position_, number); position_ += number; return true;
      case 9: case 10: {
        if (position_ >= input_.size()) return false;
        unsigned header = byte(), element = header & 15;
        number = header >> 4;
        if (number == 15 && !varint(number)) return false;
        if (number > 100000 - values_) return false;
        for (uint64_t i = 0; i < number; ++i) {
          Value child;
          if (!read(element, child, depth + 1)) return false;
          value.list.push_back(std::move(child));
        }
        return true;
      }
      case 11: {
        if (!varint(number) || number > (100000 - values_) / 2) return false;
        if (!number) return true;
        if (position_ >= input_.size()) return false;
        unsigned types = byte();
        for (uint64_t i = 0; i < number; ++i) {
          Value key, mapped;
          if (!read(types >> 4, key, depth + 1) || !read(types & 15, mapped, depth + 1)) return false;
        }
        return true;
      }
      case 12: return structure(value, depth + 1);
      default: return false;
    }
  }
  const std::string &input_;
  size_t position_ = 0;
  size_t values_ = 0;
};
inline uint32_t little32(const char *bytes) {
  return uint32_t(uint8_t(bytes[0])) | uint32_t(uint8_t(bytes[1])) << 8
       | uint32_t(uint8_t(bytes[2])) << 16 | uint32_t(uint8_t(bytes[3])) << 24;
}
// Snappy raw blocks, not Snappy framed streams. Allocation is capped before decoding.
inline bool snappy(const std::string &source, size_t expected, std::string &output) {
  if (expected > 64 * 1024 * 1024) return false;
  Compact length(source);
  uint64_t size = 0;
  if (!length.varint(size) || size != expected) return false;
  size_t position = length.position();
  output.clear(); output.reserve(expected);
  while (position < source.size()) {
    uint8_t tag = source[position++];
    size_t count = 0, offset = 0;
    if ((tag & 3) == 0) {
      count = tag >> 2;
      if (count < 60) ++count;
      else {
        unsigned bytes = count - 59;
        if (source.size() - position < bytes) return false;
        uint64_t encoded = 0;
        for (unsigned i = 0; i < bytes; ++i) encoded |= uint64_t(uint8_t(source[position++])) << (8 * i);
        count = encoded + 1;
      }
      if (count > expected - output.size() || count > source.size() - position) return false;
      output.append(source.data() + position, count); position += count;
    } else {
      unsigned bytes = (tag & 3) == 1 ? 1 : (tag & 3) == 2 ? 2 : 4;
      if (source.size() - position < bytes) return false;
      count = (tag & 3) == 1 ? 4 + ((tag >> 2) & 7) : 1 + (tag >> 2);
      for (unsigned i = 0; i < bytes; ++i) offset |= size_t(uint8_t(source[position++])) << (8 * i);
      if ((tag & 3) == 1) offset |= size_t(tag & 0xe0) << 3;
      if (!offset || offset > output.size() || count > expected - output.size()) return false;
      for (size_t i = 0; i < count; ++i) output.push_back(output[output.size() - offset]);
    }
  }
  return output.size() == expected;
}
}}}}
