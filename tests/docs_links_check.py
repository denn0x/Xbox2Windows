"""Check inline Markdown links against the local repository."""

import argparse
import html
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unicodedata
from urllib.parse import unquote


FENCE = re.compile(r"^ {0,3}(\x60{3,}|~{3,})(.*)$")
CODE = re.compile(
    r"(?<![\x60\\])(\x60+)(?!\x60)((?:(?!\n[ \t]*\n).)*?)(?<!\x60)\1(?!\x60)",
    re.DOTALL,
)
LINK = re.compile(
    r"(?<!\\)\[((?:\\.|[^\[\]\\]|\[[^\[\]]*\])*)\]\(\s*"
    r"(?:<([^<>\n]*)>|((?:\\.|[^\s()\\]|\([^()\s]*\))*))"
    r"""(?:\s+(?:"[^"]*"|'[^']*'|\([^()]*\)))?\s*\)"""
)
ATX = re.compile(r"^ {0,3}#{1,6}(?:[ \t]+(.*)|$)")
SETEXT = re.compile(r"^ {0,3}(?:=+|-+)[ \t]*$")
ESCAPE = re.compile(r"\\([!-/:-@\[-\x60{-~])")


def slug(heading):
    # Keep code span text literal while interpreting markup outside the spans.
    pieces = []
    position = 0
    for match in CODE.finditer(heading):
        pieces.append(heading[position:match.start()])
        pieces.append("".join(f"&#{ord(char)};" for char in match[2]))
        position = match.end()
    pieces.append(heading[position:])
    heading = LINK.sub(r"\1", "".join(pieces))
    heading = re.sub(r"<[^>]*>", "", heading)
    heading = html.unescape(ESCAPE.sub(r"\1", heading))
    return "".join(
        "-" if char == " " else char
        for char in heading.lower()
        if char in " -_" or unicodedata.category(char)[0] in "LMN"
    )


def markdown_lines(text):
    """Replace fenced lines with blanks to preserve source line numbers."""
    fence = None
    for line in text.splitlines():
        match = FENCE.match(line)
        if fence:
            if (match and match[1][0] == fence[0] and len(match[1]) >= len(fence)
                    and not match[2].strip()):
                fence = None
            yield ""
        elif match and (match[1][0] == "~" or "`" not in match[2]):
            fence = match[1]
            yield ""
        else:
            yield line


def heading_slugs(lines):
    used = set()
    previous = ""
    for line in lines:
        match = ATX.fullmatch(line)
        if match:
            heading = re.sub(r"(?:^|[ \t]+)#+[ \t]*$", "", match[1] or "")
        elif previous.strip() and SETEXT.fullmatch(line):
            heading = previous
        else:
            previous = line
            continue
        previous = ""
        base = slug(heading.strip())
        anchor = base
        suffix = 1
        while anchor in used:
            anchor = f"{base}-{suffix}"
            suffix += 1
        used.add(anchor)
    return used


SKIP = 77


def git_checkout(root):
    """True if root has Git metadata. A source archive has none; anything else wrong with Git
    then fails loudly in git ls-files instead of turning into a skip."""
    return (root / ".git").exists()


def tracked_files(root):
    """Paths Git tracks, so links are checked with exact case and untracked local files never count."""
    # An inherited GIT_DIR or GIT_WORK_TREE, as in a Git hook, would point at another repository.
    environment = {key: value for key, value in os.environ.items()
                   if key not in ("GIT_DIR", "GIT_WORK_TREE")}
    result = subprocess.run(["git", "ls-files", "-z"], cwd=root, check=True, capture_output=True,
                            env=environment)
    return {Path(path) for path in result.stdout.decode("utf-8").split("\0") if path}


def tracked_markdown(tracked):
    return sorted(path for path in tracked if path.suffix.lower() == ".md")


def link_error(root, source, target, headings, tracked=None):
    path, fragment, anchor = target.partition("#")
    path = unquote(path.split("?", 1)[0])
    if "\0" in path:
        return "invalid path"
    relative = Path(path)
    if relative.anchor or path.startswith(("/", "\\")):
        return "target is outside the repository"
    if "\\" in path:
        # GitHub treats a backslash as a filename character; Windows would read it as a separator.
        return "backslash in path (use /)"
    linked = ((source.parent / relative) if path else source).resolve()
    if not linked.is_relative_to(root):
        return "target is outside the repository"
    if tracked is not None:
        # resolve() returns the on-disk case on Windows; compare the path as written in the link.
        written = Path(os.path.normpath((source.parent / relative) if path else source))
        if not written.is_relative_to(root):
            return "target is outside the repository"
        # Windows paths compare case-insensitively, so compare POSIX strings to keep the case exact.
        relative_target = written.relative_to(root).as_posix()
        files, directories = tracked
        if relative_target in directories:
            return "fragment on a directory" if fragment else None
        if relative_target not in files:
            return "file is not tracked (check the case of the path)"
        if path.endswith("/"):
            # normpath drops the slash, but GitHub does not resolve a file link that ends in one.
            return "trailing slash on a file"
    elif not linked.is_file():
        return "file does not exist"
    if fragment and (not path or linked.suffix.lower() == ".md"):
        if linked not in headings:
            headings[linked] = heading_slugs(markdown_lines(linked.read_text(encoding="utf-8-sig")))
        if unquote(anchor) not in headings[linked]:
            return "heading does not exist"
    return None


def check_links(root, files, tracked=None):
    if tracked is not None:
        tracked = ({path.as_posix() for path in tracked},
                   {parent.as_posix() for path in tracked for parent in path.parents})
    headings = {}
    broken = []
    count = 0
    for path in files:
        source = root / path
        lines = list(markdown_lines(source.read_text(encoding="utf-8-sig")))
        # Preserve offsets and newlines while masking inline code, including
        # spans that cross source lines.
        text = "\n".join(lines)
        masked = CODE.sub(lambda match: re.sub(r"[^\n]", "\0", match[0]), text)
        for match in LINK.finditer(masked):
            group = 2 if match[2] is not None else 3
            original = text[match.start(group):match.end(group)]
            target = html.unescape(ESCAPE.sub(r"\1", original))
            if target.lower().startswith(("https://", "http://", "mailto:")):
                continue
            count += 1
            reason = link_error(root, source, target, headings, tracked)
            if reason:
                line = text.count("\n", 0, match.start()) + 1
                broken.append(f"{path.as_posix()}:{line}: {original} ({reason})")
    return count, broken


def self_test():
    # Adapt headings from docs/experiments.md to exercise punctuation and
    # repeated headings, which the source headings do not currently contain.
    first = "## Mapping an image without `LoadLibrary`"
    second = "### FidelityFX's [context creation/ownership](other.md)"
    assert slug(first[3:]) == "mapping-an-image-without-loadlibrary"
    assert slug(second[4:]) == "fidelityfxs-context-creationownership"
    assert heading_slugs([first, first, first, second]) == {
        "mapping-an-image-without-loadlibrary",
        "mapping-an-image-without-loadlibrary-1",
        "mapping-an-image-without-loadlibrary-2",
        "fidelityfxs-context-creationownership",
    }
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory).resolve()
        path = Path("sample.md")
        (root / path).write_text(
            "[broken](missing.md)\n"
            "`[ignored](inline.md)`\n"
            "~~~markdown\n[ignored](fenced.md)\n~~~\n"
            f"{first}\n{first}\n"
            "[local](#mapping-an-image-without-loadlibrary)\n"
            "[duplicate](sample.md#mapping-an-image-without-loadlibrary-1)\n",
            encoding="utf-8",
        )
        assert check_links(root, [path]) == (
            3, ["sample.md:1: missing.md (file does not exist)"],
        )
        # Against a tracked set, a case mismatch fails and a tracked directory passes.
        (root / "docs").mkdir()
        (root / "docs" / "Guide.md").write_text("# Guide\n", encoding="utf-8")
        (root / "links.md").write_text(
            "[case](docs/guide.md)\n[dir](docs/)\n[ok](docs/Guide.md#guide)\n[frag](docs/#guide)\n",
            encoding="utf-8")
        tracked = {Path("links.md"), Path("docs/Guide.md")}
        assert check_links(root, [Path("links.md")], tracked) == (
            4, ["links.md:1: docs/guide.md (file is not tracked (check the case of the path))",
                "links.md:4: docs/#guide (fragment on a directory)"],
        )
        # A backslash is not a separator on GitHub or Linux, so it fails everywhere; a slash
        # after a file fails, while the same slash after a tracked directory is the control.
        (root / "portable.md").write_text(
            "[back](docs\\Guide.md)\n[file](docs/Guide.md/)\n[dir](docs/)\n", encoding="utf-8")
        tracked.add(Path("portable.md"))
        assert check_links(root, [Path("portable.md")], tracked) == (
            3, ["portable.md:1: docs\\Guide.md (backslash in path (use /))",
                "portable.md:2: docs/Guide.md/ (trailing slash on a file)"],
        )
        # A directory without Git metadata, like an extracted source archive, is not a checkout.
        assert not git_checkout(root)
    print("Self-test passed.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    # The slug and masking rules are checked on every run, so CTest also covers them.
    self_test()
    if args.self_test:
        return 0
    root = Path(__file__).resolve().parents[1]
    # The tracked-path and case checks need Git metadata. A source archive has none, so the
    # check reports a capability skip there instead of failing; CI always runs in a checkout.
    if not git_checkout(root):
        print(f"Skipped: {root} has no Git metadata, so tracked paths are unknown.")
        return SKIP
    tracked = tracked_files(root)
    files = tracked_markdown(tracked)
    count, broken = check_links(root, files, tracked)
    if broken:
        print("\n".join(broken))
        return 1
    print(f"Checked {len(files)} Markdown files and {count} links.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
