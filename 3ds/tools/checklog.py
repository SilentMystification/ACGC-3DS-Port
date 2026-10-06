"""checklog.py - check a game log for the instrumentation and print one summary.

Usage: py -3 3ds/tools/checklog.py <log> [<second log>]
       py -3 3ds/tools/checklog.py --selftest
<log> is any game log: Azahar (build3ds/last_log.txt), Wi-Fi (build3ds/hw_live_log.txt) or the
SD log pulled by FTP. With a second log, it also checks that both have the same numbered lines
(for example the SD log and the Wi-Fi log of the same run).
Exit code 1 when an expected tag is missing, a sequence gap exists, or the two logs differ.
"""
import re
import sys

EXPECTED = ["[BOOT]", "[PERF]", "[PROFILE]", "[CORE]", "[FPSCR]", "[APT]", "[TEX]", "[WORK]", "[CALLS]"]
PROBLEMS = ["[CRASH]", "[HANG]", "[WATCHDOG]", "[LOCK]", "[LOG] dropped", "[livelog] GAP", "[STUTTER]"]
SEQ = re.compile(r"^#(\d+) ")


def read_lines(path):
    with open(path, "rb") as f:
        return f.read().decode("utf-8", "replace").replace("\r", "").split("\n")


def numbered(lines):
    """{seq: text} for lines with a "#seq " prefix"""
    out = {}
    for line in lines:
        m = SEQ.match(line)
        if m:
            out[int(m.group(1))] = line[m.end():]
    return out


def check(lines, name, report=print):
    ok = True
    text = [SEQ.sub("", l) for l in lines]
    report("== %s: %d lines" % (name, len(lines)))

    missing = [t for t in EXPECTED if not any(t in l for l in text)]
    report("tags missing: " + (" ".join(missing) if missing else "none"))
    ok &= not missing

    seqs = sorted(numbered(lines))
    gaps = [(a + 1, b - 1) for a, b in zip(seqs, seqs[1:]) if b != a + 1]
    if seqs:
        report("sequence: #%d-#%d, %d gaps%s" % (seqs[0], seqs[-1], len(gaps),
               "" if not gaps else " (first: #%d-#%d)" % gaps[0]))
    else:
        report("sequence: no numbered lines")
    ok &= not gaps

    for p in PROBLEMS:
        hits = [l for l in text if p in l]
        if hits:
            report("%s x%d, first: %s" % (p, len(hits), hits[0][:150]))

    def show(tag, limit=None):
        hits = [l for l in text if l.startswith(tag)]
        for l in hits[-limit:] if limit else hits:
            report("  " + l[:220])

    report("-- boot phases"); show("[BOOT]")
    report("-- cores, limits, FPU"); show("[CORE]"); show("[APT]"); show("[FPSCR]")
    report("-- last frame stats"); show("[PERF]", 2); show("[WORK]", 1); show("[TEX]", 1)
    report("-- last profile"); show("[PROFILE]", 5)
    report("-- slow calls"); show("[CALLS]", 3)
    return ok


def compare(a, b, report=print):
    na, nb = numbered(a), numbered(b)
    common = set(na) & set(nb)
    diff = [s for s in sorted(common) if na[s] != nb[s]]
    report("== compare: %d numbered lines in both, %d differ, only in first %d, only in second %d"
           % (len(common), len(diff), len(set(na) - set(nb)), len(set(nb) - set(na))))
    if diff:
        report("first difference #%d:\n  %s\n  %s" % (diff[0], na[diff[0]][:150], nb[diff[0]][:150]))
    return not diff and bool(common)


def selftest():
    good = ["#%d %s" % (i, l) for i, l in enumerate(
        ["[BOOT] t=10ms citro3d up", "[CORE] game thread on core 0", "[APT] core-1 limit 79%",
         "[FPSCR] 03c00000", "[PERF] frame 60", "[PROFILE] timers", "[TEX] uploads 3", "[WORK] vtx 1",
         "[CALLS] uidiv 5"])]
    sink = []
    assert check(good, "good", sink.append), sink
    gap = good[:3] + good[4:]
    assert not check(gap, "gap", sink.append)
    assert not check(good[1:], "no boot", sink.append) and any("[BOOT]" in s for s in sink)
    assert compare(good, good, sink.append)
    other = list(good)
    other[2] = "#2 [APT] core-1 limit 30%"
    assert not compare(good, other, sink.append)
    print("checklog selftest: ok")


if __name__ == "__main__":
    if sys.argv[1:] == ["--selftest"]:
        selftest()
        sys.exit(0)
    a = read_lines(sys.argv[1])
    ok = check(a, sys.argv[1])
    if len(sys.argv) > 2:
        b = read_lines(sys.argv[2])
        ok &= check(b, sys.argv[2])
        ok &= compare(a, b)
    sys.exit(0 if ok else 1)
