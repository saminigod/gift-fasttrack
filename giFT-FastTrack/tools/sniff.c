/*
 * $Id: sniff.c,v 1.5 2004/03/10 19:03:36 mkern Exp $
 *
 * Based on printall.c from libnids/samples, which is
 * copyright (c) 1999 Rafal Wojtczuk <nergal@avet.com.pl>. All rights reserved.
*/

/* 
 * To run (as root):
 * ./sniff [interface]
 *
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/in_systm.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>

#include <nids.h>

#include "crypt/fst_crypt.h"

extern char *nids_warnings[];

#define EVIL /* mmm... evil... */

enum {
	STATE_CLIENT_KEY,
	STATE_SERVER_KEY,
	STATE_SERVER_NETNAME,
	STATE_CLIENT_NETNAME,
	STATE_ESTABLISHED,
	STATE_UNSUPPORTED,
	STATE_HTTP,
	STATE_PUSH
};

static int id=0;
static time_t last_time=0;

struct session {
	FSTCipher *in_cipher;
        FSTCipher *out_cipher;
        unsigned int in_xinu;
        unsigned int out_xinu;
	int state;
	unsigned long rand;
	int id;
	int push;
};

#define INT(x) ntohl(*((unsigned long*)(data+x)));

#define int_ntoa(x)	inet_ntoa(*((struct in_addr *)&x))

char *
adres (struct tuple4 *addr, int id)
{
  static char buf[256];
  strcpy (buf, int_ntoa (addr->saddr));
  sprintf (buf + strlen (buf), ":%d -> ", addr->source);
  strcat (buf, int_ntoa (addr->daddr));
  sprintf (buf + strlen (buf), ":%d", addr->dest);
  if (id)
	  sprintf (buf + strlen (buf), " [%d]", id);
  return buf;
}

void print_bin_data(unsigned char * data, int len)
{
        int i;
        int i2;
        int i2_end;

//      printf("data len %d\n", data_len);

        for (i2 = 0; i2 < len; i2 = i2 + 16)
        {
                i2_end = (i2 + 16 > len) ? len: i2 + 16;
                for (i = i2; i < i2_end; i++)
                        if (isprint(data[i]))
                                fprintf(stderr, "%c", data[i]);
                        else
                        fprintf(stderr, ".");
                for ( i = i2_end ; i < i2 + 16; i++)
                        fprintf(stderr, " ");
                fprintf(stderr, " | ");
                for (i = i2; i < i2_end; i++)
                        fprintf(stderr, "%02x ", data[i]);
                fprintf(stderr, "\n");
        }
}

int verify_port (int p)
{
	return !(p==1217 ||
		 p==1216 ||
		 p==1215 ||
		 p>5000 ||
		 (p<1000 && p!=80));
}

int verify_ip (struct tuple4 *addr)
{
	unsigned long kazaa_host = inet_addr ("192.168.0.222");
	
	return (addr->saddr == kazaa_host ||
	        addr->daddr == kazaa_host);
}

void tcp_callback (struct tcp_stream *tcp, struct session **conn)
{
	char buf[32];

	struct session *c=*conn;

	switch (tcp->nids_state) {
	case NIDS_JUST_EST:
	{
		if (!verify_port (tcp->addr.dest) && !verify_port (tcp->addr.source))
			return;

		if (!verify_ip (&tcp->addr))
			return;

		c=malloc(sizeof(struct session));
		if (!c)
			abort();

		c->in_xinu=c->out_xinu=0x51;
		c->in_cipher=c->out_cipher=NULL;
		c->state=STATE_CLIENT_KEY;
		c->id=0;
		c->push=0;
		
		*conn=c;
		
		tcp->client.collect++; // we want data received by a client
		tcp->server.collect++; // and by a server, too

		//fprintf (stderr, "%s established\n", buf);
		return;
	}
	case NIDS_CLOSE:
		// connection has been closed normally
		if (c && c->id) {
			fprintf (stderr, "[%d] closing\n", c->id);
			free(c->in_cipher);
			free(c->out_cipher);
			free(c);
			*conn=NULL;
		}
		return;
	case NIDS_RESET:
		// connection has been closed by RST
		if (c && c->id) {
			fprintf (stderr, "[%d] reset\n", c->id);
			free(c->in_cipher);
			free(c->out_cipher);
			free(c);
			*conn=NULL;
		}
		break;
	case NIDS_DATA:
	{
		// new data has arrived; gotta determine in what direction
		// and if it's urgent or not

		struct half_stream *hlf;
		int done=0;
		int len;
		unsigned char *data;
		int this_len;
		int read;
		int done_read=0;
		int server;

		if (tcp->client.count_new)
			hlf = &tcp->client; 
		else
			hlf = &tcp->server;
		server = !!tcp->client.count_new;

		len = hlf->count - hlf->offset;
		this_len=hlf->count_new;
		data=hlf->data;

		if (c->state != STATE_UNSUPPORTED) {
			time_t this_time=time(NULL);
			if (this_time-last_time>120) {
				/* print the time every 2 minutes */
				fprintf(stderr, "%s", ctime(&this_time));
				last_time=this_time;
			}
		}

		do {
			sprintf (buf, "[%d %s] ",c->id, server?"<-":"->");
			read=0;
		switch (c->state) {
		case STATE_CLIENT_KEY:
			fprintf(stderr, "%s\n", adres (&tcp->addr, c->id=++id));
			sprintf (buf, "[%d %s] ",c->id, server?"<-":"->");

			if (len>=5 && !server) {
				/* first check if this looks like a transfer */
				if (!memcmp(data, "GET ",4) ||
				     !memcmp(data, "HEAD ",5)) {
					/* ignore HTTP stuff on port 80 */
					if (tcp->addr.dest==80)
						c->state=STATE_UNSUPPORTED;
					else
						c->state=STATE_HTTP;
					break;
				}
				if (!memcmp(data, "GIVE ",5)) {
					c->state=STATE_PUSH;
					break;
				}
			}

			if (len>=12 && !server) {
				unsigned long rand, seed, enc_type;

				rand=INT(0);
				seed=INT(4);
				enc_type=INT(8);
				
				c->rand=rand;

				c->out_cipher=fst_cipher_create();

				//for(i=0;i<len;i++)
				//	fprintf(stderr, "%02x ",data[i]);
				c->out_cipher->enc_type=fst_cipher_mangle_enc_type(seed, enc_type);
				c->out_cipher->seed=seed;
				
				if (c->out_cipher->enc_type & ~0xff) {
					fprintf(stderr, "%s ???\n", buf);
					print_bin_data (data, len);
					c->state=STATE_UNSUPPORTED;
					done=1;
				} else {
					read=12;
					c->state++;
				}
			} else
				done=1;

			break;

		case STATE_SERVER_KEY:
			if (len>=8 && server) {
				unsigned long seed, enc_type;
				seed=INT(0);
				enc_type=INT(4);
				c->out_cipher->seed^=seed;
				c->in_cipher=fst_cipher_create();
				enc_type=fst_cipher_mangle_enc_type(seed, enc_type);
				/* FIXME: if incoming != outgoing,  then what? OR the two?
				   currently we just use incoming, which seems to
                                   work when out=29,in=a9 */

				if (fst_cipher_init(c->out_cipher, c->out_cipher->seed, enc_type) &&
				    fst_cipher_init(c->in_cipher, seed, enc_type)) {
					fprintf(stderr, "[%d] in: %x, out %x [rand %04lx]\n", c->id, c->in_cipher->enc_type,c->out_cipher->enc_type, c->rand);
					c->state++;
				} else {
					fprintf(stderr,"[%d] init failed (%x,%x)\n", c->id, c->in_cipher->enc_type,c->out_cipher->enc_type);
#ifdef EVIL
					/* ensure we can eavesdrop on all traffic, by evilly
					   resetting the connection if decryption fails :) */
					nids_killtcp (tcp);
#endif
					c->state=STATE_UNSUPPORTED;
				}
				read=8;
			} else
				done=1;

			break;
		case STATE_SERVER_NETNAME:
			if (server) {
				//fprintf(stderr, "[<-] decrypting %d of %d bytes\n", this_len, len);
				fst_cipher_crypt (c->in_cipher, data + len - this_len, this_len);
				this_len=0;

				if (memchr(data, 0, len)) {
					fprintf(stderr, "%s network name: '%s'\n",buf, data);
					read=strlen(data)+1;
					c->state++;
				} else {
					fprintf(stderr, "buffering partially decrypted netname (%d)\n", len);
					done=1;
				}
			} else
				done=1;
			break;

		case STATE_CLIENT_NETNAME:
			if (!server) {
				//fprintf(stderr, "[->] decrypting %d of %d bytes\n", this_len, len);
				fst_cipher_crypt (c->out_cipher, data + len - this_len, this_len);
				this_len=0;

				if (memchr(data, 0, len)) {
					fprintf(stderr, "%s network name: '%s'\n",buf, data);
					read=strlen(data)+1;
					c->state++;
				} else {
					fprintf(stderr, "buffering partially decrypted netname (%d)\n", len);
					done=1;
				}
			} else
				done=1;
			break;

		case STATE_ESTABLISHED:
			if (this_len>0) {
				//fprintf(stderr, "%s decrypting %d of %d bytes\n", buf, this_len, len);
				fst_cipher_crypt (server? c->in_cipher : c->out_cipher, data + len - this_len, this_len);
				this_len=0;
			}

			switch (*data) {
			case 0x50:
				fprintf(stderr, "%s ping?\n", buf);
				read=1;
				break;
			case 0x52:
				fprintf(stderr, "%s pong\n", buf);
				read=1;
				break;
			case 0x4B:
				if (len>=5) {
					unsigned int *xinu = server?&c->in_xinu:&c->out_xinu;
					int xtype = *xinu % 3;
					int type, msglen;

#if 0
					if (data[1]==0x80) {
						if (data[3]==0) {
							fprintf(stderr, "%s guessing no_x xtype 2 (was %d)\n", buf, xtype);
							xtype=2;
						} else if (data[2]==0) {
							fprintf(stderr, "%s guessing no_x xtype 1 (was %d)\n", buf, xtype);
							xtype=1;
						}
					} else if (data[2]==0x80) {
						fprintf(stderr, "%s guessing no_x xtype 0 (was %d)\n", buf, xtype);
						xtype=0;
					}
#endif

					switch(xtype) {
					case 0:
						type=(data[2]<<8)+data[1];
						msglen=(data[3]<<8)+data[4];
						break;
					case 1:
						type=(data[1]<<8)+data[3];
						msglen=(data[2]<<8)+data[4];
						break;
					case 2:
						type=(data[1]<<8)+data[4];
						msglen=(data[3]<<8)+data[2];
						break;
					}

					if (len>=msglen+5) {
						read=msglen+5;
						fprintf(stderr, "%s message type %02x, len %d\n", buf, type, msglen);
						*xinu ^= ~(type + msglen);
						print_bin_data(data+5,msglen);
					} else {
						fprintf(stderr, "%s (%02x %d/%d... [%d: %02x %02x %02x %02x])\n", buf, type, len, msglen, xtype, data[1], data[2], data[3], data[4]);
						done=1;
					}
				} else
					done=1;
				break;
			default:
				fprintf(stderr, "%s unknown packet type %x [len %d]\n", buf, *data, len);
				print_bin_data(data, len);
				c->state=STATE_UNSUPPORTED;
				break;
			}
			break;
		case STATE_HTTP:
			if (!server ^ c->push) {
				fprintf(stderr, "%s HTTP request:\n", buf);
				fwrite(data, len, 1, stderr);
				read=len;
				done=1;
			} else {
				int i;
				for(i=0;i<len;i++) {
					if ((i<=len-4 && !memcmp(data+i, "\r\n\r\n", 4)) ||
					    (i<=len-2 && !memcmp(data+i, "\n\n", 2))) {
						fprintf(stderr, "%s HTTP reply:\n", buf);
						fwrite(data, i+2, 1, stderr);
						c->state=STATE_UNSUPPORTED; /* ignore the body */
						read=i+2;
						done=1;
						break;
					}
				}
				done=1;
			}	
			break;
		case STATE_PUSH:
			fprintf(stderr, "%s PUSH\n", buf);
			print_bin_data (data, len);
			c->push=1;
			read=len;
			done=1;
			c->state=STATE_HTTP;
			break;
		default:
			/* fprintf(stderr, "%s %d skipped\n", buf,len); */
			done_read=-1;
			done=1;
		}
		this_len-=read;
		len-=read;
		data+=read;
		done_read+=read;
		if (len<0)
			fprintf(stderr, "WARNING: len=%d, this_len=%d\n", len, this_len);
		} while (!done && len>0);
		if (done_read>=0)
			nids_discard(tcp, done_read);

	}
	}
	return;
}

void udp_callback(struct tuple4 *addr, char *buf, int len, void* iph)
{
	unsigned char type;
	
	if (!verify_port (addr->dest) && !verify_port (addr->source))
		return;

	if (!verify_ip (addr))
		return;

	/* ignore dns */
	if (addr->dest == 53 || addr->source == 53)
		return;

	type = ((unsigned char*)buf)[0];

	switch (type)
	{
	case 0x27: /* ping */
	{
		unsigned int enc_type = ntohl(*((unsigned int*)(buf+1)));
		unsigned char unknown1 = *((unsigned char*)(buf+5));
		char netname[64];
		strncpy (netname, buf+6, 64);
		netname[63] = 0;

		fprintf(stderr, "%s [UDP] (PING, len %d)\n", adres (addr, 0), len);
		fprintf(stderr, "    enc_type: 0x%02X, unk1: 0x%02X, netname: %s\n",
		        enc_type, unknown1, netname);
		break;
	}

	case 0x28: /* supernode pong */
	{
		unsigned int enc_type = ntohl(*((unsigned int*)(buf+1)));
		unsigned char unknown1 = *((unsigned char*)(buf+5));
		unsigned char unknown2 = *((unsigned char*)(buf+6));
		unsigned char unknown3 = *((unsigned char*)(buf+7));
		unsigned char unknown4 = *((unsigned char*)(buf+8));
		unsigned char load = *((unsigned char*)(buf+9));
		unsigned char unknown5 = *((unsigned char*)(buf+10));

		fprintf(stderr, "%s [UDP] (PONG, len %d)\n", adres (addr, 0), len);
		fprintf(stderr, "    enc_type: 0x%02X, unk1: 0x%02X, unk2: 0x%02X "
		        "unk3: 0x%02X, unk4: 0x%02X, load: %d, unk5: %d\n",
		        enc_type, unknown1, unknown2, unknown3, unknown4, load, unknown5);
		break;
	}

	case 0x29: /* client pong */
	{
		fprintf(stderr, "%s [UDP] (PONG_2, len %d)\n", adres (addr, 0), len);
		print_bin_data (buf, len);
		break;
	}

	default:
	{
		fprintf(stderr, "%s [UDP] (type 0x%02X, len %d)\n", adres (addr, 0), type, len);
		print_bin_data (buf, len);
		break;
	}
	}
}

void syslog (int type, int errnum, struct ip *iph, void *data)
{
	char saddr[20], daddr[20];
	struct tcphdr {
		u_int16_t th_sport;         /* source port */
		u_int16_t th_dport;         /* destination port */
	};

	if (errnum==9)
		return; /* ignore invalid TCP flags */

	switch (type) {
	case NIDS_WARN_IP:
        if (errnum != NIDS_WARN_IP_HDR) {
            strcpy(saddr, int_ntoa(iph->ip_src.s_addr));
            strcpy(daddr, int_ntoa(iph->ip_dst.s_addr));
            fprintf(stderr,
                   "%s, packet (apparently) from %s to %s\n",
                   nids_warnings[errnum], saddr, daddr);
        } else
            fprintf(stderr, "%s\n",
                   nids_warnings[errnum]);
        break;

	case NIDS_WARN_TCP:
		strcpy(saddr, int_ntoa(iph->ip_src.s_addr));
		strcpy(daddr, int_ntoa(iph->ip_dst.s_addr));
		if (errnum != NIDS_WARN_TCP_HDR)
			fprintf(stderr,
				"%s,from %s:%hu to  %s:%hu\n", nids_warnings[errnum],
				saddr, ntohs(((struct tcphdr *) data)->th_sport), daddr,
				ntohs(((struct tcphdr *) data)->th_dport));
		else
			fprintf(stderr, "%s, from %s to %s\n",
				nids_warnings[errnum], saddr, daddr);
		break;
	}
}

int main (int argc, char **argv)
{
	if (argc>1)
		nids_params.device=argv[1];

	nids_params.n_tcp_streams=1<<20; /* default is *way* too small */
	nids_params.syslog=syslog;
	nids_params.scan_num_hosts=0;

	if (!nids_init ()) {
		fprintf(stderr,"%s\n",nids_errbuf);
		exit(1);
	}
	fprintf(stderr, "listening on %s\n", nids_params.device);
	nids_register_tcp (tcp_callback);
	nids_register_udp (udp_callback);
	nids_run ();
	return 0;
}
