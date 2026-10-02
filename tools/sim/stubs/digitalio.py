class DigitalInOut:
    def __init__(self, pin):
        self.value = False

    def switch_to_output(self, value=False):
        self.value = value
