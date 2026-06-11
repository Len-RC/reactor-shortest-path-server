#pragma once

#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>

class Handler;

struct PendingMsg {
    int fd;
    std::string data;
};

class EventLoop {
public:
    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void loop();
    void stop();
    void updateHandler(Handler* handler);
    void removeHandler(Handler* handler);
    void sendToReactor(int fd, const std::string& res);

private:
    int epfd_;
    bool stop_;
    int pipe_fds_[2];
    std::unordered_map<int, Handler*> fd_to_handler;
    std::mutex mtx_;
    std::mutex q_mtx_;
    std::queue<PendingMsg> msg_q_;

    void handlePipeNotify();
};
