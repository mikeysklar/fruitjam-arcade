import simctl


class Label:
    def __init__(self, font, text="", color=0, x=0, y=0, scale=1):
        self.text = text
        self.color = color
        self.x = x
        self.y = y
        simctl.labels.append(self)
