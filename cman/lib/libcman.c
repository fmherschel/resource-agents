/******************************************************************************
*******************************************************************************
**
**  Copyright (C) 2004-2005 Red Hat, Inc.  All rights reserved.
**
**  This library is free software; you can redistribute it and/or
**  modify it under the terms of the GNU Lesser General Public
**  License as published by the Free Software Foundation; either
**  version 2 of the License, or (at your option) any later version.
**
**  This library is distributed in the hope that it will be useful,
**  but WITHOUT ANY WARRANTY; without even the implied warranty of
**  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
**  Lesser General Public License for more details.
**
**  You should have received a copy of the GNU Lesser General Public
**  License along with this library; if not, write to the Free Software
**  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
**
*******************************************************************************
******************************************************************************/

#include <sys/types.h>
#include <sys/un.h>
#include <inttypes.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <errno.h>
#include "cnxman-socket.h"
#include "libcman.h"

/* List of saved messages */
struct saved_message
{
	struct sock_header *msg;
	struct saved_message *next;
};

struct cman_handle
{
	int magic;
	int fd;
	void *private;
	int want_reply;
	cman_callback_t event_callback;
	cman_datacallback_t data_callback;

	void *reply_buffer;
	int reply_buflen;
	int reply_status;

	struct saved_message *saved_data_msg;
	struct saved_message *saved_event_msg;
	struct saved_message *saved_reply_msg;
};

#define VALIDATE_HANDLE(h) do {if (!(h) || (h)->magic != CMAN_MAGIC) {errno = EINVAL; return -1;}} while (0)

/*
 * Wait for an command/request reply.
 * Data/event messages will be queued.
 *
 */
static int wait_for_reply(struct cman_handle *h, void *msg, int max_len)
{
	int ret;

	h->want_reply = 1;
	h->reply_buffer = msg;
	h->reply_buflen = max_len;

	do
	{
		ret = cman_dispatch(h, CMAN_DISPATCH_BLOCKING | CMAN_DISPATCH_IGNORE_EVENT | CMAN_DISPATCH_IGNORE_DATA);

	} while (h->want_reply == 1 && ret >= 0);

	h->reply_buffer = NULL;
	h->reply_buflen = 0;

	/* Error in local comms */
	if (ret < 0) {
		return -1;
	}
	/* cnxman daemon returns -ve errno values on error */
	if (h->reply_status < 0) {
		errno = -h->reply_status;
		return -1;
	}
	else {
		return h->reply_status;
	}
}


static void copy_node(cman_node_t *unode, struct cl_cluster_node *knode)
{
	unode->cn_nodeid = knode->node_id;
	unode->cn_member = knode->state == NODESTATE_MEMBER?1:0;
	strcpy(unode->cn_name, knode->name);
	unode->cn_incarnation = knode->incarnation;
	unode->cn_jointime = knode->jointime;

	memcpy(&unode->cn_address.cna_address, knode->addr, sizeof(struct sockaddr_storage));
	unode->cn_address.cna_addrlen = sizeof(struct sockaddr_storage);
}

/* Add to a list. saved_message *m is the head of the list in the cman_handle */
static void add_to_waitlist(struct saved_message **m, struct sock_header *msg)
{
	struct saved_message *next = *m;
	struct saved_message *last = *m;
	struct saved_message *this;

	this = malloc(sizeof(struct saved_message));
	if (!this)
		return;

	this->msg = malloc(msg->length);
	if (!this->msg)
	{
		free(this);
		return;
	}

	memcpy(this->msg, msg, msg->length);
	this->next = NULL;

	if (!next)
	{
		*m = this;
		return;
	}

	for (; next; next = next->next)
	{
		last = next;
	}
	last->next = this;
}

static int process_cman_message(struct cman_handle *h, int flags, struct sock_header *msg)
{
	/* Data for us */
	if ((msg->command & CMAN_CMDMASK_CMD) == CMAN_CMD_DATA)
	{
		struct sock_data_header *dmsg = (struct sock_data_header *)msg;
		char *buf = (char *)msg;

		if (flags & CMAN_DISPATCH_IGNORE_DATA)
		{
			add_to_waitlist(&h->saved_data_msg, msg);
		}
		else
		{
			if (h->data_callback)
				h->data_callback(h, h->private,
						 buf+sizeof(*dmsg), msg->length-sizeof(*dmsg),
						 dmsg->port, dmsg->nodeid);
		}
		return 0;
	}

	/* Got a reply to a previous information request */
	if ((msg->command & CMAN_CMDFLAG_REPLY) && h->want_reply)
	{
		char *replybuf = (char *)msg;
		int replylen = msg->length - sizeof(struct sock_reply_header);
		struct sock_reply_header *reply = (struct sock_reply_header *)msg;

		if (flags & CMAN_DISPATCH_IGNORE_REPLY)
		{
			add_to_waitlist(&h->saved_reply_msg, msg);
			return 0;
		}

		replybuf += sizeof(struct sock_reply_header);
		if (replylen <= h->reply_buflen)
		{
			memcpy(h->reply_buffer, replybuf, replylen);
		}
		h->want_reply = 0;
		h->reply_status = reply->status;

		return 1;
	}

	/* OOB event */
	if (msg->command == CMAN_CMD_EVENT)
	{
		struct sock_event_message *emsg = (struct sock_event_message *)msg;

		if (flags & CMAN_DISPATCH_IGNORE_EVENT)
		{
			add_to_waitlist(&h->saved_event_msg, msg);
		}
		else
		{
			if (h->event_callback)
				h->event_callback(h, h->private, emsg->reason, emsg->arg);
		}
	}

	return 0;
}

static int loopy_writev(int fd, struct iovec *iovptr, size_t iovlen)
{
	size_t byte_cnt=0;
	int len;

	while (iovlen > 0)
	{
		len = writev(fd, iovptr, iovlen);
		if (len <= 0)
			return len;

		byte_cnt += len;
		while (len >= iovptr->iov_len)
		{
			len -= iovptr->iov_len;
			iovptr++;
			iovlen--;
		}

		if (iovlen <=0 )
			break;

		iovptr->iov_base += len;
		iovptr->iov_len -= len;
	}
	return byte_cnt;
}


/* Does something similar to the ioctl calls */
static int info_call(struct cman_handle *h, int msgtype, void *inbuf, int inlen, void *outbuf, int outlen)
{
	struct sock_header header;
	size_t len;
	struct iovec iov[2];
	size_t iovlen = 1;

	header.magic = CMAN_MAGIC;
	header.command = msgtype;
	header.flags = 0;
	header.length = sizeof(header) + inlen;

	iov[0].iov_len = sizeof(header);
	iov[0].iov_base = &header;
	if (inbuf)
	{
		iov[1].iov_len = inlen;
		iov[1].iov_base = inbuf;
		iovlen++;
	}

	len = loopy_writev(h->fd, iov, iovlen);
	if (len < 0)
		return len;

	return wait_for_reply(h, outbuf, outlen);
}

static cman_handle_t open_socket(const char *name, int namelen, void *private)
{
	struct cman_handle *h;
	struct sockaddr_un sockaddr;

	h = malloc(sizeof(struct cman_handle));
	if (!h)
		return NULL;

	h->magic = CMAN_MAGIC;
	h->private = private;
	h->event_callback = NULL;
	h->data_callback = NULL;
	h->want_reply = 0;
	h->saved_data_msg = NULL;
	h->saved_event_msg = NULL;
	h->saved_reply_msg = NULL;

	h->fd = socket(PF_UNIX, SOCK_STREAM, 0);
	if (h->fd == -1)
	{
		int saved_errno = errno;
		free(h);
		errno = saved_errno;
		return NULL;
	}

	fcntl(h->fd, F_SETFD, 1); /* Set close-on-exec */
	memset(&sockaddr, 0, sizeof(sockaddr));
	memcpy(sockaddr.sun_path, name, namelen);
	sockaddr.sun_family = AF_UNIX;

	if (connect(h->fd, (struct sockaddr *) &sockaddr, sizeof(sockaddr)) < 0)
	{
		int saved_errno = errno;
		close(h->fd);
		free(h);
		h = NULL;
		errno = saved_errno;
	}

	return (cman_handle_t)h;
}

cman_handle_t cman_admin_init(void *private)
{
	return open_socket(ADMIN_SOCKNAME, sizeof(ADMIN_SOCKNAME), private);
}

cman_handle_t cman_init(void *private)
{
	return open_socket(CLIENT_SOCKNAME, sizeof(CLIENT_SOCKNAME), private);
}

int cman_finish(cman_handle_t handle)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	VALIDATE_HANDLE(h);

	h->magic = 0;
	close(h->fd);
	free(h);

	return 0;
}

int cman_start_notification(cman_handle_t handle, cman_callback_t callback)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	VALIDATE_HANDLE(h);

	if (!callback)
	{
		errno = EINVAL;
		return -1;
	}

	h->event_callback = callback;

	return 0;
}

int cman_stop_notification(cman_handle_t handle)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	VALIDATE_HANDLE(h);

	h->event_callback = NULL;

	return 0;
}


int cman_get_fd(cman_handle_t handle)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	VALIDATE_HANDLE(h);

	return h->fd;
}

int cman_dispatch(cman_handle_t handle, int flags)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	int len;
	int recv_flags = 0;
	char buf[PIPE_BUF];
	struct sock_header *header = (struct sock_header *)buf;
	VALIDATE_HANDLE(h);

	if (!(flags & CMAN_DISPATCH_BLOCKING))
		recv_flags |= MSG_DONTWAIT;

	do
	{
		/* First, drain any waiting queues */
		if (h->saved_reply_msg && !(flags & CMAN_DISPATCH_IGNORE_REPLY))
		{
			struct saved_message *smsg = h->saved_reply_msg;

			process_cman_message(h, flags, smsg->msg);
			h->saved_reply_msg = smsg->next;
			len = smsg->msg->length;
			free(smsg);
			continue;
		}
		if (h->saved_data_msg && !(flags & CMAN_DISPATCH_IGNORE_DATA))
		{
			struct saved_message *smsg = h->saved_data_msg;

			process_cman_message(h, flags, smsg->msg);
			h->saved_data_msg = smsg->next;
			len = smsg->msg->length;
			free(smsg);
			continue;
		}
		if (h->saved_event_msg && !(flags & CMAN_DISPATCH_IGNORE_EVENT))
		{
			struct saved_message *smsg = h->saved_event_msg;

			process_cman_message(h, flags, smsg->msg);
			h->saved_event_msg = smsg->next;
			len = smsg->msg->length;
			free(smsg);
			continue;
		}

		/* Now look for new messages */
		len = recv(h->fd, buf, sizeof(struct sock_header), recv_flags);

		if (len == 0) {
			errno = EHOSTDOWN;
			return -1;
		}

		if (len < 0 &&
		    (errno == EINTR || errno == EAGAIN))
			return 0;

		if (len < 0)
			return -1;

		/* Read the rest */
		if (len != header->length)
		{
			len = read(h->fd, buf+len, header->length-len);
			if (len == 0) {
				errno = EHOSTDOWN;
				return -1;
			}

			if (len < 0 &&
			    (errno == EINTR || errno == EAGAIN))
				return 0;

			if (len < 0)
				return -1;
		}

		if (process_cman_message(h, flags, header))
			break;

	} while ( flags & CMAN_DISPATCH_ALL &&
		  (len < 0 && errno == EAGAIN) );

	return len;
}

/* GET_ALLMEMBERS returns the number of nodes as status */
int cman_get_node_count(cman_handle_t handle)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	VALIDATE_HANDLE(h);

	return info_call(h, CMAN_CMD_GETALLMEMBERS, NULL, 0, NULL, 0);
}

int cman_get_nodes(cman_handle_t handle, int maxnodes, int *retnodes, cman_node_t *nodes)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	struct cl_cluster_node *cman_nodes;
	int status;
	int buflen;
	int count = 0;
	VALIDATE_HANDLE(h);

	if (!retnodes || !nodes || maxnodes < 1)
	{
		errno = EINVAL;
		return -1;
	}

	buflen = sizeof(struct cl_cluster_node) * maxnodes;
	cman_nodes = malloc(buflen);
	if (!cman_nodes)
		return -1;

	status = info_call(h, CMAN_CMD_GETALLMEMBERS, NULL, 0, cman_nodes, buflen);
	if (status < 0)
	{
		int saved_errno = errno;
		free(cman_nodes);
		errno = saved_errno;
		return -1;
	}

	if (cman_nodes[0].size != sizeof(struct cl_cluster_node))
	{
		free(cman_nodes);
		errno = EINVAL;
		return -1;
	}

	if (status > maxnodes)
		status = maxnodes;

	for (count = 0; count < status; count++)
	{
		copy_node(&nodes[count], &cman_nodes[count]);
	}
	free(cman_nodes);
	*retnodes = status;
	return 0;
}

int cman_get_node(cman_handle_t handle, int nodeid, cman_node_t *node)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	struct cl_cluster_node cman_node;
	int status;
	VALIDATE_HANDLE(h);

	if (!node || strlen(node->cn_name) > sizeof(cman_node.name))
	{
		errno = EINVAL;
		return -1;
	}

	cman_node.node_id = nodeid;
	strcpy(cman_node.name, node->cn_name);
	status = info_call(h, CMAN_CMD_GETNODE, &cman_node, sizeof(struct cl_cluster_node),
			   &cman_node, sizeof(struct cl_cluster_node));
	if (status < 0)
		return -1;

	copy_node(node, &cman_node);

	return 0;
}

int cman_get_subsys_count(cman_handle_t handle)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	VALIDATE_HANDLE(h);

	return info_call(h, CMAN_CMD_GET_JOINCOUNT, NULL,0, NULL, 0);
}

int cman_is_active(cman_handle_t handle)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	VALIDATE_HANDLE(h);

	return info_call(h, CMAN_CMD_ISACTIVE, NULL, 0, NULL, 0);
}

int cman_is_listening(cman_handle_t handle, int nodeid, uint8_t port)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	struct cl_listen_request req;
	VALIDATE_HANDLE(h);

	req.port = port;
	req.nodeid = nodeid;
	return info_call(h, CMAN_CMD_ISLISTENING, &req, sizeof(struct cl_listen_request), NULL, 0);
}

int cman_is_quorate(cman_handle_t handle)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	VALIDATE_HANDLE(h);

	return info_call(h, CMAN_CMD_ISQUORATE, NULL, 0, NULL, 0);
}


int cman_get_version(cman_handle_t handle, cman_version_t *version)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	VALIDATE_HANDLE(h);

	if (!version)
	{
		errno = EINVAL;
		return -1;
	}
	return info_call(h, CMAN_CMD_GET_VERSION, NULL, 0, version, sizeof(cman_version_t));
}

int cman_set_version(cman_handle_t handle, cman_version_t *version)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	VALIDATE_HANDLE(h);

	if (!version)
	{
		errno = EINVAL;
		return -1;
	}
	return info_call(h, CMAN_CMD_SET_VERSION, version, sizeof(cman_version_t), NULL, 0);
}

int cman_set_nodename(cman_handle_t handle, char *name)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	VALIDATE_HANDLE(h);

	if (!name)
	{
		errno = EINVAL;
		return -1;
	}
	return info_call(h, CMAN_CMD_SET_NODENAME, name, strlen(name)+1, NULL, 0);
}

int cman_set_nodeid(cman_handle_t handle, int nodeid)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	VALIDATE_HANDLE(h);

	if (!nodeid)
	{
		errno = EINVAL;
		return -1;
	}
	return info_call(h, CMAN_CMD_SET_NODEID, &nodeid, sizeof(nodeid), NULL, 0);
}

int cman_kill_node(cman_handle_t handle, int nodeid)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	VALIDATE_HANDLE(h);

	if (!nodeid)
	{
		errno = EINVAL;
		return -1;
	}
	return info_call(h, CMAN_CMD_KILLNODE, &nodeid, sizeof(nodeid), NULL, 0);
}

int cman_set_votes(cman_handle_t handle, int votes, int nodeid)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	struct cl_set_votes newv;
	VALIDATE_HANDLE(h);

	if (!votes)
	{
		errno = EINVAL;
		return -1;
	}
	newv.nodeid = nodeid;
	newv.newvotes  = votes;
	return info_call(h, CMAN_CMD_SET_VOTES, &newv, sizeof(newv), NULL, 0);
}

int cman_set_expected_votes(cman_handle_t handle, int evotes)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	VALIDATE_HANDLE(h);

	if (!evotes)
	{
		errno = EINVAL;
		return -1;
	}
	return info_call(h, CMAN_CMD_SETEXPECTED_VOTES, &evotes, sizeof(evotes), NULL, 0);
}

int cman_leave_cluster(cman_handle_t handle, int reason)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	VALIDATE_HANDLE(h);

	return info_call(h, CMAN_CMD_LEAVE_CLUSTER, &reason, sizeof(reason), NULL, 0);
}

int cman_join_cluster(cman_handle_t handle, struct cman_join_info *jinfo)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	VALIDATE_HANDLE(h);

	if (!jinfo)
	{
		errno = EINVAL;
		return -1;
	}
	return info_call(h, CMAN_CMD_JOIN_CLUSTER, jinfo, sizeof(*jinfo), NULL, 0);
}

int cman_set_mcast(cman_handle_t handle, char *mcast_addr)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	VALIDATE_HANDLE(h);

	if (!mcast_addr)
	{
		errno = EINVAL;
		return -1;
	}
	return info_call(h, CMAN_CMD_ADD_MCAST, mcast_addr, strlen(mcast_addr)+1, NULL, 0);
}

int cman_set_interface(cman_handle_t handle, char *if_addr)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	VALIDATE_HANDLE(h);

	if (!if_addr)
	{
		errno = EINVAL;
		return -1;
	}
	return info_call(h, CMAN_CMD_ADD_IFADDR, if_addr, strlen(if_addr)+1, NULL, 0);
}


int cman_get_cluster(cman_handle_t handle, cman_cluster_t *clinfo)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	VALIDATE_HANDLE(h);

	if (!clinfo)
	{
		errno = EINVAL;
		return -1;
	}
	return info_call(h, CMAN_CMD_GETCLUSTER, NULL, 0, clinfo, sizeof(cman_cluster_t));
}

int cman_get_extra_info(cman_handle_t handle, cman_extra_info_t *info, int maxlen)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	VALIDATE_HANDLE(h);

	if (!info || maxlen < sizeof(cman_extra_info_t))
	{
		errno = EINVAL;
		return -1;
	}
	return info_call(h, CMAN_CMD_GETEXTRAINFO, NULL, 0, info, maxlen);
}

int cman_send_data(cman_handle_t handle, void *buf, int len, int flags, uint8_t port, int nodeid)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	struct iovec iov[2];
	struct sock_data_header header;
	VALIDATE_HANDLE(h);

	header.header.magic = CMAN_MAGIC;
	header.header.command = CMAN_CMD_DATA;
	header.header.flags = flags;
	header.header.length = len + sizeof(header);
	header.nodeid = nodeid;
	header.port = port;

	iov[0].iov_len = sizeof(header);
	iov[0].iov_base = &header;
	iov[1].iov_len = len;
	iov[1].iov_base = buf;

	return loopy_writev(h->fd, iov, 2);
}


int cman_start_recv_data(cman_handle_t handle, cman_datacallback_t callback, uint8_t port)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	int portparam;
	int status;
	VALIDATE_HANDLE(h);

/* Do a "bind" */
	portparam = port;
	status = info_call(h, CMAN_CMD_BIND, &portparam, sizeof(portparam), NULL, 0);

	if (status == 0)
		h->data_callback = callback;

	return status;
}

int cman_end_recv_data(cman_handle_t handle)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	VALIDATE_HANDLE(h);

	h->data_callback = NULL;
	return 0;
}


int cman_barrier_register(cman_handle_t handle, char *name, int flags, int nodes)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	struct cl_barrier_info binfo;
	VALIDATE_HANDLE(h);

	if (strlen(name) > MAX_BARRIER_NAME_LEN)
	{
		errno = EINVAL;
		return -1;
	}

	binfo.cmd = BARRIER_CMD_REGISTER;
	strcpy(binfo.name, name);
	binfo.arg = nodes;
	binfo.flags = flags;

	return info_call(h, CMAN_CMD_BARRIER, &binfo, sizeof(binfo), NULL, 0);
}


int cman_barrier_change(cman_handle_t handle, char *name, int flags, int arg)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	struct cl_barrier_info binfo;
	VALIDATE_HANDLE(h);

	if (strlen(name) > MAX_BARRIER_NAME_LEN)
	{
		errno = EINVAL;
		return -1;
	}

	binfo.cmd = BARRIER_CMD_CHANGE;
	strcpy(binfo.name, name);
	binfo.arg = arg;
	binfo.flags = flags;

	return info_call(h, CMAN_CMD_BARRIER, &binfo, sizeof(binfo), NULL, 0);

}

int cman_barrier_wait(cman_handle_t handle, char *name)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	struct cl_barrier_info binfo;
	VALIDATE_HANDLE(h);

	if (strlen(name) > MAX_BARRIER_NAME_LEN)
	{
		errno = EINVAL;
		return -1;
	}

	binfo.cmd = BARRIER_CMD_WAIT;
	strcpy(binfo.name, name);

	return info_call(h, CMAN_CMD_BARRIER, &binfo, sizeof(binfo), NULL, 0);
}

int cman_barrier_delete(cman_handle_t handle, char *name)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	struct cl_barrier_info binfo;
	VALIDATE_HANDLE(h);

	if (strlen(name) > MAX_BARRIER_NAME_LEN)
	{
		errno = EINVAL;
		return -1;
	}

	binfo.cmd = BARRIER_CMD_DELETE;
	strcpy(binfo.name, name);

	return info_call(h, CMAN_CMD_BARRIER, &binfo, sizeof(binfo), NULL, 0);
}

int cman_register_quorum_device(cman_handle_t handle, char *name, int votes)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	char buf[strlen(name)+1 + sizeof(int)];
	VALIDATE_HANDLE(h);

	if (strlen(name) > MAX_CLUSTER_NAME_LEN)
	{
		errno = EINVAL;
		return -1;
	}

	memcpy(buf, &votes, sizeof(int));
	strcpy(buf+sizeof(int), name);
	return info_call(h, CMAN_CMD_REG_QUORUMDEV, buf, strlen(name)+sizeof(int), NULL, 0);
}

int cman_unregister_quorum_device(cman_handle_t handle)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	VALIDATE_HANDLE(h);

	return info_call(h, CMAN_CMD_UNREG_QUORUMDEV, NULL, 0, NULL, 0);
}

int cman_poll_quorum_device(cman_handle_t handle, int isavailable)
{
	struct cman_handle *h = (struct cman_handle *)handle;
	VALIDATE_HANDLE(h);

	return info_call(h, CMAN_CMD_POLL_QUORUMDEV, &isavailable, sizeof(int), NULL, 0);
}
