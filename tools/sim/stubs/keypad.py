import simctl


class Event:
    def __init__(self, n, p):
        self.key_number = n
        self.pressed = p
        self.released = not p


class _Q:
    def __init__(self, keys):
        self.k = keys
        self.pend = []

    def get(self):
        if not self.pend:
            self.pend = [Event(n, p) for n, p in simctl.events_for(self.k.pins)]
        return self.pend.pop(0) if self.pend else None


class Keys:
    def __init__(self, pins, value_when_pressed=False, pull=True):
        self.pins = tuple(pins)
        self.pend = []
        self.events = _Q(self)
        simctl.keysets.append(self)

    def deinit(self):
        simctl.keysets.remove(self)
