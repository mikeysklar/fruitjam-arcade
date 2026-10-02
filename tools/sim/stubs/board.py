for _n in ("A1 A2 A3 A4 A5 D6 D7 D8 D9 D10 BUTTON1 BUTTON2 BUTTON3 CKP CKN D0P D0N D1P D1N "
           "D2P D2N PERIPH_RESET").split():
    globals()[_n] = _n


def I2C():
    return None
