#include <cstdio>
#include <exception>
extern "C" bool ot2_test_native_gates();
int main(){try{return ot2_test_native_gates()?0:1;}catch(const std::exception& e){fprintf(stderr,"FAIL native gates: %s\n",e.what());return 1;}}
