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
the case is listed in `XFAIL` below with one of the declared deviation IDs from
`vim/design.md` §4.9 (①–⑩); otherwise it is a FAIL and the process exits 1.

Exit status: 0 iff every case either matches or is a declared deviation.
"""
import sys
from collections import Counter

import kvhost as ih
from kvhost import Case

B3 = 'one two three\nfour five six\nseven eight\n'
B4 = 'L1\nL2\nL3\nL4\n'
B12 = ''.join('L%02d\n' % i for i in range(1, 13))
BW = 'alpha beta gamma\ndelta epsilon zeta\n'
BMIX = 'aaa bbb\ncc dddd e\nf\nggg hhh iii\n'

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

# ============================ paste/register cases (audit pp.py) ============================
for k in ['ylp', 'ylP', 'yhp', 'yhP', 'yep', 'yeP', 'ywp', 'ywP', 'yl l p', 'yllp', 'yllP']:
    add('pp-' + k.replace(' ', ''), B3, 0, 0, k)
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
}


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

    fails, xfails, xpasses = [], [], []
    for r in rows:
        if not ih.case_failed(r):
            if r['c'].name in XFAIL:
                xpasses.append(r)
            continue
        dev = XFAIL.get(r['c'].name)
        if dev is None:
            fails.append(r)
        else:
            xfails.append((r, dev))

    for r in fails:
        ih.fmt_case(r, label='FAIL')
    if all_cases:
        for r, dev in xfails:
            ih.fmt_case(r, label='XFAIL[%s]' % dev)

    print('=' * 96)
    if fails:
        print('FAILING CASES (undeclared mismatch): %d' % len(fails))
        for r in fails:
            c = r['c']
            dims = []
            if not r['ok_buf']:
                dims.append('buf')
            if r['ok_reg'] is False:
                dims.append('reg')
            if r['ok_cur'] is False:
                dims.append('cur')
            print('  %-22s buf=%-26s keys=%-12r [%s]' % (
                c.name, ih.esc_out(c.buf), c.vim, '+'.join(dims)))
    else:
        print('FAILING CASES (undeclared mismatch): 0')

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
    print('TOTAL %d  PASS %d  XFAIL %d  FAIL %d' % (len(rows), npass, len(xfails), len(fails)))
    print('(dimension diffs: buf %d, reg %d, cur %d)' % (
        sum(1 for r in rows if not r['ok_buf']),
        sum(1 for r in rows if r['ok_reg'] is False),
        sum(1 for r in rows if r['ok_cur'] is False)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
