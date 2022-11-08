#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/of_gpio.h>
#include <linux/uaccess.h>
#include <linux/types.h>
#include <linux/irq.h>
#include <linux/of_irq.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/poll.h>

#define IRQ_NAME    "key_irq"
#define IRQ_CNT     1
#define VAILD_VALUE 0x01
#define INVAILD_VALUE 0xff

struct key_describe {
    int gpio_num;
    int irq_num;
    atomic_t key_value;
    atomic_t release_value;
};

struct key_t {
    dev_t dev_id;
    int major;
    int minor;
    struct cdev cdev_t;
    struct class *class_t;
    struct device *device_t;
    struct timer_list timer_t;
    struct device_node *nd;
    struct key_describe key_des;
    wait_queue_head_t block_head;
};

static struct key_t key_dev;

static void key_timer_handle(unsigned long arg)
{
    int value = 0;
    struct key_t *dev = (struct key_t *)arg;
    struct key_describe *key_t = &dev->key_des;

    value = gpio_get_value(dev->key_des.gpio_num);
    if (0 == value) {
        atomic_set(&key_t->key_value, value);
        printk("Press on\r\n");
    } else {
        atomic_set(&key_t->key_value, 0x80 | value);
        atomic_set(&key_t->release_value, 1);
        printk("Loosen\r\n");
    }

    if (atomic_read(&key_t->release_value)) {
        printk("prepare to wake up event......\r\n");
        wake_up(&dev->block_head);
        printk("wake up event done!\r\n");     
    }
}

static irqreturn_t key_irq_hanlde(int irq, void *data)
{
    struct key_t *dev = (struct key_t *)data;

    key_dev.timer_t.data = (unsigned long)data;
    mod_timer(&dev->timer_t, jiffies + msecs_to_jiffies(20));

    return IRQ_RETVAL(IRQ_HANDLED);
}

static int key_open(struct inode * inode, struct file *filp)
{
    filp->private_data = &key_dev;

    return 0;
}

ssize_t key_read(struct file *filp, char __user *buf, size_t size, loff_t *offt)
{
    int release_key;
    int key_value;
    int ret = 0;
    struct key_t *dev = (struct key_t*)filp->private_data;

    if (filp->f_flags & O_NONBLOCK) {
        printk("no block read\r\n");
        if (atomic_read(&dev->key_des.release_value) == 0)
            return -EAGAIN;
    } else {
        wait_event_interruptible(dev->block_head, atomic_read(&dev->key_des.release_value));
    }

    release_key = atomic_read(&dev->key_des.release_value);
    key_value = atomic_read(&dev->key_des.key_value);

    if (release_key) {
        ret = copy_to_user(buf, &key_value, sizeof(key_value));
        atomic_set(&dev->key_des.release_value, 0);
    } else {
        goto err_data;
    }

    return ret;

err_data:
    return ret;
}

long key_ctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    return 0;
}

static int key_release(struct inode *inode, struct file *filp)
{
    return 0;
}

static unsigned int key_poll(struct file *filp, struct poll_table_struct *wait)
{
    unsigned int mask = 0;
    struct key_t *dev = (struct key_t*)filp->private_data;

    poll_wait(filp, &dev->block_head, wait);

    if (atomic_read(&dev->key_des.release_value))
		mask = POLLIN | POLLRDNORM;

    return mask;
}

static const struct file_operations timer_ops = {
    .owner = THIS_MODULE,
    .open = key_open,
    .read = key_read,
    .unlocked_ioctl = key_ctl,
    .release = key_release,
    .poll = key_poll
};

static int key_gpio_init(struct key_t *dev)
{
    int ret;

    dev->nd = of_find_node_by_path("/key");
	if (!dev->nd) {
		printk("node 'key' not found\n");
        ret = -EINVAL;
		goto err_node;
	}

    dev->key_des.gpio_num = of_get_named_gpio(dev->nd, "key-gpios", 0);
    if (dev->key_des.gpio_num < 0) {
        ret = -EINVAL;
        goto err_gpio;
    }

    ret = gpio_request(dev->key_des.gpio_num, "key_irq");
	if (ret) {
		printk("failed to request GPIO for KEY \n");
        goto err_request;
	}

    ret = gpio_direction_input(dev->key_des.gpio_num);
    if (ret) {
		printk("Failed to set KEY GPIO direction\n");
        goto err_dir;
	}

#if 1
    dev->key_des.irq_num = irq_of_parse_and_map(dev->nd, 0);
#else
    dev->key_des.irq_num = gpio_to_irq(dev->key_des.gpio_num);
#endif
    printk("KEY irq number:%d\r\n", dev->key_des.irq_num);

    ret = request_irq(dev->key_des.irq_num, key_irq_hanlde, IRQ_TYPE_EDGE_BOTH, "key-irq", (void*)dev);
    if (ret) {
        printk("requset iqr failed\r\n");
        goto err_irq;
    }

    return 0;

err_irq:
err_dir:
err_request:
    gpio_free(dev->key_des.gpio_num);
err_gpio:
err_node:

    return ret;
}

static int __init key6ul_init(void)
{
    int ret;

    key_dev.major = 0;

    //分配设备ID
    if (key_dev.major) {
        key_dev.dev_id = MKDEV(key_dev.major, 0);
        key_dev.minor = 0;
        register_chrdev_region(key_dev.dev_id, IRQ_CNT, IRQ_NAME);
    } else {
        alloc_chrdev_region(&key_dev.dev_id, 0, IRQ_CNT, IRQ_NAME);
        key_dev.major = MAJOR(key_dev.dev_id);
        key_dev.minor = MINOR(key_dev.dev_id);
    }
    printk("key_dev id:%d major:%d minor:%d\n", key_dev.dev_id, key_dev.major, key_dev.minor);

    //添加字符设备
    key_dev.cdev_t.owner = THIS_MODULE;
    cdev_init(&key_dev.cdev_t, &timer_ops);
    ret = cdev_add(&key_dev.cdev_t, key_dev.dev_id, IRQ_CNT);
	if (ret < 0) {
		printk("Could not add cdev (err %d)\n", -ret);
		goto err_cdev;
	}

    //添加设备节点
    key_dev.class_t = class_create(THIS_MODULE, IRQ_NAME);
	if (IS_ERR(key_dev.class_t)) {
		printk(KERN_ERR "Error creating key_dev class.\n");
        ret = PTR_ERR(key_dev.class_t);
		goto err_class;
	}
    key_dev.device_t = device_create(key_dev.class_t, NULL, key_dev.dev_id, NULL, IRQ_NAME);
	if (IS_ERR(key_dev.class_t)) {
		printk(KERN_ERR "Error creating key_dev device.\n");
        ret = PTR_ERR(key_dev.device_t);
		goto err_device;
	}

    ret = key_gpio_init(&key_dev);
    if (ret == 0) {
        printk("init led gpio success\r\n");
    } else {
        printk("init led gpio error!\r\n");
        goto err_init;
    }

    //初始化按键值
    atomic_set(&key_dev.key_des.key_value, INVAILD_VALUE);
    atomic_set(&key_dev.key_des.release_value, 0);

    //初始化定时器
    init_timer(&key_dev.timer_t);
    key_dev.timer_t.function = key_timer_handle;

    //初始化队列头
    init_waitqueue_head(&key_dev.block_head);

    return 0;

err_init:
err_device:
    device_destroy(key_dev.class_t, key_dev.dev_id);
err_class:
    class_destroy(key_dev.class_t);
err_cdev:
    cdev_del(&key_dev.cdev_t);

    return ret;
}

static void __exit key6ul_exit(void)
{
    free_irq(key_dev.key_des.irq_num, &key_dev);
    gpio_free(key_dev.key_des.gpio_num);
    device_destroy(key_dev.class_t, key_dev.dev_id);
    class_destroy(key_dev.class_t);
    cdev_del(&key_dev.cdev_t);
    unregister_chrdev_region(key_dev.dev_id, IRQ_CNT);
}

module_init(key6ul_init);
module_exit(key6ul_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Alex_min");
