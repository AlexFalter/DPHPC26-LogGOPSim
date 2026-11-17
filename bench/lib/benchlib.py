"""Shared helpers of the benchmark harness (standard library only, Python >= 3.8).

Statistics follow Hoefler & Belli, "Scientific Benchmarking of Parallel Computing Systems"
(SC'15): medians with nonparametric confidence intervals, no assumption of normality.
"""
from __future__ import annotations

import ctypes
import ctypes.util
import hashlib
import json
import math
import os
import platform
import random
import re
import shutil
import signal
import socket
import subprocess
import threading
import time
from pathlib import Path

BENCH = Path(__file__).resolve().parent.parent
REPO = BENCH.parent


# ------------------------------------------------------------------ paths and config

def work_dir() -> Path:
    """Builds, worktrees and (by default) traces. BENCH_WORK overrides bench/.work."""
    return Path(os.environ.get("BENCH_WORK", BENCH / ".work")).resolve()


def results_dir() -> Path:
    """Campaign data, tables and plots. BENCH_RESULTS overrides bench/results."""
    return Path(os.environ.get("BENCH_RESULTS", BENCH / "results")).resolve()


def traces_dir() -> Path:
    return Path(os.environ.get("BENCH_TRACES", work_dir() / "traces")).resolve()


def build_dir(name: str) -> Path:
    return work_dir() / "build" / name


def load_config(path) -> dict:
    """JSON config (YAML too if PyYAML is installed). 'include' merges other files first; the
    campaign name is the top-level file's 'name' or stem."""
    path = Path(path)
    merged = _load_config(path)
    merged["name"] = merged.get("name") or path.stem
    return merged


def _load_config(path: Path) -> dict:
    text = path.read_text()
    if path.suffix in (".yaml", ".yml"):
        import yaml  # optional dependency
        data = yaml.safe_load(text)
    else:
        data = json.loads(text)
    merged = {}
    for inc in data.pop("include", []):
        included = _load_config(path.parent / inc)
        included.pop("name", None)
        deep_merge(merged, included)
    deep_merge(merged, data)
    return merged


def deep_merge(dst: dict, src: dict) -> dict:
    for key, value in src.items():
        if isinstance(value, dict) and isinstance(dst.get(key), dict):
            deep_merge(dst[key], value)
        else:
            dst[key] = value
    return dst


def records_label(n) -> str:
    n = int(float(n))
    exp = int(math.floor(math.log10(n))) if n > 0 else 0
    mant = n / 10 ** exp
    return f"{mant:g}e{exp}" if mant != 1 else f"1e{exp}"


def trace_id(workload: str, ranks: int, records, schema: str) -> str:
    return f"{workload}-p{ranks}-n{records_label(records)}-{schema}"


# ------------------------------------------------------------------ statistics

def median(xs):
    s = sorted(xs)
    n = len(s)
    if n == 0:
        return float("nan")
    return s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2


def median_ci(xs, confidence=0.95):
    """Distribution-free CI of the median from order statistics (exact binomial; Le Boudec,
    "Performance Evaluation of Computer and Communication Systems", Thm. 2.1). Returns
    (median, lo, hi, exact) where exact is False if n is too small for the confidence level
    (then lo/hi are min/max)."""
    s = sorted(xs)
    n = len(s)
    if n == 0:
        return float("nan"), float("nan"), float("nan"), False
    m = median(s)
    alpha = 1 - confidence
    # largest j with P(Bin(n, 1/2) < j) <= alpha/2; CI = [x_(j), x_(n-j+1)] (1-based)
    cdf, j = 0.0, 0
    for k in range(n + 1):
        p = math.comb(n, k) / 2 ** n
        if cdf + p > alpha / 2:
            break
        cdf += p
        j = k + 1
    if j < 1:
        return m, s[0], s[-1], False
    return m, s[j - 1], s[n - j], True


def ci_converged(xs, rel=0.05, confidence=0.95) -> bool:
    m, lo, hi, exact = median_ci(xs, confidence)
    return exact and m > 0 and lo >= (1 - rel) * m and hi <= (1 + rel) * m


def bootstrap_ratio_ci(a, b, reps=2000, confidence=0.95, seed=1):
    """Percentile-bootstrap CI of median(a)/median(b) (independent samples)."""
    if not a or not b:
        return float("nan"), float("nan"), float("nan")
    rng = random.Random(seed)
    ratios = []
    for _ in range(reps):
        ra = median([rng.choice(a) for _ in a])
        rb = median([rng.choice(b) for _ in b])
        if rb:
            ratios.append(ra / rb)
    ratios.sort()
    lo = ratios[int((1 - confidence) / 2 * len(ratios))]
    hi = ratios[min(len(ratios) - 1, int((1 + confidence) / 2 * len(ratios)))]
    return median(a) / median(b), lo, hi


# ------------------------------------------------------------------ hashing

def sha256_file(path, block=1 << 24) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(block)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def self_describing_header(path):
    """(header_bytes, record_bits) of a self-describing trace, or None for other files.
    The format has no schema hash; the harness uses SHA-256 of the header as one."""
    with open(path, "rb") as f:
        prefix = f.read(43)
        if len(prefix) < 43 or prefix[:8] != b"GOALSD01":
            return None
        header_len = int.from_bytes(prefix[10:18], "little")
        record_bits = int.from_bytes(prefix[18:26], "little")
        f.seek(0)
        return f.read(header_len), record_bits


# ------------------------------------------------------------------ machine metadata

def _read(path, default=None):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return default


def _run(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=20).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None


def cpu_freq_info(cpu: int) -> dict:
    base = f"/sys/devices/system/cpu/cpu{cpu}/cpufreq"
    return {k: _read(f"{base}/{k}") for k in ("scaling_governor", "scaling_cur_freq", "scaling_min_freq",
                                               "scaling_max_freq", "cpuinfo_max_freq", "scaling_driver")}


def machine_info() -> dict:
    """Everything that identifies the machine and its configuration (once per campaign)."""
    affinity = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else []
    meminfo = _read("/proc/meminfo", "")
    mem = dict(re.findall(r"^(\w+):\s+(\d+) kB", meminfo, flags=re.M))
    info = {
        "hostname": socket.gethostname(),
        "platform": platform.platform(),
        "kernel": platform.release(),
        "os_release": _read("/etc/os-release"),
        "python": platform.python_version(),
        "lscpu": _run(["lscpu"]),
        "nproc_online": os.cpu_count(),
        "affinity": affinity,
        "mem_total_kb": int(mem.get("MemTotal", 0)),
        "swap_total_kb": int(mem.get("SwapTotal", 0)),
        "numa_nodes": sorted(p.name for p in Path("/sys/devices/system/node").glob("node[0-9]*")),
        "thp_enabled": _read("/sys/kernel/mm/transparent_hugepage/enabled"),
        "intel_no_turbo": _read("/sys/devices/system/cpu/intel_pstate/no_turbo"),
        "cpufreq_boost": _read("/sys/devices/system/cpu/cpufreq/boost"),
        "cpufreq": {str(c): cpu_freq_info(c) for c in affinity[:64]},
        "cpu_capacity": {str(c): _read(f"/sys/devices/system/cpu/cpu{c}/cpu_capacity") for c in affinity[:64]},
        "is_wsl": "microsoft" in (_read("/proc/version", "") or "").lower(),
        "is_root": os.geteuid() == 0 if hasattr(os, "geteuid") else False,
        "cgroup": _read("/proc/self/cgroup"),
        "cgroup_memory_max": _cgroup_memory_max(),
        "perf_event_paranoid": _read("/proc/sys/kernel/perf_event_paranoid"),
        "slurm": {k: v for k, v in os.environ.items() if k.startswith("SLURM_")},
        "loadavg": os.getloadavg() if hasattr(os, "getloadavg") else None,
        "date_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }
    return info


def _cgroup_memory_max():
    rel = None
    for line in (_read("/proc/self/cgroup", "") or "").splitlines():
        parts = line.split(":", 2)
        if len(parts) == 3 and parts[0] == "0":
            rel = parts[2]
    if rel is None:
        return None
    return _read(f"/sys/fs/cgroup{rel}/memory.max")


def fs_type(path) -> str | None:
    out = _run(["stat", "-f", "-c", "%T", str(path)])
    return out or None


def pick_cpus(count=1, prefer=None):
    """CPUs to pin to, within this process's affinity mask: the given ones, else the fastest
    cores (highest cpuinfo_max_freq / cpu_capacity, which matters on big.LITTLE parts like the
    GB10), skipping CPU 0 and SMT siblings when possible."""
    allowed = sorted(os.sched_getaffinity(0))
    if prefer:
        chosen = [c for c in prefer if c in allowed]
        if len(chosen) >= count:
            return chosen[:count]

    def speed(c):
        cap = _read(f"/sys/devices/system/cpu/cpu{c}/cpu_capacity")
        freq = _read(f"/sys/devices/system/cpu/cpu{c}/cpufreq/cpuinfo_max_freq")
        return (int(cap) if cap and cap.isdigit() else 0, int(freq) if freq and freq.isdigit() else 0)

    seen_cores, primary, secondary = set(), [], []
    for c in allowed:
        sib = _read(f"/sys/devices/system/cpu/cpu{c}/topology/thread_siblings_list") or str(c)
        (secondary if sib in seen_cores else primary).append(c)
        seen_cores.add(sib)
    best = max((speed(c) for c in allowed), default=(0, 0))
    fast = [c for c in primary if speed(c) == best and c != 0] or [c for c in primary if speed(c) == best]
    rest = [c for c in primary + secondary if c not in fast]
    ordered = fast + rest
    return ordered[:count]


def numa_node_of(cpu: int):
    for node in Path("/sys/devices/system/node").glob("node[0-9]*"):
        cpulist = _read(node / "cpulist", "")
        if cpu in parse_cpulist(cpulist):
            return int(node.name[4:])
    return None


def parse_cpulist(text):
    cpus = set()
    for part in (text or "").split(","):
        if "-" in part:
            a, b = part.split("-")
            cpus.update(range(int(a), int(b) + 1))
        elif part.strip():
            cpus.add(int(part))
    return cpus


# ------------------------------------------------------------------ page cache

_libc = None


def _get_libc():
    global _libc
    if _libc is None:
        _libc = ctypes.CDLL(ctypes.util.find_library("c") or None, use_errno=True)
        _libc.mmap.restype = ctypes.c_void_p
        _libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                               ctypes.c_long]
        _libc.munmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        _libc.mincore.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
    return _libc


def resident_fraction(path) -> float | None:
    """Fraction of the file's pages in the page cache (mincore), or None if unavailable."""
    try:
        size = os.path.getsize(path)
        if size == 0:
            return 1.0
        libc = _get_libc()
        fd = os.open(path, os.O_RDONLY)
        try:
            addr = libc.mmap(None, size, 1, 1, fd, 0)  # PROT_READ, MAP_SHARED
        finally:
            os.close(fd)
        if addr in (None, ctypes.c_void_p(-1).value):
            return None
        try:
            page = os.sysconf("SC_PAGE_SIZE")
            pages = (size + page - 1) // page
            vec = (ctypes.c_ubyte * pages)()
            if libc.mincore(addr, size, vec) != 0:
                return None
            raw = bytes(vec)  # Linux sets only bit 0 (resident); the other bits are reserved and 0
            return (len(raw) - raw.count(0)) / pages
        finally:
            libc.munmap(addr, size)
    except (OSError, AttributeError, ValueError):
        return None


def drop_page_cache(paths, method="auto") -> str:
    """Evicts the files from the page cache. 'auto': drop_caches if root, else posix_fadvise
    (DONTNEED), which needs no privileges and works for clean pages of unmapped files."""
    if method in ("auto", "drop_caches") and os.geteuid() == 0:
        os.sync()
        Path("/proc/sys/vm/drop_caches").write_text("3\n")
        return "drop_caches"
    for path in paths:
        fd = os.open(path, os.O_RDONLY)
        try:
            os.fdatasync(fd)
            os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
        finally:
            os.close(fd)
    return "posix_fadvise_dontneed"


def warm_page_cache(paths):
    for path in paths:
        with open(path, "rb") as f:
            while f.read(1 << 24):
                pass


# ------------------------------------------------------------------ measured process

_STATUS_KEYS = ("VmRSS", "VmHWM", "RssAnon", "RssFile", "RssShmem", "VmSwap")


def _sample_status(pid, peaks, stop, interval):
    path = f"/proc/{pid}/status"
    while not stop.is_set():
        try:
            text = Path(path).read_text()
        except OSError:
            break
        for key in _STATUS_KEYS:
            m = re.search(rf"^{key}:\s+(\d+) kB", text, flags=re.M)
            if m:
                v = int(m.group(1))
                if v > peaks.get(key, -1):
                    peaks[key] = v
        stop.wait(interval)


def _exit_code(status):
    if os.WIFSIGNALED(status):
        return -os.WTERMSIG(status)
    return os.WEXITSTATUS(status)


def run_measured(cmd, env=None, cwd=None, cpus=None, membind=None, timeout=None, stdout_path=None,
                 stderr_path=None, sample_interval=0.02):
    """Runs cmd once and measures it: wall time (perf_counter_ns around fork..wait), the child's
    own rusage via wait4 (max RSS, CPU times, page faults, context switches) and peak
    RssAnon/RssFile sampled from /proc (anonymous memory cannot be reclaimed; file pages can).
    Pins to cpus (sched_setaffinity) and, if membind is given and numactl exists, binds memory."""
    if membind is not None and shutil.which("numactl"):
        cmd = ["numactl", f"--membind={membind}"] + list(cmd)
    out = open(stdout_path, "wb") if stdout_path else subprocess.DEVNULL
    err = open(stderr_path, "wb") if stderr_path else subprocess.DEVNULL

    def preexec():
        if cpus:
            os.sched_setaffinity(0, cpus)

    peaks, stop = {}, threading.Event()
    timed_out = [False]
    try:
        t0 = time.perf_counter_ns()
        proc = subprocess.Popen(cmd, env=env, cwd=cwd, stdout=out, stderr=err, preexec_fn=preexec)
        sampler = threading.Thread(target=_sample_status, args=(proc.pid, peaks, stop, sample_interval),
                                   daemon=True)
        sampler.start()
        killer = None
        if timeout:
            def kill():
                timed_out[0] = True
                try:
                    os.kill(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            killer = threading.Timer(timeout, kill)
            killer.start()
        _, status, ru = os.wait4(proc.pid, 0)
        t1 = time.perf_counter_ns()
        proc.returncode = _exit_code(status)
        if killer:
            killer.cancel()
        stop.set()
        sampler.join(1)
    finally:
        if stdout_path:
            out.close()
        if stderr_path:
            err.close()
    return {
        "exit_code": proc.returncode,
        "timed_out": timed_out[0],
        "wall_ns": t1 - t0,
        "utime_s": ru.ru_utime,
        "stime_s": ru.ru_stime,
        "maxrss_kb": ru.ru_maxrss,
        "minflt": ru.ru_minflt,
        "majflt": ru.ru_majflt,
        "inblock": ru.ru_inblock,
        "nvcsw": ru.ru_nvcsw,
        "nivcsw": ru.ru_nivcsw,
        "peak_rss_anon_kb": peaks.get("RssAnon"),
        "peak_rss_file_kb": peaks.get("RssFile"),
        "peak_vmrss_kb": peaks.get("VmRSS"),
        "peak_swap_kb": peaks.get("VmSwap"),
    }


# ------------------------------------------------------------------ LogGOPSim output

_SIZE_RE = re.compile(r"size: (\d+) \((\d+) CPUs, (\d+) NICs\); L=(-?\d+), o=(-?\d+) g=(-?\d+), G=(-?\d+), "
                      r"O=(-?\d+), P=(\d+), S=(\d+)")
_PERF_RE = re.compile(r"PERFORMANCE: Processes: (\d+)\s+Events: (\d+)\s+Time: (\d+) s\s+Speed: (\S+) ev/s")
_HOST_RE = re.compile(r"^Host (\d+): (\d+)$", re.M)
_MAX_RE = re.compile(r"^Maximum finishing time at host (\d+): (\d+) ", re.M)


def parse_loggopsim(stdout: str, stderr: str = "") -> dict:
    """Parses LogGOPSim's output. 'result_lines' are the deterministic lines (sizes, queue
    leftovers, finish times) that correctness compares; timing lines are excluded."""
    res = {}
    m = _SIZE_RE.search(stdout)
    if m:
        res.update(processes=int(m.group(1)), cpus=int(m.group(2)), nics=int(m.group(3)),
                   params={"L": int(m.group(4)), "o": int(m.group(5)), "g": int(m.group(6)), "G": int(m.group(7)),
                           "O": int(m.group(8)), "P": int(m.group(9)), "S": int(m.group(10))})
    m = _PERF_RE.search(stdout)
    if m:
        res.update(events=int(m.group(2)), sim_time_s_printed=int(m.group(3)))
        try:
            res["speed_evps_printed"] = float(m.group(4))
        except ValueError:
            res["speed_evps_printed"] = None
    hosts = [(int(h), int(t)) for h, t in _HOST_RE.findall(stdout)]
    if hosts:
        res["host_times"] = [t for _, t in sorted(hosts)]
        res["max_time"] = max(t for _, t in hosts)
    m = _MAX_RE.search(stdout)
    if m:
        res["max_host"], res["max_time"] = int(m.group(1)), int(m.group(2))
    lines = []
    for line in stdout.splitlines():
        # "The schedule will be invalid after this simulation!" (--save-mem) is a notice, not a result.
        if line.startswith(("size:", "Host ", "Maximum finishing", "unexpected queue", "receive queue", " src:")):
            lines.append(line.rstrip())
    res["result_lines"] = lines
    res["result_sha256"] = hashlib.sha256("\n".join(lines).encode()).hexdigest()
    for line in stderr.splitlines():
        if line.startswith("PHASES "):
            try:
                res["phases"] = json.loads(line[7:])
            except json.JSONDecodeError:
                res["phases_error"] = line
        elif line.startswith("Trace decoder: "):
            res["decoder_actual"] = line[len("Trace decoder: "):].strip()
        elif "is unavailable for this trace" in line:
            res["decoder_fallback"] = line.strip()
        elif line.startswith("LogGOPSim: "):
            res["error"] = line.strip()
    return res


def loggopsim_args(params: dict):
    """CLI flags for the LogGOPS parameters (identical for BASE and NEW)."""
    flags = {"L": "-L", "o": "-o", "g": "-g", "G": "-G", "O": "-O", "S": "-S"}
    args = []
    for key, flag in flags.items():
        if key in params:
            args += [flag, str(params[key])]
    return args


# ------------------------------------------------------------------ campaigns

def new_campaign_dir(root: Path, name: str) -> Path:
    stamp = time.strftime("%Y%m%d-%H%M%S")
    host = socket.gethostname().split(".")[0]
    path = root / f"{stamp}-{host}-{name}"
    suffix = 1
    while path.exists():  # raw data is never overwritten
        path = root / f"{stamp}-{host}-{name}-{suffix}"
        suffix += 1
    path.mkdir(parents=True)
    return path


def load_build_manifest(name: str) -> dict:
    path = build_dir(name) / "bench-build.json"
    if not path.exists():
        raise SystemExit(f"build '{name}' missing: run bench/build.sh (expected {path})")
    return json.loads(path.read_text())


def load_trace_manifest(directory: Path | None = None) -> dict:
    directory = directory or traces_dir()
    path = directory / "manifest.json"
    if not path.exists():
        raise SystemExit(f"no trace manifest at {path}: run bench/gen_traces.py first")
    return json.loads(path.read_text())


def write_json(path, data):
    tmp = Path(str(path) + ".tmp")
    tmp.write_text(json.dumps(data, indent=2, sort_keys=True, default=str) + "\n")
    tmp.replace(path)
