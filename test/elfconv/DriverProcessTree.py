#!/usr/bin/env python3
"""Create a real subprocess tree and reap the child on group termination."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys

child = subprocess.Popen(["sleep", "60"])


def terminate(signum, frame):
    child.wait(timeout=3)
    sys.exit(0)


signal.signal(signal.SIGTERM, terminate)
path = Path(sys.argv[1])
temporary = path.with_suffix(".tmp")
temporary.write_text(json.dumps([os.getpid(), child.pid]))
temporary.replace(path)
child.wait()
