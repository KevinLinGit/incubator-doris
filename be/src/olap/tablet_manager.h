// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#ifndef DORIS_BE_SRC_OLAP_TABLET_MANAGER_H
#define DORIS_BE_SRC_OLAP_TABLET_MANAGER_H

#include <ctime>
#include <list>
#include <map>
#include <mutex>
#include <condition_variable>
#include <set>
#include <string>
#include <vector>
#include <thread>

#include <rapidjson/document.h>
#include <pthread.h>

#include "agent/status.h"
#include "common/status.h"
#include "gen_cpp/AgentService_types.h"
#include "gen_cpp/BackendService_types.h"
#include "gen_cpp/MasterService_types.h"
#include "olap/atomic.h"
#include "olap/lru_cache.h"
#include "olap/olap_common.h"
#include "olap/olap_define.h"
#include "olap/tablet.h"
#include "olap/olap_meta.h"
#include "olap/options.h"

namespace doris {

class Tablet;
class DataDir;

// TabletManager provides get,add, delete tablet method for storage engine
class TabletManager {
public:
    TabletManager();
    ~TabletManager() {
        _tablet_map.clear();
        _global_tablet_id = 0;
    }

    // Add a tablet pointer to StorageEngine
    // If force, drop the existing tablet add this new one
    //
    // Return OLAP_SUCCESS, if run ok
    //        OLAP_ERR_TABLE_INSERT_DUPLICATION_ERROR, if find duplication
    //        OLAP_ERR_NOT_INITED, if not inited
    OLAPStatus add_tablet(TTabletId tablet_id, SchemaHash schema_hash,
                         const TabletSharedPtr& tablet, bool force);

    void cancel_unfinished_schema_change();

    bool check_tablet_id_exist(TTabletId tablet_id);

    void clear();
    
    // Add empty data for Tablet
    //
    // Return OLAP_SUCCESS, if run ok
    OLAPStatus create_init_version(
            TTabletId tablet_id, SchemaHash schema_hash,
            Version version, VersionHash version_hash);

    OLAPStatus create_tablet(const TCreateTabletReq& request, 
                             std::vector<DataDir*> stores);

    // Create new tablet for StorageEngine
    //
    // Return Tablet *  succeeded; Otherwise, return NULL if failed
    TabletSharedPtr create_tablet(const TCreateTabletReq& request,
                              const std::string* ref_root_path, 
                              const bool is_schema_change_tablet,
                              const TabletSharedPtr ref_tablet, 
                              std::vector<DataDir*> stores);

    // ######################### ALTER TABLE BEGIN #########################
    // The following interfaces are all about alter tablet operation, 
    // the main logical is that generating a new tablet with different
    // schema on base tablet.
    
    // Create rollup tablet on base tablet, after create_rollup_tablet,
    // both base tablet and new tablet is effective.
    //
    // @param [in] request specify base tablet, new tablet and its schema
    // @return OLAP_SUCCESS if submit success
    OLAPStatus create_rollup_tablet(const TAlterTabletReq& request);

    // Show status of all alter tablet operation.
    // 
    // @param [in] tablet_id & schema_hash specify a tablet
    // @return alter tablet status
    AlterTableStatus show_alter_tablet_status(TTabletId tablet_id, TSchemaHash schema_hash);

    // Drop a tablet by description
    // If set keep_files == true, files will NOT be deleted when deconstruction.
    // Return OLAP_SUCCESS, if run ok
    //        OLAP_ERR_TABLE_DELETE_NOEXIST_ERROR, if tablet not exist
    //        OLAP_ERR_NOT_INITED, if not inited
    OLAPStatus drop_tablet(
            TTabletId tablet_id, SchemaHash schema_hash, bool keep_files = false);

    OLAPStatus drop_tablets_on_error_root_path(const std::vector<TabletInfo>& tablet_info_vec);

    TabletSharedPtr find_best_tablet_to_compaction(CompactionType compaction_type);

    // Get tablet pointer
    TabletSharedPtr get_tablet(TTabletId tablet_id, SchemaHash schema_hash, bool load_tablet = true);

    OLAPStatus get_tablets_by_id(TTabletId tablet_id, std::list<TabletSharedPtr>* tablet_list);  

    void get_tablet_stat(TTabletStatResult& result);

    // parse tablet header msg to generate tablet object
    OLAPStatus load_tablet_from_header(DataDir* data_dir, TTabletId tablet_id,
                TSchemaHash schema_hash, const std::string& header);

    OLAPStatus load_one_tablet(DataDir* data_dir,
                               TTabletId tablet_id,
                               SchemaHash schema_hash,
                               const std::string& schema_hash_path,
                               bool force = false);
    
    void release_schema_change_lock(TTabletId tablet_id);
    
    // 获取所有tables的名字
    //
    // Return OLAP_SUCCESS, if run ok
    //        OLAP_ERR_INPUT_PARAMETER_ERROR, if tables is null
    OLAPStatus report_tablet_info(TTabletInfo* tablet_info);

    OLAPStatus report_all_tablets_info(std::map<TTabletId, TTablet>* tablets_info);

    OLAPStatus start_trash_sweep();
    // Prevent schema change executed concurrently.
    bool try_schema_change_lock(TTabletId tablet_id);

    void update_root_path_info(std::map<std::string, DataDirInfo>* path_map, int* tablet_counter);

    void update_storage_medium_type_count(uint32_t storage_medium_type_count);

private:
    void _build_tablet_info(TabletSharedPtr tablet, TTabletInfo* tablet_info);
    
    void _build_tablet_stat();
    
    OLAPStatus _create_init_version(TabletSharedPtr tablet, const TCreateTabletReq& request);
    

    OLAPStatus _create_new_tablet_header(const TCreateTabletReq& request,
                                             DataDir* store,
                                             const bool is_schema_change_tablet,
                                             const TabletSharedPtr ref_tablet,
                                             TabletMeta* header);

    // Drop tablet directly with check schema change info.
    OLAPStatus _drop_tablet_directly(TTabletId tablet_id, TSchemaHash schema_hash, bool keep_files = false);
    
    OLAPStatus _drop_tablet_directly_unlocked(TTabletId tablet_id, TSchemaHash schema_hash, bool keep_files = false);

    TabletSharedPtr _get_tablet_with_no_lock(TTabletId tablet_id, SchemaHash schema_hash);

private:
    struct TableInstances {
        Mutex schema_change_lock;
        std::list<TabletSharedPtr> table_arr;
    };
    typedef std::map<int64_t, TableInstances> tablet_map_t;
    RWMutex _tablet_map_lock;
    tablet_map_t _tablet_map;
    size_t _global_tablet_id;
    std::map<std::string, DataDir*> _store_map;

    // cache to save tablets' statistics, such as data size and row
    // TODO(cmy): for now, this is a naive implementation
    std::map<int64_t, TTabletStat> _tablet_stat_cache;
    // last update time of tablet stat cache
    int64_t _tablet_stat_cache_update_time_ms;

    uint32_t _available_storage_medium_type_count;
};

}  // namespace doris

#endif // DORIS_BE_SRC_OLAP_TABLET_MANAGER_H
