/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#ifndef STORAGE_OB_TENANT_TABLET_SCHEDULER_H_
#define STORAGE_OB_TENANT_TABLET_SCHEDULER_H_

#include "lib/queue/ob_dedup_queue.h"
#include "share/ob_ls_id.h"
#include "share/tablet/ob_tablet_info.h"
#include "storage/ob_i_store.h"
#include "storage/compaction/ob_tablet_merge_task.h"
#include "storage/compaction/ob_partition_merge_policy.h"
#include "storage/compaction/ob_tenant_medium_checker.h"
#include "lib/hash/ob_hashset.h"
#include "storage/compaction/ob_tenant_tablet_scheduler_task_mgr.h"
#include "storage/compaction/ob_compaction_schedule_iterator.h"

namespace oceanbase
{
namespace blocksstable
{
class MacroBlockId;
}
namespace memtable
{
class ObMemtable;
struct ObMtStat;
}
namespace storage
{
class ObLS;
class ObTablet;
struct ObTabletStatKey;
}

namespace compaction
{

struct ObScheduleStatistics
{
public:
  ObScheduleStatistics() { reset(); }
  ~ObScheduleStatistics() {}
  OB_INLINE void reset()
  {
    add_weak_read_ts_event_flag_ = false;
    check_weak_read_ts_cnt_ = 0;
    start_timestamp_ = 0;
    clear_tablet_cnt();
  }
  OB_INLINE void clear_tablet_cnt()
  {
    schedule_dag_cnt_ = 0;
    submit_clog_cnt_ = 0;
    finish_cnt_ = 0;
    wait_rs_validate_cnt_ = 0;
  }
  OB_INLINE void start_merge()
  {
    add_weak_read_ts_event_flag_ = true;
    check_weak_read_ts_cnt_ = 0;
    start_timestamp_ = ObTimeUtility::fast_current_time();
    clear_tablet_cnt();
  }
  TO_STRING_KV(K_(schedule_dag_cnt), K_(submit_clog_cnt), K_(finish_cnt), K_(wait_rs_validate_cnt));
  bool add_weak_read_ts_event_flag_;
  int64_t check_weak_read_ts_cnt_;
  int64_t start_timestamp_;
  int64_t schedule_dag_cnt_;
  int64_t submit_clog_cnt_;
  int64_t finish_cnt_;
  int64_t wait_rs_validate_cnt_;
};

class ObFastFreezeChecker
{
public:
  ObFastFreezeChecker();
  virtual ~ObFastFreezeChecker();
  void reset();
  OB_INLINE bool need_check() const { return enable_fast_freeze_; }
  void reload_config(const bool enable_fast_freeze);
  int check_need_fast_freeze(const storage::ObTablet &tablet, bool &need_fast_freeze);
  TO_STRING_KV(K_(enable_fast_freeze));
private:
  void check_hotspot_need_fast_freeze(
      const memtable::ObMemtable &memtable,
      bool &need_fast_freeze);
  void check_tombstone_need_fast_freeze(
      const storage::ObTablet &tablet,
      const memtable::ObMemtable &memtable,
      bool &need_fast_freeze);
  void try_update_tablet_threshold(
      const storage::ObTabletStatKey &key,
      const memtable::ObMtStat &mt_stat,
      const int64_t memtable_create_timestamp,
      int64_t &adaptive_threshold);
private:
  static const int64_t FAST_FREEZE_INTERVAL_US = 300 * 1000 * 1000L;  //300s
  static const int64_t PRINT_LOG_INVERVAL = 2 * 60 * 1000 * 1000L; // 2m
  static const int64_t TOMBSTONE_DEFAULT_ROW_COUNT = 250000;
  static const int64_t TOMBSTONE_MAX_ROW_COUNT = 500000;
  static const int64_t TOMBSTONE_STEP_ROW_COUNT = 50000;
  common::hash::ObHashMap<ObTabletStatKey, int64_t> store_map_;
  bool enable_fast_freeze_;
};

struct ObProhibitScheduleMediumMap
{
public:

  enum class ProhibitFlag : int32_t
  {
    TRANSFER = 0,
    MEDIUM = 1,
    FLAG_MAX
  };
  struct ProhibitMediumStatus
  {
    ProhibitMediumStatus()
      : flag_(ProhibitFlag::MEDIUM),
        ref_(0)
    {}
    explicit ProhibitMediumStatus(ProhibitFlag flag)
      : flag_(flag),
        ref_(1)
    {}
    ~ProhibitMediumStatus() { reset(); }
    OB_INLINE void reset() { ref_ = 0; }
    OB_INLINE bool is_valid() const { return flag_ >= ProhibitFlag::TRANSFER && flag_ < ProhibitFlag::FLAG_MAX; }
    OB_INLINE bool is_transfer() const { return ProhibitFlag::TRANSFER == flag_; }
    OB_INLINE bool is_medium() const { return ProhibitFlag::MEDIUM == flag_; }
    OB_INLINE bool is_equal(const ProhibitFlag &type) { return type == flag_; }
    OB_INLINE bool can_erase() { return 0 == ref_; }
    OB_INLINE void dec_ref() { --ref_; }
    OB_INLINE void inc_ref() { ++ref_; }
    TO_STRING_KV(K_(flag), K_(ref));
    ProhibitFlag flag_;
    int32_t ref_;
  };

  static const char *ProhibitFlagStr[];
  ObProhibitScheduleMediumMap();
  ~ObProhibitScheduleMediumMap() { destroy(); }
  int init();
  void destroy();
  int clear_flag(const share::ObLSID &ls_id, const ProhibitFlag &input_flag);
  int add_flag(const share::ObLSID &ls_id, const ProhibitFlag &input_flag);
  int64_t to_string(char *buf, const int64_t buf_len) const;
  int64_t get_transfer_flag_cnt() const;
private:
  static const int64_t PRINT_LOG_INVERVAL = 2 * 60 * 1000 * 1000L; // 2m

  int64_t transfer_flag_cnt_;
  mutable obsys::ObRWLock lock_;
  common::hash::ObHashMap<share::ObLSID, ProhibitMediumStatus> ls_id_map_;
};

class ObTenantTabletScheduler
{
public:
  ObTenantTabletScheduler();
  ~ObTenantTabletScheduler();
  static int mtl_init(ObTenantTabletScheduler* &scheduler);

  int init();
  int start();
  void destroy();
  void reset();
  void stop();
  void wait() { timer_task_mgr_.wait(); }
  bool is_stop() const { return is_stop_; }
  int reload_tenant_config();
  bool enable_adaptive_compaction() const { return enable_adaptive_compaction_; }
  bool enable_adaptive_merge_schedule() const { return enable_adaptive_merge_schedule_; }
  int64_t get_error_tablet_cnt() { return ATOMIC_LOAD(&error_tablet_cnt_); }
  void clear_error_tablet_cnt() { ATOMIC_STORE(&error_tablet_cnt_, 0); }
  void update_error_tablet_cnt(const int64_t delta_cnt)
  {
    // called when check tablet checksum error
    (void)ATOMIC_AAF(&error_tablet_cnt_, delta_cnt);
  }
  OB_INLINE bool schedule_ignore_error(const int ret)
  {
    return OB_ITER_END == ret
      || OB_STATE_NOT_MATCH == ret
      || OB_LS_NOT_EXIST == ret;
  }
  // major merge status control
  void stop_major_merge();
  void resume_major_merge();
  OB_INLINE bool could_major_merge_start() const { return ATOMIC_LOAD(&major_merge_status_); }
  // The transfer task sets the flag that prohibits the scheduling of medium when the log stream is src_ls of transfer
  int stop_ls_schedule_medium(const share::ObLSID &ls_id);
  int clear_prohibit_medium_flag(const share::ObLSID &ls_id, const ObProhibitScheduleMediumMap::ProhibitFlag &input_flag)
  {
    return prohibit_medium_map_.clear_flag(ls_id, input_flag);
  }
  int ls_start_schedule_medium(const share::ObLSID &ls_id, bool &ls_could_schedule_medium);
  const ObProhibitScheduleMediumMap& get_prohibit_medium_ls_map() const {
    return prohibit_medium_map_;
  }
  int64_t get_frozen_version() const;
  int64_t get_inner_table_merged_scn() const { return ATOMIC_LOAD(&inner_table_merged_scn_); }
  int get_min_data_version(uint64_t &min_data_version);
  void set_inner_table_merged_scn(const int64_t merged_scn)
  {
    return ATOMIC_STORE(&inner_table_merged_scn_, merged_scn);
  }
  int64_t get_bf_queue_size() const { return bf_queue_.task_count(); }
  int schedule_merge(const int64_t broadcast_version);
  int update_upper_trans_version_and_gc_sstable();
  int check_ls_compaction_finish(const share::ObLSID &ls_id);
  int schedule_all_tablets_minor();

  int gc_info();
  int set_max();

  // Schedule an async task to build bloomfilter for the given macro block.
  // The bloomfilter build task will be ignored if a same build task exists in the queue.
  int schedule_build_bloomfilter(
      const uint64_t table_id,
      const blocksstable::MacroBlockId &macro_id,
      const int64_t prefix_len);
  static bool check_tx_table_ready(ObLS &ls, const share::SCN &check_scn);
  static int check_ls_state(ObLS &ls, bool &need_merge);
  static int fill_minor_compaction_param(
      const ObTabletHandle &tablet_handle,
      const ObGetMergeTablesResult &result,
      const int64_t total_sstable_cnt,
      const int64_t parallel_dag_cnt,
      const int64_t create_time,
      compaction::ObTabletMergeDagParam &param);
  static int check_ls_state_in_major(ObLS &ls, bool &need_merge);
  template <class T>
  static int schedule_tablet_minor_merge(
      ObLSHandle &ls_handle,
      ObTabletHandle &tablet_handle);
  static int schedule_tablet_meta_major_merge(
      ObLSHandle &ls_handle,
      ObTabletHandle &tablet_handle,
      const compaction::ObMediumCompactionInfoList &medium_list);
  template <class T>
  static int schedule_merge_execute_dag(
      const compaction::ObTabletMergeDagParam &param,
      ObLSHandle &ls_handle,
      ObTabletHandle &tablet_handle,
      const ObGetMergeTablesResult &result,
      T *&dag,
      const bool add_into_scheduler = true);
  static bool check_weak_read_ts_ready(
      const int64_t &merge_version,
      ObLS &ls);
  static int schedule_merge_dag(
      const share::ObLSID &ls_id,
      const storage::ObTablet &tablet,
      const ObMergeType merge_type,
      const int64_t &merge_snapshot_version,
      const bool is_tenant_major_merge = false);
  static int schedule_tablet_ddl_major_merge(
      ObTabletHandle &tablet_handle);

  int get_min_dependent_schema_version(int64_t &min_schema_version);

  int try_schedule_tablet_medium_merge(
    const share::ObLSID &ls_id,
    const common::ObTabletID &tablet_id,
    const bool is_rebuild_column_group);
  int schedule_next_round_for_leader(
    const ObIArray<compaction::ObTabletCheckInfo> &tablet_ls_infos,
    const ObIArray<compaction::ObTabletCheckInfo> &finish_tablet_ls_infos);

private:
  friend struct ObTenantTabletSchedulerTaskMgr;
  int schedule_next_medium_for_leader(
    ObLS &ls,
    ObTabletHandle &tablet_handle,
    const share::SCN &weak_read_ts,
    const compaction::ObMediumCompactionInfoList *medium_info_list,
    const int64_t major_merge_version);
  int schedule_all_tablets_medium();
  int schedule_ls_medium_merge(
      const int64_t merge_version,
      ObLSHandle &ls_handle,
      bool &all_ls_weak_read_ts_ready);
  OB_INLINE int schedule_tablet_medium(
    ObLS &ls,
    ObTabletHandle &tablet_handle,
    const int64_t major_frozen_scn,
    const share::SCN &weak_read_ts,
    const bool could_major_merge,
    const bool ls_could_schedule_medium,
    const int64_t merge_version,
    const bool enable_adaptive_compaction,
    bool &is_leader,
    bool &tablet_merge_finish,
    bool &tablet_need_freeze_flag);
  int after_schedule_tenant_medium(
    const int64_t merge_version,
    bool all_ls_weak_read_ts_ready);
  int update_major_progress(const int64_t merge_version);
  bool get_enable_adaptive_compaction();
  int update_tablet_report_status(
    const bool tablet_merge_finish,
    ObLS &ls,
    ObTablet &tablet);
  int schedule_ls_minor_merge(ObLSHandle &ls_handle);
  OB_INLINE int schedule_tablet_minor(
    ObLSHandle &ls_handle,
    ObTabletHandle tablet_handle,
    bool &schedule_minor_flag,
    bool &need_fast_freeze_flag);
  int update_report_scn_as_ls_leader(
      ObLS &ls);

  int get_ls_tablet_medium_list(
      const share::ObLSID &ls_id,
      const ObTabletID &tablet_id,
      common::ObArenaAllocator &allocator,
      ObLSHandle &ls_handle,
      ObTabletHandle &tablet_handle,
      const compaction::ObMediumCompactionInfoList *&medium_list,
      share::SCN &weak_read_ts);
public:
  static const int64_t INIT_COMPACTION_SCN = 1;
  typedef common::ObSEArray<ObGetMergeTablesResult, compaction::ObPartitionMergePolicy::OB_MINOR_PARALLEL_INFO_ARRAY_SIZE> MinorParallelResultArray;
private:
  static const int64_t BLOOM_FILTER_LOAD_BUILD_THREAD_CNT = 1;
  static const int64_t NO_MAJOR_MERGE_TYPE_CNT = 2;
  static const int64_t TX_TABLE_NO_MAJOR_MERGE_TYPE_CNT = 1;
  static const int64_t BF_TASK_QUEUE_SIZE = 10L * 1000;
  static const int64_t BF_TASK_MAP_SIZE = 10L * 1000;
  static const int64_t BF_TASK_TOTAL_LIMIT = 512L * 1024L * 1024L;
  static const int64_t BF_TASK_HOLD_LIMIT = 256L * 1024L * 1024L;
  static const int64_t BF_TASK_PAGE_SIZE = common::OB_MALLOC_MIDDLE_BLOCK_SIZE; //64K

  static constexpr ObMergeType MERGE_TYPES[] = {MINOR_MERGE, HISTORY_MINOR_MERGE};
  static const int64_t ADD_LOOP_EVENT_INTERVAL = 120 * 1000 * 1000L; // 120s
  static const int64_t PRINT_LOG_INVERVAL = 2 * 60 * 1000 * 1000L; // 2m
  static const int64_t WAIT_MEDIUM_CHECK_THRESHOLD = 10 * 60 * 1000 * 1000 * 1000L; // 10m
  static const int64_t MERGE_BACTH_FREEZE_CNT = 100L;
private:
  bool is_inited_;
  bool major_merge_status_;
  bool is_stop_;
  bool enable_adaptive_compaction_;
  bool enable_adaptive_merge_schedule_;
  common::ObDedupQueue bf_queue_;
  mutable obsys::ObRWLock frozen_version_lock_;
  int64_t frozen_version_;
  int64_t merged_version_; // the merged major version of the local server, may be not accurate after reboot
  int64_t inner_table_merged_scn_;
  uint64_t min_data_version_;
  ObScheduleStatistics schedule_stats_;
  ObFastFreezeChecker fast_freeze_checker_;
  ObCompactionScheduleIterator minor_ls_tablet_iter_;
  ObCompactionScheduleIterator medium_ls_tablet_iter_;
  ObCompactionScheduleIterator gc_sst_tablet_iter_;
  int64_t schedule_tablet_batch_size_;
  int64_t error_tablet_cnt_; // for diagnose
  ObProhibitScheduleMediumMap prohibit_medium_map_;
  ObTenantTabletSchedulerTaskMgr timer_task_mgr_;
};

} // namespace compaction
} // namespace oceanbase

#endif // STORAGE_OB_TENANT_TABLET_SCHEDULER_H_
