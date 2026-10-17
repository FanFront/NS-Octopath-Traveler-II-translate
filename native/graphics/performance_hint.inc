// Portable ADPF: no affinity, CPU IDs, vendor driver or fixed frequencies.
// All state is serialized by state_mutex; pause/reset discards pending samples.
#include "cpu_affinity.inc"
bool performance_hint_enabled=false,performance_hint_attempted=false;
APerformanceHintSession* performance_hint_session=nullptr;
ot2::HintWork performance_hint_work;
uint64_t performance_hint_target=33333334,performance_hint_reports=0,performance_hint_errors=0,performance_hint_dropped=0;
uint64_t performance_hint_retry_at=0,performance_hint_actual=0;
int32_t performance_hint_tid=0;
void performance_hint_close(){
    capacity_affinity_restore();
    if(performance_hint_session)APerformanceHint_closeSession(performance_hint_session);
    performance_hint_session=nullptr;performance_hint_attempted=false;performance_hint_work.clear();
}
void performance_hint_record(uint64_t sequence,uint64_t begin,uint64_t end){
    capacity_affinity_update();
    if(!performance_hint_enabled||end<performance_hint_retry_at)return;
    if(!performance_hint_attempted){
        performance_hint_attempted=true;performance_hint_tid=gettid();
        auto* manager=APerformanceHint_getManager();
        if(manager)performance_hint_session=APerformanceHint_createSession(manager,&performance_hint_tid,1,performance_hint_target);
        ot2_host_log("PERFORMANCE_HINT_CREATE tid=%d active=%u mode=frame-span",performance_hint_tid,performance_hint_session!=nullptr);
    }
    if(!performance_hint_session)return;
    if(!performance_hint_work.record(sequence,begin,end))++performance_hint_dropped;
}
void performance_hint_frame(uint64_t dependency,int interval,uint64_t end,bool visible){
    if(!visible){performance_hint_close();return;}
    auto work=performance_hint_work.consume(dependency);
    if(!performance_hint_session||!work.active||end<work.last)return;
    const auto target=ot2::hint_frame_budget(interval);
    if(target!=performance_hint_target){
        if(APerformanceHint_updateTargetWorkDuration(performance_hint_session,target)){++performance_hint_errors;performance_hint_close();performance_hint_retry_at=end+1000000000;return;}
        performance_hint_target=target;
    }
    // Match a complete frame budget with its real critical-path wall time.
    // The presenter calls this after its GPU dependency and before pacing.
    // Summing only encoder jobs hides gaps in this multi-submit frame pipeline;
    // the hilltop A/B showed that reporting them alone missed the deadline.
    const auto actual=end-work.first;
    if(actual>INT64_MAX||APerformanceHint_reportActualWorkDuration(performance_hint_session,actual)){++performance_hint_errors;performance_hint_close();performance_hint_retry_at=end+1000000000;return;}
    performance_hint_actual=actual;++performance_hint_reports;
}
void performance_hint_report(){
    capacity_affinity_report();
    ot2_host_log("PERFORMANCE_HINT enabled=%u active=%u tid=%d target_ns=%llu reports=%llu errors=%llu dropped=%llu actual_ns=%llu mode=frame-span",performance_hint_enabled,performance_hint_session!=nullptr,performance_hint_tid,(unsigned long long)performance_hint_target,(unsigned long long)performance_hint_reports,(unsigned long long)performance_hint_errors,(unsigned long long)performance_hint_dropped,(unsigned long long)performance_hint_actual);
    const bool wanted=!std::filesystem::exists(shader_directory+"/no-performance-hint");
    if(wanted!=performance_hint_enabled){performance_hint_close();performance_hint_enabled=wanted;performance_hint_retry_at=0;}
}
