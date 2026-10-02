import simctl


class _Audio:
    def play(self, sample, loop=False):
        simctl.audio_sample = sample

    def stop(self):
        pass


class Peripherals:
    def __init__(self, audio_output="headphone", sample_rate=22050, bit_depth=16, i2c=None):
        self.audio = _Audio()
        self.volume = 0.5
        self._buttons = ()
