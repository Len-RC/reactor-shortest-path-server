# Reactor 最短路径查询服务

一个基于 C++17 实现的高并发最短路径查询服务。项目采用主从 Reactor 网络模型处理连接，使用线程池执行查询任务，使用 MySQL 存储地点和道路边数据，使用 Redis 缓存离线预计算后的最短距离和路径前驱，并通过 Nginx 对多个后端实例做反向代理和负载均衡。

## 功能介绍

- 最短路径查询：输入起点和终点，返回最短距离以及完整路径。
- HTTP 查询接口：支持浏览器、Nginx、Apache Bench 等 HTTP 客户端访问。
- TCP 查询接口：支持自定义 TCP 客户端按行发送 `起点 终点` 查询。
- 静态页面访问：访问 `/` 返回前端查询页面。
- 多实例部署：同一台机器可启动多个后端端口，由 Nginx 统一转发。
- 离线构建缓存：从 MySQL 读取图数据，预计算全源最短路径并写入 Redis。
- 热更新缓存：离线构建完成后通过 Redis 发布订阅通知在线服务刷新版本。
- 连接池复用：MySQL 和 Redis 均使用连接池，降低频繁建连开销。
- 高并发处理：网络 I/O、业务查询、缓存访问分层处理，避免阻塞事件循环。

## 技术栈

| 模块 | 技术 |
| --- | --- |
| 开发语言 | C++17 |
| 网络模型 | epoll + 主从 Reactor |
| 并发模型 | 线程池 + 任务队列 |
| 数据库 | MySQL / MariaDB |
| 缓存 | Redis + hiredis |
| 负载均衡 | Nginx upstream |
| 构建工具 | CMake |
| 压测工具 | Apache Bench、自带 TCP benchmark |

## 实现方式

### 网络层

服务端使用主从 Reactor 模型：

1. 主 Reactor 监听服务端口，只负责接收新连接。
2. `Acceptor` 将新连接分发给从 Reactor。
3. 从 Reactor 基于 epoll 监听连接读写事件。
4. `ConnectionHandler` 负责读取请求、解析 HTTP/TCP 协议并组织响应。
5. 业务查询任务提交到线程池执行，避免慢查询阻塞 epoll 事件循环。
6. 查询结果通过 `EventLoop::sendToReactor()` 回到所属 Reactor 写回客户端。

### 查询层

一次最短路径查询流程：

1. 客户端请求 `/api/query?start=林1&end=林2`，或通过 TCP 发送 `林1 林2`。
2. `ConnectionHandler` 解析起点和终点。
3. 线程池调用 `ConnectionPool::handleClientRequest()`。
4. MySQL 连接池提供数据库连接，并加载地点名和地点 id 的映射。
5. Redis 根据当前版本读取 `dist` 和 `prev` 缓存。
6. 服务端从终点沿前驱节点回溯路径。
7. 返回 `最短距离=xx，路径=A->B->C`。

### 缓存层

项目把在线查询和离线构建拆开：

- `offline_build` 从 MySQL 的 `edge` 表读取图数据。
- 离线阶段计算所有起点到所有终点的最短距离和路径前驱。
- 结果写入 MySQL，并同步写入 Redis。
- Redis key 使用版本前缀，例如 `v0:dist:1:2`、`v1:prev:1:2`。
- `current_version` 记录当前生效版本。
- 离线构建完成后切换版本，并发布 `cache_update` 消息。
- 在线服务中的 `CacheUpdateListener` 收到消息后刷新地点映射和 Redis 版本号。

这种方式把复杂的路径计算从在线请求中移除，在线查询主要变成 Redis 读缓存和路径回溯。

### 负载均衡

Nginx 将 80 端口请求转发到三个后端实例：

```text
127.0.0.1:9090
127.0.0.1:9091
127.0.0.1:9092
```

Nginx upstream 使用轮询策略，并开启到后端的 keepalive 连接复用，减少 TCP 握手成本。

## 压测量化数据

测试对象：`/api/query?start=林1&end=林2`

测试方式：Nginx 80 端口入口，请求转发到 3 个后端实例，后端使用 Redis 缓存命中最短路径结果。

| 场景 | 压测命令 | 并发 | 总请求数 | 后端实例 | Keep-Alive | QPS |
| --- | --- | ---: | ---: | ---: | --- | ---: |
| HTTP 长连接 | `ab -n 100000 -c 1000 -k` | 1000 | 100000 | 3 | 是 | 约 22000 req/s |

结论：

- 开启 Keep-Alive 后，客户端到 Nginx 的连接可以复用，减少频繁 TCP 握手和连接释放成本。
- 在缓存命中的查询场景下，系统瓶颈主要从路径计算转移到网络 I/O、Redis 访问和连接调度。
- 当前项目在三实例长连接场景下，压测 QPS 约为 2.2w。

可复现的 HTTP 压测命令：

```bash
ab -n 100000 -c 1000 -k "http://127.0.0.1/api/query?start=林1&end=林2"
```

自带 TCP 压测工具命令：

```bash
./build/benchmark 127.0.0.1 9090 100 50 林1 林2
```

参数含义：

- `100`：并发 TCP 连接数。
- `50`：每个连接连续发送 50 次查询。
- 总请求数为 `100 * 50 = 5000`。

## 项目结构

```text
project/
├── CMakeLists.txt
├── nginx.conf
├── config/
│   └── mysql.conf.example
├── static/
│   └── index.html
└── src/
    ├── server.cpp
    ├── benchmark.cpp
    ├── offline_build.cpp
    ├── cache/
    │   ├── RedisPool.cpp
    │   ├── RedisPool.h
    │   ├── CacheUpdateListener.cpp
    │   └── CacheUpdateListener.h
    ├── db/
    │   ├── ConnectionPool.cpp
    │   └── ConnectionPool.h
    ├── net/
    │   ├── Acceptor.cpp
    │   ├── EventLoop.cpp
    │   ├── ReactorPool.cpp
    │   ├── ConnectionHandler.cpp
    │   └── *.h
    └── thread/
        ├── ThreadPool.cpp
        └── ThreadPool.h
```

## 依赖环境

- Linux
- CMake 3.16+
- g++，支持 C++17
- MySQL 或 MariaDB
- Redis
- hiredis
- Nginx
- Apache Bench，可选，用于 HTTP 压测

CentOS/RHEL 可参考：

```bash
sudo yum install -y gcc gcc-c++ cmake make nginx redis hiredis-devel mysql-devel httpd-tools
```

如果使用 MariaDB 开发库：

```bash
sudo yum install -y mariadb-devel
```

## MySQL 配置

项目启动时会优先读取 `config/mysql.conf`。该文件不建议提交到仓库，因为里面可能包含数据库密码。

先复制示例配置：

```bash
cd /home/amy/project
cp config/mysql.conf.example config/mysql.conf
```

然后修改 `config/mysql.conf`：

```ini
MYSQL_HOST=127.0.0.1
MYSQL_PORT=3306
MYSQL_USER=root
MYSQL_PASS=root
MYSQL_DB=reactor_db
```

如果不使用配置文件，也可以通过环境变量传入：

```bash
export MYSQL_HOST=127.0.0.1
export MYSQL_PORT=3306
export MYSQL_USER=root
export MYSQL_PASS=root
export MYSQL_DB=reactor_db
```

## 编译

在项目根目录执行：

```bash
cd /home/amy/project
cmake -S . -B build
cmake --build build -j4
```

编译完成后会生成：

- `build/server`：后端查询服务
- `build/offline_build`：离线最短路径构建工具
- `build/benchmark`：简单 TCP 压测工具

## 启动服务

建议从项目根目录启动后端服务，因为程序会读取相对路径下的 `config/mysql.conf` 和 `static/index.html`。

启动 Redis 和 Nginx：

```bash
sudo systemctl start redis
sudo systemctl start nginx
```

启动三个后端实例：

```bash
cd /home/amy/project
./build/server 9090
./build/server 9091
./build/server 9092
```

实际部署时可以分别放到三个终端里启动，或者放到后台运行。

## Nginx 配置

项目提供了 `nginx.conf`，用于把 80 端口请求转发到三个后端实例：

- `127.0.0.1:9090`
- `127.0.0.1:9091`
- `127.0.0.1:9092`

复制配置：

```bash
cd /home/amy/project
sudo cp nginx.conf /etc/nginx/conf.d/shortest_path.conf
sudo nginx -t
sudo systemctl reload nginx
```

如果出现 `duplicate upstream "shortest_path_cluster"`，说明 Nginx 已经加载了另一个同名配置文件，需要删除或备份重复配置。

## 访问方式

访问主页：

```text
http://127.0.0.1/
```

或者使用服务器 IP：

```text
http://服务器IP/
```

查询接口：

```text
http://127.0.0.1/api/query?start=林1&end=林2
```

返回示例：

```text
最短距离=10，路径=林1->林3->林2
```

## 离线构建最短路径数据

`offline_build` 会从 MySQL 的 `edge` 表读取图数据，计算所有起点到所有终点的最短路径，并写入 MySQL 和 Redis。

用法：

```bash
./build/offline_build <MySQL_IP> <MySQL端口> <用户> <密码> <数据库>
```

示例：

```bash
cd /home/amy/project
./build/offline_build 127.0.0.1 3306 root root reactor_db
```

Redis 中使用双版本机制：

- `current_version` 记录当前生效版本。
- 路径缓存使用 `v0:*` 或 `v1:*` 前缀。
- 离线构建完成后切换版本，并发布 `cache_update` 通知。
- 运行中的服务收到通知后刷新地点缓存。

## 运行状态观察

查看三个后端实例：

```bash
ps aux | grep './build/server' | grep -v grep
```

实时查看 CPU 和内存：

```bash
top
```

查看三个后端端口的连接数：

```bash
watch -n 1 'for p in 9090 9091 9092; do echo -n "$p "; ss -tn state established | grep ":$p" | wc -l; done'
```

查看 Redis 当前操作速率：

```bash
watch -n 1 "redis-cli INFO stats | grep instantaneous_ops_per_sec"
```

## 核心流程

一次 HTTP 查询的大致流程如下：

1. 浏览器或压测工具请求 Nginx 的 80 端口。
2. Nginx 将请求转发到 `9090`、`9091`、`9092` 中的某个后端实例。
3. 后端主 Reactor 通过 `Acceptor` 接收新连接。
4. 连接被分配给某个从 Reactor。
5. `ConnectionHandler` 读取 HTTP 请求并解析 `start` 和 `end` 参数。
6. 查询任务提交到线程池。
7. 线程池调用 `ConnectionPool::handleClientRequest()`。
8. MySQL 缓存提供地点名和地点 id 的映射。
9. Redis 查询最短距离和前驱节点。
10. 后端拼接结果，通过 Reactor 写回客户端。

## 注意事项

- 启动后端前，请确认 Redis 和 MySQL 可连接。
- 从项目根目录运行服务，避免找不到 `config/mysql.conf` 或 `static/index.html`。
- `config/mysql.conf` 不应该提交到公开仓库。
- Nginx 的 upstream keepalive 会复用到后端的连接，所以不同实例的连接数不一定完全平均。
- 连接数不等于请求数，压测时应重点观察 QPS、延迟、失败请求数、CPU、内存和 Redis/MySQL 压力。
