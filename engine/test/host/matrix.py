#!/usr/bin/env python3
"""Repo-resident case matrix: QMK vim engine vs real vim.tiny.

Run with `make matrix-test` (from engine/) or `python3 test/host/matrix.py`.

Each case feeds a vim key sequence to the engine probe (built from the current
`engine/src/*.c`) and to real `/usr/bin/vim.tiny`, applies the emitted host key
stream to the host-editor model in `kvhost.py`, and compares:

  * buffer content   (model buffer == Vim buffer)
  * unnamed register (model clipboard == Vim register text)
  * cursor position  (same line, column within 1)

A case that matches on all three dimensions PASSes.  A mismatch is an XFAIL if
the case is listed in `XFAIL` below with a declared-deviation ID (see
`DEVIATIONS`: §4.9's VISUAL_LINE ①–⑩ plus the other deviations declared in
`vim/design.md` §4.4/§4.9 and `readme.md`); otherwise it is a FAIL and the
process exits 1.

Exit status: 0 iff every case either matches or is a declared deviation **and no
listed deviation has started matching** (strict xfail — an XPASS also exits 1,
so the table cannot silently rot and mask a fixed defect).
"""
import os
import sys
from collections import Counter

import kvhost as ih
from kvhost import Case

B3 = 'one two three\nfour five six\nseven eight\n'
B4 = 'L1\nL2\nL3\nL4\n'
B12 = ''.join('L%02d\n' % i for i in range(1, 13))
BW = 'alpha beta gamma\ndelta epsilon zeta\n'
BMIX = 'aaa bbb\ncc dddd e\nf\nggg hhh iii\n'
BABC = 'abcdefghij\nklmnopqrst\n'   # P1-1/D26 report buffer (cursor 0,0)

cases = []


def add(name, buf, line, col, vim, eng=None, esc=False, note='', wm='vimw'):
    cases.append(Case(name, buf, line, col, vim, eng, esc, note=note, word_model=wm))


# ============================ NORMAL motions ============================
for k in ['h', 'j', 'k', 'l', 'w', 'W', 'b', 'B', 'e', 'E', '0', '^', '$', 'gg', 'G']:
    add('mot-' + k, B3, 0, 3, k)
add('mot-h0', B3, 0, 0, 'h')
add('mot-l-eol', B3, 0, 12, 'l')
add('mot-j-last', B3, 2, 0, 'j')
add('mot-k-first', B3, 0, 0, 'k')
add('mot-3j', B12, 0, 0, '3j')
add('mot-2w', BW, 0, 0, '2w')
add('mot-99j', B12, 0, 0, '99j')
add('mot-12j', B12, 0, 0, '12j')
add('mot-3G', B12, 0, 0, '3G', note='G drops count (declared 7)')
add('mot-3gg', B12, 5, 0, '3gg', note='gg drops count (declared 7)')
# cursor probes: motion + x
for k in ['w', 'W', 'b', 'B', 'e', 'E', '0', '^', '$', 'gg', 'G', 'j', 'k', 'h', 'l']:
    add('probe-' + k + 'x', B3, 1, 5, k + 'x')
add('probe-3jx', B12, 0, 0, '3jx')
add('probe-2wx', BW, 0, 0, '2wx')

# ============================ NORMAL single keys ============================
for k in ['x', 'X', 's', 'D', 'C', 'Y', 'J', 'u', 'p', 'P']:
    add('key-' + k, B3, 0, 3, k, esc=(k == 's'))
add('key-x-first', B3, 0, 0, 'x')
add('key-X-mid', B3, 0, 5, 'X')
add('key-J-last', B4, 3, 0, 'J')
add('key-3x', B3, 0, 0, '3x')
add('key-3X', B3, 0, 5, '3X')
add('key-3s', B3, 0, 0, '3s', esc=True)
add('key-3J', B3, 0, 0, '3J')
add('key-3D', B3, 0, 3, '3D')
add('key-3C', B3, 0, 3, '3C', esc=True)
add('key-3Y', B3, 0, 3, '3Y')
# register-sensitive follow-ups (buffer-visible)
add('xp', B3, 0, 0, 'xp', note='swap idiom')
add('xp2', B3, 0, 0, 'xpxp')
add('xP', B3, 0, 0, 'xP')
add('Xp', B3, 0, 3, 'Xp')
add('ddp', B4, 1, 0, 'ddp')
add('ddP', B4, 1, 0, 'ddP')
add('yyx', B4, 1, 0, 'yyx')
add('yyp', B4, 1, 0, 'yyp')
add('yyP', B4, 1, 0, 'yyP')
add('ywp', BW, 0, 0, 'ywp')
add('yep', BW, 0, 0, 'yep')
add('y$p', B3, 0, 0, 'y$p')
add('Yp', B4, 1, 0, 'Yp')
add('Dp', B3, 0, 3, 'Dp')
add('Cp', B3, 0, 3, 'Cp', esc=True)
add('3xp', B3, 0, 0, '3xp')

# ============================ NORMAL operators ============================
for op in ['d', 'y', 'c']:
    for m in ['w', 'W', 'b', 'B', 'e', 'E', '$', '0', '^', 'j', 'k', 'l', 'h', 'G', 'gg']:
        add('%s%s' % (op, m), B3, 0, 3, op + m, esc=(op == 'c'))
        add('%s%s' % (op, m), BW, 1, 6, op + m, esc=(op == 'c'))
for op in ['d', 'y', 'c']:
    for m in ['j', 'k', 'G', 'gg', 'w', 'b', '$']:
        add('%s%s-b' % (op, m), B4, 1, 0, op + m, esc=(op == 'c'))
add('2dw', B3, 0, 0, '2dw')
add('d2w', B3, 0, 0, 'd2w')
add('2d3w', B3, 0, 0, '2d3w')
add('3dw', B3, 0, 0, '3dw')
add('2yw', B3, 0, 0, '2yw')
add('2cw', B3, 0, 0, '2cw', esc=True)
add('2dj', B4, 1, 0, '2dj')
add('djx', B4, 1, 0, 'djx')
add('djy', B4, 1, 0, 'djy')
add('dkx', B4, 1, 0, 'dkx')
add('dGx', B4, 1, 0, 'dGx')
add('dggx', B4, 2, 0, 'dggx')
add('2ddx', B4, 1, 0, '2ddx')
add('2ccx', B4, 1, 0, '2ccx', esc=True)
add('2ddp', B4, 1, 0, '2ddp')
add('ddp2', B4, 0, 0, 'ddp')
add('ddpp', B4, 1, 0, 'ddpp')

# ============================ line ops ============================
for n in ['', '2', '3']:
    add(n + 'dd', B4, 1, 0, n + 'dd')
    add(n + 'yy', B4, 1, 0, n + 'yy')
    add(n + 'cc', B4, 1, 0, n + 'cc', esc=True)
    add(n + '>>', B4, 1, 0, n + '>>')
    add(n + '<<', B4, 1, 0, n + '<<')
add('S', B4, 1, 0, 'S', esc=True)
add('3S', B4, 1, 0, '3S', esc=True)
add('dd-first', B4, 0, 0, 'dd')
add('cc-first', B4, 0, 0, 'cc', esc=True)
add('dd-last', B4, 3, 0, 'dd')
add('yy-last', B4, 3, 0, 'yy')
add('cc-last', B4, 3, 0, 'cc', esc=True)
add('dd-mid-col', B4, 1, 1, 'dd')
add('yy-mid-col', B4, 1, 1, 'yy')
add('2dd-last', B4, 2, 0, '2dd')
add('99dd', B12, 0, 0, '99dd')
add('ddx', B4, 1, 0, 'ddx')
add('ddxx', B4, 1, 0, 'ddxx')
add('ccx', B4, 1, 0, 'ccx', esc=True)
add('yyy', B4, 1, 0, 'yyy')

# ============================ indent ============================
for m in ['j', 'k', 'G', 'gg', 'w', 'b', '$', '0', 'e', 'l', 'h']:
    add('>%s' % m, B4, 1, 0, '>%s' % m)
    add('<%s' % m, B4, 1, 0, '<%s' % m)
add('>2j', B4, 1, 0, '>2j')
add('2>3j', B4, 0, 0, '2>3j')
add('>j-x', B4, 1, 0, '>jx')
add('>k-x', B4, 1, 0, '>kx')
add('>>x', B4, 1, 0, '>>x')
add('2>>x', B4, 1, 0, '2>>x')
add('>>j', B4, 1, 0, '>>j')
add('>>l', B4, 1, 0, '>>l')
add('>>p', B4, 1, 0, '>>p')
add('>G-x', B4, 1, 0, '>Gx')
add('gg>G', B4, 0, 0, '>G')

# ============================ insert entries (cursor probes) ============================
for k in ['i', 'I', 'a', 'A', 'o', 'O']:
    add('ins-' + k + '-x', B4, 1, 1, k + '\\ex', esc=False, note='enter insert, Esc, then x')
    add('ins-' + k + '-p', B4, 1, 1, k + '\\ep')
add('ins-3i-x', B4, 1, 1, '3i\\ex')
add('ins-o-last-x', B4, 3, 0, 'o\\ex')
add('ins-O-first-x', B4, 0, 0, 'O\\ex')

# ==================== insert + Esc + `.` (D26, design §4.15) ====================
# Leaving INSERT after a NON-EMPTY insert must emit one `Left` (the host cursor sits
# after the inserted text, Vim sits on the last inserted char); otherwise `.` replays
# one column too far right and corrupts the buffer.  The Esc is written EXPLICITLY as
# `\e` (not via the `esc=` suffix) so the engine itself sees it and runs the
# INSERT->NORMAL commit.  `\r` is Enter (a real CR byte on the vim side).
for nm, k, b, l, c in [
    ('ins-iAB-dot',        'iAB\\e.',      B4, 1, 1),
    ('ins-iABCDEFG-dot',   'iABCDEFG\\e.', B4, 1, 1),
    ('ins-iA-CR-B-dot',    'iA\\rB\\e.',   B4, 1, 1),
    ('ins-AX-dot',         'AX\\e.',       B4, 1, 1),
    ('ins-IX-dot',         'IX\\e.',       B4, 1, 1),
    ('ins-oXY-dot',        'oXY\\e.',      B4, 1, 1),
    ('ins-oX-CR-Y-dot',    'oX\\rY\\e.',   B4, 1, 1),
    ('ins-OX-CR-Y-dot',    'OX\\rY\\e.',   B4, 1, 1),
    ('ins-sX-dot',         'sX\\e.',       B4, 1, 1),
    ('ins-CX-dot',         'CX\\e.',       B4, 1, 1),
    ('ins-ccX-dot',        'ccX\\e.',      B4, 1, 1),
    # the exact buffer/cursor from the P1-1 report
    ('ins-iAB-dot-b',      'iAB\\e.',      BABC, 0, 0),
    ('ins-iABCDEFG-dot-b', 'iABCDEFG\\e.', BABC, 0, 0),
    ('ins-iA-CR-B-dot-b',  'iA\\rB\\e.',   BABC, 0, 0),
    # empty insert at column 0: Vim does not move, the host must not either
    ('ins-i-first-x',      'i\\ex',        B4, 0, 0),
]:
    add(nm, b, l, c, k, note='D26: insert + Esc + dot')

# ============================ prefixes ============================
add('gx', B3, 0, 3, 'gx')
add('gG', B3, 0, 3, 'gG')
add('Zx', B3, 0, 3, 'Zx')
add('ZZ', B3, 0, 3, 'ZZ')
add('g\\e', B3, 0, 3, 'g\\e')
add('Z\\e', B3, 0, 3, 'Z\\e')
add('d\\e', B3, 0, 3, 'd\\e')
add('3\\e', B3, 0, 3, '3\\e')
add('dgg', B4, 2, 0, 'dgg')
add('ygg', B4, 2, 0, 'ygg')
add('cgg', B4, 2, 0, 'cgg', esc=True)
add('>gg', B4, 2, 0, '>gg')

# ============================ repeat ============================
add('x-dot', B3, 0, 0, 'x.')
add('dw-dot', B3, 0, 0, 'dw.')
add('dd-dot', B4, 0, 0, 'dd.')
add('3x-dot', B3, 0, 0, '3x.')
add('dd-dot2', B4, 0, 0, 'dd..')
add('x-w-dot', B3, 0, 0, 'xw.')
add('x-yy-dot', B4, 0, 0, 'xyy.')
add('x-J-dot', B3, 0, 0, 'xJ.')

# ============================ VISUAL charwise ============================
for m in ['h', 'l', 'j', 'k', 'w', 'W', 'b', 'B', 'e', 'E', '0', '^', '$', 'G', 'gg']:
    add('vis-' + m, B3, 0, 3, 'v' + m)
    add('vis-' + m + 'd', B3, 0, 3, 'v' + m + 'd')
    add('vis-' + m + 'y', B3, 0, 3, 'v' + m + 'y')
add('vis-3j', B12, 0, 0, 'v3j')
add('vis-2w', BW, 0, 0, 'v2w')
add('vis-10j', B12, 0, 0, 'v10j')
add('vis-99j', B12, 0, 0, 'v99j')
add('vis-123j', B12, 0, 0, 'v123j')
for a in ['d', 'y', 'c', 's', 'x', 'p']:
    add('vis-ll' + a, B3, 0, 0, 'vll' + a, esc=(a in 'cs'))
add('vis-ll' + 'd', B3, 0, 0, 'vlld')
add('vis-d', B3, 0, 3, 'vd')
add('vis-y', B3, 0, 3, 'vy')
add('vis-jd', B3, 0, 0, 'vjd')
add('vis-jy', B3, 0, 0, 'vjy')
add('vis-ky', B3, 1, 3, 'vky')
add('vis-Esc', B3, 0, 3, 'v\\e')
add('vis-3Esc', B3, 0, 3, 'v3\\e')
add('vis-Esc-x', B3, 0, 3, 'v\\ex')
add('vis-llEsc-x', B3, 0, 0, 'vll\\ex')
add('vis-llEsc-p', B3, 0, 0, 'vll\\ep')
add('vis-jEsc-x', B3, 0, 0, 'vj\\ex')
add('vis-gg-ill', B3, 0, 3, 'vgx')
add('vis-gG', B3, 0, 3, 'vgG')
add('vis-dot', B3, 0, 3, 'v\\e.')
add('vis-j-y-p', B3, 0, 0, 'vjy')
add('vis-V', B3, 0, 3, 'vV')
add('vis-Vy', B3, 0, 3, 'vVy')
add('vis-Vd', B3, 0, 3, 'vVd')
add('vis-y-p', B3, 0, 0, 'vlyp')
add('vis-p', B3, 0, 0, 'vllp')
add('vis-99l', B12, 0, 0, 'v99l')
add('vis-99ld', B12, 0, 0, 'v99ld')

# ============================ VISUAL_LINE ============================
for m in ['j', 'k', 'w', 'b', 'e', 'G', 'gg', 'h', 'l', '0', '^', '$']:
    add('vl-' + m, B4, 1, 0, 'V' + m)
    add('vl-' + m + 'd', B4, 1, 0, 'V' + m + 'd')
    add('vl-' + m + 'y', B4, 1, 0, 'V' + m + 'y')
for a in ['d', 'y', 'c', 's', 'x', 'p']:
    add('vl-' + a, B4, 1, 0, 'V' + a, esc=(a in 'cs'))
add('vl-2j', B12, 0, 0, 'V2j')
add('vl-2k', B12, 5, 0, 'V2k')
add('vl-3j-y', B12, 0, 0, 'V3jy')
add('vl-jj-y', B12, 0, 0, 'Vjjy')
add('vl-kk-y', B12, 5, 0, 'Vkky')
add('vl-jk-y', B12, 3, 0, 'Vjky')
add('vl-kj-y', B12, 3, 0, 'Vkjy')
add('vl-gg-y', B12, 5, 0, 'Vggy')
add('vl-G-y', B12, 5, 0, 'VGy')
add('vl-gg-j-y', B12, 5, 0, 'Vggjy')
add('vl-G-k-y', B12, 5, 0, 'VGky')
add('vl-G-k-k-k-y', B12, 5, 0, 'VGkkky', note='abs reverse past anchor (declared 4)')
add('vl-gg-j-j-j-y', B12, 5, 0, 'Vggjjjy', note='abs reverse past anchor (declared 4)')
add('vl-gg-j-d', B12, 5, 0, 'Vggjd')
add('vl-G-k-d', B12, 5, 0, 'VGkd')
add('vl-99k-y', B12, 5, 0, 'V99ky', note='reanchor cap')
add('vl-99j-y', B12, 5, 0, 'V99jy')
add('vl-j-y-p', B4, 1, 0, 'Vjyp')
add('vl-y-p', B4, 1, 0, 'Vyp')
add('vl-Esc', B4, 1, 0, 'V\\e')
add('vl-Esc-x', B4, 1, 0, 'V\\ex')
add('vl-v-y', B4, 1, 0, 'Vvy')
add('vl-v', B4, 1, 0, 'Vv')
add('vl-l-y', B4, 1, 0, 'Vly')
add('vl-0-y', B4, 1, 2, 'V0y')
add('vl-dollar-y', B4, 1, 0, 'V$y')
add('vl-j-x', B4, 1, 0, 'Vjx')
add('vl-j-2', B12, 0, 0, 'Vj2')

# ============================ extra targeted probes (round 2) ============================
# up-direction line ops / indent: does the CURRENT line get included?
for op in ['d', 'y', 'c', '>', '<']:
    for m in ['k', '2k', 'gg', 'G']:
        add('up-%s%s' % (op, m), B4, 1, 0, op + m, esc=(op == 'c'))
add('up-dk-col2', B4, 1, 1, 'dk')
add('up->k-col2', B4, 1, 1, '>k')
# dd / yy cursor on the last line, probed with a following x/p
add('last-Gddx', B4, 3, 0, 'Gddx')
add('last-Gddp', B4, 3, 0, 'Gddp')
add('last-GddP', B4, 3, 0, 'GddP')
add('last-jddx', B4, 1, 0, 'jddx')
add('last-jddp', B4, 1, 0, 'jddp')
add('last-jddP', B4, 1, 0, 'jddP')
# charwise visual leftward / word motions
add('v-h0-d', B3, 0, 0, 'vhd')
add('v-h0-y', B3, 0, 0, 'vhy')
add('v-h0-p', B3, 0, 0, 'vhp')
add('v-h2-d', B3, 0, 2, 'vhhd')
add('v-b0-d', B3, 0, 0, 'vbd')
add('v-b0-y', B3, 0, 0, 'vby')
add('v-e0-d', B3, 0, 0, 'ved')
add('v-w0-d', B3, 0, 0, 'vwd')
add('v-w0-y', B3, 0, 0, 'vwy')
add('v-dollar-d', B3, 0, 3, 'v$d')
add('v-dollar-y', B3, 0, 3, 'v$y')
add('v-dollar-p', B3, 0, 3, 'v$p')
add('v-G-d', B3, 0, 3, 'vGd')
add('v-G-y', B3, 0, 3, 'vGy')
add('v-gg-d', B3, 0, 3, 'vggd')
add('v-gg-y', B3, 0, 3, 'vggy')
add('v-j-d', B3, 0, 0, 'vjd')
add('v-j-y', B3, 0, 0, 'vjy')
add('v-k-y', B3, 1, 0, 'vky')
add('v-jj-d', B3, 0, 0, 'vjjd')
add('v-lld', B3, 0, 0, 'vlld')
add('v-llld', B3, 0, 0, 'vllld')
add('v-ld', B3, 0, 0, 'vld')
add('v-lld0', B3, 0, 5, 'vlld')
add('v-Esc-x0', B3, 0, 0, 'v\\ex')
add('v-Esc-p0', B3, 0, 0, 'v\\ep')
add('v-Esc-x-mid', B3, 0, 5, 'v\\ex')
# VISUAL_LINE direction / reanchor
add('vl-k-d0', B4, 2, 0, 'Vkd')
add('vl-k-y0', B4, 2, 0, 'Vky')
add('vl-k-k-d', B4, 3, 0, 'Vkkd')
add('vl-j-k-y', B4, 1, 0, 'Vjky')
add('vl-k-j-y', B4, 2, 0, 'Vkjy')
add('vl-gg-d', B12, 5, 0, 'Vggd')
add('vl-gg-y2', B12, 5, 0, 'Vggy')
add('vl-G-d', B12, 5, 0, 'VGd')
add('vl-G-y2', B12, 5, 0, 'VGy')
add('vl-G-k-k-k-d', B12, 5, 0, 'VGkkkd')
add('vl-gg-j-j-j-d', B12, 5, 0, 'Vggjjjd')
add('vl-2j-d', B12, 0, 0, 'V2jd')
add('vl-2k-d', B12, 5, 0, 'V2kd')
add('vl-99j-d', B12, 0, 0, 'V99jd')
add('vl-99k-d', B12, 5, 0, 'V99kd')
add('vl-j-x', B4, 1, 0, 'Vjx')
add('vl-j-p', B4, 1, 0, 'Vjp')
add('vl-y-x', B4, 1, 0, 'Vyx')
add('vl-y-p', B4, 1, 0, 'Vyp')
add('vl-Esc-x0', B4, 1, 0, 'V\\ex')
add('vl-v-y0', B4, 1, 0, 'Vvy')
add('vl-v-d', B4, 1, 0, 'Vvd')
add('vl-v-x', B4, 1, 0, 'Vvx')
add('vl-h-x', B4, 1, 2, 'Vhx')
add('vl-l-x', B4, 1, 0, 'Vlx')
add('vl-dollar-x', B4, 1, 0, 'V$x')
add('vl-w-x', B4, 1, 0, 'Vwx')
add('vl-b-x', B4, 1, 0, 'Vbx')
add('vl-s-x', B4, 1, 0, 'Vs\\ex', esc=False)
# EOL / trailing newline inherent probes
add('eol-dollar-x', B3, 0, 0, '$x')
add('eol-dollar-a', B3, 0, 0, '$a\\ex')
add('eol-dollar-p', B3, 0, 0, '$p')
add('eol-G-x', B3, 0, 0, 'Gx')
add('eol-G-p', B3, 0, 0, 'Gp')
add('eol-G-a', B3, 0, 0, 'Ga\\ex')
add('eol-J-last', B4, 3, 0, 'J')
add('eol-J-2ndlast', B4, 2, 0, 'J')
add('eol-3J', B3, 0, 0, '3J')
add('eol-2J', B3, 0, 0, '2J')
# repeat family
add('rep-x-dot-x', B3, 0, 0, 'x.x')
add('rep-dw-dot', B3, 0, 0, 'dw.')
add('rep-dk-dot', B4, 2, 0, 'dk.')
add('rep-dgg-dot', B4, 2, 0, 'dgg.')
add('rep->>-dot', B4, 1, 0, '>>.')
add('rep-ddp', B4, 1, 0, 'ddp')

# ============ counted dot-repeat (P2-2/D27, design §4.14 #3) ============
# `N.` REPLACES the recorded command's own count (a bare `.` replays it whole):
# `2x3.` ≡ `3x`, `d2w3.` ≡ `3dw`, `>2j3.` ≡ `>3j`.  The insert class repeats the
# whole insertion N times and compensates the D26 `Left` only on the LAST repeat,
# so `iAB<Esc>2.` = AABABB (flat) while `iAB<Esc>..` = AAABBB (nested).
BONE = 'one two three four\nfive six seven\n'
B5  = 'L1\nL2\nL3\nL4\nL5\n'                 # long enough for `3dd2.` to stay meaningful
B5W = 'one two\nthree four\nfive six\nseven eight\nnine ten\n'   # ditto for `2J3.`/`J3.`
add('dot2x-dot',          BABC, 0, 0, '2x.')
add('dot2x-2dot',         BABC, 0, 0, '2x2.')
add('dot2x-3dot',         BABC, 0, 0, '2x3.')
add('dot3x-2dot',         BABC, 0, 0, '3x2.')
add('dotx-3dot',          BABC, 0, 0, 'x3.')
add('dot2x-1dot',         BABC, 0, 0, '2x1.')     # `1.` is NOT `.`
add('dot4x-2dot',         BONE, 0, 0, '4x2.')
add('dot2X-3dot',         BABC, 0, 2, '2X3.')     # xfail D16 (X at column 0)
add('dotdw-3dot',         BW,   0, 0, 'dw3.')
add('dot2dw-3dot',        BW,   0, 0, '2dw3.')
add('dotd2w-3dot',        BW,   0, 0, 'd2w3.')
add('dotdd-2dot',         B4,   0, 0, 'dd2.')
add('dot2dd-3dot',        B4,   0, 0, '2dd3.')
add('dot3dd-2dot',        B5,   0, 0, '3dd2.')
add('dot2J-3dot',         B5W,  0, 0, '2J3.')
add('dotJ-3dot',          B5W,  0, 0, 'J3.')
add('dot2>>-3dot',        B4,   0, 0, '2>>3.')
add('dot>>-2dot',         B4,   0, 0, '>>2.')
# NB: `dot2p-3dot` (`yy2p3.`) and `dot2CAB-2dot` (`2CAB\e2.`) are deliberately NOT here:
# their EXPLICIT equivalents (`yy2p3p` / `2CAB\e2CAB\e`) fail identically in the engine
# (counted paste interleaves on the 2nd repeat; `2C` on a 1-line buffer eats the line
# break), i.e. they test pre-existing base-command defects, not the D27 count rule.
# The count rule for `p`/`C` is pinned by test_main.c's test_dot_count_replace instead.
add('dotd0-3dot',         BABC, 0, 5, 'd03.')     # `0` is a MOTION, not a count; xfail D16
add('dot2d0-3dot',        BABC, 0, 5, '2d03.')    # xfail D16
add('dot2s-3dot',         BABC, 0, 0, '2s\\e3.')
add('dotCAB-2dot',        BABC, 0, 0, 'CAB\\e2.')
add('dotins-iAB-2dot',    BABC, 0, 0, 'iAB\\e2.')
add('dotins-iAB-3dot',    BABC, 0, 0, 'iAB\\e3.')
add('dotins-iAB-dotdot',  BABC, 0, 0, 'iAB\\e..')  # `..` nests, `2.` does not
add('dotins-iAB-1dot',    BABC, 0, 0, 'iAB\\e1.')
add('dotins-iX-3dot',     BABC, 0, 0, 'iX\\e3.')
add('dotins-iA-CR-B-2dot', BABC, 0, 0, 'iA\\rB\\e2.')
add('dotins-oXY-2dot',    BABC, 0, 0, 'oXY\\e2.')
add('dotins-OXY-2dot',    BABC, 0, 0, 'OXY\\e2.')  # xfail DOTINS (cursor-only, O entry)
add('dotins-AX-2dot',     BABC, 0, 0, 'AX\\e2.')
add('dotins-IX-2dot',     BABC, 0, 0, 'IX\\e2.')
add('dotins-ccA-2dot',    B4,   0, 0, 'ccA\\e2.')
# pending / partial input before `.` must not replay a stale target
add('dot-pending-d',      BABC, 0, 5, 'd.')
add('dot-pending-2d',     BABC, 0, 5, '2d.')
add('dot-nolast-2dot',    BABC, 0, 0, '2.')

# ============================ paste/register cases (audit pp.py) ============================
# NB: the old list contained 'yl l p' — a **literal space** key.  The host model does not
# implement space-as-motion, so that case tested the model, not the engine; and its generated
# name collided with 'yllp' (hiding it).  Dropped, and the name now escapes spaces.
for k in ['ylp', 'ylP', 'yhp', 'yhP', 'yep', 'yeP', 'ywp', 'ywP', 'yllp', 'yllP']:
    add('pp-' + k.replace(' ', '_'), B3, 0, 0, k)   # '_' 保名唯一：'yl l p' 与 'yllp' 曾同名
add('pp-xp', B3, 0, 0, 'xp')
add('pp-xP', B3, 0, 0, 'xP')
add('pp-Xp', B3, 0, 5, 'Xp')
add('pp-XP', B3, 0, 5, 'XP')
add('pp-lp', B3, 0, 0, 'lp')
add('pp-lP', B3, 0, 0, 'lP')
add('pp-plain-p', B3, 0, 0, 'p')
add('pp-plain-P', B3, 0, 0, 'P')
add('pp-3ylp', B3, 0, 0, '3ylp')
add('pp-yl-dot-p', B3, 0, 0, 'yl.p')
add('pp-x-then-P', B3, 0, 0, 'xP')
add('pp-dd-then-P', B4, 1, 0, 'ddP')
# visual yank then paste
add('pp-vlyp', B3, 0, 0, 'vlyp')
add('pp-vlyP', B3, 0, 0, 'vlyP')
add('pp-vllp', B3, 0, 0, 'vllp')
add('pp-Vyp', B4, 1, 0, 'Vyp')
add('pp-VyP', B4, 1, 0, 'VyP')
# yank cursor restore
add('pp-yw-x', B3, 0, 0, 'ywx')
add('pp-yw-w', B3, 0, 0, 'yww')
add('pp-ye-x', B3, 0, 0, 'yex')
add('pp-yb-x', B3, 0, 0, 'ybx')
add('pp-yj-x', B4, 1, 0, 'yjx')
add('pp-yk-x', B4, 1, 0, 'ykx')
add('pp-yG-x', B4, 1, 0, 'yGx')
add('pp-ygg-x', B4, 1, 0, 'yggx')
# J family
add('pp-J-mid', B3, 0, 0, 'J')
add('pp-J-empty', B4, 2, 0, 'J')
add('pp-2J', B3, 0, 0, '2J')
add('pp-J-leadspace', 'aaa\n   bbb\n', 0, 0, 'J')
# s / C / D / Y register
add('pp-sp', B3, 0, 0, 'sp', esc=True)
add('pp-Cp', B3, 0, 0, 'Cp', esc=True)
add('pp-Dp', B3, 0, 0, 'Dp')
add('pp-Yp', B4, 1, 0, 'Yp')
add('pp-Sp', B4, 1, 0, 'Sp', esc=True)
add('pp-ccp', B4, 1, 0, 'ccp', esc=True)


# ============================ declared deviations (xfail table) ============================
# vim/design.md §4.9 "known deviations" list ①–⑩.  Every xfail below cites the
# deviation ID that explains the mismatch, so the mapping is auditable.  A case
# that is not listed here and does not match real Vim is a hard failure.
DEVIATIONS = {
    '①': 'VISUAL_LINE w/e/b are approximated as whole-line ±1 motion (design §4.9 ①)',
    '②': 'VISUAL_LINE action leaves the host cursor at the active end; Vim leaves it on the selection first line (design §4.9 ②)',
    '③': 'last line without a trailing newline: the action Shift+Right is a no-op (design §4.9 ③)',
    '④': 'in the abs state (after G/gg) reverse motion past the anchor is wrong (design §4.9 ④)',
    '⑤': 'V y then p: the register is linewise but Normal p only sends Ctrl+V (no linewise paste position) (design §4.9 ⑤)',
    '⑥': 'VISUAL_LINE span is capped at 100 lines (design §4.9 ⑥)',
    '⑦': 'G/gg drop the count (3G/3gg land on the doc end / start, not line N) (design §4.9 ⑦)',
    '⑧': 'visual p does not update the host clipboard (design §4.9 ⑧)',
    '⑨': 'V then v keeps the whole-line host selection instead of collapsing to one char (design §4.9 ⑨)',
    '⑩': 'V j motion selection lacks the trailing newline until the action adds it (design §4.9 ⑩)',
    # ---- deviations declared OUTSIDE the §4.9 VISUAL_LINE ①–⑩ list ----
    # (the ①–⑩ above are local to §4.9's VISUAL_LINE list; these IDs are
    #  namespaced so the two numbering schemes cannot be confused)
    'D2':     'd/c/y + word motion at EOL eats the newline (Ctrl+Shift+Right crosses lines) (design §4.9 D2)',
    'E-W':    'e/E are approximated as w (Ctrl+Right is vim w) (readme §2, design §4.9)',
    'VCUR':   'charwise VISUAL cursor sits after the last selected char; Vim sits on it (design §4.9)',
    'YCOL':   'yank cannot restore the column ($ -> Home) or the line (G/gg -> doc end) (design §4.9)',
    'D14':    '$x / Gx off-by-one at EOL / buffer end: host cannot express "on the char" (design §4.4)',
    'D15':    'J inserts a space on an empty next line and does not strip leading whitespace (design §4.4)',
    'D16':    'empty host selection (failed motion / motion at line start) makes Ctrl+X/C cut or copy the whole line (design §4.4)',
    # RESOLVED (2026-09, historical): `cgg` on line 1/2/3 now matches real Vim exactly (verified).
    # No case cites it any more; kept so the numbering stays stable.
    'D17':    'RESOLVED/historical: cgg at line 0 used to cut the line instead of leaving a blank line (design §4.4)',
    # 2026-10 RE-CLASSIFIED (D26 audit): D18 was declared as "a host insert cursor does not
    # move left on Esc" = cursor-only, keymap layer, outside the engine.  MEASURED: it is
    # BUFFER/REGISTER visible -- after an EMPTY insert the next command acts one column to
    # the right (ins-i-x is buf+reg, not cur).  The non-empty case is fixed in-engine by D26
    # (design §4.15); the empty case is the remaining inherent deviation (design §4.9 15).
    'D18':    'EMPTY-insert exit is not compensated: the next command acts one column right (buffer/register visible, not merely the cursor) (design §4.9 15, §4.15)',
    # 2026-10 (D26 audit): the six ins-* entries below used to cite D18, but their real cause
    # is unrelated to the insert cursor (verified: `o<Esc>` alone matches Vim, `x`/`p` do not).
    'XEMPTY': 'x/s on an EMPTY line: Vim is a no-op, but the host Shift+Right selects the line break and Ctrl+X deletes it (design §4.9 16)',
    'PEMPTY': 'p/P positioning on an EMPTY line: the charwise Right crosses the line break, so the host cursor lands on the next line (design §4.9 17)',
    'IND':    'indent leaves the cursor at the edit point, not the first non-blank of the range first line (design §4.4 ①②③)',
    'PASTEC': 'dd on the last line / linewise p,P leave the cursor at the pasted text end (design §4.4)',
    'EOLDEL': 'after deleting at EOL the host cursor sits on the newline; Vim moves left (design §4.4)',
    'VCAP': 'charwise VISUAL offset cap (KV_VCHAR_MAX_OFF 100): past the cap the engine refuses to extend, so a counted motion can select less than Vim (design §4.9)',
    # FIXED by D24 (2026-09): the +1 column offset after `k` made a vertical charwise-VISUAL selection
    # cover the wrong column.  Verified: vky/vkd/vkx match real Vim (44 FIXED / 0 REGRESSED).
    'VBLOCK': 'FIXED by D24: after k the +1 column offset made a vertical charwise-VISUAL selection one column off (design §4.9)',
    # INHERENT, documented in design §4.9: the register must hold the text that occupied the selection
    # BEFORE the paste, but the host has a single clipboard slot which the paste consumes first.
    # Measured: vhp/vlp/vwp/vjp buffer is always correct, only the register differs; `P` matches Vim.
    'VPASTE': 'INHERENT (design §4.9): Vim visual p writes the REPLACED text into the register; single host clipboard cannot (buffer stays correct, only the register differs)',
    'D23': 'linewise VISUAL Esc lands on the first line/column 0 of the selection; the host used to stay at the active line end (fixed 2026-09, design §4.9)',
    'FAILMOT': 'a motion that FAILS in Vim (k on line 1, j on the last line) aborts the operator; the host arrow keys only clamp, so d/c/> still act (design §4.4)',
    'GPFX':   'only the g->gg prefix is implemented; other g/Z continuations are swallowed (design §4.4)',
    # 2026-10 (P2-2/D27): the insert-class `O` (open-above) entry.  The BUFFER matches
    # Vim (`OXY<Esc>2.` = 3 `XY` lines), but Vim's counted repeat leaves the cursor on
    # line N-1 while the engine (like N explicit `OXY<Esc>`) leaves it on line 0; the
    # other insert entries (i/a/A/I/o) match exactly.  Cursor-only (design §4.14 #3).
    'DOTINS': 'insert-class `N.` via the `O` entry: buffer matches Vim, cursor lands N-1 lines higher than Vim (design §4.14 #3)',
}

XFAIL = {
    # ① w/e/b whole-line approximation (buffer/register visible)
    'vl-ed': '①', 'vl-ey': '①',
    # ② action cursor at the active end (cursor only, or cursor-caused)
    'vl-jy': '②', 'vl-wy': '②', 'vl-Gy': '②', 'vl-hy': '②', 'vl-ly': '②',
    'vl-0y': '②', 'vl-^y': '②', 'vl-$y': '②', 'vl-y': '②', 'vl-c': '②',
    'vl-s': '②', 'vl-3j-y': '②', 'vl-jj-y': '②', 'vl-jk-y': '②',
    'vl-G-y': '②', 'vl-99j-y': '②', 'vl-k-k-d': '②',
    'vl-j-k-y': '②', 'vl-G-d': '②', 'vl-G-y2': '②', 'vl-j-2': '②',
    'vl-l-y': '②', 'vl-0-y': '②', 'vl-dollar-y': '②', 'vl-y-x': '②',
    'pp-VyP': '②',
    # ④ abs state reverse motion
    'vl-G-k-y': '④', 'vl-G-k-d': '④', 'vl-G-k-k-k-y': '④', 'vl-G-k-k-k-d': '④',
    # ⑤ linewise paste positioning
    'vl-j-y-p': '⑤', 'vl-y-p': '⑤', 'pp-Vyp': '⑤',
    # ⑦ G/gg drop the count
    'mot-3G': '⑦', 'mot-3gg': '⑦',
    # ⑧ visual p does not update the clipboard
    'vl-p': '⑧', 'vl-j-p': '⑧', 'vis-p': '⑧', 'vis-llp': '⑧', 'v-dollar-p': '⑧',
    'pp-vllp': '⑧',
    # ⑨ V then v keeps the whole-line selection
    'vl-v-y': '⑨', 'vl-v-y0': '⑨', 'vl-v-d': '⑨', 'vl-v-x': '⑨',
    # ---- migrated from known_failures.txt: each cites a written declaration ----
    # D18: EMPTY-insert exit (non-empty inserts are fixed by D26, §4.15)
    'ins-i-x': 'D18',
    'ins-i-p': 'D18',
    'ins-a-x': 'D18',
    'ins-a-p': 'D18',
    'ins-A-x': 'D18',
    'ins-A-p': 'D18',
    'ins-3i-x': 'D18',
    # 2026-10 (D26 audit): re-cited -- these six are NOT the insert-exit off-by-one.
    # `o<Esc>`/`O<Esc>` alone match Vim; the failure comes from the FOLLOWING command
    # acting on the empty line / its line break.
    'ins-o-x': 'XEMPTY',
    'ins-O-x': 'XEMPTY',
    'ins-o-last-x': 'XEMPTY',
    'ins-O-first-x': 'XEMPTY',
    'ins-o-p': 'PEMPTY',
    'ins-O-p': 'PEMPTY',
    # 2026-10 (D27/P2-2): counted `.` where the REPLAYED COMMAND itself is already a declared
    # deviation -- the count-replacement rule is right, the underlying command is not.
    'dot2X-3dot': 'D16',      # `2X` at column 0: empty host selection cuts the whole line
    'dotd0-3dot': 'D16',      # `d0` at column 0
    'dot2d0-3dot': 'D16',
    'dotins-OXY-2dot': 'DOTINS',  # O-entry counted insert: buffer matches, cursor N-1 lines off
    'mot-e': 'E-W',
    'mot-E': 'E-W',
    'probe-ex': 'E-W',
    'probe-Ex': 'E-W',
    'de': 'E-W',
    'dE': 'E-W',
    'ce': 'E-W',
    'cE': 'E-W',
    'yE': 'E-W',
    'ye': 'E-W',
    'vis-ed': 'E-W',
    'vis-ey': 'E-W',
    'vis-Ed': 'E-W',
    'vis-Ey': 'E-W',
    '2cw': 'E-W',
    '2yw': 'E-W',
    'yw-b': 'E-W',
    'cw-b': 'E-W',
    'cb-b': 'E-W',
    '3dw': 'E-W',
    'eol-dollar-x': 'D14',
    'eol-dollar-a': 'D14',
    'eol-dollar-p': 'D14',
    'eol-G-x': 'D14',
    'eol-G-p': 'D14',
    'eol-G-a': 'D14',
    'mot-G': 'D14',
    'mot-j-last': 'D14',
    'mot-99j': 'D14',
    'mot-12j': 'D14',
    'probe-$x': 'D14',
    'probe-Gx': 'D14',
    'dGx': 'D14',
    'dG-b': 'D14',
    'yG': 'D14',
    'yG-b': 'D14',
    'up-dG': 'D14',
    'up-yG': 'D14',
    'dd-last': 'D14',
    '2dd-last': 'D14',
    'key-J-last': 'D15',
    'eol-J-last': 'D15',
    'pp-J-leadspace': 'D15',
    'key-Y': 'YCOL',
    'key-3Y': 'YCOL',
    'y$': 'YCOL',
    # The matrix's `yj` case sits on the LAST line: Vim's `j` fails there, so the
    # operator ABORTS and the register stays unchanged; the model yanks L4 instead.
    # Measured yj@L1/L2/L3 = ok, yj@L4 = reg+cur diff (regM 'L4\n' vs regV '').
    'yj': 'FAILMOT',
    'yk': 'FAILMOT',   # reg diff: k fails on line 1 => Vim aborts, register unchanged
    '>G': 'IND',
    '>G-x': 'IND',
    '<G': 'IND',
    '>w': 'IND',
    '<w': 'IND',
    '>e': 'IND',
    '<e': 'IND',
    'gg>G': 'IND',
    'up->G': 'IND',
    'up-<G': 'IND',
    'ddp': 'PASTEC',
    'ddP': 'PASTEC',
    'yyp': 'PASTEC',
    'yyP': 'PASTEC',
    'Yp': 'PASTEC',
    'ddp2': 'PASTEC',
    '2ddp': 'PASTEC',
    'ddpp': 'PASTEC',
    'rep-ddp': 'PASTEC',
    # Same root cause as last-Gddx: `G` -> Ctrl+End lands after the final newline (D14),
    # so `dd` acts on the wrong line -- not the paste cursor.
    'last-Gddp': 'D14',
    # Same root cause as last-Gddx: `G` -> Ctrl+End lands after the final newline (D14),
    # so `dd` acts on the wrong line -- not the paste cursor.
    'last-GddP': 'D14',
    'last-jddp': 'PASTEC',
    'last-jddP': 'PASTEC',
    'Dp': 'EOLDEL',
    'pp-Dp': 'EOLDEL',
    'xp2': 'EOLDEL',
    'gx': 'GPFX',
    'gG': 'GPFX',
    'Zx': 'GPFX',
    'ZZ': 'GPFX',
    '2dj': 'FAILMOT',
    'v-gg-d': 'D16',
    'v-gg-y': 'D16',
    'v-G-d': 'D16',
    'v-G-y': 'D16',
    'vis-ggd': 'D16',
    'vis-ggy': 'D16',
    'vis-Gd': 'D16',
    'vis-Gy': 'D16',
    'vis-$y': 'VCUR',
    'vis-Vy': 'VCUR',
    'vis-jy': 'VCUR',
    'v-dollar-y': 'VCUR',
    'v-j-y': 'VCUR',
    'v-h0-p': 'VPASTE',
    'vl-s-x': 'D23',
    'cW': 'E-W',
    'yW': 'E-W',
    'cw': 'E-W',
    'yw': 'E-W',
    'dG': 'D14',
    'cj': 'FAILMOT',
    'ck': 'FAILMOT',
    'dj': 'FAILMOT',
    'dk': 'FAILMOT',
    # ---- migrated from known_failures.txt (round 2) ----
    '3dd': 'PASTEC',
    'dw-b': 'D2',
    'last-Gddx': 'D14',
    'pp-Yp': 'PASTEC',
    'pp-dd-then-P': 'PASTEC',
    'pp-yG-x': 'YCOL',
    'pp-yeP': 'E-W',
    'pp-yep': 'E-W',
    'pp-yhP': 'D16',
    'pp-yhp': 'D16',
    'v-e0-d': 'E-W',
    'v-w0-y': 'VCUR',
    'vis-99ld': 'VCAP',
    'vis-Wy': 'VCUR',
    'vis-j-y-p': 'VCUR',
    'vis-wy': 'VCUR',
    'vl-Gd': '②',
    'yep': 'E-W',
}


KNOWN_FAIL_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              'known_failures.txt')


def load_known_failures():
    """Ratchet baseline: mismatches that are known but not yet cited as a
    declared deviation.  A NEW mismatch is a hard failure; a baseline entry
    that starts matching real Vim is also a hard failure (so the list can
    only shrink)."""
    if not os.path.exists(KNOWN_FAIL_FILE):
        return {}
    out = {}
    for line in open(KNOWN_FAIL_FILE, encoding='utf-8'):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        out[parts[0]] = set(parts[1].split('+')) if len(parts) > 1 else set()
    return out


def fail_dims(r):
    dims = []
    if not r['ok_buf']:
        dims.append('buf')
    if r['ok_reg'] is False:
        dims.append('reg')
    if r['ok_cur'] is False:
        dims.append('cur')
    return dims


def _print_fail(r):
    c = r['c']
    dims = fail_dims(r)
    print('  %-22s buf=%-26s keys=%-12r [%s]' % (
        c.name, ih.esc_out(c.buf), c.vim, '+'.join(dims)))


def assign_ids(cs):
    seen = {}
    for c in cs:
        seen[c.name] = seen.get(c.name, 0) + 1
        if seen[c.name] > 1:
            c.cid = '%s#%d' % (c.name, seen[c.name])


def main(argv):
    all_cases = '--all' in argv
    assign_ids(cases)
    rows = [ih.run_case(c) for c in cases]

    # NB: a few case names appear twice with different buffers, so a name can be
    # both XFAIL (one instance mismatches) and a passing instance.  Only treat an
    # XFAIL entry as an XPASS when NO instance with that name still fails.
    names_failing = {r['c'].name for r in rows if ih.case_failed(r)}
    fails, xfails, xpasses = [], [], []
    for r in rows:
        if not ih.case_failed(r):
            if r['c'].name in XFAIL and r['c'].name not in names_failing:
                xpasses.append(r)
            continue
        dev = XFAIL.get(r['c'].name)
        if dev is None:
            fails.append(r)
        else:
            xfails.append((r, dev))

    known = load_known_failures()
    # A case is "known" only if it is listed AND it fails on no NEW dimension —
    # otherwise a worse failure inside an already-broken case would be masked.
    def _is_known(r):
        exp = known.get(r['c'].name)
        return exp is not None and set(fail_dims(r)) <= exp
    unknown = [r for r in fails if not _is_known(r)]
    known_hit = [r for r in fails if _is_known(r)]
    # NB: case names are not unique (a few cases appear twice with different
    # buffers), so a baseline name is only "fixed" when NO case with that name
    # still fails — otherwise one passing duplicate would wrongly force a prune.
    failing_names = {r['c'].name for r in fails}
    fixed = sorted(n for n in known if n not in failing_names)

    for r in unknown:
        ih.fmt_case(r, label='FAIL')
    if all_cases:
        for r, dev in xfails:
            ih.fmt_case(r, label='XFAIL[%s]' % dev)

    print('=' * 96)
    if unknown:
        print('NEW FAILURES (not in known_failures.txt — REGRESSION): %d' % len(unknown))
        for r in unknown:
            _print_fail(r)
    else:
        print('NEW FAILURES (not in known_failures.txt — REGRESSION): 0')
    print('KNOWN FAILURES (ratchet baseline, still open): %d' % len(known_hit))
    if all_cases:
        for r in known_hit:
            _print_fail(r)
    if fixed:
        print('FIXED (in known_failures.txt but now matching real Vim — PRUNE them): %d' % len(fixed))
        print('  ' + ', '.join(sorted(fixed)))

    print()
    print('XFAIL (declared deviations): %d' % len(xfails))
    by_dev = Counter(dev for _, dev in xfails)
    for dev in sorted(by_dev):
        names = [r['c'].name for r, d in xfails if d == dev]
        print('  %s x%-3d %s' % (dev, by_dev[dev], ', '.join(names)))
    if xpasses:
        print('XPASS (in the xfail table but now matching real Vim — remove them): %d' % len(xpasses))
        print('  ' + ', '.join(r['c'].name for r in xpasses))

    npass = len(rows) - len(fails) - len(xfails)
    print()
    print('TOTAL %d  PASS %d  XFAIL %d  KNOWN-FAIL %d  NEW-FAIL %d' % (
        len(rows), npass, len(xfails), len(known_hit), len(unknown)))
    print('(dimension diffs: buf %d, reg %d, cur %d)' % (
        sum(1 for r in rows if not r['ok_buf']),
        sum(1 for r in rows if r['ok_reg'] is False),
        sum(1 for r in rows if r['ok_cur'] is False)))
    # Strict xfail semantics: a case that is listed as a declared deviation but
    # now MATCHES real Vim is a failure too — otherwise the table silently rots
    # and a fixed defect would be masked as "still broken".
    return 1 if (unknown or xpasses or fixed) else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
