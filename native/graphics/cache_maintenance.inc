// Included after draw.inc: all mutations are under state_mutex. Never wait for
// GPU work just to satisfy a cache budget; pending leases drain naturally.
CacheBudgetPolicy cache_budget_policy;
uint64_t cache_budget_seconds(){return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
void apply_cache_budget(const CacheBudget& budget){
    cache_budget=budget;max_idle_descriptors=budget.descriptors;texel_view_limit=budget.texel_views;
    trim_descriptors();trim_texel_views();trim_persistent_uploads();trim_free_buffers();trim_resource_uploads();trim_readbacks();
}
void cache_allocation_pressure(){
    cache_budget_policy.allocation_pressure(cache_budget_seconds());
    apply_cache_budget(CacheBudget::reduced());
}
void refresh_cache_budget(bool background=false){
    if(background)trim_commands();
    uint64_t total=0,available=0;bool have_available=false;
    if(auto* file=fopen("/proc/meminfo","r")){
        char line[256];unsigned long long kib=0;
        while(fgets(line,sizeof(line),file)){
            if(sscanf(line,"MemTotal: %llu kB",&kib)==1)total=kib*1024;
            else if(sscanf(line,"MemAvailable: %llu kB",&kib)==1){available=kib*1024;have_available=true;}
        }
        fclose(file);
    }
    if(!have_available)total=0;
    const auto seconds=cache_budget_seconds();
    const bool large=!std::filesystem::exists(shader_directory+"/no-large-caches");
    auto next=cache_budget_policy.update(total,available,seconds,large,resource_uploads);
    if(background&&!next.pressure)next=CacheBudget::background();
    if(std::filesystem::exists(shader_directory+"/small-descriptor-cache"))next.descriptors=std::min<size_t>(next.descriptors,1024);
    apply_cache_budget(next);
    ot2_host_log("CACHE_BUDGET large=%u background=%u pressure=%u available_mb=%llu reserve_mb=%llu allowance_mb=%llu descriptors=%zu texel_views=%zu geometry_mb=%.3f geometry_limit_mb=%zu free_mb=%.3f free_limit_mb=%zu invalidation_visits=%llu",
        large,background,next.pressure,(unsigned long long)(available/CacheBudget::MiB),(unsigned long long)(next.reserve/CacheBudget::MiB),(unsigned long long)(next.allowance/CacheBudget::MiB),
        next.descriptors,next.texel_views,double(persistent_upload_bytes)/CacheBudget::MiB,next.geometry_bytes/CacheBudget::MiB,double(free_buffer_bytes)/CacheBudget::MiB,next.free_buffer_bytes/CacheBudget::MiB,(unsigned long long)texel_view_invalidation_visits);
}
