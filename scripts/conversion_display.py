"""Conversion progress rows shared by live and plain-text output."""
from __future__ import annotations

import asyncio
from dataclasses import dataclass
from pathlib import Path
import re
import time

from rich.console import Console
from rich.live import Live
from rich.spinner import Spinner
from rich.text import Text
from rich.tree import Tree

ACTIVE_STATES = ("lifting", "compiling", "linking", "running")


@dataclass
class ProgressRow:
    state: str
    detail: str
    counter: tuple[int, int] | None
    started: float
    elapsed: float
    bytes_written: int | None


class Display:
    """ELF identity, detail, counter, then stage time; one formatter for both outputs."""
    def __init__(self, name: str, console: Console):
        self.name, self.console = name, console
        self.rows: dict[str, ProgressRow] = {}
        self.started = time.monotonic()
        self.parents: dict[str, str] = {}
        self.spinners: dict[str, Spinner] = {}
        self.live = Live(self.render(), console=console, refresh_per_second=8,
                         auto_refresh=False, vertical_overflow="visible")

    def label(self, name: str, row: ProgressRow) -> Text:
        owner = self.parents.get(name, name)
        display_name = Path(owner).name if Path(owner).is_absolute() else owner
        label = Text(f"{display_name}: {row.state}", style={
            "failed": "red", "done": "green", "cached": "dim"}.get(row.state, ""))
        if row.detail:
            label.append(f" — {row.detail}")
        if row.bytes_written is not None:
            label.append(f" [{row.bytes_written} bytes]")
        if row.counter is not None:
            completed, total = row.counter
            label.append(f" [{completed}/{total}]")
        elapsed = time.monotonic() - row.started if row.state in ACTIVE_STATES else row.elapsed
        label.append(f"  {elapsed:.1f}s", style="dim")
        return label

    def update(self, name: str, state: str, detail: str = "",
               parent: str | None = None, counter: tuple[int, int] | None = None,
               bytes_written: int | None = None) -> None:
        if re.search(r" chunk \d+/\d+$", name):
            return
        previous = self.rows.get(name)
        now = time.monotonic()
        same_stage = previous is not None and previous.state == state and (
            state != "lifting" or previous.detail == detail)
        started = previous.started if same_stage else now
        elapsed = (now - previous.started if previous and previous.state in ACTIVE_STATES
                   else 0.0)
        row = ProgressRow(state, detail, counter, started, elapsed, bytes_written)
        self.rows[name] = row
        if parent:
            self.parents[name] = parent
        if not self.console.is_terminal and (
                previous is None or state != previous.state or state in ("failed", "done", "cached")
                or (state == "lifting" and (detail, counter, bytes_written) !=
                    (previous.detail, previous.counter, previous.bytes_written))):
            self.console.print(self.label(name, row), soft_wrap=True)

    def render(self) -> Tree:
        done = sum(row.state in ("done", "cached") for row in self.rows.values())
        tree = Tree(Text(f"{self.name} [{done}/{len(self.rows)}]  "
                         f"{time.monotonic() - self.started:.1f}s"))
        parent_names = set(self.parents.values())
        nodes = {}
        for name, row in self.rows.items():
            label = self.label(name, row)
            if row.state in ACTIVE_STATES:
                if name not in self.spinners:
                    self.spinners[name] = Spinner("dots")
                node = self.spinners[name]
                node.update(text=label)
            else:
                node = label
            nodes[name] = Tree(node) if name in parent_names else node
        for name, node in nodes.items():
            parent = self.parents.get(name)
            if parent in nodes:
                nodes[parent].add(node)
            else:
                tree.add(node)
        return tree

    async def refresh(self) -> None:
        while True:
            if self.console.is_terminal:
                self.live.update(self.render(), refresh=True)
            await asyncio.sleep(0.1)
