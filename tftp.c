/**
 * nmrpflash - Netgear Unbrick Utility
 * Copyright (C) 2016 Joseph Lehner <joseph.c.lehner@gmail.com>
 *
 * nmrpflash is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * nmrpflash is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with nmrpflash.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include "nmrpd.h"

#ifndef O_BINARY
#define O_BINARY 0
#endif

#define TFTP_PKT_SIZE 516

static const char *opcode_names[] = {
	"RRQ", "WRQ", "DATA", "ACK", "ERR"
};

enum tftp_opcode {
	RRQ  = 1,
	WRQ  = 2,
	DATA = 3,
	ACK  = 4,
	ERR  = 5
};

static bool is_netascii(const char *str)
{
	uint8_t *p = (uint8_t*)str;

	for (; *p; ++p) {
		if (*p < 0x20 || *p > 0x7f) {
			return false;
		}
	}

	return true;
}

static inline void pkt_mknum(char *pkt, uint16_t n)
{
	*(uint16_t*)pkt = htons(n);
}

static inline uint16_t pkt_num(char *pkt)
{
	return ntohs(*(uint16_t*)pkt);
}

static void pkt_mkwrq(char *pkt, const char *filename)
{
	size_t len = 2;

	filename = leafname(filename);
	if (!tftp_is_valid_filename(filename)) {
		fprintf(stderr, "Overlong/illegal filename; using 'firmware'.\n");
		filename = "firmware";
	} else if (!strcmp(filename, "-")) {
		filename = "firmware";
	}

	pkt_mknum(pkt, WRQ);

	strcpy(pkt + len, filename);
	len += strlen(filename) + 1;
	strcpy(pkt + len, "octet");
}

static inline void pkt_print(char *pkt, FILE *fp)
{
	uint16_t opcode = pkt_num(pkt);
	if (!opcode || opcode > ERR) {
		fprintf(fp, "(%d)", opcode);
	} else {
		fprintf(fp, "%s", opcode_names[opcode - 1]);
		if (opcode == ACK || opcode == DATA) {
			fprintf(fp, "(%d)", pkt_num(pkt + 2));
		} else if (opcode == WRQ || opcode == RRQ) {
			fprintf(fp, "(%s, %s)", pkt + 2, pkt + 2 + strlen(pkt + 2) + 1);
		}
	}
}

static ssize_t tftp_recvfrom(int sock, char *pkt, uint16_t* port,
		unsigned timeout)
{
	ssize_t len;
	struct sockaddr_in src;
#ifndef NMRPFLASH_WINDOWS
	socklen_t alen;
#else
	int alen;
#endif

	len = select_fd(sock, timeout);
	if (len < 0) {
		return -1;
	} else if (!len) {
		return 0;
	}

	alen = sizeof(src);
	len = recvfrom(sock, pkt, TFTP_PKT_SIZE, 0, (struct sockaddr*)&src, &alen);
	if (len < 0) {
		sock_perror("recvfrom");
		return -1;
	}

	*port = ntohs(src.sin_port);

	uint16_t opcode = pkt_num(pkt);

	if (opcode == ERR) {
		fprintf(stderr, "Error (%d): %.511s\n", pkt_num(pkt + 2), pkt + 4);
		return -1;
	} else if (isprint(pkt[0])) {
		/* In case of a firmware checksum error, the EX2700 I've tested this
		 * on sends a raw UDP packet containing just an error message starting
		 * at offset 0. The limit of 32 chars is arbitrary.
		 */
		fprintf(stderr, "Error: %.32s\n", pkt);
		return -2;
	} else if (!opcode || opcode > ERR) {
		fprintf(stderr, "Received invalid packet: ");
		pkt_print(pkt, stderr);
		fprintf(stderr, ".\n");
		return -1;
	}

	if (verbosity > 2) {
		printf(">> ");
		pkt_print(pkt, stdout);
		printf("\n");
	}

	return len;
}

static ssize_t tftp_sendto(int sock, char *pkt, size_t len,
		struct sockaddr_in *dst)
{
	ssize_t sent;

	switch (pkt_num(pkt)) {
		case RRQ:
		case WRQ:
			len = 2 + strlen(pkt + 2) + 1;
			len += strlen(pkt + len) + 1;
			break;
		case DATA:
			len += 4;
			break;
		case ACK:
			len = 4;
			break;
		case ERR:
			len = 4 + strlen(pkt + 4);
			break;
		default:
			fprintf(stderr, "Attempted to send invalid packet ");
			pkt_print(pkt, stderr);
			fprintf(stderr, "; this is a bug!\n");
			return -1;
	}

	if (verbosity > 2) {
		printf("<< ");
		pkt_print(pkt, stdout);
		printf("\n");
	}

	sent = sendto(sock, pkt, len, 0, (struct sockaddr*)dst, sizeof(*dst));
	if (sent < 0) {
		sock_perror("sendto");
	}

	return sent;
}

const char *leafname(const char *path)
{
	const char *slash, *bslash;

	slash = strrchr(path, '/');
	bslash = strrchr(path, '\\');

	if (slash && bslash) {
		path = 1 + (slash > bslash ? slash : bslash);
	} else if (slash) {
		path = 1 + slash;
	} else if (bslash) {
		path = 1 + bslash;
	}

	return path;
}

#ifdef NMRPFLASH_WINDOWS
void sock_perror(const char *msg)
{
	win_perror2(msg, WSAGetLastError());
}
#endif

inline bool tftp_is_valid_filename(const char *filename)
{
	return strlen(filename) <= 500 && is_netascii(filename);
}

int tftp_put(struct nmrpd_args *args)
{
	struct sockaddr_in addr;
	uint16_t block, port;
	ssize_t len, last_len;
	int fd, sock, ret, timeout, errors, ackblock;
	char rx[TFTP_PKT_SIZE], tx[TFTP_PKT_SIZE];
	const char *file_remote = args->file_remote;

	sock = -1;
	ret = -1;
	fd = -1;

	if (g_interrupted) {
		goto cleanup;
	}

	if (!strcmp(args->file_local, "-")) {
		fd = STDIN_FILENO;
		if (!file_remote) {
			file_remote = "firmware";
		}
	} else {
		fd = open(args->file_local, O_RDONLY | O_BINARY);
		if (fd < 0) {
			xperror("open");
			ret = fd;
			goto cleanup;
		} else if (!file_remote) {
			file_remote = args->file_local;
		}
	}

	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		sock_perror("socket");
		ret = sock;
		goto cleanup;
	}

	memset(&addr, 0, sizeof(addr));

	addr.sin_family = AF_INET;

	if (args->ipaddr_intf) {
		if ((addr.sin_addr.s_addr = inet_addr(args->ipaddr_intf)) == INADDR_NONE) {
			xperror("inet_addr");
			goto cleanup;
		}

		if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
			sock_perror("bind");
			goto cleanup;
		}
	}

	if ((addr.sin_addr.s_addr = inet_addr(args->ipaddr)) == INADDR_NONE) {
		xperror("inet_addr");
		goto cleanup;
	}
	addr.sin_port = htons(args->port);

	block = 0;
	last_len = -1;
	len = 0;
	errors = 0;
	/* Not really, but this way the loop sends our WRQ before receiving */
	timeout = 1;

	pkt_mkwrq(tx, file_remote);

	while (!g_interrupted) {
		if (!timeout && pkt_num(rx) == ACK) {
			ackblock = pkt_num(rx + 2);
		} else {
			ackblock = -1;
		}

		if (timeout || ackblock == block) {
			if (!timeout) {
				++block;
				pkt_mknum(tx, DATA);
				pkt_mknum(tx + 2, block);
				len = read(fd, tx + 4, 512);
				if (len < 0) {
					xperror("read");
					ret = len;
					goto cleanup;
				} else if (!len) {
					if (last_len != 512 && last_len != -1) {
						break;
					}
				}

				last_len = len;
			}

			ret = tftp_sendto(sock, tx, len, &addr);
			if (ret < 0) {
				goto cleanup;
			}
		} else if (pkt_num(rx) != ACK || ackblock > block) {
			if (verbosity) {
				fprintf(stderr, "Expected ACK(%d), got ", block);
				pkt_print(rx, stderr);
				fprintf(stderr, ".\n");
			}

			if (ackblock != -1 && ++errors > 5) {
				fprintf(stderr, "Protocol error; bailing out.\n");
				ret = -1;
				goto cleanup;
			}
		}

		ret = tftp_recvfrom(sock, rx, &port, args->rx_timeout);
		if (ret < 0) {
			goto cleanup;
		} else if (!ret) {
			if (++timeout < 5 || (!block && timeout < 10)) {
				continue;
			} else if (block) {
				fprintf(stderr, "Timeout while waiting for ACK(%d).\n", block);
			} else {
				fprintf(stderr, "Timeout while waiting for initial reply.\n");
			}
			ret = -1;
			goto cleanup;
		} else {
			timeout = 0;
			ret = 0;

			if (!block && port != args->port) {
				if (verbosity > 1) {
					printf("Switching to port %d\n", port);
				}
				addr.sin_port = htons(port);
			}
		}
	}

	ret = !g_interrupted ? 0 : -1;

cleanup:
	if (fd >= 0) {
		close(fd);
	}

	if (sock >= 0) {
#ifndef NMRPFLASH_WINDOWS
		shutdown(sock, SHUT_RDWR);
		close(sock);
#else
		shutdown(sock, SD_BOTH);
		closesocket(sock);
#endif
	}

	return ret;
}
