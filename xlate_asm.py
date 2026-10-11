#!/usr/bin/env python3
"""xlate_asm.py — translate ID4 .asm_lbg shader text for PS5 Mesa.

NV_fragment_program2 -> ARBfp1.0 and NV_vertex_program3 -> ARBvp1.0.

The .asm_lbg files are cgc 1.4 output the game feeds to glProgramStringARB
through hle_cg.  The runtime rewrite in hle_gl_orbis.c keeps them parseable
by dropping condition-code writes and predication; this translator produces
semantics-preserving ARB 1.0 text instead:

  - H/R-suffix half ops (MOVH, MADH_SAT, MULR, RCPR, ...) -> plain ops
  - SHORT/LONG decl qualifiers -> stripped
  - XXxC opcodes (SLTHC, SGTC, MOVC, MULC, ADDC_SAT, ...) -> plain op into
    the declared dst; the written dst+mask becomes the live "CC" mirror
  - MOVC/MOVRC/MOVXC (move-to-CC forms) -> MOV into a scratch cReg
  - predicated dst (GT)/(LE)/(NE)/(EQ)/(LT)/(GE)[.x] ->
    fp:  evaluate into scratch, CMP dst, -cc, pick, pick'   (exact select)
    vp:  normalize cc to a 0/1 mask, dst = old + mask*(new-old) via SUB+MAD
  - OUTPUT alias = result.x  -> decl dropped, uses renamed
  - sampler2DSHADOW texunits -> texture[N] target becomes SHADOW and
    OPTION ARB_fragment_program_shadow is emitted (PS5 Mesa exposes it;
    the driver has a real compare path and the game sets COMPARE_MODE)

All '#' comment lines (#var, #semantic, #const, cgc header, instruction
counts) are passed through byte-identical — the game parses #var to bind
constants and attributes.

Files in SKIP_FILES are refused: they contain real control flow (BRA to
labels) that has no vp1 equivalent and would hang the GPU if flattened.
"""

import re
import sys

SKIP_FILES = {"vedge.asm_lbg", "vedgeW2.asm_lbg"}

# ---------------------------------------------------------------------------
# Opcode maps
# ---------------------------------------------------------------------------

# *H and *R precision-suffix opcodes -> plain ARB op
PLAIN_OP = {}
for _b in ("MOV", "ADD", "MUL", "SUB", "MAD", "MIN", "MAX", "RSQ", "RCP",
           "DP3", "DP4", "DPA", "FRC", "FLR", "ABS", "NRM", "LRP", "SGE",
           "SGT", "SLE", "SLT", "SEQ", "SNE", "LG2", "EX2", "POW", "LIT",
           "DST", "SWZ", "XPD"):
    PLAIN_OP[_b + "H"] = _b
    PLAIN_OP[_b + "R"] = _b
# fp2-only spellings
PLAIN_OP.update({"MADR": "MAD", "LPRH": "LRP", "LRPH": "LRP", "LGHH": "LG2",
                 "DPAH": "DP4"})

# Set-op + CC-write: "XXxC" -> plain set/compute op
CC_OP = {}
for _b in ("SLT", "SGT", "SLE", "SGE", "SEQ", "SNE",
           "MOV", "ADD", "MUL", "SUB", "MAD", "MIN", "MAX", "RCP", "RSQ",
           "DP3", "DP4"):
    CC_OP[_b + "C"] = _b
CC_OP.update({"SLTHC": "SLT", "SGTHC": "SGT", "SLEHC": "SLE", "SGEHC": "SGE",
              "SEQHC": "SEQ", "SNEHC": "SNE", "MOVHC": "MOV",
              "SLTRC": "SLT", "SGTRC": "SGT", "MOVRC": "MOV", "MOVXC": "MOV",
              "ADDC": "ADD", "MULC": "MUL", "SUBC": "SUB", "MADC": "MAD",
              "MINC": "MIN", "MAXC": "MAX"})

PREDICATES = ("GT", "LT", "GE", "LE", "EQ", "NE", "TR", "FL")

RE_DECL_TEMP = re.compile(r"^\s*(?:SHORT\s+|LONG\s+)?TEMP\s+")
RE_DECL_PARAM = re.compile(r"^\s*(?:SHORT\s+|LONG\s+)?PARAM\s+")
RE_DECL_OUTPUT = re.compile(r"^\s*(?:SHORT\s+|LONG\s+)?OUTPUT\s+([A-Za-z_][\w.]*)\s*=\s*([\w.\[\]-]+)\s*;")
RE_OPTION = re.compile(r"^\s*OPTION\s+(\S+);")
RE_INSTR = re.compile(
    r"^([A-Z][A-Z0-9_]*(?:_SAT)?)\s+"          # opcode
    r"([A-Za-z_][\w.\[\]]*?)"                  # dst register+mask
    r"(?:\s*\(([A-Z]{2})(?:\.(x|y|z|w))?\))?"  # optional (COND) predicate
    r"\s*,\s*(.*?)\s*;\s*$"                    # src operands
)
RE_LABEL = re.compile(r"^\s*[A-Za-z_][\w]*:\s*$")
RE_VAR_TEX = re.compile(r"#var\s+\w*sampler(\w+)\s+\S+\s*:\s*:\s*texunit\s+(\d+)")
RE_IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


class Xlate:
    def __init__(self, text, is_fp):
        self.src = text
        self.is_fp = is_fp
        self.aliases = {}          # OUTPUT alias -> result reg
        self.need_creg = False     # scratch CC mirror used
        self.need_pred = False     # scratch predicate temps used
        self.shadow_units = set()  # texunits with sampler2DSHADOW
        self.cur_cc = None         # (regname, comp) of live CC mirror
        self.temp_names = set()
        self.res_shadow = {}       # result reg name -> shadow temp name
        self.abs_regs = {}         # operand core -> ABS'd temp name

    # -- helpers -----------------------------------------------------------
    def map_operand(self, tok):
        """Rewrite one operand token: OUTPUT aliases; vp |src| -> ABS temp."""
        neg = tok.startswith("-")
        if neg:
            tok = tok[1:]
        absw = tok.startswith("|") and tok.endswith("|")
        core = tok.strip("|")
        base = core.split(".")[0].split("[")[0]
        if base in self.aliases:
            _, _, suf = core.partition(".")
            core = self.aliases[base] + ("." + suf if suf else "")
        if absw and not self.is_fp:
            # ARBvp1.0 has no |src| modifier: emit ABS into a scratch temp
            if core not in self.abs_regs:
                an = f"predA{len(self.abs_regs)}"
                self.abs_regs[core] = an
                self.pre_ops.append(f"ABS   {an}, {core};")
            core = self.abs_regs[core]
            absw = False
        out = ("|" if absw else "") + core + ("|" if absw else "")
        return ("-" if neg else "") + out

    # -- main ---------------------------------------------------------------
    def run(self):
        lines = self.src.split("\n")
        body = []
        shadow_opt = False
        saw_end = False

        # first pass over #var lines for shadow samplers
        for ln in lines:
            mv = RE_VAR_TEX.search(ln)
            if mv and "SHADOW" in mv.group(1):
                self.shadow_units.add(int(mv.group(2)))

        # pre-pass: find predicated writes to result.* (vp) so unconditional
        # writes earlier in the shader also mirror into shadow temps
        if not self.is_fp:
            for ln in lines:
                mi = RE_INSTR.match(ln.strip())
                if mi and mi.group(3):
                    base = re.match(r"^(.*?)(?:\.[xyzw]{1,4})?$",
                                    mi.group(2)).group(1)
                    if base.startswith("result"):
                        self.res_shadow.setdefault(
                            base, "RSH" + str(len(self.res_shadow)))

        for ln in lines:
            st = ln.strip()
            if not st or st.startswith("#"):
                body.append(ln)                # comments/blank: verbatim
                continue
            if st.startswith("!!"):
                body.append(ln)                # header: verbatim
                continue
            mo = RE_OPTION.match(ln)
            if mo:
                opt = mo.group(1)
                if opt in ("NV_fragment_program2", "NV_vertex_program3"):
                    if self.shadow_units and not shadow_opt:
                        body.append("OPTION ARB_fragment_program_shadow;")
                        shadow_opt = True
                    continue                   # drop NV option
                body.append(ln)
                continue
            if RE_DECL_TEMP.match(ln):
                # strip SHORT/LONG, record names
                decl = RE_DECL_TEMP.sub("TEMP ", ln)
                body.append(decl)
                for nm in re.findall(r"[A-Za-z_][\w]*", decl)[1:]:
                    if nm != "TEMP":
                        self.temp_names.add(nm)
                continue
            if RE_DECL_PARAM.match(ln):
                decl = RE_DECL_PARAM.sub("PARAM ", ln)
                body.append(decl)
                continue
            mo = RE_DECL_OUTPUT.match(ln)
            if mo:
                self.aliases[mo.group(1)] = mo.group(2)
                continue                       # drop OUTPUT decl
            if st.startswith("ADDRESS"):
                body.append(ln)
                continue
            if RE_LABEL.match(ln):
                body.append(ln)
                continue
            mi = RE_INSTR.match(st)
            if mi:
                body.extend(self.emit_instr(mi))
                continue
            if st == "END":
                saw_end = True
                body.append(ln)
                continue
            body.append(ln)                    # unknown: pass through

        # inject scratch decls right after the last TEMP/PARAM/ATTRIB decl
        scratch = []
        if self.need_creg:
            scratch.append("TEMP cReg;")
        if self.need_pred:
            scratch.append("TEMP predT;")
            scratch.append("TEMP predD;")
        if not self.is_fp and self.need_pred:
            scratch.append("TEMP predM;")
        if scratch:
            idx = 0
            for i, l in enumerate(body):
                if l.lstrip().startswith(("TEMP ", "PARAM ", "ATTRIB ",
                                          "ADDRESS ")):
                    idx = i + 1
            body[idx:idx] = scratch
        # scratch decls for shadow/abs temps
        addl = [f"TEMP {s};" for s in self.res_shadow.values()]
        addl += [f"TEMP {a};" for a in self.abs_regs.values()]
        if addl:
            idx = 0
            for i, l in enumerate(body):
                if l.lstrip().startswith(("TEMP ", "PARAM ", "ATTRIB ",
                                          "ADDRESS ")):
                    idx = i + 1
            body[idx:idx] = addl
        return "\n".join(body)

    def emit_instr(self, mi):
        op, dst, pred, pcomp, srcs = mi.groups()
        out = []
        base_op = op
        sat = False
        if base_op.endswith("_SAT"):
            sat = True
            base_op = base_op[:-4]

        self.pre_ops = []           # ABS expansions queued before the op

        # CC-write ops: XXxC dst, ...  -> plain op into dst; dst becomes CC
        if base_op in CC_OP:
            newop = CC_OP[base_op]
            ops = self.map_srcs(srcs)
            d, m = self.split_dst(dst)
            suff = "_SAT" if sat else ""
            out.extend(self.pre_ops)
            out.append(f"{newop}{suff}  {self.fix_dst(dst)}, {', '.join(ops)};")
            self.cur_cc = (d, m or "x")
            return out

        # ARBvp1.0 lacks SLE/SEQ/SNE (fp has them) — expand via SGE/SLT
        if not self.is_fp and base_op in ("SLE", "SEQ", "SNE"):
            return self.expand_setop(base_op, dst, srcs)

        if base_op in PLAIN_OP:
            newop = PLAIN_OP[base_op]
        else:
            newop = base_op

        ops = self.map_srcs(srcs)
        suff = "_SAT" if sat else ""
        fixed_dst = self.fix_dst(dst)
        dbase = re.match(r"^(.*?)(?:\.[xyzw]{1,4})?$", fixed_dst).group(1)
        shadow = self.res_shadow.get(dbase)
        out.extend(self.pre_ops)

        if pred and pred in PREDICATES and pred not in ("TR", "FL"):
            self.need_pred = True
            d, m = self.split_dst(fixed_dst)
            dm = d + ("." + m if m else "")
            old = dm                       # temp regs: read-modify-write
            if shadow:
                old = shadow + ("." + m if m else "")
            cc_reg, cc_comp = self.cur_cc or ("cReg", "x")
            cc = f"{cc_reg}.{cc_comp}"
            tm = "." + m if m else ""
            if self.is_fp:
                out.append(f"{newop}{suff}  predT{tm}, {', '.join(ops)};")
                out.append(self.pred_cmp(d, m, pred, cc))
            else:
                out.extend(self.pred_mask(pred, cc))
                out.append(f"{newop}{suff}  predT{tm}, {', '.join(ops)};")
                out.append(f"SUB   predD{tm}, predT{tm}, {old};")
                out.append(f"MAD   {dm}, predM.x, predD{tm}, {old};")
                if shadow:
                    out.append(
                        f"MAD   {old}, predM.x, predD{tm}, {old};")
            return out

        out.append(f"{newop}{suff}  {fixed_dst}, {', '.join(ops)};")
        if shadow:
            _, sm = self.split_dst(fixed_dst)
            out.append(f"{newop}{suff}  "
                       f"{shadow}{'.' + sm if sm else ''}, {', '.join(ops)};")
        return out

    _MMASK = re.compile(r"^(.*?)(?:\.([xyzw]{1,4}))?$")

    @classmethod
    def split_dst(cls, dst):
        m = cls._MMASK.match(dst)
        return m.group(1), (m.group(2) or "")

    def fix_dst(self, dst):
        d, dot, m = dst.partition(".")
        if d in self.aliases:
            d = self.aliases[d]
        return d + (dot + m if dot else "")

    def map_srcs(self, srcs):
        """Split operands, map each; retarget shadow-sampler units."""
        ops = [self.map_operand(t.strip()) for t in srcs.split(",")]
        # texture target is the final operand: TEX d, coord, texture[N], 2D
        if len(ops) >= 3:
            m = re.match(r"^texture\[(\d+)\]$", ops[-2])
            if m and int(m.group(1)) in self.shadow_units and \
                    ops[-1] in ("2D", "CUBE", "RECT", "3D"):
                ops[-1] = "SHADOW"
        return ops

    def expand_setop(self, op, dst, srcs):
        """vp1 lacks SLE/SEQ/SNE — expand to SGE/SLT pairs.
        SLE(a,b)=SGE(b,a);  SEQ(a,b)=SGE(a,b)*SGE(b,a);  SNE=1-SEQ."""
        self.need_pred = True
        ops = [self.map_operand(t.strip()) for t in srcs.split(",")]
        a, b = ops[0], ops[1]
        d, m = self.split_dst(self.fix_dst(dst))
        tm = "." + m if m else ""
        fd = d + tm
        if op == "SLE":
            return [f"SGE   {fd}, {b}, {a};"]
        if op == "SEQ":
            return [f"SGE   predT{tm}, {a}, {b};",
                    f"SGE   predD{tm}, {b}, {a};",
                    f"MUL   {fd}, predT{tm}, predD{tm};"]
        return [f"SLT   predT{tm}, {a}, {b};",
                f"SLT   predD{tm}, {b}, {a};",
                f"ADD   predT{tm}, predT{tm}, predD{tm};",
                f"MIN   {fd}, predT{tm}, 1.0;"]

    def pred_cmp(self, dst, m, pred, cc):
        """fp1 CMP select:  CMP dst, s, a, b  =  s<0 ? a : b"""
        tm = "." + m if m else ""
        d, p = dst + tm, "predT" + tm
        if pred == "GT":                       # cc>0 -> new
            return f"CMP   {d}, -{cc}, {p}, {d};"
        if pred == "LE":                       # cc<=0 -> new
            return f"CMP   {d}, -{cc}, {d}, {p};"
        if pred == "NE":                       # cc!=0 -> new
            return f"CMP   {d}, -|{cc}|, {p}, {d};"
        if pred == "EQ":                       # cc==0 -> new
            return f"CMP   {d}, |{cc}|, {d}, {p};"
        if pred == "LT":                       # cc<0 -> new
            return f"CMP   {d}, {cc}, {p}, {d};"
        if pred == "GE":                       # cc>=0 -> new
            return f"CMP   {d}, {cc}, {d}, {p};"
        return f"CMP   {d}, -{cc}, {p}, {d};"

    def pred_mask(self, pred, cc):
        """vp1 mask build — vp1 set-ops are SLT/SGE only, no |src|."""
        if pred == "GT":                       # cc>0  = 0<cc
            return [f"SLT   predM.x, 0, {cc};"]
        if pred == "LE":                       # cc<=0 = 0>=cc
            return [f"SGE   predM.x, 0, {cc};"]
        if pred == "LT":                       # cc<0
            return [f"SLT   predM.x, {cc}, 0;"]
        if pred == "GE":                       # cc>=0
            return [f"SGE   predM.x, {cc}, 0;"]
        if pred == "EQ":                       # cc==0 = cc>=0 & cc<=0
            return [f"SGE   predM.x, {cc}, 0;",
                    f"SGE   predD.x, 0, {cc};",
                    f"MUL   predM.x, predM.x, predD.x;"]
        if pred == "NE":                       # cc!=0 = 0
            return [f"ABS   predD.x, {cc};",
                    f"SLT   predM.x, 0, predD.x;"]
        return ["MOV   predM.x, 1;"]           # TR/FL fallback


def translate(text, fname):
    is_fp = "NV_fragment_program2" in text
    is_vp = "NV_vertex_program3" in text
    if fname in SKIP_FILES:
        return None
    if not (is_fp or is_vp):
        return None
    return Xlate(text, is_fp).run()


def main():
    import argparse, os
    ap = argparse.ArgumentParser()
    ap.add_argument("src", help="asm_lbg file or directory")
    ap.add_argument("dst", help="output file or directory")
    ap.add_argument("--all", action="store_true", help="process a directory")
    a = ap.parse_args()

    if a.all:
        os.makedirs(a.dst, exist_ok=True)
        n_ok = n_skip = n_passthrough = 0
        for fn in sorted(os.listdir(a.src)):
            if not fn.endswith(".asm_lbg"):
                continue
            text = open(os.path.join(a.src, fn), errors="replace").read()
            out = translate(text, fn)
            if out is None:
                if fn in SKIP_FILES:
                    n_skip += 1
                else:
                    n_passthrough += 1
                continue
            open(os.path.join(a.dst, fn), "w").write(out)
            n_ok += 1
        print(f"translated={n_ok} skipped(BRA)={n_skip} passthrough={n_passthrough}")
        return

    text = open(a.src, errors="replace").read()
    out = translate(text, os.path.basename(a.src))
    if out is None:
        print("not a translatable shader (or skip-listed)", file=sys.stderr)
        sys.exit(1)
    open(a.dst, "w").write(out)


if __name__ == "__main__":
    main()
