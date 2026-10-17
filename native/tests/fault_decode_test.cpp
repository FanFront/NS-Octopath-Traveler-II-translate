#include "../fault_decode.h"
#include <cstdio>
#include <stdexcept>
int main(){
    auto check=[](bool ok){if(!ok)throw std::runtime_error("ARM64 dirty write fault classification");};
    check(dirty_write_fault_candidate(SIGSEGV,SEGV_ACCERR,0x9200004f));
    check(dirty_write_fault_candidate(SIGSEGV,SEGV_MAPERR,0x9200004f));
    check(dirty_write_fault_candidate(SIGSEGV,SEGV_MAPERR,0x92000047)); // lazy anonymous page; tracker verifies live mapping
    check(dirty_write_fault_candidate(SIGSEGV,SEGV_ACCERR,0x92000047));
    check(!dirty_write_fault_candidate(SIGSEGV,SEGV_ACCERR,0x92000050)); // external abort
    check(!dirty_write_fault_candidate(SIGSEGV,SEGV_ACCERR,0x9200000f)); // read
    check(!dirty_write_fault_candidate(SIGSEGV,SEGV_ACCERR,0x8200000f)); // execute
    check(!dirty_write_fault_candidate(SIGSEGV,SI_USER,0x9200004f));
    check(!dirty_write_fault_candidate(SIGBUS,SEGV_ACCERR,0x9200004f));
    check(!dirty_write_fault_candidate(SIGSEGV,9,0x9200004f)); // synchronous MTE
    puts("PASS ARM64 write permission/translation ESR, vendor MAPERR, and external/read/execute/user/MTE negative cases");
}
