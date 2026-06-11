#include "net/ConnectionHandler.h"

#include "db/ConnectionPool.h"
#include "net/EventLoop.h"
#include "thread/ThreadPool.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/socket.h>

ConnectionHandler::ConnectionHandler(int connfd, std::shared_ptr<ThreadPool> pool, EventLoop* loop)
    : Handler(loop, connfd), pool_(pool), in_buffer_(), is_http_(false), http_parsed_(false) {
    setEvents(EPOLLIN);
    loop->updateHandler(this);
}

std::string ConnectionHandler::urlDecode(const std::string& str) {
    std::string result;
    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '%' && i + 2 < str.length()) {
            int value;
            std::istringstream is(str.substr(i + 1, 2));
            if (is >> std::hex >> value) {
                result += static_cast<char>(value);
                i += 2;
            } else {
                result += str[i];
            }
        } else if (str[i] == '+') {
            result += ' ';
        } else {
            result += str[i];
        }
    }
    return result;
}

bool ConnectionHandler::parseHttpRequest(std::string& start_place, std::string& end_place,
    std::string& api_path, std::string& request_body) {
    size_t header_end = in_buffer_.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return false;
    }

    std::string header = in_buffer_.substr(0, header_end);
    std::string method;
    api_path.clear();
    request_body.clear();

    if (header.substr(0, 3) == "GET") {
        method = "GET";
        is_http_ = true;
    } else if (header.substr(0, 4) == "POST") {
        method = "POST";
        is_http_ = true;
        if (in_buffer_.size() > header_end + 4) {
            request_body = in_buffer_.substr(header_end + 4);
        }
    } else {
        return false;
    }

    size_t path_start = header.find(' ') + 1;
    size_t path_end = header.find(' ', path_start);
    std::string path = header.substr(path_start, path_end - path_start);
    api_path = path;

    if (path == "/" || path.find("/?") == 0 || path.find("/api/query") == 0) {
        if (path == "/") {
            return true;
        }

        size_t query_start = path.find('?');
        if (query_start != std::string::npos) {
            std::string query = path.substr(query_start + 1);

            size_t start_pos = query.find("start=");
            if (start_pos != std::string::npos) {
                start_pos += 6;
                size_t start_end = query.find('&', start_pos);
                start_place = query.substr(start_pos, start_end - start_pos);
                start_place = urlDecode(start_place);
            }

            size_t end_pos = query.find("end=");
            if (end_pos != std::string::npos) {
                end_pos += 4;
                size_t end_end = query.find('&', end_pos);
                end_place = query.substr(end_pos, end_end - end_pos);
                end_place = urlDecode(end_place);
            }
        }
        return true;
    }

    if (path.find("/api/proxy") == 0 || path.find("/api/codex") == 0 ||
        path.find("/api/relay") == 0 || path.find("/api/v1/") == 0) {
        return true;
    }

    return false;
}

std::string ConnectionHandler::readHtmlFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        return "<html><body><h1>404 Not Found</h1></body></html>";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string ConnectionHandler::generateHtmlPage() {
    std::string html = readHtmlFile("static/index.html");

    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n"
             << "Content-Type: text/html; charset=utf-8\r\n"
             << "Content-Length: " << html.length() << "\r\n"
             << "Connection: keep-alive\r\n"
             << "Keep-Alive: timeout=60, max=100\r\n"
             << "\r\n"
             << html;

    return response.str();
}

void ConnectionHandler::handleRead() {
    char buf[4096];

    while (true) {
        ssize_t n = recv(getFd(), buf, sizeof(buf), 0);

        if (n > 0) {
            in_buffer_.append(buf, n);

            if (!http_parsed_) {
                if (in_buffer_.size() >= 4 &&
                    (in_buffer_.substr(0, 4) == "GET " ||
                        (in_buffer_.size() >= 5 && in_buffer_.substr(0, 5) == "POST "))) {
                    std::string start_place;
                    std::string end_place;
                    std::string api_path;
                    std::string request_body;

                    if (parseHttpRequest(start_place, end_place, api_path, request_body)) {
                        http_parsed_ = true;
                        in_buffer_.clear();

                        int fd = getFd();
                        EventLoop* loop_ = getLoop();

                        if (start_place.empty() || end_place.empty()) {
                            std::string html_response = generateHtmlPage();
                            loop_->sendToReactor(fd, html_response);
                        } else {
                            pool_->submit([loop_, start_place, end_place, fd]() {
                                try {
                                    ConnectionPool& mysql_pool = ConnectionPool::getInstance();
                                    std::string query = start_place + " " + end_place;
                                    std::string result = mysql_pool.handleClientRequest(query);

                                    std::ostringstream response;
                                    response << "HTTP/1.1 200 OK\r\n"
                                             << "Content-Type: text/plain; charset=utf-8\r\n"
                                             << "Content-Length: " << result.length() << "\r\n"
                                             << "Connection: keep-alive\r\n"
                                             << "Keep-Alive: timeout=60, max=100\r\n"
                                             << "\r\n"
                                             << result;

                                    loop_->sendToReactor(fd, response.str());
                                } catch (const std::exception& e) {
                                    std::cerr << "HTTP查询异常: " << e.what() << "\n";
                                    std::string error_response = "HTTP/1.1 500 Internal Server Error\r\n\r\n查询失败\n";
                                    loop_->sendToReactor(fd, error_response);
                                }
                            });
                        }
                        return;
                    }
                } else if (in_buffer_.size() >= 1 && in_buffer_[0] != 'G' && in_buffer_[0] != 'P') {
                    http_parsed_ = true;
                    is_http_ = false;
                } else if (in_buffer_.find('\n') != std::string::npos) {
                    http_parsed_ = true;
                    is_http_ = false;
                }
            }

            if (!is_http_ && http_parsed_) {
                while (true) {
                    size_t pos = in_buffer_.find('\n');

                    if (pos == std::string::npos) {
                        break;
                    }

                    std::string line = in_buffer_.substr(0, pos + 1);
                    in_buffer_.erase(0, pos + 1);

                    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
                        line.pop_back();
                    }

                    if (line.empty()) {
                        continue;
                    }

                    int fd = getFd();
                    EventLoop* loop_ = getLoop();

                    pool_->submit([loop_, line, fd]() {
                        try {
                            ConnectionPool& mysql_pool = ConnectionPool::getInstance();
                            std::string result = mysql_pool.handleClientRequest(line);
                            loop_->sendToReactor(fd, result);
                        } catch (const std::exception& e) {
                            std::cerr << "客户端fd:" << fd << " 查询异常: " << e.what() << "\n";
                            loop_->sendToReactor(fd, "无\n");
                        }
                    });
                }
            }
            continue;
        }

        if (n == 0) {
            handleClose();
            return;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }

        if (errno == EINTR) {
            continue;
        }

        std::cerr << "客户端fd:" << getFd() << " recv失败: " << strerror(errno) << "\n";
        handleClose();
        return;
    }
}

void ConnectionHandler::handleWrite() {
    std::string& out_buffer = getOutBuffer();
    ssize_t n = send(getFd(), out_buffer.c_str(), out_buffer.size(), 0);

    if (n > 0) {
        std::cout << "向客户端fd:" << getFd() << " 发送结果（" << (is_http_ ? "HTTP" : "TCP") << "）\n";
        out_buffer.erase(0, n);
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        std::cerr << "向客户端发送数据失败\n";
        handleClose();
    }

    if (out_buffer.empty()) {
        if (is_http_) {
            http_parsed_ = false;
            in_buffer_.clear();
            setEvents(EPOLLIN);
            getLoop()->updateHandler(this);
        } else {
            setEvents(EPOLLIN);
            getLoop()->updateHandler(this);
        }
    }
}

void ConnectionHandler::handleClose() {
    std::cout << "客户端fd:" << getFd() << " 断开连接\n";
    getLoop()->removeHandler(this);
    delete this;
}
