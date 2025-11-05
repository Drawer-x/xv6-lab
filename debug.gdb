# 关闭确认提示
set confirm off

# 指定目标架构为 RISC-V 64位
set architecture riscv:rv64

# 加载内核符号文件（注意路径要和你的内核 elf 文件一致）
symbol-file target/kernel/kernel-qemu.elf

# 自动反汇编下一条指令
set disassemble-next-line auto

# 连接 QEMU 的调试端口（1234是默认端口）
target remote 127.0.0.1:1234
