#!/usr/bin/env python3
"""Import shim for scripts/ai-workflow when executed as plain files."""
from __future__ import annotations

import importlib.util
from pathlib import Path

_ROOT = Path(__file__).resolve().parent / "ai-workflow"
_COMMON = _ROOT / "common.py"

spec = importlib.util.spec_from_file_location("ai_workflow_common", _COMMON)
if spec is None or spec.loader is None:
    raise ImportError(f"cannot load {_COMMON}")
common = importlib.util.module_from_spec(spec)
spec.loader.exec_module(common)
