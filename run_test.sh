#!/bin/bash

# ECNU OS Lab9 快速测试脚本
# 用法: ./run_test.sh [1|2|3|4|all]

TIMEOUT=30
DIR="$(cd "$(dirname "$0")" && pwd)"
INITCODE="$DIR/src/user/initcode.c"

# 生成 initcode.c
write_initcode() {
    local test_num=$1
    local msg_len
    local argv_line

    case $test_num in
        1)
            msg_len=32
            argv_line='char *argv[] = {"test_1", "111", "222", "333", 0};'
            ;;
        2|3|4)
            msg_len=13
            argv_line="char *argv[] = {\"test_$test_num\", 0};"
            ;;
    esac

cat > "$INITCODE" << EOF
/*
 * initcode.c - 第一个用户进程
 * 自动生成 - test_$test_num
 */
#include "sys.h"

int main()
{
    char *msg = "run ./test_$test_num\\n";
    syscall(SYS_write, 1, $msg_len, msg);
    
    int pid = syscall(SYS_fork);
    
    if (pid == 0) {
        $argv_line
        syscall(SYS_exec, "/test_$test_num", argv);
        syscall(SYS_exit, -1);
    } else if (pid > 0) {
        int status;
        syscall(SYS_wait, &status);
    }
    
    while(1);
}
EOF
}

# 运行单个测试
run_single_test() {
    local test_num=$1
    echo ""
    echo "========================================"
    echo "  运行 Test $test_num (timeout=${TIMEOUT}s)"
    echo "========================================"
    
    write_initcode $test_num
    
    cd "$DIR"
    make clean >/dev/null 2>&1
    
    if ! make build >/dev/null 2>&1; then
        echo "[ERROR] 编译失败!"
        return 1
    fi
    
    echo "[INFO] 编译成功, 启动 QEMU..."
    echo "----------------------------------------"
    
    # Test 1 和 Test 4 需要交互输入
    if [ "$test_num" = "1" ]; then
        echo "[提示] Test 1 需要输入, 自动输入: hello world"
        (sleep 3; echo "hello world") | timeout $TIMEOUT make run 2>/dev/null
    elif [ "$test_num" = "4" ]; then
        echo "[提示] Test 4 需要回答4个问题"
        timeout $TIMEOUT make run
    else
        timeout $TIMEOUT make run 2>/dev/null
    fi
    
    echo "----------------------------------------"
    echo "[INFO] Test $test_num 完成"
}

# 显示帮助
show_help() {
    echo "ECNU OS Lab9 测试脚本"
    echo ""
    echo "用法: $0 [选项]"
    echo ""
    echo "选项:"
    echo "  1      运行 Test 1 (stdin/stdout/stderr + exec传参)"
    echo "  2      运行 Test 2 (文件操作: open/close/read/write)"
    echo "  3      运行 Test 3 (目录操作: mkdir/chdir/link/unlink)"
    echo "  4      运行 Test 4 (设备文件: /dev/null/zero/gpt0)"
    echo "  all    依次运行全部测试"
    echo "  clean  清理编译产物"
    echo "  help   显示此帮助"
    echo ""
    echo "示例:"
    echo "  ./run_test.sh 1      # 运行测试1"
    echo "  ./run_test.sh all    # 运行全部测试"
}

# 主逻辑
case "${1:-help}" in
    1|2|3|4)
        run_single_test $1
        ;;
    all|a)
        for i in 1 2 3 4; do
            run_single_test $i
            sleep 1
        done
        echo ""
        echo "========================================"
        echo "  全部测试完成!"
        echo "========================================"
        ;;
    clean|c)
        cd "$DIR"
        make clean
        echo "清理完成"
        ;;
    help|h|*)
        show_help
        ;;
esac

