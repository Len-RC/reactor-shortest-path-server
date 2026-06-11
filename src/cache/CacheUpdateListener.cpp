#include "cache/CacheUpdateListener.h"

#include "db/ConnectionPool.h"

#include <iostream>

CacheUpdateListener::CacheUpdateListener() : sub_ctx_(nullptr) {}

CacheUpdateListener::~CacheUpdateListener() {
    stop();
}

void CacheUpdateListener::start(int port) {
    sub_ctx_ = redisConnect("127.0.0.1", 6379);
    if (sub_ctx_ == nullptr || sub_ctx_->err) {
        std::cerr << "订阅器Redis连接失败\n";
        return;
    }

    std::cout << "启动缓存更新监听器（端口 " << port << "）...\n";

    listener_thread_ = std::thread([this]() {
        redisReply* reply = (redisReply*)redisCommand(sub_ctx_, "SUBSCRIBE cache_update");
        if (reply) {
            std::cout << "已订阅 cache_update 频道\n";
            freeReplyObject(reply);
        }

        while (!stop_) {
            redisReply* msg;
            if (redisGetReply(sub_ctx_, (void**)&msg) == REDIS_OK) {
                if (msg->type == REDIS_REPLY_ARRAY && msg->elements == 3) {
                    std::cout << "\n收到缓存更新通知: " << msg->element[2]->str << "\n";
                    std::cout << "开始刷新缓存...\n";

                    ConnectionPool::getInstance().updateCache();

                    std::cout << "缓存刷新完成\n";
                }
                freeReplyObject(msg);
            }
        }
    });
}

void CacheUpdateListener::stop() {
    stop_ = true;
    if (sub_ctx_) {
        redisFree(sub_ctx_);
        sub_ctx_ = nullptr;
    }
    if (listener_thread_.joinable()) {
        listener_thread_.detach();
    }
}
