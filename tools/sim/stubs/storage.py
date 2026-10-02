# Stand-in for CircuitPython's storage module in the unix-port simulation.
# The sim's /sd is a folder that already holds the card's files, so report it as mounted.


def getmount(path):
    return object()
