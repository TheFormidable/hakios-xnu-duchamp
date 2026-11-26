/* SPDX-License-Identifier: MIT
 * Marks of the GPU's progress, for the panel's wait (boot-arg gpu_mark=1).
 *
 * Trial 72: the presenter waited 27 ms a frame for the GPU while the GPU kept each submit 16 ms: it waited for every
 * queue of every group to go idle (hakios_csf_idle), so with asynchronous submits frame N also waited for frame N+1's
 * work. CoreAnimation submits a frame's work before its swap (commitAndWaitUntilSubmitted), so a mark of the sequence
 * each queue was issued up to, taken when the swap comes, is that frame's work: once every queue has reached it, the
 * frame is drawn. Pure functions over the caller's snapshot of the groups, so they run on the host too.
 */
#ifndef HAKIOS_GPU_MARK_H
#define HAKIOS_GPU_MARK_H

#include <stdint.h>
#include <string.h>

#define GPU_MARKS 64          /* marks kept; an older one is not found, and its waiter waits as before (every queue idle) */
#define GPU_MARK_GROUPS 32    /* the backend's group slots */

struct gpu_mark {
 uint64_t ticket;                               /* 1, 2, ...: 0 is no mark */
 const void *group[GPU_MARK_GROUPS];            /* the group in each slot when the mark was taken (NULL: none) */
 uint32_t seq[GPU_MARK_GROUPS][3];              /* the sequence last issued to each of its queues */
 uint8_t count[GPU_MARK_GROUPS];                /* its queues */
};
struct gpu_marks {uint64_t taken;struct gpu_mark ring[GPU_MARKS];};

/* a new mark, cleared, its ticket set: the caller fills in the groups */
static inline struct gpu_mark *gpu_mark_begin(struct gpu_marks *m){
 struct gpu_mark *k=&m->ring[m->taken%GPU_MARKS];
 memset(k,0,sizeof(*k));
 k->ticket=++m->taken;
 return k;
}

/* the mark of `ticket`, NULL if there is none or it was written over */
static inline const struct gpu_mark *gpu_mark_find(const struct gpu_marks *m,uint64_t ticket){
 if(!ticket || ticket>m->taken || m->taken-ticket>=GPU_MARKS)return NULL;
 const struct gpu_mark *k=&m->ring[(ticket-1)%GPU_MARKS];
 return k->ticket==ticket?k:NULL;
}

/* 1 when every queue the mark saw has reached its sequence (sequences wrap), given the groups now in each slot and
 * their queues' fences; a group gone, or another in its slot, holds nothing: its work was done or dropped */
static inline int gpu_mark_reached(const struct gpu_mark *k,const void *const group[GPU_MARK_GROUPS],
 const uint32_t fence[GPU_MARK_GROUPS][3]){
 for(unsigned s=0;s<GPU_MARK_GROUPS;s++){
  if(!k->group[s] || k->group[s]!=group[s])continue;
  for(unsigned i=0;i<k->count[s] && i<3;i++)if((int32_t)(fence[s][i]-k->seq[s][i])<0)return 0;
 }
 return 1;
}

#endif
