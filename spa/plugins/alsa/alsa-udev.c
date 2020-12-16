/* Spa ALSA udev
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <stddef.h>
#include <stdio.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <fcntl.h>
#include <poll.h>

#include <libudev.h>
#include <alsa/asoundlib.h>

#include <spa/support/log.h>
#include <spa/utils/type.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/utils/result.h>
#include <spa/support/loop.h>
#include <spa/support/plugin.h>
#include <spa/monitor/device.h>
#include <spa/monitor/utils.h>

#define NAME  "alsa-udev"

#define MAX_CARDS	64

#define ACTION_ADD	0
#define ACTION_CHANGE	1
#define ACTION_REMOVE	2

struct card {
	uint32_t id;
	struct udev_device *dev;
	unsigned int accessible:1;
	unsigned int ignored:1;
	unsigned int emited:1;
};

struct impl {
	struct spa_handle handle;
	struct spa_device device;

	struct spa_log *log;
	struct spa_loop *main_loop;

	struct spa_hook_list hooks;

	uint64_t info_all;
	struct spa_device_info info;

	struct udev *udev;
	struct udev_monitor *umonitor;

	struct card cards[MAX_CARDS];
	uint32_t n_cards;

	struct spa_source source;
	struct spa_source notify;
	unsigned int use_acp:1;
};

static int impl_udev_open(struct impl *this)
{
	if (this->udev == NULL) {
		this->udev = udev_new();
		if (this->udev == NULL)
			return -ENOMEM;
	}
	return 0;
}

static int impl_udev_close(struct impl *this)
{
	if (this->udev != NULL)
		udev_unref(this->udev);
	this->udev = NULL;
	return 0;
}

static struct card *find_card(struct impl *this, uint32_t id)
{
	uint32_t i;
	for (i = 0; i < this->n_cards; i++) {
		if (this->cards[i].id == id)
			return &this->cards[i];
	}
	return NULL;
}

static const char *path_get_card_id(const char *path)
{
	const char *e;

	if (!path)
		return NULL;

	if (!(e = strrchr(path, '/')))
		return NULL;

	if (strlen(e) <= 5 || strncmp(e, "/card", 5) != 0)
		return NULL;

	return e + 5;
}

static int dehex(char x)
{
	if (x >= '0' && x <= '9')
		return x - '0';
	if (x >= 'A' && x <= 'F')
		return x - 'A' + 10;
	if (x >= 'a' && x <= 'f')
		return x - 'a' + 10;
	return -1;
}

static void unescape(const char *src, char *dst)
{
	const char *s;
	char *d;
	int h1, h2;
	enum { TEXT, BACKSLASH, EX, FIRST } state = TEXT;

	for (s = src, d = dst; *s; s++) {
		switch (state) {
		case TEXT:
			if (*s == '\\')
				state = BACKSLASH;
			else
				*(d++) = *s;
			break;

		case BACKSLASH:
			if (*s == 'x')
				state = EX;
			else {
				*(d++) = '\\';
				*(d++) = *s;
				state = TEXT;
			}
			break;

		case EX:
			h1 = dehex(*s);
			if (h1 < 0) {
				*(d++) = '\\';
				*(d++) = 'x';
				*(d++) = *s;
				state = TEXT;
			} else
				state = FIRST;
			break;

		case FIRST:
			h2 = dehex(*s);
			if (h2 < 0) {
				*(d++) = '\\';
				*(d++) = 'x';
				*(d++) = *(s-1);
				*(d++) = *s;
			} else
				*(d++) = (char) (h1 << 4) | h2;
			state = TEXT;
			break;
		}
	}
	switch (state) {
	case TEXT:
		break;
	case BACKSLASH:
		*(d++) = '\\';
		break;
	case EX:
		*(d++) = '\\';
		*(d++) = 'x';
		break;
	case FIRST:
		*(d++) = '\\';
		*(d++) = 'x';
		*(d++) = *(s-1);
		break;
	}
	*d = 0;
}

static int emit_object_info(struct impl *this, uint32_t id, struct udev_device *dev)
{
	struct spa_device_object_info info;
	snd_ctl_t *ctl_hndl;
	const char *str;
	char path[32];
	struct spa_dict_item items[23];
	uint32_t n_items = 0;
	int res, pcm;
	struct card *card;

	if ((card = find_card(this, id)) == NULL)
		return -EINVAL;

	snprintf(path, sizeof(path), "hw:%u", id);
	spa_log_debug(this->log, "open card %s", path);

	if ((res = snd_ctl_open(&ctl_hndl, path, 0)) < 0) {
		spa_log_error(this->log, "can't open control for card %s: %s",
				path, snd_strerror(res));
		return res;
	}

	pcm = -1;
	res = snd_ctl_pcm_next_device(ctl_hndl, &pcm);

	spa_log_debug(this->log, "close card %s", path);
	snd_ctl_close(ctl_hndl);

	if (res < 0) {
		spa_log_error(this->log, "error iterating devices: %s", snd_strerror(res));
		card->ignored = true;
		return res;
	}
	if (pcm < 0) {
		spa_log_debug(this->log, "no pcm devices for %s", path);
		card->ignored = true;
		return 0;
	}

	info = SPA_DEVICE_OBJECT_INFO_INIT();

	info.type = SPA_TYPE_INTERFACE_Device;
	info.factory_name = this->use_acp ?
		SPA_NAME_API_ALSA_ACP_DEVICE :
		SPA_NAME_API_ALSA_PCM_DEVICE;
	info.change_mask = SPA_DEVICE_OBJECT_CHANGE_MASK_FLAGS |
		SPA_DEVICE_OBJECT_CHANGE_MASK_PROPS;
	info.flags = 0;

	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_ENUM_API, "udev");
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_API,  "alsa");
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_MEDIA_CLASS, "Audio/Device");
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_ALSA_PATH, path);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_ALSA_CARD, path+3);

	if ((str = udev_device_get_property_value(dev, "ACP_NAME")) && *str)
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_NAME, str);

	if ((str = udev_device_get_property_value(dev, "ACP_PROFILE_SET")) && *str)
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_PROFILE_SET, str);

	if ((str = udev_device_get_property_value(dev, "SOUND_CLASS")) && *str)
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_CLASS, str);

	if ((str = udev_device_get_property_value(dev, "USEC_INITIALIZED")) && *str)
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_PLUGGED_USEC, str);

	str = udev_device_get_property_value(dev, "ID_PATH");
	if (!(str && *str))
		str = udev_device_get_syspath(dev);
	if (str && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_BUS_PATH, str);
	}
	if ((str = udev_device_get_syspath(dev)) && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_SYSFS_PATH, str);
	}
	if ((str = udev_device_get_property_value(dev, "ID_ID")) && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_BUS_ID, str);
	}
	if ((str = udev_device_get_property_value(dev, "ID_BUS")) && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_BUS, str);
	}
	if ((str = udev_device_get_property_value(dev, "SUBSYSTEM")) && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_SUBSYSTEM, str);
	}
	if ((str = udev_device_get_property_value(dev, "ID_VENDOR_ID")) && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_VENDOR_ID, str);
	}
	str = udev_device_get_property_value(dev, "ID_VENDOR_FROM_DATABASE");
	if (!(str && *str)) {
		str = udev_device_get_property_value(dev, "ID_VENDOR_ENC");
		if (!(str && *str)) {
			str = udev_device_get_property_value(dev, "ID_VENDOR");
		} else {
			char *t = alloca(strlen(str) + 1);
			unescape(str, t);
			str = t;
		}
	}
	if (str && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_VENDOR_NAME, str);
	}
	if ((str = udev_device_get_property_value(dev, "ID_MODEL_ID")) && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_PRODUCT_ID, str);
	}
	str = udev_device_get_property_value(dev, "ID_MODEL_FROM_DATABASE");
	if (!(str && *str)) {
		str = udev_device_get_property_value(dev, "ID_MODEL_ENC");
		if (!(str && *str)) {
			str = udev_device_get_property_value(dev, "ID_MODEL");
		} else {
			char *t = alloca(strlen(str) + 1);
			unescape(str, t);
			str = t;
		}
	}
	if (str && *str)
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_PRODUCT_NAME, str);

	if ((str = udev_device_get_property_value(dev, "ID_SERIAL")) && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_SERIAL, str);
	}
	if ((str = udev_device_get_property_value(dev, "SOUND_FORM_FACTOR")) && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_FORM_FACTOR, str);
	}
	info.props = &SPA_DICT_INIT(items, n_items);

	spa_device_emit_object_info(&this->hooks, id, &info);
	card->emited = true;

	return 1;
}

static bool check_access(struct impl *this, struct card *card)
{
	char path[128];

	snprintf(path, sizeof(path)-1, "/dev/snd/controlC%u", card->id);
	card->accessible = access(path, R_OK|W_OK) >= 0;
	spa_log_debug(this->log, "%s accessible:%u", path, card->accessible);

	return card->accessible;
}

static int need_notify(struct impl *this, struct udev_device *dev, uint32_t action, bool enumerated,
		uint32_t *obj_id)
{
	const char *str;
	uint32_t id;
	struct card *card;
	bool emited;

	if (udev_device_get_property_value(dev, "ACP_IGNORE"))
		return 0;

	if ((str = udev_device_get_property_value(dev, "SOUND_CLASS")) && strcmp(str, "modem") == 0)
		return 0;

	if ((str = path_get_card_id(udev_device_get_property_value(dev, "DEVPATH"))) == NULL)
		return 0;

	id = atoi(str);
	card = find_card(this, id);
	if (card && card->ignored)
		return 0;

	switch (action) {
	case ACTION_ADD:
		if (card != NULL)
			return 0;
		if (this->n_cards >= MAX_CARDS)
			return 0;
		card = &this->cards[this->n_cards++];
		spa_zero(*card);
		card->id = id;
		udev_device_ref(dev);
		card->dev = dev;
		/** don't notify on add, wait for the next change event */
		if (!enumerated)
			return 0;
		if (!check_access(this, card))
			return 0;
		break;

	case ACTION_CHANGE:
		if (card == NULL)
			return 0;
		if ((str = udev_device_get_property_value(dev, "SOUND_INITIALIZED")) == NULL)
			return 0;
		if (!check_access(this, card))
			return 0;
		break;

	case ACTION_REMOVE:
		if (card == NULL)
			return 0;
		emited =  card->emited;
		udev_device_unref(card->dev);
		*card = this->cards[--this->n_cards];
		if (!emited)
			return 0;
		break;
	default:
		return 0;
	}
	*obj_id = id;
	return 1;
}

static int emit_device(struct impl *this, uint32_t action, bool enumerated, struct udev_device *dev)
{
	uint32_t id;

	if (!need_notify(this, dev, action, enumerated, &id))
		return 0;

	switch (action) {
	case ACTION_ADD:
	case ACTION_CHANGE:
		emit_object_info(this, id, dev);
		break;
	default:
		spa_device_emit_object_info(&this->hooks, id, NULL);
		break;
	}
	return 0;
}

static int stop_inotify(struct impl *this)
{
	if (this->notify.fd == -1)
		return 0;
	spa_log_info(this->log, "stop inotify");
	spa_loop_remove_source(this->main_loop, &this->notify);
	close(this->notify.fd);
	this->notify.fd = -1;
	return 0;
}

static void impl_on_notify_events(struct spa_source *source)
{
	bool deleted = false;
	struct impl *this = source->data;
	struct {
		struct inotify_event e;
		char name[NAME_MAX+1];
	} buf;

	while (true) {
		ssize_t len;
		const struct inotify_event *event;
		void *p, *e;

		len = read(source->fd, &buf, sizeof(buf));
		if (len < 0 && errno != EAGAIN)
			break;
		if (len <= 0)
			break;

		e = SPA_MEMBER(&buf, len, void);

		for (p = &buf; p < e;
		    p = SPA_MEMBER(p, sizeof(struct inotify_event) + event->len, void)) {
			unsigned int id;
			struct card *card;

			event = (const struct inotify_event *) p;

			if ((event->mask & IN_ATTRIB)) {
				if (sscanf(event->name, "controlC%u", &id) != 1)
					continue;
				if ((card = find_card(this, id)) == NULL)
					continue;
				if (!card->emited)
					emit_device(this, ACTION_CHANGE, false, card->dev);
			}
			/* /dev/snd/ might have been removed */
			if ((event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)))
				deleted = true;
		}
	}
	if (deleted)
		stop_inotify(this);
}

static int start_inotify(struct impl *this)
{
	int res, notify_fd;

	if (this->notify.fd != -1)
		return 0;

	if ((notify_fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK)) < 0)
		return -errno;

	res = inotify_add_watch(notify_fd, "/dev/snd",
			IN_ATTRIB | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF);
	if (res < 0) {
		res = -errno;
		close(notify_fd);

		if (res == -ENOENT) {
			spa_log_debug(this->log, "/dev/snd/ does not exist yet");
			return 0;
		}
		spa_log_error(this->log, "inotify_add_watch() failed: %s", spa_strerror(res));
		return res;
	}
	spa_log_info(this->log, "start inotify");
	this->notify.func = impl_on_notify_events;
	this->notify.data = this;
	this->notify.fd = notify_fd;
	this->notify.mask = SPA_IO_IN | SPA_IO_ERR;

	spa_loop_add_source(this->main_loop, &this->notify);

	return 0;
}

static void impl_on_fd_events(struct spa_source *source)
{
	struct impl *this = source->data;
	struct udev_device *dev;
	const char *action;
	uint32_t a;

	dev = udev_monitor_receive_device(this->umonitor);
	if (dev == NULL)
                return;

	if ((action = udev_device_get_action(dev)) == NULL)
		action = "change";

	spa_log_debug(this->log, "action %s", action);

	start_inotify(this);

	if (strcmp(action, "add") == 0) {
		a = ACTION_ADD;
	} else if (strcmp(action, "change") == 0) {
		a = ACTION_CHANGE;
	} else if (strcmp(action, "remove") == 0) {
		a = ACTION_REMOVE;
	} else
		return;

	emit_device(this, a, false, dev);

	udev_device_unref(dev);
}

static int start_monitor(struct impl *this)
{
	int res;

	if (this->umonitor != NULL)
		return 0;

	this->umonitor = udev_monitor_new_from_netlink(this->udev, "udev");
	if (this->umonitor == NULL)
		return -ENOMEM;

	udev_monitor_filter_add_match_subsystem_devtype(this->umonitor,
							"sound", NULL);
	udev_monitor_enable_receiving(this->umonitor);

	this->source.func = impl_on_fd_events;
	this->source.data = this;
	this->source.fd = udev_monitor_get_fd(this->umonitor);;
	this->source.mask = SPA_IO_IN | SPA_IO_ERR;

	spa_loop_add_source(this->main_loop, &this->source);

	if ((res = start_inotify(this)) < 0)
		return res;

	return 0;
}

static int stop_monitor(struct impl *this)
{
	if (this->umonitor == NULL)
		return 0;

	spa_loop_remove_source(this->main_loop, &this->source);
	udev_monitor_unref(this->umonitor);
	this->umonitor = NULL;

	stop_inotify(this);

	return 0;
}

static int enum_devices(struct impl *this)
{
	struct udev_enumerate *enumerate;
	struct udev_list_entry *devices;

	enumerate = udev_enumerate_new(this->udev);
	if (enumerate == NULL)
		return -ENOMEM;

	udev_enumerate_add_match_subsystem(enumerate, "sound");
	udev_enumerate_scan_devices(enumerate);

	for (devices = udev_enumerate_get_list_entry(enumerate); devices;
			devices = udev_list_entry_get_next(devices)) {
		struct udev_device *dev;

		dev = udev_device_new_from_syspath(this->udev, udev_list_entry_get_name(devices));
		if (dev == NULL)
			continue;

		emit_device(this, ACTION_ADD, true, dev);

		udev_device_unref(dev);
	}
	udev_enumerate_unref(enumerate);

	return 0;
}

static const struct spa_dict_item device_info_items[] = {
	{ SPA_KEY_DEVICE_API, "udev" },
	{ SPA_KEY_DEVICE_NICK, "alsa-udev" },
	{ SPA_KEY_API_UDEV_MATCH, "sound" },
};

static void emit_device_info(struct impl *this, bool full)
{
	if (full)
		this->info.change_mask = this->info_all;
	if (this->info.change_mask) {
		this->info.props = &SPA_DICT_INIT_ARRAY(device_info_items);
		spa_device_emit_info(&this->hooks, &this->info);
		this->info.change_mask = 0;
	}
}

static void impl_hook_removed(struct spa_hook *hook)
{
	struct impl *this = hook->priv;
	if (spa_hook_list_is_empty(&this->hooks)) {
		stop_monitor(this);
		impl_udev_close(this);
	}
}

static int
impl_device_add_listener(void *object, struct spa_hook *listener,
		const struct spa_device_events *events, void *data)
{
	int res;
	struct impl *this = object;
        struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(events != NULL, -EINVAL);

	if ((res = impl_udev_open(this)) < 0)
		return res;

        spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	emit_device_info(this, true);

	if ((res = enum_devices(this)) < 0)
		return res;

	if ((res = start_monitor(this)) < 0)
		return res;

        spa_hook_list_join(&this->hooks, &save);

	listener->removed = impl_hook_removed;
	listener->priv = this;

	return 0;
}

static const struct spa_device_methods impl_device = {
	SPA_VERSION_DEVICE_METHODS,
	.add_listener = impl_device_add_listener,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (strcmp(type, SPA_TYPE_INTERFACE_Device) == 0)
		*interface = &this->device;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
        struct impl *this = (struct impl *) handle;
	stop_monitor(this);
	impl_udev_close(this);
	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct impl);
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct impl *this;
	const char *str;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;
	this->notify.fd = -1;

	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	this->main_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Loop);

	if (this->main_loop == NULL) {
		spa_log_error(this->log, "a main-loop is needed");
		return -EINVAL;
	}
	spa_hook_list_init(&this->hooks);

	this->device.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Device,
			SPA_VERSION_DEVICE,
			&impl_device, this);

	this->info = SPA_DEVICE_INFO_INIT();
	this->info_all = SPA_DEVICE_CHANGE_MASK_FLAGS |
			SPA_DEVICE_CHANGE_MASK_PROPS;
	this->info.flags = 0;

	if (info) {
		if ((str = spa_dict_lookup(info, "alsa.use-acp")) != NULL)
			this->use_acp = strcmp(str, "true") == 0 || atoi(str) != 0;
	}

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Device,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	if (*index >= SPA_N_ELEMENTS(impl_interfaces))
		return 0;

	*info = &impl_interfaces[(*index)++];
	return 1;
}

const struct spa_handle_factory spa_alsa_udev_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_ALSA_ENUM_UDEV,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
