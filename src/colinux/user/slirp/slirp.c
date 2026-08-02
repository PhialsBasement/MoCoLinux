#include "slirp.h"

/*
 * The protocol structures are overlaid on live packet bytes, so their sizes
 * are wire format, not a matter of taste. Five lines that would have saved a
 * long evening: the toolchain silently made struct ip 24 bytes and ipovly 32,
 * and nothing anywhere said so -- packets simply parsed as nonsense. A
 * mis-set SIZEOF_CHAR_P, a compiler defaulting to -mms-bitfields, or a
 * padding change now stops the build instead of the network.
 */
_Static_assert(sizeof(struct ip)        == 20, "struct ip must be 20 bytes (wire format)");
_Static_assert(sizeof(struct tcphdr)    == 20, "struct tcphdr must be 20 bytes (wire format)");
_Static_assert(sizeof(struct ipovly)    == 20, "struct ipovly overlays a 20-byte IP header");
_Static_assert(sizeof(struct tcpiphdr) == 40, "struct tcpiphdr must be 20+20");
_Static_assert(sizeof(struct udpiphdr) == 28, "struct udpiphdr must be 20+8");
_Static_assert(sizeof(struct ipasfrag)  == 20, "struct ipasfrag aliases struct ip");

/* host address */
struct in_addr our_addr;
/* host dns address */
static struct in_addr dns_addr;
/* host loopback address */
struct in_addr loopback_addr;	/* 127.0.0.1 */

/* address for slirp virtual addresses */
struct in_addr special_addr;	/* 10.0.2.0 */
/* virtual address alias for host */
struct in_addr alias_addr;	/* 10.0.2.2  windows */
struct in_addr client_addr;	/* 10.0.2.15 Linux */

const uint8_t special_ethaddr[6] = {
    0x52, 0x54, 0x00, 0x12, 0x35, 0x00
};

uint8_t client_ethaddr[6] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

const uint8_t bcast_ethaddr[6] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

int do_slowtimo;
int link_up;
struct timeval tt;
FILE *lfd;
struct ex_list *exec_list;

/* XXX: suppress those select globals */
fd_set *global_readfds, *global_writefds, *global_xfds;

char slirp_hostname[33];

#ifdef _WIN32

static int get_dns_addr(struct in_addr *pdns_addr)
{
    FIXED_INFO *FixedInfo;
    ULONG    BufLen;
    DWORD    ret;
    IP_ADDR_STRING *pIPAddr;
    struct in_addr tmp_addr;

    FixedInfo = (FIXED_INFO *)GlobalAlloc(GPTR, sizeof(FIXED_INFO));
    if (!FixedInfo)
	return -1;

    BufLen = sizeof(FIXED_INFO);

    if (ERROR_BUFFER_OVERFLOW == GetNetworkParams(FixedInfo, &BufLen)) {
        GlobalFree(FixedInfo);
        FixedInfo = GlobalAlloc(GPTR, BufLen);
	if (!FixedInfo)
	    return -1;
    }

    if ((ret = GetNetworkParams(FixedInfo, &BufLen)) != ERROR_SUCCESS) {
        printf("GetNetworkParams failed. ret = %08x\n", (u_int)ret );
        GlobalFree(FixedInfo);
        return -1;
    }

    pIPAddr = &(FixedInfo->DnsServerList);
    inet_aton(pIPAddr->IpAddress.String, &tmp_addr);
    *pdns_addr = tmp_addr;
#if 0
    printf( "DNS Servers:\n" );
    printf( "DNS Addr:%s\n", pIPAddr->IpAddress.String );

    pIPAddr = FixedInfo -> DnsServerList.Next;
    while ( pIPAddr ) {
            printf( "DNS Addr:%s\n", pIPAddr ->IpAddress.String );
            pIPAddr = pIPAddr ->Next;
    }
#endif
    GlobalFree(FixedInfo);
    return 0;
}

#else

static int get_dns_addr(struct in_addr *pdns_addr)
{
    char buff[512];
    char buff2[256];
    FILE *f;
    int found = 0;
    struct in_addr tmp_addr;

    f = fopen("/etc/resolv.conf", "r");
    if (!f)
        return -1;

    lprint("IP address of your DNS(s): ");
    while (fgets(buff, 512, f) != NULL) {
        if (sscanf(buff, "nameserver%*[ \t]%256s", buff2) == 1) {
            if (!inet_aton(buff2, &tmp_addr))
                continue;
            if (is_localhost(tmp_addr))
                tmp_addr = alias_addr;
            /* If it's the first one, set it to dns_addr */
            if (!found)
                *pdns_addr = tmp_addr;
            else
                lprint(", ");
            if (++found > 3) {
                lprint("(more)");
                break;
            } else
                lprint("%s", inet_ntoa(tmp_addr));
        }
    }
    fclose(f);
    if (!found)
        return -1;
    return 0;
}

#endif

/* Get DNS-Address from host, cached for 10 Seconds (SO_EXPIREFAST) */
struct in_addr cached_dns_addr (void)
{
    static long dns_expire;

    if (!dns_addr.s_addr || curtime > dns_expire) {
        if (get_dns_addr(&dns_addr) < 0) {
            fprintf(stderr, "Could not get DNS address\n");
            return (dns_addr);
        }

        dns_expire = curtime + SO_EXPIREFAST;
    }
    return (dns_addr);
}

#ifdef _WIN32
static void slirp_cleanup(void)
{
    WSACleanup();
}
#endif

void slirp_init(void)
{
#ifdef DEBUG
    debug_init("slirp.log", DEBUG_DEFAULT);
#endif

#ifdef _WIN32
    {
        WSADATA Data;
        WSAStartup(MAKEWORD(2,0), &Data);
	atexit(slirp_cleanup);
    }
#endif

    link_up = 1;

    if_init();
    ip_init();

    /* Initialise mbufs *after* setting the MTU */
    m_init();

    /* set default addresses */
    inet_aton("127.0.0.1", &loopback_addr);

    inet_aton(CTL_SPECIAL, &special_addr);
    alias_addr.s_addr = special_addr.s_addr | htonl(CTL_ALIAS);
    client_addr.s_addr = special_addr.s_addr | htonl(CTL_CLIENT);
#if 0
    getouraddr();
#endif
}

#define CONN_CANFSEND(so) (((so)->so_state & (SS_FCANTSENDMORE|SS_ISFCONNECTED)) == SS_ISFCONNECTED)
#define CONN_CANFRCV(so) (((so)->so_state & (SS_FCANTRCVMORE|SS_ISFCONNECTED)) == SS_ISFCONNECTED)
#define UPD_NFDS(x) if (nfds < (x)) nfds = (x)

/*
 * curtime kept to an accuracy of 1ms
 */
#ifdef _WIN32
static void updtime(void)
{
    struct _timeb tb;

    _ftime(&tb);
    curtime = (u_int)tb.time * (u_int)1000;
    curtime += (u_int)tb.millitm;
}
#else
static void updtime(void)
{
	gettimeofday(&tt, 0);

	curtime = (u_int)tt.tv_sec * (u_int)1000;
	curtime += (u_int)tt.tv_usec / (u_int)1000;

	if ((tt.tv_usec % 1000) >= 500)
	   curtime++;
}
#endif

void slirp_select_fill(int *pnfds,
                       fd_set *readfds, fd_set *writefds, fd_set *xfds)
{
    struct socket *so, *so_next;
    struct timeval timeout;
    int nfds;
    int tmp_time;

    /* fail safe */
    global_readfds = NULL;
    global_writefds = NULL;
    global_xfds = NULL;

    nfds = *pnfds;
	/*
	 * First, TCP sockets
	 */
	do_slowtimo = 0;
	if (link_up) {
		/*
		 * *_slowtimo needs calling if there are IP fragments
		 * in the fragment queue, or there are TCP connections active
		 */
		do_slowtimo = ((tcb.so_next != &tcb) ||
			       ((struct ipasfrag *)&ipq != (struct ipasfrag *)ipq.next));

		for (so = tcb.so_next; so != &tcb; so = so_next) {
			so_next = so->so_next;

			/*
			 * See if we need a tcp_fasttimo
			 */
			if (time_fasttimo == 0 && so->so_tcpcb->t_flags & TF_DELACK)
			   time_fasttimo = curtime; /* Flag when we want a fasttimo */

			/*
			 * NOFDREF can include still connecting to local-host,
			 * newly socreated() sockets etc. Don't want to select these.
	 		 */
			if (so->so_state & SS_NOFDREF || so->s == -1)
			   continue;

			/*
			 * Set for reading sockets which are accepting
			 */
			if (so->so_state & SS_FACCEPTCONN) {
                                FD_SET(so->s, readfds);
				UPD_NFDS(so->s);
				continue;
			}

			/*
			 * Set for writing sockets which are connecting
			 */
			if (so->so_state & SS_ISFCONNECTING) {
				FD_SET(so->s, writefds);
				UPD_NFDS(so->s);
				continue;
			}

			/*
			 * Set for writing if we are connected, can send more, and
			 * we have something to send
			 */
			if (CONN_CANFSEND(so) && so->so_rcv.sb_cc) {
				FD_SET(so->s, writefds);
				UPD_NFDS(so->s);
			}

			/*
			 * Set for reading (and urgent data) if we are connected,
			 * can receive more, and there is room for it.
			 *
			 * This used to demand the buffer be less than HALF full
			 * -- the original carried "XXX /2 ?" beside it, so its
			 * author was unsure too. The effect is that slirp stops
			 * reading from the internet socket while it still has
			 * 32 KB of the 64 KB buffer free, waits for the guest to
			 * drain below the halfway mark, then reads again. The
			 * pipeline runs in bursts separated by dead time instead
			 * of continuously, and half the buffer never gets used.
			 *
			 * Measured against a download: the bridge span 1,847,782
			 * loops to deliver 6,447 frames -- 287 polls per frame --
			 * with zero deferrals on a full ring. It was starved, not
			 * saturated, and this is the valve.
			 *
			 * The condition that actually matters is whether a read
			 * can make progress at all, which is one MSS of room. Any
			 * less and soread's own mss rounding would return nothing
			 * and we would spin on a readable socket.
			 */
			if (CONN_CANFRCV(so) &&
			    sbspace(&so->so_snd) >= so->so_tcpcb->t_maxseg) {
				FD_SET(so->s, readfds);
				FD_SET(so->s, xfds);
				UPD_NFDS(so->s);
			}
		}

		/*
		 * UDP sockets
		 */
		for (so = udb.so_next; so != &udb; so = so_next) {
			so_next = so->so_next;

			/*
			 * See if it's timed out
			 */
			if (so->so_expire) {
				if (so->so_expire <= curtime) {
					udp_detach(so);
					continue;
				} else
					do_slowtimo = 1; /* Let socket expire */
			}

			/*
			 * When UDP packets are received from over the
			 * link, they're sendto()'d straight away, so
			 * no need for setting for writing
			 * Limit the number of packets queued by this session
			 * to 4.  Note that even though we try and limit this
			 * to 4 packets, the session could have more queued
			 * if the packets needed to be fragmented
			 * (XXX <= 4 ?)
			 */
			if ((so->so_state & SS_ISFCONNECTED) && so->so_queued <= 4) {
				FD_SET(so->s, readfds);
				UPD_NFDS(so->s);
			}
		}
	}

	/*
	 * Setup timeout to use minimum CPU usage, especially when idle
	 */

	/*
	 * First, see the timeout needed by *timo
	 */
	timeout.tv_sec = 0;
	timeout.tv_usec = -1;
	/*
	 * If a slowtimo is needed, set timeout to 500ms from the last
	 * slow timeout. If a fast timeout is needed, set timeout within
	 * 200ms of when it was requested.
	 */
	if (do_slowtimo) {
		/* XXX + 10000 because some select()'s aren't that accurate */
		timeout.tv_usec = ((500 - (curtime - last_slowtimo)) * 1000) + 10000;
		if (timeout.tv_usec < 0)
		   timeout.tv_usec = 0;
		else if (timeout.tv_usec > 510000)
		   timeout.tv_usec = 510000;

		/* Can only fasttimo if we also slowtimo */
		if (time_fasttimo) {
			tmp_time = (200 - (curtime - time_fasttimo)) * 1000;
			if (tmp_time < 0)
			   tmp_time = 0;

			/* Choose the smallest of the 2 */
			if (tmp_time < timeout.tv_usec)
			   timeout.tv_usec = (u_int)tmp_time;
		}
	}
        *pnfds = nfds;
}

void slirp_select_poll(fd_set *readfds, fd_set *writefds, fd_set *xfds)
{
    struct socket *so, *so_next;
    int ret;

    global_readfds = readfds;
    global_writefds = writefds;
    global_xfds = xfds;

	/* Update time */
	updtime();

	/*
	 * See if anything has timed out
	 */
	if (link_up) {
		if (time_fasttimo && ((curtime - time_fasttimo) >= 2)) {
			tcp_fasttimo();
			time_fasttimo = 0;
		}
		if (do_slowtimo && ((curtime - last_slowtimo) >= 499)) {
			ip_slowtimo();
			tcp_slowtimo();
			last_slowtimo = curtime;
		}
	}

	/*
	 * What the sender is actually limited by, once a second while data is
	 * queued.
	 *
	 * Five changes were made to this path from the outside -- batching the
	 * ring ioctl, the mirror, timer resolution, poll intervals, the read
	 * valve -- and only the latency ones moved the number. Throughput stuck
	 * near 1.6 MB/s while the same host reaches 42 MB/s natively, and the
	 * three remaining candidates have identical symptoms from outside and
	 * completely different fixes: the guest's advertised window (snd_wnd),
	 * slirp's congestion window (snd_cwnd), or nothing buffered to send
	 * (sb_cc near zero, meaning the socket read side is the constraint).
	 */
	if (link_up) {
		static u_int report_last;
		struct socket *rs;

		if (curtime - report_last >= 1000) {
			report_last = curtime;
			for (rs = tcb.so_next; rs != &tcb; rs = rs->so_next) {
				struct tcpcb *rtp = rs->so_tcpcb;

				if (!rtp || rs->so_snd.sb_cc == 0)
					continue;
				lprint("slirp: snd_wnd %lu cwnd %lu ssthresh %lu"
				       " sb_cc %d/%d mss %d state %d\r\n",
				       (unsigned long)rtp->snd_wnd,
				       (unsigned long)rtp->snd_cwnd,
				       (unsigned long)rtp->snd_ssthresh,
				       rs->so_snd.sb_cc,
				       rs->so_snd.sb_datalen,
				       rtp->t_maxseg,
				       rtp->t_state);
			}
		}
	}

	/*
	 * Check sockets
	 */
	if (link_up) {
		/*
		 * Check TCP sockets
		 */
		for (so = tcb.so_next; so != &tcb; so = so_next) {
			so_next = so->so_next;

			/*
			 * FD_ISSET is meaningless on these sockets
			 * (and they can crash the program)
			 */
			if (so->so_state & SS_NOFDREF || so->s == -1)
			   continue;

			/*
			 * Check for URG data
			 * This will soread as well, so no need to
			 * test for readfds below if this succeeds
			 */
			if (FD_ISSET(so->s, xfds))
			   sorecvoob(so);
			/*
			 * Check sockets for reading
			 */
			else if (FD_ISSET(so->s, readfds)) {
				/*
				 * Check for incoming connections
				 */
				if (so->so_state & SS_FACCEPTCONN) {
					tcp_connect(so);
					continue;
				} /* else */

				/*
				 * Read until the socket is dry or the buffer is
				 * full, not once per poll.
				 *
				 * soread() fills at most the contiguous room at
				 * sb_wptr and rounds that down to a whole number
				 * of MSS, so a single call routinely leaves data
				 * in the socket even when there is plenty of
				 * buffer left -- and the next chance to collect
				 * it is a whole poll cycle away. With one read
				 * per poll the transfer rate becomes a function
				 * of how often this loop runs rather than of how
				 * fast the network is, which is exactly what the
				 * counters showed: 287 polls per frame delivered,
				 * and a ring that never once filled.
				 *
				 * Bounded so one busy socket cannot hold the loop
				 * against the others, or against the guest's TX
				 * ring. Sixteen reads is up to a full buffer.
				 */
				/*
				 * Drain the socket, do not take one bite per
				 * poll.
				 *
				 * soread() fills the contiguous room at sb_wptr
				 * and rounds it down to whole segments, so a
				 * single call takes about 15 KB however much the
				 * socket is holding. Measured over a 9.3 MB
				 * download: 6449 frames delivered in 605 batches,
				 * 108 batches a second, 15.5 KB each -- 1.68 MB/s,
				 * with the congestion window wide open and the
				 * guest's ring never once full. The sender was
				 * not window-limited, it simply had nothing left
				 * to send between polls.
				 *
				 * This was tried before and broke every TLS
				 * connection, because the second soread() would
				 * hit a would-block and slirp misread that as the
				 * peer hanging up -- see the errno note in
				 * soread(). With that fixed, a dry socket now
				 * returns 0 and ends the loop, which is what
				 * makes draining safe.
				 *
				 * Bounded at sixteen so one busy socket cannot
				 * starve the others or the guest's TX ring, and
				 * stopped as soon as there is less than a segment
				 * of room, which is the least soread() can use.
				 */
				{
					int reads = 0;

					do {
						ret = soread(so);
						if (ret > 0)
							tcp_output(sototcpcb(so));
					} while (ret > 0 && ++reads < 16 &&
						 CONN_CANFRCV(so) &&
						 sbspace(&so->so_snd) >=
							so->so_tcpcb->t_maxseg);
				}
			}

			/*
			 * Check sockets for writing
			 */
			if (FD_ISSET(so->s, writefds)) {
			  /*
			   * Check for non-blocking, still-connecting sockets
			   */
			  if (so->so_state & SS_ISFCONNECTING) {
			    /* Connected */
			    so->so_state &= ~SS_ISFCONNECTING;

			    ret = send(so->s, (void*)&ret, 0, 0);
			    if (ret < 0) {
			      /* XXXXX Must fix, zero bytes is a NOP */
			      if (errno == EAGAIN || errno == EWOULDBLOCK ||
				  errno == EINPROGRESS || errno == ENOTCONN)
				continue;

			      /* else failed */
			      so->so_state = SS_NOFDREF;
			    }
			    /* else so->so_state &= ~SS_ISFCONNECTING; */

			    /*
			     * Continue tcp_input
			     */
			    tcp_input((struct mbuf *)NULL, sizeof(struct ip), so);
			    /* continue; */
			  } else
			    ret = sowrite(so);
			  /*
			   * XXXXX If we wrote something (a lot), there
			   * could be a need for a window update.
			   * In the worst case, the remote will send
			   * a window probe to get things going again
			   */
			}

			/*
			 * Probe a still-connecting, non-blocking socket
			 * to check if it's still alive
	 	 	 */
#ifdef PROBE_CONN
			if (so->so_state & SS_ISFCONNECTING) {
			  ret = recv(so->s, (char *)&ret, 0,0);

			  if (ret < 0) {
			    /* XXX */
			    if (errno == EAGAIN || errno == EWOULDBLOCK ||
				errno == EINPROGRESS || errno == ENOTCONN)
			      continue; /* Still connecting, continue */

			    /* else failed */
			    so->so_state = SS_NOFDREF;

			    /* tcp_input will take care of it */
			  } else {
			    ret = send(so->s, &ret, 0,0);
			    if (ret < 0) {
			      /* XXX */
			      if (errno == EAGAIN || errno == EWOULDBLOCK ||
				  errno == EINPROGRESS || errno == ENOTCONN)
				continue;
			      /* else failed */
			      so->so_state = SS_NOFDREF;
			    } else
			      so->so_state &= ~SS_ISFCONNECTING;

			  }
			  tcp_input((struct mbuf *)NULL, sizeof(struct ip),so);
			} /* SS_ISFCONNECTING */
#endif
		}

		/*
		 * Now UDP sockets.
		 * Incoming packets are sent straight away, they're not buffered.
		 * Incoming UDP data isn't buffered either.
		 */
		for (so = udb.so_next; so != &udb; so = so_next) {
			so_next = so->so_next;

			if (so->s != -1 && FD_ISSET(so->s, readfds)) {
                            sorecvfrom(so);
                        }
		}
	}

	/*
	 * See if we can start outputting
	 */
	if (if_queued && link_up)
	   if_start();

	/* clear global file descriptor sets.
	 * these reside on the stack in vl.c
	 * so they're unusable if we're not in
	 * slirp_select_fill or slirp_select_poll.
	 */
	 global_readfds = NULL;
	 global_writefds = NULL;
	 global_xfds = NULL;
}

#define ETH_ALEN 6
#define ETH_HLEN 14

#define ETH_P_IP	0x0800		/* Internet Protocol packet	*/
#define ETH_P_ARP	0x0806		/* Address Resolution packet	*/

#define	ARPOP_REQUEST	1		/* ARP request			*/
#define	ARPOP_REPLY	2		/* ARP reply			*/

struct ethhdr
{
	unsigned char	h_dest[ETH_ALEN];	/* destination eth addr	*/
	unsigned char	h_source[ETH_ALEN];	/* source ether addr	*/
	unsigned short	h_proto;		/* packet type ID field	*/
};

struct arphdr
{
	unsigned short	ar_hrd;		/* format of hardware address	*/
	unsigned short	ar_pro;		/* format of protocol address	*/
	unsigned char	ar_hln;		/* length of hardware address	*/
	unsigned char	ar_pln;		/* length of protocol address	*/
	unsigned short	ar_op;		/* ARP opcode (command)		*/

	 /*
	  *	 Ethernet looks like this : This bit is variable sized however...
	  */
	unsigned char		ar_sha[ETH_ALEN];	/* sender hardware address	*/
	unsigned char		ar_sip[4];		/* sender IP address		*/
	unsigned char		ar_tha[ETH_ALEN];	/* target hardware address	*/
	unsigned char		ar_tip[4];		/* target IP address		*/
};

void arp_input(const uint8_t *pkt, int pkt_len)
{
    struct ethhdr *eh = (struct ethhdr *)pkt;
    struct arphdr *ah = (struct arphdr *)(pkt + ETH_HLEN);
    uint8_t arp_reply[ETH_HLEN + sizeof(struct arphdr)];
    struct ethhdr *reh = (struct ethhdr *)arp_reply;
    struct arphdr *rah = (struct arphdr *)(arp_reply + ETH_HLEN);
    int ar_op;
    struct ex_list *ex_ptr;

    ar_op = ntohs(ah->ar_op);
    switch(ar_op) {
    case ARPOP_REQUEST:
        if (!memcmp(ah->ar_tip, &special_addr, 3)) {
            if (ah->ar_tip[3] == CTL_DNS || ah->ar_tip[3] == CTL_ALIAS)
                goto arp_ok;
            for (ex_ptr = exec_list; ex_ptr; ex_ptr = ex_ptr->ex_next) {
                if (ex_ptr->ex_addr == ah->ar_tip[3])
                    goto arp_ok;
            }
            return;
        arp_ok:
            /* ARP request for alias/dns mac address */
            memcpy(reh->h_dest, pkt + ETH_ALEN, ETH_ALEN);
            memcpy(reh->h_source, special_ethaddr, ETH_ALEN - 1);
            reh->h_source[5] = ah->ar_tip[3];
            reh->h_proto = htons(ETH_P_ARP);

            rah->ar_hrd = htons(1);
            rah->ar_pro = htons(ETH_P_IP);
            rah->ar_hln = ETH_ALEN;
            rah->ar_pln = 4;
            rah->ar_op = htons(ARPOP_REPLY);
            memcpy(rah->ar_sha, reh->h_source, ETH_ALEN);
            memcpy(rah->ar_sip, ah->ar_tip, 4);
            memcpy(rah->ar_tha, ah->ar_sha, ETH_ALEN);
            memcpy(rah->ar_tip, ah->ar_sip, 4);
            slirp_output(arp_reply, sizeof(arp_reply));
        }
        break;
    case ARPOP_REPLY:
        if (!memcmp(ah->ar_sip, &client_addr, 4)) {
            memcpy(client_ethaddr, eh->h_source, ETH_ALEN);
        }
        break;
    default:
        break;
    }
}

void slirp_input(const uint8_t *pkt, int pkt_len)
{
    struct mbuf *m;
    int proto;

    if (pkt_len < ETH_HLEN)
        return;

    proto = ntohs(*(uint16_t *)(pkt + 12));
    switch(proto) {
    case ETH_P_ARP:
        arp_input(pkt, pkt_len);
        break;
    case ETH_P_IP:
        m = m_get();
        if (!m)
            return;
        /* Note: we add to align the IP header */
        m->m_len = pkt_len + 2;
        memcpy(m->m_data + 2, pkt, pkt_len);

        m->m_data += 2 + ETH_HLEN;
        m->m_len -= 2 + ETH_HLEN;

        ip_input(m);
        break;
    default:
        break;
    }
}

/* output the IP packet to the ethernet device */
void if_encap(const uint8_t *ip_data, int ip_data_len)
{
    uint8_t buf[1600];
    struct ethhdr *eh = (struct ethhdr *)buf;
    struct arphdr *rah = (struct arphdr *)(buf + ETH_HLEN);

    if (ip_data_len + ETH_HLEN > sizeof(buf))
        return;

    if(!memcmp(client_ethaddr,bcast_ethaddr,6))
    {
	/* make an ARP request to have the client address */
	memcpy(eh->h_dest, bcast_ethaddr, ETH_ALEN);
	memcpy(eh->h_source, special_ethaddr, ETH_ALEN - 1);
	eh->h_source[5] = CTL_ALIAS;
	eh->h_proto = htons(ETH_P_ARP);

	rah->ar_hrd = htons(1);
	rah->ar_pro = htons(ETH_P_IP);
	rah->ar_hln = ETH_ALEN;
	rah->ar_pln = 4;
	rah->ar_op = htons(ARPOP_REQUEST);
	memcpy(rah->ar_sha, eh->h_source, ETH_ALEN);
	memcpy(rah->ar_sip, &special_addr, 4);
	rah->ar_sip[3]=CTL_ALIAS;
	memcpy(rah->ar_tha, bcast_ethaddr, ETH_ALEN);
	memcpy(rah->ar_tip, &client_addr, 4);

	slirp_output(buf, sizeof(struct arphdr)+ ETH_HLEN);
/* XXX: We loose the first packet here, cause client_ethaddr
   is bcast_ethaddr. */
    }

    memcpy(eh->h_dest, client_ethaddr, ETH_ALEN);
    memcpy(eh->h_source, special_ethaddr, ETH_ALEN - 1);
    /* XXX: not correct */
    eh->h_source[5] = CTL_ALIAS;
    eh->h_proto = htons(ETH_P_IP);
    memcpy(buf + sizeof(struct ethhdr), ip_data, ip_data_len);
    slirp_output(buf, ip_data_len + ETH_HLEN);
}

int slirp_redir(int is_udp, int host_port,
                struct in_addr guest_addr, int guest_port)
{
    if (is_udp) {
        if (!udp_listen(htons(host_port), guest_addr.s_addr,
                        htons(guest_port), 0))
            return -1;
    } else {
        if (!solisten(htons(host_port), guest_addr.s_addr,
                      htons(guest_port), 0))
            return -1;
    }
    return 0;
}
