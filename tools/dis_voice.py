"""discord_voice.node の指定RVA範囲を逆アセンブル(文字列注釈付き)。
usage: python tools/dis_voice.py 0x19ab00 0x19ac00
"""
import sys,os
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from pe import PE
from capstone import Cs,CS_ARCH_X86,CS_MODE_64,CS_OP_MEM,CS_OP_IMM
from capstone.x86 import X86_REG_RIP
VOICE=r'C:\Users\utaaa\AppData\Local\Discord\app-1.0.9255\modules\discord_voice-1\discord_voice\discord_voice.node'
p=PE(VOICE); md=Cs(CS_ARCH_X86,CS_MODE_64); md.detail=True
def ann(insn):
    notes=[]
    for op in insn.operands:
        if op.type==CS_OP_MEM and op.mem.base==X86_REG_RIP:
            tgt=insn.address+insn.size+op.mem.disp
            s=p.cstr(tgt)
            if s and 2<=len(s)<80 and all(32<=ord(c)<127 for c in s): notes.append('"%s"'%s)
            else:
                sec=p.section_of(tgt)
                if sec: notes.append('%s:0x%X'%(sec,tgt))
        elif op.type==CS_OP_IMM and insn.mnemonic in ('call','jmp'):
            notes.append('sub_%X'%op.imm)
    return '  ; '+', '.join(notes) if notes else ''
lo,hi=int(sys.argv[1],0),int(sys.argv[2],0)
off=p.rva_to_off(lo)
for i in md.disasm(p.buf[off:off+(hi-lo)],lo):
    print('%08X  %-8s %-40s%s'%(i.address,i.mnemonic,i.op_str,ann(i)))
