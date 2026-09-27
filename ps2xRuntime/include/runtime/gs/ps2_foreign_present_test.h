#pragma once

// GE2: dev-only foreign-VkDevice present test (Android). A raw-Vulkan device
// created outside Granite imports the SurfaceControl sink's AHardwareBuffer
// slots on the shipped Turnip, renders a CPU-checkable test pattern, and
// queues it through the existing sink. Entered from main() after window init
// when PS2X_GS_FOREIGN_PRESENT_TEST=1; the game never boots.
#if defined(__ANDROID__)
#if defined(PS2X_HAS_PARALLEL_SHADOW)
int ps2x_foreign_present_test_run();
#else
// No Vulkan headers without the parallel wiring: the test refuses. APK
// builds always enable PS2X_GS_SHADOW_PARALLEL; this keeps OFF builds green.
inline int ps2x_foreign_present_test_run()
{
    return 1;
}
#endif
#endif
