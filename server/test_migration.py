import pathlib
import tempfile
import unittest
from migrate import migrate


class MigrationTests(unittest.TestCase):
    def test_preserves_credentials_photos_and_originals(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            server = root / 'server'
            server.mkdir()
            (root / 'appsettings.py').write_text('API_KEY = "private"')
            (root / 'client_secret.json').write_text('{}')
            (root / 'faces' / 'Alice').mkdir(parents=True)
            (root / 'faces' / 'Alice' / 'photo.jpg').write_bytes(b'photo')
            migrate(server)
            migrate(server)
            self.assertEqual((server / 'faces' / 'Alice' / 'photo.jpg').read_bytes(), b'photo')
            self.assertTrue((root / 'appsettings.py').exists())
            self.assertEqual((server / 'appsettings.py').stat().st_mode & 0o777, 0o600)
            (server / 'appsettings.py').write_text('API_KEY = "new"')
            migrate(server)  # Completed migration never overwrites later settings.
            (server / ".legacy-migrated").unlink()
            with self.assertRaises(RuntimeError):
                migrate(server)
            self.assertEqual((server / 'appsettings.py').read_text(), 'API_KEY = "new"')
