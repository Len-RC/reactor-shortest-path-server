#include "db/ConnectionPool.h"

#include "cache/RedisPool.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <thread>
#include <unordered_map>
#include <vector>

#include <hiredis/hiredis.h>

namespace {
std::string trim(const std::string& value) {
    const char* whitespace = " \t\r\n";
    size_t begin = value.find_first_not_of(whitespace);
    if (begin == std::string::npos) {
        return "";
    }
    size_t end = value.find_last_not_of(whitespace);
    return value.substr(begin, end - begin + 1);
}

std::unordered_map<std::string, std::string> loadKeyValueFile(const std::string& path) {
    std::unordered_map<std::string, std::string> values;
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cout << "MySQL配置文件未找到: " << path << "，使用环境变量/默认配置\n";
        return values;
    }

    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        size_t pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }

        std::string key = trim(line.substr(0, pos));
        std::string value = trim(line.substr(pos + 1));
        if (!key.empty()) {
            values[key] = value;
        }
    }

    std::cout << "已读取MySQL配置文件: " << path << "\n";
    return values;
}

std::string getConfigValue(const std::unordered_map<std::string, std::string>& config,
    const std::string& key, const char* env_name, const std::string& default_value) {
    auto it = config.find(key);
    if (it != config.end() && !it->second.empty()) {
        return it->second;
    }

    const char* env_value = std::getenv(env_name);
    if (env_value && env_value[0] != '\0') {
        return env_value;
    }

    return default_value;
}

unsigned int getConfigUIntValue(const std::unordered_map<std::string, std::string>& config,
    const std::string& key, const char* env_name, unsigned int default_value) {
    std::string value = getConfigValue(config, key, env_name, std::to_string(default_value));
    int parsed = std::atoi(value.c_str());
    if (parsed <= 0 || parsed > 65535) {
        return default_value;
    }
    return static_cast<unsigned int>(parsed);
}
}

long long ConnectionPool::makeKey(int start_id, int end_id) {
    return ((long long)start_id << 32) | (unsigned int)end_id;
}

void ConnectionPool::loadCacheData(MYSQL* conn, CacheData& cache) {
    // 只加载 place 表（地点名称映射）
    if (mysql_query(conn, "SELECT id, name FROM place;") != 0) {
        std::cerr << "加载place表失败\n";
        return;
    }
    MYSQL_RES* res = mysql_store_result(conn);
    if (res) {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res))) {
            int id = std::atoi(row[0]);
            std::string name = row[1] ? row[1] : "";
            cache.name_to_id[name] = id;
            cache.id_to_name[id] = name;
        }
        mysql_free_result(res);
    }

    std::cout << "缓存加载完成: place=" << cache.name_to_id.size() << "\n";
}

void ConnectionPool::loadPlaceCache(MYSQL* conn) {
    std::lock_guard<std::mutex> lock(_updateMutex);
    if (_cacheLoaded) {
        return;
    }

    std::cout << "首次加载缓存到版本 0...\n";
    loadCacheData(conn, _cache[0]);
    _cacheLoaded = true;

    auto redis_conn = RedisPool::getInstance().getConnection();
    if (redis_conn) {
        redisReply* ver_reply = (redisReply*)redisCommand(redis_conn.get(), "GET current_version");
        if (ver_reply && ver_reply->type == REDIS_REPLY_STRING) {
            int redis_version = std::atoi(ver_reply->str);
            _redis_current_version.store(redis_version, std::memory_order_release);
            std::cout << "初始化 Redis 版本号: " << redis_version << "\n";
            freeReplyObject(ver_reply);
        } else {
            std::cout << "Redis 版本号未找到，使用默认值 0\n";
            if (ver_reply) {
                freeReplyObject(ver_reply);
            }
        }
    }
}

ConnectionPool::ConnectionPool()
    : _ip("10.4.87.70"),
      _port(3306),
      _username("root"),
      _password("root"),
      _dbname("reactor_db"),
      _initSize(4),
      _maxSize(8),
      _connectionCnt(0),
      _maxTimeout(5),
      _stop(false) {
    const char* config_path_env = std::getenv("MYSQL_CONFIG");
    std::string config_path = config_path_env ? config_path_env : "config/mysql.conf";
    auto config = loadKeyValueFile(config_path);

    _ip = getConfigValue(config, "MYSQL_HOST", "MYSQL_HOST", _ip);
    _port = getConfigUIntValue(config, "MYSQL_PORT", "MYSQL_PORT", _port);
    _username = getConfigValue(config, "MYSQL_USER", "MYSQL_USER", _username);
    _password = getConfigValue(config, "MYSQL_PASS", "MYSQL_PASS", _password);
    _dbname = getConfigValue(config, "MYSQL_DB", "MYSQL_DB", _dbname);

    if (mysql_library_init(0, nullptr, nullptr) != 0) {
        std::cerr << "MySQL全局库初始化失败\n";
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < _initSize; i++) {
        MYSQL* p = createConnection();
        if (p == nullptr) {
            std::cerr << "初始化第" << i + 1 << "个连接失败，程序退出\n";
            exit(EXIT_FAILURE);
        }

        _connectionQue.push(p);
        _connectionCnt++;
        std::cout << "MySQL连接池：创建第" << i + 1 << "个初始连接成功\n";
    }

    std::thread produce(std::bind(&ConnectionPool::produceConnection, this));
    produce.detach();
    std::cout << "连接池初始化完毕\n";
}

ConnectionPool::~ConnectionPool() {
    _stop = true;

    cv_in.notify_all();
    cv_out.notify_all();

    std::unique_lock<std::mutex> lock(_queueMutex);
    while (!_connectionQue.empty()) {
        MYSQL* conn = _connectionQue.front();
        _connectionQue.pop();
        mysql_close(conn);
        _connectionCnt--;
    }

    mysql_library_end();
    std::cout << "连接池已关闭\n";
}

MYSQL* ConnectionPool::createConnection() {
    MYSQL* conn = mysql_init(nullptr);
    if (conn == nullptr) {
        std::cerr << "mysql_init失败\n";
        return nullptr;
    }

    if (!mysql_real_connect(conn, _ip.c_str(), _username.c_str(), _password.c_str(),
            _dbname.c_str(), _port, nullptr, 0)) {
        std::cerr << "mysql_real_connect失败\n";
        mysql_close(conn);
        return nullptr;
    }
    return conn;
}

void ConnectionPool::produceConnection() {
    for (;;) {
        std::unique_lock<std::mutex> lock(_queueMutex);
        cv_in.wait(lock, [this]() {
            return _stop || _connectionQue.empty();
        });

        if (_stop) {
            break;
        }

        while (_connectionCnt < _maxSize && _connectionQue.empty()) {
            MYSQL* p = createConnection();
            if (p == nullptr) {
                std::cerr << "创建连接失败，1秒后重试\n";
                std::this_thread::sleep_for(std::chrono::seconds(1));
                break;
            }
            _connectionQue.push(p);
            _connectionCnt++;
            std::cout << "MySQL连接池：动态创建连接，当前总连接数：" << _connectionCnt << "\n";
        }
        cv_out.notify_all();
    }
}

ConnectionPool& ConnectionPool::getInstance() {
    static ConnectionPool instance;
    return instance;
}

std::shared_ptr<MYSQL> ConnectionPool::getConnection() {
    std::unique_lock<std::mutex> lock(_queueMutex);
    if (!cv_out.wait_for(lock, std::chrono::seconds(_maxTimeout),
            [this]() { return !_connectionQue.empty() || _stop; })) {
        std::cout << "获取MySQL连接超时（超时时长：" << _maxTimeout << "秒）\n";
        return nullptr;
    }
    if (_stop) {
        return nullptr;
    }

    MYSQL* p = _connectionQue.front();
    std::shared_ptr<MYSQL> sp(p, [this](MYSQL* p) {
        std::unique_lock<std::mutex> lock(_queueMutex);
        _connectionQue.push(p);
        cv_in.notify_one();
    });
    _connectionQue.pop();

    if (_connectionQue.empty()) {
        cv_in.notify_one();
    }

    return sp;
}

void ConnectionPool::updateCache() {
    std::lock_guard<std::mutex> lock(_updateMutex);

    std::cout << "开始更新缓存...\n";

    std::shared_ptr<MYSQL> conn_ptr = getConnection();
    if (!conn_ptr) {
        std::cerr << "更新缓存失败：无法获取数据库连接\n";
        return;
    }

    auto redis_conn = RedisPool::getInstance().getConnection();
    if (redis_conn) {
        redisReply* ver_reply = (redisReply*)redisCommand(redis_conn.get(), "GET current_version");
        if (ver_reply && ver_reply->type == REDIS_REPLY_STRING) {
            int new_redis_version = std::atoi(ver_reply->str);
            _redis_current_version.store(new_redis_version, std::memory_order_release);
            std::cout << "Redis 版本号已更新为: " << new_redis_version << "\n";
            freeReplyObject(ver_reply);
        } else if (ver_reply) {
            freeReplyObject(ver_reply);
        }
    }

    int current = _currentVersion.load(std::memory_order_acquire);
    int backup = 1 - current;

    _cache[backup].name_to_id.clear();
    _cache[backup].id_to_name.clear();

    loadCacheData(conn_ptr.get(), _cache[backup]);

    _currentVersion.store(backup, std::memory_order_release);

    std::cout << "缓存更新完成，版本切换: " << current << " -> " << backup << "\n";
}

std::string ConnectionPool::handleClientRequest(const std::string& client_input) {
    std::shared_ptr<MYSQL> conn_ptr = getConnection();
    MYSQL* conn = conn_ptr.get();

    std::string final_result = "无最短距离";
    std::string start_place = "未知";
    std::string end_place = "未知";

    if (!conn) {
        return final_result + "\n";
    }

    loadPlaceCache(conn);

    int version = _currentVersion.load(std::memory_order_acquire);
    const CacheData& cache = _cache[version];

    auto redis_conn = RedisPool::getInstance().getConnection();
    if (!redis_conn) {
        std::cerr << "获取Redis连接失败，降级到MySQL\n";
        return "服务暂时不可用\n";
    }

    try {
        size_t space_pos = client_input.find(' ');

        if (space_pos == std::string::npos || space_pos == 0 || space_pos == client_input.size() - 1) {
            final_result = "输入格式错,格式：出发地 目的地（如：林1 林2）";
            return final_result + "\n";
        }

        start_place = client_input.substr(0, space_pos);
        end_place = client_input.substr(space_pos + 1);
        end_place.erase(std::remove_if(end_place.begin(), end_place.end(),
            [](char c) { return c == '\n' || c == '\r'; }), end_place.end());

        int start_id = -1;
        int end_id = -1;
        {
            auto it1 = cache.name_to_id.find(start_place);
            auto it2 = cache.name_to_id.find(end_place);
            if (it1 == cache.name_to_id.end() || it2 == cache.name_to_id.end()) {
                final_result = "地点不存在，重新输入";
                return final_result + "\n";
            }
            start_id = it1->second;
            end_id = it2->second;
        }

        int redis_version = _redis_current_version.load(std::memory_order_acquire);
        std::string version_prefix = "v" + std::to_string(redis_version);

        std::string dist_key = version_prefix + ":dist:" + std::to_string(start_id) + ":" + std::to_string(end_id);
        redisReply* dist_reply = (redisReply*)redisCommand(redis_conn.get(), "GET %s", dist_key.c_str());

        if (!dist_reply || dist_reply->type != REDIS_REPLY_STRING) {
            if (dist_reply) {
                freeReplyObject(dist_reply);
            }
            final_result = "无最短距离";
            return final_result + "\n";
        }

        int dist = std::atoi(dist_reply->str);
        std::string dist_str = std::to_string(dist);
        freeReplyObject(dist_reply);

        std::vector<int> path_ids;
        int cur = end_id;
        path_ids.push_back(cur);

        while (cur != start_id) {
            std::string prev_key = version_prefix + ":prev:" + std::to_string(start_id) + ":" + std::to_string(cur);
            redisReply* prev_reply = (redisReply*)redisCommand(redis_conn.get(), "GET %s", prev_key.c_str());

            if (!prev_reply || prev_reply->type != REDIS_REPLY_STRING) {
                if (prev_reply) {
                    freeReplyObject(prev_reply);
                }
                final_result = "路径回溯失败";
                return final_result + "\n";
            }

            int prev_id = std::atoi(prev_reply->str);
            freeReplyObject(prev_reply);

            path_ids.push_back(prev_id);
            cur = prev_id;
        }

        std::reverse(path_ids.begin(), path_ids.end());

        std::string path_str;
        for (size_t i = 0; i < path_ids.size(); ++i) {
            if (i > 0) {
                path_str += "->";
            }

            auto name_it = cache.id_to_name.find(path_ids[i]);
            if (name_it != cache.id_to_name.end()) {
                path_str += name_it->second;
            } else {
                path_str += "未知(" + std::to_string(path_ids[i]) + ")";
            }
        }

        final_result = "最短距离=" + dist_str + "，路径=" + path_str;
        return final_result + "\n";
    } catch (const std::exception& e) {
        std::cerr << "handleClientRequest 问题:" << e.what() << "\n";
        return final_result + "\n";
    }
}
