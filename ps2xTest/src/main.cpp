#include "MiniTest.h"
#include <cstdlib>
#include <iostream>
#include <string>

void register_code_generator_tests();
void register_r5900_decoder_tests();
void register_elf_analyzer_tests();
void register_pad_input_tests();
void register_ps2_pad_latch_tests();
void register_ps2_present_fallback_tests();
void register_ps2_present_geometry_tests();
void register_ps2_present_vk_ledger_tests();
void register_ps2_present_owner_tests();
void register_ps2_virtual_pad_tests();
void register_ps2_env_file_tests();
void register_ps2_ssx3_course_manifest_tests();
void register_ps2_ssx3_tricky_menu_tests();
void register_ps2_ssx3_tricky_list_tests();
void register_ps2_ssx3_lod_tests();
void register_ps2_native_world_tests();
// module-seam-1: game-module tests, per binary (ps2xTest/CMakeLists.txt).
#if PS2X_TEST_GAME_MODULE_SSX3
void register_ps2_ssx3_anim_pick_guard_tests();
void register_ps2_ssx3_module_tests();
void register_ps2_ssx3_copied_payload_tests();
void register_ps2_ssx3_sif_handshake_tests();
void register_ps2_snd_tests();
void register_ps2_ssx3_tricky_song_provider_tests();
void register_ps2_ssx3_movie_stub_tests();
void register_ps2_input_marker_tests();
void register_ps2_fh1_fix_tests();
void register_ps2_fh1_fh28_tests();
void register_ps2_fh1_life2_tests();
void register_ps2_fh1_savestate_tests();
void register_ps2_fh1_hooktable_tests();
void register_ps2_fh1_flip_tests();
#endif
#if PS2X_TEST_NO_GAME_MODULE
void register_game_module_none_tests();
#endif
void register_ps2_ssx3_tricky_hud_tests();
void register_ps2_ssx3_tricky_song_tests();
void register_ps2_ssx3_tricky_dj_tests();
void register_ps2_ssx3_tricky_gems_tests();
void register_ps2_ssx3_tricky_resetvol_tests();
void register_ps2_ssx3_tricky_layer_tests();
void register_ps2_ssx3_tricky_mc_tests();
void register_ps2_cd_overlay_tests();
void register_ps2_cd_dirpatch_tests();
void register_ps2_android_env_tests();
void register_ps2_knobs_tests();
void register_ps2_settings_overlay_tests();
void register_ps2_log_tests();
void register_ps2_perf_log_tests();
void register_ps2_telemetry_tests();
void register_ps2_input_diag_tests();
void register_ps2_adpf_tests();
void register_ps2_thread_affinity_tests();
void register_ps2_runtime_io_tests();
void register_ps2_runtime_kernel_tests();
void register_ps2_tz_offset_tests();
void register_ps2_runtime_interrupt_tests();
void register_ps2_memory_tests();
void register_ps2_vif_unpack_tests();
void register_ps2_vu1_tests();
void register_ps2_vu_tests();
void register_ps2_vu0_lean_entry_tests();
void register_ps2_ssx3_vis_native_tests();
void register_ps2_gs_tests();
void register_ps2_gs_queue_tests();
void register_ps2_gs_replay_tests();
void register_ps2_gs_external_tests();
void register_ps2_gpu_resolve_tests();
void register_ps2_iop_tests();
void register_ps2_sif_rpc_tests();
void register_ps2_audio_stretch_tests();
void register_ps2_sif_dma_tests();
void register_ps2_net_nolink_tests();
void register_ps2_net_protocol_tests();
void register_ps2_netsync_tests();
void register_np_fix_tests();
void register_ps2_recompiler_tests();
void register_ps2_runtime_expansion_tests();
#if PS2X_ENABLE_DIAG_TAPS
void register_ps2_e41_trace_tests();
#endif
void register_ps2_fpu_semantics_tests();
void register_ps2_fpu_cop2_audit_tests();
void register_ps2_mmi_interleave_tests();
void register_ps2_lwu_tests();
void register_ps2_ee_count_tests();
void register_ps2_vsync_pacer_tests();
void register_ps2_vsync_lock_tests();
void register_ps2_fh1_restamp_tests();
void register_ps2_fh1_replay_rate_tests();
void register_ps2_fh1_stuntchg_tests();
void register_ps2_fh1_jcam2_scratch_tests();
int ps2_fh1_jcam2_protection_probe();
void register_ps2_savestate_tests();
void register_ps2_tc1_vf0_tests();
void register_ps2_ach_tests();
void register_ps2_session_telemetry_tests();
void reset_ps2_test_function_table();
int np_fix_child_main(const std::string &name);

int main(int argc, char **argv)
{
    if (argc == 3 && std::string(argv[1]) == "--np-fix-child")
        return np_fix_child_main(argv[2]);
    if (std::getenv("PS2X_JCAM2_PROBE"))
        return ps2_fh1_jcam2_protection_probe();
    MiniTest::BeforeEach(reset_ps2_test_function_table);

    register_code_generator_tests();
    register_r5900_decoder_tests();
    register_elf_analyzer_tests();
    register_pad_input_tests();
    register_ps2_pad_latch_tests();
    register_ps2_present_fallback_tests();
    register_ps2_present_geometry_tests();
    register_ps2_present_vk_ledger_tests();
    register_ps2_present_owner_tests();
    register_ps2_virtual_pad_tests();
    register_ps2_env_file_tests();
    register_ps2_ssx3_course_manifest_tests();
    register_ps2_ssx3_tricky_menu_tests();
    register_ps2_ssx3_tricky_list_tests();
    register_ps2_ssx3_lod_tests();
    register_ps2_native_world_tests();
#if PS2X_TEST_GAME_MODULE_SSX3
    register_ps2_ssx3_anim_pick_guard_tests();
    register_ps2_ssx3_module_tests();
    register_ps2_ssx3_copied_payload_tests();
    register_ps2_ssx3_sif_handshake_tests();
    register_ps2_snd_tests();
    register_ps2_ssx3_tricky_song_provider_tests();
    register_ps2_ssx3_movie_stub_tests();
    register_ps2_input_marker_tests();
    register_ps2_fh1_fix_tests();
    register_ps2_fh1_fh28_tests();
    register_ps2_fh1_life2_tests();
    register_ps2_fh1_savestate_tests();
    register_ps2_fh1_hooktable_tests();
    register_ps2_fh1_flip_tests();
#endif
#if PS2X_TEST_NO_GAME_MODULE
    register_game_module_none_tests();
#endif
    register_ps2_ssx3_tricky_hud_tests();
    register_ps2_ssx3_tricky_song_tests();
    register_ps2_ssx3_tricky_dj_tests();
    register_ps2_ssx3_tricky_gems_tests();
    register_ps2_ssx3_tricky_resetvol_tests();
    register_ps2_ssx3_tricky_layer_tests();
    register_ps2_ssx3_tricky_mc_tests();
    register_ps2_cd_overlay_tests();
    register_ps2_cd_dirpatch_tests();
    register_ps2_android_env_tests();
    register_ps2_knobs_tests();
    register_ps2_settings_overlay_tests();
    register_ps2_log_tests();
    register_ps2_perf_log_tests();
    register_ps2_telemetry_tests();
    register_ps2_input_diag_tests();
    register_ps2_adpf_tests();
    register_ps2_thread_affinity_tests();
    register_ps2_runtime_io_tests();
    register_ps2_runtime_kernel_tests();
    register_ps2_tz_offset_tests();
    register_ps2_runtime_interrupt_tests();
    register_ps2_memory_tests();
    register_ps2_vif_unpack_tests();
    register_ps2_vu1_tests();
    register_ps2_vu_tests();
    register_ps2_vu0_lean_entry_tests();
    register_ps2_ssx3_vis_native_tests();
    register_ps2_gs_tests();
    register_ps2_gs_queue_tests();
    register_ps2_gs_replay_tests();
    register_ps2_gs_external_tests();
    register_ps2_gpu_resolve_tests();
    register_ps2_iop_tests();
    register_ps2_sif_rpc_tests();
    register_ps2_audio_stretch_tests();
    register_ps2_sif_dma_tests();
    register_ps2_net_nolink_tests();
    register_ps2_net_protocol_tests();
    register_ps2_netsync_tests();
    register_np_fix_tests();
    register_ps2_recompiler_tests();
    register_ps2_runtime_expansion_tests();
#if PS2X_ENABLE_DIAG_TAPS
    register_ps2_e41_trace_tests();
#endif
    register_ps2_fpu_semantics_tests();
    register_ps2_fpu_cop2_audit_tests();
    register_ps2_mmi_interleave_tests();
    register_ps2_lwu_tests();
    register_ps2_ee_count_tests();
    register_ps2_vsync_pacer_tests();
    register_ps2_vsync_lock_tests();
    register_ps2_fh1_restamp_tests();
    register_ps2_fh1_replay_rate_tests();
    register_ps2_fh1_stuntchg_tests();
    register_ps2_fh1_jcam2_scratch_tests();
    register_ps2_savestate_tests();
    register_ps2_tc1_vf0_tests();
    register_ps2_ach_tests();
    register_ps2_session_telemetry_tests();
    int res = MiniTest::Run();
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(res);
}
