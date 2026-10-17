// SPDX-License-Identifier: MIT
// Android adaptation of D2R Mobile's write-protect / generation mechanism.
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int nvn_dirty_add_range(const void*,size_t);
void nvn_dirty_remove_range(const void*);
int nvn_dirty_protect(const void*,size_t);
int nvn_dirty_tracked(const void*,size_t);
uint64_t nvn_dirty_now(void);
int nvn_dirty_since(const void*,size_t,uint64_t);
// Exact intersections with changed pages. Untracked ranges conservatively
// return the whole input once. cursor and outputs are relative byte offsets.
int nvn_dirty_next_range(const void*,size_t,uint64_t,size_t*,size_t*,size_t*);
void nvn_dirty_use_summary(int);
void nvn_dirty_query_stats(uint64_t* pages,uint64_t* blocks,uint64_t* skipped);
// Called only for a confirmed write protection fault. No allocation or locks.
int nvn_dirty_fault(const void*);
// Kernel writes return EFAULT instead of entering our signal handler. A write
// lease unprotects overlapping pages and excludes re-protection until end.
int nvn_dirty_begin_write(void*,size_t);
void nvn_dirty_end_write(void);
uint64_t nvn_dirty_fault_count(void);
#ifdef __cplusplus
}
struct NvnDirtyWrite {
    bool locked;
    NvnDirtyWrite(void* p,size_t n):locked(nvn_dirty_begin_write(p,n)){}
    ~NvnDirtyWrite(){if(locked)nvn_dirty_end_write();}
    NvnDirtyWrite(const NvnDirtyWrite&)=delete;
    NvnDirtyWrite& operator=(const NvnDirtyWrite&)=delete;
};
#endif
