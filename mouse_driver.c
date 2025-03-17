#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>              // needed for device registration
#include <linux/uaccess.h>         // needed for copy_to_user and copy_from_user
#include <linux/proc_fs.h>         // needed for creating /proc file
#include <linux/device.h>
#include <linux/input.h>           // needed for input devices (like mouse)
#include <linux/cdev.h>
#include <linux/usb.h>            // needed for usb devices
#include <linux/printk.h>         // needed for printk for logging
#include <linux/hid.h>            // needed for HID (mouse) devices

#define DEVICE_NAME "mouse_driver"
#define BUFFER_SIZE 1024          // size of the buffer to store data
#define URB_BUFFER_SIZE 64        // size of the usb buffer (not used in this code)

#define DEVICE_VENDOR_ID 0x248a   // vendor id for maxxter
#define DEVICE_PRODUCT_ID 0x8366  // product id for ACT-MUSW-002 (mouse)

MODULE_LICENSE("GPL");           // module license (GPL)
MODULE_AUTHOR("MAHDI, HARRY, CONOR");
MODULE_DESCRIPTION("mouse driver");
MODULE_VERSION("1.0");

/* global variables */
static int major_number;         // stores the major number for the device
static char buffer[BUFFER_SIZE]; // buffer to hold mouse data
static size_t buffer_data_size = 0; // current size of data in buffer
static struct proc_dir_entry *pentry = NULL; // proc file entry
static struct class *mouse_class = NULL; // device class for the mouse
static struct device *mouse_device = NULL; // device object for the mouse
static struct input_dev *mouse_input; // input device for the mouse

// structure for character device data
struct device_data {
    struct cdev cdev; // character device structure
};
static struct device_data dev_data;

/* function to handle IOCTL (input/output control) */
long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

/* mutex to protect buffer while accessed by different processes */
static DEFINE_MUTEX(buffer_mutex);

/* variable to track the button status (0: none, 1: left, 2: right, 3: middle) */
static int button_status = 0;

/* IOCTL commands to get or set button status */
#define IOCTL_GET_BUTTON_STATUS _IOR('M', 1, int)
#define IOCTL_SET_BUTTON_STATUS _IOW('M', 2, int)

/* file operations for character device */
static int device_open(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "mouse device opened\n");
    return 0;
}

static int device_release(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "mouse device released\n");
    return 0;
}

static ssize_t device_read(struct file *file, char __user *user_buffer,
                           size_t len, loff_t *offset)
{
    size_t bytes_to_read;
    int ret;

    mutex_lock(&buffer_mutex); // lock the mutex to avoid race conditions

    // check if buffer has data
    if (buffer_data_size == 0) {
        mutex_unlock(&buffer_mutex);
        printk(KERN_INFO "device_read: buffer is empty\n");
        return 0; // nothing to read
    }

    // decide how much data to read
    bytes_to_read = min(len, buffer_data_size);

    printk(KERN_INFO "device_read: trying to copy %zu bytes to user\n", bytes_to_read);

    // copy data to user-space buffer
    ret = copy_to_user(user_buffer, buffer, bytes_to_read);
    if (ret) {
        mutex_unlock(&buffer_mutex);
        printk(KERN_ERR "device_read: copy_to_user failed with error %d\n", ret);
        return -EFAULT; // error copying data to user
    }

    // shift remaining data in buffer to the front
    if (bytes_to_read < buffer_data_size) {
        memmove(buffer, buffer + bytes_to_read, buffer_data_size - bytes_to_read);
    }

    // update buffer size
    buffer_data_size -= bytes_to_read;

    mutex_unlock(&buffer_mutex);

    printk(KERN_INFO "device_read: successfully read %zu bytes, remaining buffer size: %zu\n",
           bytes_to_read, buffer_data_size);

    return bytes_to_read;
}

/* IOCTL function to handle commands like getting or setting button status */
long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    int ret = 0;
    switch (cmd) {
        case IOCTL_GET_BUTTON_STATUS:
            // copy button status to user-space
            if (copy_to_user((int *)arg, &button_status, sizeof(button_status))) {
                ret = -EFAULT; // failed to copy data
            }
            break;

        case IOCTL_SET_BUTTON_STATUS:
            // copy button status from user-space
            if (copy_from_user(&button_status, (int *)arg, sizeof(button_status))) {
                ret = -EFAULT; // failed to copy data
            }
            break;

        default:
            ret = -EINVAL; // invalid command
            break;
    }
    return ret;
}

/* file operations for character device */
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = device_open,
    .release = device_release,
    .read = device_read,
    .unlocked_ioctl = device_ioctl, // ioctl function to handle commands
};

/* function to create /proc file */
static struct proc_ops pops = {};

static int init_proc(void)
{
    pentry = proc_create(DEVICE_NAME, 0644, NULL, &pops); // create /proc file
    if (!pentry) {
        printk(KERN_ALERT "failed to create proc entry\n");
        return -EFAULT; // failed to create proc file
    }
    printk(KERN_INFO "proc file created at /proc/%s\n", DEVICE_NAME);
    return 0;
}

static void exit_proc(void)
{
    proc_remove(pentry); // remove /proc file
    printk(KERN_INFO "proc file /proc/%s removed\n", DEVICE_NAME);
}

/* HID device table to match our mouse device */
static struct hid_device_id mouse_hid_table[] = {
    { HID_USB_DEVICE(DEVICE_VENDOR_ID, DEVICE_PRODUCT_ID) }, // match the USB device
    { },
};
MODULE_DEVICE_TABLE(hid, mouse_hid_table);

/* function to initialize the input device for the mouse */
static int mouse_input_init(struct hid_device *hdev, const struct hid_device_id *id)
{
    int ret;

    ret = hid_parse(hdev); // parse the HID descriptor
    if (ret) {
        printk(KERN_ERR "HID parse failed: %d\n", ret);
        return ret;
    }

    ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT); // start the HID hardware
    if (ret) {
        printk(KERN_ERR "HID hw start failed: %d\n", ret);
        return ret;
    }

    mouse_input = input_allocate_device(); // allocate input device
    if (!mouse_input) {
        printk(KERN_ERR "failed to allocate input device\n");
        return -ENOMEM; // memory allocation error
    }

    mouse_input->name = "mouse";
    mouse_input->phys = "mouse0";
    mouse_input->id.bustype = BUS_USB;  // set USB bus type
    mouse_input->id.vendor = id->vendor;
    mouse_input->id.product = id->product;
    mouse_input->id.version = 0x0100;

    // set up mouse buttons and axes
    set_bit(EV_REL, mouse_input->evbit);   // relative events (e.g., mouse movement)
    set_bit(REL_X, mouse_input->relbit);   // X-axis movement
    set_bit(REL_Y, mouse_input->relbit);   // Y-axis movement
    set_bit(EV_KEY, mouse_input->evbit);   // key events (buttons)
    set_bit(BTN_LEFT, mouse_input->keybit); // left button
    set_bit(BTN_RIGHT, mouse_input->keybit); // right button
    set_bit(BTN_MIDDLE, mouse_input->keybit); // middle button

    ret = input_register_device(mouse_input); // register the input device
    if (ret) {
        input_free_device(mouse_input); // free input device if registration fails
        printk(KERN_ERR "failed to register input device\n");
        return ret;
    }

    return 0;
}

/* function for USB probe to detect the mouse */
static int mouse_usb_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
    int chrdev_result;
    dev_t dev;

    mouse_input_init(hdev, id); // initialize the mouse input device

    // register character device
    chrdev_result = alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);
    major_number = MAJOR(dev); // get major number
    if (major_number < 0) {
        printk(KERN_ALERT "failed to register major number\n");
        return major_number;
    }
    printk(KERN_INFO "%s device registered with major number %d\n", DEVICE_NAME, major_number);

    // create a class for the device
    mouse_class = class_create(THIS_MODULE, "mouse_class");
    if (IS_ERR(mouse_class)) {
        unregister_chrdev(major_number, DEVICE_NAME);
        printk(KERN_ALERT "failed to register device class\n");
        return PTR_ERR(mouse_class);
    }

    // initialize and add character device
    cdev_init(&dev_data.cdev, &fops);
    chrdev_result = cdev_add(&dev_data.cdev, dev, 1);
    if (chrdev_result < 0) {
        class_destroy(mouse_class);
        unregister_chrdev(major_number, DEVICE_NAME);
        printk(KERN_ALERT "failed to add character device\n");
        return chrdev_result;
    }

    // create the device file
    mouse_device = device_create(mouse_class, NULL, dev, NULL, DEVICE_NAME);
    if (IS_ERR(mouse_device)) {
        cdev_del(&dev_data.cdev);
        class_destroy(mouse_class);
        unregister_chrdev(major_number, DEVICE_NAME);
        printk(KERN_ALERT "failed to create device\n");
        return PTR_ERR(mouse_device);
    }

    return 0;
}

/* USB disconnect function to remove the mouse device */
static void mouse_usb_disconnect(struct hid_device *hdev)
{
    input_unregister_device(mouse_input); // unregister input device
    device_destroy(mouse_class, MKDEV(major_number, 0)); // destroy the device
    cdev_del(&dev_data.cdev); // delete character device
    class_destroy(mouse_class); // destroy the class
    unregister_chrdev(major_number, DEVICE_NAME); // unregister major number

    printk(KERN_INFO "mouse device disconnected\n");
}

/* HID driver structure */
static struct hid_driver mouse_driver = {
    .name = DEVICE_NAME,
    .id_table = mouse_hid_table, // set HID device matching table
    .probe = mouse_usb_probe,    // probe function
    .remove = mouse_usb_disconnect, // disconnect function
};

static int __init mouse_driver_init(void)
{
    int result;

    // create /proc entry
    result = init_proc();
    if (result) {
        return result; // failed to create /proc file
    }

    // register HID driver
    result = hid_register_driver(&mouse_driver);
    if (result) {
        exit_proc();
        printk(KERN_ERR "failed to register HID driver\n");
        return result;
    }

    printk(KERN_INFO "mouse driver initialized successfully\n");
    return 0;
}

static void __exit mouse_driver_exit(void)
{
    // unregister HID driver
    hid_unregister_driver(&mouse_driver);
    exit_proc(); // remove /proc file
    printk(KERN_INFO "mouse driver exited\n");
}

module_init(mouse_driver_init);  // entry point for module initialization
module_exit(mouse_driver_exit);  // exit point for module cleanup
