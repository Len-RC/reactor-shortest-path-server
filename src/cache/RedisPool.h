#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

#include <hiredis/hiredis.h>

class RedisPool {
private:
    std::string host_;
    int port_;
    std::queue<redisContext*> pool_;
    std::mutex mtx_;
    std::condition_variable cv_;
    int init_size_;
    int max_size_;
    std::atomic<int> connection_count_{0};
    bool stop_;

    redisContext* createConnection();

public:
    RedisPool(const std::string& host = "127.0.0.1", int port = 6379, int init_size = 4, int max_size = 8);
    ~RedisPool();

    RedisPool(const RedisPool&) = delete;
    RedisPool& operator=(const RedisPool&) = delete;

    std::shared_ptr<redisContext> getConnection();

    static RedisPool& getInstance();
};
