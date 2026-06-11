#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>

#include <mysql/mysql.h>

class ConnectionPool {
private:
    std::string _ip;
    unsigned int _port;
    std::string _username;
    std::string _password;
    std::string _dbname;
    int _initSize;
    int _maxSize;
    int _connectionCnt;
    int _maxTimeout;
    bool _stop;
    std::queue<MYSQL*> _connectionQue;
    std::mutex _queueMutex;
    std::condition_variable cv_in;
    std::condition_variable cv_out;

    struct CacheData {
        std::unordered_map<std::string, int> name_to_id;
        std::unordered_map<int, std::string> id_to_name;
    };

    CacheData _cache[2];
    std::atomic<int> _currentVersion{0};
    std::atomic<int> _redis_current_version{0};
    std::mutex _updateMutex;
    bool _cacheLoaded = false;

    static long long makeKey(int start_id, int end_id);
    void loadCacheData(MYSQL* conn, CacheData& cache);
    void loadPlaceCache(MYSQL* conn);

    ConnectionPool();
    MYSQL* createConnection();
    void produceConnection();

public:
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;
    ~ConnectionPool();

    static ConnectionPool& getInstance();

    std::shared_ptr<MYSQL> getConnection();
    void updateCache();
    std::string handleClientRequest(const std::string& client_input);
};
