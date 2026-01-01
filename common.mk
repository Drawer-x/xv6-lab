# 这个文件负责公共的配置

# 自动检测 RISC-V 工具链前缀
TOOLPREFIX ?= $(shell \
    if which riscv64-linux-gnu-gcc > /dev/null 2>&1; then echo riscv64-linux-gnu-; \
    elif which riscv64-unknown-elf-gcc > /dev/null 2>&1; then echo riscv64-unknown-elf-; \
    elif which riscv64-elf-gcc > /dev/null 2>&1; then echo riscv64-elf-; \
    else echo riscv64-linux-gnu-; fi)

CC = ${TOOLPREFIX}gcc
LD = ${TOOLPREFIX}ld
OBJCOPY = ${TOOLPREFIX}objcopy
OBJDUMP = ${TOOLPREFIX}objdump

# 编译相关配置
CFLAGS = -Wall -Werror -O -fno-omit-frame-pointer -ggdb -gdwarf-2
CFLAGS += -MD
CFLAGS += -mcmodel=medany
CFLAGS += -ffreestanding -fno-common -nostdlib -mno-relax
CFLAGS += -I.
CFLAGS += $(shell $(CC) -fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector)

# Disable PIE when possible (for Ubuntu 16.10 toolchain)
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]no-pie'),)
CFLAGS += -fno-pie -no-pie
endif
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]nopie'),)
CFLAGS += -fno-pie -nopie
endif

# 链接相关配置 
LDFLAGS = -z max-page-size=4096