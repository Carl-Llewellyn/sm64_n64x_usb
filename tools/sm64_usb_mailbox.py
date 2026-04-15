#!/usr/bin/env python3
import argparse
import math
import mmap
import os
import struct
import sys
import time


MAGIC = 0x534D3634  # "SM64"
VERSION = 1
MAILBOX_PATH = "/tmp/ares-sm64-usb-mailbox.bin"
BUFFER_SIZE = 256
HEADER_FMT = "<6I"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
FILE_SIZE = HEADER_SIZE + BUFFER_SIZE + BUFFER_SIZE
ACTIVE_FLAG_ACTIVE = 0x0001
ACTIVE_FLAG_UNK8 = 0x0100
GRAPH_RENDER_ACTIVE = 0x0001
GRAPH_RENDER_HAS_ANIMATION = 0x0020

TRANSPORT_HEADER_SIZE = 6
PLAYER_BLOCK_SIZE = 30
OBJECT_BLOCK_SIZE = 140
FRAME_PAYLOAD_SIZE = PLAYER_BLOCK_SIZE + OBJECT_BLOCK_SIZE
OBJECT_OFFSET = TRANSPORT_HEADER_SIZE + PLAYER_BLOCK_SIZE
DEFAULT_GOOMBA_BEHAVIOR_PTR = 0x13000F4C
MODEL_WOODEN_SIGNPOST = 0x7C
MODEL_GOOMBA = 0xC0
CASTLE_GROUNDS_LEVEL = 16
CASTLE_GROUNDS_AREA = 1
CASTLE_GROUNDS_SIGN_X = 1740.0
CASTLE_GROUNDS_SIGN_Y = 35.0
CASTLE_GROUNDS_SIGN_Z = 2500.0


def read_u16be(buf, off):
    return (buf[off] << 8) | buf[off + 1]


def read_u32be(buf, off):
    return (buf[off] << 24) | (buf[off + 1] << 16) | (buf[off + 2] << 8) | buf[off + 3]


def write_u16be(buf, off, value):
    buf[off + 0] = (value >> 8) & 0xFF
    buf[off + 1] = value & 0xFF


def write_u32be(buf, off, value):
    buf[off + 0] = (value >> 24) & 0xFF
    buf[off + 1] = (value >> 16) & 0xFF
    buf[off + 2] = (value >> 8) & 0xFF
    buf[off + 3] = value & 0xFF


def object_block_usable(frame):
    obj = frame[OBJECT_OFFSET:OBJECT_OFFSET + OBJECT_BLOCK_SIZE]
    return any(obj) and read_u32be(obj, 4) != 0 and read_u32be(obj, 12) != 0


def ensure_mailbox(path):
    fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o666)
    os.ftruncate(fd, FILE_SIZE)
    mm = mmap.mmap(fd, FILE_SIZE, access=mmap.ACCESS_WRITE)
    if len(mm) != FILE_SIZE:
      raise RuntimeError("unexpected mailbox size")
    magic, version, tx_seq, rx_seq, tx_len, rx_len = struct.unpack_from(HEADER_FMT, mm, 0)
    if magic != MAGIC or version != VERSION:
        struct.pack_into(HEADER_FMT, mm, 0, MAGIC, VERSION, 0, 0, 0, 0)
        mm[HEADER_SIZE:HEADER_SIZE + BUFFER_SIZE + BUFFER_SIZE] = b"\x00" * (BUFFER_SIZE + BUFFER_SIZE)
        mm.flush()
    return fd, mm


class LoopbackState:
    def __init__(self, pos_x_offset, pos_y_offset, pos_z_offset, anchor, motion, motion_step,
                 motion_amplitude, source_behavior_ptr, relatch):
        self.pos_x_offset = pos_x_offset
        self.pos_y_offset = pos_y_offset
        self.pos_z_offset = pos_z_offset
        self.anchor = anchor
        self.motion = motion
        self.motion_step = motion_step
        self.motion_amplitude = motion_amplitude
        self.source_behavior_ptr = source_behavior_ptr
        self.relatch = relatch
        self.latched_object = None
        self.injected_sync_id = None
        self.injected_frame = 1000
        self.motion_tick = 0

    def consider_latch(self, tx_buf):
        if not object_block_usable(tx_buf):
            return False

        obj = bytearray(tx_buf[OBJECT_OFFSET:OBJECT_OFFSET + OBJECT_BLOCK_SIZE])
        behavior = read_u32be(obj, 12)

        if self.source_behavior_ptr is not None and behavior != self.source_behavior_ptr:
            return False
        if self.latched_object is not None and not self.relatch:
            return False

        self.latched_object = obj
        original_sync_id = read_u32be(obj, 4)
        self.injected_sync_id = (original_sync_id ^ 0x100) or (original_sync_id ^ 1) or 1
        self.injected_frame = 1000
        self.motion_tick = 0
        return True

    def build_reply(self, tx_buf):
        rx = bytearray(BUFFER_SIZE)
        rx[0] = 0x53
        rx[1] = 0x4D
        rx[2] = 0x01
        rx[3] = 0x00
        write_u16be(rx, 4, FRAME_PAYLOAD_SIZE)
        rx[TRANSPORT_HEADER_SIZE + 0] = 0xFF

        self.consider_latch(tx_buf)

        if self.latched_object is None:
            return rx

        obj = bytearray(self.latched_object)
        rx[TRANSPORT_HEADER_SIZE + 25] = obj[2]
        obj[0] = 1
        obj[1] = 1
        write_u32be(obj, 4, self.injected_sync_id)
        write_u32be(obj, 8, self.injected_frame)
        self.injected_frame += 1
        write_u32be(obj, 68, read_u32be(obj, 68) | 1)

        if self.anchor == "mario":
            player = tx_buf[TRANSPORT_HEADER_SIZE:TRANSPORT_HEADER_SIZE + PLAYER_BLOCK_SIZE]
            pos_x = struct.unpack(">f", bytes(player[1:5]))[0]
            pos_y = struct.unpack(">f", bytes(player[5:9]))[0]
            pos_z = struct.unpack(">f", bytes(player[9:13]))[0]
        else:
            pos_x = struct.unpack(">f", bytes(obj[36:40]))[0]
            pos_y = struct.unpack(">f", bytes(obj[40:44]))[0]
            pos_z = struct.unpack(">f", bytes(obj[44:48]))[0]

        vel_x = 0.0
        vel_y = 0.0
        vel_z = 0.0

        if self.motion == "line-x":
            pos_x = pos_x + self.pos_x_offset + (self.motion_tick * self.motion_step)
            pos_y = pos_y + self.pos_y_offset
            pos_z = pos_z + self.pos_z_offset
            vel_x = self.motion_step
        elif self.motion == "line-z":
            pos_x = pos_x + self.pos_x_offset
            pos_y = pos_y + self.pos_y_offset
            pos_z = pos_z + self.pos_z_offset + (self.motion_tick * self.motion_step)
            vel_z = self.motion_step
        elif self.motion == "circle":
            angle = self.motion_tick * self.motion_step
            pos_x = pos_x + self.pos_x_offset + math.cos(angle) * self.motion_amplitude
            pos_y = pos_y + self.pos_y_offset
            pos_z = pos_z + self.pos_z_offset + math.sin(angle) * self.motion_amplitude
            vel_x = -math.sin(angle) * self.motion_amplitude * self.motion_step
            vel_z =  math.cos(angle) * self.motion_amplitude * self.motion_step
        else:
            pos_x = pos_x + self.pos_x_offset
            pos_y = pos_y + self.pos_y_offset
            pos_z = pos_z + self.pos_z_offset

        obj[36:40] = struct.pack(">f", pos_x)
        obj[40:44] = struct.pack(">f", pos_y)
        obj[44:48] = struct.pack(">f", pos_z)
        obj[48:52] = struct.pack(">f", vel_x)
        obj[52:56] = struct.pack(">f", vel_y)
        obj[56:60] = struct.pack(">f", vel_z)
        self.motion_tick += 1
        obj[73:80] = b"\x00" * 7
        obj[96] = 0
        obj[97:100] = b"\x00" * 3
        obj[100:108] = b"\x00" * 8
        obj[108:140] = b"\x00" * 32
        rx[OBJECT_OFFSET:OBJECT_OFFSET + OBJECT_BLOCK_SIZE] = obj
        return rx


class SpawnState:
    def __init__(self, behavior_ptr, model_id, bhv_params, sync_id, level, area, x, y, z, pitch, yaw, roll):
        self.behavior_ptr = behavior_ptr
        self.model_id = model_id
        self.bhv_params = bhv_params
        self.sync_id = sync_id
        self.level = level
        self.area = area
        self.x = x
        self.y = y
        self.z = z
        self.pitch = pitch
        self.yaw = yaw
        self.roll = roll
        self.frame = 1000

    def build_reply(self):
        rx = bytearray(BUFFER_SIZE)
        rx[0] = 0x53
        rx[1] = 0x4D
        rx[2] = 0x01
        rx[3] = 0x00
        write_u16be(rx, 4, FRAME_PAYLOAD_SIZE)
        rx[TRANSPORT_HEADER_SIZE + 0] = 0xFF
        rx[TRANSPORT_HEADER_SIZE + 25] = self.level

        obj = bytearray(OBJECT_BLOCK_SIZE)
        obj[0] = 1
        obj[1] = 1
        obj[2] = self.level
        obj[3] = 0
        write_u32be(obj, 4, self.sync_id)
        write_u32be(obj, 8, self.frame)
        write_u32be(obj, 12, self.behavior_ptr)
        write_u32be(obj, 16, 0)
        write_u32be(obj, 20, 0)
        write_u32be(obj, 24, 0)
        obj[28:30] = struct.pack(">h", self.pitch)
        obj[30:32] = struct.pack(">h", self.yaw)
        obj[32:34] = struct.pack(">h", self.roll)
        obj[34:36] = struct.pack(">h", self.yaw)
        obj[36:40] = struct.pack(">f", self.x)
        obj[40:44] = struct.pack(">f", self.y)
        obj[44:48] = struct.pack(">f", self.z)
        obj[48:52] = struct.pack(">f", 0.0)
        obj[52:56] = struct.pack(">f", 0.0)
        obj[56:60] = struct.pack(">f", 0.0)
        write_u32be(obj, 60, 0)
        write_u32be(obj, 64, 0)
        write_u32be(obj, 68, 1)
        obj[72] = self.area
        obj[73] = self.model_id & 0xFF
        write_u32be(obj, 74, self.bhv_params)
        obj[78:80] = b"\x00" * 2
        write_u32be(obj, 80, 0)
        write_u32be(obj, 84, 0)
        write_u16be(obj, 88, ACTIVE_FLAG_ACTIVE | ACTIVE_FLAG_UNK8)
        write_u16be(obj, 90, GRAPH_RENDER_ACTIVE | GRAPH_RENDER_HAS_ANIMATION)
        write_u32be(obj, 92, 0xFFFFFFFF)
        obj[96] = 0
        obj[97:100] = b"\x00" * 3
        obj[100:108] = b"\x00" * 8
        obj[108:140] = b"\x00" * 32
        rx[OBJECT_OFFSET:OBJECT_OFFSET + OBJECT_BLOCK_SIZE] = obj
        self.frame += 1
        return rx


def print_frame(prefix, frame):
    payload_len = read_u16be(frame, 4)
    obj = frame[OBJECT_OFFSET:OBJECT_OFFSET + OBJECT_BLOCK_SIZE]
    print(
        f"{prefix}: len={payload_len} "
        f"obj_sync=0x{read_u32be(obj, 4):08x} "
        f"obj_frame={read_u32be(obj, 8)} "
        f"behavior=0x{read_u32be(obj, 12):08x} "
        f"level={obj[2]} owner={obj[0]} authority={obj[1]}",
        flush=True,
    )


def print_player(prefix, frame):
    payload_len = read_u16be(frame, 4)
    player = frame[TRANSPORT_HEADER_SIZE:TRANSPORT_HEADER_SIZE + PLAYER_BLOCK_SIZE]
    x = struct.unpack(">f", bytes(player[1:5]))[0]
    y = struct.unpack(">f", bytes(player[5:9]))[0]
    z = struct.unpack(">f", bytes(player[9:13]))[0]
    pitch = struct.unpack(">h", bytes(player[13:15]))[0]
    yaw = struct.unpack(">h", bytes(player[15:17]))[0]
    roll = struct.unpack(">h", bytes(player[17:19]))[0]
    level = player[25]
    print(
        f"{prefix}: len={payload_len} x={x:.3f} y={y:.3f} z={z:.3f} "
        f"pitch={pitch} yaw={yaw} roll={roll} level={level}",
        flush=True,
    )


def print_object_scan(prefix, frame, goomba_behavior_ptr):
    obj = frame[OBJECT_OFFSET:OBJECT_OFFSET + OBJECT_BLOCK_SIZE]
    behavior = read_u32be(obj, 12)
    pos_x = struct.unpack(">f", bytes(obj[36:40]))[0]
    pos_y = struct.unpack(">f", bytes(obj[40:44]))[0]
    pos_z = struct.unpack(">f", bytes(obj[44:48]))[0]
    tag = " GOOMBA" if behavior == goomba_behavior_ptr else ""
    print(
        f"{prefix}: sync=0x{read_u32be(obj, 4):08x} frame={read_u32be(obj, 8)} "
        f"behavior=0x{behavior:08x} owner={obj[0]} auth={obj[1]} level={obj[2]} area={obj[72]} "
        f"x={pos_x:.3f} y={pos_y:.3f} z={pos_z:.3f}{tag}",
        flush=True,
    )


def run_loopback(path, poll_interval, pos_x_offset, pos_y_offset, pos_z_offset, anchor,
                 motion, motion_step, motion_amplitude,
                 source_behavior_ptr, relatch, verbose):
    fd, mm = ensure_mailbox(path)
    state = LoopbackState(pos_x_offset, pos_y_offset, pos_z_offset, anchor,
                          motion, motion_step, motion_amplitude,
                          source_behavior_ptr, relatch)
    try:
        last_tx_seq = struct.unpack_from("<I", mm, 8)[0]
        while True:
            magic, version, tx_seq, rx_seq, tx_len, rx_len = struct.unpack_from(HEADER_FMT, mm, 0)
            if magic != MAGIC or version != VERSION:
                time.sleep(poll_interval)
                continue

            if tx_seq != last_tx_seq and tx_len <= BUFFER_SIZE:
                tx_buf = bytes(mm[HEADER_SIZE:HEADER_SIZE + BUFFER_SIZE])
                rx_buf = state.build_reply(tx_buf)
                mm[HEADER_SIZE + BUFFER_SIZE:HEADER_SIZE + BUFFER_SIZE + BUFFER_SIZE] = rx_buf
                struct.pack_into("<I", mm, 20, BUFFER_SIZE)
                struct.pack_into("<I", mm, 12, rx_seq + 1)
                mm.flush()
                last_tx_seq = tx_seq
                if verbose:
                    print_frame("tx", tx_buf)
                    print_frame("rx", rx_buf)
            time.sleep(poll_interval)
    finally:
        mm.close()
        os.close(fd)


def run_monitor(path, poll_interval):
    fd, mm = ensure_mailbox(path)
    try:
        last_tx_seq = struct.unpack_from("<I", mm, 8)[0]
        last_rx_seq = struct.unpack_from("<I", mm, 12)[0]
        while True:
            magic, version, tx_seq, rx_seq, tx_len, rx_len = struct.unpack_from(HEADER_FMT, mm, 0)
            if magic == MAGIC and version == VERSION:
                if tx_seq != last_tx_seq and tx_len <= BUFFER_SIZE:
                    print_frame("tx", bytes(mm[HEADER_SIZE:HEADER_SIZE + BUFFER_SIZE]))
                    last_tx_seq = tx_seq
                if rx_seq != last_rx_seq and rx_len <= BUFFER_SIZE:
                    print_frame("rx", bytes(mm[HEADER_SIZE + BUFFER_SIZE:HEADER_SIZE + BUFFER_SIZE + BUFFER_SIZE]))
                    last_rx_seq = rx_seq
            time.sleep(poll_interval)
    finally:
        mm.close()
        os.close(fd)


def run_watch_mario(path, poll_interval):
    fd, mm = ensure_mailbox(path)
    try:
        last_tx_seq = struct.unpack_from("<I", mm, 8)[0]
        while True:
            magic, version, tx_seq, rx_seq, tx_len, rx_len = struct.unpack_from(HEADER_FMT, mm, 0)
            if magic == MAGIC and version == VERSION:
                if tx_seq != last_tx_seq and tx_len <= BUFFER_SIZE:
                    print_player("mario", bytes(mm[HEADER_SIZE:HEADER_SIZE + BUFFER_SIZE]))
                    last_tx_seq = tx_seq
            time.sleep(poll_interval)
    finally:
        mm.close()
        os.close(fd)


def run_scan_objects(path, poll_interval, goomba_behavior_ptr):
    fd, mm = ensure_mailbox(path)
    try:
        last_tx_seq = struct.unpack_from("<I", mm, 8)[0]
        while True:
            magic, version, tx_seq, rx_seq, tx_len, rx_len = struct.unpack_from(HEADER_FMT, mm, 0)
            if magic == MAGIC and version == VERSION:
                if tx_seq != last_tx_seq and tx_len <= BUFFER_SIZE:
                    frame = bytes(mm[HEADER_SIZE:HEADER_SIZE + BUFFER_SIZE])
                    print_player("mario", frame)
                    print_object_scan("object", frame, goomba_behavior_ptr)
                    last_tx_seq = tx_seq
            time.sleep(poll_interval)
    finally:
        mm.close()
        os.close(fd)


def run_spawn(path, poll_interval, state, verbose):
    fd, mm = ensure_mailbox(path)
    try:
        last_tx_seq = struct.unpack_from("<I", mm, 8)[0]
        while True:
            magic, version, tx_seq, rx_seq, tx_len, rx_len = struct.unpack_from(HEADER_FMT, mm, 0)
            if magic != MAGIC or version != VERSION:
                time.sleep(poll_interval)
                continue
            if tx_seq != last_tx_seq and tx_len <= BUFFER_SIZE:
                tx_buf = bytes(mm[HEADER_SIZE:HEADER_SIZE + BUFFER_SIZE])
                rx_buf = state.build_reply()
                mm[HEADER_SIZE + BUFFER_SIZE:HEADER_SIZE + BUFFER_SIZE + BUFFER_SIZE] = rx_buf
                struct.pack_into("<I", mm, 20, BUFFER_SIZE)
                struct.pack_into("<I", mm, 12, rx_seq + 1)
                mm.flush()
                last_tx_seq = tx_seq
                if verbose:
                    print_player("mario", tx_buf)
                    print_frame("rx", rx_buf)
            time.sleep(poll_interval)
    finally:
        mm.close()
        os.close(fd)


def main():
    parser = argparse.ArgumentParser(description="Host helper for the ares SM64 USB shared mailbox.")
    parser.add_argument("--path", default=MAILBOX_PATH, help="Mailbox file path.")
    parser.add_argument("--poll-interval", type=float, default=0.005, help="Polling interval in seconds.")
    subparsers = parser.add_subparsers(dest="command", required=True)

    loopback = subparsers.add_parser("loopback-object", help="Clone outbound object packets and send them back mutated.")
    loopback.add_argument("--pos-x-offset", type=float, default=300.0, help="Offset added to object posX.")
    loopback.add_argument("--pos-y-offset", type=float, default=0.0, help="Offset added to object posY.")
    loopback.add_argument("--pos-z-offset", type=float, default=0.0, help="Offset added to object posZ.")
    loopback.add_argument("--anchor", choices=("source", "mario"), default="source",
                          help="Anchor injected motion to the source object or Mario's transmitted position.")
    loopback.add_argument("--motion", choices=("static", "line-x", "line-z", "circle"), default="line-x",
                          help="Motion pattern applied to the injected remote object.")
    loopback.add_argument("--motion-step", type=float, default=4.0,
                          help="Per-frame step for line motion, or radians per frame for circle motion.")
    loopback.add_argument("--motion-amplitude", type=float, default=120.0,
                          help="Radius for circle motion.")
    loopback.add_argument("--source-behavior-ptr", type=lambda s: int(s, 0), default=None,
                          help="Only latch outbound objects with this runtime behavior pointer.")
    loopback.add_argument("--relatch", action="store_true",
                          help="Continuously relatch matching outbound objects instead of freezing the first match.")
    loopback.add_argument("--quiet", action="store_true", help="Suppress tx/rx logging.")

    subparsers.add_parser("monitor", help="Print mailbox tx/rx updates without replying.")
    subparsers.add_parser("watch-mario", help="Print Mario position from outbound frames.")
    scan = subparsers.add_parser("scan-objects", help="Decode the current outbound object block and flag Goombas.")
    scan.add_argument("--goomba-behavior-ptr", type=lambda s: int(s, 0), default=DEFAULT_GOOMBA_BEHAVIOR_PTR,
                      help="Goomba behavior pointer for this ROM build.")
    spawn_object = subparsers.add_parser("spawn-object", help="Send a synthetic remote object with explicit behavior, model, and coordinates.")
    spawn_object.add_argument("--x", type=float, required=True, help="Object X position.")
    spawn_object.add_argument("--y", type=float, required=True, help="Object Y position.")
    spawn_object.add_argument("--z", type=float, required=True, help="Object Z position.")
    spawn_object.add_argument("--level", type=int, required=True, help="Target level number.")
    spawn_object.add_argument("--area", type=int, default=1, help="Target area index.")
    spawn_object.add_argument("--pitch", type=int, default=0, help="Face pitch.")
    spawn_object.add_argument("--yaw", type=int, default=0, help="Face yaw.")
    spawn_object.add_argument("--roll", type=int, default=0, help="Face roll.")
    spawn_object.add_argument("--behavior-ptr", type=lambda s: int(s, 0), required=True,
                              help="Behavior pointer. Segmented or runtime address.")
    spawn_object.add_argument("--model-id", type=lambda s: int(s, 0), required=True,
                              help="Model id for the spawned object.")
    spawn_object.add_argument("--bhv-params", type=lambda s: int(s, 0), default=0,
                              help="Behavior params written into oBhvParams.")
    spawn_object.add_argument("--sync-id", type=lambda s: int(s, 0), default=0x5100,
                              help="Remote sync id to use.")
    spawn_object.add_argument("--quiet", action="store_true", help="Suppress Mario/rx logging.")

    spawn_goomba = subparsers.add_parser("spawn-goomba", help="Send a synthetic remote Goomba object.")
    spawn_goomba.add_argument("--x", type=float, required=True, help="Goomba X position.")
    spawn_goomba.add_argument("--y", type=float, required=True, help="Goomba Y position.")
    spawn_goomba.add_argument("--z", type=float, required=True, help="Goomba Z position.")
    spawn_goomba.add_argument("--level", type=int, required=True, help="Target level number.")
    spawn_goomba.add_argument("--area", type=int, default=1, help="Target area index.")
    spawn_goomba.add_argument("--pitch", type=int, default=0, help="Face pitch.")
    spawn_goomba.add_argument("--yaw", type=int, default=0, help="Face yaw.")
    spawn_goomba.add_argument("--roll", type=int, default=0, help="Face roll.")
    spawn_goomba.add_argument("--behavior-ptr", type=lambda s: int(s, 0), default=DEFAULT_GOOMBA_BEHAVIOR_PTR,
                              help="Behavior pointer. Segmented or runtime address.")
    spawn_goomba.add_argument("--sync-id", type=lambda s: int(s, 0), default=0x5100,
                              help="Remote sync id to use.")
    spawn_goomba.add_argument("--quiet", action="store_true", help="Suppress Mario/rx logging.")

    spawn_castle_sign = subparsers.add_parser("spawn-castle-sign", help="Spawn a wooden signpost at the castle grounds sign location.")
    spawn_castle_sign.add_argument("--sync-id", type=lambda s: int(s, 0), default=0x5200,
                                   help="Remote sync id to use.")
    spawn_castle_sign.add_argument("--behavior-ptr", type=lambda s: int(s, 0), default=0x13002398,
                                   help="Message panel behavior pointer. Segmented or runtime address.")
    spawn_castle_sign.add_argument("--yaw", type=int, default=90, help="Face yaw.")
    spawn_castle_sign.add_argument("--quiet", action="store_true", help="Suppress Mario/rx logging.")

    args = parser.parse_args()

    if args.command == "loopback-object":
        run_loopback(args.path, args.poll_interval, args.pos_x_offset, args.pos_y_offset,
                     args.pos_z_offset, args.anchor,
                     args.motion, args.motion_step, args.motion_amplitude,
                     args.source_behavior_ptr, args.relatch, not args.quiet)
        return 0
    if args.command == "monitor":
        run_monitor(args.path, args.poll_interval)
        return 0
    if args.command == "watch-mario":
        run_watch_mario(args.path, args.poll_interval)
        return 0
    if args.command == "scan-objects":
        run_scan_objects(args.path, args.poll_interval, args.goomba_behavior_ptr)
        return 0
    if args.command == "spawn-object":
        state = SpawnState(
            behavior_ptr=args.behavior_ptr,
            model_id=args.model_id,
            bhv_params=args.bhv_params,
            sync_id=args.sync_id,
            level=args.level & 0xFF,
            area=args.area & 0xFF,
            x=args.x,
            y=args.y,
            z=args.z,
            pitch=args.pitch,
            yaw=args.yaw,
            roll=args.roll,
        )
        run_spawn(args.path, args.poll_interval, state, not args.quiet)
        return 0
    if args.command == "spawn-goomba":
        state = SpawnState(
            behavior_ptr=args.behavior_ptr,
            model_id=MODEL_GOOMBA,
            bhv_params=0,
            sync_id=args.sync_id,
            level=args.level & 0xFF,
            area=args.area & 0xFF,
            x=args.x,
            y=args.y,
            z=args.z,
            pitch=args.pitch,
            yaw=args.yaw,
            roll=args.roll,
        )
        run_spawn(args.path, args.poll_interval, state, not args.quiet)
        return 0
    if args.command == "spawn-castle-sign":
        state = SpawnState(
            behavior_ptr=args.behavior_ptr,
            model_id=MODEL_WOODEN_SIGNPOST,
            bhv_params=0x41000000,
            sync_id=args.sync_id,
            level=CASTLE_GROUNDS_LEVEL,
            area=CASTLE_GROUNDS_AREA,
            x=CASTLE_GROUNDS_SIGN_X,
            y=CASTLE_GROUNDS_SIGN_Y,
            z=CASTLE_GROUNDS_SIGN_Z,
            pitch=0,
            yaw=args.yaw,
            roll=0,
        )
        run_spawn(args.path, args.poll_interval, state, not args.quiet)
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
