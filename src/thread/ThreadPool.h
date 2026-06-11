#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

class ThreadPool {
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex mtx;
    std::condition_variable cv;
    bool stop;

public:
    explicit ThreadPool(int num = 4);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template<class T>
    void submit(T&& t) {
        {
            std::unique_lock<std::mutex> lock(mtx);
            if (stop) {
                throw std::runtime_error("线程池已关闭");
            }
            tasks.emplace(std::forward<T>(t));
        }
        cv.notify_one();
    }
};
