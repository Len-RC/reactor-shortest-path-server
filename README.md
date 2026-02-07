# Reactor 高并发最短路径查询服务

基于 **主从多Reactor模型** 的高并发TCP服务器，提供图上任意两点最短路径的实时查询服务。

## 架构设计

```
客户端 TCP连接
     │
     ▼
┌──────────────┐
│  主 Reactor   │  ← epoll监听，accept新连接
│  (Acceptor)  │
└──────┬───────┘
       │ 按fd分配
       ▼
┌────────────────────────────────────┐
│  从 Reactor × 4 (EventLoop)       │
│  各自独立epoll，处理已连接fd的IO   │
└──────┬─────────────────────────────┘
       │ 提交查询任务
       ▼
┌──────────────┐     ┌───────────────────┐
│  线程池 × 4   │────▶│  内存缓存（O(1)查询）│
│  (ThreadPool) │     │  dist + prev表     │
└──────┬───────┘     └───────────────────┘
       │ 管道通知
       ▼
  从Reactor写回客户端
```

**核心组件：**

- **主从Reactor**：1个主Reactor负责accept，4个从Reactor各自运行独立epoll事件循环，处理客户端读写
- **线程池**：4个工作线程处理业务逻辑，通过管道（pipe）通知机制将结果安全回传给对应的从Reactor
- **MySQL连接池**：单例模式，支持动态扩容、超时控制、用完自动归还（基于shared_ptr自定义删除器）
- **内存缓存**：服务启动时一次性加载全部最短距离和前驱节点到内存，查询全程O(1)，不访问MySQL
- **离线预计算**：独立工具，基于Dijkstra计算全源最短路径，批量写入MySQL，通过RENAME原子切换表，在线服务零停机更新

## 技术栈

C++17 / Linux / epoll(ET) / 多线程 / TCP Socket / MySQL C API / CMake

## 项目结构

```
├── CMakeLists.txt          # 构建配置
├── src/
│   ├── server.cpp          # 在线查询服务器（主从Reactor + 线程池 + 连接池 + 内存缓存）
│   ├── offline_build.cpp   # 离线预计算工具（Dijkstra + 批量写入 + 原子表切换）
│   └── benchmark.cpp       # 自定义TCP压测工具
└── README.md
```

## 编译与运行

```bash
# 依赖：cmake 3.16+、MySQL客户端开发库
# CentOS: yum install mysql-community-devel
# Ubuntu: apt install libmysqlclient-dev

# 编译
mkdir -p build && cd build
cmake .. && make

# 设置数据库环境变量
export MYSQL_HOST=127.0.0.1
export MYSQL_PORT=3306
export MYSQL_USER=root
export MYSQL_PASS=yourpassword
export MYSQL_DB=reactor_db

# 1. 先运行离线预计算（需要MySQL中已有place和edge表）
./offline_build $MYSQL_HOST $MYSQL_PORT $MYSQL_USER $MYSQL_PASS $MYSQL_DB

# 2. 启动在线查询服务器（监听9090端口）
./server

# 3. 压测
./benchmark 127.0.0.1 9090 1000 200 林1 林2
```

## 数据库表结构

```sql
-- 地点表
CREATE TABLE place (
  id INT PRIMARY KEY AUTO_INCREMENT,
  name VARCHAR(50) NOT NULL
);

-- 边表（无向图）
CREATE TABLE edge (
  u_id INT NOT NULL,
  v_id INT NOT NULL,
  w INT NOT NULL
);

-- 以下两张表由 offline_build 自动生成
-- shortest_dist (start_id, end_id, dist)
-- shortest_prev (start_id, end_id, prev_id)
```

## 压测结果

测试环境：4核 CPU / 1.7G 内存 / CentOS Linux

| 并发连接 | 总请求数 | QPS | 平均延迟 | 失败率 |
|---------|---------|-----|---------|-------|
| 50 | 20,000 | 34,735 | 1.4ms | 0% |
| 100 | 40,000 | 36,078 | 2.7ms | 0% |
| 500 | 200,000 | 38,531 | 11.8ms | 0% |
| 1000 | 200,000 | 38,325 | 16.4ms | 0% |

优化前（每次查询访问MySQL）QPS约500，引入内存缓存后提升至3.8W，提升约76倍。

## 关键设计决策

- **epoll边缘触发（ET）**：相比水平触发减少epoll_wait系统调用次数，配合非阻塞IO和while循环读写
- **管道通知机制**：工作线程将查询结果放入队列，写1字节唤醒对应从Reactor，避免跨线程直接操作epoll
- **粘包处理**：基于`\n`分隔符的应用层协议，在接收缓冲区中循环切割完整消息
- **原子表切换**：离线预计算写入临时表，通过MySQL `RENAME TABLE` 一条语句原子替换，在线服务无感知
