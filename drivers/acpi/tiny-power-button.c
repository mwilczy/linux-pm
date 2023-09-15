// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/module.h>
#include <linux/sched/signal.h>
#include <linux/acpi.h>
#include <linux/platform_device.h>
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

static void acpi_tiny_power_button_notify(acpi_handle handle, u32 event, void *data)
{
	kill_cad_pid(power_signal, 1);
}

static void acpi_tiny_power_button_notify_run(void *not_used)
{
	acpi_tiny_power_button_notify(NULL, ACPI_FIXED_HARDWARE_EVENT, NULL);
}

static u32 acpi_tiny_power_button_event(void *not_used)
{
	acpi_os_execute(OSL_NOTIFY_HANDLER, acpi_tiny_power_button_notify_run, NULL);
	return ACPI_INTERRUPT_HANDLED;
}

static int acpi_tiny_power_button_probe(struct platform_device *pdev)
{
	struct acpi_device *adev = ACPI_COMPANION(&pdev->dev);
	acpi_status status;

	if (adev->device_type == ACPI_BUS_TYPE_POWER_BUTTON) {
		status = acpi_install_fixed_event_handler(ACPI_EVENT_POWER_BUTTON,
							  acpi_tiny_power_button_event,
							  NULL);
	} else {
		status = acpi_install_notify_handler(adev->handle,
						     ACPI_DEVICE_NOTIFY,
						     acpi_tiny_power_button_notify,
						     NULL);
	}
	if (ACPI_FAILURE(status))
		return -ENODEV;

	return 0;
}

static void acpi_tiny_power_button_remove(struct platform_device *pdev)
{
	struct acpi_device *adev = ACPI_COMPANION(&pdev->dev);

	if (adev->device_type == ACPI_BUS_TYPE_POWER_BUTTON) {
		acpi_remove_fixed_event_handler(ACPI_EVENT_POWER_BUTTON,
						acpi_tiny_power_button_event);
	} else {
		acpi_remove_notify_handler(adev->handle, ACPI_DEVICE_NOTIFY,
					   acpi_tiny_power_button_notify);
	}
	acpi_os_wait_events_complete();
}

static struct platform_driver acpi_tiny_power_button_driver = {
	.probe = acpi_tiny_power_button_probe,
	.remove_new = acpi_tiny_power_button_remove,
	.driver = {
		.name = "tiny-power-button",
		.acpi_match_table = tiny_power_button_device_ids,
	},
};

module_platform_driver(acpi_tiny_power_button_driver);
