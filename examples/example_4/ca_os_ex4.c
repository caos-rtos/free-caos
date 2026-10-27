// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
/*
 * Example 4 — Producer/consumer via static queue
 *
 * Two producer tasks (producer_1, producer_2) send Message structs into a
 * shared statically-allocated queue (CAQUEUE_STATIC_DEFINE, 8 slots) with a
 * bounded send timeout; consumer task(s) further down receive and process
 * them. Shows inter-task communication with ca_queue_static_send/receive
 * and static (no-heap) queue allocation.
 */

#include "caos.h"

/* Message exchanged between producers and consumers */
typedef struct {
    u32 producer_id;
    u32 sequence;
    u32 value;
} Message;

/* Static queue: 8 slots of Message (12 bytes each) */
#define MSG_SIZE  12
CAQUEUE_STATIC_DEFINE(msg_queue, 8, MSG_SIZE);

static u32 running;


static u32 producer_1(void *arg) {
    (void)arg;
    u32 seq = 0;
    ca_stdout_printf_n("Producer 1 started");
    while(running) {
        Message msg = { .producer_id = 1, .sequence = seq, .value = seq * 10 };
        if (ca_queue_static_send(&msg_queue.q, &msg, 500)) {
            ca_stdout_printf_n("P1 sent: seq=%u val=%u", msg.sequence, msg.value);
            seq++;
        } else {
            ca_stdout_printf_n("P1: queue full (timeout)");
        }
        ca_os_sleep(300);
    }
    return 0;
}


static u32 producer_2(void *arg) {
    (void)arg;
    u32 seq = 0;
    ca_stdout_printf_n("Producer 2 started");
    while(running) {
        Message msg = { .producer_id = 2, .sequence = seq, .value = seq * 100 };
        if (ca_queue_static_send(&msg_queue.q, &msg, 500)) {
            ca_stdout_printf_n("P2 sent: seq=%u val=%u", msg.sequence, msg.value);
            seq++;
        } else {
            ca_stdout_printf_n("P2: queue full (timeout)");
        }
        ca_os_sleep(500);
    }
    return 0;
}


static u32 consumer_1(void *arg) {
    (void)arg;
    Message msg;
    ca_stdout_printf_n("Consumer 1 started");
    while(running) {
        if (ca_queue_static_receive(&msg_queue.q, &msg, 1000)) {
            ca_stdout_printf_n("C1 recv: from P%u seq=%u val=%u",
                               msg.producer_id, msg.sequence, msg.value);
        } else {
            ca_stdout_printf_n("C1: queue empty (timeout)");
        }
    }
    return 0;
}


static u32 consumer_2(void *arg) {
    (void)arg;
    Message msg;
    ca_stdout_printf_n("Consumer 2 started");
    while(running) {
        if (ca_queue_static_receive(&msg_queue.q, &msg, 1000)) {
            ca_stdout_printf_n("C2 recv: from P%u seq=%u val=%u",
                               msg.producer_id, msg.sequence, msg.value);
        } else {
            ca_stdout_printf_n("C2: queue empty (timeout)");
        }
    }
    return 0;
}


#if ENABLE_CLI
/* 'overwrite': the mailbox write on one-item queues, static and heap.
 * Empty -> written; full -> replaced, still one item; read -> the newest;
 * a queue longer than one item -> refused, untouched. */
CAQUEUE_STATIC_DEFINE(mbox_queue, 1, sizeof(u32));

static void overwrite_check(void) {
    u32 v, got_s = 0U, got_h = 0U;
    v = 0x11U; const bool s1 = ca_queue_static_overwrite(&mbox_queue.q, &v);
    v = 0x22U; const bool s2 = ca_queue_static_overwrite(&mbox_queue.q, &v);
    const u32  cnt_s = ca_queue_static_count(&mbox_queue.q);
    const bool rx_s  = ca_queue_static_tryreceive(&mbox_queue.q, &got_s);

    ca_queue_t hq;
    bool h1 = false, h2 = false, rx_h = false;
    u32 cnt_h = 0U;
    if (ca_queue_heap_init(&hq, sizeof(u32), 1U)) {
        v = 0x33U; h1 = ca_queue_heap_overwrite(&hq, &v);
        v = 0x44U; h2 = ca_queue_heap_overwrite(&hq, &v);
        cnt_h = ca_queue_heap_count(&hq);
        rx_h  = ca_queue_heap_tryreceive(&hq, &got_h);
        ca_queue_heap_destroy(&hq);
    }
    /* msg_queue has 8 slots: refused, the producers' traffic untouched */
    const Message dummy = { 0U, 0U, 0U };
    const bool longq = ca_queue_static_overwrite(&msg_queue.q, &dummy);

    const bool ok = s1 && s2 && cnt_s == 1U && rx_s && got_s == 0x22U &&
                    h1 && h2 && cnt_h == 1U && rx_h && got_h == 0x44U && !longq;
    CLI_PRINTF_N("OW queue  : static=%u%u n=%u got=0x%X heap=%u%u n=%u got=0x%X long=%u  %s",
                 (u32)s1, (u32)s2, cnt_s, got_s, (u32)h1, (u32)h2, cnt_h, got_h,
                 (u32)longq, ok ? "PASS" : "FAIL");
}

static bool ex4_cli_handler(c_str *pcmd) {
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("overwrite : one-item queue mailbox write check (static + heap)");
        return false;
    }
    if (ca_strncmp(p, "overwrite", 9) == 0) {
        overwrite_check();
        return true;
    }
    return false;
}
static ca_cli_node_t s_ex4_cli_node = { ex4_cli_handler, NULL };
#endif /* ENABLE_CLI */

/* init hardware device enabled */
void ca_os_configure(){
    /* UART DBG or ITM ALREADY SET ON SYSTEM IF ENABLED! */
    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    EVAL_CLI(
        ca_cli_init(4096);
        ca_cli_set_sender(STDOUT);
        ca_cli_create_thread();
    )
    /* power management: see example_59 (ex59 is the example dedicated to the pwr driver) */
    /* DBG */
    EVAL_USB_CDC_STD(ca_usb_dev_mng_cdc_cli();)
}

void ca_os_main(void) {
    running = 1;

    ca_os_job("prod_1", CA_TH_LEVEL3, producer_1, NULL,
                        STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_job("prod_2", CA_TH_LEVEL3, producer_2, NULL,
                        STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_job("cons_1", CA_TH_LEVEL3, consumer_1, NULL,
                        STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_job("cons_2", CA_TH_LEVEL3, consumer_2, NULL,
                        STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);

    EVAL_CLI( ca_cli_register(&s_ex4_cli_node); )
    ca_os_run(true);
}
