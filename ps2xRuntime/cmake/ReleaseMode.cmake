include(CheckIPOSupported)

# BF2c: the try_compile probe cannot pass under the NDK (it loses ANDROID_ABI
# and links with -fuse-ld=gold, whose LLVMgold.so plugin NDK r28 removed) — a
# false negative. ThinLTO itself links fine in this container (our GE1 lib
# ships LTO_PCSX2_CORE=ON from it; ARMSX2 release does the same), so trust NDK
# Clang directly. Everywhere else keeps the probe.
if(ANDROID AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(IPO_SUPPORTED TRUE)
    # BF2d: CMake 3.22 appends -fuse-ld=gold for IPO links behind a stale
    # NDK<22 guard that misfires here (CMAKE_ANDROID_NDK_VERSION unset);
    # NDK r28 removed gold. lld consumes ThinLTO natively (GE1 precedent).
    set(CMAKE_CXX_LINK_OPTIONS_IPO "-fuse-ld=lld")
else()
    check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)
endif()

function(EnableFastReleaseMode TargetName)
    message("> Enabling optimization for: ${TargetName}")
    if(MSVC)
        target_compile_options(${TargetName} PRIVATE
            $<$<CONFIG:Release>:
                /O2 # speed
                /Ob2 # inline aggressively
                /Oi # intrinsics
                /GL # whole program opt
                /Gy # function-level linking
                /Gw # global data in COMDAT
                /GF # string pooling
                /Zc:inline # remove unreferenced inline
                /fp:fast # fast math (graphics friendly)
                /DNDEBUG
                /arch:AVX2 # Advanced Vector Extensions 2
                /GS- # Disable Buffer Security Check (faster)
                /Qspectre- # Disable Spectre mitigations (faster)
            >
        )

        if(TARGET ${TargetName})
            target_link_options(${TargetName} PRIVATE
                $<$<CONFIG:Release>:
                    /LTCG # link-time code generation
                    /OPT:REF # remove unreferenced
                    /OPT:ICF # fold identical COMDATs
                >
            )
        endif()
    endif()

    if(IPO_SUPPORTED)
        # BF2c: AGP builds the RelWithDebInfo cmake config (not Release), so
        # the IPO property must cover both configs to have any effect there.
        set_property(TARGET ${TargetName} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        set_property(TARGET ${TargetName} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
    else()
        message(WARNING "Interprocedural optimization not supported: ${IPO_ERROR}")
    endif()
endfunction()