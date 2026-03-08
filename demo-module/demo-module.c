/*
 * Demo kernel module for Quantum board
 *
 * A simple example kernel module demonstrating
 * Buildroot external package infrastructure.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>

#define MODULE_NAME "demo_module"
#define PROC_FILENAME "demo_module"
#define BUFFER_SIZE 1024

static struct proc_dir_entry *proc_file;
static char proc_buffer[BUFFER_SIZE];
static int proc_buffer_len = 0;

static ssize_t proc_read(struct file *file, char __user *buf,
			 size_t count, loff_t *pos)
{
	if (*pos >= proc_buffer_len)
		return 0;

	if (count > proc_buffer_len - *pos)
		count = proc_buffer_len - *pos;

	if (copy_to_user(buf, proc_buffer + *pos, count))
		return -EFAULT;

	*pos += count;
	return count;
}

static ssize_t proc_write(struct file *file, const char __user *buf,
			  size_t count, loff_t *pos)
{
	if (count > BUFFER_SIZE - 1)
		count = BUFFER_SIZE - 1;

	if (copy_from_user(proc_buffer, buf, count))
		return -EFAULT;

	proc_buffer[count] = '\0';
	proc_buffer_len = count;

	printk(KERN_INFO "Demo module: Received %zu bytes\n", count);

	return count;
}

static const struct proc_ops proc_fops = {
	.proc_read = proc_read,
	.proc_write = proc_write,
};

static int __init demo_module_init(void)
{
	proc_file = proc_create(PROC_FILENAME, 0666, NULL, &proc_fops);
	if (!proc_file) {
		printk(KERN_ERR "Demo module: Failed to create /proc/%s\n",
		       PROC_FILENAME);
		return -ENOMEM;
	}

	snprintf(proc_buffer, BUFFER_SIZE, "Demo module initialized\n");
	proc_buffer_len = strlen(proc_buffer);

	printk(KERN_INFO "Demo module: Loaded successfully\n");
	printk(KERN_INFO "Demo module: Use cat /proc/%s to read\n", PROC_FILENAME);
	printk(KERN_INFO "Demo module: Use echo 'text' > /proc/%s to write\n",
	       PROC_FILENAME);

	return 0;
}

static void __exit demo_module_exit(void)
{
	remove_proc_entry(PROC_FILENAME, NULL);
	printk(KERN_INFO "Demo module: Unloaded\n");
}

module_init(demo_module_init);
module_exit(demo_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Quantum Board");
MODULE_DESCRIPTION("Demo kernel module for Quantum board");
MODULE_VERSION("1.0");
