/* low-level driver routines for 16550a UART. */

#include "mod.h"
#include <stdint.h>  // 引入 uint8_t、uint64_t 等固定宽度类型的定义
void uart_puts(const char *s);       // 输出字符串
void uart_putint(int num);           // 输出整数
void uart_puthex(uint64 addr);       // 输出十六进制数

// 辅助函数：数字转字符串（十进制）
static void itoa(int num, char *buf, int base);
// 辅助函数：数字转字符串（十六进制，64位）
static void itoa_hex(uint64 num, char *buf);

// from printf.c 终止输出的标志
extern volatile int panicked;

// uart 初始化
void uart_init(void)
{
  	// 关闭中断
	WriteReg(IER, 0x00);

	// 进入设置比特率的模式
	WriteReg(LCR, LCR_BAUD_LATCH);

	// 设置比特率的低位和高位，最终设置为38.4K
	WriteReg(0, 0x03);
  	WriteReg(1, 0x00);

	// 设置传输字节长度为8bit,不校验
	WriteReg(LCR, LCR_EIGHT_BITS);

	// 清零和使能FIFO模式
	WriteReg(FCR, FCR_FIFO_ENABLE | FCR_FIFO_CLEAR);

	// 使能输出队列和接收队列的中断
	WriteReg(IER, IER_TX_ENABLE | IER_RX_ENABLE);
}

// 单个字符输出
void uart_putc_sync(int c)
{
	// 关闭中断
	push_off();

	// 如果错误发生则卡住
	while (panicked)
		;

	// 等待TX队列进入idle状态
	while ((ReadReg(LSR) & LSR_TX_IDLE) == 0)
		;

	// 输出
	WriteReg(THR, c);

	// 开启中断
	pop_off();
}

// 单个字符输入
// 失败返回-1
int uart_getc_sync(void)
{
	if (ReadReg(LSR) & 0x01)
		return ReadReg(RHR);
	else
		return -1;
}

void uart_intr(void)
{
    // 读取 ISR 清除 UART 硬件中断标志
    ReadReg(ISR);

    // 读取 LSR 寄存器，检查接收 FIFO 是否有数据
    while ((ReadReg(LSR) & LSR_RX_READY) != 0)
    {
        int c = uart_getc_sync();
        if (c == -1) break;

        // 调用控制台编辑函数处理输入
        cons_edit(c);
    }
}
// 输出字符串（循环调用单个字符输出）
void uart_puts(const char *s) {
    for (int i = 0; s[i] != '\0'; i++) {
        // 处理换行符（补充回车，与现有uart_putc_sync逻辑一致）
        if (s[i] == '\n') {
            uart_putc_sync('\r');
        }
        uart_putc_sync(s[i]);
    }
}
// 十进制整数转字符串（base=10）
static void itoa(int num, char *buf, int base) {
    int i = 0;
    int is_negative = 0;

    // 处理0的特殊情况
    if (num == 0) {
        buf[i++] = '0';
        buf[i] = '\0';
        return;
    }

    // 记录负数
    if (num < 0 && base == 10) {
        is_negative = 1;
        num = -num;  // 转为正数处理
    }

    // 逐位提取数字（逆序）
    while (num != 0) {
        int rem = num % base;
        buf[i++] = (rem > 9) ? (rem - 10 + 'a') : (rem + '0');
        num = num / base;
    }

    // 添加负号
    if (is_negative) {
        buf[i++] = '-';
    }

    // 字符串结束符
    buf[i] = '\0';

    // 反转字符串（因为上面是逆序存储）
    int len = i;
    for (int j = 0; j < len / 2; j++) {
        char temp = buf[j];
        buf[j] = buf[len - j - 1];
        buf[len - j - 1] = temp;
    }
}
// 输出整数（十进制）
void uart_putint(int num) {
    char buf[20];  // 足够存储32位整数的最大长度（含符号和结束符）
    itoa(num, buf, 10);
    uart_puts(buf);
}
// 64位无符号整数转十六进制字符串（固定16位）
static void itoa_hex(uint64 num, char *buf) {
    const char *digits = "0123456789abcdef";
    buf[16] = '\0';  // 字符串结束符

    // 从高位到低位处理（64位=16个4位）
    for (int i = 15; i >= 0; i--) {
        uint8_t nibble = (num >> (i * 4)) & 0x0f;  // 提取4位（一个十六进制位）
        buf[15 - i] = digits[nibble];  // 正向存储
    }
}
// 输出十六进制数（64位，带0x前缀）
void uart_puthex(uint64 addr) {
    char buf[17];  // 16位十六进制数 + 结束符
    itoa_hex(addr, buf);
    uart_puts("0x");  // 前缀
    uart_puts(buf);
}