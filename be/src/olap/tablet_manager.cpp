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

#include "olap/tablet_manager.h"

#include <signal.h>

#include <algorithm>
#include <cstdio>
#include <new>
#include <queue>
#include <set>
#include <random>
#include <regex>
#include <stdlib.h>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/filesystem.hpp>
#include <rapidjson/document.h>
#include <thrift/protocol/TDebugProtocol.h>

#include "agent/file_downloader.h"
#include "olap/base_compaction.h"
#include "olap/cumulative_compaction.h"
#include "olap/lru_cache.h"
#include "olap/tablet_meta.h"
#include "olap/tablet_meta_manager.h"
#include "olap/push_handler.h"
#include "olap/reader.h"
#include "olap/schema_change.h"
#include "olap/data_dir.h"
#include "olap/utils.h"
#include "olap/rowset/column_data_writer.h"
#include "olap/rowset/rowset_id_generator.h"
#include "util/time.h"
#include "util/doris_metrics.h"
#include "util/pretty_printer.h"

using apache::thrift::ThriftDebugString;
using boost::filesystem::canonical;
using boost::filesystem::directory_iterator;
using boost::filesystem::path;
using boost::filesystem::recursive_directory_iterator;
using std::back_inserter;
using std::copy;
using std::inserter;
using std::list;
using std::map;
using std::nothrow;
using std::pair;
using std::priority_queue;
using std::set;
using std::set_difference;
using std::string;
using std::stringstream;
using std::vector;

namespace doris {

bool _sort_tablet_by_creation_time(const TabletSharedPtr& a, const TabletSharedPtr& b) {
    return a->creation_time() < b->creation_time();
}

TabletManager::TabletManager()
    : _tablet_stat_cache_update_time_ms(0),
      _available_storage_medium_type_count(0) { }

OLAPStatus TabletManager::_add_tablet_unlock(TTabletId tablet_id, SchemaHash schema_hash,
                                 const TabletSharedPtr& tablet, bool update_meta, bool force) {
    OLAPStatus res = OLAP_SUCCESS;
    VLOG(3) << "begin to add tablet to TabletManager. "
            << "tablet_id=" << tablet_id << ", schema_hash=" << schema_hash
            << ", force=" << force;

    TabletSharedPtr table_item = nullptr;
    for (TabletSharedPtr item : _tablet_map[tablet_id].table_arr) {
        if (item->equal(tablet_id, schema_hash)) {
            table_item = item;
            break;
        }
    }

    if (table_item == nullptr) {
        LOG(INFO) << "not find exist tablet just add it to map"
                  << " tablet_id = " << tablet_id
                  << " schema_hash = " << schema_hash;
        return _add_tablet_to_map(tablet_id, schema_hash, tablet, update_meta, false, false);
    }

    if (!force) {
        if (table_item->tablet_path() == tablet->tablet_path()) {
            LOG(WARNING) << "add the same tablet twice! tablet_id="
                         << tablet_id << " schema_hash=" << schema_hash;
            return OLAP_ERR_ENGINE_INSERT_EXISTS_TABLE;
        }
        if (table_item->data_dir() == tablet->data_dir()) {
            LOG(WARNING) << "add tablet with same data dir twice! tablet_id="
                         << tablet_id << " schema_hash=" << schema_hash;
            return OLAP_ERR_ENGINE_INSERT_EXISTS_TABLE;
        }
    }

    table_item->obtain_header_rdlock();
    const RowsetSharedPtr old_rowset = table_item->rowset_with_max_version();
    const RowsetSharedPtr new_rowset = tablet->rowset_with_max_version();

    // if new tablet is empty, it is a newly created schema change tablet
    // the old tablet is dropped before add tablet. it should not exist old tablet
    if (new_rowset == nullptr) {
        table_item->release_header_lock();
        // it seems useless to call unlock and return here.
        // it could prevent error when log level is changed in the future.
        LOG(FATAL) << "new tablet is empty and old tablet exists. it should not happen."
                   << " tablet_id=" << tablet_id << " schema_hash=" << schema_hash;
        return OLAP_ERR_ENGINE_INSERT_EXISTS_TABLE;
    }
    int64_t old_time = old_rowset == nullptr ? -1 : old_rowset->creation_time();
    int64_t new_time = new_rowset->creation_time();
    int32_t old_version = old_rowset == nullptr ? -1 : old_rowset->end_version();
    int32_t new_version = new_rowset->end_version();
    table_item->release_header_lock();

    /*
     * In restore process, we replace all origin files in tablet dir with
     * the downloaded snapshot files. Than we try to reload tablet header.
     * force == true means we forcibly replace the Tablet in _tablet_map
     * with the new one. But if we do so, the files in the tablet dir will be
     * dropped when the origin Tablet deconstruct.
     * So we set keep_files == true to not delete files when the
     * origin Tablet deconstruct.
     */
    bool keep_files = force ? true : false;
    if (force || (new_version > old_version
            || (new_version == old_version && new_time > old_time))) {
        // check if new tablet's meta is in store and add new tablet's meta to meta store
        res = _add_tablet_to_map(tablet_id, schema_hash, tablet, update_meta, keep_files, true);
    } else {
        res = OLAP_ERR_ENGINE_INSERT_EXISTS_TABLE;
    }
    LOG(WARNING) << "add duplicated tablet. force=" << force << ", res=" << res
            << ", tablet_id=" << tablet_id << ", schema_hash=" << schema_hash
            << ", old_version=" << old_version << ", new_version=" << new_version
            << ", old_time=" << old_time << ", new_time=" << new_time
            << ", old_tablet_path=" << table_item->tablet_path()
            << ", new_tablet_path=" << tablet->tablet_path();

    return res;
} // add_tablet

OLAPStatus TabletManager::_add_tablet_to_map(TTabletId tablet_id, SchemaHash schema_hash,
                                 const TabletSharedPtr& tablet, bool update_meta, 
                                 bool keep_files, bool drop_old) {
     // check if new tablet's meta is in store and add new tablet's meta to meta store
    OLAPStatus res = OLAP_SUCCESS;
    if (update_meta) {
        res = TabletMetaManager::save(tablet->data_dir(), 
            tablet->tablet_id(), tablet->schema_hash(), tablet->tablet_meta());
        if (res != OLAP_SUCCESS) {
            LOG(WARNING) << "failed to save new tablet's meta to meta store" 
                            << " tablet_id = " << tablet_id
                            << " schema_hash = " << schema_hash;
            return res;
        }
    }
    if (drop_old) {
        // if the new tablet is fresher than current one
        // then delete current one and add new one
        res = _drop_tablet_unlock(tablet_id, schema_hash, keep_files);
        if (res != OLAP_SUCCESS) {
            LOG(WARNING) << "failed to drop old tablet when add new tablet"
                            << " tablet_id = " << tablet_id
                            << " schema_hash = " << schema_hash;
            return res;
        }
    }
    // Register tablet into StorageEngine, so that we can manage tablet from
    // the perspective of root path.
    // Example: unregister all tables when a bad disk found.
    res = tablet->register_tablet_into_dir();
    if (res != OLAP_SUCCESS) {
        LOG(WARNING) << "fail to register tablet into StorageEngine. res=" << res
                        << ", data_dir=" << tablet->data_dir()->path();
        return res;
    }
    _tablet_map[tablet_id].table_arr.push_back(tablet);
    _tablet_map[tablet_id].table_arr.sort(_sort_tablet_by_creation_time);
    LOG(INFO) << "add tablet to map successfully" 
                << " tablet_id = " << tablet_id
                << " schema_hash = " << schema_hash;   
    return res;                              
}

// this method is called when engine restarts so that not add any locks
void TabletManager::cancel_unfinished_schema_change() {
    // Schema Change在引擎退出时schemachange信息还保存在在Header里，
    // 引擎重启后，需清除schemachange信息，上层会重做
    uint64_t canceled_num = 0;
    LOG(INFO) << "begin to cancel unfinished schema change.";

    TTabletId tablet_id;
    TSchemaHash schema_hash;
    vector<Version> schema_change_versions;

    for (const auto& tablet_instance : _tablet_map) {
        for (TabletSharedPtr tablet : tablet_instance.second.table_arr) {
            if (tablet == nullptr) {
                LOG(WARNING) << "tablet does not exist. tablet_id=" << tablet_instance.first;
                continue;
            }
            AlterTabletTaskSharedPtr alter_task = tablet->alter_task();
            if (alter_task == nullptr) {
                continue;
            }

            tablet_id = alter_task->related_tablet_id();
            schema_hash = alter_task->related_schema_hash();
            TabletSharedPtr new_tablet = get_tablet(tablet_id, schema_hash);
            if (new_tablet == nullptr) {
                LOG(WARNING) << "new tablet created by alter tablet does not exist. "
                             << "tablet=" << tablet->full_name();
                continue;
            }

            AlterTabletTaskSharedPtr new_alter_task = new_tablet->alter_task();
            // DORIS-3741. Upon restart, it should not clear schema change request.
            if (alter_task->alter_state() == ALTER_FINISHED
                && new_alter_task != nullptr 
                && new_alter_task->alter_state() == ALTER_FINISHED) {
                continue;
            }

            tablet->set_alter_state(ALTER_FAILED);
            OLAPStatus res = tablet->save_meta();
            if (res != OLAP_SUCCESS) {
                LOG(FATAL) << "fail to save base tablet meta. res=" << res
                           << ", base_tablet=" << tablet->full_name();
                return;
            }

            new_tablet->set_alter_state(ALTER_FAILED);
            res = new_tablet->save_meta();
            if (res != OLAP_SUCCESS) {
                LOG(FATAL) << "fail to save new tablet meta. res=" << res
                           << ", new_tablet=" << new_tablet->full_name();
                return;
            }

            VLOG(3) << "cancel unfinished alter tablet task. base_tablet=" << tablet->full_name();
            ++canceled_num;
        }
    }

    LOG(INFO) << "finish to cancel unfinished schema change! canceled_num=" << canceled_num;
}

bool TabletManager::check_tablet_id_exist(TTabletId tablet_id) {
    ReadLock rlock(&_tablet_map_lock);
    return _check_tablet_id_exist_unlock(tablet_id);
} // check_tablet_id_exist

bool TabletManager::_check_tablet_id_exist_unlock(TTabletId tablet_id) {
    bool is_exist = false;

    tablet_map_t::iterator it = _tablet_map.find(tablet_id);
    if (it != _tablet_map.end() && it->second.table_arr.size() != 0) {
        is_exist = true;
    }
    return is_exist;
} // check_tablet_id_exist

void TabletManager::clear() {
    _tablet_map.clear();
    _shutdown_tablets.clear();
} // clear

OLAPStatus TabletManager::create_tablet(const TCreateTabletReq& request,
    std::vector<DataDir*> stores) {
    WriteLock wrlock(&_tablet_map_lock);
    LOG(INFO) << "begin to process create tablet. tablet=" << request.tablet_id
              << ", schema_hash=" << request.tablet_schema.schema_hash;
    OLAPStatus res = OLAP_SUCCESS;
    DorisMetrics::create_tablet_requests_total.increment(1);
    // Make sure create_tablet operation is idempotent:
    //    return success if tablet with same tablet_id and schema_hash exist,
    //           false if tablet with same tablet_id but different schema_hash exist
    // why??????
    if (_check_tablet_id_exist_unlock(request.tablet_id)) {
        TabletSharedPtr tablet = _get_tablet_with_no_lock(
                request.tablet_id, request.tablet_schema.schema_hash);
        if (tablet != nullptr) {
            LOG(INFO) << "create tablet success for tablet already exist.";
            return OLAP_SUCCESS;
        } else {
            LOG(WARNING) << "tablet with different schema hash already exists.";
            return OLAP_ERR_CE_TABLET_ID_EXIST;
        }
    }

    TabletSharedPtr tablet = _internal_create_tablet(request, false, nullptr, stores);
    if (tablet == nullptr) {
        res = OLAP_ERR_CE_CMD_PARAMS_ERROR;
        LOG(WARNING) << "fail to create tablet. res=" << res;
    }

    LOG(INFO) << "finish to process create tablet. res=" << res;
    return res;
} // create_tablet

TabletSharedPtr TabletManager::create_tablet(
        const TCreateTabletReq& request, const bool is_schema_change_tablet,
        const TabletSharedPtr ref_tablet, std::vector<DataDir*> data_dirs) {
    DCHECK(is_schema_change_tablet && ref_tablet != nullptr);
    WriteLock wrlock(&_tablet_map_lock);
    return _internal_create_tablet(request, is_schema_change_tablet,
        ref_tablet, data_dirs);
}

TabletSharedPtr TabletManager::_internal_create_tablet(
        const TCreateTabletReq& request, const bool is_schema_change_tablet,
        const TabletSharedPtr ref_tablet, std::vector<DataDir*> data_dirs) {
    DCHECK((is_schema_change_tablet && ref_tablet != nullptr) || (!is_schema_change_tablet && ref_tablet == nullptr));
    // check if the tablet with specified tablet id and schema hash already exists
    TabletSharedPtr checked_tablet = _get_tablet_with_no_lock(request.tablet_id, request.tablet_schema.schema_hash);
    if (checked_tablet != nullptr) {
        LOG(WARNING) << "failed to create tablet because tablet already exist." 
                     << " tablet id = " << request.tablet_id
                     << " schema hash = " << request.tablet_schema.schema_hash;
        return nullptr;
    }
    bool is_tablet_added = false;
    TabletSharedPtr tablet = _create_tablet_meta_and_dir(request, is_schema_change_tablet, 
        ref_tablet, data_dirs);
    if (tablet == nullptr) {
        return nullptr;
    }

    OLAPStatus res = OLAP_SUCCESS;
    do {
        res = tablet->init();
        if (res != OLAP_SUCCESS) {
            LOG(WARNING) << "tablet init failed. tablet:" << tablet->full_name();
            break;
        }
        if (!is_schema_change_tablet) {
            // Create init version if this is not a restore mode replica and request.version is set
            // bool in_restore_mode = request.__isset.in_restore_mode && request.in_restore_mode;
            // if (!in_restore_mode && request.__isset.version) {
            // create inital rowset before add it to storage engine could omit many locks
            res = _create_inital_rowset(tablet, request);
            if (res != OLAP_SUCCESS) {
                LOG(WARNING) << "fail to create initial version for tablet. res=" << res;
                break;
            }
        } else {
            // 有可能出现以下2种特殊情况：
            // 1. 因为操作系统时间跳变，导致新生成的表的creation_time小于旧表的creation_time时间
            // 2. 因为olap engine代码中统一以秒为单位，所以如果2个操作(比如create一个表,
            //    然后立即alter该表)之间的时间间隔小于1s，则alter得到的新表和旧表的creation_time会相同
            //
            // 当出现以上2种情况时，为了能够区分alter得到的新表和旧表，这里把新表的creation_time设置为
            // 旧表的creation_time加1
            if (tablet->creation_time() <= ref_tablet->creation_time()) {
                LOG(WARNING) << "new tablet's creation time is less than or equal to old tablet"
                            << "new_tablet_creation_time=" << tablet->creation_time()
                            << ", ref_tablet_creation_time=" << ref_tablet->creation_time();
                int64_t new_creation_time = ref_tablet->creation_time() + 1;
                tablet->set_creation_time(new_creation_time);
            }
        }

        // Add tablet to StorageEngine will make it visiable to user
        res = _add_tablet_unlock(request.tablet_id, request.tablet_schema.schema_hash, tablet, true, false);
        if (res != OLAP_SUCCESS) {
            LOG(WARNING) << "fail to add tablet to StorageEngine. res=" << res;
            break;
        }
        is_tablet_added = true;
        TabletSharedPtr tablet_ptr = _get_tablet_with_no_lock(request.tablet_id, request.tablet_schema.schema_hash);
        if (tablet_ptr == nullptr) {
            res = OLAP_ERR_TABLE_NOT_FOUND;
            LOG(WARNING) << "fail to get tablet. res=" << res;
            break;
        }
    } while (0);

    // should remove the pending path of tablet id no matter create tablet success or not
    tablet->data_dir()->remove_pending_ids(TABLET_ID_PREFIX + std::to_string(request.tablet_id));

    // clear environment
    if (res != OLAP_SUCCESS) {
        DorisMetrics::create_tablet_requests_failed.increment(1);
        if (is_tablet_added) {
            OLAPStatus status = _drop_tablet_unlock(
                    request.tablet_id, request.tablet_schema.schema_hash, false);
            if (status != OLAP_SUCCESS) {
                LOG(WARNING) << "fail to drop tablet when create tablet failed. res=" << res;
            }
        } else {
            tablet->delete_all_files();
            TabletMetaManager::remove(tablet->data_dir(), request.tablet_id, request.tablet_schema.schema_hash);
        }
        return nullptr;
    } else {
        LOG(INFO) << "finish to process create tablet. res=" << res;
        return tablet;
    }
} // create_tablet

TabletSharedPtr TabletManager::_create_tablet_meta_and_dir(
        const TCreateTabletReq& request, const bool is_schema_change_tablet,
        const TabletSharedPtr ref_tablet, std::vector<DataDir*> data_dirs) {
    TabletSharedPtr tablet;
    // Try to create tablet on each of all_available_root_path, util success
    DataDir* last_dir = nullptr; 
    for (auto& data_dir : data_dirs) {
        if (last_dir != nullptr) {
            // if last dir != null, it means preivous create tablet retry failed
            last_dir->remove_pending_ids(TABLET_ID_PREFIX + std::to_string(request.tablet_id));
        }
        last_dir = data_dir;
        TabletMetaSharedPtr tablet_meta;
        // if create meta faild, do not need to clean dir, because it is only in memory
        OLAPStatus res = _create_tablet_meta(request, data_dir, is_schema_change_tablet, ref_tablet, &tablet_meta);
        if (res != OLAP_SUCCESS) {
            LOG(WARNING) << "fail to create tablet meta. res=" << res << ", root=" << data_dir->path();
            continue;
        }

        stringstream schema_hash_dir_stream;
        schema_hash_dir_stream << data_dir->path()
                << DATA_PREFIX
                << "/" << tablet_meta->shard_id()
                << "/" << request.tablet_id
                << "/" << request.tablet_schema.schema_hash;
        string schema_hash_dir = schema_hash_dir_stream.str();
        boost::filesystem::path schema_hash_path(schema_hash_dir);
        boost::filesystem::path tablet_path = schema_hash_path.parent_path();
        std::string tablet_dir = tablet_path.string();
        if (!check_dir_existed(schema_hash_dir)) {
            data_dir->add_pending_ids(TABLET_ID_PREFIX + std::to_string(request.tablet_id));
            res = create_dirs(schema_hash_dir);
            if (res != OLAP_SUCCESS) {
                LOG(WARNING) << "create dir fail. [res=" << res << " path:" << schema_hash_dir;
                continue;
            }
        }

        tablet = Tablet::create_tablet_from_meta(tablet_meta, data_dir);
        if (tablet == nullptr) {
            LOG(WARNING) << "fail to load tablet from tablet_meta. root_path:" << data_dir->path();
            res = remove_all_dir(tablet_dir);
            if (res != OLAP_SUCCESS) {
                LOG(WARNING) << "remove tablet dir:" << tablet_dir;
            }
            continue;
        }
        break;
    }
    return tablet;
}

// Drop tablet specified, the main logical is as follows:
// 1. tablet not in schema change:
//      drop specified tablet directly;
// 2. tablet in schema change:
//      a. schema change not finished && dropped tablet is base :
//          base tablet cannot be dropped;
//      b. other cases:
//          drop specified tablet and clear schema change info.
OLAPStatus TabletManager::drop_tablet(
        TTabletId tablet_id, SchemaHash schema_hash, bool keep_files) {
    WriteLock wlock(&_tablet_map_lock);
    return _drop_tablet_unlock(tablet_id, schema_hash, keep_files);
} // drop_tablet


// Drop tablet specified, the main logical is as follows:
// 1. tablet not in schema change:
//      drop specified tablet directly;
// 2. tablet in schema change:
//      a. schema change not finished && dropped tablet is base :
//          base tablet cannot be dropped;
//      b. other cases:
//          drop specified tablet and clear schema change info.
OLAPStatus TabletManager::_drop_tablet_unlock(
        TTabletId tablet_id, SchemaHash schema_hash, bool keep_files) {
    LOG(INFO) << "begin to process drop tablet."
        << "tablet=" << tablet_id << ", schema_hash=" << schema_hash;
    DorisMetrics::drop_tablet_requests_total.increment(1);

    OLAPStatus res = OLAP_SUCCESS;

    // Get tablet which need to be droped
    TabletSharedPtr dropped_tablet = _get_tablet_with_no_lock(tablet_id, schema_hash);
    if (dropped_tablet == nullptr) {
        LOG(WARNING) << "tablet to drop does not exist already."
                     << " tablet_id=" << tablet_id
                     << ", schema_hash=" << schema_hash;
        return OLAP_SUCCESS;
    }

    // Try to get schema change info
    AlterTabletTaskSharedPtr alter_task = dropped_tablet->alter_task();

    // Drop tablet directly when not in schema change
    if (alter_task == nullptr) {
        return _drop_tablet_directly_unlocked(tablet_id, schema_hash, keep_files);
    }

    AlterTabletState alter_state = alter_task->alter_state();
    TTabletId related_tablet_id = alter_task->related_tablet_id();
    TSchemaHash related_schema_hash = alter_task->related_schema_hash();;

    // Check tablet is in schema change or not, is base tablet or not
    bool is_schema_change_finished = true;
    if (alter_state != ALTER_FINISHED) {
        is_schema_change_finished = false;
    }

    bool is_drop_base_tablet = false;
    TabletSharedPtr related_tablet = _get_tablet_with_no_lock(
            related_tablet_id, related_schema_hash);
    if (related_tablet == nullptr) {
        LOG(WARNING) << "drop tablet directly when related tablet not found. "
                     << " tablet_id=" << related_tablet_id
                     << " schema_hash=" << related_schema_hash;
        return _drop_tablet_directly_unlocked(tablet_id, schema_hash, keep_files);
    }

    if (dropped_tablet->creation_time() < related_tablet->creation_time()) {
        is_drop_base_tablet = true;
    }

    if (is_drop_base_tablet && !is_schema_change_finished) {
        LOG(WARNING) << "base tablet in schema change cannot be droped. tablet="
                     << dropped_tablet->full_name();
        return OLAP_ERR_PREVIOUS_SCHEMA_CHANGE_NOT_FINISHED;
    }

    // Drop specified tablet and clear schema change info
    // must first break the link and then drop the tablet
    // if drop tablet, then break link. the link maybe exists but the tablet 
    // not exist when be restarts
    related_tablet->obtain_header_wrlock();
    related_tablet->delete_alter_task();
    res = related_tablet->save_meta();
    if (res != OLAP_SUCCESS) {
        LOG(FATAL) << "fail to save tablet header. res=" << res
                   << ", tablet=" << related_tablet->full_name();
    }

    res = _drop_tablet_directly_unlocked(tablet_id, schema_hash, keep_files);
    related_tablet->release_header_lock();
    if (res != OLAP_SUCCESS) {
        LOG(WARNING) << "fail to drop tablet which in schema change. tablet="
                     << dropped_tablet->full_name();
        return res;
    }

    LOG(INFO) << "finish to drop tablet. res=" << res;
    return res;
} // drop_tablet_unlock

OLAPStatus TabletManager::drop_tablets_on_error_root_path(
        const vector<TabletInfo>& tablet_info_vec) {
    OLAPStatus res = OLAP_SUCCESS;
    WriteLock wlock(&_tablet_map_lock);

    for (const TabletInfo& tablet_info : tablet_info_vec) {
        TTabletId tablet_id = tablet_info.tablet_id;
        TSchemaHash schema_hash = tablet_info.schema_hash;
        VLOG(3) << "drop_tablet begin. tablet_id=" << tablet_id
                << ", schema_hash=" << schema_hash;
        TabletSharedPtr dropped_tablet = _get_tablet_with_no_lock(tablet_id, schema_hash);
        if (dropped_tablet == nullptr) {
            LOG(WARNING) << "dropping tablet not exist. " 
                         << " tablet=" << tablet_id
                         << " schema_hash=" << schema_hash;
            continue;
        } else {
            for (list<TabletSharedPtr>::iterator it = _tablet_map[tablet_id].table_arr.begin();
                    it != _tablet_map[tablet_id].table_arr.end();) {
                if ((*it)->equal(tablet_id, schema_hash)) {
                    it = _tablet_map[tablet_id].table_arr.erase(it);
                } else {
                    ++it;
                }
            }

            if (_tablet_map[tablet_id].table_arr.empty()) {
                _tablet_map.erase(tablet_id);
            }
        }
    }

    return res;
} // drop_tablets_on_error_root_path

TabletSharedPtr TabletManager::get_tablet(TTabletId tablet_id, SchemaHash schema_hash, bool include_deleted) {
    ReadLock rlock(&_tablet_map_lock);
    TabletSharedPtr tablet;
    tablet = _get_tablet_with_no_lock(tablet_id, schema_hash);
    if (tablet == nullptr && include_deleted) {
        for (auto& deleted_tablet : _shutdown_tablets) {
            if (deleted_tablet->tablet_id() == tablet_id && deleted_tablet->schema_hash() == schema_hash) {
                tablet = deleted_tablet;
                break;
            }
        }
    }

    if (tablet != nullptr) {
        if (!tablet->is_used()) {
            LOG(WARNING) << "tablet cannot be used. tablet=" << tablet_id;
            tablet.reset();
        }
    }

    return tablet;
} // get_tablet

bool TabletManager::get_tablet_id_and_schema_hash_from_path(const std::string& path,
        TTabletId* tablet_id, TSchemaHash* schema_hash) {
    std::vector<DataDir*> data_dirs = StorageEngine::instance()->get_stores<true>();
    for (auto data_dir : data_dirs) {
        const std::string& data_dir_path = data_dir->path();
        if (path.find(data_dir_path) != std::string::npos) {
            std::string pattern = data_dir_path + "/data/\\d+/(\\d+)/?(\\d+)?";
            std::regex rgx (pattern.c_str());
            std::smatch sm;
            bool ret = std::regex_search(path, sm, rgx);
            if (ret) {
                if (sm.size() == 3) {
                    *tablet_id = std::strtoll(sm.str(1).c_str(), nullptr, 10);
                    *schema_hash = std::strtoll(sm.str(2).c_str(), nullptr, 10);
                    return true;
                } else {
                    LOG(WARNING) << "invalid match. match size:" << sm.size();
                    return false;
                }
            }
        }
    }
    return false;
}

bool TabletManager::get_rowset_id_from_path(const std::string& path, RowsetId* rowset_id) {
    static std::regex rgx ("/data/\\d+/\\d+/\\d+/(\\d+)_.*");
    std::smatch sm;
    bool ret = std::regex_search(path, sm, rgx);
    if (ret) {
        if (sm.size() == 2) {
            *rowset_id = std::strtoll(sm.str(1).c_str(), nullptr, 10);
            return true;
        } else {
            return false;
        }
    }
    return false;
}

void TabletManager::get_tablet_stat(TTabletStatResult& result) {
    VLOG(3) << "begin to get all tablet stat.";

    // get current time
    int64_t current_time = UnixMillis();
    WriteLock wlock(&_tablet_map_lock);
    // update cache if too old
    if (current_time - _tablet_stat_cache_update_time_ms >
        config::tablet_stat_cache_update_interval_second * 1000) {
        VLOG(3) << "update tablet stat.";
        _build_tablet_stat();
    }

    result.__set_tablets_stats(_tablet_stat_cache);
} // get_tablet_stat

TabletSharedPtr TabletManager::find_best_tablet_to_compaction(CompactionType compaction_type) {
    ReadLock tablet_map_rdlock(&_tablet_map_lock);
    uint32_t highest_score = 0;
    TabletSharedPtr best_tablet;
    for (tablet_map_t::value_type& table_ins : _tablet_map){
        for (TabletSharedPtr& table_ptr : table_ins.second.table_arr) {
            AlterTabletTaskSharedPtr cur_alter_task = table_ptr->alter_task();
            if (cur_alter_task != nullptr && cur_alter_task->alter_state() != ALTER_FINISHED 
                && cur_alter_task->alter_state() != ALTER_FAILED) {
                    TabletSharedPtr related_tablet = _get_tablet_with_no_lock(cur_alter_task->related_tablet_id(), 
                        cur_alter_task->related_schema_hash());
                    if (related_tablet != nullptr && table_ptr->creation_time() > related_tablet->creation_time()) {
                        // it means cur tablet is a new tablet during schema change or rollup, skip compaction
                        continue;
                    }
            }
            if (!table_ptr->init_succeeded() || !table_ptr->can_do_compaction()) {
                continue;
            }

            ReadLock rdlock(table_ptr->get_header_lock_ptr());
            uint32_t table_score = 0;
            if (compaction_type == CompactionType::BASE_COMPACTION) {
                table_score = table_ptr->calc_base_compaction_score();
            } else if (compaction_type == CompactionType::CUMULATIVE_COMPACTION) {
                table_score = table_ptr->calc_cumulative_compaction_score();
            }
            if (table_score > highest_score) {
                highest_score = table_score;
                best_tablet = table_ptr;
            }
        }
    }
    return best_tablet;
}

OLAPStatus TabletManager::load_tablet_from_meta(DataDir* data_dir, TTabletId tablet_id,
        TSchemaHash schema_hash, const std::string& meta_binary, bool update_meta, bool force) {
    WriteLock wlock(&_tablet_map_lock);
    TabletMetaSharedPtr tablet_meta(new TabletMeta());
    OLAPStatus status = tablet_meta->deserialize(meta_binary);
    if (status != OLAP_SUCCESS) {
        LOG(WARNING) << "parse meta_binary string failed for tablet_id:" << tablet_id << ", schema_hash:" << schema_hash;
        return OLAP_ERR_HEADER_PB_PARSE_FAILED;
    }

    // init must be called
    TabletSharedPtr tablet = Tablet::create_tablet_from_meta(tablet_meta, data_dir);
    if (tablet == nullptr) {
        LOG(WARNING) << "fail to new tablet. tablet_id=" << tablet_id << ", schema_hash:" << schema_hash;
        return OLAP_ERR_TABLE_CREATE_FROM_HEADER_ERROR;
    }

    if (tablet_meta->tablet_state() == TABLET_SHUTDOWN) {
        LOG(INFO) << "tablet is to be deleted, skip load it"
                  << " tablet id = " << tablet_meta->tablet_id()
                  << " schema hash = " << tablet_meta->schema_hash();
        _shutdown_tablets.push_back(tablet);
        return OLAP_ERR_TABLE_ALREADY_DELETED_ERROR;
    }

    if (tablet->max_version().first == -1 && tablet->alter_task() == nullptr) {
        LOG(WARNING) << "tablet not in schema change state without delta is invalid."
                     << "tablet=" << tablet->full_name();
        // tablet state is invalid, drop tablet
        return OLAP_ERR_TABLE_INDEX_VALIDATE_ERROR;
    }

    OLAPStatus res = tablet->init();
    if (res != OLAP_SUCCESS) {
        LOG(WARNING) << "tablet init failed. tablet:" << tablet->full_name();
        return res;
    }
    res = _add_tablet_unlock(tablet_id, schema_hash, tablet, update_meta, force);
    if (res != OLAP_SUCCESS) {
        // insert existed tablet return OLAP_SUCCESS
        if (res == OLAP_ERR_ENGINE_INSERT_EXISTS_TABLE) {
            LOG(WARNING) << "add duplicate tablet. tablet=" << tablet->full_name();
        }

        LOG(WARNING) << "failed to add tablet. tablet=" << tablet->full_name();
        return res;
    }

    return OLAP_SUCCESS;
} // load_tablet_from_meta

OLAPStatus TabletManager::load_tablet_from_dir(
        DataDir* store, TTabletId tablet_id, SchemaHash schema_hash,
        const string& schema_hash_path, bool force) {
    // not add lock here, because load_tablet_from_meta already add lock
    stringstream header_name_stream;
    header_name_stream << schema_hash_path << "/" << tablet_id << ".hdr";
    string header_path = header_name_stream.str();
    // should change shard id before load tablet
    path boost_header_path(header_path);
    std::string shard_path = boost_header_path.parent_path().parent_path().parent_path().string();
    std::string shard_str = shard_path.substr(shard_path.find_last_of('/') + 1);
    int32_t shard = stol(shard_str);
    OLAPStatus res = OLAP_SUCCESS;
    TabletMetaSharedPtr tablet_meta(new(nothrow) TabletMeta());
    do {
        if (access(header_path.c_str(), F_OK) != 0) {
            LOG(WARNING) << "fail to find header file. [header_path=" << header_path << "]";
            res = OLAP_ERR_FILE_NOT_EXIST;
            break;
        }
        if (tablet_meta == nullptr) {
            LOG(WARNING) << "fail to malloc TabletMeta.";
            res = OLAP_ERR_ENGINE_LOAD_INDEX_TABLE_ERROR;
            break;
        }

        if (tablet_meta->create_from_file(header_path) != OLAP_SUCCESS) {
            LOG(WARNING) << "fail to load tablet_meta. file_path=" << header_path;
            res = OLAP_ERR_ENGINE_LOAD_INDEX_TABLE_ERROR;
            break;
        }
        // has to change shard id here, because meta file maybe copyed from other source
        // its shard is different from local shard
        tablet_meta->set_shard_id(shard);
        std::string meta_binary;
        tablet_meta->serialize(&meta_binary);
        res = load_tablet_from_meta(store, tablet_id, schema_hash, meta_binary, true, force);
        if (res != OLAP_SUCCESS) {
            LOG(WARNING) << "fail to load tablet. [header_path=" << header_path << "]";
            res = OLAP_ERR_ENGINE_LOAD_INDEX_TABLE_ERROR;
            break;
        }
    } while (0);
    return res;
} // load_tablet_from_dir

void TabletManager::release_schema_change_lock(TTabletId tablet_id) {
    VLOG(3) << "release_schema_change_lock begin. tablet_id=" << tablet_id;
    ReadLock rlock(&_tablet_map_lock);

    tablet_map_t::iterator it = _tablet_map.find(tablet_id);
    if (it == _tablet_map.end()) {
        LOG(WARNING) << "tablet does not exists. tablet=" << tablet_id;
    } else {
        it->second.schema_change_lock.unlock();
    }
    VLOG(3) << "release_schema_change_lock end. tablet_id=" << tablet_id;
} // release_schema_change_lock

OLAPStatus TabletManager::report_tablet_info(TTabletInfo* tablet_info) {
    DorisMetrics::report_tablet_requests_total.increment(1);
    LOG(INFO) << "begin to process report tablet info."
              << "tablet_id=" << tablet_info->tablet_id
              << ", schema_hash=" << tablet_info->schema_hash;

    OLAPStatus res = OLAP_SUCCESS;

    TabletSharedPtr tablet = get_tablet(
            tablet_info->tablet_id, tablet_info->schema_hash);
    if (tablet == nullptr) {
        LOG(WARNING) << "can't find tablet. " 
                     << " tablet=" << tablet_info->tablet_id
                     << " schema_hash=" << tablet_info->schema_hash;
        return OLAP_ERR_TABLE_NOT_FOUND;
    }

    _build_tablet_info(tablet, tablet_info);
    LOG(INFO) << "success to process report tablet info.";
    return res;
} // report_tablet_info

OLAPStatus TabletManager::report_all_tablets_info(std::map<TTabletId, TTablet>* tablets_info) {
    LOG(INFO) << "begin to process report all tablets info.";
    ReadLock rlock(&_tablet_map_lock);
    DorisMetrics::report_all_tablets_requests_total.increment(1);

    if (tablets_info == nullptr) {
        return OLAP_ERR_INPUT_PARAMETER_ERROR;
    }

    for (const auto& item : _tablet_map) {
        if (item.second.table_arr.size() == 0) {
            continue;
        }

        TTablet tablet;
        for (TabletSharedPtr tablet_ptr : item.second.table_arr) {
            if (tablet_ptr == nullptr) {
                continue;
            }

            TTabletInfo tablet_info;
            _build_tablet_info(tablet_ptr, &tablet_info);

            // report expire transaction
            vector<int64_t> transaction_ids;
            // TODO(ygl): tablet manager and txn manager may be dead lock
            StorageEngine::instance()->txn_manager()->get_expire_txns(tablet_ptr->tablet_id(), 
                tablet_ptr->schema_hash(), &transaction_ids);
            tablet_info.__set_transaction_ids(transaction_ids);

            if (_available_storage_medium_type_count > 1) {
                tablet_info.__set_storage_medium(tablet_ptr->data_dir()->storage_medium());
            }

            tablet_info.__set_version_count(tablet_ptr->version_count());
            tablet_info.__set_path_hash(tablet_ptr->data_dir()->path_hash());

            tablet.tablet_infos.push_back(tablet_info);
        }

        if (tablet.tablet_infos.size() != 0) {
            tablets_info->insert(pair<TTabletId, TTablet>(tablet.tablet_infos[0].tablet_id, tablet));
        }
    }

    LOG(INFO) << "success to process report all tablets info. tablet_num=" << tablets_info->size();
    return OLAP_SUCCESS;
} // report_all_tablets_info

OLAPStatus TabletManager::start_trash_sweep() {
    ReadLock rlock(&_tablet_map_lock);
    for (const auto& item : _tablet_map) {
        for (TabletSharedPtr tablet : item.second.table_arr) {
            if (tablet == nullptr) {
                continue;
            }
            tablet->delete_expired_inc_rowsets();
        }
    }
    auto it = _shutdown_tablets.begin();
    for (; it != _shutdown_tablets.end();) { 
        // check if the meta has the tablet info and its state is shutdown
        if (it->use_count() > 1) {
            // it means current tablet is referenced in other thread
            ++it;
            continue;
        }
        TabletMetaSharedPtr new_tablet_meta(new(nothrow) TabletMeta());
        if (new_tablet_meta == nullptr) {
            LOG(WARNING) << "fail to malloc TabletMeta.";
            ++it;
            continue;
        }
        OLAPStatus check_st = TabletMetaManager::get_header((*it)->data_dir(), 
            (*it)->tablet_id(), (*it)->schema_hash(), new_tablet_meta);
        if (check_st == OLAP_SUCCESS) {
            if (new_tablet_meta->tablet_state() != TABLET_SHUTDOWN) {
                LOG(WARNING) << "tablet's state changed to normal, skip remove dirs"
                            << " tablet id = " << new_tablet_meta->tablet_id()
                            << " schema hash = " << new_tablet_meta->schema_hash();
                // remove it from list
                it = _shutdown_tablets.erase(it);
                continue;
            }
            if (check_dir_existed((*it)->tablet_path())) {
                // take snapshot of tablet meta
                std::string meta_file = (*it)->tablet_path() + "/" + std::to_string((*it)->tablet_id()) + ".hdr";
                (*it)->tablet_meta()->save(meta_file);
                LOG(INFO) << "start to move path to trash" 
                          << " tablet path = " << (*it)->tablet_path();
                OLAPStatus rm_st = move_to_trash((*it)->tablet_path(), (*it)->tablet_path());
                if (rm_st != OLAP_SUCCESS) {
                    LOG(WARNING) << "failed to move dir to trash"
                                 << " dir = " << (*it)->tablet_path();
                    ++it;
                    continue;
                }
            }
            TabletMetaManager::remove((*it)->data_dir(), (*it)->tablet_id(), (*it)->schema_hash());
            LOG(INFO) << "successfully move tablet to trash." 
                        << " tablet id " << (*it)->tablet_id()
                        << " schema hash " << (*it)->schema_hash()
                        << " tablet path " << (*it)->tablet_path();
            it = _shutdown_tablets.erase(it);
        } else {
            // if could not find tablet info in meta store, then check if dir existed
            if (check_dir_existed((*it)->tablet_path())) {
                LOG(WARNING) << "errors while load meta from store, skip this tablet" 
                            << " tablet id " << (*it)->tablet_id()
                            << " schema hash " << (*it)->schema_hash();
                ++it;
            } else {
                LOG(INFO) << "could not find tablet dir, skip move to trash, remove it from gc queue." 
                        << " tablet id " << (*it)->tablet_id()
                        << " schema hash " << (*it)->schema_hash()
                        << " tablet path " << (*it)->tablet_path();
                it = _shutdown_tablets.erase(it);
            }
        }
    }
    return OLAP_SUCCESS;
} // start_trash_sweep

bool TabletManager::try_schema_change_lock(TTabletId tablet_id) {
    bool res = false;
    VLOG(3) << "try_schema_change_lock begin. tablet_id=" << tablet_id;
    ReadLock rlock(&_tablet_map_lock);

    tablet_map_t::iterator it = _tablet_map.find(tablet_id);
    if (it == _tablet_map.end()) {
        LOG(WARNING) << "tablet does not exists. tablet_id=" << tablet_id;
    } else {
        res = (it->second.schema_change_lock.trylock() == OLAP_SUCCESS);
    }
    VLOG(3) << "try_schema_change_lock end. tablet_id=" <<  tablet_id;
    return res;
} // try_schema_change_lock

void TabletManager::update_root_path_info(std::map<std::string, DataDirInfo>* path_map,
    int* tablet_counter) {
    ReadLock rlock(&_tablet_map_lock);
    for (auto& entry : _tablet_map) {
        TableInstances& instance = entry.second;
        for (auto& tablet : instance.table_arr) {
            (*tablet_counter) ++ ;
            int64_t data_size = tablet->tablet_footprint();
            auto find = path_map->find(tablet->data_dir()->path());
            if (find == path_map->end()) {
                continue;
            }
            if (find->second.is_used) {
                find->second.data_used_capacity += data_size;
            }
        }
    }
} // update_root_path_info

void TabletManager::update_storage_medium_type_count(uint32_t storage_medium_type_count) {
    _available_storage_medium_type_count = storage_medium_type_count;
}

void TabletManager::_build_tablet_info(TabletSharedPtr tablet, TTabletInfo* tablet_info) {
    tablet_info->tablet_id = tablet->tablet_id();
    tablet_info->schema_hash = tablet->schema_hash();
    tablet_info->row_count = tablet->num_rows();
    tablet_info->data_size = tablet->tablet_footprint();
    Version version = { -1, 0 };
    VersionHash v_hash = 0;
    tablet->max_continuous_version_from_begining(&version, &v_hash);
    tablet_info->version = version.second;
    tablet_info->version_hash = v_hash;
}

void TabletManager::_build_tablet_stat() {
    _tablet_stat_cache.clear();
    for (const auto& item : _tablet_map) {
        if (item.second.table_arr.size() == 0) {
            continue;
        }

        TTabletStat stat;
        stat.tablet_id = item.first;
        for (TabletSharedPtr tablet : item.second.table_arr) {
            if (tablet == nullptr) {
                continue;
            }
            // we only get base tablet's stat
            stat.__set_data_size(tablet->tablet_footprint());
            stat.__set_row_num(tablet->num_rows());
            VLOG(3) << "tablet_id=" << item.first
                    << ", data_size=" << tablet->tablet_footprint()
                    << ", row_num:" << tablet->num_rows();
            break;
        }

        _tablet_stat_cache.emplace(item.first, stat);
    }

    _tablet_stat_cache_update_time_ms = UnixMillis();
}

OLAPStatus TabletManager::_create_inital_rowset(
        TabletSharedPtr tablet, const TCreateTabletReq& request) {
    OLAPStatus res = OLAP_SUCCESS;

    if (request.version < 1) {
        LOG(WARNING) << "init version of tablet should at least 1.";
        return OLAP_ERR_CE_CMD_PARAMS_ERROR;
    } else {
        Version version(0, request.version);
        VLOG(3) << "begin to create init version. "
                << "begin=" << version.first << ", end=" << version.second;
        RowsetSharedPtr new_rowset;
        do {
            if (version.first > version.second) {
                LOG(WARNING) << "begin should not larger than end." 
                            << " begin=" << version.first
                            << " end=" << version.second;
                res = OLAP_ERR_INPUT_PARAMETER_ERROR;
                break;
            }
            RowsetId rowset_id = 0;
            RETURN_NOT_OK(tablet->next_rowset_id(&rowset_id));
            RowsetWriterContext context;
            context.rowset_id = rowset_id;
            context.tablet_id = tablet->tablet_id();
            context.partition_id = tablet->partition_id();
            context.tablet_schema_hash = tablet->schema_hash();
            context.rowset_type = ALPHA_ROWSET;
            context.rowset_path_prefix = tablet->tablet_path();
            context.tablet_schema = &(tablet->tablet_schema());
            context.rowset_state = VISIBLE;
            context.data_dir = tablet->data_dir();
            context.version = version;
            context.version_hash = request.version_hash;
            RowsetWriter* builder = new (std::nothrow)AlphaRowsetWriter(); 
            if (builder == nullptr) {
                LOG(WARNING) << "fail to new rowset.";
                res = OLAP_ERR_MALLOC_ERROR;
                break;
            }
            builder->init(context);
            res = builder->flush();
            if (OLAP_SUCCESS != res) {
                LOG(WARNING) << "fail to finalize writer. tablet=" << tablet->full_name();
                break;
            }

            new_rowset = builder->build();
            res = tablet->add_rowset(new_rowset);
            if (res != OLAP_SUCCESS) {
                LOG(WARNING) << "fail to add rowset to tablet. "
                            << "tablet=" << tablet->full_name();
                break;
            }
        } while (0);

        // Unregister index and delete files(index and data) if failed
        if (res != OLAP_SUCCESS) {
            StorageEngine::instance()->add_unused_rowset(new_rowset);
            LOG(WARNING) << "fail to create init base version. " 
                         << " res=" << res 
                         << " version=" << request.version;
            return res;
        }
    }
    tablet->set_cumulative_layer_point(request.version + 1);
    res = tablet->save_meta();
    if (res != OLAP_SUCCESS) {
        LOG(WARNING) << "fail to save header. [tablet=" << tablet->full_name() << "]";
    }

    return res;
}

OLAPStatus TabletManager::_create_tablet_meta(
        const TCreateTabletReq& request,
        DataDir* store,
        const bool is_schema_change_tablet,
        const TabletSharedPtr ref_tablet,
        TabletMetaSharedPtr* tablet_meta) {
    uint64_t shard_id = 0;
    OLAPStatus res = store->get_shard(&shard_id);
    if (res != OLAP_SUCCESS) {
        LOG(WARNING) << "fail to get root path shard. res=" << res;
        return res;
    }

    uint32_t next_unique_id = 0;
    uint32_t col_ordinal = 0;
    std::unordered_map<uint32_t, uint32_t> col_ordinal_to_unique_id;
    if (!is_schema_change_tablet) {
        for (TColumn column : request.tablet_schema.columns) {
            col_ordinal_to_unique_id[col_ordinal] = col_ordinal;
            col_ordinal++;
        }
        next_unique_id = col_ordinal;
    } else {
        next_unique_id = ref_tablet->next_unique_id();
        size_t num_columns = ref_tablet->num_columns();
        size_t field = 0;
        for (TColumn column : request.tablet_schema.columns) {
            /*
             * for schema change, compare old_tablet and new_tablet
             * 1. if column in both new_tablet and old_tablet,
             * assign unique_id of old_tablet to the column of new_tablet
             * 2. if column exists only in new_tablet, assign next_unique_id of old_tablet
             * to the new column
             *
            */
            for (field = 0 ; field < num_columns; ++field) {
                if (ref_tablet->tablet_schema().column(field).name() == column.column_name) {
                    uint32_t unique_id = ref_tablet->tablet_schema().column(field).unique_id();
                    col_ordinal_to_unique_id[col_ordinal] = unique_id;
                    break;
                }
            }
            if (field == num_columns) {
                col_ordinal_to_unique_id[col_ordinal] = next_unique_id;
                next_unique_id++;
            }
            col_ordinal++;
        }
    }

    LOG(INFO) << "next_unique_id:" << next_unique_id;
    TabletMeta::create(request.table_id, request.partition_id,
                       request.tablet_id, request.tablet_schema.schema_hash,
                       shard_id, request.tablet_schema,
                       next_unique_id, col_ordinal_to_unique_id,
                       tablet_meta);
    return OLAP_SUCCESS;
}

OLAPStatus TabletManager::_drop_tablet_directly_unlocked(
        TTabletId tablet_id, SchemaHash schema_hash, bool keep_files) {
    OLAPStatus res = OLAP_SUCCESS;

    TabletSharedPtr dropped_tablet = _get_tablet_with_no_lock(tablet_id, schema_hash);
    if (dropped_tablet == nullptr) {
        LOG(WARNING) << "fail to drop not existed tablet. " 
                     << " tablet_id=" << tablet_id
                     << " schema_hash=" << schema_hash;
        return OLAP_ERR_TABLE_NOT_FOUND;
    }

    for (list<TabletSharedPtr>::iterator it = _tablet_map[tablet_id].table_arr.begin();
            it != _tablet_map[tablet_id].table_arr.end();) {
        if ((*it)->equal(tablet_id, schema_hash)) {
            TabletSharedPtr tablet = *it;
            it = _tablet_map[tablet_id].table_arr.erase(it);
            if (!keep_files) {
                LOG(INFO) << "set tablet to shutdown state and remove it from memory"
                          << " tablet_id=" << tablet_id
                          << " schema_hash=" << schema_hash
                          << " tablet path=" << dropped_tablet->tablet_path();
                // has to update tablet here, must not update tablet meta directly
                // because other thread may hold the tablet object, they may save meta too
                // if update meta directly here, other thread may override the meta
                // and the tablet will be loaded at restart time.
                tablet->set_tablet_state(TABLET_SHUTDOWN);
                res = tablet->save_meta();
                if (res != OLAP_SUCCESS) {
                    LOG(WARNING) << "fail to drop tablet. " 
                                 << " tablet_id=" << tablet_id
                                 << " schema_hash=" << schema_hash;
                    return res;
                }
                _shutdown_tablets.push_back(tablet);
            }
        } else {
            ++it;
        }
    }

    if (_tablet_map[tablet_id].table_arr.empty()) {
        _tablet_map.erase(tablet_id);
    }

    res = dropped_tablet->deregister_tablet_from_dir();
    if (res != OLAP_SUCCESS) {
        LOG(WARNING) << "fail to unregister from root path. " 
                     << " res= " << res
                     << " tablet=" << tablet_id;
    }

    return res;
} // _drop_tablet_directly_unlocked

TabletSharedPtr TabletManager::_get_tablet_with_no_lock(TTabletId tablet_id, SchemaHash schema_hash) {
    VLOG(3) << "begin to get tablet. tablet_id=" << tablet_id
            << ", schema_hash=" << schema_hash;
    tablet_map_t::iterator it = _tablet_map.find(tablet_id);
    if (it != _tablet_map.end()) {
        for (TabletSharedPtr tablet : it->second.table_arr) {
            if (tablet->equal(tablet_id, schema_hash)) {
                VLOG(3) << "get tablet success. tablet_id=" << tablet_id
                        << ", schema_hash=" << schema_hash;
                return tablet;
            }
        }
    }

    VLOG(3) << "fail to get tablet. tablet_id=" << tablet_id
            << ", schema_hash=" << schema_hash;
    // Return empty tablet if fail
    TabletSharedPtr tablet;
    return tablet;
} // _get_tablet_with_no_lock

} // doris
