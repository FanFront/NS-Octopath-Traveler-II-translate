extern "C" bool ot2_test_native_gates(){
    execution_probe();auto* context=current();
    auto integer=+[](uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f,uint64_t g,uint64_t h){return a+3*b+5*c+7*d+11*e+13*f+17*g+19*h;};
    auto mixed=+[](uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f,double g,double h){return double(a+b+c+d+e+f)+g+h;};
    auto int_gate=ot2_native_gate("register_integer_fixture",reinterpret_cast<void*>(integer));
    auto fp_gate=ot2_native_gate("register_mixed_fixture",reinterpret_cast<void*>(mixed));
    auto configure=[&](const char* name,unsigned abi){for(unsigned i=0;i<native_gate_count;i++)if(!strcmp(native_gates[i].name,name)){__atomic_store_n(&ot2_native_abis[i],abi,__ATOMIC_RELEASE);return;}fail("Native fixture gate absent");};
    configure("register_integer_fixture",NativeAbi<decltype(integer)>::value);
    configure("register_mixed_fixture",NativeAbi<decltype(mixed)>::value);
    configure("direct_nested_fixture",1); // uint64_t(uint64_t), invokes guest recursively.
    uint64_t random=0x1234512345123456;auto next=[&]{random^=random<<13;random^=random>>7;random^=random<<17;return random;};
    auto saved=__atomic_load_n(&ot2_register_gates_enabled,__ATOMIC_ACQUIRE);
    for(unsigned enabled:{0u,1u}){
        ot2_guest_register_gates(enabled);
        for(unsigned i=0;i<256;i++){
            uint64_t a[8];for(auto& x:a)x=next();
            auto expected=integer(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]);
            if(ot2_guest_call(uintptr_t(int_gate),context,a,0)!=expected)fail("Native integer register ABI mismatch");
        }
        uint64_t args[]={uintptr_t(fp_gate),0,0,0,0,0,0,0};
        if(ot2_guest_call(uintptr_t(ot2_abi_fixture),context,args,0)!=25)fail("Native mixed FP/x18 ABI mismatch");
        // Includes nested direct callback, nine integer + FP stack fallback,
        // real NVN memory callback, return values and command record ownership.
        execution_probe();
    }
    auto direct=reinterpret_cast<decltype(integer)>(int_gate);
    for(unsigned enabled:{0u,1u,0u,1u}){
        ot2_guest_register_gates(enabled);uint64_t sum=0;const auto start=monotonic_ns();
        for(unsigned i=0;i<200000;i++)sum+=direct(i,2,3,4,5,6,7,8);
        auto elapsed=monotonic_ns()-start;
        if(sum!=uint64_t(200000)*199999/2+uint64_t(200000)*(6+15+28+55+78+119+152))fail("Native benchmark checksum");
        printf("NATIVE_GATE_BENCH enabled=%u calls=200000 ns_per_call=%.3f\n",enabled,double(elapsed)/200000);
    }
    ot2_guest_register_gates(saved);puts("PASS typed native gates: 512 random integer calls, mixed FP/x18, stack fallback, nested callbacks and command ownership");return true;
}
