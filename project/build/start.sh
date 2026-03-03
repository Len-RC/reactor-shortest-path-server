#!/bin/bash

# Windows MySQL连接配置
export MYSQL_HOST=10.4.97.75
export MYSQL_PORT=3306
export MYSQL_USER=root
export MYSQL_PASS=root
export MYSQL_DB=reactor_db

echo "=========================================="
echo "正在连接到 Windows MySQL"
echo "MySQL地址: $MYSQL_HOST:$MYSQL_PORT"
echo "数据库: $MYSQL_DB"
echo "=========================================="
echo ""

# 测试MySQL连接
echo "测试MySQL连接..."
if command -v mysql &> /dev/null; then
    mysql -h$MYSQL_HOST -u$MYSQL_USER -p$MYSQL_PASS -e "SELECT 1;" 2>/dev/null
    if [ $? -eq 0 ]; then
        echo "✓ MySQL连接成功"
    else
        echo "✗ MySQL连接失败，请检查："
        echo "  1. Windows MySQL是否允许远程连接"
        echo "  2. Windows防火墙是否开放3306端口"
        echo "  3. MySQL密码是否正确（当前使用: $MYSQL_PASS）"
        echo ""
        read -p "是否继续启动服务器？(y/n) " -n 1 -r
        echo
        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            exit 1
        fi
    fi
else
    echo "⚠ 未安装mysql客户端，跳过连接测试"
fi

echo ""
echo "启动服务器..."
./server
