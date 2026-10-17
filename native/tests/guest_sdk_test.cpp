#include <cstdio>
#include <exception>
#include <cstring>
extern "C" bool ot2_test_sdk_codec(const char*,const char*,bool);
int main(int argc,char** argv){try{if(argc!=3&&(argc!=4||strcmp(argv[3],"--time-only")))return 2;const bool codec=argc==3;if(!ot2_test_sdk_codec(argv[1],argv[2],codec))return 1;puts(codec?"PASS real SDK time and original Opus stereo/multistream decoder, reference PCM, malformed packet and guards":"PASS real SDK initialization and time: paused clock, libc ABI, explicit/default zone; codec not tested");return 0;}catch(const std::exception& e){fprintf(stderr,"FAIL SDK contracts: %s\n",e.what());return 1;}}
