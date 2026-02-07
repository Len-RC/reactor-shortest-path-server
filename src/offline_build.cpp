#include <mysql/mysql.h>
#include <iostream>
#include <vector>
#include <queue>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <cstdlib>

using namespace std;

//存储边信息
struct edge {
    int id;
    int weight;
    edge* next;
};

//邻接表
class watch {
public:
    watch(int size) {
        node_num = size;
        node.resize(size + 1, nullptr);

        for (int i = 0; i <= size; ++i) {
            edge* p = new edge;
            p->id = 0;
            p->weight = 0;
            p->next = nullptr;
            node[i] = p;
        }
    }

    ~watch() {
        for (int i = 0; i <= node_num; ++i) {
            edge* cur = node[i];

            while (cur != nullptr) {
                edge* temp = cur;
                cur = cur->next;
                delete temp;
            }
        }
        node.clear();
    }

    void input(int l, int r, int num) {
        //处理数据库错误数据
        if (l < 0 || l > node_num || r < 0 || r > node_num) {
            cout << "节点编号越界：l=" << l << " r=" << r << " node_num=" << node_num << "\n";
            return;
        }

        edge* p = new edge;
        p->id = r;
        p->weight = num;
        p->next = node[l]->next;
        node[l]->next = p;
    }

    pair<vector<int>, vector<int>> dijkstra(int start) {  //第一个vector存储最短距离，第二个vector存储前驱节点
        const int INF = 99999999;
        vector<int> dist(node_num + 1, INF);
        vector<int> pre(node_num + 1, -1);
        dist[start] = 0;

        priority_queue<pair<int,int>, vector<pair<int,int>>, greater<>> pq;
        pq.emplace(0, start);

        while (!pq.empty()) {
            auto [cur_dist, u] = pq.top();
            pq.pop();
            if (cur_dist > dist[u]) continue;

            edge* p = node[u]->next;
            while (p != nullptr) {
                int v = p->id;
                int w = p->weight;
                if (dist[v] > dist[u] + w) {
                    dist[v] = dist[u] + w;
                    pre[v] = u;
                    pq.emplace(dist[v], v);
                }
                p = p->next;
            }
        }
        return {dist, pre};
    }

private:
    int node_num;  //节点数量
    vector<edge*> node;   //存储各个节点的边信息
};

const int INF = 99999999;


//负责专门执行sql语句
static void execSQL(MYSQL* conn, const string& sql) {
    if (mysql_query(conn, sql.c_str()) != 0) {
        cerr << "执行sql语句失败\n";
    }
}


//批量插入最短距离
static void batchInsertDist(MYSQL* conn, int start_id,
                            const vector<int>& idx_to_id,
                            const vector<int>& dist,
                            int batch_from, int batch_to) {
    ostringstream oss;  //动态扩展
    oss << "INSERT INTO shortest_dist_new(start_id,end_id,dist) VALUES ";
    bool first = true;

    for (int i = batch_from; i < batch_to; ++i) {
      //每次检查first来判断是否要添加","
        if (!first)   
          oss << ",";
        first = false;

        int end_id = idx_to_id[i];
        oss << "(" << start_id << "," << end_id << ",";
        if (dist[i] == INF) 
           oss << "NULL";
        else 
           oss << dist[i];
        oss << ")";
    }
    oss << ";";
    execSQL(conn, oss.str());
}


//批量插入前驱节点
static void batchInsertPrev(MYSQL* conn, int start_id,
                            const vector<int>& idx_to_id,
                            const vector<int>& dist,
                            const vector<int>& pre,
                            int batch_from, int batch_to) {
    ostringstream oss;
    oss << "INSERT INTO shortest_prev_new(start_id,end_id,prev_id) VALUES ";
    bool first = true;

    for (int i = batch_from; i < batch_to; ++i) {
        if (!first) 
            oss << ",";
        first = false;

        int end_id = idx_to_id[i];
        oss << "(" << start_id << "," << end_id << ",";

        if (dist[i] == INF || pre[i] == -1) {
            oss << "NULL";
        } 
         else {
            int prev_idx = pre[i];
            int prev_id = idx_to_id[prev_idx];
            oss << prev_id;
        }
        oss << ")";
    }
    oss << ";";
    execSQL(conn, oss.str());
}

int main(int argc, char** argv) {
    if (argc < 6) {
        cerr << "示例：" << argv[0] << " 192.168.3.9 3306 root root reactor_db\n";
        return 1;
    }

    string ip = argv[1];
    int port = atoi(argv[2]);
    string user = argv[3];
    string pass = argv[4];
    string db = argv[5];

    if (mysql_library_init(0, nullptr, nullptr) != 0) {
        cerr << "MySQL全局库初始化失败\n";
        return 1;
    }

    MYSQL* conn = mysql_init(nullptr);
    if (!conn) {
        cerr << "mysql_init失败\n";
        return 1;
    }


    if (!mysql_real_connect(conn, ip.c_str(), user.c_str(), pass.c_str(), db.c_str(), port, nullptr, 0)) {
        cerr << "mysql连接失败\n";
        return 1;
    }

    execSQL(conn, "SET NAMES utf8mb4;");    //防止地点名乱码

    cout <<"开始从 edge 表读取无向图数据...\n";

    // 读取 edge(u_id, v_id, w)
    MYSQL_RES* res;
    if (mysql_query(conn, "SELECT u_id, v_id, w FROM edge;") != 0) {
        cerr<<"查询失败\n";
        return 1;
    }
     res=mysql_store_result(conn);
    if (!res){
        cerr << "res没有获得结果集指针\n";
        return 1;
    }

    struct RawEdge { int u; int v; int w; };
    vector<RawEdge> edges;
    unordered_set<int> node_ids;   //获取节点数量和对于id

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        if (!row[0] || !row[1] || !row[2]) continue;
        int u = atoi(row[0]);
        int v = atoi(row[1]);
        int w = atoi(row[2]);
        node_ids.insert(u);
        node_ids.insert(v);
        edges.push_back({u,v,w});
    }
    mysql_free_result(res);

    if (node_ids.empty()) {
        cout << "edge表中没有任何节点/边数据\n";
        mysql_close(conn);
        mysql_library_end();
        return 0;
    }

    // id -> idx (0..n-1)
    vector<int> idx_to_id(node_ids.begin(), node_ids.end());
    sort(idx_to_id.begin(), idx_to_id.end());
    int n = (int)idx_to_id.size();

    unordered_map<int,int> id_to_idx;  //节点真实id到程序临时id的映射表
    id_to_idx.reserve(n * 2);    //预分配大小，防止频繁扩容，提高性能
    for (int i = 0; i < n; ++i) 
      id_to_idx[idx_to_id[i]] = i;

    cout << "读取完成，节点数N=" << n << "，边数M=" << edges.size() << "\n";

    // 将数据存入邻接表中
    watch graph(n - 1);
    for (auto& e : edges) {
        int ui = id_to_idx[e.u];
        int vi = id_to_idx[e.v];
        graph.input(ui, vi, e.w);
        graph.input(vi, ui, e.w);
    }


    // 建临时表
    execSQL(conn, "DROP TABLE IF EXISTS shortest_dist_new;");
    execSQL(conn, "DROP TABLE IF EXISTS shortest_prev_new;");

    execSQL(conn,
        "CREATE TABLE shortest_dist_new ("
        " start_id INT NOT NULL,"
        " end_id   INT NOT NULL,"
        " dist     INT NULL,"
        " PRIMARY KEY(start_id,end_id),"
        " INDEX idx_end(end_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;"
    );

    execSQL(conn,
        "CREATE TABLE shortest_prev_new ("
        " start_id INT NOT NULL,"
        " end_id   INT NOT NULL,"
        " prev_id  INT NULL,"
        " PRIMARY KEY(start_id,end_id),"
        " INDEX idx_end(end_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;"
    );

    // 批量写入
    execSQL(conn, "SET autocommit=0;");
    execSQL(conn, "START TRANSACTION;");  //开启事务，防止插入过程中异常导致数据丢失

    const int BATCH = 100;

    for (int s = 0; s < n; ++s) {
        auto [dist, pre] = graph.dijkstra(s);
        int start_id = idx_to_id[s];

        for (int i = 0; i < n; i += BATCH) {
            int j = min(i + BATCH, n);
            batchInsertDist(conn, start_id, idx_to_id, dist, i, j);
        }

        for (int i = 0; i < n; i += BATCH) {
            int j = min(i + BATCH, n);
            batchInsertPrev(conn, start_id, idx_to_id, dist, pre, i, j);
        }

        execSQL(conn, "COMMIT;");
        execSQL(conn, "START TRANSACTION;");

        cout << "已完成起点 " << (s+1) << "/" << n << "（start_id=" << start_id << "）\n";
    }

    execSQL(conn, "COMMIT;");
    execSQL(conn, "SET autocommit=1;");

    // 原子切换
    execSQL(conn, "DROP TABLE IF EXISTS shortest_dist_old;");
    execSQL(conn, "DROP TABLE IF EXISTS shortest_prev_old;");

    execSQL(conn,
        "RENAME TABLE "
        "shortest_dist TO shortest_dist_old, "
        "shortest_dist_new TO shortest_dist, "
        "shortest_prev TO shortest_prev_old, "
        "shortest_prev_new TO shortest_prev;"
    );

    execSQL(conn, "DROP TABLE shortest_dist_old;");
    execSQL(conn, "DROP TABLE shortest_prev_old;");

    cout << "mysql更新数据成功\n";

    mysql_close(conn);
    mysql_library_end();
    return 0;
}
