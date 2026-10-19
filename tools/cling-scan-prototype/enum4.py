import ijson, collections, re, json
want = {"MatrixBase", "DenseBase", "PlainObjectBase", "DenseCoeffsBase", "EigenBase"}
def targ_types(spec):
    return [c.get("type", {}).get("qualType", "") for c in spec.get("inner", []) if c.get("kind") == "TemplateArgument"]
out = {}
kept = collections.Counter(); dropped = collections.Counter()
with open("full.json", "rb") as f:
    for item in ijson.items(f, "inner.item.inner.item", use_float=True):
        if item.get("kind") != "ClassTemplateDecl" or item.get("name") not in want: continue
        for s in item.get("inner", []):
            if s.get("kind") != "ClassTemplateSpecializationDecl": continue
            ta = targ_types(s)
            # keep only the specialization whose first template arg is Matrix<double,-1,-1> (the receiver type)
            if not ta or not re.match(r'^Eigen::Matrix<double, -1, -1(, 0, -1, -1)?>$', ta[0].strip()):
                dropped[s["name"]] += 1; continue
            kept[s["name"]] += 1
            for c in s.get("inner", []):
                k = c.get("kind"); nm = c.get("name", "")
                if not nm or nm.startswith("~") or c.get("isImplicit"): continue
                mem = c
                tmpl = []
                if k == "FunctionTemplateDecl":
                    ms = [x for x in c.get("inner", []) if x.get("kind") == "CXXMethodDecl"]
                    if not ms: continue
                    mem = ms[0]; tmpl = [x.get("name") for x in c["inner"] if x.get("kind") in ("TemplateTypeParmDecl", "NonTypeTemplateParmDecl")]
                elif k != "CXXMethodDecl": continue
                params = [p.get("type", {}).get("qualType", "") for p in mem.get("inner", []) if p.get("kind") == "ParmVarDecl"]
                qt = (mem.get("type") or {}).get("qualType", "")
                const = bool(re.search(r'\)\s*const\b', qt))
                key = (nm, tuple(params), const, mem.get("storageClass") == "static", bool(tmpl))
                out[key] = dict(name=nm, params=params, const=const, static=key[3], templ=tmpl, cls=s["name"], decl_ret=qt)
print("specializations kept:", dict(kept), "| dropped (other receiver types):", dict(dropped))
print("distinct (name, params, const, static, templ) probes:", len(out), "| distinct names:", len({v['name'] for v in out.values()}))
json.dump(list(out.values()), open("probes.json", "w"))
