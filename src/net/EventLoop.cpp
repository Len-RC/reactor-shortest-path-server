#include "net/EventLoop.h"

#include "net/Handler.h"

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <sys/epoll.h>
#include <unistd.h>

EventLoop::EventLoop() : epfd_(epoll_create(1)), stop_(false) {
    if (epfd_ == -1) {
        std::cerr << "epoll创建失败\n";
        exit(EXIT_FAILURE);
    }

    if (pipe(pipe_fds_) == -1) {
        std::cerr << "管道创建失败\n";
        exit(EXIT_FAILURE);
    }

    fcntl(pipe_fds_[0], F_SETFL, fcntl(pipe_fds_[0], F_GETFL) | O_NONBLOCK);
    fcntl(pipe_fds_[1], F_SETFL, fcntl(pipe_fds_[1], F_GETFL) | O_NONBLOCK);

    struct epoll_event ev = {0};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = pipe_fds_[0];
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, pipe_fds_[0], &ev) == -1) {
        std::cerr << "管道注册到epoll失败\n";
        exit(EXIT_FAILURE);
    }
}

EventLoop::~EventLoop() {
    stop();
    close(epfd_);
    close(pipe_fds_[0]);
    close(pipe_fds_[1]);
}

void EventLoop::loop() {
    stop_ = false;
    struct epoll_event events[1024] = {0};

    while (!stop_) {
        int nfds = epoll_wait(epfd_, events, 1024, -1);
        if (nfds == -1) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "epoll_wait失败\n";
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == pipe_fds_[0]) {
                handlePipeNotify();
                continue;
            }

            Handler* handler = static_cast<Handler*>(events[i].data.ptr);
            if (!handler) {
                continue;
            }

            if (events[i].events & (EPOLLIN | EPOLLPRI)) {
                handler->handleRead();
            }

            if (events[i].events & EPOLLOUT) {
                std::cout << "检测到EPOLLOUT事件，准备向客户端发送结果\n";
                handler->handleWrite();
            }

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                handler->handleClose();
            }
        }
    }
}

void EventLoop::stop() {
    stop_ = true;
}

void EventLoop::updateHandler(Handler* handler) {
    struct epoll_event ev = {0};
    ev.events = handler->getEvents();
    ev.data.ptr = handler;

    int op = EPOLL_CTL_MOD;
    auto it = fd_to_handler.find(handler->getFd());

    if (it == fd_to_handler.end()) {
        op = EPOLL_CTL_ADD;
        std::lock_guard<std::mutex> lock(mtx_);
        fd_to_handler[handler->getFd()] = handler;
    }

    if (epoll_ctl(epfd_, op, handler->getFd(), &ev) == -1) {
        std::cerr << "epoll_ctl失败\n";
    }
}

void EventLoop::removeHandler(Handler* handler) {
    epoll_ctl(epfd_, EPOLL_CTL_DEL, handler->getFd(), nullptr);
    std::lock_guard<std::mutex> lock(mtx_);
    fd_to_handler.erase(handler->getFd());
}

void EventLoop::sendToReactor(int fd, const std::string& res) {
    {
        std::lock_guard<std::mutex> lk(q_mtx_);
        msg_q_.push(PendingMsg{fd, res});
    }

    uint8_t one = 1;
    ssize_t n = write(pipe_fds_[1], &one, 1);

    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        std::cerr << "唤醒管道写入失败: " << strerror(errno) << "\n";
    }
}

void EventLoop::handlePipeNotify() {
    while (true) {
        uint8_t buf[256];
        ssize_t n = read(pipe_fds_[0], buf, sizeof(buf));
        if (n > 0) {
            continue;
        }

        if (n == 0) {
            std::cerr << "管道写端被关闭\n";
            break;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }

        std::cerr << "读取唤醒管道失败: " << strerror(errno) << "\n";
        break;
    }

    std::queue<PendingMsg> local;
    {
        std::lock_guard<std::mutex> lk(q_mtx_);
        std::swap(local, msg_q_);
    }

    while (!local.empty()) {
        PendingMsg msg = std::move(local.front());
        local.pop();

        Handler* handler = nullptr;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            auto it = fd_to_handler.find(msg.fd);
            if (it != fd_to_handler.end()) {
                handler = it->second;
            }
        }

        if (!handler) {
            continue;
        }

        handler->setOutBuffer(msg.data);
        handler->setEvents(EPOLLIN | EPOLLOUT);
        updateHandler(handler);
    }
}
