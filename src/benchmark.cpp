#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <errno.h>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>

/*
 * 压测工具 - 模拟多个并发客户端向服务器发送查询请求
 *
 * 用法: ./benchmark <服务器IP> <端口> <并发数> <每个连接请求数> <起点> <终点>
 * 示例: ./benchmark 127.0.0.1 9090 100 50 林1 林2
 *
 * 参数说明:
 *   并发数     - 同时建立多少个TCP连接
 *   每个连接请求数 - 每个连接连续发送多少次查询
 */

std::atomic<int> success_count{0};
std::atomic<int> fail_count{0};
std::atomic<int> connect_fail{0};
std::atomic<long long> total_latency_us{0};  // 微秒

// 单个客户端线程的工作函数
void client_worker(const std::string& ip, int port,
                   int requests, const std::string& query) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        connect_fail++;
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    if (connect(sockfd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sockfd);
        connect_fail++;
        return;
    }

    std::string send_data = query + "\n";

    for (int i = 0; i < requests; ++i) {
        auto start = std::chrono::high_resolution_clock::now();

        // 发送请求
        ssize_t sent = send(sockfd, send_data.c_str(), send_data.size(), 0);
        if (sent <= 0) {
            fail_count++;
            continue;
        }

        // 接收响应（读到 \n 为止）
        std::string response;
        char buf[1024];
        bool got_response = false;

        while (true) {
            ssize_t n = recv(sockfd, buf, sizeof(buf), 0);
            if (n <= 0) {
                fail_count++;
                break;
            }
            response.append(buf, n);
            if (response.find('\n') != std::string::npos) {
                got_response = true;
                break;
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        long long us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

        if (got_response) {
            success_count++;
            total_latency_us += us;
        }
    }

    close(sockfd);
}

int main(int argc, char** argv) {
    if (argc < 7) {
        std::cerr << "用法: " << argv[0]
                  << " <IP> <端口> <并发数> <每连接请求数> <起点> <终点>\n"
                  << "示例: " << argv[0]
                  << " 127.0.0.1 9090 100 50 林1 林2\n";
        return 1;
    }

    std::string ip = argv[1];
    int port = std::atoi(argv[2]);
    int concurrency = std::atoi(argv[3]);
    int requests = std::atoi(argv[4]);
    std::string query = std::string(argv[5]) + " " + argv[6];

    int total_requests = concurrency * requests;

    std::cout << "========== 压测配置 ==========\n"
              << "目标服务器: " << ip << ":" << port << "\n"
              << "并发连接数: " << concurrency << "\n"
              << "每连接请求: " << requests << "\n"
              << "总请求数:   " << total_requests << "\n"
              << "查询内容:   " << query << "\n"
              << "==============================\n\n"
              << "压测开始...\n";

    auto start = std::chrono::high_resolution_clock::now();

    // 启动并发线程
    std::vector<std::thread> threads;
    threads.reserve(concurrency);
    for (int i = 0; i < concurrency; ++i) {
        threads.emplace_back(client_worker, ip, port, requests, query);
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    double elapsed_s = std::chrono::duration<double>(end - start).count();

    // 输出结果
    int succ = success_count.load();
    int fail = fail_count.load();
    int conn_fail = connect_fail.load();
    double avg_latency_ms = succ > 0 ? (total_latency_us.load() / (double)succ / 1000.0) : 0;
    double qps = succ / elapsed_s;

    std::cout << "\n========== 压测结果 ==========\n"
              << "总耗时:       " << elapsed_s << " 秒\n"
              << "成功请求:     " << succ << "\n"
              << "失败请求:     " << fail << "\n"
              << "连接失败:     " << conn_fail << "\n"
              << "QPS:          " << qps << " req/s\n"
              << "平均延迟:     " << avg_latency_ms << " ms\n"
              << "==============================\n";

    return 0;
}
