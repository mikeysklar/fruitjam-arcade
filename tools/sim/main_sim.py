# Runs the launcher's code.py under the unix port. Exit 3 = supervisor.reload().
import sys
import simctl

src = open(simctl.ROOT + "/code.py").read()
try:
    exec(src, {"__name__": "__main__"})
    print("SIM: code.py returned")
    simctl.dump()
except simctl.Reload:
    print("SIM: reload")
    simctl.dump()
    sys.exit(3)
except simctl.Done:
    print("SIM: done")
    simctl.dump()
