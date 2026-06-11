#include "thread/ThreadPool.h"

#include <iostream>

ThreadPool::ThreadPool(int num) : stop(false) {
    for (int i = 0; i < num; ++i) {
        workers.emplace_back([this, i]() {
            while (true) {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [this]() { return stop || !tasks.empty(); });
                if (stop && tasks.empty()) {
                    return;
                }

                std::function<void()> task = std::move(tasks.front());
                tasks.pop();
                lock.unlock();

                std::cout << "线程" << i << "正在处理客户端请求\n";
                task();
                std::cout << "线程" << i << "请求处理完成\n";
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(mtx);
        stop = true;
    }
    cv.notify_all();
    for (auto& t : workers) {
        if (t.joinable()) {
            t.join();
        }
    }
}
