#!/usr/bin/env python3
"""Report the effective length of each variable in a U-Boot env text file.

The env text format joins backslash-continued lines into ONE value, and
cli_simple_run_command() refuses to run any command whose string is not
strictly shorter than CONFIG_SYS_CBSIZE:

    if (strlen(cmd) >= CONFIG_SYS_CBSIZE) {
            puts("## Command too long!\\n");
            return -1;
    }

so a bootcmd a few hundred bytes over the limit fails *completely* - nothing
in it runs, no partition lookup, no sysboot - and the only evidence on the
panel is that one line. Check it here instead.

Usage: envlen.py <env file> [limit]
"""
import sys

path = sys.argv[1]
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 2048

variables = {}
name = None
parts = []

for raw in open(path, encoding="utf-8"):
    line = raw.rstrip("\n")
    if name is None:
        stripped = line.strip()
        if not stripped or stripped.startswith("#") or "=" not in line:
            continue
        name, value = line.split("=", 1)
        parts = [value]
    else:
        parts.append(line)

    # a trailing backslash means the value continues on the next line, and the
    # backslash itself is consumed
    if parts[-1].endswith("\\"):
        parts[-1] = parts[-1][:-1]
        continue

    variables[name] = "".join(parts)
    name = None

rc = 0
for var in sorted(variables):
    value = variables[var]
    n = len(value)
    note = ""
    if n >= limit:
        note = "   <-- TOO LONG: cli_simple_run_command() will refuse this"
        rc = 1
    elif n > limit * 0.75:
        note = "   <-- close to the limit"
    print("%-24s %6d bytes%s" % (var, n, note))

print("(limit: CONFIG_SYS_CBSIZE = %d)" % limit)
sys.exit(rc)
