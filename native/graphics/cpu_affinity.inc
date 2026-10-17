// Diagnostic opt-in only. This state is protected by state_mutex. Changes are
// made by the encoder itself; suspension can restore its mask after draining.
bool capacity_affinity_requested=false,capacity_affinity_attempted=false,capacity_affinity_applied=false;
int capacity_affinity_tid=0,capacity_affinity_error=0;
cpu_set_t capacity_affinity_original{},capacity_affinity_preferred{};
std::vector<unsigned> capacity_affinity_cpus;
void capacity_affinity_restore(){
    if(!capacity_affinity_applied)return;
    if(sched_setaffinity(capacity_affinity_tid,sizeof(cpu_set_t),&capacity_affinity_original)){
        capacity_affinity_error=errno;return;
    }
    capacity_affinity_applied=false;capacity_affinity_error=0;
}
void capacity_affinity_update(){
    if(!capacity_affinity_requested){capacity_affinity_restore();return;}
    if(capacity_affinity_applied)return;
    if(!capacity_affinity_attempted){
        capacity_affinity_attempted=true;capacity_affinity_tid=gettid();
        if(sched_getaffinity(0,sizeof(cpu_set_t),&capacity_affinity_original)){capacity_affinity_error=errno;return;}
        std::vector<ot2::CpuCapacity> allowed;
        for(unsigned cpu=0;cpu<CPU_SETSIZE;++cpu)if(CPU_ISSET(cpu,&capacity_affinity_original)){
            unsigned capacity=0;
            std::ifstream input("/sys/devices/system/cpu/cpu"+std::to_string(cpu)+"/cpu_capacity");
            input>>capacity;allowed.push_back({cpu,capacity});
        }
        capacity_affinity_cpus=ot2::highest_capacity_cpus(allowed);
        CPU_ZERO(&capacity_affinity_preferred);
        for(auto cpu:capacity_affinity_cpus)CPU_SET(cpu,&capacity_affinity_preferred);
    }
    // Do not repeatedly retry an unsupported/denied operation in a draw loop.
    if(capacity_affinity_cpus.empty()||capacity_affinity_error)return;
    if(sched_setaffinity(0,sizeof(cpu_set_t),&capacity_affinity_preferred)){capacity_affinity_error=errno;return;}
    capacity_affinity_applied=true;
}
void capacity_affinity_report(){
    const bool wanted=std::filesystem::exists(shader_directory+"/prefer-capacity-cores");
    if(wanted!=capacity_affinity_requested){
        capacity_affinity_restore();
        if(!capacity_affinity_applied){capacity_affinity_attempted=false;capacity_affinity_error=0;capacity_affinity_cpus.clear();}
        capacity_affinity_requested=wanted;
    }
    if(wanted||capacity_affinity_applied||capacity_affinity_error){
        std::string cpus;for(auto cpu:capacity_affinity_cpus){if(!cpus.empty())cpus+=",";cpus+=std::to_string(cpu);}
        ot2_host_log("CPU_AFFINITY requested=%u applied=%u tid=%d cpus=%s error=%d source=cpu_capacity",capacity_affinity_requested,capacity_affinity_applied,capacity_affinity_tid,cpus.c_str(),capacity_affinity_error);
    }
}
