#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"
#include "net.h"
#include <stddef.h>

// === transmit data structures ===

#define TX_RING_SIZE 16

// the transmit descriptor ring buffer, see 3.4
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));

// an array of in-flight mbuf structs, one per tx_desc in the tx_ring
static struct mbuf *tx_mbufs[TX_RING_SIZE];

// a spinlock that guards access to the tx_ring and E1000_TDT register
struct spinlock e1000_tx_lock;

// === receive data structures ===

#define RX_RING_SIZE 16

// the receive descriptor ring buffer, see 3.2.6
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));

// an array of pending mbuf structs, one per rx_desc in the rx_ring
static struct mbuf *rx_mbufs[RX_RING_SIZE];

// a spinlock that guards access to the rx_ring and E1000_RDT register
struct spinlock e1000_rx_lock;

// === end ===

// remember where the e1000's registers live.
static volatile uint32 *regs;

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
void
e1000_init(uint32 *xregs)
{
  int i;

  initlock(&e1000_tx_lock, "e1000_tx");
  initlock(&e1000_rx_lock, "e1000_rx");

  regs = xregs;

  // Reset the device
  regs[E1000_IMS] = 0; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // redisable interrupts
  __sync_synchronize();

  // [E1000 14.5] Transmit initialization
  memset(tx_ring, 0, sizeof(tx_ring));
  for (i = 0; i < TX_RING_SIZE; i++) {
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_mbufs[i] = 0;
  }
  regs[E1000_TDBAL] = (uint64) tx_ring;
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;

  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_mbufs[i] = mbufalloc(0);
    if (!rx_mbufs[i])
      panic("e1000");
    rx_ring[i].addr = (uint64) rx_mbufs[i]->head;
  }
  regs[E1000_RDBAL] = (uint64) rx_ring;
  if(sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // filter by qemu's MAC address, 52:54:00:12:34:56
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA+1] = 0x5634 | (1<<31);
  // multicast table
  for (int i = 0; i < 4096/32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |  // enable
    E1000_TCTL_PSP |                  // pad short packets
    (0x10 << E1000_TCTL_CT_SHIFT) |   // collision stuff
    (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8<<10) | (6<<20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN | // enable receiver
    E1000_RCTL_BAM |                 // enable broadcast
    E1000_RCTL_SZ_2048 |             // 2048-byte rx buffers
    E1000_RCTL_SECRC;                // strip CRC

  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0; // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0; // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 7); // RXDW -- Receiver Descriptor Write Back
}

int
e1000_transmit(struct mbuf *m)
{
  acquire(&e1000_tx_lock);

  // the index of the next available transmit descriptor
  int tx_index = regs[E1000_TDT];

  // the next available transmit descriptor
  struct tx_desc *tx_desc = &tx_ring[tx_index];

  // E1000_TXD_STAT_DD ("descriptor done") is set when the transmit descriptor has been sent, and
  // if that's not set, the ring has overflowed - we've looped back to a packet in the ring that
  // has not yet been sent
  if ((tx_desc->status & E1000_TXD_STAT_DD) == 0) {
    printf("e1000_transmit overflow: transmit not yet complete for tx_desc #%d", tx_index);
    release(&e1000_tx_lock);

    return -1;
  }

  // free the mbuf associated with the last sent packet, if there was one
  if (tx_mbufs[tx_index]) {
    mbuffree(tx_mbufs[tx_index]);
  }

  tx_mbufs[tx_index] = m;

  tx_desc->addr    = (uint64)m->head;
  tx_desc->length  = m->len;
  tx_desc->cmd    |= E1000_TXD_CMD_RS | E1000_TXD_CMD_EOP;

  regs[E1000_TDT] = (tx_index + 1 == TX_RING_SIZE) ? 0 : tx_index + 1;

  release(&e1000_tx_lock);

  return 0;
}

static void
e1000_recv(void)
{
  acquire(&e1000_rx_lock);

  // the index of the last processed rx_desc in the ring
  int rx_index = regs[E1000_RDT];

  while (1) {
    // move on to the next entry in the ring, which may wrap around to 0
    if (++rx_index == RX_RING_SIZE) {
      rx_index = 0;
    }

    struct rx_desc *rx_desc = &rx_ring[rx_index];
    struct mbuf    *rx_mbuf = rx_mbufs[rx_index];

    // check to see if the e1000 card has put anything in this rx_desc
    if ((rx_desc->status & E1000_RXD_STAT_DD) == 0) {
      break;
    }

    // set the length of the mbuf to the length of the received packet
    rx_mbuf->len = rx_desc->length;

    // and then pass it off to the rest of the networking stack
    net_rx(rx_mbuf);

    // allocate a new mbuf for this rx_desc since we've handed off the last one
    if (!(rx_mbufs[rx_index] = mbufalloc(0))) {
      panic("e1000_recv");
    }

    // point the rx_desc to this new mbuf and clear the status field so the e1000 can set it
    rx_desc->addr   = (uint64)rx_mbufs[rx_index]->head;
    rx_desc->status = 0;

    // mark our progress in the ring buffer
    regs[E1000_RDT] = rx_index;
  }

  release(&e1000_rx_lock);
}

void
e1000_intr(void)
{
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xffffffff;

  e1000_recv();
}
