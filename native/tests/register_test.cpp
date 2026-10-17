#include <cstdio>
#include <exception>
extern "C" bool ot2_test_register_contract();
extern "C" bool ot2_test_tls_contract();
int main(){try{return ot2_test_register_contract()&&ot2_test_tls_contract()?0:1;}catch(const std::exception& e){fprintf(stderr,"FAIL register contract: %s\n",e.what());return 1;}}
