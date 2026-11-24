import os
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory() as root:
    env = dict(os.environ, X2W_STORAGE_PATH=".", X2W_FAKE_USER="1")
    result = subprocess.run(sys.argv[1:], cwd=root, env=env)
    sys.exit(result.returncode)
