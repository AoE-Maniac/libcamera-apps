/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2020, Raspberry Pi (Trading) Ltd.
 *
 * net_output.cpp - send output over network.
 */

#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <fcntl.h>

#include "net_output.hpp"

NetOutput::NetOutput(VideoOptions const *options) : Output(options)
{
	char protocol[4];
	int start, end, a, b, c, d, port;
	if (sscanf(options->output.c_str(), "%3s://%n%d.%d.%d.%d%n:%d", protocol, &start, &a, &b, &c, &d, &end, &port) != 6)
		throw std::runtime_error("bad network address " + options->output);
	std::string address = options->output.substr(start, end - start);

	if (strcmp(protocol, "udp") == 0)
	{
		saddr_ = {};
		saddr_.sin_family = AF_INET;
		saddr_.sin_port = htons(port);
		if (inet_aton(address.c_str(), &saddr_.sin_addr) == 0)
			throw std::runtime_error("inet_aton failed for " + address);

		fd_ = socket(AF_INET, SOCK_DGRAM, 0);
		if (fd_ < 0)
			throw std::runtime_error("unable to open udp socket");

		listen_fd_ = -1;
    
		saddr_ptr_ = (const sockaddr *)&saddr_; // sendto needs these for udp
		sockaddr_in_size_ = sizeof(sockaddr_in);
	}
	else if (strcmp(protocol, "tcp") == 0)
	{
		// WARNING: I've not actually tried this yet...
		if (options->listen)
		{
			// We are the server.
			listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
			if (listen_fd_ < 0)
				throw std::runtime_error("unable to open listen socket");

			sockaddr_in server_saddr = {};
			server_saddr.sin_family = AF_INET;
			server_saddr.sin_addr.s_addr = INADDR_ANY;
			server_saddr.sin_port = htons(port);

			int enable = 1;
			if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0)
				throw std::runtime_error("failed to setsockopt listen socket");

			int non_blocking = 1;
			if (fcntl(listen_fd_, F_SETFL, O_NONBLOCK, non_blocking) < 0)
				throw std::runtime_error("failed to fcntl listen socket");

			if (bind(listen_fd_, (struct sockaddr *)&server_saddr, sizeof(server_saddr)) < 0)
				throw std::runtime_error("failed to bind listen socket");
			listen(listen_fd_, 1);

			LOG(2, "Waiting for client to connect...");
			fd_ = -1;
		}
		else
		{
			// We are a client.
			saddr_ = {};
			saddr_.sin_family = AF_INET;
			saddr_.sin_port = htons(port);
			if (inet_aton(address.c_str(), &saddr_.sin_addr) == 0)
				throw std::runtime_error("inet_aton failed for " + address);

			fd_ = socket(AF_INET, SOCK_STREAM, 0);
			if (fd_ < 0)
				throw std::runtime_error("unable to open client socket");

			LOG(2, "Connecting to server...");
			if (connect(fd_, (struct sockaddr *)&saddr_, sizeof(sockaddr_in)) < 0)
				throw std::runtime_error("connect to server failed");
			LOG(2, "Connected");

			listen_fd_ = -1;
		}

		saddr_ptr_ = NULL; // sendto doesn't want these for tcp
		sockaddr_in_size_ = 0;
	}
	else
		throw std::runtime_error("unrecognised network protocol " + options->output);
}

NetOutput::~NetOutput()
{
	if (fd_ >= 0)
		close(fd_);
	if (listen_fd_ >= 0)
		close(listen_fd_);
}

// Maximum size that sendto will accept.
constexpr size_t MAX_UDP_SIZE = 65507;

void NetOutput::outputBuffer(void *mem, size_t size, int64_t /*timestamp_us*/, uint32_t /*flags*/)
{
	if (listen_fd_ >= 0 && fd_ < 0)
	{
		int fd = accept(listen_fd_, (struct sockaddr *)&saddr_, &sockaddr_in_size_);
		if (fd >= 0)
		{
			LOG(2, "Client connection accepted");
			fd_ = fd;
      
			int enable = 1;
			if (setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable)) < 0)
				throw std::runtime_error("failed to setsockopt client socket");
		}
	}

	LOG(2, "NetOutput: output buffer " << mem << " size " << size);
	size_t max_size = saddr_ptr_ ? MAX_UDP_SIZE : size;
	for (uint8_t *ptr = (uint8_t *)mem; size;)
	{
		size_t bytes_to_send = std::min(size, max_size);
		if (sendto(fd_, ptr, bytes_to_send, 0, saddr_ptr_, sockaddr_in_size_) < 0)
		{
			if (listen_fd_)
				fd_ = -1;
			else
				throw std::runtime_error("failed to send data on socket");
		}
		ptr += bytes_to_send;
		size -= bytes_to_send;
	}
}
