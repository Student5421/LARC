#ifndef ROCKSDB_CEPHFS_MONITOR_H
#define ROCKSDB_CEPHFS_MONITOR_H

#include <string>
#include <vector>
#include <cstdint>

namespace rocksdb {

struct CephFSObjectInfo {
    std::string sst_filename;
    std::string sst_path;
    uint64_t inode;
    std::string object_name;
    std::string pg_id;
    int primary_osd;
    std::vector<int> acting_set;
    std::string timestamp;
    uint64_t file_size;
};

class CephFSMonitor {
public:
    // SST 파일 삭제 전 오브젝트 위치 정보 수집
    static bool CollectSSTObjectLocations(const std::string& sst_path, 
                                         const std::string& db_path,
                                         const std::string& event_type = "deletion");
    
    // 단일 오브젝트의 OSD 위치 조회
    static bool GetObjectLocation(const std::string& object_name, 
                                 CephFSObjectInfo& info);
    
    // CephFS 설정 초기화
    static void Initialize(const std::string& cephfs_data_pool = "cephfs_data");
    
    // 활성화/비활성화
    static void SetEnabled(bool enabled);
    
private:
    static std::string GetInodeHex(const std::string& file_path);
    static std::vector<std::string> GetInodeObjects(const std::string& inode_hex);
    static bool ParseCephLocation(const std::string& location_output, 
                                 std::string& pg_id, 
                                 int& primary_osd, 
                                 std::vector<int>& acting_set);
    static std::string GetTimestamp();
    static std::string ExecuteCommand(const std::string& command);
    
    // CSV 파일에 로깅
    static void LogToCSV(const CephFSObjectInfo& info, 
                        const std::string& csv_path, 
                        const std::string& event_type = "deletion");

    static bool enabled_;
    static std::string cephfs_data_pool_;
    static std::string csv_log_path_;
};

} // namespace rocksdb

#endif // ROCKSDB_CEPHFS_MONITOR_H