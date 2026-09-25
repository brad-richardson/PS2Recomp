// SS1 (DEV, default off): save-state section for the syscall-side inline
// globals in Helpers/State.h (RPC, bootmode, TLS, OSD, paths, SIF modules).

#include "Common.h"
#include "runtime/ps2_savestate.h"

namespace
{
    using ps2_savestate::Reader;
    using ps2_savestate::Writer;

    void syscallSave(Writer &w)
    {
        w.u32(static_cast<uint32_t>(g_nextFd));
        ps2_savestate::writeOrdered(w, g_rpc_servers, [](Writer &ww, const auto &e) {
            ww.u32(e.first);
            ww.u32(e.second.sid);
            ww.u32(e.second.sd_ptr);
        });
        ps2_savestate::writeOrdered(w, g_rpc_clients, [](Writer &ww, const auto &e) {
            ww.u32(e.first);
            ww.b(e.second.busy);
            ww.u32(e.second.last_rpc);
            ww.u32(e.second.sid);
        });
        w.b(g_rpc_initialized);
        for (uint32_t v : {g_rpc_next_id, g_rpc_packet_index, g_rpc_server_index, g_rpc_active_queue})
            w.u32(v);
        w.b(g_bootmode_initialized);
        w.u32(g_bootmode_pool_offset);
        ps2_savestate::writeOrderedPod(w, g_bootmode_addresses);
        w.u32(g_tls_index);
        w.b(g_osd_config_initialized);
        w.u32(g_osd_config_raw);
        w.u32(g_osd_config2_raw);
        w.b(g_ps2_paths_initialized);
        for (const auto *p : {&g_host_base, &g_cdrom_base, &g_host_cwd, &g_cdrom_cwd})
            w.str(p->string());
        w.str(g_ps2_cwd_device);
        ps2_savestate::writeOrdered(w, g_sif_modules_by_id, [](Writer &ww, const auto &e) {
            ww.pod(e.first);
            ww.pod(e.second.id);
            ww.str(e.second.path);
            ww.str(e.second.pathKey);
            ww.u32(e.second.refCount);
            ww.b(e.second.loaded);
        });
        ps2_savestate::writeOrdered(w, g_sif_module_id_by_path, [](Writer &ww, const auto &e) {
            ww.str(e.first);
            ww.pod(e.second);
        });
        w.pod(g_next_sif_module_id);
    }

    bool syscallLoad(Reader &r)
    {
        g_nextFd = static_cast<int>(r.u32());
        ps2_savestate::readOrdered(r, g_rpc_servers, [](Reader &rr, auto &e) {
            e.first = rr.u32();
            e.second.sid = rr.u32();
            e.second.sd_ptr = rr.u32();
        });
        ps2_savestate::readOrdered(r, g_rpc_clients, [](Reader &rr, auto &e) {
            e.first = rr.u32();
            e.second.busy = rr.b();
            e.second.last_rpc = rr.u32();
            e.second.sid = rr.u32();
        });
        g_rpc_initialized = r.b();
        for (uint32_t *v : {&g_rpc_next_id, &g_rpc_packet_index, &g_rpc_server_index, &g_rpc_active_queue})
            *v = r.u32();
        g_bootmode_initialized = r.b();
        g_bootmode_pool_offset = r.u32();
        ps2_savestate::readOrderedPod(r, g_bootmode_addresses);
        g_tls_index = r.u32();
        g_osd_config_initialized = r.b();
        g_osd_config_raw = r.u32();
        g_osd_config2_raw = r.u32();
        g_ps2_paths_initialized = r.b();
        for (auto *p : {&g_host_base, &g_cdrom_base, &g_host_cwd, &g_cdrom_cwd})
            *p = r.str();
        g_ps2_cwd_device = r.str();
        ps2_savestate::readOrdered(r, g_sif_modules_by_id, [](Reader &rr, auto &e) {
            rr.pod(e.first);
            rr.pod(e.second.id);
            e.second.path = rr.str();
            e.second.pathKey = rr.str();
            e.second.refCount = rr.u32();
            e.second.loaded = rr.b();
        });
        ps2_savestate::readOrdered(r, g_sif_module_id_by_path, [](Reader &rr, auto &e) {
            e.first = rr.str();
            rr.pod(e.second);
        });
        r.pod(g_next_sif_module_id);
        return r.ok();
    }

    std::string syscallReady()
    {
        if (!g_fileDescriptors.empty())
            return "fio host file descriptors open";
        return {};
    }

    const bool kSyscallSavestateRegistered =
        ps2_savestate::registerSection("syscalls", {1u, &syscallSave, &syscallLoad, &syscallReady});
} // namespace

// Referenced from ps2_savestate.cpp: nothing else uses this object, so the
// static-library link would drop it (and its registration) otherwise.
void ps2_savestate_linkSyscallSection() { (void)kSyscallSavestateRegistered; }
