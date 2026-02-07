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
    std::unordered_map<std::string,int> name_to_id;  //映射表， 通过name转换成相应id
std::unordered_map<int,std::string> id_to_name;    //同理，id变name
std::mutex _cacheMtx;
bool _cacheLoaded = false;    //记录是否已经加载过id表

// 内存缓存：最短距离和前驱节点（启动时一次性加载，查询时不再访问MySQL）
// key: (start_id << 32 | end_id)  用64位整数做联合key，避免嵌套map
std::unordered_map<long long, int> _distCache;     // (start,end) -> dist, 不存在代表不可达
std::unordered_map<long long, int> _prevCache;     // (start,end) -> prev_id, 不存在代表无前驱
bool _distCacheLoaded = false;

static long long makeKey(int start_id, int end_id) {
    return ((long long)start_id << 32) | (unsigned int)end_id;
}

void loadPlaceCache(MYSQL* conn) {
    std::lock_guard<std::mutex> lock(_cacheMtx);
    if (_cacheLoaded) 
        return;

    if (mysql_query(conn, "SELECT id, name FROM place;") != 0) {
        std::cerr << "加载id表失败\n";
        return;
    }
    MYSQL_RES* res = mysql_store_result(conn);
    if (!res) 
       return;

    MYSQL_ROW row;

    //获取id表中数据并存储
    while ((row = mysql_fetch_row(res))) {
        int id = std::atoi(row[0]);
        std::string name = row[1] ? row[1] : "";
        name_to_id[name] = id;
        id_to_name[id] = name;
    }

    mysql_free_result(res);
    _cacheLoaded = true;
}

// 加载最短距离和前驱表到内存
void loadDistCache(MYSQL* conn) {
    std::lock_guard<std::mutex> lock(_cacheMtx);
    if (_distCacheLoaded)
        return;

    // 加载 shortest_dist
    if (mysql_query(conn, "SELECT start_id, end_id, dist FROM shortest_dist;") != 0) {
        std::cerr << "加载shortest_dist缓存失败\n";
        return;
    }
    MYSQL_RES* res = mysql_store_result(conn);
    if (res) {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res))) {
            if (!row[0] || !row[1]) continue;
            int s = std::atoi(row[0]);
            int e = std::atoi(row[1]);
            if (row[2])
                _distCache[makeKey(s, e)] = std::atoi(row[2]);
        }
        mysql_free_result(res);
    }

    // 加载 shortest_prev
    if (mysql_query(conn, "SELECT start_id, end_id, prev_id FROM shortest_prev;") != 0) {
        std::cerr << "加载shortest_prev缓存失败\n";
        _distCacheLoaded = true;
        return;
    }
    res = mysql_store_result(conn);
    if (res) {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res))) {
            if (!row[0] || !row[1]) continue;
            int s = std::atoi(row[0]);
            int e = std::atoi(row[1]);
            if (row[2])
                _prevCache[makeKey(s, e)] = std::atoi(row[2]);
        }
        mysql_free_result(res);
    }

    std::cout << "内存缓存加载完成: dist=" << _distCache.size()
              << " prev=" << _prevCache.size() << "\n";
    _distCacheLoaded = true;
}

	ConnectionPool() :_ip(getenv("MYSQL_HOST") ? getenv("MYSQL_HOST") : "127.0.0.1"),
		_username(getenv("MYSQL_USER") ? getenv("MYSQL_USER") : "root"),
		_password(getenv("MYSQL_PASS") ? getenv("MYSQL_PASS") : ""),
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

    // 确保缓存加载
    loadPlaceCache(conn);
    loadDistCache(conn);

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
        //通过name找相应的id
        {
            std::lock_guard<std::mutex> lk(_cacheMtx);
            auto it1 = name_to_id.find(start_place);
            auto it2 = name_to_id.find(end_place);
            if (it1 == name_to_id.end() || it2 == name_to_id.end()) {
                final_result = "地点不存在，重新输入";
                return final_result + "\n";
            }
            start_id = it1->second;
            end_id = it2->second;
        }

        // 从内存缓存查最短距离
        {
            auto dist_it = _distCache.find(makeKey(start_id, end_id));
            if (dist_it == _distCache.end()) {
                final_result = "无最短距离";
                return final_result + "\n";
            }

            std::string dist_str = std::to_string(dist_it->second);

            // 从内存缓存回溯路径
            std::vector<int> path_ids;  //存储路径回溯的id
            int cur = end_id;
            path_ids.push_back(cur);

            while(cur!=start_id)
            {
                auto it = _prevCache.find(makeKey(start_id, cur));
                if(it != _prevCache.end())
                {
                    path_ids.push_back(it->second);
                    cur = it->second;
                }else{
                    final_result = "路径回溯失败";
                    return final_result + "\n";
                }
            }

            std::reverse(path_ids.begin(), path_ids.end());  //反转路径，将倒序的路径归正

            std::string path_str;
            {
                std::lock_guard<std::mutex> lk(_cacheMtx);
                path_str+=id_to_name[path_ids[0]];
                for(size_t i=1;i<path_ids.size();++i)
                {
                    path_str+="->";
                    path_str+=id_to_name[path_ids[i]];
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
        
        // 设置管道读端为非阻塞，并给epoll监管
        fcntl(pipe_fds_[0], F_SETFL, fcntl(pipe_fds_[0], F_GETFL) | O_NONBLOCK);
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

    // 只写1字节用于唤醒 reactor
    uint8_t one = 1;
    ssize_t n = write(pipe_fds_[1], &one, 1);

    // 管道满了，无法继续写入
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        std::cerr << "唤醒管道写入失败\n ";
    }
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
        : Handler(loop, connfd), pool_(pool), in_buffer_() {
        setEvents(EPOLLIN); // 初始注册读事件，接收客户端数据
        loop->updateHandler(this);
    }

void handleRead() override {
    char buf[1024];

    while (true) {
        ssize_t n = recv(getFd(), buf, sizeof(buf), 0);

        if (n > 0) {
            in_buffer_.append(buf, n);

            // 处理粘包：可能一次收到了多行，用while一直读
            while (true) {
                size_t pos = in_buffer_.find('\n');

                // 半包，还没收到完整一行
                if (pos == std::string::npos) 
                     break; 

                std::string line = in_buffer_.substr(0, pos + 1);
                in_buffer_.erase(0, pos + 1);

                // 去掉 \r\n
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
                        // 你那边解析时会去掉 '\n'，这里加不加都行，保持一致即可

                        loop_->sendToReactor(fd, result);
                    } catch (const std::exception& e) {
                        std::cerr << "客户端fd:" << fd << " 查询异常: " << e.what() << "\n";
                        loop_->sendToReactor(fd, "无\n");
                    }
                });
            }
            continue;
        }
        
        //客户端连接关闭
        if (n == 0) {    
            handleClose();
            return;
        }

        // n < 0
        if (errno == EAGAIN || errno == EWOULDBLOCK) {   // 已经读空了（非阻塞）
            break;
        }

        if (errno == EINTR) {    // 被信号打断
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
            // 无数据可发，将读写事件重新注册回读事件
            setEvents(EPOLLIN);
            getLoop()->updateHandler(this);
            return;
        }

        ssize_t n = send(getFd(), out_buffer.c_str(), out_buffer.size(), 0);

        if (n > 0) {
            std::cout << "向客户端fd:" << getFd() << " 发送结果：" << out_buffer.substr(0, n) <<"\n";
            out_buffer.erase(0, n); // 移除已发送的数据
        } 
        
        else {
            // 写异常（非阻塞的EAGAIN/EWOULDBLOCK除外）
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                 std::cerr << "向客户端发送数据失败\n";
                handleClose();
            }
        }

        // 数据发送完毕，重新注册为读事件
        if (out_buffer.empty()) {
            setEvents(EPOLLIN);
            getLoop()->updateHandler(this);
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
};


// 程序退出处理
void exit_handler() {
    std::cout << "服务器开始优雅退出，释放所有资源...\n";
}


int main() {
    atexit(exit_handler); //注册程序退出处理器，保证异常/正常退出时释放资源

     //初始化你的MySQL连接池
    try {
        ConnectionPool::getInstance();
        std::cout << "MySQL连接池初始化完成\n";
    } catch (const std::exception& e) {
        std::cerr << "MySQL连接池初始化失败: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    //初始化Reactor组件：主Reactor+4个从Reactor+线程池
    EventLoop main_loop;
    Reactors reactors(4);
    auto thread_pool = std::make_shared<ThreadPool>(4);

    //初始化Acceptor处理器
    Acceptor acceptor(&main_loop, 9090, &reactors);

    //设置新连接回调：创建客户端连接处理器
    acceptor.setNewConnectionCallback([thread_pool](int connfd,EventLoop* loop) {
        new ConnectionHandler(connfd, thread_pool, loop);
    });

    std::cout << "Reactor高并发查询服务器启动成功\n";
    // 启动主Reactor事件循环
    main_loop.loop();

    return 0;
}
