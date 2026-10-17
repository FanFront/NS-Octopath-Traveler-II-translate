// Driver cache is an optional acceleration hint, never authoritative state.
// Header, UUID, bounded size and full checksum are checked before Vulkan sees
// bytes. Foreground creation, warm-up merge and snapshots share one mutex.
std::mutex driver_cache_mutex,driver_cache_file_mutex;
std::atomic<bool> persist_driver_cache{true},driver_cache_pending{false};
uint64_t driver_cache_saved_count=0;
// Cache blobs also depend on our compatibility policy. UUID alone cannot
// detect a changed shader workaround or a vendor reusing its cache UUID.
std::string driver_policy_identity(){
    VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(physical,&props);
    return std::string("switch2android-vulkan-policy-1:")+std::to_string(props.apiVersion)+":"+
        std::to_string(props.vendorID)+":"+std::to_string(props.deviceID)+":"+
        std::to_string(props.driverVersion)+":"+content_key(props.pipelineCacheUUID,VK_UUID_SIZE)+":"+
        std::to_string(geometry_query_workaround);
}
std::vector<uint8_t> read_driver_cache(const std::string& directory){
    std::ifstream policy(directory+"/driver.policy");std::string identity;std::getline(policy,identity);
    if(!policy||identity!=driver_policy_identity())return {};
    std::ifstream file(directory+"/driver.bin",std::ios::binary|std::ios::ate);
    if(!file)return {};const auto size=file.tellg();if(size<48||size>64*1024*1024)return {};
    std::vector<uint8_t> bytes(size_t(size)-16);std::string checksum(16,'0');file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()),bytes.size());file.read(checksum.data(),16);
    if(!file||content_key(bytes.data(),bytes.size())!=checksum)return {};
    uint32_t header[4];memcpy(header,bytes.data(),sizeof(header));VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(physical,&props);
    if(header[0]!=32||header[1]!=VK_PIPELINE_CACHE_HEADER_VERSION_ONE||header[2]!=props.vendorID||header[3]!=props.deviceID||memcmp(bytes.data()+16,props.pipelineCacheUUID,16))return {};
    return bytes;
}
void load_driver_cache(const std::string& directory){
    if(!persist_driver_cache)return;auto bytes=read_driver_cache(directory);if(bytes.empty())return;
    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};info.initialDataSize=bytes.size();info.pInitialData=bytes.data();VkPipelineCache loaded{};
    if(vkCreatePipelineCache(device,&info,nullptr,&loaded)!=VK_SUCCESS)return;
    {std::lock_guard lock(driver_cache_mutex);vkMergePipelineCaches(device,pipeline_cache,1,&loaded);}
    vkDestroyPipelineCache(device,loaded,nullptr);ot2_host_log("DRIVER_CACHE loaded_bytes=%zu",bytes.size());
}
void write_driver_cache(){
    if(pipeline_directory.empty()||!persist_driver_cache)return;
    std::lock_guard file_lock(driver_cache_file_mutex);std::vector<uint8_t> bytes;
    {std::lock_guard lock(driver_cache_mutex);size_t size=0;
        if(vkGetPipelineCacheData(device,pipeline_cache,&size,nullptr)!=VK_SUCCESS||size<32||size>64*1024*1024)return;
        bytes.resize(size);if(vkGetPipelineCacheData(device,pipeline_cache,&size,bytes.data())!=VK_SUCCESS)return;bytes.resize(size);
    }
    const auto path=pipeline_directory+"/driver.bin";std::ofstream out(path+".tmp",std::ios::binary|std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());out<<content_key(bytes.data(),bytes.size());out.close();std::error_code error;
    if(out)std::filesystem::rename(path+".tmp",path,error);
    if(!error){
        std::ofstream policy(pipeline_directory+"/driver.policy.tmp");policy<<driver_policy_identity()<<"\n";policy.close();
        if(policy)std::filesystem::rename(pipeline_directory+"/driver.policy.tmp",pipeline_directory+"/driver.policy",error);
        if(!error)ot2_host_log("DRIVER_CACHE saved_bytes=%zu",bytes.size());
    }
}
SerialQueue& driver_cache_writer(){static auto* writer=new SerialQueue(1);return *writer;}
void checkpoint_driver_cache(bool force=false){
    const auto count=foreground_created.load()+warm_created.load();
    if(!persist_driver_cache||pipeline_directory.empty()||warm_running||(!force&&count==driver_cache_saved_count)||driver_cache_pending.exchange(true))return;
    driver_cache_saved_count=count;
    driver_cache_writer().push([](uint64_t){write_driver_cache();driver_cache_pending=false;});
}
