#include "csapp.h"
#include "cache.h"

void initializeCache(cache_t *cache) {
    cache->head = Malloc(sizeof(*(cache->head)));
    cache->head->flag = '@';
    cache->head->prev = NULL;
    cache->head->next = NULL;

    cache->tail = Malloc(sizeof(*(cache->tail)));
    cache->tail->flag = '@';
    cache->tail->prev = NULL;
    cache->tail->next = NULL;

    /* construct the doubly linked list */
    cache->head->next = cache->tail;
    cache->tail->prev = cache->head;

    cache->nitems = 0;
    cacheSize = 0;

    /* initialize the read / write lock*/
    readcnt = 0;
    Sem_init(&mutex, 0, READER_SAME_TIME);
    Sem_init(&W, 0, 1);
}

obj_t *readItem(char *targetURI, int clientfd) {
    P(&mutex);
    readcnt++;
    if (readcnt == 1) {
        P(&W);
    }
    V(&mutex);

    /***** reading section starts *****/
    obj_t *cur = cache.head->next;
    rio_t rio;
    Rio_readinitb(&rio, clientfd);
    while (cur->flag != '@') {
        if (strcmp(targetURI, cur->uri) == 0) {
            return cur;
        }

        cur = cur->next;
    }
    /***** reading section ends *****/

    P(&mutex);
    readcnt--;
    if (readcnt == 0) {
        V(&W);
    }
    V(&mutex);

    return NULL;
}

/*
 * This function is guarded by Write Lock, thus is thread safe
 * assume head is the newest part, we evict the last part
 * if possible
 */
void writeToCache(obj_t *obj) {
    P(&W);
    /* step1: check current capacity, if full ,delete one */
    while (obj->respBodyLen + cacheSize > MAX_CACHE_SIZE && cache.head->next != cache.tail) {
        obj_t *last = cache.tail->prev;
        last->next->prev = last->prev;
        last->prev->next = last->next;

        last->next = NULL;
        last->prev = NULL;
        Free(last);
    }

    /* step2: add into the cache */
    //mount the current obj into cache
    obj->next = cache.head->next;
    obj->prev = cache.head;
    cache.head->next->prev = obj;
    cache.head->next = obj;
    cacheSize += obj->respBodyLen;

    V(&W);
}
