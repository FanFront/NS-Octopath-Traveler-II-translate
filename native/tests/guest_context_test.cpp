#include <cstdio>
#include <thread>
#include <stdexcept>
extern "C" bool ot2_test_context_fresh();
extern "C" size_t ot2_test_context_live();
extern "C" size_t ot2_test_context_bytes();
extern "C" bool ot2_test_context_exit();
int main(){try{
    const auto baseline=ot2_test_context_live();
    const auto bytes=ot2_test_context_bytes();
    for(int i=0;i<400;i++){
        bool fresh=false;std::thread worker([&]{fresh=ot2_test_context_fresh();});worker.join();
        if(!fresh)throw std::runtime_error("a reused pthread must not inherit prior guest TLS pointers");
        if(ot2_test_context_live()!=baseline)throw std::runtime_error("exited guest thread must release its execution context");
        if(ot2_test_context_bytes()!=bytes)throw std::runtime_error("exited guest thread must release its original TLS and stacks");
    }
    puts("PASS 400 guest contexts: fresh TLS after pointer overwrite, exit reclamation, slot reuse");
    if(!ot2_test_context_exit())throw std::runtime_error("real guest ExitThread and nested signal context reclamation");
    puts("PASS 400 real guest ExitThread calls: nested signal/callback exit, stack unmap after completion, TLS overwrite, slot reuse");return 0;
}catch(const std::exception& e){fprintf(stderr,"FAIL guest context: %s\n",e.what());return 1;}}
