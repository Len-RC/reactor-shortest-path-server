#pragma once

#include "net/Handler.h"

#include <memory>
#include <string>

class EventLoop;
class ThreadPool;

class ConnectionHandler : public Handler {
public:
    ConnectionHandler(int connfd, std::shared_ptr<ThreadPool> pool, EventLoop* loop);

    std::string urlDecode(const std::string& str);
    bool parseHttpRequest(std::string& start_place, std::string& end_place,
        std::string& api_path, std::string& request_body);
    std::string readHtmlFile(const std::string& filename);
    std::string generateHtmlPage();

    void handleRead() override;
    void handleWrite() override;
    void handleClose() override;

private:
    std::shared_ptr<ThreadPool> pool_;
    std::string in_buffer_;
    bool is_http_;
    bool http_parsed_;
};
