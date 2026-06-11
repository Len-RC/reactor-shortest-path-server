#pragma once

#include <cstdint>
#include <string>

class EventLoop;

class Handler {
public:
    Handler(EventLoop* loop, int fd);
    virtual ~Handler();

    virtual void handleRead() = 0;
    virtual void handleWrite() = 0;
    virtual void handleClose() = 0;

    int getFd() const;
    EventLoop* getLoop() const;

    void setEvents(uint32_t events);
    uint32_t getEvents() const;

    void setOutBuffer(const std::string& data);
    std::string& getOutBuffer();

private:
    EventLoop* loop_;
    int fd_;
    uint32_t events_;
    std::string out_buffer_;
};
