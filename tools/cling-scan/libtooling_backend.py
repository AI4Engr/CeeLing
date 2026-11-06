"""LibTooling backend of cling-scan: runs `cling-scan-extract` (tools/cling-scan/libtooling/, a C++ program on clang's AST) and reads its records.

Unlike the libclang C API, LibTooling sees the instantiated classes of a template, so this is the backend that reads `--type` (Eigen::VectorXd...)
from the compiler's own AST instead of the text dump. The analysis (fresh / borrow / mutator / function) stays in cling_scan.py, shared by all backends.

The extractor is built by tools/cling-scan/libtooling/build.sh against the LLVM/Clang development packages. It is found through `--extractor`,
CLING_SCAN_EXTRACT, or next to build.sh. On Windows the extractor is a Linux program: it is run through WSL (the paths are converted).
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def find_extractor(explicit=None):
    for c in (explicit, os.environ.get('CLING_SCAN_EXTRACT'), os.path.join(HERE, 'libtooling', 'cling-scan-extract'), '/tmp/cling-scan-extract'):
        if c and os.path.exists(c):
            return c
    return None


def available(explicit=None):
    return find_extractor(explicit) is not None


def _wsl_path(p):
    p = os.path.abspath(p).replace(os.sep, '/')
    return '/mnt/' + p[0].lower() + p[2:] if len(p) > 1 and p[1] == ':' else p


def _from_wsl(p):
    """/mnt/c/dir/file -> C:/dir/file (the extractor reports the paths it saw, which are WSL paths)."""
    if p.startswith('/mnt/') and len(p) > 6 and p[6] == '/':
        return p[5].upper() + ':' + p[6:]
    return p


def extract(header, cls, typ, std, includes, extractor=None, extra=()):
    exe = find_extractor(extractor)
    if exe is None:
        raise RuntimeError('cling-scan-extract was not found (build it with tools/cling-scan/libtooling/build.sh, or set --extractor / CLING_SCAN_EXTRACT)')
    windows = sys.platform == 'win32'
    conv = _wsl_path if windows else (lambda p: p)
    cmd = ['wsl', '-d', os.environ.get('WSL_DISTRO', 'Ubuntu-22.04'), '--', conv(exe)] if windows else [exe]
    cmd += ['--header', conv(header), '-std', std]
    cmd += ['--type', typ] if typ else ['--class', cls]
    for i in includes:
        cmd += ['-I', conv(i)]
    for a in extra:
        cmd += ['--clang-arg', a]
    env = dict(os.environ, MSYS_NO_PATHCONV='1')
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace', env=env)
    if r.returncode != 0:
        sys.exit('cling-scan-extract failed:\n' + '\n'.join(l for l in (r.stderr or r.stdout).splitlines() if 'systemd user session' not in l)[:2000])
    methods, skipped, enums, deps = {}, [], set(), []
    name, count, version = typ or cls, 1, ''
    for line in r.stdout.splitlines():
        f = line.split('\t')
        if f[0] == 'version':
            version = f[1]
        elif f[0] == 'class':
            name, count = f[1], int(f[2])
        elif f[0] == 'dep':
            deps.append(os.path.normpath(os.path.abspath(_from_wsl(f[1]) if windows else f[1])))
        elif f[0] == 'enum':
            enums.add(f[1])
        elif f[0] == 'skipped':
            skipped.append(f[1])
        elif f[0] == 'method':
            n = int(f[5])
            methods.setdefault(f[1], []).append(dict(ret=f[4], params=f[6:6 + n], const=f[3] == '1', static=f[2] == '1'))
    entry = os.path.normpath(os.path.abspath(header))
    if entry not in deps:
        deps.append(entry)
    return dict(cls=name, methods=methods, skipped=skipped, enums=enums, class_count=count, dependencies=sorted(set(deps)), version=version)
