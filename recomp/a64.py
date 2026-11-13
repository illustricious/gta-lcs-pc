"""ARM64 instruction -> C statement translation.

Input is Capstone's (mnemonic, op_str); output is C that operates on the CPU
struct declared in rt/recomp.h.  Anything not recognised raises Unsupported
so the caller can emit a runtime trap and report the shape.
"""
import re
import struct


class Unsupported(Exception):
    pass


CONDS = {'eq', 'ne', 'hs', 'cs', 'lo', 'cc', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le', 'al'}
COND_ALIAS = {'cs': 'hs', 'cc': 'lo'}
EXTS = {'uxtb', 'uxth', 'uxtw', 'uxtx', 'sxtb', 'sxth', 'sxtw', 'sxtx'}
SHIFTS = {'lsl', 'lsr', 'asr', 'ror', 'msl'}
ARR = {'16b': (16, 'b'), '8b': (8, 'b'), '8h': (8, 'h'), '4h': (4, 'h'), '4s': (4, 's'), '2s': (2, 's'),
       '2d': (2, 'd'), '1d': (1, 'd')}
EBITS = {'b': 8, 'h': 16, 's': 32, 'd': 64}
EBYTES = {'b': 1, 'h': 2, 's': 4, 'd': 8, 'q': 16}
SIGNED = {'b': 'sb', 'h': 'sh', 's': 'ss', 'd': 'sd'}
FLOATF = {'s': 'f', 'd': 'df'}
HALF = {'h': 'b', 's': 'h', 'd': 's'}
UTYPE = {'b': 'uint8_t', 'h': 'uint16_t', 's': 'uint32_t', 'd': 'uint64_t'}
STYPE = {'b': 'int8_t', 'h': 'int16_t', 's': 'int32_t', 'd': 'int64_t'}
MEMU = {1: 'M8', 2: 'M16', 4: 'M32', 8: 'M64'}
MEMS = {1: 'MS8', 2: 'MS16', 4: 'MS32', 8: 'MS64'}


class Reg:
    __slots__ = ('kind', 'n', 'zr', 'sp', 'arr', 'elem', 'idx')

    def __init__(self, kind, n, zr=False, sp=False, arr=None, elem=None, idx=None):
        self.kind, self.n, self.zr, self.sp, self.arr, self.elem, self.idx = kind, n, zr, sp, arr, elem, idx

    @property
    def gpr(self):
        return self.kind in ('x', 'w')

    @property
    def bits(self):
        return 64 if self.kind == 'x' else 32


class Imm:
    __slots__ = ('v', 'is_float')

    def __init__(self, v, is_float=False):
        self.v, self.is_float = v, is_float


class Shift:
    __slots__ = ('kind', 'amount')

    def __init__(self, kind, amount):
        self.kind, self.amount = kind, amount


class Ext:
    __slots__ = ('kind', 'amount')

    def __init__(self, kind, amount):
        self.kind, self.amount = kind, amount


class Mem:
    __slots__ = ('base', 'off', 'index', 'mod', 'pre')

    def __init__(self, base, off, index, mod, pre):
        self.base, self.off, self.index, self.mod, self.pre = base, off, index, mod, pre


class VList:
    __slots__ = ('regs', 'idx')

    def __init__(self, regs, idx):
        self.regs, self.idx = regs, idx


class Cond:
    __slots__ = ('name',)

    def __init__(self, name):
        self.name = COND_ALIAS.get(name, name)


class Other:
    __slots__ = ('text',)

    def __init__(self, text):
        self.text = text


_GPR = re.compile(r'^([xw])(\d+)$')
_FPR = re.compile(r'^([bhsdq])(\d+)$')
_VEC = re.compile(r'^v(\d+)(?:\.(\d*[bhsd]))?(?:\[(\w+)\])?$')


def parse_imm(text):
    t = text.lstrip('#')
    try:
        return Imm(int(t, 0))
    except ValueError:
        return Imm(float(t), True)


def parse_reg(t):
    if t in ('sp', 'wsp'):
        return Reg('x' if t == 'sp' else 'w', 31, sp=True)
    if t in ('xzr', 'wzr'):
        return Reg(t[0], 31, zr=True)
    m = _GPR.match(t)
    if m:
        return Reg(m.group(1), int(m.group(2)))
    m = _FPR.match(t)
    if m:
        return Reg(m.group(1), int(m.group(2)))
    m = _VEC.match(t)
    if m:
        n, spec, idx = int(m.group(1)), m.group(2), m.group(3)
        idx = int(idx, 0) if idx is not None else None
        if spec and spec[0].isdigit():
            return Reg('v', n, arr=spec, idx=idx)
        return Reg('v', n, elem=spec, idx=idx)
    return None


def split_top(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch in '[{':
            depth += 1
        elif ch in ']}':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def parse_modifier(t):
    parts = t.split()
    if parts and parts[0] in SHIFTS:
        return Shift(parts[0], int(parts[1].lstrip('#'), 0))
    if parts and parts[0] in EXTS:
        return Ext(parts[0], int(parts[1].lstrip('#'), 0) if len(parts) > 1 else None)
    return None


def parse_op(t):
    if t.startswith('['):
        pre = t.endswith('!')
        inner = split_top(t[1:t.index(']')])
        base = parse_reg(inner[0])
        off, index, mod = 0, None, None
        if len(inner) > 1:
            if inner[1].startswith('#'):
                off = parse_imm(inner[1]).v
            else:
                index = parse_reg(inner[1])
                if len(inner) > 2:
                    mod = parse_modifier(inner[2])
        return Mem(base, off, index, mod, pre)
    if t.startswith('{'):
        end = t.index('}')
        regs = [parse_reg(r.strip()) for r in t[1:end].split(',')]
        rest = t[end + 1:].strip()
        idx = int(rest.strip('[]'), 0) if rest else None
        return VList(regs, idx)
    if t.startswith('#'):
        return parse_imm(t)
    mod = parse_modifier(t)
    if mod:
        return mod
    r = parse_reg(t)
    if r:
        return r
    if t in CONDS:
        return Cond(t)
    return Other(t)


def parse_ops(op_str):
    return [parse_op(t) for t in split_top(op_str)]


def hx(v, bits=64):
    v &= (1 << bits) - 1
    return ('0x%xull' % v) if bits == 64 else ('0x%xu' % v)


def float_bits32(v):
    return struct.unpack('<I', struct.pack('<f', v))[0]


def float_bits64(v):
    return struct.unpack('<Q', struct.pack('<d', v))[0]


# ---------------------------------------------------------------- GPR access

def rd(r, bits=None):
    """Read a general register at its own width (or forced width)."""
    bits = bits or r.bits
    if r.zr:
        return '0ull' if bits == 64 else '0u'
    core = 'X(%d)' % r.n
    return core if bits == 64 else '(uint32_t)%s' % core


def wr(r, expr):
    if r.zr:
        return '(void)(%s);' % expr
    if r.kind == 'x':
        return 'X(%d) = (uint64_t)(%s);' % (r.n, expr)
    return 'X(%d) = (uint32_t)(%s);' % (r.n, expr)


def utype(bits):
    return 'uint64_t' if bits == 64 else 'uint32_t'


def stype(bits):
    return 'int64_t' if bits == 64 else 'int32_t'


def shifted(expr, sh, bits):
    if sh is None or (sh.amount == 0 and sh.kind != 'ror'):
        return expr
    n = sh.amount
    if sh.kind == 'lsl':
        return '((%s)(%s) << %d)' % (utype(bits), expr, n)
    if sh.kind == 'lsr':
        return '((%s)(%s) >> %d)' % (utype(bits), expr, n)
    if sh.kind == 'asr':
        return '((%s)((%s)(%s) >> %d))' % (utype(bits), stype(bits), expr, n)
    if sh.kind == 'ror':
        return 'ror%d(%s, %d)' % (bits, expr, n)
    raise Unsupported('shift ' + sh.kind)


def extended(r, ext, bits):
    src = {'b': 'int8_t', 'h': 'int16_t', 'w': 'int32_t', 'x': 'int64_t'}[ext.kind[3]]
    if ext.kind[0] == 'u':
        src = 'u' + src
    low = '(%s)X(%d)' % (src, r.n) if not r.zr else '0'
    e = '(%s)(%s)(%s)' % (utype(bits), stype(bits) if ext.kind[0] == 's' else utype(bits), low)
    if ext.amount:
        e = '(%s << %d)' % (e, ext.amount)
    return '(%s)' % e


def operand2(ops, i, bits):
    """Second source operand at ops[i] with an optional shift/extend after it."""
    o = ops[i]
    mod = ops[i + 1] if len(ops) > i + 1 and isinstance(ops[i + 1], (Shift, Ext)) else None
    if isinstance(o, Imm):
        v = o.v
        if isinstance(mod, Shift):
            v <<= mod.amount
        return hx(v, bits)
    if isinstance(mod, Ext):
        return extended(o, mod, bits)
    return shifted(rd(o, bits), mod, bits)


def cond_expr(c):
    return 'C_' + c.name.upper()


INVERT = {'eq': 'ne', 'ne': 'eq', 'hs': 'lo', 'lo': 'hs', 'mi': 'pl', 'pl': 'mi', 'vs': 'vc', 'vc': 'vs',
          'hi': 'ls', 'ls': 'hi', 'ge': 'lt', 'lt': 'ge', 'gt': 'le', 'le': 'gt'}


# ---------------------------------------------------------------- memory

def mem_addr(m, post=None):
    """Return (address expression, writeback statement or '')."""
    base = 'X(%d)' % m.base.n
    if m.index is not None:
        if isinstance(m.mod, Ext):
            idx = extended(m.index, m.mod, 64)
        else:
            idx = shifted(rd(m.index, 64), m.mod, 64)
        return '%s + %s' % (base, idx), ''
    off = m.off
    if post is not None:
        if isinstance(post, Imm):
            return base, '%s += %s;' % (base, hx(post.v))
        return base, '%s += %s;' % (base, rd(post, 64))
    addr = base if off == 0 else '%s + %s' % (base, hx(off))
    if m.pre:
        return addr, '%s = a_;' % base
    return addr, ''


def fp_get(r):
    """Raw-bit read expression of a scalar FP/SIMD register."""
    return 'V(%d).%s[0]' % (r.n, r.kind) if r.kind != 'q' else 'V(%d)' % r.n


def fp_set_raw(r, expr):
    if r.kind == 'q':
        return 'SET_Q(%d, %s);' % (r.n, expr)
    return 'SET_%sU(%d, %s);' % (r.kind.upper(), r.n, expr)


def load_stmt(dst, m, post, size, signed):
    addr, wb = mem_addr(m, post)
    if dst.gpr:
        macro = (MEMS if signed else MEMU)[size]
        if signed:
            val = '(%s)%s(a_)' % (stype(dst.bits), macro)
        else:
            val = '%s(a_)' % macro
        return '{ uint64_t a_ = %s; uint64_t t_ = (%s)(%s); %s %s }' % (addr, utype(dst.bits), val, wb, wr(dst, 't_'))
    if dst.kind == 'q':
        return '{ uint64_t a_ = %s; V128 t_ = M128(a_); %s SET_Q(%d, t_); }' % (addr, wb, dst.n)
    return '{ uint64_t a_ = %s; uint64_t t_ = %s(a_); %s %s }' % (addr, MEMU[size], wb, fp_set_raw(dst, 't_'))


def store_stmt(src, m, post, size):
    addr, wb = mem_addr(m, post)
    if src.gpr:
        val = rd(src, 64)
        return '{ uint64_t a_ = %s; %s(a_) = (%s)%s; %s }' % (addr, MEMU[size], UTYPE['bhsd'[size.bit_length() - 1]], val, wb)
    if src.kind == 'q':
        return '{ uint64_t a_ = %s; M128(a_) = V(%d); %s }' % (addr, src.n, wb)
    return '{ uint64_t a_ = %s; %s(a_) = %s; %s }' % (addr, MEMU[size], fp_get(src), wb)


def reg_size(r):
    if r.gpr:
        return r.bits // 8
    return EBYTES[r.kind]


# ---------------------------------------------------------------- vectors

def vinfo(r):
    """(count, element letter) for a vector arrangement or scalar FP register."""
    if r.kind == 'v':
        if r.arr:
            return ARR[r.arr]
        return 1, r.elem
    if r.kind in 'bhsd':
        return 1, r.kind
    raise Unsupported('vector operand')


def vloop(d, cnt, body, srcs=(), keep=False):
    """Element loop writing r into V(d).  srcs: list of (name, regnum)."""
    decl = ''.join('V128 %s = V(%d); ' % (nm, n) for nm, n in srcs)
    init = 'V128 r = V(%d); ' % d if keep else 'V128 r = {0}; '
    return '{ %s%sfor (int i = 0; i < %d; i++) { %s } V(%d) = r; }' % (decl, init, cnt, body, d)


def elem_src(o, name, field):
    """Element expression for an operand that is either a full vector or one lane."""
    if o.kind == 'v' and o.idx is not None:
        return '%s.%s[%d]' % (name, field, o.idx)
    if o.kind == 'v' and o.arr:
        return '%s.%s[i]' % (name, field)
    return '%s.%s[0]' % (name, field)


INT3 = {
    'add': '{a} + {b}', 'sub': '{a} - {b}', 'mul': '{a} * {b}',
    'and': '{a} & {b}', 'orr': '{a} | {b}', 'eor': '{a} ^ {b}', 'bic': '{a} & ~{b}', 'orn': '{a} | ~{b}',
    'cmeq': '{a} == {b} ? -1 : 0', 'cmhi': '{a} > {b} ? -1 : 0', 'cmhs': '{a} >= {b} ? -1 : 0',
    'cmtst': '({a} & {b}) ? -1 : 0',
    'umax': '{a} > {b} ? {a} : {b}', 'umin': '{a} < {b} ? {a} : {b}',
    'uabd': '{a} > {b} ? {a} - {b} : {b} - {a}',
    'uhadd': '(uint64_t)(((unsigned __int128){a} + {b}) >> 1)',
    'urhadd': '(uint64_t)(((unsigned __int128){a} + {b} + 1) >> 1)',
    'uqsub': '{a} > {b} ? {a} - {b} : 0',
    'uqadd': '(({ut})({a} + {b}) < {a}) ? ({ut})-1 : {a} + {b}',
}
SINT3 = {
    'cmgt': '{a} > {b} ? -1 : 0', 'cmge': '{a} >= {b} ? -1 : 0',
    'smax': '{a} > {b} ? {a} : {b}', 'smin': '{a} < {b} ? {a} : {b}',
    'sabd': '{a} > {b} ? {a} - {b} : {b} - {a}',
}
FP3 = {
    'fadd': '{a} + {b}', 'fsub': '{a} - {b}', 'fmul': '{a} * {b}', 'fdiv': '{a} / {b}',
    'fnmul': '-({a} * {b})',
    'fmax': 'fmax{w}({a}, {b})', 'fmin': 'fmin{w}({a}, {b})',
    'fmaxnm': 'fmaxnm{w}({a}, {b})', 'fminnm': 'fminnm{w}({a}, {b})',
    'fabd': '{abs}({a} - {b})',
}
FPCMP = {'fcmeq': '==', 'fcmgt': '>', 'fcmge': '>=', 'fcmlt': '<', 'fcmle': '<='}
ZCMP = {'cmeq': '==', 'cmgt': '>', 'cmge': '>=', 'cmlt': '<', 'cmle': '<='}
FP1 = {
    'fabs': '{abs}({a})', 'fneg': '-{a}', 'fsqrt': '{sqrt}({a})',
    'frintm': '{floor}({a})', 'frintp': '{ceil}({a})', 'frintz': '{trunc}({a})', 'frinta': '{round}({a})',
    'frintn': '{rint}({a})', 'frintx': '{rint}({a})', 'frinti': '{rint}({a})',
}


def fnames(e):
    if e == 's':
        return dict(w='32', abs='fabsf', sqrt='sqrtf', floor='floorf', ceil='ceilf', trunc='truncf',
                    round='roundf', rint='nearbyintf', fma='fmaf', ft='float')
    return dict(w='64', abs='fabs', sqrt='sqrt', floor='floor', ceil='ceil', trunc='trunc',
                round='round', rint='nearbyint', fma='fma', ft='double')


def fpv(r):
    """Scalar float value expression of an s/d register."""
    return 'V(%d).%s[0]' % (r.n, FLOATF[r.kind])


def fp_set(r, expr):
    return 'SET_%s(%d, %s);' % (r.kind.upper(), r.n, expr)


class Translator:
    """ctx must provide jump(target)->stmt, call(target)->stmt, addr_const(va)->expr."""

    def __init__(self, ctx):
        self.ctx = ctx

    def translate(self, addr, mn, op_str):
        self.addr = addr
        ops = parse_ops(op_str) if op_str else []
        if mn.startswith('b.'):
            return 'if (%s) %s' % (cond_expr(Cond(mn[2:])), self.ctx.jump(ops[0].v))
        fn = getattr(self, 'i_' + mn.replace('.', '_'), None)
        if fn is None and mn.startswith(('ldadd', 'ldset', 'ldclr', 'ldeor', 'swp')):
            fn = self._ldop
        if fn is None and mn.startswith('cas'):
            fn = self._cas
        if fn is None:
            raise Unsupported(mn)
        out = fn(mn, ops)
        if out is None:
            raise Unsupported('%s %s' % (mn, op_str))
        return out

    # ---- control flow
    def i_b(self, mn, o):
        return self.ctx.jump(o[0].v)

    def i_bl(self, mn, o):
        return self.ctx.call(o[0].v)

    def i_blr(self, mn, o):
        return '{ uint64_t t_ = %s; X(30) = %s; rt_call(c, t_); }' % (rd(o[0]), self.ctx.addr_const(self.addr + 4))

    def i_br(self, mn, o):
        return self.ctx.indirect_jump(rd(o[0]))

    def i_ret(self, mn, o):
        return 'return;'

    def i_cbz(self, mn, o):
        return 'if (%s == 0) %s' % (rd(o[0]), self.ctx.jump(o[1].v))

    def i_cbnz(self, mn, o):
        return 'if (%s != 0) %s' % (rd(o[0]), self.ctx.jump(o[1].v))

    def i_tbz(self, mn, o):
        return 'if (!((%s >> %d) & 1)) %s' % (rd(o[0], 64), o[1].v, self.ctx.jump(o[2].v))

    def i_tbnz(self, mn, o):
        return 'if ((%s >> %d) & 1) %s' % (rd(o[0], 64), o[1].v, self.ctx.jump(o[2].v))

    def i_nop(self, mn, o):
        return ';'

    i_bti = i_paciasp = i_autiasp = i_autia1716 = i_autib1716 = i_pacibsp = i_autibsp = i_xpaclri = i_nop
    i_hint = i_prfm = i_prfum = i_yield = i_isb = i_dmb = i_dsb = i_nop

    def i_brk(self, mn, o):
        return 'rt_trap(c, %s, "brk");' % self.ctx.addr_const(self.addr)

    i_udf = i_brk

    def i_svc(self, mn, o):
        return 'rt_trap(c, %s, "svc");' % self.ctx.addr_const(self.addr)

    def i_mrs(self, mn, o):
        if o[1].text == 'tpidr_el0':
            return wr(o[0], 'c->tpidr')
        if o[1].text in ('fpcr', 'fpsr'):
            return wr(o[0], '0')
        if o[1].text == 'dczid_el0':
            return wr(o[0], '0x10')
        return None

    def i_msr(self, mn, o):
        if o[0].text in ('fpcr', 'fpsr'):
            return ';'
        if o[0].text == 'tpidr_el0':
            return 'c->tpidr = %s;' % rd(o[1])
        return None

    # ---- addresses and moves
    def i_adr(self, mn, o):
        return wr(o[0], self.ctx.addr_const(o[1].v))

    i_adrp = i_adr

    def i_mov(self, mn, o):
        d, s = o[0], o[1]
        if d.gpr:
            if isinstance(s, Imm):
                return wr(d, hx(s.v, d.bits))
            if s.gpr:
                return wr(d, rd(s, d.bits))
            if s.kind == 'v' and s.idx is not None:  # umov
                return wr(d, 'V(%d).%s[%d]' % (s.n, s.elem, s.idx))
            return None
        if d.kind == 'v' and d.arr:  # vector copy (orr alias)
            cnt, e = ARR[d.arr]
            if cnt * EBITS[e] == 128:
                return 'V(%d) = V(%d);' % (d.n, s.n)
            return '{ uint64_t t_ = V(%d).d[0]; V(%d).d[0] = t_; V(%d).d[1] = 0; }' % (s.n, d.n, d.n)
        if d.kind == 'v' and d.idx is not None:  # ins
            if s.gpr:
                return 'V(%d).%s[%d] = (%s)%s;' % (d.n, d.elem, d.idx, UTYPE[d.elem], rd(s, 64))
            return 'V(%d).%s[%d] = V(%d).%s[%d];' % (d.n, d.elem, d.idx, s.n, s.elem, s.idx)
        if d.kind in 'bhsd' and s.kind == 'v':  # scalar dup of a lane
            return fp_set_raw(d, 'V(%d).%s[%d]' % (s.n, s.elem, s.idx))
        return None

    def i_movz(self, mn, o):
        v = o[1].v << (o[2].amount if len(o) > 2 else 0)
        return wr(o[0], hx(v, o[0].bits))

    def i_movn(self, mn, o):
        v = ~(o[1].v << (o[2].amount if len(o) > 2 else 0))
        return wr(o[0], hx(v, o[0].bits))

    def i_movk(self, mn, o):
        sh = o[2].amount if len(o) > 2 else 0
        b = o[0].bits
        return wr(o[0], '(%s & %s) | %s' % (rd(o[0]), hx(~(0xffff << sh), b), hx(o[1].v << sh, b)))

    def i_umov(self, mn, o):
        return wr(o[0], 'V(%d).%s[%d]' % (o[1].n, o[1].elem, o[1].idx))

    def i_smov(self, mn, o):
        return wr(o[0], '(%s)V(%d).%s[%d]' % (stype(o[0].bits), o[1].n, SIGNED[o[1].elem], o[1].idx))

    def i_ins(self, mn, o):
        return self.i_mov(mn, o)

    # ---- integer arithmetic
    def _arith(self, mn, o):
        d, a = o[0], o[1]
        if not d.gpr:
            return self._vec3(mn, o)
        bits = d.bits
        b = operand2(o, 2, bits)
        base = mn.rstrip('s') if mn in ('adds', 'subs') else mn
        if mn in ('adds', 'subs'):
            return wr(d, '%s%d(c, %s, %s)' % (mn, bits, rd(a, bits), b))
        return wr(d, '%s %s %s' % (rd(a, bits), '+' if base == 'add' else '-', b))

    i_add = i_sub = i_adds = i_subs = _arith

    def i_cmp(self, mn, o):
        bits = o[0].bits
        return 'subs%d(c, %s, %s);' % (bits, rd(o[0]), operand2(o, 1, bits))

    def i_cmn(self, mn, o):
        bits = o[0].bits
        return 'adds%d(c, %s, %s);' % (bits, rd(o[0]), operand2(o, 1, bits))

    def i_neg(self, mn, o):
        if not o[0].gpr:
            return self._vec1(mn, o)
        bits = o[0].bits
        return wr(o[0], '0 - %s' % operand2(o, 1, bits))

    def i_negs(self, mn, o):
        bits = o[0].bits
        return wr(o[0], 'subs%d(c, 0, %s)' % (bits, operand2(o, 1, bits)))

    def i_adc(self, mn, o):
        bits = o[0].bits
        return wr(o[0], 'adcs%d(c, %s, %s, %d)' % (bits, rd(o[1]), rd(o[2]), mn.endswith('s')))

    i_adcs = i_adc

    def i_sbc(self, mn, o):
        bits = o[0].bits
        return wr(o[0], 'adcs%d(c, %s, ~%s, %d)' % (bits, rd(o[1]), rd(o[2]), mn.endswith('s')))

    i_sbcs = i_sbc

    def i_ngc(self, mn, o):
        bits = o[0].bits
        return wr(o[0], 'adcs%d(c, 0, ~%s, %d)' % (bits, rd(o[1]), mn.endswith('s')))

    i_ngcs = i_ngc

    def _logic(self, mn, o):
        d = o[0]
        if not d.gpr:
            if isinstance(o[1], Imm):
                return self._vec_imm_logic(mn, o)
            return self._vec3(mn, o)
        bits = d.bits
        op = {'and': '&', 'ands': '&', 'orr': '|', 'eor': '^', 'bic': '&', 'bics': '&', 'orn': '|', 'eon': '^'}[mn]
        b = operand2(o, 2, bits)
        if mn in ('bic', 'bics', 'orn', 'eon'):
            b = '~' + b
        e = '%s %s %s' % (rd(o[1], bits), op, b)
        if mn in ('ands', 'bics'):
            e = 'logs%d(c, %s)' % (bits, e)
        return wr(d, e)

    i_and = i_ands = i_orr = i_eor = i_bic = i_bics = i_orn = i_eon = _logic

    def i_tst(self, mn, o):
        bits = o[0].bits
        return 'logs%d(c, %s & %s);' % (bits, rd(o[0]), operand2(o, 1, bits))

    def i_mvn(self, mn, o):
        if not o[0].gpr:
            return self._vec1(mn, o)
        return wr(o[0], '~%s' % operand2(o, 1, o[0].bits))

    def _shift(self, mn, o):
        d = o[0]
        bits = d.bits
        a = rd(o[1], bits)
        if isinstance(o[2], Imm):
            return wr(d, shifted(a, Shift(mn, o[2].v), bits))
        n = '(%s & %d)' % (rd(o[2], bits), bits - 1)
        if mn == 'lsl':
            return wr(d, '%s << %s' % (a, n))
        if mn == 'lsr':
            return wr(d, '%s >> %s' % (a, n))
        if mn == 'asr':
            return wr(d, '(%s)%s >> %s' % (stype(bits), a, n))
        return wr(d, 'ror%d(%s, (unsigned)%s)' % (bits, a, n))

    i_lsl = i_lsr = i_asr = i_ror = _shift
    i_lslv = i_lsrv = i_asrv = i_rorv = lambda self, mn, o: self._shift(mn[:3], o)

    def i_mul(self, mn, o):
        if not o[0].gpr:
            return self._vec3(mn, o)
        return wr(o[0], '%s * %s' % (rd(o[1]), rd(o[2])))

    def i_mneg(self, mn, o):
        return wr(o[0], '0 - %s * %s' % (rd(o[1]), rd(o[2])))

    def i_madd(self, mn, o):
        return wr(o[0], '%s + %s * %s' % (rd(o[3]), rd(o[1]), rd(o[2])))

    def i_msub(self, mn, o):
        return wr(o[0], '%s - %s * %s' % (rd(o[3]), rd(o[1]), rd(o[2])))

    def _wide(self, r, signed):
        return '(uint64_t)(int64_t)(int32_t)X(%d)' % r.n if signed else '(uint64_t)(uint32_t)X(%d)' % r.n

    def _mull(self, mn, o):
        if not o[0].gpr:
            return self._vec_widen3(mn, o)
        s = mn[0] == 's'
        p = '%s * %s' % (self._wide(o[1], s), self._wide(o[2], s))
        if mn.endswith('maddl'):
            return wr(o[0], '%s + %s' % (rd(o[3]), p))
        if mn.endswith('msubl'):
            return wr(o[0], '%s - %s' % (rd(o[3]), p))
        if mn.endswith('mnegl'):
            return wr(o[0], '0 - %s' % p)
        return wr(o[0], p)

    i_smull = i_umull = i_smaddl = i_umaddl = i_smsubl = i_umsubl = i_smnegl = i_umnegl = _mull

    def i_smulh(self, mn, o):
        return wr(o[0], '%s64(%s, %s)' % (mn, rd(o[1]), rd(o[2])))

    i_umulh = i_smulh

    def i_sdiv(self, mn, o):
        return wr(o[0], '%s%d(%s, %s)' % (mn, o[0].bits, rd(o[1]), rd(o[2])))

    i_udiv = i_sdiv

    # ---- bitfields
    def i_ubfx(self, mn, o):
        b = o[0].bits
        return wr(o[0], '(%s >> %d) & %s' % (rd(o[1], b), o[2].v, hx((1 << o[3].v) - 1, b)))

    def i_sbfx(self, mn, o):
        b, lsb, w = o[0].bits, o[2].v, o[3].v
        return wr(o[0], '(%s)((%s)(%s << %d) >> %d)' % (utype(b), stype(b), rd(o[1], b), b - lsb - w, b - w))

    def i_ubfiz(self, mn, o):
        b = o[0].bits
        return wr(o[0], '(%s & %s) << %d' % (rd(o[1], b), hx((1 << o[3].v) - 1, b), o[2].v))

    def i_sbfiz(self, mn, o):
        b, lsb, w = o[0].bits, o[2].v, o[3].v
        return wr(o[0], '(%s)((%s)(%s << %d) >> %d)' % (utype(b), stype(b), rd(o[1], b), b - w, b - w - lsb))

    def i_bfi(self, mn, o):
        b, lsb, w = o[0].bits, o[2].v, o[3].v
        m = (1 << w) - 1
        return wr(o[0], '(%s & %s) | ((%s & %s) << %d)' % (rd(o[0]), hx(~(m << lsb), b), rd(o[1], b), hx(m, b), lsb))

    def i_bfxil(self, mn, o):
        b, lsb, w = o[0].bits, o[2].v, o[3].v
        m = (1 << w) - 1
        return wr(o[0], '(%s & %s) | ((%s >> %d) & %s)' % (rd(o[0]), hx(~m, b), rd(o[1], b), lsb, hx(m, b)))

    def i_bfc(self, mn, o):
        b, lsb, w = o[0].bits, o[1].v, o[2].v
        return wr(o[0], '%s & %s' % (rd(o[0]), hx(~(((1 << w) - 1) << lsb), b)))

    def _sxt(self, mn, o):
        t = {'sxtb': 'int8_t', 'sxth': 'int16_t', 'sxtw': 'int32_t', 'uxtb': 'uint8_t', 'uxth': 'uint16_t',
             'uxtw': 'uint32_t'}[mn]
        return wr(o[0], '(%s)(%s)X(%d)' % (stype(o[0].bits), t, o[1].n))

    i_sxtb = i_sxth = i_sxtw = i_uxtb = i_uxth = i_uxtw = _sxt

    def i_rev(self, mn, o):
        if not o[0].gpr:
            return None
        return wr(o[0], '__builtin_bswap%d(%s)' % (o[0].bits, rd(o[1])))

    def i_rev16(self, mn, o):
        if not o[0].gpr:
            return None
        return wr(o[0], 'rev16_%d(%s)' % (o[0].bits, rd(o[1])))

    def i_rev32(self, mn, o):
        if not o[0].gpr:
            return None
        return wr(o[0], 'rev32_64(%s)' % rd(o[1]))

    def i_rbit(self, mn, o):
        if not o[0].gpr:
            return None
        return wr(o[0], 'rbit%d(%s)' % (o[0].bits, rd(o[1])))

    def i_clz(self, mn, o):
        if not o[0].gpr:
            return None
        return wr(o[0], 'clz%d(%s)' % (o[0].bits, rd(o[1])))

    # ---- conditionals
    def i_csel(self, mn, o):
        b = o[0].bits
        f = {'csel': '%s', 'csinc': '%s + 1', 'csinv': '~%s', 'csneg': '0 - %s'}[mn]
        return wr(o[0], '%s ? %s : %s' % (cond_expr(o[3]), rd(o[1], b), f % rd(o[2], b)))

    i_csinc = i_csinv = i_csneg = i_csel

    def i_cset(self, mn, o):
        return wr(o[0], '%s ? 1 : 0' % cond_expr(o[1]))

    def i_csetm(self, mn, o):
        return wr(o[0], '%s ? %s : 0' % (cond_expr(o[1]), hx(-1, o[0].bits)))

    def i_cinc(self, mn, o):
        b = o[0].bits
        f = {'cinc': '%s + 1', 'cinv': '~%s', 'cneg': '0 - %s'}[mn]
        return wr(o[0], '%s ? %s : %s' % (cond_expr(o[2]), f % rd(o[1], b), rd(o[1], b)))

    i_cinv = i_cneg = i_cinc

    def i_ccmp(self, mn, o):
        b = o[0].bits
        rhs = hx(o[1].v, b) if isinstance(o[1], Imm) else rd(o[1], b)
        fn = 'subs' if mn == 'ccmp' else 'adds'
        return 'if (%s) %s%d(c, %s, %s); else set_nzcv(c, %d);' % (cond_expr(o[3]), fn, b, rd(o[0]), rhs, o[2].v)

    i_ccmn = i_ccmp

    # ---- loads and stores
    def _post(self, o, i):
        return o[i] if len(o) > i else None

    def _ld(self, mn, o):
        d, m = o[0], o[1]
        if isinstance(m, Imm):  # literal
            m = Mem(None, 0, None, None, False)
            addr = self.ctx.addr_const(o[1].v)
            size = reg_size(d)
            if d.gpr:
                signed = mn == 'ldrsw'
                macro = (MEMS if signed else MEMU)[4 if signed else size]
                return wr(d, '%s%s(%s)' % ('(int64_t)' if signed else '', macro, addr))
            if d.kind == 'q':
                return 'SET_Q(%d, M128(%s));' % (d.n, addr)
            return fp_set_raw(d, '%s(%s)' % (MEMU[size], addr))
        base = mn.replace('u', '', 1) if mn.startswith('ldu') else mn
        signed = base in ('ldrsb', 'ldrsh', 'ldrsw')
        size = {'ldr': reg_size(d), 'ldrb': 1, 'ldrh': 2, 'ldrsb': 1, 'ldrsh': 2, 'ldrsw': 4}[base]
        return load_stmt(d, m, self._post(o, 2), size, signed)

    i_ldr = i_ldrb = i_ldrh = i_ldrsb = i_ldrsh = i_ldrsw = _ld
    i_ldur = i_ldurb = i_ldurh = i_ldursb = i_ldursh = i_ldursw = _ld

    def _st(self, mn, o):
        s, m = o[0], o[1]
        base = mn.replace('u', '', 1) if mn.startswith('stu') else mn
        size = {'str': reg_size(s), 'strb': 1, 'strh': 2}[base]
        return store_stmt(s, m, self._post(o, 2), size)

    i_str = i_strb = i_strh = i_stur = i_sturb = i_sturh = _st

    def i_ldp(self, mn, o):
        r1, r2, m = o[0], o[1], o[2]
        addr, wb = mem_addr(m, self._post(o, 3))
        size = 4 if mn == 'ldpsw' else reg_size(r1)
        if r1.gpr:
            macro = (MEMS if mn == 'ldpsw' else MEMU)[size]
            cast = '(int64_t)' if mn == 'ldpsw' else ''
            return '{ uint64_t a_ = %s; uint64_t t1_ = %s%s(a_), t2_ = %s%s(a_ + %d); %s %s %s }' % (
                addr, cast, macro, cast, macro, size, wb, wr(r1, 't1_'), wr(r2, 't2_'))
        if r1.kind == 'q':
            return '{ uint64_t a_ = %s; V128 t1_ = M128(a_), t2_ = M128(a_ + 16); %s V(%d) = t1_; V(%d) = t2_; }' % (
                addr, wb, r1.n, r2.n)
        return '{ uint64_t a_ = %s; uint64_t t1_ = %s(a_), t2_ = %s(a_ + %d); %s %s %s }' % (
            addr, MEMU[size], MEMU[size], size, wb, fp_set_raw(r1, 't1_'), fp_set_raw(r2, 't2_'))

    i_ldpsw = i_ldp

    def i_stp(self, mn, o):
        r1, r2, m = o[0], o[1], o[2]
        addr, wb = mem_addr(m, self._post(o, 3))
        size = reg_size(r1)
        if r1.gpr:
            return '{ uint64_t a_ = %s; uint64_t t1_ = %s, t2_ = %s; %s(a_) = (%s)t1_; %s(a_ + %d) = (%s)t2_; %s }' % (
                addr, rd(r1, 64), rd(r2, 64), MEMU[size], utype(size * 8), MEMU[size], size, utype(size * 8), wb)
        if r1.kind == 'q':
            return '{ uint64_t a_ = %s; M128(a_) = V(%d); M128(a_ + 16) = V(%d); %s }' % (addr, r1.n, r2.n, wb)
        return '{ uint64_t a_ = %s; %s(a_) = %s; %s(a_ + %d) = %s; %s }' % (
            addr, MEMU[size], fp_get(r1), MEMU[size], size, fp_get(r2), wb)

    # ---- atomics (x86 is TSO; exclusives are emulated with compare-and-swap)
    def _atomic_size(self, mn, r):
        if mn.endswith('b'):
            return 1
        if mn.endswith('h'):
            return 2
        return r.bits // 8

    def _ldx(self, mn, o):
        size = self._atomic_size(mn, o[0])
        excl = 'x' in mn
        s = '{ uint64_t a_ = X(%d); uint64_t t_ = %s(a_); ' % (o[1].base.n, MEMU[size])
        if excl:
            s += 'c->excl_addr = a_; c->excl_val = t_; '
        return s + wr(o[0], 't_') + ' }'

    i_ldxr = i_ldaxr = i_ldxrb = i_ldaxrb = i_ldxrh = i_ldaxrh = i_ldar = i_ldarb = i_ldarh = _ldx

    def _stx(self, mn, o):
        size = self._atomic_size(mn, o[1])
        t = UTYPE['bhsd'[size.bit_length() - 1]]
        return ('{ uint64_t a_ = X(%d); uint32_t ok_ = c->excl_addr == a_ && '
                '__sync_bool_compare_and_swap((%s *)a_, (%s)c->excl_val, (%s)%s); c->excl_addr = 0; %s }') % (
            o[2].base.n, t, t, t, rd(o[1], 64), wr(o[0], '!ok_'))

    i_stxr = i_stlxr = i_stxrb = i_stlxrb = i_stxrh = i_stlxrh = _stx

    def _stl(self, mn, o):
        size = self._atomic_size(mn, o[0])
        t = UTYPE['bhsd'[size.bit_length() - 1]]
        return '__atomic_store_n((%s *)X(%d), (%s)%s, __ATOMIC_SEQ_CST);' % (t, o[1].base.n, t, rd(o[0], 64))

    i_stlr = i_stlrb = i_stlrh = _stl

    def _cas(self, mn, o):
        size = 1 if mn.endswith('b') else 2 if mn.endswith('h') else o[0].bits // 8
        t = UTYPE['bhsd'[size.bit_length() - 1]]
        return wr(o[0], '__sync_val_compare_and_swap((%s *)X(%d), (%s)%s, (%s)%s)' % (
            t, o[2].base.n, t, rd(o[0], 64), t, rd(o[1], 64)))

    i_cas = i_casa = i_casl = i_casal = _cas

    def _ldop(self, mn, o):
        for name, fn in (('ldadd', 'add'), ('ldset', 'or'), ('ldclr', 'and'), ('ldeor', 'xor'), ('swp', 'swap')):
            if mn.startswith(name):
                break
        else:
            return None
        rest = mn[len(name):]
        t = 'uint8_t' if rest.endswith('b') else 'uint16_t' if rest.endswith('h') else utype(o[0].bits)
        val = '(%s)%s' % (t, rd(o[0], 64))
        if fn == 'swap':
            e = '__atomic_exchange_n((%s *)X(%d), %s, __ATOMIC_SEQ_CST)' % (t, o[2].base.n, val)
        else:
            if name == 'ldclr':
                val = '~' + val
            e = '__sync_fetch_and_%s((%s *)X(%d), %s)' % (fn, t, o[2].base.n, val)
        return wr(o[1], e)

    i_ldaddal = i_ldadd = i_ldadda = i_ldaddl = _ldop
    i_ldsetal = i_ldset = i_ldclral = i_ldclr = i_ldeoral = i_swpal = i_swp = _ldop

    # ---- scalar floating point
    def _fp3(self, mn, o):
        d = o[0]
        if d.kind == 'v' or (len(o) > 2 and isinstance(o[2], Reg) and o[2].kind == 'v'):
            return self._vfp3(mn, o)
        fn = fnames(d.kind)
        return fp_set(d, FP3[mn].format(a=fpv(o[1]), b=fpv(o[2]), **fn))

    i_fadd = i_fsub = i_fmul = i_fdiv = i_fnmul = i_fmax = i_fmin = i_fmaxnm = i_fminnm = i_fabd = _fp3

    def _fp1(self, mn, o):
        d = o[0]
        if d.kind == 'v':
            return self._vfp1(mn, o)
        return fp_set(d, FP1[mn].format(a=fpv(o[1]), **fnames(d.kind)))

    i_fabs = i_fneg = i_fsqrt = i_frintm = i_frintp = i_frintz = i_frinta = i_frintn = i_frintx = i_frinti = _fp1

    def _fma(self, mn, o):
        d, n, m, a = o
        fma = fnames(d.kind)['fma']
        n_, m_, a_ = fpv(n), fpv(m), fpv(a)
        e = {'fmadd': '%s(%s, %s, %s)' % (fma, n_, m_, a_),
             'fmsub': '%s(-%s, %s, %s)' % (fma, n_, m_, a_),
             'fnmadd': '%s(-%s, %s, -%s)' % (fma, n_, m_, a_),
             'fnmsub': '%s(%s, %s, -%s)' % (fma, n_, m_, a_)}[mn]
        return fp_set(d, e)

    i_fmadd = i_fmsub = i_fnmadd = i_fnmsub = _fma

    def i_fcmp(self, mn, o):
        b = '0.0' if isinstance(o[1], Imm) else fpv(o[1])
        return 'fcmp64(c, (double)%s, (double)%s);' % (fpv(o[0]), b)

    i_fcmpe = i_fcmp

    def i_fccmp(self, mn, o):
        return 'if (%s) fcmp64(c, (double)%s, (double)%s); else set_nzcv(c, %d);' % (
            cond_expr(o[3]), fpv(o[0]), fpv(o[1]), o[2].v)

    i_fccmpe = i_fccmp

    def i_fcsel(self, mn, o):
        return fp_set(o[0], '%s ? %s : %s' % (cond_expr(o[3]), fpv(o[1]), fpv(o[2])))

    def i_fcvt(self, mn, o):
        return fp_set(o[0], '(%s)%s' % ('float' if o[0].kind == 's' else 'double', fpv(o[1])))

    def i_fmov(self, mn, o):
        d, s = o[0], o[1]
        if d.gpr:
            if s.kind == 'v':
                return wr(d, 'V(%d).d[%d]' % (s.n, s.idx))
            return wr(d, fp_get(s))
        if d.kind == 'v':
            if d.arr and isinstance(s, Imm):
                cnt, e = ARR[d.arr]
                bits = float_bits32(s.v) if e == 's' else float_bits64(s.v)
                return vloop(d.n, cnt, 'r.%s[i] = %s;' % (e, hx(bits, EBITS[e])))
            if d.idx is not None and s.gpr:
                return 'V(%d).d[%d] = %s;' % (d.n, d.idx, rd(s, 64))
            return None
        if isinstance(s, Imm):
            bits = float_bits32(float(s.v)) if d.kind == 's' else float_bits64(float(s.v))
            return fp_set_raw(d, hx(bits, 64))
        if s.gpr:
            return fp_set_raw(d, rd(s, 64) if not s.zr else '0')
        return fp_set_raw(d, fp_get(s))

    def _itof(self, mn, o):
        d, s = o[0], o[1]
        signed = mn[0] == 's'
        if d.kind == 'v':
            cnt, e = ARR[d.arr]
            src = 'a.%s[i]' % (SIGNED[e] if signed else e)
            return vloop(d.n, cnt, 'r.%s[i] = (%s)%s;' % (FLOATF[e], fnames(e)['ft'], src), [('a', s.n)])
        ft = 'float' if d.kind == 's' else 'double'
        if s.gpr:
            src = '(%s)%s' % (stype(s.bits) if signed else utype(s.bits), rd(s))
        else:
            src = 'V(%d).%s[0]' % (s.n, SIGNED[s.kind] if signed else s.kind)
        if len(o) > 2:
            return fp_set(d, '(%s)((double)%s / %r)' % (ft, src, float(1 << o[2].v)))
        return fp_set(d, '(%s)%s' % (ft, src))

    i_scvtf = i_ucvtf = _itof

    def _ftoi(self, mn, o):
        d, s = o[0], o[1]
        mode = mn[4]
        signed = mn[5] == 's'
        rnd = {'z': '%s', 'm': 'floor(%s)', 'p': 'ceil(%s)', 'a': 'round(%s)', 'n': 'rint_even(%s)'}[mode]
        if d.kind == 'v' or not d.gpr:
            cnt, e = vinfo(d)
            fn = 'cvt_%s%d' % ('s' if signed else 'u', EBITS[e])
            body = 'r.%s[i] = %s(%s);' % (e, fn, rnd % ('(double)a.%s[i]' % FLOATF[e]))
            return vloop(d.n, cnt, body, [('a', s.n)])
        src = '(double)%s' % fpv(s)
        if len(o) > 2:
            src = '(%s * %r)' % (src, float(1 << o[2].v))
        return wr(d, 'cvt_%s%d(%s)' % ('s' if signed else 'u', d.bits, rnd % src))

    i_fcvtzs = i_fcvtzu = i_fcvtms = i_fcvtmu = i_fcvtps = i_fcvtpu = i_fcvtas = i_fcvtau = _ftoi
    i_fcvtns = i_fcvtnu = _ftoi

    # ---- SIMD
    def _vec3(self, mn, o):
        d, a, b = o[0], o[1], o[2]
        cnt, e = vinfo(d)
        if mn in INT3:
            body = 'r.%s[i] = (%s)(%s);' % (e, UTYPE[e], INT3[mn].format(a='a.%s[i]' % e, b='b.%s[i]' % e, ut=UTYPE[e]))
        elif mn in SINT3:
            se = SIGNED[e]
            body = 'r.%s[i] = (%s)(%s);' % (e, UTYPE[e], SINT3[mn].format(a='a.%s[i]' % se, b='b.%s[i]' % se))
        else:
            return None
        return vloop(d.n, cnt, body, [('a', a.n), ('b', b.n)])

    i_cmhi = i_cmhs = i_cmtst = i_umax = i_umin = i_uabd = i_uhadd = i_urhadd = i_uqsub = i_uqadd = _vec3
    i_smax = i_smin = i_sabd = _vec3

    def _vcmp(self, mn, o):
        d, a, b = o
        cnt, e = vinfo(d)
        if isinstance(b, Imm):
            body = 'r.%s[i] = a.%s[i] %s 0 ? -1 : 0;' % (e, SIGNED[e], ZCMP[mn])
            return vloop(d.n, cnt, body, [('a', a.n)])
        return self._vec3(mn, o)

    i_cmeq = i_cmgt = i_cmge = i_cmlt = i_cmle = _vcmp

    def _vshl_reg(self, mn, o):
        d, a, b = o
        cnt, e = vinfo(d)
        if mn == 'sshl':
            body = 'r.%s[i] = (%s)sshl_e(a.%s[i], b.sb[i * %d], %d);' % (e, UTYPE[e], SIGNED[e], EBYTES[e], EBITS[e])
        else:
            body = 'r.%s[i] = (%s)ushl_e(a.%s[i], b.sb[i * %d], %d);' % (e, UTYPE[e], e, EBYTES[e], EBITS[e])
        return vloop(d.n, cnt, body, [('a', a.n), ('b', b.n)])

    i_sshl = i_ushl = _vshl_reg

    def _vmla(self, mn, o):
        d, a, b = o
        cnt, e = vinfo(d)
        op = '+' if mn == 'mla' else '-'
        body = 'r.%s[i] = (%s)(q.%s[i] %s a.%s[i] * b.%s[i]);' % (e, UTYPE[e], e, op, e, e)
        return vloop(d.n, cnt, body, [('q', d.n), ('a', a.n), ('b', b.n)])

    i_mla = i_mls = _vmla

    def _vsel(self, mn, o):
        d, a, b = o
        cnt, e = vinfo(d)
        n = cnt * EBYTES[e] // 8
        f = {'bsl': '(q.d[i] & a.d[i]) | (~q.d[i] & b.d[i])',
             'bit': '(q.d[i] & ~b.d[i]) | (a.d[i] & b.d[i])',
             'bif': '(q.d[i] & b.d[i]) | (a.d[i] & ~b.d[i])'}[mn]
        return vloop(d.n, n, 'r.d[i] = %s;' % f, [('q', d.n), ('a', a.n), ('b', b.n)])

    i_bsl = i_bit = i_bif = _vsel

    def _vec1(self, mn, o):
        d, a = o[0], o[1]
        cnt, e = vinfo(d)
        se = SIGNED[e]
        f = {'neg': '0 - a.%s[i]' % e, 'mvn': '~a.%s[i]' % e, 'not': '~a.%s[i]' % e,
             'abs': 'a.%s[i] < 0 ? 0 - a.%s[i] : a.%s[i]' % (se, e, e),
             'cnt': '__builtin_popcount(a.%s[i])' % e,
             'clz': '__builtin_clz((uint32_t)a.%s[i] << %d | %d)' % (e, 32 - EBITS[e], (1 << (32 - EBITS[e])) >> 1)
             }[mn]
        return vloop(d.n, cnt, 'r.%s[i] = (%s)(%s);' % (e, UTYPE[e], f), [('a', a.n)])

    i_abs = i_cnt = i_not = _vec1

    def _vec_imm_logic(self, mn, o):
        d = o[0]
        cnt, e = vinfo(d)
        v = o[1].v << (o[2].amount if len(o) > 2 else 0)
        op = {'bic': '& ~', 'orr': '| '}[mn]
        return vloop(d.n, cnt, 'r.%s[i] = q.%s[i] %s%s;' % (e, e, op, hx(v, 64)), [('q', d.n)])

    def i_movi(self, mn, o):
        d = o[0]
        cnt, e = vinfo(d)
        v = o[1].v
        if len(o) > 2:
            if o[2].kind == 'msl':
                v = (v << o[2].amount) | ((1 << o[2].amount) - 1)
            else:
                v <<= o[2].amount
        if mn == 'mvni':
            v = ~v
        return vloop(d.n, cnt, 'r.%s[i] = (%s)%s;' % (e, UTYPE[e], hx(v, 64)))

    i_mvni = i_movi

    def i_dup(self, mn, o):
        d, s = o
        cnt, e = vinfo(d)
        if s.gpr:
            return vloop(d.n, cnt, 'r.%s[i] = (%s)%s;' % (e, UTYPE[e], rd(s, 64)))
        return vloop(d.n, cnt, 'r.%s[i] = a.%s[%d];' % (e, s.elem, s.idx), [('a', s.n)])

    def _vshift_imm(self, mn, o):
        d, a, n = o[0], o[1], o[2].v
        cnt, e = vinfo(d)
        se, bits = SIGNED[e], EBITS[e]
        srcs = [('a', a.n)]
        if mn == 'shl':
            f = 'a.%s[i] << %d' % (e, n)
        elif mn == 'ushr':
            f = '0' if n >= bits else 'a.%s[i] >> %d' % (e, n)
        elif mn == 'sshr':
            f = 'a.%s[i] >> %d' % (se, min(n, bits - 1))
        elif mn == 'usra':
            f = 'q.%s[i] + (%s)' % (e, '0' if n >= bits else 'a.%s[i] >> %d' % (e, n))
            srcs.append(('q', d.n))
        elif mn == 'ssra':
            f = 'q.%s[i] + (a.%s[i] >> %d)' % (e, se, min(n, bits - 1))
            srcs.append(('q', d.n))
        elif mn in ('urshr', 'srshr'):
            src = 'a.%s[i]' % (e if mn[0] == 'u' else se)
            f = '((%s)%s + ((%s)1 << %d)) >> %d' % ('unsigned __int128' if mn[0] == 'u' else '__int128', src,
                                                     '__int128', n - 1, n)
        else:
            return None
        return vloop(d.n, cnt, 'r.%s[i] = (%s)(%s);' % (e, UTYPE[e], f), srcs)

    i_shl = i_ushr = i_sshr = i_usra = i_ssra = i_urshr = i_srshr = _vshift_imm

    def _vshll(self, mn, o):
        d, a, n = o[0], o[1], o[2].v
        cnt, e = vinfo(d)
        he = HALF[e]
        signed = mn[0] == 's'
        hi = cnt if mn.endswith('2') else 0
        src = 'a.%s[i + %d]' % (SIGNED[he] if signed else he, hi)
        cast = STYPE[e] if signed else UTYPE[e]
        return vloop(d.n, cnt, 'r.%s[i] = (%s)((%s)%s << %d);' % (e, UTYPE[e], UTYPE[e], '(%s)%s' % (cast, src), n),
                     [('a', a.n)])

    i_sshll = i_ushll = i_sshll2 = i_ushll2 = i_shll = i_shll2 = _vshll

    def _vxtl(self, mn, o):
        return self._vshll(mn.replace('xtl', 'shll'), [o[0], o[1], Imm(0)])

    i_sxtl = i_uxtl = i_sxtl2 = i_uxtl2 = _vxtl

    def _vec_widen3(self, mn, o):
        d, a, b = o
        cnt, e = vinfo(d)
        he = HALF[e]
        signed = mn[0] == 's'
        hi = cnt if mn.endswith('2') else 0
        base = mn.rstrip('2')
        cast = STYPE[e] if signed else UTYPE[e]
        hf = SIGNED[he] if signed else he
        srcs = [('a', a.n), ('b', b.n)]
        bx = '(%s)b.%s[i + %d]' % (cast, hf, hi)
        if base[1:] in ('addw', 'subw'):
            ax = 'a.%s[i]' % e
        else:
            ax = '(%s)a.%s[i + %d]' % (cast, hf, hi)
        kind = base[1:]
        if kind in ('addl', 'addw'):
            f = '%s + %s' % (ax, bx)
        elif kind in ('subl', 'subw'):
            f = '%s - %s' % (ax, bx)
        elif kind == 'mull':
            f = '%s * %s' % (ax, bx)
        elif kind == 'mlal':
            f = 'q.%s[i] + %s * %s' % (e, ax, bx)
            srcs.append(('q', d.n))
        elif kind == 'mlsl':
            f = 'q.%s[i] - %s * %s' % (e, ax, bx)
            srcs.append(('q', d.n))
        elif kind == 'abdl':
            f = '%s > %s ? %s - %s : %s - %s' % (ax, bx, ax, bx, bx, ax)
        else:
            return None
        return vloop(d.n, cnt, 'r.%s[i] = (%s)(%s);' % (e, UTYPE[e], f), srcs)

    i_uaddl = i_uaddl2 = i_saddl = i_saddl2 = i_usubl = i_usubl2 = i_ssubl = i_ssubl2 = _vec_widen3
    i_uaddw = i_uaddw2 = i_saddw = i_saddw2 = i_usubw = i_usubw2 = i_ssubw = i_ssubw2 = _vec_widen3
    i_umull2 = i_smull2 = i_umlal = i_umlal2 = i_smlal = i_smlal2 = i_umlsl = i_smlsl = _vec_widen3
    i_uabdl = i_uabdl2 = i_sabdl = i_sabdl2 = _vec_widen3

    def _vnarrow(self, mn, o):
        d, a = o[0], o[1]
        cnt, e = vinfo(d)
        we = {'b': 'h', 'h': 's', 's': 'd'}[e]
        if mn == 'xtn':
            f = 'a.%s[i]' % we
            srcs = [('a', a.n)]
        elif mn == 'shrn':
            f = 'a.%s[i] >> %d' % (we, o[2].v)
            srcs = [('a', a.n)]
        elif mn == 'addhn':
            f = '(a.%s[i] + b.%s[i]) >> %d' % (we, we, EBITS[e])
            srcs = [('a', a.n), ('b', o[2].n)]
        else:
            return None
        return vloop(d.n, cnt, 'r.%s[i] = (%s)(%s);' % (e, UTYPE[e], f), srcs)

    i_xtn = i_shrn = i_addhn = _vnarrow

    def i_fcvtn2(self, mn, o):
        return vloop(o[0].n, 2, 'r.f[2 + i] = (float)a.df[i];', [('a', o[1].n)], keep=True)

    def _vpairl(self, mn, o):
        d, a = o
        cnt, e = vinfo(d)
        he = HALF[e]
        signed = mn[0] == 's'
        hf = SIGNED[he] if signed else he
        cast = STYPE[e] if signed else UTYPE[e]
        acc = mn[1:4] == 'ada'
        f = '(%s)a.%s[2 * i] + (%s)a.%s[2 * i + 1]' % (cast, hf, cast, hf)
        if acc:
            return vloop(d.n, cnt, 'r.%s[i] = (%s)(q.%s[i] + %s);' % (e, UTYPE[e], e, f), [('a', a.n), ('q', d.n)])
        return vloop(d.n, cnt, 'r.%s[i] = (%s)(%s);' % (e, UTYPE[e], f), [('a', a.n)])

    i_uaddlp = i_saddlp = i_uadalp = i_sadalp = _vpairl

    def i_fcvtn(self, mn, o):
        cnt, e = vinfo(o[0])
        return vloop(o[0].n, cnt, 'r.f[i] = (float)a.df[i];', [('a', o[1].n)])

    def i_fcvtl(self, mn, o):
        cnt, e = vinfo(o[0])
        return vloop(o[0].n, cnt, 'r.df[i] = (double)a.f[i];', [('a', o[1].n)])

    def _vperm(self, mn, o):
        d, a, b = o[0], o[1], o[2]
        cnt, e = vinfo(d)
        srcs = [('a', a.n), ('b', b.n)]
        pick = lambda j: '(%s < %d ? a.%s[%s] : b.%s[%s - %d])' % (j, cnt, e, j, e, j, cnt)
        h = cnt // 2
        if mn == 'uzp1':
            body = 'r.%s[i] = %s;' % (e, pick('(2 * i)'))
        elif mn == 'uzp2':
            body = 'r.%s[i] = %s;' % (e, pick('(2 * i + 1)'))
        elif mn == 'zip1':
            body = 'r.%s[i] = (i & 1) ? b.%s[i >> 1] : a.%s[i >> 1];' % (e, e, e)
        elif mn == 'zip2':
            body = 'r.%s[i] = (i & 1) ? b.%s[%d + (i >> 1)] : a.%s[%d + (i >> 1)];' % (e, e, h, e, h)
        elif mn == 'trn1':
            body = 'r.%s[i] = (i & 1) ? b.%s[i & ~1] : a.%s[i & ~1];' % (e, e, e)
        elif mn == 'trn2':
            body = 'r.%s[i] = (i & 1) ? b.%s[i | 1] : a.%s[i | 1];' % (e, e, e)
        elif mn == 'ext':
            body = 'r.b[i] = %s;' % pick('(i + %d)' % o[3].v)
        else:
            return None
        return vloop(d.n, cnt, body, srcs)

    i_uzp1 = i_uzp2 = i_zip1 = i_zip2 = i_trn1 = i_trn2 = i_ext = _vperm

    def i_rev64(self, mn, o):
        cnt, e = vinfo(o[0])
        k = {'rev64': 8, 'rev32': 4, 'rev16': 2}[mn] // EBYTES[e]
        return vloop(o[0].n, cnt, 'r.%s[i] = a.%s[(i / %d) * %d + (%d - i %% %d)];' % (e, e, k, k, k - 1, k),
                     [('a', o[1].n)])

    def i_tbl(self, mn, o):
        d, lst, idx = o
        cnt, e = vinfo(d)
        n = len(lst.regs)
        decl = 'uint8_t t_[%d]; ' % (16 * n) + ''.join(
            'memcpy(t_ + %d, &V(%d), 16); ' % (16 * k, r.n) for k, r in enumerate(lst.regs))
        keep = mn == 'tbx'
        body = 'if (b.b[i] < %d) r.b[i] = t_[b.b[i]];' % (16 * n)
        inner = vloop(d.n, cnt, body, [('b', idx.n)], keep=False)
        if keep:
            inner = inner.replace('V128 r = {0};', 'V128 r = {0}; memcpy(&r, &V(%d), %d);' % (d.n, cnt))
        return '{ %s%s }' % (decl, inner)

    i_tbx = i_tbl

    def i_addv(self, mn, o):
        d, a = o
        cnt, e = ARR[a.arr]
        if mn == 'addv':
            return '{ V128 a = V(%d); uint64_t s_ = 0; for (int i = 0; i < %d; i++) s_ += a.%s[i]; %s }' % (
                a.n, cnt, e, fp_set_raw(d, '(%s)s_' % UTYPE[e]))
        if mn in ('uaddlv', 'saddlv'):
            f = e if mn[0] == 'u' else SIGNED[e]
            return '{ V128 a = V(%d); int64_t s_ = 0; for (int i = 0; i < %d; i++) s_ += a.%s[i]; %s }' % (
                a.n, cnt, f, fp_set_raw(d, 's_'))
        cmp = {'umaxv': (e, '>'), 'uminv': (e, '<'), 'smaxv': (SIGNED[e], '>'), 'sminv': (SIGNED[e], '<')}[mn]
        return ('{ V128 a = V(%d); %s s_ = a.%s[0]; for (int i = 1; i < %d; i++) if (a.%s[i] %s s_) s_ = a.%s[i]; %s }'
                % (a.n, (UTYPE if cmp[0] == e else STYPE)[e], cmp[0], cnt, cmp[0], cmp[1], cmp[0],
                   fp_set_raw(d, '(%s)s_' % UTYPE[e])))

    i_uaddlv = i_saddlv = i_umaxv = i_uminv = i_smaxv = i_sminv = i_addv

    def i_addp(self, mn, o):
        d, a = o[0], o[1]
        if d.kind in 'bhsd' and d.kind != 'v':
            return fp_set_raw(d, 'V(%d).d[0] + V(%d).d[1]' % (a.n, a.n))
        b = o[2]
        cnt, e = vinfo(d)
        h = cnt // 2
        body = 'r.%s[i] = (%s)(i < %d ? a.%s[2 * i] + a.%s[2 * i + 1] : b.%s[2 * (i - %d)] + b.%s[2 * (i - %d) + 1]);' % (
            e, UTYPE[e], h, e, e, e, h, e, h)
        return vloop(d.n, cnt, body, [('a', a.n), ('b', b.n)])

    def i_faddp(self, mn, o):
        d, a = o[0], o[1]
        if d.kind != 'v':
            f = FLOATF[d.kind]
            return '{ V128 a = V(%d); %s }' % (a.n, fp_set(d, 'a.%s[0] + a.%s[1]' % (f, f)))
        b = o[2]
        cnt, e = vinfo(d)
        f, h = FLOATF[e], cnt // 2
        body = 'r.%s[i] = i < %d ? a.%s[2 * i] + a.%s[2 * i + 1] : b.%s[2 * (i - %d)] + b.%s[2 * (i - %d) + 1];' % (
            f, h, f, f, f, h, f, h)
        return vloop(d.n, cnt, body, [('a', a.n), ('b', b.n)])

    # vector / by-element floating point
    def _vfp3(self, mn, o):
        d, a, b = o[0], o[1], o[2]
        cnt, e = vinfo(d)
        f = FLOATF[e]
        fn = fnames(e)
        body = 'r.%s[i] = %s;' % (f, FP3[mn].format(a='a.%s[i]' % f, b=elem_src(b, 'b', f), **fn))
        return vloop(d.n, cnt, body, [('a', a.n), ('b', b.n)])

    def _vfp1(self, mn, o):
        d, a = o[0], o[1]
        cnt, e = vinfo(d)
        f = FLOATF[e]
        return vloop(d.n, cnt, 'r.%s[i] = %s;' % (f, FP1[mn].format(a='a.%s[i]' % f, **fnames(e))), [('a', a.n)])

    def _vfmla(self, mn, o):
        d, a, b = o
        cnt, e = vinfo(d)
        f = FLOATF[e]
        fma = fnames(e)['fma']
        sign = '' if mn == 'fmla' else '-'
        body = 'r.%s[i] = %s(%sa.%s[i], %s, q.%s[i]);' % (f, fma, sign, f, elem_src(b, 'b', f), f)
        return vloop(d.n, cnt, body, [('q', d.n), ('a', a.n), ('b', b.n)])

    i_fmla = i_fmls = _vfmla

    def _vfcmp(self, mn, o):
        d, a, b = o
        cnt, e = vinfo(d)
        f = FLOATF[e]
        if isinstance(b, Imm):
            body = 'r.%s[i] = a.%s[i] %s 0 ? -1 : 0;' % (e, f, FPCMP[mn])
            return vloop(d.n, cnt, body, [('a', a.n)])
        body = 'r.%s[i] = a.%s[i] %s b.%s[i] ? -1 : 0;' % (e, f, FPCMP[mn], f)
        return vloop(d.n, cnt, body, [('a', a.n), ('b', b.n)])

    i_fcmeq = i_fcmgt = i_fcmge = i_fcmlt = i_fcmle = _vfcmp

    # structure loads and stores
    def _ldst(self, mn, o):
        lst, m = o[0], o[1]
        post = self._post(o, 2)
        load = mn.startswith('ld')
        n = len(lst.regs)
        r0 = lst.regs[0]
        base = 'X(%d)' % m.base.n
        if mn == 'ld1r':
            cnt, e = ARR[r0.arr]
            total = EBYTES[e]
            body = '{ V128 r = {0}; %s t_ = %s(a_); for (int i = 0; i < %d; i++) r.%s[i] = t_; V(%d) = r; }' % (
                UTYPE[e], MEMU[EBYTES[e]], cnt, e, r0.n)
        elif lst.idx is not None:  # single lane
            e = r0.elem
            total = EBYTES[e] * n
            parts = []
            for k, r in enumerate(lst.regs):
                loc = '%s(a_ + %d)' % (MEMU[EBYTES[e]], k * EBYTES[e])
                lane = 'V(%d).%s[%d]' % (r.n, e, lst.idx)
                parts.append('%s = %s;' % ((lane, loc) if load else (loc, lane)))
            body = ' '.join(parts)
        else:
            cnt, e = ARR[r0.arr]
            sz = EBYTES[e]
            total = cnt * sz * n
            interleave = int(mn[2]) > 1
            parts = []
            for k, r in enumerate(lst.regs):
                if interleave:
                    loc = '%s(a_ + (i * %d + %d) * %d)' % (MEMU[sz], n, k, sz)
                else:
                    loc = '%s(a_ + %d + i * %d)' % (MEMU[sz], k * cnt * sz, sz)
                if load:
                    parts.append('{ V128 r = {0}; for (int i = 0; i < %d; i++) r.%s[i] = %s; t_[%d] = r; }' % (cnt, e, loc, k))
                else:
                    parts.append('{ V128 r = V(%d); for (int i = 0; i < %d; i++) %s = r.%s[i]; }' % (r.n, cnt, loc, e))
            if load:
                body = 'V128 t_[%d]; %s %s' % (n, ' '.join(parts),
                                                ' '.join('V(%d) = t_[%d];' % (r.n, k) for k, r in enumerate(lst.regs)))
            else:
                body = ' '.join(parts)
        wb = ''
        if post is not None:
            wb = '%s = a_ + %s;' % (base, hx(post.v) if isinstance(post, Imm) else rd(post, 64))
        return '{ uint64_t a_ = %s; %s %s }' % (base, body, wb)

    i_ld1 = i_ld2 = i_ld3 = i_ld4 = i_ld1r = i_st1 = i_st2 = i_st3 = i_st4 = _ldst
