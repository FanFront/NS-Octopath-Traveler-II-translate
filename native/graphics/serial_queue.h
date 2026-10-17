#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <stdexcept>
// One ordered completion domain for translation, GPU work and presentation.
// Completion is published only after the job has released all borrowed data.
class SerialQueue {
    struct Job {uint64_t sequence;std::function<void(uint64_t)> run;};
    std::mutex mutex;std::condition_variable changed;
    std::deque<Job> jobs;uint64_t submitted=0,completed=0;
    bool paused=false,stopping=false;size_t capacity;std::thread worker;
    void loop(){
        for(;;){Job job;
            {std::unique_lock lock(mutex);changed.wait(lock,[&]{return stopping||!jobs.empty();});if(jobs.empty())return;job=std::move(jobs.front());jobs.pop_front();}
            job.run(job.sequence);job.run={};
            {std::lock_guard lock(mutex);completed=job.sequence;}changed.notify_all();
        }
    }
public:
    explicit SerialQueue(size_t limit=32):capacity(limit),worker([this]{loop();}){}
    ~SerialQueue(){{std::lock_guard lock(mutex);stopping=true;paused=false;}changed.notify_all();worker.join();}
    uint64_t push(std::function<void(uint64_t)> run,std::function<void(uint64_t)> publish={}){
        std::unique_lock lock(mutex);changed.wait(lock,[&]{return stopping||(!paused&&submitted-completed<capacity);});
        if(stopping)throw std::runtime_error("Submission after queue shutdown");
        const auto sequence=++submitted;if(publish)publish(sequence);jobs.push_back({sequence,std::move(run)});changed.notify_all();return sequence;
    }
    uint64_t last(){std::lock_guard lock(mutex);return submitted;}
    bool is_paused(){std::lock_guard lock(mutex);return paused;}
    bool wait(uint64_t sequence,uint64_t timeout=UINT64_MAX){
        std::unique_lock lock(mutex);auto ready=[&]{return completed>=sequence;};if(ready())return true;if(!timeout)return false;
        if(timeout==UINT64_MAX){changed.wait(lock,ready);return true;}
        return changed.wait_for(lock,std::chrono::nanoseconds(std::min<uint64_t>(timeout,INT64_MAX)),ready);
    }
    void drain(){wait(last());}
    void suspend(bool on){
        std::unique_lock lock(mutex);paused=on;changed.notify_all();
        if(on){auto target=submitted;changed.wait(lock,[&]{return completed>=target;});}
    }
};
