#!/usr/bin/env python3
"""
EVONIX Kernel V4.6.1 Regression Suite:
Static verification of SELinux prototype ABI and ColorOS stability guards.
"""

import sys
import os
import re

def verify_selinux_header(tree_root):
    header_path = os.path.join(tree_root, "security/selinux/include/security.h")
    if not os.path.exists(header_path):
        return True # Not present or normal branch
    with open(header_path, "r") as f:
        content = f.read()
    if "security_context_to_sid_with_policy" not in content:
        return True
    
    # Must declare 8 arguments including orig_sid_p and orig_rc_p
    proto_re = re.compile(
        r"int\s+security_context_to_sid_with_policy\s*\([^;]+\);",
        re.MULTILINE | re.DOTALL
    )
    match = proto_re.search(content)
    if not match:
        print(f"[FAIL] {header_path}: prototype not found")
        return False
    proto = match.group(0)
    if "orig_sid_p" not in proto or "orig_rc_p" not in proto:
        print(f"[FAIL] {header_path}: prototype missing orig_sid_p / orig_rc_p:\n{proto}")
        return False
    print(f"[PASS] {header_path}: authoritative 8-arg prototype present")
    return True

def extract_call_args(text, func_name):
    idx = 0
    calls = []
    while True:
        pos = text.find(func_name, idx)
        if pos == -1:
            break
        # find opening '('
        open_paren = text.find('(', pos)
        if open_paren == -1:
            break
        # trace matching closing paren
        depth = 0
        arg_str = ""
        end_paren = -1
        for i in range(open_paren, len(text)):
            c = text[i]
            if c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
                if depth == 0:
                    end_paren = i
                    break
        if end_paren != -1:
            raw_args = text[open_paren+1:end_paren]
            # split args by comma at depth 0
            d = 0
            cur = ""
            args = []
            for c in raw_args:
                if c in "({[":
                    d += 1
                    cur += c
                elif c in ")}]":
                    d -= 1
                    cur += c
                elif c == ',' and d == 0:
                    args.append(cur.strip())
                    cur = ""
                else:
                    cur += c
            if cur.strip():
                args.append(cur.strip())
            calls.append(args)
            idx = end_paren + 1
        else:
            break
    return calls

def verify_selinux_callsites(tree_root):
    files = [
        "security/selinux/selinuxfs.c",
        "security/selinux/hooks.c"
    ]
    all_ok = True
    for rel_path in files:
        full_path = os.path.join(tree_root, rel_path)
        if not os.path.exists(full_path):
            continue
        with open(full_path, "r") as f:
            content = f.read()
        calls = extract_call_args(content, "security_context_to_sid_with_policy")
        # Exclude definition/declaration if any
        # Call sites have backup_sepolicy as arg 0
        for args in calls:
            if len(args) == 0:
                continue
            if args[0] in ["struct selinux_policy *policy", "policy"]:
                continue # definition or declaration
            if len(args) != 8:
                print(f"[FAIL] {full_path}: call site has {len(args)} arguments instead of 8: {args}")
                all_ok = False
            else:
                if args[-2] != "NULL" or args[-1] != "NULL":
                    print(f"[WARN] {full_path}: last 2 args are not NULL, NULL: {args[-2]}, {args[-1]}")
    if all_ok:
        print(f"[PASS] {tree_root}: All SELinux call sites pass exactly 8 arguments")
    return all_ok

def verify_adios_recovery(tree_root):
    adios_path = os.path.join(tree_root, "block/adios.c")
    if not os.path.exists(adios_path):
        return True
    with open(adios_path, "r") as f:
        content = f.read()
    if "Fallback recovery: if dispatch_from_pq yielded due to anti-starvation quota" not in content:
        print(f"[FAIL] {adios_path}: missing anti-starvation deadlock recovery")
        return False
    if "ad->pq1_dispatch_count = 0;" not in content:
        print(f"[FAIL] {adios_path}: missing pq1_dispatch_count reset in fallback")
        return False
    print(f"[PASS] {adios_path}: ADIOS anti-starvation deadlock recovery verified")
    return True

def verify_cpufreq_guard(tree_root):
    cpufreq_path = os.path.join(tree_root, "drivers/cpufreq/cpufreq.c")
    if not os.path.exists(cpufreq_path):
        return True
    with open(cpufreq_path, "r") as f:
        content = f.read()
    if "#ifndef CONFIG_OPLUS_FEATURE_SCHED_ASSIST" not in content:
        print(f"[FAIL] {cpufreq_path}: missing CONFIG_OPLUS_FEATURE_SCHED_ASSIST guard")
        return False
    print(f"[PASS] {cpufreq_path}: SchedAssist governor guard verified")
    return True

def main():
    tree_root = sys.argv[1] if len(sys.argv) > 1 else "."
    print(f"Auditing tree: {tree_root}")
    ok1 = verify_selinux_header(tree_root)
    ok2 = verify_selinux_callsites(tree_root)
    ok3 = verify_adios_recovery(tree_root)
    ok4 = verify_cpufreq_guard(tree_root)
    if ok1 and ok2 and ok3 and ok4:
        print("ALL REGRESSION CHECKS PASSED.")
        sys.exit(0)
    else:
        print("REGRESSION CHECKS FAILED.")
        sys.exit(1)

if __name__ == "__main__":
    main()
