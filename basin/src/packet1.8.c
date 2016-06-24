/*
 * packet1.8.c
 *
 *  Created on: Jun 23, 2016
 *      Author: root
 */

#include "globals.h"

#ifdef MC_VERSION_1_8_9

#include "network.h"
#include "packet1.8.h"
#include <zlib.h>
#include <errno.h>
#include "xstring.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "nbt.h"
#include <zlib.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include "accept.h"
#include "util.h"

ssize_t readPacket(struct conn* conn, unsigned char* buf, size_t buflen, struct packet* packet) {
	void* pktbuf = buf;
	int32_t pktlen = buflen;
	int tf = 0;
	if (conn->comp >= 0) {
		int32_t dl = 0;
		//printf("reading pl\n");
		//printf("pl = %i\n", pl);
		int rx = readVarInt(&dl, pktbuf, pktlen);
		if(rx == 0) return -1;
		pktlen -= rx;
		pktbuf += rx;
		//printf("dl = %i\n", dl);
		if(dl > 0 && pktlen > 0) {
			pktlen = dl;
			void* decmpbuf = malloc(dl);
			//
			z_stream strm;
			strm.zalloc = Z_NULL;
			strm.zfree = Z_NULL;
			strm.opaque = Z_NULL;
			int dr = 0;
			if ((dr = inflateInit(&strm)) != Z_OK) {
				free(decmpbuf);
				printf("Compression initialization error!\n");
				return -1;
			}
			strm.avail_in = pktlen;
			strm.next_in = pktbuf;
			strm.avail_out = dl;
			strm.next_out = decmpbuf;
			do {
				dr = inflate(&strm, Z_FINISH);
				if (dr == Z_STREAM_ERROR) {
					free(decmpbuf);
					printf("Compression Read Error\n");
					return -1;
				}
				strm.avail_out = pktlen - strm.total_out;
				strm.next_out = decmpbuf + strm.total_out;
			}while (strm.avail_in > 0 || strm.total_out < pktlen);
			inflateEnd(&strm);
			pktbuf = decmpbuf;
			pktlen = dl;
			tf = 1;
		}
	}
	if (pktbuf == NULL) return 0;
	unsigned char* pbuf = (unsigned char*) pktbuf;
	size_t ps = pktlen;
	int32_t id = 0;
	size_t t = readVarInt(&id, pbuf, ps);
	//printf("pktid = %02X\n", id);
	pbuf += t;
	ps -= t;
	packet->id = id;
	if (conn->state == STATE_PLAY) {
		if (id == PKT_PLAY_CLIENT_KEEPALIVE) {

		}
	} else if (conn->state == STATE_HANDSHAKE) {
		if(id == PKT_HANDSHAKE_CLIENT_HANDSHAKE) {
			int rx = readVarInt(&packet->data.handshake_client.handshake.protocol_version, pbuf, ps);
			if(rx == 0) goto rer;
			pbuf += rx;
			ps -= rx;
			rx = readString(&packet->data.handshake_client.handshake.ip, pbuf, ps);
			if(rx == 0) goto rer;
			pbuf += rx;
			ps -= rx;
			if(ps < 2) goto rer;
			memcpy(&packet->data.handshake_client.handshake.port, pbuf, 2);
			pbuf += 2;
			ps -= 2;
			swapEndian(&packet->data.handshake_client.handshake.port, 2);
			rx = readVarInt(&packet->data.handshake_client.handshake.state, pbuf, ps);
			if(rx == 0) goto rer;
			pbuf += rx;
			ps -= rx;
		}
	} else if (conn->state == STATE_LOGIN) {
		if (id == PKT_LOGIN_CLIENT_LOGINSTART) {
			//pi += writeString(packet->data.login_client.loginstart.name, pktbuf + pi, ps - pi);
		} else if (id == PKT_LOGIN_CLIENT_ENCRYPTIONRESPONSE) {
			//ps = packet->data.login_client.encryptionresponse.sharedSecret_size + packet->data.login_client.encryptionresponse.verifyToken_size + 64;
			//pktbuf = xrealloc(pktbuf, ps);
			//pi += writeVarInt(packet->data.login_client.encryptionresponse.sharedSecret_size, pktbuf + pi);
			//memcpy(pktbuf + pi, packet->data.login_client.encryptionresponse.sharedSecret, packet->data.login_client.encryptionresponse.sharedSecret_size);
			//pi += packet->data.login_client.encryptionresponse.sharedSecret_size;
			//pi += writeVarInt(packet->data.login_client.encryptionresponse.verifyToken_size, pktbuf + pi);
			//memcpy(pktbuf + pi, packet->data.login_client.encryptionresponse.verifyToken, packet->data.login_client.encryptionresponse.verifyToken_size);
			//pi += packet->data.login_client.encryptionresponse.verifyToken_size;
		}
	} else if (conn->state == STATE_STATUS) {
		if(id == PKT_STATUS_CLIENT_PING) {
			if(ps < 8) goto rer;
			memcpy(&packet->data.status_client.ping.payload, pbuf, 8);
			pbuf += 8;
			ps -= 8;
		}
	}
	goto rx;
	rer:;
	return -1;
	rx:;
	if(tf) xfree(pktbuf);
	return buflen;
}

ssize_t writePacket(struct conn* conn, struct packet* packet) {
	unsigned char* pktbuf = xmalloc(1024); // TODO free
	unsigned char* pbuf = pktbuf;
	size_t ps = 1024;
	size_t pi = 0;
	int32_t id = packet->id;
	pi += writeVarInt(id, pktbuf + pi);
	if (conn->state == STATE_PLAY) {
		if (packet->id == PKT_PLAY_SERVER_KEEPALIVE) {

		}
	} else if (conn->state == STATE_LOGIN) {
		if (id == PKT_LOGIN_SERVER_DISCONNECT) {
			//int rx = readString(&packet->data.login_server.disconnect.reason, pbuf, ps);
			//pbuf += rx;
			//ps -= rx;
		} else if (id == PKT_LOGIN_SERVER_ENCRYPTIONREQUEST) {
			//int rx = readString(&packet->data.login_server.encryptionrequest.serverID, pbuf, ps);
			//pbuf += rx;
			//ps -= rx;
			//rx = readVarInt(&packet->data.login_server.encryptionrequest.publicKey_size, pbuf, ps);
			//pbuf += rx;
			//ps -= rx;
			//if (ps < packet->data.login_server.encryptionrequest.publicKey_size) {
			//	free(pktbuf);
			//	free(packet->data.login_server.encryptionrequest.serverID);
			//	return -1;
			//}
			//packet->data.login_server.encryptionrequest.publicKey = malloc(packet->data.login_server.encryptionrequest.publicKey_size);
//			memcpy(packet->data.login_server.encryptionrequest.publicKey, pbuf, packet->data.login_server.encryptionrequest.publicKey_size);
//			rx = readVarInt(&packet->data.login_server.encryptionrequest.verifyToken_size, pbuf, ps);
//			pbuf += rx;
//			ps -= rx;
//			if (ps < packet->data.login_server.encryptionrequest.verifyToken_size) {
//				free(pktbuf);
//				free(packet->data.login_server.encryptionrequest.serverID);
//				free(packet->data.login_server.encryptionrequest.publicKey);
//				return -1;
//			}
//			packet->data.login_server.encryptionrequest.verifyToken = malloc(packet->data.login_server.encryptionrequest.verifyToken_size);
//			memcpy(packet->data.login_server.encryptionrequest.verifyToken, pbuf, packet->data.login_server.encryptionrequest.verifyToken_size);
		} else if (id == PKT_LOGIN_SERVER_LOGINSUCCESS) {
//			int rx = readString(&packet->data.login_server.loginsuccess.UUID, pbuf, ps);
//			pbuf += rx;
//			ps -= rx;
//			rx = readString(&packet->data.login_server.loginsuccess.username, pbuf, ps);
//			pbuf += rx;
//			ps -= rx;
		} else if (id == PKT_LOGIN_SERVER_SETCOMPRESSION) {
//			int rx = readVarInt(&packet->data.login_server.setcompression.threshold, pbuf, ps);
//			pbuf += rx;
//			ps -= rx;
		}
	} else if (conn->state == STATE_STATUS) {
		if (id == PKT_STATUS_SERVER_RESPONSE) {
			pi += writeString(packet->data.status_server.response.json, pktbuf + pi, ps - pi);
		} else if (id == PKT_STATUS_SERVER_PONG) {
			memcpy(&packet->data.status_server.pong.payload, pktbuf + pi, 8);
			pi += 8;
		}
	}
	int fpll = getVarIntSize(pi);
	void* wrt = NULL;
	size_t wrt_s = 0;
	int frp = 0;
	unsigned char prep[10];
	uint8_t preps = 0;
	if (conn->comp >= 0 && (pi + fpll > conn->comp + 1) && (pi + fpll) <= 2097152) {
		frp = 1;
		z_stream strm;
		strm.zalloc = Z_NULL;
		strm.zfree = Z_NULL;
		strm.opaque = Z_NULL;
		int dr = 0;
		if ((dr = deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY)) != Z_OK) { // TODO: configurable level?
			return -1;
		}
		strm.avail_in = pi;
		strm.next_in = pktbuf;
		void* cdata = malloc(16384);
		size_t ts = 0;
		size_t cc = 16384;
		strm.avail_out = cc - ts;
		strm.next_out = cdata + ts;
		do {
			dr = deflate(&strm, Z_FINISH);
			ts = strm.total_out;
			if (ts >= cc) {
				cc = ts + 16384;
				cdata = realloc(cdata, cc);
			}
			if (dr == Z_STREAM_ERROR) {
				free(cdata);
				return -1;
			}
			strm.avail_out = cc - ts;
			strm.next_out = cdata + ts;
		}while (strm.avail_out == 0);
		deflateEnd(&strm);
		cdata = xrealloc(cdata, ts); // shrink
		preps += writeVarInt(ts + getVarIntSize(pi), prep + preps);
		preps += writeVarInt(pi, prep + preps);
		wrt = cdata;
		wrt_s = ts;
	} else if (conn->comp >= 0) {
		preps += writeVarInt(pi + getVarIntSize(0), prep + preps);
		preps += writeVarInt(0, prep + preps);
		wrt = pktbuf;
		wrt_s = pi;
	} else {
		preps += writeVarInt(pi, prep + preps);
		wrt = pktbuf;
		wrt_s = pi;
	}
//TODO: encrypt
	if(conn->writeBuffer == NULL) {
		conn->writeBuffer = xmalloc(preps);
		memcpy(conn->writeBuffer, prep, preps);
		conn->writeBuffer_size = preps;
	} else {
		conn->writeBuffer = xrealloc(conn->writeBuffer, conn->writeBuffer_size + preps);
		memcpy(conn->writeBuffer + conn->writeBuffer_size, prep, preps);
		conn->writeBuffer_size += preps;
	}
	conn->writeBuffer = xrealloc(conn->writeBuffer, conn->writeBuffer_size + wrt_s);
	memcpy(conn->writeBuffer + conn->writeBuffer_size, wrt, wrt_s);
	conn->writeBuffer_size += wrt_s;
	if (frp) xfree(wrt);
	xfree(pktbuf);
	//printf("write success\n");
	return wrt_s;
}

#endif
