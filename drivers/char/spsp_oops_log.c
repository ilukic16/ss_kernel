// The spsp_oops module
// must have includes
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/moduleparam.h>

#include <linux/platform_device.h>      /* For platform devices */
#include <linux/of.h>                   /* For DT*/

// interface for userspace through sysfs
extern int spsp_pid;
module_param(spsp_pid, int, S_IWUSR | S_IRUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(spsp_pid, "Spirosphere application pid");

// automatic loading of the driver during boot process
static const struct of_device_id spsp_oops_gpiod_dt_ids[] = {
    { .compatible = "spsp_oops", },
    { /* sentinel */}
};

// LDDD: pg 152-154, inform the kernel, register driver IDs with platform core
MODULE_DEVICE_TABLE(of, spsp_oops_gpiod_dt_ids);

static struct platform_driver spsp_oops_drv = {
    .driver = {
             .name = "spsp_oops",
             .of_match_table = of_match_ptr(spsp_oops_gpiod_dt_ids),
             .owner = THIS_MODULE, },
};

/* module_platform_driver() - Helper macro for drivers that don't do
 * anything special in module init/exit.  This eliminates a lot of
 * boilerplate.  Each module may only use this macro once, and
 * calling it replaces module_init() and module_exit()
 */
// additionally it will register driver with kernel
module_platform_driver(spsp_oops_drv);

MODULE_AUTHOR("ilukic");
MODULE_DESCRIPTION("spsp oops logger support");
MODULE_LICENSE("GPL");      // allowed usage of GPL internal kernel exports
MODULE_VERSION("0.1");
