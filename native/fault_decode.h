#pragma once
#include <cstdint>
#include <signal.h>

inline bool dirty_write_fault_candidate(int signal,int code,uint64_t esr){
    if(signal!=SIGSEGV||(code!=SEGV_ACCERR&&code!=SEGV_MAPERR))return false;
    const auto kind=(esr>>26)&63;
    // The OPD2201 kernel can report MAPERR for stores to a known mapped,
    // read-only page. Untouched anonymous pages have no PTE yet and report a
    // translation fault on both devices. This only selects a candidate: the
    // tracker must own the protection AND successfully restore a live mapping.
    // Never accept instruction aborts, reads, external aborts or MTE faults.
    const auto fault=esr&0x3c;
    return (kind==0x24||kind==0x25)&&(esr&(1u<<6))&&(fault==0x04||fault==0x0c);
}
