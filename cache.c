/*
 * cache.c a web cache used by web proxy to cache web objects as needed
 *
 * Zhexin Qiu zhexinq 
 *
 * maximum cache size: 100 MiB
 * maximum object size: 100 KiB
 *
 * cache is maintained in a double-linked list of cache nodes
 * a queue
 * 
 * working cache for writing:
 * free_bytes, free cache size
 * ei_w, mutex for eviction and insertion on working cache
 * u_w, mutex for updating position
 * head, beginning of the cache
 * tail, end of the cache
 *
 * parallel list for reading:
 * read_cnt, count current number of readers
 * r_p, mutex for read_cnt
 * ei_p, mutex for eviction and insertion on parallel list
 * head, beginning of the list
 * tail, end of the list
 *
 * cache node structure:
 * url, id
 * buf, holds the actual web object
 * content_length, web object size
 * pre/succ, pointers to previous/next cache node
 *
 * cache out operation:
 * lock reader mutex,
 * lock working cache eviction and insertion if first reader
 * unlock reader mutex,
 * read on parallel list, 
 * use match (if found) to answer client request,
 * lock udpating mutex,
 * update match position using working cache,
 * unlock updating mutex,
 * lock reader mutex,
 * unlock working cache eviction and insertion if last reader
 * unlock reader mutex.
 * 
 * cache in operation:
 * 
 */

#include "csapp.h"
#include "cache.h"

#define CACHE_HITS 32

/* global variables */
cache_list cache;
cache_bag cache_hits;
int read_cnt;

/* mutexes */
sem_t w; /* modify list lock */
sem_t u; /* access to hits bag */
sem_t read_mutex; /* reader counter lock */


/*
 * initialize cache hits 
 */
void init_cache_hits() {
	cache_hits.bag = malloc(CACHE_HITS * sizeof(cache_node *));
	cache_hits.bag_index = 0;
	cache_hits.bag_size = CACHE_HITS;
}

/*
 * free cache hits
 */
void free_cache_hits() {
	free(cache_hits.bag);
}

/*
 * check if a cache_hits is empty
 */
int is_cache_hits_empty() {
	return !cache_hits.bag_index;
}

/*
 * put a cache_node pointer in cache_hits
 */
void cache_hits_put(cache_node *n) {
	cache_hits.bag[cache_hits.bag_index] = n;
	cache_hits.bag_index++;
	if (cache_hits.bag_index >= cache_hits.bag_size)
		resize_cache_hits();
}

/*
 * pop a cache_node pointer in cache_hits
 */
cache_node *cache_hits_get() {
	if (!is_cache_hits_empty()) {
		int i = --cache_hits.bag_index;
		return cache_hits.bag[i];
	}
	return NULL;
}

/*
 * double the size of cache_hits
 */
void resize_cache_hits() {
	cache_hits.bag = realloc(cache_hits.bag, 
		                     2*cache_hits.bag_size*sizeof(cache_node *));
	cache_hits.bag_size = 2*cache_hits.bag_size;
}

/*
 * initialize cache list
 */
void init_cache_list() {
	cache.free_bytes = MAX_CACHE_SIZE;
	cache.head = NULL;
	cache.tail = NULL;
}

/*
 * initialize a working cache and its parallel
 */
void init_cache() {
	/* init global variables */
	init_cache_list();
	init_cache_hits();
	read_cnt = 0;
	/* init semaphores */
	Sem_init(&w);
	Sem_init(&u);
	Sem_init(&read_mutex);
}

/*
 * clear the cache
 */
void deinit_cache() {
	cache_node *p = cache->head;
	cache_node *p_succ;
	while (p) {
		p_succ = p->succ;
		free(p->buf);
		free(p);
		p = p_succ;
	}
}

/*
 * delete a node from the cache list 
 */
void delete_node(cache_node *node) {
	if (node->pre) 
		node->pre->succ = node->succ;
	if (node->succ)
		node->succ->pre = node->pre;
	if (node == cache.tail)
		cache.tail = node->pre;
}

/*
 * insert a node in front of the cache list
 */
void insert_node(cache_node *node) {
	if (cache.head) {
		node->pre = NULL;
		node->succ = cache.head;
		cache.head->pre = node;
		cache.head = node;
	}
	else {
		node.pre = NULL;
		node.succ = NULL;
		cache.head = node;
		cache.tail = node;
	}
}

/*
 * LRU policy: least used node is put in the tail of 
 * working list
 */
void evict_cache(int bytes) {
	cache_node *lru_node;
	lru_node = cache.tail;
	while (cache.free_bytes < bytes) {
		delete_node(lru_node);
		cache.free_bytes += lru_node->content_length;
		cache.tail = lru_node->pre;
		free(lru_node->buf);
		free(lru_node);
	}
}

/*
 * cache in the web object passed by proxy buffer
 * 1. if not enough space, evict least recently used nodes
 * 2. update the cached node's position according to LRU rule
 */
void cache_in(char *url, char *buf, int bytes) {
	cache_node *curr_n;

	P(&w);

	/* update the list ordering for any queued cache hits */
	while((curr_n = cache_hits_get())) {
		if (curr_n != cache.head) {
			delete_node(curr_n);
			insert_node(curr_n);
		}
	}

	/* evict nodes till enough available space */
	evict_cache(bytes);

	/* create a new node to hold the cached in data */
	cache_node *new_node = malloc(sizeof(cache_node *));
	strcpy(new_node->url, url);
	memcpy(new_node->buf, buf, bytes);
	new_node->content_length = bytes;

	/* insert the new node to front of working list */
	insert_node(new_node);
	cache->free_bytes -= bytes;

	V(&w);
}

/*
 * traverse the list to find if a client request has been cached
 * return pointer to the node if cached, other wise return NULL
 */
cache_node *find_cached(char *url) {
	cache_node *curr_node = cache.head;
	for (; curr_node; curr_node = curr_node->succ) {
		if (!strcmp(url, curr_node->url))
			return curr_node;
	}
	return NULL;
}

/*
 * cache out the web object to client request
 * if url matched in cache, write cached content to client,
 * and update hit bag.
 * return 1, if request cached
 * return 0, if not cached
 */
int cache_out(char *url, int client_fd) {
	cache_node cached_node;
	int cached = 0;

	P(&read_mutex);
	read_cnt++;
	if (read_cnt == 1) /* first in */
		P(&w);
	V(&read_mutex);

	/* read the cache list */
	cached_node = find_cached(url);
	if (cached_node) {
		P(&u); 
		cached = 1;
		rio_writen(client_fd, cache_node->buf, cache_node->content_length);
		cache_hits_put(cached_node);
		V(&u);
	}

	P(&read_mutex);
	read_cnt--;
	if (read_cnt == 0) /* last out */
		V(&w);
	V(&read_mutex);

	return cached;
}



