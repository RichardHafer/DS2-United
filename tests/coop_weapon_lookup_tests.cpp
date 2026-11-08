#include <cassert>
#include <cstring>
#include <map>
#include "../src/coop_weapon_lookup.h"

int main() {
    constexpr uint32_t p=0x100000, vt=0x1B294D0, entries=0x200000, gun=0x300000;
    std::map<uint32_t,uint32_t> memory{{p,vt},{p+0xC08,entries},{p+0xC0C,2},
        {p+0xC10,4},{entries+4,0},{entries+16,gun+0x10},
        {gun+0x25C,123},{gun+0x240,p+0x10}};
    auto read=[&](uint32_t a,void* out,size_t n) {
        auto it=memory.find(a);
        if(it==memory.end() || n!=4) return false;
        std::memcpy(out,&it->second,4); return true;
    };
    auto find=[&](uint32_t hash){return coop_weapon::Find(p,hash,vt,read);};
    using S=coop_weapon::Status;
    assert(find(123).status==S::Found && find(123).owner==p);
    assert(find(124).status==S::Missing);
    assert(find(0).status==S::Unarmed);
    memory[gun+0x240]=0;
    assert(find(123).status==S::Found && find(123).owner==0);
    memory.erase(gun+0x25C);
    assert(find(123).status==S::Invalid);
    constexpr uint32_t attributes=0x400000;
    auto identity=[&] { return coop_weapon::InspectIdentity(gun, 0x400000, read); };
    memory[gun]=0x1B51A00;
    for (unsigned i=0;i<4;++i) {
        memory[gun+0x24+i*4]=10+i;
        memory[gun+0x404+i*4]=20+i;
    }
    auto id=identity();
    assert(id.kind==coop_weapon::Kind::Gun && id.readable==7);
    assert(id.templateGuid[3]==13 && id.proxyGuid[3]==23);
    memory.erase(gun+0x410);
    id=identity();
    assert(id.readable==3 && id.proxyGuid[0]==0);
    memory[gun]=0x1B51E20;
    memory[gun+0xC]=attributes;
    for (unsigned i=0;i<4;++i) memory[attributes+0x138+i*4]=30+i;
    id=identity();
    assert(id.kind==coop_weapon::Kind::GunProxy && id.readable==7 && id.proxyGuid[3]==33);
    memory[gun+0xC]=0xFFFFFFF0;
    assert(identity().readable==3);
    // the Ripper (class Yoyo) and the Flamethrower are guns too (13.10.: never synced)
    memory[gun]=0x400000+0x1752580;
    assert(identity().kind==coop_weapon::Kind::Gun);
    memory[gun]=0x400000+0x1753520;
    assert(identity().kind==coop_weapon::Kind::Gun);
    memory[gun]=0x12345678;
    assert(identity().kind==coop_weapon::Kind::Unknown && identity().readable==1);
    assert(coop_weapon::InspectIdentity(0xFFFFFFF0,0x400000,read).readable==0);
    memory[p+0xC0C]=5;
    assert(find(123).status==S::Invalid);
    memory[p+0xC10]=10000;
    assert(find(123).status==S::Invalid);
    memory[p]=0;
    assert(find(123).status==S::Invalid);
}
