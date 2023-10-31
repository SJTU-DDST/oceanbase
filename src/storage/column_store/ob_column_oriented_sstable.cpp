/*************************************************************************
  * Copyright (c) 2022 OceanBase
  * OceanBase is licensed under Mulan PubL v2.
  * You can use this software according to the terms and conditions of the Mulan PubL v2
  * You may obtain a copy of Mulan PubL v2 at:
  *          http://license.coscl.org.cn/MulanPubL-2.0
  * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
  * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
  * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
  * See the Mulan PubL v2 for more details.
  * File Name   : ob_column_oriented_sstable.cpp
  * Created  on : 09/05/2022
 ************************************************************************/
#define USING_LOG_PREFIX STORAGE

#include "ob_column_oriented_sstable.h"
#include "ob_co_sstable_row_getter.h"
#include "ob_co_sstable_row_scanner.h"
#include "ob_co_sstable_row_multi_getter.h"
#include "ob_co_sstable_row_multi_scanner.h"
#include "ob_cg_scanner.h"
#include "ob_cg_tile_scanner.h"
#include "ob_cg_aggregated_scanner.h"
#include "ob_cg_group_by_scanner.h"
#include "ob_virtual_cg_scanner.h"
#include "storage/ob_storage_struct.h"
#include "storage/meta_mem/ob_tenant_meta_mem_mgr.h"
#include "common/ob_tablet_id.h"
#include "share/schema/ob_table_schema.h"
#include "storage/tablet/ob_tablet_create_delete_helper.h"
#include "storage/access/ob_sstable_row_multi_scanner.h"
#include "storage/tablet/ob_tablet_table_store.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;
using namespace oceanbase::blocksstable;
using namespace oceanbase::share::schema;

namespace oceanbase
{
namespace storage
{

ObCGTableWrapper::ObCGTableWrapper()
  : meta_handle_(),
    cg_sstable_(nullptr),
    need_meta_(true)
{
}

void ObCGTableWrapper::reset()
{
  meta_handle_.reset();
  cg_sstable_ = nullptr;
  need_meta_ = true;
}

bool ObCGTableWrapper::is_valid() const
{
  bool bret = false;

  if (OB_ISNULL(cg_sstable_)) {
  } else if (!need_meta_) {
    bret = true;
  } else if (cg_sstable_->is_loaded() || meta_handle_.is_valid()) {
    bret = true;
  }
  return bret;
}

int ObCGTableWrapper::get_sstable(ObSSTable *&table)
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(!is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("wrapper not valid", K(ret), KPC(this));
  } else if (cg_sstable_->is_loaded()) {
    table = cg_sstable_;
  } else if (OB_FAIL(meta_handle_.get_sstable(table))) {
    LOG_WARN("failed to get sstable", K(ret), KPC(this));
  }
  return ret;
}


int64_t ObCOSSTableMeta::get_serialize_size() const
{
  int64_t len = 0;
  LST_DO_CODE(OB_UNIS_ADD_LEN,
      data_macro_block_cnt_,
      use_old_macro_block_cnt_,
      data_micro_block_cnt_,
      index_macro_block_cnt_,
      occupy_size_,
      original_size_,
      data_checksum_,
      column_group_cnt_,
      full_column_cnt_);
  return len;
}

int ObCOSSTableMeta::serialize(char *buf, const int64_t buf_len, int64_t &pos) const
{
  int ret = OB_SUCCESS;
  const int64_t len = get_serialize_size();
  if (OB_ISNULL(buf) || OB_UNLIKELY(buf_len < 0 || pos + len > buf_len)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(buf_len), K(len));
  } else {
    LST_DO_CODE(OB_UNIS_ENCODE,
        data_macro_block_cnt_,
        use_old_macro_block_cnt_,
        data_micro_block_cnt_,
        index_macro_block_cnt_,
        occupy_size_,
        original_size_,
        data_checksum_,
        column_group_cnt_,
        full_column_cnt_);
    }
  return ret;
}

int ObCOSSTableMeta::deserialize(const char *buf, const int64_t data_len, int64_t &pos)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(buf) || OB_UNLIKELY(data_len < 0 || data_len < pos)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret));
  } else {
    LST_DO_CODE(OB_UNIS_DECODE,
        data_macro_block_cnt_,
        use_old_macro_block_cnt_,
        data_micro_block_cnt_,
        index_macro_block_cnt_,
        occupy_size_,
        original_size_,
        data_checksum_,
        column_group_cnt_,
        full_column_cnt_);
  }
  return ret;
}


/************************************* ObCOSSTableV2 *************************************/
ObCOSSTableV2::ObCOSSTableV2()
  : ObSSTable(),
    cg_sstables_(),
    cs_meta_(),
    base_type_(ObCOSSTableBaseType::INVALID_TYPE),
    is_empty_co_(false),
    valid_for_cs_reading_(false),
    tmp_allocator_("CGAlloc", OB_MALLOC_NORMAL_BLOCK_SIZE, MTL_ID())
{
}

ObCOSSTableV2::~ObCOSSTableV2()
{
  reset();
}

void ObCOSSTableV2::reset()
{
  ObSSTable::reset();
  cs_meta_.reset();
  cg_sstables_.reset();
  valid_for_cs_reading_ = false;
  tmp_allocator_.reset();
}

int ObCOSSTableV2::init(
    const ObTabletCreateSSTableParam &param,
    common::ObArenaAllocator *allocator)
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(!param.is_valid() ||
                  ObCOSSTableBaseType::INVALID_TYPE >= param.co_base_type_ ||
                  ObCOSSTableBaseType::MAX_TYPE <= param.co_base_type_ ||
                  1 >= param.column_group_cnt_ ||
                  NULL == allocator)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("get invalid arguments", K(ret), K(param), K(allocator));
  } else if (OB_FAIL(ObSSTable::init(param, allocator))) {
    LOG_WARN("failed to init basic ObSSTable", K(ret), K(param));
    // TODO(@zhouxinlan.zxl) use allocator in param
  } else if (param.is_empty_co_table_) {
    // current co sstable is empty, no need to init cg sstable
    cs_meta_.column_group_cnt_ = param.column_group_cnt_;
  } else if (OB_FAIL(cg_sstables_.init_empty_array_for_cg(
        tmp_allocator_, param.column_group_cnt_ - 1/*should reduce the basic cg*/))) {
    LOG_WARN("failed to alloc memory for cg sstable array", K(ret), K(param));
  }

  if (OB_SUCC(ret)) {
    base_type_ = static_cast<ObCOSSTableBaseType>(param.co_base_type_);
    is_empty_co_ = param.is_empty_co_table_;
    valid_for_cs_reading_ = param.is_empty_co_table_;
    cs_meta_.full_column_cnt_ = param.full_column_cnt_;
  } else {
    reset();
  }
  return ret;
}

int ObCOSSTableV2::fill_cg_sstables(const common::ObIArray<ObITable *> &cg_tables)
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(!valid_for_reading_ || valid_for_cs_reading_ || 0 == cg_sstables_.count() || is_empty_co_)) {
    ret = OB_STATE_NOT_MATCH;
    LOG_WARN("this co sstable can't init cg sstables", K(ret),
        K(valid_for_reading_), K(valid_for_cs_reading_), K(cg_sstables_), K(is_empty_co_));
  } else if (OB_UNLIKELY(cg_sstables_.count() != cg_tables.count())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("get invalid arguments", K(ret), K(cg_sstables_), K(cg_tables.count()));
  } else if (OB_FAIL(cg_sstables_.add_tables_for_cg(tmp_allocator_, cg_tables))) {
    LOG_WARN("failed to add cg sstables", K(ret), K(cg_tables));
  } else if (OB_FAIL(build_cs_meta())) {
    LOG_WARN("failed to build cs meta", K(ret), KPC(this));
  } else {
    valid_for_cs_reading_ = true;
    FLOG_INFO("success to init co sstable", K(ret), K_(cs_meta), K_(cg_sstables), KPC(this)); // tmp debug code
  }
  return ret;
}

int ObCOSSTableV2::inc_macro_ref(bool &inc_success) const
{
  int ret = OB_SUCCESS;
  inc_success = false;
  bool co_success = false;
  bool cg_success = false;
  if (OB_FAIL(ObSSTable::inc_macro_ref(co_success))) {
    LOG_WARN("fail to increase row store macro blocks' ref cnt", K(ret), K(co_success));
  } else if (is_empty_co_) { // no cg sstable
    inc_success = true;
  } else if (!valid_for_cs_reading_) {
    cg_success = true;
  } else if (OB_FAIL(cg_sstables_.inc_macro_ref(cg_success))) {
    LOG_WARN("fail to increase ref cnt of cg sstables' macro blocks", K(ret), K(cg_success));
  }

  if (OB_FAIL(ret)) {
    if (co_success) {
      ObSSTable::dec_macro_ref();
    }
    if (cg_success) {
      cg_sstables_.dec_macro_ref();
    }
  } else {
    inc_success = true;
  }
  return ret;
}

void ObCOSSTableV2::dec_macro_ref() const
{
  ObSSTable::dec_macro_ref();
  if (is_empty_co_) {
    // do nothing
  } else if (valid_for_cs_reading_) {
    cg_sstables_.dec_macro_ref();
  }
}

int ObCOSSTableV2::build_cs_meta()
{
  int ret = OB_SUCCESS;
  ObSSTableMetaHandle co_meta_handle;
  blocksstable::ObSSTableMeta *meta = nullptr;
  const int64_t cg_table_cnt = cg_sstables_.count() + 1/*base_cg_table*/;
  cs_meta_.column_group_cnt_ = cg_table_cnt;

  if (OB_UNLIKELY(is_empty_co_)) {
    ret = OB_STATE_NOT_MATCH;
    LOG_WARN("no need to build cs meta for empty co table", K(ret), KPC(this));
  } else if (OB_FAIL(get_meta(co_meta_handle))) {
    LOG_WARN("Failed to get co sstable meta", K(ret), KPC(this));
  } else {
    const blocksstable::ObSSTableMeta &meta = co_meta_handle.get_sstable_meta();
    for (int64_t idx = 0; OB_SUCC(ret) && idx < cg_table_cnt; ++idx) {
      ObSSTable *cg_sstable = (cg_table_cnt - 1 == idx) ? this : cg_sstables_[idx];
      ObSSTableMetaHandle cg_meta_handle;
      if (OB_ISNULL(cg_sstable)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null cg sstable", K(ret));
      } else if (OB_UNLIKELY(cg_sstable->is_rowkey_cg_sstable()
          && ObCOSSTableBaseType::ROWKEY_CG_TYPE == base_type_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected rowkey cg table", K(ret), K(base_type_), KPC(cg_sstable));
      } else if (OB_UNLIKELY(cg_sstable->get_snapshot_version() != get_snapshot_version())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("the snapshot version of cg sstables must be equal", K(ret));
      } else if (OB_FAIL(cg_sstable->get_meta(cg_meta_handle))) {
        LOG_WARN("Failed to get cg sstable meta", K(ret), KPC(cg_sstable));
      } else if (OB_UNLIKELY(cg_meta_handle.get_sstable_meta().get_schema_version() != meta.get_schema_version())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("the schema version of cg sstables must be equal", K(ret), K(meta), K(cg_meta_handle));
      } else if (OB_UNLIKELY(cg_meta_handle.get_sstable_meta().get_row_count() != meta.get_row_count())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("the row count of cg sstables must be equal", K(ret), KPC(cg_sstable), K(meta), K(cg_meta_handle));
      } else {
        cs_meta_.data_macro_block_cnt_ += cg_meta_handle.get_sstable_meta().get_basic_meta().data_macro_block_count_;
        cs_meta_.use_old_macro_block_cnt_ += cg_meta_handle.get_sstable_meta().get_basic_meta().use_old_macro_block_count_;
        cs_meta_.data_micro_block_cnt_ += cg_meta_handle.get_sstable_meta().get_basic_meta().data_micro_block_count_;
        cs_meta_.index_macro_block_cnt_ += cg_meta_handle.get_sstable_meta().get_basic_meta().index_macro_block_count_;
        cs_meta_.occupy_size_ += cg_meta_handle.get_sstable_meta().get_basic_meta().occupy_size_;
        cs_meta_.original_size_ += cg_meta_handle.get_sstable_meta().get_basic_meta().original_size_;
        cs_meta_.data_checksum_ += cg_meta_handle.get_sstable_meta().get_basic_meta().data_checksum_;
      }
    }
  }
  return ret;
}

int64_t ObCOSSTableV2::get_serialize_size() const
{
  int64_t len = 0;
  len += ObSSTable::get_serialize_size();
  len += serialization::encoded_length_i32(base_type_);
  len += serialization::encoded_length_bool(is_empty_co_);
  len += cs_meta_.get_serialize_size();
  if (!is_empty_co_) {
    len += cg_sstables_.get_serialize_size();
  }
  return len;
}

int ObCOSSTableV2::serialize(char *buf, const int64_t buf_len, int64_t &pos) const
{
  int ret = OB_SUCCESS;
  const int64_t old_pos = pos;

  if (OB_UNLIKELY(!is_cs_valid())) {
    ret = OB_NOT_INIT;
    LOG_WARN("co sstable not init", K(ret), KPC(this));
  } else if (OB_UNLIKELY(NULL == buf || buf_len <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("argument is invalid", K(ret), KP(buf), K(buf_len));
  } else if (OB_FAIL(ObSSTable::serialize(buf, buf_len, pos))) {
    LOG_WARN("failed to serialize basic ObSSTable", K(ret), KP(buf), K(buf_len), K(pos));
  } else if (OB_FAIL(serialization::encode_i32(buf, buf_len, pos, base_type_))) {
    LOG_WARN("failed to serialize base type", K(ret), KP(buf), K(buf_len), K(pos));
  } else if (OB_FAIL(serialization::encode_bool(buf, buf_len, pos, is_empty_co_))) {
    LOG_WARN("failed to serialize is empty co", K(ret), KP(buf), K(buf_len), K(pos));
  } else if (OB_FAIL(cs_meta_.serialize(buf, buf_len, pos))) {
    LOG_WARN("failed to serialize cs meta", K(ret), KP(buf), K(buf_len), K(pos));
  } else if (is_empty_co_) {
    // no need to serialize cg sstable
  } else if (OB_FAIL(cg_sstables_.serialize(buf, buf_len, pos))) {
    LOG_WARN("failed to serialize cg sstables", K(ret), KP(buf), K(buf_len), K(pos));
  }
  FLOG_INFO("chaser debug serialize co sstable", K(ret), KPC(this), K(buf_len), K(old_pos), K(pos)); // tmp debug code
  return ret;
}

int ObCOSSTableV2::deserialize(
    common::ObArenaAllocator &allocator,
    const char *buf,
    const int64_t data_len,
    int64_t &pos)
{
  int ret = OB_SUCCESS;
  int64_t old_pos = pos;

  if (OB_UNLIKELY(is_cs_valid())) {
    ret = OB_INIT_TWICE;
    LOG_WARN("co sstable has been inited", K(ret), KPC(this));
  } else if (OB_UNLIKELY(NULL == buf || data_len <= 0 || pos < 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("get invalid arguments.", KP(buf),K(data_len), K(pos), K(ret));
  } else if (OB_FAIL(ObSSTable::deserialize(allocator, buf, data_len, pos))) {
    LOG_WARN("failed to deserialize basic ObSSTable", K(ret), KP(buf),K(data_len), K(pos));
  } else if (OB_FAIL(serialization::decode_i32(buf, data_len, pos, reinterpret_cast<int32_t *>(&base_type_)))) {
    LOG_WARN("failed to decode base type", K(ret), KP(buf),K(data_len), K(pos));
  } else if (OB_FAIL(serialization::decode_bool(buf, data_len, pos, &is_empty_co_))) {
    LOG_WARN("failed to decode is empty co", K(ret), KP(buf),K(data_len), K(pos));
  } else if (OB_FAIL(cs_meta_.deserialize(buf, data_len, pos))) {
    LOG_WARN("failed to deserialize cs meta", K(ret), KP(buf), K(data_len), K(pos));
  } else if (is_empty_co_) {
    // no need to deserialize cg sstable
  } else if (OB_FAIL(cg_sstables_.deserialize(allocator, buf, data_len, pos))) {
    LOG_WARN("failed to deserialize cg sstable", K(ret), KP(buf), K(data_len), K(pos));
  }

  if (OB_SUCC(ret)) {
    valid_for_cs_reading_ = true;
    FLOG_INFO("success to deserialize co sstable", K(ret), KPC(this), K(data_len), K(pos), K(old_pos)); // tmp debug code
  }
  return ret;
}

int ObCOSSTableV2::serialize_full_table(char *buf, const int64_t buf_len, int64_t &pos) const
{
  int ret = OB_SUCCESS;
  const int64_t old_pos = pos;

  if (OB_UNLIKELY(!is_cs_valid())) {
    ret = OB_NOT_INIT;
    LOG_WARN("co sstable not init", K(ret), KPC(this));
  } else if (OB_UNLIKELY(NULL == buf || buf_len <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("argument is invalid", K(ret), KP(buf), K(buf_len));
  } else if (OB_FAIL(ObSSTable::serialize_full_table(buf, buf_len, pos))) {
    LOG_WARN("failed to serialize full sstable", K(ret));
  } else if (OB_FAIL(serialization::encode_i32(buf, buf_len, pos, base_type_))) {
    LOG_WARN("failed to serialize base type", K(ret), KP(buf), K(buf_len), K(pos));
  } else if (OB_FAIL(serialization::encode_bool(buf, buf_len, pos, is_empty_co_))) {
    LOG_WARN("failed to serialize is empty co", K(ret), KP(buf), K(buf_len), K(pos));
  } else if (OB_FAIL(cs_meta_.serialize(buf, buf_len, pos))) {
    LOG_WARN("failed to deserialize cs meta", K(ret), KP(buf), K(buf_len), K(pos));
  } else if (is_empty_co_) {
    // no need to serialize cg sstable
  } else if (OB_FAIL(cg_sstables_.serialize(buf, buf_len, pos))) {
    LOG_WARN("failed to serialize cg sstables", K(ret), KP(buf), K(buf_len), K(pos));
  }
  FLOG_INFO("chaser debug serialize co sstable", K(ret), KPC(this), K(buf_len), K(old_pos), K(pos)); // tmp debug code
  return ret;
}

int64_t ObCOSSTableV2::get_full_serialize_size() const
{
  int64_t len = 0;
  len += ObSSTable::get_full_serialize_size();
  if (len > 0) {
    len += serialization::encoded_length_i32(base_type_);
    len += serialization::encoded_length_bool(is_empty_co_);
    len += cs_meta_.get_serialize_size();
    if (!is_empty_co_) {
      len += cg_sstables_.get_serialize_size();
    }
  }
  return len;
}

int ObCOSSTableV2::deep_copy(char *buf, const int64_t buf_len, ObIStorageMetaObj *&value) const
{
  int ret = OB_SUCCESS;
  value = nullptr;
  const int64_t deep_copy_size = get_deep_copy_size();
  if (OB_ISNULL(buf) || OB_UNLIKELY(buf_len < deep_copy_size)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(buf), K(buf_len), K(deep_copy_size));
  } else {
    ObCOSSTableV2 *new_co_table = new (buf) ObCOSSTableV2();
    int64_t pos = sizeof(ObCOSSTableV2);
    // deep copy
    new_co_table->key_ = key_;
    new_co_table->addr_ = addr_;
    new_co_table->meta_cache_ = meta_cache_;
    new_co_table->is_tmp_sstable_ = false;
    new_co_table->valid_for_reading_ = valid_for_reading_;
    if (is_loaded()) {
      if (OB_FAIL(meta_->deep_copy(buf, buf_len, pos, new_co_table->meta_))) {
        LOG_WARN("fail to deep copy for tiny memory", K(ret), KP(buf), K(buf_len), K(pos), KPC(meta_));
      }
    }

    if (OB_FAIL(ret)) {
    } else if (!is_empty_co_ && OB_FAIL(cg_sstables_.deep_copy(buf, buf_len, pos, new_co_table->cg_sstables_))) {
      LOG_WARN("failed to deep copy cg sstables", K(ret), KP(buf), K(buf_len), K(pos));
    } else {
      MEMCPY(&new_co_table->cs_meta_, &cs_meta_, sizeof(ObCOSSTableMeta));
      new_co_table->is_empty_co_ = is_empty_co_;
      new_co_table->base_type_ = base_type_;
      new_co_table->valid_for_cs_reading_ = true;

      value = new_co_table;
    }
  }
  return ret;
}

int ObCOSSTableV2::deep_copy(
    common::ObArenaAllocator &allocator,
    const common::ObIArray<ObMetaDiskAddr> &cg_addrs,
    ObCOSSTableV2 *&dst)
{
  int ret = OB_SUCCESS;
  const int64_t deep_copy_size = get_deep_copy_size();
  char *buf = nullptr;
  ObCOSSTableV2 *new_co_table = nullptr;
  ObIStorageMetaObj *meta_obj = nullptr;

  if (OB_UNLIKELY(!valid_for_cs_reading_ || !cg_sstables_.is_valid())) {
    ret = OB_STATE_NOT_MATCH;
    LOG_WARN("this co sstable can't set cg table addr", K(ret), K_(valid_for_cs_reading), K_(cg_sstables));
  } else if (OB_UNLIKELY(cg_addrs.count() != cg_sstables_.count())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("get invalid arguments", K(ret), K(cg_addrs.count()), K_(cg_sstables));
  } else if (OB_ISNULL(buf = static_cast<char *>(allocator.alloc(deep_copy_size)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed to allocate memory for deep copy sstable", K(ret), K(deep_copy_size));
  } else if (OB_FAIL(deep_copy(buf, deep_copy_size, meta_obj))) {
    LOG_WARN("failed to deep copy co sstable", K(ret));
  } else {
    new_co_table = static_cast<ObCOSSTableV2 *>(meta_obj);
  }

  // set cg sstable addr
  for (int64_t idx = 0; OB_SUCC(ret) && idx < new_co_table->cg_sstables_.count(); ++idx) {
    ObSSTable *cg_table = new_co_table->cg_sstables_[idx];
    const ObMetaDiskAddr &cg_addr = cg_addrs.at(idx);
    if (OB_ISNULL(cg_table)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null cg table", K(ret), KPC(this));
    } else if (OB_FAIL(cg_table->set_addr(cg_addr))) {
      LOG_WARN("failed to set cg addr", K(ret));
    }
  }

  if (OB_SUCC(ret)) {
    dst = new_co_table;
  }
  return ret;
}

int ObCOSSTableV2::fetch_cg_sstable(
    const uint32_t cg_idx,
    ObCGTableWrapper &cg_wrapper,
    const bool need_meta)
{
  int ret = OB_SUCCESS;
  cg_wrapper.reset();
  cg_wrapper.need_meta_ = need_meta;

  uint32_t real_cg_idx = cg_idx < cs_meta_.column_group_cnt_ ? cg_idx : key_.column_group_idx_;
  if (OB_UNLIKELY(is_empty_co_ && real_cg_idx != key_.get_column_group_id())) {
    ret = OB_STATE_NOT_MATCH;
    LOG_WARN("co sstable is empty, cannot fetch cg sstable", K(ret), K(cg_idx), K(real_cg_idx), KPC(this));
  } else if (OB_FAIL(get_cg_sstable(real_cg_idx, cg_wrapper.cg_sstable_))) {
    LOG_WARN("failed to get cg sstable", K(ret));
  } else if (OB_ISNULL(cg_wrapper.cg_sstable_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null cg table", K(ret), K(cg_wrapper));
  } else if (!need_meta || cg_wrapper.cg_sstable_->is_loaded()) {
    // do nothing
  } else if (OB_FAIL(ObTabletTableStore::load_sstable(cg_wrapper.cg_sstable_->get_addr(),
                                                      false, /*load_co_sstable*/
                                                      cg_wrapper.meta_handle_))) {
    LOG_WARN("failed to load sstable", K(ret), K(cg_wrapper));
  }
  return ret;
}

int ObCOSSTableV2::get_cg_sstable(
    const uint32_t cg_idx,
    ObSSTable *&cg_sstable) const
{
  int ret = OB_SUCCESS;
  cg_sstable = nullptr;

  if (OB_UNLIKELY(!is_cs_valid())) {
    ret = OB_NOT_INIT;
    LOG_WARN("co sstable has not inited", K(ret), KPC(this));
  } else if (cg_idx >= cs_meta_.column_group_cnt_) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("get invalid arguments", K(ret), K(cg_idx), K(cs_meta_));
  } else if (cg_idx == key_.get_column_group_id()) {
    cg_sstable = const_cast<ObCOSSTableV2 *>(this);
  } else if (OB_UNLIKELY(is_empty_co_)) {
    ret = OB_STATE_NOT_MATCH;
    LOG_WARN("co sstable is empty, cannot fetch normal cg sstable", K(ret), K(cg_idx), KPC(this));
  } else if (cg_idx < key_.column_group_idx_) {
    cg_sstable = cg_sstables_[cg_idx];
  } else {
    cg_sstable = cg_sstables_[cg_idx - 1];
  }
  return ret;
}

int ObCOSSTableV2::get_all_tables(common::ObIArray<ObITable *> &tables) const
{
  int ret = OB_SUCCESS;

  if (is_empty_co_) {
    if (OB_FAIL(tables.push_back(const_cast<ObCOSSTableV2 *>(this)))) {
      LOG_WARN("failed to push back", K(ret), K(is_empty_co_));
    }
  } else {
    for (int64_t cg_idx = 0; OB_SUCC(ret) && cg_idx <= cg_sstables_.count(); ++cg_idx) {
      ObSSTable *cg_sstable = nullptr;
      if (OB_FAIL(get_cg_sstable(cg_idx, cg_sstable))) {
        LOG_WARN("failed to get cg sstable", K(ret), K(cg_idx));
      } else if (OB_FAIL(tables.push_back(cg_sstable))) {
        LOG_WARN("failed to push back", K(ret));
      }
    }
  }
  return ret;
}

int ObCOSSTableV2::scan(
    const ObTableIterParam &param,
    ObTableAccessContext &context,
    const blocksstable::ObDatumRange &key_range,
    ObStoreRowIterator *&row_iter)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_cs_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("SSTable is not ready for accessing", K(ret), K_(valid_for_cs_reading), K_(key), K_(base_type), K_(meta));
  } else if (OB_UNLIKELY(param.tablet_id_ != key_.tablet_id_)) {
    ret = OB_ERR_SYS;
    LOG_ERROR("Tablet id is not match", K(ret), K(*this), K(param));
  } else if (OB_UNLIKELY(!param.is_valid() || !context.is_valid() || !key_range.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Invalid argument", K(ret), K(param), K(context), K(key_range));
  } else if (context.query_flag_.is_daily_merge() ||
             (!param.is_use_column_store() && is_all_cg_base())) {
    if (OB_FAIL(ObSSTable::scan(param, context, key_range, row_iter))) {
      LOG_WARN("Fail to scan in row store sstable", K(ret));
    }
  } else {
    // TODO: check whether use row_store/rowkey sstable when primary keys accessed only
    ObStoreRowIterator *row_scanner = nullptr;
    ALLOCATE_TABLE_STORE_ROW_IETRATOR(context, ObCOSSTableRowScanner, row_scanner);
    if (OB_SUCC(ret) && OB_FAIL(row_scanner->init(param, context, this, &key_range))) {
      LOG_WARN("Fail to open row scanner", K(ret), K(param), K(context), K(key_range), K(*this));
    }

    if (OB_FAIL(ret)) {
      if (nullptr != row_scanner) {
        row_scanner->~ObStoreRowIterator();
        FREE_TABLE_STORE_ROW_IETRATOR(context, row_scanner);
        row_scanner = nullptr;
      }
    } else {
      row_iter = row_scanner;
    }
  }
  return ret;
}

int ObCOSSTableV2::multi_scan(
    const ObTableIterParam &param,
    ObTableAccessContext &context,
    const common::ObIArray<blocksstable::ObDatumRange> &ranges,
    ObStoreRowIterator *&row_iter)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_cs_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("SSTable is not ready for accessing", K(ret), K_(valid_for_reading), K_(meta));
  } else if (OB_UNLIKELY(param.tablet_id_ != key_.tablet_id_)) {
    ret = OB_ERR_SYS;
    LOG_ERROR("Tablet id is not match", K(ret), K(*this), K(param));
  } else if (OB_UNLIKELY(!param.is_valid() || !context.is_valid() || 0 >= ranges.count())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Invalid argument", K(ret), K(param), K(context), K(ranges));
  } else if (!param.is_use_column_store() && is_all_cg_base()) {
    if (OB_FAIL(ObSSTable::multi_scan(param, context, ranges, row_iter))) {
      LOG_WARN("Fail to scan in row store sstable", K(ret));
    }
  } else {
    // TODO: check whether use row_store/rowkey sstable when primary keys accessed only
    ObStoreRowIterator *row_scanner = nullptr;
    ALLOCATE_TABLE_STORE_ROW_IETRATOR(context, ObCOSSTableRowMultiScanner, row_scanner);
    if (OB_SUCC(ret) && OB_FAIL(row_scanner->init(param, context, this, &ranges))) {
      LOG_WARN("Fail to open row scanner", K(ret), K(param), K(context), K(ranges), K(*this));
    }

    if (OB_FAIL(ret)) {
      if (nullptr != row_scanner) {
        row_scanner->~ObStoreRowIterator();
        FREE_TABLE_STORE_ROW_IETRATOR(context, row_scanner);
        row_scanner = nullptr;
      }
    } else {
      row_iter = row_scanner;
    }
  }
  return ret;
}

int ObCOSSTableV2::cg_scan(
    const ObTableIterParam &param,
    ObTableAccessContext &context,
    ObICGIterator *&cg_iter,
    const bool is_projector,
    const bool project_single_row)
{
  int ret = OB_SUCCESS;
  cg_iter = nullptr;
  ObICGIterator *cg_scanner = nullptr;
  ObCGTableWrapper table_wrapper;

  if (OB_UNLIKELY(!is_cs_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("SSTable is not ready for accessing", K(ret), K_(valid_for_reading), K_(meta));
  } else if (OB_UNLIKELY(!context.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Invalid argument", K(ret), K(context));
  } else if (is_virtual_cg(param.cg_idx_)) {
    ALLOCATE_TABLE_STORE_CG_IETRATOR(context, param.cg_idx_, ObVirtualCGScanner, cg_scanner);
  } else if (OB_FAIL(fetch_cg_sstable(param.cg_idx_, table_wrapper))) {
    LOG_WARN("failed to fetch cg table wrapper", K(ret), K(param), KPC(this));
  } else if (project_single_row) {
    if (param.cg_idx_ >= cs_meta_.column_group_cnt_) {
      ALLOCATE_TABLE_STORE_CG_IETRATOR(context, param.cg_idx_, ObDefaultCGScanner, cg_scanner);
    } else {
      ALLOCATE_TABLE_STORE_CG_IETRATOR(context, param.cg_idx_, ObCGSingleRowScanner, cg_scanner);
    }
  } else if (param.cg_idx_ >= cs_meta_.column_group_cnt_) {
    if (param.enable_pd_group_by() && is_projector) {
      ALLOCATE_TABLE_STORE_CG_IETRATOR(context, param.cg_idx_, ObDefaultCGGroupByScanner, cg_scanner);
    } else {
      ALLOCATE_TABLE_STORE_CG_IETRATOR(context, param.cg_idx_, ObDefaultCGScanner, cg_scanner);
    }
  } else if (param.enable_pd_group_by() && is_projector) {
    ALLOCATE_TABLE_STORE_CG_IETRATOR(context, param.cg_idx_, ObCGGroupByScanner, cg_scanner);
  } else if (param.enable_pd_aggregate()) {
    ALLOCATE_TABLE_STORE_CG_IETRATOR(context, param.cg_idx_, ObCGAggregatedScanner, cg_scanner);
  } else if (is_projector) {
    ALLOCATE_TABLE_STORE_CG_IETRATOR(context, param.cg_idx_, ObCGRowScanner, cg_scanner);
  } else {
    ALLOCATE_TABLE_STORE_CG_IETRATOR(context, param.cg_idx_, ObCGScanner, cg_scanner);
  }
  if (OB_SUCC(ret)) {
    if (OB_ISNULL(cg_scanner)) {
      ret = common::OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("Fail to alloc cg scanner", K(ret));
    } else if (cg_scanner->is_valid()) {
      if (OB_UNLIKELY(param.cg_idx_ != cg_scanner->get_cg_idx())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Unexpected cg scanner", K(ret), K(param.cg_idx_), K(cg_scanner->get_cg_idx()));
      } else if (OB_FAIL(cg_scanner->switch_context(param, context, table_wrapper))) {
        LOG_WARN("Fail to switch context for cg scanner", K(ret));
      }
    } else if (OB_FAIL(cg_scanner->init(param, context, table_wrapper))) {
      LOG_WARN("Fail to init cg scanner", K(ret));
    }
    if (OB_SUCC(ret)) {
      cg_iter = cg_scanner;
    } else {
      cg_scanner->~ObICGIterator();
      FREE_TABLE_STORE_CG_IETRATOR(context, cg_scanner);
    }
  }
  return ret;
}

int ObCOSSTableV2::get(
    const storage::ObTableIterParam &param,
    storage::ObTableAccessContext &context,
    const blocksstable::ObDatumRowkey &rowkey,
    ObStoreRowIterator *&row_iter)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_cs_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("SSTable is not ready for accessing", K(ret), K_(valid_for_reading), K_(meta));
  } else if (OB_UNLIKELY(!param.is_valid() || param.tablet_id_ != key_.tablet_id_)) {
    ret = OB_ERR_SYS;
    LOG_ERROR("Tablet id is not match", K(ret), K(*this), K(param));
  } else if (param.read_info_->is_access_rowkey_only() || is_all_cg_base()) {
    if (OB_FAIL(ObSSTable::get(param, context, rowkey, row_iter))) {
      LOG_WARN("Fail to scan in row store sstable", K(ret));
    }
  } else if (OB_UNLIKELY(!context.is_valid() || !rowkey.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Invalid argument", K(ret), K(param), K(context), K(rowkey));
  } else {
    ObStoreRowIterator *row_getter = nullptr;
    ALLOCATE_TABLE_STORE_ROW_IETRATOR(context, ObCOSSTableRowGetter, row_getter);
    if (OB_SUCC(ret) && OB_FAIL(row_getter->init(param, context, this, &rowkey))) {
      LOG_WARN("Fail to open row scanner", K(ret), K(param), K(context), K(rowkey), K(*this));
    }

    if (OB_FAIL(ret)) {
      if (nullptr != row_getter) {
        row_getter->~ObStoreRowIterator();
        FREE_TABLE_STORE_ROW_IETRATOR(context, row_getter);
        row_getter = nullptr;
      }
    } else {
      row_iter = row_getter;
    }
  }
  return ret;
}

int ObCOSSTableV2::multi_get(
    const ObTableIterParam &param,
    ObTableAccessContext &context,
    const common::ObIArray<blocksstable::ObDatumRowkey> &rowkeys,
    ObStoreRowIterator *&row_iter)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_cs_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("SSTable is not ready for accessing", K(ret), K_(valid_for_reading), K_(meta));
  } else if (OB_UNLIKELY(param.tablet_id_ != key_.tablet_id_)) {
    ret = OB_ERR_SYS;
    LOG_ERROR("Tablet id is not match", K(ret), K(*this), K(param));
  } else if (OB_UNLIKELY(!param.is_valid() || !context.is_valid() || 0 >= rowkeys.count())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Invalid argument", K(ret), K(param), K(context), K(rowkeys));
  } else if (param.read_info_->is_access_rowkey_only() || is_all_cg_base()) {
    if (OB_FAIL(ObSSTable::multi_get(param, context, rowkeys, row_iter))) {
      LOG_WARN("Fail to scan in row store sstable", K(ret));
    }
  } else {
    ObStoreRowIterator *row_getter = nullptr;
    ALLOCATE_TABLE_STORE_ROW_IETRATOR(context, ObCOSSTableRowMultiGetter, row_getter);
    if (OB_SUCC(ret) && OB_FAIL(row_getter->init(param, context, this, &rowkeys))) {
      LOG_WARN("Fail to open row scanner", K(ret), K(param), K(context), K(rowkeys), K(*this));
    }

    if (OB_FAIL(ret)) {
      if (nullptr != row_getter) {
        row_getter->~ObStoreRowIterator();
        FREE_TABLE_STORE_ROW_IETRATOR(context, row_getter);
        row_getter = nullptr;
      }
    } else {
      row_iter = row_getter;
    }
  }
  return ret;
}

} /* storage */
} /* oceanbase */
