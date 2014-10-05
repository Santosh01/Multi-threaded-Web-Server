/*
 * Multithread myhttpd server 
*/

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>

#include <getopt.h>
#include <pthread.h>

#include <dirent.h>

#define true 1
#define false 0

FILE *log_fp = NULL;
char *run_dir;

pthread_cond_t execution_thread_condition;
pthread_cond_t req_came_to_queuing_thread_condition;
pthread_mutex_t exec_mutex;
pthread_mutex_t queue_mutex;

#define MAX_NUM_THREADS 1024

#define LOGGING 1

#ifdef LOGGING
#define LOG(str) fprintf(log_fp, str);
#endif

#define BACKLOG 10

int run_dir_is_home_dir = 0;

/* structure for storeing clinet information */
struct clientDetails
{
        int fd;
        struct sockaddr_in addr;
        int length;
        char * filename;
        char * ipaddress;
        char * message;
	char * firstline;
        int status;
        time_t dispatched_time;
	time_t executed_time;
};

/* Linked list istructure of ready queue which contains accepeted clients */
struct ready_queue
{
        struct clientDetails *client;
        struct ready_queue *next;
};

struct ready_queue *new, *temp,*p,*head=NULL,*rear=NULL;

char BUF[1024];
int wait_time = 60; /* default waiting time of 60 sec */

/* default scheduling to FCFS */
char* sched_policy = "FCFS";

/* basic setup of http server socket */
int make_server_socket_q(int portnum, int backlog)
{
	struct sockaddr_in saddr;
	int sock_id;
	socklen_t opt = 1;

	sock_id = socket(AF_INET, SOCK_STREAM, 0);
	if (sock_id == -1) {
		perror("call to socket");
		return -1;
	}

	setsockopt(sock_id, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	memset((void *) &saddr, 0, sizeof(saddr));
	saddr.sin_port = htons(portnum);
	saddr.sin_family = AF_INET;
	saddr.sin_addr.s_addr = INADDR_ANY;

	if (bind(sock_id, (struct sockaddr *) &saddr, sizeof(saddr)) != 0) {
		perror("call to bind");
		return -1;
	}

	if (listen(sock_id, backlog) != 0) {
		perror("call to listen");
		return -1;
	} else {
		return sock_id;
	}
}

int make_server_socket(int portnum) {
	return make_server_socket_q(portnum, BACKLOG);
}

void do_404(const char *item, int fd) {
	FILE *fp = fdopen(fd, "w");

	fprintf(fp, "HTTP/1.0 404 Not Found\r\n");
	fprintf(fp, "Content-type: text/plain\r\n");
	fprintf(fp, "\r\n");
	fprintf(fp, "The requested URL %s is not found on this server.\r\n", item);
	fclose(fp);
}

/* Cant handle this request, so send appropriate error response */
void canot_do(int fd) {
	FILE *fp = fdopen(fd, "w");

	fprintf(fp, "HTTP/1.0 501 Not Implemented\r\n");
	fprintf(fp, "Content-type: text/plain\r\n");
	fprintf(fp, "\r\n");
	fprintf(fp, "That command is not yet implemented\r\n");
	fclose(fp);
}

/* Extract the extension of the file requested from the client */
char *get_mime_type(const char *name) {
  char *ext = strrchr(name, '.');
  if (!ext) return NULL;
  if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return "text/html";
  if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
  if (strcmp(ext, ".gif") == 0) return "image/gif";
  if (strcmp(ext, ".png") == 0) return "image/png";
  if (strcmp(ext, ".css") == 0) return "text/css";
  if (strcmp(ext, ".au") == 0) return "audio/basic";
  if (strcmp(ext, ".wav") == 0) return "audio/wav";
  if (strcmp(ext, ".avi") == 0) return "video/x-msvideo";
  if (strcmp(ext, ".mpeg") == 0 || strcmp(ext, ".mpg") == 0) return "video/mpeg";
  if (strcmp(ext, ".mp3") == 0) return "audio/mpeg";
  return NULL;
}

/* Send the response to HEAD request from client */
void do_head(const char*f, FILE *fp, int status, char *title, int length)
{
	time_t now;
	char timebuf[128];
	time_t date;
	struct stat statbuf;

#define SERVER "webserver/1.0"
#define PROTOCOL "\nHTTP/1.0"
#define RFC1123FMT "%a, %d %b %Y %H:%M:%S GMT"

	if (stat(f, &statbuf) >= 0)
		date = statbuf.st_mtime;

	fprintf(fp, "%s %d %s \r\n", PROTOCOL, status, title);
	fprintf(fp, "Server: %s\r\n", SERVER);
	now = time(NULL);
	strftime(timebuf, sizeof(timebuf), RFC1123FMT, gmtime(&now));
	fprintf(fp, "Date: %s\r\n", timebuf);
	
	fprintf(fp, "Content-Type: %s\r\n", get_mime_type(f));
	if (length >= 0) fprintf(fp, "Content-Length: %d\r\n", length);

	if (date != -1) {
		strftime(timebuf, sizeof(timebuf), RFC1123FMT, gmtime(&date));
		fprintf(fp, "Last-Modified: %s\r\n", timebuf);
	}
	fprintf(fp, "Connection: close\r\n");
	fprintf(fp, "\r\n");
}


void header(FILE *fp, const char *content_type)
{
	fprintf(fp, "HTTP/1.0 200 OK\r\n");
	if (content_type) {
		fprintf(fp, "Content-type: %s\r\n", content_type);
	}
}

/* Check if file requested from client is present on the server or not */
int not_exist(const char *f)
{
	struct stat *info = (struct stat *)malloc(sizeof(struct stat));

	if (stat(f, info) == -1) {
		free(info);
		return 1;
	} else {
		free(info);
		return 0;
	}
}

/* Check if file requested from client is a directory */
int isadir(const char *f)
{
	struct stat *st = (struct stat *)malloc(sizeof(struct stat));;

	if (stat(f, st) != 0) {
		free(st);
		return 0;
	} else {
		if(S_ISDIR(st->st_mode)) {
			free(st);
			return 1;
		} else {
			free(st);
			return 0;
		}
	}
}

/* view & send the file's content to remote client */
void do_cat(const char *f, int fd)
{
	char *content = get_mime_type(f);
	FILE *fpsock = NULL;
	FILE *fpfile = NULL;
	int c;

	fprintf(log_fp, "sending file : %s\n", f);

	fpsock = fdopen(fd, "w");
	fpfile = fopen(f, "r");
	if (fpsock != NULL && fpfile != NULL) {
		header(fpsock, content);
		fprintf(fpsock, "\r\n");

		while ((c = getc(fpfile)) != EOF) {
			putc(c, fpsock);
		}

		fclose(fpfile);
		fclose(fpsock);
	}
}

/* If file requested from client is a direcory
   Sort the direcories and send the directory structure to the client
   which can be visible to the client in browser */
int do_ls(const char *path, int fd, int length)
{
	FILE *fp;
	int len = strlen(path);
	struct stat statbuf;
	char pathbuf[4096];

        struct dirent *de;
	struct dirent **namelist;
	int i=0, n=0;


	snprintf(pathbuf, sizeof(pathbuf), "%sindex.html", path);
	if (stat(pathbuf, &statbuf) != 0) {
		printf("file or dir not present: %s\n", pathbuf);
//		return 0;
	}

	if (S_ISREG(statbuf.st_mode)) {
		do_cat(pathbuf, fd);
		fprintf(log_fp, "is a regular file\n");
		return 0;
	}

	fp = fdopen(fd, "w");
	do_head(path, fp, 200, "OK", length);
        fprintf(fp, "<HTML>\r\n<HEAD>\r\n<TITLE>Index of %s</TITLE>\r\n</HEAD>\r\n<BODY>", path);
        fprintf(fp, "<H4>Index of %s</H4>\r\n<PRE>\n", path);
        fprintf(fp, "Name                             Last Modified              Size\r\n");
        fprintf(fp, "<HR>\r\n");
        if (len > 1) fprintf(fp, "<A HREF=\"..\">..</A>\r\n");

	/* used for sorting of directory listing */
	n = scandir(path, &namelist, NULL, alphasort);

	 if (n < 0)
		perror("scandir");
	else {
		for (i=0; i<n; i++ ) {
			de = namelist[i];
			char timebuf[32];
			struct tm *tm;

			strcpy(pathbuf, path);
			strcat(pathbuf, de->d_name);

			stat(pathbuf, &statbuf);
			tm = gmtime(&statbuf.st_mtime);
			strftime(timebuf, sizeof(timebuf), "%d-%b-%Y %H:%M:%S", tm);

			fprintf(fp, "<A HREF=\"%s%s\">", de->d_name, S_ISDIR(statbuf.st_mode) ? "/" : "");
			fprintf(fp, "%s%s", de->d_name, S_ISDIR(statbuf.st_mode) ? "/</A>" : "</A> ");
			if (strlen(de->d_name) < 32) fprintf(fp, "%*s", (int)(32 - strlen(de->d_name)), "");

			if (S_ISDIR(statbuf.st_mode))
				fprintf(fp, "%s\r\n", timebuf);
			else
				fprintf(fp, "%s %10d\r\n", timebuf, (int)statbuf.st_size);

			free(namelist[i]);
		} 
		free(namelist);
	}

        fprintf(fp, "</PRE>\r\n<HR>\r\n<ADDRESS>%s</ADDRESS>\r\n</BODY></HTML>\r\n", SERVER);
	fclose(fp);
	return 0;
}

/*
 * skip over all request info until a CRNL is seen
 */
void read_til_crnl(FILE *fp)
{
	char buf[BUFSIZ] = { 0 };
	while (fgets(buf, BUFSIZ, fp) != NULL && strcmp(buf, "\r\n") != 0);
}

/* 
 * Check who is the current logged in user, from which the server is started 
 * This is used to send the response if ~ is used in the browser 
*/
char *get_home_dir (void) {
	char *buf = (char *)malloc(1024*sizeof(char));
	if(buf == NULL) {
		fprintf(log_fp, "error: allocating memory for buffer %s\n",strerror(errno));
		exit(errno);
	}
	char *temp = (char *)malloc(256*sizeof(char));
	if(temp == NULL) {
		fprintf(log_fp, "error: allocating memory for temp %s\n",strerror(errno));
		free(buf);
		exit(errno);
	}

	strcpy(buf, "/home/");
	cuserid(temp);
	strcat(buf, temp);
	strcat(buf, "/myhttpd");

	free(temp);
	return buf;
}

/* Handle the request HEAD/GET from the client and 
   send the appropriate response
 */
void process_rq(struct clientDetails *cli)
{
	char cmd[BUFSIZ] = {0};
	char arg[BUFSIZ] = {0};
	char *rq = cli->message;
	int fd = cli->fd;

	cli->executed_time = time(0);

	strcpy(arg, run_dir);

	if (sscanf(rq, "%s %s", cmd, arg + strlen(run_dir)) != 2)
		return;
	
	if (run_dir_is_home_dir) {
		strcpy(arg, cli->filename);
		run_dir_is_home_dir = 0;
	}

	fprintf(log_fp, "cmd == %s, arg=%s\n", cmd, arg);

	if (strcmp(cmd, "GET") != 0) {
		if (strcmp(cmd, "HEAD") == 0) {
			FILE *fp = fdopen(fd, "w");
			fprintf(log_fp, "do HEAD\n");
			do_head(arg, fp, 200, "OK", cli->length);
			fclose(fp);
		} else {
			fprintf(log_fp, "can not do\n");
			canot_do(fd);
		}
	} else if (not_exist(arg)) {
		fprintf(log_fp, "not exist\n");
		do_404(arg, fd);
	} else if (isadir(arg)) {
		fprintf(log_fp, "is a dir\n");
		do_ls(arg, fd, cli->length);
	} else {
		fprintf(log_fp, "do cat\n");
		do_cat(arg, fd);
	}
}

/* Print help */
void help(void) {
	printf("\n\t=========================== Run this server as =================================\n\n");
	printf("\tmyhttpd [-d] [-h] [-l file] [-p port] [-r dir] [-t time] [-n threadnum] [-s sched]");
	printf("\n\n");
	printf("\t[-d] Enter debugging mode, do not daemonize\n");
	printf("\t[-h] Print usage summary\n");
	printf("\t[-l file] Log all requests to given file\n");
	printf("\t[-p port] Listen on the given port\n");
	printf("\t[-r dir] Set the root dir\n");
	printf("\t[-t time] Set the queuing time to time seconds\n");
	printf("\t[-n threadnum] Set number of threads waiting in execution thread\n");
	printf("\t[-s sched] Set the scheduling policy, FCFS / SJF\n\n");
}

/* Used for debugging purpose to check how many cients are in ready queue */
void display_queue()
{
	fprintf(log_fp, "displaying queue \n");

        if(head == NULL)
               fprintf(log_fp, "empty queue");
        else {
                temp = head;
                while(temp != NULL)
              {
                        fprintf(log_fp, "\n acceptfd is %d, file name is %s, ip addr is %s\n",
				temp->client->fd,temp->client->filename, temp->client->ipaddress);
			fprintf(log_fp, "message: %s\n", temp->client->message);	
                        temp = temp->next;
              }
        }
}

/* Insert client into the linked list implementation of the ready queue */
void insert_to_ready_queue(struct clientDetails * client) {

	new = (struct ready_queue *)malloc(sizeof(struct ready_queue));
	new->client = client;
	new->next = NULL;

	if(head == NULL) {
		head = new;
		rear = head; /* This is start node */
	}
        else {
                rear->next = new;
                rear = new;
	}
}

/* Formatted Logging */
void log_http_request(struct clientDetails *client) {

        //fprintf(log_fp, "log:");
        char logbuf[4096] = {0};
        char firstline[4096];
	int i=0;
	struct clientDetails *temp = client;

	char logstatus[10];
	char loglength[10];

	fprintf(log_fp, "log request: ");
        strcpy(logbuf, client->ipaddress);
        strcat(logbuf," -  ");

        strcat(logbuf, ctime(&client->dispatched_time));

        strcat(logbuf, ctime(&client->executed_time));

	while (*temp->firstline != '\n') {
		firstline[i++] = *temp->firstline;
		temp->firstline++;
        }

	firstline[i] = '\0';

        strcat(logbuf, firstline);

	sprintf(logstatus,"%d",client->status);
	sprintf(loglength,"%d",client->length);

	strcat(logbuf," ");
	strcat(logbuf,logstatus);
	strcat(logbuf," ");
	strcat(logbuf,loglength);

	fprintf(log_fp, "%s", logbuf);
}

char dir_Name[1024];
#define BUFFER_SIZE 1024

/* Implementation of queuing thread  */
void *queuing_thread(void *arg) {
	int sock = *(int *)arg;

       struct clientDetails *client_ptr;
        int n,size;
	struct stat *st = NULL;
        struct sockaddr_in cliaddr;
        char  *file = (char *)malloc(4096),reldir[4096];
        int clientfd;
	socklen_t clientlength = sizeof(struct sockaddr_in);
	char *home_dir;
	char *charposition = NULL;
	char *temp = (char *)malloc(4096);

	while (1) {

		st = malloc(sizeof(struct stat));
		if(st == NULL) {
			fprintf(log_fp, "error: allocating memory for client_ptr %s\n",strerror(errno));
			exit(errno);
		}

		/* Accept remote client */
                clientfd = accept(sock,(struct sockaddr*)&cliaddr, &clientlength);
                if(clientfd < 0) {
			fprintf(log_fp, "error: accepting client %s\n",strerror(errno));
			free(st);
                        exit(2);
                }

                if(clientfd > 0) {
			n = recv(clientfd,BUF,BUFFER_SIZE,0);//max_size
                        if(n == 1) {
				fprintf(log_fp, "error: receive %s\n",strerror(errno));
			}

                        if(n == 0) {
                                //close(clientfd);
                                fprintf(log_fp, "directory requested\n");
                        }

                        sscanf(BUF,"%*s %s",file);

			if ( (charposition = strchr(file, '~')) != NULL) {
				strcpy(temp, (charposition + 1));
				home_dir = get_home_dir();
				strcpy(file, home_dir);
				strcat(file, "/"); /* attached received filename at the end to skip ~ */
				strcat(file, temp); /* attached received filename at the end to skip ~ */
//				free(home_dir);
				run_dir_is_home_dir = 1;
			}

			strcpy(reldir, run_dir);
			strcat(reldir, file); /* make file as relative path dir */

                        stat(reldir, st);
                        size = st->st_size;
			free(st); /* Free memory used for getting info from stat */
                        client_ptr = (struct clientDetails *)malloc(sizeof(struct clientDetails));
			if(client_ptr == NULL) {
				fprintf(log_fp, "error: allocating memory for client_ptr %s\n",strerror(errno));
				exit(errno);
			}
                        client_ptr->ipaddress = inet_ntoa(cliaddr.sin_addr);
                        client_ptr->dispatched_time = time(0);
                        client_ptr->addr = cliaddr;
                        client_ptr->fd = clientfd;
                        client_ptr->length = size;
                        client_ptr->filename = file;
			client_ptr->message = BUF;
			client_ptr->firstline = BUF;
			client_ptr->status = 1;
			fprintf(log_fp, "server: got connection from %s, file=%s\n", client_ptr->ipaddress, client_ptr->filename);

			insert_to_ready_queue(client_ptr);
			pthread_cond_signal(&req_came_to_queuing_thread_condition); /* Signal to Scheduling Thread that we have one more client */
		}
	}

	fprintf(log_fp, "completed queuing thread\n");
}

/* Remove clinet from ready queue as per FCFS scheduling policy */
struct ready_queue *get_client_fcfs(void) {
	struct ready_queue *temp = NULL;;

	if(head == NULL)
                fprintf(log_fp, "empty queue\n");
        else {
		temp = head;
                head = head->next;
        }

	return(temp);
}

int identify_shortest_job(void) {
	int shortestjob_fd = 0;
	int min, b;

	temp = head;

	if (temp == NULL) {
		fprintf(log_fp, "SJF Queue is Empty\n");
	} else if(temp->next == NULL) {
		/* This has only one client */
		shortestjob_fd = temp->client->fd;
	} else {
		min = temp->client->length;
		while(temp->next != NULL) {
			b = temp->next->client->length;
			if(min <= b) {
				shortestjob_fd = temp->client->fd;
			} else if (min > b) {
				min = temp->next->client->length;
				shortestjob_fd = temp->next->client->fd;
			}
			temp = temp->next;
		}
	}
	fprintf(log_fp, "returning fd %d\n", shortestjob_fd);
	return shortestjob_fd;
}

/* Remove clinet from ready queue as per SJF scheduling policy */
struct ready_queue* get_client_sjf(void) {
	int shortestjob_fd;
	struct ready_queue* previous = NULL;

	shortestjob_fd = identify_shortest_job();
	if (!shortestjob_fd) {
                fprintf(log_fp, "SJF Queue is Empty\n");
        } else {
                temp = head;
                while(temp != NULL) {
			if(temp->client->fd == shortestjob_fd) {

				if(temp == head)
					head = temp->next; /* update head */
				else {
					previous->next = temp->next;
				}
				return temp;
			} else {
				previous = temp;
				temp = temp->next; /* Go to next client in the list */
			} 
		}
	}
	return NULL;
}

struct ready_queue *g_client = NULL; /* Global Pointer Shared between scheduling & Execution Thread */

/* Implementation of execution thread  */
void *execution_thread(void *arg) {
	struct clientDetails *cli;
	fprintf(log_fp, "started execution thread\n");
	
	while (1) {
		pthread_mutex_lock(&queue_mutex);

		fprintf(log_fp, "waiting for request to come from scheduling thread\n\n");	
		pthread_cond_wait(&execution_thread_condition, &queue_mutex);
		fprintf(log_fp, "signal received into execution thread, start processing\n");
	
		if (g_client != NULL)	
			cli = g_client->client;

		/* Remove client from ready queue, as per FCFS scheduling policy */
		if(cli != NULL) {
			process_rq(cli);
			log_http_request(cli);
			//free(cli); /*we have served this client, now free its memory*/
			free(g_client); /*we have served this client, now free its memory*/
		}

	        pthread_mutex_unlock(&queue_mutex);
	}
	fprintf(log_fp, "completed execution thread\n");
}

/* Implementation of Scheduling thread  */
void *scheduling_thread(void *arg) {
			
	fprintf(log_fp, "started scheduling thread\n");
	
	while(1) {	
		pthread_mutex_lock(&queue_mutex);
		pthread_cond_wait(&req_came_to_queuing_thread_condition, &queue_mutex);

		if(strcmp(sched_policy, "FCFS") == 0) {
			g_client = get_client_fcfs();
		} else if(strcmp(sched_policy, "SJF") == 0) {
			g_client = get_client_sjf();
		}
		pthread_mutex_unlock(&queue_mutex);
		printf("request came to queuing thread, signal execution thread to execute it\n");	
		pthread_cond_signal(&execution_thread_condition);
	}
	fprintf(log_fp, "completed scheduling thread\n");
}

int main(int argc, char *argv[])
{
	int sock;
	int c, threadnum = 4, i=0;
	static int option_index;
	char *log_file;
	int portnum = 8080, do_daemonize = true;
	int use_default_dir = 1;

	pthread_t queuing_tid, execution_tid[MAX_NUM_THREADS], scheduling_tid;
	void* ret = NULL;

	run_dir = (char *)malloc(1024); /* Default Run Dir is pwd */
	if(run_dir == NULL) {
		fprintf(stderr, "error: allocating memory for run_dir: %s\n",strerror(errno));
		exit(errno);
	}

	/* Structure for handling command line arguments as used while starting main program */
	static struct option long_options[] = {
		{"daemon", no_argument, 0, 'd'},
		{"help", no_argument, 0, 'h'},
		{"log_file", required_argument, 0, 'l'},
		{"port", required_argument, 0, 'p'},
		{"rundir", required_argument, 0,  'r'},
		{"time", required_argument, 0,  't'},
		{"threadnum", required_argument, 0,  'n'},
		{"sched", required_argument, 0,  's'},
		{0, 0, 0, 0}
        };

	while(1) {
		c = getopt_long(argc, argv, "dhl:p:r:t:n:s:", long_options, &option_index);
		if (c == -1)
			break;

		switch(c) {
		case 0 :
			if (long_options[option_index].flag != 0)
			break;
		case 'd':
			printf("Starting myhttpd in debug mode\n");
			do_daemonize = false;
			break;
		case 'h':
			help();
			exit(0);
		case 'l':
			log_file  = optarg;
			break;
		case 'p':
			portnum = atoi(optarg);
			break;
		case 'r':
			use_default_dir = 0;
			run_dir = optarg;
			break;
		case 't':
			break;
		case 'n':
			threadnum = atoi(optarg);
			break;
		case 's':
			sched_policy = optarg;
			break;
		}
	}
	
	/* Logging */
	log_fp = fopen(log_file, "w+");
	if (!log_fp) {
		log_fp = stdout;
		fprintf(log_fp, "error: unable to open log file, continuing with stdout %s\n",strerror(errno));
	}

	/* use current directory as default directory for accessing */
	if (use_default_dir) {
		if (getcwd(run_dir, 1024) != NULL){
			fprintf(log_fp, "Current working dir: %s\n", run_dir);
		}
	}

	/* Start this application as daemon */
	if (do_daemonize) {
		fprintf(log_fp, "starting myhttpd daemon\n");
		daemon(1, 1);
	}

	fprintf(log_fp, "started daemon successfully\n");

	sock = make_server_socket(portnum);
	if (sock == -1) {
		fprintf(log_fp, "error: creating socket %s\n",strerror(errno));
		exit(2);
	}

	/* Initialize mutex and condition variable objects */
	pthread_mutex_init(&exec_mutex, NULL);
	pthread_mutex_init(&queue_mutex, NULL);
	pthread_cond_init (&execution_thread_condition, NULL);
	pthread_cond_init (&req_came_to_queuing_thread_condition, NULL);

	/* Queuing Thread Implementation */
	pthread_create(&queuing_tid,NULL,&queuing_thread,&sock);
	fprintf(log_fp, "started queuing thread\n");
	/* End of Queuing Thread Implementation */

	/* Scheduling Thread Implementation */
	pthread_create(&scheduling_tid,NULL,&scheduling_thread, NULL);
	fprintf(log_fp, "started scheduling thread\n");

	/* Execution Thread Implementation, start n thread as per command line argument */
	for (i=0; i < threadnum; i++) {
		pthread_create(&execution_tid[i],NULL,&execution_thread,NULL);
	}
	fprintf(log_fp, "started %d execution threads\n", i);

	/* wait for thread to complete */
	if(pthread_join(queuing_tid, &ret))
		fprintf(log_fp, "error: unable to wait for queuing thread completion :%s\n", strerror(errno));
	
	for (i=0; i < threadnum; i++) {
		if(pthread_join(execution_tid[i], &ret))
			fprintf(log_fp, "error: unable to wait for execution thread completion :%s\n", strerror(errno));
	}

	if(pthread_join(scheduling_tid, &ret))
		fprintf(log_fp, "error: unable to wait for scheduling thread completion :%s\n", strerror(errno));

	/* Distroy The Mutexes */
	pthread_mutex_destroy(&exec_mutex);
	pthread_mutex_destroy(&queue_mutex);
	pthread_cond_destroy(&execution_thread_condition);
	pthread_cond_destroy(&req_came_to_queuing_thread_condition);
	
	fprintf(log_fp, "exiting from main\n");
	return 0;
}

