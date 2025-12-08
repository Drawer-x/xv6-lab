/* 标准输出和报错机制 */

#include "mod.h"
#include <stdarg.h>   // 新增：可变参数

static char digits[] = "0123456789abcdef";

/* printf的自旋锁 */
static spinlock_t print_lk;

/* 如果发生panic, UART的停止标志 */
volatile int panicked = 0;

/* 初始化uart + 初始化printf锁 */
void print_init(void)
{
    uart_init();
    spinlock_init(&print_lk, "printf");
}

/* %d / %u / %x 的公共实现 */
static void printint_unsigned(uint64 x, int base)
{
    char buf[32];
    int i = 0;
    do {
        buf[i++] = digits[x % base];
        x /= base;
    } while (x != 0);

    while (--i >= 0)
        uart_putc_sync(buf[i]);
}

static void printint_signed(int xx)
{
    uint32 x;
    if (xx < 0) {
        uart_putc_sync('-');
        // 用无符号算术避免 INT_MIN 溢出
        x = (uint32)(-(int64)xx);
    } else {
        x = (uint32)xx;
    }
    printint_unsigned((uint64)x, 10);
}

/* %p */
static void printptr(uint64 x)
{
    uart_putc_sync('0');
    uart_putc_sync('x');
    for (int i = 0; i < (int)(sizeof(uint64) * 2); i++, x <<= 4)
        uart_putc_sync(digits[x >> ((int)sizeof(uint64) * 8 - 4)]);
}

/*
    标准化输出, 支持:
    %d (32位有符号10进制)
    %u (32位无符号10进制)
    %x (32位无符号16进制)
    %p (64位指针)
    %c (字符)
    %s (字符串)
    %% (百分号)
*/
void printf(const char *fmt, ...)
{
    va_list ap;
    int i, c;
    char *s;

    va_start(ap, fmt);

    // 如果已经panic，直接裸写 UART，避免锁 & 复杂路径
    int need_unlock = 0;
    if (!panicked) {
        spinlock_acquire(&print_lk);
        need_unlock = 1;
    }

    for (i = 0; (c = fmt[i] & 0xff) != 0; i++) {
        if (c != '%') {
            uart_putc_sync(c);
            continue;
        }

        c = fmt[++i] & 0xff;
        if (c == 0) break;

        switch (c) {
        case 'd':
            printint_signed(va_arg(ap, int));
            break;
        case 'u':
            printint_unsigned((uint32)va_arg(ap, unsigned int), 10);
            break;
        case 'x':
            printint_unsigned((uint32)va_arg(ap, unsigned int), 16);
            break;
        case 'p':
            printptr(va_arg(ap, uint64));
            break;
        case 'c':
            uart_putc_sync(va_arg(ap, int));
            break;
        case 's':
            s = va_arg(ap, char *);
            if (s == 0) s = "(null)";
            for (; *s; s++)
                uart_putc_sync(*s);
            break;
        case '%':
            uart_putc_sync('%');
            break;
        default:
            // 打印未知格式字符
            uart_putc_sync('%');
            uart_putc_sync(c);
            break;
        }
    }

    if (need_unlock)
        spinlock_release(&print_lk);

    va_end(ap);
}

/* 报错并终止输出：避免走 printf 路径造成递归/死锁 */
void panic(const char *s)
{
    push_off();          // 关中断，防止打断
    panicked = 1;        // 标记后，printf 将不会试图加锁
    uart_puts("panic! ");
    uart_puts(s);
    uart_puts("\n");
    while (1) { /* spin */ }
}

/* 如果不满足条件, 则调用panic */
void assert(bool condition, const char *warning)
{
    if (!condition) {
        panic(warning);
    }
}
