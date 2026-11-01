#!/usr/bin/env python3
"""What CeeLing did for app-cling, in numbers: app-cling's own scripts/ceeling_stats.sh run on C++ generated now, and scripts/shim_lines.sh.

Compiles the modules exactly as 16_app_cling_pointers.py does (it is imported from the file next to this one), then runs the two scripts of the sibling project
app-cling. Exit status 77 and a message on stderr if app-cling, cjson-cling, the compiler or bash is missing.
"""
import importlib.util
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('app_cling_pointers', os.path.join(HERE, '16_app_cling_pointers.py'))
pointers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pointers)


def main():
    with tempfile.TemporaryDirectory() as tmp:
        info = pointers.prepare(tmp)
        work = pointers.posix(info['final'])
        app = pointers.posix(info['app'])
        out = []
        for script, args in (('ceeling_stats.sh', [work]), ('shim_lines.sh', [])):
            r = subprocess.run([info['bash'], app + '/scripts/' + script] + args, capture_output=True, cwd=info['app'])
            text = (r.stdout + r.stderr).decode('utf-8', 'replace').replace('\r\n', '\n')
            if r.returncode != 0 and script == 'ceeling_stats.sh':
                sys.stderr.write(text)
                sys.exit(1)
            out.append('$ scripts/%s%s' % (script, ' <work directory>' if args else ''))
            out.append(text.replace(work, '<work directory>').rstrip())
        print('\n'.join(out))


if __name__ == '__main__':
    main()
