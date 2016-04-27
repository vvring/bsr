﻿#include "drbd_windows.h"
#include "wsk2.h"
#include "drbd_wingenl.h"
#include "linux-compat/drbd_endian.h"
#include "linux-compat/idr.h"
#include "disp.h" 
#include "drbd_int.h"
#include "send_buf.h"	
#include <linux/drbd_limits.h>

#ifdef _WIN32_SEND_BUFFING
#define EnterCriticalSection mutex_lock
#define LeaveCriticalSection mutex_unlock

#define MAX_ONETIME_SEND_BUF	(1024*1024*10) // 10MB //(64*1024) // 64K // (1024*1024*10) // 10MB

ring_buffer *create_ring_buffer(char *name, unsigned int length)
{
	ring_buffer *ring;
	int sz = sizeof(*ring) + length;

	if (length == 0 || length > DRBD_SNDBUF_SIZE_MAX)
	{
		WDRBD_ERROR("bab(%s) size(%d) is bad. max(%d)\n", name, length, DRBD_SNDBUF_SIZE_MAX);
		return NULL;
	}

	ring = (ring_buffer *) ExAllocatePoolWithTag(NonPagedPool, sz, '0ADW');
	if (ring)
	{
		ring->mem = (char*) (ring + 1);
		ring->length = length + 1;
		ring->read_pos = 0;
		ring->write_pos = 0;
		ring->que = 0;
		ring->deque = 0;
		ring->seq = 0;
		ring->name = name;

#ifdef _WIN32_TMP_DEBUG_MUTEX
		mutex_init(&ring->cs, "sendbuf");
#else
		mutex_init(&ring->cs);
#endif
		//WDRBD_INFO("bab(%s) size(%d)\n", name, length);
#ifdef SENDBUF_TRACE
		INIT_LIST_HEAD(&ring->send_req_list);
#endif
		ring->static_big_buf = (char *) ExAllocatePoolWithTag(NonPagedPool, MAX_ONETIME_SEND_BUF, '1ADW');
		if (!ring->static_big_buf)
		{
			ExFreePool(ring);
			WDRBD_ERROR("bab(%s): alloc(%d) failed.\n", name, MAX_ONETIME_SEND_BUF);
			return NULL;
		}
	}
	else
	{
		WDRBD_ERROR("bab(%s):alloc(%u) failed\n", name, sz);
	}
	return ring;
}

void destroy_ring_buffer(ring_buffer *ring)
{
	if (ring)
	{
			kfree(ring->static_big_buf);
		ExFreePool(ring);
	}
}

unsigned int get_ring_buffer_size(ring_buffer *ring)
{
	unsigned int s;
	if (!ring)
	{
		return 0;
	}

	EnterCriticalSection(&ring->cs);
	s = (ring->write_pos - ring->read_pos + ring->length) % ring->length;
	LeaveCriticalSection(&ring->cs);

	return s;
}

int write_ring_buffer(struct drbd_transport *transport, enum drbd_stream stream, ring_buffer *ring, const char *data, int len, int highwater, int retry)
{
	unsigned int remain;
	int ringbuf_size = 0;
	LARGE_INTEGER	Interval;
	Interval.QuadPart = (-1 * 100 * 10000);   //// wait 100ms relative // wait 20ms relative

	EnterCriticalSection(&ring->cs);

	ringbuf_size = (ring->write_pos - ring->read_pos + ring->length) % ring->length;

	if ((ringbuf_size + len) > highwater) {

		LeaveCriticalSection(&ring->cs);
		// DW-764 remove debug print "kocount" when congestion is not occurred.
		do {
			int loop = 0;
			for (loop = 0; loop < retry; loop++) {
				KeDelayExecutionThread(KernelMode, FALSE, &Interval);
				//KTIMER ktimer;
				//KeInitializeTimer(&ktimer);
				//KeSetTimerEx(&ktimer, Interval, 0, NULL);
				//KeWaitForSingleObject(&ktimer, Executive, KernelMode, FALSE, NULL);

				// _WIN32_V9: redefine struct drbd_tcp_transport, buffer. 추후 멤버함수로 drbd_tcp_transport 에 접근하도록 처리하고 다음 2개 자료구조는 제거
				struct buffer {
					void *base;
					void *pos;
				};

				struct drbd_tcp_transport {
					struct drbd_transport transport; /* Must be first! */
					struct mutex paths_mutex;
					unsigned long flags;
					struct socket *stream[2];
					struct buffer rbuf[2];
				};

				struct drbd_tcp_transport *tcp_transport =
					container_of(transport, struct drbd_tcp_transport, transport);
				if (tcp_transport->stream[stream]->buffering_attr.quit == TRUE)
				{
					WDRBD_INFO("Stop send and quit\n");
					return -EIO;
				}

				EnterCriticalSection(&ring->cs);
				ringbuf_size = (ring->write_pos - ring->read_pos + ring->length) % ring->length;
				if ((ringbuf_size + len) > highwater) {
				} else {
					goto $GO_BUFFERING;
				}
				LeaveCriticalSection(&ring->cs);
			}
		} while (!drbd_stream_send_timed_out(transport, stream));
		 		
		return -EAGAIN;
	}

$GO_BUFFERING:
	////////////////////////////////////////////////////////////////////////////////
	remain = (ring->read_pos - ring->write_pos - 1 + ring->length) % ring->length;
	if (remain < len) {
		len = remain;
	}

	if (len > 0) {
		remain = ring->length - ring->write_pos;
		if (remain < len) {
			memcpy(ring->mem + (ring->write_pos), data, remain);
			memcpy(ring->mem, data + remain, len - remain);
		} else {
			memcpy(ring->mem + ring->write_pos, data, len);
		}

		ring->write_pos += len;
		ring->write_pos %= ring->length;
	}
	else {
		WDRBD_ERROR("unexpected bab case\n");
		BUG();
	}

	ring->que++;
	ring->seq++;
	ring->sk_wmem_queued = (ring->write_pos - ring->read_pos + ring->length) % ring->length;

	LeaveCriticalSection(&ring->cs);

	return len;
}

unsigned long read_ring_buffer(IN ring_buffer *ring, OUT char *data, OUT unsigned int* pLen)
{
	unsigned int remain;
	unsigned int ringbuf_size = 0;
	unsigned int tx_sz = 0;

	EnterCriticalSection(&ring->cs);
	ringbuf_size = (ring->write_pos - ring->read_pos + ring->length) % ring->length;
	
	if (ringbuf_size == 0) {
		LeaveCriticalSection(&ring->cs);
		return 0;
	}
 
	tx_sz = (ringbuf_size > MAX_ONETIME_SEND_BUF) ? MAX_ONETIME_SEND_BUF : ringbuf_size;

	remain = ring->length - ring->read_pos;
	if (remain < tx_sz) {
		memcpy(data, ring->mem + ring->read_pos, remain);
		memcpy(data + remain, ring->mem, tx_sz - remain);
	}
	else {
		memcpy(data, ring->mem + ring->read_pos, tx_sz);
	}

	ring->read_pos += tx_sz;
	ring->read_pos %= ring->length;
	ring->sk_wmem_queued = (ring->write_pos - ring->read_pos + ring->length) % ring->length;

	*pLen = tx_sz;

	LeaveCriticalSection(&ring->cs);
	
	return 1;

}

int send_buf(struct drbd_transport *transport, enum drbd_stream stream, struct socket *socket, PVOID buf, ULONG size)
{
	// struct drbd_connection *connection = container_of(transport, struct drbd_connection, transport);
	struct _buffering_attr *buffering_attr = &socket->buffering_attr;
	ULONG timeout = socket->sk_linux_attr->sk_sndtimeo;

	if (buffering_attr->send_buf_thread_handle == NULL || buffering_attr->bab == NULL) {
		return Send(socket->sk, buf, size, 0, timeout, NULL, transport, stream);
	}

	unsigned long long  tmp = (long long)buffering_attr->bab->length * 99;
	int highwater = (unsigned long long)tmp / 100; // 99% // refacto: global
	// 기존에 비해 buffer write time 대기시간을 줄이고 재시도 횟수를 늘려 송신버퍼링 타임아웃 설정에 맞춤.(성능 관련 튜닝 포인트)
	int retry = socket->sk_linux_attr->sk_sndtimeo / 100; //retry default count : 6000/100 = 60 => write buffer delay time : 100ms => 60*100ms = 6sec //retry default count : 6000/20 = 300 => write buffer delay time : 20ms => 300*20ms = 6sec

	size = write_ring_buffer(transport, stream, buffering_attr->bab, buf, size, highwater, retry);

	KeSetEvent(&buffering_attr->ring_buf_event, 0, FALSE);
	return size;

}

#ifdef _WSK_IRP_REUSE
int do_send(PIRP pReuseIrp, PWSK_SOCKET sock, struct ring_buffer *bab, int timeout, KEVENT *send_buf_kill_event)
#else
int do_send(PWSK_SOCKET sock, struct ring_buffer *bab, int timeout, KEVENT *send_buf_kill_event)
#endif
{
	int ret = 0;

	if (bab == NULL) {
		WDRBD_ERROR("bab is null.\n");
		return 0;
	}

	while (1) {
		unsigned int tx_sz = 0;

		if (!read_ring_buffer(bab, bab->static_big_buf, &tx_sz)) {
			break;
		}
		
#ifdef _WSK_IRP_REUSE
		ret = SendEx(pReuseIrp, sock, bab->static_big_buf, tx_sz, 0, NULL, 0);
#else
		ret = Send(sock, bab->static_big_buf, tx_sz, 0, timeout, send_buf_kill_event, NULL, 0);
#endif
		if (ret != tx_sz) {
			if (ret < 0) {
				if (ret == -EINTR) {
					ret = -EINTR;
				}
				else {
					WDRBD_WARN("Send Error(%d)\n", ret);
					ret = 0;
				}
				break;
			} else {
				WDRBD_WARN("Tx mismatch. req(%d) sent(%d)\n", tx_sz, ret);
				// will be recovered by upper drbd protocol 
			}
		}
	}

	return ret;
}

//
// send buffring thread
//

VOID NTAPI send_buf_thread(PVOID p)
{
	struct _buffering_attr *buffering_attr = (struct _buffering_attr *)p;
	struct socket *socket = container_of(buffering_attr, struct socket, buffering_attr);
	LONG readcount;
	NTSTATUS status;
	LARGE_INTEGER nWaitTime;
	LARGE_INTEGER *pTime;

	buffering_attr->quit = FALSE;

	//KeSetPriorityThread(KeGetCurrentThread(), HIGH_PRIORITY);
	//WDRBD_INFO("start send_buf_thread\n");
	KeSetEvent(&buffering_attr->send_buf_thr_start_event, 0, FALSE);
	nWaitTime = RtlConvertLongToLargeInteger(-10 * 1000 * 1000 * 10);
	pTime = &nWaitTime;

#define MAX_EVT		2
	PVOID waitObjects[MAX_EVT];
	waitObjects[0] = &buffering_attr->send_buf_kill_event;
	waitObjects[1] = &buffering_attr->ring_buf_event;
#ifdef _WSK_IRP_REUSE
	// 패킷을 한번에 하나씩만 보내는 구조이므로, Irp 재사용 하여 중복되는 Irp 할당/해제 코드를 개선.
	PIRP		pReuseIrp = IoAllocateIrp(1, FALSE);
	if (pReuseIrp == NULL) {
		WDRBD_ERROR("WSK alloc. reuse Irp is NULL.\n");
		return;
	}
#endif

	while (TRUE)
	{
		status = KeWaitForMultipleObjects(MAX_EVT, &waitObjects[0], WaitAny, Executive, KernelMode, FALSE, pTime, NULL);
		switch (status)
		{
		case STATUS_TIMEOUT:
			break;

		case STATUS_WAIT_0:
			WDRBD_INFO("response kill-ack-event\n");
			goto done;

		case (STATUS_WAIT_0 + 1) :
#ifdef _WSK_IRP_REUSE
			if (do_send(pReuseIrp , socket->sk, buffering_attr->bab, socket->sk_linux_attr->sk_sndtimeo, &buffering_attr->send_buf_kill_event) == -EINTR)
#else
			if (do_send(socket->sk, buffering_attr->bab, socket->sk_linux_attr->sk_sndtimeo, &buffering_attr->send_buf_kill_event) == -EINTR)
#endif
			{
				goto done;
			}
			break;

		default:
			WDRBD_ERROR("unexpected wakwup case(0x%x). ignore.\n", status);
		}
	}

done:
#ifdef _WSK_IRP_REUSE
	IoFreeIrp(pReuseIrp);
#endif
	WDRBD_INFO("send_buf_killack_event!\n");
	KeSetEvent(&buffering_attr->send_buf_killack_event, 0, FALSE);
	WDRBD_INFO("sendbuf thread done.!!\n");
	PsTerminateSystemThread(STATUS_SUCCESS);
}

#endif // _WIN32_SEND_BUFFING