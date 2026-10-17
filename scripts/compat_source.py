"""Shared file handling for build-local compatibility sources."""
import re


def resolve_local_includes(text, source):
    """Keep local SDK includes valid when a translation unit is relocated."""
    def resolve(match):
        candidate = source.parent / match.group(1)
        if candidate.is_file():
            return '#include "' + candidate.resolve().as_posix() + '"'
        return match.group(0)

    return re.sub(r'#include "([^"]+)"', resolve, text)


def write_if_changed(path, text):
    """Avoid touching unchanged generated files and recompiling their users."""
    if path.exists() and path.read_text(encoding='utf-8') == text:
        return False
    path.write_text(text, encoding='utf-8')
    return True
