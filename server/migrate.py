"""Copy legacy Pi data into server/ without overwriting either installation.

The original files remain usable until systemd has switched to the new paths.
"""
import filecmp
import pathlib
import shutil


def copy_missing(source, destination):
    if source.is_dir():
        destination.mkdir(parents=True, exist_ok=True)
        for child in source.iterdir():
            copy_missing(child, destination / child.name)
    elif destination.exists():
        if not filecmp.cmp(source, destination, shallow=False):
            raise RuntimeError(f'Both old and new copies differ: {destination.name}. Resolve before installing.')
    else:
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)


def migrate(server_dir):
    marker = server_dir / ".legacy-migrated"
    if marker.exists():
        return
    project = server_dir.parent
    for name in ['appsettings.py', 'faces', 'models']:
        source = project / name
        if source.exists():
            copy_missing(source, server_dir / name)
    for source in project.glob('client_secret*.json'):
        copy_missing(source, server_dir / source.name)
    for name in ['appsettings.py', *[p.name for p in server_dir.glob('client_secret*.json')]]:
        path = server_dir / name
        if path.exists():
            path.chmod(0o600)
    marker.touch(mode=0o600)


if __name__ == '__main__':
    migrate(pathlib.Path(__file__).resolve().parent)
