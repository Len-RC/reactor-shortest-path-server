#pragma once

#include <atomic>
#include <thread>

#include <hiredis/hiredis.h>

class CacheUpdateListener {
private:
    redisContext* sub_ctx_;
    std::thread listener_thread_;
    std::atomic<bool> stop_{false};

public:
    CacheUpdateListener();
    ~CacheUpdateListener();

    void start(int port);
    void stop();
};
