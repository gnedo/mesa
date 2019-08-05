
#ifndef __NOUVEAU_DEBUG_H__
#define __NOUVEAU_DEBUG_H__

#include <stdio.h>

#include "util/u_debug.h"

#define NOUVEAU_DEBUG_MISC       0x0001
#define NOUVEAU_DEBUG_SHADER     0x0100
#define NOUVEAU_DEBUG_PROG_IR    0x0200
#define NOUVEAU_DEBUG_PROG_RA    0x0400
#define NOUVEAU_DEBUG_PROG_CFLOW 0x0800
#define NOUVEAU_DEBUG_PROG_ALL   0x1f00

#define NOUVEAU_DEBUG 0

#define NOUVEAU_ERR(fmt, ...)                                 \
   fprintf(stderr, "%s:%d - " fmt, __FUNCTION__, __LINE__, ##__VA_ARGS__)

#define NOUVEAU_DBG(ch, ...)           \
   if ((NOUVEAU_DEBUG) & (NOUVEAU_DEBUG_##ch))        \
      debug_printf( __VA_ARGS__)

#endif /* __NOUVEAU_DEBUG_H__ */
