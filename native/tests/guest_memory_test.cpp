#include <array>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../host_services.h"
extern "C" bool ot2_test_memory_init(uint64_t*);
extern "C" void ot2_test_memory_svc(unsigned,uint64_t*);
extern "C" bool ot2_test_memory_page_size(size_t);
extern "C" uintptr_t ot2_test_memory_module();
extern "C" int ot2_test_memory_permission(uintptr_t);
extern "C" uint64_t ot2_test_memory_permission_scans();
namespace {
constexpr uint64_t page=4096,unit=2*1024*1024;
constexpr uint64_t invalid_size=0xca01,invalid_address=0xcc01,invalid_memory=0xd401,invalid_region=0xdc01;
uint64_t regions[6],heap,stack,alias,host_page;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
std::array<uint64_t,31> call(unsigned id,uint64_t a=0,uint64_t b=0,uint64_t c=0,uint64_t d=0){
    std::array<uint64_t,31> r{};r[0]=a;r[1]=b;r[2]=c;r[3]=d;ot2_test_memory_svc(id,r.data());return r;
}
struct MemoryInfo {uint64_t base,size;uint32_t state,attribute,permission,ipc,device,padding;};
MemoryInfo query(uint64_t address){
    struct {uint64_t before=0xa55aa55a;MemoryInfo value{};uint64_t after=0x5aa55aa5;} guarded;
    auto result=call(6,reinterpret_cast<uint64_t>(&guarded.value),0,address);
    require(result[0]==0&&result[1]==0,"QueryMemory status and page flags");
    require(guarded.before==0xa55aa55a&&guarded.after==0x5aa55aa5,"QueryMemory 40-byte output guards");
    for(unsigned i=0;i<2;++i)require(ot2_test_memory_permission(address)==int(guarded.value.permission),"cached permission agrees after map/unmap/borrow/restore");
    return guarded.value;
}
uint8_t& byte(uint64_t address){return *reinterpret_cast<uint8_t*>(address);}
bool inaccessible(uint64_t address){
    pid_t child=fork();require(child>=0,"fork access probe");
    if(!child){
        struct sigaction action{};action.sa_handler=[](int){_exit(77);};sigemptyset(&action.sa_mask);
        sigaction(SIGSEGV,&action,nullptr);sigaction(SIGBUS,&action,nullptr);
        volatile uint8_t value=*reinterpret_cast<volatile uint8_t*>(address);(void)value;_exit(0);
    }
    int status=0;require(waitpid(child,&status,0)==child,"wait access probe");return WIFEXITED(status)&&WEXITSTATUS(status)==77;
}
void heap_contract(){
    require(call(1,0,2*unit)[0]==0,"commit initial heap");byte(heap+3*unit/2)=0x7d;byte(heap+page)=0x39;
    auto scans=ot2_test_memory_permission_scans();
    for(unsigned i=0;i<1000;++i)require(ot2_test_memory_permission(heap+3*unit/2)==3,"stable mapped permission");
    require(ot2_test_memory_permission_scans()-scans<=1,"unchanged memory registry must not be rescanned for every frame setting");
    // FrameRatePatch resolves both ends of each 4/8-byte guest field. These
    // neighboring exact addresses must coexist, even when their hash collides.
    scans=ot2_test_memory_permission_scans();
    for(unsigned frame=0;frame<1000;++frame)for(unsigned field=0;field<10;++field){
        const auto address=heap+page+64*field;
        require(ot2_test_memory_permission(address)==3&&ot2_test_memory_permission(address+7)==3,"field endpoint permissions");
    }
    require(ot2_test_memory_permission_scans()-scans<=20,"repeated field endpoints must stay cached together");
    require(call(1,0,unit)[0]==0,"shrink heap");
    require(query(heap+3*unit/2).state==0,"shrunk heap queries as free");
    require(inaccessible(heap+3*unit/2),"shrunk heap must actually revoke host access");
    require(call(1,0,2*unit)[0]==0,"regrow heap");
    require(byte(heap+3*unit/2)==0&&byte(heap+page)==0x39,"regrow zeroes released pages and retains live bytes");
    require(call(1,0,unit+page)[0]==invalid_size,"heap size uses 2 MiB granularity");
    require(query(heap+3*unit/2).permission==3,"failed resize keeps old mapping");
    require(call(1,0,UINT64_MAX-(unit-1))[0]!=0,"overflowing heap size rejected");
    require(call(1,0,0)[0]==0&&inaccessible(heap+page),"release full heap");
}
void info_contract(){
    constexpr uint64_t self=0xffff8001;
    auto alias_info=call(0x29,0,3,self,0),extra=call(0x29,0,28,self,0);
    require(alias_info[0]==0&&alias_info[1]==regions[3]&&extra[0]==0&&extra[1]==0,"GetInfo reports actual fixed alias layout without optional extension");
    require(call(0x29,0,28,self,1)[0]==0xf001,"GetInfo invalid sub-id is a guest error");
    require(call(0x29,0,28,7,0)[0]==0xe401,"GetInfo rejects unsupported process handle");
    constexpr uint64_t other=0x0100ad401a2f0000ULL;ot2::configure_services("","",other);
    require(call(0x29,0,18,self,0)[1]==other,"GetInfo ProgramId follows launch identity");
}
void stack_contract(){
    require(call(0x2c,alias,8*page)[0]==0,"allocate stack source pages");
    for(unsigned i=0;i<8;i++)byte(alias+i*page)=0x30+i;
    require(call(4,stack+page,alias+page,2*page)[0]==0,"map stack from physical pool");
    auto src=query(alias+page),before=query(alias),after=query(alias+3*page),dst=query(stack+page);
    require(src.state==5&&src.attribute==1&&src.permission==0&&src.base==alias+page&&src.size==2*page,"borrowed stack source reports exact range, no permission and borrowed attribute");
    require(before.base==alias&&before.size==page&&after.base==alias+3*page&&after.permission==3,"source query splits around the borrowed range");
    require(dst.state==0xb&&dst.permission==3&&byte(stack+page)==0x31,"stack destination contains source bytes");
    if(host_page==page)require(inaccessible(alias+page),"borrowed source inaccessible while mapped");
    require(call(4,stack+4*page,alias+page,page)[0]==invalid_memory,"reject a second borrower before reading inaccessible source");
    require(call(0x2d,alias,8*page)[0]==invalid_memory,"cannot free physical pages backing a live stack");
    require(call(5,stack+page,alias+2*page,2*page)[0]==invalid_memory,"mismatched unmap rejected without changing ownership");
    byte(stack+page)=0xa1;byte(stack+2*page)=0xa2;
    require(call(5,stack+page,alias+page,2*page)[0]==0,"unmap and restore source");
    require(byte(alias+page)==0xa1&&byte(alias+2*page)==0xa2&&query(alias+page).attribute==0,"stack writes return to source");
    require(query(stack+page).state==0&&inaccessible(stack+page),"unmapped stack inaccessible");
    require(call(1,0,unit)[0]==0,"heap source commit");
    byte(heap)=0xc1;require(call(4,stack,heap,page)[0]==0,"stack can borrow heap source");
    require(call(1,0,0)[0]==invalid_memory,"heap shrink cannot free borrowed source");
    require(call(5,stack,heap,page)[0]==0&&byte(heap)==0xc1,"heap source restore");
    // These addresses share one 16 KiB host page. Releasing one 4 KiB guest
    // mapping must not damage its live neighbor (on current 4 KiB device too).
    require(call(4,stack,alias,page)[0]==0&&call(4,stack+page,alias+page,page)[0]==0,"adjacent guest stack mappings");
    byte(stack+page)=0xe2;
    require(call(5,stack,alias,page)[0]==0&&byte(stack+page)==0xe2,"unmap preserves adjacent destination");
    require(call(5,stack+page,alias+page,page)[0]==0&&byte(alias+page)==0xe2,"adjacent source restored");
    const auto aligned_source=(alias+host_page-1)&~(host_page-1),aligned_stack=(stack+host_page-1)&~(host_page-1);
    require(call(4,aligned_stack,aligned_source,host_page)[0]==0,"borrow complete host page");
    require(inaccessible(aligned_source),"complete borrowed host page is inaccessible with either host granularity");
    byte(aligned_stack)=0x83;require(call(5,aligned_stack,aligned_source,host_page)[0]==0&&byte(aligned_source)==0x83,"complete host page restoration");
}
void physical_contract(){
    require(call(0x2c,alias,3*page)[0]==0,"physical allocation");
    byte(alias)=0x11;byte(alias+page)=0x22;byte(alias+2*page)=0x33;
    require(call(0x2d,alias+page,page)[0]==0,"partial physical unmap");
    require(query(alias+page).state==0,"partial unmap publishes hole");
    if(host_page==page)require(inaccessible(alias+page),"independent host page loses access after partial unmap");
    require(byte(alias)==0x11&&byte(alias+2*page)==0x33,"partial unmap preserves both neighbors");
    require(call(0x2c,alias,4*page)[0]==0,"mapping existing pages plus holes preserves existing storage");
    require(byte(alias)==0x11&&byte(alias+page)==0&&byte(alias+2*page)==0x33&&byte(alias+3*page)==0,"only newly committed physical pages are zeroed");
    require(call(0x2d,alias,4*page)[0]==0&&query(alias+page).state==0,"unmap combined physical ranges");
    require(call(0x2d,alias,4*page)[0]==0,"unmapping already free physical pages is harmless");
    // Independent calls race on adjacent and overlapping ranges. A resulting
    // extent must be valid and the remaining half must survive partial free.
    std::atomic<unsigned> failures{};std::thread first([&]{for(int i=0;i<100;i++)if(call(0x2c,alias,2*page)[0])++failures;});
    std::thread second([&]{for(int i=0;i<100;i++)if(call(0x2c,alias+page,2*page)[0])++failures;});first.join();second.join();
    require(!failures&&query(alias+2*page).permission==3,"concurrent overlapping physical commits serialize");
    byte(alias+2*page)=0x89;require(call(0x2d,alias,2*page)[0]==0&&byte(alias+2*page)==0x89,"concurrent commit result splits correctly");
}
void invalid_contract(){
    require(call(0x2c,alias,page)[0]==0,"valid source for negative requests");byte(alias)=0x52;
    require(call(4,stack,alias,0)[0]==invalid_size,"zero-length stack mapping is invalid size");
    require(call(4,stack+1,alias,page)[0]==invalid_address,"unaligned stack address is invalid address");
    require(call(4,stack,alias,page+1)[0]==invalid_size,"unaligned stack size is invalid size");
    require(call(4,stack,alias+page,page)[0]==invalid_memory,"uncommitted source rejected before copying");
    require(call(4,stack,alias,2*page)[0]==invalid_memory,"source spanning a committed page and a hole is rejected");
    require(call(4,alias+page,alias,page)[0]==invalid_region,"stack destination must lie in stack reservation");
    require(call(4,stack,UINT64_MAX-(page-1),2*page)[0]==invalid_memory,"overflowing source rejected");
    require(call(0x2c,alias,0)[0]==invalid_size,"zero physical size rejected");
    require(call(0x2c,alias+1,page)[0]==invalid_address,"physical address alignment");
    require(call(0x2c,alias,page+1)[0]==invalid_size,"physical size alignment");
    require(call(0x2c,stack,page)[0]==invalid_region,"physical allocation restricted to alias region");
    require(call(0x2d,UINT64_MAX-(page-1),2*page)[0]==invalid_region,"physical overflow rejected");
    require(byte(alias)==0x52&&query(alias).permission==3,"invalid requests preserve prior data and state");
    auto beyond=query(UINT64_MAX);require(beyond.state==0x10&&beyond.permission==0,"outside 39-bit guest VA reports reserved instead of aborting");
}
void transfer_contract(){
    require(host_page==page,"transfer contract currently requires a native 4 KiB device");
    require(call(0x2c,alias,page)[0]==0&&call(0x2c,alias+page,page)[0]==0,"adjacent transfer source allocations");
    byte(alias)=0x41;byte(alias+page)=0x42;
    require(call(4,stack,alias,page)[0]==0,"borrow source as stack");
    require(call(0x15,0,alias,page,3)[0]==invalid_memory,"transfer cannot borrow live stack source");
    require(call(5,stack,alias,page)[0]==0,"restore stack source");
    auto created=call(0x15,0,alias,2*page,0);require(!created[0],"transfer spans adjacent committed ranges");const auto h=created[1];
    auto src=query(alias);require(src.attribute==1&&src.permission==0&&inaccessible(alias),"isolated transfer source is borrowed and inaccessible");
    require(call(0x2d,alias,2*page)[0]==invalid_memory,"cannot free live transfer source");
    require(call(4,stack,alias,page)[0]==invalid_memory,"stack cannot borrow live transfer source");
    require(call(1,0,unit)[0]==0,"heap for occupied-destination check");byte(heap)=0x29;
    require(call(0x51,h,heap,2*page,0)[0]==invalid_region&&byte(heap)==0x29,"transfer cannot overwrite committed heap");
    require(call(0x51,h,heap+2*unit,2*page,0)[0]==invalid_region,"transfer cannot occupy uncommitted heap reservation");
    require(call(0x51,h,alias+16*page,2*page,0)[0]==invalid_region,"transfer cannot occupy physical allocator reservation");
    const auto destination=heap+regions[4]+unit;
    require(call(0x51,h,destination,page,0)[0]==invalid_size,"transfer mapped size must match object");
    require(call(0x51,h,destination,2*page,3)[0]==0xfa01,"transfer owner permissions must match");
    require(call(0x51,h,destination,2*page,0)[0]==0,"map isolated transfer");
    require(call(0x51,h,destination+4*page,2*page,0)[0]==0xfa01,"transfer object may have only one mapped destination");
    require(query(destination).state==0xd&&byte(destination)==0x41&&byte(destination+page)==0x42,"isolated transfer view preserves both source pages");
    byte(destination)=0x67;
    require(call(0x52,h,destination+4*page,2*page)[0]==invalid_memory&&byte(heap)==0x29&&byte(destination)==0x67,"wrong unmap leaves heap and valid transfer unchanged");
    require(call(0x52,h,destination,2*page)[0]==0&&inaccessible(destination),"unmap transfer destination");
    require(query(alias).attribute==1,"unmap leaves source borrowed while handle is open");
    require(call(0x16,h)[0]==0&&query(alias).attribute==0&&byte(alias)==0x67,"last transfer reference restores source permissions and bytes");
    // A closed handle may be recycled while its old mapped object remains
    // alive. The mapping's identity must not be that reusable handle number.
    auto first=call(0x15,0,alias,page,3);require(!first[0],"transfer for close-before-unmap");
    require(call(0x51,first[1],destination,page,3)[0]==0&&call(0x16,first[1])[0]==0,"mapped transfer retains owner after handle close");
    auto second=call(0x15,0,alias+page,page,3);require(!second[0]&&second[1]==first[1],"exercise recycled transfer handle");
    require(call(0x52,second[1],destination,page)[0]==invalid_memory&&byte(destination)==0x67,"recycled handle cannot unmap prior object's view");
    require(call(0x16,second[1])[0]==0&&query(alias+page).attribute==0,"new object closes independently of old retained mapping");
    require(query(alias).attribute==1&&call(0x2d,alias,page)[0]==invalid_memory,"old mapped object keeps its source pinned");
}
void module_contract(){
    const auto module=ot2_test_memory_module();require(module!=0,"synthetic module with RX/R/RW segments");
    const auto data=module+2*host_page;byte(data)=0xa4;
    require(call(4,stack,data,host_page)[0]==0,"SDK can borrow a static thread stack from module .data/.bss");
    require(query(data).state==4&&query(data).attribute==1&&query(data).permission==0,"borrowed module source preserves CodeMutable state");
    require(inaccessible(data)&&byte(stack)==0xa4,"module stack source inaccessible and destination copied");
    byte(stack)=0x76;require(call(5,stack,data,host_page)[0]==0&&byte(data)==0x76,"module-backed stack writes restored");
    auto text=query(module),ro=query(module+host_page),rw=query(data);
    require(text.state==3&&text.permission==5&&text.size==host_page,"QueryMemory reports executable module segment");
    require(ro.state==3&&ro.permission==1&&ro.size==host_page,"QueryMemory reports read-only module segment");
    require(rw.state==4&&rw.permission==3&&rw.attribute==0,"QueryMemory reports writable module segment");
    require(call(4,stack,module,host_page)[0]==invalid_memory&&call(4,stack,module+host_page,host_page)[0]==invalid_memory,"executable and read-only module segments cannot be borrowed as writable stacks");
    require(call(4,stack,module+host_page,2*host_page)[0]==invalid_memory,"read-only to writable crossing is rejected before mutation");
    require(query(stack).state==0&&byte(data)==0x76,"failed module source validation preserves destination and data");
}
}
int main(int argc,char** argv){try{
    require(argc==2||argc==3,"select heap, stack, physical or invalid contract");require(ot2_test_memory_init(regions),"reserve isolated guest VA fixture");
    host_page=argc==3?std::stoul(argv[2]):sysconf(_SC_PAGESIZE);require(ot2_test_memory_page_size(host_page),"host page policy must cover actual host pages");
    heap=regions[0];stack=regions[1];alias=regions[2];
    if(!strcmp(argv[1],"info"))info_contract();else if(!strcmp(argv[1],"heap"))heap_contract();else if(!strcmp(argv[1],"stack"))stack_contract();
    else if(!strcmp(argv[1],"physical"))physical_contract();else if(!strcmp(argv[1],"invalid"))invalid_contract();else if(!strcmp(argv[1],"transfer"))transfer_contract();else if(!strcmp(argv[1],"module"))module_contract();else return 2;
    printf("PASS actual guest memory SVC: %s, native_page=%ld policy_page=%llu\n",argv[1],sysconf(_SC_PAGESIZE),(unsigned long long)host_page);return 0;
}catch(const std::exception& e){fprintf(stderr,"FAIL actual guest memory SVC: %s: %s\n",argc>1?argv[1]:"?",e.what());return 1;}}
