// GE2: foreign-VkDevice present test (Android only). Proves the RV14 §6
// mechanism on our side of the PCSX2-GS boundary: a VkDevice the present
// sink does not own (raw Vulkan here; PCSX2's GSDeviceVK later) imports the
// sink's AHardwareBuffer slots on the shipped Turnip HAL, renders into them,
// and queues them through the existing SurfaceControl sink with correct
// barriers, fences and slot retirement. No Granite, no game boot.
//
// Knobs (all dev-only, default off): PS2X_GS_FOREIGN_PRESENT_TEST=1 enters
// from main() after window init; PS2X_GS_TURNIP=1 is required (no silent
// system-driver fallback); PS2X_GS_FOREIGN_FRAMES (default 1000),
// PS2X_GS_FOREIGN_HOLD_S (default 15: last frame stays up for screencap),
// PS2X_GS_FOREIGN_SIZE=WxH (default 1280x720),
// PS2X_GS_FOREIGN_COMPARE_EVERY (default 250: gralloc-lock pixel verify).
#if defined(__ANDROID__)

#include "runtime/gs/ps2_foreign_present_test.h"

#include "runtime/gs/ps2_present_vk.h"

#include <EGL/egl.h>
#include <android/hardware_buffer.h>
#include <android/native_activity.h>
#include <android_native_app_glue.h>
#include <dirent.h>
#include <dlfcn.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" struct android_app *GetAndroidApp(void); // raylib rcore_android.c

namespace
{
// Turnip HAL ABI, same shape as G43's loader in
// ps2_gs_parallel_backend.cpp (kept local so this test never depends on
// Granite/parallel sources).
struct HalModuleMethods
{
    int (*open)(const void *module, const char *id, void **device);
};
struct HalModule
{
    uint32_t tag;
    uint16_t moduleApiVersion;
    uint16_t halApiVersion;
    const char *id;
    const char *name;
    const char *author;
    HalModuleMethods *methods;
};
struct HalDevice
{
    uint32_t tag;
    uint32_t version;
    void *module;
    uint64_t reserved[12];
    int (*close)(void *device);
    void *enumerateInstanceExtensions;
    void *createInstance;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr;
};
static_assert(sizeof(void *) == 8, "Turnip HAL loader requires arm64");

long envLong(const char *name, long fallback)
{
    if (const char *v = std::getenv(name))
    {
        if (v[0] != '\0')
        {
            char *end = nullptr;
            const long n = std::strtol(v, &end, 10);
            if (end != v)
                return n;
        }
    }
    return fallback;
}

int openFdCount()
{
    DIR *d = opendir("/proc/self/fd");
    if (!d)
        return -1;
    int n = 0;
    while (const dirent *e = readdir(d))
        n += e->d_name[0] != '.';
    closedir(d);
    return n - 1;
}

long rssPages()
{
    FILE *f = std::fopen("/proc/self/statm", "r");
    if (!f)
        return -1;
    long size = -1, rss = -1;
    if (std::fscanf(f, "%ld %ld", &size, &rss) != 2)
        rss = -1;
    std::fclose(f);
    return rss;
}

// Deterministic pattern: white border (crop/offset), thirds with a
// frame-varying wash (freshness), and a 32-cell frame counter top-left.
void patternPixel(uint32_t x, uint32_t y, uint32_t f, uint32_t w, uint32_t h,
                  uint8_t &r, uint8_t &g, uint8_t &b)
{
    if (x < 4u || y < 4u || x + 4u >= w || y + 4u >= h)
    {
        r = g = b = 255u;
        return;
    }
    const uint32_t third = h / 3u;
    const uint8_t wash = static_cast<uint8_t>(f & 0xFFu);
    if (y < third)
    {
        r = static_cast<uint8_t>((x * 255u / (w - 1u) + wash) & 0xFFu);
        g = 16u;
        b = 16u;
    }
    else if (y < 2u * third)
    {
        r = 16u;
        g = static_cast<uint8_t>((x * 255u / (w - 1u) + wash) & 0xFFu);
        b = 16u;
    }
    else
    {
        r = 16u;
        g = 16u;
        b = static_cast<uint8_t>((x * 255u / (w - 1u) + wash) & 0xFFu);
    }
    // Frame counter: 32 cells of 8x8 at (8 + i*8, 8), bit i of f.
    if (y >= 8u && y < 16u && x >= 8u)
    {
        const uint32_t bit = (x - 8u) / 8u;
        if (bit < 32u)
            r = g = b = ((f >> bit) & 1u) ? 255u : 0u;
    }
}

void fillPattern(std::vector<uint8_t> &rgba, uint32_t f, uint32_t w, uint32_t h)
{
    rgba.resize(static_cast<size_t>(w) * h * 4u);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
        {
            uint8_t r, g, b;
            patternPixel(x, y, f, w, h, r, g, b);
            uint8_t *p = rgba.data() + (static_cast<size_t>(y) * w + x) * 4u;
            p[0] = r;
            p[1] = g;
            p[2] = b;
            p[3] = 255u;
        }
}

// RGB-only gralloc verify of one queued slot (alpha is not display alpha).
long verifySlot(AHardwareBuffer *ahb, const std::vector<uint8_t> &want, uint32_t w, uint32_t h)
{
    AHardwareBuffer_Desc desc = {};
    AHardwareBuffer_describe(ahb, &desc);
    void *ptr = nullptr;
    if (AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, nullptr, &ptr) != 0 || !ptr)
        return -1;
    long diff = 0;
    const uint8_t *src = static_cast<const uint8_t *>(ptr);
    for (uint32_t y = 0; y < h; ++y)
    {
        const uint8_t *ra = src + static_cast<size_t>(y) * desc.stride * 4u;
        const uint8_t *rb = want.data() + static_cast<size_t>(y) * w * 4u;
        for (uint32_t x = 0; x < w; ++x)
            diff += std::memcmp(ra + x * 4u, rb + x * 4u, 3u) != 0;
    }
    AHardwareBuffer_unlock(ahb, nullptr);
    return diff;
}

struct VulkanProcs
{
    PFN_vkGetInstanceProcAddr getInstance = nullptr;
    PFN_vkCreateInstance createInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices enumPhys = nullptr;
    PFN_vkGetPhysicalDeviceProperties getPhysProps = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties getQueueProps = nullptr;
    PFN_vkCreateDevice createDevice = nullptr;
    PFN_vkGetDeviceProcAddr getDevice = nullptr;
    PFN_vkGetDeviceQueue getQueue = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties enumDevExt = nullptr;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID getAhbProps = nullptr;
    PFN_vkCreateImage createImage = nullptr;
    PFN_vkAllocateMemory allocMemory = nullptr;
    PFN_vkBindImageMemory bindImageMemory = nullptr;
    PFN_vkCreateBuffer createBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements getBufferMemReq = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties getMemProps = nullptr;
    PFN_vkMapMemory mapMemory = nullptr;
    PFN_vkUnmapMemory unmapMemory = nullptr;
    PFN_vkCreateCommandPool createCmdPool = nullptr;
    PFN_vkAllocateCommandBuffers allocCmd = nullptr;
    PFN_vkBeginCommandBuffer beginCmd = nullptr;
    PFN_vkCmdPipelineBarrier cmdBarrier = nullptr;
    PFN_vkCmdCopyBufferToImage cmdCopy = nullptr;
    PFN_vkEndCommandBuffer endCmd = nullptr;
    PFN_vkCreateFence createFence = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;
    PFN_vkWaitForFences waitFences = nullptr;
    PFN_vkResetFences resetFences = nullptr;
    PFN_vkDeviceWaitIdle deviceWaitIdle = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkDestroyBuffer destroyBuffer = nullptr;
    PFN_vkDestroyCommandPool destroyCmdPool = nullptr;
    PFN_vkDestroyFence destroyFence = nullptr;
    PFN_vkDestroyDevice destroyDevice = nullptr;
    PFN_vkDestroyInstance destroyInstance = nullptr;
};

#define GE2_LOAD_INST(var, name)                                                                     \
    procs.var = reinterpret_cast<PFN_##name>(procs.getInstance(inst, #name));                         \
    if (!procs.var)                                                                                  \
    {                                                                                                \
        std::fprintf(stderr, "[foreign-present] missing %s\n", #name);                                \
        return 1;                                                                                    \
    }
#define GE2_LOAD_DEV(var, name)                                                                      \
    procs.var = reinterpret_cast<PFN_##name>(procs.getDevice(dev, #name));                            \
    if (!procs.var)                                                                                  \
    {                                                                                                \
        std::fprintf(stderr, "[foreign-present] missing %s\n", #name);                                \
        return 1;                                                                                    \
    }

struct Slot
{
    AHardwareBuffer *ahb = nullptr;
    uint64_t id = 0u;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

} // namespace

int ps2x_foreign_present_test_run()
{
    const long frames = envLong("PS2X_GS_FOREIGN_FRAMES", 1000);
    const long holdS = envLong("PS2X_GS_FOREIGN_HOLD_S", 15);
    const long compareEvery = envLong("PS2X_GS_FOREIGN_COMPARE_EVERY", 250);
    uint32_t w = 1280u, h = 720u;
    if (const char *size = std::getenv("PS2X_GS_FOREIGN_SIZE"))
        if (std::sscanf(size, "%ux%u", &w, &h) != 2 || w == 0u || h == 0u || w > 4096u || h > 4096u)
        {
            std::fprintf(stderr, "[foreign-present] bad PS2X_GS_FOREIGN_SIZE=%s\n", size);
            return 1;
        }
    if (frames <= 0 || frames > 100000 || holdS < 0 || holdS > 300)
    {
        std::fprintf(stderr, "[foreign-present] bad FRAMES=%ld HOLD_S=%ld\n", frames, holdS);
        return 1;
    }
    const char *turnip = std::getenv("PS2X_GS_TURNIP");
    if (!turnip || std::strcmp(turnip, "1") != 0)
    {
        std::fprintf(stderr, "[foreign-present] refused: need PS2X_GS_TURNIP=1 (shipped Turnip only)\n");
        return 1;
    }
    if (!ps2x_present_vk::enabled())
    {
        std::fprintf(stderr, "[foreign-present] refused: present-vk sink disabled\n");
        return 1;
    }
    struct android_app *app = GetAndroidApp();
    if (!app || !app->window)
    {
        std::fprintf(stderr, "[foreign-present] no app window\n");
        return 1;
    }
    // Prime the GL window's buffers so the child gets buffer-to-window
    // scaling (a window layer without a buffer shows the child at raw size).
    EGLint bufW = 0, bufH = 0;
    const EGLDisplay dpy = eglGetCurrentDisplay();
    const EGLSurface surf = eglGetCurrentSurface(EGL_DRAW);
    if (dpy != EGL_NO_DISPLAY && surf != EGL_NO_SURFACE)
    {
        eglQuerySurface(dpy, surf, EGL_WIDTH, &bufW);
        eglQuerySurface(dpy, surf, EGL_HEIGHT, &bufH);
        for (int i = 0; i < 3; ++i)
            eglSwapBuffers(dpy, surf);
    }
    std::fprintf(stderr, "[foreign-present] window buffers %dx%d, slots %ux%u\n",
                 (int)bufW, (int)bufH, w, h);
    // Aspect 1 = SixteenNine (ps2x::present::Aspect), the anamorphic default.
    ps2x_present_vk::setHostWindow(app->window, app->activity, 1, bufW, bufH);
    if (ps2x_present_vk::broken())
    {
        std::fprintf(stderr, "[foreign-present] sink broken after setHostWindow\n");
        return 1;
    }

    // Turnip HAL -> vkGetInstanceProcAddr (same driver the backend uses).
    void *library = dlopen("libvulkan_freedreno.so", RTLD_NOW | RTLD_LOCAL);
    if (!library)
    {
        std::fprintf(stderr, "[foreign-present] Turnip dlopen failed: %s\n", dlerror());
        return 1;
    }
    dlerror();
    auto *hmi = reinterpret_cast<HalModule *>(dlsym(library, "HMI"));
    const char *symErr = dlerror();
    if (symErr || !hmi || hmi->tag != 0x48574d54u || !hmi->methods || !hmi->methods->open)
    {
        std::fprintf(stderr, "[foreign-present] Turnip HMI invalid\n");
        return 1;
    }
    void *halDev = nullptr;
    if (hmi->methods->open(hmi, "vulkan0", &halDev) != 0 || !halDev)
    {
        std::fprintf(stderr, "[foreign-present] Turnip HAL open failed\n");
        return 1;
    }
    auto *hal = reinterpret_cast<HalDevice *>(halDev);
    if (!hal->getInstanceProcAddr)
    {
        std::fprintf(stderr, "[foreign-present] Turnip HAL get-proc is null\n");
        return 1;
    }
    Dl_info mapped = {};
    if (dladdr(hmi, &mapped) && mapped.dli_fname)
        std::fprintf(stderr, "[foreign-present] Turnip %s\n", mapped.dli_fname);

    VulkanProcs procs{};
    procs.getInstance = hal->getInstanceProcAddr;
    procs.createInstance =
        reinterpret_cast<PFN_vkCreateInstance>(procs.getInstance(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!procs.createInstance)
    {
        std::fprintf(stderr, "[foreign-present] missing vkCreateInstance\n");
        return 1;
    }
    VkApplicationInfo appInfo = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &appInfo;
    VkInstance inst = VK_NULL_HANDLE;
    if (procs.createInstance(&ici, nullptr, &inst) != VK_SUCCESS || !inst)
    {
        std::fprintf(stderr, "[foreign-present] vkCreateInstance failed\n");
        return 1;
    }
    GE2_LOAD_INST(enumPhys, vkEnumeratePhysicalDevices);
    GE2_LOAD_INST(getPhysProps, vkGetPhysicalDeviceProperties);
    GE2_LOAD_INST(getQueueProps, vkGetPhysicalDeviceQueueFamilyProperties);
    GE2_LOAD_INST(createDevice, vkCreateDevice);
    GE2_LOAD_INST(getDevice, vkGetDeviceProcAddr);
    GE2_LOAD_INST(enumDevExt, vkEnumerateDeviceExtensionProperties);
    GE2_LOAD_INST(getAhbProps, vkGetAndroidHardwareBufferPropertiesANDROID);

    uint32_t physCount = 0u;
    procs.enumPhys(inst, &physCount, nullptr);
    if (physCount == 0u)
    {
        std::fprintf(stderr, "[foreign-present] no physical devices\n");
        return 1;
    }
    std::vector<VkPhysicalDevice> phys(physCount);
    procs.enumPhys(inst, &physCount, phys.data());
    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    uint32_t gfxFamily = ~0u;
    for (VkPhysicalDevice p : phys)
    {
        uint32_t qCount = 0u;
        procs.getQueueProps(p, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> qps(qCount);
        procs.getQueueProps(p, &qps, qps.data());
        for (uint32_t i = 0; i < qCount; ++i)
            if (qps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            {
                chosen = p;
                gfxFamily = i;
                break;
            }
        if (chosen)
            break;
    }
    if (!chosen)
    {
        std::fprintf(stderr, "[foreign-present] no graphics queue\n");
        return 1;
    }
    VkPhysicalDeviceProperties props = {};
    procs.getPhysProps(chosen, &props);
    std::fprintf(stderr, "[foreign-present] device %s api=%u.%u driver=0x%x gfxFamily=%u\n",
                 props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
                 VK_VERSION_MINOR(props.apiVersion), props.driverVersion, gfxFamily);

    uint32_t extCount = 0u;
    procs.enumDevExt(chosen, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    procs.enumDevExt(chosen, nullptr, &extCount, exts.data());
    bool haveAhb = false, haveForeign = false;
    for (const auto &e : exts)
    {
        haveAhb = haveAhb || std::strcmp(e.extensionName, "VK_ANDROID_external_memory_android_hardware_buffer") == 0;
        haveForeign = haveForeign || std::strcmp(e.extensionName, "VK_EXT_queue_family_foreign") == 0;
    }
    if (!haveAhb || !haveForeign)
    {
        std::fprintf(stderr, "[foreign-present] missing exts ahb=%d foreign=%d\n", haveAhb, haveForeign);
        return 1;
    }
    const char *wantExt[] = {"VK_ANDROID_external_memory_android_hardware_buffer",
                             "VK_EXT_queue_family_foreign"};
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = gfxFamily;
    qci.queueCount = 1u;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1u;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 2u;
    dci.ppEnabledExtensionNames = wantExt;
    VkDevice dev = VK_NULL_HANDLE;
    if (procs.createDevice(chosen, &dci, nullptr, &dev) != VK_SUCCESS || !dev)
    {
        std::fprintf(stderr, "[foreign-present] vkCreateDevice failed\n");
        return 1;
    }
    std::fprintf(stderr, "[foreign-present] foreign VkDevice %p created (own, non-Granite)\n",
                 (void *)dev);
    GE2_LOAD_DEV(getQueue, vkGetDeviceQueue);
    GE2_LOAD_DEV(createImage, vkCreateImage);
    GE2_LOAD_DEV(allocMemory, vkAllocateMemory);
    GE2_LOAD_DEV(bindImageMemory, vkBindImageMemory);
    GE2_LOAD_DEV(createBuffer, vkCreateBuffer);
    GE2_LOAD_DEV(getBufferMemReq, vkGetBufferMemoryRequirements);
    GE2_LOAD_DEV(mapMemory, vkMapMemory);
    GE2_LOAD_DEV(unmapMemory, vkUnmapMemory);
    GE2_LOAD_DEV(createCmdPool, vkCreateCommandPool);
    GE2_LOAD_DEV(allocCmd, vkAllocateCommandBuffers);
    GE2_LOAD_DEV(beginCmd, vkBeginCommandBuffer);
    GE2_LOAD_DEV(cmdBarrier, vkCmdPipelineBarrier);
    GE2_LOAD_DEV(cmdCopy, vkCmdCopyBufferToImage);
    GE2_LOAD_DEV(endCmd, vkEndCommandBuffer);
    GE2_LOAD_DEV(createFence, vkCreateFence);
    GE2_LOAD_DEV(queueSubmit, vkQueueSubmit);
    GE2_LOAD_DEV(waitFences, vkWaitForFences);
    GE2_LOAD_DEV(resetFences, vkResetFences);
    GE2_LOAD_DEV(deviceWaitIdle, vkDeviceWaitIdle);
    GE2_LOAD_DEV(destroyImage, vkDestroyImage);
    GE2_LOAD_DEV(freeMemory, vkFreeMemory);
    GE2_LOAD_DEV(destroyBuffer, vkDestroyBuffer);
    GE2_LOAD_DEV(destroyCmdPool, vkDestroyCommandPool);
    GE2_LOAD_DEV(destroyFence, vkDestroyFence);
    GE2_LOAD_DEV(destroyDevice, vkDestroyDevice);
    GE2_LOAD_DEV(destroyInstance, vkDestroyInstance);
    auto getMemProps = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        procs.getInstance(inst, "vkGetPhysicalDeviceMemoryProperties"));
    if (!getMemProps)
    {
        std::fprintf(stderr, "[foreign-present] missing vkGetPhysicalDeviceMemoryProperties\n");
        return 1;
    }
    VkQueue queue = VK_NULL_HANDLE;
    procs.getQueue(dev, gfxFamily, 0u, &queue);

    // Four AHB slots imported on the foreign device (mirrors the VK1 recipe).
    constexpr int kSlots = 4;
    Slot slots[kSlots];
    const VkImageUsageFlags kUsage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    auto makeSlots = [&]() -> bool {
        for (Slot &s : slots)
        {
            s.id = ps2x_present_vk::allocateBuffer(w, h, &s.ahb);
            if (s.id == 0u || !s.ahb)
            {
                std::fprintf(stderr, "[foreign-present] AHB alloc failed\n");
                return false;
            }
            VkAndroidHardwareBufferFormatPropertiesANDROID fmt = {
                VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
            VkAndroidHardwareBufferPropertiesANDROID ahbProps = {
                VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, &fmt};
            if (procs.getAhbProps(dev, s.ahb, &ahbProps) != VK_SUCCESS || ahbProps.memoryTypeBits == 0u)
            {
                std::fprintf(stderr, "[foreign-present] AHB props failed\n");
                return false;
            }
            if (&s == &slots[0])
                std::fprintf(stderr,
                             "[foreign-present] AHB props size=%llu memTypeBits=0x%x format=%d "
                             "externalFormat=0x%llx\n",
                             (unsigned long long)ahbProps.allocationSize, ahbProps.memoryTypeBits,
                             (int)fmt.format, (unsigned long long)fmt.externalFormat);
            VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
            ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
            VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext};
            ii.imageType = VK_IMAGE_TYPE_2D;
            ii.format = VK_FORMAT_R8G8B8A8_UNORM;
            ii.extent = {w, h, 1u};
            ii.mipLevels = 1u;
            ii.arrayLayers = 1u;
            ii.samples = VK_SAMPLE_COUNT_1_BIT;
            ii.tiling = VK_IMAGE_TILING_OPTIMAL;
            ii.usage = kUsage;
            ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (procs.createImage(dev, &ii, nullptr, &s.image) != VK_SUCCESS)
            {
                std::fprintf(stderr, "[foreign-present] vkCreateImage failed\n");
                return false;
            }
            VkImportAndroidHardwareBufferInfoANDROID imp = {
                VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
            imp.buffer = s.ahb;
            VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &imp};
            ded.image = s.image;
            VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded};
            mai.allocationSize = ahbProps.allocationSize;
            mai.memoryTypeIndex = static_cast<uint32_t>(__builtin_ctz(ahbProps.memoryTypeBits));
            if (procs.allocMemory(dev, &mai, nullptr, &s.memory) != VK_SUCCESS)
            {
                std::fprintf(stderr, "[foreign-present] import alloc failed\n");
                return false;
            }
            if (procs.bindImageMemory(dev, s.image, s.memory, 0) != VK_SUCCESS)
            {
                std::fprintf(stderr, "[foreign-present] bind failed\n");
                return false;
            }
        }
        return true;
    };
    auto dropSlots = [&](bool retire) {
        procs.deviceWaitIdle(dev);
        for (Slot &s : slots)
        {
            if (s.image)
                procs.destroyImage(dev, s.image, nullptr);
            if (s.memory)
                procs.freeMemory(dev, s.memory, nullptr);
            if (retire)
                ps2x_present_vk::retireBuffer(s.id);
            s = Slot{};
        }
    };
    if (!makeSlots())
    {
        dropSlots(true);
        return 1;
    }
    uint32_t epoch = ps2x_present_vk::bufferEpoch(slots[0].id);
    std::fprintf(stderr, "[foreign-present] %d slots %ux%u imported, epoch %u\n", kSlots, w, h, epoch);

    // Host-visible staging buffer for the per-frame pattern.
    const VkDeviceSize stageSize = static_cast<VkDeviceSize>(w) * h * 4u;
    VkBuffer stage = VK_NULL_HANDLE;
    VkDeviceMemory stageMem = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = stageSize;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (procs.createBuffer(dev, &bi, nullptr, &stage) != VK_SUCCESS)
        {
            std::fprintf(stderr, "[foreign-present] staging buffer failed\n");
            dropSlots(true);
            return 1;
        }
        VkMemoryRequirements req = {};
        procs.getBufferMemReq(dev, stage, &req);
        VkPhysicalDeviceMemoryProperties mp = {};
        getMemProps(chosen, &mp);
        uint32_t memIdx = ~0u;
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((req.memoryTypeBits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags &
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            {
                memIdx = i;
                break;
            }
        if (memIdx == ~0u)
        {
            std::fprintf(stderr, "[foreign-present] no host-visible memory\n");
            dropSlots(true);
            return 1;
        }
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = memIdx;
        auto bindBuf =
            reinterpret_cast<PFN_vkBindBufferMemory>(procs.getDevice(dev, "vkBindBufferMemory"));
        if (!bindBuf)
        {
            std::fprintf(stderr, "[foreign-present] missing vkBindBufferMemory\n");
            dropSlots(true);
            return 1;
        }
        if (procs.allocMemory(dev, &mai, nullptr, &stageMem) != VK_SUCCESS ||
            bindBuf(dev, stage, stageMem, 0) != VK_SUCCESS)
        {
            std::fprintf(stderr, "[foreign-present] staging alloc/bind failed\n");
            dropSlots(true);
            return 1;
        }
    }
    VkCommandPool pool = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = gfxFamily;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (procs.createCmdPool(dev, &pi, nullptr, &pool) != VK_SUCCESS)
        {
            std::fprintf(stderr, "[foreign-present] cmd pool failed\n");
            dropSlots(true);
            return 1;
        }
    }
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    {
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1u;
        if (procs.allocCmd(dev, &ai, &cmd) != VK_SUCCESS)
        {
            std::fprintf(stderr, "[foreign-present] cmd alloc failed\n");
            dropSlots(true);
            return 1;
        }
    }
    VkFence fence = VK_NULL_HANDLE;
    {
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (procs.createFence(dev, &fi, nullptr, &fence) != VK_SUCCESS)
        {
            std::fprintf(stderr, "[foreign-present] fence failed\n");
            dropSlots(true);
            return 1;
        }
    }

    const int fdsStart = openFdCount();
    const long rssStart = rssPages();
    int fdsMid = -1;
    long rssMid = -1;
    long queued = 0, skipped = 0, compares = 0, compareFails = 0;
    int start = 0;
    std::vector<uint8_t> pattern;
    for (long f = 0; f < frames; ++f)
    {
        if (ps2x_present_vk::poolEpoch() != epoch)
        {
            std::fprintf(stderr, "[foreign-present] window changed at frame %ld: rebuilding slots\n", f);
            dropSlots(true);
            if (!makeSlots())
                return 1;
            epoch = ps2x_present_vk::bufferEpoch(slots[0].id);
        }
        uint64_t ids[kSlots];
        for (int i = 0; i < kSlots; ++i)
            ids[i] = slots[i].id;
        const ps2x_present_vk::Pick pick = ps2x_present_vk::pickReusable(ids, kSlots, start, 1000);
        if (pick.index < 0)
        {
            if (pick.giveUp)
            {
                std::fprintf(stderr, "[foreign-present] pickReusable gave up at frame %ld\n", f);
                dropSlots(true);
                return 1;
            }
            ++skipped;
            continue;
        }
        Slot &s = slots[pick.index];
        start = (pick.index + 1) % kSlots;
        fillPattern(pattern, static_cast<uint32_t>(f), w, h);
        void *mapped = nullptr;
        if (procs.mapMemory(dev, stageMem, 0, stageSize, 0, &mapped) != VK_SUCCESS || !mapped)
        {
            std::fprintf(stderr, "[foreign-present] stage map failed at frame %ld\n", f);
            dropSlots(true);
            return 1;
        }
        std::memcpy(mapped, pattern.data(), pattern.size());
        procs.unmapMemory(dev, stageMem);

        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        procs.beginCmd(cmd, &bi);
        VkImageMemoryBarrier acq = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        acq.srcAccessMask = 0;
        acq.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        acq.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // whole image rewritten
        acq.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        acq.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        acq.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        acq.image = s.image;
        acq.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        procs.cmdBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &acq);
        VkBufferImageCopy region = {};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {w, h, 1u};
        procs.cmdCopy(cmd, stage, s.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        VkImageMemoryBarrier rel = acq; // hand the buffer to SurfaceFlinger/HWC
        rel.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        rel.dstAccessMask = 0;
        rel.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        rel.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        rel.srcQueueFamilyIndex = gfxFamily;
        rel.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        procs.cmdBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &rel);
        procs.endCmd(cmd);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1u;
        si.pCommandBuffers = &cmd;
        if (procs.queueSubmit(queue, 1, &si, fence) != VK_SUCCESS)
        {
            std::fprintf(stderr, "[foreign-present] submit failed at frame %ld\n", f);
            dropSlots(true);
            return 1;
        }
        // The sink queues with acquireFenceFd=-1, so the GPU work must be
        // done before queue(): wait here (synchronous test, no pipelining).
        if (procs.waitFences(dev, 1, &fence, VK_TRUE, 5000000000ull) != VK_SUCCESS)
        {
            std::fprintf(stderr, "[foreign-present] fence wait failed at frame %ld\n", f);
            dropSlots(true);
            return 1;
        }
        procs.resetFences(dev, 1, &fence);
        if (compareEvery > 0 && f % compareEvery == 0)
        {
            ++compares;
            const long diff = verifySlot(s.ahb, pattern, w, h);
            std::fprintf(stderr, "[foreign-present] frame %ld gralloc-verify diff_px=%ld\n", f, diff);
            if (diff != 0)
            {
                ++compareFails;
                std::fprintf(stderr, "[foreign-present] PATTERN MISMATCH at frame %ld\n", f);
            }
        }
        if (!ps2x_present_vk::queue(s.id, w, h))
        {
            std::fprintf(stderr, "[foreign-present] queue refused at frame %ld\n", f);
            dropSlots(true);
            return 1;
        }
        ++queued;
        if (f == frames / 2)
        {
            fdsMid = openFdCount();
            rssMid = rssPages();
        }
        if ((f + 1) % 100 == 0)
            std::fprintf(stderr, "[foreign-present] frame %ld/%ld queued=%ld skipped=%ld\n",
                         f + 1, frames, queued, skipped);
    }
    const int fdsEnd = openFdCount();
    const long rssEnd = rssPages();
    char stats[1024];
    ps2x_present_vk::appendStats(stats, sizeof(stats));
    std::fprintf(stderr, "[foreign-present] stats%s\n", stats);
    const bool leakOk = fdsEnd >= 0 && fdsStart >= 0 && fdsEnd <= fdsStart + 8;
    const bool pass = compareFails == 0 && !ps2x_present_vk::broken() && leakOk && skipped <= 10;
    std::fprintf(stderr,
                 "[foreign-present] %s frames=%ld queued=%ld skipped=%ld compares=%ld compareFails=%ld "
                 "fds=%d/%d/%d rss=%ld/%ld/%ld broken=%d hold=%lds\n",
                 pass ? "PASS" : "FAIL", frames, queued, skipped, compares, compareFails, fdsStart,
                 fdsMid, fdsEnd, rssStart, rssMid, rssEnd, ps2x_present_vk::broken() ? 1 : 0, holdS);
    if (holdS > 0)
    {
        std::fprintf(stderr, "[foreign-present] holding last frame for screencap\n");
        sleep(static_cast<unsigned>(holdS));
    }
    dropSlots(true);
    procs.deviceWaitIdle(dev);
    procs.destroyFence(dev, fence, nullptr);
    procs.destroyCmdPool(dev, pool, nullptr);
    procs.destroyBuffer(dev, stage, nullptr);
    procs.freeMemory(dev, stageMem, nullptr);
    // Layer + instance teardown is process exit's; the sink outlives us by design.
    return pass ? 0 : 1;
}

#undef GE2_LOAD_INST
#undef GE2_LOAD_DEV

#endif
