/* low-level driver routines for 16550a UART. */

#include "mod.h"

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

// 中断处理(键盘输入->屏幕输出)
void uart_intr(void)
{
    while (1)
    {
        int c = uart_getc_sync();
        if (c == -1) // 无更多输入，退出循环
            break;

        // 1. 处理换行：将 Windows 风格的 \r（回车）转为 Unix 风格的 \n（换行）
        // 原因：键盘按下 Enter 键时，部分终端发送 \r，需转为 \n 才会换行显示
        if (c == '\r')
        {
            uart_putc_sync('\n'); // 输出 \n 实现换行
        }
        // 2. 处理退格：支持 Backspace（\b，ASCII 8）和 Delete（127，ASCII 127）
        // 退格逻辑：光标回退(\b) → 输出空格覆盖原字符 → 光标再次回退(\b)
        else if (c == '\b' || c == 127)
        {
            uart_putc_sync('\b');  // 第一步：光标左移1位
            uart_putc_sync(' ');   // 第二步：用空格覆盖原字符（清除显示）
            uart_putc_sync('\b');  // 第三步：光标再左移1位（准备输入新字符）
        }
        // 3. 普通字符：直接回显（如字母、数字、符号等）
        else
        {
            uart_putc_sync(c);
        }
    }
}