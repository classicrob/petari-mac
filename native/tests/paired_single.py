"""A small Gekko/Broadway paired-single interpreter for checking native branches.

It runs the Wii asm text from the sources (the `#ifdef __MWERKS__` side) on a byte
memory, so tests can compare the native `#else` branch with what the Wii code does,
not with a second reading of it. Only the instructions these sources use are
modeled; anything else raises, so a new instruction cannot pass silently.

Semantics follow the Broadway manual (and Dolphin where the manual is silent):
- Single-precision results are rounded to f32. Fused multiply-adds are computed in
  double and rounded once to f32 (a product of two f32 is exact in double).
- psq_l with W=1 loads ps1 = 1.0. Quantized loads scale by 2^-scale; quantized
  stores scale by 2^scale, saturate to the type's range and truncate toward zero.
- lfs, and single-precision scalar arithmetic, set ps1 = ps0; fmr/fneg/fabs set ps0 only.
- fres and frsqrte are modeled as exact reciprocals (the hardware is an estimate
  that the sources refine with Newton steps), so compare their users with a tolerance.
"""
import math
import re
import struct

_F32 = struct.Struct("<f")


def f32(value):
    """Round a Python float to the nearest float32 (overflow gives an infinity, as the hardware does)."""
    try:
        return _F32.unpack(_F32.pack(value))[0]
    except OverflowError:
        return math.copysign(math.inf, value)


# GQRs as J3DSys::drawInit / OSInitFastCast set them: 0 = f32, 2 = u8, 3 = u16, 4 = s8, 5 = s16, scale 0.
GQR_TYPES = {0: ("f", 0), 2: ("B", 0), 3: ("H", 0), 4: ("b", 0), 5: ("h", 0)}
RANGES = {"B": (0, 255), "H": (0, 65535), "b": (-128, 127), "h": (-32768, 32767)}


class Memory:
    def __init__(self, size=1 << 20):
        self.data = bytearray(size)
        self.next = 0x1000

    def alloc(self, size, align=32):
        self.next = (self.next + align - 1) & ~(align - 1)
        address = self.next
        self.next += size
        return address

    def f32s(self, address, count):
        return list(struct.unpack_from(">%df" % count, self.data, address))

    def put_f32s(self, address, values):
        struct.pack_into(">%df" % len(values), self.data, address, *[f32(v) for v in values])

    def s16s(self, address, count):
        return list(struct.unpack_from(">%dh" % count, self.data, address))

    def put_s16s(self, address, values):
        struct.pack_into(">%dh" % len(values), self.data, address, *values)


class Machine:
    def __init__(self, memory, symbols=None):
        self.mem = memory
        self.gpr = {}
        self.fpr = {}  # name -> [ps0, ps1]
        self.alias = {}
        self.ctr = 0
        self.cr0 = 0  # -1 lt, 0 eq, 1 gt, None unordered
        self.symbols = symbols or {}
        self.gpr["r1"] = memory.alloc(0x400) + 0x3F0

    # Register naming: fN / fpN / FPn are float registers, rN / sp are integer ones; other
    # identifiers are C `register` variables bound by the caller or created on first use.
    def canon(self, name):
        name = self.alias.get(name, name)
        match = re.fullmatch(r"(?:f|fp|FP)(\d+)", name)
        if match:
            return "f" + match.group(1)
        if name == "sp":
            return "r1"
        return name

    def bind_f(self, name, value, ps1=None):
        self.fpr[self.canon(name)] = [f32(value), f32(value if ps1 is None else ps1)]

    def bind_r(self, name, value):
        self.gpr[self.canon(name)] = value & 0xFFFFFFFF

    def f(self, name):
        name = self.canon(name)
        if name not in self.fpr:
            self.fpr[name] = [0.0, 0.0]
        return self.fpr[name]

    def r(self, name):
        name = self.canon(name)
        if name == "0":
            return 0
        if name not in self.gpr:
            self.gpr[name] = 0
        return self.gpr[name]

    def setr(self, name, value):
        self.gpr[self.canon(name)] = value & 0xFFFFFFFF

    def setf(self, name, ps0, ps1):
        self.fpr[self.canon(name)] = [f32(ps0), f32(ps1)]

    def ea(self, operand):
        match = re.fullmatch(r"\s*(-?(?:0x[0-9a-fA-F]+|\d+))\s*\(\s*(\w+)\s*\)\s*", operand)
        if not match:
            raise ValueError("unsupported address operand %r" % operand)
        return (int(match.group(1), 0) + self.r(match.group(2))) & 0xFFFFFFFF, match.group(2)

    def imm(self, text):
        text = text.strip()
        match = re.fullmatch(r"(\w+)@(ha|l)", text)
        if match:
            address = self.symbols[match.group(1)]
            if match.group(2) == "ha":
                return ((address + 0x8000) >> 16) & 0xFFFF
            low = address & 0xFFFF
            return low - 0x10000 if low & 0x8000 else low
        return int(text, 0)

    def load_q(self, address, gqr):
        kind, scale = GQR_TYPES[gqr]
        size = struct.calcsize(">" + kind)
        value = struct.unpack_from(">" + kind, self.mem.data, address)[0]
        return f32(value * 2.0 ** -scale) if kind != "f" else value, size

    def store_q(self, address, gqr, value):
        kind, scale = GQR_TYPES[gqr]
        if kind == "f":
            struct.pack_into(">f", self.mem.data, address, f32(value))
            return 4
        low, high = RANGES[kind]
        scaled = value * 2.0 ** scale
        if math.isnan(scaled):
            integer = 0
        else:
            integer = int(max(low, min(high, math.trunc(max(-1e30, min(1e30, scaled))))))
        struct.pack_into(">" + kind, self.mem.data, address, integer)
        return struct.calcsize(">" + kind)

    def run(self, text, defines=None):
        lines = []
        labels = {}
        for raw in text.split("\n"):
            line = raw.split("//")[0].split(";")[0]
            line = re.sub(r"/\*.*?\*/", "", line).strip()
            if not line or line.startswith("#") or line in ("nofralloc", "{", "}", "asm", "asm {", "__asm {", "__asm"):
                continue
            if line.endswith(":"):
                labels[line[:-1]] = len(lines)
                continue
            for key, value in (defines or {}).items():
                line = re.sub(r"\b%s\b" % re.escape(key), value, line)
            lines.append(line)
        pc = 0
        steps = 0
        while pc < len(lines):
            steps += 1
            if steps > 100000:
                raise RuntimeError("runaway asm")
            op, _, rest = lines[pc].partition(" ")
            args = [a.strip() for a in re.split(r",(?![^()]*\))", rest)] if rest.strip() else []
            pc += 1
            jump = self.step(op.strip(), args, labels)
            if jump == "return":
                return
            if jump is not None:
                pc = jump

    def step(self, op, a, labels):
        F, R = self.f, self.r
        # Loads and stores
        if op in ("psq_l", "psq_lu"):
            address, base = self.ea(a[1])
            w, gqr = int(a[2], 0), int(a[3], 0)
            v0, size = self.load_q(address, gqr)
            v1 = 1.0 if w else self.load_q(address + size, gqr)[0]
            self.setf(a[0], v0, v1)
            if op == "psq_lu":
                self.setr(base, address)
        elif op in ("psq_st", "psq_stu"):
            address, base = self.ea(a[1])
            w, gqr = int(a[2], 0), int(a[3], 0)
            value = F(a[0])
            size = self.store_q(address, gqr, value[0])
            if not w:
                self.store_q(address + size, gqr, value[1])
            if op == "psq_stu":
                self.setr(base, address)
        elif op in ("lfs", "lfsu"):
            address, base = self.ea(a[1])
            value = struct.unpack_from(">f", self.mem.data, address)[0]
            self.setf(a[0], value, value)
            if op == "lfsu":
                self.setr(base, address)
        elif op in ("stfs", "stfsu"):
            address, base = self.ea(a[1])
            struct.pack_into(">f", self.mem.data, address, F(a[0])[0])
            if op == "stfsu":
                self.setr(base, address)
        elif op == "stfd":
            address, _ = self.ea(a[1])
            struct.pack_into(">d", self.mem.data, address, F(a[0])[0])
        elif op == "lfd":
            address, _ = self.ea(a[1])
            value = struct.unpack_from(">d", self.mem.data, address)[0]
            self.fpr[self.canon(a[0])] = [value, F(a[0])[1]]
        elif op == "lwz":
            self.setr(a[0], struct.unpack_from(">I", self.mem.data, self.ea(a[1])[0])[0])
        elif op == "stw":
            struct.pack_into(">I", self.mem.data, self.ea(a[1])[0], R(a[0]))
        elif op == "stwu":
            address, base = self.ea(a[1])
            struct.pack_into(">I", self.mem.data, address, R(a[0]))
            self.setr(base, address)
        elif op == "lha":
            self.setr(a[0], struct.unpack_from(">h", self.mem.data, self.ea(a[1])[0])[0])
        elif op == "lhz":
            self.setr(a[0], struct.unpack_from(">H", self.mem.data, self.ea(a[1])[0])[0])
        elif op == "lbz":
            self.setr(a[0], self.mem.data[self.ea(a[1])[0]])
        # Integer
        elif op == "li":
            self.setr(a[0], self.imm(a[1]))
        elif op == "lis":
            self.setr(a[0], self.imm(a[1]) << 16)
        elif op == "addi":
            self.setr(a[0], R(a[1]) + self.imm(a[2]))
        elif op == "subi":
            self.setr(a[0], R(a[1]) - self.imm(a[2]))
        elif op == "addis":
            self.setr(a[0], R(a[1]) + (self.imm(a[2]) << 16))
        elif op == "oris":
            self.setr(a[0], R(a[1]) | (self.imm(a[2]) << 16))
        elif op == "mr":
            self.setr(a[0], R(a[1]))
        elif op == "xor":
            self.setr(a[0], R(a[1]) ^ R(a[2]))
        elif op == "mtctr":
            self.ctr = R(a[0])
        elif op == "mtspr":
            pass  # GQR setup; GQR_TYPES already models the game's values.
        # Branches
        elif op == "bdnz":
            self.ctr = (self.ctr - 1) & 0xFFFFFFFF
            return labels[a[0]] if self.ctr != 0 else None
        elif op in ("bne", "beq", "blt", "bgt", "ble", "bge"):
            target = a[-1]
            c = self.cr0
            taken = {"bne": c != 0, "beq": c == 0, "blt": c == -1, "bgt": c == 1, "ble": c in (-1, 0), "bge": c in (0, 1)}[op]
            return labels[target] if taken else None
        elif op == "b":
            return labels[a[0]]
        elif op == "blr":
            return "return"
        # Floating-point compares
        elif op in ("ps_cmpo0", "fcmpo", "fcmpu", "ps_cmpu0"):
            x, y = F(a[1])[0], F(a[2])[0]
            self.cr0 = None if (math.isnan(x) or math.isnan(y)) else (-1 if x < y else 1 if x > y else 0)
        # Paired single
        elif op.startswith("ps_"):
            self.paired(op[3:], a)
        # Scalar single precision
        else:
            self.scalar(op, a)
        return None

    def paired(self, op, a):
        F = self.f
        d = a[0]
        x = F(a[1]) if len(a) > 1 else None
        y = F(a[2]) if len(a) > 2 else None
        z = F(a[3]) if len(a) > 3 else None
        fused = lambda p, q, r, sign=1.0, neg=False: (-(p * q + sign * r)) if neg else (p * q + sign * r)
        if op == "add":
            self.setf(d, x[0] + y[0], x[1] + y[1])
        elif op == "sub":
            self.setf(d, x[0] - y[0], x[1] - y[1])
        elif op == "mul":
            self.setf(d, x[0] * y[0], x[1] * y[1])
        elif op == "div":
            self.setf(d, x[0] / y[0], x[1] / y[1])
        elif op == "muls0":
            self.setf(d, x[0] * y[0], x[1] * y[0])
        elif op == "muls1":
            self.setf(d, x[0] * y[1], x[1] * y[1])
        elif op == "madd":
            self.setf(d, fused(x[0], y[0], z[0]), fused(x[1], y[1], z[1]))
        elif op == "msub":
            self.setf(d, fused(x[0], y[0], z[0], -1.0), fused(x[1], y[1], z[1], -1.0))
        elif op == "nmadd":
            self.setf(d, fused(x[0], y[0], z[0], 1.0, True), fused(x[1], y[1], z[1], 1.0, True))
        elif op == "nmsub":
            self.setf(d, fused(x[0], y[0], z[0], -1.0, True), fused(x[1], y[1], z[1], -1.0, True))
        elif op == "madds0":
            self.setf(d, fused(x[0], y[0], z[0]), fused(x[1], y[0], z[1]))
        elif op == "madds1":
            self.setf(d, fused(x[0], y[1], z[0]), fused(x[1], y[1], z[1]))
        elif op == "sum0":
            self.setf(d, x[0] + z[1], y[1])
        elif op == "sum1":
            self.setf(d, y[0], x[0] + z[1])
        elif op == "merge00":
            self.setf(d, x[0], y[0])
        elif op == "merge01":
            self.setf(d, x[0], y[1])
        elif op == "merge10":
            self.setf(d, x[1], y[0])
        elif op == "merge11":
            self.setf(d, x[1], y[1])
        elif op == "neg":
            self.setf(d, -x[0], -x[1])
        elif op == "abs":
            self.setf(d, abs(x[0]), abs(x[1]))
        elif op == "nabs":
            self.setf(d, -abs(x[0]), -abs(x[1]))
        elif op == "mr":
            self.setf(d, x[0], x[1])
        else:
            raise NotImplementedError("ps_" + op)

    def scalar(self, op, a):
        F = self.f
        d = a[0]
        x = F(a[1]) if len(a) > 1 else None
        y = F(a[2]) if len(a) > 2 else None
        z = F(a[3]) if len(a) > 3 else None
        one = lambda v: self.setf(d, v, v)
        if op == "fadds":
            one(x[0] + y[0])
        elif op == "fsubs":
            one(x[0] - y[0])
        elif op == "fmuls":
            one(x[0] * y[0])
        elif op == "fdivs":
            one(x[0] / y[0])
        elif op == "fmadds":
            one(x[0] * y[0] + z[0])
        elif op == "fmsubs":
            one(x[0] * y[0] - z[0])
        elif op == "fnmadds":
            one(-(x[0] * y[0] + z[0]))
        elif op == "fnmsubs":
            one(-(x[0] * y[0] - z[0]))
        elif op == "fres":
            one(1.0 / x[0] if x[0] != 0 else math.copysign(math.inf, x[0]))
        elif op == "frsqrte":
            one(1.0 / math.sqrt(x[0]) if x[0] > 0 else (math.inf if x[0] == 0 else math.nan))
        elif op == "fneg":
            self.fpr[self.canon(d)] = [-x[0], F(d)[1]]
        elif op == "fabs":
            self.fpr[self.canon(d)] = [abs(x[0]), F(d)[1]]
        elif op == "fmr":
            self.fpr[self.canon(d)] = [x[0], F(d)[1]]
        else:
            raise NotImplementedError(op)


def extract_asm(path, anchor):
    """Return the Wii asm after `anchor`: the body of an `asm void` function, or the first `asm { }` block."""
    text = open(path, encoding="utf-8", errors="replace").read()
    start = text.index(anchor)
    if anchor.startswith("asm void"):
        match_start = text.index("{", start)
    else:
        match = re.compile(r"\b(?:__asm|asm)\b(?:\s+volatile)?\s*\{").search(text, start)
        if match is None:
            raise ValueError("no asm block after %r in %s" % (anchor, path))
        match_start = match.end() - 1
    depth, i = 1, match_start + 1
    while depth:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    return text[match_start + 1:i - 1]
