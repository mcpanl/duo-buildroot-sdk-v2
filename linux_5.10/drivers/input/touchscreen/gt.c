// goodix.c
#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/of.h>

struct goodix_data {
    struct gpio_desc *reset_gpio;
};

static int goodix_probe(struct platform_device *pdev)
{
    struct goodix_data *goodix;

    goodix = devm_kzalloc(&pdev->dev, sizeof(*goodix), GFP_KERNEL);
    if (!goodix)
        return -ENOMEM;

    platform_set_drvdata(pdev, goodix);

    // 从DT中获取复位GPIO
    goodix->reset_gpio = devm_gpiod_get_optional(&pdev->dev, "reset", GPIOD_OUT_HIGH);
    if (IS_ERR(goodix->reset_gpio)) {
        dev_err(&pdev->dev, "Unable to get reset GPIO\n");
        return PTR_ERR(goodix->reset_gpio);
    }

    if (goodix->reset_gpio) {
        // 复位过程：
        gpiod_set_value(goodix->reset_gpio, 1);
        msleep(20);
        gpiod_set_value(goodix->reset_gpio, 0);
        msleep(20);
        gpiod_set_value(goodix->reset_gpio, 1);
        msleep(20);
        dev_info(&pdev->dev, "Goodix reset successfully\n");

    }

    return 0;
}

static int goodix_remove(struct platform_device *pdev)
{
    return 0;
}

static const struct of_device_id goodix_dt_ids[] = {
    { .compatible = "gt,gt" },
    { }
};

MODULE_DEVICE_TABLE(of, goodix_dt_ids);

static struct platform_driver goodix_driver = {
    .driver = {
        .name = "goodix",
        .of_match_table = goodix_dt_ids,
    },
    .probe = goodix_probe,
    .remove = goodix_remove,
};

module_platform_driver(goodix_driver);

MODULE_LICENSE("GPL");

MODULE_AUTHOR("Your Name");

MODULE_DESCRIPTION("Goodix Touch Controller - Reset GPIO example");


