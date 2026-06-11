#include "net/Acceptor.h"

#include "net/EventLoop.h"
#include "net/ReactorPool.h"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>

Acceptor::Acceptor(EventLoop* loop, int port, Reactors* reacc)
    : Handler(loop, socket(AF_INET, SOCK_STREAM, 0)),
      new_conn_cb_(nullptr),
      reac(reacc) {
    int flags = fcntl(getFd(), F_GETFL, 0);
    fcntl(getFd(), F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(getFd(), (sockaddr*)&addr, sizeof(addr)) == -1) {
        std::cerr << "监听绑定失败\n";
        exit(EXIT_FAILURE);
    }

    listen(getFd(), 128);
    setEvents(EPOLLIN);
    loop->updateHandler(this);
}

void Acceptor::handleRead() {
    while (true) {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);
        int connfd = accept(getFd(), (sockaddr*)&client_addr, &len);

        if (connfd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            std::cerr << "客户端连接\n";
            break;
        }

        std::cout << "Accept 新客户端连接，fd: " << connfd << "\n";

        fcntl(connfd, F_SETFL, fcntl(connfd, F_GETFL) | O_NONBLOCK);

        if (new_conn_cb_) {
            new_conn_cb_(connfd, reac->getreactors()[reac->selectl(connfd)]);
        }
    }
}

void Acceptor::handleWrite() {}

void Acceptor::handleClose() {
    getLoop()->removeHandler(this);
    delete this;
}

void Acceptor::setNewConnectionCallback(NewConnectionCallback cb) {
    new_conn_cb_ = std::move(cb);
}
