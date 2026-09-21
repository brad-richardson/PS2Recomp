// E15 read-only MPEG call/scheduler observations. No MPEG implementation edits.
// PS2X_E15_TRACE=1 AND PS2X_E7_DIR gate all work and share bounded E7 output.
#pragma once
#include "ps2_e7.h"
#include "ps2_runtime_macros.h"
#include <array>

namespace ps2_e15 {
inline bool enabled() {
    static const bool yes=[] { const char *p=std::getenv("PS2X_E15_TRACE"); return p && std::strcmp(p,"1")==0; }();
    return yes && ps2_e7::enabled();
}
struct Registration { uint32_t mpeg=0,type=0,func=0,data=0,handle=0; };
struct State {
    std::mutex mutex;
    std::array<Registration,16> registrations{};
    uint64_t calls=0,returned=0,unwound=0,pending=0,selections=0,invocations=0;
    size_t registered=0; bool registrationTruncated=false;
};
inline State &state() { static State s; return s; }
inline bool address(uint32_t a,uint32_t n) { return a && n<=0x2000000u && a<=0x2000000u-n; }
inline bool callback(uint32_t pc) {
    State &s=state(); std::lock_guard<std::mutex> lock(s.mutex);
    for(size_t i=0;i<s.registered;++i) if(s.registrations[i].func==pc && pc) return true;
    return false;
}
inline bool target(uint32_t pc) {
    return pc==0x402708u || pc==0x4027b8u || pc==0x4029c8u || pc==0x4029d0u ||
           pc==0x402a10u || pc==0x402a58u || pc==0x402aa0u || pc==0x402b58u ||
           pc==0x402c08u || pc==0x3b0b10u || pc==0x3b0b40u || pc==0x3b06b0u ||
           pc==0x3b06f8u || pc==0x3b0c58u || callback(pc);
}
inline void selection(uint64_t tick,const char *route,uint32_t pc,const R5900Context *c,int thread) {
    if(!enabled() || !callback(pc)) return;
    { auto &s=state(); std::lock_guard<std::mutex> lock(s.mutex); ++s.selections; }
    ps2_e7::event(tick,"mpeg-selected","route=%s target=0x%x a0=0x%x a1=0x%x a2=0x%x ra=0x%x sp=0x%x thread=%d",
        route,pc,getRegU32(c,4),getRegU32(c,5),getRegU32(c,6),getRegU32(c,31),getRegU32(c,29),thread);
}
struct Trace {
    bool active=false,isCallback=false,done=false;
    uint64_t id=0,tick=0;
    const char *route=nullptr;
    const uint8_t *ram=nullptr;
    const R5900Context *ctx=nullptr;
    uint32_t pc=0,source=0,a[5]{},sp=0,ra=0;
    int thread=0;
    Trace(const char *r,uint64_t t,const uint8_t *mem,const R5900Context *c,uint32_t p,uint32_t src,int tid)
        :tick(t),route(r),ram(mem),ctx(c),pc(p),source(src),thread(tid) {
        if(!enabled() || !target(pc)) return;
        active=true;isCallback=callback(pc);
        for(unsigned i=0;i<5;++i) a[i]=getRegU32(c,4+i);
        sp=getRegU32(c,29);ra=getRegU32(c,31);
        {auto &s=state();std::lock_guard<std::mutex> lock(s.mutex);id=++s.calls;++s.pending;if(isCallback)++s.invocations;}
        ps2_e7::event(tick,"mpeg-call","id=%llu route=%s target=0x%x source=0x%x a0=0x%x a1=0x%x a2=0x%x a3=0x%x t0=0x%x sp=0x%x ra=0x%x thread=%d callback=%u cbType=%u cbWordReadable=%u",
            static_cast<unsigned long long>(id),route,pc,source,a[0],a[1],a[2],a[3],a[4],sp,ra,thread,isCallback,
            isCallback&&ram&&address(a[1],4)?ps2_e7::word(ram,a[1]):0u,isCallback&&ram&&address(a[1],4));
        if(pc==0x402a10u) {
            auto &s=state();std::lock_guard<std::mutex> lock(s.mutex);
            for(size_t i=0;i<s.registered;++i) {
                const auto &v=s.registrations[i];if(v.mpeg!=a[0])continue;
                ps2_e7::event(tick,"mpeg-request-registration","id=%llu mpeg=0x%x type=%u func=0x%x userdata=0x%x handle=%u source=observed-API-args",
                    static_cast<unsigned long long>(id),v.mpeg,v.type,v.func,v.data,v.handle);
            }
        }
        if((isCallback || pc==0x3b0b40u) && ram) {
            const uint32_t u=isCallback?a[2]:a[0];
            bool guard=false;
            {auto &s=state();std::lock_guard<std::mutex> lock(s.mutex);for(size_t i=0;i<s.registered;++i)guard|=s.registrations[i].data==u;}
            if(guard && address(u,0x80u)) ps2_e7::event(tick,"mpeg-producer","id=%llu userdata=0x%x registrationGuard=1 sourceObject=0x%x buffer78=0x%x buffer7c=0x%x",
                static_cast<unsigned long long>(id),u,ps2_e7::word(ram,u+0x28u),ps2_e7::word(ram,u+0x78u),ps2_e7::word(ram,u+0x7cu));
        }
        if(pc==0x4029d0u && ram) {
            const uint32_t n=a[2]<65536u?a[2]:65536u;
            const bool safe=address(a[1],n);
            char first[129]{};const uint32_t retained=safe?(n<64u?n:64u):0u;
            for(uint32_t i=0;i<retained;++i)std::snprintf(first+i*2u,3,"%02x",ram[a[1]+i]);
            ps2_e7::event(tick,"mpeg-input","id=%llu mpeg=0x%x buffer=0x%x requested=%u hashed=%u safe=%u fnv64=0x%llx retainedHex=%s",
                static_cast<unsigned long long>(id),a[0],a[1],a[2],safe?n:0u,safe,
                static_cast<unsigned long long>(safe?ps2_e7::hash(ram+a[1],n):0u),first);
        }
    }
    void end(uint64_t t,bool unwind) {
        if(!active || done)return;
        done=true;
        if(!unwind && pc==0x402c08u) {
            auto &s=state();std::lock_guard<std::mutex> lock(s.mutex);
            if(s.registered<s.registrations.size())s.registrations[s.registered++]={a[0],a[1],a[2],a[3],getRegU32(ctx,2)};
            else s.registrationTruncated=true;
        }
        {auto &s=state();std::lock_guard<std::mutex> lock(s.mutex);--s.pending;if(unwind)++s.unwound;else ++s.returned;}
        ps2_e7::event(t,unwind?"mpeg-unwind":"mpeg-return","id=%llu route=%s target=0x%x entryA0=0x%x pc=0x%x v0=0x%x sp=0x%x ra=0x%x suppliedRA=0x%x thread=%d callback=%u",
            static_cast<unsigned long long>(id),route,pc,a[0],ctx->pc,getRegU32(ctx,2),getRegU32(ctx,29),getRegU32(ctx,31),ra,thread,isCallback);
        if(!unwind && pc==0x3b06b0u && ram) {
            const uint32_t p=getRegU32(ctx,2);const bool safe=address(p,12);
            ps2_e7::event(t,"mpeg-source-result","id=%llu descriptor=0x%x safe=%u data=0x%x bytes=%u",
                static_cast<unsigned long long>(id),p,safe,safe?ps2_e7::word(ram,p+4u):0u,safe?ps2_e7::word(ram,p+8u):0u);
        }
    }
    void finish(uint64_t t){end(t,false);}
    ~Trace(){end(tick,true);}
};
inline void closure(uint64_t tick) {
    if(!enabled())return;
    auto &s=state();std::lock_guard<std::mutex> lock(s.mutex);
    // Direct footer remains available even after the normal E7 tick-window closes.
    auto &out=ps2_e7::sink();std::lock_guard<std::mutex> outputLock(out.mutex);
    if(out.file && !out.closed) {
        std::fprintf(out.file,"# E15 CLOSURE tick=%llu calls=%llu returned=%llu unwound=%llu pending=%llu selections=%llu invocations=%llu registrations=%zu registrationTruncated=%u\n",
            static_cast<unsigned long long>(tick),static_cast<unsigned long long>(s.calls),static_cast<unsigned long long>(s.returned),
            static_cast<unsigned long long>(s.unwound),static_cast<unsigned long long>(s.pending),
            static_cast<unsigned long long>(s.selections),static_cast<unsigned long long>(s.invocations),s.registered,s.registrationTruncated);
        std::fflush(out.file);
    }
}
} // namespace ps2_e15
