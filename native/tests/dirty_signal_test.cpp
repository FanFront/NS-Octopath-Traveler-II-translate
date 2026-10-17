#include "../guest.h"
#include "../graphics/backend.h"
#include "../graphics/nvn_dirty.h"
#include <cstdio>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
int main(){
    ot2::execution_probe();
    // Real negative controls must still terminate, rather than returning from
    // the handler and repeatedly retrying a genuinely invalid access.
    for(unsigned mode=0;mode<5;mode++){
        auto child=fork();if(child<0)return 2;
        if(!child){
            alarm(5);size_t page=sysconf(_SC_PAGESIZE);auto* p=static_cast<uint8_t*>(mmap(nullptr,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
            if(p==MAP_FAILED)_exit(3);if(mode!=3){if(!nvn_dirty_add_range(p,page)||!nvn_dirty_protect(p,page))_exit(3);}else if(mprotect(p,page,PROT_READ))_exit(3);
            if(mode==0){munmap(p,page);*static_cast<volatile uint8_t*>(p)=1;}
            if(mode==1)reinterpret_cast<void(*)()>(p)();
            if(mode==2){mprotect(p,page,PROT_NONE);auto value=*static_cast<volatile uint8_t*>(p);(void)value;}
            if(mode==3)*static_cast<volatile uint8_t*>(p)=1;
            if(mode==4){*static_cast<volatile uint8_t*>(p)=1;munmap(p,page);*static_cast<volatile uint8_t*>(p)=2;}
            _exit(2);
        }
        int status{};if(waitpid(child,&status,0)!=child||!WIFEXITED(status)||WEXITSTATUS(status)!=71)return 4;
    }
    puts("PASS actual handler rejects unmapped/execute/read/untracked/stale-unprotected writes");
    vkhost_init();
    if(!vkhost_probe_texture_coherence())return 1;
    puts("PASS actual guest signal handler and real GPU texture write coherence");
}
