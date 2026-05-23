#include <stdint.h>

#define UART_TX_DATA  (*(volatile uint32_t *)0x10000000)
#define UART_STATUS   (*(volatile uint32_t *)0x10000004)
#define TERM_DATA     (*(volatile uint32_t *)0x10000020)
#define TERM_STATUS   (*(volatile uint32_t *)0x10000024)
#define LED_REG       (*(volatile uint32_t *)0x10000010)

#define ST_TX_BUSY    (1u << 0)
#define ST_TERM_BUSY  (1u << 0)

static void uart_putc(char c) {
    while (UART_STATUS & ST_TX_BUSY) ;
    UART_TX_DATA = (uint32_t)(uint8_t)c;
}

static void term_putc(char c) {
    while (TERM_STATUS & ST_TERM_BUSY) ;
    TERM_DATA = (uint32_t)(uint8_t)c;
}

static void putc_both(char c) { uart_putc(c); term_putc(c); }

static void puts_both(const char *s) {
    while (*s) putc_both(*s++);
}

int main(void) {
    LED_REG = 0x2A;
    puts_both("\r\nHello, world!\r\n");
    puts_both("(this came from app at 0x1000)\r\n");
    for (;;) ;
}
