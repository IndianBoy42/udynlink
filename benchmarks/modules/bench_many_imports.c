#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void *host_malloc(unsigned int);
extern void  host_free(void *);
extern int   host_read_reg(int);
extern void  host_write_reg(int, int);
extern void  host_delay_us(unsigned int);
extern int   host_uart_send(const unsigned char *, unsigned int);
extern int   host_uart_recv(unsigned char *, unsigned int);
extern void  host_gpio_set(int, int);
extern int   host_gpio_get(int);
extern void  host_spi_xfer(int, const unsigned char *, unsigned char *, unsigned int);

int bench_imports(void) {
    void *p = host_malloc(64);
    if (!p) return -1;
    host_free(p);

    int r = host_read_reg(0x10);
    host_write_reg(0x14, r | 1);
    host_delay_us(1000);

    unsigned char tx[4] = {0x9F, 0, 0, 0};
    unsigned char rx[4];
    host_spi_xfer(1, tx, rx, 4);

    host_gpio_set(5, 1);
    int pin = host_gpio_get(5);

    unsigned char msg[] = "ok";
    host_uart_send(msg, 2);

    char buf[16];
    memset(buf, 0, sizeof(buf));
    sprintf(buf, "%d", pin);

    return 0;
}
