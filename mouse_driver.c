#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>              // for device registration
#include <linux/uaccess.h>         // copy_to_user
#include <linux/proc_fs.h>         // proc file
#include <linux/device.h>
#include <linux/input.h>           // input device handling
#include <linux/cdev.h>
#include <linux/usb.h>            // for mutexes
#include <linux/printk.h>         // for deferred work
#include <linux/hid.h>            // for usbhid

#define DEVICE_NAME "mouse_driver"
#define BUFFER_SIZE 1024
#define URB_BUFFER_SIZE 64

#define DEVICE_VENDOR_ID 0x248a   // Vendor ID (Maxxter) 
#define DEVICE_PRODUCT_ID 0x8366  // Product ID (ACT-MUSW-002)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MAHDI, HARRY, CONOR");
MODULE_DESCRIPTION("Mouse Driver");
MODULE_VERSION("1.0");

/* Global variables */
static int major_number;
static char buffer[BUFFER_SIZE];           // Buffer for storing input data (mouse clicks/movements)
static size_t buffer_data_size = 0;        // Size of data stored in the buffer
static struct proc_dir_entry *pentry = NULL; // Proc file entry for monitoring data
static struct class *mouse_class = NULL;    // Device class for the mouse device
static struct device *mouse_device = NULL;  // Device structure for the mouse
static struct input_dev *mouse_input;       // Input device structure for mouse
struct device_data {
    struct cdev cdev;                      // Character device structure
};
static struct device_data dev_data;

/* Mutex for protecting the log buffer in process context */
static DEFINE_MUTEX(buffer_mutex);

/* Button status tracking (for ioctl) */
static int button_status = 0; // 0: None, 1: Left, 2: Right, 3: Middle

/* IOCTL commands for interacting with the driver */
#define IOCTL_GET_BUTTON_STATUS _IOR('M', 1, int)   // Command to get the button status
#define IOCTL_SET_BUTTON_STATUS _IOW('M', 2, int)   // Command to set the button status

/* Character device file operations */
static int device_open(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "Mouse device opened\n");
    return 0; // Return success
}

static int device_release(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "Mouse device released\n");
    return 0; // Return success
}

static ssize_t device_read(struct file *file, char __user *user_buffer,
                           size_t len, loff_t *offset)
{
    size_t bytes_to_read;
    int ret;

    mutex_lock(&buffer_mutex); // Lock buffer to prevent race conditions

    // Check if buffer has data
    if (buffer_data_size == 0) {
        mutex_unlock(&buffer_mutex);  // Unlock buffer if empty
        printk(KERN_INFO "device_read: Buffer is empty\n");
        return 0;
    }

    // Determine how much data to read
    bytes_to_read = min(len, buffer_data_size);

    printk(KERN_INFO "device_read: Attempting to copy %zu bytes to user space\n", bytes_to_read);

    // Copy data to user-space buffer
    ret = copy_to_user(user_buffer, buffer, bytes_to_read);
    if (ret) {
        mutex_unlock(&buffer_mutex); // Unlock buffer
        printk(KERN_ERR "device_read: copy_to_user failed with error %d\n", ret);
        return -EFAULT; // Return error if copy_to_user fails
    }

    // Shift remaining data in buffer
    if (bytes_to_read < buffer_data_size) {
        memmove(buffer, buffer + bytes_to_read, buffer_data_size - bytes_to_read);
    }

    // Update buffer size after reading
    buffer_data_size -= bytes_to_read;

    mutex_unlock(&buffer_mutex); // Unlock buffer

    printk(KERN_INFO "device_read: Successfully read %zu bytes, remaining buffer size: %zu\n",
           bytes_to_read, buffer_data_size);

    return bytes_to_read; // Return number of bytes read
}

/* IOCTL function for controlling button status */
long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    int ret = 0;
    switch (cmd) {
        case IOCTL_GET_BUTTON_STATUS:
            // Copy button status to user space
            if (copy_to_user((int *)arg, &button_status, sizeof(button_status))) {
                ret = -EFAULT;
            }
            break;

        case IOCTL_SET_BUTTON_STATUS:
            // Set button status from user space
            if (copy_from_user(&button_status, (int *)arg, sizeof(button_status))) {
                ret = -EFAULT;
            }
            break;

        default:
            ret = -EINVAL; // Invalid command
            break;
    }
    return ret; // Return result of the IOCTL operation
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = device_open,
    .release = device_release,
    .read = device_read,
    .unlocked_ioctl = device_ioctl,  // Add ioctl function
};

/* Proc file setup */
static struct proc_ops pops = {};  // Define empty proc operations

static int init_proc(void)
{
    pentry = proc_create(DEVICE_NAME, 0644, NULL, &pops); // Create proc file entry
    if (!pentry) {
        printk(KERN_ALERT "Failed to create proc entry\n");
        return -EFAULT;
    }
    printk(KERN_INFO "Proc file created at /proc/%s\n", DEVICE_NAME);
    return 0;
}

static void exit_proc(void)
{
    proc_remove(pentry); // Remove the proc file
    printk(KERN_INFO "Proc file /proc/%s removed\n", DEVICE_NAME);
}

/* HID device table for mouse */
static struct hid_device_id mouse_hid_table[] = {
    { HID_USB_DEVICE(DEVICE_VENDOR_ID, DEVICE_PRODUCT_ID) },
    { },
};
MODULE_DEVICE_TABLE(hid, mouse_hid_table); // Register device table

/* HID input initialization function */
static int mouse_input_init(struct hid_device *hdev, const struct hid_device_id *id)
{
    int ret;

    ret = hid_parse(hdev);  // Parse HID report descriptor
    if (ret) {
        printk(KERN_ERR "HID parse failed: %d\n", ret);
        return ret;
    }

    ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);  // Start the hardware
    if (ret) {
        printk(KERN_ERR "HID hw start failed: %d\n", ret);
        return ret;
    }

    mouse_input = input_allocate_device();  // Allocate memory for input device
    if (!mouse_input) {
        printk(KERN_ERR "Failed to allocate input device\n");
        return -ENOMEM;
    }

    // Initialize the input device
    mouse_input->name = "mouse";
    mouse_input->phys = "mouse0";
    mouse_input->id.bustype = BUS_USB;
    mouse_input->id.vendor = id->vendor;
    mouse_input->id.product = id->product;
    mouse_input->id.version = 0x0100;

    // Set input events for the mouse
    set_bit(EV_REL, mouse_input->evbit);
    set_bit(REL_X, mouse_input->relbit);
    set_bit(REL_Y, mouse_input->relbit);
    set_bit(EV_KEY, mouse_input->evbit);
    set_bit(BTN_LEFT, mouse_input->keybit);
    set_bit(BTN_RIGHT, mouse_input->keybit);
    set_bit(BTN_MIDDLE, mouse_input->keybit);

    ret = input_register_device(mouse_input);  // Register input device
    if (ret) {
        input_free_device(mouse_input);  // Free memory if registration fails
        printk(KERN_ERR "Failed to register input device\n");
        return ret;
    }

    return 0;
}

/* USB probe function */
static int mouse_usb_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
    int chrdev_result;
    dev_t dev;

    mouse_input_init(hdev, id);  // Initialize mouse input device

    chrdev_result = alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);  // Allocate major number
    major_number = MAJOR(dev);
    if (major_number < 0) {
        printk(KERN_ALERT "Failed to register major number\n");
        return major_number;
    }
    printk(KERN_INFO "%s device registered with major number %d\n", DEVICE_NAME, major_number);

    mouse_class = class_create(THIS_MODULE, "mouse_class");  // Create class
    if (IS_ERR(mouse_class)) {
        unregister_chrdev(major_number, DEVICE_NAME);  // Cleanup if class creation fails
        printk(KERN_ALERT "Failed to register device class\n");
        return PTR_ERR(mouse_class);
    }

    cdev_init(&dev_data.cdev, &fops);  // Initialize character device
    dev_data.cdev.owner = THIS_MODULE;
    cdev_add(&dev_data.cdev, MKDEV(major_number, 0), 1);  // Add character device
    printk(KERN_INFO "Device node created at /dev/%s\n", DEVICE_NAME);

    mouse_device = device_create(mouse_class, NULL, MKDEV(major_number, 0), NULL, DEVICE_NAME); // Create device
    if (IS_ERR(mouse_device)) {
        class_destroy(mouse_class);  // Cleanup if device creation fails
        unregister_chrdev(major_number, DEVICE_NAME);
        printk(KERN_ALERT "Failed to create the device\n");
        return PTR_ERR(mouse_device);
    }

    init_proc();  // Create proc file

    printk(KERN_INFO "Mouse driver - Probe executed\n");
    return 0;
}

/* USB remove function */
static void mouse_usb_remove(struct hid_device *hdev)
{
    hid_hw_stop(hdev);  // Stop the hardware
    input_unregister_device(mouse_input);  // Unregister input device
    exit_proc();  // Cleanup proc file
    device_destroy(mouse_class, MKDEV(major_number, 0));  // Destroy device
    class_destroy(mouse_class);  // Destroy class
    unregister_chrdev(major_number, DEVICE_NAME);  // Unregister character device
    printk(KERN_INFO "Mouse - Disconnect executed\n");
}

/* Handle HID raw events (button presses, movements) */
static int mouse_raw_event(struct hid_device *hdev, struct hid_report *report, u8 *data, int size)
{
    int buttons;
    int x_delta;
    int y_delta;
    unsigned int available;  

    if (size < 3) {
        return 0;  // Return if invalid data size
    }

    buttons = data[0];  // Extract button data
    x_delta = (int)((signed char)data[1]);  // Extract X movement
    y_delta = (int)((signed char)data[2]);  // Extract Y movement

    input_report_rel(mouse_input, REL_X, x_delta);  // Report X movement
    input_report_rel(mouse_input, REL_Y, y_delta);  // Report Y movement
    input_sync(mouse_input);  // Sync input data

    available = BUFFER_SIZE - buffer_data_size; 

    // Track button presses and update buffer
    if (buttons & (1 << 0)) {
        printk(KERN_INFO "Left Button Pressed\n");
        snprintf(buffer + buffer_data_size, available, "Left Button Pressed");
        button_status = 1; // Left button pressed
    }

    if (buttons & (1 << 1)) {
        printk(KERN_INFO "Right Button Pressed\n");
        button_status = 2; // Right button pressed
    }

    if (buttons & (1 << 2)) {
        printk(KERN_INFO "Middle button Pressed\n");
        button_status = 3; // Middle button pressed
    }

    return 0;
}

/* HID driver setup */
static struct hid_driver mouse_hid_driver = {
    .name = "mouse_driver",
    .id_table = mouse_hid_table,
    .probe = mouse_usb_probe,
    .remove = mouse_usb_remove,
    .raw_event = mouse_raw_event,
};

static int __init mouse_driver_init(void)
{
    int result;

    result = hid_register_driver(&mouse_hid_driver);  // Register the HID driver
    if (result) {
        printk(KERN_ERR "Failed to register HID driver\n");
    }

    printk(KERN_INFO "Mouse driver module loaded\n");
    return result;
}

static void __exit mouse_driver_exit(void)
{
    hid_unregister_driver(&mouse_hid_driver);  // Unregister HID driver
    printk(KERN_INFO "Mouse driver module unloaded\n");
}

module_init(mouse_driver_init);
module_exit(mouse_driver_exit);
