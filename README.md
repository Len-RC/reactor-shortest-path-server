高性能最短路径查询系统
License C++ Performance

一个基于 C++ 实现的高性能最短路径查询系统，采用 Reactor 网络模型，支持 12w+ QPS，适用于大规模图数据的实时查询场景。

✨ 核心特性
🚀 高性能: HTTP 协议下 QPS 达到 12w+，平均延迟 4-9ms
🔧 手写框架: 基于 Epoll 的 Reactor 网络模型，非阻塞 I/O
💪 高并发: 支持 1000+ 并发连接，多线程 + 线程池架构
🎯 负载均衡: Nginx 反向代理，支持多实例部署
💾 双层缓存: MySQL + Redis，双缓冲机制实现零停机更新
🌐 双协议支持: 同时支持 HTTP 和 TCP 协议
📊 性能指标
指标	数值
QPS (HTTP)	120,000 req/s
QPS (TCP 直连)	28,700 req/s
平均延迟	4-9 ms
并发连接	1000+
成功率	99.999%
🏗️ 系统架构
                    ┌─────────────┐
                    │   客户端     │
                    └──────┬──────┘
                           │
                    ┌──────▼──────┐
                    │   Nginx     │ (负载均衡)
                    │  端口 80    │
                    └──────┬──────┘
                           │
        ┌──────────────────┼──────────────────┐
        │                  │                  │
   ┌────▼────┐       ┌────▼────┐       ┌────▼────┐
   │ 后端1   │       │ 后端2   │       │ 后端3   │
   │ :9090   │       │ :9091   │       │ :9092   │
   └────┬────┘       └────┬────┘       └────┬────┘
        │                  │                  │
        └──────────────────┼──────────────────┘
                           │
                    ┌──────▼──────┐
                    │    Redis    │ (缓存层)
                    │   MySQL     │ (持久化)
                    └─────────────┘
🛠️ 技术栈
核心技术
语言: C++17
网络模型: Reactor (主从模式)
I/O 多路复用: Epoll (边缘触发)
并发: 多线程 + 线程池
数据库: MySQL (连接池)
缓存: Redis (主从复制)
负载均衡: Nginx
关键技术点
非阻塞 I/O
双缓冲机制
连接池管理
零拷贝优化
HTTP Keep-Alive
🚀 快速开始
环境要求
- Linux (CentOS 7+/Ubuntu 18.04+)
- GCC 7.0+ (支持 C++17)
- CMake 3.10+
- MySQL 5.7+
- Redis 5.0+
- Nginx 1.14+
编译安装
# 1. 克隆项目
git clone https://github.com/your-username/shortest-path-system.git
cd shortest-path-system

# 2. 安装依赖
sudo yum install -y mysql-devel hiredis-devel  # CentOS
# 或
sudo apt install -y libmysqlclient-dev libhiredis-dev  # Ubuntu

# 3. 编译
mkdir build && cd build
cmake ..
make -j4

# 4. 初始化数据库
mysql -u root -p < ../sql/init.sql

# 5. 启动 Redis
redis-server

# 6. 启动后端服务
cd ..
bash start_services.sh

# 7. 配置 Nginx (可选)
sudo cp nginx.conf /etc/nginx/conf.d/shortest-path.conf
sudo nginx -s reload
使用示例
HTTP 请求
# 查询从"林1"到"林2"的最短路径
curl "http://localhost/?start=林1&end=林2"

# 响应示例
最短距离=10，路径=林1->林3->林2
TCP 请求
# 使用 telnet
echo "林1 林2" | telnet localhost 9090
📈 性能测试
HTTP 压测
# 使用 Apache Bench
ab -n 100000 -c 1000 -k "http://localhost/?start=林1&end=林2"

# 结果
Requests per second:    120,220 [#/sec]
Time per request:       8.3 [ms]
TCP 压测
# 使用自带的 benchmark 工具
./build/benchmark 127.0.0.1 9090 1000 100 林1 林2

# 结果
QPS: 28,700 req/s
平均延迟: 28.9 ms
🔧 配置说明
后端配置
// 线程池大小
ThreadPool pool(4);

// Reactor 数量
Reactors reactors(4);

// MySQL 连接池
ConnectionPool(init_size=4, max_size=8);

// Redis 连接池
RedisPool(init_size=4, max_size=8);
Nginx 配置
upstream shortest_path_cluster {
    least_conn;
    server 127.0.0.1:9090;
    server 127.0.0.1:9091;
    server 127.0.0.1:9092;
}
📚 核心实现
1. Reactor 网络模型
// 主 Reactor: 接收新连接
class Acceptor : public Handler {
    void handleRead() override {
        int connfd = accept(getFd(), ...);
        // 分配到从 Reactor
        new ConnectionHandler(connfd, thread_pool, sub_reactor);
    }
};

// 从 Reactor: 处理 I/O
class ConnectionHandler : public Handler {
    void handleRead() override {
        // 读取请求
        // 提交到线程池处理
        thread_pool->submit([this]() {
            processRequest();
        });
    }
};
2. 双缓冲机制
// 零停机更新缓存
void updateCache() {
    int current = _currentVersion.load();
    int backup = 1 - current;
    
    // 加载新数据到备用缓存
    loadCacheData(_cache[backup]);
    
    // 原子切换版本
    _currentVersion.store(backup);
}
3. 非阻塞管道通信
// 工作线程通过管道通知 Reactor
void sendToReactor(int fd, const std::string& res) {
    // 消息入队
    msg_queue.push({fd, res});
    
    // 非阻塞写入管道唤醒 Reactor
    write(pipe_fd[1], &one, 1);
}
🐛 问题排查
高并发崩溃
问题: 1000+ 并发时后端崩溃
原因: 管道写端阻塞导致死锁
解决: 将管道写端设置为非阻塞模式

TCP 性能低
问题: TCP QPS 只有 2.9w，HTTP 有 12w
原因: 测试工具使用阻塞 I/O，串行发送请求
解决: 使用专业压测工具 (ab, wrk)

📖 项目文档
性能测试报告
架构设计文档
API 文档
部署指南
🤝 贡献
欢迎提交 Issue 和 Pull Request！

📄 许可证
本项目采用 MIT 许可证 - 详见 LICENSE 文件

👨‍💻 作者
GitHub: @your-username
Email: your-email@example.com
🙏 致谢
感谢以下开源项目：

hiredis - Redis C 客户端
MySQL Connector/C - MySQL C API
Nginx - 高性能 Web 服务器
📝 更新日志
v1.0.0 (2024-03-03)
✨ 实现 Reactor 网络模型
✨ 支持 HTTP/TCP 双协议
✨ 实现 Nginx 负载均衡
✨ 实现双缓冲机制
🐛 修复高并发崩溃问题
⚡ 性能优化，QPS 达到 12w+
⭐ 如果这个项目对你有帮助，请给个 Star！
