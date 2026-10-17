#include "time_zone.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <time.h>
#include <vector>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace ot2 { namespace {
constexpr uint32_t Invalid=116+(900<<9),InvalidPointer=116+(901<<9),Overflow=116+(201<<9);
constexpr size_t RuleSize=0x4000,HeaderSize=32;
constexpr char Magic[8]={'O','T','2','T','Z','0','1',0};
using Bytes=std::span<const uint8_t>;
uint32_t be32(const uint8_t* p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
int64_t signed_be(const uint8_t* p,size_t n){uint64_t v=0;for(size_t i=0;i<n;i++)v=v<<8|p[i];return n==4?std::bit_cast<int32_t>(uint32_t(v)):std::bit_cast<int64_t>(v);}
uint32_t checksum(Bytes b){uint32_t v=2166136261u;for(auto c:b)v=(v^c)*16777619u;return v;}
bool alpha(char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z');}
bool digit(char c){return c>='0'&&c<='9';}
bool valid_location(std::string_view s){
    if(s.empty()||s.size()>=36||s.front()=='/'||s.back()=='/')return false;
    for(char c:s)if(!alpha(c)&&!digit(c)&&c!='/'&&c!='_'&&c!='-'&&c!='+'&&c!='.')return false;
    for(size_t i=0;i<s.size();){auto end=s.find('/',i);if(end==s.npos)end=s.size();auto part=s.substr(i,end-i);if(part.empty()||part=="."||part=="..")return false;i=end+1;}
    return true;
}
struct Type {int32_t offset;bool dst;std::string name;};
struct DateRule {char kind=0;int a=0,b=0,c=0,seconds=7200;};
struct Future {bool present=false,dst=false;Type standard{},summer{};DateRule start,end;};
struct Zone {std::vector<int64_t> transitions;std::vector<uint8_t> indices;std::vector<Type> types;Future future;};
struct Parser {
    std::string_view s;size_t i=0;
    bool take(char c){if(i<s.size()&&s[i]==c){i++;return true;}return false;}
    bool number(int& out,int max){size_t start=i;out=0;while(i<s.size()&&digit(s[i])){int d=s[i++]-'0';if(out>(max-d)/10)return false;out=out*10+d;if(out>max)return false;}return i>start;}
    bool name(std::string& out){size_t start=i;if(take('<')){start=i;while(i<s.size()&&(alpha(s[i])||digit(s[i])||s[i]=='+'||s[i]=='-'))i++;out=s.substr(start,i-start);return out.size()>=3&&take('>');}while(i<s.size()&&alpha(s[i]))i++;out=s.substr(start,i-start);return out.size()>=3;}
    bool seconds(int& out,int maxhour){int sign=1;if(take('-'))sign=-1;else take('+');int h,m=0,s2=0;if(!number(h,maxhour))return false;if(take(':')){if(!number(m,59))return false;if(take(':')&&!number(s2,59))return false;}out=sign*(h*3600+m*60+s2);return true;}
    bool date(DateRule& r){
        if(take('M')){r.kind='M';if(!number(r.a,12)||r.a<1||!take('.')||!number(r.b,5)||r.b<1||!take('.')||!number(r.c,6))return false;}
        else if(take('J')){r.kind='J';if(!number(r.a,365)||r.a<1)return false;}
        else {r.kind='N';if(!number(r.a,365))return false;}
        return !take('/')||seconds(r.seconds,167);
    }
};
bool parse_future(std::string_view text,Future& f){
    if(text.empty())return true;
    Parser p{text};int offset=0;if(!p.name(f.standard.name)||!p.seconds(offset,24))return false;
    f.present=true;f.standard.offset=-offset;f.standard.dst=false;
    if(p.i==text.size())return true;
    f.dst=true;f.summer.dst=true;if(!p.name(f.summer.name))return false;
    f.summer.offset=f.standard.offset+3600;
    if(p.i<text.size()&&text[p.i]!=','){if(!p.seconds(offset,24))return false;f.summer.offset=-offset;}
    // TZif footers with DST carry explicit rules; no process-global posixrules.
    return p.take(',')&&p.date(f.start)&&p.take(',')&&p.date(f.end)&&p.i==text.size();
}
struct Header {uint32_t utc,standard,leaps,times,types,chars;char version;size_t bytes;};
bool header(Bytes b,size_t pos,size_t width,Header& h){
    if(pos>b.size()||b.size()-pos<44||memcmp(b.data()+pos,"TZif",4))return false;
    auto p=b.data()+pos;h={be32(p+20),be32(p+24),be32(p+28),be32(p+32),be32(p+36),be32(p+40),char(p[4]),0};
    if(h.version!=0&&h.version!='2'&&h.version!='3'&&h.version!='4')return false;
    if(!h.types||h.types>256||!h.chars||h.chars>65536||h.times>65536||h.leaps>65536||(h.utc&&h.utc!=h.types)||(h.standard&&h.standard!=h.types))return false;
    h.bytes=size_t(h.times)*(width+1)+h.types*6+h.chars+size_t(h.leaps)*(width+4)+h.standard+h.utc;
    return h.bytes<=b.size()-pos-44;
}
bool parse(Bytes b,Zone& z){
    Header h{};if(!header(b,0,4,h))return false;size_t pos=44,width=4;
    if(h.version){char version=h.version;pos+=h.bytes;if(!header(b,pos,8,h)||h.version!=version)return false;pos+=44;width=8;}
    size_t end=pos+h.bytes;
    // Host wall clocks use POSIX seconds. Do not silently interpret leap-time
    // TZif files (e.g. right/ zones) as ordinary POSIX data.
    if(h.leaps)return false;
    z.transitions.reserve(h.times);z.indices.reserve(h.times);z.types.reserve(h.types);
    for(unsigned i=0;i<h.times;i++,pos+=width){int64_t t=signed_be(b.data()+pos,width);if(i&&t<=z.transitions.back())return false;z.transitions.push_back(t);}
    for(unsigned i=0;i<h.times;i++){auto v=b[pos++];if(v>=h.types)return false;z.indices.push_back(v);}
    auto names=b.subspan(pos+h.types*6,h.chars);
    for(unsigned i=0;i<h.types;i++,pos+=6){auto p=b.data()+pos;unsigned name=p[5];auto offset=signed_be(p,4);if(offset==std::numeric_limits<int32_t>::min()||p[4]>1||name>=names.size())return false;auto zero=static_cast<const uint8_t*>(memchr(names.data()+name,0,names.size()-name));if(!zero)return false;z.types.push_back({int32_t(offset),bool(p[4]),std::string(reinterpret_cast<const char*>(names.data()+name),size_t(zero-names.data()-name))});}
    pos+=h.chars;auto flags=pos;for(;pos<end;pos++)if(b[pos]>1)return false;
    if(h.utc)for(unsigned i=0;i<h.types;i++)if(b[flags+h.standard+i]&&(!h.standard||!b[flags+i]))return false;
    if(width==8){if(end+2>b.size()||b[end]!='\n'||b.back()!='\n')return false;std::string_view tail(reinterpret_cast<const char*>(b.data()+end+1),b.size()-end-2);if(!parse_future(tail,z.future))return false;}
    else if(end!=b.size())return false;
    return true;
}
bool leap(int y){return y%4==0&&(y%100!=0||y%400==0);}
int64_t days(int y,int m,int d){
    // Proleptic Gregorian civil date to days since 1970-01-01. Floor division
    // preserves the Gregorian cycle for years before the epoch as well.
    y-=m<=2;int era=(y>=0?y:y-399)/400;unsigned year=y-era*400;
    unsigned day=(153*(m+(m>2?-3:9))+2)/5+d-1;
    return int64_t(era)*146097+year*365+year/4-year/100+day-719468;
}
int64_t transition(const DateRule& r,int year,int offset){
    int day=0;
    if(r.kind=='N')day=r.a;
    else if(r.kind=='J')day=r.a-1+(leap(year)&&r.a>=60);
    else {int64_t first=days(year,r.a,1);int weekday=int((first+4)%7);if(weekday<0)weekday+=7;int dom=1+(r.c-weekday+7)%7+(r.b-1)*7;int length=int(days(year+(r.a==12),r.a==12?1:r.a+1,1)-first);if(dom>length)dom-=7;day=int(first-days(year,1,1))+dom-1;}
    return (days(year,1,1)+day)*86400+r.seconds-offset;
}
const Type& type_at(const Zone& z,int64_t t){
    if(z.future.present&&(z.transitions.empty()||t>z.transitions.back())){
        auto& f=z.future;if(!f.dst)return f.standard;
        time_t seconds=t;tm utc{};gmtime_r(&seconds,&utc);int year=utc.tm_year+1900;
        int64_t latest=std::numeric_limits<int64_t>::min();const Type* result=&f.standard;
        // Signed transition times may spill into a neighboring civil year.
        for(int y=year-1;y<=year+1;y++){
            auto end=transition(f.end,y,f.summer.offset),start=transition(f.start,y,f.standard.offset);
            if(end<=t&&end>=latest){latest=end;result=&f.standard;}
            if(start<=t&&start>=latest){latest=start;result=&f.summer;}
        }
        return *result;
    }
    auto it=std::upper_bound(z.transitions.begin(),z.transitions.end(),t);
    return z.types[it==z.transitions.begin()?0:z.indices[size_t(it-z.transitions.begin()-1)]];
}
uint32_t calendar(CalendarTime* out,CalendarAdditionalInfo* info,int64_t seconds,const Type& type){
    int64_t local;if(__builtin_add_overflow(seconds,int64_t(type.offset),&local))return Overflow;
    time_t value=local;tm t{};if(!gmtime_r(&value,&t))return Overflow;
    int64_t year=int64_t(t.tm_year)+1900;if(year<-32768||year>32767)return Overflow;
    CalendarTime c{int16_t(year),uint8_t(t.tm_mon+1),uint8_t(t.tm_mday),uint8_t(t.tm_hour),uint8_t(t.tm_min),uint8_t(t.tm_sec),0};
    CalendarAdditionalInfo a{};a.weekday=t.tm_wday;a.yearday=t.tm_yday;a.dst=type.dst;a.offset=type.offset;memcpy(a.zone,type.name.data(),std::min(type.name.size(),sizeof(a.zone)-1));
    if(out)*out=c;if(info)*info=a;return 0;
}
std::vector<uint8_t> read(const std::string& path,size_t limit){
    std::ifstream in(path,std::ios::binary|std::ios::ate);if(!in)return {};auto n=in.tellg();if(n<=0||uint64_t(n)>limit)return {};std::vector<uint8_t> b(size_t(n),0);in.seekg(0);if(!in.read(reinterpret_cast<char*>(b.data()),n))return {};return b;
}
std::vector<uint8_t> database_zone(const std::string& name){
#ifdef __ANDROID__
    for(auto path:{"/apex/com.android.tzdata/etc/tz/tzdata","/system/usr/share/zoneinfo/tzdata"}){
        auto db=read(path,8*1024*1024);if(db.size()<24||memcmp(db.data(),"tzdata",6)||db[11])continue;
        size_t index=be32(db.data()+12),data=be32(db.data()+16),end=be32(db.data()+20);
        if(index<24||data<index||end<data||end>db.size()||(data-index)%52)continue;
        for(size_t i=index;i<data;i+=52){auto p=db.data()+i;auto zero=static_cast<const uint8_t*>(memchr(p,0,40));if(!zero)continue;if(std::string_view(reinterpret_cast<const char*>(p),zero-p)!=name)continue;
            size_t offset=be32(p+40),size=be32(p+44);if(offset>end-data||size>end-data-offset||size>RuleSize-HeaderSize)return {};return {db.begin()+data+offset,db.begin()+data+offset+size};}
    }
#else
    for(auto root:{"/usr/share/zoneinfo/","/var/db/timezone/zoneinfo/"}){auto b=read(std::string(root)+name,RuleSize-HeaderSize);if(!b.empty())return b;}
#endif
    return {};
}
} // namespace
uint32_t time_zone_location(void* out){
    if(!out)return InvalidPointer;std::string name;
#ifdef __ANDROID__
    char property[PROP_VALUE_MAX]{};__system_property_get("persist.sys.timezone",property);name=property;
#else
    if(auto tz=getenv("TZ");tz&&*tz){name=tz;if(name.front()==':')name.erase(0,1);}
    else {std::error_code ec;auto target=std::filesystem::read_symlink("/etc/localtime",ec).string();auto p=target.find("zoneinfo/");if(!ec&&p!=target.npos)name=target.substr(p+9);}
#endif
    if(name.empty())name="UTC";
    if(!valid_location(name))return Invalid;
    std::array<char,36> value{};memcpy(value.data(),name.data(),name.size());memcpy(out,value.data(),value.size());return 0;
}
uint32_t time_zone_encode(void* out,Bytes tzif){
    if(!out)return InvalidPointer;if(tzif.size()>RuleSize-HeaderSize)return Invalid;
    Zone z;if(!parse(tzif,z))return Invalid;
    std::array<uint8_t,RuleSize> value{};memcpy(value.data(),Magic,8);uint32_t n=tzif.size(),hash=checksum(tzif);memcpy(value.data()+8,&n,4);memcpy(value.data()+12,&hash,4);memcpy(value.data()+HeaderSize,tzif.data(),tzif.size());memcpy(out,value.data(),value.size());return 0;
}
uint32_t time_zone_load(void* out,const char* location){
    if(!out||!location)return InvalidPointer;auto n=strnlen(location,36);if(n==36)return Invalid;std::string name(location,n);if(!valid_location(name))return Invalid;auto b=database_zone(name);return time_zone_encode(out,b);
}
uint32_t time_zone_convert(CalendarTime* out,CalendarAdditionalInfo* info,int64_t seconds,const void* rule){
    if(!rule)return InvalidPointer;
    auto p=static_cast<const uint8_t*>(rule);if(memcmp(p,Magic,8))return Invalid;uint32_t n,hash;memcpy(&n,p+8,4);memcpy(&hash,p+12,4);if(n>RuleSize-HeaderSize)return Invalid;Bytes bytes(p+HeaderSize,n);if(checksum(bytes)!=hash)return Invalid;
    // Keep recurrence arithmetic and the SDK's signed 16-bit year bounded,
    // allowing one day at each edge for a timezone offset across New Year.
    if(seconds<days(-32768,1,1)*86400-86400||seconds>=days(32768,1,1)*86400+86400)return Overflow;
    Zone z;if(!parse(bytes,z))return Invalid;return calendar(out,info,seconds,type_at(z,seconds));
}
uint32_t time_zone_utc(CalendarTime* out,CalendarAdditionalInfo* info,int64_t seconds){return calendar(out,info,seconds,{0,false,"UTC"});}
} // namespace ot2
