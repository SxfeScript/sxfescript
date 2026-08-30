import subprocess, time, sys, os, signal
DIR="benchmarks/wintercg"; SXN="build/release/sxn"
def run20(label, cmd, n=20):
    ts=[]
    for _ in range(n):
        t=time.perf_counter()
        subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        ts.append((time.perf_counter()-t)*1000)
    ts.sort()
    print("%-22s mean=%6.1f ms  median=%6.1f ms  min=%6.1f ms" %
          (label, sum(ts)/len(ts), ts[len(ts)//2], ts[0]))
print("== real-world end-to-end (20 launches each, interleaved) ==")
srv=subprocess.Popen([SXN, DIR+"/server.sx"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1)
try:
    for label,cmd in [("sxn",[SXN,DIR+"/realworld.sx"]),("node",["node",DIR+"/realworld.js"]),("bun",["bun",DIR+"/realworld.bun.js"])]:
        run20(label+" realworld", cmd)
finally:
    srv.terminate(); srv.wait()
print("== cold start (20 launches each) ==")
for label,cmd in [("sxn",[SXN,DIR+"/coldstart.sx"]),("node",["node",DIR+"/coldstart.js"]),("bun",["bun",DIR+"/coldstart.bun.js"])]:
    run20(label+" coldstart", cmd)
