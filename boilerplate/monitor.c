#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>

#define DEVICE_NAME "container_monitor"

static int major;

// IOCTL handler
static long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    int pid;

    // Get PID from user
    if (copy_from_user(&pid, (int *)arg, sizeof(int)))
        return -EFAULT;

    printk(KERN_INFO "Monitor: Received PID %d\n", pid);

    // Find process
    struct task_struct *task = pid_task(find_vpid(pid), PIDTYPE_PID);

    if (!task) {
        printk(KERN_WARNING "Monitor: PID %d not found\n", pid);
        return -1;
    }

    // Get memory usage (approx in KB)
    unsigned long mem_kb = 0;
    if (task->mm)
        mem_kb = get_mm_rss(task->mm) * 4;

    printk(KERN_INFO "Monitor: PID %d memory = %lu KB\n", pid, mem_kb);

    // Limits
    int soft_limit = 20000; // 20 MB
    int hard_limit = 50000; // 50 MB

    if (mem_kb > hard_limit) {
        printk(KERN_ALERT "HARD LIMIT exceeded → Killing PID %d\n", pid);
        kill_pid(find_vpid(pid), SIGKILL, 1);
    }
    else if (mem_kb > soft_limit) {
        printk(KERN_WARNING "SOFT LIMIT exceeded for PID %d\n", pid);
    }

    return 0;
}

// File operations
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = device_ioctl,
};

// Init
static int __init monitor_init(void)
{
    major = register_chrdev(0, DEVICE_NAME, &fops);

    printk(KERN_INFO "Monitor module loaded\n");
    printk(KERN_INFO "Major number: %d\n", major);

    return 0;
}

// Exit
static void __exit monitor_exit(void)
{
    unregister_chrdev(major, DEVICE_NAME);
    printk(KERN_INFO "Monitor module unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ananya");
MODULE_DESCRIPTION("Container Memory Monitor");

