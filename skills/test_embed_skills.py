"""Run with a Python that provides SCons; all build artifacts are temporary."""

import ast
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


SKILLS_DIR = Path(__file__).resolve().parent
PACKAGE_DIR = SKILLS_DIR.parent

# Import the real generator without creating __pycache__ in the source tree.
_spec = importlib.util.spec_from_file_location(
    "agent_embed_skills_under_test", SKILLS_DIR / "embed_skills.py"
)
embed_skills = importlib.util.module_from_spec(_spec)
_old_bytecode_setting = sys.dont_write_bytecode
try:
    sys.dont_write_bytecode = True
    _spec.loader.exec_module(embed_skills)
finally:
    sys.dont_write_bytecode = _old_bytecode_setting


C_LITERAL = re.compile(r'"(?:\\[0-7]{3}|\\[tnr"\\]|[^"\\\r\n])*"')
C_ARRAY = re.compile(
    r"(?:static )?const char (\w+)\[\] =\n(.*?);\n", re.DOTALL
)


def decode_c_literals(value):
    """Independently decode the shared ASCII subset of C/Python byte literals."""
    value.encode("ascii")
    literals = list(C_LITERAL.finditer(value))
    if not literals or C_LITERAL.sub("", value).strip():
        raise AssertionError("Invalid adjacent C string literals: {!r}".format(value))
    return b"".join(ast.literal_eval("b" + item.group()) for item in literals)


class TemporaryCase(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="agent-skills-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()

    def write(self, relative_path, data, root=None):
        path = (self.root if root is None else root) / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def assert_embedded(self, source, expected):
        """Check decoded bytes, catalog metadata and const table references."""
        self.assertTrue(source.isascii())
        self.assertIn('#include "skill.h"', source)
        arrays = {
            name: decode_c_literals(value)
            for name, value in C_ARRAY.findall(source)
        }
        self.assertEqual(len(arrays), 2 * len(expected) + 1)
        catalog = []
        entries = []
        for index, (name, data, title) in enumerate(expected):
            self.assertEqual(arrays["skill_name_{}".format(index)], name.encode("utf-8"))
            self.assertEqual(arrays["skill_content_{}".format(index)], data)
            self.assertIn("static const char skill_name_{}[]".format(index), source)
            self.assertIn("static const char skill_content_{}[]".format(index), source)
            catalog.append({"name": name, "title": title, "size": len(data)})
            entries.append(
                "    {{skill_name_{0}, skill_content_{0}, sizeof(skill_content_{0}) - 1}},".format(index)
            )
        entries.append("    {NULL, NULL, 0}")
        self.assertIn(
            "const struct agent_skill agent_skill_table[] =\n{\n"
            + "\n".join(entries) + "\n};",
            source,
        )
        self.assertIn(
            "const size_t agent_skill_table_count = {};".format(len(expected)), source
        )
        self.assertEqual(json.loads(arrays["agent_skill_catalog"].decode("utf-8")), catalog)
        return arrays


class CollectSkillsTests(TemporaryCase):
    def test_empty_directory(self):
        self.assertEqual(embed_skills.collect_skills(self.root), ([], []))

    def test_recursive_case_insensitive_extensions_and_posix_sort(self):
        relative_paths = ["z.md", "sub/Z.Md", "sub/A.MD", "A.md", "中文/技能.mD"]
        for name in relative_paths:
            self.write(name, b"body")
        self.write("ignored.txt", b"not a skill")
        self.write("ignored.markdown", b"not a skill")
        (self.root / "directory.md").mkdir()
        paths, names = embed_skills.collect_skills(str(self.root))
        expected = sorted(relative_paths)
        self.assertEqual([path.relative_to(self.root).as_posix() for path in paths], expected)
        self.assertEqual(names, [Path(name).with_suffix("").as_posix() for name in expected])
        self.assertTrue(all("\\" not in name for name in names))

    def test_same_basename_in_different_subdirectories(self):
        for name in ["b/shared.md", "shared.md", "a/shared.md"]:
            self.write(name, b"")
        _, names = embed_skills.collect_skills(self.root)
        self.assertEqual(names, ["a/shared", "b/shared", "shared"])

    def test_only_final_extension_is_removed(self):
        self.write("folder.with.dots/skill.v2.MD", b"")
        self.assertEqual(
            embed_skills.collect_skills(self.root)[1], ["folder.with.dots/skill.v2"]
        )

    def test_duplicate_extension_variants_are_rejected(self):
        first = self.write("same.md", b"first")
        second = self.root / "same.MD"
        if second.exists():
            # Windows cannot store these two names: simulate only the directory
            # listing to exercise the duplicate guard, not any SCons behavior.
            with mock.patch.object(Path, "rglob", return_value=iter([first, second])):
                with self.assertRaisesRegex(ValueError, "Duplicate skill names"):
                    embed_skills.collect_skills(self.root)
        else:
            second.write_bytes(b"second")
            with self.assertRaisesRegex(ValueError, "Duplicate skill names"):
                embed_skills.collect_skills(self.root)

    def test_missing_directory_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "Skill directory does not exist"):
            embed_skills.collect_skills(self.root / "missing")

    def test_file_instead_of_directory_is_rejected(self):
        path = self.write("not-a-directory.md", b"")
        with self.assertRaisesRegex(ValueError, "Skill directory does not exist"):
            embed_skills.collect_skills(path)

    def test_collection_does_not_depend_on_creation_or_enumeration_order(self):
        for name in ["z.md", "nested/b.md", "a.md"]:
            self.write(name, b"same")
        expected = embed_skills.collect_skills(self.root)
        with mock.patch.object(Path, "rglob", return_value=iter(reversed(expected[0]))):
            self.assertEqual(embed_skills.collect_skills(self.root), expected)
        self.assertEqual(embed_skills.collect_skills(self.root), expected)


class CStringTests(unittest.TestCase):
    def test_empty_bytes(self):
        self.assertEqual(embed_skills.c_string(b""), '    ""')
        self.assertEqual(decode_c_literals(embed_skills.c_string(b"")), b"")

    def test_all_byte_values_round_trip_as_ascii(self):
        data = bytes(range(256))
        encoded = embed_skills.c_string(data)
        self.assertTrue(encoded.isascii())
        self.assertEqual(decode_c_literals(encoded), data)

    def test_chinese_utf8_uses_three_digit_octal(self):
        data = "中文技能".encode("utf-8")
        encoded = embed_skills.c_string(data)
        self.assertEqual(encoded, '    "' + "".join("\\{:03o}".format(b) for b in data) + '"')
        self.assertEqual(decode_c_literals(encoded), data)

    def test_quotes_backslashes_and_common_whitespace(self):
        data = b'"\\\t\r\n'
        self.assertEqual(embed_skills.c_string(data), '    "\\"\\\\\\t\\r\\n"')
        self.assertEqual(decode_c_literals(embed_skills.c_string(data)), data)

    def test_control_escapes_do_not_consume_following_digits(self):
        data = b"\x007\x017\x1f0\x7f9\xff1"
        encoded = embed_skills.c_string(data)
        for escape in [r"\0007", r"\0017", r"\0370", r"\1779", r"\3771"]:
            self.assertIn(escape, encoded)
        self.assertEqual(decode_c_literals(encoded), data)

    def test_all_trigraphs_are_prevented(self):
        data = b"??= ??/ ??' ??( ??) ??! ??< ??> ??- ?"
        encoded = embed_skills.c_string(data)
        self.assertNotIn("?", encoded)
        self.assertIn(r"\077", encoded)
        self.assertEqual(decode_c_literals(encoded), data)

    def test_adjacent_literal_chunk_boundaries(self):
        for size in [63, 64, 65, 127, 128, 129]:
            with self.subTest(size=size):
                # The UTF-8 character straddles the 64-byte split.
                data = (b"x" * 63 + "中".encode("utf-8") + b"\\?\n" * 32)[:size]
                encoded = embed_skills.c_string(data)
                self.assertEqual(len(encoded.splitlines()), (size + 63) // 64)
                self.assertEqual(decode_c_literals(encoded), data)

    def test_encoding_is_deterministic(self):
        data = bytes(range(256)) * 3
        self.assertEqual(embed_skills.c_string(data), embed_skills.c_string(data))


class RenderSkillsTests(TemporaryCase):
    def render_one(self, data, name="skill"):
        path = self.write("input.md", data)
        return embed_skills.render_skills([path], [name])

    def test_chinese_name_title_and_utf8_byte_size(self):
        data = "# 中文标题\n正文：你好，世界。\n".encode("utf-8")
        self.assert_embedded(self.render_one(data, "中文/技能"), [("中文/技能", data, "中文标题")])

    def test_utf8_bom_is_removed(self):
        data = "# 带 BOM 的标题\r\n正文".encode("utf-8")
        self.assert_embedded(self.render_one(b"\xef\xbb\xbf" + data), [("skill", data, "带 BOM 的标题")])

    def test_bom_only_file_is_empty(self):
        self.assert_embedded(self.render_one(b"\xef\xbb\xbf"), [("skill", b"", "skill")])

    def test_first_exact_h1_is_the_title(self):
        data = b"## Not H1\n # Indented\n#NoSpace\n# First title  \n# Second title\n"
        self.assert_embedded(self.render_one(data), [("skill", data, "First title")])

    def test_no_h1_falls_back_to_full_name(self):
        data = b"## Heading\n # Indented\n#NoSpace\nplain text"
        self.assert_embedded(self.render_one(data, "sub/fallback"), [("sub/fallback", data, "sub/fallback")])

    def test_escaping_in_names_content_and_json_titles(self):
        name = 'sub/中文"\\?'
        title = '标题 "quote" \\ ??/\tend'
        data = ("# " + title + "\ncontrol:\x01\x1f\x7f\t\\\"??=\n").encode("utf-8")
        self.assert_embedded(self.render_one(data, name), [(name, data, title)])

    def test_crlf_and_mixed_newlines_are_preserved(self):
        data = b"# CRLF\r\nfirst\r\nsecond\nthird\rlast\r\n"
        self.assert_embedded(self.render_one(data), [("skill", data, "CRLF")])

    def test_no_trailing_newline_is_added(self):
        data = b"# No final newline\r\nbody"
        self.assert_embedded(self.render_one(data), [("skill", data, "No final newline")])

    def test_empty_file_has_zero_size_and_name_title(self):
        self.assert_embedded(self.render_one(b""), [("skill", b"", "skill")])

    def test_empty_directory_has_sentinel_and_empty_catalog(self):
        paths, names = embed_skills.collect_skills(self.root)
        self.assert_embedded(embed_skills.render_skills(paths, names), [])

    def test_invalid_utf8_is_rejected(self):
        for data in [b"\xff", b"\xe4\xb8", b"\xc0\xaf", b"\xed\xa0\x80", b"\xef\xbb\xbf\xff"]:
            with self.subTest(data=data):
                with self.assertRaises(UnicodeDecodeError):
                    self.render_one(data)

    def test_nul_is_rejected_at_any_position(self):
        for data in [b"\0start", b"mid\0dle", b"end\0", b"\xef\xbb\xbf\0"]:
            with self.subTest(data=data):
                with self.assertRaisesRegex(ValueError, "Skill contains a NUL byte"):
                    self.render_one(data)

    def test_sorted_multiple_skills_and_determinism(self):
        expected = [("a/shared", b"# First\n", "First"), ("b/shared", b"body", "b/shared")]
        for name, data, _ in reversed(expected):
            self.write(name + ".md", data)
        paths, names = embed_skills.collect_skills(self.root)
        first = embed_skills.render_skills(paths, names)
        self.assert_embedded(first, expected)
        self.assertEqual(first, embed_skills.render_skills(paths, names))
        other = self.root / "another-root"
        for name, data, _ in expected:
            self.write(name + ".md", data, root=other)
        self.assertEqual(first, embed_skills.render_skills(*embed_skills.collect_skills(other)))


class SConsIntegrationTests(TemporaryCase):
    def setUp(self):
        super().setUp()
        self.skill_dir = self.root / "input skills 中文"
        self.skill_dir.mkdir()
        self.target = self.root / "variant" / "skill_data.c"
        self.sconstruct = self.root / "SConstruct"
        self.sconstruct.write_text(
            "import sys\n"
            "import types\n"
            "from SCons.Script import ARGUMENTS, Default, Dir, Environment, SConscript\n"
            "building = types.ModuleType('building')\n"
            "building.GetCurrentDir = lambda: Dir('.').srcnode().get_abspath()\n"
            "building.GetConfigValue = lambda name: ARGUMENTS.get('config_skill_dir', '')\n"
            "sys.modules['building'] = building\n"
            "env = Environment(tools=[])\n"
            "nodes = SConscript({!r}, variant_dir=Dir('variant').get_abspath(), "
            "duplicate=0, exports={{'env': env}})\n"
            "Default(nodes)\n".format(str(SKILLS_DIR / "SConscript")),
            encoding="utf-8",
        )

    def build(self, directory, rebuilt=True, error=None, configured_directory=None):
        command = [sys.executable, "-B", "-m", "SCons", "-Q", "-f", str(self.sconstruct)]
        if directory is not None:
            command.append("agent_skill_dir=" + str(directory))
        if configured_directory is not None:
            command.append("config_skill_dir=" + json.dumps(str(configured_directory)))
        environment = os.environ.copy()
        environment["PYTHONDONTWRITEBYTECODE"] = "1"
        environment["PYTHONUTF8"] = "1"
        environment.pop("SCONSFLAGS", None)
        result = subprocess.run(
            command,
            cwd=str(self.root),
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            encoding="utf-8",
            errors="replace",
            timeout=60,
            check=False,
        )
        if error is not None:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertIn(error, result.stdout)
            return result
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(result.stdout.count("Embedding Agent skills ->"), int(rebuilt), result.stdout)
        self.assertTrue(self.target.is_file(), result.stdout)
        self.assertFalse((self.root / "skill_data.c").exists())
        return result

    def assert_output(self, expected):
        return self.assert_embedded(self.target.read_bytes().decode("ascii"), expected)

    def assert_no_rebuild(self, directory):
        before = self.target.read_bytes()
        timestamp = self.target.stat().st_mtime_ns
        self.build(directory, rebuilt=False)
        self.assertEqual(self.target.read_bytes(), before)
        self.assertEqual(self.target.stat().st_mtime_ns, timestamp)

    def seed(self):
        return self.write("first.md", b"# First\nbody", root=self.skill_dir)

    def test_absolute_directory_initial_build_and_noop(self):
        data = "# 中文\r\n引号\"反斜杠\\问号??/".encode("utf-8")
        self.write("子目录/技能.MD", b"\xef\xbb\xbf" + data, root=self.skill_dir)
        self.write("empty.md", b"", root=self.skill_dir)
        self.build(self.skill_dir)
        self.assert_output([("empty", b"", "empty"), ("子目录/技能", data, "中文")])
        self.assert_no_rebuild(self.skill_dir)

    def test_default_directory_is_the_real_package_skills_directory(self):
        # Read existing package Markdown only; never edit the default math.md.
        paths, names = embed_skills.collect_skills(SKILLS_DIR)
        expected = []
        for path, name in zip(paths, names):
            text = path.read_bytes().decode("utf-8-sig")
            title = next((line[2:].strip() for line in text.splitlines() if line.startswith("# ")), name)
            expected.append((name, text.encode("utf-8"), title))
        self.build(None)
        self.assert_output(expected)
        self.assert_no_rebuild(None)

    def test_package_relative_directory_not_sconstruct_relative(self):
        # A temporary sibling of the package is necessarily on the same drive,
        # so relpath also works when Windows TEMP and the checkout differ.
        temporary = tempfile.TemporaryDirectory(prefix="agent-skill-relative-", dir=str(PACKAGE_DIR.parent))
        self.addCleanup(temporary.cleanup)
        directory = Path(temporary.name) / "skills with spaces"
        self.write("relative.md", b"# Relative", root=directory)
        relative = os.path.relpath(directory, PACKAGE_DIR)
        self.assertFalse(os.path.isabs(relative))
        self.assertFalse((self.root / relative).is_dir())
        self.build(relative)
        self.assert_output([("relative", b"# Relative", "Relative")])
        self.assert_no_rebuild(relative)

    def test_kconfig_directory(self):
        self.seed()
        self.build(None, configured_directory=self.skill_dir)
        self.assert_output([("first", b"# First\nbody", "First")])
        self.build(None, rebuilt=False, configured_directory=self.skill_dir)

    def test_command_line_directory_overrides_kconfig(self):
        self.seed()
        self.build(self.skill_dir, configured_directory=self.root / "not selected")
        self.assert_output([("first", b"# First\nbody", "First")])

    def test_modified_markdown_rebuilds(self):
        path = self.seed()
        self.build(self.skill_dir)
        path.write_bytes(b"# Updated\r\nlonger body without final newline")
        self.build(self.skill_dir)
        self.assert_output([("first", path.read_bytes(), "Updated")])
        self.assert_no_rebuild(self.skill_dir)

    def test_added_nested_markdown_rebuilds(self):
        self.seed()
        self.build(self.skill_dir)
        self.write("nested/first.mD", b"# Nested", root=self.skill_dir)
        self.build(self.skill_dir)
        self.assert_output([("first", b"# First\nbody", "First"), ("nested/first", b"# Nested", "Nested")])
        self.assert_no_rebuild(self.skill_dir)

    def test_deleted_markdown_rebuilds(self):
        self.seed()
        removed = self.write("remove.md", b"# Remove", root=self.skill_dir)
        self.build(self.skill_dir)
        removed.unlink()
        self.build(self.skill_dir)
        self.assert_output([("first", b"# First\nbody", "First")])
        self.assert_no_rebuild(self.skill_dir)

    def test_renamed_markdown_rebuilds_even_with_unchanged_bytes(self):
        path = self.seed()
        self.build(self.skill_dir)
        path.rename(self.skill_dir / "renamed.MD")
        self.build(self.skill_dir)
        self.assert_output([("renamed", b"# First\nbody", "First")])
        self.assert_no_rebuild(self.skill_dir)

    def test_non_markdown_changes_do_not_rebuild(self):
        self.seed()
        self.build(self.skill_dir)
        ignored = self.write("ignored.txt", b"one", root=self.skill_dir)
        self.assert_no_rebuild(self.skill_dir)
        ignored.write_bytes(b"modified ignored file")
        self.assert_no_rebuild(self.skill_dir)
        ignored.unlink()
        self.assert_no_rebuild(self.skill_dir)

    def test_directory_switch_with_identical_contents_rebuilds(self):
        self.seed()
        other = self.root / "second directory"
        self.write("first.md", b"# First\nbody", root=other)
        self.build(self.skill_dir)
        initial = self.target.read_bytes()
        self.build(other)
        self.assertEqual(self.target.read_bytes(), initial)
        self.assert_no_rebuild(other)
        self.build(self.skill_dir)
        self.assertEqual(self.target.read_bytes(), initial)
        self.assert_no_rebuild(self.skill_dir)

    def test_empty_directory_and_last_file_transitions(self):
        self.build(self.skill_dir)
        self.assert_output([])
        self.assert_no_rebuild(self.skill_dir)
        path = self.seed()
        self.build(self.skill_dir)
        self.assert_output([("first", b"# First\nbody", "First")])
        path.unlink()
        self.build(self.skill_dir)
        self.assert_output([])
        self.assert_no_rebuild(self.skill_dir)

    def test_switch_between_empty_directories_tracks_directory_value(self):
        other = self.root / "other empty directory"
        other.mkdir()
        self.build(self.skill_dir)
        initial = self.target.read_bytes()
        self.build(other)
        self.assertEqual(self.target.read_bytes(), initial)
        self.assert_output([])
        self.assert_no_rebuild(other)
        self.build(self.skill_dir)
        self.assert_no_rebuild(self.skill_dir)

    def test_missing_directory_fails_without_creating_target(self):
        self.build(self.root / "missing", error="Skill directory does not exist")
        self.assertFalse(self.target.exists())

    def test_switch_to_missing_directory_fails_without_overwriting_target(self):
        self.seed()
        self.build(self.skill_dir)
        initial = self.target.read_bytes()
        timestamp = self.target.stat().st_mtime_ns
        self.build(self.root / "missing", error="Skill directory does not exist")
        self.assertEqual(self.target.read_bytes(), initial)
        self.assertEqual(self.target.stat().st_mtime_ns, timestamp)

    def test_invalid_utf8_fails_the_real_scons_action(self):
        self.write("invalid.md", b"\xff", root=self.skill_dir)
        self.build(self.skill_dir, error="UnicodeDecodeError")
        self.assertFalse(self.target.exists())

    def test_nul_fails_the_real_scons_action(self):
        self.write("invalid.md", b"# Invalid\nnull\0byte", root=self.skill_dir)
        self.build(self.skill_dir, error="Skill contains a NUL byte")
        self.assertFalse(self.target.exists())

    def test_recreating_target_is_byte_deterministic(self):
        self.seed()
        self.write("nested/中文.md", "# 标题\r\n内容".encode("utf-8"), root=self.skill_dir)
        self.build(self.skill_dir)
        initial = self.target.read_bytes()
        self.target.unlink()
        self.build(self.skill_dir)
        self.assertEqual(self.target.read_bytes(), initial)
        self.assert_no_rebuild(self.skill_dir)


if __name__ == "__main__":
    unittest.main(verbosity=2)
