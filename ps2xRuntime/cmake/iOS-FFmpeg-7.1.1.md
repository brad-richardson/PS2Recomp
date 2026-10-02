# FFmpeg 7.1.x for iOS (I23) — build recipe, license notice, provision pointer

## What this is

The iOS target links a minimal FFmpeg 7.1.x subset as three static
archives (see the `elseif(PS2X_IS_IOS)` block in
`ps2xRuntime/CMakeLists.txt`, configured via
`-DPS2X_FFMPEG_IOS_ROOT=<prefix>`):

- `lib/libavcodec.a` — MPEG-1/2 video decoder (`mpeg2video`) + parser (`mpegvideo`)
- `lib/libavutil.a`
- `lib/libswscale.a`
- `include/` — matching public + generated (`avconfig.h`, `ffversion.h`) headers

`avformat`/`swresample` are NOT wired: `MPEG.cpp` calls no symbol from
either (verified by grep at integration time; re-check if the includes grow).

## Build recipe (reproduces the prefix)

Source: `https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz`
sha256 `733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1`

```sh
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
./configure --target-os=darwin --arch=aarch64 --cpu=generic --enable-cross-compile \
  --cc="xcrun -sdk iphoneos clang" --sysroot="$SDK" \
  --extra-cflags="-arch arm64 -mios-version-min=17.0 -isysroot $SDK -Os" \
  --extra-ldflags="-arch arm64 -mios-version-min=17.0 -isysroot $SDK" \
  --disable-everything --disable-programs --disable-doc --disable-avdevice --disable-avfilter \
  --disable-avformat --disable-swresample --disable-network --disable-iconv --disable-bzlib \
  --disable-lzma --disable-zlib --enable-decoder=mpeg2video --enable-parser=mpegvideo \
  --enable-swscale --disable-asm --enable-static --disable-shared --disable-debug \
  --prefix="$PREFIX"
make -j2 && make install
```

Reference build: configure exit 0, `make -j2` exit 0 with 0 `error:`
lines, all three archives `arm64`, bare link of the exact `MPEG.cpp`
API surface exit 0 with zero extra frameworks.

## License notice (fact, not advice)

- This subset configures as `License: LGPL version 2.1 or later`.
- Components are native LGPL decoders/parsers/scaler/utils; the build
  enables no GPL component and no external library.
- Static LGPL code ships inside the app binary. The static-link
  relink/provision obligations and the host-side version pin
  (7.1.x both sides) are tracked in the I23 evidence, not here.

## Relink provision artifact (pointer)

Compliance fulfillment (object files / written offer + this notice) is
the owner's call at distribution time. Retained for that purpose: the
three archives, the `make install` prefix, and this recipe. Nothing
else in this directory carries redistribution obligations.
