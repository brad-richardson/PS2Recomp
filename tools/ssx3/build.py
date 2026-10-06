#!/usr/bin/env python3
"""Local SSX 3 preparation. Never downloads or publishes game data."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
GENERATOR = 'fc1f3effa3f2a79f4482ac0b7952191fa5f3a51e'
FORK = 'https://github.com/brad-richardson/PS2Recomp.git'
ISO_SHA = '3c2f8eb182c9c6208a6e8172a41e61c98f420abe3f42c845f6829aeb9761ebf5'
ELF_SHA = '1b49d05ca2793922180851b9e1ce9ae2291d61a7863565ac4e71f12e967af7bc'
REGISTER_SHA = 'e982523ca1aa271500c5a40d9562fa27e2926ca87393f5d88cb40eb8157af789'


def run(*args, cwd=None):
    print('+', ' '.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), cwd=cwd, check=True)


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def extract_elf(iso, dest):
    # Read the ISO9660 root directly; no mounting, BIOS or external extraction tool.
    with iso.open('rb') as f:
        f.seek(16 * 2048)
        pvd = f.read(2048)
        if pvd[:7] != b'\x01CD001\x01':
            raise ValueError('expected ISO9660 primary volume descriptor')
        root = pvd[156:190]
        extent, length = struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0]
        f.seek(extent * 2048)
        directory = f.read(length)
        pos = 0
        while pos < len(directory):
            size = directory[pos]
            if not size:
                pos = (pos // 2048 + 1) * 2048
                continue
            entry = directory[pos:pos + size]
            name = entry[33:33 + entry[32]].decode('ascii', errors='replace').split(';')[0]
            if name == 'SLUS_207.72':
                sector = struct.unpack_from('<I', entry, 2)[0]
                count = struct.unpack_from('<I', entry, 10)[0]
                f.seek(sector * 2048)
                dest.write_bytes(f.read(count))
                return
            pos += size
    raise ValueError('SLUS_207.72 missing from disc root')


def compiler():
    # LLVM 20+ is required by the runtime; use caller's CXX or an installed LLVM.
    cxx = os.environ.get('CXX') or shutil.which('clang++')
    brew = shutil.which('brew')
    if not os.environ.get('CXX') and brew:
        prefix = subprocess.check_output([brew, '--prefix', 'llvm'], text=True).strip()
        if Path(prefix, 'bin/clang++').exists():
            cxx = str(Path(prefix, 'bin/clang++'))
    if not cxx:
        raise ValueError('install LLVM and set CC/CXX')
    cc = os.environ.get('CC') or str(Path(cxx).with_name('clang'))
    return ['-DCMAKE_C_COMPILER=' + cc, '-DCMAKE_CXX_COMPILER=' + cxx]


def configure(src, dst, *flags):
    run('cmake', '-S', src, '-B', dst, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
        '-DPS2X_BUILD_STUDIO=OFF', '-DPS2X_ENABLE_SCCACHE=OFF', *compiler(), *flags)


def prepare(a):
    iso = a.iso.resolve()
    if sha(iso) != ISO_SHA:
        raise ValueError('unsupported disc SHA256; see docs/ssx3-inputs.md')
    elf = a.work / 'SLUS_207.72'
    extract_elf(iso, elf)
    if sha(elf) != ELF_SHA:
        raise ValueError('extracted ELF SHA256 mismatch')
    gen = a.work / 'generator-source'
    if not gen.exists():
        run('git', 'clone', '--filter=blob:none', '--no-checkout', FORK, gen)
        run('git', '-C', gen, 'checkout', '--detach', GENERATOR)
    actual = subprocess.check_output(['git', '-C', str(gen), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != GENERATOR:
        raise ValueError('generator source pin mismatch')
    build = a.work / 'generator-build'
    configure(gen, build, '-DPS2X_BUILD_RUNTIME=OFF', '-DPS2X_BUILD_ANALYZER=OFF', '-DPS2X_BUILD_TEST=OFF')
    run('cmake', '--build', build, '--parallel', a.jobs, '--target', 'ps2_recomp')
    output = a.work / 'codegen'
    if output.exists():
        raise ValueError('refusing existing codegen; choose a fresh --work directory')
    config = (ROOT / 'games/ssx3/ssx3.toml').read_text()
    paths = {'input': elf, 'ghidra_output': ROOT / 'games/ssx3/ssx3-functions.sweep.csv', 'output': output}
    for key, value in paths.items():
        config = re.sub(r'^' + key + r' = .*$', key + ' = ' + json.dumps(str(value)), config, flags=re.M)
    toml = a.work / 'generation.toml'
    toml.write_text(config)
    run(build / 'ps2xRecomp/ps2_recomp', toml)
    if sha(output / 'register_functions.cpp') != REGISTER_SHA:
        raise ValueError('generated registration differs from EQG1 canonical pin')
    (a.work / 'inputs.json').write_text(json.dumps({'iso': str(iso), 'iso_sha256': ISO_SHA,
        'elf_sha256': ELF_SHA, 'generator': GENERATOR, 'register_sha256': REGISTER_SHA, 'toml_sha256': sha(ROOT / 'games/ssx3/ssx3.toml'),
        'map_sha256': sha(ROOT / 'games/ssx3/ssx3-functions.sweep.csv')}, indent=2) + '\n')
    print('Prepared local game sources. Keep this work directory private.')


def mac(a):
    extra = 'ON' if a.suite else 'OFF'
    configure(ROOT, a.work / 'runner-build', '-DPS2X_GAME_CODEGEN_DIR=' + str(a.work / 'codegen'),
        '-DPS2X_VU0_RECOMP_DIR=', '-DPS2X_BUILD_RECOMP=' + extra, '-DPS2X_BUILD_ANALYZER=' + extra,
        '-DPS2X_BUILD_TEST=' + extra, '-DPS2X_ENABLE_DEBUG_UI=OFF', '-DPS2X_ENABLE_RUNTIME_LOGS=OFF',
        '-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF', '-DPS2X_ENABLE_DIAG_TAPS=OFF',
        '-DPS2X_ENABLE_DIAG_WATCH=OFF', '-DPS2X_ENABLE_TS2_DIAG=OFF')
    run('cmake', '--build', a.work / 'runner-build', '--parallel', a.jobs,
        '--target', 'ps2EntryRunner', *(['ps2x_tests'] if a.suite else []))


def android(a):
    if not a.jnilibs:
        raise ValueError('android requires --jnilibs; see tools/ssx3/README.md for library recipe')
    run('bash', ROOT / 'android/gradlew', '-p', ROOT / 'android', 'assembleRelease',
        '-Pps2xGameCodegenDir=' + str(a.work / 'codegen'),
        '-Pps2xJniLibsDir=' + str(a.jnilibs.resolve()), '-Pps2xVu0RecompDir=',
        '-Pps2xPgoData=', '-Pps2xBootElf=/storage/emulated/0/Android/data/com.ps2x.runner/files/SLUS_207.72',
        '--max-workers=' + str(a.jobs))


def source(url, revision, destination):
    if not destination.exists():
        run('git', 'clone', '--filter=blob:none', '--no-checkout', url, destination)
        run('git', '-C', destination, 'checkout', '--detach', revision)
    actual = subprocess.check_output(['git', '-C', str(destination), 'rev-parse', 'HEAD'], text=True).strip()
    expected = subprocess.check_output(['git', '-C', str(destination), 'rev-parse', revision + '^{commit}'], text=True).strip()
    if actual != expected:
        raise ValueError('source pin mismatch: ' + str(destination))


def libs(a):
    vendor = a.work / 'mac-assembly/vendor-pcsx2'
    source('https://github.com/brad-richardson/pcsx2.git',
           '46093cbc36c59e11992163a2054fdc4795bb8108', vendor)
    prefix = a.work / 'deps'
    brew_prefix = subprocess.check_output(['brew', '--prefix'], text=True).strip()
    prefixes = str(prefix) + ';' + brew_prefix
    for name, tag in [('plutovg', 'v1.1.0'), ('plutosvg', 'v0.0.7')]:
        src = a.work / (name + '-source')
        source('https://github.com/sammycage/' + name + '.git', tag, src)
        dst = a.work / (name + '-build')
        run('cmake', '-S', src, '-B', dst, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
            '-DCMAKE_INSTALL_PREFIX=' + str(prefix), '-DCMAKE_PREFIX_PATH=' + prefixes,
            '-DBUILD_SHARED_LIBS=ON')
        run('cmake', '--build', dst, '--parallel', a.jobs)
        run('cmake', '--install', dst)
    assembly = vendor.parent
    for src, dest in [(ROOT / 'ps2xRuntime/third_party/armsx2/ge1', assembly / 'adapter'),
                      (ROOT / 'ps2xRuntime/third_party/armsx2/mv2', assembly / 'mv2-adapter'),
                      (ROOT / 'tools/ssx3/mac-libs', assembly / 'mac-platform')]:
        shutil.copytree(src, dest, dirs_exist_ok=True)
    link = assembly / 'mac-platform/3rdparty'
    if not link.exists():
        link.symlink_to(vendor / '3rdparty', target_is_directory=True)
    build = a.work / 'libs-build'
    run('cmake', '-S', assembly / 'mac-platform', '-B', build, '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER=/usr/bin/cc',
        '-DCMAKE_CXX_COMPILER=/usr/bin/c++', '-DCMAKE_PREFIX_PATH=' + prefixes,
        '-DPS2RECOMP_SOURCE=' + str(ROOT), '-DLTO_PCSX2_CORE=OFF')
    run('cmake', '--build', build, '--parallel', a.jobs, '--target', 'ge1_gs', 'mv2_microvu', 'ge1_metallibs')
    resources = a.work / 'resources'
    shutil.copytree(vendor / 'bin/resources', resources, dirs_exist_ok=True)
    for metallib in (build / 'ge1-adapter/metallibs').glob('*.metallib'):
        shutil.copy2(metallib, resources)


def play(a):
    inputs = json.loads((a.work / 'inputs.json').read_text())
    env = os.environ.copy()
    env.update({'PS2X_CD_IMAGE': inputs['iso'], 'PS2X_MC_ROOT': str(a.work / 'mc0'),
        'PS2X_SKIP_MOVIE': '1', 'PS2X_GS_BACKEND': 'external',
        'PS2X_GS_EXTERNAL_LIBRARY': str(a.work / 'libs-build/ge1-adapter/libge1_gs.dylib'),
        'PS2X_MICROVU_LIB': str(a.work / 'libs-build/libmv2_microvu.dylib'),
        'PS2X_VU1_ENGINE': 'microvu', 'PS2X_MTVU': '1', 'PS2X_VU0_RECOMP': '0',
        'GE1_RENDERER': 'metal', 'GE1_GS_RESOURCES_DIR': str(a.work / 'resources'),
        'GE1_GS_DATA_DIR': str(a.work / 'gs-data')})
    (a.work / 'gs-data').mkdir(exist_ok=True)
    subprocess.run([str(a.work / 'runner-build/ps2xRuntime/ps2EntryRunner'),
                    str(a.work / 'SLUS_207.72')], env=env, check=True, cwd=a.work)


def android_libs(a):
    if not a.ndk or not (a.ndk / 'build/cmake/android.toolchain.cmake').is_file():
        raise ValueError('android-libs requires --ndk (NDK 28.2.13676358)')
    vendor = a.work / 'android-assembly/vendor-pcsx2'
    source('https://github.com/brad-richardson/pcsx2.git',
           '46093cbc36c59e11992163a2054fdc4795bb8108', vendor)
    prefix = a.work / 'android-deps'
    prefix.mkdir(exist_ok=True)
    env = os.environ.copy()
    env.update({'ANDROID_NDK': str(a.ndk.resolve()), 'ANDROID_ABI': 'arm64-v8a', 'ANDROID_API': '29'})
    # The pinned vendor recipe downloads and builds source dependencies locally.
    subprocess.run(['bash', str(vendor / '.github/workflows/scripts/android/build-dependencies.sh'),
                    str(prefix)], env=env, cwd=a.work, check=True)
    assembly = vendor.parent
    for src, dest in [(ROOT / 'ps2xRuntime/third_party/armsx2/ge1', assembly / 'adapter'),
                      (ROOT / 'ps2xRuntime/third_party/armsx2/mv2', assembly / 'mv2-adapter'),
                      (ROOT / 'tools/ssx3/android-libs', assembly / 'android-platform')]:
        shutil.copytree(src, dest, dirs_exist_ok=True)
    shutil.copy2(ROOT / 'ps2xRuntime/third_party/armsx2/platform/android/ax4b_android_stubs.cpp',
                 assembly / 'adapter/ax4b_android_stubs.cpp')
    for name, target in [('3rdparty', vendor / '3rdparty'), ('pcsx2', vendor / 'pcsx2'),
                         ('tests', vendor / 'tests'), ('common', vendor / 'common')]:
        link = assembly / ('android-platform/3rdparty' if name == '3rdparty' else name)
        if not link.exists():
            link.symlink_to(target, target_is_directory=True)
    build = a.work / 'android-libs-build'
    run('cmake', '-S', assembly / 'android-platform', '-B', build, '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_TOOLCHAIN_FILE=' + str(a.ndk.resolve() / 'build/cmake/android.toolchain.cmake'),
        '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-29', '-DANDROID_STL=c++_static',
        '-DCMAKE_PREFIX_PATH=' + str(prefix), '-DCMAKE_FIND_ROOT_PATH=' + str(prefix),
        '-DLTO_PCSX2_CORE=OFF')
    run('cmake', '--build', build, '--parallel', a.jobs, '--target', 'ge1_gs', 'mv2_microvu')
    jni = a.work / 'jniLibs/arm64-v8a'
    jni.mkdir(parents=True, exist_ok=True)
    shutil.copy2(build / 'ge1-adapter/libge1_gs.so', jni)
    shutil.copy2(build / 'mv2-adapter/libmv2_microvu.so', jni)
    shutil.copytree(vendor / 'bin/resources', a.work / 'android-resources', dirs_exist_ok=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('step', choices=['prepare', 'mac', 'mac-libs', 'run', 'android', 'android-libs'])
    p.add_argument('--work', type=Path, required=True, help='external private output directory')
    p.add_argument('--iso', type=Path, help='own supported USA disc image (prepare only)')
    p.add_argument('--jnilibs', type=Path, help='own source-built Android JNI libraries')
    p.add_argument('--ndk', type=Path, help='local Android NDK 28.2.13676358')
    p.add_argument('--suite', action='store_true', help='also build optional Mac development suite and its analyzer/recompiler fixtures')
    p.add_argument('--jobs', type=int, default=8)
    a = p.parse_args()
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        p.error('supported build host: macOS Apple silicon; Windows/x86 is unsupported')
    a.work = a.work.resolve()
    if a.work == ROOT or ROOT in a.work.parents:
        p.error('--work must be outside the source checkout')
    if a.jobs < 1 or a.jobs > 16:
        p.error('--jobs must be 1..16')
    if a.step == 'prepare' and not a.iso:
        p.error('prepare requires --iso')
    if a.step != 'prepare' and not (a.work / 'inputs.json').is_file():
        p.error('run prepare in this work directory first')
    a.work.mkdir(parents=True, exist_ok=True)
    {'prepare': prepare, 'mac': mac, 'android': android, 'mac-libs': libs, 'run': play, 'android-libs': android_libs}[a.step](a)


if __name__ == '__main__':
    main()
