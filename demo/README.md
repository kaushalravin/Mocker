# demo

Small, intentionally-misbehaving programs used to prove that `mocker`'s
isolation and resource limits actually hold at the kernel level, not just
according to `mocker`'s own output (CLAUDE.md section 10).

- `leak.c`: leaks 1 MB every 50 ms until the cgroup's `memory.max` gets it
  OOM-killed.
- `forkbomb.c`: forks until `fork()` is refused by `pids.max`.
- `demo-app/`: minimal test app for namespace isolation demos (not started).

Both programs are **bounded on purpose** (`leak` stops itself at 512 MB, `forkbomb`
at 500 children that expire after 20 s) so that running one on the host by mistake
can't take down the VM. If either hits its safety cap, no limit stopped it, and it
prints that, so a broken limit shows up as a failure rather than a hung machine.
Even so, run them only inside a container.

## Build and install into the test rootfs (on the Lubuntu VM)

They're linked `-static` because the BusyBox rootfs has no libc to load.

```bash
cd ~/Mocker && git pull && make && make demos
file demo/leak demo/forkbomb                   # both: "statically linked"

# HOST COMMAND: adds two files to the shared test rootfs; nothing is overwritten or deleted.
sudo cp demo/leak demo/forkbomb /root/test-rootfs/bin/
ls -l /root/test-rootfs/bin/leak /root/test-rootfs/bin/forkbomb
```

## Demo 1: memory limit (`leak`)

**Terminal A (CONTAINER COMMAND runs inside `leak1`'s namespaces and 64 MB cgroup):**

```bash
sudo ./mocker run --name leak1 --mem-mb 64 /root/test-rootfs -- /bin/leak
```

Expect progress lines counting up to about 60 MB, then output stops and `mocker run` reports
the container done (`leak` was killed by SIGKILL, so there's no "safety cap" line). It must **not**
print `reached the 512 MB safety cap`.

**Terminal B (HOST COMMAND, read-only), kernel evidence:**

```bash
cat /sys/fs/cgroup/mocker/leak1/memory.max      # 67108864
cat /sys/fs/cgroup/mocker/leak1/memory.events   # oom_kill 1  <- the kernel did it
cat /sys/fs/cgroup/mocker/leak1/memory.swap.max # 0 (swap can't hide the leak)
sudo dmesg | grep -i 'out of memory\|killed process' | tail -n 3
free -m                                         # host RAM barely moved
sudo ./mocker ps                                # host and mocker still responsive
```

Pass if:
- `memory.events` shows `oom_kill 1`.
- `dmesg` names `/mocker/leak1` (e.g. `oom_memcg=/mocker/leak1`, `constraint=CONSTRAINT_MEMCG`).
  That is a cgroup-scoped OOM, not a host-wide one.
- Host `free -m` shows no meaningful drop, and nothing else on the VM was killed.

Then clean up: `sudo ./mocker rm leak1`.

## Demo 2: pids limit (`forkbomb`)

**Terminal A:**

```bash
sudo ./mocker run --name bomb1 --pids 32 /root/test-rootfs -- /bin/forkbomb
```

Expect `fork() refused after N children: Resource temporarily unavailable (errno 11)`, with N
around 30-31 (the limit of 32 includes the forkbomb parent itself), then a 20 s hold. It must
**not** print `NEVER refused`.

**Terminal B, within the 20 s hold (HOST COMMAND, read-only):**

```bash
cat /sys/fs/cgroup/mocker/bomb1/pids.max        # 32
cat /sys/fs/cgroup/mocker/bomb1/pids.current    # 32 (the group is full)
cat /sys/fs/cgroup/mocker/bomb1/pids.events     # max N  <- forks the kernel refused
ps -e | wc -l                                   # host process count: nowhere near +500
uptime                                          # host load not spiking
```

Pass if `pids.current` is at the limit, `pids.events` shows `max` > 0, and the host is
unaffected. After the hold, `forkbomb` exits; check `mocker ps` shows `exited`, then
`sudo ./mocker rm bomb1`.

## Optional: prove the limits are what stopped it

Re-run each demo **without** the flag (`--mem-mb` or `--pids` omitted) *only* to see the safety
cap behave: `leak` should run to 512 MB and `forkbomb` to 500 children, each printing its
"NOT killed" / "NEVER refused" message. Do this with a small VM-memory margin in mind (512 MB of
RAM will be touched). Skip it if the VM has under ~1 GB free.
