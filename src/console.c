/*
Copyright (C) 2013  
Baptiste Lepers <baptiste.lepers@gmail.com>

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
version 2, as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/tty.h>

#include "console.h"
#if !DISABLE_CONSOLE_PRINT
int printu(const char *fmt, ...) {
   struct tty_struct *my_tty;
   va_list args;
   int ret;
   char buf[512];

   va_start(args, fmt);
   ret=vsprintf(buf,fmt,args);
   va_end(args);

   my_tty = current->signal->tty;
   if (my_tty != NULL) {
      int len = strlen(buf);
      if(len > 1 && buf[len - 1] == '\n' && buf[len - 2] != '\r') {
         buf[len - 1] = '\r';
         buf[len] = '\n';
         buf[len + 1] = '\0';
         len += 1;
      }
      ((my_tty->driver->ops)->write) (my_tty,  buf, len);
   }
   return 0;
}
#endif
