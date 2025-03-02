#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h> // for device registration
#include <linux/uaccess.h> // provides functions to copy data from the user space

#define DEVICE_NAME "Loopback"
#define BUFFER_SIZE 1024

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("A simple kernel module example");
MODULE_VERSION("1.0");

static int major_number; // stores dynamically allocated major number
static char buffer[BUFFER_SIZE]; // internal buffer size
static size_t buffer_size = 0; // keeps track of how much data is stored in the buffer

// Function to handle and log open operations
static int device_open(struct inode *inode, struct file *file) {
    printk(KERN_INFO "Device opened\n");
    return 0;
}

// Function to handle and log release operations
static int device_release(struct inode *inode, struct file *file) {
    printk(KERN_INFO "Device closed\n");
    return 0;
}

// Function to handle and log read operations
static ssize_t device_read(struct file *file, char __user *user_buffer, size_t len, loff_t *offset) {
    size_t bytes_to_read = min(len, buffer_size); // determine the minimum of requested length & available data
    if (copy_to_user(user_buffer, buffer, bytes_to_read)) { // Copy data to user-space
        return -EFAULT;
    }
    buffer_size = 0; // After reading, the buffer is cleared
    printk(KERN_INFO "Device read %zu bytes\n", bytes_to_read); // Log device activity
    return bytes_to_read;
}

// Function to handle write operations
static ssize_t device_write(struct file *file, const char __user *user_buffer, size_t len, loff_t *offset) {
    size_t bytes_to_write = min(len, BUFFER_SIZE); // determine the minimum of requested length & available buffer size
    if (copy_from_user(buffer, user_buffer, bytes_to_write)) { // copy data from user-space
        return -EFAULT;
    }
    buffer[bytes_to_write] = '\0'; // Null-terminate the string to prevent garbage in the buffer
    buffer_size = bytes_to_write; // Update the buffer size
    printk(KERN_INFO "Loopback device wrote %zu bytes\n", bytes_to_write); // Log device activity
    return bytes_to_write;
}

static struct file_operations fops = {
    .open = device_open,
    .release = device_release,
    .read = device_read,
    .write = device_write,
};

// Module initialization function
static int __init loopback_init(void) {
    major_number = register_chrdev(0, DEVICE_NAME, &fops); // Register the character device
    if (major_number < 0) {
        printk(KERN_ALERT "Failed to register a major number\n");
        return major_number;
    }
    printk(KERN_INFO "Loopback Device Registered with major number %d\n", major_number);
    return 0;
}

// Module exit function
static void __exit loopback_exit(void) {
    unregister_chrdev(major_number, DEVICE_NAME); // Unregister the device
    printk(KERN_INFO "Loopback device unregistered\n");
}

module_init(loopback_init); // Initialize the module
module_exit(loopback_exit); // Exit the module
