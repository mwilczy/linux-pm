// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/module.h>
#include <linux/sched/signal.h>
#include <linux/acpi.h>
#include <acpi/button.h>

MODULE_AUTHOR("Josh Triplett");
MODULE_DESCRIPTION("ACPI Tiny Power Button Driver");
MODULE_LICENSE("GPL");

static int power_signal __read_mostly = CONFIG_ACPI_TINY_POWER_BUTTON_SIGNAL;
module_param(power_signal, int, 0644);
MODULE_PARM_DESC(power_signal, "Power button sends this signal to init");

static const struct acpi_device_id tiny_power_button_device_ids[] = {
	{ ACPI_BUTTON_HID_POWER, 0 },
	{ ACPI_BUTTON_HID_POWERF, 0 },
	{ "", 0 },
};
MODULE_DEVICE_TABLE(acpi, tiny_power_button_device_ids);

static int acpi_tiny_power_button_add(struct acpi_device *device)
{
	return acpi_device_install_notify_handler(device, ACPI_DEVICE_NOTIFY,
						  acpi_tiny_power_button_notify);
}

static void acpi_tiny_power_button_remove(struct acpi_device *device)
{
	acpi_remove_notify_handler(device->handle, ACPI_DEVICE_NOTIFY,
				   acpi_tiny_power_button_notify);
}

static void acpi_tiny_power_button_notify(acpi_handle handle, u32 event, void *data)
{
	kill_cad_pid(power_signal, 1);
}

static struct acpi_driver acpi_tiny_power_button_driver = {
	.name = "tiny-power-button",
	.class = "tiny-power-button",
	.ids = tiny_power_button_device_ids,
	.ops = {
		.add = acpi_tiny_power_button_add,
		.remove = acpi_tiny_power_button_remove,
	},
};

module_acpi_driver(acpi_tiny_power_button_driver);
