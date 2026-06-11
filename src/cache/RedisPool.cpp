#include "cache/RedisPool.h"

#include <chrono>
#include <iostream>

redisContext* RedisPool::createConnection() {
    redisContext* ctx = redisConnect(host_.c_str(), port_);
    if (ctx == nullptr || ctx->err) {
        if (ctx) {
            std::cerr << "Redis连接失败: " << ctx->errstr << "\n";
            redisFree(ctx);
        } else {
            std::cerr << "Redis连接失败: 无法分配内存\n";
        }
        return nullptr;
    }
    return ctx;
}

RedisPool::RedisPool(const std::string& host, int port, int init_size, int max_size)
    : host_(host), port_(port), init_size_(init_size), max_size_(max_size), stop_(false) {
    for (int i = 0; i < init_size_; ++i) {
        redisContext* ctx = createConnection();
        if (ctx) {
            pool_.push(ctx);
            connection_count_++;
            std::cout << "Redis连接池：创建第" << i + 1 << "个初始连接成功\n";
        }
    }
    std::cout << "Redis连接池初始化完毕\n";
}

RedisPool::~RedisPool() {
    stop_ = true;
    cv_.notify_all();
    std::unique_lock<std::mutex> lock(mtx_);
    while (!pool_.empty()) {
        redisContext* ctx = pool_.front();
        pool_.pop();
        redisFree(ctx);
        connection_count_--;
    }
    std::cout << "Redis连接池已关闭\n";
}

std::shared_ptr<redisContext> RedisPool::getConnection() {
    std::unique_lock<std::mutex> lock(mtx_);
    if (!cv_.wait_for(lock, std::chrono::seconds(5), [this]() { return !pool_.empty() || stop_; })) {
        std::cerr << "获取Redis连接超时\n";
        return nullptr;
    }
    if (stop_) {
        return nullptr;
    }

    redisContext* ctx = pool_.front();
    pool_.pop();
    return std::shared_ptr<redisContext>(ctx, [this](redisContext* c) {
        std::unique_lock<std::mutex> lock(mtx_);
        pool_.push(c);
        cv_.notify_one();
    });
}

RedisPool& RedisPool::getInstance() {
    static RedisPool instance;
    return instance;
}
