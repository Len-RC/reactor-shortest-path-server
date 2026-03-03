#!/bin/bash
# 启动三个后端服务实例

echo "正在启动后端服务..."

# 启动三个实例
nohup ./build/server 9090 > logs/server_9090.log 2>&1 &
echo "已启动服务实例: 端口 9090, PID: $!"

nohup ./build/server 9091 > logs/server_9091.log 2>&1 &
echo "已启动服务实例: 端口 9091, PID: $!"

nohup ./build/server 9092 > logs/server_9092.log 2>&1 &
echo "已启动服务实例: 端口 9092, PID: $!"

sleep 2

echo ""
echo "服务启动完成，当前运行状态："
ps aux | grep -E "server (9090|9091|9092)" | grep -v grep
