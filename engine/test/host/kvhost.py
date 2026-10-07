#!/usr/bin/env python3
"""Repo-resident host-editor model + real-Vim runner for the QMK vim engine.

This is the ported, self-contained version of the independent audit harness
(`/tmp/indep/ih.py`).  It compares the engine's host-visible key stream against
real Vim 9.1 (`/usr/bin/vim.tiny`) by simulating a plain-text host editor.

HOST MODEL (the contract the engine is written against; ported verbatim from
the independent audit's `ih.py`):
  * buffer = list of chars, cursor = absolute offset in [0,len].
  * selection = half-open [min(anchor,cur), max(anchor,cur)); anchor is the
    end that does NOT move.  cursor sits AFTER the last selected char.
  * Home/End/arrows with no Shift: cancel the selection, then move.
  * Shift+Home/End: anchor the current end, move to line/doc start/end.
  * Shift+Up/Down: anchor, move vertically keeping the column (clamped).
  * Shift+Left/Right: anchor, move one char (clamped).
  * Ctrl+Right/Left = word forward/back (vim's w / b).
  * Ctrl+Home/End = doc start/end; Ctrl+Shift+Home/End = select to doc ends.
  * Ctrl+C copy (selection retained; no selection -> copy current line text).
  * Ctrl+X cut  (selection deleted, selection cancelled).
  * Ctrl+V paste (replaces selection, else inserts at cursor).
  * Esc cancels the selection (cursor unchanged).
  * Delete/Backspace delete the selection, else one char, then cancel sel.
  * Space inserts ' ' (replacing selection).
  * Tab: with selection -> indent every line touched by it; else insert '\t'.
    Shift+Tab -> outdent.
  * Shift+Enter inserts '\n'.
  * any other printable key inserts its char (for passthrough/insert tests).
  * undo stack: one snapshot per host edit.  NB: `dd` is now a SINGLE host edit
    (`Home,Home,Shift+End,Shift+Right,Ctrl+X`, see command.c), so one `u` fully
    restores it -- verified against vim.tiny (`ddu`/`3ddu` match).

The vim ground truth uses the exact known-correct recipe:
    /usr/bin/vim.tiny -Nu NONE -N -es -c 'set nofixendofline' \
        -c "silent! normal! <KEYS>" -c 'w! <OUT>' -c 'qall!' <IN>
with a REAL \\x1b escape byte (never the two-character token `\\e`) and
`silent!` before every `normal!` (a ringing key otherwise aborts the rest).

One vim process is used per case: the case's buffer result, cursor and unnamed
register are all extracted in a single invocation, and the temp dir is created
once and reused for every case.  (Batching several cases into one vim process
is not possible with vim.tiny: it is built `-eval`, so the unnamed register
cannot be reset between cases and `p`-without-yank cases would leak state.)
"""
import atexit
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PROBE = os.path.join(HERE, 'probe')
WORK = tempfile.mkdtemp(prefix='kvhost-')
atexit.register(shutil.rmtree, WORK, ignore_errors=True)
VIM = '/usr/bin/vim.tiny'

# ---- keycodes (mirror kv_kc.h) ----
KV = {
    'a': 0x04, 'b': 0x05, 'c': 0x06, 'd': 0x07, 'e': 0x08, 'f': 0x09,
    'g': 0x0A, 'h': 0x0B, 'i': 0x0C, 'j': 0x0D, 'k': 0x0E, 'l': 0x0F,
    'm': 0x10, 'n': 0x11, 'o': 0x12, 'p': 0x13, 'q': 0x14, 'r': 0x15,
    's': 0x16, 't': 0x17, 'u': 0x18, 'v': 0x19, 'w': 0x1A, 'x': 0x1B,
    'y': 0x1C, 'z': 0x1D,
    '1': 0x1E, '2': 0x1F, '3': 0x20, '4': 0x21, '5': 0x22, '6': 0x23,
    '7': 0x24, '8': 0x25, '9': 0x26, '0': 0x27,
    ' ': 0x2C, '.': 0x37, '^': 0x223, '$': 0x221,
}
ESC = 0x29
ENT = 0x28
BSPC = 0x2A
TAB = 0x2B
DEL = 0x4C
HOME = 0x4A
END = 0x4D
RGHT = 0x4F
LEFT = 0x50
DOWN = 0x51
UP = 0x52
LSFT = 0x200
LCTL = 0x100
LCTL_LSFT = 0x300


def kc_of(ch):
    """single vim key char -> kv keycode (uppercase = shifted)."""
    if ch == '>':
        return 0x37 | LSFT          # Shift+.
    if ch == '<':
        return 0x36 | LSFT          # Shift+,
    if ch.isupper():
        return KV[ch.lower()] | LSFT
    if ch in KV:
        return KV[ch]
    raise ValueError('unknown key %r' % ch)


def parse_keys(s):
    """vim key string -> keycode list.  `\\e` is a real ESC (0x29), `\\r` is Enter (0x28)."""
    out = []
    i = 0
    while i < len(s):
        if s[i] == '\\' and i + 1 < len(s) and s[i + 1] == 'e':
            out.append(ESC)
            i += 2
            continue
        if s[i] == '\\' and i + 1 < len(s) and s[i + 1] == 'r':
            # `\r` = Enter (needed by the multi-line insert cases `iA<CR>B`).  The vim
            # side carries a REAL CR byte, so `:normal!` sees it as <CR> (a CR in the
            # middle of the script line is NOT a line terminator; see vim_run()).
            out.append(ENT)
            i += 2
            continue
        out.append(kc_of(s[i]))
        i += 1
    return out


# ---- engine probe ----
def engine(keys):
    """Feed keycodes to the freshly-built probe -> (host stream, meta)."""
    if not os.path.exists(PROBE):
        raise SystemExit('probe not built: %s missing\n'
                         'run `make matrix-test` from engine/ (or build it with '
                         'cc -std=c11 -O2 -Iinclude -Isrc -o test/host/probe '
                         'test/host/probe.c src/*.c)' % PROBE)
    args = [PROBE, 'KM', 'NORMAL'] + ['%04X' % k for k in keys]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError('probe failed: %s' % r.stderr)
    host = None
    meta = {}
    for line in r.stdout.splitlines():
        if line.startswith('HOST'):
            host = [int(x, 16) for x in line.split()[1:]]
        elif line.startswith('META'):
            meta = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', line)}
    if host is None:
        raise RuntimeError('probe produced no HOST line: %r' % r.stdout)
    return host, meta


# ---- host editor model ----
class Host:
    def __init__(self, buf, line, col, word_model='vimw'):
        self.b = list(buf)
        self.word_model = word_model
        o = 0
        l = 0
        while l < line and o < len(self.b):
            if self.b[o] == '\n':
                l += 1
            o += 1
        o += col
        self.cur = min(o, len(self.b))
        self.anchor = None
        self.clip = ''
        self.undo = []
        self.log = []
        self.warn = []

    # -- basics
    def s(self):
        return ''.join(self.b)

    def __len__(self):
        return len(self.b)

    def ls(self, o):
        o = min(o, len(self.b))
        i = o
        while i > 0 and self.b[i - 1] != '\n':
            i -= 1
        return i

    def le(self, o):
        i = min(o, len(self.b))
        while i < len(self.b) and self.b[i] != '\n':
            i += 1
        return i

    def line_of(self, o):
        return self.s()[:o].count('\n')

    def col_of(self, o):
        return o - self.ls(o)

    def has_sel(self):
        return self.anchor is not None and self.anchor != self.cur

    def bounds(self):
        a, c = self.anchor, self.cur
        return (a, c) if a < c else (c, a)

    def sel_text(self):
        if not self.has_sel():
            return ''
        s, e = self.bounds()
        return self.s()[s:e]

    def anchor_here(self):
        if self.anchor is None:
            self.anchor = self.cur

    def cancel(self):
        self.anchor = None

    def snap(self):
        self.undo.append((list(self.b), self.cur, self.anchor, self.clip))

    def del_range(self, s, e):
        if e <= s:
            return
        self.snap()
        del self.b[s:e]
        if self.cur > e:
            self.cur -= (e - s)
        elif self.cur > s:
            self.cur = s
        if self.anchor is not None:
            if self.anchor > e:
                self.anchor -= (e - s)
            elif self.anchor > s:
                self.anchor = s

    def del_sel(self):
        s, e = self.bounds()
        self.del_range(s, e)
        self.anchor = None

    def insert(self, t):
        if self.has_sel():
            s, e = self.bounds()
            self.del_range(s, e)
        elif self.anchor is not None:
            # empty selection -> just place cursor
            self.anchor = None
        self.snap()
        self.b[self.cur:self.cur] = list(t)
        self.cur += len(t)

    def move_v(self, down, extend):
        if extend:
            self.anchor_here()
        ls, le = self.ls(self.cur), self.le(self.cur)
        col = self.cur - ls
        if down:
            if le >= len(self.b):
                return
            nls = le + 1
            nle = self.le(nls)
            self.cur = nls + min(col, nle - nls)
        else:
            if ls == 0:
                return
            ple = ls - 1
            pls = self.ls(ple)
            self.cur = pls + min(col, ple - pls)

    def iskw(self, c):
        return c.isalnum() or c == '_'

    def word_fwd(self, i):
        """vim's w: skip current word/punct run, then whitespace."""
        n = len(self.b)
        b = self.b
        if i >= n:
            return n
        if not b[i].isspace():
            if self.iskw(b[i]):
                while i < n and self.iskw(b[i]):
                    i += 1
            else:
                while i < n and not self.iskw(b[i]) and not b[i].isspace():
                    i += 1
        while i < n and b[i].isspace():
            i += 1
        return i

    def word_fwd_big(self, i):
        n = len(self.b)
        b = self.b
        if i >= n:
            return n
        while i < n and not b[i].isspace():
            i += 1
        while i < n and b[i].isspace():
            i += 1
        return i

    def word_back(self, i):
        b = self.b
        if i <= 0:
            return 0
        i -= 1
        while i > 0 and b[i].isspace():
            i -= 1
        if self.iskw(b[i]):
            while i > 0 and self.iskw(b[i - 1]):
                i -= 1
        elif not b[i].isspace():
            while i > 0 and not self.iskw(b[i - 1]) and not b[i - 1].isspace():
                i -= 1
        return i

    def word_back_big(self, i):
        b = self.b
        if i <= 0:
            return 0
        i -= 1
        while i > 0 and b[i].isspace():
            i -= 1
        while i > 0 and not b[i - 1].isspace():
            i -= 1
        return i

    def word_end(self, i):
        """vim's e."""
        n = len(self.b)
        b = self.b
        if i >= n:
            return n
        i += 1
        while i < n and b[i].isspace():
            i += 1
        if i >= n:
            return n - 1 if n else 0
        if self.iskw(b[i]):
            while i + 1 < n and self.iskw(b[i + 1]):
                i += 1
        else:
            while i + 1 < n and not self.iskw(b[i + 1]) and not b[i + 1].isspace():
                i += 1
        return i

    def _insert_at(self, o, t):
        self.b[o:o] = list(t)
        if self.cur >= o:
            self.cur += len(t)
        if self.anchor is not None and self.anchor >= o:
            self.anchor += len(t)

    def _delete_at(self, o, k):
        if k <= 0:
            return
        del self.b[o:o + k]
        for attr in ('cur', 'anchor'):
            p = getattr(self, attr)
            if p is None:
                continue
            if p > o + k:
                setattr(self, attr, p - k)
            elif p > o:
                setattr(self, attr, o)

    def indent(self, out):
        if self.has_sel():
            s, e = self.bounds()
            l0 = self.line_of(s)
            l1 = self.line_of(e - 1)
        else:
            l0 = l1 = self.line_of(self.cur)
        self.snap()
        for ln in range(l1, l0 - 1, -1):
            o = 0
            for _ in range(ln):
                o = self.b.index('\n', o) + 1
            if out:
                if o < len(self.b) and self.b[o] == '\t':
                    self._delete_at(o, 1)
                else:
                    k = 0
                    while k < 4 and o + k < len(self.b) and self.b[o + k] == ' ':
                        k += 1
                    self._delete_at(o, k)
            else:
                self._insert_at(o, '\t')
        if self.cur > len(self.b):
            self.cur = len(self.b)
        if self.anchor is not None and self.anchor > len(self.b):
            self.anchor = len(self.b)
        if not self.has_sel():
            self.anchor = None

    # -- key application
    def apply(self, kc):
        basic = kc & 0xFF
        mods = kc & 0x1F00
        shift = bool(mods & LSFT)
        ctrl = bool(mods & LCTL)
        self.log.append(kc)
        if basic == ESC:
            self.cancel()
            return
        if basic == ENT and shift:
            self.insert('\n')
            return
        if basic == ENT:
            self.insert('\n')
            return
        if basic in (HOME, END):
            doc = ctrl
            start = (basic == HOME)
            if shift:
                self.anchor_here()
                if doc:
                    self.cur = 0 if start else len(self.b)
                else:
                    self.cur = self.ls(self.cur) if start else self.le(self.cur)
            else:
                self.cancel()
                if doc:
                    self.cur = 0 if start else len(self.b)
                else:
                    self.cur = self.ls(self.cur) if start else self.le(self.cur)
            return
        if basic in (LEFT, RGHT):
            fwd = (basic == RGHT)
            if ctrl:
                if shift:
                    self.anchor_here()
                else:
                    self.cancel()
                if self.word_model == 'vimbig':
                    self.cur = self.word_fwd_big(self.cur) if fwd else self.word_back_big(self.cur)
                else:
                    self.cur = self.word_fwd(self.cur) if fwd else self.word_back(self.cur)
            else:
                if shift:
                    self.anchor_here()
                else:
                    self.cancel()
                self.cur = min(self.cur + 1, len(self.b)) if fwd else max(self.cur - 1, 0)
            return
        if basic in (UP, DOWN):
            self.move_v(basic == DOWN, shift)
            return
        if basic == 0x06 and ctrl:      # Ctrl+C
            if self.has_sel():
                self.clip = self.sel_text()
            else:
                self.clip = self.s()[self.ls(self.cur):self.le(self.cur)]
                self.warn.append('Ctrl+C with no selection (copied line)')
            return
        if basic == 0x1B and ctrl:      # Ctrl+X
            if self.has_sel():
                s, e = self.bounds()
                self.clip = self.sel_text()
                self.del_range(s, e)
                self.anchor = None
            else:
                s, e = self.ls(self.cur), self.le(self.cur)
                if e < len(self.b):
                    e += 1
                self.clip = self.s()[s:e]
                self.del_range(s, e)
                self.anchor = None
                self.warn.append('Ctrl+X with no selection (cut line)')
            return
        if basic == 0x19 and ctrl:      # Ctrl+V
            self.insert(self.clip)
            self.cancel()
            return
        if basic == 0x1D and ctrl:      # Ctrl+Z
            if self.undo:
                b, c, a, cl = self.undo.pop()
                self.b = list(b)
                self.cur = c
                self.anchor = a
                self.clip = cl
            return
        if basic == DEL:
            if self.has_sel():
                self.del_sel()
            elif self.cur < len(self.b):
                self.del_range(self.cur, self.cur + 1)
            self.cancel()
            return
        if basic == BSPC:
            if self.has_sel():
                self.del_sel()
            elif self.cur > 0:
                self.del_range(self.cur - 1, self.cur)
            self.cancel()
            return
        if basic == 0x2C:               # Space
            self.insert(' ')
            return
        if basic == TAB:
            self.indent(shift)
            return
        # generic text passthrough (lowercase ascii)
        if not ctrl and 0x04 <= basic <= 0x1D:
            ch = chr(ord('a') + basic - 0x04)
            self.insert(ch.upper() if shift else ch)
            return
        if not ctrl and 0x1E <= basic <= 0x26:
            self.insert(chr(ord('1') + basic - 0x1E))
            return
        self.warn.append('unmodelled key %04X' % kc)


def model(buf, line, col, keys, word_model='vimw'):
    h = Host(buf, line, col, word_model)
    for k in keys:
        h.apply(k)
    return h


# ---- real vim driver ----
def vim_run(buf, line, col, keys, esc_suffix=False):
    """One vim.tiny run per case -> (buf_after, reg_text, reg_type, cur_offset).

    The script (sourced with -S, because vim caps -c at 10):
      1. run the keys from (line,col)
      2. write the buffer result
      3. insert a '@' marker at the cursor, write it -> cursor offset
      4. open a scratch buffer, paste the unnamed register after an 'X',
         then append a 'QQQ' sentinel line and write it.
    Register framing:  p-data = "X\\n" + L + "QQQ\\n"  (linewise, L = reg text)
                       p-data = "X"  + C + "\\nQQQ\\n" (charwise, C = reg text)
    so the trailing newline added by 'fixendofline' never matters.
    """
    cf = os.path.join(WORK, 'case.txt')
    of = os.path.join(WORK, 'out.txt')
    cuf = os.path.join(WORK, 'cur.txt')
    rf = os.path.join(WORK, 'reg.txt')
    sf = os.path.join(WORK, 'sc.vim')
    with open(cf, 'wb') as f:
        f.write(buf.encode())
    for f in (of, cuf, rf):
        if os.path.exists(f):
            os.unlink(f)
    pos = 'gg%dG0' % (line + 1) + ('l' * col)
    # A REAL CR byte inside `k` is part of the `:normal!` argument (Vim splits the
    # script on NL, not on CR), which is how `\r`-encoded <CR> reaches Vim.  A
    # TRAILING CR would instead be eaten as a DOS line ending, so no case may end
    # with `\r` (all current `\r` cases have more keys after the Enter).
    k = keys + ('\x1b' if esc_suffix else '')
    lines = [
        'set nofixendofline',
        'silent! normal! ' + pos + k,
        'w! ' + of,
        'silent! normal! i@',
        'w! ' + cuf,
        'new',
        'silent! normal! iX',
        'silent! normal! p',
        'silent! normal! GoQQQ',
        'w! ' + rf,
        'qall!',
    ]
    with open(sf, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    subprocess.run([VIM, '-Nu', 'NONE', '-N', '-es', '-S', sf, cf],
                   capture_output=True)
    out = open(of, 'rb').read().decode('utf-8', 'replace') if os.path.exists(of) else None
    cur = None
    if os.path.exists(cuf):
        t = open(cuf, 'rb').read().decode('utf-8', 'replace')
        if t.count('@') == 1:
            cur = t.find('@')
    reg = rtype = None
    if os.path.exists(rf):
        d = open(rf, 'rb').read().decode('utf-8', 'replace')
        tail = '\nQQQ\n'
        if d == 'X\nQQQ\n':
            rtype, reg = 'char', ''          # empty register: p pasted nothing
        elif d.startswith('X\n') and d.endswith(tail):
            rtype = 'line'
            L = d[2:-len(tail)]              # register lines, no final newline
            reg = L + '\n' if L else '\n'
        elif d.startswith('X') and d.endswith(tail):
            rtype, reg = 'char', d[1:-len(tail)]
        else:
            rtype, reg = '?', d
    return out, reg, rtype, cur


# ---- case driver ----
class Case:
    def __init__(self, name, buf, line, col, vim, eng=None, esc=False,
                 mode='NORMAL', note='', check_cursor=True, word_model='vimw'):
        self.name = name
        self.buf = buf
        self.line = line
        self.col = col
        self.vim = vim
        # the vim side needs REAL bytes, not the two-char '\e' / '\r' tokens
        self.vim_keys = vim.replace('\\e', '\x1b').replace('\\r', '\r')
        self.eng = parse_keys(eng if eng is not None else vim)
        self.esc = esc
        self.mode = mode
        self.note = note
        self.check_cursor = check_cursor
        self.word_model = word_model
        self.cid = name


def esc_out(s):
    if s is None:
        return 'None'
    return s.replace('\n', '\\n').replace('\t', '\\t')


def cur_off(buf, off):
    if off is None:
        return None
    pre = buf[:off]
    return (pre.count('\n'), off - (pre.rfind('\n') + 1), off)


def run_case(c):
    host, meta = engine(c.eng)
    h = model(c.buf, c.line, c.col, host, c.word_model)
    if c.esc:
        h.apply(ESC)
    vb, vreg, vtype, vcur = vim_run(c.buf, c.line, c.col, c.vim_keys, c.esc)
    mcur = (h.line_of(h.cur), h.col_of(h.cur), h.cur)
    vcur3 = cur_off(vb, vcur) if (vcur is not None and vb is not None) else None
    ok_buf = (h.s() == vb)
    # register: compare text only (host clipboard has no linewise flag)
    if vreg is None:
        ok_reg = None
    else:
        ok_reg = (h.clip == vreg)
    # cursor: same line, |col diff| <= 1 (host sits after the char)
    if vcur3 is None:
        ok_cur = None
    else:
        ok_cur = (mcur[0] == vcur3[0] and abs(mcur[1] - vcur3[1]) <= 1)
    return dict(c=c, host=host, meta=meta, h=h, mbuf=h.s(), mclip=h.clip,
                mcur=mcur, vbuf=vb, vreg=vreg, vtype=vtype, vcur=vcur3,
                ok_buf=ok_buf, ok_reg=ok_reg, ok_cur=ok_cur)


def case_failed(r):
    """A case fails when any compared dimension mismatches."""
    return (not r['ok_buf']) or (r['ok_reg'] is False) or (r['ok_cur'] is False)


def fmt_case(r, out=sys.stdout, label='FAIL'):
    c = r['c']
    print('=' * 96, file=out)
    print('%s case=%-20s buf=%-30s line=%d col=%d keys=%-14r %s' % (
        label, c.name, esc_out(c.buf), c.line, c.col, c.vim, c.note), file=out)
    print('  HOST  %s' % ' '.join('%04X' % k for k in r['host']), file=out)
    print('  MODEL buf=%-30s clip=%-16s cur=%s' % (
        esc_out(r['mbuf']), esc_out(r['mclip']), r['mcur']), file=out)
    print('  VIM   buf=%-30s reg=%-16s cur=%s type=%s' % (
        esc_out(r['vbuf']), esc_out(r['vreg']), r['vcur'], r['vtype']), file=out)
    print('  %s buf:%s reg:%s cur:%s' % (
        '***' if case_failed(r) else 'OK ',
        'MATCH' if r['ok_buf'] else 'DIFF',
        'MATCH' if r['ok_reg'] else ('DIFF' if r['ok_reg'] is False else 'n/a'),
        'n/a' if r['ok_cur'] is None else ('MATCH' if r['ok_cur'] else 'DIFF')), file=out)
    if r['h'].warn:
        print('  WARN  %s' % r['h'].warn, file=out)


def summarize(rows):
    nb = [r['c'].name for r in rows if not r['ok_buf']]
    nr = [r['c'].name for r in rows if r['ok_reg'] is False]
    nc = [r['c'].name for r in rows if r['ok_cur'] is False]
    print('TOTAL %d  buf-diff %d  reg-diff %d  cur-diff %d' % (
        len(rows), len(nb), len(nr), len(nc)))
