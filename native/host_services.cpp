// Dedicated host services. ABI layouts follow the D2R host and the original SDK.
#include "host_services.h"
#include "text_input.h"
#include "thread_services.h"
#include "graphics/backend.h"
#include "graphics/nvn_dirty.h"
#include "save_transaction.h"
#include "time_zone.h"
#include "resource_reader.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <fstream>
#include <memory>
#include <atomic>

namespace ot2 {
namespace {
using Registers=uint64_t*;
template<class T> T* ptr(uint64_t p) { return reinterpret_cast<T*>(p); }
const char* str(uint64_t p) { return ptr<const char>(p); }
uint64_t ok(Registers) { return 0; }
uint64_t yes(Registers) { return 1; }
std::string romfs,user;
std::map<int,std::string> addon_content;
uint64_t program_id=legacy_ot2_program_id;
std::mutex activity_mutex;bool activity_active=true,focus_notification=false;unsigned activity_focus=1;
std::atomic<unsigned> operation_mode{0};
bool operation_notification=false,performance_notification=false;
bool operation_notify_enabled=false,performance_notify_enabled=false;
std::chrono::steady_clock::time_point active_since=std::chrono::steady_clock::now();
uint64_t active_ns=0;
uint64_t active_time(){std::lock_guard lock(activity_mutex);return active_ns+(activity_active?std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-active_since).count():0);}

std::atomic<unsigned> save_notice{0};
void notify_save(unsigned notice,int error){save_notice.fetch_or(notice);ot2_host_log("SAVE_NOTICE kind=%u errno=%d",notice,error);}
struct Mount {std::string directory;bool read_only;std::shared_ptr<SaveTransaction> save;bool save_read_only=false;};
std::map<std::string,Mount> mounts;
std::atomic<bool> io_prefetch{true};
std::atomic<bool> stream_io{true};
std::atomic<uint64_t> io_calls{0},io_bytes{0},io_wall_ns{0},io_reused_fds{0},io_hints{0};
ResourcePrefetch& resource_prefetch(){static ResourcePrefetch queue;return queue;}
struct File {int descriptor;std::shared_ptr<SaveTransaction> save;bool resource=false;uint64_t last_end=0,prefetch_end=0;std::shared_ptr<ResourceReadFile> reader;};
std::map<uint64_t,File> files;
struct DirectoryEntry {char name[0x301];uint8_t pad0[3];int8_t type;uint8_t pad1[3];int64_t size;};
static_assert(sizeof(DirectoryEntry)==0x310);
struct Directory {std::vector<DirectoryEntry> entries;size_t position=0;};
std::map<uint64_t,Directory> directories;
struct CacheList {std::vector<int32_t> indices;size_t position=0;};
std::map<uint64_t,CacheList> cache_lists;
std::mutex filesystem_mutex;
uint64_t next_handle=0x100000;
constexpr uint32_t PathNotFound=0x202,PathExists=0x402,PermissionDenied=0x3202,IoError=0x2a2;
uint32_t error() {
    if(errno==ENOENT || errno==ENOTDIR) return PathNotFound;
    if(errno==EEXIST) return PathExists;
    if(errno==EACCES || errno==EPERM || errno==EROFS) return PermissionDenied;
    return IoError;
}
// Called with filesystem_mutex. Reject traversal before any host filesystem operation.
std::string path(const char* guest,bool write=false) {
    std::string input(guest);auto colon=input.find(':');
    if(colon==std::string::npos) return {};
    const auto it=mounts.find(input.substr(0,colon));
    if(it==mounts.end() || (write && it->second.read_only) || (it->second.save&&!it->second.save->ready())) return {};
    std::filesystem::path relative=input.substr(colon+1);
    relative=relative.relative_path();
    for(const auto& component:relative) if(component=="..") return {};
    return (std::filesystem::path(it->second.directory)/relative).string();
}
std::shared_ptr<SaveTransaction> save_for_path(const char* path){
    std::string name(path);auto colon=name.find(':');if(colon==std::string::npos)return {};
    auto i=mounts.find(name.substr(0,colon));return i==mounts.end()?nullptr:i->second.save;
}
void poison_file(uint64_t handle,int e){auto it=files.find(handle);if(it!=files.end()&&it->second.save)it->second.save->poison(e);}
void invalidate_save_handles(const std::shared_ptr<SaveTransaction>& save){
    for(auto i=files.begin();i!=files.end();)if(i->second.save==save){close(i->second.descriptor);i=files.erase(i);}else ++i;
}
uint64_t mount(const char* name,const std::string& directory,bool read_only,bool transactional=false) {
    std::lock_guard lock(filesystem_mutex);
    if(!name || !*name || strchr(name,':') || strchr(name,'/')) return IoError;
    if(mounts.contains(name))return PathExists;
    std::shared_ptr<SaveTransaction> save;
    bool save_read_only=false;
    if(transactional){
        for(const auto& [_,m]:mounts)if((m.save&&m.save->committed()==directory)||(m.save_read_only&&m.directory==directory))return PermissionDenied;
        save=std::make_shared<SaveTransaction>(directory);
        if(int e=save->begin()){
            // The last committed tree is still usable even when disk pressure
            // prevents a working copy. Never write directly into that tree.
            std::error_code ec;
            if(!std::filesystem::is_directory(std::filesystem::symlink_status(directory,ec))){notify_save(SaveCommitFailed,e);return IoError;}
            save.reset();read_only=true;save_read_only=true;notify_save(SaveReadOnly,e);
        }
    }
    if(!read_only) {std::error_code ec;std::filesystem::create_directories(directory,ec);if(ec)return IoError;}
    auto target=save?save->working():directory;
    mounts[name]={target,read_only,save,save_read_only};ot2_host_log("MOUNT %s: -> %s readonly=%d",name,target.c_str(),read_only);return 0;
}
int fd(uint64_t handle) {auto it=files.find(handle);return it==files.end()?-1:it->second.descriptor;}
uint64_t open_file(Registers r) {
    std::lock_guard lock(filesystem_mutex);auto p=path(str(r[1]),r[2]&2);
    if(p.empty())return PermissionDenied;
    int mode=(r[2]&3)==3?O_RDWR:(r[2]&2)?O_WRONLY:O_RDONLY;
    int f=open(p.c_str(),mode|O_CLOEXEC|O_NOFOLLOW);
    if(f<0){if(errno!=ENOENT && errno!=ENOTDIR)ot2_host_log("OPEN failed %s errno=%d",str(r[1]),errno);return error();}
    const auto h=next_handle++;files[h]={f,save_for_path(str(r[1])),mode==O_RDONLY&&!romfs.empty()&&p.starts_with(romfs+"/")};*ptr<uint64_t>(r[0])=h;
    if(files[h].resource)files[h].reader=std::make_shared<ResourceReadFile>(f);
    ot2_host_log("OPEN %s handle=%llu",str(r[1]),(unsigned long long)h);return 0;
}
uint64_t read_file(Registers r) {
    // Keep this inode alive across close/unmount/rollback without serializing
    // all resource reads. The handle's fd may otherwise be reused mid-pread.
    const auto started=std::chrono::steady_clock::now();
    int f;bool prefetch=false,owned=false;std::shared_ptr<ResourceReadFile> reader;{
        std::lock_guard lock(filesystem_mutex);auto it=files.find(r[1]);if(it==files.end()||int64_t(r[2])<0||r[4]>INT64_MAX-r[2])return IoError;
        auto& file=it->second;reader=file.reader;
        owned=stream_io&&bool(reader);f=owned?reader->fd:fcntl(file.descriptor,F_DUPFD_CLOEXEC,0);
        prefetch=io_prefetch&&file.resource&&r[4]>=65536&&file.last_end==r[2]&&r[2]+r[4]>file.prefetch_end;
        file.last_end=r[2]+r[4];if(prefetch)file.prefetch_end=file.last_end+2*1024*1024;
    }
#ifdef __ANDROID__
    if(f>=0&&prefetch){if(owned){if(resource_prefetch().add(reader,r[2]+r[4],2*1024*1024))++io_hints;}else posix_fadvise(f,r[2]+r[4],2*1024*1024,POSIX_FADV_WILLNEED);}
#endif
    if(f<0)return IoError;
    struct CloseRead {int fd;~CloseRead(){if(fd>=0)close(fd);}} keep{owned?-1:f};
    if(owned)++io_reused_fds;
    size_t total=0;auto* dest=ptr<uint8_t>(r[3]);
    NvnDirtyWrite writable_destination(dest,r[4]);
    while(total<r[4]) {auto n=pread(f,dest+total,r[4]-total,r[2]+total);if(n<0){if(errno==EINTR)continue;return error();}if(!n)break;total+=n;}
    ++io_calls;io_bytes+=total;io_wall_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count();
    if(r[0])*ptr<uint64_t>(r[0])=total;return 0;
}
uint64_t write_file(Registers r) {
    std::lock_guard lock(filesystem_mutex);int f=fd(r[0]);if(f<0 || int64_t(r[1])<0)return IoError;
    if(int e=save_test_fault("write")){poison_file(r[0],e);return IoError;}
    size_t done=0;while(done<r[3]){auto n=pwrite(f,ptr<const uint8_t>(r[2])+done,r[3]-done,r[1]+done);if(n<0){if(errno==EINTR)continue;poison_file(r[0],errno);return error();}if(!n){poison_file(r[0],EIO);return IoError;}done+=n;}
    if(r[4] && (*ptr<uint32_t>(r[4])&1) && fsync(f)){poison_file(r[0],errno);return error();}return 0;
}
uint64_t commit(Registers r) {
    std::lock_guard lock(filesystem_mutex);auto it=mounts.find(str(r[0]));if(it==mounts.end())return PermissionDenied;
    // D2R's degraded save mount has no transaction to commit. Writes already
    // fail and the UI reports that this session cannot save.
    if(it->second.save_read_only)return 0;
    if(it->second.read_only)return PermissionDenied;
    if(auto save=it->second.save){
        // Rollback replaces the work tree. No handle may keep writing the
        // discarded inode and later appear to have saved successfully.
        if(save->failed())invalidate_save_handles(save);
        int e=save->commit();
        ot2_host_log("SAVE_COMMIT result=%d published=%d",e,save->published());
        if(e)notify_save(save->published()?SaveDurabilityUncertain:SaveCommitFailed,e);
        return e?IoError:0;
    }
    for(const auto& [handle,file]:files){int f=file.descriptor;int flags=fcntl(f,F_GETFL);if(flags>=0 && (flags&O_ACCMODE)!=O_RDONLY && fsync(f))return error();}
    int d=open(it->second.directory.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(d<0)return error();int rc=fsync(d);close(d);return rc?IoError:0;
}
// Read-only mounts still report AlreadyExists for an existing create target,
// as in D2R. This lets SDK "ensure directory/file" paths proceed to read it.
bool create_target_exists(const char* guest){auto p=path(guest);struct stat st{};return !p.empty()&&!lstat(p.c_str(),&st);}
uint64_t open_directory(Registers r) {
    std::lock_guard lock(filesystem_mutex);auto p=path(str(r[1]));
    if(p.empty())return PathNotFound;
    auto* dir=opendir(p.c_str());if(!dir)return error();
    Directory snapshot;
    while(auto* ent=readdir(dir)) {
        if(!strcmp(ent->d_name,".") || !strcmp(ent->d_name,".."))continue;
        struct stat st{};auto child=p+"/"+ent->d_name;
        if(lstat(child.c_str(),&st) || S_ISLNK(st.st_mode))continue;
        bool is_dir=S_ISDIR(st.st_mode);if(!(r[2]&(is_dir?1:2)))continue;
        DirectoryEntry item{};strncpy(item.name,ent->d_name,sizeof(item.name)-1);
        item.type=is_dir?0:1;item.size=is_dir?0:st.st_size;snapshot.entries.push_back(item);
    }
    closedir(dir);const auto h=next_handle++;directories.emplace(h,std::move(snapshot));*ptr<uint64_t>(r[0])=h;
    ot2_host_log("DIRECTORY %s entries=%zu",str(r[1]),directories.at(h).entries.size());return 0;
}
std::string cache_directory(int32_t index){return index<0 || index>65535?std::string{}:user+"/cache/"+std::to_string(index);}
uint64_t check_storage_size(int64_t size,int64_t journal) {
    if(size<0 || journal<0)return IoError;
    struct statvfs st{};if(statvfs(user.c_str(),&st))return error();
    uint64_t available=uint64_t(st.f_bavail)*st.f_frsize;
    return uint64_t(size)>available || uint64_t(journal)>available-uint64_t(size)?0x6402:0;
}
uint64_t create_cache(Registers r) {
    std::lock_guard lock(filesystem_mutex);auto directory=cache_directory(int32_t(r[0]));if(directory.empty())return IoError;
    if(auto result=check_storage_size(r[1],r[2]))return result;
    std::error_code ec;if(std::filesystem::exists(directory,ec))return PathExists;if(ec)return IoError;
    std::filesystem::create_directories(directory+"/data",ec);if(ec)return IoError;
    std::ofstream metadata(directory+"/sizes",std::ios::binary);int64_t sizes[]={int64_t(r[1]),int64_t(r[2])};
    metadata.write(reinterpret_cast<const char*>(sizes),sizeof(sizes));metadata.close();if(!metadata){std::filesystem::remove_all(directory,ec);return IoError;}return 0;
}
uint64_t mount_cache(Registers r) {
    auto directory=cache_directory(int32_t(r[1]));if(directory.empty())return IoError;
    std::error_code ec;if(!std::filesystem::is_directory(directory+"/data",ec))return PathNotFound;
    return mount(str(r[0]),directory+"/data",false);
}
uint64_t open_cache_list(Registers r) {
    std::lock_guard lock(filesystem_mutex);CacheList snapshot;std::error_code ec;
    auto directory=user+"/cache";if(std::filesystem::exists(directory,ec)) {
        for(std::filesystem::directory_iterator i(directory,ec),end;!ec&&i!=end;i.increment(ec)) {
            auto name=i->path().filename().string();if(name.empty() || name.find_first_not_of("0123456789")!=std::string::npos || name.size()>5)continue;
            int index=std::stoi(name);if(index<=65535 && std::filesystem::is_directory(i->path()/"data",ec))snapshot.indices.push_back(index);
        }
    }
    if(ec)return IoError;std::sort(snapshot.indices.begin(),snapshot.indices.end());auto h=next_handle++;cache_lists.emplace(h,std::move(snapshot));*ptr<uint64_t>(r[0])=h;return 0;
}
uint64_t calendar_time(Registers r) {
    if(!r[2])return 116+(901<<9);
    return time_zone_convert(ptr<CalendarTime>(r[0]),ptr<CalendarAdditionalInfo>(r[1]),*ptr<int64_t>(r[2]),ptr<void>(r[3]));
}
uint64_t calendar_time_current(Registers r) {
    if(!r[2])return 116+(901<<9);
    std::array<char,36> location{};std::array<uint8_t,0x4000> rule{};
    if(auto result=time_zone_location(location.data()))return result;
    if(auto result=time_zone_load(rule.data(),location.data()))return result;
    return time_zone_convert(ptr<CalendarTime>(r[0]),ptr<CalendarAdditionalInfo>(r[1]),*ptr<int64_t>(r[2]),rule.data());
}
// Original SDK libc uses UTC without its Horizon timezone service. D2R also
// replaces these three exports. Keep the original SDK formatting/UTC routines;
// its tm ABI agrees with both supported 64-bit hosts, including zone pointers.
static_assert(sizeof(tm)==56&&offsetof(tm,tm_isdst)==32&&offsetof(tm,tm_gmtoff)==40&&offsetof(tm,tm_zone)==48);
const std::array<uint64_t,2> uid{0x4f54324c4f43414cULL,1};
uint64_t display_object=0x444953504c4159,layer_object=0x4c41594552,native_window_object=0x414e44524f4944;
uint64_t display_event(Registers r) {
    const uint64_t args[]={r[0],1,1,0,0,0,0,0};
    auto result=ot2_call_guest("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb",args);
    if(result)return result;
    const auto handle=ot2_call_guest("_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE",args);
    std::thread([handle]{
        auto deadline=std::chrono::steady_clock::now();
        while(ot2_signal_event(handle)){deadline+=std::chrono::nanoseconds(16666667);std::this_thread::sleep_until(deadline);}
    }).detach();
    return 0;
}
uint64_t open_user(Registers r) {auto* out=ptr<uint64_t>(r[0]);out[0]=uid[0];out[1]=uid[1];out[2]=0x4f543255534552ULL;return 1;}
uint64_t unavailable(Registers){return 0x2a2;}
thread_local int socket_error=50; // Horizon/FreeBSD ENETDOWN, not Android errno.
uint64_t socket_failure(Registers){socket_error=50;return uint64_t(-1);}
uint64_t completed_account_event(Registers r){
    // Let the original SDK construct its own version-specific SystemEvent ABI.
    const uint64_t args[]={r[1],1,1,0,0,0,0,0};
    auto result=ot2_call_guest("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb",args);if(result)return result;
    auto handle=ot2_call_guest("_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE",args);
    return ot2_signal_event(handle)?0:0x2a2;
}
}
void set_io_prefetch(bool enabled){io_prefetch=enabled;}
extern "C" void ot2_host_stream_profile(int enabled){
    stream_io=enabled!=0;if(!enabled)resource_prefetch().clear();
    ot2_host_log("RESOURCE_IO enabled=%d calls=%llu mb=%.3f wall_ms=%.3f reused_fd=%llu hints=%llu",enabled,(unsigned long long)io_calls.exchange(0),io_bytes.exchange(0)/1048576.,io_wall_ns.exchange(0)/1e6,(unsigned long long)io_reused_fds.exchange(0),(unsigned long long)io_hints.exchange(0));
}
void configure_services(const std::string& romfs_directory,const std::string& user_directory,uint64_t identity) {if(!identity)throw std::runtime_error("Invalid game ProgramId");romfs=romfs_directory;user=user_directory;program_id=identity;
    addon_content.clear();
    auto root=std::filesystem::path(romfs_directory).parent_path()/"AddOnContent";
    std::error_code ec;
    for(std::filesystem::directory_iterator it(root,ec),end;!ec&&it!=end;it.increment(ec)){
        const auto name=it->path().filename().string();
        if(name.empty()||name.size()>4||name[0]=='0'||name.find_first_not_of("0123456789")!=std::string::npos)continue;
        int index=std::stoi(name);
        if(index>=1&&index<=2000&&std::filesystem::is_directory(it->symlink_status(ec)))addon_content.emplace(index,it->path().string());
    }
    std::lock_guard lock(activity_mutex);active_ns=0;active_since=std::chrono::steady_clock::now();}
uint64_t configured_program_id(){return program_id;}
unsigned take_save_notice(){return save_notice.exchange(0);}
void set_activity_focus(unsigned state){
    if(state<1 || state>3)ot2_host_die("Invalid activity focus state %u",state);
    {std::lock_guard lock(activity_mutex);auto now=std::chrono::steady_clock::now();
        if(activity_active)active_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(now-active_since).count();
        active_since=now;if(activity_focus!=state){focus_notification=true;ot2_host_log("ACTIVITY_FOCUS state=%u",state);}activity_focus=state;activity_active=state==1;}
    set_audio_active(state==1);
}
void set_activity_active(bool active){set_activity_focus(active?1:2);}
void set_operation_mode(unsigned mode){
    if(mode>1)return;
    const auto previous=operation_mode.exchange(mode,std::memory_order_relaxed);
    if(previous==mode)return;
    std::lock_guard lock(activity_mutex);
    if(operation_notify_enabled)operation_notification=true;
    if(performance_notify_enabled)performance_notification=true;
    ot2_host_log("OPERATION_MODE mode=%u",mode);
}
unsigned get_operation_mode(){return operation_mode.load(std::memory_order_relaxed);}
const std::vector<HostService>& host_services() {
    static const std::vector<HostService> services={
        {"_ZN2nv18InitializeGraphicsEPvm",[](Registers)->uint64_t{vkhost_init();return 0;}},
        {"nvnBootstrapLoader",[](Registers r)->uint64_t{return reinterpret_cast<uint64_t>(ot2_nvn_lookup(str(r[0])));}},
        {"_ZN2nn5audio39AcquireAudioDeviceNotificationForOutputEPNS_2os11SystemEventE",[](Registers r)->uint64_t{
            // The dedicated output endpoint is fixed for the current session.
            const uint64_t args[]={r[0],0,1,0,0,0,0,0};
            return ot2_call_guest("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb",args);
        }},
        {"_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv",[](Registers)->uint64_t{return 2;}},
        {"_ZN2nn5audio24GetActiveAudioDeviceNameEPNS0_15AudioDeviceNameE",[](Registers r)->uint64_t{memset(ptr<void>(r[0]),0,256);memcpy(ptr<void>(r[0]),"DeviceOut",10);return 0;}},
        {"_ZN2nn2vi10InitializeEv",ok},
        {"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE",[](Registers r)->uint64_t{*ptr<uint64_t>(r[0])=reinterpret_cast<uint64_t>(&display_object);return 0;}},
        {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE",[](Registers r)->uint64_t{*ptr<uint64_t>(r[0])=reinterpret_cast<uint64_t>(&layer_object);return 0;}},
        {"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE",[](Registers r)->uint64_t{*ptr<uint64_t>(r[0])=reinterpret_cast<uint64_t>(&native_window_object);return 0;}},
        {"_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE",display_event},
        {"_ZN2nn2vi20GetLatestFrameNumberEPmPKNS0_5LayerE",[](Registers r)->uint64_t{*ptr<uint64_t>(r[0])=vkhost_frame_count();return 0;}},
        // LoadedGame owns the static NSO module registry; no Horizon RO session.
        {"_ZN2nn2ro10InitializeEv",ok},
        {"nnsslInitialize",ok},
        {"nnsslFinalize",ok},
        {"_ZN2nn7friends10InitializeEv",ok},
        // Native input has no Horizon shared-memory activation. Device state is
        // supplied by the Android frontend; initialization only selects types.
        {"_ZN2nn3hid18InitializeKeyboardEv",ok},
        {"_ZN2nn3hid15InitializeMouseEv",ok},
        {"_ZN2nn3hid21InitializeTouchScreenEv",ok},
        {"_ZN2nn3hid14InitializeNpadEv",ok},
        {"_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm",ok},
        {"_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",ok},
        {"_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE",ok},
        {"_ZN2nn3hid18GetNpadJoyHoldTypeEv",ok},
        {"_ZN2nn4nifm10InitializeEv",ok},
        {"_ZN2nn4nifm18IsNetworkAvailableEv",ok},
        {"_ZN2nn4nifm20CancelNetworkRequestEv",ok},
        {"_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv",[](Registers)->uint64_t{return IoError;}},
        {"_ZN2nn4nifm31HandleNetworkRequestErrorResultEv",[](Registers)->uint64_t{return IoError;}},
        {"_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE",[](Registers)->uint64_t{return IoError;}},
        {"_ZN2nn6socket10InitializeEPvmmi",ok},
        {"_ZN2nn6socket10GetSockOptEiiiPvPj",socket_failure},
        {"_ZN2nn6socket10SetSockOptEiiiPKvj",socket_failure},
        {"_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_",[](Registers r)->uint64_t{socket_error=50;if(r[3])*ptr<uint64_t>(r[3])=0;return 4;}},
        {"_ZN2nn6socket11GetNameInfoEPK8sockaddrjPcjS4_ji",[](Registers)->uint64_t{socket_error=50;return 4;}},
        {"_ZN2nn6socket11GetPeerNameEiP8sockaddrPj",socket_failure},
        {"_ZN2nn6socket11GetSockNameEiP8sockaddrPj",socket_failure},
        {"_ZN2nn6socket12FreeAddrInfoEP8addrinfo",ok},
        {"_ZN2nn6socket12GetLastErrnoEv",[](Registers)->uint64_t{return uint32_t(socket_error);}},
        {"_ZN2nn6socket12GetLastErrorEv",[](Registers)->uint64_t{return uint32_t(socket_error);}},
        {"_ZN2nn6socket4BindEiPK8sockaddrj",socket_failure},
        {"_ZN2nn6socket4PollEP6pollfdmi",socket_failure},
        {"_ZN2nn6socket4RecvEiPvmi",socket_failure},
        {"_ZN2nn6socket4SendEiPKvmi",socket_failure},
        {"_ZN2nn6socket5CloseEi",socket_failure},
        {"_ZN2nn6socket5FcntlEiiz",socket_failure},
        {"_ZN2nn6socket5IoctlEijPvm",socket_failure},
        {"_ZN2nn6socket6AcceptEiP8sockaddrPj",socket_failure},
        {"_ZN2nn6socket6ListenEii",socket_failure},
        {"_ZN2nn6socket6SelectEiP6fd_setS2_S2_P7timeval",socket_failure},
        {"_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj",socket_failure},
        {"_ZN2nn6socket6SocketEiii",socket_failure},
        {"_ZN2nn6socket7ConnectEiPK8sockaddrj",socket_failure},
        {"_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj",socket_failure},
        {"_ZN2nn6socket8ShutdownEii",socket_failure},
        {"nnsocketBind",socket_failure},
        {"nnsocketClose",socket_failure},
        {"nnsocketConnect",socket_failure},
        {"nnsocketFcntl",socket_failure},
        {"nnsocketFreeAddrInfo",ok},
        {"nnsocketGetAddrInfo",[](Registers r)->uint64_t{socket_error=50;if(r[3])*ptr<uint64_t>(r[3])=0;return 4;}},
        {"nnsocketGetAddrInfoWithOptions",[](Registers r)->uint64_t{socket_error=50;if(r[3])*ptr<uint64_t>(r[3])=0;return 4;}},
        {"nnsocketGetHostByAddr",[](Registers)->uint64_t{socket_error=50;return 0;}},
        {"nnsocketGetHostByAddrWithOptions",[](Registers)->uint64_t{socket_error=50;return 0;}},
        {"nnsocketGetHostByName",[](Registers)->uint64_t{socket_error=50;return 0;}},
        {"nnsocketGetHostByNameWithOptions",[](Registers)->uint64_t{socket_error=50;return 0;}},
        {"nnsocketGetLastErrno",[](Registers)->uint64_t{return uint32_t(socket_error);}},
        {"nnsocketGetNameInfo",[](Registers)->uint64_t{socket_error=50;return 4;}},
        {"nnsocketGetNameInfoWithOptions",[](Registers)->uint64_t{socket_error=50;return 4;}},
        {"nnsocketGetPeerName",socket_failure},
        {"nnsocketGetSockName",socket_failure},
        {"nnsocketGetSockOpt",socket_failure},
        {"nnsocketPoll",socket_failure},
        {"nnsocketRecv",socket_failure},
        {"nnsocketResolverGetOption",socket_failure},
        {"nnsocketResolverSetOption",socket_failure},
        {"nnsocketSend",socket_failure},
        {"nnsocketSetLastErrno",[](Registers r)->uint64_t{socket_error=int32_t(r[0]);return 0;}},
        {"nnsocketSetSockOpt",socket_failure},
        {"nnsocketSocket",socket_failure},
        {"_ZN2nn3ngc15ProfanityFilter10InitializeEPvmb",ok},
        {"_ZN2nn3ngc15ProfanityFilter11SetMaskModeENS1_8MaskModeE",ok},
        {"_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtj",[](Registers r)->uint64_t{if(r[1])*ptr<int32_t>(r[1])=0;return 0;}},
        {"_ZN2nn3ngc15ProfanityFilterC1Ev",ok},
        {"_ZN2nn3ngc15ProfanityFilterD1Ev",ok},
        {"_ZN2nn4time10InitializeEv",ok},
        {"_ZN2nn4time13IsInitializedEv",yes},
        {"_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE",[](Registers r)->uint64_t{*ptr<int64_t>(r[0])=time(nullptr);return 0;}},
        {"_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE",[](Registers r)->uint64_t{return time_zone_location(ptr<void>(r[0]));}},
        {"_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE",[](Registers r)->uint64_t{return time_zone_load(ptr<void>(r[0]),str(r[1]));}},
        {"_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeERKNS0_12TimeZoneRuleE",calendar_time},
        {"_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE",calendar_time_current},
        {"_ZN2nn4time19ToCalendarTimeInUtcERKNS0_9PosixTimeE",[](Registers r)->uint64_t{CalendarTime c{};time_zone_utc(&c,nullptr,*ptr<int64_t>(r[0]));uint64_t out;memcpy(&out,&c,8);return out;}},
        {"localtime",[](Registers r)->uint64_t{return reinterpret_cast<uint64_t>(localtime(ptr<const time_t>(r[0])));}},
        {"localtime_r",[](Registers r)->uint64_t{return reinterpret_cast<uint64_t>(localtime_r(ptr<const time_t>(r[0]),ptr<tm>(r[1])));}},
        {"mktime",[](Registers r)->uint64_t{return uint64_t(mktime(ptr<tm>(r[0])));}},
        // This SDK's helper only recognizes its own UTC string pointer, so
        // strftime("%Z") otherwise drops valid host-produced timezone names.
        {"__tm_to_tzname",[](Registers r)->uint64_t{const auto name=ptr<const tm>(r[0])->tm_zone;return reinterpret_cast<uint64_t>(name?name:"");}},
        {"_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE",show_keyboard},
        {"_ZN2nn7account10InitializeEv",ok},
        {"_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE",open_user},
        {"_ZN2nn7account8OpenUserEPNS0_10UserHandleERKNS0_3UidE",[](Registers r)->uint64_t{open_user(r);return 0;}},
        {"_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE",[](Registers r)->uint64_t{memcpy(ptr<void>(r[0]),uid.data(),16);return 0;}},
        {"_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE",[](Registers r)->uint64_t{memset(ptr<void>(r[0]),0,33);memcpy(ptr<void>(r[0]),"Player",6);return 0;}},
        {"_ZN2nn7account16ShowUserSelectorEPNS0_3UidERKNS0_21UserSelectionSettingsE",[](Registers r)->uint64_t{memcpy(ptr<void>(r[0]),uid.data(),16);return 0;}},
        {"_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE",[](Registers r)->uint64_t{*ptr<bool>(r[0])=false;return 0;}},
        {"_ZN2nn7account9CloseUserERKNS0_10UserHandleE",ok},
        {"_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE",unavailable},
        {"_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE",unavailable},
        {"_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE",[](Registers r)->uint64_t{if(r[0])*ptr<uint64_t>(r[0])=0;return 0x2a2;}},
        {"_ZN2nn7account12AsyncContextC1Ev",ok},
        {"_ZN2nn7account12AsyncContextD1Ev",ok},
        {"_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE",unavailable},
        {"_ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE",completed_account_event},
        {"_ZN2nn7account12AsyncContext9GetResultEv",unavailable},
        {"_ZN2nn7account42NintendoAccountAuthorizationRequestContextC1Ev",ok},
        {"_ZN2nn7account42NintendoAccountAuthorizationRequestContextD1Ev",ok},
        {"_ZN2nn7account41CreateNintendoAccountAuthorizationRequestEPNS0_42NintendoAccountAuthorizationRequestContextERKNS0_10UserHandleERKNS0_45NintendoAccountAuthorizationRequestParametersEPvm",unavailable},
        {"_ZN2nn7account42NintendoAccountAuthorizationRequestContext14GetSystemEventEPNS_2os11SystemEventE",completed_account_event},
        {"_ZN2nn7account42NintendoAccountAuthorizationRequestContext34GetResultWithInteractionIfRequiredEv",unavailable},
        {"_ZN2nn7account42NintendoAccountAuthorizationRequestContext10GetIdTokenEPmPcm",[](Registers r)->uint64_t{if(r[1])*ptr<uint64_t>(r[1])=0;return 0x2a2;}},
        {"_ZN2nn7friends12UserPresence10InitializeERKNS_7account3UidE",unavailable},
        {"_ZN2nn7friends12UserPresence14SetDescriptionEPKc",unavailable},
        {"_ZN2nn7friends12UserPresence6CommitEv",unavailable},
        {"_ZN2nn7friends18ShowUserDetailInfoERKNS_7account3UidERKNS1_23NetworkServiceAccountIdERKNS0_15InAppScreenNameESA_",unavailable},
        {"_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb",unavailable},
        {"_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_",unavailable},
        {"_ZN2nn4pctl25TryBeginFreeCommunicationEb",[](Registers)->uint64_t{return 0;}},
        {"_ZN2nn3err9ShowErrorENS_6ResultE",[](Registers r)->uint64_t{ot2_host_log("SDK_ERROR result=%x module=%u description=%u",unsigned(r[0]),unsigned(r[0])&511,unsigned(r[0])>>9);return 0;}},
        {"_ZN2nn2oe18GetDesiredLanguageEv",[](Registers)->uint64_t{uint64_t code=0;memcpy(&code,"zh-Hans",7);return code;}},
        {"_ZN2nn2oe25GetProgramTotalActiveTimeEv",[](Registers)->uint64_t{return active_time();}},
        {"_ZN2nn2oe16GetOperationModeEv",[](Registers)->uint64_t{return get_operation_mode();}},
        {"_ZN2nn2oe18GetPerformanceModeEv",[](Registers)->uint64_t{return get_operation_mode();}},
        {"_ZN2nn2oe20GetCurrentFocusStateEv",[](Registers)->uint64_t{std::lock_guard lock(activity_mutex);return activity_focus;}},
        {"_ZN2nn2oe25TryPopNotificationMessageEPj",[](Registers r)->uint64_t{
            std::lock_guard lock(activity_mutex);
            if(!r[0])return 0;
            if(focus_notification){*ptr<uint32_t>(r[0])=15;focus_notification=false;return 1;}
            if(operation_notification){*ptr<uint32_t>(r[0])=30;operation_notification=false;return 1;}
            if(performance_notification){*ptr<uint32_t>(r[0])=31;performance_notification=false;return 1;}
            return 0;
        }},
        {"_ZN2nn2oe17FinishStartupLogoEv",ok},
        {"_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE",ok},
        {"_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi",ok},
        {"_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE",ok},
        {"_ZN2nn2oe28SetResumeNotificationEnabledEb",ok},
        {"_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb",[](Registers r)->uint64_t{
            std::lock_guard lock(activity_mutex);operation_notify_enabled=r[0]!=0;
            if(!operation_notify_enabled)operation_notification=false;return 0;
        }},
        {"_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb",[](Registers r)->uint64_t{
            std::lock_guard lock(activity_mutex);performance_notify_enabled=r[0]!=0;
            if(!performance_notify_enabled)performance_notification=false;return 0;
        }},
        {"_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb",ok},
        {"_ZN2nn2oe31EnterExitRequestHandlingSectionEv",ok},
        {"_ZN2nn2oe31LeaveExitRequestHandlingSectionEv",ok},
        {"_ZN2nn2oe16EnableScreenShotEv",ok},
        {"_ZN2nn2oe17DisableScreenShotEv",ok},
        {"_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE",[](Registers r)->uint64_t{memcpy(ptr<void>(r[0]),uid.data(),16);return 0;}},
        {"_ZN2nn3aoc17CountAddOnContentEv",[](Registers)->uint64_t{return addon_content.size();}},
        {"_ZN2nn3aoc16ListAddOnContentEPiii",[](Registers r)->uint64_t{
            int offset=int32_t(r[1]),count=int32_t(r[2]);if(offset<0||count<=0||!r[0])return 0;
            int position=0,written=0;auto* out=ptr<int32_t>(r[0]);
            for(const auto& [index,_]:addon_content)if(position++>=offset&&written<count)out[written++]=index;
            return written;
        }},
        {"_ZN2nn3aoc31GetAddOnContentListChangedEventEPNS_2os11SystemEventE",[](Registers r)->uint64_t{
            // The installed DLC set is immutable during a run. A real unsignaled
            // host event lets the original SDK wait without a service-session pointer.
            const uint64_t args[]={r[0],0,1,0,0,0,0,0};
            return ot2_call_guest("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb",args);
        }},
        {"_ZN2nn2fs18IsSaveDataExistingENS_13ApplicationIdERKNS_7account3UidE",[](Registers r)->uint64_t{return r[0]==program_id && std::filesystem::is_directory(user+"/save");}},
        {"_ZN2nn2fs21MountSaveDataReadOnlyEPKcNS_13ApplicationIdERKNS_7account3UidE",[](Registers r)->uint64_t{if(r[1]!=program_id || !std::filesystem::is_directory(user+"/save"))return PathNotFound;return mount(str(r[0]),user+"/save",true);}},
        {"_ZN2nn2fs7UnmountEPKc",[](Registers r)->uint64_t{std::lock_guard lock(filesystem_mutex);auto it=mounts.find(str(r[0]));if(it!=mounts.end()){if(it->second.save)invalidate_save_handles(it->second.save);mounts.erase(it);}return 0;}},
        {"_ZN2nn2fs10CreateFileEPKcl",[](Registers r)->uint64_t{std::lock_guard lock(filesystem_mutex);if(int64_t(r[1])<0)return PermissionDenied;if(create_target_exists(str(r[0])))return PathExists;auto p=path(str(r[0]),true);if(p.empty())return PermissionDenied;
            auto save=save_for_path(str(r[0]));int f=open(p.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
            if(f<0){if(save&&(errno==ENOSPC||errno==EDQUOT||errno==EIO))save->poison(errno);return error();}
            int rc=ftruncate(f,r[1]),e=rc?errno:0;if(close(f)&&!e)e=errno;
            if(e){if(save)save->poison(e);unlink(p.c_str());return IoError;}return 0;}},
        {"_ZN2nn2fs10DeleteFileEPKc",[](Registers r)->uint64_t{std::lock_guard lock(filesystem_mutex);auto p=path(str(r[0]),true);if(p.empty())return PermissionDenied;return unlink(p.c_str())?error():0;}},
        {"_ZN2nn2fs10RenameFileEPKcS2_",[](Registers r)->uint64_t{std::lock_guard lock(filesystem_mutex);auto a=path(str(r[0]),true),b=path(str(r[1]),true);if(a.empty() || b.empty())return PermissionDenied;
            struct stat existing{};if(!lstat(b.c_str(),&existing))return PathExists;if(errno!=ENOENT)return error();return rename(a.c_str(),b.c_str())?error():0;}},
        {"_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl",[](Registers r)->uint64_t{std::lock_guard lock(filesystem_mutex);if(fd(r[0])<0 || int64_t(r[1])<0)return IoError;
            if(int e=save_test_fault("resize")){poison_file(r[0],e);return IoError;}if(ftruncate(fd(r[0]),r[1])){poison_file(r[0],errno);return error();}return 0;}},
        {"_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE",write_file},
        {"_ZN2nn2fs9FlushFileENS0_10FileHandleE",[](Registers r)->uint64_t{std::lock_guard lock(filesystem_mutex);if(fsync(fd(r[0]))){poison_file(r[0],errno);return error();}return 0;}},
        {"_ZN2nn2fs14CommitSaveDataEPKc",commit},
        {"_ZN2nn2fs6CommitEPKc",commit},
        {"_ZN2nn2fs15CreateDirectoryEPKc",[](Registers r)->uint64_t{std::lock_guard lock(filesystem_mutex);if(create_target_exists(str(r[0])))return PathExists;auto p=path(str(r[0]),true);if(p.empty())return PermissionDenied;return mkdir(p.c_str(),0700)?error():0;}},
        {"_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc",[](Registers r)->uint64_t{
            std::lock_guard lock(filesystem_mutex);auto p=path(str(r[0]),true);if(p.empty())return PermissionDenied;
            for(const auto& [name,m]:mounts){std::error_code same_error;if(std::filesystem::equivalent(p,m.directory,same_error))return PermissionDenied;}
            std::error_code ec;if(!std::filesystem::is_directory(p,ec))return PathNotFound;std::filesystem::remove_all(p,ec);return ec?IoError:0;
        }},
        {"_ZN2nn2fs14ExtendSaveDataERKNS_7account3UidEll",[](Registers r)->uint64_t{return check_storage_size(r[1],r[2]);}},
        {"_ZN2nn2fs18CreateCacheStorageEill",create_cache},
        {"_ZN2nn2fs17MountCacheStorageEPKci",mount_cache},
        {"_ZN2nn2fs17MountCacheStorageEPKc",[](Registers r)->uint64_t{uint64_t args[]={r[0],0};return mount_cache(args);}},
        {"_ZN2nn2fs19GetCacheStorageSizeEPlS1_i",[](Registers r)->uint64_t{
            std::lock_guard lock(filesystem_mutex);auto directory=cache_directory(int32_t(r[2]));if(directory.empty())return IoError;
            std::ifstream metadata(directory+"/sizes",std::ios::binary);if(!metadata)return PathNotFound;int64_t sizes[2]{};
            if(!metadata.read(reinterpret_cast<char*>(sizes),sizeof(sizes)))return IoError;*ptr<int64_t>(r[0])=sizes[0];*ptr<int64_t>(r[1])=sizes[1];return 0;
        }},
        {"_ZN2nn2fs18DeleteCacheStorageEi",[](Registers r)->uint64_t{
            std::lock_guard lock(filesystem_mutex);auto directory=cache_directory(int32_t(r[0]));if(directory.empty())return IoError;
            for(const auto& [name,m]:mounts)if(m.directory==directory+"/data")return PermissionDenied;
            std::error_code ec;if(!std::filesystem::exists(directory,ec))return PathNotFound;std::filesystem::remove_all(directory,ec);return ec?IoError:0;
        }},
        {"_ZN2nn2fs20OpenCacheStorageListEPNS0_22CacheStorageListHandleE",open_cache_list},
        {"_ZN2nn2fs20ReadCacheStorageListEPiPNS0_16CacheStorageInfoENS0_22CacheStorageListHandleEi",[](Registers r)->uint64_t{
            std::lock_guard lock(filesystem_mutex);auto it=cache_lists.find(r[2]);if(it==cache_lists.end() || int32_t(r[3])<0)return IoError;
            auto& list=it->second;size_t n=std::min<size_t>(int32_t(r[3]),list.indices.size()-list.position);auto* out=ptr<uint8_t>(r[1]);
            for(size_t i=0;i<n;i++){memset(out+i*32,0,32);memcpy(out+i*32,&list.indices[list.position++],4);}*ptr<int32_t>(r[0])=n;return 0;
        }},
        {"_ZN2nn2fs21CloseCacheStorageListENS0_22CacheStorageListHandleE",[](Registers r)->uint64_t{std::lock_guard lock(filesystem_mutex);return cache_lists.erase(r[0])?0:IoError;}},
        {"_ZN2nn2fs31QueryMountAddOnContentCacheSizeEPmi",[](Registers r)->uint64_t{*ptr<uint64_t>(r[0])=0x1000;return addon_content.contains(int32_t(r[1]))?0:0x7d402;}},
        {"_ZN2nn2fs17MountAddOnContentEPKciPvm",[](Registers r)->uint64_t{auto it=addon_content.find(int32_t(r[1]));return it==addon_content.end()?0x7d402:mount(str(r[0]),it->second,true);}},
        {"_ZN2nn2fs22QueryMountRomCacheSizeEPm",[](Registers r)->uint64_t{*ptr<uint64_t>(r[0])=4096;return 0;}},
        {"_ZN2nn2fs8MountRomEPKcPvm",[](Registers r)->uint64_t{return mount(str(r[0]),romfs,true);}},
        {"_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE",[](Registers r)->uint64_t{return mount(str(r[0]),user+"/save",false,true);}},
        {"_ZN2nn2fs21MountTemporaryStorageEPKc",[](Registers r)->uint64_t{return mount(str(r[0]),user+"/temporary",false);}},
        {"_ZN2nn2fs14EnsureSaveDataERKNS_7account3UidE",[](Registers)->uint64_t{std::error_code ec;std::filesystem::create_directories(user+"/save",ec);return ec?IoError:0;}},
        {"_ZN2nn2fs18IsSaveDataExistingERKNS_7account3UidE",[](Registers)->uint64_t{return std::filesystem::is_directory(user+"/save");}},
        {"_ZN2nn2fs12SetAllocatorEPFPvmEPFvS1_mE",ok},
        {"_ZN2nn2fs25EnableGlobalFileDataCacheEPvm",ok},
        {"_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci",open_file},
        {"_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm",read_file},
        {"_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm",[](Registers r)->uint64_t{uint64_t count=0;uint64_t args[]={reinterpret_cast<uint64_t>(&count),r[0],r[1],r[2],r[3]};auto result=read_file(args);return result?result:count==r[3]?0:IoError;}},
        {"_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE",[](Registers r)->uint64_t{std::lock_guard lock(filesystem_mutex);struct stat st{};if(fstat(fd(r[1]),&st))return error();*ptr<int64_t>(r[0])=st.st_size;return 0;}},
        {"_ZN2nn2fs9CloseFileENS0_10FileHandleE",[](Registers r)->uint64_t{std::lock_guard lock(filesystem_mutex);auto it=files.find(r[0]);if(it==files.end())return IoError;
            // Commit may follow CloseFile; sync writable content before losing
            // its descriptor, then Commit persists the mount directory entry.
            int f=it->second.descriptor,flags=fcntl(f,F_GETFL);uint32_t result=0;
            if(flags>=0 && (flags&O_ACCMODE)!=O_RDONLY && fsync(f)){poison_file(r[0],errno);result=error();}
            if(!it->second.reader&&close(f) && !result){poison_file(r[0],errno);result=error();}files.erase(it);return result;}},
        {"_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc",[](Registers r)->uint64_t{std::lock_guard lock(filesystem_mutex);auto p=path(str(r[1]));if(p.empty())return PathNotFound;struct stat st{};if(lstat(p.c_str(),&st))return error();*ptr<int32_t>(r[0])=S_ISDIR(st.st_mode)?0:1;return 0;}},
        {"_ZN2nn2fs16GetFreeSpaceSizeEPlPKc",[](Registers r)->uint64_t{struct statvfs st{};if(statvfs(user.c_str(),&st))return error();*ptr<int64_t>(r[0])=uint64_t(st.f_bavail)*st.f_frsize;return 0;}},
        {"_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci",open_directory},
        {"_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE",[](Registers r)->uint64_t{std::lock_guard lock(filesystem_mutex);auto it=directories.find(r[1]);if(it==directories.end())return IoError;*ptr<int64_t>(r[0])=it->second.entries.size();return 0;}},
        {"_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl",[](Registers r)->uint64_t{std::lock_guard lock(filesystem_mutex);auto it=directories.find(r[2]);if(it==directories.end() || int64_t(r[3])<0)return IoError;auto& d=it->second;size_t n=std::min<size_t>(r[3],d.entries.size()-d.position);if(n)memcpy(ptr<void>(r[1]),d.entries.data()+d.position,n*sizeof(DirectoryEntry));d.position+=n;*ptr<int64_t>(r[0])=n;return 0;}},
        {"_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE",[](Registers r)->uint64_t{std::lock_guard lock(filesystem_mutex);return directories.erase(r[0])?0:IoError;}},
    };
    return services;
}
}
