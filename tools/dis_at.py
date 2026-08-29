"""任意の RVA を逆アセンブルする（内部実装の追跡用）。

usage: python tools/dis_at.py 0x837690 [命令数]
"""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import PE
from dis_exports import MODULE, disasm
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

pe = PE(MODULE)
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
rva = int(sys.argv[1], 0)
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 60
disasm(pe, md, rva, 'sub_%X' % rva, limit)
