"""discord_krisp.node のエクスポート関数を逆アセンブルし、
RIP 相対で参照される文字列を注釈して表示する。

usage: python tools/dis_exports.py [関数名 ...] [--depth N] [--max N]
"""
import sys
import os
import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_OP_MEM, CS_OP_IMM, x86_const

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import PE

import glob as _glob
def _find_module():
    env = os.environ.get('KRISP_NODE')
    if env and os.path.exists(env):
        return env
    pat = os.path.join(os.environ['LOCALAPPDATA'],
        'Discord*', 'app-*', 'modules', 'discord_krisp-*', 'discord_krisp', 'discord_krisp.node')
    hits = _glob.glob(pat)
    hits.sort(key=os.path.getmtime, reverse=True)
    return hits[0] if hits else ''
MODULE = _find_module()


def annotate(pe, insn):
    """RIP 相対参照 / 即値から文字列や既知シンボルを引く。"""
    notes = []
    for op in insn.operands:
        if op.type == CS_OP_MEM and op.mem.base == x86_const.X86_REG_RIP:
            target = insn.address + insn.size + op.mem.disp
            sec = pe.section_of(target)
            s = pe.cstr(target)
            if s and len(s) >= 3:
                notes.append('"%s"' % s)
            else:
                w = pe.wstr(target)
                if w and len(w) >= 3:
                    notes.append('L"%s"' % w)
                elif sec:
                    notes.append('%s:0x%X' % (sec, target))
        elif op.type == CS_OP_IMM and insn.mnemonic in ('call', 'jmp'):
            notes.append('sub_%X' % op.imm)
    return '  ; ' + ', '.join(notes) if notes else ''


def disasm(pe, md, rva, name, limit=120, indent=''):
    off = pe.rva_to_off(rva)
    code = pe.buf[off:off + limit * 16]
    print('%s--- %s @ .text:0x%X ---' % (indent, name, rva))
    n = 0
    for insn in md.disasm(code, rva):
        print('%s  %08X  %-8s %-42s%s' % (
            indent, insn.address, insn.mnemonic, insn.op_str, annotate(pe, insn)))
        n += 1
        if insn.mnemonic == 'ret' or (insn.mnemonic == 'jmp' and insn.operands[0].type == CS_OP_IMM):
            break
        if n >= limit:
            print('%s  ... (打ち切り)' % indent)
            break
    print()


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    limit = 120
    for a in sys.argv[1:]:
        if a.startswith('--max='):
            limit = int(a.split('=')[1])

    pe = PE(MODULE)
    exp = pe.exports()
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True

    names = args or [n for n in sorted(exp) if n.startswith("Krisp")]
    for name in names:
        if name not in exp:
            print('!! エクスポートに %s がない' % name)
            continue
        disasm(pe, md, exp[name], name, limit)


if __name__ == '__main__':
    main()
