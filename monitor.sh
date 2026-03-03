#!/bin/bash
# 服务监控脚本 - 实时监控3个实例的运行状态

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# 配置
PORTS=(9090 9091 9092)
NGINX_PORT=80
SERVER_IP="192.168.233.129"
LOG_FILE="/tmp/monitor.log"

# 清屏
clear

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}   服务监控面板 - $(date '+%Y-%m-%d %H:%M:%S')${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""

# 1. 检查后端实例状态
echo -e "${YELLOW}[1] 后端实例状态${NC}"
echo "----------------------------------------"
for port in "${PORTS[@]}"; do
    # 检查进程
    pid=$(ps aux | grep "./server $port" | grep -v grep | awk '{print $2}')
    
    if [ -n "$pid" ]; then
        # 获取CPU和内存使用率
        cpu=$(ps -p $pid -o %cpu= | tr -d ' ')
        mem=$(ps -p $pid -o %mem= | tr -d ' ')
        
        # 检查端口监听
        if ss -tlnp 2>/dev/null | grep -q ":$port "; then
            status="${GREEN}✓ 运行中${NC}"
        else
            status="${RED}✗ 端口未监听${NC}"
        fi
        
        # 获取连接数
        conn_count=$(ss -tn 2>/dev/null | grep ":$port " | wc -l)
        
        echo -e "  端口 $port: $status | PID: $pid | CPU: ${cpu}% | 内存: ${mem}% | 连接数: $conn_count"
    else
        echo -e "  端口 $port: ${RED}✗ 未运行${NC}"
    fi
done
echo ""

# 2. 检查 Nginx 状态
echo -e "${YELLOW}[2] Nginx 状态${NC}"
echo "----------------------------------------"
if systemctl is-active --quiet nginx; then
    nginx_pid=$(systemctl show -p MainPID nginx | cut -d= -f2)
    nginx_cpu=$(ps -p $nginx_pid -o %cpu= 2>/dev/null | tr -d ' ')
    nginx_mem=$(ps -p $nginx_pid -o %mem= 2>/dev/null | tr -d ' ')
    nginx_conn=$(ss -tn 2>/dev/null | grep ":80 " | wc -l)
    
    echo -e "  状态: ${GREEN}✓ 运行中${NC}"
    echo -e "  PID: $nginx_pid | CPU: ${nginx_cpu}% | 内存: ${nginx_mem}% | 连接数: $nginx_conn"
else
    echo -e "  状态: ${RED}✗ 未运行${NC}"
fi
echo ""

# 3. 检查 Redis 状态
echo -e "${YELLOW}[3] Redis 状态${NC}"
echo "----------------------------------------"
if pgrep -x redis-server > /dev/null; then
    redis_pid=$(pgrep -x redis-server)
    redis_cpu=$(ps -p $redis_pid -o %cpu= | tr -d ' ')
    redis_mem=$(ps -p $redis_pid -o %mem= | tr -d ' ')
    
    # 获取 Redis 信息
    if command -v redis-cli &> /dev/null; then
        redis_clients=$(redis-cli INFO clients 2>/dev/null | grep "connected_clients:" | cut -d: -f2 | tr -d '\r')
        redis_ops=$(redis-cli INFO stats 2>/dev/null | grep "instantaneous_ops_per_sec:" | cut -d: -f2 | tr -d '\r')
        redis_mem_used=$(redis-cli INFO memory 2>/dev/null | grep "used_memory_human:" | cut -d: -f2 | tr -d '\r')
        
        echo -e "  状态: ${GREEN}✓ 运行中${NC}"
        echo -e "  PID: $redis_pid | CPU: ${redis_cpu}% | 内存: ${redis_mem}%"
        echo -e "  客户端连接: $redis_clients | QPS: $redis_ops | 内存使用: $redis_mem_used"
    else
        echo -e "  状态: ${GREEN}✓ 运行中${NC}"
        echo -e "  PID: $redis_pid | CPU: ${redis_cpu}% | 内存: ${redis_mem}%"
    fi
else
    echo -e "  状态: ${RED}✗ 未运行${NC}"
fi
echo ""

# 4. 检查 MySQL 状态
echo -e "${YELLOW}[4] MySQL 状态${NC}"
echo "----------------------------------------"
if pgrep -x mysqld > /dev/null; then
    mysql_pid=$(pgrep -x mysqld)
    mysql_cpu=$(ps -p $mysql_pid -o %cpu= | tr -d ' ')
    mysql_mem=$(ps -p $mysql_pid -o %mem= | tr -d ' ')
    mysql_conn=$(ss -tn 2>/dev/null | grep ":3306 " | wc -l)
    
    echo -e "  状态: ${GREEN}✓ 运行中${NC}"
    echo -e "  PID: $mysql_pid | CPU: ${mysql_cpu}% | 内存: ${mysql_mem}% | 连接数: $mysql_conn"
else
    echo -e "  状态: ${RED}✗ 未运行${NC}"
fi
echo ""

# 5. 负载分布（通过日志分析最近的请求）
echo -e "${YELLOW}[5] 负载分布（最近1分钟）${NC}"
echo "----------------------------------------"
if [ -f /var/log/nginx/shortest_path_access.log ]; then
    # 统计最近1分钟各后端的请求数
    cutoff_time=$(date -d '1 minute ago' '+%d/%b/%Y:%H:%M:%S')
    
    for port in "${PORTS[@]}"; do
        # 这里简化处理，实际需要解析 Nginx 日志
        count=$(tail -1000 /var/log/nginx/shortest_path_access.log 2>/dev/null | grep -c "upstream.*$port" || echo "0")
        echo -e "  端口 $port: $count 个请求"
    done
else
    echo -e "  ${YELLOW}日志文件不存在${NC}"
fi
echo ""

# 6. 系统资源
echo -e "${YELLOW}[6] 系统资源${NC}"
echo "----------------------------------------"
# CPU 使用率
cpu_usage=$(top -bn1 | grep "Cpu(s)" | awk '{print $2}' | cut -d% -f1)
echo -e "  CPU 使用率: ${cpu_usage}%"

# 内存使用率
mem_total=$(free -m | awk 'NR==2{print $2}')
mem_used=$(free -m | awk 'NR==2{print $3}')
mem_percent=$(awk "BEGIN {printf \"%.1f\", ($mem_used/$mem_total)*100}")
echo -e "  内存使用: ${mem_used}MB / ${mem_total}MB (${mem_percent}%)"

# 磁盘使用率
disk_usage=$(df -h / | awk 'NR==2{print $5}')
echo -e "  磁盘使用: $disk_usage"

# 网络连接数
total_conn=$(ss -tn 2>/dev/null | grep ESTAB | wc -l)
echo -e "  总连接数: $total_conn"
echo ""

# 7. 快速测试
echo -e "${YELLOW}[7] 服务可用性测试${NC}"
echo "----------------------------------------"
# 测试 Nginx
if curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1/ | grep -q "200"; then
    echo -e "  Nginx HTTP: ${GREEN}✓ 正常${NC}"
else
    echo -e "  Nginx HTTP: ${RED}✗ 异常${NC}"
fi

# 测试各个后端（需要安装 telnet 或 nc）
for port in "${PORTS[@]}"; do
    if timeout 1 bash -c "echo > /dev/tcp/127.0.0.1/$port" 2>/dev/null; then
        echo -e "  后端 $port: ${GREEN}✓ 可连接${NC}"
    else
        echo -e "  后端 $port: ${RED}✗ 无法连接${NC}"
    fi
done
echo ""

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}提示: 使用 'watch -n 2 ./monitor.sh' 实时监控${NC}"
echo -e "${BLUE}========================================${NC}"
