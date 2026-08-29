"""RVA 範囲を素直に線形逆アセンブルする。

usage: python tools/dis_range.py 0x39400 0x39700
"""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import PE
from dis_exports import MODULE, annotate
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

pe = PE(MODULE)
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
lo, hi = int(sys.argv[1], 0), int(sys.argv[2], 0)
off = pe.rva_to_off(lo)
for insn in md.disasm(pe.buf[off:off + (hi - lo)], lo):
    print('%08X  %-9s %-44s%s' % (insn.address, insn.mnemonic, insn.op_str, annotate(pe, insn)))
