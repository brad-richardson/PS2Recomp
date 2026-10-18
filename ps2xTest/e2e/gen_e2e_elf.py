#!/usr/bin/env python3
"""ci-e2e-1 synthetic guest: hand-rolled MIPS32LE ELF + loader header + toml.

No toolchain, no SDK, no game bytes. The emitted guest does one "frame" per
call: counter++, 16-word table sum loop, taken + not-taken branches, FPU
add.s, MFC0 Count, MULT/MFLO in a JAL callee, results to RAM, JR ra.

Usage: gen_e2e_elf.py <out-dir>
Writes <out-dir>/e2e.elf, <out-dir>/e2e_image.h, <out-dir>/e2e.toml (the toml
input/output paths are rewritten by the caller for the CI workspace).
"""

import struct
import sys
from pathlib import Path

TEXT_BASE = 0x00100000
DATA_BASE = 0x00102000
# LUI loads the upper half (0x0010); all data offsets ride in the low half.
DATA_HI = 0x10
DATA_LO = 0x2000

COUNTER = DATA_LO + 0x00
OUT_SUM = DATA_LO + 0x04
OUT_FPU = DATA_LO + 0x08
OUT_COUNT = DATA_LO + 0x0C
OUT_CSUM = DATA_LO + 0x10
TABLE = DATA_LO + 0x20
TABLE2 = DATA_LO + 0x60

ZERO, V0 = 0, 2
T0, T1, T2, T3, T4, T5, T6, T7, T8, T9 = 8, 9, 10, 11, 12, 13, 14, 15, 24, 25
RA = 31


def lui(rt, imm):
    return (0x0F << 26) | (rt << 16) | (imm & 0xFFFF)


def ori(rt, rs, imm):
    return (0x0D << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def addiu(rt, rs, imm):
    return (0x09 << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def _special(rs, rt, rd, sh, fn):
    return (rs << 21) | (rt << 16) | (rd << 11) | (sh << 6) | fn


def add(rs, rt, rd):
    return _special(rs, rt, rd, 0, 0x20)


def addu(rs, rt, rd):
    return _special(rs, rt, rd, 0, 0x21)


def or_(rd, rs, rt):
    return _special(rs, rt, rd, 0, 0x25)


def sll(rd, rt, sh):
    return _special(0, rt, rd, sh, 0x00)


def jr(rs):
    return _special(rs, 0, 0, 0, 0x08)


def mult(rs, rt):
    return _special(rs, rt, 0, 0, 0x18)


def mflo(rd):
    return _special(0, 0, rd, 0, 0x12)


def lw(rt, base, off):
    return (0x23 << 26) | (base << 21) | (rt << 16) | (off & 0xFFFF)


def sw(rt, base, off):
    return (0x2B << 26) | (base << 21) | (rt << 16) | (off & 0xFFFF)


def slti(rt, rs, imm):
    return (0x0A << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def beq(rs, rt, off):
    return (0x04 << 26) | (rs << 21) | (rt << 16) | (off & 0xFFFF)


def bne(rs, rt, off):
    return (0x05 << 26) | (rs << 21) | (rt << 16) | (off & 0xFFFF)


def jal(target):
    return (0x03 << 26) | ((target >> 2) & 0x03FFFFFF)


def mtc1(rt, fs):
    return (0x11 << 26) | (0x04 << 21) | (rt << 16) | fs


def mfc1(rt, fs):
    return (0x11 << 26) | (0x00 << 21) | (rt << 16) | fs


def add_s(fd, fs, ft):
    return (0x11 << 26) | (16 << 21) | (ft << 16) | (fs << 11) | (fd << 6)


def mfc0(rt, rd):
    return (0x10 << 26) | (rt << 16) | (rd << 11)


NOP = 0


class Builder:
    def __init__(self):
        self.words = []
        self.labels = {}

    def at(self):
        return TEXT_BASE + 4 * len(self.words)

    def label(self, name):
        self.labels[name] = self.at()

    def emit(self, *ws):
        self.words.extend(ws)

    def branch_off(self, from_pc, target):
        return (target - (from_pc + 4)) // 4


b = Builder()

# --- _start ---
b.label("_start")
b.emit(
    lui(T9, DATA_HI),
    lw(T0, T9, COUNTER),
    addiu(T0, T0, 1),
    sw(T0, T9, COUNTER),
    or_(T1, ZERO, ZERO),
    or_(T2, ZERO, ZERO),
)
b.label("loop")
loop_pc = b.at()
b.emit(
    sll(T3, T2, 2),
    addu(T3, T3, T9),
    lw(T4, T3, TABLE),
    addu(T1, T1, T4),
    addiu(T2, T2, 1),
    slti(T5, T2, 16),
)
bne_pc = b.at()
b.emit(0, NOP)  # bne placeholder + delay
b.label("after_loop")
beq_pc = b.at()
b.emit(0, NOP)  # beq placeholder + delay
b.emit(addiu(T1, T1, 0x1111))
b.label("skip1")
bne2_pc = b.at()
b.emit(0, NOP)  # bne placeholder + delay
b.emit(addiu(T1, T1, 0x2222))
b.label("skip2")
b.emit(
    sw(T1, T9, OUT_SUM),
    mtc1(T0, 1),
    mtc1(T1, 2),
    add_s(3, 1, 2),
    mfc1(T6, 3),
    sw(T6, T9, OUT_FPU),
    mfc0(T7, 9),
    sw(T7, T9, OUT_COUNT),
)
jal_pc = b.at()
b.emit(0, NOP)  # jal placeholder + delay
b.emit(
    sw(V0, T9, OUT_CSUM),
    jr(RA),
    NOP,
)

# --- checksum ---
b.label("checksum")
csum_addr = b.at()
b.emit(
    lui(T8, DATA_HI),
    or_(V0, ZERO, ZERO),
    or_(T2, ZERO, ZERO),
)
b.label("cloop")
cloop_pc = b.at()
b.emit(
    sll(T3, T2, 2),
    addu(T3, T3, T8),
    lw(T4, T3, TABLE2),
    addiu(T5, T2, 1),
    mult(T4, T5),
    mflo(T6),
    addu(V0, V0, T6),
    addiu(T2, T2, 1),
    slti(T3, T2, 8),
)
cbne_pc = b.at()
b.emit(0, NOP)
b.emit(jr(RA), NOP)

# fix up branches now that targets are known
idx = lambda pc: (pc - TEXT_BASE) // 4  # noqa: E731
b.words[idx(bne_pc)] = bne(T5, ZERO, b.branch_off(bne_pc, loop_pc))
b.words[idx(beq_pc)] = beq(T0, ZERO, b.branch_off(beq_pc, b.labels["skip1"]))
b.words[idx(bne2_pc)] = bne(T0, ZERO, b.branch_off(bne2_pc, b.labels["skip2"]))
b.words[idx(jal_pc)] = jal(csum_addr)
b.words[idx(cbne_pc)] = bne(T3, ZERO, b.branch_off(cbne_pc, cloop_pc))

text = struct.pack("<%dI" % len(b.words), *b.words)
entry = b.labels["_start"]

# --- .data: counter/outs zero, tables initialized ---
data_words = [0] * 8
data_words.extend(range(1, 17))  # TABLE 1..16
data_words.extend([i * i for i in range(1, 9)])  # TABLE2 squares
data = struct.pack("<%dI" % len(data_words), *data_words)


def build_elf():
    shstr = b"\x00.text\x00.data\x00.shstrtab\x00"
    text_off = 0x100
    data_off = text_off + len(text)
    shstr_off = data_off + len(data)
    shoff = (shstr_off + len(shstr) + 15) & ~15

    def sh(name_off, stype, flags, addr, off, size):
        return struct.pack("<IIIIIIIIII", name_off, stype, flags, addr, off, size, 0, 0, 4, 0)

    n_text = shstr.index(b".text")
    n_data = shstr.index(b".data")
    n_shstr = shstr.index(b".shstrtab")
    SHT_PROGBITS, SHT_STRTAB = 1, 3
    SHF_ALLOC, SHF_EXEC, SHF_WRITE = 0x2, 0x4, 0x1
    shdrs = b"\x00" * 40
    shdrs += sh(n_text, SHT_PROGBITS, SHF_ALLOC | SHF_EXEC, TEXT_BASE, text_off, len(text))
    shdrs += sh(n_data, SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, DATA_BASE, data_off, len(data))
    shdrs += sh(n_shstr, SHT_STRTAB, 0, 0, shstr_off, len(shstr))

    eh = struct.pack(
        "<16sHHIIIIIHHHHHH",
        b"\x7fELF\x01\x01\x01\x00" + b"\x00" * 8,  # e_ident
        2,  # ET_EXEC
        8,  # EM_MIPS
        1,  # version
        entry,  # e_entry
        0x40,  # e_phoff (one phdr right after ehdr)
        shoff,  # e_shoff
        0,  # flags
        0x34,  # e_ehsize
        0x20,  # e_phentsize
        1,  # e_phnum
        0x28,  # e_shentsize
        4,  # e_shnum
        3,  # e_shstrndx
    )
    PT_LOAD = 1
    PF_RX = 0x5
    ph = struct.pack("<IIIIIIII", PT_LOAD, text_off, TEXT_BASE, TEXT_BASE,
                     (data_off + len(data)) - text_off, (data_off + len(data)) - text_off,
                     PF_RX, 0x1000)
    blob = bytearray(shoff + 4 * 40)
    blob[0:0x34] = eh
    blob[0x40:0x40 + 0x20] = ph
    blob[text_off:text_off + len(text)] = text
    blob[data_off:data_off + len(data)] = data
    blob[shstr_off:shstr_off + len(shstr)] = shstr
    blob[shoff:shoff + len(shdrs)] = shdrs
    return bytes(blob)


def c_array(name, payload):
    lines = [f"static const uint8_t {name}[] = {{"]
    for i in range(0, len(payload), 12):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in payload[i:i + 12]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    elf = build_elf()
    (out / "e2e.elf").write_bytes(elf)
    (out / "e2e_image.h").write_text(
        "// Generated by gen_e2e_elf.py — never committed.\n"
        "#pragma once\n#include <cstdint>\n"
        f"static const uint32_t kE2EEntry = 0x{entry:08x}u;\n"
        f"static const uint32_t kE2ETextBase = 0x{TEXT_BASE:08x}u;\n"
        f"static const uint32_t kE2EDataBase = 0x{DATA_BASE:08x}u;\n"
        f"static const uint32_t kE2ETextSize = {len(text)}u;\n"
        f"static const uint32_t kE2EDataSize = {len(data)}u;\n"
        + c_array("kE2EText", text)
        + "\n"
        + c_array("kE2EData", data)
        + "\n"
    )
    (out / "e2e.toml").write_text(
        "[general]\n"
        f'input = "{out / "e2e.elf"}"\n'
        f'output = "{out / "codegen"}"\n'
        "single_file_output = true\n"
        "patch_syscalls = false\n"
        "patch_cop0 = true\n"
        "patch_cache = true\n"
    )
    print(f"entry=0x{entry:08x} text={len(text)}B data={len(data)}B funcs=_start,checksum")


main()
