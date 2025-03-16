#include <linux/module.h> // provides macros/functions for init,cleanup,etc
#include <linux/kernel.h> // provides functions such as printk
#include <linux/init.h> // init/exit 
#include <linux/fs.h> // provides structures and functions for working with files,etc
#include <linux/uaccess.h> // functions for user to kernel data copy etc
#include <linux/ioctl.h> // input/output
#include <linux/mutex.h> // mutexes
#include <linux/proc_fs.h> // /proc filesystem
#include <linux/seq_file.h> 

#define DEVICE_NAME "CharDevModule"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("HARRY KIKKERS, MAHDI MIRZAY, CONOR MCCARTHY");
MODULE_DESCRIPTION("Our kernel module");
MODULE_VERSION("1.0");

static int major; // major number 
static int device_open_counter = 0;
static ssize_t total_bytes_read = 0;
static ssize_t total_bytes_written = 0;

static DEFINE_MUTEX(device_mutex); // create mutex

static int device_open(struct inode *inode, struct file *file);
static int device_release(struct inode *inode, struct file *file);
static ssize_t device_read(struct file *file, char __user *buf, size_t count, loff_t *offset);
static ssize_t device_write(struct file *file, const char __user *buf, size_t count, loff_t *offset);

struct device_stats { // new struct for stats
    int device_opens;
    ssize_t total_bytes_read;
    ssize_t total_bytes_written;
};

// pointer to file, pointer to buffer in user space, num bytes, pointer to file to be wrote
static ssize_t procfile_write(struct file *file, const char __user *buffer, size_t len, loff_t *off) {
    char buf[1000]; // buffer 

    if (len > 99) {
        return -EINVAL; // invalid argument 
    }
    if (copy_from_user(buf, buffer, len)) {
        return -EFAULT; // bad address if copy fails
    }

    buf[len] = '\0'; 

    printk(KERN_INFO "Received from user: %s\n", buf);

    return len;
}

// pointer to seq_file structure, pointer to priv data 
static int procfile_showstats(struct seq_file *m, void *v) {
    seq_printf(m, "Device Statistics:\n");
    seq_printf(m, "Device Opens: %d\n", device_open_counter);
    seq_printf(m, "Total Bytes Read: %zd\n", total_bytes_read);
    seq_printf(m, "Total Bytes Written: %zd\n", total_bytes_written);
    return 0; // success 
}

// inode of procfile with mdata, file being opened
static int procfile_open_stats(struct inode *inode, struct file *file) {
    return single_open(file, procfile_showstats, NULL); 
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
    .owner = THIS_MODULE,
};

static int __init CharDevMod_init(void) {
    major = register_chrdev(0, DEVICE_NAME, &fops);
    struct proc_dir_entry *entry_stats;
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

    if (major < 0) {
        printk(KERN_ERR "Registering failed with major: %d\n", major);
        remove_proc_entry("myprocfile", NULL);
        remove_proc_entry("myprocfile_stats", NULL);
        return major;
    }
    printk(KERN_INFO "Registering successful, major number: %d.\n", major);
    printk(KERN_INFO "Driver module loaded\n");
    return 0;
}

static void __exit CharDevMod_exit(void) {
    unregister_chrdev(major, DEVICE_NAME); // unregister device with major number
    remove_proc_entry("myprocfile", NULL); // remove procfile 
    remove_proc_entry("myprocfile_stats", NULL); // remove procfile
    printk(KERN_INFO "/proc/myprocfile removed\n");
    printk(KERN_INFO "/proc/myprocfile_stats removed\n");
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
    char temp_buf[1000];
    ssize_t ret = 0;

    if (count > sizeof(temp_buf)) {
        count = sizeof(temp_buf);
    }

    snprintf(temp_buf, sizeof(temp_buf), "Hello from kernel!");



    if (copy_to_user(buf, temp_buf, count)) {
        ret = -EFAULT;
    } else {
        ret = count;
        total_bytes_read += count;
    }

    return ret;
}

static ssize_t device_write(struct file *file, const char __user *buf, size_t count, loff_t *offset) {
    char temp_buf[1000];
    ssize_t ret = 0;

    if (count > sizeof(temp_buf)) {
        count = sizeof(temp_buf);
    }

    if (copy_from_user(temp_buf, buf, count)) {
        ret = -EFAULT;
    } else {

        printk(KERN_INFO "Received from user: %s\n", temp_buf);
        ret = count;
        total_bytes_written += count;
    }

    return ret;
}

module_init(CharDevMod_init);
module_exit(CharDevMod_exit);
