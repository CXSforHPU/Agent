import json
from pathlib import Path


def collect_skills(directory):
    root = Path(directory)
    if not root.is_dir():
        raise ValueError("Skill directory does not exist: {}".format(root))
    paths = sorted(
        (path for path in root.rglob("*") if path.is_file() and path.suffix.lower() == ".md"),
        key=lambda path: path.relative_to(root).as_posix(),
    )
    names = [path.relative_to(root).with_suffix("").as_posix() for path in paths]
    if len(names) != len(set(names)):
        raise ValueError("Duplicate skill names in {}".format(root))
    return paths, names


def c_string(data):
    escaped = {9: r"\t", 10: r"\n", 13: r"\r", 34: r'\"', 63: r"\077", 92: r"\\"}
    lines = []
    for offset in range(0, len(data), 64):
        chunk = []
        for byte in data[offset:offset + 64]:
            if byte in escaped:
                chunk.append(escaped[byte])
            elif 32 <= byte < 127:
                chunk.append(chr(byte))
            else:
                chunk.append("\\{:03o}".format(byte))
        lines.append('    "' + "".join(chunk) + '"')
    return "\n".join(lines) if lines else '    ""'


def render_skills(paths, names):
    lines = ['/* Generated from Markdown by SCons. Do not edit. */', '#include "skill.h"', '']
    catalog = []
    for index, (path, name) in enumerate(zip(paths, names)):
        text = Path(path).read_bytes().decode("utf-8-sig")
        if "\0" in text:
            raise ValueError("Skill contains a NUL byte: {}".format(path))
        data = text.encode("utf-8")
        title = next((line.lstrip("#").strip() for line in text.splitlines()
                      if line.startswith("# ")), name)
        catalog.append({"name": name, "title": title, "size": len(data)})
        lines.extend([
            "static const char skill_name_{}[] =".format(index),
            c_string(name.encode("utf-8")) + ";", "",
            "static const char skill_content_{}[] =".format(index),
            c_string(data) + ";", "",
        ])
    lines.extend(["const struct agent_skill agent_skill_table[] =", "{"])
    for index in range(len(names)):
        lines.append("    {{skill_name_{0}, skill_content_{0}, sizeof(skill_content_{0}) - 1}},".format(index))
    lines.extend([
        "    {NULL, NULL, 0}", "};", "",
        "const size_t agent_skill_table_count = {};".format(len(names)), "",
        "const char agent_skill_catalog[] =",
        c_string(json.dumps(catalog, ensure_ascii=False).encode("utf-8")) + ";", "",
    ])
    return "\n".join(lines)


def generate(target, source, env):
    paths = [node.get_abspath() for node in source]
    content = render_skills(paths, env["AGENT_SKILL_NAMES"])
    with Path(target[0].get_abspath()).open("w", encoding="ascii", newline="\n") as output:
        output.write(content)
    return 0
