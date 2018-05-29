#include "etask_tree.h"
#include "eraft_evts.h"
#include "eraft_confs.h"

typedef enum
{
	HANDSHAKE_FAILURE,
	HANDSHAKE_SUCCESS,
} handshake_state_e;

/** Message types used for peer to peer traffic
 * These values are used to identify message types during deserialization */
typedef enum
{
	/** Handshake is a special non-raft message type
	 * We send a handshake so that we can identify ourselves to our peers */
	MSG_HANDSHAKE,
	/** Successful responses mean we can start the Raft periodic callback */
	MSG_HANDSHAKE_RESPONSE,
	/** Tell leader we want to leave the cluster */
	/* When instance is ctrl-c'd we have to gracefuly disconnect */
	MSG_LEAVE,
	/* Receiving a leave response means we can shutdown */
	MSG_LEAVE_RESPONSE,
	MSG_REQUESTVOTE,
	MSG_REQUESTVOTE_RESPONSE,
	MSG_APPENDENTRIES,
	MSG_APPENDENTRIES_RESPONSE,
} peer_message_type_e;

#include "eraft_network.h"
#if 1

/** Peer protocol handshake
 * Send handshake after connecting so that our peer can identify us */
typedef struct
{
	int     raft_port;
	int     http_port;
	int     node_id;
} msg_handshake_t;

typedef struct
{
	int     success;

	/* leader's Raft port */
	int     leader_port;

	/* the responding node's HTTP port */
	int     http_port;

	/* my Raft node ID.
	 * Sometimes we don't know who we did the handshake with */
	int     node_id;

	char    leader_host[IPV4_HOST_LEN];
} msg_handshake_response_t;

typedef struct
{
	int     type;
	int     node_id;
	char    identity[MAX_GROUP_IDENTITY_LEN];
	union
	{
		msg_handshake_t                 hs;
		msg_handshake_response_t        hsr;
		msg_requestvote_t               rv;
		msg_requestvote_response_t      rvr;
		msg_appendentries_t             ae;
		msg_appendentries_response_t    aer;
	};
	int     padding[100];
} msg_t;
#endif	/* if 0 */

/** Add/remove Raft peer */
typedef struct
{
	int     raft_port;
	int     http_port;
	int     node_id;
	char    host[IPV4_HOST_LEN];
} entry_cfg_change_t;


static void __peer_msg_send(uv_stream_t *s, uv_buf_t buf[], int num)
{
	uint64_t all = 0;
	uv_buf_t tmps[num + 1];
	tmps[0].len = sizeof(uint64_t);
	tmps[0].base = (char *)&all;
	memcpy(&tmps[1], buf, sizeof(uv_buf_t) * num);
	for (int i = 0; i < (num + 1); i++) {
		all += tmps[i].len;
	}
#if 1
	for (int i = 0; i < (num + 1); i++) {
		while (tmps[i].len) {
			int e = uv_try_write(s, &tmps[i], 1);

			if (e < 0) {
				// uv_fatal(e);
			} else {
				tmps[i].base += e;
				tmps[i].len -= e;
			}
		}
	}
#else
	uv_stream_set_blocking(s, 1);

	for (int i = 0; i < (num + 1); i++) {
		size_t nwritten = 0;

		while (nwritten < tmps[i].len) {
			/* The stream is in blocking mode so uv_try_write() should always succeed
			 * with the exact number of bytes that we wanted written.
			 */
			int e = uv_try_write(s, &tmps[i], 1);

			if (e < 0) {
				uv_fatal(e);
			} else {
				assert(e == tmps[i].len);
				nwritten += e;
			}
		}
	}
	uv_stream_set_blocking(s, 0);
#endif		/* if 1 */
}

static int __append_cfg_change(struct eraft_group *group,
	raft_logtype_e change_type,
	char *host,
	int raft_port, int http_port,
	int node_id)
{
	entry_cfg_change_t *change = calloc(1, sizeof(*change));

	change->raft_port = raft_port;
	change->http_port = http_port;
	change->node_id = node_id;
	strcpy(change->host, host);
	change->host[IPV4_HOST_LEN - 1] = 0;

	msg_entry_t entry;
	entry.id = rand();
	entry.data.buf = (void *)change;
	entry.data.len = sizeof(*change);
	entry.type = change_type;

	msg_entry_response_t    r;
	int                     e = raft_recv_entry(group->raft, &entry, &r);

	if (0 != e) {
		return -1;
	}

	return 0;
}

static int __send_handshake_response(struct eraft_group *group,
	eraft_connection_t                              *conn,
	handshake_state_e                               success,
	raft_node_t                                     *leader)
{
	msg_t msg = {};

	msg.type = MSG_HANDSHAKE_RESPONSE;
	msg.node_id = group->node_id;
	snprintf(msg.identity, sizeof(msg.identity), "%s", group->identity);
	msg.hsr.success = success;
	msg.hsr.leader_port = 0;
	msg.hsr.node_id = group->node_id;

	/* allow the peer to redirect to the leader */
	if (leader) {
		eraft_connection_t *leader_conn = raft_node_get_udata(leader);

		if (leader_conn) {
			msg.hsr.leader_port = atoi(leader_conn->port);
			snprintf(msg.hsr.leader_host, IPV4_HOST_LEN, "%s",
				inet_ntoa(leader_conn->addr.sin_addr));
		}
	}

	msg.hsr.http_port = msg.hsr.leader_port + 1000;

	uv_buf_t        bufs[1];
	bufs[0].base = (char *)&msg;
	bufs[0].len = sizeof(msg_t);
	__peer_msg_send(conn->stream, bufs, 1);

	return 0;
}

struct _on_network_info
{
	struct eraft_evts       *evts;
	eraft_connection_t      *conn;
};

/** Parse raft peer traffic using binary protocol, and respond to message */
static int __handle_msg(void *img, size_t sz, struct _on_network_info *info)
{

#ifdef JUST_FOR_TEST
	eraft_connection_t *conn = info->conn;
#endif
	struct eraft_evts *evts = info->evts;

	int e;

	msg_t           m = *(msg_t *)img;

	struct eraft_group      *group = eraft_multi_get_group(&evts->multi, m.identity);
	raft_node_t             *node = raft_get_node(group->raft, m.node_id);
#ifdef JUST_FOR_TEST
#else
	struct eraft_node       *enode = &group->conf->nodes[m.node_id];
	eraft_connection_t      *conn = eraft_network_find_connection(&evts->network, &evts->loop, enode->raft_host, enode->raft_port);
#endif
	switch (m.type)
	{
		case MSG_HANDSHAKE:
		{
			conn->state = CONNECTION_STATE_CONNECTED;
			// conn->http_port = m.hs.http_port;
			// conn->raft_port = m.hs.raft_port;

			/* Is this peer in our configuration already? */
			node = raft_get_node(group->raft, m.hs.node_id);

			if (node) {
				// raft_node_set_udata(node, conn);
			}

			raft_node_t *leader = raft_get_current_leader_node(group->raft);

			if (!leader) {
				return __send_handshake_response(group, conn, HANDSHAKE_FAILURE, NULL);
			} else if (raft_node_get_id(leader) != group->node_id) {
				return __send_handshake_response(group, conn, HANDSHAKE_FAILURE, leader);
			} else if (node) {
				return __send_handshake_response(group, conn, HANDSHAKE_SUCCESS, NULL);
			} else {
				int e = __append_cfg_change(group, RAFT_LOGTYPE_ADD_NONVOTING_NODE,
						inet_ntoa(conn->addr.sin_addr),
						m.hs.raft_port, m.hs.http_port,
						m.hs.node_id);

				if (0 != e) {
					return __send_handshake_response(group, conn, HANDSHAKE_FAILURE, NULL);
				}

				return __send_handshake_response(group, conn, HANDSHAKE_SUCCESS, NULL);
			}
		}
		break;

		case MSG_HANDSHAKE_RESPONSE:

			if (0 == m.hsr.success) {
				// conn->http_port = m.hsr.http_port;

				/* We're being redirected to the leader */
				if (m.hsr.leader_port) {
					printf("Redirecting to %s:%d...\n", m.hsr.leader_host, m.hsr.leader_port);
					char port[IPV4_PORT_LEN] = {};
					snprintf(port, sizeof(port), "%d", m.hsr.leader_port);
					eraft_network_find_connection(&evts->network, &evts->loop, m.hsr.leader_host, port);
				}
			} else {
				printf("Connected to leader: %s:%s\n", inet_ntoa(conn->addr.sin_addr), conn->port);

				// if (!conn->node) {
				//	conn->node = raft_get_node(group->raft, m.hsr.node_id);
				// }
			}

			break;

		case MSG_LEAVE:
		{
			// if (!conn->node) {
			//	printf("ERROR: no node\n");
			//	return 0;
			// }

			int                     id = raft_node_get_id(node);
			struct eraft_node       *enode = &group->conf->nodes[id];
			int                     e = __append_cfg_change(group, RAFT_LOGTYPE_REMOVE_NODE,
					inet_ntoa(conn->addr.sin_addr),
					atoi(enode->raft_port),
					atoi(enode->raft_port) + 1000,
					raft_node_get_id(node));

			if (0 != e) {
				printf("ERROR: Leave request failed\n");
			}
		}
		break;

		case MSG_LEAVE_RESPONSE:
		{
			//__drop_db(group->lmdb);
			printf("Shutdown complete. Quitting...\n");
			exit(0);
		}
		break;

		case MSG_REQUESTVOTE:
		{
			msg_t msg = {};
			msg.type = MSG_REQUESTVOTE_RESPONSE;
			msg.node_id = group->node_id;
			snprintf(msg.identity, sizeof(msg.identity), "%s", group->identity);
			printf("===========node id %d ask me vote ============\n", m.node_id);

			e = raft_recv_requestvote(group->raft, node, &m.rv, &msg.rvr);

			uv_buf_t        bufs[1];
			bufs[0].base = (char *)&msg;
			bufs[0].len = sizeof(msg_t);
			__peer_msg_send(conn->stream, bufs, 1);
		}
		break;

		case MSG_REQUESTVOTE_RESPONSE:
		{
			e = raft_recv_requestvote_response(group->raft, node, &m.rvr);
			printf("===========node id %d for me vote ============\n", m.node_id);
			printf("Leader is %d\n", raft_get_current_leader(group->raft));
		}
		break;

		case MSG_APPENDENTRIES:
			{
				// printf("unpack count ---------------------------------------------------->%d\n", m.ae.n_entries);
				if (0 < m.ae.n_entries) {
					/* handle appendentries payload */
					m.ae.entries = calloc(m.ae.n_entries, sizeof(msg_entry_t));

					char *p = ((char *)img) + sizeof(msg_t);
					for (int i = 0; i < m.ae.n_entries; i++) {
						memcpy(&m.ae.entries[i], p, sizeof(msg_entry_t));
						p += sizeof(msg_entry_t);

						int len = m.ae.entries[i].data.len;
						m.ae.entries[i].data.buf = calloc(1, len);
						memcpy(m.ae.entries[i].data.buf, p, len);
						p += len;
					}
				}
				/* this is a keep alive message */
				msg_t msg = {};
				msg.type = MSG_APPENDENTRIES_RESPONSE;
				msg.node_id = group->node_id;
				snprintf(msg.identity, sizeof(msg.identity), "%s", group->identity);

				e = raft_recv_appendentries(group->raft, node, &m.ae, &msg.aer);

				/* send response */
				uv_buf_t        bufs[1];
				bufs[0].base = (char *)&msg;
				bufs[0].len = sizeof(msg_t);
				__peer_msg_send(conn->stream, bufs, 1);
			}
			break;

		case MSG_APPENDENTRIES_RESPONSE:
			e = raft_recv_appendentries_response(group->raft, node, &m.aer);
			/*FIXME*/
			int     first_idx = m.aer.first_idx;
			int     over_idx = raft_get_commit_idx(group->raft);

			for (int id = first_idx; id <= over_idx; id++) {
				// printf("<---%d\n", id);
				etask_tree_awake_task(evts->wait_idx_tree, &id, sizeof(id));
			}

			break;

		default:
			printf("unknown msg\n");
			exit(0);
	}
	return 0;
}

void _on_transmit_fcb(eraft_connection_t *conn, const uv_buf_t *buf, ssize_t nread, void *usr)
{
	struct eraft_evts *evts = usr;

	struct _on_network_info info = { .evts = evts, .conn = conn };

	bool ok = commcache_import(&conn->cache, buf->base, nread);
	assert(ok);

	do {
		size_t have = commcache_size(&conn->cache);
		if (have <= sizeof(uint64_t)) {
			break;
		}
		uint64_t all = 0;
		ok = commcache_export(&conn->cache, (char *)&all, sizeof(uint64_t));
		assert(ok);
		if (have < all) {
			ok = commcache_resume(&conn->cache, (char *)&all, sizeof(uint64_t));
			assert(ok);
			break;
		}
		uint64_t len = all - sizeof(uint64_t);
		char *msg = calloc(1, len);
		ok = commcache_export(&conn->cache, (char *)msg, len);
		assert(ok);

		__handle_msg(msg, len, &info);
		free(msg);
	} while (1);
}

/** Raft callback for sending request vote message */
static int __raft_send_requestvote(
	raft_server_t           *raft,
	void                    *user_data,
	raft_node_t             *node,
	msg_requestvote_t       *m
	)
{
	struct eraft_group      *group = raft_get_udata(raft);
	int                     id = raft_node_get_id(node);
	struct eraft_node       *enode = &group->conf->nodes[id];
	struct eraft_evts       *evts = group->evts;

	eraft_connection_t *conn = eraft_network_find_connection(&evts->network, &evts->loop, enode->raft_host, enode->raft_port);

	if (!eraft_network_usable_connection(conn)) {
		return 0;
	}

	msg_t           msg = {};
	msg.type = MSG_REQUESTVOTE;
	msg.node_id = group->node_id;
	snprintf(msg.identity, sizeof(msg.identity), "%s", group->identity);
	msg.rv = *m;

	uv_buf_t        bufs[1];
	bufs[0].base = (char *)&msg;
	bufs[0].len = sizeof(msg_t);
	__peer_msg_send(conn->stream, bufs, 1);
	return 0;
}

/** Raft callback for sending appendentries message */
static int __raft_send_appendentries(
	raft_server_t           *raft,
	void                    *user_data,
	raft_node_t             *node,
	msg_appendentries_t     *m
	)
{
	struct eraft_group      *group = raft_get_udata(raft);
	int                     id = raft_node_get_id(node);
	struct eraft_node       *enode = &group->conf->nodes[id];
	struct eraft_evts       *evts = group->evts;

	eraft_connection_t *conn = eraft_network_find_connection(&evts->network, &evts->loop, enode->raft_host, enode->raft_port);

	if (!eraft_network_usable_connection(conn)) {
		return 0;
	}

	msg_t           msg = {};
	msg.type = MSG_APPENDENTRIES;
	msg.node_id = group->node_id;
	snprintf(msg.identity, sizeof(msg.identity), "%s", group->identity);
	msg.ae.term = m->term;
	msg.ae.prev_log_idx = m->prev_log_idx;
	msg.ae.prev_log_term = m->prev_log_term;
	msg.ae.leader_commit = m->leader_commit;
	msg.ae.n_entries = m->n_entries;

	uv_buf_t        bufs[(m->n_entries * 2) + 1];
	bufs[0].base = (char *)&msg;
	bufs[0].len = sizeof(msg_t);

	if (0 < m->n_entries) {
		/* appendentries with payload */
		//	printf("pack count ---------------------------------------------------->%d\n", m->n_entries);
		for (int i = 0; i < m->n_entries; i++) {
			bufs[(i * 2) + 1].base = (char *)&m->entries[i];
			bufs[(i * 2) + 1].len = sizeof(raft_entry_t);
			bufs[(i * 2) + 2].base = (char *)m->entries[i].data.buf;
			bufs[(i * 2) + 2].len = m->entries[i].data.len;
		}

		__peer_msg_send(conn->stream, bufs, (m->n_entries * 2) + 1);
	} else {
		/* keep alive appendentries only */
		__peer_msg_send(conn->stream, bufs, 1);
	}


	return 0;
}

static int __send_leave_response(struct eraft_group *group, eraft_connection_t *conn)
{
	if (!conn) {
		printf("no connection??\n");
		return -1;
	}

	if (!conn->stream) {
		return -1;
	}

	msg_t           msg = {};
	msg.type = MSG_LEAVE_RESPONSE;
	msg.node_id = group->node_id;
	snprintf(msg.identity, sizeof(msg.identity), "%s", group->identity);

	uv_buf_t        bufs[1];
	bufs[0].base = (char *)&msg;
	bufs[0].len = sizeof(msg_t);
	__peer_msg_send(conn->stream, bufs, 1);
	return 0;
}

/** Raft callback for applying an entry to the finite state machine */
static int __raft_applylog(
	raft_server_t   *raft,
	void            *udata,
	raft_entry_t    *ety,
	int             entry_idx
	)
{
	struct eraft_group      *group = raft_get_udata(raft);
	struct eraft_evts       *evts = group->evts;

	/* Check if it's a configuration change */
	if (raft_entry_is_cfg_change(ety)) {
		entry_cfg_change_t *change = ety->data.buf;

		if ((RAFT_LOGTYPE_REMOVE_NODE == ety->type) && raft_is_leader(group->raft)) {
			char port[IPV4_PORT_LEN] = {};
			snprintf(port, sizeof(port), "%d", change->raft_port);
			eraft_connection_t *conn = eraft_network_find_connection(&evts->network, &evts->loop, change->host, port);
			__send_leave_response(group, conn);
		}

		goto commit;
	}

	if (group->applylog_fcb) {
		group->applylog_fcb(group, ety);
	}

commit:
	;
	/* We save the commit idx for performance reasons.
	 * Note that Raft doesn't require this as it can figure it out itself. */
	int commit_idx = raft_get_commit_idx(raft);
	eraft_journal_set_state(&group->journal, "commit_idx", strlen("commit_idx") + 1, (char *)&commit_idx, sizeof(commit_idx));

	return 0;
}

/** Raft callback for saving voted_for field to disk.
 * This only returns when change has been made to disk. */
static int __raft_persist_vote(
	raft_server_t   *raft,
	void            *udata,
	const int       voted_for
	)
{
	struct eraft_group *group = raft_get_udata(raft);

	return eraft_journal_set_state(&group->journal, "voted_for", strlen("voted_for") + 1, (char *)&voted_for, sizeof(voted_for));
}

/** Raft callback for saving term field to disk.
 * This only returns when change has been made to disk. */
static int __raft_persist_term(
	raft_server_t   *raft,
	void            *udata,
	int             term,
	int             vote
	)
{
	struct eraft_group *group = raft_get_udata(raft);

	return eraft_journal_set_state(&group->journal, "term", strlen("term") + 1, (char *)&term, sizeof(term));
}

static int __offer_cfg_change(struct eraft_group        *group,
	raft_server_t                                   *raft,
	const unsigned char                             *data,
	raft_logtype_e                                  change_type)
{
	entry_cfg_change_t      *change = (void *)data;
	struct eraft_evts       *evts = group->evts;

	/* Node is being removed */
	if (RAFT_LOGTYPE_REMOVE_NODE == change_type) {
		raft_remove_node(raft, raft_get_node(group->raft, change->node_id));

		// TODO: if all no use delete the connection
		return 0;
	}

	/* Node is being added */
	char raft_port[IPV4_PORT_LEN];
	snprintf(raft_port, sizeof(raft_port), "%d", change->raft_port);
	eraft_connection_t *conn = eraft_network_find_connection(&evts->network, &evts->loop, change->host, raft_port);

	// conn->http_port = change->http_port;

	int is_self = change->node_id == group->node_id;

	raft_node_t *node = NULL;
	switch (change_type)
	{
		case RAFT_LOGTYPE_ADD_NONVOTING_NODE:
			node = raft_add_non_voting_node(raft, conn, change->node_id, is_self);
			break;

		case RAFT_LOGTYPE_ADD_NODE:
			node = raft_add_node(raft, conn, change->node_id, is_self);
			break;

		default:
			assert(0);
	}

	raft_node_set_udata(node, conn);

	return 0;
}

int __set_append_log(struct eraft_journal *store, raft_entry_t *ety, int ety_idx)
{
#ifdef TEST_NETWORK_ONLY
	return 0;
#endif
	void *txn = eraft_journal_tx_begin(store);

	struct eraft_entry eentry;
	eentry.entry = *ety;
	eentry.aid = 0;
	eentry.iid = ety_idx;

	int num = eraft_journal_set_record(store, txn, ety_idx, &eentry);
	if (0 == num) {
		eraft_journal_tx_abort(store, txn);
		return -1;
	}

	int e = eraft_journal_tx_commit(store, txn);
	assert(e == 0);

	return 0;
}

int __set_append_log_batch(struct eraft_journal *store, raft_batch_t *bat, int start_idx)
{
#ifdef TEST_NETWORK_ONLY
	return 0;
#endif
	void *txn = eraft_journal_tx_begin(store);

	for (int i = 0; i < bat->n_entries; i ++) {
		struct eraft_entry eentry;
		eentry.entry = *bat->entries[i];
		eentry.aid = 0;
		eentry.iid = start_idx + i;

		int num = eraft_journal_set_record(store, txn, start_idx + i, &eentry);
		if (0 == num) {
			eraft_journal_tx_abort(store, txn);
			return -1;
		}
	}

	int e = eraft_journal_tx_commit(store, txn);
	assert(e == 0);

	return 0;
}

int __get_append_log(struct eraft_journal *store, raft_entry_t *ety, int ety_idx)
{
#ifdef TEST_NETWORK_ONLY
	return 0;
#endif
	void *txn = eraft_journal_tx_begin(store);

	struct eraft_entry eentry;

	int num = eraft_journal_get_record(store, txn, ety_idx, &eentry);
	if (num) {
		*ety = eentry.entry;
	}

	int e = eraft_journal_tx_commit(store, txn);
	assert(e == 0);

	return 0;
}

/** Raft callback for appending an item to the log */
static int __raft_logentry_offer(
	raft_server_t   *raft,
	void            *udata,
	raft_entry_t    *ety,
	int             ety_idx
	)
{
	struct eraft_group *group = raft_get_udata(raft);

	if (raft_entry_is_cfg_change(ety)) {
		__offer_cfg_change(group, raft, ety->data.buf, ety->type);
	}

	//printf("-----%d\n", ety_idx);
	int e = __set_append_log(&group->journal, ety, ety_idx);
	assert (0 == e);

#if 0
	/* So that our entry points to a valid buffer, get the mmap'd buffer.
	 * This is because the currently pointed to buffer is temporary. */
	int e = __get_append_log(&group->journal, ety, ety_idx)
	assert(0 == e);
#endif

	return 0;
}

static int __raft_logentry_offer_batch(
	raft_server_t   *raft,
	void            *udata,
	raft_batch_t    *bat,
	int             start_idx
	)
{
	struct eraft_group *group = raft_get_udata(raft);

	//if (raft_entry_is_cfg_change(ety)) {
	//	__offer_cfg_change(group, raft, ety->data.buf, ety->type);
	//}

	//printf("-----%d\n", start_idx);
	int e = __set_append_log_batch(&group->journal, bat, start_idx);
	assert (0 == e);

#if 0
	/* So that our entry points to a valid buffer, get the mmap'd buffer.
	 * This is because the currently pointed to buffer is temporary. */
	int e = __get_append_log(&group->journal, ety, ety_idx)
	assert(0 == e);
#endif

	return 0;
}

/** Raft callback for removing the first entry from the log
 * @note this is provided to support log compaction in the future */
static int __raft_logentry_poll(
	raft_server_t   *raft,
	void            *udata,
	raft_entry_t    *entry,
	int             ety_idx
	)
{
	//struct eraft_group *group = raft_get_udata(raft);

	//__pop_oldest_log(group->lmdb);

	return 0;
}

/** Raft callback for deleting the most recent entry from the log.
 * This happens when an invalid leader finds a valid leader and has to delete
 * superseded log entries. */
static int __raft_logentry_pop(
	raft_server_t   *raft,
	void            *udata,
	raft_entry_t    *entry,
	int             ety_idx
	)
{
	//struct eraft_group *group = raft_get_udata(raft);

	//__pop_newest_log(group->lmdb);

	return 0;
}

/** Non-voting node now has enough logs to be able to vote.
 * Append a finalization cfg log entry. */
static int __raft_node_has_sufficient_logs(
	raft_server_t   *raft,
	void            *user_data,
	raft_node_t     *node)
{
	struct eraft_group      *group = raft_get_udata(raft);
	int                     id = raft_node_get_id(node);
	struct eraft_node       *enode = &group->conf->nodes[id];

	// struct eraft_evts *evts = (struct eraft_evts *)group->evts;
	// eraft_connection_t *conn = raft_node_get_udata(node);
	// inet_ntoa(conn->addr.sin_addr)

	__append_cfg_change(group, RAFT_LOGTYPE_ADD_NODE,
		enode->raft_host,
		atoi(enode->raft_port),
		atoi(enode->raft_port) + 1000,
		id);
	return 0;
}

/** Raft callback for displaying debugging information */
void __raft_log(raft_server_t *raft, raft_node_t *node, void *udata,
	const char *buf)
{
	if (0) {
		printf("raft: %s\n", buf);
	}
}

raft_cbs_t g_default_raft_funcs = {
	.send_requestvote               = __raft_send_requestvote,
	.send_appendentries             = __raft_send_appendentries,
	.applylog                       = __raft_applylog,
	.persist_vote                   = __raft_persist_vote,
	.persist_term                   = __raft_persist_term,
	.log_offer                      = __raft_logentry_offer,
	.log_offer_batch                = __raft_logentry_offer_batch,
	.log_poll                       = __raft_logentry_poll,
	.log_pop                        = __raft_logentry_pop,
	.node_has_sufficient_logs       = __raft_node_has_sufficient_logs,
	.log                            = __raft_log,
};

/*****************************************************************************/
static void __send_handshake(struct eraft_evts *evts, struct eraft_group *group, eraft_connection_t *conn)
{
	struct eraft_node *enode = eraft_group_get_self_node(group);

	msg_t msg = {};

	msg.type = MSG_HANDSHAKE;
	msg.node_id = group->node_id;
	snprintf(msg.identity, sizeof(msg.identity), "%s", group->identity);
	msg.hs.raft_port = atoi(enode->raft_port);
	msg.hs.http_port = atoi(enode->raft_port) + 1000;
	msg.hs.node_id = group->node_id;

	uv_buf_t        bufs[1];
	bufs[0].base = (char *)&msg;
	bufs[0].len = sizeof(msg_t);
	__peer_msg_send(conn->stream, bufs, 1);
}

static bool __connected_for_lookup_fcb(struct eraft_group *group, size_t idx, void *usr)
{
	struct _on_network_info *info = usr;

	__send_handshake(info->evts, group, info->conn);
	return true;
}

void _on_connected_fcb(eraft_connection_t *conn, void *usr)
{
	struct eraft_evts *evts = usr;

	struct _on_network_info info = { .evts = evts, .conn = conn };

	eraft_multi_foreach_group(&evts->multi, __connected_for_lookup_fcb, NULL, &info);
}

/*****************************************************************************/
bool __periodic_for_lookup_fcb(struct eraft_group *group, size_t idx, void *usr)
{
	// struct eraft_evts *evts = usr;

	if (0 || (raft_get_current_leader(group->raft) == -1)) {
		raft_periodic(group->raft, PERIOD_MSEC);
	}

	raft_apply_all(group->raft);
	return true;
}

/** Raft callback for handling periodic logic */
static void _periodic(uv_timer_t *handle)
{
	struct eraft_evts *evts = handle->data;

	for(int i = 0; i < 0; ++i) {
		//TODO: if not connect, reconnect.
	}
	eraft_multi_foreach_group(&evts->multi, __periodic_for_lookup_fcb, NULL, evts);
}

static void _start_raft_periodic_timer(struct eraft_evts *evts)
{
	uv_timer_t *periodic_timer = &evts->periodic_timer;

	periodic_timer->data = evts;
	uv_timer_init(&evts->loop, periodic_timer);
	uv_timer_start(periodic_timer, _periodic, 0, PERIOD_MSEC);
}

static void _stop_raft_periodic_timer(struct eraft_evts *evts)
{
	uv_timer_stop(&evts->periodic_timer);
}

/*****************************************************************************/
static void _eraft_tasker_work(struct eraft_tasker *tasker, struct eraft_task *task, void *usr);

struct eraft_evts *eraft_evts_make(struct eraft_evts *evts, int self_port)
{
	if (evts) {
		bzero(evts, sizeof(*evts));
		evts->canfree = false;
	} else {
		New(evts);
		assert(evts);
		evts->canfree = true;
	}

	/* eventfd by callback register in rbtree. */
	evts->wait_idx_tree = etask_tree_make();

	/*初始化事件loop*/
	memset(&evts->loop, 0, sizeof(uv_loop_t));
	int e = uv_loop_init(&evts->loop);

	if (0 != e) {
		uv_fatal(e);
	}

	/*开启周期定时器*/
	_start_raft_periodic_timer(evts);

	/*绑定端口,开启raft服务*/
	e = eraft_network_init(&evts->network, &evts->loop, self_port, _on_connected_fcb, NULL, NULL, _on_transmit_fcb, evts);

	if (0 != e) {
		uv_fatal(e);
	}

	eraft_tasker_init(&evts->tasker, &evts->loop, _eraft_tasker_work, evts);

	eraft_multi_init(&evts->multi);

	evts->init = true;

	return evts;
}

void eraft_evts_free(struct eraft_evts *evts)
{
	if (evts && evts->init) {
		etask_tree_free(evts->wait_idx_tree);

		_stop_raft_periodic_timer(evts);

		eraft_network_free(&evts->network);

		eraft_tasker_free(&evts->tasker);

		eraft_multi_free(&evts->multi);

		evts->init = false;

		if (evts->canfree) {
			Free(evts);
		}
	}
}

void eraft_evts_once(struct eraft_evts *evts)
{
	uv_run(&evts->loop, UV_RUN_ONCE);
}

/*****************************************************************************/
#if 0
static void __send_leave(eraft_connection_t *conn)
{
	msg_t           msg = {};
	msg.type = MSG_LEAVE;

	uv_buf_t        bufs[1];
	bufs[0].base = (char *)&msg;
	bufs[0].len = sizeof(msg_t);
	__peer_msg_send(conn->stream, bufs, 1);
}

#endif

static void _eraft_tasker_work(struct eraft_tasker *tasker, struct eraft_task *task, void *usr)
{
	struct eraft_evts *evts = usr;

	switch (task->type)
	{
		case ERAFT_TASK_GROUP_ADD:
		{
			struct eraft_task_add_group     *object = (struct eraft_task_add_group *)task;
			struct eraft_group              *group = object->group;
			eraft_multi_add_group(&evts->multi, group);

			/* Rejoin cluster */
			// if (1 == raft_get_num_nodes(group->raft)) {
			if (1 == group->conf->num_nodes) {
				raft_become_leader(group->raft);
			} else {
				/*连接其它节点*/
				for (int i = 0; i < raft_get_num_nodes(group->raft); i++) {
					raft_node_t *node = raft_get_node_from_idx(group->raft, i);

					if (raft_node_get_id(node) == group->node_id) {
						continue;
					}

					/*创建连接*/
					assert(i < group->conf->num_nodes);
					struct eraft_node *enode = &group->conf->nodes[i];

					eraft_connection_t *conn = eraft_network_find_connection(&evts->network, &evts->loop,
							enode->raft_host, enode->raft_port);
					raft_node_set_udata(node, conn);
				}
			}

			eraft_task_add_group_free(object);
		}
		break;

		case ERAFT_TASK_GROUP_DEL:
		{
			struct eraft_task_del_group *object = (struct eraft_task_del_group *)task;
			// TODO:eraft_multi_xxx_group(&evts->multi, object->identity);

			eraft_task_del_group_free(object);
		}
		break;

#if 0
		case ERAFT_TASK_RELATIONSHIP_DEL:
			raft_node_t *leader = raft_get_current_leader_node(group->raft);

			if (leader) {
				if (raft_node_get_id(leader) == group->node_id) {
					printf("I'm the leader, leave the cluster...\n");
				} else {
					eraft_connection_t *leader_conn = raft_node_get_udata(leader);

					if (leader_conn) {
						printf("Leaving cluster...\n");
						__send_leave(leader_conn);
					}
				}
			} else {
				printf("Try again no leader at the moment...\n");
			}
			break;
#endif
		case ERAFT_TASK_ENTRY_SEND:
		{
			struct eraft_task_send_entry    *object = (struct eraft_task_send_entry *)task;
			struct eraft_group              *group = eraft_multi_get_group(&evts->multi, object->identity);

			int idx = raft_get_current_idx(group->raft) + 1;
			// printf("--->%d\n", idx);

			int e = raft_recv_entry(group->raft, object->entry, object->entry_response);

			if (0 != e) {
				abort();
			}

			idx = object->entry_response->idx;
			// printf("--->%d\n", idx);
			object->efd = etask_tree_make_task(evts->wait_idx_tree, &idx, sizeof(idx));
			object->idx = idx;

			etask_awake(&object->etask);
		}
		break;

		default:
			abort();
	}
}

/*****************************************************************************/
void eraft_task_dispose_del_group(struct eraft_evts *evts, char *identity)
{
	struct eraft_task_del_group *task = eraft_task_del_group_make(identity);

	eraft_tasker_give(&evts->tasker, (struct eraft_task *)task);
}

void eraft_task_dispose_add_group(struct eraft_evts *evts, struct eraft_group *group)
{
	group->evts = evts;	// FIXME
	struct eraft_task_add_group *task = eraft_task_add_group_make(group);

	eraft_tasker_give(&evts->tasker, (struct eraft_task *)task);
}

void eraft_task_dispose_send_entry(struct eraft_evts *evts, char *identity, msg_entry_t *entry)
{
	msg_entry_response_t            r;
	struct eraft_task_send_entry    *task = eraft_task_send_entry_make(identity, entry, &r);

	eraft_tasker_give(&evts->tasker, (struct eraft_task *)task);

	etask_sleep(&task->etask);

	// struct eraft_group *group = eraft_multi_get_group(&evts->multi, identity);

	/* When we receive an entry from the client we need to block until the
	 * entry has been committed. This efd is used to wake us up. */
	int ret = etask_tree_await_task(evts->wait_idx_tree, &task->idx, sizeof(task->idx), task->efd, -1);
	assert(ret == 0);
#if 0
	e = raft_msg_entry_response_committed(group->raft, &r);

	if (1 != e) {
		/* not committed yet */
		printf("ERROR: failed to commit entry\n");
		abort();
	}
#endif

	eraft_task_send_entry_free(task);
}

