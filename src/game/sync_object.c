#include "sync_object.h"

#include "engine/graph_node.h"
#include "game/area.h"
#include "game/game_init.h"
#include "model_ids.h"
#include "object_constants.h"
#include "object_helpers.h"
#include "object_list_processor.h"
#include "object_fields.h"
#include "sm64_usb_protocol.h"
#include "spawn_object.h"

#define SYNC_LOCAL_PLAYER_SLOT 0
#define SYNC_AUTHORITY_LOCAL 1
#define SYNC_RESEND_INTERVAL 15
#define SYNC_REMOTE_STALE_FRAMES 90

static struct SyncObject sSyncObjects[SYNC_OBJECT_POOL_CAPACITY];
static u32 sNextSyncId = (SYNC_ID_BLOCK_SIZE / 2);
static u16 sSyncGeneration = 1;
static u8 sSyncSystemReady = FALSE;
static u8 sSyncTxPackets[SYNC_OBJECT_TX_QUEUE_CAPACITY][SYNC_OBJECT_PACKET_SIZE];
static u32 sSyncTxReadIndex = 0;
static u32 sSyncTxWriteIndex = 0;
static u32 sSyncTxCount = 0;

static s32 sync_object_find_slot_by_id(u32 syncId);
static struct SyncObject *sync_object_attach_remote(struct Object *o, u32 syncId,
                                                    const BehaviorScript *behavior,
                                                    u8 ownerSlot, u8 authority, u32 frame);
static struct Object *sync_object_spawn_remote(const u8 *packet);

static s32 sync_object_frame_is_newer(u32 a, u32 b) {
    return (s32)(a - b) > 0;
}

static u32 sync_object_f32_to_u32(f32 f) {
    union {
        f32 f;
        u32 u;
    } u;
    u.f = f;
    return u.u;
}

static f32 sync_object_u32_to_f32(u32 v) {
    union {
        f32 f;
        u32 u;
    } u;
    u.u = v;
    return u.f;
}

static u32 sync_object_mix_hash(u32 hash, u32 value) {
    hash ^= value + 0x9E3779B9u + (hash << 6) + (hash >> 2);
    return hash;
}

static u32 sync_object_compute_hash(const struct Object *o) {
    u32 hash = 2166136261u;

    hash = sync_object_mix_hash(hash, o->oSyncID);
    hash = sync_object_mix_hash(hash, (u32)o->oAction);
    hash = sync_object_mix_hash(hash, (u32)o->oSubAction);
    hash = sync_object_mix_hash(hash, (u32)o->oTimer);
    hash = sync_object_mix_hash(hash, (u32)o->oAnimState);
    hash = sync_object_mix_hash(hash, (u32)o->oHeldState);
    hash = sync_object_mix_hash(hash, o->oSyncDeath);
    hash = sync_object_mix_hash(hash, sync_object_f32_to_u32(o->oPosX));
    hash = sync_object_mix_hash(hash, sync_object_f32_to_u32(o->oPosY));
    hash = sync_object_mix_hash(hash, sync_object_f32_to_u32(o->oPosZ));
    hash = sync_object_mix_hash(hash, sync_object_f32_to_u32(o->oVelX));
    hash = sync_object_mix_hash(hash, sync_object_f32_to_u32(o->oVelY));
    hash = sync_object_mix_hash(hash, sync_object_f32_to_u32(o->oVelZ));
    hash = sync_object_mix_hash(hash, (u32)o->oFaceAnglePitch);
    hash = sync_object_mix_hash(hash, (u32)o->oFaceAngleYaw);
    hash = sync_object_mix_hash(hash, (u32)o->oFaceAngleRoll);
    hash = sync_object_mix_hash(hash, (u32)o->oMoveAngleYaw);
    return hash;
}

static u8 sync_object_enqueue_packet(const u8 *packet) {
    u32 i;
    u8 *dst;

    if (sSyncTxCount >= SYNC_OBJECT_TX_QUEUE_CAPACITY) {
        return FALSE;
    }

    dst = sSyncTxPackets[sSyncTxWriteIndex];
    for (i = 0; i < SYNC_OBJECT_PACKET_SIZE; i++) {
        dst[i] = packet[i];
    }

    sSyncTxWriteIndex++;
    if (sSyncTxWriteIndex >= SYNC_OBJECT_TX_QUEUE_CAPACITY) {
        sSyncTxWriteIndex = 0;
    }
    sSyncTxCount++;
    return TRUE;
}

/*
 * Fixed object block format (80 bytes, no inner header):
 *   0  ownerSlot   (u8)
 *   1  authority   (u8)
 *   2  levelNum    (u8)
 *   3  heldState   (u8)
 *   4  syncId      (u32 be)
 *   8  frame       (u32 be)
 *   12 behavior    (u32 be)
 *   16 action      (u32 be)
 *   20 subAction   (u32 be)
 *   24 animState   (u32 be)
 *   28 facePitch   (s16 be)
 *   30 faceYaw     (s16 be)
 *   32 faceRoll    (s16 be)
 *   34 moveYaw     (s16 be)
 *   36 posX        (f32 bits u32 be)
 *   40 posY        (f32 bits u32 be)
 *   44 posZ        (f32 bits u32 be)
 *   48 velX        (f32 bits u32 be)
 *   52 velY        (f32 bits u32 be)
 *   56 velZ        (f32 bits u32 be)
 *   60 timer       (u32 be)
 *   64 syncDeath   (u32 be)
 *   68 coopFlags   (u32 be)
 *   72 areaIndex   (u8)
 *   73-79 reserved
 */
static u8 sync_object_serialize(const struct SyncObject *so, u8 *out) {
    const struct Object *o;
    u32 coopFlags;

    if (so == NULL || so->o == NULL || out == NULL) {
        return FALSE;
    }

    o = so->o;
    coopFlags = o->oCoopFlags & (COOP_OBJ_FLAG_NETWORK | COOP_OBJ_FLAG_LUA);

    out[0] = so->ownerSlot;
    out[1] = so->authority;
    out[2] = (u8)gCurrLevelNum;
    out[3] = (u8)o->oHeldState;

    sm64usb_write_be32(&out[4], so->id);
    sm64usb_write_be32(&out[8], (u32)gGlobalTimer);
    sm64usb_write_be32(&out[12], (u32)(uintptr_t)o->behavior);
    sm64usb_write_be32(&out[16], (u32)o->oAction);
    sm64usb_write_be32(&out[20], (u32)o->oSubAction);
    sm64usb_write_be32(&out[24], (u32)o->oAnimState);
    sm64usb_write_be16(&out[28], (u16)o->oFaceAnglePitch);
    sm64usb_write_be16(&out[30], (u16)o->oFaceAngleYaw);
    sm64usb_write_be16(&out[32], (u16)o->oFaceAngleRoll);
    sm64usb_write_be16(&out[34], (u16)o->oMoveAngleYaw);
    sm64usb_write_be32(&out[36], sync_object_f32_to_u32(o->oPosX));
    sm64usb_write_be32(&out[40], sync_object_f32_to_u32(o->oPosY));
    sm64usb_write_be32(&out[44], sync_object_f32_to_u32(o->oPosZ));
    sm64usb_write_be32(&out[48], sync_object_f32_to_u32(o->oVelX));
    sm64usb_write_be32(&out[52], sync_object_f32_to_u32(o->oVelY));
    sm64usb_write_be32(&out[56], sync_object_f32_to_u32(o->oVelZ));
    sm64usb_write_be32(&out[60], (u32)o->oTimer);
    sm64usb_write_be32(&out[64], o->oSyncDeath);
    sm64usb_write_be32(&out[68], coopFlags);
    out[72] = (u8)o->header.gfx.areaIndex;
    out[73] = 0;
    out[74] = 0;
    out[75] = 0;
    out[76] = 0;
    out[77] = 0;
    out[78] = 0;
    out[79] = 0;

    return TRUE;
}

static void sync_object_apply_remote_state(struct SyncObject *so, const u8 *packet) {
    struct Object *o;
    u32 syncId;
    u32 frame;
    u32 behaviorPtr;
    u32 coopFlags;
    u8 ownerSlot;
    u8 authority;
    s16 facePitch;
    s16 faceYaw;
    s16 faceRoll;
    s16 moveYaw;
    f32 posX;
    f32 posY;
    f32 posZ;
    f32 velX;
    f32 velY;
    f32 velZ;

    if (so == NULL || so->o == NULL || packet == NULL) {
        return;
    }

    o = so->o;
    syncId = (u32)sm64usb_read_be32(&packet[4]);
    frame = (u32)sm64usb_read_be32(&packet[8]);
    behaviorPtr = (u32)sm64usb_read_be32(&packet[12]);
    ownerSlot = packet[0];
    authority = packet[1];
    coopFlags = (u32)sm64usb_read_be32(&packet[68]);
    facePitch = (s16)sm64usb_read_be16(&packet[28]);
    faceYaw = (s16)sm64usb_read_be16(&packet[30]);
    faceRoll = (s16)sm64usb_read_be16(&packet[32]);
    moveYaw = (s16)sm64usb_read_be16(&packet[34]);
    posX = sync_object_u32_to_f32((u32)sm64usb_read_be32(&packet[36]));
    posY = sync_object_u32_to_f32((u32)sm64usb_read_be32(&packet[40]));
    posZ = sync_object_u32_to_f32((u32)sm64usb_read_be32(&packet[44]));
    velX = sync_object_u32_to_f32((u32)sm64usb_read_be32(&packet[48]));
    velY = sync_object_u32_to_f32((u32)sm64usb_read_be32(&packet[52]));
    velZ = sync_object_u32_to_f32((u32)sm64usb_read_be32(&packet[56]));

    if (syncId != so->id) {
        return;
    }
    if (ownerSlot == SYNC_LOCAL_PLAYER_SLOT) {
        return;
    }
    if ((u32)(uintptr_t)o->behavior != behaviorPtr) {
        return;
    }
    if (!sync_object_frame_is_newer(frame, so->lastRecvFrame)) {
        return;
    }

    so->ownerSlot = ownerSlot;
    so->authority = authority;
    so->lastRecvFrame = frame;
    so->lastUpdateFrame = frame;

    o->oAction = (s32)sm64usb_read_be32(&packet[16]);
    o->oSubAction = (s32)sm64usb_read_be32(&packet[20]);
    o->oAnimState = (s32)sm64usb_read_be32(&packet[24]);
    o->oHeldState = packet[3];
    o->oTimer = (s32)sm64usb_read_be32(&packet[60]);
    o->oSyncDeath = (u32)sm64usb_read_be32(&packet[64]);
    o->oPosX = posX;
    o->oPosY = posY;
    o->oPosZ = posZ;
    o->oVelX = velX;
    o->oVelY = velY;
    o->oVelZ = velZ;
    o->oFaceAnglePitch = facePitch;
    o->oFaceAngleYaw = faceYaw;
    o->oFaceAngleRoll = faceRoll;
    o->oMoveAngleYaw = moveYaw;
    o->oCoopFlags = (o->oCoopFlags & (COOP_OBJ_FLAG_INITIALIZED | COOP_OBJ_FLAG_NON_SYNC))
                  | (coopFlags & (COOP_OBJ_FLAG_NETWORK | COOP_OBJ_FLAG_LUA));

    o->header.gfx.pos[0] = posX;
    o->header.gfx.pos[1] = posY;
    o->header.gfx.pos[2] = posZ;
    o->header.gfx.angle[0] = facePitch;
    o->header.gfx.angle[1] = faceYaw;
    o->header.gfx.angle[2] = faceRoll;
    o->header.gfx.areaIndex = packet[72];
}

static s32 sync_object_find_slot_by_id(u32 syncId) {
    s32 i;
    for (i = 0; i < SYNC_OBJECT_POOL_CAPACITY; i++) {
        if (!sSyncObjects[i].valid) {
            continue;
        }
        if (sSyncObjects[i].id == syncId) {
            return i;
        }
    }
    return -1;
}

static s32 sync_object_find_slot_by_object(struct Object *o) {
    s32 i;
    for (i = 0; i < SYNC_OBJECT_POOL_CAPACITY; i++) {
        if (!sSyncObjects[i].valid) {
            continue;
        }
        if (sSyncObjects[i].o == o) {
            return i;
        }
    }
    return -1;
}

static s32 sync_object_find_free_slot(void) {
    s32 i;
    for (i = 0; i < SYNC_OBJECT_POOL_CAPACITY; i++) {
        if (!sSyncObjects[i].valid) {
            return i;
        }
    }
    return -1;
}

static struct GraphNode *sync_object_find_shared_child(const BehaviorScript *behavior) {
    s32 i;

    if (behavior == NULL) {
        return NULL;
    }

    for (i = 0; i < OBJECT_POOL_CAPACITY; i++) {
        struct Object *obj = &gObjectPool[i];

        if (!(obj->activeFlags & ACTIVE_FLAG_ACTIVE)) {
            continue;
        }
        if (obj->behavior != behavior) {
            continue;
        }
        if (obj->header.gfx.sharedChild == NULL) {
            continue;
        }
        return obj->header.gfx.sharedChild;
    }

    return NULL;
}

static struct SyncObject *sync_object_attach_remote(struct Object *o, u32 syncId,
                                                    const BehaviorScript *behavior,
                                                    u8 ownerSlot, u8 authority, u32 frame) {
    struct SyncObject *so;

    if (o == NULL || behavior == NULL || syncId == SYNC_ID_NONE) {
        return NULL;
    }

    so = sync_object_get(o->oSyncID);
    if (so == NULL || so->o != o) {
        return NULL;
    }

    so->id = syncId;
    so->o = o;
    so->behavior = behavior;
    so->lastUpdateFrame = frame;
    so->lastSyncHash = sync_object_compute_hash(o);
    so->lastSentFrame = 0;
    so->lastRecvFrame = frame;
    so->ownerSlot = ownerSlot;
    so->authority = authority;
    so->valid = TRUE;
    so->dirty = FALSE;

    o->oSyncID = syncId;
    o->oSyncDeath = 0;
    o->oCoopFlags |= COOP_OBJ_FLAG_INITIALIZED | COOP_OBJ_FLAG_NETWORK;
    return so;
}

static struct Object *sync_object_spawn_remote(const u8 *packet) {
    const BehaviorScript *behavior;
    struct GraphNode *sharedChild;
    struct Object *obj;
    struct Object *parent;
    u32 behaviorPtr;
    u8 areaIndex;

    if (packet == NULL) {
        return NULL;
    }

    behaviorPtr = (u32)sm64usb_read_be32(&packet[12]);
    if (behaviorPtr == 0) {
        return NULL;
    }

    behavior = (const BehaviorScript *)(uintptr_t)behaviorPtr;
    areaIndex = packet[72];
    parent = (gMarioObject != NULL) ? gMarioObject : &gMacroObjectDefaultParent;

    obj = create_object(behavior);
    if (obj == NULL) {
        return NULL;
    }

    obj->parentObj = parent;
    obj->header.gfx.areaIndex = areaIndex;
    obj->header.gfx.activeAreaIndex = areaIndex;
    geo_obj_init((struct GraphNodeObject *)&obj->header.gfx, gLoadedGraphNodes[MODEL_NONE],
                 gVec3fZero, gVec3sZero);

    sharedChild = sync_object_find_shared_child(behavior);
    if (sharedChild != NULL) {
        obj->header.gfx.sharedChild = sharedChild;
    }

    return obj;
}

void sync_object_system_init(void) {
    if (sSyncSystemReady) {
        return;
    }
    sync_object_system_reset();
    sSyncSystemReady = TRUE;
}

void sync_object_system_reset(void) {
    s32 i;

    for (i = 0; i < SYNC_OBJECT_POOL_CAPACITY; i++) {
        if (sSyncObjects[i].valid && sSyncObjects[i].o != NULL) {
            sSyncObjects[i].o->oSyncID = 0;
            sSyncObjects[i].o->oSyncDeath = 0;
            sSyncObjects[i].o->oCoopFlags = 0;
        }
        sSyncObjects[i].id = SYNC_ID_NONE;
        sSyncObjects[i].o = NULL;
        sSyncObjects[i].behavior = NULL;
        sSyncObjects[i].maxSyncDistance = 0.0f;
        sSyncObjects[i].generation = 0;
        sSyncObjects[i].randomSeed = 0;
        sSyncObjects[i].lastUpdateFrame = 0;
        sSyncObjects[i].lastSyncHash = 0;
        sSyncObjects[i].lastSentFrame = 0;
        sSyncObjects[i].lastRecvFrame = 0;
        sSyncObjects[i].ownerSlot = 0;
        sSyncObjects[i].authority = 0;
        sSyncObjects[i].valid = FALSE;
        sSyncObjects[i].dirty = FALSE;
    }

    sSyncTxReadIndex = 0;
    sSyncTxWriteIndex = 0;
    sSyncTxCount = 0;

    sNextSyncId = (SYNC_ID_BLOCK_SIZE / 2);
    sSyncGeneration++;
    if (sSyncGeneration == 0) {
        sSyncGeneration = 1;
    }
}

void sync_object_system_update(void) {
    s32 i;
    u32 now = gGlobalTimer;

    for (i = 0; i < SYNC_OBJECT_POOL_CAPACITY; i++) {
        struct SyncObject *so = &sSyncObjects[i];
        struct Object *o;
        u32 hash;
        u8 packet[SYNC_OBJECT_PACKET_SIZE];

        if (!so->valid || so->o == NULL) {
            continue;
        }

        o = so->o;
        if (!(o->activeFlags & ACTIVE_FLAG_ACTIVE)) {
            continue;
        }
        if (o->oCoopFlags & COOP_OBJ_FLAG_NON_SYNC) {
            continue;
        }
        if (so->ownerSlot != SYNC_LOCAL_PLAYER_SLOT) {
            if ((u32)(now - so->lastRecvFrame) > (u32)SYNC_REMOTE_STALE_FRAMES) {
                mark_obj_for_deletion(o);
            }
            continue;
        }

        so->authority = SYNC_AUTHORITY_LOCAL;

        hash = sync_object_compute_hash(o);
        so->dirty = (hash != so->lastSyncHash);
        if (!so->dirty && (now - so->lastSentFrame) < SYNC_RESEND_INTERVAL) {
            continue;
        }

        if (sync_object_serialize(so, packet)) {
            if (sync_object_enqueue_packet(packet)) {
                so->lastSyncHash = hash;
                so->lastSentFrame = now;
                so->lastUpdateFrame = now;
                so->dirty = FALSE;
            }
        }
    }
}

u32 sync_object_generate_id(void) {
    u32 attempts;
    u32 candidate;

    for (attempts = 0; attempts < SYNC_ID_BLOCK_SIZE; attempts++) {
        sNextSyncId++;
        sNextSyncId %= SYNC_ID_BLOCK_SIZE;
        if (sNextSyncId == SYNC_ID_NONE) {
            continue;
        }

        candidate = sNextSyncId;
        if (sync_object_find_slot_by_id(candidate) < 0) {
            return candidate;
        }
    }

    return SYNC_ID_NONE;
}

struct SyncObject *sync_object_get(u32 syncId) {
    s32 slot;
    if (syncId == SYNC_ID_NONE) {
        return NULL;
    }
    slot = sync_object_find_slot_by_id(syncId);
    if (slot < 0) {
        return NULL;
    }
    return &sSyncObjects[slot];
}

u8 sync_object_is_initialized(u32 syncId) {
    struct SyncObject *so = sync_object_get(syncId);
    if (so == NULL || so->o == NULL) {
        return FALSE;
    }
    return (so->o->oCoopFlags & COOP_OBJ_FLAG_INITIALIZED) != 0;
}

struct SyncObject *sync_object_init(struct Object *o, f32 maxSyncDistance) {
    s32 slot;
    u32 syncId;
    struct SyncObject *so;

    if (o == NULL) {
        return NULL;
    }

    if (!sSyncSystemReady) {
        sync_object_system_init();
    }

    if (o->oCoopFlags & COOP_OBJ_FLAG_NON_SYNC) {
        return NULL;
    }

    if (o->oSyncID != SYNC_ID_NONE) {
        so = sync_object_get(o->oSyncID);
        if (so != NULL && so->o == o) {
            return so;
        }
        o->oSyncID = SYNC_ID_NONE;
    }

    slot = sync_object_find_slot_by_object(o);
    if (slot >= 0) {
        return &sSyncObjects[slot];
    }

    slot = sync_object_find_free_slot();
    if (slot < 0) {
        o->oSyncID = SYNC_ID_NONE;
        return NULL;
    }

    syncId = sync_object_generate_id();
    if (syncId == SYNC_ID_NONE) {
        o->oSyncID = SYNC_ID_NONE;
        return NULL;
    }

    so = &sSyncObjects[slot];
    so->id = syncId;
    so->o = o;
    so->behavior = o->behavior;
    so->maxSyncDistance = maxSyncDistance;
    so->generation = sSyncGeneration;
    so->randomSeed = (u16)(syncId * 7951);
    so->lastUpdateFrame = gGlobalTimer;
    so->lastSyncHash = 0;
    so->lastSentFrame = 0;
    so->lastRecvFrame = 0;
    so->ownerSlot = SYNC_LOCAL_PLAYER_SLOT;
    so->authority = SYNC_AUTHORITY_LOCAL;
    so->valid = TRUE;
    so->dirty = TRUE;

    o->oSyncID = syncId;
    o->oSyncDeath = 0;
    o->oCoopFlags |= COOP_OBJ_FLAG_INITIALIZED;

    return so;
}

void sync_object_forget(struct Object *o) {
    s32 slot;

    if (o == NULL) {
        return;
    }

    slot = sync_object_find_slot_by_object(o);
    if (slot < 0) {
        o->oSyncID = SYNC_ID_NONE;
        o->oSyncDeath = 0;
        o->oCoopFlags = 0;
        return;
    }

    sSyncObjects[slot].id = SYNC_ID_NONE;
    sSyncObjects[slot].o = NULL;
    sSyncObjects[slot].behavior = NULL;
    sSyncObjects[slot].maxSyncDistance = 0.0f;
    sSyncObjects[slot].generation = 0;
    sSyncObjects[slot].randomSeed = 0;
    sSyncObjects[slot].lastUpdateFrame = 0;
    sSyncObjects[slot].lastSyncHash = 0;
    sSyncObjects[slot].lastSentFrame = 0;
    sSyncObjects[slot].lastRecvFrame = 0;
    sSyncObjects[slot].ownerSlot = 0;
    sSyncObjects[slot].authority = 0;
    sSyncObjects[slot].valid = FALSE;
    sSyncObjects[slot].dirty = FALSE;

    o->oSyncID = SYNC_ID_NONE;
    o->oSyncDeath = 0;
    o->oCoopFlags = 0;
}

void sync_object_on_unload(struct Object *o) {
    sync_object_forget(o);
}

u8 sync_object_pop_outgoing_packet(u8 *dst, u32 dstSize) {
    const u8 *src;
    u32 i;

    if (dst == NULL || dstSize < SYNC_OBJECT_PACKET_SIZE) {
        return FALSE;
    }
    if (sSyncTxCount == 0) {
        return FALSE;
    }

    src = sSyncTxPackets[sSyncTxReadIndex];
    for (i = 0; i < SYNC_OBJECT_PACKET_SIZE; i++) {
        dst[i] = src[i];
    }

    sSyncTxReadIndex++;
    if (sSyncTxReadIndex >= SYNC_OBJECT_TX_QUEUE_CAPACITY) {
        sSyncTxReadIndex = 0;
    }
    sSyncTxCount--;
    return TRUE;
}

void sync_object_consume_packet(const u8 *data, u32 len) {
    u32 offset = 0;

    if (data == NULL || len == 0) {
        return;
    }

    while (offset + SYNC_OBJECT_PACKET_SIZE <= len) {
        const u8 *packet = &data[offset];
        struct SyncObject *so;
        u32 syncId;
        u32 frame;

        if (packet[2] != (u8)gCurrLevelNum) {
            offset += SYNC_OBJECT_PACKET_SIZE;
            continue;
        }

        syncId = (u32)sm64usb_read_be32(&packet[4]);
        if (syncId == SYNC_ID_NONE) {
            offset += SYNC_OBJECT_PACKET_SIZE;
            continue;
        }

        frame = (u32)sm64usb_read_be32(&packet[8]);
        so = sync_object_get(syncId);
        if (so == NULL && packet[0] != SYNC_LOCAL_PLAYER_SLOT) {
            struct Object *obj = sync_object_spawn_remote(packet);

            if (obj != NULL) {
                so = sync_object_attach_remote(
                    obj,
                    syncId,
                    (const BehaviorScript *)(uintptr_t)sm64usb_read_be32(&packet[12]),
                    packet[0],
                    packet[1],
                    frame);
            }
        }
        if (so != NULL) {
            sync_object_apply_remote_state(so, packet);
        }
        offset += SYNC_OBJECT_PACKET_SIZE;
    }
}
