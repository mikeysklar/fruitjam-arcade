# Test control for the unix-port simulation. Not used on the board.
import os
import time

ROOT = os.getenv("FJ_ROOT") or "."
RUN = int(os.getenv("FJ_RUN") or "0")
T0 = time.time_ns()
fb = None
labels = []
keysets = []
_script = []
_end_ms = 10_000


class Reload(Exception):
    pass


class Done(Exception):
    pass


def now_ms():
    return (time.time_ns() - T0) // 1_000_000


def _load():
    global _end_ms
    run = -1
    try:
        f = open(os.getenv("FJ_SCRIPT"))
    except (OSError, TypeError):
        return
    for line in f:
        p = line.split("#", 1)[0].split()
        if not p:
            continue
        if p[0] == "run":
            run = int(p[1])
        elif run == RUN:
            if p[0] == "end":
                _end_ms = int(p[1])
            else:
                _script.append((int(p[0]), p[1], p[2] == "1"))
    f.close()


_load()


def events_for(pins):
    """Scripted (key_number, pressed) events now due for this key set."""
    t = now_ms()
    if t >= _end_ms:
        raise Done()
    while _script and _script[0][0] <= t:
        _, pin, pressed = _script.pop(0)
        for ks in keysets:
            if pin in ks.pins:
                ks.pend.append((ks.pins.index(pin), pressed))
    for ks in keysets:
        if ks.pins is pins:
            out = ks.pend
            ks.pend = []
            return out
    return []


ALL_PINS = ("A1", "A2", "A3", "A4", "A5", "D6", "D7", "D8", "D9", "D10",
            "BUTTON1", "BUTTON2", "BUTTON3")


def dump():
    if fb is not None:
        with open(ROOT + "/fb_run%d.bin" % RUN, "wb") as f:
            f.write(bytes(fb))
    with open(ROOT + "/labels_run%d.txt" % RUN, "w") as f:
        for lb in labels:
            if lb.text:
                f.write(lb.text + "\n")
