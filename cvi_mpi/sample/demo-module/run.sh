#! /bin/bash

# 1. 本地编译
make

# 2. 检查远程是不是有同名进程，如果有，则优雅地退出
ssh root@192.168.100.161 "pkill -2 demo"

# 3. 复制到远程
scp demo root@192.168.100.161:/root

# 4. 远程启动并输出到本地
ssh root@192.168.100.161 "/root/demo"
