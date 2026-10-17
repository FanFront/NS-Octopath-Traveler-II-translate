#include "save_transaction.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#include <linux/fs.h>
#elif defined(__APPLE__)
#include <stdio.h>
#endif

namespace ot2 {
int save_test_fault(const char* point) {
#ifdef OT2_FS_TESTING
    if(const char* kill=getenv("OT2_SAVE_TEST_KILL");kill&&!strcmp(kill,point))_exit(91);
    if(const char* fail=getenv("OT2_SAVE_TEST_FAIL");fail&&!strcmp(fail,point)){
        unsetenv("OT2_SAVE_TEST_FAIL");errno=ENOSPC;return ENOSPC;
    }
#else
    (void)point;
#endif
    return 0;
}
namespace {
namespace fs=std::filesystem;
int sync_path(const fs::path& path,bool directory) {
    int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|(directory?O_DIRECTORY:0));
    if(fd<0)return errno;
    int rc=fsync(fd),e=rc?errno:0;
    if(close(fd)&&!e)e=errno;
    return e;
}
int remove_tree(const std::string& path){std::error_code ec;fs::remove_all(path,ec);return ec.value();}
int copy_tree(const fs::path& from,const fs::path& to,bool durable) {
    std::error_code ec;
    if(!fs::is_directory(fs::symlink_status(from,ec)))return ec?ec.value():EINVAL;
    if(!fs::create_directory(to,ec))return ec?ec.value():EEXIST;
    fs::directory_iterator i(from,ec),end;
    for(;!ec&&i!=end;i.increment(ec)){
        auto type=i->symlink_status(ec);if(ec)break;
        auto dst=to/i->path().filename();
        if(fs::is_directory(type)){if(int e=copy_tree(i->path(),dst,durable))return e;}
        else if(fs::is_regular_file(type)){
            if(durable)if(int e=save_test_fault("snapshot_file"))return e;
            if(!fs::copy_file(i->path(),dst,fs::copy_options::none,ec))return ec?ec.value():EIO;
            if(durable)if(int e=sync_path(dst,false))return e;
        }else return EINVAL;
    }
    if(ec)return ec.value();
    return durable?sync_path(to,true):0;
}
int exchange_directories(const std::string& a,const std::string& b){
#if defined(__linux__)
    return syscall(SYS_renameat2,AT_FDCWD,a.c_str(),AT_FDCWD,b.c_str(),RENAME_EXCHANGE)?errno:0;
#elif defined(__APPLE__)
    return renamex_np(a.c_str(),b.c_str(),RENAME_SWAP)?errno:0;
#else
    return ENOTSUP; // Never replace atomic exchange with a two-rename gap.
#endif
}
}
SaveTransaction::SaveTransaction(std::string committed):committed_(std::move(committed)),working_(committed_+".work"),snapshot_(committed_+".new"){}
void SaveTransaction::poison(int error){if(!failure_)failure_=error?error:EIO;}
int SaveTransaction::rollback(){
    // Prepare recovery before discarding the tainted tree. A full disk must
    // leave the transaction poisoned and retryable, never half reset.
    const auto recovery=working_+".re";
    if(int e=remove_tree(recovery))return e;
    if(int e=save_test_fault("reclone"))return e;
    if(int e=copy_tree(committed_,recovery,false))return e;
    if(int e=exchange_directories(recovery,working_))return e;
    remove_tree(recovery);
    failure_=0;ready_=true;return 0;
}
int SaveTransaction::begin(){
    std::error_code ec;std::filesystem::create_directories(committed_,ec);if(ec)return ec.value();
    // A killed commit leaves .new either incomplete or holding the old tree.
    // Neither .new nor .work is ever promoted during recovery.
    if(int e=remove_tree(snapshot_))return e;
    if(int e=remove_tree(working_+".re"))return e;
    if(int e=remove_tree(working_))return e;
    if(int e=save_test_fault("mountclone"))return e;
    if(int e=copy_tree(committed_,working_,false))return e;
    failure_=0;ready_=true;return 0;
}
int SaveTransaction::commit(){
    published_=false;
    if(!ready_)return failure_?failure_:EIO;
    if(failure_){int error=failure_;if(int e=rollback()){poison(e);return e;}return error;}
    if(int e=remove_tree(snapshot_))return e;
    if(int e=copy_tree(working_,snapshot_,true))return e;
    if(int e=save_test_fault("snapshot_ready"))return e;
    if(int e=save_test_fault("before_exchange"))return e;
    if(int e=exchange_directories(snapshot_,committed_))return e;
    published_=true;
    save_test_fault("after_exchange");
    // Files and nested directories were synced before publication. Sync the
    // parent to persist the exchange. A failure here reports an uncertain
    // durability result, but the visible tree is still a complete generation.
    if(int e=save_test_fault("parent_sync"))return e;
    if(int e=sync_path(std::filesystem::path(committed_).parent_path(),true))return e;
    save_test_fault("parent_synced");
    // Removal is housekeeping; it must not turn a successful commit into an
    // apparent failure. A later begin/commit cleans any leftover old tree.
    remove_tree(snapshot_);
    return 0;
}
}
