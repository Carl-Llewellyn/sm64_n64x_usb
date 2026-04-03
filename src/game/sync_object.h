#ifndef SYNC_OBJECT_H
#define SYNC_OBJECT_H

#include <PR/ultratypes.h>

#include "types.h"

#define SYNC_ID_NONE 0
#define SYNC_ID_BLOCK_SIZE 4096
#define SYNC_OBJECT_POOL_CAPACITY 512
#define SYNC_OBJECT_PACKET_SIZE 80
#define SYNC_OBJECT_TX_QUEUE_CAPACITY 128

struct SyncObject {
    u32 id;
    struct Object *o;
    const BehaviorScript *behavior;
    f32 maxSyncDistance;
    u16 generation;
    u16 randomSeed;
    u32 lastUpdateFrame;
    u32 lastSyncHash;
    u32 lastSentFrame;
    u32 lastRecvFrame;
    u8 ownerSlot;
    u8 authority;
    u8 valid;
    u8 dirty;
};

void sync_object_system_init(void);
void sync_object_system_reset(void);
void sync_object_system_update(void);

struct SyncObject *sync_object_init(struct Object *o, f32 maxSyncDistance);
struct SyncObject *sync_object_get(u32 syncId);
void sync_object_forget(struct Object *o);
void sync_object_on_unload(struct Object *o);
u32 sync_object_generate_id(void);
u8 sync_object_is_initialized(u32 syncId);
u8 sync_object_pop_outgoing_packet(u8 *dst, u32 dstSize);
void sync_object_consume_packet(const u8 *data, u32 len);

#endif // SYNC_OBJECT_H
