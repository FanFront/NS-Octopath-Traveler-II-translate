#include "../loader.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <cstdio>
#include <unistd.h>

namespace {
void require(bool ok){if(!ok)throw std::runtime_error("weak import contract");}
struct Symbol {const char* name;uint8_t binding;uint16_t section;uint64_t value;};
struct Rel {uint32_t symbol,type;int64_t addend;};
// A real, uncompressed NSO fixture is parsed and relocated by LoadedGame.
// No guest instructions or constructors run in this test.
void module(const std::filesystem::path& file,std::vector<Symbol> symbols,std::vector<Rel> rels={}) {
    std::vector<uint8_t> bytes(0x3100);
    auto put=[&]<class T>(size_t at,T v){require(at+sizeof(v)<=bytes.size());memcpy(bytes.data()+at,&v,sizeof(v));};
    memcpy(bytes.data(),"NSO0",4);
    for(uint32_t i=0;i<3;i++){put(0x10+i*16,0x100+i*0x1000);put(0x14+i*16,i*0x1000);put(0x18+i*16,uint32_t(0x1000));}
    put(0x104,uint32_t(0x80));put(0x180,uint32_t(0x30444f4d));put(0x184,uint32_t(0x80));
    put(0x200,uint64_t(7));put(0x208,uint64_t(0x300));put(0x210,uint64_t(8));put(0x218,uint64_t(rels.size()*24));
    std::string names(1,'\0');
    for(size_t i=0;i<symbols.size();i++){
        auto s=symbols[i];size_t at=0x1200+i*24;put(at,uint32_t(names.size()));names+=s.name;names+='\0';
        put(at+4,uint8_t(s.binding<<4));put(at+6,s.section);put(at+8,s.value);
    }
    require(names.size()<0x500);memcpy(bytes.data()+0x1600,names.data(),names.size());
    put(0x90,uint32_t(0x500));put(0x94,uint32_t(names.size()));put(0x98,uint32_t(0x100));put(0x9c,uint32_t(symbols.size()*24));
    for(size_t i=0;i<rels.size();i++){
        size_t at=0x400+i*24;put(at,uint64_t(0x2000+i*8));put(at+8,(uint64_t(rels[i].symbol)<<32)|rels[i].type);put(at+16,rels[i].addend);
    }
    std::ofstream(file,std::ios::binary).write(reinterpret_cast<char*>(bytes.data()),bytes.size());
}
}
int main(){
    char pattern[]="/tmp/ot2-link-contract-XXXXXX";auto* made=mkdtemp(pattern);require(made);
    const std::filesystem::path root=made;
    struct Cleanup {std::filesystem::path p;~Cleanup(){std::filesystem::remove_all(p);}} cleanup{root};
    std::vector<Symbol> imports={{"",0,0,0},{"missing",2,0,0},{"provided",2,0,0},{"choice",2,1,0x800},{"local",0,1,0x900}};
    module(root/"main",imports,{{1,1025,0},{1,1026,0},{1,257,17},{2,1026,0},{3,1025,8},{4,257,-8}});
    module(root/"sdk",{{"",0,0,0},{"provided",2,1,0x800},{"choice",1,1,0x900}});
    {
        ot2::LoadedGame game(root);auto views=game.modules();require(views.size()==2);
        uint64_t values[6];memcpy(values,views[0].base+0x2000,sizeof(values));
        require(values[0]==0&&values[1]==0&&values[2]==17);
        require(values[3]==uintptr_t(views[1].base+0x800));
        require(values[4]==uintptr_t(views[1].base+0x908));
        require(values[5]==uintptr_t(views[0].base+0x8f8));
        require(game.symbol("missing")==0&&game.symbol("choice")==uintptr_t(views[1].base+0x900));
    }
    require(!ot2::inspect_modules(root).empty());
    imports[1].binding=1;module(root/"main",imports,{{1,1026,0}});
    bool rejected=false;try{ot2::LoadedGame game(root);}catch(const std::runtime_error& e){rejected=std::string(e.what())=="Unresolved strong import";}require(rejected);
    rejected=false;try{ot2::inspect_modules(root);}catch(const std::runtime_error& e){rejected=std::string(e.what())=="Unresolved strong import";}require(rejected);
    puts("PASS actual NSO linking: absent weak, defined weak, strong precedence, local/addend, missing strong and inspect parity");
}
