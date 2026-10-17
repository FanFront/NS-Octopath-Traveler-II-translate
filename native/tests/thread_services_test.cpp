#include "../thread_services.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <sys/mman.h>
extern "C" size_t ot2_thread_test_objects();
extern "C" size_t ot2_thread_test_waiters(int,uint64_t);
using namespace std::chrono_literals;
static void require(bool b,const char* why){if(!b)throw std::runtime_error(why);}
static std::array<uint64_t,32> svc(int id,std::initializer_list<uint64_t> args){std::array<uint64_t,32> r{};std::copy(args.begin(),args.end(),r.begin());require(svc_thread(id,r.data())==1,"SVC handled");return r;}
template<class T>static uint64_t address(T* p){return reinterpret_cast<uint64_t>(p);}
template<class F>static void until(F f){auto end=std::chrono::steady_clock::now()+2s;while(!f()&&std::chrono::steady_clock::now()<end)std::this_thread::yield();require(f(),"thread test deadline");}
static std::atomic<uint32_t> gate{0};static std::atomic<unsigned> finished{0};static unsigned order[8];
static void worker(uint64_t index){require(svc(0x34,{address(&gate),2,0,uint64_t(-1)})[0]==0,"address wake");order[finished.fetch_add(1)]=index;}
static uint32_t create(unsigned index,int priority){auto r=svc(0x08,{0,address(worker),index,0,uint64_t(priority),0});require(!r[0],"create thread");auto h=uint32_t(r[1]);require(!svc(0x09,{h})[0],"start thread");return h;}
int main(){try{
    auto main=svc_thread_main_init();require(svc_thread_main_init()==main,"main registration is idempotent");auto baseline=ot2_thread_test_objects();
    const int priority[]={50,20,20,40};std::array<uint32_t,4> handles{};
    for(unsigned i=0;i<4;i++){handles[i]=create(i,priority[i]);until([&]{return ot2_thread_test_waiters(3,address(&gate))==i+1;});}
    const unsigned expected[]={1,2,3,0};for(unsigned i=0;i<4;i++){
        require(!svc(0x35,{address(&gate),0,0,1})[0],"signal one address waiter");until([&]{return finished.load()==i+1;});
        auto h=handles[expected[i]];require(!svc(0x18,{0,address(&h),1,2000000000})[0],"join signaled thread");require(order[i]==expected[i],"priority then FIFO wake order");require(!svc(0x16,{h})[0],"close completed thread");
    }
    until([&]{return ot2_thread_test_objects()==baseline;});
    finished=0;auto early=create(7,44);until([&]{return ot2_thread_test_waiters(3,address(&gate))==1;});svc(0x16,{early});
    require(ot2_thread_test_objects()==baseline+1,"closing a live thread handle retains the execution object");svc(0x35,{address(&gate),0,0,1});until([&]{return ot2_thread_test_objects()==baseline;});
    uint32_t lock=main,key=0;auto timeout=svc(0x1c,{address(&lock),address(&key),main,1000000});require(timeout[0]==0xea01&&lock==0,"condition timeout returns unlocked for SDK reacquisition");svc(0x1d,{address(&key),0});require(key==0,"empty condition signal clears waiter flag");
    auto event=svc(0x45,{});auto write=uint32_t(event[1]),read=uint32_t(event[2]);require(!event[0],"event create");
    require(svc(0x18,{0,address(&read),1,0})[0]==0xea01,"unsignaled event times out");require(svc(0x11,{read})[0]==0xe401,"read handle cannot signal");
    require(!svc(0x11,{write})[0]&&!svc(0x18,{0,address(&read),1,0})[0],"event signals waiters");svc(0x16,{write});require(!svc(0x17,{read})[0]&&svc(0x17,{read})[0]==0xfa01,"read end survives write-end close and resets once");svc(0x16,{read});
    auto* source=mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);auto* alias=mmap(nullptr,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);require(source!=MAP_FAILED&&alias!=MAP_FAILED,"transfer mappings");
    *static_cast<uint32_t*>(source)=0x12345678;auto transfer=svc(0x15,{0,address(source),4096,3});require(!transfer[0],"transfer create");auto h=transfer[1];require(!svc(0x51,{h,address(alias),4096,3})[0],"transfer map");
    require(*static_cast<uint32_t*>(alias)==0x12345678,"transfer initial bytes");*static_cast<uint32_t*>(alias)=0xabcdef;require(*static_cast<uint32_t*>(source)==0xabcdef,"transfer shares real backing");
    require(!svc(0x52,{h,address(alias),4096})[0],"transfer unmap");svc(0x16,{h});munmap(source,4096);munmap(alias,4096);
    puts("PASS thread services: priority/FIFO, close/exit lifetime, monotonic timeout, event endpoints, shared transfer memory");return 0;
}catch(const std::exception& e){fprintf(stderr,"FAIL thread service: %s\n",e.what());return 1;}}
