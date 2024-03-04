// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

struct {
  struct bcache_bucket {
    struct spinlock lock;
    struct buf     *head;
  } table[13];

  struct spinlock lock;
  struct buf      buf[NBUF];
} bcache;

void
binit(void)
{
  initlock(&bcache.lock, "bcache");

  // initialize the sharded locks
  for (int i = 0; i < NELEM(bcache.table); i++) {
    struct bcache_bucket *b = bcache.table + i;

    initlock(&b->lock, "bcache shard");

    b->head = 0;
  }

  // initalize the lock for each buf, and allocate them in advance to one of the sharded locks
  for (int i = 0; i < NELEM(bcache.buf); i++) {
    struct buf           *buf    = bcache.buf + i;
    struct bcache_bucket *bucket = bcache.table + (i % NELEM(bcache.table));

    initsleeplock(&buf->lock, "buffer");

    buf->next    = bucket->head;
    bucket->head = buf;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  // grab the bucket that corresponds to the given blockno
  struct bcache_bucket *bucket = bcache.table + (blockno % NELEM(bcache.table));

  // then acquire the lock before inspecting the list
  acquire(&bucket->lock);

  for (struct buf *b = bucket->head; b != 0; b = b->next) {
    // is the buf for this block already in the list?
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;

      release(&bucket->lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // since we didn't find it in the list, we'll look for a free buf in the list we already have a
  // lock for
  for (struct buf *b = bucket->head; b != 0; b = b->next) {
    if (b->refcnt == 0) {
      b->refcnt++;

      b->dev     = dev;
      b->blockno = blockno;

      b->valid  = 0;
      b->refcnt = 1;

      release(&bucket->lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // no suitable buf was found, so we have to give up the lock to let other threads proceed
  release(&bucket->lock);

  // we're now going to attempt to steal a buf from a different shard, so we must first grab the
  // "bcache" lock, which we use as a way to grant permission to hold multiple lock shards.
  //
  // since only one process can have this lock, we know that we can't run into a scenario where
  // process A holds lock 1, and process B holds lock 2, and they're both trying to acquire the
  // lock the other process has.
  acquire(&bcache.lock);

  // grab the original bucket lock before we start our journey
  acquire(&bucket->lock);

  // the bucket we're attempting to steal from. starts at the bucket to the right of the bucket
  // we expected to find the buf in
  struct bcache_bucket *victim = bucket;

  while (1) {
    // move to the next lock shard, potentially looping around
    if (++victim == bcache.table + NELEM(bcache.table)) {
      victim = bcache.table;
    }

    // we've looped around completely
    if (victim == bucket) {
      panic("bget: no buffers");
    }

    acquire(&victim->lock);

    // look for a suitable (i.e. unused) buf from the victim bucket
    for (struct buf *curr = victim->head, *prev = 0; curr != 0; prev = curr, curr = curr->next) {
      if (curr->refcnt == 0) {
        curr->dev     = dev;
        curr->blockno = blockno;

        curr->valid  = 0;
        curr->refcnt = 1;

        // remove curr from the victim
        if (prev) {
          prev->next = curr->next;
        } else {
          victim->head = curr->next;
        }

        // append cur to the target
        curr->next   = bucket->head;
        bucket->head = curr;

        release(&bucket->lock);
        release(&victim->lock);
        release(&bcache.lock);

        acquiresleep(&curr->lock);

        return curr;
      }
    }

    release(&victim->lock);
  }
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  b->refcnt--;

  releasesleep(&b->lock);
}

void
bpin(struct buf *b)
{
  b->refcnt++;
}

void
bunpin(struct buf *b)
{
  b->refcnt--;
}
