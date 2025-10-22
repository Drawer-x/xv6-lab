# ==========================================
# _entry.S — QEMU 启动验证用 UART 输出程序
# ==========================================
# 功能：
#   1. 从 QEMU 加载后立即运行（位于 0x80200000）
#   2. 向 UART (0x10000000) 连续输出字符串 "HELLO!\n"
#   3. 进入死循环，防止 QEMU 退出
# ==========================================

.section .text
.globl _entry
_entry:
    # UART MMIO 基地址
    li t0, 0x10000000

print_loop:
    # 打印字符串
    la t1, message
print_char:
    lbu t2, 0(t1)          # 取当前字符
    beqz t2, done          # 若为 0，结束
    sb t2, 0(t0)           # 写入 UART 寄存器
    addi t1, t1, 1         # 指向下一个字符
    j print_char

done:
    # 死循环防止退出
    j done

# 数据区 — 要打印的字符串
.section .rodata
message:
    .string "HELLO!\n"
