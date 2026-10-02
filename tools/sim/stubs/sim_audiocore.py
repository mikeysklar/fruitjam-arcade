class RawSample:
    def __init__(self, buf, channel_count=1, sample_rate=22050, single_buffer=True):
        self.buf = buf
        self.channel_count = channel_count
