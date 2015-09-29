#ifndef __CONSOLE__
#define __CONSOLE__

#define DISABLE_CONSOLE_PRINT    1

#if DISABLE_CONSOLE_PRINT
#define printu printk
#else
int printu(const char *fmt, ...);
#endif

#endif
