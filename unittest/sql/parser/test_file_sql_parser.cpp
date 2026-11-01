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
#include <gtest/gtest.h>
#include "sql/parser/ob_parser.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace {
const ParseNode *find_node(const ParseNode *node, ObItemType type)
{
  if (node == nullptr || node->type_ == type) return node;
  for (int i = 0; i < node->num_child_; ++i) {
    if (const ParseNode *found = find_node(node->children_[i], type)) return found;
  }
  return nullptr;
}

class FileSqlParser : public testing::Test {
protected:
  ObArenaAllocator allocator_;
  ParseResult result_{};
  int parse(const char *query)
  {
    result_ = {};
    ObParser parser(allocator_, 0);
    return parser.parse(ObString::make_string(query), result_);
  }
};

TEST_F(FileSqlParser, file_functions_aliases_and_named_options)
{
  for (const char *query : {
      "select * from read_csv('/tmp/input.csv')",
      "select * from read_csv('/tmp/input.csv',header=true,sample_size=-1) as f",
      "select j.id from read_json('/tmp/input.jsonl',format='newline_delimited') j",
      "select * from read_ndjson('/tmp/input.jsonl')",
      "select c1 from read_parquet('/tmp/input.parquet') p",
      "select file from glob('/tmp/*.csv')"}) {
    SCOPED_TRACE(query);
    ASSERT_EQ(OB_SUCCESS, parse(query));
    const ParseNode *file = find_node(result_.result_tree_, T_FILE_TABLE_EXPRESSION);
    ASSERT_NE(nullptr, file);
    ASSERT_EQ(3, file->num_child_);
    ASSERT_NE(nullptr, file->children_[1]);
    EXPECT_EQ(T_EXPR_LIST, file->children_[1]->type_);
    EXPECT_EQ(T_VARCHAR, file->children_[1]->children_[0]->type_);
  }
  ASSERT_EQ(OB_SUCCESS, parse("select * from read_csv('/tmp/a',header=true,sample_size=-1) f"));
  const ParseNode *file = find_node(result_.result_tree_, T_FILE_TABLE_EXPRESSION);
  ASSERT_NE(nullptr, file);
  ASSERT_EQ(3, file->children_[1]->num_child_);
  EXPECT_EQ(T_OP_EQ, file->children_[1]->children_[1]->type_);
  ASSERT_NE(nullptr, file->children_[2]);
}

TEST_F(FileSqlParser, describe_copy_and_ordinary_sql)
{
  ASSERT_EQ(OB_SUCCESS, parse("describe select * from read_csv('/tmp/a.csv')"));
  const ParseNode *select = find_node(result_.result_tree_, T_SELECT);
  ASSERT_NE(nullptr, select);
  EXPECT_TRUE(select->is_file_describe_);
  for (const char *query : {
      "copy t to '/tmp/result.csv' (format csv,header)",
      "copy (select id from read_csv('/tmp/a.csv') order by id) to '/tmp/result.csv' (delimiter ';',quote '\"',escape '\"',nullstr 'NULL')"}) {
    SCOPED_TRACE(query);
    ASSERT_EQ(OB_SUCCESS, parse(query));
    const ParseNode *into = find_node(result_.result_tree_, T_INTO_OUTFILE);
    ASSERT_NE(nullptr, into);
    EXPECT_TRUE(into->is_file_copy_);
  }
  ASSERT_EQ(OB_SUCCESS, parse("select read_csv,read_json from ordinary_table where id=1"));
  EXPECT_EQ(nullptr, find_node(result_.result_tree_, T_FILE_TABLE_EXPRESSION));
  ASSERT_EQ(OB_SUCCESS, parse("explain select * from ordinary_table"));
  ASSERT_NE(nullptr, find_node(result_.result_tree_, T_EXPLAIN));
}
} // namespace
