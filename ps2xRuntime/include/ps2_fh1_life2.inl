// Included inside namespace ps2_fh1, after rd32/wr32 (LCY2).
// LCY1 source: 3a7e10..28 caches int(rate*.1); 3a8448 refreshes,
// 3a84c8 forces expiry, 3a8668 compares raw update age. Rescale BOTH
// live ages and threshold on flips. The separate five-render-tick wait stays
// untouched. Preserve the half-stock-tick remainder across repeated flips.
inline constexpr uint32_t kLife2Targets[] = {0x3a7dd0u, 0x3a7e38u, 0x3a8448u, 0x3a84c8u,
                                            0x26d168u, 0x26d130u, 0x26d4d8u};
inline constexpr uint32_t kLife2Sources[] = {0x26d7acu, 0x26d7f4u};
struct Life2World
{
    uint32_t object = 0u;
    std::array<uint32_t,256> stamps{};
    std::array<uint8_t,256> half{};
};
struct Life2Anchor
{
    uint32_t input = 0u, timeline = 0u, ordinal = 0u;
};
inline std::array<Life2World,16> g_life2Worlds{};
inline std::vector<Life2Anchor> g_life2Anchors;
inline bool life2Fix() noexcept { return enabled() && (fixMask() & kFixLife2) != 0u; }
inline void life2ForgetInput(uint32_t input)
{
    auto &v = g_life2Anchors;
    v.erase(std::remove_if(v.begin(),v.end(),[input](const auto &a){return a.input==input;}),v.end());
}
inline Life2World *life2World(uint32_t object, bool create)
{
    if (!object || (object & 3u)) return nullptr;
    for (auto &w : g_life2Worlds) if (w.object==object) return &w;
    if (create) for (auto &w : g_life2Worlds) if (!w.object) {w={}; w.object=object; return &w;}
    return nullptr;
}
inline void life2FlipWorld(uint8_t *ram, Life2World &w, uint32_t update, bool toActive)
{
    uint32_t n=0u, threshold=0u;
    if (!rd32(ram,w.object+0x3ecu,n) || n>256u ||
        !rd32(ram,w.object+0x1bf0u,threshold)) return;
    // Constructor thresholds are six stock ticks or twelve 120 ticks.
    if (threshold != (toActive ? 6u : 12u)) return;
    for (uint32_t i=0;i<n;++i)
    {
        const uint32_t addr=w.object+0x400u+24u*i;
        uint32_t stamp=0u,state=0u;
        if (!rd32(ram,addr,stamp) || !rd32(ram,addr-16u,state) || !state) continue;
        const int64_t age=static_cast<int32_t>(update-stamp);
        const int64_t halfAge=toActive ? age*2+(w.stamps[i]==stamp?w.half[i]:0u) : age;
        // floor division keeps a future stamp's signed age and lets stock
        // service naturally quantize expiry to its next whole update.
        const int64_t newAge=toActive ? halfAge : (halfAge>=0?halfAge/2:(halfAge-1)/2);
        const uint32_t next=update-static_cast<uint32_t>(newAge);
        wr32(ram,addr,next); w.stamps[i]=next;
        w.half[i]=toActive ? 0u : static_cast<uint8_t>(halfAge-newAge*2);
    }
    wr32(ram,w.object+0x1bf0u,toActive?12u:6u);
}
inline void life2Flip(uint8_t *ram, uint32_t manager, bool toActive)
{
    uint32_t update=0u;
    if (!rd32(ram,manager+0x1cu,update)) return;
    for (auto &w : g_life2Worlds) if (w.object) life2FlipWorld(ram,w,update,toActive);
}
inline bool life2Samples(const uint8_t *ram, uint32_t input, uint32_t &samples)
{
    uint32_t n=0u, data=0u; samples=0u;
    if (!input || !rd32(ram,input,n) || n>4096u || !rd32(ram,input+12u,data)) return false;
    for (uint32_t i=0;i<n;++i)
    { uint32_t word=0u; if (!rd32(ram,data+i*8u,word)) return false; samples+=word&4095u; }
    return true;
}
inline void life2Remember(uint32_t input, uint32_t timeline, uint32_t ordinal)
{
    for (auto &a : g_life2Anchors) if (a.input==input && a.timeline==timeline)
    {a.ordinal=ordinal; return;}
    if (g_life2Anchors.size()>=16384u) {std::fprintf(stderr,"life2-refused anchor capacity\n");std::abort();}
    g_life2Anchors.push_back({input,timeline,ordinal});
}
inline bool life2Ordinal(uint32_t input, uint32_t timeline, uint32_t &ordinal)
{
    if (!timeline) {ordinal=0u; return true;}
    for (const auto &a : g_life2Anchors) if (a.input==input && a.timeline==timeline)
    {ordinal=a.ordinal; return true;}
    return false;
}
inline void life2Capture(const uint8_t *ram, uint32_t session)
{
    uint32_t timeline=0u, state=0u;
    if (!session || !rd32(ram,session,state) || state!=0u || !rd32(ram,session+0x3c8u,timeline)) return;
    // The exact recording cursor supplies the rate/epoch metadata, including
    // pauses, first-update offset (2*t-1), and mixed recording cadences.
    for (uint32_t slot=0u;slot<2u;++slot)
    {
        uint32_t input=0u,samples=0u;
        if (rd32(ram,session+0x48cu+slot*4u,input) && life2Samples(ram,input,samples))
            life2Remember(input,timeline,samples);
    }
}
inline void life2Hook(uint8_t *ram, R5900Context *ctx, uint32_t source, uint32_t target)
{
    if (!ctx) return;
    const uint32_t object=getRegU32(ctx,4);
    if (target==0x3a7dd0u)
    {
        auto *w=life2World(object,true);
        if (!w) {std::fprintf(stderr,"life2-refused world capacity\n");std::abort();}
        *w={}; w->object=object;
    }
    else if (target==0x3a7e38u)
    {if (auto *w=life2World(object,false)) *w={};}
    else if (target==0x3a8448u || target==0x3a84c8u)
    {
        const uint32_t ch=getRegU32(ctx,5);
        if (auto *w=life2World(object,false); w && ch<256u) w->half[ch]=0u;
    }
    if (target==0x26d168u || target==0x26d130u) life2ForgetInput(object);
    // Capture after input recording, at both snapshot writers. Delay slots
    // have run: 26d7ac has stored +30; 26d7f4 has saved session in s1.
    if (source==0x26d7acu || source==0x26d7f4u) life2Capture(ram,getRegU32(ctx,17));
    if (target!=0x26d4d8u) return;
    if (source!=0x26e500u && source!=0x270350u && source!=0x2703c8u && source!=0x270464u) return;
    const uint32_t timeline=getRegU32(ctx,5);
    uint32_t ordinal=0u;
    if (life2Ordinal(object,timeline,ordinal)) {SET_GPR_U32(ctx,5,ordinal);return;}
    // Legacy stock recordings remain stock indexed. Legacy 120/mixed clips
    // have no epoch metadata: refuse rather than guess from the paused rate.
    uint32_t length=0u,total=0u;
    if (rd32(ram,getRegU32(ctx,16)+0x3ccu,length) && life2Samples(ram,object,total) && total==length) return;
    std::fprintf(stderr,"life2-refused replay seek without recording metadata input=%x timeline=%u\n",object,timeline);
    std::abort();
}
