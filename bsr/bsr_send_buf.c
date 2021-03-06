/*
	Copyright(C) 2007-2016, ManTechnology Co., LTD.
	Copyright(C) 2007-2016, bsr@mantech.co.kr

	Windows BSR is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2, or (at your option)
	any later version.

	Windows BSR is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Windows BSR; see the file COPYING. If not, write to
	the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "bsr_int.h"
#ifdef _WIN
#include "bsr-kernel-compat/windows/bsr_windows.h"
#include "../bsr-platform/windows/bsrvflt/wsk_wrapper.h"
#include "bsr-kernel-compat/windows/bsr_wingenl.h"
#include "bsr-kernel-compat/windows/bsr_endian.h"
#include "bsr-kernel-compat/windows/idr.h"
#include "../bsr-platform/windows/bsrvflt/disp.h" 
#include "bsr_send_buf.h"
#else // _LIN
#include <linux/vmalloc.h>
#include <linux/net.h>
#include <net/sock.h>
#endif
#include "../bsr-headers/linux/bsr_limits.h"

#ifdef _SEND_BUF
#define EnterCriticalSection mutex_lock
#define LeaveCriticalSection mutex_unlock

#define MAX_ONETIME_SEND_BUF	(1024*1024*10) // 10MB

struct buffer {
	void *base;
	void *pos;
};

struct bsr_tcp_transport {
	struct bsr_transport transport; /* Must be first! */
	spinlock_t paths_lock;
	ULONG_PTR flags;
	struct socket *stream[2];
	struct buffer rbuf[2];
#ifdef _LIN_SEND_BUF
	struct _buffering_attr buffering_attr[2];
#endif
};

#ifdef _LIN_SEND_BUF
#define FALSE false;
#define TRUE true;

void *bsr_kvmalloc(size_t size, gfp_t flags)
{
	void *ret;

	ret = kmalloc(size, flags | __GFP_NOWARN, '');
	if (!ret)
		ret = __vmalloc(size, flags, PAGE_KERNEL);
	return ret;
}
#endif

bool alloc_bab(struct bsr_connection* connection, struct net_conf* nconf) 
{
	ring_buffer* ring = NULL;
	signed long long sz = 0;

	if(0 == nconf->sndbuf_size) {
		return FALSE;
	}

	do {
		if(nconf->sndbuf_size < BSR_SNDBUF_SIZE_MIN ) {
			bsr_info(NO_OBJECT,"alloc bab fail nconf->sndbuf_size < BSR_SNDBUF_SIZE_MIN connection->peer_node_id:%u nconf->sndbuf_size:%llu\n", connection->peer_node_id, nconf->sndbuf_size);
			goto $ALLOC_FAIL;
		}
#ifdef _WIN_SEND_BUF
		__try {
#endif
			sz = sizeof(*ring) + nconf->sndbuf_size;
#ifdef _WIN_SEND_BUF
			ring = (ring_buffer*)ExAllocatePoolWithTag(NonPagedPool|POOL_RAISE_IF_ALLOCATION_FAILURE, (size_t)sz, '0ADW'); //POOL_RAISE_IF_ALLOCATION_FAILURE flag is required for big pool
#else // _LIN_SEND_BUF
			// BSR-453 Exception handling when there is not enough memory available
			ring = (ring_buffer*)bsr_kvmalloc((size_t)sz, GFP_ATOMIC | __GFP_NOWARN);
#endif
			if(!ring) {
				bsr_info(NO_OBJECT,"alloc data bab fail connection->peer_node_id:%u nconf->sndbuf_size:%llu\n", connection->peer_node_id, nconf->sndbuf_size);
				goto $ALLOC_FAIL;
			}
			// DW-1927 Sets the size value when the buffer is allocated.
			ring->length = nconf->sndbuf_size + 1;
#ifdef _WIN_SEND_BUF
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			bsr_info(NO_OBJECT,"EXCEPTION_EXECUTE_HANDLER alloc data bab fail connection->peer_node_id:%u nconf->sndbuf_size:%llu\n", connection->peer_node_id, nconf->sndbuf_size);
			if(ring) {
				ExFreePool(ring);
			}
			goto $ALLOC_FAIL;
		}
#endif
		
		connection->ptxbab[DATA_STREAM] = ring;
#ifdef _WIN_SEND_BUF
		__try {
#endif
			sz = sizeof(*ring) + CONTROL_BUFF_SIZE; // meta bab is about 5MB
#ifdef _WIN_SEND_BUF
			ring = (ring_buffer*)ExAllocatePoolWithTag(NonPagedPool | POOL_RAISE_IF_ALLOCATION_FAILURE, (size_t)sz, '2ADW');
#else // _LIN_SEND_BUF
			// BSR-453 Exception handling when there is not enough memory available
			ring = (ring_buffer*)bsr_kvmalloc((size_t)sz, GFP_ATOMIC | __GFP_NOWARN);
#endif
			if(!ring) {
				bsr_info(NO_OBJECT,"alloc meta bab fail connection->peer_node_id:%u nconf->sndbuf_size:%llu\n", connection->peer_node_id, nconf->sndbuf_size);
				kvfree2(connection->ptxbab[DATA_STREAM]); // fail, clean data bab
				goto $ALLOC_FAIL;
			}
			// DW-1927 Sets the size value when the buffer is allocated.
			ring->length = CONTROL_BUFF_SIZE + 1;
#ifdef _WIN_SEND_BUF
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			bsr_info(NO_OBJECT,"EXCEPTION_EXECUTE_HANDLER alloc meta bab fail connection->peer_node_id:%u nconf->sndbuf_size:%llu\n", connection->peer_node_id, nconf->sndbuf_size);
			if(ring) {
				ExFreePool(ring);
			}
			goto $ALLOC_FAIL;
		}
#endif
		connection->ptxbab[CONTROL_STREAM] = ring;
		
	} while (false);
	
	bsr_info(NO_OBJECT,"alloc_bab ok connection->peer_node_id:%d nconf->sndbuf_size:%lld\n", connection->peer_node_id, nconf->sndbuf_size);
	return TRUE;

$ALLOC_FAIL:
	connection->ptxbab[DATA_STREAM] = NULL;
	connection->ptxbab[CONTROL_STREAM] = NULL;
	return FALSE;
}

void destroy_bab(struct bsr_connection* connection)
{
	kvfree2(connection->ptxbab[DATA_STREAM]);
	kvfree2(connection->ptxbab[CONTROL_STREAM]);
	return;
}

ring_buffer *create_ring_buffer(struct bsr_connection* connection, char *name, signed long long length, enum bsr_stream stream)
{
	ring_buffer *ring;
	signed long long sz = sizeof(*ring) + length;

	if (length == 0 || length > BSR_SNDBUF_SIZE_MAX) {
		bsr_err(NO_OBJECT,"bab(%s) size(%lld) is bad. max(%ld)\n", name, length, BSR_SNDBUF_SIZE_MAX);
		return NULL;
	}

	//ring = (ring_buffer *) ExAllocatePoolWithTag(NonPagedPool, sz, '0ADW');
	if(stream == DATA_STREAM) {
		ring = connection->ptxbab[DATA_STREAM];
	} else {
		ring = connection->ptxbab[CONTROL_STREAM];
	}
	if (ring) {
		ring->mem = (char*) (ring + 1);
		ring->read_pos = 0;
		ring->write_pos = 0;
		ring->que = 0;
		ring->deque = 0;
		ring->seq = 0;
		ring->name = name;

		mutex_init(&ring->cs);

		//bsr_info(NO_OBJECT,"bab(%s) size(%d)\n", name, length);
#ifdef SENDBUF_TRACE
		INIT_LIST_HEAD(&ring->send_req_list);
#endif
#ifdef _WIN_SEND_BUF
		ring->static_big_buf = (char *) ExAllocatePoolWithTag(NonPagedPool, MAX_ONETIME_SEND_BUF, '1ADW');
#else
		// BSR-453 Exception handling when there is not enough memory available
		ring->static_big_buf = (char *)bsr_kvmalloc(MAX_ONETIME_SEND_BUF, GFP_ATOMIC | __GFP_NOWARN);
#endif
		if (!ring->static_big_buf) {
			//ExFreePool(ring);
			//kfree2(ring);
			bsr_err(NO_OBJECT,"bab(%s): static_big_buf alloc(%d) failed.  \n", name, MAX_ONETIME_SEND_BUF);
			return NULL;
		}
	} else {
		bsr_err(NO_OBJECT,"bab(%s):alloc(%lld) failed\n", name, sz);
	}
	return ring;
}

void destroy_ring_buffer(ring_buffer *ring)
{
	if (ring) {
		kvfree2(ring->static_big_buf);
		//ExFreePool(ring);
 		//kfree2(ring);
	}
}

signed long long get_ring_buffer_size(ring_buffer *ring)
{
	signed long long s;
	if (!ring) {
		return 0;
	}

	EnterCriticalSection(&ring->cs);
	s = (ring->write_pos - ring->read_pos + ring->length) % ring->length;
	LeaveCriticalSection(&ring->cs);

	return s;
}

signed long long write_ring_buffer(struct bsr_transport *transport, enum bsr_stream stream, ring_buffer *ring, const char *data, signed long long len, signed long long highwater, int retry)
{
	signed long long remain;
	signed long long ringbuf_size = 0;

	EnterCriticalSection(&ring->cs);

	ringbuf_size = (ring->write_pos - ring->read_pos + ring->length) % ring->length;

	if ((ringbuf_size + len) > highwater) {

		LeaveCriticalSection(&ring->cs);
		// DW-764 remove debug print "kocount" when congestion is not occurred.
		do {
			int loop = 0;
			for (loop = 0; loop < retry; loop++) {
				struct bsr_tcp_transport *tcp_transport;

				msleep(100);

				tcp_transport =	container_of(transport, struct bsr_tcp_transport, transport);

#ifdef _WIN_SEND_BUF
				if (tcp_transport->stream[stream]) {
					if (tcp_transport->stream[stream]->buffering_attr.quit == TRUE)	{
						bsr_info(NO_OBJECT,"Stop send and quit\n");
						return -EIO;
					}
				}
#else // _LIN_SEND_BUF
				if (tcp_transport) {
					if (tcp_transport->buffering_attr[stream].quit == true)	{
						bsr_info(NO_OBJECT,"Stop send and quit\n");
						return -EIO;
					}
				}
#endif
				EnterCriticalSection(&ring->cs);
				ringbuf_size = (ring->write_pos - ring->read_pos + ring->length) % ring->length;
				if ((ringbuf_size + len) > highwater) {
				} else {
					goto $GO_BUFFERING;
				}
				LeaveCriticalSection(&ring->cs);
			}
		} while (!bsr_stream_send_timed_out(transport, stream));
		 		
		return -EAGAIN;
	}

$GO_BUFFERING:

	remain = (ring->read_pos - ring->write_pos - 1 + ring->length) % ring->length;
	if (remain < len) {
		len = remain;
	}

	if (len > 0) {
		remain = ring->length - ring->write_pos;
		if (remain < len) {
			memcpy(ring->mem + (ring->write_pos), data, (size_t)remain);
			memcpy(ring->mem, data + remain, (size_t)(len - remain));
		} else {
			memcpy(ring->mem + ring->write_pos, data, (size_t)len);
		}

		ring->write_pos += len;
		ring->write_pos %= ring->length;
	}
	else {
		bsr_err(NO_OBJECT,"unexpected bab case\n");
		BUG();
	}

	ring->que++;
	ring->seq++;
	ring->sk_wmem_queued = (ring->write_pos - ring->read_pos + ring->length) % ring->length;

	LeaveCriticalSection(&ring->cs);

	return len;
}

#ifdef _WIN_SEND_BUF
bool read_ring_buffer(IN ring_buffer *ring, OUT char *data, OUT signed long long* pLen)
#else // _LIN_SEND_BUF
bool read_ring_buffer(ring_buffer *ring, char *data, signed long long* pLen)
#endif
{
	signed long long remain;
	signed long long ringbuf_size = 0;
	signed long long tx_sz = 0;

	EnterCriticalSection(&ring->cs);
	ringbuf_size = (ring->write_pos - ring->read_pos + ring->length) % ring->length;
	
	if (ringbuf_size == 0) {
		LeaveCriticalSection(&ring->cs);
		return 0;
	}
 
	tx_sz = (ringbuf_size > MAX_ONETIME_SEND_BUF) ? MAX_ONETIME_SEND_BUF : ringbuf_size;

	remain = ring->length - ring->read_pos;
	if (remain < tx_sz) {
		memcpy(data, ring->mem + ring->read_pos, (size_t)remain);
		memcpy(data + remain, ring->mem, (size_t)(tx_sz - remain));
	}
	else {
		memcpy(data, ring->mem + ring->read_pos, (size_t)tx_sz);
	}

	ring->read_pos += tx_sz;
	ring->read_pos %= ring->length;
	ring->sk_wmem_queued = (ring->write_pos - ring->read_pos + ring->length) % ring->length;
	*pLen = tx_sz;
	LeaveCriticalSection(&ring->cs);
	
	return 1;
}

#ifdef _WIN_SEND_BUF
int send_buf(struct bsr_transport *transport, enum bsr_stream stream, socket *socket, PVOID buf, LONG size)
{
	struct _buffering_attr *buffering_attr = &socket->buffering_attr;
	ULONG timeout = socket->sk_linux_attr->sk_sndtimeo;

	if (buffering_attr->send_buf_thread_handle == NULL || buffering_attr->bab == NULL) {
		return Send(socket, buf, size, 0, timeout, NULL, transport, stream);
	}

	signed long long  tmp = (long long)buffering_attr->bab->length * 99;
	signed long long highwater = (signed long long)tmp / 100; // 99% // refacto: global
	// performance tuning point for delay time
	int retry = socket->sk_linux_attr->sk_sndtimeo / 100; //retry default count : 6000/100 = 60 => write buffer delay time : 100ms => 60*100ms = 6sec //retry default count : 6000/20 = 300 => write buffer delay time : 20ms => 300*20ms = 6sec

	size = (LONG)write_ring_buffer(transport, stream, buffering_attr->bab, buf, size, highwater, retry);

	KeSetEvent(&buffering_attr->ring_buf_event, 0, FALSE);
	return (int)size;
}
#else // _LIN_SEND_BUF
int send_buf(struct bsr_tcp_transport *tcp_transport, enum bsr_stream stream, struct socket *socket, void *buf, size_t size)
{
	struct _buffering_attr *buffering_attr = &tcp_transport->buffering_attr[stream];
	signed long long tmp;
	signed long long highwater;
	int retry;

	if (buffering_attr->send_buf_thread_handle == NULL || buffering_attr->bab == NULL) {
		struct kvec iov;
		struct msghdr msg;
		int rv, msg_flags = 0;;

		iov.iov_base = buf;
		iov.iov_len  = size;

		msg.msg_name       = NULL;
		msg.msg_namelen    = 0;
		msg.msg_control    = NULL;
		msg.msg_controllen = 0;
		msg.msg_flags      = msg_flags | MSG_NOSIGNAL;

		rv = kernel_sendmsg(socket, &msg, &iov, 1, size);
		if (rv == -EAGAIN) {
		}
		return rv;
	}

	tmp = (long long)buffering_attr->bab->length * 99;
	highwater = (signed long long)tmp / 100; // 99% // refacto: global
	// performance tuning point for delay time
	retry = socket->sk->sk_sndtimeo / 100;
	
	size = write_ring_buffer(&tcp_transport->transport, stream, buffering_attr->bab, buf, size, highwater, retry);

	set_bit(RING_BUF_EVENT, &buffering_attr->flags);
	wake_up(&buffering_attr->ring_buf_event);
	return (int)size;
}
#endif

#ifdef _WIN_SEND_BUF
int do_send(struct socket *socket, struct ring_buffer *bab, int timeout, KEVENT *send_buf_kill_event)
{
	UNREFERENCED_PARAMETER(send_buf_kill_event);
	int ret = 0;

	if (bab == NULL) {
		bsr_err(NO_OBJECT,"bab is null.\n");
		return 0;
	}

	while (true) {
		long long tx_sz = 0;

		if (!read_ring_buffer(bab, bab->static_big_buf, &tx_sz)) {
			break;
		}
#ifndef _WIN64
		BUG_ON_UINT32_OVER(tx_sz);
#endif
		// DW-1095 SendAsync is only used on Async mode (adjust retry_count) 
		//ret = SendAsync(socket, bab->static_big_buf, tx_sz, 0, timeout, NULL, 0);
		ret = Send(socket, bab->static_big_buf, (ULONG)tx_sz, 0, timeout, NULL, NULL, 0);
		if (ret != tx_sz) {
			if (ret < 0) {
				if (ret != -EINTR) {
					bsr_info(NO_OBJECT,"Send Error(%d)\n", ret);
					ret = 0;
				}
				break;
			} else {
				bsr_info(NO_OBJECT,"Tx mismatch. req(%d) sent(%d)\n", tx_sz, ret);
				// will be recovered by upper bsr protocol
			}
		}
	}

	return ret;
}
#else // _LIN_SEND_BUF
int do_send(struct socket *socket, struct ring_buffer *bab, int timeout)
{
	int rv = 0;
	int msg_flags = 0;

	if (bab == NULL) {
		bsr_err(NO_OBJECT,"bab is null.\n");
		return 0;
	}

	while (true) {
		long long tx_sz = 0;
		struct kvec iov;
		struct msghdr msg;

		if (!read_ring_buffer(bab, bab->static_big_buf, &tx_sz)) {
			break;
		}

		iov.iov_base = bab->static_big_buf;
		iov.iov_len  = (unsigned long)tx_sz;

		msg.msg_name       = NULL;
		msg.msg_namelen    = 0;
		msg.msg_control    = NULL;
		msg.msg_controllen = 0;
		msg.msg_flags      = msg_flags | MSG_NOSIGNAL;

		rv = kernel_sendmsg(socket, &msg, &iov, 1, (size_t)tx_sz);
		if (rv == -EAGAIN) {
		}
		if (rv == -EINTR) {
			flush_signals(current);
			rv = 0;
		}
	}

	return rv;
}
#endif

//
// send buffring thread
//
#ifdef _WIN_SEND_BUF
VOID NTAPI send_buf_thread(PVOID p)
{
	struct _buffering_attr *buffering_attr = (struct _buffering_attr *)p;
	struct socket *socket = container_of(buffering_attr, struct socket, buffering_attr);
	NTSTATUS status;
	LARGE_INTEGER nWaitTime;
	LARGE_INTEGER *pTime;

	ct_add_thread((int)PsGetCurrentThreadId(), "sendbuf", FALSE, '25DW');

	buffering_attr->quit = FALSE;

	//KeSetPriorityThread(KeGetCurrentThread(), HIGH_PRIORITY);
	//bsr_info(NO_OBJECT,"start send_buf_thread\n");

	KeSetEvent(&buffering_attr->send_buf_thr_start_event, 0, FALSE);
	nWaitTime = RtlConvertLongToLargeInteger(-10 * 1000 * 1000 * 10);
	pTime = &nWaitTime;

#define MAX_EVT		2
	PVOID waitObjects[MAX_EVT];
	waitObjects[0] = &buffering_attr->send_buf_kill_event;
	waitObjects[1] = &buffering_attr->ring_buf_event;

	while (true) {
		status = KeWaitForMultipleObjects(MAX_EVT, &waitObjects[0], WaitAny, Executive, KernelMode, FALSE, pTime, NULL);
		switch (status) {
		case STATUS_TIMEOUT:
			break;

		case STATUS_WAIT_0:
			bsr_info(NO_OBJECT,"response kill-ack-event\n");
			goto done;

		case (STATUS_WAIT_0 + 1) :
			if (do_send(socket, buffering_attr->bab, socket->sk_linux_attr->sk_sndtimeo, &buffering_attr->send_buf_kill_event) == -EINTR) {
				goto done;
			}
			break;

		default:
			bsr_err(NO_OBJECT,"unexpected wakeup case(0x%x). ignore.\n", status);
			goto done;
		}
	}

done:
	bsr_info(NO_OBJECT,"send_buf_killack_event!\n");
	KeSetEvent(&buffering_attr->send_buf_killack_event, 0, FALSE);
	bsr_info(NO_OBJECT,"sendbuf thread[%p] terminate!!\n",KeGetCurrentThread());
	ct_delete_thread((int)PsGetCurrentThreadId());
	PsTerminateSystemThread(STATUS_SUCCESS);
}
#else // _LIN_SEND_BUF
int send_buf_thread(void *p)
{
	struct bsr_tcp_transport *tcp_transport = (struct bsr_tcp_transport *)p;
	//struct bsr_tcp_transport *tcp_transport = container_of(buffering_attr, struct bsr_tcp_transport, buffering_attr);
	struct socket *socket;
	struct _buffering_attr *buffering_attr;
	int ret = 0;
	
	long timeo = 1 * HZ;

	if(test_bit(IDX_STREAM, &tcp_transport->flags)) {
		socket = tcp_transport->stream[CONTROL_STREAM];
		buffering_attr = &tcp_transport->buffering_attr[CONTROL_STREAM];
	}
	else {
		socket = tcp_transport->stream[DATA_STREAM];
		buffering_attr = &tcp_transport->buffering_attr[DATA_STREAM];
	}
	

	buffering_attr->quit = false;

	set_bit(SEND_BUF_START, &buffering_attr->flags);
	wake_up(&buffering_attr->send_buf_thr_start_event);

	while (!buffering_attr->send_buf_kill_event) {
		wait_event_timeout_ex(buffering_attr->ring_buf_event, test_bit(RING_BUF_EVENT, &buffering_attr->flags), timeo, ret);
		if(test_bit(RING_BUF_EVENT, &buffering_attr->flags))
			do_send(socket, buffering_attr->bab, socket->sk->sk_sndtimeo);
		clear_bit(RING_BUF_EVENT, &buffering_attr->flags);
	}

	bsr_info(NO_OBJECT,"send_buf_killack_event!\n");
	set_bit(SEND_BUF_KILLACK, &buffering_attr->flags);
	wake_up(&buffering_attr->send_buf_killack_event);
	bsr_info(NO_OBJECT,"sendbuf thread terminate!!\n");
	return 0;
}
#endif
#endif // _SEND_BUF