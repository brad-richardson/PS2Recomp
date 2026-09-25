#include "MiniTest.h"
#include <cstdlib>
#include <iostream>

void register_code_generator_tests();
void register_r5900_decoder_tests();
void register_elf_analyzer_tests();
void register_pad_input_tests();
void register_ps2_pad_latch_tests();
void register_ps2_present_fallback_tests();
void register_ps2_present_geometry_tests();
void register_ps2_virtual_pad_tests();
void register_ps2_env_file_tests();
void register_ps2_android_env_tests();
void register_ps2_thread_affinity_tests();
void register_ps2_runtime_io_tests();
void register_ps2_runtime_kernel_tests();
void register_ps2_runtime_interrupt_tests();
void register_ps2_memory_tests();
void register_ps2_vu1_tests();
void register_ps2_vu_tests();
void register_ps2_gs_tests();
void register_ps2_gs_queue_tests();
void register_ps2_gs_replay_tests();
void register_ps2_gs_shadow_tests();
void register_ps2_iop_tests();
void register_ps2_sif_rpc_tests();
void register_ps2_snd_tests();
void register_ps2_sif_dma_tests();
void register_ps2_recompiler_tests();
void register_ps2_runtime_expansion_tests();
void register_ps2_gfx_stats_tests();
void register_ps2_vif_mpg_log_tests();
#if PS2X_ENABLE_DIAG_TAPS
void register_ps2_mpg_src_trace_tests();
void register_ps2_e41_trace_tests();
void register_ps2_e43_trace_tests();
void register_ps2_e44_trace_tests();
#endif
void register_ps2_vu1_trace_tests();
void register_ps2_vu1_entry_trace_tests();
void register_ps2_fpu_semantics_tests();
void register_ps2_fpu_cop2_audit_tests();
void register_ps2_mmi_interleave_tests();
void register_ps2_lwu_tests();
void register_ps2_ee_count_tests();
void register_ps2_vsync_pacer_tests();
void register_ps2_tc1_vf0_tests();
void reset_ps2_test_function_table();

int main()
{
    MiniTest::BeforeEach(reset_ps2_test_function_table);

    register_code_generator_tests();
    register_r5900_decoder_tests();
    register_elf_analyzer_tests();
    register_pad_input_tests();
    register_ps2_pad_latch_tests();
    register_ps2_present_fallback_tests();
    register_ps2_present_geometry_tests();
    register_ps2_virtual_pad_tests();
    register_ps2_env_file_tests();
    register_ps2_android_env_tests();
    register_ps2_thread_affinity_tests();
    register_ps2_runtime_io_tests();
    register_ps2_runtime_kernel_tests();
    register_ps2_runtime_interrupt_tests();
    register_ps2_memory_tests();
    register_ps2_vu1_tests();
    register_ps2_vu_tests();
    register_ps2_gs_tests();
    register_ps2_gs_queue_tests();
    register_ps2_gs_replay_tests();
    register_ps2_gs_shadow_tests();
    register_ps2_iop_tests();
    register_ps2_sif_rpc_tests();
    register_ps2_snd_tests();
    register_ps2_sif_dma_tests();
    register_ps2_recompiler_tests();
    register_ps2_runtime_expansion_tests();
    register_ps2_gfx_stats_tests();
    register_ps2_vif_mpg_log_tests();
#if PS2X_ENABLE_DIAG_TAPS
    register_ps2_mpg_src_trace_tests();
    register_ps2_e41_trace_tests();
    register_ps2_e43_trace_tests();
    register_ps2_e44_trace_tests();
#endif
    register_ps2_vu1_trace_tests();
    register_ps2_vu1_entry_trace_tests();
    register_ps2_fpu_semantics_tests();
    register_ps2_fpu_cop2_audit_tests();
    register_ps2_mmi_interleave_tests();
    register_ps2_lwu_tests();
    register_ps2_ee_count_tests();
    register_ps2_vsync_pacer_tests();
    register_ps2_tc1_vf0_tests();
    int res = MiniTest::Run();
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(res);
}
