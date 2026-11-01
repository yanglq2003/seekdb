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
#include "file_scan_reader.h"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace oceanbase { namespace sql { namespace file_sql {
// This descriptor is a plan constant, never a table containing file rows.
struct Config {
  std::string format;
  std::string path;
  std::string identity;
  CsvOptions options;
  std::vector<Column> columns;
  std::string encode() const
  {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("format"); writer.String(format.data(), format.size());
    writer.Key("identity"); writer.String(identity.data(), identity.size());
    writer.Key("path"); writer.String(path.data(), path.size());
    writer.Key("header"); writer.Bool(options.header);
    writer.Key("delimiter"); writer.Uint(static_cast<unsigned char>(options.delimiter));
    writer.Key("quote"); writer.Uint(static_cast<unsigned char>(options.quote));
    writer.Key("escape"); writer.Uint(static_cast<unsigned char>(options.escape));
    writer.Key("null"); writer.String(options.null_text.data(), options.null_text.size());
    writer.Key("columns"); writer.StartArray();
    for (const auto &column : columns) {
      writer.StartArray(); writer.String(column.name.data(), column.name.size());
      writer.Uint(static_cast<unsigned>(column.type)); writer.Bool(column.nullable); writer.Uint(column.scale); writer.EndArray();
    }
    writer.EndArray(); writer.EndObject();
    return std::string("file_sql:v1:") + std::string(buffer.GetString(), buffer.GetSize());
  }
  bool decode(const std::string &text)
  {
    if (text.compare(0, 12, "file_sql:v1:") || text.size() > 1024 * 1024) return false;
    rapidjson::Document d;
    d.Parse<rapidjson::kParseValidateEncodingFlag>(text.data() + 12, text.size() - 12);
    if (d.HasParseError() || !d.IsObject()) return false;
    for (const char *key : {"format", "path", "null", "identity"}) if (!d.HasMember(key) || !d[key].IsString()) return false;
    if (!d.HasMember("header") || !d["header"].IsBool() || !d.HasMember("columns")
        || !d["columns"].IsArray() || d["columns"].Size() > 4096) return false;
    for (const char *key : {"delimiter", "quote", "escape"})
      if (!d.HasMember(key) || !d[key].IsUint() || d[key].GetUint() > 255 || !d[key].GetUint()) return false;
    format.assign(d["format"].GetString(), d["format"].GetStringLength());
    identity.assign(d["identity"].GetString(), d["identity"].GetStringLength());
    path.assign(d["path"].GetString(), d["path"].GetStringLength());
    options.header = d["header"].GetBool(); options.delimiter = d["delimiter"].GetUint();
    options.quote = d["quote"].GetUint(); options.escape = d["escape"].GetUint();
    options.null_text.assign(d["null"].GetString(), d["null"].GetStringLength());
    columns.clear();
    for (auto &c : d["columns"].GetArray()) {
      if (!c.IsArray() || (c.Size() != 3 && c.Size() != 4) || !c[0].IsString() || !c[1].IsUint() || !c[2].IsBool()
          || c[1].GetUint() > static_cast<unsigned>(Type::datetime)) return false;
      if (c.Size() == 4 && (!c[3].IsUint() || c[3].GetUint() > 6)) return false;
      columns.push_back({std::string(c[0].GetString(), c[0].GetStringLength()), static_cast<Type>(c[1].GetUint()), c[2].GetBool(), c.Size() == 4 ? c[3].GetUint() : 0});
    }
    return !columns.empty();
  }
};
}}}
