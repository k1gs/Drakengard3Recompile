"""Shared file handling for build-local compatibility sources."""
import re


def resolve_local_includes(text, source, sdk):
    """Preserve SDK header identity when a source is copied into the build tree."""
    sdk_root = sdk.resolve()

    def resolve(match):
        candidate = source.parent / match["header"]
        if candidate.is_file():
            header = candidate.resolve().relative_to(sdk_root).as_posix()
            return f'{match["directive"]}"{header}"'
        return match.group(0)

    return re.sub(r'^(?P<directive>[ \t]*#[ \t]*include[ \t]*)"(?P<header>[^"\r\n]+)"',
                  resolve, text, flags=re.MULTILINE)


def write_if_changed(path, text):
    """Avoid touching unchanged generated files and recompiling their users."""
    if path.exists() and path.read_text(encoding='utf-8') == text:
        return False
    path.write_text(text, encoding='utf-8')
    return True
