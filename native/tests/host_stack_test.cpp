#include <cstdio>
#include <exception>
extern "C" bool ot2_test_host_stack();
int main(){try{return ot2_test_host_stack()?0:1;}catch(const std::exception& e){fprintf(stderr,"FAIL host stack: %s\n",e.what());return 1;}}
