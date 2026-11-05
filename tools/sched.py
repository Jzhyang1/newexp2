import subprocess
import heapq
import threading
import time
import readline  # Enables left/right arrow navigation and history for input()

term = False
running_proc = None
killed_procs = set()
prioq = []   # heap of important procs

def _run(cmd: list[str]):
    global running_proc
    name = cmd[0]

    if name in killed_procs:
        killed_procs.remove(name)
        return

    start = time.time()
    pinfo = subprocess.Popen(cmd, text=True)
    running_proc = (name, pinfo)
    res = pinfo.wait()
    runtime = time.time() - start
    if res:
        print(f"\033[31mProcess exited with code {res} after {runtime} seconds\033[0m")
    else:
        print(f"\033[32mProcess exited with code {res} after {runtime} seconds\033[0m")
    running_proc = None

def kill(name_or_pid: str):
    if running_proc is not None:
        running_name, running_pinfo = running_proc
        if running_name == name_or_pid or str(running_pinfo.pid) == name_or_pid:
            running_pinfo.terminate()
            running_pinfo = None
        else:
            time.sleep(1)

    killed_procs.add(name_or_pid)

def enqueue(prio: float, cmd: list[str]):
    heapq.heappush(prioq, (prio, cmd))

def show():
    prioq.sort()
    print("\033[34m", end='')
    print(running_proc)
    for item in prioq:
        print(item)
    print("\033[0m", end='')

def _bgp():
    '''background polling process'''
    while not term:
        if running_proc is None and len(prioq) > 0:
            _, proc = heapq.heappop(prioq)
            _run(proc)

if __name__ == "__main__":
    worker = threading.Thread(target=_bgp)

    cmd = input("$ ")
    worker.start()

    while cmd.lower() != "exit":
        if cmd.lower() == "show":
            show()
        elif cmd.lower() == "kill":
            if running_proc:
                kill(running_proc[1].pid)
        else:
            cmd_split = cmd.split()

            if len(cmd_split) == 0:
                print("rejected")
                continue

            try:
                cmd_prio = int(cmd_split[0])
                cmd_split = cmd_split[1:]
            except Exception:
                cmd_prio = 100

            if len(cmd_split) == 0:
                print("rejected")
                continue
            
            enqueue(cmd_prio, cmd_split)
            print("\033[34mSuccess\033[0m")
        cmd = input("$ ")
    term = True