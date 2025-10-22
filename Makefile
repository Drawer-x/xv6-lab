# ================================================================
# ECNU-OSLAB-2025 内核 Makefile（修正版）
# 自动生成 user/initcode.h + 编译 kernel + 运行 QEMU
# ================================================================

# 交叉编译工具链
CC      = riscv64-linux-gnu-gcc
LD      = riscv64-linux-gnu-ld
OBJCOPY = riscv64-linux-gnu-objcopy
OBJDUMP = riscv64-linux-gnu-objdump
GDB     = riscv64-linux-gnu-gdb

# 目录
KERNEL_DIR = src/kernel
USER_DIR   = src/user
TARGET_DIR = target

# 编译参数
CFLAGS = -Wall -Werror -O -fno-omit-frame-pointer -ggdb -gdwarf-2 \
         -MD -mcmodel=medany -ffreestanding -fno-common -nostdlib \
         -mno-relax -I. -fno-stack-protector -fno-pie -no-pie

LDFLAGS = -T kernel.ld -nostdlib -no-pie -v

# 目标文件
KERNEL_ELF = $(TARGET_DIR)/kernel-qemu.elf
INITCODE_H = $(USER_DIR)/initcode.h
INITCODE_ELF = $(USER_DIR)/initcode.elf
INITCODE_BIN = $(USER_DIR)/initcode.bin

# 搜索所有源文件
C_SRC  := $(shell find $(KERNEL_DIR) -name "*.c")
S_SRC  := $(shell find $(KERNEL_DIR) -name "*.S")
OBJ    := $(patsubst src/%.c, $(TARGET_DIR)/%.o, $(C_SRC)) \
           $(patsubst src/%.S, $(TARGET_DIR)/%.o, $(S_SRC))

# ================================================================
# 默认目标
# ================================================================

.PHONY: all run qemu clean

all: $(KERNEL_ELF)

run: $(KERNEL_ELF)
	qemu-system-riscv64 -machine virt -kernel $(KERNEL_ELF) -nographic

qemu: run

# ================================================================
# 编译规则
# ================================================================

# 生成 kernel ELF
$(KERNEL_ELF): $(OBJ)
	@mkdir -p $(dir $@)
	$(LD) -o $@ $^ $(LDFLAGS)
	@echo "[LD]  -> $@"

# C 源文件
$(TARGET_DIR)/%.o: src/%.c $(INITCODE_H)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<
	@echo "[CC]  -> $<"

# 汇编源文件
$(TARGET_DIR)/%.o: src/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<
	@echo "[AS]  -> $<"

# ================================================================
# user/initcode 构建规则
# ================================================================

# 1. 编译 initcode.c -> initcode.elf
$(INITCODE_ELF): $(USER_DIR)/initcode.c $(USER_DIR)/initcode.ld
	$(CC) -nostdlib -nostartfiles -Wl,--build-id=none -T $(USER_DIR)/initcode.ld -o $@ $<
	@echo "[LD]  -> $@"

# 2. 导出成二进制 -> initcode.bin
$(INITCODE_BIN): $(INITCODE_ELF)
	$(OBJCOPY) -S -O binary $< $@
	@echo "[OBJCOPY] -> $@"

# 3. 转换成 C 数组 -> initcode.h
$(INITCODE_H): $(INITCODE_BIN)
	xxd -i $< > $@
	@echo "[GEN] -> $@"

# ================================================================
# 清理
# ================================================================

clean:
	rm -rf $(TARGET_DIR) $(USER_DIR)/initcode.{elf,bin,h}
	@echo "[CLEAN] done."
