/*
 * monitor.c - Multi-Container Memory Monitor (Linux Kernel Module)
 *
 * Provided boilerplate:
 *   - device registration and teardown
 *   - timer setup
 *   - RSS helper
 *   - soft-limit and hard-limit event helpers
 *   - ioctl dispatch shell
 *
 * Implementation fills all sections marked // TODO.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/hashtable.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_SEC 1
#define MONITOR_CONTAINER_ID_LEN 256
/* ==============================================================
 * TODO 1: Linked-list node struct.
 *
 * Tracks PID, container ID, soft/hard limits, and whether the
 * soft-limit warning has already been emitted for this entry.
 * ============================================================== */
struct monitored_entry {
    pid_t          pid;
    char           container_id[MONITOR_CONTAINER_ID_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            soft_warned;   /* 1 after first soft-limit log */
    struct list_head list;        /* kernel linked-list linkage   */
};

/* ==============================================================
 * TODO 2: Global monitored list and its lock.
 *
 * A mutex is the right choice here: the timer callback runs in
 * process context (softirq-deferred workqueue path on modern kernels
 * via mod_timer), and the ioctl handler also runs in process context.
 * Neither path is called from hard-IRQ context, so sleeping inside
 * mutex_lock() is safe.  Using a spinlock would work too but would
 * needlessly disable preemption/IRQs during list traversal, which
 * can be non-trivial if many containers are tracked.
 * ============================================================== */
static LIST_HEAD(monitored_list);
static DEFINE_MUTEX(monitored_list_lock);

/* --- Provided: internal device / timer state --- */
static struct timer_list monitor_timer;
static dev_t dev_num;
static struct cdev c_dev;
static struct class *cl;

/* ---------------------------------------------------------------
 * Provided: RSS Helper
 *
 * Returns the Resident Set Size in bytes for the given PID,
 * or -1 if the task no longer exists.
 * --------------------------------------------------------------- */
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct *mm;
    long rss_pages = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm);
    }
    put_task_struct(task);

    return rss_pages * PAGE_SIZE;
}

/* ---------------------------------------------------------------
 * Provided: soft-limit helper
 *
 * Log a warning when a process exceeds the soft limit.
 * --------------------------------------------------------------- */
static void log_soft_limit_event(const char *container_id,
                                 pid_t pid,
                                 unsigned long limit_bytes,
                                 long rss_bytes)
{
    printk(KERN_WARNING
           "[container_monitor] SOFT LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ---------------------------------------------------------------
 * Provided: hard-limit helper
 *
 * Kill a process when it exceeds the hard limit.
 * --------------------------------------------------------------- */
static void kill_process(const char *container_id,
                         pid_t pid,
                         unsigned long limit_bytes,
                         long rss_bytes)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        send_sig(SIGKILL, task, 1);
    rcu_read_unlock();

    printk(KERN_WARNING
           "[container_monitor] HARD LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ---------------------------------------------------------------
 * Timer Callback - fires every CHECK_INTERVAL_SEC seconds.
 * --------------------------------------------------------------- */
static void timer_callback(struct timer_list *t)
{
    /* ==============================================================
     * TODO 3: Periodic monitoring.
     *
     * We use list_for_each_entry_safe() so we can delete entries
     * during iteration without corrupting the list walk.
     *
     * For each entry:
     *   1. Call get_rss_bytes().  If the task is gone (-1), remove
     *      the entry and free it (avoid use-after-free via list_del
     *      before kfree).
     *   2. If RSS exceeds the hard limit: kill the process, then
     *      remove and free the entry.
     *   3. If RSS exceeds the soft limit and we have not yet warned:
     *      emit one soft-limit log and set soft_warned = 1.
     * ============================================================== */
    struct monitored_entry *entry, *tmp;
    LIST_HEAD(to_free);

    mutex_lock(&monitored_list_lock);

    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
        long rss = get_rss_bytes(entry->pid);

        /* Process has exited – clean up silently */
        if (rss < 0) {
            list_del(&entry->list);
            list_add(&entry->list, &to_free);
            continue;
        }

        /* Hard limit: kill then remove */
        if ((unsigned long)rss >= entry->hard_limit_bytes) {
            kill_process(entry->container_id, entry->pid,
                         entry->hard_limit_bytes, rss);
            list_del(&entry->list);
            list_add(&entry->list, &to_free);
            continue;
        }

        /* Soft limit: warn once */
        if ((unsigned long)rss >= entry->soft_limit_bytes &&
            !entry->soft_warned) {
            log_soft_limit_event(entry->container_id, entry->pid,
                                 entry->soft_limit_bytes, rss);
            entry->soft_warned = 1;
        }
    }

    mutex_unlock(&monitored_list_lock);

    /* Free collected entries outside the lock to keep lock hold-time short */
    list_for_each_entry_safe(entry, tmp, &to_free, list) {
        list_del(&entry->list);
        kfree(entry);
    }

    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ---------------------------------------------------------------
 * IOCTL Handler
 *
 * Supported operations:
 *   - register a PID with soft + hard limits
 *   - unregister a PID when the runtime no longer needs tracking
 * --------------------------------------------------------------- */
static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;

    (void)f;

    if (cmd != MONITOR_REGISTER && cmd != MONITOR_UNREGISTER)
        return -EINVAL;

    if (copy_from_user(&req, (struct monitor_request __user *)arg, sizeof(req)))
        return -EFAULT;

    if (cmd == MONITOR_REGISTER) {
        printk(KERN_INFO
               "[container_monitor] Registering container=%s pid=%d soft=%lu hard=%lu\n",
               req.container_id, req.pid, req.soft_limit_bytes, req.hard_limit_bytes);

        /* ==============================================================
         * TODO 4: Add a monitored entry.
         *
         * Validate that soft <= hard (reject garbage limits).
         * Allocate a new node with GFP_KERNEL (we are in process
         * context, sleeping is fine).
         * Populate all fields from req, then prepend to the list
         * under the mutex.
         * ============================================================== */
        if (req.soft_limit_bytes > req.hard_limit_bytes) {
            printk(KERN_ERR
                   "[container_monitor] Rejected: soft limit (%lu) > hard limit (%lu)\n",
                   req.soft_limit_bytes, req.hard_limit_bytes);
            return -EINVAL;
        }

        struct monitored_entry *entry = kzalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid              = req.pid;
        entry->soft_limit_bytes = req.soft_limit_bytes;
        entry->hard_limit_bytes = req.hard_limit_bytes;
        entry->soft_warned      = 0;
        /* Ensure container_id is NUL-terminated even if the user-space
         * buffer was completely full and lacked a terminator. */
        strncpy(entry->container_id, req.container_id,
                MONITOR_CONTAINER_ID_LEN - 1);
        entry->container_id[MONITOR_CONTAINER_ID_LEN - 1] = '\0';
        INIT_LIST_HEAD(&entry->list);

        mutex_lock(&monitored_list_lock);
        list_add(&entry->list, &monitored_list);
        mutex_unlock(&monitored_list_lock);

        return 0;
    }

    printk(KERN_INFO
           "[container_monitor] Unregister request container=%s pid=%d\n",
           req.container_id, req.pid);

    /* ==============================================================
     * TODO 5: Remove a monitored entry on explicit unregister.
     *
     * Search by PID (sufficient because PIDs are unique on the host
     * at any given moment).  If found, remove from the list under
     * the mutex, then free outside the lock so we do not hold the
     * mutex during kfree.
     * Return -ENOENT if no matching entry exists.
     * ============================================================== */
    {
        struct monitored_entry *entry, *tmp;
        struct monitored_entry *found = NULL;

        mutex_lock(&monitored_list_lock);
        list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
            if (entry->pid == req.pid) {
                list_del(&entry->list);
                found = entry;
                break;
            }
        }
        mutex_unlock(&monitored_list_lock);

        if (!found)
            return -ENOENT;

        kfree(found);
        return 0;
    }
}

/* --- Provided: file operations --- */
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* --- Provided: Module Init --- */
static int __init monitor_init(void)
{
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0)
        return -1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    cl = class_create(DEVICE_NAME);
#else
    cl = class_create(THIS_MODULE, DEVICE_NAME);
#endif
    if (IS_ERR(cl)) {
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(cl);
    }

    if (IS_ERR(device_create(cl, NULL, dev_num, NULL, DEVICE_NAME))) {
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    cdev_init(&c_dev, &fops);
    if (cdev_add(&c_dev, dev_num, 1) < 0) {
        device_destroy(cl, dev_num);
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO "[container_monitor] Module loaded. Device: /dev/%s\n", DEVICE_NAME);
    return 0;
}

/* --- Provided: Module Exit --- */
static void __exit monitor_exit(void)
{
    timer_delete_sync(&monitor_timer);

    /* ==============================================================
     * TODO 6: Free all remaining monitored entries.
     *
     * del_timer_sync() above guarantees the timer callback has
     * finished and will not fire again, so we can safely walk the
     * list without holding the lock for the entire operation.
     * We take the lock anyway for correctness in case a concurrent
     * ioctl is still in-flight (module unload races are real).
     *
     * list_for_each_entry_safe() lets us delete inside the loop.
     * ============================================================== */
    {
        struct monitored_entry *entry, *tmp;

        mutex_lock(&monitored_list_lock);
        list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
            list_del(&entry->list);
            kfree(entry);
        }
        mutex_unlock(&monitored_list_lock);
    }

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[container_monitor] Module unloaded.\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Supervised multi-container memory monitor");
