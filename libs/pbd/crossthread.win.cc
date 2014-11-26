/*
    Copyright (C) 2009 Paul Davis 

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#ifdef PLATFORM_WINDOWS

#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include <csignal> // or signal.h if C code

#include <winsock2.h>
#include <ws2tcpip.h>

#include "pbd/error.h"
#include "pbd/crossthread.h"

using namespace std;
using namespace PBD;
using namespace Glib;

CrossThreadChannel::CrossThreadChannel (bool non_blocking)
	: _ios()
	, _send_socket()
	, _receive_socket()
	, _p_recv_channel(0)
{
	WSADATA	wsaData;

    if(WSAStartup(MAKEWORD(1,1),&wsaData) != 0)
    {
		std::cerr << "CrossThreadChannel::CrossThreadChannel() Winsock initialization failed with error: " << WSAGetLastError() << std::endl;
        return;
    }

	struct sockaddr_in send_address;

	// Create Send Socket
    _send_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    send_address.sin_family = AF_INET;
    send_address.sin_addr.s_addr = INADDR_ANY;
    send_address.sin_port = htons(0);
    int status = bind(_send_socket, (SOCKADDR*)&send_address,   
                  sizeof(send_address));

	if (status != 0) {
		std::cerr << "CrossThreadChannel::CrossThreadChannel() Send socket binding failed with error: " << WSAGetLastError() << std::endl;
        return;
	}

	// make the socket non-blockable if required
	u_long mode = (u_long)non_blocking;
	int otp_result = 0;
	
	otp_result = ioctlsocket(_send_socket, FIONBIO, &mode);
	if (otp_result != NO_ERROR) {
		std::cerr << "CrossThreadChannel::CrossThreadChannel() Send socket cannot be set to non blocking mode with error: " << WSAGetLastError() << std::endl;
	}

	// Create Receive Socket, this socket will be set to unblockable mode by IO channel
    _receive_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    _recv_address.sin_family = AF_INET;
    _recv_address.sin_addr.s_addr = inet_addr("127.0.0.1");
    _recv_address.sin_port = htons(0);
    status = bind(_receive_socket, (SOCKADDR*)&_recv_address, 
                    sizeof(_recv_address));
    
	if (status != 0) {
		std::cerr << "CrossThreadChannel::CrossThreadChannel() Receive socket binding failed with error: " << WSAGetLastError() << std::endl;
        return;
	}

	// make the socket non-blockable if required
	mode = (u_long)non_blocking;
	otp_result = 0;
	
	otp_result = ioctlsocket(_receive_socket, FIONBIO, &mode);
	if (otp_result != NO_ERROR) {
		std::cerr << "CrossThreadChannel::CrossThreadChannel() Receive socket cannot be set to non blocking mode with error: " << WSAGetLastError() << std::endl;
	}

	// get assigned port number for Receive Socket
	int recv_addr_len = sizeof(_recv_address);
    status = getsockname(_receive_socket, (SOCKADDR*)&_recv_address, &recv_addr_len);
    
	if (status != 0) {
		std::cerr << "CrossThreadChannel::CrossThreadChannel() Setting receive socket address to local failed with error: " << WSAGetLastError() << std::endl;
        return;
	}
	
	// construct IOChannel
	_p_recv_channel = g_io_channel_win32_new_socket((gint)_receive_socket);
	
	int flags = G_IO_FLAG_APPEND;
	if (non_blocking) {
		flags |= G_IO_FLAG_NONBLOCK;
	}
	
	GIOStatus g_status = g_io_channel_set_flags(_p_recv_channel, (GIOFlags)flags,
                           NULL);

	if (G_IO_STATUS_NORMAL != g_status ) {
		std::cerr << "CrossThreadChannel::CrossThreadChannel() Cannot set IOChannel flags " << std::endl;
        return;
	}
}

CrossThreadChannel::~CrossThreadChannel ()
{
	/* glibmm hack */
	drop_ios ();
	delete _p_recv_channel;
	closesocket(_send_socket);
	closesocket(_receive_socket);
	WSACleanup();
}

void
CrossThreadChannel::wakeup ()
{
	char c = 0;

	// write one byte to wake up a thread which is listening our IOS	
	sendto(_send_socket, &c, sizeof(c), 0, (SOCKADDR*)&_recv_address, sizeof(_recv_address) );
}

RefPtr<IOSource>
CrossThreadChannel::ios () 
{
	if (!_ios) {
		_ios = IOSource::create (wrap(_p_recv_channel), IOCondition(IO_IN|IO_PRI|IO_ERR|IO_HUP|IO_NVAL));
	}

	return _ios;
}

void
CrossThreadChannel::drop_ios ()
{
	_ios.reset ();
}

void
CrossThreadChannel::drain ()
{
	/* flush the buffer - empty the channel from all requests */
	GError *g_error = 0;
	gchar* buffer;
	gsize read = 0;

	g_io_channel_read_to_end (_p_recv_channel, &buffer, &read, &g_error);
	g_free(buffer);
}


int
CrossThreadChannel::deliver (char msg)
{

	// write one particular byte to wake up the thread which is listening our IOS
	int status = sendto(_send_socket, &msg, sizeof(msg), 0, (SOCKADDR*)&_recv_address, sizeof(_recv_address) );

	if (SOCKET_ERROR  == status) {
		return -1;
	}

	return status;
}

int 
CrossThreadChannel::receive (char& msg)
{
	gsize read = 0;
	GError *g_error = 0;
	
	// fetch the message from the channel.
	GIOStatus g_status = g_io_channel_read_chars (_p_recv_channel, &msg, sizeof(msg), &read, &g_error);

	if (G_IO_STATUS_NORMAL != g_status) {
		read = -1;
	}

	return read;
}

#endif