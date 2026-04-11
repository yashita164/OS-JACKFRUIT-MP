# Supervised Multi-Container Runtime

## Operating Systems Project – OS Jackfruit

## Overview

This project implements a lightweight container runtime inspired by modern container systems. It enables creation, execution, and management of isolated containers using Linux system calls and namespaces.

The system integrates user-space and kernel-space components to demonstrate core operating system concepts such as process isolation, inter-process communication, and kernel monitoring.

## Key Features

* Container creation using `clone()`
* Namespace-based isolation (PID, mount)
* Multi-container support
* Supervisor-client architecture using UNIX domain sockets
* Logging system for container output
* Kernel module for monitoring memory usage
* Soft and hard memory limits enforcement

## System Architecture

### User-Space Runtime (Engine)

* Manages container lifecycle (`run`, `ps`, `logs`, `stop`)
* Communicates with supervisor via UNIX socket
* Uses `clone()` for container creation

### Supervisor

* Central control process
* Maintains container state
* Handles client requests

### Kernel Module (monitor.ko)

* Tracks container memory usage
* Logs events using `printk`
* Communicates via `ioctl`

## Implementation Tasks

### Task 1 – Basic Execution

Demonstrates execution of a program inside the container runtime.

**Command:**

```
sudo ./engine run c80 / /home/yashita-anand/OS-Jackfruit/boilerplate/cpu_hog
```

**Observation:**
The container executes successfully and exits with status 0.


### Task 2 – Container Isolation

Demonstrates process isolation using Linux namespaces and the `clone()` system call.

**Observation:**
Containers run independently without affecting the host system.


### Task 3 – Multi-Container Runtime

Demonstrates support for multiple containers using unique container IDs.

**Commands:**

```
sudo ./engine run c210 / /home/yashita-anand/OS-Jackfruit/boilerplate/cpu_hog
sudo ./engine run c211 / /home/yashita-anand/OS-Jackfruit/boilerplate/cpu_hog
```

**Observation:**
Multiple containers execute independently.


### Task 4 – Logging System

Demonstrates storage and retrieval of container output logs.

**Commands:**

```
sudo ./engine run c300 / /home/yashita-anand/OS-Jackfruit/boilerplate/cpu_hog
sudo ./engine logs c300
```

**Observation:**
Container output is stored in log files and retrieved successfully.


### Task 5 – Kernel Monitoring

Demonstrates kernel-level monitoring using a custom kernel module.

**Commands:**

```
sudo insmod monitor.ko
sudo ./engine run c400 / /home/yashita-anand/OS-Jackfruit/boilerplate/memory_hog
sudo dmesg | tail
```

**Observation:**
Kernel logs show container registration and monitoring details.


### Task 6 – Supervisor and Control Plane

Demonstrates supervisor-based control of container execution.

**Command:**

```
sudo ./engine supervisor /
```

**Observation:**
Supervisor manages container lifecycle through a UNIX socket.


## How to Run

### Build

```
make
```

### Load Kernel Module

```
sudo insmod monitor.ko
```

### Start Supervisor

```
sudo ./engine supervisor /
```

### Run Container

```
sudo ./engine run <id> / <program>
```

---

## Screenshots

### Task 1 – Basic Execution

(Add screenshot here)

### Task 2 – Container Isolation

(Add screenshot here)

### Task 3 – Multi-Container Runtime

(Add screenshot here)

### Task 4 – Logging System

(Add screenshot here)

### Task 5 – Kernel Monitoring

(Add screenshot here)

### Task 6 – Supervisor

(Add screenshot here)


## Design Decisions

* Used `clone()` instead of `fork()` to enable namespace isolation
* Implemented supervisor-client architecture for modularity
* Integrated kernel module for low-level monitoring


## Conclusion

This project demonstrates key operating system concepts such as process isolation, container runtime design, kernel-user communication, and resource monitoring. It provides a simplified understanding of how container technologies like Docker function internally.


