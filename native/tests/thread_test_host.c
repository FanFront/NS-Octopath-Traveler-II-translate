#include "../thread_services.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
void crash_thread_init(void) {}
void guest_thread_init(void) {}
void guest_thread_exit(void) { svc_thread_exit_on_host(); }
void dm_thread_enter(uint64_t entry,uint64_t arg,uint64_t sp) {
    (void)sp;((void(*)(uint64_t))entry)(arg);guest_thread_exit();
}
void ot2_host_log(const char* fmt,...) {(void)fmt;}
void ot2_host_die(const char* fmt,...) {
    va_list ap;va_start(ap,fmt);vfprintf(stderr,fmt,ap);va_end(ap);abort();
}
