#include <sys/epoll.h>
#include <unistd.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <iostream>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <chrono>
#include <unordered_map>
#include <cstdlib>
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mysql/mysql.h>
#include <signal.h>
#include <atomic>
#include <hiredis/hiredis.h>
#include <fstream>
#include <sstream>


//提前声明
class EventLoop;

struct PendingMsg {
    int fd;
    std::string data;
};

// 线程池
class ThreadPool {
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex mtx;
    std::condition_variable cv;
    bool stop;

public:
    ThreadPool(int num = 4) : stop(false) {
        for (int i = 0; i < num; ++i) {
            workers.emplace_back([this, i]() {
                while (true) {
                    std::unique_lock<std::mutex> lock(mtx);
                    cv.wait(lock, [this]() { return stop || !tasks.empty(); });
                    if (stop && tasks.empty()) return;

                    std::function<void()> task = std::move(tasks.front());    //move移动，将队首资源所有权给task，减少拷贝
                    tasks.pop();
                    lock.unlock();

                    std::cout << "线程" << i << "正在处理客户端请求\n";
                    task();
                    std::cout << "线程" << i << "请求处理完成\n";
                }
            });
        }
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(mtx);
            stop = true;
        }
        cv.notify_all();
        for (auto& t : workers) {
            if (t.joinable()) t.join();
        }
    }

    ThreadPool(const ThreadPool&) = delete;//  禁用拷贝
    ThreadPool& operator=(const ThreadPool&) = delete;      //禁用赋值

    template<class T>
    void submit(T&& t) {
        {
            std::unique_lock<std::mutex> lock(mtx);
            if (stop) 
              throw std::runtime_error("线程池已关闭");
            tasks.emplace(std::forward<T>(t));
        }
        cv.notify_one();
    }
};

// 处理器基类，用于处理读写关闭事件，一个连接对应一个handler
class Handler {
public:
    Handler(EventLoop* loop, int fd) : loop_(loop), fd_(fd), events_(0) {}
    virtual ~Handler() { 
        if (fd_ >= 0) close(fd_); 
    }

    virtual void handleRead() = 0;
    virtual void handleWrite() = 0;
    virtual void handleClose() = 0;

    int getFd() const { return fd_; }
    EventLoop* getLoop() const { return loop_; }

    void setEvents(uint32_t events) { events_ = events; }
    uint32_t getEvents() const { return events_; }

    void setOutBuffer(const std::string& data) { out_buffer_ = data; }
    std::string& getOutBuffer() { return out_buffer_; }

private:
    EventLoop* loop_;
    int fd_;  //存储连接fd
    uint32_t events_;  //epoll需要监听的事件
    std::string out_buffer_;//输出缓冲，存储要向客户端发送的数据
};


//Redis连接池
class RedisPool {
private:
    std::string host_;
    int port_;
    std::queue<redisContext*> pool_;
    std::mutex mtx_;
    std::condition_variable cv_;
    int init_size_;
    int max_size_;
    std::atomic<int> connection_count_{0};
    bool stop_;

    redisContext* createConnection() {
        redisContext* ctx = redisConnect(host_.c_str(), port_);
        if (ctx == nullptr || ctx->err) {
            if (ctx) {
                std::cerr << "Redis连接失败: " << ctx->errstr << "\n";
                redisFree(ctx);
            } else {
                std::cerr << "Redis连接失败: 无法分配内存\n";
            }
            return nullptr;
        }
        return ctx;
    }

public:
    RedisPool(const std::string& host = "127.0.0.1", int port = 6379, int init_size = 4, int max_size = 8)
        : host_(host), port_(port), init_size_(init_size), max_size_(max_size), stop_(false) {
        for (int i = 0; i < init_size_; ++i) {
            redisContext* ctx = createConnection();
            if (ctx) {
                pool_.push(ctx);
                connection_count_++;
                std::cout << "Redis连接池：创建第" << i+1 << "个初始连接成功\n";
            }
        }
        std::cout << "Redis连接池初始化完毕\n";
    }

    ~RedisPool() {
        stop_ = true;
        cv_.notify_all();
        std::unique_lock<std::mutex> lock(mtx_);
        while (!pool_.empty()) {
            redisContext* ctx = pool_.front();
            pool_.pop();
            redisFree(ctx);
            connection_count_--;
        }
        std::cout << "Redis连接池已关闭\n";
    }

    RedisPool(const RedisPool&) = delete;
    RedisPool& operator=(const RedisPool&) = delete;

    std::shared_ptr<redisContext> getConnection() {
        std::unique_lock<std::mutex> lock(mtx_);
        if (!cv_.wait_for(lock, std::chrono::seconds(5), [this]() { return !pool_.empty() || stop_; })) {
            std::cerr << "获取Redis连接超时\n";
            return nullptr;
        }
        if (stop_) return nullptr;
        redisContext* ctx = pool_.front();
        pool_.pop();
        return std::shared_ptr<redisContext>(ctx, [this](redisContext* c) {
            std::unique_lock<std::mutex> lock(mtx_);
            pool_.push(c);
            cv_.notify_one();
        });
    }

    static RedisPool& getInstance() {
        static RedisPool instance;
        return instance;
    }
};




//MySQL连接池
class ConnectionPool
{
private:
	std::string _ip;     //mysql所在服务器地址
	unsigned int _port;   ///端口
	std::string _username;    //用户名
	std::string _password;     //密码
	std::string _dbname;     //要用到的数据库名
	int _initSize;      //连接池初始可用连接数
	int _maxSize;       //最大连接数
	int _connectionCnt;   ////当前已经创建的连接数
	int _maxTimeout;      ///最大超时时间
	bool _stop;       
	std::queue<MYSQL*> _connectionQue;
	std::mutex _queueMutex;
	std::condition_variable cv_in;  //往队列添加新连接的条件变量
	std::condition_variable cv_out;  //在队列取连接的条件变量
    
        // 双缓存结构（仅缓存地点名称映射，距离和路径数据已迁移到Redis）
    struct CacheData {
        std::unordered_map<std::string, int> name_to_id;
        std::unordered_map<int, std::string> id_to_name;
    };

    
    CacheData _cache[2];  // 双缓存：0 和 1
    std::atomic<int> _currentVersion{0};  // 当前版本号（0 或 1）
    std::atomic<int> _redis_current_version{0};
    std::mutex _updateMutex;  // 更新缓存时的互斥锁
    bool _cacheLoaded = false;

static long long makeKey(int start_id, int end_id) {
    return ((long long)start_id << 32) | (unsigned int)end_id;
}

void loadCacheData(MYSQL* conn, CacheData& cache) {
    // 只加载 place 表（地点名称映射）
    if (mysql_query(conn, "SELECT id, name FROM place;") != 0) {
        std::cerr << "加载place表失败\n";
        return;
    }
    MYSQL_RES* res = mysql_store_result(conn);
    if (res) {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res))) {
            int id = std::atoi(row[0]);
            std::string name = row[1] ? row[1] : "";
            cache.name_to_id[name] = id;
            cache.id_to_name[id] = name;
        }
        mysql_free_result(res);
    }

    std::cout << "缓存加载完成: place=" << cache.name_to_id.size() << "\n";
}


void loadPlaceCache(MYSQL* conn) {
    std::lock_guard<std::mutex> lock(_updateMutex);
    if (_cacheLoaded) 
        return;

    std::cout << "首次加载缓存到版本 0...\n";
    // 初次加载：加载到版本 0
    loadCacheData(conn, _cache[0]);
    _cacheLoaded = true;
    
    // 初始化 Redis 版本号缓存
    auto redis_conn = RedisPool::getInstance().getConnection();
    if (redis_conn) {
        redisReply* ver_reply = (redisReply*)redisCommand(redis_conn.get(), "GET current_version");
        if (ver_reply && ver_reply->type == REDIS_REPLY_STRING) {
            int redis_version = std::atoi(ver_reply->str);
            _redis_current_version.store(redis_version, std::memory_order_release);
            std::cout << "初始化 Redis 版本号: " << redis_version << "\n";
            freeReplyObject(ver_reply);
        } else {
            std::cout << "Redis 版本号未找到，使用默认值 0\n";
            if (ver_reply) freeReplyObject(ver_reply);
        }
    }
}

	ConnectionPool() :_ip(getenv("MYSQL_HOST") ? getenv("MYSQL_HOST") : "10.4.126.93"),
		_username(getenv("MYSQL_USER") ? getenv("MYSQL_USER") : "root"),
		_password(getenv("MYSQL_PASS") ? getenv("MYSQL_PASS") : "root"),
		_dbname(getenv("MYSQL_DB") ? getenv("MYSQL_DB") : "reactor_db"),
		_port(getenv("MYSQL_PORT") ? std::atoi(getenv("MYSQL_PORT")) : 3306),
		_initSize(4),
		_maxSize(8),
		_maxTimeout(5),
	    _connectionCnt(0),
	    _stop(false){
        // 初始化MySQL全局库
        if (mysql_library_init(0, nullptr, nullptr) != 0) {
            std::cerr << "MySQL全局库初始化失败\n";
            exit(EXIT_FAILURE);
        }
		
        //创建初始连接
		for (int i = 0; i < _initSize; i++)
		{
			MYSQL* p = createConnection();
            // 校验连接是否创建成功
			if (p == nullptr) { 
				std::cerr << "初始化第" << i + 1 << "个连接失败，程序退出\n";
				exit(EXIT_FAILURE);
			}

			_connectionQue.push(p);
			_connectionCnt++;
            std::cout << "MySQL连接池：创建第" << i+1 << "个初始连接成功\n";
		}

		// 启动生产线程,监控连接队列
		std::thread produce(std::bind(&ConnectionPool::produceConnection, this));  //类中非静态方法需要bind进行参数绑定
		produce.detach();
        std::cout<<"连接池初始化完毕\n"; 
	}

	ConnectionPool(const ConnectionPool&) = delete;
	ConnectionPool& operator=(const ConnectionPool&) = delete;


	~ConnectionPool()
	{
		_stop = true;

		// 唤醒所有等待的线程，避免死锁
		cv_in.notify_all();
		cv_out.notify_all();

		std::unique_lock<std::mutex> lock(_queueMutex);
		while (!_connectionQue.empty())
		{
			MYSQL* conn = _connectionQue.front();
			_connectionQue.pop();
			mysql_close(conn);
			_connectionCnt--;
		}

		mysql_library_end();    // 释放MySQL全局库资源
		std::cout << "连接池已关闭\n";
	}


	//创建新连接
	MYSQL* createConnection() 
	{
		MYSQL* conn = mysql_init(nullptr);

       //校验mysql_init是否成功
		if (conn == nullptr) {  
			std::cerr << "mysql_init失败\n";
			return nullptr;
		}

		if (!mysql_real_connect(conn, _ip.c_str(), _username.c_str(), _password.c_str(),
			_dbname.c_str(), _port, nullptr, 0)) {
			std::cerr << "mysql_real_connect失败\n" ;
			mysql_close(conn);
			return nullptr;
		}
		return conn;
	}

	//监控连接队列并生产可用连接
	void produceConnection()
	{
		for (;; )
		{
			std::unique_lock<std::mutex> lock(_queueMutex);
				cv_in.wait(lock, 
					[this](){
				return _stop || _connectionQue.empty();
			});

			if (_stop)
				break;

			while (_connectionCnt < _maxSize && _connectionQue.empty())
			{
				MYSQL* p = createConnection();
				if (p == nullptr)
				{
					std::cerr << "创建连接失败，1秒后重试\n";
					std::this_thread::sleep_for(std::chrono::seconds(1));
					break;
				}
				_connectionQue.push(p);
				_connectionCnt++;
                std::cout << "MySQL连接池：动态创建连接，当前总连接数：" << _connectionCnt << "\n";
			}
			cv_out.notify_all();
		}
	}

public:

	//外部获取实例，单例模式
	static ConnectionPool& getInstance()
	{
		static ConnectionPool instance; // 局部静态变量，仅初始化一次，线程安全
		return instance;
	}


	//外部从队列中获取可用连接
	std::shared_ptr<MYSQL> getConnection()
	{
		std::unique_lock<std::mutex> lock(_queueMutex);
		if (!cv_out.wait_for(lock, std::chrono::seconds(_maxTimeout),
			[this](){return !_connectionQue.empty()||_stop;}))
			{
				std::cout << "获取MySQL连接超时（超时时长：" << _maxTimeout << "秒）\n";
				return nullptr;
			}
		if (_stop)
			return nullptr;

		MYSQL* p = _connectionQue.front();

		// 自定义shared_ptr删除，用完后的归还到队列
		std::shared_ptr<MYSQL> sp(p, 
			[this](MYSQL* p){
			std::unique_lock<std::mutex> lock(_queueMutex);
			_connectionQue.push(p);
            cv_in.notify_one();    // 归还后通知生产线程,提高效率
		});
		_connectionQue.pop();

		if (_connectionQue.empty())
			cv_in.notify_one(); 

		return sp;
	}

// 更新缓存（双缓冲方案，零停机更新）
void updateCache() {
    std::lock_guard<std::mutex> lock(_updateMutex);
    
    std::cout << "开始更新缓存...\n";
    
    // 获取 MySQL 连接
    std::shared_ptr<MYSQL> conn_ptr = getConnection();
    if (!conn_ptr) {
        std::cerr << "更新缓存失败：无法获取数据库连接\n";
        return;
    }
    
   //  获取 Redis 连接并更新版本号缓存
    auto redis_conn = RedisPool::getInstance().getConnection();
    if (redis_conn) {
        redisReply* ver_reply = (redisReply*)redisCommand(redis_conn.get(), "GET current_version");
        if (ver_reply && ver_reply->type == REDIS_REPLY_STRING) {
            int new_redis_version = std::atoi(ver_reply->str);
            _redis_current_version.store(new_redis_version, std::memory_order_release);
            std::cout << "Redis 版本号已更新为: " << new_redis_version << "\n";
            freeReplyObject(ver_reply);
        } else {
            if (ver_reply) freeReplyObject(ver_reply);
        }
    }


    // 确定备用版本
    int current = _currentVersion.load(std::memory_order_acquire);
    int backup = 1 - current;  // 0->1 或 1->0
    
    // 清空备用缓存
    _cache[backup].name_to_id.clear();
    _cache[backup].id_to_name.clear();

    
    // 从 MySQL 加载数据到备用缓存
    loadCacheData(conn_ptr.get(), _cache[backup]);
    
    // 5. 原子切换版本号
    _currentVersion.store(backup, std::memory_order_release);
    
    std::cout << "缓存更新完成，版本切换: " << current << " -> " << backup << "\n";
}

// 处理客户端请求（双地点名查询最短距离）
std::string handleClientRequest(const std::string& client_input) {
    std::shared_ptr<MYSQL> conn_ptr = getConnection();
    MYSQL* conn = conn_ptr.get();

    std::string final_result = "无最短距离";
    std::string start_place = "未知";
    std::string end_place = "未知";

    if (!conn) {
        return final_result + "\n";
    }

    // 确保place表缓存加载（name到id的映射仍然从MySQL加载）
    loadPlaceCache(conn);

    // 读取当前版本号
    int version = _currentVersion.load(std::memory_order_acquire);
    const CacheData& cache = _cache[version];

    // 获取Redis连接
    auto redis_conn = RedisPool::getInstance().getConnection();
    if (!redis_conn) {
        std::cerr << "获取Redis连接失败，降级到MySQL\n";
        // 这里可以降级到MySQL，暂时返回错误
        return "服务暂时不可用\n";
    }


    try {
        size_t space_pos = client_input.find(' ');  ///查找客户端发来数据的第一个空格的位置下标

        if (space_pos == std::string::npos || space_pos == 0 || space_pos == client_input.size()-1) {  //std::string::npos为没找到
            final_result = "输入格式错,格式：出发地 目的地（如：林1 林2）";
            return final_result + "\n";
        }

        start_place = client_input.substr(0, space_pos);
        end_place = client_input.substr(space_pos + 1);  //读取剩下内容，但最后会有个换行符
        end_place.erase(std::remove_if(end_place.begin(), end_place.end(),  //去掉换行符
            [](char c){ return c=='\n' || c=='\r'; }), end_place.end());

        int start_id = -1, end_id = -1;
        //通过name找相应的id（无锁读取）
        {
            auto it1 = cache.name_to_id.find(start_place);
            auto it2 = cache.name_to_id.find(end_place);
            if (it1 == cache.name_to_id.end() || it2 == cache.name_to_id.end()) {
                final_result = "地点不存在，重新输入";
                return final_result + "\n";
            }
            start_id = it1->second;
            end_id = it2->second;
        }

        // 从Redis查询最短距离（使用缓存的版本前缀）
        {
            // 使用缓存的 Redis 版本号（不需要每次查询 Redis）
            int redis_version = _redis_current_version.load(std::memory_order_acquire);
            std::string version_prefix = "v" + std::to_string(redis_version);
            
            // 构造带版本前缀的键
            std::string dist_key = version_prefix + ":dist:" + std::to_string(start_id) + ":" + std::to_string(end_id);
            redisReply* dist_reply = (redisReply*)redisCommand(redis_conn.get(), "GET %s", dist_key.c_str());
            
            if (!dist_reply || dist_reply->type != REDIS_REPLY_STRING) {
                if (dist_reply) freeReplyObject(dist_reply);
                final_result = "无最短距离";
                return final_result + "\n";
            }
            
            int dist = std::atoi(dist_reply->str);
            std::string dist_str = std::to_string(dist);
            freeReplyObject(dist_reply);


            // 从Redis回溯路径（使用相同的版本前缀）
            std::vector<int> path_ids;
            int cur = end_id;
            path_ids.push_back(cur);

            while(cur != start_id)
            {
                std::string prev_key = version_prefix + ":prev:" + std::to_string(start_id) + ":" + std::to_string(cur);
                redisReply* prev_reply = (redisReply*)redisCommand(redis_conn.get(), "GET %s", prev_key.c_str());
                
                if (!prev_reply || prev_reply->type != REDIS_REPLY_STRING) {
                    if (prev_reply) freeReplyObject(prev_reply);
                    final_result = "路径回溯失败";
                    return final_result + "\n";
                }
                
                int prev_id = std::atoi(prev_reply->str);
                freeReplyObject(prev_reply);
                
                path_ids.push_back(prev_id);
                cur = prev_id;
            }

            std::reverse(path_ids.begin(), path_ids.end());  //反转路径，将倒序的路径归正

            std::string path_str;
            // 无锁读取 id_to_name（使用 find 避免异常）
            for(size_t i=0; i<path_ids.size(); ++i)
            {
                if (i > 0) path_str += "->";
                
                auto name_it = cache.id_to_name.find(path_ids[i]);
                if (name_it != cache.id_to_name.end()) {
                    path_str += name_it->second;
                } else {
                    path_str += "未知(" + std::to_string(path_ids[i]) + ")";
                }
            }

            final_result = "最短距离=" + dist_str + "，路径=" + path_str;
            return final_result + "\n";
        }

    } catch (const std::exception& e) {
        std::cerr << "handleClientRequest 问题:" << e.what() << "\n";
        return final_result + "\n";
    }
}
};


// Redis订阅监听器（用于接收缓存更新通知）
class CacheUpdateListener {
private:
    redisContext* sub_ctx_;
    std::thread listener_thread_;
    std::atomic<bool> stop_{false};

public:
    CacheUpdateListener() : sub_ctx_(nullptr) {}

    ~CacheUpdateListener() {
        stop();
    }

    void start() {
        // 创建独立的Redis连接用于订阅
        sub_ctx_ = redisConnect("127.0.0.1", 6379);
        if (sub_ctx_ == nullptr || sub_ctx_->err) {
            std::cerr << "订阅器Redis连接失败\n";
            return;
        }

        std::cout << "启动缓存更新监听器...\n";

        // 启动监听线程
        listener_thread_ = std::thread([this]() {
            // 订阅频道
            redisReply* reply = (redisReply*)redisCommand(sub_ctx_, "SUBSCRIBE cache_update");
            if (reply) {
                std::cout << "已订阅 cache_update 频道\n";
                freeReplyObject(reply);
            }

            // 循环接收消息
            while (!stop_) {
                redisReply* msg;
                if (redisGetReply(sub_ctx_, (void**)&msg) == REDIS_OK) {
                    if (msg->type == REDIS_REPLY_ARRAY && msg->elements == 3) {
                        // msg->element[0] = "message"
                        // msg->element[1] = "cache_update"
                        // msg->element[2] = 消息内容
                        std::cout << "\n收到缓存更新通知: " << msg->element[2]->str << "\n";
                        std::cout << "开始刷新缓存...\n";
                        
                        // 触发缓存更新
                        ConnectionPool::getInstance().updateCache();
                        
                        std::cout << "缓存刷新完成\n";
                    }
                    freeReplyObject(msg);
                }
            }
        });
    }

    void stop() {
        stop_ = true;
        if (sub_ctx_) {
            redisFree(sub_ctx_);
            sub_ctx_ = nullptr;
        }
        if (listener_thread_.joinable()) {
            listener_thread_.detach();  // 让线程自然结束
        }
    }
};




// Reactor核心，处理IO事件和管道通知
class EventLoop {
public:
    EventLoop() : epfd_(epoll_create(1)), stop_(false) {
        if (epfd_ == -1) {
            std::cerr<<"epoll创建失败\n";
            exit(EXIT_FAILURE);
        }
        
        if (pipe(pipe_fds_) == -1) {    // 创建管道
             std::cerr<<"管道创建失败\n";
            exit(EXIT_FAILURE);
        }
        
        // 设置管道读端和写端都为非阻塞
        fcntl(pipe_fds_[0], F_SETFL, fcntl(pipe_fds_[0], F_GETFL) | O_NONBLOCK);
        fcntl(pipe_fds_[1], F_SETFL, fcntl(pipe_fds_[1], F_GETFL) | O_NONBLOCK);
        struct epoll_event ev = {0};
        ev.events = EPOLLIN | EPOLLET;  // 边缘触发模式，提升效率
        ev.data.fd = pipe_fds_[0];
        if (epoll_ctl(epfd_, EPOLL_CTL_ADD, pipe_fds_[0], &ev) == -1) {
             std::cerr<<"管道注册到epoll失败\n";
            exit(EXIT_FAILURE);
        }
    }

    ~EventLoop() {
        stop();
        close(epfd_);
        close(pipe_fds_[0]);
        close(pipe_fds_[1]);
    }

    EventLoop(const EventLoop&) = delete;   //禁用拷贝
    EventLoop& operator=(const EventLoop&) = delete;   //禁用赋值

    //循环监听epoll
    void loop() {
        stop_ = false;
        struct epoll_event events[1024] = {0};

        while (!stop_) {
            int nfds = epoll_wait(epfd_, events, 1024, -1);
            if (nfds == -1) {
                if (errno == EINTR) continue;
                 std::cerr<<"epoll_wait失败\n";
                break;
            }

            for (int i = 0; i < nfds; ++i) {
                // 处理管道事件
                if (events[i].data.fd == pipe_fds_[0]) {
                    handlePipeNotify();
                    continue;
                }
                
                // 处理客户端连接IO事件
                Handler* handler = static_cast<Handler*>(events[i].data.ptr);
                if (!handler) continue;
                
                //读事件
                if (events[i].events & (EPOLLIN | EPOLLPRI)) {
                    handler->handleRead();
                }

                //写事件
                if (events[i].events & EPOLLOUT) {
                    std::cout << "检测到EPOLLOUT事件，准备向客户端发送结果\n" ;
                    handler->handleWrite();
                }

                //释放连接
                if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                    handler->handleClose();
                }
            }
        }
    }

    void stop() { stop_ = true; }

    // 更新epoll中的事件
    void updateHandler(Handler* handler) {
        struct epoll_event ev = {0};
        ev.events = handler->getEvents();
        ev.data.ptr = handler;

        int op = EPOLL_CTL_MOD;
        auto it = fd_to_handler.find(handler->getFd());

        //更新fd到handler的映射表
        if (it == fd_to_handler.end()) {
            op = EPOLL_CTL_ADD;
            std::lock_guard<std::mutex> lock(mtx_);
            fd_to_handler[handler->getFd()] = handler;
        }

        if (epoll_ctl(epfd_, op, handler->getFd(), &ev) == -1) {
             std::cerr<<"epoll_ctl失败\n";
        }
    }


    // 从epoll中移除事件
    void removeHandler(Handler* handler) {
        epoll_ctl(epfd_, EPOLL_CTL_DEL, handler->getFd(), nullptr);
        std::lock_guard<std::mutex> lock(mtx_);
        fd_to_handler.erase(handler->getFd());
    }


    // 用于工作线程，通过管道向主线程发送查询结果
   void sendToReactor(int fd,const std::string& res) {
    {
        std::lock_guard<std::mutex> lk(q_mtx_);
        msg_q_.push(PendingMsg{fd, res});   //将结果放到队列中，等管道reactor唤醒后统一处理
    }

    // 只写1字节用于唤醒 reactor（非阻塞模式）
    uint8_t one = 1;
    ssize_t n = write(pipe_fds_[1], &one, 1);

    // 管道满了（EAGAIN/EWOULDBLOCK）是正常的，reactor会处理队列中的消息
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        std::cerr << "唤醒管道写入失败: " << strerror(errno) << "\n";
    }
    // 注意：即使管道满了写入失败，消息已经在队列中，reactor下次处理时会取出
}

private:
    int epfd_;
    bool stop_;
    int pipe_fds_[2];   // 管道：[0]读端，[1]写端
    std::unordered_map<int, Handler*> fd_to_handler; // fd到handler的映射
    std::mutex mtx_; 
    std::mutex q_mtx_;
    std::queue<PendingMsg> msg_q_;

    // 处理管道中的通知：将结果发送给对应客户端
    void handlePipeNotify() {
   //while读完管道信息
    while (true) {
        uint8_t buf[256];
        ssize_t n = read(pipe_fds_[0], buf, sizeof(buf));
        if (n > 0) 
           continue;

       // 写端关闭
        if (n == 0) {
            std::cerr << "管道写端被关闭\n";
            break;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) //已读完
            break;
        if (errno == EINTR) //信号被打断
            continue;

        std::cerr << "读取唤醒管道失败: " << strerror(errno) << "\n";
        break;
    }

   
    std::queue<PendingMsg> local;
    {
        std::lock_guard<std::mutex> lk(q_mtx_);
        std::swap(local, msg_q_);          //把队列里的消息全部取出来处理
    }

    //一一读取队列信息并输入到对于的输出缓冲区
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

        //fd已经被关闭了，直接丢弃消息
        if (!handler) 
            continue;

        handler->setOutBuffer(msg.data);
        handler->setEvents(EPOLLIN | EPOLLOUT);
        updateHandler(handler);
    }
  }
};




// Reactors：多Reactor模型，管理子Reactor线程
class Acceptor;
class Reactors
{
public:
    
    int selectl(int connfd)
    {
        return connfd % 4;// 将连接分配到不同的子Reactor
    }

    // 获取所有子Reactor
    std::vector<EventLoop*>& getreactors()
    {
        return reactors;
    }

    //创建指定数量的从reactor和线程
    Reactors(int num)
    {
        for(int i=0;i<num;++i)  
        {
            reactors.emplace_back(new EventLoop);//从reactor
            threads.emplace_back([i,this](){
               this->reactors[i]->loop();
            });
        }
    } 

    ~Reactors() {
        for (auto& reactor : reactors) {
            reactor->stop();
        }
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }
private:
    std::vector<EventLoop*> reactors;
    std::vector<std::thread> threads;
};


// 监听端口，接收新的客户端连接
class Acceptor : public Handler {
public:
    using NewConnectionCallback = std::function<void(int connfd,EventLoop* loopp)>;  //封装函数，用于创建客户端处理器的回调

    Acceptor(EventLoop* loop,int port,Reactors* reacc) 
        : Handler(loop, socket(AF_INET, SOCK_STREAM, 0)), 
          new_conn_cb_(nullptr),
          reac(reacc) {
       
        int flags = fcntl(getFd(), F_GETFL, 0); // 非阻塞
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
        setEvents(EPOLLIN);   //注册读事件，监听新连接
        loop->updateHandler(this);
    }

    //处理新连接
    void handleRead() override {
        while (true) {
            sockaddr_in client_addr{};
            socklen_t len = sizeof(client_addr);
            int connfd = accept(getFd(), (sockaddr*)&client_addr, &len);

            if (connfd == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break; // 非阻塞模式，无新连接则退出
                }
                 std::cerr << "客户端连接\n";
                break;
            }

            std::cout << "Accept 新客户端连接，fd: " << connfd <<"\n";
           
            fcntl(connfd, F_SETFL, fcntl(connfd, F_GETFL) | O_NONBLOCK); //设置客户端fd为非阻塞

            // 回调创建新的连接处理器，分配到从reactor
            if (new_conn_cb_) {
                new_conn_cb_(connfd,reac->getreactors()[reac->selectl(connfd)]);
            }
        }
    }

    void handleWrite() override {} // 无需处理写事件

    // 处理关闭事件
    void handleClose() override {
        getLoop()->removeHandler(this);
        delete this;
    }

    // 设置新连接回调函数
    void setNewConnectionCallback(NewConnectionCallback cb) {
        new_conn_cb_ = std::move(cb);
    }

private:
    NewConnectionCallback new_conn_cb_;
    Reactors* reac;
};


// 客户端连接处理器
class ConnectionHandler : public Handler {
public:
    ConnectionHandler(int connfd, std::shared_ptr<ThreadPool> pool,EventLoop* loop)
        : Handler(loop, connfd), pool_(pool), in_buffer_(), is_http_(false), http_parsed_(false) {
        setEvents(EPOLLIN); // 初始注册读事件，接收客户端数据
        loop->updateHandler(this);
    }

    // URL解码函数
    std::string urlDecode(const std::string& str) {
        std::string result;
        for (size_t i = 0; i < str.length(); ++i) {
            if (str[i] == '%' && i + 2 < str.length()) {
                // 将%XX转换为字符
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

    // 解析HTTP请求
    bool parseHttpRequest(std::string& start_place, std::string& end_place) {
        // 查找请求行结束位置
        size_t header_end = in_buffer_.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            return false; // 还没收到完整HTTP头
        }

        std::string header = in_buffer_.substr(0, header_end);
        
        // 检查是否是GET请求
        if (header.substr(0, 3) == "GET") {
            is_http_ = true;
            
            // 提取请求路径
            size_t path_start = header.find(' ') + 1;
            size_t path_end = header.find(' ', path_start);
            std::string path = header.substr(path_start, path_end - path_start);
            
            // 处理根路径 - 返回HTML页面
            if (path == "/" || path.find("/?") == 0 || path.find("/api/query") == 0) {
                if (path == "/") {
                    return true; // 返回主页
                }
                
                // 解析查询参数 /?start=林1&end=林2 或 /api/query?start=林1&end=林2
                size_t query_start = path.find('?');
                if (query_start != std::string::npos) {
                    std::string query = path.substr(query_start + 1);
                    
                    // 解析start参数
                    size_t start_pos = query.find("start=");
                    if (start_pos != std::string::npos) {
                        start_pos += 6; // "start="的长度
                        size_t start_end = query.find('&', start_pos);
                        start_place = query.substr(start_pos, start_end - start_pos);
                        start_place = urlDecode(start_place);
                    }
                    
                    // 解析end参数
                    size_t end_pos = query.find("end=");
                    if (end_pos != std::string::npos) {
                        end_pos += 4; // "end="的长度
                        size_t end_end = query.find('&', end_pos);
                        end_place = query.substr(end_pos, end_end - end_pos);
                        end_place = urlDecode(end_place);
                    }
                }
                return true;
            }
        }
        
        return false;
    }

    // 读取HTML文件
    std::string readHtmlFile(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            return "<html><body><h1>404 Not Found</h1></body></html>";
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    // 生成HTML主页
    std::string generateHtmlPage() {
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

void handleRead() override {
    char buf[4096];

    while (true) {
        ssize_t n = recv(getFd(), buf, sizeof(buf), 0);

        if (n > 0) {
            in_buffer_.append(buf, n);

            // 如果还没判断协议类型，先尝试解析
            if (!http_parsed_) {
                // 检查是否是HTTP请求（需要至少4个字节）
                if (in_buffer_.size() >= 4 && 
                    (in_buffer_.substr(0, 4) == "GET " || 
                     (in_buffer_.size() >= 5 && in_buffer_.substr(0, 5) == "POST "))) {
                    
                    std::string start_place, end_place;
                    if (parseHttpRequest(start_place, end_place)) {
                        http_parsed_ = true;
                        in_buffer_.clear();
                        
                        int fd = getFd();
                        EventLoop* loop_ = getLoop();
                        
                        // 如果没有查询参数，返回HTML页面
                        if (start_place.empty() || end_place.empty()) {
                            std::string html_response = generateHtmlPage();
                            loop_->sendToReactor(fd, html_response);
                        } else {
                            // 有查询参数，处理查询请求
                            auto pool = pool_;
                            pool_->submit([loop_, start_place, end_place, fd]() {
                                try {
                                    ConnectionPool& mysql_pool = ConnectionPool::getInstance();
                                    std::string query = start_place + " " + end_place;
                                    std::string result = mysql_pool.handleClientRequest(query);
                                    
                                    // 构造HTTP响应
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
                        return; // HTTP请求处理完毕
                    }
                } else if (in_buffer_.size() >= 1 && in_buffer_[0] != 'G' && in_buffer_[0] != 'P') {
                    // 第一个字符不是 'G' 或 'P'，肯定不是 HTTP 请求
                    http_parsed_ = true;
                    is_http_ = false;
                } else if (in_buffer_.find('\n') != std::string::npos) {
                    // 已经收到换行符，但不是HTTP请求，按TCP协议处理
                    http_parsed_ = true;
                    is_http_ = false;
                }
                // 否则继续等待更多数据来判断协议类型
            }

            // TCP协议处理（原有逻辑）
            if (!is_http_ && http_parsed_) {
                while (true) {
                    size_t pos = in_buffer_.find('\n');

                    if (pos == std::string::npos) 
                         break; 

                    std::string line = in_buffer_.substr(0, pos + 1);
                    in_buffer_.erase(0, pos + 1);

                    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) 
                        line.pop_back();

                    if (line.empty()) 
                       continue;

                    int fd = getFd();
                    EventLoop* loop_ = getLoop();
                    auto pool = pool_; 

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
        
        //客户端连接关闭
        if (n == 0) {    
            handleClose();
            return;
        }

        // n < 0
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


    // 处理客户端写事件
    void handleWrite() override {
        std::string& out_buffer = getOutBuffer();
        if (out_buffer.empty()) {
            // 无数据可发，对于HTTP请求直接关闭连接
            if (is_http_) {
                handleClose();
                return;
            }
            // TCP连接重新注册为读事件
            setEvents(EPOLLIN);
            getLoop()->updateHandler(this);
            return;
        }

        ssize_t n = send(getFd(), out_buffer.c_str(), out_buffer.size(), 0);

        if (n > 0) {
            std::cout << "向客户端fd:" << getFd() << " 发送结果（" << (is_http_ ? "HTTP" : "TCP") << "）\n";
            out_buffer.erase(0, n); // 移除已发送的数据
        } 
        
        else {
            // 写异常（非阻塞的EAGAIN/EWOULDBLOCK除外）
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                 std::cerr << "向客户端发送数据失败\n";
                handleClose();
            }
        }

        // 数据发送完毕
        if (out_buffer.empty()) {
            if (is_http_) {
                // HTTP请求处理完毕，关闭连接
                handleClose();
            } else {
                // TCP连接重新注册为读事件
                setEvents(EPOLLIN);
                getLoop()->updateHandler(this);
            }
        }
    }

    // 处理客户端关闭事件
    void handleClose() override {
        std::cout << "客户端fd:" << getFd() << " 断开连接\n";
        getLoop()->removeHandler(this);
        delete this;
    }

private:
    std::shared_ptr<ThreadPool> pool_; // 线程池指针，用于提交查询任务
    std::string in_buffer_;            // 客户端输入缓冲区
    bool is_http_;                     // 是否是HTTP协议
    bool http_parsed_;                 // 是否已解析协议类型
};


// 程序退出处理
void exit_handler() {
    std::cout << "服务器开始优雅退出，释放所有资源...\n";
}



int main(int argc, char* argv[]) {
    // 支持命令行参数指定端口，默认9090
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

    atexit(exit_handler); //注册程序退出处理器，保证异常/正常退出时释放资源
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

     //初始化MySQL连接池
    try {
        ConnectionPool::getInstance();
        std::cout << "MySQL连接池初始化完成\n";
    } catch (const std::exception& e) {
        std::cerr << "MySQL连接池初始化失败: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    // 启动缓存更新监听器
    CacheUpdateListener cache_listener;
    cache_listener.start();


    //初始化Reactor组件：主Reactor+4个从Reactor+线程池
    EventLoop main_loop;
    Reactors reactors(4);
    auto thread_pool = std::make_shared<ThreadPool>(4);

    //初始化Acceptor处理器
    Acceptor acceptor(&main_loop, port, &reactors);

    //设置新连接回调：创建客户端连接处理器
    acceptor.setNewConnectionCallback([thread_pool](int connfd,EventLoop* loop) {
        new ConnectionHandler(connfd, thread_pool, loop);
    });

    std::cout << "Reactor高并发查询服务器启动成功\n";
    std::cout << "监听端口: " << port << "\n";
    std::cout << "服务器 PID: " << getpid() << "\n";
    // 启动主Reactor事件循环
    main_loop.loop();

    return 0;
}
