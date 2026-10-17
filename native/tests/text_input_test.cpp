#include "../text_input.h"
#include <array>
#include <cstring>
#include <future>
#include <stdexcept>
#include <cstdio>
#include <thread>
static void require(bool yes,const char* why){if(!yes)throw std::runtime_error(why);}
int main(){try{
    std::array<uint8_t,0x500> arg{};std::array<uint8_t,32> result;result.fill(0xa5);
    struct String {uint8_t* data;uint64_t capacity;} output{result.data(),16};uint64_t registers[]={reinterpret_cast<uint64_t>(&output),reinterpret_cast<uint64_t>(arg.data())};
    int maximum=10,minimum=2;memcpy(arg.data()+0x3ac,&maximum,4);memcpy(arg.data()+0x3b0,&minimum,4);arg[0x3bd]=1;
    require(ot2::show_keyboard(registers)==0x29f&&result[0]==0xa5,"No window cancels without modifying the result");
    ot2::set_text_input_available(true);
    auto run=[&](const std::u16string& text,bool accept){
        auto worker=std::async(std::launch::async,[&]{return ot2::show_keyboard(registers);});std::optional<ot2::TextInputRequest> pending;
        auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);while(!(pending=ot2::pending_text_input())&&std::chrono::steady_clock::now()<until)std::this_thread::yield();
        require(bool(pending),"Keyboard request published");require(!ot2::complete_text_input(pending->id+1,true,text),"Stale UI result ignored");
        require(!ot2::complete_text_input(pending->id,true,u"x"),"Minimum UTF-16 length enforced");
        require(ot2::complete_text_input(pending->id,accept,text),"Keyboard response accepted");return worker.get();
    };
    require(run(u"旅人😀",true)==0,"UTF-8 confirm");require(!strcmp(reinterpret_cast<char*>(result.data()),"旅人😀")&&result[16]==0xa5,"UTF-8 supplementary character and output guard");
    auto original=result;require(run(u"取消",false)==0x29f&&result==original,"Cancel preserves original output");
    arg[0x3bd]=0;output.capacity=7;result.fill(0xa5);require(run(u"旅😀",true)==0,"UTF-16 confirm");
    char16_t first;memcpy(&first,result.data(),2);require(first==u'旅'&&result[2]==0&&result[3]==0&&result[7]==0xa5,"Odd UTF-16 capacity never splits surrogate or overwrites guard");
    auto worker=std::async(std::launch::async,[&]{return ot2::show_keyboard(registers);});while(!ot2::pending_text_input())std::this_thread::yield();ot2::set_text_input_available(false);
    require(worker.get()==0x29f,"Losing the activity cancels and unblocks guest thread");
    puts("PASS keyboard SDK protocol: Unicode, bounds, length, cancellation, stale replies, activity loss");return 0;
}catch(const std::exception& e){fprintf(stderr,"FAIL keyboard: %s\n",e.what());return 1;}}
