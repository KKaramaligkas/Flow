#!/usr/bin/env python3
"""Embed the audited DOM bootstrap; called by host and PSP builds."""
from pathlib import Path
import sys
source = Path(sys.argv[1]).read_bytes()
Path(sys.argv[2]).write_text('/* Generated from dom_bootstrap.js. */\nstatic const char dom_bootstrap[] = {' + ','.join(map(str, source)) + ',0};\n')
