//功能：内存分配，默认使用 jemalloc
#ifndef __MALLOC_HOOK_H
#define __MALLOC_HOOK_H

#include <stdlib.h>

extern size_t malloc_used_memory(void);
extern size_t malloc_memory_block(void);
extern void   memory_info_dump(void);
extern size_t mallctl_int64(const char* name, size_t* newval);
extern int    mallctl_opt(const char* name, int* newval);
extern void   dump_c_mem(void);

#endif /* __MALLOC_HOOK_H */