#!/usr/bin/env python3
"""Exercise bounded scheduling and atomic publications without launching games."""
import importlib.util
from pathlib import Path
import tempfile
import threading
import time
import json
import sys

spec = importlib.util.spec_from_file_location("stage_sweep", Path(__file__).parents[1] / "tools/stage_sweep.py")
sweep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sweep)
lock = threading.Lock()
active = maximum = finished = 0
seen = []

def action(stage, scenario):
    global active, maximum
    with lock:
        active += 1
        maximum = max(maximum, active)
    time.sleep(.025)
    with lock:
        seen.append((stage, scenario))
        active -= 1

def complete():
    global finished
    finished += 1

chosen = [(i, i + 1) for i in range(9)]
sweep.run_selected(chosen, 3, action, complete)
assert maximum == 3 and active == 0 and finished == 9 and sorted(seen) == chosen
with tempfile.TemporaryDirectory() as temporary:
    result = Path(temporary) / 'result.json'
    stop = threading.Event()
    failures = []
    def reader():
        while not stop.is_set():
            try:
                if result.exists():
                    value = json.loads(result.read_text())
                    assert len(value['payload']) == 500
            except Exception as error:
                failures.append(error)
    thread = threading.Thread(target=reader)
    thread.start()
    for number in range(50):
        sweep.write_result(result, {'number': number, 'payload': 'x' * 500})
    stop.set()
    thread.join()
    assert not failures, failures
    assert json.loads(result.read_text())['number'] == 49
started = []
barrier = threading.Barrier(3)
def failing(stage, scenario):
    started.append(stage)
    barrier.wait(timeout=2)
    if stage == 0:
        raise RuntimeError('infrastructure failure')
    time.sleep(.05)
try:
    sweep.run_selected(chosen, 3, failing, lambda: None)
    raise AssertionError('failure was swallowed')
except RuntimeError as error:
    assert str(error) == 'infrastructure failure'
assert sorted(started) == [0, 1, 2], started
print('Bounded scheduling, failure stop/join and atomic result publication passed.')
