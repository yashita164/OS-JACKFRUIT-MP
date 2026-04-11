# Multi-Container Runtime

## Operating Systems Project – OS Jackfruit

## Overview

This project implements a lightweight container runtime inspired by modern container systems such as Docker. It enables the creation, execution, and management of isolated containers using Linux kernel primitives such as namespaces and process control mechanisms.

The system integrates both user-space and kernel-space components to demonstrate key operating system concepts including process isolation, inter-process communication, and resource monitoring.

## Key Features

* Lightweight container runtime using `clone()`
* Process isolation using Linux namespaces (PID and mount)
* Multi-container support
* Supervisor-client architecture using UNIX domain sockets
* Logging system for container execution
* Kernel module for monitoring memory usage
* Soft and hard memory limit enforcement

## System Architecture

### User-Space Runtime (Engine)

* Handles container lifecycle (`run`, `ps`, `logs`, `stop`)
* Communicates with supervisor via UNIX socket (`/tmp/mini_runtime.sock`)
* Uses `clone()` to create isolated containers

### Supervisor

* Central controller for container management
* Maintains container state and metadata
* Handles client requests from engine commands

### Kernel Module (monitor.ko)

* Tracks memory usage of containers
* Enforces soft and hard limits
* Logs events using `printk`
* Communicates with user-space using `ioctl`

## Implementation Tasks

### Task 1: Basic Execution

Implemented process execution using `fork()` and `execvp()`.

### Task 2: Container Isolation

Used `clone()` with namespace flags:

* `CLONE_NEWPID`
* `CLONE_NEWNS`

### Task 3: Multi-Container Support

Enabled execution and management of multiple containers.

### Task 4: Logging System

Container outputs can be accessed using:

```
./engine logs <container_id>
```

### Task 5: Kernel Monitoring

Kernel module monitors:

* Memory usage
* Soft and hard limits
* Logs through `dmesg`

### Task 6: Supervisor System

Implemented control plane using:

* UNIX domain sockets
* Client-server communication model

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
sudo ./engine supervisor rootfs-alpha
```

### Run Containers

```
sudo ./engine run c1 rootfs-alpha ./cpu_hog
sudo ./engine run c2 rootfs-alpha ./memory_hog
```

## Observations

* CPU-intensive workloads (`cpu_hog`) do not trigger memory monitoring events
* Memory-intensive workloads (`memory_hog`) generate kernel logs
* Containers terminate quickly, so `engine ps` may appear empty

## Sample Outputs

* `engine ps` shows active containers
* `engine logs <id>` shows logs
* `dmesg | tail` shows kernel monitoring output

## Design Decisions

* Used `clone()` instead of `fork()` for namespace isolation
* Implemented supervisor-client architecture for scalability
* Used kernel module for accurate monitoring

## Screenshots

Add screenshots of:

* Container execution
* engine ps output
* dmesg output
* logs

## Conclusion

This project demonstrates core operating system concepts such as process isolation, kernel-user communication, and container runtime design.

## Author

Yashita Anand
https://github.com/yashita164
