#include "net/Handler.h"

#include <unistd.h>

Handler::Handler(EventLoop* loop, int fd) : loop_(loop), fd_(fd), events_(0) {}

Handler::~Handler() {
    if (fd_ >= 0) {
        close(fd_);
    }
}

int Handler::getFd() const {
    return fd_;
}

EventLoop* Handler::getLoop() const {
    return loop_;
}

void Handler::setEvents(uint32_t events) {
    events_ = events;
}

uint32_t Handler::getEvents() const {
    return events_;
}

void Handler::setOutBuffer(const std::string& data) {
    out_buffer_ = data;
}

std::string& Handler::getOutBuffer() {
    return out_buffer_;
}
