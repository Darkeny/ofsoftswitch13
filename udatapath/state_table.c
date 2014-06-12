#include "state_table.h"
#include "oflib/ofl-structs.h" 
#include "oflib/oxm-match.h"
#include "lib/hash.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>

void __extract_key(uint8_t *, struct key_extractor *, struct packet *);

struct state_table * state_table_create(void) {
	struct state_table *table = malloc(sizeof(struct state_table));
	memset(table, 0, sizeof(*table));
	 
	table->state_entries = (struct hmap) HMAP_INITIALIZER(&table->state_entries);

	/* default state entry */
	table->default_state_entry.state = STATE_DEFAULT;
	
	return table;
}

void state_table_destroy(struct state_table *table) {
	hmap_destroy(&table->state_entries);
	free(table);
}
/* having the key extractor field goes to look for these key inside the packet and map to corresponding value and copy the value into buf. */ 
void __extract_key(uint8_t *buf, struct key_extractor *extractor, struct packet *pkt) {
	int i, l=0;
	struct ofl_match_tlv *f;

	for (i=0; i<extractor->field_count; i++) {		
		uint32_t type = (int)extractor->fields[i];
		HMAP_FOR_EACH_WITH_HASH(f, struct ofl_match_tlv,
        	hmap_node, hash_int(type, 0), &pkt->handle_std->match.match_fields){
				//if (type == OXM_TYPE(f->header)) {
				if (type == f->header) {
					memcpy(&buf[l], f->value, OXM_LENGTH(f->header));
					l = l + OXM_LENGTH(f->header);//keeps only 8 last bits of oxm_header that contains oxm_length(in which length of oxm_payload).
					break;
				}
		}
	}
}
/*having the read_key, look for the state vaule inside the state_table */
struct state_entry * state_table_lookup(struct state_table* table, struct packet *pkt) {
	struct state_entry * e = NULL;	
	uint8_t key[MAX_STATE_KEY_LEN] = {0};

        __extract_key(key, &table->read_key, pkt);

	HMAP_FOR_EACH_WITH_HASH(e, struct state_entry, 
		hmap_node, hash_bytes(key, MAX_STATE_KEY_LEN, 0), &table->state_entries){
			if (!memcmp(key, e->key, MAX_STATE_KEY_LEN)){
				return e;
			}
	}

	if (e == NULL)
		return &table->default_state_entry;
	else 
		return e;
}

static int __timeout_expired(struct timeval *now, struct timeval *to) {
	if (now->tv_sec > to->tv_sec)
		return 1;
	else if (now->tv_sec == to->tv_sec)
		return (now->tv_usec > to->tv_usec);
	else
		return 0;
}


void state_table_write_metadata(struct state_entry *entry, struct packet *pkt) {
	struct  ofl_match_tlv *f;
	uint32_t state;

	if (entry->to_state) {
		struct timeval now;
		gettimeofday(&now, NULL);
		if (__timeout_expired(&now, &entry->timeout))
			state = entry->to_state;
		else	
			state = entry->state;
	}	
	else 
		state = entry->state;

	//in any case reset timeout
	entry->to_state = 0;
	
	HMAP_FOR_EACH_WITH_HASH(f, struct ofl_match_tlv, 
		hmap_node, hash_int(OXM_OF_METADATA,0), &pkt->handle_std->match.match_fields){
                uint64_t *metadata = (uint64_t*) f->value;
                *metadata = (*metadata & 0x0) | state;
    	}
}
void state_table_del_state(struct state_table *table, uint8_t *key, uint32_t len) {
	struct state_entry *e;
	int found = 0;

	HMAP_FOR_EACH_WITH_HASH(e, struct state_entry, 
		hmap_node, hash_bytes(key, MAX_STATE_KEY_LEN, 0), &table->state_entries){
			if (!memcmp(key, e->key, MAX_STATE_KEY_LEN)){
				found = 1;
				break;
			}
	}
	if (found)
		hmap_remove_and_shrink(&table->state_entries, &e->hmap_node);
}

void state_table_set_extractor(struct state_table *table, struct key_extractor *ke, int update) {
	struct key_extractor *dest;
	if (update)
		dest = &table->write_key;
	else
		dest = &table->read_key;

	dest->field_count = ke->field_count;
	memcpy(dest->fields, ke->fields, MAX_EXTRACTION_FIELD_COUNT);
	return;
}

void state_table_set_state(struct state_table *table, struct packet *pkt, uint32_t state, uint8_t *k, uint32_t len, uint32_t to, uint32_t to_state) {
	uint8_t key[MAX_STATE_KEY_LEN] = {0};	
	struct state_entry *e;

	if (pkt)
		__extract_key(key, &table->write_key, pkt);
	else 
		memcpy(key, k, MAX_STATE_KEY_LEN);

	HMAP_FOR_EACH_WITH_HASH(e, struct state_entry, 
		hmap_node, hash_bytes(key, MAX_STATE_KEY_LEN, 0), &table->state_entries){
			if (!memcmp(key, e->key, MAX_STATE_KEY_LEN)){
				if (to_state) {
					struct timeval now;
					gettimeofday(&now, NULL);
					now.tv_sec += to/1000000;
					now.tv_usec += to%1000000; 
					memcpy(&e->timeout, &now, sizeof(struct timeval));
					e->to_state = to_state;
				}
				e->state = state;
				return;
			}
	}

	e = malloc(sizeof(struct state_entry));
	memcpy(e->key, key, MAX_STATE_KEY_LEN);
	e->state = state;
        hmap_insert(&table->state_entries, &e->hmap_node, hash_bytes(key, MAX_STATE_KEY_LEN, 0));
}
