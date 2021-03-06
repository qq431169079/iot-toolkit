/*
 * iot-tddp: An implementation of TDDP
 *
 * Copyright (C) 2017 Fernando Gont <fgont@si6networks.com>
 *
 * Programmed by Fernando Gont for SI6 Networks <https://www.si6networks.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Build with: make iot-tddp
 *
 * It requires that the libpcap and the openssl libraries be installed
 * on your system.
 *
 * Please send any bug reports to Fernando Gont <fgont@si6networks.com>
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <net/if.h>
#include <netdb.h>
#include <pcap.h>

#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <unistd.h>
#include <openssl/md5.h>
#include <openssl/des.h>
#include <openssl/err.h>

#include "iot-tddp.h"
#include "iot-toolkit.h"
#include "libiot.h"

/* #define DEBUG */

/* Function prototypes */
void				init_packet_data(struct iface_data *);
void				free_host_entries(struct host_list *);
int					host_scan_local(pcap_t *, struct iface_data *, struct in6_addr *, unsigned char, \
									struct host_entry *);
void				print_help(void);
int					print_host_entries(struct host_list *, unsigned char);
void				local_sig_alarm(int);
void				usage(void);
int					process_config_file(const char *);

/* Used for multiscan */
struct host_list			host_local, host_global, host_candidate;
struct host_entry			*host_locals[MAX_IPV6_ENTRIES], *host_globals[MAX_IPV6_ENTRIES];
struct host_entry			*host_candidates[MAX_IPV6_ENTRIES];

/* Used for router discovery */
struct iface_data			idata;

/* Variables used for learning the default router */
struct ether_addr			router_ether, rs_ether;
struct in6_addr				router_ipv6, rs_ipv6;

struct in6_addr				randprefix;
unsigned char				randpreflen;

bpf_u_int32				my_netmask;
bpf_u_int32				my_ip;
struct bpf_program		pcap_filter;
char 					dev[64], errbuf[PCAP_ERRBUF_SIZE];
unsigned char			buffer[BUFFER_SIZE], buffrh[MIN_IPV6_HLEN + MIN_TCP_HLEN];
char			readbuff[BUFFER_SIZE], sendbuff[BUFFER_SIZE];
ssize_t					nreadbuff, nsendbuff;
char					line[LINE_BUFFER_SIZE];
unsigned char			*v6buffer, *ptr, *startofprefixes;
char					*pref;
    
struct ether_header		*ethernet;
unsigned int			ndst=0;

char					*lasts, *rpref;
char					*charptr;

size_t					nw;
unsigned long			ul_res, ul_val;
unsigned int			i, j, startrand;
unsigned int			skip;
unsigned char			dstpreflen;

uint16_t				mask;

char 					plinkaddr[ETHER_ADDR_PLEN], pv4addr[INET_ADDRSTRLEN];
char 					pv6addr[INET6_ADDRSTRLEN];
unsigned char 			verbose_f=FALSE;
unsigned char 			dstaddr_f=FALSE, timestamps_f=FALSE, scan_local_f=FALSE;



unsigned char			dst_f=FALSE, end_f=FALSE, endpscan_f=FALSE;
unsigned char			donesending_f=FALSE;
uint16_t				srcport, dstport;
uint32_t				scan_type;
char					scan_type_f=FALSE;
unsigned long			pktinterval, rate;
unsigned int			packetsize;

struct prefixv4_entry	prefix;

char					*charstart, *charend, *lastcolon;
unsigned int			nsleep;
int						sel;
fd_set					srset, swset, seset, rset, wset, eset;
struct timeval			curtime, pcurtime, lastprobe;
struct tm				pcurtimetm;
unsigned int			retrans=0;

int main(int argc, char **argv){
	extern char				*optarg;
	int						r;
	struct addrinfo			hints, *res, *aiptr;
	struct target_ipv6		target;
	struct timeval			timeout;
	void					*voidptr;
	const int				on=1;
	struct sockaddr_in		sockaddr_in, sockaddr_from, sockaddr_to;
	socklen_t				sockaddrfrom_len;
	struct	tddp_hdr		*tddp_hdr;
	unsigned int			npayload=0;

	char				username_admin[]="admin";
	char				password_admin[]="admin";
	char				*username;
	char				*password;
	unsigned int		nusername;
	unsigned int		npassword;
	char				preparekey[BUFFER_SIZE];
	char				keydigest[MD5_DIGEST_LENGTH];
    DES_cblock			des_key[8];
    DES_key_schedule 	key;
	uint8_t		tddp_version=2;

/*
# 0x01	SET_USR_CFG - set configuration information
# 0x02	GET_SYS_INF - get configuration information
# 0x03	CMD_SPE_OPR - special configuration commands
# 0x04	HEART_BEAT -  the heartbeat package
*/
	uint8_t		tddp_type= 0x03; /* GETSYSINF*/

/*
## Code Request Type
# 0x01 TDDP_REQUEST
# 0x02 TDDP_REPLY
tddp_code = "01"
*/
	uint8_t		tddp_code=0x01;

/*
## Reply Info Status
# 0x00 REPLY_OK
# 0x02 ?
# 0x03 ?
# 0x09 REPLY_ERROR
# 0xFF ?
tddp_reply = "00"
*/
	uint8_t		tddp_replyinfo= 0x00;
	uint16_t	tddp_pktid= 0x0020;
	uint8_t		tddp_subtype= 0x00;
    MD5_CTX		mdContext;

	static struct option longopts[] = {
		{"interface", required_argument, 0, 'i'},
		{"pktid", required_argument, 0, 'I'},
		{"code", required_argument, 0, 'c'},
		{"dst-address", required_argument, 0, 'd'},
		{"scan", no_argument, 0, 'Z'},
		{"retrans", required_argument, 0, 'x'},
		{"timeout", required_argument, 0, 'O'},
		{"subtype", required_argument, 0, 't'},
		{"type", required_argument, 0, 'T'},
		{"user", required_argument, 0, 'u'},
		{"src-port", required_argument, 0, 'o'},
		{"dst-port", required_argument, 0, 'a'},
		{"password", required_argument, 0, 'p'},
		{"replyinfo", required_argument, 0, 'r'},
		{"verbose", no_argument, 0, 'v'},
		{"version", required_argument, 0, 'V'},
		{"help", no_argument, 0, 'h'},
		{0, 0, 0,  0 }
	};

	char shortopts[]= "i:I:c:d:Zx:O:t:T:u:o:a:p:r:vV:h";

	char option;

	if(argc<=1){
		usage();
		exit(EXIT_FAILURE);
	}

	srandom(time(NULL));

	init_iface_data(&idata);

	/* Set defaults */
	username= username_admin;
	password= password_admin;
	tddp_type= 0x03; /* GETSYSINF*/
	tddp_code=0x01;
	tddp_replyinfo= 0x00;
	tddp_pktid= 0x0020;
	tddp_subtype= 0x00;



	while((r=getopt_long(argc, argv, shortopts, longopts, NULL)) != -1) {
		option= r;

		switch(option) {
			case 'i':  /* Interface */
				strncpy(idata.iface, optarg, IFACE_LENGTH-1);
				idata.iface[IFACE_LENGTH-1]=0;
				idata.ifindex= if_nametoindex(idata.iface);
				idata.iface_f=TRUE;
				break;

			case 'I':
				tddp_pktid=atoi(optarg);
				break;

			case 'c':
				tddp_code=atoi(optarg);
				break;

			case 't':
				tddp_subtype=atoi(optarg);
				break;

			case 'T':
				tddp_type=atoi(optarg);
				break;

			case 'V':
				tddp_version=atoi(optarg);
				break;

			case 'u':
				username= optarg;
				break;

			case 'p':
				password= optarg;
				break;

			case 'r':
				tddp_replyinfo=atoi(optarg);
				break;

			case 'd':	/* IPv6 Destination Address/Prefix */
				/* The '-d' option contains a domain name */
				if((charptr = strtok_r(optarg, "/", &lasts)) == NULL){
					puts("Error in Destination Address");
					exit(EXIT_FAILURE);
				}

				strncpy(target.name, charptr, NI_MAXHOST);
				target.name[NI_MAXHOST-1]=0;

				if((charptr = strtok_r(NULL, " ", &lasts)) != NULL){
					prefix.len = atoi(charptr);
		
					if(prefix.len > 32){
						puts("Prefix length error in IP Destination Address");
						exit(EXIT_FAILURE);
					}
				}
				else{
					prefix.len= 32;
				}

				memset(&hints, 0, sizeof(hints));
				hints.ai_family= AF_INET;
				hints.ai_canonname = NULL;
				hints.ai_addr = NULL;
				hints.ai_next = NULL;
				hints.ai_socktype= SOCK_DGRAM;

				if( (target.res = getaddrinfo(target.name, NULL, &hints, &res)) != 0){
					printf("Unknown Destination '%s': %s\n", target.name, gai_strerror(target.res));
					exit(1);
				}

				for(aiptr=res; aiptr != NULL; aiptr=aiptr->ai_next){
					if(aiptr->ai_family != AF_INET)
							continue;

					if(aiptr->ai_addrlen != sizeof(struct sockaddr_in))
						continue;

					if(aiptr->ai_addr == NULL)
						continue;

					prefix.ip= ( (struct sockaddr_in *)aiptr->ai_addr)->sin_addr;
				}

				freeaddrinfo(res);

				idata.dstaddr= prefix.ip;				
				idata.dstaddr_f= TRUE;
				dst_f=TRUE;
				break;
	    
			case 'L':
				scan_local_f=TRUE;
				break;

			case 'x':
				idata.local_retrans=atoi(optarg);
				break;

			case 'o':	/* UDP Source Port */
				idata.srcport= atoi(optarg);
				idata.srcport_f= 1;
				break;

			case 'a':	/* UDP Destination Port */
				idata.dstport= atoi(optarg);
				idata.dstport_f= 1;
				break;

			case 'O':
				idata.local_timeout=atoi(optarg);
				break;

			case 'v':	/* Be verbose */
				idata.verbose_f++;
				break;
		
			case 'h':	/* Help */
				print_help();
				exit(EXIT_FAILURE);
				break;

			default:
				usage();
				exit(EXIT_FAILURE);
				break;
		
		} /* switch */
	} /* while(getopt) */


	nusername= Strnlen(username, BUFFER_SIZE);
	npassword= Strnlen(password, BUFFER_SIZE);

	if( (nusername+npassword+2) > BUFFER_SIZE){
		puts("Username/password too long");
		exit(EXIT_FAILURE);
	}

	strncpy(preparekey, username, nusername);
	strncpy( (char *)preparekey + nusername, password, npassword);
	/* Preparekey now contains the concatenated username and assword */

    MD5_Init(&mdContext);
    MD5_Update(&mdContext, preparekey, nusername+npassword);
	MD5_Final((unsigned char *)keydigest, &mdContext);

	memcpy(des_key, keydigest, 8);


	/*
	    XXX: This is rather ugly, but some local functions need to check for verbosity, and it was not warranted
	    to pass &idata as an argument
	 */
	verbose_f= idata.verbose_f;

	if(geteuid()){
		puts("iot-tddp needs superuser privileges to run");
		exit(EXIT_FAILURE);
	}

	if(scan_local_f && !idata.iface_f){
		/* XXX This should later allow to just specify local scan and automatically choose an interface */
/*		puts("Must specify the network interface with the -i option when a local scan is selected"); */
/*		exit(EXIT_FAILURE); */
	}

	if(!dst_f && !scan_local_f){
		if(idata.verbose_f)
			puts("Must specify either a destination prefix ('-d'), or a local scan ('-L')");

		exit(EXIT_FAILURE);
	}

	release_privileges();

	if(get_local_addrs(&idata) == FAILURE){
		puts("Error obtaining list of local interfaces and addresses");
		exit(EXIT_FAILURE);
	}

	if(scan_local_f){
		/* If an interface was specified, we select an IPv4 address from such interface */
		if(idata.iface_f){
			if( (voidptr=find_v4addr_for_iface(&(idata.iflist), idata.iface)) == NULL){
				printf("No IPv4 address for interface %s\n", idata.iface);
				exit(EXIT_FAILURE);
			}

			idata.srcaddr= *((struct in_addr *) voidptr);
		}
		else{
			if( (voidptr=find_v4addr(&(idata.iflist))) == NULL){
				puts("No IPv4 address available on local host");
				exit(EXIT_FAILURE);
			}

			idata.srcaddr= *((struct in_addr *)voidptr);
		}

		if( (idata.fd=socket(AF_INET, SOCK_DGRAM, 0)) == -1){
			puts("Could not create socket");
			exit(EXIT_FAILURE);
		}

		if( setsockopt(idata.fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) == -1){
			puts("Error while setting SO_BROADCAST socket option");
			exit(EXIT_FAILURE);
		}

		memset(&sockaddr_in, 0, sizeof(sockaddr_in));
		sockaddr_in.sin_family= AF_INET;
		sockaddr_in.sin_port= 0;  /* Allow Sockets API to set an ephemeral port */
		sockaddr_in.sin_addr= idata.srcaddr;

		if(bind(idata.fd, (struct sockaddr *) &sockaddr_in, sizeof(sockaddr_in)) == -1){
			puts("Error bind()ing socket to local address");
			exit(EXIT_FAILURE);
		}

		/* Second (receive) socket */
		if( (idata.fd2=socket(AF_INET, SOCK_DGRAM, 0)) == -1){
			puts("Could not create socket");
			exit(EXIT_FAILURE);
		}

		memset(&sockaddr_in, 0, sizeof(sockaddr_in));
		sockaddr_in.sin_family= AF_INET;
		if(idata.srcport_f){
			sockaddr_in.sin_port= htons(idata.srcport);  /* Allow Sockets API to set an ephemeral port */
		}
		else{
			sockaddr_in.sin_port= htons(TDDP_RECEIVE_PORT);  /* Allow Sockets API to set an ephemeral port */
		}

		sockaddr_in.sin_addr= idata.srcaddr;

		if(bind(idata.fd2, (struct sockaddr *) &sockaddr_in, sizeof(sockaddr_in)) == -1){
			puts("Error bind()ing socket to local address");
			exit(EXIT_FAILURE);
		}

		memset(&sockaddr_to, 0, sizeof(sockaddr_to));
		sockaddr_to.sin_family= AF_INET;

		if(idata.dstport_f){
			sockaddr_to.sin_port= htons(idata.dstport);
		}
		else{
			sockaddr_to.sin_port= htons(TDDP_SERVICE_PORT);
		}
		sockaddr_to.sin_addr= idata.dstaddr;		

		memset(&sockaddr_from, 0, sizeof(sockaddr_from));
		sockaddr_from.sin_family= AF_INET;
		sockaddrfrom_len=sizeof(sockaddr_from);
		tddp_hdr= (struct tddp_hdr *) sendbuff;
		memset(tddp_hdr, 0, sizeof(struct tddp_hdr));

		tddp_hdr->version= tddp_version;
		tddp_hdr->type= tddp_type;
		tddp_hdr->code= tddp_code;
		tddp_hdr->replyinfo= tddp_replyinfo;
		tddp_hdr->pktid= tddp_pktid;
		tddp_hdr->subtype= tddp_subtype;
		tddp_hdr->res= 0x00;

		charptr= (char *) (sendbuff + sizeof(struct tddp_hdr));;
		/* here we should insert any data */
		npayload= charptr- sendbuff - sizeof(struct tddp_hdr);

		/* XXX check bounds */
		for(i=0; i <  (npayload%8); i++){
			*charptr= 0x00;
			charptr++;
		}


		nsendbuff= charptr- sendbuff;
		npayload= nsendbuff - sizeof(struct tddp_hdr);
		tddp_hdr->pktlength= htonl( (uint32_t)npayload);

		if(npayload){
			ERR_clear_error();

			if(DES_set_key(des_key, &key) < 0){
		        puts("Error setting up DES key");
				exit(EXIT_FAILURE);
			}

			for(i=0; i< (npayload/8); i++){
			    DES_ecb_encrypt( (DES_cblock *) charptr, (DES_cblock *) charptr, &key, 1);
				charptr= charptr+8;
			}
		}

		/* Now compute MD5 checksum */
	    MD5_Init(&mdContext);
        MD5_Update(&mdContext, sendbuff, nsendbuff);
	    MD5_Final (tddp_hdr->md5_digest ,&mdContext);
		puts("Sending TDDP Packet:");
		print_tddp_packet(sendbuff, nsendbuff);

		if(!idata.dstaddr_f){
			puts("Must specify destination address");
			exit(EXIT_FAILURE);
		}


		FD_ZERO(&srset);
		FD_ZERO(&swset);
		FD_ZERO(&seset);

		FD_SET(idata.fd, &swset);
		FD_SET(idata.fd, &srset);
		FD_SET(idata.fd2, &srset);
		FD_SET(idata.fd, &seset);
		FD_SET(idata.fd2, &seset);

		lastprobe.tv_sec= 0;	
		lastprobe.tv_usec=0;
		idata.pending_write_f=TRUE;	

		/* The end_f flag is set after the last probe has been sent and a timeout period has elapsed.
		   That is, we give responses enough time to come back
		 */
		while(!end_f){
			rset= srset;
			wset= swset;
			eset= seset;

			if(!donesending_f){
				/* This is the retransmission timer */
				timeout.tv_sec= 1;
				timeout.tv_usec= 0;
			}
			else{
				/* XXX: This should use the parameter from command line */
				timeout.tv_sec= idata.local_timeout;
				timeout.tv_usec=0;
			}

			/*
				Check for readability and exceptions. We only check for writeability if there is pending data
				to send.
			 */
			if((sel=select(idata.fd2+1, &rset, (idata.pending_write_f?&wset:NULL), &eset, &timeout)) == -1){
				if(errno == EINTR){
					continue;
				}
				else{
					perror("iot-tddp:");
					exit(EXIT_FAILURE);
				}
			}

			if(gettimeofday(&curtime, NULL) == -1){
				if(idata.verbose_f)
					perror("iot-tddp");

				exit(EXIT_FAILURE);
			}

			/* Check whether we have finished probing all targets */
			if(donesending_f){
				/*
				   Just wait for SELECT_TIMEOUT seconds for any incoming responses.
				*/

				if(is_time_elapsed(&curtime, &lastprobe, idata.local_timeout * 1000000)){
					end_f=TRUE;
				}
			}


			if(sel && FD_ISSET(idata.fd2, &rset)){
				/* XXX: Process response packet */

				if( (nreadbuff = recvfrom(idata.fd2, readbuff, sizeof(readbuff), 0, (struct sockaddr *)&sockaddr_from, &sockaddrfrom_len)) == -1){
					perror("iot-tddp: ");
					exit(EXIT_FAILURE);
				}

				if(inet_ntop(AF_INET, &(sockaddr_from.sin_addr), pv4addr, sizeof(pv4addr)) == NULL){
					perror("iot-tddp: ");
					exit(EXIT_FAILURE);
				}

				printf("Read %u bytes from %s\n", (unsigned int)nreadbuff, pv4addr);
				print_tddp_packet(readbuff, nreadbuff);
			}

			if(sel && FD_ISSET(idata.fd, &rset)){
				/* XXX: Process response packet */

				if( (nreadbuff = recvfrom(idata.fd, readbuff, sizeof(readbuff), 0, (struct sockaddr *)&sockaddr_from, &sockaddrfrom_len)) == -1){
					perror("iot-tddp: ");
					exit(EXIT_FAILURE);
				}

				if(inet_ntop(AF_INET, &(sockaddr_from.sin_addr), pv4addr, sizeof(pv4addr)) == NULL){
					perror("iot-tddp: ");
					exit(EXIT_FAILURE);
				}

				/* There's data to be desencrypted */
				if(nreadbuff> sizeof(struct tddp_hdr)){
					for(i=0; i< ((nreadbuff-sizeof(struct tddp_hdr))/8); i++){
					    DES_ecb_encrypt( (DES_cblock *)(readbuff+ sizeof(struct tddp_hdr) + i * 8), \
						(DES_cblock *)(readbuff+ sizeof(struct tddp_hdr) + i * 8), &key, 0);
					}
				}

				printf("Read %u bytes from %s\n", (unsigned int)nreadbuff, pv4addr);
				print_tddp_packet(readbuff, nreadbuff);
			}


			if(!donesending_f && !idata.pending_write_f && is_time_elapsed(&curtime, &lastprobe, 1 * 1000000)){
				idata.pending_write_f=TRUE;
				continue;
			}

			if(!donesending_f && idata.pending_write_f && FD_ISSET(idata.fd, &wset)){
				idata.pending_write_f=FALSE;

				/* XXX: SEND PROBE PACKET */

				if( sendto(idata.fd, sendbuff, nsendbuff, 0, (struct sockaddr *) &sockaddr_to, sizeof(sockaddr_to)) == -1){
					perror("iot-tddp: ");
					exit(EXIT_FAILURE);
				}


				if(gettimeofday(&lastprobe, NULL) == -1){
					if(idata.verbose_f)
						perror("iot-tddp");

					exit(EXIT_FAILURE);
				}

				retrans++;

				if(retrans >= idata.local_retrans)
					donesending_f= 1;

			}


			if(FD_ISSET(idata.fd, &eset)){
				if(idata.verbose_f)
					puts("iot-tddp: Found exception on descriptor");

				exit(EXIT_FAILURE);
			}
			if(FD_ISSET(idata.fd2, &eset)){
				if(idata.verbose_f)
					puts("iot-tddp: Found exception on descriptor");

				exit(EXIT_FAILURE);
			}
		}

	}	

	exit(EXIT_SUCCESS);
}




/*
 * Function: match_strings()
 *
 * Checks whether one string "matches" within another string
 */

int match_strings(char *buscar, char *buffer){
	unsigned int buscars, buffers;
	unsigned int i=0, j=0;

	buscars= Strnlen(buscar, MAX_IEEE_OUIS_LINE_SIZE);
	buffers= Strnlen(buffer, MAX_IEEE_OUIS_LINE_SIZE);

	if(buscars > buffers)
		return(0);

	while(i <= (buffers - buscars)){
		j=0;

		while(j < buscars){
			if(toupper((int) ((unsigned char)buscar[j])) != toupper((int) ((unsigned char)buffer[i+j])))
				break;

			j++;
		}

		if(j >= buscars)
			return(1);

		i++;
	}

	return(0);
}





/*
 * Function: usage()
 *
 * Prints the syntax of the iot-tddp tool
 */

void usage(void){
	puts("usage: iot-tddp (-L | -d) [-i INTERFACE] [-v] [-h]");
}


/*
 * Function: print_help()
 *
 * Prints help information for the iot-tddp tool
 */

void print_help(void){
	puts(SI6_TOOLKIT);
	puts( "iot-tddp: A tool to play with the TDDP protocol\n");
	usage();
    
	puts("\nOPTIONS:\n"
	     "  --interface, -i             Network interface\n"
	     "  --dst-address, -d           IP Destination Address\n"
		 "  --src-port, -o              Transport Source Port\n"
		 "  --dst-port, -a              Transport Destination Port\n"
	     "  --retrans, -x               Number of retransmissions of each packet\n"
	     "  --timeout, -O               Timeout in seconds (default: 1 second)\n"
		 "  --version, -V               TDDP version\n"
		 "  --pktid, -I                 TDDP PktId\n"
		 "  --replyinfo, -r             TDDP ReplyInfo\n"
		 "  --type, -T                  TDDP Type\n"
		 "  --subtype, -t               TDDP SubType\n"
		 "  --code,-c                   TDDP Code\n"
		 "  --user, -u                  TP-Link device Username\n"
		 "  --password, -p              TP-Link device Password\n"
		 "  --scan, -Z      	        Scan for TP-Link devices\n"
	     "  --help, -h                  Print help for the iot-tddp tool\n"
	     "  --verbose, -v               Be verbose\n"
	     "\n"
	     " Programmed by Fernando Gont for SI6 Networks <https://www.si6networks.com>\n"
	     " Please send any bug reports to <fgont@si6networks.com>\n"
	);
}




/*
 * Function: print_host_entries()
 *
 * Prints the IPv6 addresses (and optionally the Ethernet addresses) in a list
 */

int print_host_entries(struct host_list *hlist, unsigned char flag){
	unsigned int i;

	for(i=0; i < (hlist->nhosts); i++){
		if(inet_ntop(AF_INET6, &((hlist->host[i])->ip6), pv6addr, sizeof(pv6addr)) == NULL){
			if(verbose_f>1)
				puts("inet_ntop(): Error converting IPv6 address to presentation format");

			return(-1);
		}

		if(flag == PRINT_ETHER_ADDR){
			if(ether_ntop( &((hlist->host[i])->ether), plinkaddr, sizeof(plinkaddr)) == 0){
				if(verbose_f>1)
					puts("ether_ntop(): Error converting address");

				return(-1);
			}

			printf("%s @ %s\n", pv6addr, plinkaddr);
		}
		else
			printf("%s\n", pv6addr);
	}

	return 0;
}



/*
 * Function: print_unique_host_entries()
 *
 * Prints only one IPv6 address (and optionally the Ethernet addresses) per Ethernet 
 * address in a list.
 */

int print_unique_host_entries(struct host_list *hlist, unsigned char flag){
	unsigned int i, j, k;

	for(i=0; i < (hlist->nhosts); i++){

		if(i){
			for(j=0; j < i; j++){
				for(k=0; k < ETH_ALEN; k++){
					if((hlist->host[i])->ether.a[k] != (hlist->host[j])->ether.a[k])
						break;
				}

				if(k == ETH_ALEN)
					break;
			}			

			if(j < i)
				continue;
		}
			
		if(inet_ntop(AF_INET6, &((hlist->host[i])->ip6), pv6addr, sizeof(pv6addr)) == NULL){
			if(verbose_f>1)
				puts("inet_ntop(): Error converting IPv6 address to presentation format");

			return(-1);
		}

		if(flag == PRINT_ETHER_ADDR){
			if(ether_ntop( &((hlist->host[i])->ether), plinkaddr, sizeof(plinkaddr)) == 0){
				if(verbose_f>1)
					puts("ether_ntop(): Error converting address");

				return(-1);
			}

			printf("%s @ %s\n", pv6addr, plinkaddr);
		}
		else
			printf("%s\n", pv6addr);
	}

	return 0;
}



/*
 * Function: free_host_entries()
 *
 * Releases memory allocated for holding IPv6 addresses and Ethernet addresses
 */

void free_host_entries(struct host_list *hlist){
	unsigned int i;

	for(i=0; i< hlist->nhosts; i++)
		free(hlist->host[i]);

	hlist->nhosts=0;	/* Set the number of entries to 0, to reflect the released memory */
	return;
}





void	print_tddp_packet(void *ptr, unsigned int len){
	struct tddp_hdr *tddp_hdr;
	unsigned int 	i;
	tddp_hdr= ptr;

	if(len < sizeof(struct tddp_hdr))
		return;

	printf("Version: %02x   Type: %02x   Subtype: %02x  Code: %02x  ReplyInfo: %02x   PktLength: %08x"\
			"   PktId: %04x\nMD5 Digest: ",\
			tddp_hdr->version, tddp_hdr->type, tddp_hdr->subtype, tddp_hdr->code, tddp_hdr->replyinfo, \
			ntohl(tddp_hdr->pktlength), ntohs(tddp_hdr->pktid));

	for(i=0; i<16; i++){
		printf("%02x", tddp_hdr->md5_digest[i]);
	}

	puts("");
	puts("Payload:");
	dump_text((char *) ptr + sizeof(struct tddp_hdr), len- sizeof(struct tddp_hdr));
}

