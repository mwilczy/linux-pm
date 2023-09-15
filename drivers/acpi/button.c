// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  button.c - ACPI Button Driver
 *
 *  Copyright (C) 2001, 2002 Andy Grover <andrew.grover@intel.com>
 *  Copyright (C) 2001, 2002 Paul Diefenbaugh <paul.s.diefenbaugh@intel.com>
 */

#define pr_fmt(fmt) "ACPI: button: " fmt

#include <linux/compiler.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/input.h>
#include <linux/slab.h>
#include <linux/acpi.h>
#include <linux/dmi.h>
#include <acpi/button.h>

#define ACPI_BUTTON_CLASS		"button"
#define ACPI_BUTTON_FILE_STATE		"state"
#define ACPI_BUTTON_TYPE_UNKNOWN	0x00
#define ACPI_BUTTON_NOTIFY_STATUS	0x80

#define ACPI_BUTTON_SUBCLASS_POWER	"power"
#define ACPI_BUTTON_DEVICE_NAME_POWER	"Power Button"
#define ACPI_BUTTON_TYPE_POWER		0x01

#define ACPI_BUTTON_SUBCLASS_SLEEP	"sleep"
#define ACPI_BUTTON_DEVICE_NAME_SLEEP	"Sleep Button"
#define ACPI_BUTTON_TYPE_SLEEP		0x03

#define ACPI_BUTTON_SUBCLASS_LID	"lid"
#define ACPI_BUTTON_DEVICE_NAME_LID	"Lid Switch"
#define ACPI_BUTTON_TYPE_LID		0x05

enum {
	ACPI_BUTTON_LID_INIT_IGNORE,
	ACPI_BUTTON_LID_INIT_OPEN,
	ACPI_BUTTON_LID_INIT_METHOD,
	ACPI_BUTTON_LID_INIT_DISABLED,
};

static const char * const lid_init_state_str[] = {
	[ACPI_BUTTON_LID_INIT_IGNORE]		= "ignore",
	[ACPI_BUTTON_LID_INIT_OPEN]		= "open",
	[ACPI_BUTTON_LID_INIT_METHOD]		= "method",
	[ACPI_BUTTON_LID_INIT_DISABLED]		= "disabled",
};

MODULE_AUTHOR("Paul Diefenbaugh");
MODULE_DESCRIPTION("ACPI Button Driver");
MODULE_LICENSE("GPL");

static const struct acpi_device_id button_device_ids[] = {
	{ACPI_BUTTON_HID_LID,    0},
	{ACPI_BUTTON_HID_SLEEP,  0},
	{ACPI_BUTTON_HID_SLEEPF, 0},
	{ACPI_BUTTON_HID_POWER,  0},
	{ACPI_BUTTON_HID_POWERF, 0},
	{"", 0},
};
MODULE_DEVICE_TABLE(acpi, button_device_ids);

/* Please keep this list sorted alphabetically by vendor and model */
static const struct dmi_system_id dmi_lid_quirks[] = {
	{
		/* GP-electronic T701, _LID method points to a floating GPIO */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Insyde"),
			DMI_MATCH(DMI_PRODUCT_NAME, "T701"),
			DMI_MATCH(DMI_BIOS_VERSION, "BYT70A.YNCHENG.WIN.007"),
		},
		.driver_data = (void *)(long)ACPI_BUTTON_LID_INIT_DISABLED,
	},
	{
		/* Nextbook Ares 8A tablet, _LID device always reports lid closed */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Insyde"),
			DMI_MATCH(DMI_PRODUCT_NAME, "CherryTrail"),
			DMI_MATCH(DMI_BIOS_VERSION, "M882"),
		},
		.driver_data = (void *)(long)ACPI_BUTTON_LID_INIT_DISABLED,
	},
	{
		/*
		 * Lenovo Yoga 9 14ITL5, initial notification of the LID device
		 * never happens.
		 */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_NAME, "82BG"),
		},
		.driver_data = (void *)(long)ACPI_BUTTON_LID_INIT_OPEN,
	},
	{
		/*
		 * Medion Akoya E2215T, notification of the LID device only
		 * happens on close, not on open and _LID always returns closed.
		 */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "MEDION"),
			DMI_MATCH(DMI_PRODUCT_NAME, "E2215T"),
		},
		.driver_data = (void *)(long)ACPI_BUTTON_LID_INIT_OPEN,
	},
	{
		/*
		 * Medion Akoya E2228T, notification of the LID device only
		 * happens on close, not on open and _LID always returns closed.
		 */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "MEDION"),
			DMI_MATCH(DMI_PRODUCT_NAME, "E2228T"),
		},
		.driver_data = (void *)(long)ACPI_BUTTON_LID_INIT_OPEN,
	},
	{
		/*
		 * Razer Blade Stealth 13 late 2019, notification of the LID device
		 * only happens on close, not on open and _LID always returns closed.
		 */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Razer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Razer Blade Stealth 13 Late 2019"),
		},
		.driver_data = (void *)(long)ACPI_BUTTON_LID_INIT_OPEN,
	},
	{}
};

static int acpi_button_probe(struct platform_device *pdev);
static void acpi_button_remove(struct platform_device *pdev);

#ifdef CONFIG_PM_SLEEP
static int acpi_button_suspend(struct device *dev);
static int acpi_button_resume(struct device *dev);
#else
#define acpi_button_suspend NULL
#define acpi_button_resume NULL
#endif
static SIMPLE_DEV_PM_OPS(acpi_button_pm, acpi_button_suspend, acpi_button_resume);

static struct platform_driver acpi_button_driver = {
	.probe = acpi_button_probe,
	.remove_new = acpi_button_remove,
	.driver = {
		.name = "button",
		.acpi_match_table = button_device_ids,
		.pm = &acpi_button_pm,
	},
};

struct acpi_button {
	struct device *dev;
	unsigned int type;
	struct input_dev *input;
	char phys[32];			/* for input device */
	unsigned long pushed;
	int last_state;
	ktime_t last_time;
	bool suspended;
	bool lid_state_initialized;
};

static struct acpi_device *lid_device;
static long lid_init_state = -1;

static unsigned long lid_report_interval __read_mostly = 500;
module_param(lid_report_interval, ulong, 0644);
MODULE_PARM_DESC(lid_report_interval, "Interval (ms) between lid key events");

/* FS Interface (/proc) */
static struct proc_dir_entry *acpi_button_dir;
static struct proc_dir_entry *acpi_lid_dir;

static int acpi_lid_evaluate_state(struct acpi_device *adev)
{
	unsigned long long lid_state;
	acpi_status status;

	status = acpi_evaluate_integer(adev->handle, "_LID", NULL, &lid_state);
	if (ACPI_FAILURE(status))
		return -ENODEV;

	return lid_state ? 1 : 0;
}

static int acpi_lid_notify_state(struct acpi_button *button, int state)
{
	ktime_t next_report;
	bool do_update;

	/*
	 * In lid_init_state=ignore mode, if user opens/closes lid
	 * frequently with "open" missing, and "last_time" is also updated
	 * frequently, "close" cannot be delivered to the userspace.
	 * So "last_time" is only updated after a timeout or an actual
	 * switch.
	 */
	if (lid_init_state != ACPI_BUTTON_LID_INIT_IGNORE ||
	    button->last_state != !!state)
		do_update = true;
	else
		do_update = false;

	next_report = ktime_add(button->last_time,
				ms_to_ktime(lid_report_interval));
	if (button->last_state == !!state &&
	    ktime_after(ktime_get(), next_report)) {
		/* Complain the buggy firmware */
		pr_warn_once("The lid device is not compliant to SW_LID.\n");

		/*
		 * Send the unreliable complement switch event:
		 *
		 * On most platforms, the lid device is reliable. However
		 * there are exceptions:
		 * 1. Platforms returning initial lid state as "close" by
		 *    default after booting/resuming:
		 *     https://bugzilla.kernel.org/show_bug.cgi?id=89211
		 *     https://bugzilla.kernel.org/show_bug.cgi?id=106151
		 * 2. Platforms never reporting "open" events:
		 *     https://bugzilla.kernel.org/show_bug.cgi?id=106941
		 * On these buggy platforms, the usage model of the ACPI
		 * lid device actually is:
		 * 1. The initial returning value of _LID may not be
		 *    reliable.
		 * 2. The open event may not be reliable.
		 * 3. The close event is reliable.
		 *
		 * But SW_LID is typed as input switch event, the input
		 * layer checks if the event is redundant. Hence if the
		 * state is not switched, the userspace cannot see this
		 * platform triggered reliable event. By inserting a
		 * complement switch event, it then is guaranteed that the
		 * platform triggered reliable one can always be seen by
		 * the userspace.
		 */
		if (lid_init_state == ACPI_BUTTON_LID_INIT_IGNORE) {
			do_update = true;
			/*
			 * Do generate complement switch event for "close"
			 * as "close" is reliable and wrong "open" won't
			 * trigger unexpected behaviors.
			 * Do not generate complement switch event for
			 * "open" as "open" is not reliable and wrong
			 * "close" will trigger unexpected behaviors.
			 */
			if (!state) {
				input_report_switch(button->input,
						    SW_LID, state);
				input_sync(button->input);
			}
		}
	}
	/* Send the platform triggered reliable event */
	if (do_update) {
		acpi_handle_debug(ACPI_HANDLE(button->dev), "ACPI LID %s\n",
				  state ? "open" : "closed");
		input_report_switch(button->input, SW_LID, !state);
		input_sync(button->input);
		button->last_state = !!state;
		button->last_time = ktime_get();
	}

	return 0;
}

static int __maybe_unused acpi_button_state_seq_show(struct seq_file *seq,
						     void *offset)
{
	struct acpi_device *adev = seq->private;
	int state;

	state = acpi_lid_evaluate_state(adev);
	seq_printf(seq, "state:      %s\n",
		   state < 0 ? "unsupported" : (state ? "open" : "closed"));
	return 0;
}

static int acpi_button_add_fs(struct acpi_button *button)
{
	struct acpi_device *adev = ACPI_COMPANION(button->dev);
	struct proc_dir_entry *entry;
	int ret = 0;

	/* procfs I/F for ACPI lid device only */
	if (button->type != ACPI_BUTTON_TYPE_LID)
		return 0;

	if (acpi_button_dir || acpi_lid_dir) {
		pr_info("More than one Lid device found!\n");
		return -EEXIST;
	}

	/* create /proc/acpi/button */
	acpi_button_dir = proc_mkdir(ACPI_BUTTON_CLASS, acpi_root_dir);
	if (!acpi_button_dir)
		return -ENODEV;

	/* create /proc/acpi/button/lid */
	acpi_lid_dir = proc_mkdir(ACPI_BUTTON_SUBCLASS_LID, acpi_button_dir);
	if (!acpi_lid_dir) {
		ret = -ENODEV;
		goto remove_button_dir;
	}

	/* create /proc/acpi/button/lid/LID/ */
	acpi_device_dir(adev) = proc_mkdir(acpi_device_bid(adev), acpi_lid_dir);
	if (!acpi_device_dir(adev)) {
		ret = -ENODEV;
		goto remove_lid_dir;
	}

	/* create /proc/acpi/button/lid/LID/state */
	entry = proc_create_single_data(ACPI_BUTTON_FILE_STATE, S_IRUGO,
			acpi_device_dir(adev), acpi_button_state_seq_show,
			adev);
	if (!entry) {
		ret = -ENODEV;
		goto remove_dev_dir;
	}

done:
	return ret;

remove_dev_dir:
	remove_proc_entry(acpi_device_bid(adev),
			  acpi_lid_dir);
	acpi_device_dir(adev) = NULL;
remove_lid_dir:
	remove_proc_entry(ACPI_BUTTON_SUBCLASS_LID, acpi_button_dir);
	acpi_lid_dir = NULL;
remove_button_dir:
	remove_proc_entry(ACPI_BUTTON_CLASS, acpi_root_dir);
	acpi_button_dir = NULL;
	goto done;
}

static int acpi_button_remove_fs(struct acpi_button *button)
{
	struct acpi_device *adev = ACPI_COMPANION(button->dev);
	if (button->type != ACPI_BUTTON_TYPE_LID)
		return 0;

	remove_proc_entry(ACPI_BUTTON_FILE_STATE,
			  acpi_device_dir(adev));
	remove_proc_entry(acpi_device_bid(adev),
			  acpi_lid_dir);
	acpi_device_dir(adev) = NULL;
	remove_proc_entry(ACPI_BUTTON_SUBCLASS_LID, acpi_button_dir);
	acpi_lid_dir = NULL;
	remove_proc_entry(ACPI_BUTTON_CLASS, acpi_root_dir);
	acpi_button_dir = NULL;

	return 0;
}

/* Driver Interface */
int acpi_lid_open(void)
{
	if (!lid_device)
		return -ENODEV;

	return acpi_lid_evaluate_state(lid_device);
}
EXPORT_SYMBOL(acpi_lid_open);

static int acpi_lid_update_state(struct acpi_button *button,
				 bool signal_wakeup)
{
	struct acpi_device *adev = ACPI_COMPANION(button->dev);
	int state;

	state = acpi_lid_evaluate_state(adev);
	if (state < 0)
		return state;

	if (state && signal_wakeup)
		acpi_pm_wakeup_event(button->dev);

	return acpi_lid_notify_state(button, state);
}

static void acpi_lid_initialize_state(struct acpi_button *button)
{
	switch (lid_init_state) {
	case ACPI_BUTTON_LID_INIT_OPEN:
		(void)acpi_lid_notify_state(button, 1);
		break;
	case ACPI_BUTTON_LID_INIT_METHOD:
		(void)acpi_lid_update_state(button, false);
		break;
	case ACPI_BUTTON_LID_INIT_IGNORE:
	default:
		break;
	}

	button->lid_state_initialized = true;
}

static void acpi_lid_notify(acpi_handle handle, u32 event, void *data)
{
	struct acpi_button *button = data;

	if (event != ACPI_BUTTON_NOTIFY_STATUS) {
		acpi_handle_debug(handle, "Unsupported event [0x%x]\n",
				  event);
		return;
	}

	if (!button->lid_state_initialized)
		return;

	acpi_lid_update_state(button, true);
}

static void acpi_button_notify(acpi_handle handle, u32 event, void *data)
{
	struct acpi_button *button = data;
	struct acpi_device *adev = ACPI_COMPANION(button->dev);
	struct input_dev *input;
	int keycode;

	if (event != ACPI_BUTTON_NOTIFY_STATUS) {
		acpi_handle_debug(adev->handle,
				  "Unsupported event [0x%x]\n",
				  event);
		return;
	}

	acpi_pm_wakeup_event(button->dev);

	if (button->suspended)
		return;

	input = button->input;
	keycode = test_bit(KEY_SLEEP, input->keybit) ? KEY_SLEEP : KEY_POWER;

	input_report_key(input, keycode, 1);
	input_sync(input);
	input_report_key(input, keycode, 0);
	input_sync(input);

	acpi_bus_generate_netlink_event(adev->pnp.device_class,
					dev_name(button->dev),
					event, ++button->pushed);
}

static void acpi_button_notify_run(void *data)
{
	acpi_button_notify(NULL, ACPI_BUTTON_NOTIFY_STATUS, data);
}

static u32 acpi_button_event(void *data)
{
	acpi_os_execute(OSL_NOTIFY_HANDLER, acpi_button_notify_run, data);
	return ACPI_INTERRUPT_HANDLED;
}

#ifdef CONFIG_PM_SLEEP
static int acpi_button_suspend(struct device *dev)
{
	struct acpi_button *button = dev_get_drvdata(dev);

	button->suspended = true;
	return 0;
}

static int acpi_button_resume(struct device *dev)
{
	struct acpi_button *button = dev_get_drvdata(dev);
	struct acpi_device *adev = ACPI_COMPANION(dev);

	button->suspended = false;
	if (button->type == ACPI_BUTTON_TYPE_LID) {
		button->last_state = !!acpi_lid_evaluate_state(adev);
		button->last_time = ktime_get();
		acpi_lid_initialize_state(button);
	}
	return 0;
}
#endif

static int acpi_lid_input_open(struct input_dev *input)
{
	struct acpi_button *button = input_get_drvdata(input);
	struct acpi_device *adev = ACPI_COMPANION(button->dev);

	button->last_state = !!acpi_lid_evaluate_state(adev);
	button->last_time = ktime_get();
	acpi_lid_initialize_state(button);

	return 0;
}

static int acpi_button_probe(struct platform_device *pdev)
{
	struct acpi_device *adev = ACPI_COMPANION(&pdev->dev);
	acpi_notify_handler handler;
	struct acpi_button *button;
	struct input_dev *input;
	const char *hid = acpi_device_hid(adev);
	acpi_status status;
	char *name, *class;
	int error = 0;

	if (!strcmp(hid, ACPI_BUTTON_HID_LID) &&
	     lid_init_state == ACPI_BUTTON_LID_INIT_DISABLED)
		return -ENODEV;

	button = kzalloc(sizeof(struct acpi_button), GFP_KERNEL);
	if (!button)
		return -ENOMEM;

	button->dev = &pdev->dev;

	platform_set_drvdata(pdev, button);

	button->input = input = input_allocate_device();
	if (!input) {
		error = -ENOMEM;
		goto err_free_button;
	}

	name = acpi_device_name(adev);
	class = acpi_device_class(adev);

	if (!strcmp(hid, ACPI_BUTTON_HID_POWER) ||
	    !strcmp(hid, ACPI_BUTTON_HID_POWERF)) {
		button->type = ACPI_BUTTON_TYPE_POWER;
		handler = acpi_button_notify;
		strcpy(name, ACPI_BUTTON_DEVICE_NAME_POWER);
		sprintf(class, "%s/%s",
			ACPI_BUTTON_CLASS, ACPI_BUTTON_SUBCLASS_POWER);
	} else if (!strcmp(hid, ACPI_BUTTON_HID_SLEEP) ||
		   !strcmp(hid, ACPI_BUTTON_HID_SLEEPF)) {
		button->type = ACPI_BUTTON_TYPE_SLEEP;
		handler = acpi_button_notify;
		strcpy(name, ACPI_BUTTON_DEVICE_NAME_SLEEP);
		sprintf(class, "%s/%s",
			ACPI_BUTTON_CLASS, ACPI_BUTTON_SUBCLASS_SLEEP);
	} else if (!strcmp(hid, ACPI_BUTTON_HID_LID)) {
		button->type = ACPI_BUTTON_TYPE_LID;
		handler = acpi_lid_notify;
		strcpy(name, ACPI_BUTTON_DEVICE_NAME_LID);
		sprintf(class, "%s/%s",
			ACPI_BUTTON_CLASS, ACPI_BUTTON_SUBCLASS_LID);
		input->open = acpi_lid_input_open;
	} else {
		pr_info("Unsupported hid [%s]\n", hid);
		error = -ENODEV;
	}

	if (!error)
		error = acpi_button_add_fs(button);

	if (error) {
		input_free_device(input);
		goto err_free_button;
	}

	snprintf(button->phys, sizeof(button->phys), "%s/button/input0", hid);

	input->name = name;
	input->phys = button->phys;
	input->id.bustype = BUS_HOST;
	input->id.product = button->type;
	input->dev.parent = &pdev->dev;

	switch (button->type) {
	case ACPI_BUTTON_TYPE_POWER:
		input_set_capability(input, EV_KEY, KEY_POWER);
		break;

	case ACPI_BUTTON_TYPE_SLEEP:
		input_set_capability(input, EV_KEY, KEY_SLEEP);
		break;

	case ACPI_BUTTON_TYPE_LID:
		input_set_capability(input, EV_SW, SW_LID);
		break;
	}

	input_set_drvdata(input, button);
	error = input_register_device(input);
	if (error)
		goto err_remove_fs;

	switch (adev->device_type) {
	case ACPI_BUS_TYPE_POWER_BUTTON:
		status = acpi_install_fixed_event_handler(ACPI_EVENT_POWER_BUTTON,
							  acpi_button_event,
							  button);
		break;
	case ACPI_BUS_TYPE_SLEEP_BUTTON:
		status = acpi_install_fixed_event_handler(ACPI_EVENT_SLEEP_BUTTON,
							  acpi_button_event,
							  button);
		break;
	default:
		status = acpi_install_notify_handler(adev->handle,
						     ACPI_DEVICE_NOTIFY, handler,
						     button);
		break;
	}
	if (ACPI_FAILURE(status)) {
		error = -ENODEV;
		goto err_input_unregister;
	}

	if (button->type == ACPI_BUTTON_TYPE_LID) {
		/*
		 * This assumes there's only one lid device, or if there are
		 * more we only care about the last one...
		 */
		lid_device = adev;
	}

	device_init_wakeup(&pdev->dev, true);
	pr_info("%s [%s]\n", name, acpi_device_bid(adev));
	return 0;

err_input_unregister:
	input_unregister_device(input);
err_remove_fs:
	acpi_button_remove_fs(button);
err_free_button:
	kfree(button);
	return error;
}

static void acpi_button_remove(struct platform_device *pdev)
{
	struct acpi_button *button = platform_get_drvdata(pdev);
	struct acpi_device *adev = ACPI_COMPANION(button->dev);

	switch (adev->device_type) {
	case ACPI_BUS_TYPE_POWER_BUTTON:
		acpi_remove_fixed_event_handler(ACPI_EVENT_POWER_BUTTON,
						acpi_button_event);
		break;
	case ACPI_BUS_TYPE_SLEEP_BUTTON:
		acpi_remove_fixed_event_handler(ACPI_EVENT_SLEEP_BUTTON,
						acpi_button_event);
		break;
	default:
		acpi_remove_notify_handler(adev->handle, ACPI_DEVICE_NOTIFY,
					   button->type == ACPI_BUTTON_TYPE_LID ?
						acpi_lid_notify :
						acpi_button_notify);
		break;
	}
	acpi_os_wait_events_complete();

	acpi_button_remove_fs(button);
	input_unregister_device(button->input);
	kfree(button);
}

static int param_set_lid_init_state(const char *val,
				    const struct kernel_param *kp)
{
	int i;

	i = sysfs_match_string(lid_init_state_str, val);
	if (i < 0)
		return i;

	lid_init_state = i;
	pr_info("Initial lid state set to '%s'\n", lid_init_state_str[i]);
	return 0;
}

static int param_get_lid_init_state(char *buf, const struct kernel_param *kp)
{
	int i, c = 0;

	for (i = 0; i < ARRAY_SIZE(lid_init_state_str); i++)
		if (i == lid_init_state)
			c += sprintf(buf + c, "[%s] ", lid_init_state_str[i]);
		else
			c += sprintf(buf + c, "%s ", lid_init_state_str[i]);

	buf[c - 1] = '\n'; /* Replace the final space with a newline */

	return c;
}

module_param_call(lid_init_state,
		  param_set_lid_init_state, param_get_lid_init_state,
		  NULL, 0644);
MODULE_PARM_DESC(lid_init_state, "Behavior for reporting LID initial state");

static int acpi_button_register_driver(struct platform_driver *driver)
{
	const struct dmi_system_id *dmi_id;

	if (lid_init_state == -1) {
		dmi_id = dmi_first_match(dmi_lid_quirks);
		if (dmi_id)
			lid_init_state = (long)dmi_id->driver_data;
		else
			lid_init_state = ACPI_BUTTON_LID_INIT_METHOD;
	}

	/*
	 * Modules such as nouveau.ko and i915.ko have a link time dependency
	 * on acpi_lid_open(), and would therefore not be loadable on ACPI
	 * capable kernels booted in non-ACPI mode if the return value of
	 * acpi_bus_register_driver() is returned from here with ACPI disabled
	 * when this driver is built as a module.
	 */
	if (acpi_disabled)
		return 0;

	return platform_driver_register(driver);
}

static void acpi_button_unregister_driver(struct platform_driver *driver)
{
	if (!acpi_disabled)
		platform_driver_unregister(driver);
}

module_driver(acpi_button_driver, acpi_button_register_driver,
	       acpi_button_unregister_driver);
