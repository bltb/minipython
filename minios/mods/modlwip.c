/*
 * This file is part of the Micro Python project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2013, 2014 Damien P. George
 * Copyright (c) 2015 Galen Hazelwood
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <string.h>
#include <errno.h>
#include <stdio.h>

#include "py/nlr.h"
#include "py/objlist.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "py/mphal.h"

#include "netutils.h"

#include "lwip/init.h"
#include "lwip/timers.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/dns.h"
#include "lwip/tcp_impl.h"
#include "lwip/netif.h"
#include "lwip/inet.h"
#include <mini-os/lwip-net.h>
#include "modlwip.h"
#include "xenbus.h"

#if 0 // print debugging info
#define DEBUG_printf DEBUG_printf
#else // don't print debugging info
#define DEBUG_printf(...) (void)0
#endif

// For compatibilily with older lwIP versions.
#ifndef ip_set_option
#define ip_set_option(pcb, opt)   ((pcb)->so_options |= (opt))
#endif
#ifndef ip_reset_option
#define ip_reset_option(pcb, opt) ((pcb)->so_options &= ~(opt))
#endif

#ifdef MICROPY_PY_LWIP_SLIP
#include "netif/slipif.h"
#include "lwip/sio.h"
#endif

STATIC bool LWIP_INIT = false;
#define ETHER_MAX 4

typedef struct _lwip_ether_obj_t {
  mp_obj_base_t     base;
  struct eth_addr   mac;
  struct netif      netif;
  struct netfrontif nfi;
  ip4_addr_t        ip;
  ip4_addr_t        mask;
  ip4_addr_t        gw;
} lwip_ether_obj_t;

STATIC const mp_obj_type_t lwip_ether_type;

static lwip_ether_obj_t lwip_ether_objs[ETHER_MAX];
static int lwip_ether_objs_count = 0;

STATIC int lwip_find_ip(const char *ip, char *found_ip);
STATIC int lwip_find_next_noip(int offset);
STATIC lwip_ether_obj_t *lwip_addif(const ip4_addr_t *ip, const ip4_addr_t *mask, const ip4_addr_t *gw);

STATIC lwip_ether_obj_t *lwip_addif(const ip4_addr_t *ip,
                                    const ip4_addr_t *mask,
                                    const ip4_addr_t *gw) {
    static int noip_off = 0;

    lwip_ether_obj_t *obj;
    char str_ip[20] = { 0 };
    char str_found_ip[20] = { 0 };
    int vifnum = -1;

    /* Check we haven't exceeded the max number of devs */
    if (lwip_ether_objs_count >= ETHER_MAX) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_ValueError, "Max num of Ether interfaces reached!"));
        return NULL;
    }

    /* --- CREATE A NEW INTERFACE --- */
    obj = &(lwip_ether_objs[lwip_ether_objs_count]);

    /* Check specified IP is one of the VM's devs in the xenstore
     * If args[0] is set to '0.0.0.0', take over the IP set in Xenstore
     * If no IPs were specified on xenstore, find the first unused one (but take 0.0.0.0 as an invalid argument) */
    ip4addr_ntoa_r(ip, str_ip, sizeof(str_ip));
    vifnum = lwip_find_ip(str_ip, str_found_ip);
    if (vifnum < 0) {
        /* We did not find any interface with a matching IP address
         * now we will start searching for vifs without IPs set but we do not accept */
        /* Ensure that args[0] is not '0.0.0.0' */
        vifnum = lwip_find_next_noip(noip_off);
        if (vifnum < 0) {
            nlr_raise(mp_obj_new_exception_msg(&mp_type_ValueError, "Could not find any suitable interface!"));
            return NULL;
        }

        /* increase offset for the next search */
        noip_off = vifnum + 1;
    }
    ASSERT(vifnum >= 0);

    /* Create a new interface */
    obj->base.type = &lwip_ether_type;
    obj->nfi.vif_id = vifnum;
    if (!ip4_addr_isany_val(*ip)) {
        /* specified IP is non-0.0.0.0, so we take this one */
        ip4_addr_copy(obj->ip, *ip);
    } else {
        /* specified IP is 0.0.0.0, we will take the one found by the interface */
        if (!ipaddr_aton(str_found_ip, &obj->ip)) {
            /* ...did not work, we generate just one now... */
            IP4_ADDR(&obj->ip, 192, 168, 0, 55);
            ip4addr_ntoa_r(&obj->ip, str_ip, sizeof(str_ip));
        }
    }
    ip4_addr_copy(obj->mask, *mask);
    ip4_addr_copy(obj->gw,   *gw);

    printk("Initialize vif%d with %s\n", vifnum, str_ip);
    netif_add(&obj->netif,
              &obj->ip,
              &obj->mask,
              &obj->gw,
              &obj->nfi,
              netfrontif_init,
              ethernet_input);
    if (lwip_ether_objs_count == 0)
        netif_set_default(&obj->netif);
    netif_set_up(&obj->netif);

    /* Get ready for next device */
    ++lwip_ether_objs_count;

    return (mp_obj_t) obj;
}

STATIC mp_obj_t lwip_ether_make_new(const mp_obj_type_t *type_in,
				    mp_uint_t n_args,
				    mp_uint_t n_kw,
				    const mp_obj_t *args) {
    ip4_addr_t        ip;
    ip4_addr_t        mask;
    ip4_addr_t        gw;

    /* Arguments check */
    mp_arg_check_num(n_args, n_kw, 3, 3, false);

    if (!ipaddr_aton(mp_obj_str_get_str(args[0]), &ip)) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_ValueError, "not a valid IP address"));
        return NULL;
    }
    if (!ipaddr_aton(mp_obj_str_get_str(args[1]), &mask)) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_ValueError, "not a valid mask"));
        return NULL;
    }
    if (!ipaddr_aton(mp_obj_str_get_str(args[2]), &gw)) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_ValueError, "not a valid gateway"));
        return NULL;
    }

    return lwip_addif(&ip, &mask, &gw);
}

STATIC mp_obj_t lwip_ether_poll(mp_obj_t e) {
  lwip_ether_obj_t *obj = (lwip_ether_obj_t*)e;
  netfrontif_poll(&obj->netif);
  return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(lwip_ether_poll_obj, lwip_ether_poll);



STATIC const mp_map_elem_t lwip_ether_locals_dict_table[] = {
  { MP_OBJ_NEW_QSTR(MP_QSTR_poll), (mp_obj_t)&lwip_ether_poll_obj },
};

STATIC MP_DEFINE_CONST_DICT(lwip_ether_locals_dict, lwip_ether_locals_dict_table);

STATIC const mp_obj_type_t lwip_ether_type = {
  { &mp_type_type },
  .name = MP_QSTR_ether,
  .make_new = lwip_ether_make_new,
  .locals_dict = (mp_obj_t)&lwip_ether_locals_dict,
};


#ifdef MICROPY_PY_LWIP_SLIP
/******************************************************************************/
// Slip object for modlwip. Requires a serial driver for the port that supports
// the lwip serial callback functions.

typedef struct _lwip_slip_obj_t {
    mp_obj_base_t base;
    struct netif lwip_netif;
} lwip_slip_obj_t;

// Slip object is unique for now. Possibly can fix this later. FIXME
STATIC lwip_slip_obj_t lwip_slip_obj;

// Declare these early.
void mod_lwip_register_poll(void (*poll)(void *arg), void *poll_arg);
void mod_lwip_deregister_poll(void (*poll)(void *arg), void *poll_arg);

STATIC void slip_lwip_poll(void *netif) {
    slipif_poll((struct netif*)netif);
}

STATIC const mp_obj_type_t lwip_slip_type;

// lwIP SLIP callback functions
sio_fd_t sio_open(u8_t dvnum) {
    // We support singleton SLIP interface, so just return any truish value.
    return (sio_fd_t)1;
}

void sio_send(u8_t c, sio_fd_t fd) {
    mp_obj_type_t *type = mp_obj_get_type(MP_STATE_VM(lwip_slip_stream));
    int error;
    type->stream_p->write(MP_STATE_VM(lwip_slip_stream), &c, 1, &error);
}

u32_t sio_tryread(sio_fd_t fd, u8_t *data, u32_t len) {
    mp_obj_type_t *type = mp_obj_get_type(MP_STATE_VM(lwip_slip_stream));
    int error;
    mp_uint_t out_sz = type->stream_p->read(MP_STATE_VM(lwip_slip_stream), data, len, &error);
    if (out_sz == MP_STREAM_ERROR) {
        if (mp_is_nonblocking_error(error)) {
            return 0;
        }
        // Can't do much else, can we?
        return 0;
    }
    return out_sz;
}

// constructor lwip.slip(device=integer, iplocal=string, ipremote=string)
mp_obj_t lwip_slip_make_new(mp_obj_t type_in, mp_uint_t n_args, mp_uint_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 3, 3, false);

    lwip_slip_obj.base.type = &lwip_slip_type;

    MP_STATE_VM(lwip_slip_stream) = args[0];

    ip_addr_t iplocal, ipremote;
    if (!ipaddr_aton(mp_obj_str_get_str(args[1]), &iplocal)) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_ValueError, "not a valid local IP"));
    }
    if (!ipaddr_aton(mp_obj_str_get_str(args[2]), &ipremote)) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_ValueError, "not a valid remote IP"));
    }

    struct netif *n = &lwip_slip_obj.lwip_netif;
    if (netif_add(n, &iplocal, IP_ADDR_BROADCAST, &ipremote, NULL, slipif_init, ip_input) == NULL) {
       nlr_raise(mp_obj_new_exception_msg(&mp_type_ValueError, "out of memory"));
    }
    netif_set_up(n);
    netif_set_default(n);
    mod_lwip_register_poll(slip_lwip_poll, n);

    return (mp_obj_t)&lwip_slip_obj;
}

STATIC mp_obj_t lwip_slip_status(mp_obj_t self_in) {
    // Null function for now.
    return mp_const_none;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_1(lwip_slip_status_obj, lwip_slip_status);

STATIC const mp_map_elem_t lwip_slip_locals_dict_table[] = {
    { MP_OBJ_NEW_QSTR(MP_QSTR_status), (mp_obj_t)&lwip_slip_status_obj },
};

STATIC MP_DEFINE_CONST_DICT(lwip_slip_locals_dict, lwip_slip_locals_dict_table);

STATIC const mp_obj_type_t lwip_slip_type = {
    { &mp_type_type },
    .name = MP_QSTR_slip,
    .make_new = lwip_slip_make_new,
    .locals_dict = (mp_obj_t)&lwip_slip_locals_dict,
};

#endif // MICROPY_PY_LWIP_SLIP

/******************************************************************************/
// Table to convert lwIP err_t codes to socket errno codes, from the lwIP
// socket API.

// Extension to lwIP error codes
#define _ERR_BADF -16
// TODO: We just know that change happened somewhere between 1.4.0 and 1.4.1,
// investigate in more detail.
#if LWIP_VERSION < 0x01040100
static const int error_lookup_table[] = {
    0,             /* ERR_OK          0      No error, everything OK. */
    ENOMEM,        /* ERR_MEM        -1      Out of memory error.     */
    ENOBUFS,       /* ERR_BUF        -2      Buffer error.            */
    EWOULDBLOCK,   /* ERR_TIMEOUT    -3      Timeout                  */
    EHOSTUNREACH,  /* ERR_RTE        -4      Routing problem.         */
    EINPROGRESS,   /* ERR_INPROGRESS -5      Operation in progress    */
    EINVAL,        /* ERR_VAL        -6      Illegal value.           */
    EWOULDBLOCK,   /* ERR_WOULDBLOCK -7      Operation would block.   */

    ECONNABORTED,  /* ERR_ABRT       -8      Connection aborted.      */
    ECONNRESET,    /* ERR_RST        -9      Connection reset.        */
    ENOTCONN,      /* ERR_CLSD       -10     Connection closed.       */
    ENOTCONN,      /* ERR_CONN       -11     Not connected.           */
    EIO,           /* ERR_ARG        -12     Illegal argument.        */
    EADDRINUSE,    /* ERR_USE        -13     Address in use.          */
    -1,            /* ERR_IF         -14     Low-level netif error    */
    EALREADY,      /* ERR_ISCONN     -15     Already connected.       */
    EBADF,         /* _ERR_BADF      -16     Closed socket (null pcb) */
};
#else
static const int error_lookup_table[] = {
    0,             /* ERR_OK          0      No error, everything OK. */
    ENOMEM,        /* ERR_MEM        -1      Out of memory error.     */
    ENOBUFS,       /* ERR_BUF        -2      Buffer error.            */
    EWOULDBLOCK,   /* ERR_TIMEOUT    -3      Timeout                  */
    EHOSTUNREACH,  /* ERR_RTE        -4      Routing problem.         */
    EINPROGRESS,   /* ERR_INPROGRESS -5      Operation in progress    */
    EINVAL,        /* ERR_VAL        -6      Illegal value.           */
    EWOULDBLOCK,   /* ERR_WOULDBLOCK -7      Operation would block.   */

    EADDRINUSE,    /* ERR_USE        -8      Address in use.          */
    EALREADY,      /* ERR_ISCONN     -9      Already connected.       */
    ECONNABORTED,  /* ERR_ABRT       -10     Connection aborted.      */
    ECONNRESET,    /* ERR_RST        -11     Connection reset.        */
    ENOTCONN,      /* ERR_CLSD       -12     Connection closed.       */
    ENOTCONN,      /* ERR_CONN       -13     Not connected.           */
    EIO,           /* ERR_ARG        -14     Illegal argument.        */
    -1,            /* ERR_IF         -15     Low-level netif error    */
    EBADF,         /* _ERR_BADF      -16     Closed socket (null pcb) */
};
#endif

/*******************************************************************************/
// The socket object provided by lwip.socket.

#define MOD_NETWORK_AF_INET (2)
#define MOD_NETWORK_AF_INET6 (10)

#define MOD_NETWORK_SOCK_STREAM (1)
#define MOD_NETWORK_SOCK_DGRAM (2)
#define MOD_NETWORK_SOCK_RAW (3)

static inline void poll_sockets(void) {
    int i;

    for (i = 0; i < lwip_ether_objs_count; i++)
        netfrontif_poll(&lwip_ether_objs[i].netif);
}

/*******************************************************************************/
// Callback functions for the lwIP raw API.

static inline void exec_user_callback(lwip_socket_obj_t *socket) {
    if (socket->callback != MP_OBJ_NULL) {
        mp_call_function_1_protected(socket->callback, socket);
    }
}

// Callback for incoming UDP packets. We simply stash the packet and the source address,
// in case we need it for recvfrom.
STATIC void _lwip_udp_incoming(void *arg, struct udp_pcb *upcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    lwip_socket_obj_t *socket = (lwip_socket_obj_t*)arg;

    if (socket->incoming.pbuf != NULL) {
        // That's why they call it "unreliable". No room in the inn, drop the packet.
        pbuf_free(p);
    } else {
        socket->incoming.pbuf = p;
        socket->peer_port = (mp_uint_t)port;
        memcpy(&socket->peer, addr, sizeof(socket->peer));
    }
}

// Callback for general tcp errors.
STATIC void _lwip_tcp_error(void *arg, err_t err) {
    lwip_socket_obj_t *socket = (lwip_socket_obj_t*)arg;

    // Pass the error code back via the connection variable.
    socket->state = err;
    // If we got here, the lwIP stack either has deallocated or will deallocate the pcb.
    socket->pcb.tcp = NULL;
}

// Callback for tcp connection requests. Error code err is unused. (See tcp.h)
STATIC err_t _lwip_tcp_connected(void *arg, struct tcp_pcb *tpcb, err_t err) {
    lwip_socket_obj_t *socket = (lwip_socket_obj_t*)arg;

    socket->state = STATE_CONNECTED;
    return ERR_OK;
}

// By default, a child socket of listen socket is created with recv
// handler which discards incoming pbuf's. We don't want to do that,
// so set this handler which requests lwIP to keep pbuf's and deliver
// them later. We cannot cache pbufs in child socket on Python side,
// until it is created in accept().
STATIC err_t _lwip_tcp_recv_unaccepted(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    return ERR_BUF;
}

// Callback for incoming tcp connections.
STATIC err_t _lwip_tcp_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    lwip_socket_obj_t *socket = (lwip_socket_obj_t*)arg;
    tcp_recv(newpcb, _lwip_tcp_recv_unaccepted);

    if (socket->incoming.connection != NULL) {
        DEBUG_printf("_lwip_tcp_accept: Tried to queue >1 pcb waiting for accept\n");
        // We need to handle this better. This single-level structure makes the
        // backlog setting kind of pointless. FIXME
        return ERR_BUF;
    } else {
        socket->incoming.connection = newpcb;
        exec_user_callback(socket);
        return ERR_OK;
    }
}

// Callback for inbound tcp packets.
STATIC err_t _lwip_tcp_recv(void *arg, struct tcp_pcb *tcpb, struct pbuf *p, err_t err) {
    lwip_socket_obj_t *socket = (lwip_socket_obj_t*)arg;

    if (p == NULL) {
        // Other side has closed connection.
        DEBUG_printf("_lwip_tcp_recv[%p]: other side closed connection\n", socket);
        socket->state = STATE_PEER_CLOSED;
        exec_user_callback(socket);
        return ERR_OK;
    } else if (socket->incoming.pbuf != NULL) {
        // No room in the inn, let LWIP know it's still responsible for delivery later
        return ERR_BUF;
    }
    socket->incoming.pbuf = p;

    exec_user_callback(socket);

    return ERR_OK;
}

/*******************************************************************************/
// Functions for socket send/recieve operations. Socket send/recv and friends call
// these to do the work.

// Helper function for send/sendto to handle UDP packets.
STATIC mp_uint_t lwip_udp_send(lwip_socket_obj_t *socket, const byte *buf, mp_uint_t len, byte *ip, mp_uint_t port, int *_errno) {
    if (len > 0xffff) {
        // Any packet that big is probably going to fail the pbuf_alloc anyway, but may as well try
        len = 0xffff;
    }

    // FIXME: maybe PBUF_ROM?
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
    if (p == NULL) {
        *_errno = ENOMEM;
        return -1;
    }

    memcpy(p->payload, buf, len);

    err_t err;
    if (ip == NULL) {
        err = udp_send(socket->pcb.udp, p);
    } else {
        ip_addr_t dest;
        IP4_ADDR(&dest, ip[0], ip[1], ip[2], ip[3]);
        err = udp_sendto(socket->pcb.udp, p, &dest, port);
    }

    pbuf_free(p);

    // udp_sendto can return 1 on occasion for ESP8266 port.  It's not known why
    // but it seems that the send actually goes through without error in this case.
    // So we treat such cases as a success until further investigation.
    if (err != ERR_OK && err != 1) {
        *_errno = error_lookup_table[-err];
        return -1;
    }

    return len;
}

// Helper function for recv/recvfrom to handle UDP packets
STATIC mp_uint_t lwip_udp_receive(lwip_socket_obj_t *socket, byte *buf, mp_uint_t len, byte *ip, mp_uint_t *port, int *_errno) {

    if (socket->incoming.pbuf == NULL) {
        if (socket->timeout != -1) {
            for (mp_uint_t retries = socket->timeout / 100; retries--;) {
                mp_hal_delay_ms(100);
                if (socket->incoming.pbuf != NULL) break;
            }
            if (socket->incoming.pbuf == NULL) {
                *_errno = ETIMEDOUT;
                return -1;
            }
        } else {
            while (socket->incoming.pbuf == NULL) {
                poll_sockets();
            }
        }
    }

    if (ip != NULL) {
        memcpy(ip, &socket->peer, sizeof(socket->peer));
        *port = socket->peer_port;
    }

    struct pbuf *p = socket->incoming.pbuf;

    u16_t result = pbuf_copy_partial(p, buf, ((p->tot_len > len) ? len : p->tot_len), 0);
    pbuf_free(p);
    socket->incoming.pbuf = NULL;

    return (mp_uint_t) result;
}

// For use in stream virtual methods
#define STREAM_ERROR_CHECK(socket) \
        if (socket->state < 0) { \
            *_errno = error_lookup_table[-socket->state]; \
            return MP_STREAM_ERROR; \
        } \
        assert(socket->pcb.tcp);


// Helper function for send/sendto to handle TCP packets
STATIC mp_uint_t lwip_tcp_send(lwip_socket_obj_t *socket, const byte *buf, mp_uint_t len, int *_errno) {
    // Check for any pending errors
    STREAM_ERROR_CHECK(socket);

    u16_t available = tcp_sndbuf(socket->pcb.tcp);

    if (available == 0) {
        // Non-blocking socket
        if (socket->timeout == 0) {
            *_errno = EAGAIN;
            return MP_STREAM_ERROR;
        }

        mp_uint_t start = mp_hal_ticks_ms();
        // Assume that STATE_PEER_CLOSED may mean half-closed connection, where peer closed it
        // sending direction, but not receiving. Consequently, check for both STATE_CONNECTED
        // and STATE_PEER_CLOSED as normal conditions and still waiting for buffers to be sent.
        // If peer fully closed socket, we would have socket->state set to ERR_RST (connection
        // reset) by error callback.
        // Avoid sending too small packets, so wait until at least 16 bytes available
        while (socket->state >= STATE_CONNECTED && (available = tcp_sndbuf(socket->pcb.tcp)) < 16) {
            if (socket->timeout != -1 && mp_hal_ticks_ms() - start > socket->timeout) {
                *_errno = ETIMEDOUT;
                return MP_STREAM_ERROR;
            }
            poll_sockets();
        }

        // While we waited, something could happen
        STREAM_ERROR_CHECK(socket);
    }

    u16_t write_len = MIN(available, len);

    err_t err = tcp_write(socket->pcb.tcp, buf, write_len, TCP_WRITE_FLAG_COPY);

    if (err != ERR_OK) {
        *_errno = error_lookup_table[-err];
        return MP_STREAM_ERROR;
    }

    return write_len;
}

// Helper function for recv/recvfrom to handle TCP packets
STATIC mp_uint_t lwip_tcp_receive(lwip_socket_obj_t *socket, byte *buf, mp_uint_t len, int *_errno) {
    // Check for any pending errors
    STREAM_ERROR_CHECK(socket);

    if (socket->incoming.pbuf == NULL) {

        // Non-blocking socket
        if (socket->timeout == 0) {
            if (socket->state == STATE_PEER_CLOSED) {
                return 0;
            }
            *_errno = EAGAIN;
            return -1;
        }

        mp_uint_t start = mp_hal_ticks_ms();
        while (socket->state == STATE_CONNECTED && socket->incoming.pbuf == NULL) {
            if (socket->timeout != -1 && mp_hal_ticks_ms() - start > socket->timeout) {
                *_errno = ETIMEDOUT;
                return -1;
            }
            poll_sockets();
        }

        if (socket->state == STATE_PEER_CLOSED) {
            if (socket->incoming.pbuf == NULL) {
                // socket closed and no data left in buffer
                return 0;
            }
        } else if (socket->state != STATE_CONNECTED) {
            assert(socket->state < 0);
            *_errno = error_lookup_table[-socket->state];
            return -1;
        }
    }

    assert(socket->pcb.tcp != NULL);

    struct pbuf *p = socket->incoming.pbuf;

    if (socket->leftover_count == 0) {
        socket->leftover_count = p->tot_len;
    }

    u16_t result = pbuf_copy_partial(p, buf, ((socket->leftover_count >= len) ? len : socket->leftover_count), (p->tot_len - socket->leftover_count));
    if (socket->leftover_count > len) {
        // More left over...
        socket->leftover_count -= len;
    } else {
        pbuf_free(p);
        socket->incoming.pbuf = NULL;
        socket->leftover_count = 0;
    }

    tcp_recved(socket->pcb.tcp, result);
    return (mp_uint_t) result;
}

/*******************************************************************************/
// The socket functions provided by lwip.socket.

STATIC const mp_obj_type_t lwip_socket_type;

void lwip_socket_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    lwip_socket_obj_t *self = self_in;
    mp_printf(print, "<socket state=%d timeout=%d incoming=%p remaining=%d>", self->state, self->timeout,
        self->incoming.pbuf, self->leftover_count);
}

// FIXME: Only supports two arguments at present
mp_obj_t lwip_socket_make_new(const mp_obj_type_t *type, mp_uint_t n_args,
    mp_uint_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 4, false);

    if (!LWIP_INIT) {
      lwip_init();
      LWIP_INIT = true;      
    }
    
    lwip_socket_obj_t *socket = m_new_obj_with_finaliser(lwip_socket_obj_t);
    socket->base.type = (mp_obj_t)&lwip_socket_type;
    socket->domain = MOD_NETWORK_AF_INET;
    socket->type = MOD_NETWORK_SOCK_STREAM;
    socket->callback = MP_OBJ_NULL;
    if (n_args >= 1) {
        socket->domain = mp_obj_get_int(args[0]);
        if (n_args >= 2) {
            socket->type = mp_obj_get_int(args[1]);
        }
    }

    switch (socket->type) {
        case MOD_NETWORK_SOCK_STREAM: socket->pcb.tcp = tcp_new(); break;
        case MOD_NETWORK_SOCK_DGRAM: socket->pcb.udp = udp_new(); break;
        //case MOD_NETWORK_SOCK_RAW: socket->pcb.raw = raw_new(); break;
        default: nlr_raise(mp_obj_new_exception_arg1(&mp_type_OSError, MP_OBJ_NEW_SMALL_INT(EINVAL)));
    }
    if (socket->pcb.tcp == NULL) {
        nlr_raise(mp_obj_new_exception_arg1(&mp_type_OSError, MP_OBJ_NEW_SMALL_INT(ENOMEM)));
    }
    
    switch (socket->type) {
        case MOD_NETWORK_SOCK_STREAM: {
            // Register the socket object as our callback argument.
            tcp_arg(socket->pcb.tcp, (void*)socket);
            // Register our error callback.
            tcp_err(socket->pcb.tcp, _lwip_tcp_error);
            break;
        }
        case MOD_NETWORK_SOCK_DGRAM: {
            // Register our receive callback now. Since UDP sockets don't require binding or connection
            // before use, there's no other good time to do it.
            udp_recv(socket->pcb.udp, _lwip_udp_incoming, (void*)socket);
            break;
        }
    }
    socket->incoming.pbuf = NULL;
    socket->timeout = -1;
    socket->state = STATE_NEW;
    socket->leftover_count = 0;
    return socket;
}

mp_obj_t lwip_socket_close(mp_obj_t self_in) {
    lwip_socket_obj_t *socket = self_in;
    bool socket_is_listener = false;

    if (socket->pcb.tcp == NULL) {
        return mp_const_none;
    }

    switch (socket->type) {
        case MOD_NETWORK_SOCK_STREAM: {
            if (socket->pcb.tcp->state == LISTEN) {
                socket_is_listener = true;
            }
            if (tcp_close(socket->pcb.tcp) != ERR_OK) {
                DEBUG_printf("lwip_close: had to call tcp_abort()\n");
                tcp_abort(socket->pcb.tcp);
            }
            break;
        }
        case MOD_NETWORK_SOCK_DGRAM: udp_remove(socket->pcb.udp); break;
        //case MOD_NETWORK_SOCK_RAW: raw_remove(socket->pcb.raw); break;
    }
    
    socket->pcb.tcp = NULL;
    socket->state = _ERR_BADF;
    if (socket->incoming.pbuf != NULL) {
        if (!socket_is_listener) {
            pbuf_free(socket->incoming.pbuf);
        } else {
            tcp_abort(socket->incoming.connection);
        }
        socket->incoming.pbuf = NULL;
    }

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(lwip_socket_close_obj, lwip_socket_close);

/* Same as version in xenbus.h except this one won't print
 * an error when it doesn't find an entry, so it can be used
 * as a form of fstat */
int lwip_xenbus_read_integer(const char *path)
{
  char *res, *buf;
  int t;

  res = xenbus_read(XBT_NIL, path, &buf);
  if (res) {
    free(res);
    return -1;
  }
  sscanf(buf, "%d", &t);
  free(buf);
  return t;
}

STATIC int lwip_address_bindable(const ip_addr_t *ip)
{
    int i;

    if (ip4_addr_islinklocal(ip))
        return 1; /* local addresses are always fine to bind */

    if (lwip_ether_objs_count == 0)
        return 0; /* no vif installed currently -> we cannot bind anything else */

    if (ip4_addr_isany_val(*ip))
        return 1; /* since we have at least one interface, we are able to bind ANY to it */

    if (ip4_addr_ismulticast(ip))
        return 1; /* since we have at least one interface, we are able to bind a multicast address to it */

    /* check if addr is a broadcast address of a already known vif */
    for (i=0; i<lwip_ether_objs_count; ++i) {
        if (ip4_addr_isbroadcast(ip, &lwip_ether_objs[i].netif))
            return 1; /* a broadcast address is fine to be bind */
    }

    /* check if there exists already an interface with this address */
    for (i=0; i<lwip_ether_objs_count; ++i) {
        if (ip4_addr_cmp(&lwip_ether_objs[i].ip, ip))
            return 1; /* great! it is an interface address */
    }

    return 0; /**/
}


/* Searches through the domain's available vifs to check for a given ip
 * address. If the ip is 0.0.0.0, res will be set to the ip address of
 * the first vif for which an ip is set; if no vif has an ip set then the
 * function resturns -1. If the ip is something other than 0.0.0.0, the
 * function checks to see if there's a vif with an IP that matches it and
 * returns 0 on success and -1 on failure */
STATIC int lwip_find_ip(const char *ip, char *found_ip) {
  char path[256];
  char *msg, *res;
  int n_vifs, id, i, ret;

  /* Get dom ID */
  snprintf(path, sizeof(path), "domid");
  id = lwip_xenbus_read_integer("domid");
  if (id == -1) {
    printk("modlwip: Could not retrieve dom id from Xenstore!\n");
    return -1;
  }

  /* Find out how many vifs the domain has */
  n_vifs = 0;
  while (1) {
    snprintf(path, sizeof(path), "/local/domain/0/backend/vif/%d/%d", id, n_vifs);
    ret = lwip_xenbus_read_integer(path);
    if (ret == -1)
      break;
    n_vifs++;
  }
    
  /* We use dir only to know how many vifs there are. For each
   * we check if an entry "ip" exists. If it does and the given
   * IP is 0.0.0.0, we return the first entry. If it the IP is 
   * something else, we try to match it to the entry's IP. If no
   * ip entry exists we skip the vif. */
  for (i = 0; i <= n_vifs; i++) {

    /* See if this vif has an ip entry */
    snprintf(path,
	     sizeof(path),
	     "/local/domain/0/backend/vif/%d/%d/ip",
	     id,
	     i);
    msg = xenbus_read(XBT_NIL, path, &res);

    /* It does! */
    if (!msg) {
      /* If given ip is 0.0.0.0, return any ip, i.e., the one we
       * just found. If multiple vifs exist then the last one with
       * an ip is used. */
      if (strcmp(ip, "0.0.0.0") == 0) {
	strncpy(found_ip, res, strlen(res));
	free(msg);
	free(res);
	return 0;
      }
      /* See if this vif's ip matches the given ip. If it does,
         return success (i.e., no need to search for other vifs) */
      else if (strcmp(ip, res) == 0) {
	strncpy(found_ip, res, strlen(res));
	free(msg);
	free(res);
	return 0;
      }
    }
    else {
      /* We don't free res since xenbus_read sets it to NULL on error */
      free(msg);
    }
  }

  return -1;
}

/* Searches through the domain's available vifs to check for an vif without
 * an IP set. It returns the vif id of the first vif found.
 * The first vif to be first looked at can be specified by offset
 * The function returns the -1 if no more vifs were found */
STATIC int lwip_find_next_noip(int offset) {
  char path[256];
  char *msg, *res;
  int n_vifs, id, i, ret;

  /* Get dom ID */
  snprintf(path, sizeof(path), "domid");
  id = lwip_xenbus_read_integer("domid");
  if (id == -1) {
    printk("modlwip: Could not retrieve dom id from Xenstore!\n");
    return -1;
  }

  /* Find out how many vifs the domain has */
  n_vifs = 0;
  while (1) {
    snprintf(path, sizeof(path), "/local/domain/0/backend/vif/%d/%d", id, n_vifs);
    ret = lwip_xenbus_read_integer(path);
    if (ret == -1)
      break;
    n_vifs++;
  }

  /* We use dir only to know how many vifs there are. For each
   * we check if an entry "ip" exists. If it does we skip the interface
   */
  for (i = offset; i <= n_vifs; i++) {

    /* See if this vif has an ip entry */
    snprintf(path,
	     sizeof(path),
	     "/local/domain/0/backend/vif/%d/%d/ip",
	     id,
	     i);
    msg = xenbus_read(XBT_NIL, path, &res);

    if (!msg) {
        /* It does, so we skip this interface */
        free(msg);
	free(res);
        continue;
    } else {
        /* We don't free res since xenbus_read sets it to NULL on error */
        free(msg);
        return i;
    }
  }
  return -1; /* no more interfaces */
}


mp_obj_t lwip_socket_bind(mp_obj_t self_in, mp_obj_t addr_in) {
    lwip_socket_obj_t *socket = self_in;

    uint8_t ip[NETUTILS_IPV4ADDR_BUFSIZE];
    mp_uint_t port = netutils_parse_inet_addr(addr_in, ip, NETUTILS_BIG);

    ip_addr_t bind_addr;
    IP4_ADDR(&bind_addr, ip[0], ip[1], ip[2], ip[3]);

#if 1
    /* Add interface, if bind_addr is non-existing */
    ip_addr_t bind_mask;
    ip_addr_t bind_gw;

    IP4_ADDR(&bind_mask, 255,255,255,0);
    IP4_ADDR(&bind_gw, 0,0,0,0);


    /* If there is no suitable interface yet added for doing the socket bind,
     * we will try to add (another) one */
    if ((!lwip_address_bindable(&bind_addr)) &&
        (!lwip_addif(&bind_addr, &bind_mask, &bind_gw))) {
        printk("modlwip: Error while implicitly adding interface!\n");
        return NULL;
    }
#endif

    err_t err = ERR_ARG;
    switch (socket->type) {
        case MOD_NETWORK_SOCK_STREAM: {
            err = tcp_bind(socket->pcb.tcp, &bind_addr, port);
            break;
        }
        case MOD_NETWORK_SOCK_DGRAM: {
            err = udp_bind(socket->pcb.udp, &bind_addr, port);
            break;
        }
    }

    if (err != ERR_OK) {
        nlr_raise(mp_obj_new_exception_arg1(&mp_type_OSError, MP_OBJ_NEW_SMALL_INT(error_lookup_table[-err])));
    }

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(lwip_socket_bind_obj, lwip_socket_bind);

mp_obj_t lwip_socket_listen(mp_obj_t self_in, mp_obj_t backlog_in) {
    lwip_socket_obj_t *socket = self_in;
    mp_int_t backlog = mp_obj_get_int(backlog_in);

    if (socket->pcb.tcp == NULL) {
        nlr_raise(mp_obj_new_exception_arg1(&mp_type_OSError, MP_OBJ_NEW_SMALL_INT(EBADF)));
    }
    if (socket->type != MOD_NETWORK_SOCK_STREAM) {
        nlr_raise(mp_obj_new_exception_arg1(&mp_type_OSError, MP_OBJ_NEW_SMALL_INT(EOPNOTSUPP)));
    }

    struct tcp_pcb *new_pcb = tcp_listen_with_backlog(socket->pcb.tcp, (u8_t)backlog);
    if (new_pcb == NULL) {
        nlr_raise(mp_obj_new_exception_arg1(&mp_type_OSError, MP_OBJ_NEW_SMALL_INT(ENOMEM)));
    }
    socket->pcb.tcp = new_pcb;
    tcp_accept(new_pcb, _lwip_tcp_accept);

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(lwip_socket_listen_obj, lwip_socket_listen);

mp_obj_t lwip_socket_accept(mp_obj_t self_in) {
    lwip_socket_obj_t *socket = self_in;

    if (socket->pcb.tcp == NULL) {
        nlr_raise(mp_obj_new_exception_arg1(&mp_type_OSError, MP_OBJ_NEW_SMALL_INT(EBADF)));
    }
    if (socket->type != MOD_NETWORK_SOCK_STREAM) {
        nlr_raise(mp_obj_new_exception_arg1(&mp_type_OSError, MP_OBJ_NEW_SMALL_INT(EOPNOTSUPP)));
    }
    // I need to do this because "tcp_accepted", later, is a macro.
    struct tcp_pcb *listener = socket->pcb.tcp;
    if (listener->state != LISTEN) {
        nlr_raise(mp_obj_new_exception_arg1(&mp_type_OSError, MP_OBJ_NEW_SMALL_INT(EINVAL)));
    }

    // accept incoming connection
    if (socket->incoming.connection == NULL) {
        if (socket->timeout != -1) {
            for (mp_uint_t retries = socket->timeout / 100; retries--;) {
                mp_hal_delay_ms(100);
                if (socket->incoming.connection != NULL) break;
            }
            if (socket->incoming.connection == NULL) {
                nlr_raise(mp_obj_new_exception_arg1(&mp_type_OSError, MP_OBJ_NEW_SMALL_INT(ETIMEDOUT)));
            }
        } else {
            while (socket->incoming.connection == NULL) {
	      poll_sockets();
            }
        }
    }

    // create new socket object
    lwip_socket_obj_t *socket2 = m_new_obj_with_finaliser(lwip_socket_obj_t);
    socket2->base.type = (mp_obj_t)&lwip_socket_type;

    // We get a new pcb handle...
    socket2->pcb.tcp = socket->incoming.connection;
    socket->incoming.connection = NULL;

    // ...and set up the new socket for it.
    socket2->domain = MOD_NETWORK_AF_INET;
    socket2->type = MOD_NETWORK_SOCK_STREAM;
    socket2->incoming.pbuf = NULL;
    socket2->timeout = socket->timeout;
    socket2->state = STATE_CONNECTED;
    socket2->leftover_count = 0;
    socket2->callback = MP_OBJ_NULL;
    tcp_arg(socket2->pcb.tcp, (void*)socket2);
    tcp_err(socket2->pcb.tcp, _lwip_tcp_error);
    tcp_recv(socket2->pcb.tcp, _lwip_tcp_recv);

    tcp_accepted(listener);

    // make the return value
    uint8_t ip[NETUTILS_IPV4ADDR_BUFSIZE];
    memcpy(ip, &(socket2->pcb.tcp->remote_ip), sizeof(ip));
    mp_uint_t port = (mp_uint_t)socket2->pcb.tcp->remote_port;
    mp_obj_tuple_t *client = mp_obj_new_tuple(2, NULL);
    client->items[0] = socket2;
    client->items[1] = netutils_format_inet_addr(ip, port, NETUTILS_BIG);

    return client;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(lwip_socket_accept_obj, lwip_socket_accept);

mp_obj_t lwip_socket_connect(mp_obj_t self_in, mp_obj_t addr_in) {
    ip_addr_t dest;
    lwip_socket_obj_t *socket = self_in;

    if (socket->pcb.tcp == NULL) {
        nlr_raise(mp_obj_new_exception_arg1(&mp_type_OSError, MP_OBJ_NEW_SMALL_INT(EBADF)));
    }

    // get address
    uint8_t ip[NETUTILS_IPV4ADDR_BUFSIZE];
    mp_uint_t port = netutils_parse_inet_addr(addr_in, ip, NETUTILS_BIG);
    IP4_ADDR(&dest, ip[0], ip[1], ip[2], ip[3]);

#if 1
    /* Add interface, if we don't have one yet */
    /* TODO/FIXME: Add interface if one in Xenstore specifies everything required to route traffic to dest */
    ip_addr_t bind_addr;
    ip_addr_t bind_mask;
    ip_addr_t bind_gw;

    IP4_ADDR(&bind_addr, 0,0,0,0); /* let lwip_addif() select a proper IP */
    IP4_ADDR(&bind_mask, 255,255,255,0);
    IP4_ADDR(&bind_gw, 0,0,0,0);

    if ((lwip_ether_objs_count == 0) &&
        (!lwip_addif(&bind_addr, &bind_mask, &bind_mask))) {
        printk("modlwip: Error while implicitly adding interface!\n");
        return NULL;
    }
#endif

    err_t err = ERR_ARG;
    switch (socket->type) {
        case MOD_NETWORK_SOCK_STREAM: {
            if (socket->state != STATE_NEW) {
                if (socket->state == STATE_CONNECTED) {
                    nlr_raise(mp_obj_new_exception_arg1(&mp_type_OSError, MP_OBJ_NEW_SMALL_INT(EALREADY)));
                } else {
                    nlr_raise(mp_obj_new_exception_arg1(&mp_type_OSError, MP_OBJ_NEW_SMALL_INT(EINPROGRESS)));
                }
            }
            // Register our recieve callback.
            tcp_recv(socket->pcb.tcp, _lwip_tcp_recv);
            socket->state = STATE_CONNECTING;
            err = tcp_connect(socket->pcb.tcp, &dest, port, _lwip_tcp_connected);
            if (err != ERR_OK) {
                socket->state = STATE_NEW;
                nlr_raise(mp_obj_new_exception_arg1(&mp_type_OSError, MP_OBJ_NEW_SMALL_INT(error_lookup_table[-err])));
            }
            socket->peer_port = (mp_uint_t)port;
            memcpy(socket->peer, &dest, sizeof(socket->peer));
            // And now we wait...
            if (socket->timeout != -1) {
                for (mp_uint_t retries = socket->timeout / 100; retries--;) {
                    mp_hal_delay_ms(100);
                    if (socket->state != STATE_CONNECTING) break;
                }
                if (socket->state == STATE_CONNECTING) {
                    nlr_raise(mp_obj_new_exception_arg1(&mp_type_OSError, MP_OBJ_NEW_SMALL_INT(ETIMEDOUT)));
                }
            } else {
                while (socket->state == STATE_CONNECTING) {
                    poll_sockets();
                }
            }
            if (socket->state == STATE_CONNECTED) {
               err = ERR_OK;
            } else {
               err = socket->state;
            }
            break;
        }
        case MOD_NETWORK_SOCK_DGRAM: {
            err = udp_connect(socket->pcb.udp, &dest, port);
            break;
        }
    }

    if (err != ERR_OK) {
        nlr_raise(mp_obj_new_exception_arg1(&mp_type_OSError, MP_OBJ_NEW_SMALL_INT(error_lookup_table[-err])));
    }

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(lwip_socket_connect_obj, lwip_socket_connect);

void lwip_socket_check_connected(lwip_socket_obj_t *socket) {
    if (socket->pcb.tcp == NULL) {
        // not connected
        int _errno = error_lookup_table[-socket->state];
        socket->state = _ERR_BADF;
        nlr_raise(mp_obj_new_exception_arg1(&mp_type_OSError, MP_OBJ_NEW_SMALL_INT(_errno)));
    }
}

mp_obj_t lwip_socket_send(mp_obj_t self_in, mp_obj_t buf_in) {
    lwip_socket_obj_t *socket = self_in;
    int _errno;
    lwip_socket_check_connected(socket);

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buf_in, &bufinfo, MP_BUFFER_READ);
    mp_uint_t ret = 0;
    switch (socket->type) {
        case MOD_NETWORK_SOCK_STREAM: {
            ret = lwip_tcp_send(socket, bufinfo.buf, bufinfo.len, &_errno);
            break;
        }
        case MOD_NETWORK_SOCK_DGRAM: {
            ret = lwip_udp_send(socket, bufinfo.buf, bufinfo.len, NULL, 0, &_errno);
            break;
        }
    }
    if (ret == -1) {
        nlr_raise(mp_obj_new_exception_arg1(&mp_type_OSError, MP_OBJ_NEW_SMALL_INT(_errno)));
    }
    
    return mp_obj_new_int_from_uint(ret);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(lwip_socket_send_obj, lwip_socket_send);

mp_obj_t lwip_socket_recv(mp_obj_t self_in, mp_obj_t len_in) {
    lwip_socket_obj_t *socket = self_in;
    int _errno;

    lwip_socket_check_connected(socket);

    mp_int_t len = mp_obj_get_int(len_in);
    vstr_t vstr;
    vstr_init_len(&vstr, len);

    mp_uint_t ret = 0;
    switch (socket->type) {
        case MOD_NETWORK_SOCK_STREAM: {
            ret = lwip_tcp_receive(socket, (byte*)vstr.buf, len, &_errno);
	    
            break;
        }
        case MOD_NETWORK_SOCK_DGRAM: {
            ret = lwip_udp_receive(socket, (byte*)vstr.buf, len, NULL, NULL, &_errno);
            break;
        }
    }
    if (ret == -1) {
        nlr_raise(mp_obj_new_exception_arg1(&mp_type_OSError, MP_OBJ_NEW_SMALL_INT(_errno)));
    }

    if (ret == 0) {
        return mp_const_empty_bytes;
    }
    vstr.len = ret;
    return mp_obj_new_str_from_vstr(&mp_type_bytes, &vstr);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(lwip_socket_recv_obj, lwip_socket_recv);

mp_obj_t lwip_socket_sendto(mp_obj_t self_in, mp_obj_t data_in, mp_obj_t addr_in) {
    lwip_socket_obj_t *socket = self_in;
    int _errno;

    lwip_socket_check_connected(socket);

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_in, &bufinfo, MP_BUFFER_READ);

    uint8_t ip[NETUTILS_IPV4ADDR_BUFSIZE];
    mp_uint_t port = netutils_parse_inet_addr(addr_in, ip, NETUTILS_BIG);

    mp_uint_t ret = 0;
    switch (socket->type) {
        case MOD_NETWORK_SOCK_STREAM: {
            ret = lwip_tcp_send(socket, bufinfo.buf, bufinfo.len, &_errno);
            break;
        }
        case MOD_NETWORK_SOCK_DGRAM: {
            ret = lwip_udp_send(socket, bufinfo.buf, bufinfo.len, ip, port, &_errno);
            break;
        }
    }
    if (ret == -1) {
        nlr_raise(mp_obj_new_exception_arg1(&mp_type_OSError, MP_OBJ_NEW_SMALL_INT(_errno)));
    }

    return mp_obj_new_int_from_uint(ret);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_3(lwip_socket_sendto_obj, lwip_socket_sendto);

mp_obj_t lwip_socket_recvfrom(mp_obj_t self_in, mp_obj_t len_in) {
    lwip_socket_obj_t *socket = self_in;
    int _errno;

    lwip_socket_check_connected(socket);

    mp_int_t len = mp_obj_get_int(len_in);
    vstr_t vstr;
    vstr_init_len(&vstr, len);
    byte ip[4];
    mp_uint_t port;

    mp_uint_t ret = 0;
    switch (socket->type) {
        case MOD_NETWORK_SOCK_STREAM: {
            memcpy(ip, &socket->peer, sizeof(socket->peer));
            port = (mp_uint_t) socket->peer_port;
            ret = lwip_tcp_receive(socket, (byte*)vstr.buf, len, &_errno);
            break;
        }
        case MOD_NETWORK_SOCK_DGRAM: {
            ret = lwip_udp_receive(socket, (byte*)vstr.buf, len, ip, &port, &_errno);
            break;
        }
    }
    if (ret == -1) {
        nlr_raise(mp_obj_new_exception_arg1(&mp_type_OSError, MP_OBJ_NEW_SMALL_INT(_errno)));
    }

    mp_obj_t tuple[2];
    if (ret == 0) {
        tuple[0] = mp_const_empty_bytes;
    } else {
        vstr.len = ret;
        tuple[0] = mp_obj_new_str_from_vstr(&mp_type_bytes, &vstr);
    }
    tuple[1] = netutils_format_inet_addr(ip, port, NETUTILS_BIG);
    return mp_obj_new_tuple(2, tuple);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(lwip_socket_recvfrom_obj, lwip_socket_recvfrom);

mp_obj_t lwip_socket_sendall(mp_obj_t self_in, mp_obj_t buf_in) {
    lwip_socket_obj_t *socket = self_in;
    lwip_socket_check_connected(socket);

    int _errno;
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buf_in, &bufinfo, MP_BUFFER_READ);

    mp_uint_t ret = 0;
    switch (socket->type) {
        case MOD_NETWORK_SOCK_STREAM: {
            if (socket->timeout == 0) {
                // Behavior of sendall() for non-blocking sockets isn't explicitly specified.
                // But it's specified that "On error, an exception is raised, there is no
                // way to determine how much data, if any, was successfully sent." Then, the
                // most useful behavior is: check whether we will be able to send all of input
                // data without EAGAIN, and if won't be, raise it without sending any.
                if (bufinfo.len > tcp_sndbuf(socket->pcb.tcp)) {
                    nlr_raise(mp_obj_new_exception_arg1(&mp_type_OSError, MP_OBJ_NEW_SMALL_INT(EAGAIN)));
                }
            }
            // TODO: In CPython3.5, socket timeout should apply to the
            // entire sendall() operation, not to individual send() chunks.
            while (bufinfo.len != 0) {
                ret = lwip_tcp_send(socket, bufinfo.buf, bufinfo.len, &_errno);
                if (ret == -1) {
                    nlr_raise(mp_obj_new_exception_arg1(&mp_type_OSError, MP_OBJ_NEW_SMALL_INT(_errno)));
                }
                bufinfo.len -= ret;
                bufinfo.buf = (char*)bufinfo.buf + ret;
            }
            break;
        }
        case MOD_NETWORK_SOCK_DGRAM:
            mp_not_implemented("");
            break;
    }

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(lwip_socket_sendall_obj, lwip_socket_sendall);

mp_obj_t lwip_socket_settimeout(mp_obj_t self_in, mp_obj_t timeout_in) {
    lwip_socket_obj_t *socket = self_in;
    mp_uint_t timeout;
    if (timeout_in == mp_const_none) {
        timeout = -1;
    } else {
        #if MICROPY_PY_BUILTINS_FLOAT
        timeout = 1000 * mp_obj_get_float(timeout_in);
        #else
        timeout = 1000 * mp_obj_get_int(timeout_in);
        #endif
    }
    socket->timeout = timeout;
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(lwip_socket_settimeout_obj, lwip_socket_settimeout);

mp_obj_t lwip_socket_setblocking(mp_obj_t self_in, mp_obj_t flag_in) {
    lwip_socket_obj_t *socket = self_in;
    bool val = mp_obj_is_true(flag_in);
    if (val) {
        socket->timeout = -1;
    } else {
        socket->timeout = 0;
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(lwip_socket_setblocking_obj, lwip_socket_setblocking);

mp_obj_t lwip_socket_setsockopt(mp_uint_t n_args, const mp_obj_t *args) {
    (void)n_args; // always 4
    lwip_socket_obj_t *socket = args[0];

    int opt = mp_obj_get_int(args[2]);
    if (opt == 20) {
        if (args[3] == mp_const_none) {
            socket->callback = MP_OBJ_NULL;
        } else {
            socket->callback = args[3];
        }
        return mp_const_none;
    }

    // Integer options
    mp_int_t val = mp_obj_get_int(args[3]);
    switch (opt) {
        case SOF_REUSEADDR:
            // Options are common for UDP and TCP pcb's.
            if (val) {
                ip_set_option(socket->pcb.tcp, SOF_REUSEADDR);
            } else {
                ip_reset_option(socket->pcb.tcp, SOF_REUSEADDR);
            }
            break;
        default:
            printf("Warning: lwip.setsockopt() not implemented\n");
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(lwip_socket_setsockopt_obj, 4, 4, lwip_socket_setsockopt);

mp_obj_t lwip_socket_makefile(mp_uint_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return args[0];
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(lwip_socket_makefile_obj, 1, 3, lwip_socket_makefile);

mp_uint_t lwip_socket_read(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode) {
    lwip_socket_obj_t *socket = self_in;

    switch (socket->type) {
        case MOD_NETWORK_SOCK_STREAM:
            return lwip_tcp_receive(socket, buf, size, errcode);
        case MOD_NETWORK_SOCK_DGRAM:
            return lwip_udp_receive(socket, buf, size, NULL, NULL, errcode);
    }
    // Unreachable
    return MP_STREAM_ERROR;
}

mp_uint_t lwip_socket_write(mp_obj_t self_in, const void *buf, mp_uint_t size, int *errcode) {
    lwip_socket_obj_t *socket = self_in;

    switch (socket->type) {
        case MOD_NETWORK_SOCK_STREAM:
            return lwip_tcp_send(socket, buf, size, errcode);
        case MOD_NETWORK_SOCK_DGRAM:
            return lwip_udp_send(socket, buf, size, NULL, 0, errcode);
    }

    // Unreachable
    return MP_STREAM_ERROR;
}

STATIC const mp_map_elem_t lwip_socket_locals_dict_table[] = {
    { MP_OBJ_NEW_QSTR(MP_QSTR___del__), (mp_obj_t)&lwip_socket_close_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_close), (mp_obj_t)&lwip_socket_close_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_bind), (mp_obj_t)&lwip_socket_bind_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_listen), (mp_obj_t)&lwip_socket_listen_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_accept), (mp_obj_t)&lwip_socket_accept_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_connect), (mp_obj_t)&lwip_socket_connect_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_send), (mp_obj_t)&lwip_socket_send_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_recv), (mp_obj_t)&lwip_socket_recv_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_sendto), (mp_obj_t)&lwip_socket_sendto_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_recvfrom), (mp_obj_t)&lwip_socket_recvfrom_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_sendall), (mp_obj_t)&lwip_socket_sendall_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_settimeout), (mp_obj_t)&lwip_socket_settimeout_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_setblocking), (mp_obj_t)&lwip_socket_setblocking_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_setsockopt), (mp_obj_t)&lwip_socket_setsockopt_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_makefile), (mp_obj_t)&lwip_socket_makefile_obj },

    { MP_OBJ_NEW_QSTR(MP_QSTR_read), (mp_obj_t)&mp_stream_read_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_readline), (mp_obj_t)&mp_stream_unbuffered_readline_obj},
    { MP_OBJ_NEW_QSTR(MP_QSTR_write), (mp_obj_t)&mp_stream_write_obj },
};
STATIC MP_DEFINE_CONST_DICT(lwip_socket_locals_dict, lwip_socket_locals_dict_table);

STATIC const mp_stream_p_t lwip_socket_stream_p = {
    .read = lwip_socket_read,
    .write = lwip_socket_write,
};

STATIC const mp_obj_type_t lwip_socket_type = {
    { &mp_type_type },
    .name = MP_QSTR_socket,
    .print = lwip_socket_print,
    .make_new = lwip_socket_make_new,
    .stream_p = &lwip_socket_stream_p,
    .locals_dict = (mp_obj_t)&lwip_socket_locals_dict,
};

/******************************************************************************/
// Support functions for memory protection. lwIP has its own memory management
// routines for its internal structures, and since they might be called in
// interrupt handlers, they need some protection.
sys_prot_t sys_arch_protect() {
    return (sys_prot_t)MICROPY_BEGIN_ATOMIC_SECTION();
}

void sys_arch_unprotect(sys_prot_t state) {
    MICROPY_END_ATOMIC_SECTION((mp_uint_t)state);
}

/******************************************************************************/
// Polling callbacks for the interfaces connected to lwIP. Right now it calls
// itself a "list" but isn't; we only support a single interface.

typedef struct nic_poll {
    void (* poll)(void *arg);
    void *poll_arg;
} nic_poll_t;

STATIC nic_poll_t lwip_poll_list;

void mod_lwip_register_poll(void (* poll)(void *arg), void *poll_arg) {
    lwip_poll_list.poll = poll;
    lwip_poll_list.poll_arg = poll_arg;
}

void mod_lwip_deregister_poll(void (* poll)(void *arg), void *poll_arg) {
    lwip_poll_list.poll = NULL;
}


/**
 * ARGUMENT PARSING
 */
/*
struct mcargs {
  struct eth_addr mac;
  struct netif    netif;  
  ip4_addr_t      ip;
  ip4_addr_t      mask;
  ip4_addr_t      gw;
  #if LWIP_DNS
  ip4_addr_t      dns0;
  ip4_addr_t      dns1;
  #endif
} args;
*/

/******************************************************************************/
// The lwip global functions.
/*
STATIC mp_obj_t mod_lwip_netifadd(mp_obj_t ip, mp_obj_t mask, mp_obj_t gw) {
  struct netif *niret;
  
  if (!ipaddr_aton(mp_obj_str_get_str(ip), &args.ip)) {
    nlr_raise(mp_obj_new_exception_msg(&mp_type_ValueError, "not a valid IP address"));
  }
  if (!ipaddr_aton(mp_obj_str_get_str(mask), &args.mask)) {
    nlr_raise(mp_obj_new_exception_msg(&mp_type_ValueError, "not a valid mask"));
  }
  if (!ipaddr_aton(mp_obj_str_get_str(gw), &args.gw)) {
    nlr_raise(mp_obj_new_exception_msg(&mp_type_ValueError, "not a valid gateway"));
  }    
  
  niret = netif_add(&args.netif, &args.ip, &args.mask, &args.gw, NULL,
  	            netfrontif_init, ethernet_input);
  netif_set_default(&args.netif);
  netif_set_up(&args.netif);
  return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_3(mod_lwip_netifadd_obj, mod_lwip_netifadd);
*/

STATIC mp_obj_t mod_lwip_reset() {
    lwip_init();
    lwip_poll_list.poll = NULL;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(mod_lwip_reset_obj, mod_lwip_reset);

/*
STATIC mp_obj_t mod_lwip_callback() {
    if (lwip_poll_list.poll != NULL) {
        lwip_poll_list.poll(lwip_poll_list.poll_arg);
    }
    sys_check_timeouts();
    return mp_const_none;
    }*/

//MP_DEFINE_CONST_FUN_OBJ_0(mod_lwip_callback_obj, mod_lwip_callback);

typedef struct _getaddrinfo_state_t {
    volatile int status;
    volatile ip_addr_t ipaddr;
} getaddrinfo_state_t;

// Callback for incoming DNS requests.
STATIC void lwip_getaddrinfo_cb(const char *name, ip_addr_t *ipaddr, void *arg) {
    getaddrinfo_state_t *state = arg;
    if (ipaddr != NULL) {
        state->status = 1;
        state->ipaddr = *ipaddr;
    } else {
        // error
        state->status = -2;
    }
}

// lwip.getaddrinfo
mp_obj_t lwip_getaddrinfo(mp_obj_t host_in, mp_obj_t port_in) {
    mp_uint_t hlen;
    const char *host = mp_obj_str_get_data(host_in, &hlen);
    mp_int_t port = mp_obj_get_int(port_in);

    getaddrinfo_state_t state;
    state.status = 0;

    err_t ret = dns_gethostbyname(host, (ip_addr_t*)&state.ipaddr, lwip_getaddrinfo_cb, &state);
    switch (ret) {
        case ERR_OK:
            // cached
            state.status = 1;
            break;
        case ERR_INPROGRESS:
            while (state.status == 0) {
                poll_sockets();
            }
            break;
        default:
            state.status = ret;
    }

    if (state.status < 0) {
        // TODO: CPython raises gaierror, we raise with native lwIP negative error
        // values, to differentiate from normal errno's at least in such way.
        nlr_raise(mp_obj_new_exception_arg1(&mp_type_OSError, MP_OBJ_NEW_SMALL_INT(state.status)));
    }
    
    mp_obj_tuple_t *tuple = mp_obj_new_tuple(5, NULL);
    tuple->items[0] = MP_OBJ_NEW_SMALL_INT(MOD_NETWORK_AF_INET);
    tuple->items[1] = MP_OBJ_NEW_SMALL_INT(MOD_NETWORK_SOCK_STREAM);
    tuple->items[2] = MP_OBJ_NEW_SMALL_INT(0);
    tuple->items[3] = MP_OBJ_NEW_QSTR(MP_QSTR_);
    tuple->items[4] = netutils_format_inet_addr((uint8_t*)&state.ipaddr, port, NETUTILS_BIG);
    return mp_obj_new_list(1, (mp_obj_t*)&tuple);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(lwip_getaddrinfo_obj, lwip_getaddrinfo);

// Debug functions

STATIC mp_obj_t lwip_print_pcbs() {
    tcp_debug_print_pcbs();
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(lwip_print_pcbs_obj, lwip_print_pcbs);

#ifdef MICROPY_PY_LWIP

STATIC const mp_map_elem_t mp_module_lwip_globals_table[] = {
    { MP_OBJ_NEW_QSTR(MP_QSTR___name__), MP_OBJ_NEW_QSTR(MP_QSTR_lwip) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_reset), (mp_obj_t)&mod_lwip_reset_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_getaddrinfo), (mp_obj_t)&lwip_getaddrinfo_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_print_pcbs), (mp_obj_t)&lwip_print_pcbs_obj },
    //    { MP_OBJ_NEW_QSTR(MP_QSTR_netifadd), (mp_obj_t)&mod_lwip_netifadd_obj },
    //{ MP_OBJ_NEW_QSTR(MP_QSTR_poll), (mp_obj_t)&mod_lwip_poll_obj },        
    // objects
    { MP_OBJ_NEW_QSTR(MP_QSTR_socket), (mp_obj_t)&lwip_socket_type },
#ifdef MICROPY_PY_LWIP_SLIP
    { MP_OBJ_NEW_QSTR(MP_QSTR_slip), (mp_obj_t)&lwip_slip_type },
#endif
    { MP_OBJ_NEW_QSTR(MP_QSTR_ether), (mp_obj_t)&lwip_ether_type },    
    // class constants
    { MP_OBJ_NEW_QSTR(MP_QSTR_AF_INET), MP_OBJ_NEW_SMALL_INT(MOD_NETWORK_AF_INET) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_AF_INET6), MP_OBJ_NEW_SMALL_INT(MOD_NETWORK_AF_INET6) },

    { MP_OBJ_NEW_QSTR(MP_QSTR_SOCK_STREAM), MP_OBJ_NEW_SMALL_INT(MOD_NETWORK_SOCK_STREAM) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_SOCK_DGRAM), MP_OBJ_NEW_SMALL_INT(MOD_NETWORK_SOCK_DGRAM) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_SOCK_RAW), MP_OBJ_NEW_SMALL_INT(MOD_NETWORK_SOCK_RAW) },

    { MP_OBJ_NEW_QSTR(MP_QSTR_SOL_SOCKET), MP_OBJ_NEW_SMALL_INT(1) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_SO_REUSEADDR), MP_OBJ_NEW_SMALL_INT(SOF_REUSEADDR) },
};

STATIC MP_DEFINE_CONST_DICT(mp_module_lwip_globals, mp_module_lwip_globals_table);

const mp_obj_module_t mp_module_lwip = {
    .base = { &mp_type_module },
    .name = MP_QSTR_lwip,
    .globals = (mp_obj_dict_t*)&mp_module_lwip_globals,
};

#endif // MICROPY_PY_LWIP
