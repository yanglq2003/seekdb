/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "sql/engine/basic/file_sql_config.h"
#include "share/schema/ob_schema_utils.h"
#include "share/ob_server_struct.h"
#include "common/timezone/ob_time_convert.h"
#include <new>
#include <charconv>

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_function_table_op.h"
#include "sql/pl/ob_pl_user_type.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"


namespace oceanbase
{
using namespace common;
namespace sql
{

namespace file_sql {
struct ScanState {
  Config config;
  CsvReader csv;
  JsonLineReader json;
  ParquetReader parquet;
  GlobReader glob;
  bool is_glob = false;
  bool is_parquet = false;
  std::vector<Cell> row;
  std::vector<size_t> high_water;
  size_t result_bytes = 0;
  bool is_csv = false;
};
}

OB_SERIALIZE_MEMBER((ObFunctionTableSpec, ObOpSpec), value_expr_, column_exprs_, has_correlated_expr_, file_column_ids_, is_file_table_);

int ObFunctionTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  already_calc_ = false;
  if (MY_SPEC.is_file_table_) return open_file_table();
  if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType == MY_SPEC.value_expr_->datum_meta_.type_) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_udf;
  } else {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_sys_func;
  }
  return ret;
}

int ObFunctionTableOp::inner_rescan()
{
  if (MY_SPEC.is_file_table_) return open_file_table();
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else {
    node_idx_ = 0;
    if (MY_SPEC.has_correlated_expr_) {
      row_count_ = 0;
      col_count_ = 0;
      value_table_ = NULL;
      already_calc_ = false;
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_close()
{
  delete file_state_; file_state_ = nullptr;
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  row_count_ = 0;
  col_count_ = 0;
  value_table_ = NULL;
  return ret;
}

void ObFunctionTableOp::destroy()
{
  delete file_state_; file_state_ = nullptr;
  ObOperator::destroy();
}


int ObFunctionTableOp::open_file_table()
{
  int ret = OB_SUCCESS;
  delete file_state_; file_state_ = nullptr;
  bool enabled = false;
  ObString root;
  common::ObArenaAllocator path_allocator("FileSqlPath");
  ObDatum *descriptor = nullptr;
  CK (ctx_.get_my_session() != nullptr);
  OZ (ctx_.get_my_session()->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled));
  if (OB_SUCC(ret) && !enabled) { ret = OB_NOT_SUPPORTED; LOG_USER_ERROR(OB_NOT_SUPPORTED, "file SQL is disabled"); }
  OZ (share::schema::ObSchemaUtils::get_runtime_varchar_variable(*GCTX.schema_service_, share::SYS_VAR_SECURE_FILE_PRIV, path_allocator, root));
  OZ (MY_SPEC.value_expr_->eval(eval_ctx_, descriptor));
  if (OB_SUCC(ret)) {
    file_state_ = new (std::nothrow) file_sql::ScanState;
    if (!file_state_) ret = OB_ALLOCATE_MEMORY_FAILED;
    else if (!file_state_->config.decode(std::string(descriptor->get_string().ptr(), descriptor->get_string().length()))) ret = OB_INVALID_ARGUMENT;
  }
  if (OB_SUCC(ret)) {
    auto &state = *file_state_;
    state.high_water.assign(state.config.columns.size(), 0);
    std::string directory(root.ptr(), root.length());
    state.is_csv = state.config.format == "read_csv";
    state.is_glob = state.config.format == "glob";
    state.is_parquet = state.config.format == "read_parquet";
    bool valid = state.is_glob ? state.glob.open(state.config.path, directory)
                 : state.is_parquet ? state.parquet.open(state.config.path, directory) && state.parquet.infer()
                 : state.is_csv ? state.csv.open(state.config.path, directory, state.config.options) && state.csv.infer()
                             : state.json.open(state.config.path, directory) && state.json.infer();
    if (!valid) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, state.is_glob ? state.glob.error().c_str() : state.is_parquet ? state.parquet.error().c_str() : state.is_csv ? state.csv.error().c_str() : state.json.error().c_str());
    } else {
      const auto &columns = state.is_glob ? state.glob.columns() : state.is_parquet ? state.parquet.columns() : state.is_csv ? state.csv.columns() : state.json.columns();
      const std::string identity = state.is_glob ? std::string() : state.is_parquet ? state.parquet.identity() : state.is_csv ? state.csv.identity() : state.json.identity();
      if (identity != state.config.identity) ret = OB_INVALID_ARGUMENT;
      if (state.is_parquet) {
        std::vector<size_t> selected;
        for (int64_t i = 0; i < MY_SPEC.file_column_ids_.count(); ++i) selected.push_back(MY_SPEC.file_column_ids_.at(i));
        if (!state.parquet.project(selected)) ret = OB_INVALID_ARGUMENT;
      }
      if (columns.size() != state.config.columns.size()) ret = OB_SCHEMA_ERROR;
      for (size_t i = 0; OB_SUCC(ret) && i < columns.size(); ++i) {
        if (columns[i].name != state.config.columns[i].name || columns[i].type != state.config.columns[i].type
            || columns[i].nullable != state.config.columns[i].nullable || columns[i].scale != state.config.columns[i].scale) ret = OB_SCHEMA_ERROR;
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::next_file_row()
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  OZ (ctx_.check_status());
  CK (file_state_ != nullptr);
  bool end = false;
  if (OB_SUCC(ret)) {
    auto &state = *file_state_;
    bool valid = state.is_glob ? state.glob.next(state.row, end) : state.is_parquet ? state.parquet.next(state.row, end) : state.is_csv ? state.csv.next(state.row, end) : state.json.next(state.row, end);
    if (!valid) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, state.is_glob ? state.glob.error().c_str() : state.is_parquet ? state.parquet.error().c_str() : state.is_csv ? state.csv.error().c_str() : state.json.error().c_str());
    } else if (end) ret = OB_ITER_END;
    for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.column_exprs_.count(); ++i) {
      int64_t index = MY_SPEC.file_column_ids_.at(i);
      if (index < 0 || index >= static_cast<int64_t>(state.row.size())) { ret = OB_ERR_UNEXPECTED; break; }
      const auto &cell = state.row[index];
      ObExpr *expr = MY_SPEC.column_exprs_.at(i);
      ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
      if (cell.is_null) datum.set_null();
      else {
        switch (state.config.columns[index].type) {
          case file_sql::Type::bigint: {
            int64_t value = 0;
            auto result = std::from_chars(cell.text.data() + (cell.text[0] == '+'), cell.text.data() + cell.text.size(), value);
            if (result.ec != std::errc() || result.ptr != cell.text.data() + cell.text.size()) ret = OB_INVALID_ARGUMENT;
            else datum.set_int(value);
            break;
          }
          case file_sql::Type::real: {
            double value = 0;
            auto result = std::from_chars(cell.text.data() + (cell.text[0] == '+'), cell.text.data() + cell.text.size(), value, std::chars_format::general);
            if (result.ec != std::errc() || result.ptr != cell.text.data() + cell.text.size()) ret = OB_INVALID_ARGUMENT;
            else datum.set_double(value);
            break;
          }
          case file_sql::Type::boolean: datum.set_int(!cell.text.empty() && (cell.text[0] == 't' || cell.text[0] == 'T')); break;
          case file_sql::Type::date: {
            int32_t value = 0;
            OZ (ObTimeConverter::str_to_date(ObString(cell.text.size(), cell.text.data()), value));
            if (OB_SUCC(ret)) datum.set_date(value);
            break;
          }
          case file_sql::Type::datetime: {
            int64_t value = 0;
            OZ (ObTimeConverter::str_to_datetime(ObString(cell.text.size(), cell.text.data()), ObTimeConvertCtx(nullptr, false), value));
            if (OB_SUCC(ret)) datum.set_datetime(value);
            break;
          }
          default: {
            if (cell.text.size() > state.high_water[index]) {
              state.result_bytes += cell.text.size() - state.high_water[index];
              state.high_water[index] = cell.text.size();
            }
            if (state.result_bytes > 128 * 1024 * 1024) { ret = OB_SIZE_OVERFLOW; break; }
            char *buffer = expr->get_str_res_mem(eval_ctx_, cell.text.size());
            if (!buffer && !cell.text.empty()) ret = OB_ALLOCATE_MEMORY_FAILED;
            else { if (!cell.text.empty()) MEMCPY(buffer, cell.text.data(), cell.text.size()); datum.set_string(buffer, cell.text.size()); }
            break;
          }
        }
      }
      if (OB_SUCC(ret)) expr->set_evaluated_projected(eval_ctx_);
    }
  }
  return ret;
}

int ObFunctionTableOp::get_current_result(ObObj &result)
{
  int ret = OB_SUCCESS;
  void *data = NULL;
  CK (already_calc_);
  if (node_idx_ < 0 || node_idx_ >= row_count_ * col_count_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail to get current result in table function",
              K(node_idx_), K(row_count_), K(col_count_));
  }
  do {
    CK (node_idx_ >= 0);
    if (OB_SUCC(ret) && node_idx_ >= row_count_) {
      ret = OB_ITER_END;
    }
    CK (OB_NOT_NULL(value_table_));
    OX (data = value_table_->get_data());
    CK (OB_NOT_NULL(data));
    OX (result = (static_cast<ObObj*>(data))[node_idx_++]);
  } while (OB_SUCC(ret) && result.get_meta().get_type() == ObMaxType);
  return ret;
}

int ObFunctionTableOp::inner_get_next_row()
{
  if (MY_SPEC.is_file_table_) return next_file_row();
  return (this->*next_row_func_)();
}

int ObFunctionTableOp::inner_get_next_row_udf()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType != MY_SPEC.value_expr_->datum_meta_.type_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (FALSE_IT(plan_ctx->set_autoinc_id_tmp(0))) {
  } else if (OB_FAIL(ctx_.check_status())) {
  } else {
    ObDatum *value = nullptr;
    if (!already_calc_) {
      if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
      } else if (value->is_null()) {
        //do nothing
      } else if (OB_ISNULL(value_table_ 
                 = reinterpret_cast<pl::ObPLCollection*>(value->get_ext()))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        row_count_ = value_table_->is_inited() ? value_table_->get_count() : 0;
        col_count_ = value_table_->get_column_count();
      }
      already_calc_ = true;
    }
    if (OB_SUCC(ret) && OB_UNLIKELY(node_idx_ >= row_count_)) {
      ret = OB_ITER_END;
    }
    CK (MY_SPEC.column_exprs_.count() >= col_count_);
    ObObj obj_stack[col_count_];
    if (OB_SUCC(ret)) {
      if (nullptr != value_table_ 
          && ObExtendType == value_table_->get_element_type().get_obj_type()) {
        pl::ObPLComposite *composite = NULL;
        pl::ObPLRecord *record = NULL;
        ObObj record_obj;
        OZ (get_current_result(record_obj));
        if (OB_FAIL(ret)) {
        } else if (ObUserDefinedSQLType == record_obj.get_type()) {
          obj_stack[0] = record_obj;   
        } else if (record_obj.is_pl_extend()) {
          CK (OB_NOT_NULL(composite = reinterpret_cast<pl::ObPLComposite*>(record_obj.get_ext())));
          if (OB_SUCC(ret)) {
            if (composite->is_record()) {
              OX (record = static_cast<pl::ObPLRecord*>(composite));
              CK (record->get_count() == col_count_);
              for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
                OZ (record->get_element(i, obj_stack[i]));
              }
            } else if (composite->is_collection()) {
              OX (obj_stack[0] = record_obj);
            } else {
              ret = OB_ERR_UNEXPECTED;
            }
          }
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_ERROR("unexpected here", K(ret), K(record_obj), K(record_obj.meta_));
        }
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
          if (OB_FAIL(get_current_result(obj_stack[i]))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
        if (obj_stack[i].is_null()) {
          MY_SPEC.column_exprs_.at(i)->locate_datum_for_write(eval_ctx_).set_null();
        } else {
          const ObObjDatumMapType &datum_map = MY_SPEC.column_exprs_.at(i)->obj_datum_map_;
          ObExpr * const &expr = MY_SPEC.column_exprs_.at(i);
          ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
          if (OB_FAIL(datum.from_obj(obj_stack[i], datum_map))) {
          } else if (is_lob_storage(obj_stack[i].get_type()) &&
                     OB_FAIL(ob_adjust_lob_datum(get_exec_ctx(), obj_stack[i],
                                                 expr->obj_meta_, datum_map,
                                                 get_exec_ctx().get_allocator(), datum))) {
          }
        }
        if (OB_SUCC(ret)) {
          MY_SPEC.column_exprs_.at(i)->set_evaluated_projected(eval_ctx_);
        }
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_sys_func()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  ObDatum *value = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ctx_.check_status())) {
  } else if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
    if (OB_ITER_END != ret) {
    }
  } else {
    MY_SPEC.column_exprs_.at(0)->locate_datum_for_write(eval_ctx_).set_datum(*value);
    MY_SPEC.column_exprs_.at(0)->set_evaluated_projected(eval_ctx_);
  }
  return ret;
}


} // end namespace sql
} // end namespace oceanbase
