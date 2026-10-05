import simctl


def reload():
    raise simctl.Reload()


def ticks_ms():
    import time
    return (time.monotonic_ns() // 1_000_000) & ((1 << 29) - 1)
