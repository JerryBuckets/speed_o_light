#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/gpio.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/cdev.h>
#include <linux/math64.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>

#define DEVICE_NAME    "project"
#define BTN1_GPIO      26
#define BTN2_GPIO      12
#define WINDOW_SIZE    10

/* ── PWM/sysfs parameters ───────────────────────────────────────────────── */

static unsigned int pwm_period_ns = 10000000;
module_param(pwm_period_ns, uint, 0444);
MODULE_PARM_DESC(pwm_period_ns, "PWM period (ns), default 10ms");

static unsigned int gpio1 = 25, gpio2 = 6, gpio3 = 17;
module_param(gpio1, uint, 0444); MODULE_PARM_DESC(gpio1, "LED1 GPIO");
module_param(gpio2, uint, 0444); MODULE_PARM_DESC(gpio2, "LED2 GPIO");
module_param(gpio3, uint, 0444); MODULE_PARM_DESC(gpio3, "LED3 GPIO");

struct pwm_led {
    unsigned int    gpio;
    int             duty, state;
    struct hrtimer  timer;
    ktime_t         high_time, low_time;
    char            name[10];
};

static struct pwm_led leds[3];
static DEFINE_MUTEX(duty_lock);

static void update_pwm_timing(struct pwm_led *led);
static void __led_set(struct pwm_led *led, int duty);

/* ── sysfs show/store for led?_duty ────────────────────────────────────────── */

static ssize_t led_duty_show(struct device *dev,
                             struct device_attribute *attr,
                             char *buf)
{
    struct pwm_led *led;
    if (!strcmp(attr->attr.name, "led1_duty")) led = &leds[0];
    else if (!strcmp(attr->attr.name, "led2_duty")) led = &leds[1];
    else                                      led = &leds[2];
    return scnprintf(buf, PAGE_SIZE, "%d\n", led->duty);
}

static ssize_t led_duty_store(struct device *dev,
                              struct device_attribute *attr,
                              const char *buf,
                              size_t count)
{
    int val, ret;
    struct pwm_led *led;
    ret = kstrtoint(buf, 10, &val);
    if (ret < 0 || val < 0 || val > 100) return -EINVAL;
    if (!strcmp(attr->attr.name, "led1_duty")) led = &leds[0];
    else if (!strcmp(attr->attr.name, "led2_duty")) led = &leds[1];
    else                                          led = &leds[2];
    __led_set(led, val);
    return count;
}

static DEVICE_ATTR(led1_duty, 0644, led_duty_show, led_duty_store);
static DEVICE_ATTR(led2_duty, 0644, led_duty_show, led_duty_store);
static DEVICE_ATTR(led3_duty, 0644, led_duty_show, led_duty_store);

static struct device_attribute *led_attrs[] = {
    &dev_attr_led1_duty,
    &dev_attr_led2_duty,
    &dev_attr_led3_duty,
};

/* ── Sliding‑window alternating‑press counter ────────────────────────────── */

static spinlock_t   window_lock;
static int          buckets[WINDOW_SIZE];
static int          idx;
static int          last_gpio = -1;
static struct hrtimer slide_timer;

/* advance window every second */
static enum hrtimer_restart slide_cb(struct hrtimer *t)
{
    ktime_t period = ktime_set(1, 0);
    unsigned long flags;

    spin_lock_irqsave(&window_lock, flags);
    idx = (idx + 1) % WINDOW_SIZE;
    buckets[idx] = 0;
    spin_unlock_irqrestore(&window_lock, flags);

    hrtimer_forward_now(t, period);
    return HRTIMER_RESTART;
}

/* IRQ handler: count only if GPIO alternates */
static irqreturn_t button_irq(int irq, void *dev_id)
{
    int gpio = *(int *)dev_id;
    unsigned long flags;

    spin_lock_irqsave(&window_lock, flags);
    if (gpio != last_gpio) {
        buckets[idx]++;
        last_gpio = gpio;
    }
    spin_unlock_irqrestore(&window_lock, flags);

    return IRQ_HANDLED;
}

/* ── Character‑device callbacks ──────────────────────────────────────────── */

static dev_t           devno;
static struct cdev     project_cdev;
static struct class   *project_class;
static struct device  *project_device;

static int project_open(struct inode *inode, struct file *file)
{
    try_module_get(THIS_MODULE);
    return 0;
}

static int project_release(struct inode *inode, struct file *file)
{
    module_put(THIS_MODULE);
    return 0;
}

/* read: sum of last 10s of presses */
static ssize_t project_read(struct file *file,
                            char __user *buf,
                            size_t count,
                            loff_t *ppos)
{
    char kbuf[32];
    int sum = 0, i, len;
    unsigned long flags;

    spin_lock_irqsave(&window_lock, flags);
    for (i = 0; i < WINDOW_SIZE; i++)
        sum += buckets[i];
    spin_unlock_irqrestore(&window_lock, flags);

    len = scnprintf(kbuf, sizeof(kbuf), "%d\n", sum);
    return simple_read_from_buffer(buf, count, ppos, kbuf, len);
}

/* write: your existing 0–100% → split across LEDs */
static ssize_t project_write(struct file *file,
                             const char __user *ubuf,
                             size_t count,
                             loff_t *ppos)
{
    char kbuf[16];
    int p, scaled, d1, d2, d3;

    if (!count || count >= sizeof(kbuf)) return -EINVAL;
    if (copy_from_user(kbuf, ubuf, count)) return -EFAULT;
    kbuf[count] = '\0';

    if (kstrtoint(kbuf, 10, &p) < 0 || p < 0 || p > 100)
        return -EINVAL;

    scaled = p * 3;
    d1 = min(scaled, 100);
    d2 = min(max(scaled - 100, 0), 100);
    d3 = min(max(scaled - 200, 0), 100);

    __led_set(&leds[0], d1);
    __led_set(&leds[1], d2);
    __led_set(&leds[2], d3);

    return count;
}

static const struct file_operations project_fops = {
    .owner   = THIS_MODULE,
    .open    = project_open,
    .release = project_release,
    .read    = project_read,
    .write   = project_write,
};

/* ── PWM timer callback + helpers ───────────────────────────────────────── */

static enum hrtimer_restart pwm_timer_callback(struct hrtimer *timer)
{
    struct pwm_led *led = container_of(timer, struct pwm_led, timer);
    ktime_t interval;

    mutex_lock(&duty_lock);
    if (led->duty == 0) {
        gpio_set_value(led->gpio, 0);
        mutex_unlock(&duty_lock);
        return HRTIMER_NORESTART;
    }
    if (led->duty == 100) {
        gpio_set_value(led->gpio, 1);
        mutex_unlock(&duty_lock);
        return HRTIMER_NORESTART;
    }
    if (led->state) {
        gpio_set_value(led->gpio, 0);
        interval = led->low_time;
        led->state = 0;
    } else {
        gpio_set_value(led->gpio, 1);
        interval = led->high_time;
        led->state = 1;
    }
    mutex_unlock(&duty_lock);

    hrtimer_forward_now(timer, interval);
    return HRTIMER_RESTART;
}

static void update_pwm_timing(struct pwm_led *led)
{
    u64 high_ns, low_ns;
    if (led->duty == 0 || led->duty == 100) return;

    {
        u64 tmp = (u64)pwm_period_ns * led->duty;
        high_ns = div_u64(tmp, 100);
    }
    low_ns = pwm_period_ns - high_ns;

    led->high_time = ktime_set(0, high_ns);
    led->low_time  = ktime_set(0, low_ns);
}

static void __led_set(struct pwm_led *led, int duty)
{
    mutex_lock(&duty_lock);
    led->duty = duty;
    update_pwm_timing(led);
    mutex_unlock(&duty_lock);

    hrtimer_cancel(&led->timer);
    gpio_set_value(led->gpio, duty == 100);

    if (duty > 0 && duty < 100) {
        led->state = 1;
        gpio_set_value(led->gpio, 1);
        hrtimer_start(&led->timer,
                      led->high_time,
                      HRTIMER_MODE_REL);
    }
}

/* ── File‐scope for button IRQ cleanup ───────────────────────────────────── */

static int btn1_id, btn2_id;
static int irq1, irq2;

/* ── Module init / exit ─────────────────────────────────────────────────── */

static int __init project_init(void)
{
    int ret, i;
    ktime_t period = ktime_set(1, 0);

    /* PWM LEDs */
    leds[0].gpio = gpio1; snprintf(leds[0].name, 10, "led1");
    leds[1].gpio = gpio2; snprintf(leds[1].name, 10, "led2");
    leds[2].gpio = gpio3; snprintf(leds[2].name, 10, "led3");
    for (i = 0; i < 3; i++) {
        hrtimer_init(&leds[i].timer,
                     CLOCK_MONOTONIC,
                     HRTIMER_MODE_REL);
        leds[i].timer.function = pwm_timer_callback;
        leds[i].duty  = 0;
        leds[i].state = 0;
    }

    /* sliding window */
    spin_lock_init(&window_lock);
    for (i = 0; i < WINDOW_SIZE; i++)
        buckets[i] = 0;
    idx = 0;
    hrtimer_init(&slide_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    slide_timer.function = slide_cb;
    hrtimer_start(&slide_timer, period, HRTIMER_MODE_REL);

    /* char device */
    ret = alloc_chrdev_region(&devno, 0, 1, DEVICE_NAME);
    if (ret) return ret;
    cdev_init(&project_cdev, &project_fops);
    project_cdev.owner = THIS_MODULE;
    ret = cdev_add(&project_cdev, devno, 1);
    if (ret) goto err_chr;
    project_class = class_create(THIS_MODULE, DEVICE_NAME);
    if (IS_ERR(project_class)) { ret = PTR_ERR(project_class); goto err_cdev; }
    project_device = device_create(project_class, NULL, devno, NULL, DEVICE_NAME);
    if (IS_ERR(project_device)) { ret = PTR_ERR(project_device); goto err_cls; }

    /* sysfs LED files */
    for (i = 0; i < ARRAY_SIZE(led_attrs); i++) {
        ret = device_create_file(project_device, led_attrs[i]);
        if (ret) {
            pr_err("%s: sysfs %s create failed: %d\n",
                   DEVICE_NAME, led_attrs[i]->attr.name, ret);
            while (--i >= 0)
                device_remove_file(project_device, led_attrs[i]);
            goto err_cls;
        }
    }

    /* request LED GPIOs */
    for (i = 0; i < 3; i++) {
        ret = devm_gpio_request_one(project_device,
                                    leds[i].gpio,
                                    GPIOF_OUT_INIT_LOW,
                                    leds[i].name);
        if (ret) goto err_dev;
    }

    /* button1 */
    btn1_id = BTN1_GPIO;
    ret = gpio_request(BTN1_GPIO, "button1");
    if (ret) goto err_dev;
    gpio_direction_input(BTN1_GPIO);
    irq1 = gpio_to_irq(BTN1_GPIO);
    ret = request_irq(irq1,
                      button_irq,
                      IRQF_TRIGGER_RISING | IRQF_ONESHOT,
                      DEVICE_NAME,
                      &btn1_id);
    if (ret) { gpio_free(BTN1_GPIO); goto err_dev; }

    /* button2 */
    btn2_id = BTN2_GPIO;
    ret = gpio_request(BTN2_GPIO, "button2");
    if (ret) goto free_btn1;
    gpio_direction_input(BTN2_GPIO);
    irq2 = gpio_to_irq(BTN2_GPIO);
    ret = request_irq(irq2,
                      button_irq,
                      IRQF_TRIGGER_RISING | IRQF_ONESHOT,
                      DEVICE_NAME,
                      &btn2_id);
    if (ret) { gpio_free(BTN2_GPIO); goto free_btn1; }

    pr_info("%s: loaded; alt presses on %d/%d over last %ds\n",
            DEVICE_NAME, BTN1_GPIO, BTN2_GPIO, WINDOW_SIZE);
    return 0;

free_btn1:
    free_irq(irq1, &btn1_id);
    gpio_free(BTN1_GPIO);
err_dev:
    device_destroy(project_class, devno);
err_cls:
    class_destroy(project_class);
err_cdev:
    cdev_del(&project_cdev);
err_chr:
    unregister_chrdev_region(devno, 1);
    hrtimer_cancel(&slide_timer);
    return ret;
}

static void __exit project_exit(void)
{
    int i;

    free_irq(irq2, &btn2_id);
    gpio_free(BTN2_GPIO);
    free_irq(irq1, &btn1_id);
    gpio_free(BTN1_GPIO);

    hrtimer_cancel(&slide_timer);

    /* remove sysfs LED files */
    for (i = 0; i < ARRAY_SIZE(led_attrs); i++)
        device_remove_file(project_device, led_attrs[i]);

    device_destroy(project_class, devno);
    class_destroy(project_class);
    cdev_del(&project_cdev);
    unregister_chrdev_region(devno, 1);

    /* cleanup PWM */
    for (i = 0; i < 3; i++) {
        hrtimer_cancel(&leds[i].timer);
        gpio_set_value(leds[i].gpio, 0);
    }

    pr_info("%s: unloaded\n", DEVICE_NAME);
}

module_init(project_init);
module_exit(project_exit);

MODULE_LICENSE("GPL");
