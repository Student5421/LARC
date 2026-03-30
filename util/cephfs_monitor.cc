#include "rocksdb/cephfs_monitor.h"
#include <fstream>
#include <sstream>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>
#include <iomanip>
#include <iostream>

namespace rocksdb {

// 정적 변수 초기화
bool CephFSMonitor::enabled_ = false;
std::string CephFSMonitor::cephfs_data_pool_ = "cephfs_data";
std::string CephFSMonitor::csv_log_path_ = "rocksdb_cephfs_sst_objects.csv";

void CephFSMonitor::Initialize(const std::string& cephfs_data_pool) {
    cephfs_data_pool_ = cephfs_data_pool;
    enabled_ = true;
    
    // CSV 헤더 생성
    std::ofstream csv(csv_log_path_, std::ios::out);
    if (csv.is_open()) {
        csv << "timestamp,event_type,sst_file,inode,object_name,pg_id,osd_primary,osd_acting_set,file_size" << std::endl;
        csv.close();
    }
    
    std::cout << "CephFS Monitor initialized. Pool: " << cephfs_data_pool_ 
              << ", Log: " << csv_log_path_ << std::endl;
}

void CephFSMonitor::SetEnabled(bool enabled) {
    enabled_ = enabled;
}

bool CephFSMonitor::CollectSSTObjectLocations(const std::string& sst_path, 
                                             const std::string& db_path,
                                             const std::string& event_type) {
    (void)db_path;                                            
    if (!enabled_) return true;
    
    // 파일 존재 확인
    struct stat st;
    if (stat(sst_path.c_str(), &st) != 0) {
        return false;
    }
    
    std::string sst_filename = sst_path.substr(sst_path.find_last_of('/') + 1);
    std::string inode_hex = GetInodeHex(sst_path);
    
    if (inode_hex.empty()) {
        std::cerr << "Failed to get inode for: " << sst_path << std::endl;
        return false;
    }
    
    // 해당 inode의 모든 오브젝트 찾기
    std::vector<std::string> objects = GetInodeObjects(inode_hex);
    
    std::cout << "SST " << event_type << " detected: " << sst_filename 
              << " (inode: " << st.st_ino << ", objects: " << objects.size() << ")" << std::endl;
    
    // 각 오브젝트의 위치 정보 수집
    for (const auto& object_name : objects) {
        CephFSObjectInfo info;
        info.sst_filename = sst_filename;
        info.sst_path = sst_path;
        info.inode = st.st_ino;
        info.object_name = object_name;
        info.file_size = st.st_size;
        info.timestamp = GetTimestamp();
        
        if (GetObjectLocation(object_name, info)) {
            LogToCSV(info, csv_log_path_, event_type);
            std::cout << "  Object: " << object_name 
                      << " -> PG:" << info.pg_id 
                      << ", Primary OSD:" << info.primary_osd << std::endl;
        }
    }
    
    return true;
}

std::string CephFSMonitor::GetInodeHex(const std::string& file_path) {
    struct stat st;
    if (stat(file_path.c_str(), &st) != 0) {
        return "";
    }
    
    char inode_hex[32];
    snprintf(inode_hex, sizeof(inode_hex), "%llx", (unsigned long long)st.st_ino);
    return std::string(inode_hex);
}

std::vector<std::string> CephFSMonitor::GetInodeObjects(const std::string& inode_hex) {
    std::vector<std::string> objects;
    
    std::string cmd = "rados -p " + cephfs_data_pool_ + " ls 2>/dev/null | grep '^" + inode_hex + "\\.'";
    std::string result = ExecuteCommand(cmd);
    
    std::istringstream iss(result);
    std::string object_name;
    while (std::getline(iss, object_name)) {
        if (!object_name.empty()) {
            objects.push_back(object_name);
        }
    }
    
    return objects;
}

bool CephFSMonitor::GetObjectLocation(const std::string& object_name, 
                                     CephFSObjectInfo& info) {
    std::string cmd = "ceph osd map " + cephfs_data_pool_ + " " + object_name + " 2>/dev/null";
    std::string result = ExecuteCommand(cmd);
    
    if (result.empty()) {
        return false;
    }
    
    return ParseCephLocation(result, info.pg_id, info.primary_osd, info.acting_set);
}

bool CephFSMonitor::ParseCephLocation(const std::string& location_output, 
                                     std::string& pg_id, 
                                     int& primary_osd, 
                                     std::vector<int>& acting_set) {
    // PG ID 추출: "pg 1.a3c" 형태
    size_t pg_pos = location_output.find("pg ");
    if (pg_pos != std::string::npos) {
        size_t pg_start = pg_pos + 3;
        size_t pg_end = location_output.find(" ", pg_start);
        if (pg_end != std::string::npos) {
            pg_id = location_output.substr(pg_start, pg_end - pg_start);
        }
    }
    
    // Primary OSD 추출: "p5" 형태
    size_t primary_pos = location_output.find(", p");
    if (primary_pos != std::string::npos) {
        size_t primary_start = primary_pos + 3;
        size_t primary_end = location_output.find(")", primary_start);
        if (primary_end != std::string::npos) {
            std::string primary_str = location_output.substr(primary_start, primary_end - primary_start);
            primary_osd = std::stoi(primary_str);
        }
    }
    
    // Acting set 추출: "acting ([5,12,8]" 형태
    size_t acting_pos = location_output.find("acting ([");
    if (acting_pos != std::string::npos) {
        size_t acting_start = acting_pos + 9;
        size_t acting_end = location_output.find("]", acting_start);
        if (acting_end != std::string::npos) {
            std::string acting_str = location_output.substr(acting_start, acting_end - acting_start);
            
            std::istringstream iss(acting_str);
            std::string osd_str;
            while (std::getline(iss, osd_str, ',')) {
                if (!osd_str.empty()) {
                    acting_set.push_back(std::stoi(osd_str));
                }
            }
        }
    }
    
    return !pg_id.empty() && primary_osd >= 0;
}

void CephFSMonitor::LogToCSV(const CephFSObjectInfo& info, 
                            const std::string& csv_path,
                            const std::string& event_type) {
    std::ofstream csv(csv_path, std::ios::app);
    if (csv.is_open()) {
        // acting_set을 문자열로 변환
        std::string acting_str;
        for (size_t i = 0; i < info.acting_set.size(); ++i) {
            if (i > 0) acting_str += ",";
            acting_str += std::to_string(info.acting_set[i]);
        }
        
        csv << info.timestamp << ","
            << event_type << ","
            << info.sst_filename << ","
            << info.inode << ","
            << info.object_name << ","
            << info.pg_id << ","
            << info.primary_osd << ","
            << "\"" << acting_str << "\","
            << info.file_size << std::endl;
        csv.close();
    }
}

std::string CephFSMonitor::GetTimestamp() {
    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);
    
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

std::string CephFSMonitor::ExecuteCommand(const std::string& command) {
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return "";
    
    std::string result;
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        result += buffer;
    }
    pclose(pipe);
    
    // 마지막 개행 문자 제거
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    
    return result;
}

} // namespace rocksdb