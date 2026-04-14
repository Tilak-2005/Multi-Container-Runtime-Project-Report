# Multi-Container-Runtime-Project-Report
Team Members:
# Name	SRN
# Tilak Kumta	PES1UG24CS696
# Arjun Jagli	PES1UG24CS717

Course: Operating Systems
Date: April 2026
# 1. Introduction

This project implements a lightweight Linux container runtime in C with a long-running parent supervisor and a kernel-space memory monitor. The runtime manages multiple containers concurrently, coordinates logging through a bounded buffer, provides a CLI interface over UNIX domain sockets, and enforces memory limits via a Linux Kernel Module (LKM).
2. Build and Run Instructions
2.1 Prerequisites
bash

sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)

2.2 Preflight Check
bash

chmod +x environment-check.sh
sudo ./environment-check.sh

2.3 Prepare Alpine Root Filesystem
bash

mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
sudo tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

2.4 Build the Project
bash

make clean
make all

2.5 Create Per-Container Rootfs and Copy Workloads
bash

sudo cp -a ./rootfs-base ./rootfs-alpha
sudo cp -a ./rootfs-base ./rootfs-beta

sudo cp workload_cpu workload_io workload_mem ./rootfs-alpha/
sudo cp workload_cpu workload_io ./rootfs-beta/

2.6 Load Kernel Module and Start Supervisor

Terminal 1:
bash

sudo insmod monitor.ko
sudo ./engine supervisor ./rootfs-base

2.7 Run CLI Commands (Terminal 2)
bash

sudo ./engine start demo1 ./rootfs-alpha /bin/sleep 60
sudo ./engine start demo2 ./rootfs-beta /bin/sleep 60
sudo ./engine ps
sudo ./engine logs demo1
sudo ./engine stop demo1

2.8 Teardown
bash

# Terminal 1: Ctrl+C
sudo rmmod monitor

3. Demo with Screenshots
### Screenshot 1: Multi-container Supervision
![Screenshot 1](screenshots/1.png)zzz
Caption: Two containers (demo1, demo2) running simultaneously under one supervisor process. Both appear in ps output with STATE=running, demonstrating the supervisor's ability to manage multiple isolated containers concurrently.
### Screenshot 2: Metadata Tracking
![Screenshot 2](screenshots/2.png)

Caption: Output of engine ps showing tracked container metadata including Container ID, Host PID, Start Time, State, Soft/Hard Memory Limits, and Nice Value. The supervisor maintains this metadata in memory with proper synchronization.
### Screenshot 3: Bounded-buffer Logging
![Screenshot 3](screenshots/3.png)

Caption: Container stdout and stderr captured via pipes, passed through a bounded ring buffer with producer-consumer threads, timestamped with stream identifiers ([stdout]/[stderr]), and written to persistent per-container log files. Demonstrates no data loss and correct stream separation.
### Screenshot 4: CLI and IPC
![Screenshot 4](screenshots/4.png)

Caption: CLI commands (engine start) sent to the supervisor over a UNIX domain socket (Path B IPC). The supervisor processes requests and responds with confirmation messages (OK started). Supervisor also logs container lifecycle events to stderr.
### Screenshot 5: Soft-limit Warning
![Screenshot 5](screenshots/5.png)


Caption: Kernel module (cmon) logs a warning via dmesg when container RSS reaches the configured soft limit (30 MiB). The warning is advisory—the container continues running, giving the application an opportunity to self-regulate memory usage.
### Screenshot 6: Hard-limit Enforcement
![Screenshot 6](screenshots/6.png)

Caption: When container RSS reaches the hard limit (64 MiB), the kernel module sends SIGKILL to terminate the process. The supervisor correctly classifies the termination as hard_limit_killed in its metadata, distinguishing it from normal exits or manual stops.
### Screenshot 7: Scheduling Experiment
![Screenshot 7](screenshots/7.1.png)

Caption: Comparison of CPU-bound (workload_cpu) and I/O-bound (workload_io) workloads. The CPU workload completes in 0.535 seconds of continuous computation, while the I/O workload takes 40.281 seconds due to frequent blocking on disk fdatasync operations. This demonstrates the Linux CFS scheduler's efficient handling of mixed workload types—the CPU-bound process runs while the I/O-bound process is blocked.
### Screenshot 8: Clean Teardown
![Screenshot 8](screenshots/8.png)

Caption: Complete cleanup verification: supervisor performs orderly shutdown of all containers, kernel module unloads cleanly, no engine processes remain, socket file is removed, and device node is cleaned up. The system returns to a pristine state with no resource leaks or zombie processes.
4. Engineering Analysis
4.1 Isolation Mechanisms

The runtime achieves process and filesystem isolation through Linux namespaces and chroot:
Namespace	Flag	Purpose
PID	CLONE_NEWPID	Container processes get private PID space starting at 1. Prevents containers from seeing or signaling host processes by PID.
UTS	CLONE_NEWUTS	Allows container to set its own hostname via sethostname() without affecting host or other containers.
Mount	CLONE_NEWNS	Isolates filesystem mount points. chroot() re-roots the container's view to its private rootfs directory.

What the kernel still shares: Network stack (no CLONE_NEWNET), physical CPU and memory, system time, and device files. All processes share the same CFS scheduler run queues and global page allocator.

chroot vs pivot_root: chroot changes the root directory but a privileged process can escape. pivot_root is more secure. We use chroot for simplicity with the acknowledged tradeoff.
4.2 Supervisor and Process Lifecycle

A long-running supervisor is necessary because:

    SIGCHLD Reaping: Linux delivers SIGCHLD only to the direct parent. A persistent supervisor that is parent to all container processes can reliably reap them and capture exit status.

    Metadata Persistence: Container state must outlive any single CLI invocation. Storing it in the supervisor's address space is the simplest correct solution.

    Ordered Shutdown: The supervisor can iterate all running containers, send SIGTERM, wait, then SIGKILL, ensuring nothing is left behind.

Process creation uses clone() (not fork + exec) to atomically specify namespace flags at creation time.
4.3 IPC, Threads, and Synchronization

Two IPC Paths:
Path	Mechanism	Purpose
A (Logging)	pipe(2)	Container stdout/stderr → supervisor
B (Control)	UNIX domain socket	CLI → supervisor commands

Bounded Ring Buffer:

    Protected by pthread_mutex_t

    Two condition variables: not_full (producers wait) and not_empty (consumers wait)

Race Conditions Prevented:

    Without mutex: Two producers could read tail, increment it, and write to same slot (lost update/data corruption)

    Condition variables prevent busy-waiting CPU waste

    done flag ensures consumer drains all entries before exit

4.4 Memory Management and Enforcement

RSS (Resident Set Size): Measures physical memory pages currently mapped and resident in RAM. Does not measure swapped pages, shared library double-counting, or un-faulted virtual allocations.

Soft vs Hard Limits:

    Soft limit: Advisory—dmesg warning only. Gives application chance to self-limit.

    Hard limit: Mandatory—SIGKILL sent. Process cannot catch, block, or ignore.

Why kernel space? User-space poller has scheduling latency; a runaway process could consume many megabytes before detection. Kernel module runs at ring 0 with direct access to mm_struct and cannot be killed by monitored process.
4.5 Scheduling Behavior

Linux uses CFS (Completely Fair Scheduler) for normal processes. CFS tracks vruntime—accumulated CPU time weighted by inverse of priority weight.

CFS Weight Table (selected):
nice	weight
-10	9548
0	1024
+10	110

Observations from Experiment:

    CPU-bound workload (workload_cpu) completes in 0.535s of continuous computation

    I/O-bound workload (workload_io) takes 40.281s due to fdatasync blocking

    CFS gives CPU to compute-bound process while I/O process is blocked, maximizing throughput

5. Design Decisions and Tradeoffs
Subsystem	Choice	Tradeoff	Justification
Namespace Isolation	PID + UTS + Mount (no network)	Containers share host network stack	Network isolation requires veth/bridge setup. PID+mount sufficient to demonstrate core OS concepts.
Supervisor Architecture	Single process with select loop + detached producers	Detached producers cannot be explicitly joined	Pipes close on container exit, causing producer thread to exit naturally via read() returning 0.
IPC/Logging	UNIX socket for CLI, pipes for logs	Socket file may persist after crash	Handled with unlink() on startup/shutdown. Sockets provide bidirectional reliable communication.
Kernel Monitor	Timer-based polling (1-second interval)	Up to 1s enforcement latency	Simpler than hooking mm accounting paths. Adequate for workload timescales.
Scheduling	nice value via --nice flag	Applies to whole process, not per-thread	nice is simplest portable API for demonstrating CFS priority effects.
6. Scheduler Experiment Results
Experiment: CPU-bound vs I/O-bound Workloads
Container	Workload Type	Completion Time	CPU Usage
cpu	CPU-bound (workload_cpu)	0.535 seconds	~99%
io	I/O-bound (workload_io)	40.281 seconds	<1%

Analysis:
The I/O-bound container voluntarily yields the CPU on every fdatasync() call. CFS notices it accumulates vruntime very slowly (blocked, not running), so when it wakes up it has the smallest vruntime and is immediately scheduled—giving fast I/O responsiveness. Meanwhile, the CPU-bound container runs freely during the I/O container's blocked periods, completing its work in roughly the same time as if it ran alone. The I/O container's longer wall-clock time is dominated by disk latency, not CPU starvation.
7. Conclusion

This project successfully implements a multi-container runtime with:

    User-space supervisor managing container lifecycle, metadata, and logging

    Kernel module enforcing soft and hard memory limits

    IPC over pipes (logging) and UNIX sockets (control)

    Bounded buffer with proper producer-consumer synchronization

    Namespace isolation (PID, UTS, mount) with chroot

    Scheduling experiments demonstrating CFS behavior

All eight required demonstrations were captured and validated, confirming correct implementation of core operating system concepts including process isolation, memory management, inter-process communication, and CPU scheduling.

End of Report
Instructions for Creating the Word Document

    Copy all content above (from # Multi-Container Runtime Project Report to the end)

    Open Microsoft Word

    Paste the content (Ctrl+V)

    Insert your screenshots at the marked locations

    Replace placeholder text ([Your Name], [Your SRN], etc.) with your actual information

    Adjust image paths to match where you store the screenshots

    Save as Project_Report.docx

This is a complete, submission-ready report. Add your screenshots and team details, and you're done! 🚀
