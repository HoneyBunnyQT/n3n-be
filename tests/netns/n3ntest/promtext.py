#
# Copyright (C) Honey Bunny QT
# SPDX-License-Identifier: AGPL-3.0-only
#
"""A strict check of Prometheus' text format 0.0.4, as the daemons' metrics
(/metrics.txt) are to be: what Prometheus' own parser takes, and more of
what its linter asks for

- a family's "# HELP" and "# TYPE" at most once, before its samples;
  "# TYPE" for every family (counter or gauge here)
- the samples of a family together, none twice (the same labels)
- names and label names as the format allows, label values quoted with
  backslash, quote and line feed escaped, values that are numbers
- other lines starting with "#" are comments

    python3 -m n3ntest.promtext FILE    (from tests/netns; "-" for stdin)
"""

import math
import re
import sys

NAME = re.compile(r"[a-zA-Z_:][a-zA-Z0-9_:]*$")
LABEL = re.compile(r"[a-zA-Z_][a-zA-Z0-9_]*$")
TYPES = ("counter", "gauge", "histogram", "summary", "untyped")


def _labels(text, pos):
    """The labels from text[pos] (just after "{"): ({name: value}, the
    position after "}"); ValueError for anything malformed"""
    labels = {}
    while True:
        if text[pos:pos + 1] == "}":
            return labels, pos + 1
        m = re.match(r"([a-zA-Z_][a-zA-Z0-9_]*)=\"", text[pos:])
        if not m:
            raise ValueError("bad label at {!r}".format(text[pos:pos + 20]))
        name = m.group(1)
        pos += m.end()
        value = []
        while True:
            if pos >= len(text):
                raise ValueError("label {} not closed".format(name))
            c = text[pos]
            if c == "\\":
                esc = text[pos + 1:pos + 2]
                if esc not in ("\\", "\"", "n"):
                    raise ValueError("bad escape \\{} in label {}".format(
                        esc, name))
                value.append("\n" if esc == "n" else esc)
                pos += 2
            elif c == "\"":
                pos += 1
                break
            elif c == "\n":
                raise ValueError("line feed in label {}".format(name))
            else:
                value.append(c)
                pos += 1
        if name in labels:
            raise ValueError("label {} twice".format(name))
        labels[name] = "".join(value)
        if text[pos:pos + 1] == ",":
            pos += 1
        elif text[pos:pos + 1] != "}":
            raise ValueError("expected , or }} after label {}".format(name))


def _value(s):
    if s in ("NaN", "+Inf", "-Inf"):
        return float(s.replace("Inf", "inf"))
    v = float(s)
    if math.isinf(v) or math.isnan(v):
        raise ValueError
    return v


def check(text):
    """The problems of text, as a list of strings (empty: fine), and the
    families with their samples: {name: {"type":, "help":, "samples":
    [(labels, value)]}}"""
    problems = []
    families = {}
    done = set()        # families whose samples are over
    current = None
    seen = set()
    for no, line in enumerate(text.split("\n"), 1):
        def bad(msg):
            problems.append("line {}: {} ({!r})".format(no, msg, line[:80]))
        if not line.strip():
            continue
        if line.startswith("#"):
            parts = line.split(None, 3)
            if len(parts) >= 3 and parts[0] == "#" and \
                    parts[1] in ("HELP", "TYPE"):
                name = parts[2]
                if not NAME.match(name):
                    bad("bad metric name")
                    continue
                f = families.setdefault(name, {"type": None, "help": None,
                                               "samples": []})
                key = parts[1].lower()
                if f[key] is not None:
                    bad("second {} of {}".format(parts[1], name))
                if f["samples"]:
                    bad("{} of {} after its samples".format(parts[1], name))
                if parts[1] == "TYPE":
                    t = parts[3].strip() if len(parts) > 3 else ""
                    if t not in TYPES:
                        bad("unknown type {!r}".format(t))
                    f["type"] = t
                else:
                    f["help"] = parts[3] if len(parts) > 3 else ""
                if current and current != name:
                    done.add(current)
                current = name
            continue
        m = re.match(r"([a-zA-Z_:][a-zA-Z0-9_:]*)", line)
        if not m:
            bad("not a sample")
            continue
        name = m.group(1)
        pos = m.end()
        labels = {}
        try:
            if line[pos:pos + 1] == "{":
                labels, pos = _labels(line, pos + 1)
            rest = line[pos:].split()
            if not line[pos:pos + 1].isspace() or len(rest) not in (1, 2):
                raise ValueError("expected a value (and a time stamp)")
            value = _value(rest[0])
            if len(rest) == 2:
                int(rest[1])
        except ValueError as e:
            bad(str(e) or "bad value")
            continue
        f = families.get(name)
        if f is None or f["type"] is None:
            bad("sample of {} without # TYPE".format(name))
            f = families.setdefault(name, {"type": None, "help": None,
                                           "samples": []})
        if name in done:
            bad("samples of {} apart from the others".format(name))
        if current and current != name:
            done.add(current)
        current = name
        key = (name, tuple(sorted(labels.items())))
        if key in seen:
            bad("the same sample twice")
        seen.add(key)
        if f["type"] == "counter" and value < 0:
            bad("a negative counter")
        f["samples"].append((labels, value))
    return problems, families


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "-"
    text = sys.stdin.read() if path == "-" else open(path).read()
    problems, families = check(text)
    for p in problems:
        print(p)
    print("{} families, {} samples, {} problems".format(
        len(families), sum(len(f["samples"]) for f in families.values()),
        len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
