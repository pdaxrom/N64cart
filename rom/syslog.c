#include "syslog.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "n64cart.h"

void openlog(char *ident, int option, int facility)
{
}

int setlogmask(int __mask)
{
    return 0;
}

void syslog(int pri, const char *fmt, ...)
{
    char message[2048];

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    n64cart_uart_puts("syslog: ");
    n64cart_uart_puts(message);
    n64cart_uart_puts("\n");
}

void closelog(void)
{
}
