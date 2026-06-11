#pragma once

#include "net/Handler.h"

#include <functional>

class EventLoop;
class Reactors;

class Acceptor : public Handler {
public:
    using NewConnectionCallback = std::function<void(int connfd, EventLoop* loopp)>;

    Acceptor(EventLoop* loop, int port, Reactors* reacc);

    void handleRead() override;
    void handleWrite() override;
    void handleClose() override;

    void setNewConnectionCallback(NewConnectionCallback cb);

private:
    NewConnectionCallback new_conn_cb_;
    Reactors* reac;
};
