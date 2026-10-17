#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <thread>
#include <unistd.h>

// Read-only resource descriptors are shared by reads already in progress.
// Closing a guest handle removes it immediately but cannot recycle its fd
// underneath another thread's pread or queued hint. Save fds use their existing
// explicit sync/close/error path and never enter this queue.
struct ResourceReadFile {
    explicit ResourceReadFile(int value):fd(value){}
    ~ResourceReadFile(){close(fd);}
    const int fd;
};
class ResourcePrefetch {
    struct Hint {std::shared_ptr<ResourceReadFile> file;uint64_t offset,bytes;};
    std::mutex mutex;std::condition_variable changed;std::deque<Hint> pending;
    bool stopping=false;std::thread worker;
public:
    ResourcePrefetch():worker([this]{
        for(;;){Hint hint;
            {std::unique_lock lock(mutex);changed.wait(lock,[&]{return stopping||!pending.empty();});if(stopping)return;hint=std::move(pending.front());pending.pop_front();}
#ifdef __ANDROID__
            posix_fadvise(hint.file->fd,hint.offset,hint.bytes,POSIX_FADV_WILLNEED);
#endif
        }
    }){}
    ~ResourcePrefetch(){{std::lock_guard lock(mutex);stopping=true;pending.clear();}changed.notify_all();worker.join();}
    bool add(std::shared_ptr<ResourceReadFile> file,uint64_t offset,uint64_t bytes){
        std::lock_guard lock(mutex);
        // Kernel page cache stays reclaimable. Bound queued work/fd lifetimes,
        // and never allocate a duplicate userspace copy of the game archive.
        if(stopping||pending.size()>=4||!bytes||bytes>2*1024*1024||offset>INT64_MAX-bytes)return false;
        pending.push_back({std::move(file),offset,bytes});changed.notify_one();return true;
    }
    void clear(){std::lock_guard lock(mutex);pending.clear();}
};
