import os,re,sys
def ev(expr, defs):
    e=expr.split('//')[0].replace('&&',' and ').replace('||',' or ')
    e=re.sub(r'!(?!=)',' not ',e)
    e=re.sub(r'\b([A-Za-z_]\w*)\b', lambda m: m.group(1) if m.group(1) in ('and','or','not','True','False') else str(defs.get(m.group(1),False)), e)
    return bool(eval(e))
def pp(lines, defs):
    out=[]; stack=[]
    for l in lines:
        s=l.strip(' \t\x00')
        if s.startswith('#if '):
            parent=all(x[0] for x in stack); v=ev(s[4:],defs); stack.append([parent and v, v]); continue
        if s.startswith('#elseif'):
            parent=all(x[0] for x in stack[:-1]); taken=stack[-1][1]; v=(not taken) and ev(s[7:],defs); stack[-1]=[parent and v, taken or v]; continue
        if s.startswith('#else'):
            parent=all(x[0] for x in stack[:-1]); stack[-1]=[parent and not stack[-1][1], True]; continue
        if s.startswith('#endif'):
            if not stack: return None
            stack.pop(); continue
        if all(x[0] for x in stack) and s: out.append(s)
    return out if not stack else None
rd=lambda p: open(p,'rb').read().decode('latin1').replace('\r','').split('\n')
X,M=sys.argv[1],sys.argv[2]
bad=0; n=0
for vm,suf,key in (('script_client','.client','CLIENT'),('script_ui','.ui','UI')):
    root=os.path.join(X,'export',vm,'wrap','scripts','vscripts')
    for dp,dn,fn in os.walk(root):
        for f in fn:
            rel=os.path.relpath(os.path.join(dp,f),root)[:-len(suf)]
            o=rd(os.path.join(dp,f)); m=rd(os.path.join(M,rel))
            for dev in (False,True):
                defs={key:True,'DEV':dev}
                n+=1
                if pp(o,defs)!=pp(m,defs):
                    bad+=1
                    if bad<=10: print('MISMATCH',vm,rel,'DEV' if dev else '')
print('checked',n,'mismatches',bad)
