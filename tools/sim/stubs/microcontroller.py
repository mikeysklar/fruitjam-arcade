import simctl

_PATH = simctl.ROOT + "/nvm.bin"


class _NVM:
    def __init__(self):
        try:
            with open(_PATH, "rb") as f:
                self.b = bytearray(f.read())
        except OSError:
            self.b = bytearray(b"\xff" * 4096)

    def __len__(self):
        return len(self.b)

    def __getitem__(self, i):
        return self.b[i]

    def __setitem__(self, i, v):
        self.b[i] = v
        with open(_PATH, "wb") as f:
            f.write(self.b)


nvm = _NVM()
