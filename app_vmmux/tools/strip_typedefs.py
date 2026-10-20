#!/usr/bin/env python3
"""Remove the per-chunk copies of the shared typedefs from the Stage 4 headers and
include vm_types.h instead (PLATFORM.md section 6, rules 1-3). One-off migration aid,
kept so the change can be re-run on a fresh copy of the Stage 4 headers."""
import re, sys
SHARED = {"ul_timer_t", "rtc_date_t", "rtc_time_t", "Schedule_t", "Sequences_t",
          "ScheduleMUX_t", "LoggingTableStruct_t"}
for path in sys.argv[1:]:
    s = open(path).read()
    out, i, removed = [], 0, []
    for m in re.finditer(r"typedef\s+struct\s*\{", s):
        if m.start() < i: continue
        depth, j = 0, m.end() - 1
        while True:
            if s[j] == "{": depth += 1
            elif s[j] == "}":
                depth -= 1
                if depth == 0: break
            j += 1
        k = s.index(";", j)
        name = s[j+1:k].strip()
        if name in SHARED:
            end = k + 1
            # drop a trailing comment on the same line and the newline
            nl = s.find("\n", end)
            tail = s[end:nl]
            if tail.strip().startswith("/*") and tail.strip().endswith("*/"): end = nl
            if s[end:end+1] == "\n": end += 1
            # drop leading indentation
            st = m.start()
            while st > 0 and s[st-1] in " \t": st -= 1
            out.append(s[i:st]); i = end; removed.append(name)
    out.append(s[i:])
    s = "".join(out)
    if removed and '#include "vm_types.h"' not in s:
        s = re.sub(r"(#define STAGE\w+\n)", r'\1#include "vm_types.h"   /* shared types and system tags (app_vmmux) */\n', s, count=1)
    open(path, "w").write(s)
    print(path, "removed:", ", ".join(removed) or "-")
