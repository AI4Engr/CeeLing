"""libclang backend of cling-scan: reads the declarations through clang's C API (ctypes) instead of parsing `-ast-dump` text.

The C API is clang's stable interface (it keeps its ABI across releases), so this does not break when the text dump changes its layout. It needs
only the libclang shared library (libclang.so.1 / libclang-NN.so / libclang.dll), no LLVM development headers. Not LibTooling: it sees what
the C API shows (declarations, canonical types, access, const/static), which is all the signature-level scan reads.

LIMIT, found by trying it on Eigen: the C API does not enumerate the members of a class template INSTANTIATION (`clang_visitChildren` on
`Eigen::Matrix<double,-1,1>` yields only its template arguments, even after an explicit instantiation), nor its base specifiers. So this backend
reads non-template classes (`--class`); `--type` stays on the text backend.

extract() returns the same data the text backend builds: the methods of the class as canonical type spellings, the enum names, what was skipped, the non-system headers the translation unit includes, and clang's version.
"""
import ctypes
import glob
import os
import sys
import tempfile
from ctypes import (CDLL, CFUNCTYPE, POINTER, Structure, c_char_p, c_int, c_uint, c_void_p, byref)


class CXString(Structure):
    _fields_ = [('data', c_void_p), ('flags', c_uint)]


class CXCursor(Structure):
    _fields_ = [('kind', c_int), ('xdata', c_int), ('data', c_void_p * 3)]


class CXType(Structure):
    _fields_ = [('kind', c_int), ('data', c_void_p * 2)]


class CXSourceLocation(Structure):
    _fields_ = [('ptr_data', c_void_p * 2), ('int_data', c_uint)]


# CXCursorKind
STRUCT_DECL, CLASS_DECL, ENUM_DECL = 2, 4, 5
TYPEDEF_DECL, CXX_METHOD, NAMESPACE, CONSTRUCTOR, DESTRUCTOR, CONVERSION = 20, 21, 22, 24, 25, 26
FUNCTION_TEMPLATE, CLASS_TEMPLATE, CXX_BASE_SPECIFIER, VAR_DECL, TYPE_ALIAS_DECL = 30, 31, 44, 9, 36
# CXTypeKind
TYPE_LVALUE_REF, TYPE_RVALUE_REF = 103, 104
# CX_CXXAccessSpecifier
ACCESS_PUBLIC = 1
CHILD_CONTINUE = 1

_VISITOR = CFUNCTYPE(c_int, CXCursor, CXCursor, c_void_p)
_INCLUDE_VISITOR = CFUNCTYPE(None, c_void_p, POINTER(CXSourceLocation), c_uint, c_void_p)


def find_library(explicit=None):
    candidates = [explicit] if explicit else []
    candidates += [os.environ.get('CLING_SCAN_LIBCLANG')]
    for pattern in ('/usr/lib/llvm-*/lib/libclang.so*', '/usr/lib/x86_64-linux-gnu/libclang*.so*', '/usr/lib/libclang*.so*',
                    '/usr/local/lib/libclang*.so*', '/opt/homebrew/opt/llvm/lib/libclang.dylib', '/usr/local/opt/llvm/lib/libclang.dylib',
                    'C:/Program Files/LLVM/bin/libclang.dll'):
        candidates += sorted(glob.glob(pattern), reverse=True)
    for c in candidates:
        if c and os.path.exists(c):
            try:
                return CDLL(c)
            except OSError:
                continue
    return None


def available(explicit=None):
    return find_library(explicit) is not None


def builtin_header_dirs():
    """libclang finds gcc's include directory by itself but needs its OWN builtin headers (stddef.h...) from the `clang` resource directory,
    which only the libclang-common-dev package installs. When that is missing, fall back to gcc's directory, which has the same files."""
    if glob.glob('/usr/lib/llvm-*/lib/clang/*/include/stddef.h') or glob.glob('/usr/lib/clang/*/include/stddef.h'):
        return []
    found = sorted(glob.glob('/usr/lib/gcc/*/*/include/stddef.h'), reverse=True)
    return ['-isystem', os.path.dirname(found[0])] if found else []


class Scanner:
    def __init__(self, library_path=None):
        lib = find_library(library_path)
        if lib is None:
            raise RuntimeError('libclang was not found (set --libclang or CLING_SCAN_LIBCLANG to the shared library)')
        self.lib = lib
        L = lib
        L.clang_getClangVersion.restype = CXString
        L.clang_getCString.argtypes = [CXString]
        L.clang_getCString.restype = c_char_p
        L.clang_disposeString.argtypes = [CXString]
        L.clang_createIndex.restype = c_void_p
        L.clang_parseTranslationUnit.argtypes = [c_void_p, c_char_p, POINTER(c_char_p), c_int, c_void_p, c_uint, c_uint]
        L.clang_parseTranslationUnit.restype = c_void_p
        L.clang_getNumDiagnostics.argtypes = [c_void_p]
        L.clang_getNumDiagnostics.restype = c_uint
        L.clang_getDiagnostic.argtypes = [c_void_p, c_uint]
        L.clang_getDiagnostic.restype = c_void_p
        L.clang_getDiagnosticSeverity.argtypes = [c_void_p]
        L.clang_getDiagnosticSeverity.restype = c_int
        L.clang_formatDiagnostic.argtypes = [c_void_p, c_uint]
        L.clang_formatDiagnostic.restype = CXString
        L.clang_getTranslationUnitCursor.argtypes = [c_void_p]
        L.clang_getTranslationUnitCursor.restype = CXCursor
        L.clang_visitChildren.argtypes = [CXCursor, _VISITOR, c_void_p]
        L.clang_visitChildren.restype = c_uint
        L.clang_getCursorKind.argtypes = [CXCursor]
        L.clang_getCursorKind.restype = c_int
        L.clang_getCursorSpelling.argtypes = [CXCursor]
        L.clang_getCursorSpelling.restype = CXString
        L.clang_getCursorType.argtypes = [CXCursor]
        L.clang_getCursorType.restype = CXType
        L.clang_getCanonicalType.argtypes = [CXType]
        L.clang_getCanonicalType.restype = CXType
        L.clang_getTypeSpelling.argtypes = [CXType]
        L.clang_getTypeSpelling.restype = CXString
        L.clang_getResultType.argtypes = [CXType]
        L.clang_getResultType.restype = CXType
        L.clang_getNumArgTypes.argtypes = [CXType]
        L.clang_getNumArgTypes.restype = c_int
        L.clang_getArgType.argtypes = [CXType, c_uint]
        L.clang_getArgType.restype = CXType
        L.clang_getPointeeType.argtypes = [CXType]
        L.clang_getPointeeType.restype = CXType
        L.clang_getTypeDeclaration.argtypes = [CXType]
        L.clang_getTypeDeclaration.restype = CXCursor
        L.clang_getCXXAccessSpecifier.argtypes = [CXCursor]
        L.clang_getCXXAccessSpecifier.restype = c_int
        L.clang_CXXMethod_isConst.argtypes = [CXCursor]
        L.clang_CXXMethod_isConst.restype = c_uint
        L.clang_CXXMethod_isStatic.argtypes = [CXCursor]
        L.clang_CXXMethod_isStatic.restype = c_uint
        L.clang_isCursorDefinition.argtypes = [CXCursor]
        L.clang_isCursorDefinition.restype = c_uint
        L.clang_getCursorSemanticParent.argtypes = [CXCursor]
        L.clang_getCursorSemanticParent.restype = CXCursor
        L.clang_getCursorLocation.argtypes = [CXCursor]
        L.clang_getCursorLocation.restype = CXSourceLocation
        L.clang_Location_isInSystemHeader.argtypes = [CXSourceLocation]
        L.clang_Location_isInSystemHeader.restype = c_int
        L.clang_getInclusions.argtypes = [c_void_p, _INCLUDE_VISITOR, c_void_p]
        L.clang_getFile.argtypes = [c_void_p, c_char_p]
        L.clang_getFile.restype = c_void_p
        L.clang_getLocation.argtypes = [c_void_p, c_void_p, c_uint, c_uint]
        L.clang_getLocation.restype = CXSourceLocation
        L.clang_getFileName.argtypes = [c_void_p]
        L.clang_getFileName.restype = CXString
        L.clang_disposeTranslationUnit.argtypes = [c_void_p]
        self.index = L.clang_createIndex(0, 0)

    # ---- small helpers -------------------------------------------------------------------------------------------------------------
    def string(self, cx):
        s = self.lib.clang_getCString(cx)
        text = s.decode('utf-8', 'replace') if s else ''
        self.lib.clang_disposeString(cx)
        return text

    def version(self):
        v = self.string(self.lib.clang_getClangVersion())
        return v.replace('clang version', '').split('(')[0].replace('Ubuntu', '').strip()

    def spelling(self, cursor):
        return self.string(self.lib.clang_getCursorSpelling(cursor))

    def kind(self, cursor):
        return self.lib.clang_getCursorKind(cursor)

    def children(self, cursor):
        out = []

        def visit(child, parent, data):
            out.append(CXCursor(child.kind, child.xdata, (c_void_p * 3)(*child.data)))
            return CHILD_CONTINUE

        self.lib.clang_visitChildren(cursor, _VISITOR(visit), None)
        return out

    def type_text(self, t):
        return self.string(self.lib.clang_getTypeSpelling(self.lib.clang_getCanonicalType(t)))

    def qualified_name(self, cursor):
        parts = []
        c = cursor
        while True:
            k = self.kind(c)
            if k in (NAMESPACE, STRUCT_DECL, CLASS_DECL, CLASS_TEMPLATE):
                name = self.spelling(c)
                if name:
                    parts.append(name)
            elif k >= 300:        # the translation unit
                break
            parent = self.lib.clang_getCursorSemanticParent(c)
            if parent.kind == c.kind and parent.data[0] == c.data[0] and parent.data[1] == c.data[1]:
                break
            c = parent
        return '::'.join(reversed(parts))

    def in_system_header(self, cursor):
        return bool(self.lib.clang_Location_isInSystemHeader(self.lib.clang_getCursorLocation(cursor)))

    # ---- parsing -------------------------------------------------------------------------------------------------------------------
    def parse(self, text, std, includes, extra=()):
        d = tempfile.mkdtemp()
        path = os.path.join(d, 'tu.cc')
        open(path, 'w', encoding='utf-8').write(text)
        own = [] if any(a.startswith('-resource-dir') for a in extra) else builtin_header_dirs()
        argv = ['-std=' + std, '-x', 'c++'] + own + list(extra) + ['-I' + i for i in includes]
        arr = (c_char_p * len(argv))(*[a.encode() for a in argv])
        tu = self.lib.clang_parseTranslationUnit(self.index, path.encode(), arr, len(argv), None, 0, 0)
        if not tu:
            raise RuntimeError('libclang could not parse the translation unit')
        errors = []
        for i in range(self.lib.clang_getNumDiagnostics(tu)):
            diag = self.lib.clang_getDiagnostic(tu, i)
            if self.lib.clang_getDiagnosticSeverity(diag) >= 3:          # error, fatal
                errors.append(self.string(self.lib.clang_formatDiagnostic(diag, 0)))
        return tu, path, errors

    def dependencies(self, tu, entry):
        """Every non-system header the translation unit includes, transitively (what `clang -MM` lists)."""
        files = set()

        def visit(file, stack, depth, data):
            name = self.string(self.lib.clang_getFileName(file))
            if not name or name.endswith('tu.cc'):
                return
            loc = self.lib.clang_getLocation(tu, file, 1, 1)
            if not self.lib.clang_Location_isInSystemHeader(loc) and os.path.exists(name):
                files.add(os.path.normpath(name))

        self.lib.clang_getInclusions(tu, _INCLUDE_VISITOR(visit), None)
        files.add(os.path.normpath(entry))
        return sorted(files)

    # ---- the extraction --------------------------------------------------------------------------------------------------------------
    def find_class(self, tu_cursor, qualified):
        """The definition of a non-template class or struct named `qualified` (`Grid`, `Json::Value`) declared outside the system headers."""
        found = []

        def walk(cursor):
            for child in self.children(cursor):
                k = self.kind(child)
                if k in (STRUCT_DECL, CLASS_DECL) and self.lib.clang_isCursorDefinition(child) and not self.in_system_header(child):
                    q = self.qualified_name(child)
                    if q == qualified or q.endswith('::' + qualified):
                        found.append(child)
                if k in (NAMESPACE, STRUCT_DECL, CLASS_DECL) and not self.in_system_header(child):
                    walk(child)

        walk(tu_cursor)
        return found[0] if found else None

    def read_class(self, cursor, methods, skipped, enums):
        """Add the public methods of one class definition."""
        L = self.lib
        for member in self.children(cursor):
            k = self.kind(member)
            if k == FUNCTION_TEMPLATE:
                if L.clang_getCXXAccessSpecifier(member) == ACCESS_PUBLIC:
                    skipped.append('a member template (needs the arguments it is used with)')
                continue
            if k != CXX_METHOD:
                continue
            name = self.spelling(member)
            if L.clang_getCXXAccessSpecifier(member) != ACCESS_PUBLIC:
                continue
            if (name.startswith('operator') and (len(name) == 8 or not (name[8].isalnum() or name[8] == '_'))):
                skipped.append('%s: operators need `operator` facts written by hand' % name)
                continue
            ftype = L.clang_getCursorType(member)
            ret = L.clang_getResultType(ftype)
            params = [L.clang_getArgType(ftype, i) for i in range(max(L.clang_getNumArgTypes(ftype), 0))]
            for t in [ret] + params:
                self.note_enum(t, enums)
            methods.setdefault(name, []).append(dict(ret=self.type_text(ret), params=[self.type_text(p) for p in params],
                                                    const=bool(L.clang_CXXMethod_isConst(member)), static=bool(L.clang_CXXMethod_isStatic(member))))

    def note_enum(self, t, enums):
        L = self.lib
        while L.clang_getCanonicalType(t).kind in (TYPE_LVALUE_REF, TYPE_RVALUE_REF):
            t = L.clang_getPointeeType(L.clang_getCanonicalType(t))
        decl = L.clang_getTypeDeclaration(L.clang_getCanonicalType(t))
        if self.kind(decl) == ENUM_DECL:
            enums.add(self.string(L.clang_getTypeSpelling(L.clang_getCursorType(decl))))


def extract(header, cls, std, includes, library_path=None, extra=()):
    """-> dict(cls, methods, skipped, enums, class_count, dependencies, version)  for a non-template class (`--class`)."""
    sc = Scanner(library_path)
    include_line = '#include "%s"' % header.replace(os.sep, '/') + chr(10)
    methods, skipped, enums = {}, [], set()
    tu, path, errors = sc.parse(include_line, std, includes, extra)
    root_cursor = sc.find_class(sc.lib.clang_getTranslationUnitCursor(tu), cls)
    if root_cursor is None:
        sys.exit('no definition of a class or struct named `%s` found in %s (for a template instantiation use --type with --backend text)'
                 % (cls, header) + chr(10) + chr(10).join(errors[:5]))
    sc.read_class(root_cursor, methods, skipped, enums)
    result = dict(cls=cls, methods=methods, skipped=skipped, enums=enums, class_count=1,
                  dependencies=sc.dependencies(tu, header), version=sc.version())
    sc.lib.clang_disposeTranslationUnit(tu)
    return result
