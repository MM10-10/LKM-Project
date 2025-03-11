#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#define DEVICE_NAME "CharDevModule"
#define FIFO_SIZE 1024

MODULE_LICENSE("GPL");
MODULE_AUTHOR("HARRY KIKKERS, MAHDI MIRZAY, CONOR MCCARTHY");
MODULE_DESCRIPTION("Our kernel module");
MODULE_VERSION("1.0");

static int major;
static char fifo_buffer[FIFO_SIZE];
static int read_pos = 0;
static int write_pos = 0;
static int data_size = 0;
static int device_open_counter = 0;
static ssize_t total_bytes_read = 0;
static ssize_t total_bytes_written = 0;

static DEFINE_MUTEX(fifo_mutex);
static DEFINE_MUTEX(device_mutex);

static int device_open(struct inode *inode, struct file *file);
static int device_release(struct inode *inode, struct file *file);
static ssize_t device_read(struct file *file, char __user *buf, size_t count, loff_t *offset);
static ssize_t device_write(struct file *file, const char __user *buf, size_t count, loff_t *offset);
static long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

struct device_stats {
    int device_opens;
    ssize_t total_bytes_read;
    ssize_t total_bytes_written;
};

static void rot13_transform(char *str, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (str[i] >= 'a' && str[i] <= 'z') {
            str[i] = ((str[i] - 'a' + 13) % 26) + 'a';
        } else if (str[i] >= 'A' && str[i] <= 'Z') {
            str[i] = ((str[i] - 'A' + 13) % 26) + 'A';
        }
    }
}

static ssize_t procfile_write(struct file *file, const char __user *buffer, size_t len, loff_t *off) {
    char buf[1000];

    if (len > 99) {
        return -EINVAL;
    }
    if (copy_from_user(buf, buffer, len)) {
        return -EFAULT;
    }

    buf[len] = '\0';

    printk(KERN_INFO "Received from user: %s\n", buf);

    return len;
}

static int procfile_showstats(struct seq_file *m, void *v) {
    seq_printf(m, "Device Statistics:\n");
    seq_printf(m, "Device Opens: %d\n", device_open_counter);
    seq_printf(m, "Total Bytes Read: %zd\n", total_bytes_read);
    seq_printf(m, "Total Bytes Written: %zd\n", total_bytes_written);
    return 0;
}

static int procfile_show_fifo_status(struct seq_file *m, void *v) {
    seq_printf(m, "FIFO Buffer Status:\n");
    seq_printf(m, "Data Size: %d\n", data_size);
    seq_printf(m, "Free Space: %d\n", FIFO_SIZE - data_size);
    seq_printf(m, "Read Position: %d\n", read_pos);
    seq_printf(m, "Write Position: %d\n", write_pos);
    return 0;
}

static int procfile_open_stats(struct inode *inode, struct file *file) {
    return single_open(file, procfile_showstats, NULL);
}

static int procfile_open_fifo_status(struct inode *inode, struct file *file) {
    return single_open(file, procfile_show_fifo_status, NULL);
}

static int procfile_show(struct seq_file *m, void *v) {
    seq_printf(m, "Hello from driver\n");
    return 0;
}

static int procfile_open(struct inode *inode, struct file *file) {
    return single_open(file, procfile_show, NULL);
}

static const struct proc_ops proc_fops_stats = {
    .proc_open = procfile_open_stats,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};

static const struct proc_ops proc_fops_fifo_status = {
    .proc_open = procfile_open_fifo_status,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};

static const struct proc_ops proc_fops = {
    .proc_open = procfile_open,
    .proc_read = seq_read,
    .proc_write = procfile_write,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};

struct file_operations fops = {
    .open = device_open,
    .release = device_release,
    .read = device_read,
    .write = device_write,
    .unlocked_ioctl = device_ioctl,
    .owner = THIS_MODULE,
};

static int __init CharDevMod_init(void) {
    major = register_chrdev(0, DEVICE_NAME, &fops);
    struct proc_dir_entry *entry_stats;
    struct proc_dir_entry *entry_fifo_status;
    struct proc_dir_entry *entry;

    entry = proc_create("myprocfile", 0666, NULL, &proc_fops);
    if (!entry) {
        printk(KERN_INFO "Failed to create /proc/myprocfile\n");
        return -ENOMEM;
    }
    printk(KERN_INFO "/proc/myprocfile created\n");

    entry_stats = proc_create("myprocfile_stats", 0444, NULL, &proc_fops_stats);
    if (!entry_stats) {
        printk(KERN_INFO "Failed to create /proc/myprocfile_stats\n");
        remove_proc_entry("myprocfile", NULL);
        return -ENOMEM;
    }
    printk(KERN_INFO "/proc/myprocfile_stats created\n");

    entry_fifo_status = proc_create("myprocfile_fifo_status", 0444, NULL, &proc_fops_fifo_status);
    if (!entry_fifo_status) {
        printk(KERN_INFO "Failed to create /proc/myprocfile_fifo_status\n");
        remove_proc_entry("myprocfile", NULL);
        remove_proc_entry("myprocfile_stats", NULL);
        return -ENOMEM;
    }
    printk(KERN_INFO "/proc/myprocfile_fifo_status created\n");

    if (major < 0) {
        printk(KERN_ERR "Registering failed with major: %d\n", major);
        remove_proc_entry("myprocfile", NULL);
        remove_proc_entry("myprocfile_stats", NULL);
        remove_proc_entry("myprocfile_fifo_status", NULL);
        return major;
    }
    printk(KERN_INFO "Registering successful, major number: %d.\n", major);
    printk(KERN_INFO "Driver module loaded\n");
    return 0;
}

static void __exit CharDevMod_exit(void) {
    unregister_chrdev(major, DEVICE_NAME);
    remove_proc_entry("myprocfile", NULL);
    remove_proc_entry("myprocfile_stats", NULL);
    remove_proc_entry("myprocfile_fifo_status", NULL);
    printk(KERN_INFO "/proc/myprocfile removed\n");
    printk(KERN_INFO "/proc/myprocfile_stats removed\n");
    printk(KERN_INFO "/proc/myprocfile_fifo_status removed\n");
    printk(KERN_INFO "Unregistered device\n");
    printk(KERN_INFO "Driver module unloaded!\n");
}

static int device_open(struct inode *inode, struct file *file) {
    if (!mutex_trylock(&device_mutex)) {
        printk(KERN_WARNING "Device open failed: in use\n");
        return -EBUSY;
    }
    device_open_counter++;
    printk(KERN_INFO "Device opened\n");
    return 0;
}

static int device_release(struct inode *inode, struct file *file) {
    device_open_counter--;
    printk(KERN_INFO "Device closed\n");
    mutex_unlock(&device_mutex);
    return 0;
}

static ssize_t device_read(struct file *file, char __user *buf, size_t count, loff_t *offset) {
    ssize_t ret = 0;
    if (!mutex_trylock(&fifo_mutex)) {
        printk(KERN_WARNING "Device read failed: in use\n");
        return -EBUSY;
    }

    if (count > data_size) {
        count = data_size;
    }

    rot13_transform(fifo_buffer + read_pos, count);

    if (copy_to_user(buf, fifo_buffer + read_pos, count)) {
        ret = -EFAULT;
    } else {
        read_pos = (read_pos + count) % FIFO_SIZE;
        data_size -= count;
        ret = count;
        total_bytes_read += count;
    }

    mutex_unlock(&fifo_mutex);
    return ret;
}

static ssize_t device_write(struct file *file, const char __user *buf, size_t count, loff_t *offset) {
    ssize_t ret = 0;
    if (!mutex_trylock(&fifo_mutex)) {
        printk(KERN_WARNING "Device in use\n");
        return -EBUSY;
    }

    if (count > FIFO_SIZE - data_size) {
        count = FIFO_SIZE - data_size;
    }

    if (copy_from_user(fifo_buffer + write_pos, buf, count)) {
        ret = -EFAULT;
    } else {
       
        rot13_transform(fifo_buffer + write_pos, count);

        write_pos = (write_pos + count) % FIFO_SIZE;
        data_size += count;
        ret = count;
        total_bytes_written += count;
    }

    mutex_unlock(&fifo_mutex);
    return ret;
}

module_init(CharDevMod_init);
module_exit(CharDevMod_exit);
