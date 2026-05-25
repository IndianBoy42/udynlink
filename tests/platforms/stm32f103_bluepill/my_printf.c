#include <stdarg.h>
#include <string.h>
#include <unistd.h>

extern int _write(int file, char *ptr, int len);

int my_printf(const char *fmt, ...) {
    char buf[128];
    int len = 0;
    va_list args;
    va_start(args, fmt);
    
    for (const char *p = fmt; *p; p++) {
        if (*p == '%' && *(p+1) == 'd') {
            p++;
            int val = va_arg(args, int);
            // Simple int to string
            char tmp[12];
            int i = 0, neg = 0;
            if (val < 0) { neg = 1; val = -val; }
            do { tmp[i++] = '0' + (val % 10); val /= 10; } while (val);
            if (neg) tmp[i++] = '-';
            while (i--) buf[len++] = tmp[i];
        } else {
            buf[len++] = *p;
        }
    }
    va_end(args);
    buf[len] = '\0';
    _write(1, buf, len);
    return len;
}
