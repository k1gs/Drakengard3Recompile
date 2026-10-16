"""Launch the Windows game with direct file handles, returning only its PID.

The PowerShell runner owns the process lifetime. Avoid .NET line callbacks on
the game's hot stdout/stderr paths: they can backpressure every SPURS task.
"""
import argparse
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('exe', 'elf', 'cwd', 'stdout', 'stderr'):
        p.add_argument('--' + name, required=True)
    p.add_argument('--interactive', action='store_true')
    a = p.parse_args()
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 5 if a.interactive else 0
    with open(a.stdout, 'xb') as out, open(a.stderr, 'xb') as err:
        child = subprocess.Popen([a.exe, a.elf], cwd=a.cwd, stdout=out,
                                 stderr=err, startupinfo=startup,
                                 creationflags=subprocess.CREATE_NO_WINDOW)
        print(child.pid, flush=True)


if __name__ == '__main__':
    main()
