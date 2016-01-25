﻿#pragma once
#include <ntddk.h>
#include <wsk.h>

#define SOCKET_ERROR -1

NTSTATUS NTAPI SocketsInit();
VOID NTAPI SocketsDeinit();

NTSTATUS
InitWskBuffer(
	__in  PVOID		Buffer,
	__in  ULONG		BufferSize,
	__out PWSK_BUF	WskBuffer
	);

NTSTATUS
InitWskData(
	__out PIRP*		pIrp,
	__out PKEVENT	CompletionEvent
	);

VOID
ReInitWskData(
	__out PIRP*		pIrp,
	__out PKEVENT	CompletionEvent
	);

VOID
FreeWskBuffer(
	__in PWSK_BUF WskBuffer
	);

VOID
FreeWskData(
	__in PIRP pIrp
	);

PWSK_SOCKET
NTAPI
  CreateSocket(
    __in ADDRESS_FAMILY	AddressFamily,
    __in USHORT			SocketType,
    __in ULONG			Protocol,
    __in PVOID          *SocketContext,
    __in PWSK_CLIENT_LISTEN_DISPATCH Dispatch,
    __in ULONG			Flags
    );

NTSTATUS
NTAPI
  CloseSocket(
	__in PWSK_SOCKET WskSocket
	);

NTSTATUS
NTAPI
  Connect(
	__in PWSK_SOCKET	WskSocket,
	__in PSOCKADDR		RemoteAddress
	);

extern
NTSTATUS NTAPI
Disconnect(
	__in PWSK_SOCKET	WskSocket
	);

PWSK_SOCKET
NTAPI
  SocketConnect(
	__in USHORT		SocketType,
	__in ULONG		Protocol,
	__in PSOCKADDR	RemoteAddress,
	__in PSOCKADDR	LocalAddress
	);
#ifdef _WSK_IRP_REUSE
LONG
NTAPI
SendEx(
__in PIRP           pIrp,
__in PWSK_SOCKET	WskSocket,
__in PVOID			Buffer,
__in ULONG			BufferSize,
__in ULONG			Flags,
__in struct			drbd_transport *transport,
__in enum			drbd_stream stream
);
#endif

LONG
NTAPI
  Send(
	__in PWSK_SOCKET	WskSocket,
	__in PVOID			Buffer,
	__in ULONG			BufferSize,
	__in ULONG			Flags,
	__in ULONG			Timeout,
	__in KEVENT			*send_buf_kill_event,
	__in struct			drbd_transport *transport,
	__in enum			drbd_stream stream
	);

//_WIN32_V9: _WIN32_SEND_BUFFING: normal local send: netlink, helper
LONG
NTAPI
SendLocal(
	__in PWSK_SOCKET	WskSocket,
	__in PVOID			Buffer,
	__in ULONG			BufferSize,
	__in ULONG			Flags,
	__in ULONG			Timeout
);

LONG
NTAPI
  SendTo(
	__in PWSK_SOCKET	WskSocket,
	__in PVOID			Buffer,
	__in ULONG			BufferSize,
	__in_opt PSOCKADDR	RemoteAddress
	);

LONG
NTAPI
  Receive(
	__in  PWSK_SOCKET	WskSocket,
	__out PVOID			Buffer,
	__in  ULONG			BufferSize,
	__in  ULONG			Flags,
	__in ULONG			Timeout
	);

LONG
NTAPI
  ReceiveFrom(
	__in  PWSK_SOCKET	WskSocket,
	__out PVOID			Buffer,
	__in  ULONG			BufferSize,
	__out_opt PSOCKADDR	RemoteAddress,
	__out_opt PULONG	ControlFlags
	);

NTSTATUS
NTAPI
  Bind(
	__in PWSK_SOCKET	WskSocket,
	__in PSOCKADDR		LocalAddress
	);

PWSK_SOCKET
NTAPI
  Accept(
	__in PWSK_SOCKET	WskSocket,
	__out_opt PSOCKADDR	LocalAddress,
	__out_opt PSOCKADDR	RemoteAddress,
	__out PNTSTATUS		Error,
	int					timeout
   );

NTSTATUS
NTAPI
ControlSocket(
__in PWSK_SOCKET	WskSocket,
__in ULONG			RequestType,
__in ULONG		    ControlCode,
__in ULONG			Level,
__in SIZE_T			InputSize,
__in_opt PVOID		InputBuffer,
__in SIZE_T			OutputSize,
__out_opt PVOID		OutputBuffer,
__out_opt SIZE_T	*OutputSizeReturned
);

NTSTATUS
NTAPI
GetRemoteAddress(
__in PWSK_SOCKET	WskSocket,
__out PSOCKADDR	pRemoteAddress
);

#define HTONS(n)		(((((unsigned short)(n) & 0xFFu  )) << 8) | \
				(((unsigned short) (n) & 0xFF00u) >> 8))

#define TC_PRIO_INTERACTIVE_BULK	1
#define TC_PRIO_INTERACTIVE		1

extern void sock_release(void  *sock);

#define HTON_SHORT(n) (((((unsigned short)(n) & 0xFFu  )) << 8) | \
    (((unsigned short)(n)& 0xFF00u) >> 8))

extern PWSK_SOCKET netlink_server_socket;

extern
NTSTATUS InitWskEvent();

extern
PWSK_SOCKET CreateSocketEvent(
__in ADDRESS_FAMILY	AddressFamily,
__in USHORT			SocketType,
__in ULONG			Protocol,
__in ULONG			Flags
);

extern
NTSTATUS CloseWskEventSocket();

extern
void ReleaseProviderNPI();

extern
NTSTATUS
NTAPI
SetEventCallbacks(
__in PWSK_SOCKET    Socket,
__in LONG			mask
);

extern
NTSTATUS
NTAPI
SetConditionalAccept(
    __in PWSK_SOCKET ListeningSocket,
    __in ULONG       Mode
);

extern
NTSTATUS WSKAPI
AcceptEvent(
_In_  PVOID         SocketContext,
_In_  ULONG         Flags,
_In_  PSOCKADDR     LocalAddress,
_In_  PSOCKADDR     RemoteAddress,
_In_opt_  PWSK_SOCKET AcceptSocket,
_Outptr_result_maybenull_ PVOID *AcceptSocketContext,
_Outptr_result_maybenull_ CONST WSK_CLIENT_CONNECTION_DISPATCH **AcceptSocketDispatch
);
