#ifndef DIALOG_DIALOG_STORE_H_
#define DIALOG_DIALOG_STORE_H_

#include <math.h>

#include <functional>
#include <numeric>
#include <thread>

#include "storage.h"
#include "monolog.h"
#include "read_tail.h"
#include "schema.h"
#include "table_metadata.h"
#include "radix_tree.h"
#include "tiered_index.h"
#include "expression_compiler.h"
#include "filter.h"
#include "trigger.h"
#include "exceptions.h"

#include "time_utils.h"
#include "string_utils.h"

using namespace ::dialog::monolog;
using namespace ::dialog::index;
using namespace ::dialog::monitor;
using namespace ::utils;

// TODO: Add more tests
// TODO: Improve documentation

namespace dialog {

template<class storage_mode = storage::in_memory>
class dialog_table {
 public:
  template<typename T, class sm>
  using aux_log_t = monolog_linear<T, 256, 65536, 0, sm>;

  typedef monolog::monolog_exp2<uint64_t, 24> reflog;
  typedef index::tiered_index<reflog, 2, 1> idx_bool_t;
  typedef index::tiered_index<reflog, 256, 1> idx1_t;
  typedef index::tiered_index<reflog, 256, 2> idx2_t;
  typedef index::tiered_index<reflog, 256, 4> idx4_t;
  typedef index::tiered_index<reflog, 256, 8> idx8_t;

  dialog_table(const std::vector<column_t>& table_schema,
               const std::string& path = ".")
      : data_log_("data_log", path),
        rt_(path),
        schema_(path, table_schema),
        metadata_(path) {
  }

  dialog_table(const schema_builder& builder, const std::string& path = ".")
      : data_log_("data_log", path),
        rt_(path),
        schema_(path, builder.get_columns()),
        metadata_(path) {
  }

  // Management interface
  void add_index(const std::string& field_name, double bucket_size) {
    uint16_t idx;
    try {
      idx = schema_.name_map.at(string_utils::to_upper(field_name));
    } catch (std::exception& e) {
      THROW(management_exception,
          "Could not add index for " + field_name + " : " + e.what());
    }

    column_t& col = schema_[idx];
    bool success = col.set_indexing();
    if (success) {
      uint16_t index_id;
      switch (col.type().id) {
        case type_id::D_BOOL: {
          index_id = idx_.push_back(new radix_tree(1, 2));
          break;
        }
        case type_id::D_CHAR:
        case type_id::D_SHORT:
        case type_id::D_INT:
        case type_id::D_LONG:
        case type_id::D_FLOAT:
        case type_id::D_DOUBLE:
        case type_id::D_STRING:
          index_id = idx_.push_back(new radix_tree(col.type().size, 256));
          break;
        default:
          col.set_unindexed();
          THROW(management_exception,"Index not supported for field type");
      }
      col.set_indexed(index_id, bucket_size);
      metadata_.write_index_info(index_id, field_name, bucket_size);
    } else {
      THROW(management_exception,
          "Could not index " + field_name + ": already indexed/indexing");
    }
  }

  void remove_index(const std::string& field_name) {
    uint16_t idx;
    try {
      idx = schema_.name_map.at(string_utils::to_upper(field_name));
    } catch (std::exception& e) {
      THROW(management_exception,
          "Could not remove index for " + field_name + " : " + e.what());
    }

    if (!schema_.columns[idx].disable_indexing()) {
      THROW(management_exception,
          "Could not remove index for " + field_name + ": No index exists");
    }
  }

  uint32_t add_filter(const std::string& expression, size_t monitor_ms) {
    auto cexpr = expression_compiler::compile(expression, schema_);
    uint32_t filter_id = filters_.push_back(new filter(cexpr, monitor_ms));
    metadata_.write_filter_info(filter_id, expression);
    return filter_id;
  }

  uint32_t add_trigger(uint32_t filter_id, const std::string& field_name,
                       aggregate_id agg, relop_id op,
                       const numeric_t& threshold) {
    trigger *t = new trigger(filter_id, op, threshold);
    uint32_t trigger_id = triggers_.push_back(t);
    metadata_.write_trigger_info(trigger_id, filter_id, agg, field_name, op,
                                 threshold);
    return trigger_id;
  }

  uint64_t append(const void* data, size_t length, uint64_t ts =
                      time_utils::cur_ns()) {
    uint64_t offset = data_log_.append((const uint8_t*) data, length);
    record_t r = schema_.apply(offset, data, offset + length, ts);

    size_t nfilters = filters_.size();
    for (size_t i = 0; i < nfilters; i++)
      filters_.at(i)->update(r);

    for (const field_t& f : r)
      if (f.is_indexed())
        idx_.at(f.index_id())->insert(f.get_key(), offset);

    data_log_.flush(offset, length);
    rt_.advance(offset, length);
    return offset;
  }

  void* ptr(uint64_t offset, uint64_t tail) const {
    if (offset < tail)
      return data_log_.cptr(offset);
    return nullptr;
  }

  void* ptr(uint64_t offset) const {
    return ptr(offset, rt_.get());
  }

  bool read(uint64_t offset, void* data, size_t length, uint64_t tail) const {
    if (offset < tail) {
      data_log_.read(offset, (uint8_t*) data, length);
      return true;
    }
    return false;
  }

  bool get(uint64_t offset, uint8_t* data, size_t length) const {
    return read(offset, (uint8_t*) data, length, rt_.get());
  }

  size_t num_records() const {
    return rt_.get();
  }

 protected:
  monolog_linear<uint8_t, 65536, 1073741824, 1048576, storage_mode> data_log_;
  read_tail<storage_mode> rt_;
  schema_t<storage_mode> schema_;
  metadata_writer<storage_mode> metadata_;

  // In memory structures
  aux_log_t<filter*, storage::in_memory> filters_;
  aux_log_t<trigger*, storage::in_memory> triggers_;
  aux_log_t<radix_tree*, storage::in_memory> idx_;
  aux_log_t<idx_bool_t*, storage::in_memory> idx_bool_;

  aux_log_t<idx1_t*, storage::in_memory> idx1_;
  aux_log_t<idx2_t*, storage::in_memory> idx2_;
  aux_log_t<idx4_t*, storage::in_memory> idx4_;
  aux_log_t<idx8_t*, storage::in_memory> idx8_;
};

}

#endif /* DIALOG_DIALOG_STORE_H_ */
