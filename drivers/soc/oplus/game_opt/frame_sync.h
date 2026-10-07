// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Oplus. All rights reserved.
 */

#ifndef __FRAME_SYNC_H__
#define __FRAME_SYNC_H__

// for epoll sync
enum gameopt_frame_sync {
    NOTIFY_FRAME_PRODUCE,
    NOTIFY_FRAME_CONSUME,
    NOTIFY_FRAME_TLPRED,
    NOTIFY_FRAME_STOP,
    NOTIFY_FRAME_MAX_ID,
};

struct gameopt_frame_data {
    int mode;
    int bufferN;
    long timeStamp1;
    long timeStamp2;
    char reserved[128];
};

#define GAMEOPT_EPOLL_MAGIC 0xDF
#define CMD_ID_GAMEOPT_EPOLL_PRODUCE  \
    _IOWR(GAMEOPT_EPOLL_MAGIC, NOTIFY_FRAME_PRODUCE, struct gameopt_frame_data)
#define CMD_ID_GAMEOPT_EPOLL_CONSUME  \
    _IOWR(GAMEOPT_EPOLL_MAGIC, NOTIFY_FRAME_CONSUME, struct gameopt_frame_data)
#define CMD_ID_GAMEOPT_EPOLL_TLPRED  \
    _IOWR(GAMEOPT_EPOLL_MAGIC, NOTIFY_FRAME_TLPRED, struct gameopt_frame_data)

int frame_sync_init(void);

#endif // __FRAME_SYNC_H__
