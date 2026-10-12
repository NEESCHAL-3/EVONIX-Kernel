#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Actual enqueue/dequeue C with mocked lists and pthread wait-queue semantics."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "tools/testing/selftests/sched"))
from evonix_final_audit_test import function

MOCK = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <pthread.h>
struct list_head { struct list_head *next, *prev; };
static struct list_head free_list={&free_list,&free_list};
static bool list_empty(struct list_head *h) { return h->next==h; }
static void INIT_LIST_HEAD(struct list_head *h) { h->next=h->prev=h; }
static void list_add(struct list_head *item,struct list_head *h)
{ item->next=h->next; item->prev=h; h->next->prev=item; h->next=item; }
static void list_del(struct list_head *item)
{ item->prev->next=item->next; item->next->prev=item->prev; }
#define list_first_entry(h,type,member) ((type *)((char *)(h)->next - offsetof(type,member)))
enum df_reason { DF_NORMAL, DF_UNDER_PRESSURE };
struct deferred_freelist_item {
    struct list_head list;
    size_t nr_pages;
    void (*free)(struct deferred_freelist_item *, enum df_reason);
};
static size_t list_nr_pages;
static pthread_mutex_t free_list_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t wait_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t freelist_waitqueue=PTHREAD_COND_INITIALIZER;
static atomic_ulong wakes, released;
static atomic_bool done;
#define spin_lock_irqsave(lock,flags) do { (flags)=0; pthread_mutex_lock(lock); } while(0)
#define spin_unlock_irqrestore(lock,flags) do { (void)(flags); pthread_mutex_unlock(lock); } while(0)
static void wake_up(pthread_cond_t *queue)
{
    atomic_fetch_add(&wakes,1);
    pthread_mutex_lock(&wait_lock);
    pthread_cond_broadcast(queue);
    pthread_mutex_unlock(&wait_lock);
}
static void release_item(struct deferred_freelist_item *item,enum df_reason reason)
{ (void)reason; atomic_fetch_add(&released,1); free(item); }
"""

MAIN = r"""
static void enqueue(void) {
    struct deferred_freelist_item *item=malloc(sizeof(*item));
    assert(item);
    deferred_free(item,release_item,1);
}
static void *producer(void *arg) {
    (void)arg;
    for(int i=0;i<10000;i++) enqueue();
    return NULL;
}
static void *consumer(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&wait_lock);
        while(!get_freelist_nr_pages() && !atomic_load(&done))
            pthread_cond_wait(&freelist_waitqueue,&wait_lock);
        bool stop=atomic_load(&done) && !get_freelist_nr_pages();
        pthread_mutex_unlock(&wait_lock);
        if(stop) return NULL;
        free_one_item(DF_NORMAL);
    }
}
int main(int argc,char **argv) {
    assert(argc==2);
    int mode=atoi(argv[1]);
    if(mode==0) {
        for(int i=0;i<5000;i++) enqueue();
        assert(atomic_load(&wakes)==1 && get_freelist_nr_pages()==5000);
        while(free_one_item(DF_NORMAL)) {}
        assert(atomic_load(&released)==5000);
    } else if(mode==1) {
        for(int i=0;i<1000;i++) {
            enqueue();
            assert(free_one_item(DF_UNDER_PRESSURE)==1);
        }
        assert(atomic_load(&wakes)==1000 && atomic_load(&released)==1000);
    } else {
        pthread_t worker, producers[4];
        assert(!pthread_create(&worker,NULL,consumer,NULL));
        for(int i=0;i<4;i++) assert(!pthread_create(&producers[i],NULL,producer,NULL));
        for(int i=0;i<4;i++) assert(!pthread_join(producers[i],NULL));
        atomic_store(&done,true);
        wake_up(&freelist_waitqueue);
        assert(!pthread_join(worker,NULL));
        assert(atomic_load(&released)==40000);
        assert(atomic_load(&wakes)>0 && atomic_load(&wakes)<=40001);
    }
    assert(get_freelist_nr_pages()==0 && list_empty(&free_list));
    return 0;
}
"""


class DeferredFreeC(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="evonix-deferred-free-")
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        text = (ROOT / "drivers/dma-buf/heaps/deferred-free-helper.c").read_text()
        c = MOCK
        c += ("static void deferred_free(struct deferred_freelist_item *item,"
              "void (*free)(struct deferred_freelist_item *,enum df_reason),size_t nr_pages) {\n")
        c += function(text,"deferred_free") + "\n}\n"
        c += "static size_t free_one_item(enum df_reason reason) {\n"
        c += function(text,"free_one_item") + "\n}\n"
        c += "static unsigned long get_freelist_nr_pages(void) {\n"
        c += function(text,"get_freelist_nr_pages") + "\n}\n"
        (directory / "queue.c").write_text(c + MAIN)
        cls.exe = directory / "queue"
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-pthread", "-fsanitize=undefined",
                        "-fno-sanitize-recover=all", str(directory / "queue.c"),
                        "-o", str(cls.exe)],check=True)

    def test_pending_burst_needs_one_notification(self):
        subprocess.run([str(self.exe),"0"],check=True,timeout=20)

    def test_each_new_nonempty_transition_notifies(self):
        subprocess.run([str(self.exe),"1"],check=True,timeout=20)

    def test_concurrent_producers_and_consumer_do_not_lose_work(self):
        for _ in range(10):
            subprocess.run([str(self.exe),"2"],check=True,timeout=20)


if __name__ == "__main__":
    unittest.main(verbosity=2)
