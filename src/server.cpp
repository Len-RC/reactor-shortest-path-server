#include "cache/CacheUpdateListener.h"
#include "cache/RedisPool.h"
#include "db/ConnectionPool.h"
#include "net/Acceptor.h"
#include "net/ConnectionHandler.h"
#include "net/EventLoop.h"
#include "net/ReactorPool.h"
#include "thread/ThreadPool.h"

#include <cstdlib>
#include <exception>
#include <hiredis/hiredis.h>
#include <iostream>
#include <memory>
#include <unistd.h>

void exit_handler() {
    std::cout << "服务器开始优雅退出，释放所有资源...\n";
}

int main(int argc, char* argv[]) {
    int port = 9090;
    if (argc > 1) {
        port = std::atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            std::cerr << "无效的端口号: " << argv[1] << "\n";
            std::cerr << "用法: " << argv[0] << " [端口号]\n";
            std::cerr << "示例: " << argv[0] << " 9090\n";
            return EXIT_FAILURE;
        }
    }

    atexit(exit_handler);

    try {
        RedisPool& redis_pool = RedisPool::getInstance();
        auto conn = redis_pool.getConnection();
        if (conn) {
            redisReply* reply = (redisReply*)redisCommand(conn.get(), "PING");
            if (reply) {
                std::cout << "Redis连接测试成功: " << reply->str << "\n";
                freeReplyObject(reply);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Redis连接池初始化失败: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    try {
        ConnectionPool::getInstance();
        std::cout << "MySQL连接池初始化完成\n";
    } catch (const std::exception& e) {
        std::cerr << "MySQL连接池初始化失败: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    CacheUpdateListener cache_listener;
    cache_listener.start(port);

    EventLoop main_loop;
    Reactors reactors(4);
    auto thread_pool = std::make_shared<ThreadPool>(4);

    Acceptor acceptor(&main_loop, port, &reactors);
    acceptor.setNewConnectionCallback([thread_pool](int connfd, EventLoop* loop) {
        new ConnectionHandler(connfd, thread_pool, loop);
    });

    std::cout << "Reactor高并发查询服务器启动成功\n";
    std::cout << "监听端口: " << port << "\n";
    std::cout << "服务器 PID: " << getpid() << "\n";

    main_loop.loop();

    return 0;
}
