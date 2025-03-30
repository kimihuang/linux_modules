/**
 * hw_module_detector.c - 硬件模块检测驱动
 *
 * 这个驱动负责读取32位硬件标识寄存器，并提供接口给其他驱动
 * 用于判断当前硬件包含哪些模块。
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>

#define DRIVER_NAME "hw_module_detector"
#define HW_MODULE_REG_SIZE 4  /* 32位寄存器 = 4字节 */

struct hw_module_detector {
    void __iomem *reg_base;
    u32 module_bits;
    struct device *dev;
    struct class *hw_class;
    struct device *hw_device;
};

static struct hw_module_detector *hw_detector;

/* 读取硬件模块寄存器 */
static void read_hw_module_reg(struct hw_module_detector *detector)
{
    if (!detector || !detector->reg_base)
        return;

    /* 从寄存器读取32位值 */
    detector->module_bits = readl(detector->reg_base);
    dev_info(detector->dev, "硬件模块寄存器值: 0x%08x\n", detector->module_bits);
}

/**
 * hw_module_present - 检查特定的硬件模块是否存在
 * @module_bit: 要检查的模块对应的位号(0-31)
 *
 * 返回: 如果模块存在返回true，否则返回false
 */
bool hw_module_present(unsigned int module_bit)
{
    if (!hw_detector || module_bit >= 32)
        return false;

    return !!(hw_detector->module_bits & BIT(module_bit));
}
EXPORT_SYMBOL_GPL(hw_module_present);

/**
 * hw_get_module_mask - 获取完整的模块位掩码
 *
 * 返回: 32位模块寄存器的值
 */
u32 hw_get_module_mask(void)
{
    if (!hw_detector)
        return 0;

    return hw_detector->module_bits;
}
EXPORT_SYMBOL_GPL(hw_get_module_mask);

/* sysfs接口用于用户空间访问 */
static ssize_t module_bits_show(struct device *dev, 
                              struct device_attribute *attr, char *buf)
{
    if (!hw_detector)
        return -EINVAL;

    return sprintf(buf, "0x%08x\n", hw_detector->module_bits);
}

static ssize_t module_present_show(struct device *dev,
                                 struct device_attribute *attr, char *buf)
{
    unsigned int bit;
    int ret;
    
    if (!hw_detector)
        return -EINVAL;
    
    ret = kstrtouint(attr->attr.name + 7, 10, &bit);
    if (ret || bit >= 32)
        return -EINVAL;
    
    return sprintf(buf, "%d\n", hw_module_present(bit) ? 1 : 0);
}

static DEVICE_ATTR_RO(module_bits);

/* 创建每个位的sysfs节点 */
static struct device_attribute *module_present_attrs[32];

static int create_module_sysfs_files(struct device *dev)
{
    int i, ret;
    char name[20];

    ret = device_create_file(dev, &dev_attr_module_bits);
    if (ret)
        return ret;

    for (i = 0; i < 32; i++) {
        snprintf(name, sizeof(name), "module_%d", i);
        module_present_attrs[i] = kzalloc(sizeof(struct device_attribute), GFP_KERNEL);
        if (!module_present_attrs[i]) {
            ret = -ENOMEM;
            goto err_cleanup;
        }
        
        module_present_attrs[i]->attr.name = kstrdup(name, GFP_KERNEL);
        if (!module_present_attrs[i]->attr.name) {
            kfree(module_present_attrs[i]);
            module_present_attrs[i] = NULL;
            ret = -ENOMEM;
            goto err_cleanup;
        }
        
        module_present_attrs[i]->attr.mode = 0444;
        module_present_attrs[i]->show = module_present_show;
        
        ret = device_create_file(dev, module_present_attrs[i]);
        if (ret) {
            kfree(module_present_attrs[i]->attr.name);
            kfree(module_present_attrs[i]);
            module_present_attrs[i] = NULL;
            goto err_cleanup;
        }
    }
    
    return 0;

err_cleanup:
    for (i = 0; i < 32; i++) {
        if (module_present_attrs[i]) {
            device_remove_file(dev, module_present_attrs[i]);
            kfree(module_present_attrs[i]->attr.name);
            kfree(module_present_attrs[i]);
        }
    }
    device_remove_file(dev, &dev_attr_module_bits);
    return ret;
}

static void remove_module_sysfs_files(struct device *dev)
{
    int i;
    
    for (i = 0; i < 32; i++) {
        if (module_present_attrs[i]) {
            device_remove_file(dev, module_present_attrs[i]);
            kfree(module_present_attrs[i]->attr.name);
            kfree(module_present_attrs[i]);
            module_present_attrs[i] = NULL;
        }
    }
    
    device_remove_file(dev, &dev_attr_module_bits);
}

/* DT匹配表 */
static const struct of_device_id hw_module_detector_of_match[] = {
    { .compatible = "vendor,hw-module-detector" },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, hw_module_detector_of_match);

static int hw_module_detector_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct resource *res;
    struct hw_module_detector *detector;
    int ret;

    detector = devm_kzalloc(dev, sizeof(*detector), GFP_KERNEL);
    if (!detector)
        return -ENOMEM;

    detector->dev = dev;

    /* 获取寄存器地址 */
    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!res) {
        dev_err(dev, "无法获取寄存器地址资源\n");
        return -EINVAL;
    }

    detector->reg_base = devm_ioremap_resource(dev, res);
    if (IS_ERR(detector->reg_base))
        return PTR_ERR(detector->reg_base);

    /* 读取硬件模块寄存器值 */
    read_hw_module_reg(detector);

    /* 创建sysfs类和设备 */
    detector->hw_class = class_create("hw_module");
    if (IS_ERR(detector->hw_class)) {
        dev_err(dev, "无法创建sysfs类\n");
        return PTR_ERR(detector->hw_class);
    }

    detector->hw_device = device_create(detector->hw_class, NULL, 
                                      MKDEV(0, 0), detector, "hw_module");
    if (IS_ERR(detector->hw_device)) {
        dev_err(dev, "无法创建sysfs设备\n");
        ret = PTR_ERR(detector->hw_device);
        goto err_class;
    }

    /* 创建sysfs属性文件 */
    ret = create_module_sysfs_files(detector->hw_device);
    if (ret) {
        dev_err(dev, "无法创建sysfs属性文件\n");
        goto err_device;
    }

    hw_detector = detector;
    platform_set_drvdata(pdev, detector);

    dev_info(dev, "硬件模块检测驱动初始化成功\n");
    return 0;

err_device:
    device_destroy(detector->hw_class, MKDEV(0, 0));
err_class:
    class_destroy(detector->hw_class);
    return ret;
}

static int hw_module_detector_remove(struct platform_device *pdev)
{
    struct hw_module_detector *detector = platform_get_drvdata(pdev);

    if (detector) {
        remove_module_sysfs_files(detector->hw_device);
        device_destroy(detector->hw_class, MKDEV(0, 0));
        class_destroy(detector->hw_class);
    }

    hw_detector = NULL;
    return 0;
}

static struct platform_driver hw_module_detector_driver = {
    .probe = hw_module_detector_probe,
    .remove = hw_module_detector_remove,
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = hw_module_detector_of_match,
    },
};

module_platform_driver(hw_module_detector_driver);

MODULE_AUTHOR("Developer");
MODULE_DESCRIPTION("硬件模块检测驱动");
MODULE_LICENSE("GPL v2");
