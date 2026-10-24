#!/usr/bin/env python3
"""Fails unless every BLE_API function in include/IBleServer.h has a matching [DllImport] in
unity/BleServer.cs (same name, same number of parameters, compatible parameter kinds), and no
[DllImport] exists without a header function. Also checks that the native callback delegates are
static [MonoPInvokeCallback] thunks held in static fields (no transient delegates)."""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
header = (ROOT / "include" / "IBleServer.h").read_text()
cs = (ROOT / "unity" / "BleServer.cs").read_text()


def split_params(text):
    text = text.strip()
    if not text or text == "void":
        return []
    return [p.strip() for p in text.split(",")]


def kind_c(param):
    p = re.sub(r"\s+", " ", param)
    if "*" in p:
        if "char*" in p.replace(" ", "") and "const" in p and "uint8" not in p:
            return "string"
        if "Fn" in p:
            return "callback"
        return "pointer"
    if "Fn " in p + " " and p.split()[0].endswith("Fn"):
        return "callback"
    if p.startswith("bool"):
        return "bool"
    if p.startswith(("int", "uint32_t")):
        return "int"
    return "?"


def kind_cs(param):
    if "LPUTF8Str" in param:
        return "string"
    p = re.sub(r"\[\w+\([^\]]*\)\]\s*", "", param).strip()  # drop [MarshalAs(...)] attributes
    if p.startswith("bool"):
        return "bool"
    if re.match(r"(ref\s+)?(Native)?\w*Config\b", p) or p.startswith("ref "):
        return "pointer"
    if p.startswith(("IntPtr", "byte[]")):
        return "pointer"
    if p.startswith(("int", "uint")):
        return "int"
    if re.match(r"\w*Callback\w*\s", p):
        return "callback"
    return "?"


c_funcs = {}
for m in re.finditer(r"BLE_API\s+[\w\s\*]+?\b((?:BleServer|BleChunker)_\w+)\s*\(([^)]*)\)\s*;", header):
    c_funcs[m.group(1)] = split_params(m.group(2))

cs_funcs = {}
for m in re.finditer(r"\[DllImport[^\]]*\]\s*(?:\[return:[^\]]*\]\s*)?public static extern\s+[\w\[\]]+\s+(\w+)\s*\(((?:[^()]|\([^()]*\))*)\)\s*;", cs):
    cs_funcs[m.group(1)] = split_params(re.sub(r"\s+", " ", m.group(2)))

errors = []
for name in sorted(set(c_funcs) - set(cs_funcs)):
    errors.append(f"missing [DllImport] for {name}")
for name in sorted(set(cs_funcs) - set(c_funcs)):
    errors.append(f"[DllImport] {name} has no BLE_API function in IBleServer.h")
for name in sorted(set(c_funcs) & set(cs_funcs)):
    c, s = c_funcs[name], cs_funcs[name]
    if len(c) != len(s):
        errors.append(f"{name}: header has {len(c)} parameters, C# has {len(s)}")
        continue
    for i, (cp, sp) in enumerate(zip(c, s)):
        kc, ks = kind_c(cp), kind_cs(sp)
        # A callback is a function pointer in C; the Non-Ex variants are IntPtr (unused) in C#.
        if kc == "callback" and ks in ("callback", "pointer"):
            continue
        if kc == "pointer" and ks == "pointer":
            continue
        if kc != ks:
            errors.append(f"{name}: parameter {i + 1} is '{cp}' in C but '{sp}' in C#")

# Delegates handed to native code must be static [MonoPInvokeCallback] thunks in static fields.
thunks = re.findall(r"private static readonly (\w+) \w+Thunk = (\w+);", cs)
if len(thunks) < 4:
    errors.append("expected static readonly *Thunk delegate fields for connection/error/write/read callbacks")
for _, method in thunks:
    if not re.search(r"\[MonoPInvokeCallback\(typeof\(\w+\)\)\]\s*private static \w+ " + method + r"\(", cs):
        errors.append(f"callback {method} is not a static [MonoPInvokeCallback] method")

if errors:
    print("check_abi_parity: FAIL", file=sys.stderr)
    for e in errors:
        print("  " + e, file=sys.stderr)
    sys.exit(1)
print(f"check_abi_parity: OK ({len(c_funcs)} functions, {len(thunks)} callback thunks)")
