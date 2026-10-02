// HR1: shared by ps2_present_share_mac.cpp and ps2_present_share_ios.mm.
// Needs <CoreVideo/CoreVideo.h> and <unordered_map> included before it.
namespace ps2x_present_share
{
namespace
{
std::unordered_map<void *, CVPixelBufferRef> &pixelBuffers()
{
    static std::unordered_map<void *, CVPixelBufferRef> m;
    return m;
}
} // namespace

void *createSurface(uint32_t width, uint32_t height)
{
    CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef ioProps = CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0,
                                                 &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attrs, kCVPixelBufferIOSurfacePropertiesKey, ioProps);
    CFDictionarySetValue(attrs, kCVPixelBufferMetalCompatibilityKey, kCFBooleanTrue);
#if TARGET_OS_IOS
    CFDictionarySetValue(attrs, kCVPixelBufferOpenGLESCompatibilityKey, kCFBooleanTrue);
#else
    CFDictionarySetValue(attrs, kCVPixelBufferOpenGLCompatibilityKey, kCFBooleanTrue);
#endif
    CVPixelBufferRef pb = nullptr;
    const CVReturn rc = CVPixelBufferCreate(kCFAllocatorDefault, width, height, kCVPixelFormatType_32BGRA, attrs, &pb);
    CFRelease(ioProps);
    CFRelease(attrs);
    IOSurfaceRef surface = (rc == kCVReturnSuccess && pb) ? CVPixelBufferGetIOSurface(pb) : nullptr;
    std::fprintf(stderr, "[present-share] surface %ux%u rc=%d surface=%p\n", width, height, rc, (void *)surface);
    if (!surface)
        return nullptr;
    pixelBuffers()[surface] = pb; // retained for the process (prototype)
    return surface;
}

void *pixelBufferFor(void *surface)
{
    auto it = pixelBuffers().find(surface);
    return it == pixelBuffers().end() ? nullptr : it->second;
}
} // namespace ps2x_present_share
