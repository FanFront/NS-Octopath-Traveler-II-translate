// Included only by the independent contract-test shared library. No test hook
// or synthetic packet is compiled into the application.
#include "opus_fixture.h"
extern "C" bool ot2_test_sdk_codec(const char* directory,const char* scratch,bool codec){
    std::lock_guard lock(setup_mutex);
    configure_services("",std::string(scratch)+"/userdata");
    auto* context=create_thread_context();install_handlers();
    address_space=static_cast<uint8_t*>(mmap(nullptr,reserve_size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));if(address_space==MAP_FAILED)return false;
    context->handle=svc_thread_main_init();thread_system_ready=true;
    LoadedGame game(directory);active_game=&game;images=game.modules();
    for(auto& module:images){image_size+=module.size;patch(module.base+module.text_offset,module.text_size);}
    install_service_initializers(game);game.make_executable();
    auto address_call=[&](uint64_t address,std::initializer_list<uint64_t> values){uint64_t args[8]{};if(values.size()>8||!address)throw std::runtime_error("SDK codec entry/ABI absent");std::copy(values.begin(),values.end(),args);return ot2_guest_call(address,context,args,0);};
    auto call=[&](const char* name,std::initializer_list<uint64_t> args={}){return address_call(game.symbol(name),args);};
    call("_init_libc0");call("nnosInitialize",{context->handle,0});call("_init_libc1");call("nninitInitializeSdkModule");call("nninitStartup");call("_init_libc2");
    bool valid=true;
    {
        // All original SDK counter wrappers and the SVC must share the same
        // paused clock. Host sleep still uses real time while it is frozen.
        freeze_guest_clock(1,true);
        const auto frozen=ticks();
        usleep(20000);
        bool clock_valid=true;
        for(const auto* name:{"nnosGetSystemTick","_ZN2nn2os13GetSystemTickEv","_ZN2nn2os20GetSystemTickOrderedEv"})
            clock_valid&=call(name)==frozen;
        ucontext_t tick_context{};svc(0x1e,&tick_context,context);
        clock_valid&=tick_context.uc_mcontext.regs[0]==frozen;
        clock_valid&=call("_ZN2nn2os22GetSystemTickFrequencyEv")==19200000;
        freeze_guest_clock(2,true);freeze_guest_clock(1,false);
        usleep(20000);clock_valid&=call("nnosGetSystemTick")==frozen;
        freeze_guest_clock(2,false);
        const auto resumed=call("nnosGetSystemTick");
        clock_valid&=resumed>=frozen&&resumed-frozen<192000; // No 40-ms jump.
        usleep(20000);clock_valid&=call("nnosGetSystemTick")>resumed;
        printf("SDK_CLOCK frozen=%llu resumed=%llu valid=%d\n",(unsigned long long)frozen,(unsigned long long)resumed,clock_valid);
        valid&=clock_valid;
    }
    {
        // Exercise the original SDK's libc exports as well as the nn::time
        // adapters through the real ARM64 guest/host register boundary.
        static_assert(sizeof(tm)==56&&offsetof(tm,tm_gmtoff)==40&&offsetof(tm,tm_zone)==48);
        time_t epoch=1704067200;tm expected{};localtime_r(&epoch,&expected);
        struct GuardedTm {tm value;uint64_t guard=0x123456789abcdef0;} actual{};
        const auto result=call("localtime_r",{reinterpret_cast<uint64_t>(&epoch),reinterpret_cast<uint64_t>(&actual.value)});
        auto same=[](const tm& a,const tm& b){return a.tm_year==b.tm_year&&a.tm_mon==b.tm_mon&&a.tm_mday==b.tm_mday&&a.tm_hour==b.tm_hour&&a.tm_min==b.tm_min&&a.tm_sec==b.tm_sec&&a.tm_wday==b.tm_wday&&a.tm_yday==b.tm_yday&&a.tm_isdst==b.tm_isdst&&a.tm_gmtoff==b.tm_gmtoff;};
        bool time_valid=result==reinterpret_cast<uint64_t>(&actual.value)&&same(actual.value,expected)&&actual.guard==0x123456789abcdef0;
        auto* static_tm=reinterpret_cast<tm*>(call("localtime",{reinterpret_cast<uint64_t>(&epoch)}));time_valid&=static_tm&&same(*static_tm,expected);
        auto normalized=expected;auto stamp=call("mktime",{reinterpret_cast<uint64_t>(&normalized)});time_valid&=int64_t(stamp)==epoch&&same(normalized,expected);
        const char format[]="%Y-%m-%d %H:%M:%S %Z %z";char expected_text[96]{};strftime(expected_text,sizeof(expected_text),format,&expected);std::array<char,104> formatted;formatted.fill(0x5a);
        auto length=call("strftime",{reinterpret_cast<uint64_t>(formatted.data()),96,reinterpret_cast<uint64_t>(format),reinterpret_cast<uint64_t>(&actual.value)});
        printf("SDK_TIME formatted=%s expected=%s\n",formatted.data(),expected_text);
        time_valid&=length==strlen(expected_text)&&!strcmp(formatted.data(),expected_text);for(size_t i=96;i<formatted.size();i++)time_valid&=formatted[i]==0x5a;
        printf("SDK_TIME local_hour=%d expected=%d offset=%ld expected_offset=%ld libc_valid=%d\n",actual.value.tm_hour,expected.tm_hour,actual.value.tm_gmtoff,expected.tm_gmtoff,time_valid);valid&=time_valid;
        struct Calendar {int16_t year;uint8_t month,day,hour,minute,second,pad;};
        struct Info {uint32_t weekday,yearday;char name[8];uint32_t dst;int32_t offset;};
        std::array<uint8_t,0x4000> rule{};std::array<char,36> location{};memcpy(location.data(),"America/New_York",17);
        time_valid=!call("_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE",{reinterpret_cast<uint64_t>(rule.data()),reinterpret_cast<uint64_t>(location.data())});
        struct {Calendar value;uint64_t guard=0x123456789abcdef0;} cal{};struct {Info value;uint64_t guard=0xfedcba9876543210;} info{};
        time_valid&=!call("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeERKNS0_12TimeZoneRuleE",{reinterpret_cast<uint64_t>(&cal.value),reinterpret_cast<uint64_t>(&info.value),reinterpret_cast<uint64_t>(&epoch),reinterpret_cast<uint64_t>(rule.data())});
        time_valid&=cal.value.year==2023&&cal.value.month==12&&cal.value.day==31&&cal.value.hour==19&&info.value.offset==-18000&&info.value.dst==0&&cal.guard==0x123456789abcdef0&&info.guard==0xfedcba9876543210;
        auto packed=call("_ZN2nn4time19ToCalendarTimeInUtcERKNS0_9PosixTimeE",{reinterpret_cast<uint64_t>(&epoch)});Calendar utc{};memcpy(&utc,&packed,8);time_valid&=utc.year==2024&&utc.month==1&&utc.day==1&&utc.hour==0;
        time_valid&=!call("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE",{reinterpret_cast<uint64_t>(&cal.value),reinterpret_cast<uint64_t>(&info.value),reinterpret_cast<uint64_t>(&epoch)});
        time_valid&=cal.value.hour==expected.tm_hour&&info.value.offset==expected.tm_gmtoff&&cal.guard==0x123456789abcdef0&&info.guard==0xfedcba9876543210;
        printf("SDK_TIME explicit_rule_valid=%d UTC_year=%d\n",time_valid,utc.year);valid&=time_valid;
    }
    if(!codec){active_game=nullptr;return valid;}
    for(bool multi:{false,true}){
        alignas(16) std::array<uint8_t,128> object;object.fill(0xa5);const auto self=reinterpret_cast<uint64_t>(object.data());
        call(multi?"_ZN2nn5codec30HardwareOpusMultiStreamDecoderC1Ei":"_ZN2nn5codec19HardwareOpusDecoderC1Ei",{self,0});
        auto* vtable=*reinterpret_cast<const uint64_t**>(object.data());
        const char* prefix=multi?"_ZN2nn5codec22OpusMultiStreamDecoder17DecodeInterleavedEPmPiPsmPKvm":"_ZN2nn5codec11OpusDecoder17DecodeInterleavedEPmPiPsmPKvm";
        const auto decode=game.symbol(prefix);bool in_vtable=false;for(unsigned i=0;i<(multi?12:10);i++)in_vtable|=vtable[i]==decode;valid&=in_vtable;
        auto size=multi?call("_ZNK2nn5codec22OpusMultiStreamDecoder17GetWorkBufferSizeEiiii",{self,48000,2,1,1}):call("_ZNK2nn5codec11OpusDecoder17GetWorkBufferSizeEii",{self,48000,2});
        if(!size||size>16*1024*1024)throw std::runtime_error("Invalid codec work buffer size");
        void* work=nullptr;if(posix_memalign(&work,4096,size+4096))throw std::bad_alloc();memset(work,0xa5,size+4096);const uint8_t mapping[]={0,1};
        auto initialized=multi?call("_ZN2nn5codec22OpusMultiStreamDecoder10InitializeEiiiiPKhPvm",{self,48000,2,1,1,reinterpret_cast<uint64_t>(mapping),reinterpret_cast<uint64_t>(work),size}):call("_ZN2nn5codec11OpusDecoder10InitializeEiiPvm",{self,48000,2,reinterpret_cast<uint64_t>(work),size});
        valid&=initialized==0;
        std::array<uint8_t,sizeof(opus_packet)+8> packet{};auto length=sizeof(opus_packet);for(int i=0;i<4;i++)packet[i]=uint8_t(length>>(24-i*8));memcpy(packet.data()+8,opus_packet,length);
        std::array<int16_t,1936> output;output.fill(12345);uint64_t consumed=0;int samples=0;
        auto status=address_call(decode,{self,reinterpret_cast<uint64_t>(&consumed),reinterpret_cast<uint64_t>(&samples),reinterpret_cast<uint64_t>(output.data()),1920*2,reinterpret_cast<uint64_t>(packet.data()),packet.size()});
        // The game's older decoder and libopus 1.6.1 need not be
        // bit-identical. Bound both RMS (2 LSB) and isolated error (8 LSB),
        // rather than accepting silence or merely checking the result code.
        int maximum_error=0;uint64_t squared_error=0;for(unsigned i=0;i<1920;i++){int error=int(output[i])-int(opus_expected[i]);maximum_error=std::max(maximum_error,abs(error));squared_error+=error*error;}
        valid&=status==0&&consumed==packet.size()&&samples==960&&maximum_error<=8&&squared_error<=1920*4;
        for(unsigned i=1920;i<output.size();i++)valid&=output[i]==12345;
        auto original=output;status=address_call(decode,{self,reinterpret_cast<uint64_t>(&consumed),reinterpret_cast<uint64_t>(&samples),reinterpret_cast<uint64_t>(output.data()),1920*2,reinterpret_cast<uint64_t>(packet.data()),7});valid&=status!=0&&output==original;
        call(multi?"_ZN2nn5codec22OpusMultiStreamDecoder8FinalizeEv":"_ZN2nn5codec11OpusDecoder8FinalizeEv",{self});
        for(unsigned i=multi?0x38:0x30;i<object.size();i++)valid&=object[i]==0xa5;
        for(unsigned i=0;i<4096;i++)valid&=static_cast<uint8_t*>(work)[size+i]==0xa5;
        free(work);printf("SDK_OPUS multi=%d samples=%d max_reference_error=%d squared_error=%llu valid=%d\n",multi,samples,maximum_error,(unsigned long long)squared_error,valid);
    }
    active_game=nullptr;return valid;
}
