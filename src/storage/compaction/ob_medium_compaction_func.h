//Copyright (c) 2021 OceanBase
// OceanBase is licensed under Mulan PubL v2.
// You can use this software according to the terms and conditions of the Mulan PubL v2.
// You may obtain a copy of Mulan PubL v2 at:
//          http://license.coscl.org.cn/MulanPubL-2.0
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
// See the Mulan PubL v2 for more details.
#ifndef OB_STORAGE_COMPACTION_MEDIUM_COMPACTION_FUNC_H_
#define OB_STORAGE_COMPACTION_MEDIUM_COMPACTION_FUNC_H_

#include "storage/ls/ob_ls.h"
#include "storage/compaction/ob_partition_merge_policy.h"
#include "share/tablet/ob_tablet_filter.h"
#include "share/ob_tablet_meta_table_compaction_operator.h"
#include "storage/tablet/ob_tablet.h"
#include "storage/compaction/ob_tenant_tablet_scheduler.h"
#include "storage/compaction/ob_tenant_medium_checker.h"
#include "storage/compaction/ob_tablet_merge_ctx.h"

namespace oceanbase
{
namespace compaction
{

class ObMediumCompactionScheduleFunc
{
public:
  ObMediumCompactionScheduleFunc(
    ObLS &ls,
    ObTabletHandle &tablet_handle,
    const SCN &weak_read_ts,
    const ObMediumCompactionInfoList &medium_info_list,
    ObScheduleStatistics *schedule_stat,
    const bool is_rebuild_column_group = false)
    : allocator_("MediumSchedule"),
      ls_(ls),
      tablet_handle_(tablet_handle),
      weak_read_ts_(weak_read_ts.get_val_for_tx()),
      medium_info_list_(&medium_info_list),
      schedule_stat_(schedule_stat),
      is_rebuild_column_group_(is_rebuild_column_group)
  {}
  ~ObMediumCompactionScheduleFunc() {}

  static int schedule_tablet_medium_merge(
      ObLS &ls,
      ObTablet &tablet,
      bool &tablet_need_freeze_flag,
      bool &create_dag_flag,
      const int64_t major_frozen_scn = 0,
      const bool scheduler_called = false);
  static int read_medium_info_from_list(
      const ObMediumCompactionInfoList &medium_list,
      const int64_t major_frozen_snapshot,
      const int64_t last_major_snapshot,
      ObMediumCompactionInfo::ObCompactionType &compaction_type,
      int64_t &schedule_scn);
  static int is_election_leader(const share::ObLSID &ls_id, bool &ls_election_leader);
  static int get_max_sync_medium_scn(
    const ObTablet &tablet,
    const ObMediumCompactionInfoList &medium_list,
    int64_t &max_sync_medium_scn);
  static int get_table_schema_to_merge(
    ObMultiVersionSchemaService &schema_service,
    const ObTablet &tablet,
    const int64_t schema_version,
    ObIAllocator &allocator,
    ObMediumCompactionInfo &medium_info);
  static int batch_check_medium_finish(
    hash::ObHashMap<ObLSID, share::ObLSInfo> &ls_info_map,
    ObIArray<ObTabletCheckInfo> &finish_tablet_ls_infos,
    const ObIArray<ObTabletCheckInfo> &tablet_ls_infos,
    ObCompactionTimeGuard &time_guard);

  int schedule_next_medium_for_leader(
    const int64_t major_snapshot,
    const bool force_schedule);

  int decide_medium_snapshot(
      const ObAdaptiveMergePolicy::AdaptiveMergeReason merge_reason);

  int64_t to_string(char* buf, const int64_t buf_len) const;
protected:
  static int get_status_from_inner_table(
      const ObLSID &ls_id,
      const ObTabletID &tablet_id,
      share::ObTabletCompactionScnInfo &ret_info);
  int prepare_medium_info(
    const ObGetMergeTablesResult &result,
    const int64_t schema_version,
    ObMediumCompactionInfo &medium_info);
  int init_parallel_range_and_schema_changed(
      const ObGetMergeTablesResult &result,
      ObMediumCompactionInfo &medium_info);
  int init_schema_changed(
    ObMediumCompactionInfo &medium_info);
  static int get_result_for_major(
      ObTablet &tablet,
      const ObMediumCompactionInfo &medium_info,
      ObGetMergeTablesResult &result);
  int prepare_iter(
      const ObGetMergeTablesResult &result,
      ObTableStoreIterator &table_iter);
  int submit_medium_clog(ObMediumCompactionInfo &medium_info);
  static int batch_check_medium_meta_table(
      const ObIArray<ObTabletCheckInfo> &tablet_ls_infos,
      hash::ObHashMap<ObLSID, share::ObLSInfo> &ls_info_map,
      ObIArray<ObTabletCheckInfo> &finish_tablet_ls,
      ObCompactionTimeGuard &time_guard);
  static int check_medium_meta_table(
      const int64_t medium_snapshot,
      const ObTabletInfo &tablet_info,
      const share::ObTabletReplicaFilterHolder &filters,
      hash::ObHashMap<ObLSID, share::ObLSInfo> &ls_info_map,
      bool &merge_finish);
  static int init_tablet_filters(share::ObTabletReplicaFilterHolder &filters);
  static int check_medium_checksum(
      const ObIArray<ObTabletReplicaChecksumItem> &checksum_items,
      ObIArray<ObTabletLSPair> &error_pairs,
      int64_t &item_idx,
      int &check_ret);
  static int batch_check_medium_checksum(
      const ObIArray<ObTabletCheckInfo> &tablet_ls_infos,
      const ObIArray<ObTabletReplicaChecksumItem> &checksum_items);
  static int choose_medium_snapshot(
      const ObMediumCompactionScheduleFunc &func,
      ObLS &ls,
      ObTablet &tablet,
      const ObAdaptiveMergePolicy::AdaptiveMergeReason &merge_reason,
      ObArenaAllocator &allocator,
      ObMediumCompactionInfo &medium_info,
      ObGetMergeTablesResult &result,
      int64_t &schema_version);
  static int choose_major_snapshot(
      const ObMediumCompactionScheduleFunc &func,
      ObLS &ls,
      ObTablet &tablet,
      const ObAdaptiveMergePolicy::AdaptiveMergeReason &merge_reason,
      ObArenaAllocator &allocator,
      ObMediumCompactionInfo &medium_info,
      ObGetMergeTablesResult &result,
      int64_t &schema_version);
  static int switch_to_choose_medium_snapshot(
    const ObMediumCompactionScheduleFunc &func,
    ObArenaAllocator &allocator,
    ObLS &ls,
    ObTablet &tablet,
    const int64_t freeze_version,
    ObMediumCompactionInfo &medium_info,
    int64_t &schema_version);

  static int check_need_merge_and_schedule(
      ObLS &ls,
      ObTablet &tablet,
      const int64_t schedule_scn,
      const ObMediumCompactionInfo::ObCompactionType compaction_type,
      bool &tablet_need_freeze_flag,
      bool &create_dag_flag);
  int schedule_next_medium_primary_cluster(
    const int64_t major_snapshot,
    const bool force_schedule);

  int choose_new_medium_snapshot(
    const int64_t max_reserved_snapshot,
    ObMediumCompactionInfo &medium_info,
    ObGetMergeTablesResult &result,
    int64_t &schema_version);
  int get_max_reserved_snapshot(int64_t &max_reserved_snapshot);
  static int get_table_id(
      ObMultiVersionSchemaService &schema_service,
      const ObTabletID &tablet_id,
      const int64_t schema_version,
      uint64_t &table_id);

  int check_medium_scn_valid_and_fix(ObMediumCompactionInfo &medium_info);
  int choose_medium_scn_for_user_request(
    ObMediumCompactionInfo &medium_info,
    ObGetMergeTablesResult &result,
    int64_t &schema_version);
  int get_adaptive_reason(
    const int64_t schedule_major_snapshot,
    const bool force_schedule,
    ObAdaptiveMergePolicy::AdaptiveMergeReason &adaptive_merge_reason);
  static const int64_t DEFAULT_SCHEDULE_MEDIUM_INTERVAL = 60L * 1000L * 1000L; // 60s
  static constexpr double SCHEDULE_RANGE_INC_ROW_COUNT_PERCENRAGE_THRESHOLD = 0.2;
  static const int64_t SCHEDULE_RANGE_ROW_COUNT_THRESHOLD = 1000 * 1000L; // 100w
  static const int64_t MEDIUM_FUNC_CNT = 2;
  typedef int (*ChooseMediumScn)(
      const ObMediumCompactionScheduleFunc &func,
      ObLS &ls,
      ObTablet &tablet,
      const ObAdaptiveMergePolicy::AdaptiveMergeReason &merge_reason,
      ObArenaAllocator &allocator,
      ObMediumCompactionInfo &medium_info,
      ObGetMergeTablesResult &result,
      int64_t &schema_version);
  static ChooseMediumScn choose_medium_scn[MEDIUM_FUNC_CNT];
  static bool is_user_request(const ObAdaptiveMergePolicy::AdaptiveMergeReason merge_reason)
  {
    return ObAdaptiveMergePolicy::USER_REQUEST == merge_reason
      || ObAdaptiveMergePolicy::REBUILD_COLUMN_GROUP == merge_reason;
  }

private:
  ObArenaAllocator allocator_;
  ObLS &ls_;
  ObTabletHandle tablet_handle_;
  int64_t weak_read_ts_; // weak_read_ts_ should get before tablet
  const ObMediumCompactionInfoList *medium_info_list_;
  ObScheduleStatistics *schedule_stat_;
  const bool is_rebuild_column_group_;
};

} //namespace compaction
} // namespace oceanbase

#endif // OB_STORAGE_COMPACTION_MEDIUM_COMPACTION_FUNC_H_
