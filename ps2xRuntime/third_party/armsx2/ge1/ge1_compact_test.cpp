// NRS1: differential suite for the compact record ingest (GSCompactRecord.h,
// GSState::TransferCompact, the GIFCompactHandler* arms) against the GIF kick
// it replaces. Two probes, identical setup, one stream, one arm each, then a
// byte comparison of everything the kick leaves behind (the same list as
// tests/ctest/core/gs/gs_kick_kernel_tests.cpp compares for kernel vs legacy).
//
// No gtest: plain main, prints the first mismatch per case, exits nonzero on
// any failure. Shipped inputs only (synthesized register runs + directed
// edges); with file arguments, each file holds GIF packets (u32 npackets,
// then u32 nbytes + bytes each) dumped from the runtime model on synthetic
// VU1 images (no game data), run as record-level differentials.
//
// Built with the Mac GE1 build (adapter CMake, APPLE non-IOS only).

#include "pcsx2/GS/GS.h"
#include "pcsx2/GS/GSCompactRecord.h"
#include "pcsx2/GS/GSState.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace
{
int g_fail = 0;
int g_cases = 0;

#define CHECK(cond, ...) \
	do \
	{ \
		if (!(cond)) \
		{ \
			std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			std::printf(__VA_ARGS__); \
			std::printf("\n"); \
			g_fail++; \
			return false; \
		} \
	} while (0)

// ---------------------------------------------------------------- stream ---

struct VertexSpec
{
	u16 x = 0, y = 0;
	u32 z = 0;
	u32 rgba = 0; // R in bits 0-7, G 8-15, B 16-23, A 24-31
	float s = 0, t = 0, q = 1;
	u32 fog = 0;
	bool adc = false;
};

// One PACKED {STQ, RGBAQ, XYZF2} record. Same encoding as the vendor kick
// suite's EncodeVertex (xyzf2 = true).
void EncodeVertex(GIFPackedReg* r, const VertexSpec& v)
{
	std::memset(r, 0, sizeof(GIFPackedReg) * 3);

	std::memcpy(&r[0].U32[0], &v.s, sizeof(float));
	std::memcpy(&r[0].U32[1], &v.t, sizeof(float));
	std::memcpy(&r[0].U32[2], &v.q, sizeof(float));

	r[1].U32[0] = (v.rgba >> 0) & 0xFF;
	r[1].U32[1] = (v.rgba >> 8) & 0xFF;
	r[1].U32[2] = (v.rgba >> 16) & 0xFF;
	r[1].U32[3] = (v.rgba >> 24) & 0xFF;

	r[2].U32[0] = v.x;
	r[2].U32[1] = v.y;
	r[2].U32[2] = (v.z & 0x00FFFFFF) << 4;
	r[2].U32[3] = (v.fog & 0xFF) << 4;
	if (v.adc)
		r[2].U32[3] |= 0x8000;
}

// The runtime builder's field extraction (ps2_native_world compact emit),
// mirrored here so the test derives dense vertices from GIF regs the same
// way. The builder itself is pinned against packet bytes by ps2x_tests.
void DenseFromRegs(const GIFPackedReg* r, Ge1CompactVertex& d)
{
	d.S = r[0].U32[0];
	d.T = r[0].U32[1];
	d.RGBA = (r[1].U32[0] & 0xFFu) | ((r[1].U32[1] & 0xFFu) << 8) |
	         ((r[1].U32[2] & 0xFFu) << 16) | ((r[1].U32[3] & 0xFFu) << 24);
	d.Q = r[0].U32[2];
	d.X = r[2].U32[0];
	d.Y = r[2].U32[1];
	d.Z = r[2].U32[2];
	d.W3 = r[2].U32[3];
}

// One GIF tag for a compact-shape packet: PACKED, PRE, NREG 3,
// REGS {STQ, RGBAQ, XYZF2}, given PRIM and NLOOP. EOP set, as the native
// packets carry it.
void EncodeTag(u8 out[16], u32 prim, u32 nloop)
{
	GIFTag tag{};
	tag.NLOOP = nloop;
	tag.EOP = 1;
	tag.PRE = 1;
	tag.PRIM = prim;
	tag.FLG = GIF_FLG_PACKED;
	tag.NREG = 3;
	u64 regs = 0;
	regs |= static_cast<u64>(GIF_REG_STQ) << 0;
	regs |= static_cast<u64>(GIF_REG_RGBA) << 4;
	regs |= static_cast<u64>(GIF_REG_XYZF2) << 8;
	std::memcpy(&tag.REGS, &regs, 8);
	std::memcpy(out, &tag, 16);
}

u32 PrimWord(u32 prim, u32 iip, u32 tme, u32 fst, u32 ctxt, u32 aa1)
{
	GIFRegPRIM p{};
	p.PRIM = prim;
	p.IIP = iip;
	p.TME = tme;
	p.FST = fst;
	p.CTXT = ctxt;
	p.AA1 = aa1;
	u32 w = 0;
	std::memcpy(&w, &p, 4);
	return w;
}

// ----------------------------------------------------------------- setup ---

struct KickSetup
{
	int scissor_x0 = 0, scissor_y0 = 0, scissor_x1 = 639, scissor_y1 = 447;
	u32 ofx = 0, ofy = 0;
	u32 iip = 1, tme = 0, fst = 0, aa1 = 0, ctxt = 0;
	u32 uv = 0; // latched m_v.UV at call entry
	bool draw_buffering = false;
	bool recent_buffer_switch = false;
	int cull_shift = 4;
	bool empty_scissor = false;
	GSHWAutoFlushLevel autoflush = GSHWAutoFlushLevel::Disabled;
	bool autoflush_hit = false;
};

class KickProbe final : public GSState
{
public:
	KickProbe()
		: m_regs_storage(std::make_unique<GSPrivRegSet>())
	{
		std::memset(m_regs_storage.get(), 0, sizeof(GSPrivRegSet));
		m_regs = m_regs_storage.get();
	}

	// A draw is where the environment can move under a run, and the only
	// place the autoflush predicate can move inside a handler call. Every
	// draw moves the scissor, the offset, the shading bits and the TEX0 block,
	// so runs that flush cross midpoints both arms must handle identically
	// (same idea as the vendor kick suite's moving-environment Draw).
	void Draw() override
	{
		m_draws++;
		GSDrawingContext& ctx = m_env.CTXT[m_env.PRIM.CTXT];
		ctx.SCISSOR.SCAX0 = 40 + (m_draws * 13) % 100;
		ctx.SCISSOR.SCAY0 = 30 + (m_draws * 7) % 80;
		ctx.SCISSOR.SCAX1 = 300 + (m_draws * 11) % 200;
		ctx.SCISSOR.SCAY1 = 200 + (m_draws * 5) % 150;
		ctx.XYOFFSET.OFX = (m_draws & 1) ? 0u : 64u;
		ctx.XYOFFSET.OFY = (m_draws & 1) ? 32u : 0u;
		ctx.UpdateScissor();
		m_env.PRIM.IIP = (m_draws & 1) ? 0 : 1;
		m_env.PRIM.TME = (m_draws & 1) ? 1 : 0;
		ctx.TEX0.TBP0 = (m_draws & 1) ? 0u : 0x400u;
		UpdateContext();
	}
	bool IsCoverageAlphaSupported() override { return true; }

	u32 m_draws = 0;
	std::unique_ptr<GSPrivRegSet> m_regs_storage;

	using GSState::GrowVertexBuffer;
	using GSState::m_cull_bounds_band;
	using GSState::m_cull_bounds_raw;
	using GSState::m_env_buffers;
	using GSState::m_index;
	using GSState::m_kick_side_meta;
	using GSState::m_kick_side_xyp;
	using GSState::m_q;
	using GSState::m_track_native_draw_rect;
	using GSState::m_v;
	using GSState::m_vertex;
	using GSState::m_xyof;
	using GSState::s_fused_kick_use_kernel;

	void Configure(const KickSetup& s)
	{
		GSDrawingContext& ctx = m_env.CTXT[s.ctxt];
		if (s.empty_scissor)
		{
			ctx.SCISSOR.SCAX0 = 200;
			ctx.SCISSOR.SCAY0 = 200;
			ctx.SCISSOR.SCAX1 = 100;
			ctx.SCISSOR.SCAY1 = 100;
		}
		else
		{
			ctx.SCISSOR.SCAX0 = static_cast<u32>(s.scissor_x0);
			ctx.SCISSOR.SCAY0 = static_cast<u32>(s.scissor_y0);
			ctx.SCISSOR.SCAX1 = static_cast<u32>(s.scissor_x1);
			ctx.SCISSOR.SCAY1 = static_cast<u32>(s.scissor_y1);
		}
		ctx.XYOFFSET.OFX = s.ofx;
		ctx.XYOFFSET.OFY = s.ofy;
		ctx.UpdateScissor();

		ctx.FRAME.FBP = 0;
		ctx.FRAME.FBW = 10;
		ctx.FRAME.PSM = PSMCT32;
		ctx.FRAME.FBMSK = 0;
		ctx.ZBUF.ZBP = 0x100;
		ctx.ZBUF.PSM = PSMZ32;
		ctx.ZBUF.ZMSK = 1;
		ctx.TEX0.TBP0 = s.autoflush_hit ? 0u : 0x400u;
		ctx.TEX0.TBW = 4;
		ctx.TEX0.PSM = PSMCT32;
		ctx.TEX0.TW = 8;
		ctx.TEX0.TH = 8;
		ctx.TEX0.TCC = 1;
		ctx.TEX0.TFX = 0;
		ctx.TEX1.MXL = 0;
		ctx.TEX1.MMIN = 0;
		ctx.TEX1.LCM = 0;
		ctx.TEST.ATE = 0;
		ctx.CLAMP.WMS = 0;
		ctx.CLAMP.WMT = 0;

		m_env.PRIM.CTXT = s.ctxt;
		m_env.PRIM.IIP = s.iip;
		m_env.PRIM.TME = s.tme;
		m_env.PRIM.FST = s.fst;
		m_env.PRIM.AA1 = s.aa1;

		m_nativeres = (s.cull_shift == 4);
		SetCullGrid(GSVertexKernels::MakeCullGrid(s.cull_shift, (s.cull_shift == 4) ? 4 : 0));
		UpdateContext();
		UpdateVertexKick();

		m_recent_buffer_switch = s.recent_buffer_switch;

		m_v.UV = s.uv;
		m_q = 1.0f;

		temp_draw_rect = GSVector4i::zero();
		temp_native_draw_rect = GSVector4i::zero();
	}

	void SetPrim(u32 prim)
	{
		m_env.PRIM.PRIM = prim;
		UpdateVertexKick();
	}
};

struct AutoFlushScope
{
	GSHWAutoFlushLevel saved;
	bool saved_db;
	explicit AutoFlushScope(const KickSetup& s)
		: saved(GSConfig.UserHacks_AutoFlush), saved_db(GSConfig.UserHacks_DrawBuffering)
	{
		GSConfig.UserHacks_AutoFlush = s.autoflush;
		GSConfig.UserHacks_DrawBuffering = s.draw_buffering;
	}
	~AutoFlushScope()
	{
		GSConfig.UserHacks_AutoFlush = saved;
		GSConfig.UserHacks_DrawBuffering = saved_db;
	}
};

// ------------------------------------------------------------ comparison ---

bool SameBytes(const char* what, const void* a, const void* b, size_t n, std::string& err)
{
	const u8* pa = static_cast<const u8*>(a);
	const u8* pb = static_cast<const u8*>(b);
	for (size_t i = 0; i < n; i++)
	{
		if (pa[i] != pb[i])
		{
			char buf[256];
			std::snprintf(buf, sizeof(buf), "%s differs at byte %zu: gif %u compact %u",
				what, i, pa[i], pb[i]);
			err = buf;
			return false;
		}
	}
	return true;
}

// Same relationship-comparison as the vendor kick suite: two GSState objects
// hold different POINTERS in the offset tables, so the pointer windows are
// excluded and the transfer-register window (flush-path residue) with them.
bool SameEnvCopy(const char* what, const GSDrawingEnvironment& ca,
	const GSDrawingEnvironment& live_a, const GSDrawingEnvironment& live_b,
	const GSDrawingEnvironment& cb, std::string& err)
{
	const u8* pa = reinterpret_cast<const u8*>(&ca);
	const u8* pb = reinterpret_cast<const u8*>(&cb);
	const u8* la = reinterpret_cast<const u8*>(&live_a);
	const u8* lb = reinterpret_cast<const u8*>(&live_b);

	const u8* env_base = reinterpret_cast<const u8*>(&live_a);
	size_t skip_lo[3], skip_hi[3];
	for (int i = 0; i < 2; i++)
	{
		skip_lo[i] = static_cast<size_t>(reinterpret_cast<const u8*>(&live_a.CTXT[i].offset) - env_base);
		skip_hi[i] = skip_lo[i] + sizeof(live_a.CTXT[i].offset);
	}
	skip_lo[2] = static_cast<size_t>(reinterpret_cast<const u8*>(&live_a.BITBLTBUF) - env_base);
	skip_hi[2] = static_cast<size_t>(reinterpret_cast<const u8*>(&live_a.CTXT[0]) - env_base);

	for (size_t i = 0; i < sizeof(GSDrawingEnvironment); i++)
	{
		if ((i >= skip_lo[0] && i < skip_hi[0]) || (i >= skip_lo[1] && i < skip_hi[1]) ||
			(i >= skip_lo[2] && i < skip_hi[2]))
			continue;

		const bool carried_a = (pa[i] == la[i]);
		const bool carried_b = (pb[i] == lb[i]);
		if (carried_a != carried_b)
		{
			char buf[256];
			std::snprintf(buf, sizeof(buf), "%s: byte %zu matches live on the %s arm only",
				what, i, carried_a ? "gif" : "compact");
			err = buf;
			return false;
		}

		if (pa[i] != pb[i])
		{
			char buf[256];
			std::snprintf(buf, sizeof(buf), "%s differs at byte %zu: gif %u compact %u",
				what, i, pa[i], pb[i]);
			err = buf;
			return false;
		}
	}
	return true;
}

bool ExpectSameKickResult(KickProbe& a, KickProbe& b, std::string& err)
{
	char buf[256];
#define EQW(x, y, what) \
	if ((x) != (y)) \
	{ \
		std::snprintf(buf, sizeof(buf), "%s: gif %u compact %u", what, (u32)(x), (u32)(y)); \
		err = buf; \
		return false; \
	}
	EQW(a.m_vertex->head, b.m_vertex->head, "head");
	EQW(a.m_vertex->tail, b.m_vertex->tail, "tail");
	EQW(a.m_vertex->next, b.m_vertex->next, "next");
	EQW(a.m_vertex->xy_tail, b.m_vertex->xy_tail, "xy_tail");
	EQW(a.m_index->tail, b.m_index->tail, "index tail");
#undef EQW

	if (!SameBytes("vertex buffer [0, tail)", a.m_vertex->buff, b.m_vertex->buff,
			sizeof(GSVertex) * a.m_vertex->tail, err))
		return false;
	if (!SameBytes("vertex buffer [0, max(tail,next))", a.m_vertex->buff, b.m_vertex->buff,
			sizeof(GSVertex) * std::max(a.m_vertex->tail, a.m_vertex->next), err))
		return false;
	if (!SameBytes("index buffer [0, itail)", a.m_index->buff, b.m_index->buff,
			sizeof(u16) * a.m_index->tail, err))
		return false;

	if (!SameBytes("xy ring", a.m_vertex->xy, b.m_vertex->xy, sizeof(a.m_vertex->xy), err))
		return false;
	if (!SameBytes("kick_ring", a.m_vertex->kick_ring, b.m_vertex->kick_ring,
			sizeof(a.m_vertex->kick_ring), err))
		return false;
	if (!SameBytes("xyhead", &a.m_vertex->xyhead, &b.m_vertex->xyhead, sizeof(GSVector4i), err))
		return false;

	if (a.m_vertex->fmm_valid != b.m_vertex->fmm_valid)
	{
		err = "fmm_valid";
		return false;
	}
	if (a.m_vertex->fmm_watermark != b.m_vertex->fmm_watermark)
	{
		err = "fmm_watermark";
		return false;
	}
	if (a.m_vertex->fmm_valid && b.m_vertex->fmm_valid)
	{
		if (!SameBytes("fmm_acc", &a.m_vertex->fmm_acc, &b.m_vertex->fmm_acc,
				sizeof(GSVertexKernels::FmmAcc), err))
			return false;
	}

	if (!SameBytes("temp_draw_rect", &a.temp_draw_rect, &b.temp_draw_rect, sizeof(GSVector4i), err))
		return false;
	if (!SameBytes("temp_native_draw_rect", &a.temp_native_draw_rect,
			&b.temp_native_draw_rect, sizeof(GSVector4i), err))
		return false;

	if (!SameEnvCopy("m_prev_env", a.m_prev_env, a.m_env, b.m_env, b.m_prev_env, err))
		return false;
	if (a.m_dirty_gs_regs != b.m_dirty_gs_regs)
	{
		err = "m_dirty_gs_regs";
		return false;
	}
	if (a.m_backed_up_ctx != b.m_backed_up_ctx)
	{
		err = "m_backed_up_ctx";
		return false;
	}
	if (!SameEnvCopy("draw-buffer env snapshot", a.m_env_buffers[0].m_env, a.m_env,
			b.m_env, b.m_env_buffers[0].m_env, err))
		return false;
	if (a.m_env_buffers[0].m_backed_up_ctx != b.m_env_buffers[0].m_backed_up_ctx)
	{
		err = "draw-buffer env snapshot ctx";
		return false;
	}

	if (!SameBytes("m_v", &a.m_v, &b.m_v, sizeof(GSVertex), err))
		return false;
	if (a.m_q != b.m_q)
	{
		std::snprintf(buf, sizeof(buf), "m_q: gif %f compact %f", a.m_q, b.m_q);
		err = buf;
		return false;
	}
	if (a.m_draws != b.m_draws)
	{
		std::snprintf(buf, sizeof(buf), "draw count: gif %u compact %u", a.m_draws, b.m_draws);
		err = buf;
		return false;
	}
	return true;
}

// --------------------------------------------------------------- runners ---

// One packet's GIF bytes (tag + regs) and its compact twin.
struct PacketPair
{
	std::vector<u8> gif; // 16 + 48*N
	std::vector<u8> compact; // 16 + 32*N
};

PacketPair MakePair(u32 prim, const std::vector<VertexSpec>& verts)
{
	PacketPair p;
	const u32 n = static_cast<u32>(verts.size());
	p.gif.resize(16 + 48u * n);
	p.compact.resize(16 + 32u * n);
	EncodeTag(p.gif.data(), prim, n);
	std::memcpy(p.compact.data(), p.gif.data(), 16);
	for (u32 i = 0; i < n; i++)
	{
		GIFPackedReg regs[3];
		EncodeVertex(regs, verts[i]);
		std::memcpy(p.gif.data() + 16 + 48u * i, regs, 48);
		Ge1CompactVertex d;
		DenseFromRegs(regs, d);
		std::memcpy(p.compact.data() + 16 + 32u * i, &d, 32);
	}
	return p;
}

std::vector<u8> MakeCompactRecord(const std::vector<PacketPair>& packets)
{
	const u32 count = static_cast<u32>(packets.size());
	const u32 table = (8u + 4u * count + 15u) & ~15u;
	size_t body = 0;
	for (const auto& p : packets)
		body += p.compact.size();
	std::vector<u8> out(16u + table + body);
	const u64 magic[2] = {GE1_COMPACT_RECORD_MAGIC_LO, GE1_COMPACT_RECORD_MAGIC_HI};
	std::memcpy(out.data(), magic, 16);
	std::memset(out.data() + 16, 0, table);
	std::memcpy(out.data() + 16, &count, 4);
	size_t off = 16u + table;
	for (u32 i = 0; i < count; i++)
	{
		const u32 n = static_cast<u32>(packets[i].compact.size());
		std::memcpy(out.data() + 24 + 4 * i, &n, 4);
		std::memcpy(out.data() + off, packets[i].compact.data(), n);
		off += n;
	}
	return out;
}

std::unique_ptr<KickProbe> MakeProbe(const KickSetup& s, u32 prim, bool use_kernel)
{
	auto p = std::make_unique<KickProbe>();
	KickProbe::s_fused_kick_use_kernel = use_kernel;
	p->ResetHandlers();
	p->Configure(s);
	p->SetPrim(prim);
	return p;
}

// GIF arm: one Transfer<3> per packet (whole-packet calls, as the runtime
// delivers them). Compact arm: one TransferCompact with the whole record.
bool RunRecordPair(const KickSetup& setup, u32 prim, const std::vector<PacketPair>& packets,
	bool use_kernel, std::string& err)
{
	AutoFlushScope af(setup);
	auto a = MakeProbe(setup, prim, use_kernel);
	auto b = MakeProbe(setup, prim, use_kernel);
	for (const auto& p : packets)
		a->Transfer<3>(p.gif.data(), static_cast<u32>(p.gif.size() / 16));
	const std::vector<u8> rec = MakeCompactRecord(packets);
	if (!b->TransferCompact(rec.data(), static_cast<u32>(rec.size())))
	{
		err = "TransferCompact refused a well-formed record";
		return false;
	}
	return ExpectSameKickResult(*a, *b, err);
}

// Same stream, split across calls: GIF arm one Transfer per packet, compact
// arm one TransferCompact per packet-sized record. Exercises the per-call
// snapshot rule on both sides.
bool RunSplitPair(const KickSetup& setup, u32 prim, const std::vector<PacketPair>& packets,
	bool use_kernel, std::string& err)
{
	AutoFlushScope af(setup);
	auto a = MakeProbe(setup, prim, use_kernel);
	auto b = MakeProbe(setup, prim, use_kernel);
	for (const auto& p : packets)
	{
		a->Transfer<3>(p.gif.data(), static_cast<u32>(p.gif.size() / 16));
		const std::vector<PacketPair> one(1, p);
		const std::vector<u8> rec = MakeCompactRecord(one);
		if (!b->TransferCompact(rec.data(), static_cast<u32>(rec.size())))
		{
			err = "TransferCompact refused a well-formed record";
			return false;
		}
	}
	return ExpectSameKickResult(*a, *b, err);
}

bool RunCase(const char* name, const KickSetup& setup, u32 prim,
	const std::vector<PacketPair>& packets, bool use_kernel)
{
	g_cases++;
	std::string err;
	if (!RunRecordPair(setup, prim, packets, use_kernel, err))
	{
		std::printf("FAIL %s (whole): %s\n", name, err.c_str());
		g_fail++;
		return false;
	}
	if (packets.size() > 1 && !RunSplitPair(setup, prim, packets, use_kernel, err))
	{
		std::printf("FAIL %s (split): %s\n", name, err.c_str());
		g_fail++;
		return false;
	}
	// The expansion round-trip: every consumed byte of the GIF packets must
	// survive the compact round trip (tag verbatim, ST.w0-2, RGBAQ low bytes,
	// XYZF2 verbatim).
	for (size_t pi = 0; pi < packets.size(); pi++)
	{
		const PacketPair& p = packets[pi];
		const u32 n = static_cast<u32>((p.compact.size() - 16) / 32);
		std::vector<u8> exp(16 + 48u * n);
		if (!ge1_compact_expand_packet(p.compact.data(), static_cast<u32>(p.compact.size()),
				exp.data(), static_cast<u32>(exp.size())))
		{
			std::printf("FAIL %s: expansion refused packet %zu\n", name, pi);
			g_fail++;
			return false;
		}
		if (std::memcmp(exp.data(), p.gif.data(), 16) != 0)
		{
			std::printf("FAIL %s: expansion tag differs packet %zu\n", name, pi);
			g_fail++;
			return false;
		}
		for (u32 i = 0; i < n; i++)
		{
			const u32* g = reinterpret_cast<const u32*>(p.gif.data() + 16 + 48u * i);
			const u32* e = reinterpret_cast<const u32*>(exp.data() + 16 + 48u * i);
			const bool same = g[0] == e[0] && g[1] == e[1] && g[2] == e[2] &&
				(g[4] & 0xFFu) == e[4] && (g[5] & 0xFFu) == e[5] && (g[6] & 0xFFu) == e[6] &&
				(g[7] & 0xFFu) == e[7] && g[8] == e[8] && g[9] == e[9] && g[10] == e[10] &&
				g[11] == e[11];
			if (!same)
			{
				std::printf("FAIL %s: expansion consumed bytes differ packet %zu vert %u\n",
					name, pi, i);
				g_fail++;
				return false;
			}
		}
	}
	return true;
}

// A strip shaped like the terrain packets: 2-row strips, ADC on each strip's
// first two vertices, positions on a grid inside the guard band.
std::vector<VertexSpec> StripVerts(u32 n, u32 x0 = 100, u32 y0 = 100, u32 step = 4)
{
	std::vector<VertexSpec> v;
	v.reserve(n);
	for (u32 i = 0; i < n; i++)
	{
		VertexSpec s;
		const u32 col = (i / 2) % 8, row = (i / 2) / 8;
		s.x = static_cast<u16>(x0 + col * step);
		s.y = static_cast<u16>(y0 + row * step + (i & 1) * step);
		s.z = 0x123456;
		s.rgba = 0x80808080u;
		s.s = 0.25f * col;
		s.t = 0.25f * row;
		s.q = 1.0f;
		s.fog = 0;
		s.adc = (i % 16) < 2;
		v.push_back(s);
	}
	return v;
}

void DirectedCases()
{
	const u32 strip = PrimWord(GS_TRIANGLESTRIP, 1, 0, 0, 0, 0);
	const KickSetup base;
	// Counts across every routing seam: the kernel threshold (6), the chunk
	// size (128) and the flush horizon (200+).
	for (u32 n : {1u, 2u, 3u, 5u, 6u, 7u, 31u, 64u, 127u, 128u, 129u, 200u, 300u})
	{
		char name[64];
		std::snprintf(name, sizeof(name), "strip-n%u", n);
		for (bool kernel : {true, false})
			RunCase(name, base, GS_TRIANGLESTRIP, {MakePair(strip, StripVerts(n))}, kernel);
	}
	// Every prim the tag can carry (kernel-carried ones through the kernel,
	// the rest through the legacy/staged twins).
	for (u32 prim : {GS_POINTLIST, GS_LINELIST, GS_LINESTRIP, GS_TRIANGLELIST,
			 GS_TRIANGLESTRIP, GS_TRIANGLEFAN, GS_SPRITE})
	{
		char name[64];
		std::snprintf(name, sizeof(name), "prim-%u", prim);
		const u32 pw = PrimWord(prim, 1, 0, 0, 0, 0);
		RunCase(name, base, prim, {MakePair(pw, StripVerts(17))}, true);
		RunCase(name, base, prim, {MakePair(pw, StripVerts(17))}, false);
	}
	// Multi-packet records with mixed sizes and PRIMs.
	{
		const u32 list = PrimWord(GS_TRIANGLELIST, 0, 1, 0, 0, 0);
		const std::vector<PacketPair> pk = {
			MakePair(strip, StripVerts(24)),
			MakePair(list, StripVerts(3, 300, 300, 8)),
			MakePair(strip, StripVerts(130)),
		};
		RunCase("multi", base, GS_TRIANGLESTRIP, pk, true);
		RunCase("multi", base, GS_TRIANGLESTRIP, pk, false);
	}
	// NLOOP 0: the tag-only packet outputs nothing but still walks the tag.
	{
		const std::vector<PacketPair> pk = {
			MakePair(strip, StripVerts(9)),
			MakePair(strip, std::vector<VertexSpec>()),
			MakePair(strip, StripVerts(9, 500, 100, 4)),
		};
		RunCase("nloop0", base, GS_TRIANGLESTRIP, pk, true);
	}
	// ADC/Q edges: every vertex skipped, Q +0.0 (fixup) and -0.0 (passes).
	{
		std::vector<VertexSpec> v = StripVerts(12);
		for (auto& s : v)
			s.adc = true;
		RunCase("all-adc", base, GS_TRIANGLESTRIP, {MakePair(strip, v)}, true);

		std::vector<VertexSpec> q = StripVerts(8);
		q[0].q = 0.0f;
		q[1].q = -0.0f;
		q[2].q = 1e-30f;
		RunCase("q-edges", base, GS_TRIANGLESTRIP, {MakePair(strip, q)}, true);
	}
	// Degenerate tris (coincident positions) and scissor edges.
	{
		std::vector<VertexSpec> v = StripVerts(9);
		v[4].x = v[3].x;
		v[4].y = v[3].y;
		RunCase("degenerate", base, GS_TRIANGLESTRIP, {MakePair(strip, v)}, true);

		KickSetup sc = base;
		sc.scissor_x0 = 100;
		sc.scissor_y0 = 100;
		sc.scissor_x1 = 131;
		sc.scissor_y1 = 131;
		RunCase("scissor-edge", sc, GS_TRIANGLESTRIP, {MakePair(strip, StripVerts(40))}, true);
		RunCase("scissor-edge", sc, GS_TRIANGLESTRIP, {MakePair(strip, StripVerts(40))}, false);

		KickSetup empty = base;
		empty.empty_scissor = true;
		RunCase("scissor-empty", empty, GS_TRIANGLESTRIP, {MakePair(strip, StripVerts(20))}, true);
		RunCase("scissor-empty", empty, GS_TRIANGLESTRIP, {MakePair(strip, StripVerts(20))}, false);

		KickSetup ofs = base;
		ofs.ofx = 64;
		ofs.ofy = 32;
		RunCase("xyoffset", ofs, GS_TRIANGLESTRIP, {MakePair(strip, StripVerts(20))}, true);
	}
	// Latched UV, shading modes, buffering, cull grids, autoflush.
	{
		KickSetup s = base;
		s.uv = 0x02A00140u;
		RunCase("uv-latched", s, GS_TRIANGLESTRIP, {MakePair(strip, StripVerts(20))}, true);

		for (u32 iip : {0u, 1u})
			for (u32 tme : {0u, 1u})
				for (u32 fst : {0u, 1u})
				{
					KickSetup m = base;
					m.iip = iip;
					m.tme = tme;
					m.fst = fst;
					char name[64];
					std::snprintf(name, sizeof(name), "shade-%u%u%u", iip, tme, fst);
					const u32 pw = PrimWord(GS_TRIANGLESTRIP, iip, tme, fst, 0, 0);
					RunCase(name, m, GS_TRIANGLESTRIP, {MakePair(pw, StripVerts(25))}, true);
				}

		KickSetup db = base;
		db.draw_buffering = true;
		RunCase("drawbuf", db, GS_TRIANGLESTRIP, {MakePair(strip, StripVerts(60))}, true);

		KickSetup sw = base;
		sw.draw_buffering = true;
		sw.recent_buffer_switch = true;
		RunCase("bufswitch", sw, GS_TRIANGLESTRIP, {MakePair(strip, StripVerts(60))}, true);

		KickSetup g2 = base;
		g2.cull_shift = 2;
		RunCase("grid-2x", g2, GS_TRIANGLESTRIP, {MakePair(strip, StripVerts(60))}, true);

		// RZV1 S4c: also without the knob (the legacy shift-0 cull is the reference then).
		{
			KickSetup g0 = base;
			g0.cull_shift = 0;
			RunCase("grid-shift0", g0, GS_TRIANGLESTRIP, {MakePair(strip, StripVerts(60))}, true);
		}

		for (auto level : {GSHWAutoFlushLevel::SpritesOnly, GSHWAutoFlushLevel::Enabled})
		{
			for (bool hit : {false, true})
			{
				KickSetup as = base;
				as.autoflush = level;
				as.autoflush_hit = hit;
				char name[64];
				std::snprintf(name, sizeof(name), "autoflush-%d-hit%d",
					level == GSHWAutoFlushLevel::Enabled ? 2 : 1, hit ? 1 : 0);
				RunCase(name, as, GS_TRIANGLESTRIP, {MakePair(strip, StripVerts(70))}, true);
				// Sprites stay on the staged loop under autoflush: the per-prim
				// predicate path.
				const u32 sp = PrimWord(GS_SPRITE, 1, 0, 0, 0, 0);
				RunCase(name, as, GS_SPRITE, {MakePair(sp, StripVerts(8))}, true);
			}
		}
	}
}

// Kernel-level differential: RunChunkDense vs RunChunk on the same inputs,
// counts 1..140, both clamp instantiations, several seeds. This is the
// PassOneDense coverage (including the depth-clamp path, which the
// record-level tests can only reach when the probe's ZBUF enables it).
bool RunKernelPair(u32 prim, u32 count, bool clamp_on, u32 seed, std::string& err)
{
	KickSetup setup;
	AutoFlushScope af(setup);
	auto a = MakeProbe(setup, prim, true);
	auto b = MakeProbe(setup, prim, true);

	// Primer: the kernel's contract needs itail != 0 (the driver's snapshot
	// seam establishes it), so one accepted triangle goes through the GIF
	// path on both probes first.
	{
		PacketPair primer = MakePair(PrimWord(prim, 1, 0, 0, 0, 0), StripVerts(3));
		a->Transfer<3>(primer.gif.data(), static_cast<u32>(primer.gif.size() / 16));
		b->Transfer<3>(primer.gif.data(), static_cast<u32>(primer.gif.size() / 16));
	}

	std::mt19937 rng(seed);
	std::vector<GIFPackedReg> regs(count * 3);
	std::vector<Ge1CompactVertex> dense(count);
	for (u32 i = 0; i < count; i++)
	{
		VertexSpec s;
		s.x = static_cast<u16>(rng() % 700);
		s.y = static_cast<u16>(rng() % 500);
		s.z = rng() & 0xFFFFFFu;
		s.rgba = rng();
		s.s = (rng() % 2000) * 0.001f;
		s.t = (rng() % 2000) * 0.001f;
		s.q = (i % 7 == 0) ? 0.0f : 1.0f;
		s.fog = rng() & 0xFFu;
		s.adc = (rng() % 5) == 0;
		EncodeVertex(&regs[i * 3], s);
		DenseFromRegs(&regs[i * 3], dense[i]);
	}

	// The driver's invariant build for a TripleXYZF2 call, both arms (every
	// field the kernel reads, mirrored from KickPackedBatchKernel).
	auto buildInv = [&](KickProbe& p) {
		GSVertexKickKernel::Invariants inv{};
		u64 uvfog = 0;
		std::memcpy(&uvfog, &p.m_v.UV, sizeof(uvfog));
		inv.uvfog = uvfog;
		inv.clamp_enabled = clamp_on;
		GSVertexKickKernel::MakeDepthClampMasks(
			clamp_on ? GSLimit24BitDepth::PrioritizeLower : GSLimit24BitDepth::Disabled,
			inv.clamp_keep, inv.clamp_shifted);
		inv.last_out = &p.m_v;
		inv.track_native_rect = p.m_track_native_draw_rect;
		inv.xyof = p.m_xyof;
		inv.grid = p.m_cull_grid;
		inv.bounds = (inv.grid.shift == 4) ? p.m_cull_bounds_band : p.m_cull_bounds_raw;
		inv.shift0_keepall = false; // the kernel cases run shift-4 grids only
		inv.shade = (p.m_env.PRIM.TME ? 1u : 0u) | (p.m_env.PRIM.FST ? 2u : 0u) |
		            (p.m_env.PRIM.IIP ? 4u : 0u);
		inv.sprite_q_fix = (prim == GS_SPRITE) && (p.m_env.PRIM.FST == 0);
		inv.carry_m0 = GSVector4i::zero(); // the triple never reads it
		return inv;
	};
	GSVertexKickKernel::Invariants inva = buildInv(*a), invb = buildInv(*b);

	// Room for the chunk plus slack, as the driver reserves.
	while ((a->m_vertex->tail + count + 3) > a->m_vertex->maxcount)
		a->GrowVertexBuffer();
	while ((b->m_vertex->tail + count + 3) > b->m_vertex->maxcount)
		b->GrowVertexBuffer();

	u32 acc_a = 0, acc_b = 0;
	GSVector4i nat_a = GSVector4i::zero(), nat_b = GSVector4i::zero();
	GSVector4i ra, rb;
	switch (prim)
	{
		case GS_TRIANGLESTRIP:
			ra = GSVertexKickKernel::RunChunk<GS_TRIANGLESTRIP,
				GSVertexKernels::PackedLayout::TripleXYZF2>(regs.data(), count, a->m_vertex,
				a->m_index, a->m_kick_side_xyp, a->m_kick_side_meta, inva, &acc_a, &nat_a);
			rb = GSVertexKickKernel::RunChunkDense<GS_TRIANGLESTRIP>(dense.data(), count,
				b->m_vertex, b->m_index, b->m_kick_side_xyp, b->m_kick_side_meta, invb, &acc_b,
				&nat_b);
			break;
		case GS_TRIANGLELIST:
			ra = GSVertexKickKernel::RunChunk<GS_TRIANGLELIST,
				GSVertexKernels::PackedLayout::TripleXYZF2>(regs.data(), count, a->m_vertex,
				a->m_index, a->m_kick_side_xyp, a->m_kick_side_meta, inva, &acc_a, &nat_a);
			rb = GSVertexKickKernel::RunChunkDense<GS_TRIANGLELIST>(dense.data(), count,
				b->m_vertex, b->m_index, b->m_kick_side_xyp, b->m_kick_side_meta, invb, &acc_b,
				&nat_b);
			break;
		default: // GS_SPRITE
			ra = GSVertexKickKernel::RunChunk<GS_SPRITE,
				GSVertexKernels::PackedLayout::TripleXYZF2>(regs.data(), count, a->m_vertex,
				a->m_index, a->m_kick_side_xyp, a->m_kick_side_meta, inva, &acc_a, &nat_a);
			rb = GSVertexKickKernel::RunChunkDense<GS_SPRITE>(dense.data(), count, b->m_vertex,
				b->m_index, b->m_kick_side_xyp, b->m_kick_side_meta, invb, &acc_b, &nat_b);
			break;
	}
	if (acc_a != acc_b)
	{
		err = "acc_state";
		return false;
	}
	if (!SameBytes("acc_rect", &ra, &rb, sizeof(GSVector4i), err))
		return false;
	if (!SameBytes("native_acc", &nat_a, &nat_b, sizeof(GSVector4i), err))
		return false;
	return ExpectSameKickResult(*a, *b, err);
}

void KernelCases()
{
	for (u32 prim : {GS_TRIANGLESTRIP, GS_TRIANGLELIST, GS_SPRITE})
	{
		for (bool clamp_on : {false, true})
		{
			// One chunk at most: the driver never calls the kernel with more
			// (multi-chunk runs are the record-level cases' job).
			for (u32 count = 1; count <= GSVertexKickKernel::kChunkVertices; count++)
			{
				g_cases++;
				std::string err;
				if (!RunKernelPair(prim, count, clamp_on, 1000 + count, err))
				{
					std::printf("FAIL kernel prim=%u count=%u clamp=%d: %s\n",
						prim, count, clamp_on ? 1 : 0, err.c_str());
					g_fail++;
				}
			}
		}
	}
}

// Malformed records refuse the whole call without changing any state.
bool RunMalformedCase(const char* name, const std::vector<u8>& rec)
{
	g_cases++;
	KickSetup setup;
	AutoFlushScope af(setup);
	auto a = MakeProbe(setup, GS_TRIANGLESTRIP, true);
	auto b = MakeProbe(setup, GS_TRIANGLESTRIP, true);
	// Dirty the state first, so a partial ingest would show.
	auto dirt = StripVerts(11);
	const std::vector<PacketPair> pk = {
		MakePair(PrimWord(GS_TRIANGLESTRIP, 1, 0, 0, 0, 0), dirt)};
	a->Transfer<3>(pk[0].gif.data(), static_cast<u32>(pk[0].gif.size() / 16));
	b->Transfer<3>(pk[0].gif.data(), static_cast<u32>(pk[0].gif.size() / 16));
	if (b->TransferCompact(rec.data(), static_cast<u32>(rec.size())))
	{
		std::printf("FAIL %s: malformed record accepted\n", name);
		g_fail++;
		return false;
	}
	std::string err;
	if (!ExpectSameKickResult(*a, *b, err))
	{
		std::printf("FAIL %s: refused call changed state: %s\n", name, err.c_str());
		g_fail++;
		return false;
	}
	return true;
}

void MalformedCases()
{
	const u32 strip = PrimWord(GS_TRIANGLESTRIP, 1, 0, 0, 0, 0);
	const std::vector<PacketPair> pk = {MakePair(strip, StripVerts(9))};
	const std::vector<u8> good = MakeCompactRecord(pk);
	auto mutate = [&](auto fn) {
		std::vector<u8> r = good;
		fn(r);
		return r;
	};
	RunMalformedCase("bad-magic", mutate([](std::vector<u8>& r) { r[0] ^= 0xFF; }));
	RunMalformedCase("truncated", mutate([](std::vector<u8>& r) { r.resize(r.size() - 16); }));
	RunMalformedCase("count-0", mutate([](std::vector<u8>& r) {
		r[16] = r[17] = r[18] = r[19] = 0;
	}));
	RunMalformedCase("count-65", mutate([](std::vector<u8>& r) {
		r[16] = 65;
		r[17] = r[18] = r[19] = 0;
	}));
	RunMalformedCase("bad-size", mutate([](std::vector<u8>& r) {
		r[24] = 24;
		r[25] = r[26] = r[27] = 0;
	}));
	// Tag shape: FLG, NREG, REGS, PRE each broken once.
	RunMalformedCase("tag-reglist", mutate([](std::vector<u8>& r) {
		// Tag starts at 16 + table(16) = 32; FLG is tag bits 58-59 (byte 7 bits 2-3).
		r[32 + 7] = (r[32 + 7] & 0xF3) | 0x04; // PACKED(0) -> REGLIST(1)
	}));
	RunMalformedCase("tag-nreg2", mutate([](std::vector<u8>& r) {
		r[32 + 7] = (r[32 + 7] & 0x0F) | 0x20; // NREG 3 -> 2
	}));
	RunMalformedCase("tag-regs", mutate([](std::vector<u8>& r) {
		r[32 + 8] ^= 0x01; // first REGS descriptor STQ(2) -> RGBAQ(1)... any change breaks the triple
	}));
	RunMalformedCase("tag-nopre", mutate([](std::vector<u8>& r) {
		r[32 + 5] &= 0xBF; // PRE is tag bit 46 (byte 5 bit 6)
	}));
	// NLOOP/tag-count mismatch.
	RunMalformedCase("nloop-mismatch", mutate([](std::vector<u8>& r) {
		r[32] ^= 0x01; // NLOOP 9 -> 8, vertices still 9
	}));
}

// Randomized sweeps over stream shapes and environments.
// RZV1 S4c: shift0 = every stream at cull shift 0 (non-power-of-two upscale).
void RandomCases(u32 seed0, u32 nstreams, bool shift0 = false)
{
	std::mt19937 rng(seed0);
	const u32 prims[7] = {GS_POINTLIST, GS_LINELIST, GS_LINESTRIP, GS_TRIANGLELIST,
		GS_TRIANGLESTRIP, GS_TRIANGLEFAN, GS_SPRITE};
	for (u32 s = 0; s < nstreams; s++)
	{
		KickSetup setup;
		setup.scissor_x0 = rng() % 100;
		setup.scissor_y0 = rng() % 100;
		setup.scissor_x1 = 400 + rng() % 240;
		setup.scissor_y1 = 300 + rng() % 148;
		setup.ofx = (rng() % 3) * 32;
		setup.ofy = (rng() % 3) * 32;
		setup.iip = rng() & 1;
		setup.tme = rng() & 1;
		setup.fst = rng() & 1;
		setup.uv = (rng() % 3 == 0) ? (rng() & 0x3FFF3FFFu) : 0;
		setup.draw_buffering = (rng() % 3) == 0;
		setup.recent_buffer_switch = setup.draw_buffering && ((rng() % 2) == 0);
		setup.cull_shift = ((rng() % 2) == 0) ? 4 : 2;
		if (shift0)
			setup.cull_shift = 0;
		setup.autoflush = ((rng() % 3) == 0) ? GSHWAutoFlushLevel::Enabled :
		                     (((rng() % 2) == 0) ? GSHWAutoFlushLevel::SpritesOnly :
		                                          GSHWAutoFlushLevel::Disabled);
		setup.autoflush_hit = (rng() % 2) == 0;
		const u32 prim = prims[rng() % 7];
		const u32 npackets = 1 + rng() % 3;
		std::vector<PacketPair> packets;
		for (u32 k = 0; k < npackets; k++)
		{
			const u32 n = 1 + rng() % 150;
			std::vector<VertexSpec> v;
			v.reserve(n);
			for (u32 i = 0; i < n; i++)
			{
				VertexSpec vs;
				vs.x = static_cast<u16>(rng() % 700);
				vs.y = static_cast<u16>(rng() % 500);
				vs.z = rng() & 0xFFFFFFu;
				vs.rgba = rng();
				vs.s = (rng() % 2000) * 0.01f;
				vs.t = (rng() % 2000) * 0.01f;
				vs.q = ((rng() % 9) == 0) ? 0.0f : 1.0f;
				vs.fog = rng() & 0xFFu;
				vs.adc = ((rng() % 4) == 0);
				v.push_back(vs);
			}
			packets.push_back(MakePair(
				PrimWord(prim, setup.iip, setup.tme, setup.fst, 0, 0), v));
		}
		char name[64];
		std::snprintf(name, sizeof(name), "rand-%u", seed0 + s);
		RunCase(name, setup, prim, packets, (rng() % 2) == 0);
	}
}

// File mode: GIF packets dumped from the runtime model (u32 npackets, then
// u32 nbytes + bytes each). Dense twins are derived, then the record-level
// differential runs.
bool RunFile(const char* path)
{
	std::FILE* f = std::fopen(path, "rb");
	if (!f)
	{
		std::printf("FAIL open %s\n", path);
		g_fail++;
		return false;
	}
	std::fseek(f, 0, SEEK_END);
	const long len = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::vector<u8> buf(len);
	if (len > 0 && std::fread(buf.data(), 1, len, f) != static_cast<size_t>(len))
	{
		std::fclose(f);
		std::printf("FAIL read %s\n", path);
		g_fail++;
		return false;
	}
	std::fclose(f);
	if (len < 4)
	{
		std::printf("FAIL %s: too short\n", path);
		g_fail++;
		return false;
	}
	u32 np = 0;
	std::memcpy(&np, buf.data(), 4);
	size_t off = 4;
	std::vector<PacketPair> packets;
	for (u32 i = 0; i < np; i++)
	{
		if (off + 4 > buf.size())
		{
			std::printf("FAIL %s: truncated at packet %u\n", path, i);
			g_fail++;
			return false;
		}
		u32 n = 0;
		std::memcpy(&n, buf.data() + off, 4);
		off += 4;
		if (n < 16u || (n % 48u) != 16u || off + n > buf.size())
		{
			std::printf("FAIL %s: bad packet %u size %u\n", path, i, n);
			g_fail++;
			return false;
		}
		const u32 nv = (n - 16u) / 48u;
		PacketPair p;
		p.gif.assign(buf.data() + off, buf.data() + off + n);
		p.compact.resize(16 + 32u * nv);
		std::memcpy(p.compact.data(), p.gif.data(), 16);
		for (u32 v = 0; v < nv; v++)
		{
			Ge1CompactVertex d;
			DenseFromRegs(
				reinterpret_cast<const GIFPackedReg*>(p.gif.data() + 16 + 48u * v), d);
			std::memcpy(p.compact.data() + 16 + 32u * v, &d, 32);
		}
		packets.push_back(std::move(p));
		off += n;
	}
	// The tag's PRIM drives the run; the probe's PRIM starts wherever the
	// record leaves it (TransferCompact applies PRE).
	GIFTag tag{};
	std::memcpy(&tag, packets[0].gif.data(), 16);
	KickSetup setup;
	g_cases++;
	std::string err;
	char name[512];
	std::snprintf(name, sizeof(name), "file:%s", path);
	// Model packets whose tags are not the compact shape (the synthetic
	// suite images' tag words are arbitrary) refuse fail-closed, exactly as
	// in production; the compact arm then expands and runs the GIF path,
	// mirroring the frontend fallback. Either way the arms must agree.
	AutoFlushScope af(setup);
	auto a = MakeProbe(setup, tag.PRIM, true);
	auto b = MakeProbe(setup, tag.PRIM, true);
	for (const auto& p : packets)
		a->Transfer<3>(p.gif.data(), static_cast<u32>(p.gif.size() / 16));
	const std::vector<u8> rec = MakeCompactRecord(packets);
	if (!b->TransferCompact(rec.data(), static_cast<u32>(rec.size())))
	{
		for (const auto& p : packets)
		{
			const u32 nv = static_cast<u32>((p.compact.size() - 16) / 32);
			std::vector<u8> exp(16 + 48u * nv);
			if (!ge1_compact_expand_packet(p.compact.data(),
					static_cast<u32>(p.compact.size()), exp.data(),
					static_cast<u32>(exp.size())))
			{
				std::printf("FAIL %s: expansion refused\n", name);
				g_fail++;
				return false;
			}
			b->Transfer<3>(exp.data(), static_cast<u32>(exp.size() / 16));
		}
	}
	if (!ExpectSameKickResult(*a, *b, err))
	{
		std::printf("FAIL %s: %s\n", name, err.c_str());
		g_fail++;
		return false;
	}
	// Second run with compact-shaped tags (same vertex bytes): the real
	// model packets through the compact ingest itself.
	{
		std::vector<PacketPair> norm = packets;
		const u32 strip = PrimWord(GS_TRIANGLESTRIP, 1, 0, 0, 0, 0);
		for (auto& p : norm)
		{
			const u32 nv = static_cast<u32>((p.gif.size() - 16) / 48);
			u8 tagbytes[16];
			EncodeTag(tagbytes, strip, nv);
			std::memcpy(p.gif.data(), tagbytes, 16);
			std::memcpy(p.compact.data(), tagbytes, 16);
		}
		g_cases++;
		char name2[512];
		std::snprintf(name2, sizeof(name2), "file-norm:%s", path);
		std::string err2;
		if (!RunRecordPair(setup, GS_TRIANGLESTRIP, norm, true, err2))
		{
			std::printf("FAIL %s: %s\n", name2, err2.c_str());
			g_fail++;
			return false;
		}
	}
	return true;
}

} // namespace

int main(int argc, char** argv)
{
	// Buddy-rule pins: the dense layout both sides agree on.
	static_assert(sizeof(Ge1CompactVertex) == 32);
	static_assert(offsetof(Ge1CompactVertex, Q) == 12);
	static_assert(offsetof(Ge1CompactVertex, X) == 16);

	if (argc > 1)
	{
		for (int i = 1; i < argc; i++)
			RunFile(argv[i]);
	}
	else
	{
		DirectedCases();
		KernelCases();
		MalformedCases();
		RandomCases(5000, 120);
		RandomCases(9000, 120);
		RandomCases(7000, 120, true); // RZV1 S4c: shift 0
	}
	GSStaticStatsPrint(); // RZV1 S4c: how many packets the static fast path took (GE1_STATIC_FAST=1)
	std::printf("ge1_compact_test: %d cases, %d failures\n", g_cases, g_fail);
	return g_fail ? 1 : 0;
}
