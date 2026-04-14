## Multi-Container Runtime
Operating Systems Project – OS Jackfruit

## 1. Team Information
1.Yashita Anand
Task 1 – Multi-Container Supervision:
Implemented support for running multiple containers under a single supervisor process. Ensured proper namespace isolation and container lifecycle handling.
Task 4 – CLI and IPC Mechanism:
Developed the command-line interface and implemented communication between CLI and supervisor using IPC (UNIX domain sockets).
Task 6 – Hard Limit Enforcement:
Integrated kernel monitoring with the runtime and ensured containers are tracked with memory limits. Demonstrated enforcement pipeline through kernel logs.
Task 7 – Scheduling Experiment:
Designed and executed experiments with multiple containers to analyze Linux scheduling behavior and CPU sharing.
Task 8 – Resource Cleanup:
Ensured proper cleanup of processes, threads, and kernel structures, preventing zombie processes and resource leaks.

2.Vismaya Harish – Tasks 2, 3, 5
Contributions:

Task 2 – Metadata Tracking:
Implemented container state tracking and developed the engine ps command to display container metadata such as PID, state, and limits.
Task 3 – Bounded-Buffer Logging:
Designed and implemented a logging system using pipes and a bounded-buffer producer-consumer model to safely capture container output.
Task 5 – Soft Limit Monitoring:
Developed kernel module functionality for monitoring container memory usage and logging soft-limit events.

## 2. Overview

This project implements a lightweight container runtime inspired by modern container systems. It enables creation, execution, and management of isolated containers using Linux system calls and namespaces.

The system integrates both user-space and kernel-space components to demonstrate core operating system concepts such as process isolation, inter-process communication, memory monitoring, and scheduling behavior.

## 3. Key Features
Container creation using clone()
Namespace-based isolation (PID, UTS, mount)
Multi-container runtime support
Supervisor-client architecture using UNIX domain sockets
Bounded-buffer logging system
Kernel module for memory monitoring
Soft and hard memory limit tracking
Scheduling experiment support

## 4. System Architecture
## User-Space Runtime (engine.c)
Manages container lifecycle (start, run, ps, logs, stop)
Communicates with supervisor using UNIX domain sockets
Uses clone() for container creation
Maintains metadata for each container
## Supervisor
Long-running control process
Handles all container requests
Tracks container state and lifecycle
Implements IPC communication with CLI
## Kernel Module (monitor.c)
Tracks container memory usage
Logs events using printk
Supports soft and hard memory limits
Communicates with user space using ioctl

## 5. Build, Load, and Run Instructions
## Build
make
## Load Kernel Module
sudo insmod monitor.ko

Verify:

ls -l /dev/container_monitor
## Start Supervisor
sudo ./engine supervisor ./rootfs-base
## Prepare Root Filesystems
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
## Run Containers
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 40 --hard-mib 64
sudo ./engine start beta ./rootfs-beta /bin/sh --soft-mib 40 --hard-mib 64
## Run Foreground Container
sudo ./engine run gamma ./rootfs-alpha /cpu_hog
## View Containers
sudo ./engine ps
## View Logs
sudo ./engine logs alpha
## Stop Containers
sudo ./engine stop alpha
sudo ./engine stop beta
## Check Kernel Logs
sudo dmesg | tail
## Unload Module
sudo rmmod monitor

## 6. Demo with Screenshots
## Task 1 – Multi-Container Supervision (Yashita Anand)

Multiple containers are executed under a single supervisor, demonstrating concurrent execution and container management.

## Task 2 – Metadata Tracking (Vismaya Harish)

The engine ps command displays container metadata including ID, PID, state, and memory limits.

## Task 3 – Bounded-Buffer Logging (Vismaya Harish)

Container output is captured using pipes and stored in log files via a bounded-buffer logging system.

## Task 4 – CLI and IPC (Yashita Anand)

CLI commands communicate with the supervisor via IPC, demonstrating control-plane interaction.

## Task 5 – Soft Limit Monitoring (Vismaya Harish)

Kernel logs show container registration and configured soft/hard limits, demonstrating monitoring functionality.

## Task 6 – Hard Limit Enforcement (Yashita Anand)

The kernel module tracks containers and applies memory limits. The logs demonstrate the enforcement pipeline.

## Task 7 – Scheduling Experiment

Multiple containers are executed simultaneously to observe scheduling behavior and CPU sharing.

## Task 8 – Clean Teardown

Supervisor exits cleanly and no container processes remain, demonstrating proper cleanup.

## 7. Engineering Analysis
## Isolation Mechanisms

The runtime uses Linux namespaces (PID, UTS, mount) to isolate containers. Each container runs in its own namespace, ensuring independent process trees and system views. Filesystem isolation is achieved using chroot, restricting the container’s root directory. However, all containers share the same underlying kernel.

## Supervisor and Process Lifecycle

A long-running supervisor manages container creation and termination. It tracks metadata, handles signals, and ensures child processes are reaped correctly, preventing zombie processes.

## IPC, Threads, and Synchronization

Two IPC mechanisms are used:

UNIX domain sockets for CLI communication
Pipes for logging

A bounded-buffer logging system ensures safe producer-consumer synchronization, preventing race conditions and data loss.

## Memory Management and Enforcement

RSS (Resident Set Size) measures physical memory usage. Soft limits act as warning thresholds, while hard limits define maximum usage. Monitoring is implemented in kernel space for accurate tracking.

## Scheduling Behavior

Running multiple CPU-bound containers demonstrates Linux scheduling. CPU time is shared fairly between containers, showing how the scheduler balances performance and fairness.

## 8. Design Decisions and Tradeoffs
## Namespace Isolation
Used chroot with namespaces
Simpler implementation
Tradeoff: less secure than pivot_root
## Supervisor Architecture
Centralized supervisor process
Easier lifecycle management
Tradeoff: single control point
## Logging System
Bounded-buffer design
Prevents data loss
Tradeoff: added complexity
## Kernel Monitoring
Implemented as kernel module
Accurate tracking
Tradeoff: kernel-level complexity
## Scheduling Experiment
Used CPU-bound workloads
Clearly demonstrates scheduling
Tradeoff: limited workload diversity
## 9. Scheduler Experiment Results
Container	Workload	Observation
c1000	CPU-bound	High CPU usage
c1001	CPU-bound	Shares CPU time
## Analysis
Containers execute concurrently
CPU is shared between processes
Demonstrates fairness of Linux scheduler

## 10. Conclusion
This project demonstrates key operating system concepts including container runtime design, process isolation, kernel-user communication, and scheduling behavior. It provides insight into how modern container systems like Docker operate internally.
