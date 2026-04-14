## Multi-Container Runtime
Operating Systems Project – OS Jackfruit
## 1. Team Information
Team Members and Contributions
Yashita Anand

Assigned Tasks: Task 1, Task 4, Task 6

Contributions:

Task 1 – Multi-Container Supervision
Implemented execution of multiple containers under a single supervisor process and ensured correct lifecycle handling.
Task 4 – CLI and IPC Mechanism
Developed CLI commands and implemented communication between CLI and supervisor using UNIX domain sockets.
Task 6 – Hard Limit Enforcement
Integrated kernel monitoring with the runtime and demonstrated memory limit tracking through kernel logs.
Vismaya Harish

Assigned Tasks: Task 2, Task 3, Task 5

Contributions:

Task 2 – Metadata Tracking
Implemented container metadata tracking and developed the engine ps command.
Task 3 – Bounded-Buffer Logging
Designed a logging pipeline using pipes and a producer-consumer model.
Task 5 – Soft Limit Monitoring
Implemented kernel module functionality for monitoring container memory usage.
Joint Contributions
Task 7 – Scheduling Experiment
Task 8 – Clean Teardown
## 2. Overview

This project implements a lightweight container runtime inspired by modern container systems. It enables creation, execution, and management of isolated containers using Linux system calls and namespaces.

The system integrates user-space and kernel-space components to demonstrate core operating system concepts such as process isolation, inter-process communication, memory monitoring, and scheduling behavior.

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
User-Space Runtime (engine.c)
Manages container lifecycle (run, ps, logs, stop)
Uses clone() for container creation
Communicates with supervisor using UNIX sockets
Supervisor
Central control process
Maintains container metadata
Handles client requests
Kernel Module (monitor.ko)
Tracks container memory usage
Logs events using printk
Communicates using ioctl
## 5. Build, Load, and Run Instructions
1. Fork the Repository
Go to the original repository
Click Fork
Clone your fork:
git clone https://github.com/<your-username>/OS-Jackfruit.git
cd OS-Jackfruit
2. Set Up the Environment

Install dependencies:

sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
3. Run Environment Check
cd boilerplate
chmod +x environment-check.sh
sudo ./environment-check.sh
4. Build the Project
make
5. Load Kernel Module
sudo insmod monitor.ko

Verify:

ls -l /dev/container_monitor
6. Start Supervisor
sudo ./engine supervisor ./rootfs-base
7. Prepare Root Filesystems
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
8. Run Containers
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 40 --hard-mib 64
sudo ./engine start beta ./rootfs-beta /bin/sh --soft-mib 40 --hard-mib 64
9. Run Foreground Container
sudo ./engine run gamma ./rootfs-alpha /cpu_hog
10. View Containers
sudo ./engine ps
11. View Logs
sudo ./engine logs alpha
12. Stop Containers
sudo ./engine stop alpha
sudo ./engine stop beta
13. Check Kernel Logs
sudo dmesg | tail
14. Unload Module
sudo rmmod monitor
## 6. Demo with Screenshots
Task 1 – Multi-Container Supervision (Yashita Anand)
(Add Screenshot: Screenshots/Task1.png)
Task 2 – Metadata Tracking (Vismaya Harish)
(Add Screenshot: Screenshots/Task2.png)
Task 3 – Logging System (Vismaya Harish)
(Add Screenshot: Screenshots/Task3.png)
Task 4 – CLI and IPC (Yashita Anand)
(Add Screenshot: Screenshots/Task4.png)
Task 5 – Kernel Monitoring (Vismaya Harish)
(Add Screenshot: Screenshots/Task5.png)
Task 6 – Memory Limit Enforcement (Yashita Anand)
(Add Screenshot: Screenshots/Task6.png)
Task 7 – Scheduling Experiment
(Add Screenshot: Screenshots/Task7.png)
Task 8 – Clean Teardown
(Add Screenshot: Screenshots/Task8.png)
## 7. Engineering Analysis
Isolation Mechanisms

Uses Linux namespaces (PID, mount, UTS) to isolate containers. Filesystem isolation is implemented using chroot.

Supervisor and Lifecycle Management

A central supervisor manages container execution and ensures proper cleanup of processes.

IPC and Synchronization

Uses UNIX sockets for CLI communication and pipes for logging. Synchronization is handled using producer-consumer logic.

Memory Management

Soft and hard memory limits are tracked using a kernel module. RSS is used for memory measurement.

Scheduling Behavior

Multiple containers share CPU resources, demonstrating fairness of the Linux scheduler.

## 8. Design Decisions and Tradeoffs
Used clone() instead of fork() for namespace support
Used chroot instead of pivot_root for simplicity
Implemented kernel module for accurate monitoring
Used bounded-buffer logging for safe concurrency
## 9. Conclusion

This project demonstrates container runtime design, process isolation, kernel-user communication, and scheduling behavior. It provides insight into how container systems such as Docker operate internally.
