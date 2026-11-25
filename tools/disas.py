#!/usr/bin/env python3
"""Disassemble a VA range of OMNI.exe (self-contained, uses the local venv)."""
import sys
import capstone
import pefile

PE_PATH = "/home/niko/Загрузки/dexp-omni-re-kit/03-windows-app-extracted/OMNI.exe"

pe = pefile.PE(PE_PATH)
image_base = pe.OPTIONAL_HEADER.ImageBase

iat = {}
for d in pe.DIRECTORY_ENTRY_IMPORT:
    dll = d.dll.decode()
    for imp in d.imports:
        if imp.name:
            iat[imp.address] = f"{dll}!{imp.name.decode()}"

secs = [(image_base + s.VirtualAddress, s.Misc_VirtualSize, s) for s in pe.sections]


def va2off(va):
    for base, size, s in secs:
        if base <= va < base + max(size, s.SizeOfRawData):
            return s.PointerToRawData + (va - base)
    return None


md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True


def disas(start_va, nbytes, show_bytes=False):
    off = va2off(start_va)
    if off is None:
        print(f"{start_va:#x}: not in any section")
        return
    data = pe.__data__[off:off + nbytes]
    for ins in md.disasm(data, start_va):
        line = f"{ins.address:#010x}:  "
        if show_bytes:
            line += f"{ins.bytes.hex():20s} "
        line += f"{ins.mnemonic} {ins.op_str}"
        if ins.mnemonic in ("call", "jmp") and "[" in ins.op_str:
            try:
                tgt = int(ins.op_str.split("[")[1].rstrip("]"), 16)
                if tgt in iat:
                    line += f"     ; {iat[tgt]}"
            except ValueError:
                pass
        print(line)


if __name__ == "__main__":
    start = int(sys.argv[1], 16)
    n = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x80
    disas(start, n, "-b" in sys.argv)
