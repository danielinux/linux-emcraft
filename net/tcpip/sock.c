/*********************************************************************
PicoTCP. Copyright (c) 2013 TASS Belgium NV. Some rights reserved.
See LICENSE and COPYING for usage.
Do not redistribute without a written permission by the Copyright
holders.

Author: Daniele Lacamera
*********************************************************************/

#include "pico_defines.h"
#include "pico_config.h"    /* for zalloc and free */
#include "pico_sntp_client.h"
#include "linux/kthread.h"
#include <picotcp.h>

/* pico stack lock */
void * picoLock = NULL;

/************************/
/* Public API functions */
/************************/
void pico_bsd_init(void)
{
    picoLock = pico_mutex_init();
}

void pico_bsd_deinit(void)
{
    pico_mutex_deinit(picoLock);
}

void pico_bsd_stack_tick(void)
{
    pico_mutex_lock(picoLock);
    pico_stack_tick();
    pico_stack_tick();
    pico_mutex_unlock(picoLock);
}

/*** Public socket functions ***/
#if 0
/* Socket interface. */
int pico_newsocket(int domain, int type, int proto)
{
    struct pico_bsd_endpoint * ep = NULL;
    (void)proto;

#ifdef PICO_SUPPORT_IPV6
    VALIDATE_TWO(domain,AF_INET, AF_INET6);
#else
    VALIDATE_ONE(domain, AF_INET);
#endif
    VALIDATE_TWO(type,SOCK_STREAM,SOCK_DGRAM);

    if (AF_INET6 != PICO_PROTO_IPV6) {
        if (domain == AF_INET6)
            domain = PICO_PROTO_IPV6;
        else
            domain = PICO_PROTO_IPV4;
    }

    if (SOCK_STREAM != PICO_PROTO_TCP) {
        if (type == SOCK_STREAM)
            type = PICO_PROTO_TCP;
        else
            type = PICO_PROTO_UDP;
    }

    pico_mutex_lock(picoLock);
    ep = pico_bsd_create_socket();
    VALIDATE_NULL(ep);

    ep->proto = type;

    ep->s = pico_socket_open(domain, type,&pico_socket_event);
    if (!ep->s)
    {
        pico_free(ep);
        return -1;
    }

    ep->s->priv = ep; /* let priv point to the endpoint struct */

    /* open picotcp endpoint */
    ep->state = SOCK_OPEN;
    ep->mutex_lock = pico_mutex_init();
    ep->signal = pico_signal_init();
    pico_mutex_unlock(picoLock);
    return ep->socket_fd;
}


int pico_bind(struct socket *sock, struct sockaddr * local_addr, socklen_t socklen)
{ 
    union pico_address addr;
    uint16_t port;
    struct pico_bsd_endpoint *ep = get_endpoint(sd);

    VALIDATE_NULL(ep);
    VALIDATE_NULL(local_addr);
    VALIDATE_TWO(socklen, SOCKSIZE, SOCKSIZE6);

    if (bsd_to_pico_addr(&addr, local_addr, socklen) < 0)
        return -1;
    port = bsd_to_pico_port(local_addr, socklen);

    pico_mutex_lock(picoLock);
    if(pico_socket_bind(ep->s, &addr, &port) < 0)
    {
        pico_mutex_unlock(picoLock);
        return -1;
    }

    ep->state = SOCK_BOUND;
    pico_mutex_unlock(picoLock);

    return 0;
}

int pico_getsockname(struct socket *sock, struct sockaddr * local_addr, socklen_t *socklen)
{ 
    union pico_address addr;
    uint16_t port, proto;
    struct pico_bsd_endpoint *ep = get_endpoint(sd);

    VALIDATE_NULL(ep);
    VALIDATE_NULL(local_addr);
    VALIDATE_NULL(socklen);
    pico_mutex_lock(picoLock);
    if(pico_socket_getname(ep->s, &addr, &port, &proto) < 0)
    {
        pico_mutex_unlock(picoLock);
        return -1;
    }

    if (proto == PICO_PROTO_IPV6)
        *socklen = SOCKSIZE6;
    else
        *socklen = SOCKSIZE;

    if (pico_addr_to_bsd(local_addr, *socklen, &addr, proto) < 0) {
        pico_mutex_unlock(picoLock);
        return -1;
    }
    pico_port_to_bsd(local_addr, *socklen, port);
    return 0;
}


int pico_listen(struct socket *sock, int backlog)
{
    struct pico_bsd_endpoint *ep = get_endpoint(sd);

    VALIDATE_NULL(ep);
    VALIDATE_NULL(ep->s);
    VALIDATE_ONE(ep->state, SOCK_BOUND);

    pico_mutex_lock(picoLock);

    if(pico_socket_listen(ep->s, backlog) < 0)
    {
        pico_mutex_unlock(picoLock);
        return -1;
    }
    ep->state = SOCK_LISTEN;

    pico_mutex_unlock(picoLock);
    return 0;
}

int pico_connect(struct socket *sock, struct sockaddr *_saddr, socklen_t socklen)
{
    struct pico_bsd_endpoint *ep = get_endpoint(sd);
    union pico_address addr;
    uint16_t port;
    uint16_t ev;

    VALIDATE_NULL(ep);
    VALIDATE_NULL(_saddr);
    if (bsd_to_pico_addr(&addr, _saddr, socklen) < 0)
        return -1;
    port = bsd_to_pico_port(_saddr, socklen);
    pico_mutex_lock(picoLock);
    pico_socket_connect(ep->s, &addr, port);
    pico_mutex_unlock(picoLock);

    if (ep->nonblocking) {
        pico_err = PICO_ERR_EAGAIN;
        return -1;
    } else {
        /* wait for event */
        ev = pico_bsd_wait(ep, 0, 0, 0); /* wait for ERR, FIN and CONN */
    }

    if(ev & PICO_SOCK_EV_CONN)
    {
        /* clear the EV_CONN event */
        pico_event_clear(ep, PICO_SOCK_EV_CONN);
        return 0;
    } else {
        pico_socket_close(ep->s);
    }
    return -1;
}

int pico_accept(struct socket *sock, struct sockaddr *_orig, socklen_t *socklen)
{
    struct pico_bsd_endpoint *ep, * client_ep = NULL;
    uint16_t events;
    union pico_address picoaddr;
    uint16_t port;

    ep = get_endpoint(sd);

    VALIDATE_NULL(ep);
    VALIDATE_ONE(ep->state, SOCK_LISTEN);

    pico_mutex_lock(picoLock);

    client_ep = pico_bsd_create_socket();

    if (!client_ep)
    {
        pico_mutex_unlock(picoLock);
        return -1;
    }

    client_ep->state = SOCK_OPEN;
    client_ep->mutex_lock = pico_mutex_init();
    client_ep->signal = pico_signal_init();

    pico_mutex_unlock(picoLock);



    if (ep->nonblocking)
        events = PICO_SOCK_EV_CONN;
    else 
        events = pico_bsd_wait(ep, 0, 0, 0); /* Wait for CONN, FIN and ERR */

    if(events & PICO_SOCK_EV_CONN)
    {
        pico_mutex_lock(picoLock);
        client_ep->s = pico_socket_accept(ep->s,&picoaddr,&port);
        if (!client_ep->s)
        {
            pico_mutex_unlock(picoLock);
            return -1;
        }
        client_ep->s->priv = client_ep;
        pico_event_clear(ep, PICO_SOCK_EV_CONN); /* clear the CONN event the listening socket */
        if (client_ep->s->net->proto_number == PICO_PROTO_IPV4)
            *socklen = SOCKSIZE;
        else
            *socklen = SOCKSIZE6;
        client_ep->state = SOCK_CONNECTED;
        if (pico_addr_to_bsd(_orig, *socklen, &picoaddr, client_ep->s->net->proto_number) < 0) {
            pico_mutex_unlock(picoLock);
            return -1;
        }
        pico_port_to_bsd(_orig, *socklen, port);

        client_ep->in_use = 1;
        pico_mutex_unlock(picoLock);
        return client_ep->socket_fd;
    }
    return -1;
}

int pico_sendto(struct socket *sock, void * buf, int len, int flags, struct sockaddr *_dst, socklen_t socklen)
{
    int retval = 0;
    int tot_len = 0;
    uint16_t port;
    union pico_address picoaddr;
    struct pico_bsd_endpoint *ep = get_endpoint(sd);

    VALIDATE_NULL(ep);

    if (!buf || (len <= 0)) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }

    while (tot_len < len) {
        /* Write to the pico socket */
        pico_mutex_lock(picoLock);
        if (_dst == NULL) {
            retval = pico_socket_send(ep->s, ((uint8_t *)buf) + tot_len, len - tot_len);
        } else {
            if (bsd_to_pico_addr(&picoaddr, _dst, socklen) < 0) {
                pico_mutex_unlock(picoLock);
                return -1;
            }
            port = bsd_to_pico_port(_dst, socklen);
            retval = pico_socket_sendto(ep->s, ((uint8_t *)buf) + tot_len, len - tot_len, &picoaddr, port);
        }
        pico_mutex_unlock(picoLock);

        /* If sending failed, return an error */
        if (retval < 0)
            return -1;

        /* No, error so clear the revent */
        pico_event_clear(ep, PICO_SOCK_EV_WR);

        if (retval > 0)
        {
            tot_len += retval;
            break;
        }

        if (ep->nonblocking)
            break;

        /* If sent bytes (retval) < len-tot_len: socket full, we need to wait for a new WR event */
        if (retval < (len - tot_len))
        {
            uint16_t ev = 0;
            /* wait for a new WR or CLOSE event */
            ev = pico_bsd_wait(ep, 0, 1, 1);

            if (ev & (PICO_SOCK_EV_ERR | PICO_SOCK_EV_FIN | PICO_SOCK_EV_CLOSE ))
            {
                /* closing and freeing the socket is done in the event handler */
                return -1;
            }
        }
        tot_len += retval;
    }
    return tot_len;
}

int pico_fcntl(struct socket *sock, int cmd, int arg)
{
    struct pico_bsd_endpoint *ep = get_endpoint(sd);
    if (!ep) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }

    if (cmd == F_SETFL) {
        if ((arg & O_NONBLOCK) != 0) {
            ep->nonblocking = 1;
        } else {
            ep->nonblocking = 0;
        }
        return 0;
    }

    if (cmd == F_GETFL) {
        (void)arg; /* F_GETFL: arg is ignored */
        if (ep->nonblocking)
            return O_NONBLOCK;
        else
            return 0;
    }
    pico_err = PICO_ERR_EINVAL;
    return -1;
}

int pico_recvfrom(struct socket *sock, void * _buf, int len, int flags, struct sockaddr *_addr, socklen_t *socklen)
{
    int retval = 0;
    int tot_len = 0;
    struct pico_bsd_endpoint *ep = get_endpoint(sd);
    union pico_address picoaddr;
    uint16_t port;
    unsigned char *buf = (unsigned char *)_buf;
    bsd_dbg("Recvfrom called \n");

    if (!buf || (len <= 0)) {
        pico_err = PICO_ERR_EINVAL;
        return  -1;
    }

    VALIDATE_NULL(ep);


    while (tot_len < len) {
        pico_mutex_lock(picoLock);
        retval = pico_socket_recvfrom(ep->s, buf + tot_len ,  len - tot_len, &picoaddr, &port);
        pico_mutex_unlock(picoLock);
        bsd_dbg("pico_socket_recvfrom returns %d, first bytes are %c-%c-%c-%c\n", retval, buf[0], buf[1], buf[2], buf[3]);

        /* If receiving failed, return an error */
        if (retval == 0) {
            pico_event_clear(ep, PICO_SOCK_EV_RD);
            if (tot_len > 0) {
                bsd_dbg("Recvfrom returning %d\n", tot_len);
                return tot_len;
            }
        }

        if (retval < 0) {
            return -1;
        }

        if (retval > 0) {
            if (ep->proto == PICO_PROTO_UDP) {
                if (_addr && socklen > 0)
                {
                    if (pico_addr_to_bsd(_addr, *socklen, &picoaddr, ep->s->net->proto_number) < 0) {
                        pico_err = PICO_ERR_EINVAL;
                        return -1;
                    }
                    pico_port_to_bsd(_addr, *socklen, port);
                }
                /* If in a recvfrom call, for UDP we should return immediately after the first dgram */
                return retval + tot_len; 
            } else {
                /* TCP: continue until recvfrom = 0, socket buffer empty */
                tot_len += retval;
                continue;
            }
        }

        if (ep->nonblocking)
            break;

        /* If recv bytes (retval) < len-tot_len: socket empty, we need to wait for a new RD event */
        if (retval < (len - tot_len))
        {
            uint16_t ev = 0;
            /* wait for a new RD event -- also wait for CLOSE event */
            ev = pico_bsd_wait(ep, 1, 0, 1); 
            if (ev & (PICO_SOCK_EV_ERR | PICO_SOCK_EV_FIN | PICO_SOCK_EV_CLOSE))
            {
                /* closing and freeing the socket is done in the event handler */
                pico_event_clear(ep, PICO_SOCK_EV_RD);
                return -1;
            }
        }
        tot_len += retval;
    }
    pico_event_clear(ep, PICO_SOCK_EV_RD);
    bsd_dbg("Recvfrom returning %d (full block)\n", tot_len);
    return tot_len;
}

int pico_write(struct socket *sock, void * buf, int len)
{
    return pico_sendto(sd, buf, len, 0, NULL, 0);
}

int pico_send(struct socket *sock, void * buf, int len, int flags)
{
    return pico_sendto(sd, buf, len, flags, NULL, 0);
}

int pico_read(struct socket *sock, void * buf, int len)
{
    return pico_recvfrom(sd, buf, len, 0, NULL, 0);
}

int pico_recv(struct socket *sock, void * buf, int len, int flags)
{
    return pico_recvfrom(sd, buf, len, flags, NULL, 0);
}



int pico_close(int sd)
{
    struct pico_bsd_endpoint *ep = get_endpoint(sd);
    VALIDATE_NULL(ep);

    if(ep->s) /* valid socket, try to close it */
    {
        pico_mutex_lock(picoLock);
        pico_socket_close(ep->s);
        pico_mutex_unlock(picoLock);
    }
    ep->in_use = 0;
    return 0;
}

int pico_shutdown(struct socket *sock, int how)
{
    struct pico_bsd_endpoint *ep = get_endpoint(sd);
    VALIDATE_NULL(ep);

    if(ep->s) /* valid socket, try to close it */
    {
        pico_mutex_lock(picoLock);
        pico_socket_shutdown(ep->s, how);
        pico_mutex_unlock(picoLock);
    }
    return 0;
}

static void free_up_ep(struct pico_bsd_endpoint *ep)
{
    if (ep->signal)
        pico_signal_deinit(ep->signal);
    if (ep->mutex_lock)
        pico_mutex_deinit(ep->mutex_lock);
    pico_free(ep);
}

static int get_free_sd(struct pico_bsd_endpoint *ep)
{
    int i;
    for (i = 0; i < PicoSocket_max; i++) {
        if (!PicoSockets[i]->in_use) {
            free_up_ep(PicoSockets[i]);
            PicoSockets[i] = ep;
            return i;
        }
    }
    return -1;
}

/* DLA TODO: make a GC for freeing up the last socket descriptor periodically if not in use */

static int new_sd(struct pico_bsd_endpoint *ep)
{
    int sd = PicoSocket_max;
    struct pico_bsd_endpoint **new;
    new = pico_zalloc(sizeof(void *) * ++PicoSocket_max);
    if (!new) {
        PicoSocket_max--;
        pico_err = PICO_ERR_ENOMEM;
        return -1;
    }
    if (sd > 0) {
        memcpy(new, PicoSockets, sd * sizeof(void *));
        pico_free(PicoSockets);
    }
    PicoSockets = new;
    new[sd] = ep;
    return sd;
}

/* picoLock must be taken already ! */
static struct pico_bsd_endpoint *pico_bsd_create_socket(void)
{
    struct pico_bsd_endpoint *ep = pico_zalloc(sizeof(struct pico_bsd_endpoint));
    if (!ep) {
        pico_err = PICO_ERR_ENOMEM;
    }
    ep->in_use = 1;
    ep->socket_fd = get_free_sd(ep);
    if (ep->socket_fd < 0) {
        ep->socket_fd = new_sd(ep);
    }
    return ep;
}

static struct pico_bsd_endpoint *get_endpoint(int sd)
{
    if ((sd > PicoSocket_max) || (sd < 0) || 
         (PicoSockets[sd]->in_use == 0)) {
        pico_err = PICO_ERR_EINVAL;
        return NULL;
    }
    return PicoSockets[sd];
}

int pico_bsd_check_events(struct socket *sock, uint16_t events, uint16_t *revents)
{
    struct pico_bsd_endpoint *ep = get_endpoint(sd);
    if (!ep)
        return -1;
    if (ep->state == SOCK_CLOSED)
        *revents = PICO_SOCK_EV_FIN | PICO_SOCK_EV_CLOSE;
    else
        *revents = ep->revents & events;
    return 0;
}

/* wait for one of the selected events, return any of those that occurred */
void * caller_select = 0;
uint16_t pico_bsd_select(struct pico_bsd_endpoint *ep)
{
    uint16_t events = ep->events & ep->revents; /* maybe an event we are waiting for, was already queued ? */
    /* wait for one of the selected events... */
    caller_select = __builtin_return_address(0);
    while (!events)
    {
        pico_signal_wait(ep->signal);
        events = (ep->revents & ep->events); /* filter for the events we were waiting for */
    }
    /* the event we were waiting for happened, now report it */
    caller_select = NULL;

    return events; /* return any event(s) that occurred, that we were waiting for */
}


/****************************/
/* Private helper functions */
/****************************/
void * caller_wait = 0;
static uint16_t pico_bsd_wait(struct pico_bsd_endpoint * ep, int read, int write, int close)
{
  pico_mutex_lock(ep->mutex_lock);

  caller_wait = __builtin_return_address(0);
  ep->events = PICO_SOCK_EV_ERR;
  ep->events |= PICO_SOCK_EV_FIN;
  ep->events |= PICO_SOCK_EV_CONN;
  if (close)
      ep->events |= PICO_SOCK_EV_CLOSE;
  if (read) 
      ep->events |= PICO_SOCK_EV_RD;
  if (write)
      ep->events |= PICO_SOCK_EV_WR;

  pico_mutex_unlock(ep->mutex_lock);

  return pico_bsd_select(ep);
}


static void pico_event_clear(struct pico_bsd_endpoint *ep, uint16_t events)
{
    pico_mutex_lock(ep->mutex_lock);
    ep->revents &= ~events; /* clear those events */
    pico_mutex_unlock(ep->mutex_lock);
}

/* NOTE: __NO__ picoLock'ing here !! */
/* this is called from pico_stack_tick, so picoLock is already locked */
static void pico_socket_event(uint16_t ev, struct pico_socket *s)
{
    struct pico_bsd_endpoint * ep = (struct pico_bsd_endpoint *)s->priv;
    if(!ep || !ep->s || !ep->mutex_lock || !ep->signal )
    {
        if(ev & (PICO_SOCK_EV_CLOSE | PICO_SOCK_EV_FIN) )
            pico_socket_close(s);

        /* endpoint not initialized yet! */
        return;
    }

    if(ep->in_use != 1)
        return;


    pico_mutex_lock(ep->mutex_lock); /* lock over the complete body is needed,
                                        as the event might get cleared in another process.. */
    ep->revents |= ev; /* set those events */

    if(ev & PICO_SOCK_EV_CONN)
    {
        if(ep->state != SOCK_LISTEN)
        {
            ep->state  = SOCK_CONNECTED;
        }
    }

    if(ev & PICO_SOCK_EV_ERR)
    {
      if(pico_err == PICO_ERR_ECONNRESET)
      {
        bsd_dbg("Connection reset by peer...\n");
        ep->state = SOCK_RESET_BY_PEER;
      }
    }

    if (ev & PICO_SOCK_EV_CLOSE) {
        ep->state = SOCK_CLOSED;
    }

    if (ev & PICO_SOCK_EV_FIN) {
        /* DO NOT set ep->s = NULL,
            we might still be transmitting stuff! */
        ep->state = SOCK_CLOSED;
    }

    /* Was any1 waiting for this event? */
    /* if(ep->events & ep->revents) */
    {
        /* sending the event, while no one was listening,
           will just cause an extra loop in select() */
        pico_signal_send(ep->signal); /* signal the one that was blocking on this events */
    }

    pico_mutex_unlock(ep->mutex_lock);
}


#define DNSQUERY_OK 1
#define DNSQUERY_FAIL 0xFF
struct dnsquery_cookie
{
    struct addrinfo **res;
    void            *signal;
    uint8_t         block;
    uint8_t        revents;
    int             cleanup;
};

static struct dnsquery_cookie *dnsquery_cookie_create(struct addrinfo **res, uint8_t block)
{
    struct dnsquery_cookie *ck = pico_zalloc(sizeof(struct dnsquery_cookie));
    if (!ck) {
        pico_err = PICO_ERR_ENOMEM;
        return NULL;
    }
    ck->signal = pico_signal_init();
    ck->res = res;
    ck->block = block;
    ck->cleanup = 0;
    return ck;
}

#ifdef PICO_SUPPORT_IPV6
static void dns_ip6_cb(char *ip, void *arg)
{
    struct dnsquery_cookie *ck = (struct dnsquery_cookie *)arg;
    struct addrinfo *new;
    if (ck->cleanup) { /* call is no more valid. Caller got tired of waiting. */
        pico_signal_deinit(ck->signal);
        pico_free(ck);
        return;
    }
    if (ip) {
        new = pico_zalloc(sizeof(struct addrinfo));
        if (!new) {
            ck->revents = DNSQUERY_FAIL;
            if (ck->block)
                pico_signal_send(ck->signal);
            return;
        }
        new->ai_family = AF_INET6;
        new->ai_addr = pico_zalloc(sizeof(struct sockaddr_in6));
        if (!new->ai_addr) {
            pico_free(new);
            ck->revents = DNSQUERY_FAIL;
            if (ck->block)
                pico_signal_send(ck->signal);
            return;
        }
        new->ai_addrlen = sizeof(struct sockaddr_in6);
        pico_string_to_ipv6(ip, (((struct sockaddr_in6*)(new->ai_addr))->sin6_addr.s6_addr)); 
        ((struct sockaddr_in6*)(new->ai_addr))->sin6_family = AF_INET6;
        new->ai_next = *ck->res;
        *ck->res = new;
        ck->revents = DNSQUERY_OK;
    }
    if (ck->block)
        pico_signal_send(ck->signal);
}
#endif

static void dns_ip4_cb(char *ip, void *arg)
{
    struct dnsquery_cookie *ck = (struct dnsquery_cookie *)arg;
    struct addrinfo *new;
    if (ck->cleanup) { /* call is no more valid. Caller got tired of waiting. */
        pico_signal_deinit(ck->signal);
        pico_free(ck);
        return;
    }
    if (ip) {
        new = pico_zalloc(sizeof(struct addrinfo));
        if (!new) {
            ck->revents = DNSQUERY_FAIL;
            if (ck->block)
                pico_signal_send(ck->signal);
            return;
        }
        new->ai_family = AF_INET;
        new->ai_addr = pico_zalloc(sizeof(struct sockaddr_in));
        if (!new->ai_addr) {
            pico_free(new);
            ck->revents = DNSQUERY_FAIL;
            if (ck->block)
                pico_signal_send(ck->signal);
            return;
        }
        new->ai_addrlen = sizeof(struct sockaddr_in);
        pico_string_to_ipv4(ip, &(((struct sockaddr_in*)new->ai_addr)->sin_addr.s_addr));    
        ((struct sockaddr_in*)(new->ai_addr))->sin_family = AF_INET;
        new->ai_next = *ck->res;
        *ck->res = new;
        ck->revents = DNSQUERY_OK;
    }
    if (ck->block)
        pico_signal_send(ck->signal);
}

#ifdef PICO_SUPPORT_DNS_CLIENT
int pico_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
    struct dnsquery_cookie *ck4 = NULL;
    struct dnsquery_cookie *ck6 = NULL;
    struct sockaddr_in sa4;
    *res = NULL;
    (void)service;
    bsd_dbg("Called pico_getaddrinfo, looking for %s\n", node);
#ifdef PICO_SUPPORT_IPV6
    {
        struct sockaddr_in6 sa6;
        if (pico_string_to_ipv6(node, sa6.sin6_addr.s6_addr) == 0) {
            ck6 = dnsquery_cookie_create(res, 0);
            dns_ip6_cb((char *)node, ck6);
            return 0;
        }
        if (!hints || (hints->ai_family == AF_INET6)) {
            ck6 = dnsquery_cookie_create(res, 1);
            if (!ck6)
                return -1;
            pico_mutex_lock(picoLock);
            pico_dns_client_getaddr6(node, dns_ip6_cb, ck6);
            bsd_dbg("Resolving AAAA record %s\n", node);
            pico_mutex_unlock(picoLock);
        }
    }
#endif /* PICO_SUPPORT_IPV6 */
    if (pico_string_to_ipv4(node, &sa4.sin_addr.s_addr) == 0) {
        ck4 = dnsquery_cookie_create(res, 0);
        dns_ip4_cb((char*)node, ck4);
        return 0;
    }

    if (!hints || (hints->ai_family == AF_INET)) {
        ck4 = dnsquery_cookie_create(res, 1);
        pico_mutex_lock(picoLock);
        pico_dns_client_getaddr(node, dns_ip4_cb, ck4);
        bsd_dbg("Resolving A record %s\n", node);
        pico_mutex_unlock(picoLock);
    }

    if (ck6) {
        if (pico_signal_wait_timeout(ck6->signal, 2000) == 0) {
            pico_signal_deinit(ck6->signal);
            pico_free(ck6);
        } else {
            ck6->cleanup = 1;
        }
    }
    if (ck4) {
        if (pico_signal_wait_timeout(ck4->signal, 2000) == 0) {
            pico_signal_deinit(ck4->signal);
            pico_free(ck4);
        } else {
            ck4->cleanup = 1;
        }
    }
    if (*res)
        return 0;
    return -1;
}

void pico_freeaddrinfo(struct addrinfo *res)
{
    struct addrinfo *cur = res;
    struct addrinfo *nxt;
    while(cur) {
        if (cur->ai_addr)
            pico_free(cur->ai_addr);
        nxt = cur->ai_next;
        pico_free(cur);
        cur = nxt;
    }
}

/* Legacy gethostbyname call implementation */
static struct hostent PRIV_HOSTENT = { };
struct hostent *pico_gethostbyname(const char *name)
{
    struct addrinfo *res;
    struct addrinfo hint = {.ai_family = AF_INET};
    int ret;
    if (!PRIV_HOSTENT.h_addr_list) {
        /* Done only once: reserve space for 2 entries */
        PRIV_HOSTENT.h_addr_list = pico_zalloc(2 * sizeof(void*));
        PRIV_HOSTENT.h_addr_list[1] = NULL;
    }
    ret = pico_getaddrinfo(name, NULL, &hint, &res);
    if (ret == 0) {
        if (PRIV_HOSTENT.h_name != NULL) {
            pico_free(PRIV_HOSTENT.h_name);
            PRIV_HOSTENT.h_name = NULL;
        }
        if (PRIV_HOSTENT.h_addr_list[0] != NULL) {
            pico_free(PRIV_HOSTENT.h_addr_list[0]);
            PRIV_HOSTENT.h_addr_list[0] = NULL;
        }
        PRIV_HOSTENT.h_name = pico_zalloc(strlen(name));
        if (!PRIV_HOSTENT.h_name) {
            pico_freeaddrinfo(res);
            return NULL;
        }
        strcpy(PRIV_HOSTENT.h_name, name);
        PRIV_HOSTENT.h_addrtype = res->ai_addr->sa_family;
        if (PRIV_HOSTENT.h_addrtype == AF_INET) {
            PRIV_HOSTENT.h_length = 4;
            PRIV_HOSTENT.h_addr_list[0] = pico_zalloc(4); 
            if (!PRIV_HOSTENT.h_addr_list[0]) {
                pico_freeaddrinfo(res);
                return NULL;
            }
            memcpy (PRIV_HOSTENT.h_addr_list[0], &(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr), 4);
        } else {
            /* Only IPv4 supported by this ancient call. */
            pico_freeaddrinfo(res);
            return NULL;
        }
        pico_freeaddrinfo(res);
        return &PRIV_HOSTENT;
    }
    return NULL;
}
#endif

int pico_getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen)
{
    struct pico_bsd_endpoint *ep = get_endpoint(sockfd);
    int ret;
    bsd_dbg("called setsockopt\n");
    if (level != SOL_SOCKET) {
        pico_err = PICO_ERR_EPROTONOSUPPORT;
        return -1;
    }
    if (!ep) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }
    if (!optval) {
        pico_err = PICO_ERR_EFAULT;
        return -1;
    }
    pico_mutex_lock(ep->mutex_lock);
    ret = pico_socket_getoption(ep->s, sockopt_get_name(optname), optval);
    pico_mutex_unlock(ep->mutex_lock);
    return ret;

}

int pico_setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen)
{

    struct pico_bsd_endpoint *ep = get_endpoint(sockfd);
    int ret;
    bsd_dbg("called setsockopt\n");
    if (level != SOL_SOCKET) {
        pico_err = PICO_ERR_EPROTONOSUPPORT;
        return -1;
    }
    if (!ep) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }
    if (!optval) {
        pico_err = PICO_ERR_EFAULT;
        return -1;
    }
    pico_mutex_lock(ep->mutex_lock);
    ret = pico_socket_setoption(ep->s, sockopt_get_name(optname), (void *)optval);
    pico_mutex_unlock(ep->mutex_lock);
    return ret;
}
#ifdef PICO_SUPPORT_SNTP_CLIENT
int pico_gettimeofday(struct timeval *tv, struct timezone *tz)
{
    (void)tz;
    return pico_sntp_gettimeofday((struct pico_timeval *)tv);
}
#endif
#endif
