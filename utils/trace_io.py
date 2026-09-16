"""Shared parser/writer for the booksim_flowtest trace format.

Trace line format (see src/tools/booksim_flowtest.cpp):
    <id> <src> <dst> <bytes> [after=<id>[,<id>...]] [at=<ns>] [class=P|NP|Cpl]

'#' starts a comment (either a whole-line comment or trailing on a data line).
Used by gen_collective.py (writer), merge_traces.py (reader+writer), and
place_search.py (reader). Centralized here so the grammar can't drift between
independent hand-written copies.

class= is the PCIe ordering class (Posted/Non-Posted/Completion) for the
VC-range/credit-pool separation described in PCIE_MODEL_LIMITATIONS.md.
Omitted = untagged, which is the full undivided VC pool -- exactly the
pre-existing behavior, so every trace written before this field existed
still means the same thing.
"""

PCIE_CLASSES = ("P", "NP", "Cpl")


class Flow:
    __slots__ = ("id", "src", "dst", "bytes", "after", "at", "pcie_class")

    def __init__(self, id, src, dst, bytes, after=None, at=0, pcie_class=None):
        self.id = id
        self.src = src
        self.dst = dst
        self.bytes = bytes
        self.after = list(after) if after else []
        self.at = at
        self.pcie_class = pcie_class  # None, "P", "NP", or "Cpl"

    def format(self):
        parts = [str(self.id), str(self.src), str(self.dst), str(int(self.bytes))]
        if self.after:
            parts.append("after=" + ",".join(str(a) for a in sorted(set(self.after))))
        if self.at:
            parts.append("at=%d" % self.at)
        if self.pcie_class:
            parts.append("class=" + self.pcie_class)
        return " ".join(parts)


def parse_line(line):
    """Parse one trace line (comment already allowed inline). Returns a Flow,
    or None if the line is blank/comment-only."""
    hash_pos = line.find("#")
    if hash_pos != -1:
        line = line[:hash_pos]
    tok = line.split()
    if not tok:
        return None
    if len(tok) < 4:
        raise ValueError("trace line needs at least <id> <src> <dst> <bytes>: %r" % line)

    fid = int(tok[0])
    src = int(tok[1])
    dst = int(tok[2])
    nbytes = int(tok[3])
    after = []
    at = 0
    pcie_class = None
    for extra in tok[4:]:
        if extra.startswith("after="):
            after = [int(x) for x in extra[len("after="):].split(",") if x]
        elif extra.startswith("at="):
            at = int(extra[len("at="):])
        elif extra.startswith("class="):
            v = extra[len("class="):]
            if v not in PCIE_CLASSES:
                raise ValueError("unknown class %r in line %r (expected P, NP, or Cpl)" % (v, line))
            pcie_class = v
        else:
            raise ValueError("unknown trace field %r in line %r" % (extra, line))
    return Flow(fid, src, dst, nbytes, after, at, pcie_class)


def parse(path):
    """Read a trace file, return (header_lines, list[Flow])."""
    header_lines = []
    flows = []
    with open(path) as f:
        for raw in f:
            stripped = raw.strip()
            if stripped.startswith("#"):
                header_lines.append(stripped.lstrip("#").strip())
                continue
            flow = parse_line(raw)
            if flow is not None:
                flows.append(flow)
    return header_lines, flows


def write(out, flows, header):
    """Write a trace file. `header` is a single descriptive line (or list of
    lines); `flows` is an iterable of Flow, written in the given order."""
    headers = [header] if isinstance(header, str) else list(header)
    for h in headers:
        out.write("# %s\n" % h)
    out.write("# id src dst bytes [after=...] [at=ns] [class=P|NP|Cpl]\n")
    for flow in flows:
        out.write(flow.format() + "\n")
