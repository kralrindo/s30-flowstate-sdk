"""Calls that only compile with DEV on and name nothing the S30 client knows.

    py dev_missing.py <merged vscripts> <r5apex_dx12.exe>

A name is known when a script defines it (`function Name(`) or the exe carries it as a string
(retail registers its natives by name; dev-only natives are compiled out, names included).
Prints name, count and the first sites (file:line).
"""
import collections, os, re, sys
sys.path.insert(0, os.path.dirname(__file__))

KEYWORDS = set('if while for foreach switch return function typeof thread waitthread delaythread wait '
               'array table void var int float bool string entity vector asset struct enum const '
               'local global untyped ornull functionref expect clone delete new in case catch try throw '
               'resume yield extends constructor this null assert'.split())
CALL = re.compile(r'(?<![\.\w])([A-Za-z_]\w*)\s*\(')
DEF = re.compile(r'\bfunction\s+([A-Za-z_]\w*)\s*\(')


def ev(expr, defs):
    e = expr.split('//')[0].replace('&&', ' and ').replace('||', ' or ')
    e = re.sub(r'!(?!=)', ' not ', e)
    e = re.sub(r'\b([A-Za-z_]\w*)\b', lambda m: m.group(1) if m.group(1) in ('and', 'or', 'not', 'True', 'False') else str(defs.get(m.group(1), False)), e)
    return bool(eval(e))


def active(lines, defs):
    """Set of 0-based line numbers the preprocessor keeps."""
    out = set(); stack = []
    for i, l in enumerate(lines):
        s = l.strip(' \t\x00')
        if s.startswith('#if '):
            parent = all(x[0] for x in stack); v = ev(s[4:], defs); stack.append([parent and v, v]); continue
        if s.startswith('#elseif'):
            parent = all(x[0] for x in stack[:-1]); taken = stack[-1][1]; v = (not taken) and ev(s[7:], defs); stack[-1] = [parent and v, taken or v]; continue
        if s.startswith('#else'):
            parent = all(x[0] for x in stack[:-1]); stack[-1] = [parent and not stack[-1][1], True]; continue
        if s.startswith('#endif'):
            stack.pop(); continue
        if all(x[0] for x in stack):
            out.add(i)
    return out


def code(line):
    line = re.sub(r'"(\\.|[^"\\])*"', '""', line)
    return line.split('//')[0]


def main():
    root, exe = sys.argv[1], sys.argv[2]
    files = {}
    for dp, _, fn in os.walk(root):
        for f in fn:
            if f.endswith(('.nut', '.gnut')):
                p = os.path.join(dp, f)
                files[os.path.relpath(p, root).replace('\\', '/')] = open(p, 'rb').read().decode('latin1').replace('\r', '').split('\n')
    known = set()
    for lines in files.values():
        for l in lines:
            known.update(DEF.findall(code(l)))
    known.update(m.decode() for m in re.findall(rb'(?<![\w])([A-Za-z_]\w{2,})\x00', open(exe, 'rb').read()))
    sites = collections.defaultdict(list)
    for rel, lines in sorted(files.items()):
        text = '\n'.join(lines)
        for vm in ('CLIENT', 'UI'):
            if '#if' not in text:
                dev_only = set()
            else:
                dev_only = active(lines, {vm: True, 'DEV': True}) - active(lines, {vm: True, 'DEV': False})
            for i in sorted(dev_only):
                c = code(lines[i])
                if DEF.search(c):
                    continue
                for name in CALL.findall(c):
                    if name not in KEYWORDS and name not in known:
                        site = '%s:%d' % (rel, i + 1)
                        if site not in sites[name]:
                            sites[name].append(site)
    for name, s in sorted(sites.items(), key=lambda kv: (-len(kv[1]), kv[0])):
        print('%-40s %3d  %s' % (name, len(s), ' '.join(s[:3])))
    print(len(sites), 'names')


if __name__ == '__main__':
    main()
