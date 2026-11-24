import os
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory() as root:
    result = subprocess.run(sys.argv[1:], cwd=root, env=dict(os.environ, X2W_STORAGE_PATH="."))
    sys.exit(result.returncode)
