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
 * YOUR WORK: Fill in all sections marked // TODO.
 */
#include <linux/init.h>
#include <linux/timer.h>
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
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_SEC 1

/* ==============================================================
 * TODO 1: Define your linked-list node struct.
 *
 * Requirements:
 *   - track PID, container ID, soft limit, and hard limit
 *   - remember whether the soft-limit warning was already emitted
 *   - include `struct list_head` linkage
 * ============================================================== */

struct monitor_node {
    pid_t pid;
    char container_id[32];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int soft_warned;
    struct list_head list;
};

/* ==============================================================
 * TODO 2: Declare the global monitored list and a lock.
 *
 * Requirements:
 *   - shared across ioctl and timer code paths
 *   - protect insert, remove, and iteration safely
 *
 * You may choose either a mutex or a spinlock, but your README must
 * justify the choice in terms of the code paths you implemented.
 * ============================================================== */

static LIST_HEAD(monitor_list);
static DEFINE_MUTEX(monitor_lock);

/* --- Provided: internal device / timer state --- */
static struct timer_list monitor_timer;
static dev_t dev_num;
static struct cdev c_dev;
static struct class *cl;

/* ---------------------------------------------------------------
 * Provided: RSS Helper
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
 * Timer Callback
 * --------------------------------------------------------------- */
static void timer_callback(struct timer_list *t)
{
    struct monitor_node *node, *tmp;

    mutex_lock(&monitor_lock);

    list_for_each_entry_safe(node, tmp, &monitor_list, list) {
        long rss = get_rss_bytes(node->pid);

        if (rss < 0) {
            printk(KERN_INFO "[container_monitor] Process pid=%d exited. Removing from monitor.\n", node->pid);
            list_del(&node->list);
            kfree(node);
            continue;
        }

        if (rss > node->hard_limit_bytes) {
            kill_process(node->container_id, node->pid,
                         node->hard_limit_bytes, rss);
            list_del(&node->list);
            kfree(node);
        }
        else if (rss > node->soft_limit_bytes && !node->soft_warned) {
            log_soft_limit_event(node->container_id, node->pid,
                                 node->soft_limit_bytes, rss);
            node->soft_warned = 1;
        }
    }

    mutex_unlock(&monitor_lock);

    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ---------------------------------------------------------------
 * IOCTL Handler
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

        struct monitor_node *node;

        if (req.soft_limit_bytes >= req.hard_limit_bytes)
            return -EINVAL;

        node = kmalloc(sizeof(*node), GFP_KERNEL);
        if (!node)
            return -ENOMEM;

        node->pid = req.pid;
        strncpy(node->container_id, req.container_id, sizeof(node->container_id) - 1);
        node->container_id[sizeof(node->container_id) - 1] = '\0';
        node->soft_limit_bytes = req.soft_limit_bytes;
        node->hard_limit_bytes = req.hard_limit_bytes;
        node->soft_warned = 0;

        mutex_lock(&monitor_lock);
        list_add(&node->list, &monitor_list);
        mutex_unlock(&monitor_lock);

        return 0;
    }

    printk(KERN_INFO
           "[container_monitor] Unregister request container=%s pid=%d\n",
           req.container_id, req.pid);

    struct monitor_node *node, *tmp;

    mutex_lock(&monitor_lock);

    list_for_each_entry_safe(node, tmp, &monitor_list, list) {
        if (node->pid == req.pid) {
            list_del(&node->list);
            kfree(node);
            mutex_unlock(&monitor_lock);
            return 0;
        }
    }

    mutex_unlock(&monitor_lock);

    return -ENOENT;
}

/* --- Provided --- */
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* --- Provided --- */
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

/* --- Provided --- */
static void __exit monitor_exit(void)
{
    timer_delete_sync(&monitor_timer);

    struct monitor_node *node, *tmp;

    mutex_lock(&monitor_lock);

    list_for_each_entry_safe(node, tmp, &monitor_list, list) {
        list_del(&node->list);
        kfree(node);
    }

    mutex_unlock(&monitor_lock);

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
