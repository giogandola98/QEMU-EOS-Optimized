#!/usr/bin/env python3
"""Compare an MPU message log from a real camera with qemu-eos' MPU spell table.

Usage: mpu_compare.py [--spells hw/eos/mpu_spells/EOSM.h] LOG [LOG ...]

LOG is either the Penumbra capture kit format (BOOTMPU.LOG / MPUnn.LOG: "<s.us> S|R <hex bytes>", S = sent to
the MPU) or the dm-spy format ("... mpu_send(<hex>)" / "... mpu_recv(<hex>)"). The logs come from the user's camera
and are kept privately; this script only reads them.

For every message the camera sends, the script looks it up in the spell table the way hw/eos/mpu.c does (first
in_spell whose bytes match, ARGn matching anything, compared over the shorter length) and prints:
  - sends the table does not answer ("unknown" in the emulator log), with how often they occur;
  - replies of the real MPU, by message class/id, that the matched spells never produce, and those whose bytes
    differ from the table's.
Replies are compared per class/id, not per send: the capture kit reads Canon's MPU rings after the fact, so the
order of sends and replies in its log is not causal. Exit status 0 always: this is a report, not a test.
"""
import argparse
import collections
import os
import re
import sys

ARG = None  # wildcard marker in parsed spells

SPELL_RE = re.compile(r"^\s*\{\s*\{\s*(0x[0-9a-fA-F]{2}[^}]*)\}\s*,(.*)$")
REPLY_RE = re.compile(r"^\s*\{\s*(0x[0-9a-fA-F]{2}[^}]*)\}\s*,")
DESC_RE = re.compile(r'\.description\s*=\s*"([^"]*)"')
INCLUDE_RE = re.compile(r'^\s*#include\s+"([^"]+)"')


def parse_bytes(text):
    out = []
    for tok in text.split(","):
        tok = tok.strip()
        if not tok:
            continue
        if tok.startswith("ARG"):
            out.append(ARG)
        elif tok.startswith("MPU_"):
            out.append(ARG)
        else:
            out.append(int(tok, 16))
    return out


def parse_spells(path, seen=None):
    """Return a list of (in_spell, description, [replies]) from a spell header, following #includes."""
    seen = seen if seen is not None else set()
    path = os.path.normpath(path)
    if path in seen:
        return []
    seen.add(path)
    spells = []
    cur = None
    for line in open(path, errors="replace"):
        if line.lstrip().startswith("//"):
            continue
        m = INCLUDE_RE.match(line)
        if m:
            spells += parse_spells(os.path.join(os.path.dirname(path), m.group(1)), seen)
            continue
        m = SPELL_RE.match(line)
        if m:
            d = DESC_RE.search(m.group(2))
            cur = (parse_bytes(m.group(1)), d.group(1) if d else "", [])
            spells.append(cur)
            continue
        m = REPLY_RE.match(line)
        if m and cur is not None:
            cur[2].append(parse_bytes(m.group(1)))
    return spells


def match(received, spell):
    """As match_spell() in hw/eos/mpu.c: bytes 1 .. min(length bytes) compared, the C array zero-padded."""
    n = min(received[1], spell[1]) + 1
    sp = spell + [0] * max(0, n - len(spell))
    rc = received + [0] * max(0, n - len(received))
    return all(sp[i] is ARG or sp[i] == rc[i] for i in range(1, n))


def fmt(msg):
    return " ".join("xx" if b is ARG else "%02x" % b for b in msg)


def read_log(path):
    """Yield (time_s, 'S'|'R', [bytes])."""
    for line in open(path, errors="replace"):
        m = re.match(r"^\s*([0-9]+\.[0-9]+)\s+([SR])\s+([0-9a-fA-F ]+)$", line)
        if m:
            yield float(m.group(1)), m.group(2), [int(x, 16) for x in m.group(3).split()]
            continue
        m = re.match(r"^\s*([0-9.]+).*mpu_(send|recv)\(([0-9a-fA-F ]+)\)", line)
        if m:
            yield float(m.group(1)), "S" if m.group(2) == "send" else "R", [int(x, 16) for x in m.group(3).split()]


def reply_equal(real, table, sent):
    if len(real) != len(table):
        return False
    for i, (a, b) in enumerate(zip(real, table)):
        if b is ARG:
            continue  # ARGn copies a byte of the send; mpu.c substitutes it
        if a != b:
            return False
    return True


def norm(msg):
    """Byte 1 is the message length after byte 0; the kit logs the whole transfer buffer (byte 0 bytes, padded)
    and the tables are not consistent about the padding, so compare msg[0 .. msg[1]] only."""
    return msg[:msg[1] + 1] if len(msg) > 2 and msg[1] + 1 <= len(msg) else msg


def pid(msg):
    return "%02x %02x" % (msg[2], msg[3]) if len(msg) > 3 else fmt(msg)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--spells", default=os.path.join(here, "../../../mpu_spells/EOSM.h"))
    ap.add_argument("--max", type=int, default=40, help="max lines per section")
    ap.add_argument("logs", nargs="+")
    a = ap.parse_args()

    spells = parse_spells(a.spells)
    print("spell table: %s, %d spells (with includes)" % (os.path.basename(a.spells), len(spells)))

    unknown = collections.OrderedDict()      # class/id -> [count, example]
    used = []                                # spells matched by a real send, in order of first use
    real = collections.OrderedDict()         # class/id -> Counter(payload)
    nsend = nrecv = 0
    for log in a.logs:
        for t, d, msg in read_log(log):
            msg = norm(msg)
            if d == "R":
                nrecv += 1
                real.setdefault(pid(msg), collections.Counter())[fmt(msg)] += 1
                continue
            nsend += 1
            hit = next((s for s in spells if match(msg, s[0])), None)
            if hit is None:
                u = unknown.setdefault(pid(msg), [0, fmt(msg), "%s %.3f" % (os.path.basename(log), t)])
                u[0] += 1
            elif hit not in used:
                used.append(hit)

    table = collections.OrderedDict()        # class/id -> set(payload) the table emits for the sends seen
    for s in used:
        for r in s[2]:
            table.setdefault(pid(r), set()).add(fmt(norm(r)))

    print("messages: %d sent, %d received; %d table spells used, %d sends unanswered by the table"
          % (nsend, nrecv, len(used), sum(u[0] for u in unknown.values())))

    print("\n== sends the table does not answer, by class/id (count, first example)")
    for k, (c, ex, where) in list(unknown.items())[:a.max]:
        print("%5dx  %s   %s   [%s]" % (c, k, ex[:110], where))

    print("\n== replies the real MPU sent that the table never emits for these sends, by class/id")
    for k, cnt in real.items():
        if k not in table:
            (p, c), = cnt.most_common(1)
            print("%5dx  %s   %d variant(s), most frequent: %s" % (sum(cnt.values()), k, len(cnt), p[:110]))

    print("\n== replies present in both, with different bytes")
    for k, cnt in real.items():
        if k in table:
            rset = set(cnt)
            tset = table[k]
            if not any(reply_equal(bytes_of(r), bytes_of(t), None) for r in rset for t in tset):
                print("-- %s" % k)
                for r in list(rset)[:3]:
                    print("   real : %s" % r[:150])
                for t in list(tset)[:3]:
                    print("   table: %s" % t[:150])

    print("\n== table replies the real MPU never sent (class/id)")
    print("   " + ", ".join(k for k in table if k not in real))
    return 0


def bytes_of(text):
    return [ARG if b == "xx" else int(b, 16) for b in text.split()]


if __name__ == "__main__":
    sys.exit(main())
