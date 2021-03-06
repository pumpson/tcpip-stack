#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "ip.h"
#include "util.h"

// TODO: user timeout should set by user
#define USER_TIMEOUT (20)          /* user timeout (seconds) */
#define TIME_WAIT_TIMEOUT (2 * 10) /* TIME_WAIT timeout (seconds) */
#define TCP_SND_BUF_SIZE (10 * 1024)

#define TCP_CB_TABLE_SIZE 128
#define TCP_SOURCE_PORT_MIN 49152
#define TCP_SOURCE_PORT_MAX 65535

#define TCP_CB_STATE_CLOSED 0
#define TCP_CB_STATE_LISTEN 1
#define TCP_CB_STATE_SYN_SENT 2
#define TCP_CB_STATE_SYN_RCVD 3
#define TCP_CB_STATE_ESTABLISHED 4
#define TCP_CB_STATE_FIN_WAIT1 5
#define TCP_CB_STATE_FIN_WAIT2 6
#define TCP_CB_STATE_CLOSING 7
#define TCP_CB_STATE_TIME_WAIT 8
#define TCP_CB_STATE_CLOSE_WAIT 9
#define TCP_CB_STATE_LAST_ACK 10

#define TCP_FLG_FIN 0x01
#define TCP_FLG_SYN 0x02
#define TCP_FLG_RST 0x04
#define TCP_FLG_PSH 0x08
#define TCP_FLG_ACK 0x10
#define TCP_FLG_URG 0x20

#define TCP_FLG_IS(x, y) (((x)&0x3f) == (y))
#define TCP_FLG_ISSET(x, y) (((x)&0x3f) & (y))

#define TCP_HDR_LEN(hdr) (((hdr)->off >> 4) << 2)
#define TCP_DATA_LEN(hdr, len) ((len)-TCP_HDR_LEN(hdr))

#define IS_FREE_CB(cb) (!(cb)->used && (cb)->state == TCP_CB_STATE_CLOSED)

#define TCP_SOCKET_INVALID(x) ((x) < 0 || (x) >= TCP_CB_TABLE_SIZE)

#ifndef TCP_DEBUG
#ifdef DEBUG
#define TCP_DEBUG 1
#endif
#endif

struct tcp_hdr {
  uint16_t src;
  uint16_t dst;
  uint32_t seq;
  uint32_t ack;
  uint8_t off;
  uint8_t flg;
  uint16_t win;
  uint16_t sum;
  uint16_t urg;
};

struct tcp_txq_entry {
  struct tcp_hdr *segment;
  uint16_t len;
  struct timeval timestamp;
  struct tcp_txq_entry *next;
};

struct tcp_txq_head {
  struct tcp_txq_entry *head;
  struct tcp_txq_entry *tail;
  uint16_t snt;
};

struct tcp_cb {
  uint8_t used;
  uint8_t state;
  struct netif *iface;
  uint16_t port;  // network byte order
  struct {
    ip_addr_t addr;
    uint16_t port;  // network byte order
  } peer;
  struct {
    uint32_t nxt;
    uint32_t una;
    uint16_t up;
    uint32_t wl1;
    uint32_t wl2;
    uint16_t wnd;
  } snd;
  uint32_t iss;
  struct {
    uint32_t nxt;
    uint16_t up;
    uint16_t wnd;
  } rcv;
  uint32_t irs;
  struct tcp_txq_head txq;
  uint8_t window[65535];
  struct tcp_cb *parent;
  struct queue_head backlog;
  pthread_cond_t cond;
  long timeout;
};

static struct tcp_cb cb_table[TCP_CB_TABLE_SIZE];
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t timer_thread;
static pthread_cond_t timer_cond;

static ssize_t tcp_tx(struct tcp_cb *cb, uint32_t seq, uint32_t ack,
                      uint8_t flg, struct timeval *now, uint8_t *buf,
                      size_t len);

static char *tcp_flg_ntop(uint8_t flg, char *buf, int len) {
  int i = 0;
  if (TCP_FLG_ISSET(flg, TCP_FLG_FIN)) {
    buf[i++] = 'F';
  }
  if (TCP_FLG_ISSET(flg, TCP_FLG_SYN)) {
    buf[i++] = 'S';
  }
  if (TCP_FLG_ISSET(flg, TCP_FLG_RST)) {
    buf[i++] = 'R';
  }
  if (TCP_FLG_ISSET(flg, TCP_FLG_PSH)) {
    buf[i++] = 'P';
  }
  if (TCP_FLG_ISSET(flg, TCP_FLG_ACK)) {
    buf[i++] = 'A';
  }
  if (TCP_FLG_ISSET(flg, TCP_FLG_URG)) {
    buf[i++] = 'U';
  }
  buf[i] = 0;
  return buf;
}

static char *tcp_state_ntop(uint8_t state) {
  switch (state) {
    case TCP_CB_STATE_CLOSED:
      return "CLOSED";
    case TCP_CB_STATE_LISTEN:
      return "LISTEN";
    case TCP_CB_STATE_SYN_SENT:
      return "SYN_SENT";
    case TCP_CB_STATE_SYN_RCVD:
      return "SYN_RCVD";
    case TCP_CB_STATE_ESTABLISHED:
      return "ESTABLISHED";
    case TCP_CB_STATE_FIN_WAIT1:
      return "FIN_WAIT1";
    case TCP_CB_STATE_FIN_WAIT2:
      return "FIN_WAIT2";
    case TCP_CB_STATE_CLOSING:
      return "CLOSING";
    case TCP_CB_STATE_TIME_WAIT:
      return "TIME_WAIT";
    case TCP_CB_STATE_CLOSE_WAIT:
      return "CLOSE_WAIT";
    case TCP_CB_STATE_LAST_ACK:
      return "LAST_ACK";

    default:
      return "UNKNOWN";
  }
}

static void tcp_state_dump(struct tcp_cb *cb) {
  char buf[64];

  fprintf(stderr, "      used: %d\n", cb->used);
  fprintf(stderr, "     state: %s\n", tcp_state_ntop(cb->state));
  fprintf(stderr, " self.port: %u\n", ntoh16(cb->port));
  fprintf(stderr, " peer.addr: %s\n", ip_addr_ntop(&cb->peer.addr, buf, 64));
  fprintf(stderr, " peer.port: %u\n", ntoh16(cb->peer.port));
  fprintf(stderr, "   snd.nxt: %u\n", cb->snd.nxt);
  fprintf(stderr, "   snd.una: %u\n", cb->snd.una);
  fprintf(stderr, "   snd.wnd: %u\n", cb->snd.wnd);
  fprintf(stderr, "   txq.snt: %u\n", cb->txq.snt);
  fprintf(stderr, "   rcv.nxt: %u\n", cb->rcv.nxt);
  fprintf(stderr, "   rcv.wnd: %u\n", cb->rcv.wnd);
  fprintf(stderr, " n_backlog: %u\n", cb->backlog.num);
  fprintf(stderr, "   timeout: %ld\n", cb->timeout);
}

static void tcp_dump(struct tcp_cb *cb, struct tcp_hdr *hdr, size_t plen) {
  char buf[64];

  tcp_state_dump(cb);
  fprintf(stderr, " len: %lu\n", plen);
  fprintf(stderr, " src: %u\n", ntoh16(hdr->src));
  fprintf(stderr, " dst: %u\n", ntoh16(hdr->dst));
  fprintf(stderr, " seq: %u\n", ntoh32(hdr->seq));
  fprintf(stderr, " ack: %u\n", ntoh32(hdr->ack));
  fprintf(stderr, " off: %u\n", hdr->off);
  fprintf(stderr, " flg: [%s]\n", tcp_flg_ntop(hdr->flg, buf, 64));
  fprintf(stderr, " win: %u\n", ntoh16(hdr->win));
  fprintf(stderr, " sum: %u\n", ntoh16(hdr->sum));
  fprintf(stderr, " urg: %u\n", ntoh16(hdr->urg));
}

static uint16_t tcp_checksum(ip_addr_t self, ip_addr_t peer, uint8_t *segment,
                             size_t len) {
  uint32_t pseudo = 0;

  pseudo += (self >> 16) & 0xffff;
  pseudo += self & 0xffff;
  pseudo += (peer >> 16) & 0xffff;
  pseudo += self & 0xffff;
  pseudo += hton16((uint16_t)IP_PROTOCOL_TCP);
  pseudo += hton16(len);
  return cksum16((uint16_t *)segment, len, pseudo);
}

/*
 * Segment Queue
 */

static struct tcp_txq_entry *tcp_txq_add(struct tcp_cb *cb, struct tcp_hdr *hdr,
                                         size_t len) {
  struct tcp_txq_entry *txq;

  txq = malloc(sizeof(struct tcp_txq_entry));
  if (!txq) {
    return NULL;
  }
  txq->segment = hdr;
  txq->len = len;
  // clear timestamp
  memset(&txq->timestamp, 0, sizeof(txq->timestamp));
  txq->next = NULL;
  // set txq to next of tail entry
  if (cb->txq.head == NULL) {
    cb->txq.head = txq;
  } else {
    cb->txq.tail->next = txq;
  }
  // update tail entry
  cb->txq.tail = txq;

  return txq;
}

static void tcp_txq_clear_all(struct tcp_cb *cb) {
  struct tcp_txq_entry *txq = cb->txq.head, *next;
  while (txq) {
    next = txq->next;
    free(txq->segment);
    free(txq);
    txq = next;
  }
  cb->txq.head = cb->txq.tail = NULL;
}

/*
 * EVENT PROCESSING
 * https://tools.ietf.org/html/rfc793#section-3.9
 */

static void tcp_close_cb(struct tcp_cb *cb) {
  cb->state = TCP_CB_STATE_CLOSED;
  memset(&cb->snd, 0, sizeof(cb->snd));
  cb->iss = 0;
  memset(&cb->rcv, 0, sizeof(cb->rcv));
  cb->irs = 0;
  tcp_txq_clear_all(cb);
  return;
}

// SEGMENT ARRIVES
// https://tools.ietf.org/html/rfc793#page-65
static void tcp_event_segment_arrives(struct tcp_cb *cb, struct tcp_hdr *hdr,
                                      size_t len) {
  size_t plen;
  int acceptable = 0;
  struct timeval now;

  plen = TCP_DATA_LEN(hdr, len);
  if (gettimeofday(&now, NULL) == -1) {
    perror("gettimeofday");
    return;
  }

  switch (cb->state) {
    case TCP_CB_STATE_CLOSED:
      if (!TCP_FLG_ISSET(hdr->flg, TCP_FLG_RST)) {
        if (TCP_FLG_ISSET(hdr->flg, TCP_FLG_ACK)) {
          tcp_tx(cb, ntoh32(hdr->ack), 0, TCP_FLG_RST, &now, NULL, 0);
        } else {
          tcp_tx(cb, 0, ntoh32(hdr->seq) + plen, TCP_FLG_RST | TCP_FLG_ACK,
                 &now, NULL, 0);
        }
      }
      return;

    case TCP_CB_STATE_LISTEN:
      // first check for an RST
      if (TCP_FLG_ISSET(hdr->flg, TCP_FLG_RST)) {
        // incoming RST is ignored
        goto ERROR_RX_LISTEN;
      }

      // second check for an ACK
      if (TCP_FLG_ISSET(hdr->flg, TCP_FLG_ACK)) {
        tcp_tx(cb, ntoh32(hdr->ack), 0, TCP_FLG_RST, &now, NULL, 0);
        goto ERROR_RX_LISTEN;
      }

      // third check for a SYN
      if (TCP_FLG_ISSET(hdr->flg, TCP_FLG_SYN)) {
        // TODO: check the security

        // TODO: If the SEG.PRC is greater than the TCB.PRC

        // else
        cb->rcv.wnd = sizeof(cb->window);
        cb->rcv.nxt = ntoh32(hdr->seq) + 1;
        cb->irs = ntoh32(hdr->seq);
        cb->iss = (uint32_t)random();
        tcp_tx(cb, cb->iss, cb->rcv.nxt, TCP_FLG_SYN | TCP_FLG_ACK, &now, NULL,
               0);
        cb->snd.nxt = cb->iss + 1;
        cb->snd.una = cb->iss;
        cb->timeout = now.tv_sec + USER_TIMEOUT;
        cb->state = TCP_CB_STATE_SYN_RCVD;

        // TODO: ?  queue to backlog ?
        // TODO: increment hdr->seq for save text
        goto CHECK_URG;
      }

      // no packet should come here. drop segment
    ERROR_RX_LISTEN:
      // return state to CLOSED
      tcp_close_cb(cb);
      pthread_cond_broadcast(&cb->cond);
      cb->parent = NULL;
      return;

    case TCP_CB_STATE_SYN_SENT:
      // first check the ACK bit
      if (TCP_FLG_ISSET(hdr->flg, TCP_FLG_ACK)) {
        if (ntoh32(hdr->ack) <= cb->iss || ntoh32(hdr->ack) > cb->snd.nxt) {
          tcp_tx(cb, ntoh32(hdr->ack), 0, TCP_FLG_RST, &now, NULL, 0);
          return;
        }
        if (cb->snd.una <= ntoh32(hdr->ack) &&
            ntoh32(hdr->ack) <= cb->snd.nxt) {
          acceptable = 1;
        } else {
          // drop invalid ack
          return;
        }
      }

      // second check the RST bit
      if (TCP_FLG_ISSET(hdr->flg, TCP_FLG_RST)) {
        if (!acceptable) {
          // drop segment
          return;
        }
        fprintf(stderr, "error: connection reset\n");
        tcp_close_cb(cb);
        pthread_cond_signal(&cb->cond);
        return;
      }

      // TODO: third check the security and precedence

      // fourth check the SYN bit
      if (TCP_FLG_ISSET(hdr->flg, TCP_FLG_SYN)) {
        cb->rcv.nxt = ntoh32(hdr->seq) + 1;
        cb->irs = ntoh32(hdr->seq);
        // TODO: ? if there is an ACK ?
        if (cb->snd.una < ntoh32(hdr->ack)) {
          // update snd.una and user timeout
          cb->snd.una = ntoh32(hdr->ack);
          cb->timeout = now.tv_sec + USER_TIMEOUT;
          pthread_cond_signal(&timer_cond);
        }

        // TODO: clear all retransmission queue

        if (cb->snd.una > cb->iss) {
          // our SYN has been ACKed
          cb->state = TCP_CB_STATE_ESTABLISHED;
          tcp_tx(cb, cb->snd.nxt, cb->rcv.nxt, TCP_FLG_ACK, &now, NULL, 0);
          pthread_cond_signal(&cb->cond);
          if (plen > 0 || TCP_FLG_ISSET(hdr->flg, TCP_FLG_URG)) {
            goto CHECK_URG;
          }
          return;
        } else {
          cb->state = TCP_CB_STATE_SYN_RCVD;
          tcp_tx(cb, cb->iss, cb->rcv.nxt, TCP_FLG_SYN | TCP_FLG_ACK, &now,
                 NULL, 0);
          pthread_cond_signal(&cb->cond);
          // TODO: If there are other controls or text in the segment, queue
          // them for processing after the ESTABLISHED state has been reached,
          return;
        }
      }

      // fifth, if neither of the SYN or RST bits is set
      if (!TCP_FLG_ISSET(hdr->flg, TCP_FLG_ACK | TCP_FLG_RST)) {
        // drop segment
        return;
      }

      return;

    case TCP_CB_STATE_SYN_RCVD:
    case TCP_CB_STATE_ESTABLISHED:
    case TCP_CB_STATE_FIN_WAIT1:
    case TCP_CB_STATE_FIN_WAIT2:
    case TCP_CB_STATE_CLOSING:
    case TCP_CB_STATE_TIME_WAIT:
    case TCP_CB_STATE_CLOSE_WAIT:
    case TCP_CB_STATE_LAST_ACK:
      break;

    default:
      fprintf(stderr, ">>> segment recv : not implement tcp state (%d) <<<\n",
              cb->state);
      return;
  }

  // first check sequence number
  if (plen > 0) {
    if (cb->rcv.wnd > 0) {
      acceptable = (cb->rcv.nxt <= ntoh32(hdr->seq) &&
                    ntoh32(hdr->seq) < cb->rcv.nxt + cb->rcv.wnd) ||
                   (cb->rcv.nxt <= ntoh32(hdr->seq) &&
                    ntoh32(hdr->seq) + plen - 1 < cb->rcv.nxt + cb->rcv.wnd);
    } else {
      acceptable = 0;
    }
  } else {
    if (cb->rcv.wnd > 0) {
      acceptable = (cb->rcv.nxt <= ntoh32(hdr->seq) &&
                    ntoh32(hdr->seq) < cb->rcv.nxt + cb->rcv.wnd);
    } else {
      acceptable = ntoh32(hdr->seq) == cb->rcv.nxt;
    }
  }

  if (!acceptable) {
    if (!TCP_FLG_ISSET(hdr->flg, TCP_FLG_RST)) {
      fprintf(stderr, "is not acceptable !!!\n");
      tcp_tx(cb, cb->snd.nxt, cb->rcv.nxt, TCP_FLG_ACK, &now, NULL, 0);
    }
    // drop segment
    return;
  }

  // second check the RST bit
  switch (cb->state) {
    case TCP_CB_STATE_SYN_RCVD:
      if (TCP_FLG_ISSET(hdr->flg, TCP_FLG_RST)) {
        // close connection
        tcp_close_cb(cb);
        pthread_cond_signal(&cb->cond);
        return;
      }
      break;

    case TCP_CB_STATE_ESTABLISHED:
    case TCP_CB_STATE_FIN_WAIT1:
    case TCP_CB_STATE_FIN_WAIT2:
    case TCP_CB_STATE_CLOSE_WAIT:
      if (TCP_FLG_ISSET(hdr->flg, TCP_FLG_RST)) {
        // TODO: signal to SEND, RECEIVE waiting thread.
        tcp_close_cb(cb);
        pthread_cond_broadcast(&cb->cond);
        return;
      }
      break;

    case TCP_CB_STATE_CLOSING:
    case TCP_CB_STATE_TIME_WAIT:
    case TCP_CB_STATE_LAST_ACK:
      if (TCP_FLG_ISSET(hdr->flg, TCP_FLG_RST)) {
        // close connection
        tcp_close_cb(cb);
        pthread_cond_broadcast(&cb->cond);
        return;
      }
      break;

    default:
      fprintf(stderr, ">>> seg recv (2) : not implement tcp state (%d) <<<\n",
              cb->state);
      break;
  }

  // TODO: third check security and precedence

  // fourth, check the SYN bit
  if (TCP_FLG_ISSET(hdr->flg, TCP_FLG_SYN)) {
    tcp_tx(cb, 0, cb->rcv.nxt, TCP_FLG_RST, &now, NULL, 0);
    tcp_close_cb(cb);
    pthread_cond_broadcast(&cb->cond);
    return;
  }
  // TODO: ? If the SYN is not in the window this step would not be reached and
  // an ack would have been sent in the first step (sequence number check). ?

  // fifth check the ACK field
  if (TCP_FLG_ISSET(hdr->flg, TCP_FLG_ACK)) {
    switch (cb->state) {
      case TCP_CB_STATE_SYN_RCVD:
        if (!(cb->snd.una <= ntoh32(hdr->ack) &&
              ntoh32(hdr->ack) <= cb->snd.nxt)) {
          // hdr->ack is not acceptable
          tcp_tx(cb, ntoh32(hdr->ack), cb->rcv.nxt, TCP_FLG_RST, &now, NULL, 0);
          // The connection remains in the same state after send RST
          break;
        }
        cb->state = TCP_CB_STATE_ESTABLISHED;
        if (cb->parent) {
          // add cb to backlog
          queue_push(&cb->parent->backlog, cb, sizeof(*cb));
          pthread_cond_signal(&cb->parent->cond);
        } else {
          // parent == NULL means cb is created by user and first state was
          // SYN_SENT
          pthread_cond_signal(&cb->cond);
        }

        // enter ESTABLISHED state and continue processing
      case TCP_CB_STATE_ESTABLISHED:
      case TCP_CB_STATE_FIN_WAIT1:
      case TCP_CB_STATE_FIN_WAIT2:
      case TCP_CB_STATE_CLOSE_WAIT:
      case TCP_CB_STATE_CLOSING:
        if (cb->snd.una <= ntoh32(hdr->ack) &&
            ntoh32(hdr->ack) <= cb->snd.nxt) {
          if (cb->snd.una < ntoh32(hdr->ack)) {
            // update snd.una and user timeout
            cb->snd.una = ntoh32(hdr->ack);
            cb->timeout = now.tv_sec + USER_TIMEOUT;
            pthread_cond_signal(&timer_cond);
          }
          // TODO: retransmission queue send
          pthread_cond_broadcast(&cb->cond);

          if ((cb->snd.wl1 < ntoh32(hdr->seq)) ||
              (cb->snd.wl1 == ntoh32(hdr->seq) &&
               cb->snd.wl2 <= ntoh32(hdr->ack))) {
            cb->snd.wnd = ntoh16(hdr->win);
            cb->snd.wl1 = ntoh32(hdr->seq);
            cb->snd.wl2 = ntoh32(hdr->ack);
          }
        } else if (ntoh32(hdr->ack) > cb->snd.nxt) {
          fprintf(stderr, "recv ack but ack is advanced to snd.nxt\n");
          tcp_tx(cb, cb->snd.nxt, cb->rcv.nxt, TCP_FLG_ACK, &now, NULL, 0);
          // drop the segment
          return;
        }
        // else if SEG.ACK < SND.UNA then it can be ignored

        if (cb->state == TCP_CB_STATE_FIN_WAIT1) {
          // if this ACK is for sent FIN
          if (ntoh32(hdr->ack) + 1 == cb->snd.nxt) {
            cb->state = TCP_CB_STATE_FIN_WAIT2;
          }
        } else if (cb->state == TCP_CB_STATE_FIN_WAIT2) {
          // TODO: if the retransmission queue is empty, the user's CLOSE can be
          // acknowledged ("ok")
        } else if (cb->state == TCP_CB_STATE_CLOSING) {
          // if this ACK is for sent FIN
          if (ntoh32(hdr->ack) + 1 == cb->snd.nxt) {
            cb->state = TCP_CB_STATE_TIME_WAIT;
          }
        }

        break;

      case TCP_CB_STATE_LAST_ACK:
        // if this ACK is for sent FIN
        if (ntoh32(hdr->ack) == cb->snd.nxt) {
          tcp_close_cb(cb);
          pthread_cond_broadcast(&cb->cond);
          return;
        }
        break;

      case TCP_CB_STATE_TIME_WAIT:
        // TODO: restart the 2 MSL timeout
        break;

      default:
        fprintf(stderr, ">>> seg recv (5) : not implement tcp state (%d) <<<\n",
                cb->state);
        break;
    }
  }

CHECK_URG:
  // sixth, check the URG bit
  if (TCP_FLG_ISSET(hdr->flg, TCP_FLG_URG)) {
    switch (cb->state) {
      case TCP_CB_STATE_ESTABLISHED:
      case TCP_CB_STATE_FIN_WAIT1:
      case TCP_CB_STATE_FIN_WAIT2:
        cb->rcv.up = MAX(cb->rcv.up, ntoh16(hdr->urg));
        // TODO: signal

        break;

      case TCP_CB_STATE_CLOSING:
      case TCP_CB_STATE_TIME_WAIT:
      case TCP_CB_STATE_CLOSE_WAIT:
      case TCP_CB_STATE_LAST_ACK:
        // this should not occur. ignore the urg
        break;

      case TCP_CB_STATE_SYN_RCVD:
        // do nothing
        break;

      default:
        fprintf(stderr, ">>> seg recv (6) : not implement tcp state (%d) <<<\n",
                cb->state);
        break;
    }
  }

  // seventh, process the segment text
  switch (cb->state) {
    case TCP_CB_STATE_ESTABLISHED:
    case TCP_CB_STATE_FIN_WAIT1:
    case TCP_CB_STATE_FIN_WAIT2:
      // TODO: accept not ordered packet
      if (plen > 0 && cb->rcv.nxt == ntoh32(hdr->seq)) {
        // copy segment to receive buffer
        memcpy(cb->window + (sizeof(cb->window) - cb->rcv.wnd),
               (uint8_t *)hdr + TCP_HDR_LEN(hdr), plen);
        cb->rcv.nxt = ntoh32(hdr->seq) + plen;
        cb->rcv.wnd -= plen;
        tcp_tx(cb, cb->snd.nxt, cb->rcv.nxt, TCP_FLG_ACK, &now, NULL, 0);
        pthread_cond_broadcast(&cb->cond);
      } else if (TCP_FLG_ISSET(hdr->flg, TCP_FLG_PSH)) {
        tcp_tx(cb, cb->snd.nxt, cb->rcv.nxt, TCP_FLG_ACK, &now, NULL, 0);
        pthread_cond_broadcast(&cb->cond);
      }
      break;

    case TCP_CB_STATE_CLOSING:
    case TCP_CB_STATE_TIME_WAIT:
    case TCP_CB_STATE_CLOSE_WAIT:
    case TCP_CB_STATE_LAST_ACK:
      // this should not occur. ignore the text
      break;

    case TCP_CB_STATE_SYN_RCVD:
      // do nothing
      break;

    default:
      fprintf(stderr, ">>> seg recv (7) : not implement tcp state (%d) <<<\n",
              cb->state);
      break;
  }

  // eighth, check the FIN bit
  if (TCP_FLG_ISSET(hdr->flg, TCP_FLG_FIN)) {
    cb->rcv.nxt = ntoh32(hdr->seq) + 1;
    tcp_tx(cb, cb->snd.nxt, cb->rcv.nxt, TCP_FLG_ACK, &now, NULL, 0);
    switch (cb->state) {
      case TCP_CB_STATE_SYN_RCVD:
      case TCP_CB_STATE_ESTABLISHED:
        cb->state = TCP_CB_STATE_CLOSE_WAIT;
        break;

      case TCP_CB_STATE_FIN_WAIT1:
        cb->state = TCP_CB_STATE_CLOSING;
        break;

      case TCP_CB_STATE_FIN_WAIT2:
        cb->state = TCP_CB_STATE_TIME_WAIT;
        // start time-wait timer
        cb->timeout = now.tv_sec + TIME_WAIT_TIMEOUT;
        // TODO: turn off other timers
        break;

      case TCP_CB_STATE_CLOSING:
      case TCP_CB_STATE_CLOSE_WAIT:
      case TCP_CB_STATE_LAST_ACK:
        // remain state
        break;

      case TCP_CB_STATE_TIME_WAIT:
        // remain state
        // restart the 2MSL timeout
        cb->timeout = now.tv_sec + TIME_WAIT_TIMEOUT;
        break;

      default:
        fprintf(stderr, ">>> seg recv (8) : not implement tcp state (%d) <<<\n",
                cb->state);
        break;
    }
    // signal "connection closing"
    pthread_cond_broadcast(&cb->cond);
  }
  return;
}

/*
 * TCP APPLICATION CONTROLLER
 */

// TODO: where to get current time.
static ssize_t tcp_tx(struct tcp_cb *cb, uint32_t seq, uint32_t ack,
                      uint8_t flg, struct timeval *now, uint8_t *buf,
                      size_t len) {
  uint8_t *segment;
  struct tcp_hdr *hdr;
  ip_addr_t self, peer;
  struct tcp_txq_entry *txq = NULL;
  int have_unsent;

  // allocate segment
  segment = malloc(sizeof(struct tcp_hdr) + len);
  if (!segment) {
    return -1;
  }

  // set header params
  hdr = (struct tcp_hdr *)segment;
  memset(hdr, 0, sizeof(struct tcp_hdr));
  hdr->src = cb->port;
  hdr->dst = cb->peer.port;
  hdr->seq = hton32(seq);
  hdr->ack = hton32(ack);
  hdr->off = (sizeof(struct tcp_hdr) >> 2) << 4;
  hdr->flg = flg;
  hdr->win = hton16(cb->rcv.wnd);
  hdr->sum = 0;
  hdr->urg = 0;

  // copy data
  memcpy(hdr + 1, buf, len);

  if (len > 0 || flg & (TCP_FLG_SYN | TCP_FLG_FIN)) {
    // add txq list only packets which have ack reply

    // check unsent segment exists in cb->txq
    have_unsent = cb->txq.tail && cb->txq.tail->timestamp.tv_sec == 0;

    // add txq
    txq = tcp_txq_add(cb, hdr, sizeof(struct tcp_hdr) + len);
    if (!txq) {
      free(segment);
      return -1;
    }

    // flow control
    // TODO: cb->txq.snt is not exactly the sliding window size
    if (have_unsent || (!TCP_FLG_ISSET(hdr->flg, TCP_FLG_SYN) &&
                        cb->txq.snt + len > cb->snd.wnd)) {
      // before established snd.wnd is 0. so SYN does not care snd.wnd
      // segment is stored in txq and send later in timer_thread
      fprintf(stderr, ">>> not send havesent %d %d<<<\n", have_unsent,
              cb->txq.tail);
      tcp_dump(cb, hdr, len);
      return 0;
    }
  }

  // calculate checksum
  self = ((struct netif_ip *)cb->iface)->unicast;
  peer = cb->peer.addr;
  hdr->sum =
      tcp_checksum(self, peer, (uint8_t *)hdr, sizeof(struct tcp_hdr) + len);

#ifdef TCP_DEBUG
  fprintf(stderr, ">>> tcp_tx <<<\n");
  tcp_dump(cb, hdr, len);
#endif

  // send packet
  if (ip_tx(cb->iface, IP_PROTOCOL_TCP, (uint8_t *)hdr,
            sizeof(struct tcp_hdr) + len, &peer) == -1) {
    // failed to send ip packet
    if (!txq) {
      // this packet does not expect reply so not queued into txq list
      free(segment);
    }
    return -1;
  }

  if (txq) {
    // set timestamp
    txq->timestamp = *now;
    cb->txq.snt += len;
  } else {
    // this packet does not expect reply so not queued into txq list
    free(segment);
  }

  return len;
}

static void tcp_rx(uint8_t *segment, size_t len, ip_addr_t *src, ip_addr_t *dst,
                   struct netif *iface) {
  struct tcp_hdr *hdr;
  int i;
  struct tcp_cb *cb, *fcb = NULL, *lcb = NULL;

  // validate tcp packet
  if (*dst != ((struct netif_ip *)iface)->unicast) {
    return;
  }
  if (len < sizeof(struct tcp_hdr)) {
    return;
  }

  // validate checksum
  hdr = (struct tcp_hdr *)segment;
  if (tcp_checksum(*src, *dst, segment, len) != 0) {
    fprintf(stderr, "tcp checksum error\n");
    return;
  }

  pthread_mutex_lock(&mutex);

  // find connection cb or listener cb
  for (i = 0; i < TCP_CB_TABLE_SIZE; i++) {
    cb = &cb_table[i];
    if (IS_FREE_CB(cb)) {
      // cache cb for the case SYN message and no cb is connected to this peer
      if (fcb == NULL) {
        fcb = cb;
      }
    } else if ((!cb->iface || cb->iface == iface) && cb->port == hdr->dst) {
      // if ip address and dst port number matches
      if (cb->peer.addr == *src && cb->peer.port == hdr->src) {
        // this cb is connection for this tcp packet
        break;
      } else if (cb->state == TCP_CB_STATE_LISTEN && !lcb) {
        // listener socket is found
        lcb = cb;
      }
    }
  }

  // cb that matches this tcp packet is not found.
  // create socket if listener socket exists and packet is SYN packet.
  if (i == TCP_CB_TABLE_SIZE) {
    if (!fcb) {
      // cb resource is run out
      // TODO: send RST
      pthread_mutex_unlock(&mutex);
      return;
    }

    // create accept socket
    cb = fcb;
    cb->iface = iface;
    if (lcb) {
      // TODO: ? if SYN is not set ?
      cb->state = lcb->state;
      cb->port = lcb->port;
      cb->parent = lcb;
    } else {
      // this port is not listened.
      // this packet is invalid. no connection is found.
      cb->port = 0;
    }
    cb->peer.addr = *src;
    cb->peer.port = hdr->src;
  }
  // else cb that matches this tcp packet is found.

#ifdef TCP_DEBUG
  fprintf(stderr, ">>> tcp_rx <<<\n");
  tcp_dump(cb, hdr, TCP_DATA_LEN(hdr, len));
#endif

  // handle message
  tcp_event_segment_arrives(cb, hdr, len);
  pthread_mutex_unlock(&mutex);
  return;
}

static void *tcp_timer_thread(void *arg) {
  struct timeval timestamp, diff;
  struct timespec timeout;
  struct tcp_cb *cb;
  struct tcp_txq_entry *txq, *prev, *tmp;
  ip_addr_t self, peer;
  size_t sum = 0;
  int i;

  diff.tv_sec = 0;
  diff.tv_usec = 100 * 1000;

  pthread_mutex_lock(&mutex);
  while (1) {
    gettimeofday(&timestamp, NULL);
    for (i = 0; i < TCP_CB_TABLE_SIZE; i++) {
      cb = &cb_table[i];

      if (cb->state == TCP_CB_STATE_CLOSED) {
        // skip check timeout
        continue;
      }

      // check user timeout
      if ((cb->snd.una != cb->snd.nxt || cb->state == TCP_CB_STATE_TIME_WAIT) &&
          cb->timeout < timestamp.tv_sec) {
        // force close connection because of ack timeout or TIME_WAIT timeout

#ifdef TCP_DEBUG
        fprintf(stderr, ">>> find user timeout (%ld - %ld) <<<\n",
                timestamp.tv_sec, cb->timeout);
        tcp_state_dump(cb);
#endif
        tcp_close_cb(cb);
        pthread_cond_broadcast(&cb->cond);
        continue;
      }

      // check retransmission timeout and vacuum acked segment entry
      prev = NULL;
      txq = cb->txq.head;
      sum = 0;
      if (cb->txq.head) {
        self = ((struct netif_ip *)cb->iface)->unicast;
        peer = cb->peer.addr;
      }
      while (txq) {
        if (ntoh32(txq->segment->seq) >= cb->snd.una) {
          // TODO: check sum + datalen should compare with snd.wnd. but
          // txq->segment does not support splitting. so sum add later
          /* sum += TCP_DATA_LEN(txq->segment, txq->len); */

          if (sum < cb->snd.wnd) {
            // this txq is in sliding send window

            if (txq->timestamp.tv_sec == 0) {
              // this txq is not sent

              // re-calc checksum
              txq->segment->ack = hton32(cb->rcv.nxt);
              txq->segment->sum = 0;
              txq->segment->sum =
                  tcp_checksum(self, peer, txq->segment, txq->len);

#ifdef TCP_DEBUG
              fprintf(stderr, ">>> tcp_tx in timer_thread <<<\n");
              tcp_dump(cb, txq->segment, TCP_DATA_LEN(txq->segment, txq->len));
#endif

              // send packet
              ip_tx(cb->iface, IP_PROTOCOL_TCP, (uint8_t *)txq->segment,
                    txq->len, &cb->peer.addr);
              txq->timestamp = timestamp;
            } else if (timestamp.tv_sec - txq->timestamp.tv_sec > 3) {
              // if retransmission timeout (3 seconds) then resend

              // re-calc checksum
              txq->segment->ack = hton32(cb->rcv.nxt);
              txq->segment->sum = 0;
              txq->segment->sum =
                  tcp_checksum(self, peer, txq->segment, txq->len);

#ifdef TCP_DEBUG
              fprintf(stderr,
                      ">>> find retransmission timeout (%ld - %ld) <<<\n",
                      timestamp.tv_sec, txq->timestamp.tv_sec);
              tcp_dump(cb, txq->segment, TCP_DATA_LEN(txq->segment, txq->len));
#endif

              // resend packet
              ip_tx(cb->iface, IP_PROTOCOL_TCP, (uint8_t *)txq->segment,
                    txq->len, &cb->peer.addr);
              txq->timestamp = timestamp;
            }
          }

          // FIXME: temporary sum add here. no support splitting
          sum += TCP_DATA_LEN(txq->segment, txq->len);

          // update previous tcp_txq_entry
          prev = txq;
          txq = txq->next;
        } else {
          // remove tcp_txq_entry from list
          cb->txq.snt -= TCP_DATA_LEN(txq->segment, txq->len);

          // swap tail tcp_txq_entry
          if (!txq->next) {
            // txq is tail entry
            cb->txq.tail = prev;
          }
          // swap previous tcp_txq_entry
          if (prev) {
            prev->next = txq->next;
          } else {
            cb->txq.head = txq->next;
          }

          // free tcp_txq_entry
          tmp = txq->next;
          free(txq->segment);
          free(txq);
          // check next entry
          txq = tmp;
        }
      }
    }
    // sleep 100 ms
    timeradd(&timestamp, &diff, &timestamp);
    timeout.tv_sec = timestamp.tv_sec;
    timeout.tv_nsec = timestamp.tv_usec * 1000;
    pthread_cond_timedwait(&timer_cond, &mutex, &timeout);
  }
  pthread_mutex_unlock(&mutex);

  return NULL;
}

/*
 * TCP APPLICATION INTERFACE
 */

int tcp_api_open(void) {
  struct tcp_cb *cb;
  int i;

  pthread_mutex_lock(&mutex);
  for (i = 0; i < TCP_CB_TABLE_SIZE; i++) {
    cb = &cb_table[i];
    if (IS_FREE_CB(cb)) {
      cb->used = 1;
      pthread_mutex_unlock(&mutex);
      return i;
    }
  }
  pthread_mutex_unlock(&mutex);
  fprintf(stderr, "error:  insufficient resources\n");
  return -1;
}

int tcp_close(struct tcp_cb *cb) {
  struct tcp_cb *backlog = NULL;
  size_t size;
  struct timeval now;
  if (!cb->used) {
    fprintf(stderr, "error:  connection illegal for this process\n");
    return -1;
  }

  cb->used = 0;
  if (gettimeofday(&now, NULL) == -1) {
    return -1;
  }

  switch (cb->state) {
    case TCP_CB_STATE_CLOSED:
      break;

    case TCP_CB_STATE_LISTEN:
      // close all cb in backlog
      while (queue_pop(&cb->backlog, (void **)&backlog, &size) != -1) {
        tcp_close(backlog);
      }
    case TCP_CB_STATE_SYN_SENT:
      // close socket
      tcp_close_cb(cb);
      pthread_cond_broadcast(&cb->cond);
      break;

    case TCP_CB_STATE_SYN_RCVD:
      // if send buffer is empty
      tcp_tx(cb, cb->snd.nxt, cb->rcv.nxt, TCP_FLG_FIN | TCP_FLG_ACK, &now,
             NULL, 0);
      cb->snd.nxt++;
      cb->state = TCP_CB_STATE_FIN_WAIT1;
      // TODO: else then wait change to ESTABLISHED state
      break;

    case TCP_CB_STATE_ESTABLISHED:
      // if send buffer is empty
      // TODO: else then wait send all data in send buffer
      // TODO: ? linux tcp need ack with fin ?
      tcp_tx(cb, cb->snd.nxt, cb->rcv.nxt, TCP_FLG_FIN | TCP_FLG_ACK, &now,
             NULL, 0);
      cb->snd.nxt++;
      cb->state = TCP_CB_STATE_FIN_WAIT1;
      break;

    case TCP_CB_STATE_FIN_WAIT1:
    case TCP_CB_STATE_FIN_WAIT2:
    // "ok" response would be acceptable
    case TCP_CB_STATE_CLOSING:
    case TCP_CB_STATE_TIME_WAIT:
    case TCP_CB_STATE_LAST_ACK:
      fprintf(stderr, "error:  connection closing\n");
      return -1;

    case TCP_CB_STATE_CLOSE_WAIT:
      // wait send all data in send buffer
      tcp_tx(cb, cb->snd.nxt, cb->rcv.nxt, TCP_FLG_FIN | TCP_FLG_ACK, &now,
             NULL, 0);
      cb->snd.nxt++;
      cb->state = TCP_CB_STATE_CLOSING;
      break;
  }

  return 0;
}

int tcp_api_close(int soc) {
  int ret;

  // validate soc id
  if (TCP_SOCKET_INVALID(soc)) {
    return -1;
  }

  pthread_mutex_lock(&mutex);
  ret = tcp_close(&cb_table[soc]);
  pthread_mutex_unlock(&mutex);

  return ret;
}

int tcp_api_connect(int soc, ip_addr_t *addr, uint16_t port) {
  struct tcp_cb *cb, *tmp;
  struct timeval now;
  int i, j;
  int offset;

  // validate soc id
  if (TCP_SOCKET_INVALID(soc)) {
    return -1;
  }

  pthread_mutex_lock(&mutex);
  cb = &cb_table[soc];

  // check cb state
  if (!cb->used || cb->state != TCP_CB_STATE_CLOSED) {
    pthread_mutex_unlock(&mutex);
    return -1;
  }

  if (gettimeofday(&now, NULL) == -1) {
    perror("gettimeofday");
    return -1;
  }

  // if port number is not specified then generate nice port
  if (!cb->port) {
    offset = time(NULL) % 1024;
    // find port number which is not used between TCP_SOURCE_PORT_MIN and
    // TCP_SOURCE_PORT_MAX
    for (i = TCP_SOURCE_PORT_MIN + offset; i <= TCP_SOURCE_PORT_MAX; i++) {
      for (j = 0; j < TCP_CB_TABLE_SIZE; j++) {
        tmp = &cb_table[j];
        if (!IS_FREE_CB(tmp) && tmp->port == hton16((uint16_t)i)) {
          break;
        }
      }
      if (j == TCP_CB_TABLE_SIZE) {
        // port number (i) is not used
        cb->port = hton16((uint16_t)i);
        break;
      }
    }
    if (!cb->port) {
      // could not find unused port number
      pthread_mutex_unlock(&mutex);
      return -1;
    }
  }

  // initalize cb
  cb->peer.addr = *addr;
  cb->peer.port = hton16(port);
  cb->iface = ip_netif_by_peer(&cb->peer.addr);
  if (!cb->iface) {
    pthread_mutex_unlock(&mutex);
    return -1;
  }
  cb->rcv.wnd = sizeof(cb->window);
  cb->iss = (uint32_t)random();

  // send SYN packet
  if (tcp_tx(cb, cb->iss, 0, TCP_FLG_SYN, &now, NULL, 0) == -1) {
    pthread_mutex_unlock(&mutex);
    return -1;
  }
  cb->snd.una = cb->iss;
  cb->snd.nxt = cb->iss + 1;
  cb->timeout = now.tv_sec + USER_TIMEOUT;
  cb->state = TCP_CB_STATE_SYN_SENT;

  // wait until state change
  while (cb->state == TCP_CB_STATE_SYN_SENT) {
    pthread_cond_wait(&cb_table[soc].cond, &mutex);
  }

  pthread_mutex_unlock(&mutex);
  return 0;
}

int tcp_api_bind(int soc, uint16_t port) {
  struct tcp_cb *cb;
  int i;

  // validate soc id
  if (TCP_SOCKET_INVALID(soc)) {
    return -1;
  }

  pthread_mutex_lock(&mutex);

  // check port is already used
  for (i = 0; i < TCP_CB_TABLE_SIZE; i++) {
    if (cb_table[i].port == hton16(port)) {
      pthread_mutex_unlock(&mutex);
      fprintf(stderr, "error:  port is already used\n");
      return -1;
    }
  }

  // check cb is closed
  cb = &cb_table[soc];
  if (!cb->used || cb->state != TCP_CB_STATE_CLOSED) {
    pthread_mutex_unlock(&mutex);
    return -1;
  }

  // TODO: bind ip address

  // set port number
  cb->port = hton16(port);
  pthread_mutex_unlock(&mutex);
  return 0;
}

int tcp_api_listen(int soc) {
  struct tcp_cb *cb;

  // validate soc id
  if (TCP_SOCKET_INVALID(soc)) {
    return -1;
  }

  pthread_mutex_lock(&mutex);
  cb = &cb_table[soc];
  if (!cb->used || cb->state != TCP_CB_STATE_CLOSED || !cb->port) {
    pthread_mutex_unlock(&mutex);
    return -1;
  }
  cb->state = TCP_CB_STATE_LISTEN;
  pthread_mutex_unlock(&mutex);
  return 0;
}

int tcp_api_accept(int soc) {
  struct tcp_cb *cb, *backlog = NULL;
  size_t size;

  // validate soc id
  if (TCP_SOCKET_INVALID(soc)) {
    return -1;
  }

  pthread_mutex_lock(&mutex);
  cb = &cb_table[soc];
  if (!cb->used || cb->state != TCP_CB_STATE_LISTEN) {
    pthread_mutex_unlock(&mutex);
    return -1;
  }

  while (cb->state == TCP_CB_STATE_LISTEN &&
         queue_pop(&cb->backlog, (void **)&backlog, &size) == -1) {
    pthread_cond_wait(&cb->cond, &mutex);
  }

  if (!backlog) {
    pthread_mutex_unlock(&mutex);
    return -1;
  }

  backlog->used = 1;
  pthread_mutex_unlock(&mutex);

  return array_offset(cb_table, backlog);
}

ssize_t tcp_api_recv(int soc, uint8_t *buf, size_t size) {
  struct tcp_cb *cb;
  size_t total, len;
  char *err;

  // validate soc id
  if (TCP_SOCKET_INVALID(soc)) {
    return -1;
  }

  pthread_mutex_lock(&mutex);
  cb = &cb_table[soc];
  if (!cb->used) {
    pthread_mutex_unlock(&mutex);
    return -1;
  }

TCP_RECEIVE_RETRY:
  switch (cb->state) {
    case TCP_CB_STATE_CLOSED:
      err = "error:  connection illegal for this process\n";
      goto ERROR_RECEIVE;

    case TCP_CB_STATE_LISTEN:
    case TCP_CB_STATE_SYN_SENT:
    case TCP_CB_STATE_SYN_RCVD:
      // TODO: wait change to ESTABLISHED
      err = "error:  connection illegal for this process\n";
      goto ERROR_RECEIVE;

    case TCP_CB_STATE_CLOSE_WAIT:
    case TCP_CB_STATE_ESTABLISHED:
    case TCP_CB_STATE_FIN_WAIT1:
    case TCP_CB_STATE_FIN_WAIT2:
      total = sizeof(cb->window) - cb->rcv.wnd;
      if (total == 0) {
        if (cb->state == TCP_CB_STATE_CLOSE_WAIT) {
          err = "error:  connection closing\n";
          goto ERROR_RECEIVE;
        }

        // wait and retry to read rcv buffer
        pthread_cond_wait(&cb->cond, &mutex);
        goto TCP_RECEIVE_RETRY;
      }
      len = total > size ? size : total;
      memcpy(buf, cb->window, len);
      memmove(cb->window, cb->window + len, total - len);
      cb->rcv.wnd += len;
      pthread_mutex_unlock(&mutex);
      return len;

    case TCP_CB_STATE_CLOSING:
    case TCP_CB_STATE_TIME_WAIT:
    case TCP_CB_STATE_LAST_ACK:
      err = "error:  connection closing\n";
      goto ERROR_RECEIVE;

    default:
      pthread_mutex_unlock(&mutex);
      return -1;
  }

ERROR_RECEIVE:
  pthread_mutex_unlock(&mutex);
  fprintf(stderr, err);
  return -1;
}

ssize_t tcp_api_send(int soc, uint8_t *buf, size_t len) {
  struct tcp_cb *cb;
  struct timeval now;
  size_t snt = 0, size;
  uint16_t wnd;
  char *err;

  // validate soc id
  if (TCP_SOCKET_INVALID(soc)) {
    return -1;
  }

  pthread_mutex_lock(&mutex);
  cb = &cb_table[soc];
  if (!cb->used) {
    pthread_mutex_unlock(&mutex);
    return -1;
  }

TCP_API_SEND_NEXT:
  switch (cb->state) {
    case TCP_CB_STATE_CLOSED:
      err = "error:  connection illegal for this process\n";
      goto ERROR_SEND;

    case TCP_CB_STATE_LISTEN:
      // TODO: change to active mode if foreign socket is specified
    case TCP_CB_STATE_SYN_SENT:
    case TCP_CB_STATE_SYN_RCVD:
      // TODO: wait change to ESTABLISHED
      err = "error:  connection illegal for this process\n";
      goto ERROR_SEND;

    case TCP_CB_STATE_CLOSE_WAIT:
    case TCP_CB_STATE_ESTABLISHED:
      // send buf
      break;

    case TCP_CB_STATE_FIN_WAIT1:
    case TCP_CB_STATE_FIN_WAIT2:
    case TCP_CB_STATE_CLOSING:
    case TCP_CB_STATE_TIME_WAIT:
    case TCP_CB_STATE_LAST_ACK:
      err = "error:  connection closing\n";
      goto ERROR_SEND;

    default:
      pthread_mutex_unlock(&mutex);
      return -1;
  }

  if (gettimeofday(&now, NULL) == -1) {
    perror("gettimeofday");
    pthread_mutex_unlock(&mutex);
    return (snt == 0) ? -1 : ((ssize_t)snt);
  }

  if (len > 0) {
    // mtu may changes, so calc size each time
    size = cb->iface->dev->mtu - IP_HDR_SIZE_MAX - sizeof(struct tcp_hdr);

    // check data size
    if (len < size) {
      size = len;
    }

    // check send buffer size
    wnd = TCP_SND_BUF_SIZE - (cb->snd.nxt - cb->snd.una);
    if (wnd < size) {
      size = wnd;
      if (size <= 0) {
        // wait until some data ack
        fprintf(stderr,
                ">>> send : wait for ack snd_buf_size: %d, snd.nxt: %u, "
                "snd.una: %u <<<\n",
                TCP_SND_BUF_SIZE, cb->snd.nxt, cb->snd.una);
        pthread_cond_wait(&cb->cond, &mutex);
        // retry
        goto TCP_API_SEND_NEXT;
      }
    }

    // send segment. if send window is not enough then stored into txq in tcp_tx
    if (tcp_tx(cb, cb->snd.nxt, cb->rcv.nxt, TCP_FLG_PSH | TCP_FLG_ACK, &now,
               buf + snt, size) == -1) {
      // TODO: memory allocation error or ip_tx error
      return snt;
    }
    cb->timeout = now.tv_sec + USER_TIMEOUT;
    cb->snd.nxt += size;
    snt += size;
    len -= size;

    if (len > 0) {
      // send next segment
      goto TCP_API_SEND_NEXT;
    }
  }

  pthread_mutex_unlock(&mutex);
  // TODO: support urg pointer
  return snt;

ERROR_SEND:
  pthread_mutex_unlock(&mutex);
  fprintf(stderr, err);
  return -1;
}

int tcp_init(void) {
  int i;

  // initialize mutex and condition variables
  for (i = 0; i < TCP_CB_TABLE_SIZE; i++) {
    pthread_cond_init(&cb_table[i].cond, NULL);
  }
  pthread_mutex_init(&mutex, NULL);
  pthread_cond_init(&timer_cond, NULL);

  if (ip_add_protocol(IP_PROTOCOL_TCP, tcp_rx) == -1) {
    return -1;
  }
  if (pthread_create(&timer_thread, NULL, tcp_timer_thread, NULL) == -1) {
    return -1;
  }
  return 0;
}
