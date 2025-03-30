#include <linux/module.h>
#include <linux/fs.h>
#include <linux/device.h>

#define DEVICE_NAME "my_device"
static int major_num;

static int my_open(struct inode *inode, struct file *file) {
    printk(KERN_INFO "my_device: Device opened\n");
    return 0;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = my_open,
};

static int __init my_init(void) {
    major_num = register_chrdev(0, DEVICE_NAME, &fops);
    if (major_num < 0) {
        printk(KERN_ALERT "my_device: Registration failed\n");
        return major_num;
    }
    printk(KERN_INFO "my_device: Registered with major %d\n", major_num);
    return 0;
}

static void __exit my_exit(void) {
    unregister_chrdev(major_num, DEVICE_NAME);
    printk(KERN_INFO "my_device: Unregistered\n");
}

module_init(my_init);
module_exit(my_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Sample Out-of-Tree Linux Driver");
