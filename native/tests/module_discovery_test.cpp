#include "../loader.h"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <cstdio>
#include <unistd.h>
int main(){
    auto require=[](bool ok){if(!ok)throw std::runtime_error("module discovery contract");};
    char pattern[]="/tmp/module-contract-XXXXXX";auto* made=mkdtemp(pattern);require(made);
    const std::filesystem::path root=made;
    struct Cleanup {std::filesystem::path p;~Cleanup(){std::filesystem::remove_all(p);}} cleanup{root};
    auto file=[&](const char* name,const char* header="NSO0"){std::ofstream(root/name)<<header;};
    file("main");file("main.npdm","META");require(ot2::discover_modules(root)==std::vector<std::string>{"main"});
    file("sdk");file("subsdk9");file("rtld");file("subsdk2");
    require(ot2::discover_modules(root)==std::vector<std::string>({"rtld","main","subsdk2","subsdk9","sdk"}));
    file("extra");bool rejected=false;try{ot2::discover_modules(root);}catch(const std::exception&){rejected=true;}require(rejected);
    std::filesystem::remove(root/"extra");std::filesystem::remove(root/"main");rejected=false;try{ot2::discover_modules(root);}catch(const std::exception&){rejected=true;}require(rejected);
    puts("PASS module discovery: missing middleware, sparse subsdk, platform order, unknown NSO and missing main");
}
