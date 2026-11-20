"""Reject C++ identifiers that the Win32 headers redefine as macros.

<windows.h> (via <windef.h>/<rpc.h>/<objbase.h>) defines `far`, `near`,
`interface`, `small`, `pascal`, `cdecl` and `hyper` as macros. Any src/
translation unit that includes it after a header using one of those names as
an identifier fails to compile on MSVC and MSYS2 only. Devel builds Windows
only on PR trains, so the break otherwise surfaces late (#1059, #1078).
"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE_SUFFIXES = (".h", ".hpp", ".cpp", ".inl", ".cc")
BANNED = frozenset(("far", "near", "interface", "small", "pascal", "cdecl", "hyper"))

# Comments, string/char literals (including raw strings) and identifiers.
TOKENS = re.compile(
    r'//[^\n]*|/\*.*?\*/|R"(?P<delim>[^(\s"\\]*)\(.*?\)(?P=delim)"'
    r'|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|\b[A-Za-z_]\w*\b',
    re.DOTALL,
)


def banned_identifiers(text):
    """Yield (line, word) for each banned identifier outside comments and literals."""
    for match in TOKENS.finditer(text):
        word = match[0]
        if word in BANNED:
            yield text.count("\n", 0, match.start()) + 1, word


class Win32MacroIdentifiers(unittest.TestCase):
    def test_scanner_ignores_comments_and_literals(self):
        sample = (
            "// far and near in a comment\n"
            "/* interface */ const char* s = \"small far\";\n"
            "auto r = R\"x(near)x\"; char c = 'f';\n"
            "int far_state = 0; bool nearly = far_frame_settled(x);\n"
        )
        self.assertEqual(list(banned_identifiers(sample)), [])

    def test_scanner_reports_declarations_and_members(self):
        sample = "const auto far = f();\nint x = s.near;\nstruct interface {};\n"
        self.assertEqual(list(banned_identifiers(sample)),
                         [(1, "far"), (2, "near"), (3, "interface")])

    def test_src_uses_no_win32_macro_identifiers(self):
        offenders = []
        for path in sorted((ROOT / "src").rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            offenders.extend(f"{path.relative_to(ROOT).as_posix()}:{line}: {word}"
                             for line, word in banned_identifiers(text))
        self.assertEqual(offenders, [], "Win32 headers define these names as macros; "
                         "rename the identifiers")


if __name__ == "__main__":
    unittest.main()
