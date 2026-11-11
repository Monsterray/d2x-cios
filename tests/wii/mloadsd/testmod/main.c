/*
 * mloadtest: a module for the SD loader's Wii check (tests/wii/mloadsd).
 * It registers /dev/mloadtest, answers ioctl 1 with MLT_MAGIC, and says so on
 * the IOS log with svc_write. Nothing else: it touches no hardware.
 */
#include "types.h"
#include "ipc.h"
#include "syscalls.h"

#define MLT_DEVICE	"/dev/mloadtest"
#define MLT_MAGIC	0x4D4C5453	/* 'MLTS' */
#define MLT_IOCTL_PING	1

int main(void)
{
	static u32 queue_buf[8] ATTRIBUTE_ALIGN(32);
	s32 queue;

	svc_write("mloadtest: started\n");

	queue = os_message_queue_create(queue_buf, 8);
	if (queue < 0)
		return queue;
	if (os_device_register(MLT_DEVICE, queue) < 0)
		return -1;
	svc_write("mloadtest: /dev/mloadtest registered\n");

	while (1) {
		ipcmessage *message = NULL;
		s32 ret = 0;

		if (os_message_queue_receive(queue, (u32 *)&message, 0))
			continue;

		switch (message->command) {
		case IOS_OPEN:
			ret = message->open.resultfd;
			break;
		case IOS_CLOSE:
			break;
		case IOS_IOCTL:
			ret = (message->ioctl.command == MLT_IOCTL_PING) ? MLT_MAGIC : IPC_EINVAL;
			break;
		default:
			ret = IPC_EINVAL;
			break;
		}
		os_message_queue_ack(message, ret);
	}
}
