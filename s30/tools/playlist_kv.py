"""Minimal reader for Respawn playlist KeyValues text, with the S30.1 parser's semantics.

S30.1 RecursiveLoadFromBuffer (0x5DFA10): when a '{' follows a key/value pair, the
node is renamed "key value" and keeps the value ("PlaylistSchedule ranked { ... }" ->
node "PlaylistSchedule ranked", string value "ranked"). Comments are // to end of line.
Conditionals ([$X]) are kept as text on the node.

Node: dict(name, value, children (list of Node) or None, start, end) where start/end are
character offsets of the node's text in the source (end exclusive, including the block).
"""
import re

_TOKEN = re.compile(r'\s+|//[^\n]*|"((?:[^"\\]|\\.)*)"|(\{|\})|(\[[^\]]*\])|([^\s{}"]+)')


def tokenize(text):
    out = []
    pos = 0
    n = len(text)
    while pos < n:
        m = _TOKEN.match(text, pos)
        if not m:
            raise ValueError("bad token at %d: %r" % (pos, text[pos:pos + 40]))
        if m.group(1) is not None:
            out.append(("str", m.group(1), m.start(), m.end()))
        elif m.group(2) is not None:
            out.append((m.group(2), m.group(2), m.start(), m.end()))
        elif m.group(3) is not None:
            out.append(("cond", m.group(3), m.start(), m.end()))
        elif m.group(4) is not None:
            out.append(("str", m.group(4), m.start(), m.end()))
        pos = m.end()
    return out


class Parser:
    def __init__(self, text):
        self.text = text
        self.toks = tokenize(text)
        self.i = 0

    def peek(self):
        return self.toks[self.i] if self.i < len(self.toks) else None

    def take(self):
        t = self.peek()
        self.i += 1
        return t

    def block(self):
        """Children until '}' or EOF."""
        nodes = []
        while True:
            t = self.peek()
            if t is None:
                return nodes, None
            if t[0] == "}":
                self.take()
                return nodes, t[3]
            k = self.take()
            if k[0] != "str":
                raise ValueError("expected key at %d, got %r" % (k[2], k[1]))
            node = {"name": k[1], "value": None, "children": None, "start": k[2], "end": k[3], "cond": None}
            t = self.peek()
            if t and t[0] == "cond":
                node["cond"] = self.take()[1]
                t = self.peek()
            if t and t[0] == "{":
                self.take()
                node["children"], node["end"] = self.block()
            elif t and t[0] == "str":
                v = self.take()
                node["value"] = v[1]
                node["end"] = v[3]
                t = self.peek()
                if t and t[0] == "cond":
                    node["cond"] = self.take()[1]
                    t = self.peek()
                if t and t[0] == "{":  # S30: "key value {" -> node "key value" with value
                    self.take()
                    node["name"] = "%s %s" % (k[1], v[1])
                    node["children"], node["end"] = self.block()
            else:
                raise ValueError("no value for key %r at %d" % (k[1], k[2]))
            nodes.append(node)


def parse(text):
    p = Parser(text.split("\x00", 1)[0])  # the rpak wrap text ends with its NUL
    nodes, _ = p.block()
    return nodes


def find(nodes, name):
    for n in nodes or []:
        if n["name"].lower() == name.lower():
            return n
    return None
