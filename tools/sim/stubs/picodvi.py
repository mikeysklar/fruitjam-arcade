import array
import simctl


def Framebuffer(w, h, color_depth=16, **pins):
    # A plain array so native modules get a real buffer, like the board's.
    simctl.fb = array.array("H" if color_depth == 16 else "B", bytes(w * h * (color_depth // 8)))
    return simctl.fb
