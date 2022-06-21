/* SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause) */
/* Copyright (c) 2022, Oracle and/or its affiliates. */

#include "bpftune.bpf.h"
#include "tcp_buffer_tuner.h"

bool under_memory_pressure = false;
bool near_memory_pressure = false;
bool near_memory_exhaustion = false;
int conn_count;

/* set from userspace */
int kernel_page_size;
int kernel_page_shift;
int sk_mem_quantum;
int sk_mem_quantum_shift;
unsigned long nr_free_buffer_pages;

static __always_inline bool tcp_nearly_out_of_memory(struct sock *sk,
						     struct bpftune_event *event)
{
	long allocated, limit_sk_mem_quantum[3] = {};
	long tcp_mem[3] = {}, tcp_mem_new[3] = {};
	struct net *net;
	int i;

	if (!sk->sk_prot)
		return false;

	allocated = sk->sk_prot->memory_allocated->counter;
	if (bpf_probe_read(tcp_mem,
			   sizeof(tcp_mem),
			   sk->sk_prot->sysctl_mem))
		return false;

	if (!tcp_mem[2])
		return false;

	for (i = 0; i < 3; i++) {
		limit_sk_mem_quantum[i] = tcp_mem[i];

		if (kernel_page_size > limit_sk_mem_quantum[i])
			limit_sk_mem_quantum[i] <<= kernel_page_shift -
						    sk_mem_quantum_shift;
		else if (kernel_page_size < limit_sk_mem_quantum[i])
			limit_sk_mem_quantum[i] >>= sk_mem_quantum_shift -
						    kernel_page_shift;
	}	

	//__bpf_printk("memory allocated %ld, mem pressure/high %ld %ld\n",
	//	     allocated, limit_sk_mem_quantum[1],
	//	     limit_sk_mem_quantum[2]);
	if (NEARLY_FULL(allocated, limit_sk_mem_quantum[1])) {
		/* send approaching memory pressure event */
		tcp_mem_new[0] = BPFTUNE_GROW_BY_QUARTER(tcp_mem[0]);
		tcp_mem_new[1] = BPFTUNE_GROW_BY_QUARTER(tcp_mem[1]);
		tcp_mem_new[2] = BPFTUNE_GROW_BY_QUARTER(tcp_mem[2]);
		send_sysctl_event(sk, TCP_MEM_PRESSURE,
				  TCP_BUFFER_TCP_MEM, tcp_mem,
				  tcp_mem_new, event);
		near_memory_pressure = true;
	} else {
		near_memory_pressure = false;
	}

	if (NEARLY_FULL(allocated, limit_sk_mem_quantum[2])) {
		/* send approaching memory exhaustion event */
		tcp_mem_new[0] = tcp_mem[0];
		tcp_mem_new[1] = tcp_mem[1];
		tcp_mem_new[2] = BPFTUNE_GROW_BY_QUARTER(tcp_mem[2]);
		send_sysctl_event(sk, TCP_MEM_EXHAUSTION,
				  TCP_BUFFER_TCP_MEM, tcp_mem,
				  tcp_mem_new, event);
		near_memory_exhaustion = true;
	} else {
		near_memory_exhaustion = false;
	}

	return near_memory_pressure || near_memory_exhaustion;
}

SEC("fentry/tcp_enter_memory_pressure")
int BPF_PROG(bpftune_enter_memory_pressure, struct sock *sk)
{
	under_memory_pressure = true;
	return 0;
}

SEC("fentry/tcp_leave_memory_pressure")
int BPF_PROG(bpftune_leave_memory_pressure, struct sock *sk)
{
	under_memory_pressure = false;
	return 0;
}

/* By instrumenting tcp_sndbuf_expand() we know the following, due to the
 * fact tcp_should_expand_sndbuf() has returned true:
 *
 * - the socket is not locked (SOCK_SNDBUF_LOCKED);
 * - we are not under global TCP memory pressure; and
 * - not under soft global TCP memory pressure; and
 * - we have not filled the congestion window.
 *
 * However, all that said, we may soon run out of sndbuf space, so
 * if it is nearly exhausted (>75% full), expand by 25%.
 */
SEC("fentry/tcp_sndbuf_expand")
int BPF_PROG(bpftune_sndbuf_expand, struct sock *sk)
{
	struct bpftune_event event = {};
	struct net *net = sk->sk_net.net;
	long wmem[3], wmem_new[3];
	long sndbuf;

	if (!sk || !net || near_memory_pressure || near_memory_exhaustion)
		return 0;

	sndbuf = sk->sk_sndbuf;
	wmem[2] = net->ipv4.sysctl_tcp_wmem[2];

	if (NEARLY_FULL(sndbuf, wmem[2])) {
		if (tcp_nearly_out_of_memory(sk, &event))
			return 0;

		wmem[0] = wmem_new[0] = net->ipv4.sysctl_tcp_wmem[0];
		wmem[1] = wmem_new[1] = net->ipv4.sysctl_tcp_wmem[1];
		wmem_new[2] = BPFTUNE_GROW_BY_QUARTER(wmem[2]);

		send_sysctl_event(sk, TCP_BUFFER_INCREASE, TCP_BUFFER_TCP_WMEM,
				  wmem, wmem_new, &event);
	}
	return 0;
}

/* sadly tcp_rcv_space_adjust() has checks internal to it so it is called
 * regardless of if we are under memory pressure or not; so use the variable
 * we set when memory pressure is triggered.
 */
SEC("fentry/tcp_rcv_space_adjust")
int BPF_PROG(bpftune_rcvbuf_adjust, struct sock *sk)
{
	struct bpftune_event event = {};
	struct net *net = sk->sk_net.net;
	long rmem[3], rmem_new[3];
	long rcvbuf;

	if (!sk || !net)
		return 0;

	if ((sk->sk_userlocks & SOCK_RCVBUF_LOCK) || near_memory_pressure ||
	    near_memory_exhaustion)
		return 0;

	rcvbuf = sk->sk_rcvbuf;
	rmem[2] = net->ipv4.sysctl_tcp_rmem[2];

	if (NEARLY_FULL(rcvbuf, rmem[2])) {
		if (tcp_nearly_out_of_memory(sk, &event))
			return 0;

		rmem[0] = rmem_new[0] = net->ipv4.sysctl_tcp_rmem[0];
		rmem[1] = rmem_new[1] = net->ipv4.sysctl_tcp_rmem[1];
		rmem_new[2] = BPFTUNE_GROW_BY_QUARTER(rmem[2]);
		send_sysctl_event(sk, TCP_BUFFER_INCREASE, TCP_BUFFER_TCP_RMEM,
				  rmem, rmem_new, &event);
	}
	return 0;
}

SEC("fentry/tcp_init_sock")
int BPF_PROG(bpftune_tcp_init_sock, struct sock *sk)
{
	struct bpftune_event event = {};

	if (sk)
		(void) tcp_nearly_out_of_memory(sk, &event);
	return 0;
}
