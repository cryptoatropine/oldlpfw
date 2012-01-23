#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <sys/msg.h>
#include <sys/ipc.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h> //required for netfilter.h
#include <sys/time.h>
#include <sys/capability.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdlib.h> //for malloc
#include <ctype.h> // for toupper
#include <unistd.h>
#include <signal.h>
#include <stdarg.h> //for dynamic arguments
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <syslog.h>
#include <grp.h>
#include <libnfnetlink/libnfnetlink.h>
#include <libnetfilter_queue/libnetfilter_queue.h>
#include <libnetfilter_conntrack/libnetfilter_conntrack.h>
#include <arpa/inet.h> //for ntohl()
#include <linux/netfilter.h> //for NF_ACCEPT, NF_DROP etc
#include <assert.h>
#include "common/includes.h"
#include "common/defines.h"
#include "argtable/argtable2.h"
#include "version.h" //for version string during packaging

gid_t lpfwuser_gid;

//should be available globally to call nfq_close from sigterm handler
struct nfq_handle *globalh_out_tcp, *globalh_out_udp, *globalh_out_rest, *globalh_in;

//command line arguments available globally
struct arg_str *ipc_method, *logging_facility, *frontend;
struct arg_file *rules_file, *pid_file, *log_file, *cli_path, *gui_path, *pygui_path;
struct arg_int *log_info, *log_traffic, *log_debug;

char ownpath[PATHSIZE]; //full path of lpfw executable
char owndir[PATHSIZE]; //full path to the dir lpfw executable is in (with trailing /)

FILE *fileloginfo_stream, *filelogtraffic_stream, *filelogdebug_stream;

//first element of dlist is an empty one,serves as reference to determine the start of dlist
dlist *first_rule;
#ifndef WITHOUT_SYSVIPC
dlist*copy_first;
#endif

//type has to be initialized to one, otherwise if it is 0 we'll get EINVAL on msgsnd
msg_struct msg_d2f = {1, 0};
msg_struct msg_f2d = {1, 0};
msg_struct msg_d2fdel = {1, 0};
msg_struct msg_d2flist = {1, 0};
msg_struct_creds msg_creds = {1, 0};

extern int fe_ask_out ( char*, char*, unsigned long long* );
extern int fe_ask_in(char *path, char *pid, unsigned long long *stime, char *ipaddr, int sport, int dport);
extern int fe_list();
extern void init_msgq();
extern int sha512_stream ( FILE *stream, void *resblock );
extern int fe_awaiting_reply;
extern int mqd_d2ftraffic;
extern struct msqid_ds *msgqid_d2ftraffic;
extern void* run_tests(void *);


//Forward declarations to make code parser happy
int dlist_add ( char *path, char *pid, char *perms, mbool current, char *sha, unsigned long long stime, off_t size, int nfmark, unsigned char first_instance );
int ( *m_printf ) ( int loglevel, char *logstring );

//mutex to protect dlist AND nfmark_count
pthread_mutex_t dlist_mutex = PTHREAD_MUTEX_INITIALIZER;
//mutex to avoid fe_ask_* to send data simultaneously
pthread_mutex_t msgq_mutex = PTHREAD_MUTEX_INITIALIZER;
//mutex to lock fe_active_flag
pthread_mutex_t fe_active_flag_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t logstring_mutex = PTHREAD_MUTEX_INITIALIZER;

//thread which listens for command and thread which scans for rynning apps and removes them from the dlist
pthread_t refresh_thread, nfqinput_thread, cachebuild_thread, nfqout_udp_thread, nfqout_rest_thread;
pthread_t conntrack_export_thread, trafficdestroy_thread, read_stats_thread, ct_del_thread;

#ifdef DEBUG
pthread_t unittest_thread, rulesdump_thread;
#endif

//flag which shows whether frontend is running
int fe_active_flag = 0;
//fe_was_busy_* is a flag to know whether frontend was processing some request
//Normally, if path is not found in dlist, we send a request to frontend
//But in case it was busy when we started iterating dlist, we assume FRONTEND_BUSY
//This prevents possible duplicate entries in dlist
int fe_was_busy_in, fe_was_busy_out;

//netfilter mark to be put on an ALLOWed packet
int nfmark_to_set_out, nfmark_to_set_in, nfmark_to_delete_in, nfmark_to_delete_out;
//numbers of rules to which current process belongs
int rule_ordinal_out, rule_ordinal_in;

// holds currently-being-processed packet's size for in and out NFQUEUE
int out_packet_size, in_packet_size;

char* tcp_membuf, *tcp6_membuf, *udp_membuf, *udp6_membuf; //MEMBUF_SIZE to fread /tcp/net/* in one swoop
char tcp_smallbuf[4096], udp_smallbuf[4096];
FILE *tcpinfo, *tcp6info, *udpinfo, *udp6info;
int tcpinfo_fd, tcp6info_fd, udpinfo_fd, udp6info_fd, procnetrawfd;

struct nf_conntrack *ct_out_tcp, *ct_out_udp, *ct_out_icmp, *ct_in;
struct nfct_handle *dummy_handle;
struct nfct_handle *setmark_handle_out, *setmark_handle_in;

int nfqfd_input,
nfqfd_tcp, nfqfd_udp, nfqfd_rest;

pthread_cond_t condvar = PTHREAD_COND_INITIALIZER;
pthread_mutex_t condvar_mutex = PTHREAD_MUTEX_INITIALIZER;
char predicate = FALSE;
//holds the time when last packet was seen
struct timeval lastpacket = {0};
pthread_mutex_t lastpacket_mutex = PTHREAD_MUTEX_INITIALIZER;

//netfilter mark number for the packet (to be added to NF_MARK_BASE)
int nfmark_count = 0;

int tcp_stats, udp_stats;
int tcp_table[MEMBUF_SIZE], udp_table[MEMBUF_SIZE], tcp6_table[MEMBUF_SIZE], udp6_table[MEMBUF_SIZE];

char logstring[PATHSIZE];


#define M_PRINTF(loglevel, ...) \
    pthread_mutex_lock(&logstring_mutex); \
    snprintf (logstring, PATHSIZE, __VA_ARGS__); \
    m_printf (loglevel, logstring); \
    pthread_mutex_unlock(&logstring_mutex); \
 



void * readstatsthread( void *ptr)
{
  static int old_tcp_stats;
  static int old_udp_stats;

  old_tcp_stats = 0;
  old_udp_stats = 0;
  int new_tcp_stats, new_udp_stats;

  while(1)
    {
      sleep(1);
      new_tcp_stats = tcp_stats - old_tcp_stats;
      new_udp_stats = udp_stats - old_udp_stats;
      printf (" %d %d \n", new_tcp_stats, new_udp_stats);
      old_tcp_stats = tcp_stats;
      old_udp_stats = udp_stats;
    }

}



int delete_mark(enum nf_conntrack_msg_type type, struct nf_conntrack *mct,void *data)
{
  //while(1){printf("DELMARK");}
  int mark = nfct_get_attr_u32(mct, ATTR_MARK);
  if ( mark == nfmark_to_delete_in || mark == nfmark_to_delete_out)
    {
      if (nfct_query(dummy_handle, NFCT_Q_DESTROY, mct) == -1)
        {
          M_PRINTF ( MLOG_DEBUG, "nfct_query DESTROY %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
          return NFCT_CB_CONTINUE;
        }
      M_PRINTF ( MLOG_DEBUG, "deleted entry %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
      return NFCT_CB_CONTINUE;
    }
  return NFCT_CB_CONTINUE;
}

//this array is used inside lpfw for debug purposes
int ct_entries[CT_ENTRIES_EXPORT_MAX][9] = {};
//this array is built for export to frontend based on ct_entries
int ct_entries_export[CT_ENTRIES_EXPORT_MAX][3] = {};
/*
  [0] nfmark
  [1] bytes in
  [2] bytes out
  [3] bytes in from all previously destroyed conntracks which had this nfmark
  [4] bytes out from all previously destroyed conntracks which had this nfmark
  [5] [1] + [3]
  [6] [2] + [4]
  For debug purposes:
  [7] how many times previous [5] was larger than the present [5]
  [8] how many times previous [6] was larger than the present [6]
*/

//process rules that trafficthread dumps every second to extract traffic statistics
int traffic_callback(enum nf_conntrack_msg_type type, struct nf_conntrack *mct,void *data)
{
  int mark;
  ulong in_bytes, out_bytes;
  if ((mark = nfct_get_attr_u32(mct, ATTR_MARK)) == 0)
    {
      //printf ("nfmark 0 detected \n");
      return NFCT_CB_CONTINUE;
    }
  out_bytes = nfct_get_attr_u32(mct, ATTR_ORIG_COUNTER_BYTES);
  in_bytes = nfct_get_attr_u32(mct, ATTR_REPL_COUNTER_BYTES);

  int i;
  for (i = 0; ct_entries[i][0] != 0; ++i)
    {
      if (ct_entries[i][0] != mark) continue;
      ct_entries[i][1] += in_bytes;
      ct_entries[i][2] += out_bytes;
      return NFCT_CB_CONTINUE;
    }
  //the entry is not yet in array, adding now
  ct_entries[i][0] = mark;
  ct_entries[i][1] = in_bytes;
  ct_entries[i][2] = out_bytes;
  return NFCT_CB_CONTINUE;
}

//process rules that trafficthread dumps every second to extract traffic statistics
int traffic_destroyfilter_callback(enum nf_conntrack_msg_type type, struct nf_conntrack *mct,void *data)
{
  int mark;
  ulong in_bytes, out_bytes;
  if ((mark = nfct_get_attr_u32(mct, ATTR_MARK)) == 0)
    {
      //printf ("destroy nfmark 0 detected \n");
      return NFCT_CB_CONTINUE;
    }
  out_bytes = nfct_get_attr_u32(mct, ATTR_ORIG_COUNTER_BYTES);
  in_bytes = nfct_get_attr_u32(mct, ATTR_REPL_COUNTER_BYTES);

  int i;
  for (i = 0; ct_entries[i][0] != 0; ++i)
    {
      if (ct_entries[i][0] != mark) continue;
      ct_entries[i][3] += in_bytes;
      ct_entries[i][4] += out_bytes;
      return NFCT_CB_CONTINUE;
    }
  printf ("Error: there was a request to destroy nfmark which is not in the list \n");
  return NFCT_CB_CONTINUE;
}

typedef struct
{
  long type;
  int ct_entries_export[CT_ENTRIES_EXPORT_MAX][3];
} mymsg;


//dump all conntrack entries every second and send them to frontend
void * conntrackexporthread( void *ptr)
{
  u_int8_t family = AF_INET;
  struct nfct_handle *traffic_handle;
  if ((traffic_handle = nfct_open(NFNL_SUBSYS_CTNETLINK, 0)) == NULL)
    {
      perror("nfct_open");
    }
  if ((nfct_callback_register(traffic_handle, NFCT_T_ALL, traffic_callback, NULL) == -1))
    {
      perror("cb_reg");
    }
  while(1)
    {
      //zero out from previous iteration
      int i;
      for (i=0; i<CT_ENTRIES_EXPORT_MAX; ++i)
        {
	  ct_entries[i][1] = ct_entries[i][2] = ct_entries_export[i][0] = ct_entries_export[i][1] = ct_entries_export[i][2] = 0;
        }
      if (nfct_query(traffic_handle, NFCT_Q_DUMP, &family) == -1)
        {
          perror("query-DELETE");
        }
//we get here only when dumping operation finishes and traffic_callback has created a new array of
//conntrack entries
      for (i = 0; ct_entries[i][0] != 0; ++i)
        {
	  if (ct_entries[i][5] > ct_entries[i][1]+ct_entries[i][3]) ++ct_entries[i][7];
	  if (ct_entries[i][6] > ct_entries[i][2]+ct_entries[i][4]) ++ct_entries[i][8];
	  ct_entries[i][5] = ct_entries[i][1]+ct_entries[i][3];
	  ct_entries[i][6] = ct_entries[i][2]+ct_entries[i][4];

          // printf ("%d >%d <%d >%d <%d >%d <%d errin:%d errout:%d \n", rules[i][0],
          //    rules[i][1], rules[i][2], rules[i][3], rules[i][4], rules[i][5], rules[i][6],
          //    rules[i][7], rules[i][8]);
        }

      //rearrange array for export
      int j;
      for (i=0; ct_entries[i][0] != 0; ++i)
        {
	  for (j=0; ct_entries_export[j][0] !=0; ++j)
            {
	      if (ct_entries[i][0] >= NFMARKIN_BASE)
                {
		  int delta = ct_entries[i][0] - NFMARK_DELTA;
		  if (delta == ct_entries_export[j][0])
                    {
		      ct_entries_export[j][1] += ct_entries[i][6];
		      ct_entries_export[j][2] += ct_entries[i][5];
                      goto next;
                    }
                }
	      if (ct_entries[i][0] == ct_entries_export[j][0])
                {
		  ct_entries_export[j][1] += ct_entries[i][5];
		  ct_entries_export[j][2] += ct_entries[i][6];
                  goto next;
                }
            }
	  if (ct_entries[i][0] >= NFMARKIN_BASE)
            {
	      ct_entries_export[j][0] = ct_entries[i][0] - NFMARK_DELTA;
	      ct_entries_export[j][1] = ct_entries[i][6];
	      ct_entries_export[j][2] = ct_entries[i][5];
            }
          else
            {
	      ct_entries_export[j][0] = ct_entries[i][0];
	      ct_entries_export[j][1] = ct_entries[i][5];
	      ct_entries_export[j][2] = ct_entries[i][6];
            }
next:
        }
#ifdef DEBUG
      for (i = 0; ct_entries_export[i][0] != 0; ++i)
        {
          //printf("rulesexp: %d: >%d <%d \n", rulesexp[i][0], rulesexp[i][1], rulesexp[i][2]);
        }
#endif

      mymsg msg;
      msg.type = 1;
      memcpy (msg.ct_entries_export, ct_entries_export, sizeof(msg.ct_entries_export));

      msgctl(mqd_d2ftraffic, IPC_STAT, msgqid_d2ftraffic);
      //don't send if there is already some data down the queue that frontend hasn't yet received
      if (msgqid_d2ftraffic->msg_qnum == 0)
        {
	  if ( msgsnd ( mqd_d2ftraffic, &msg, sizeof ( msg.ct_entries_export ), IPC_NOWAIT ) == -1 )
            {
              M_PRINTF (MLOG_INFO, "msgsnd: %d %s,%s,%d\n",errno, strerror ( errno ), __FILE__, __LINE__ );
            }
        }
      sleep(1);
    }
}

void * trafficdestroythread( void *ptr)
{
  struct nfct_handle *traffic_handle;
  if ((traffic_handle = nfct_open(NFNL_SUBSYS_CTNETLINK, NF_NETLINK_CONNTRACK_DESTROY)) == NULL)
    {
      perror("nfct_open");
    }
  if ((nfct_callback_register(traffic_handle, NFCT_T_ALL, traffic_destroyfilter_callback, NULL) == -1))
    {
      perror("cb_reg");
    }
  int res = 0;
  res = nfct_catch(traffic_handle); //the thread should block here
}


void* ct_delthread ( void* ptr )
{
  u_int8_t family = AF_INET; //used by conntrack
  struct nfct_handle *deletemark_handle;
  if ((deletemark_handle = nfct_open(NFNL_SUBSYS_CTNETLINK, 0)) == NULL)
    {
      perror("nfct_open");
    }
  if ((nfct_callback_register(deletemark_handle, NFCT_T_ALL, delete_mark, NULL) == -1))
    {
      perror("cb_reg");
    }

  while(1)
    {
      pthread_mutex_lock(&condvar_mutex);
      while(predicate == FALSE)
        {
          pthread_cond_wait(&condvar, &condvar_mutex);
        }
      predicate = FALSE;
      pthread_mutex_unlock(&condvar_mutex);
      if (nfct_query(deletemark_handle, NFCT_Q_DUMP, &family) == -1)
        {
          perror("query-DELETE");
        }
    }
}



int setmark_out (enum nf_conntrack_msg_type type, struct nf_conntrack *mct,void *data)
{
  nfct_set_attr_u32(mct, ATTR_MARK, nfmark_to_set_out);
  nfct_query(setmark_handle_out, NFCT_Q_UPDATE, mct);
  return NFCT_CB_CONTINUE;
}

int setmark_in (enum nf_conntrack_msg_type type, struct nf_conntrack *mct,void *data)
{
  nfmark_to_set_in += NFMARK_DELTA;
  nfct_set_attr_u32(mct, ATTR_MARK, nfmark_to_set_in);
  nfct_query(setmark_handle_in, NFCT_Q_UPDATE, mct);
  return NFCT_CB_CONTINUE;
}

void  init_conntrack()
{
  if ((ct_out_tcp = nfct_new()) == NULL)
    {
      perror("new");
    }
  if ((ct_out_udp = nfct_new()) == NULL)
    {
      perror("new");
    }
  if ((ct_out_icmp = nfct_new()) == NULL)
    {
      perror("new");
    }
  if ((ct_in = nfct_new()) == NULL)
    {
      perror("new");
    }
  if ((dummy_handle = nfct_open(NFNL_SUBSYS_CTNETLINK, 0)) == NULL)
    {
      perror("nfct_open");
    }
  if ((setmark_handle_out = nfct_open(NFNL_SUBSYS_CTNETLINK, 0)) == NULL)
    {
      perror("nfct_open");
    }
  if ((setmark_handle_in = nfct_open(NFNL_SUBSYS_CTNETLINK, 0)) == NULL)
    {
      perror("nfct_open");
    }
  if ((nfct_callback_register(setmark_handle_out, NFCT_T_ALL, setmark_out, NULL) == -1))
    {
      perror("cb_reg");
    }
  if ((nfct_callback_register(setmark_handle_in, NFCT_T_ALL, setmark_in, NULL) == -1))
    {
      perror("cb_reg");
    }
  return;
}

void child_close_nfqueue()
{
  if (nfq_close( globalh_out_tcp ))
    {
      M_PRINTF ( MLOG_INFO,"error in nfq_close\n" );
    }
  else
    M_PRINTF ( MLOG_DEBUG, "Done closing nfqueue\n" );
  if (nfq_close( globalh_out_udp ))
    {
      M_PRINTF ( MLOG_INFO,"error in nfq_close\n" );
    }
  else
    M_PRINTF ( MLOG_DEBUG, "Done closing nfqueue\n" );
  return;
}

void fe_active_flag_set ( int boolean )
{
  pthread_mutex_lock ( &fe_active_flag_mutex );
  fe_active_flag = boolean;
  pthread_mutex_unlock ( &fe_active_flag_mutex );
}

int fe_active_flag_get()
{
  int temp;
  pthread_mutex_lock ( &fe_active_flag_mutex );
  temp = fe_active_flag;
  pthread_mutex_unlock ( &fe_active_flag_mutex );
  return temp;
}

void die()
{
  exit ( 0 );
}

int  m_printf_stdout ( int loglevel, char * logstring )
{
  switch ( loglevel )
    {
    case MLOG_INFO:
      // check if INFO logging enabled
      if ( !* ( log_info->ival ) ) return 0;
      printf ( "%s", logstring );
      return 0;
    case MLOG_TRAFFIC:
      // check if  logging enabled
      if ( !* ( log_traffic->ival ) ) return 0;
      printf ( "%s", logstring );
      return 0;
    case MLOG_DEBUG:
      // check if  logging enabled
      if ( !* ( log_debug->ival ) ) return 0;
      printf ( "%s", logstring );
      return 0;
    case MLOG_DEBUG2:
#ifdef DEBUG2
      // check if  logging enabled
      if ( !* ( log_debug->ival ) ) return 0;
      printf ( "%s", logstring );
#endif
      return 0;
    case MLOG_DEBUG3:
#ifdef DEBUG3
      // check if  logging enabled
      if ( !* ( log_debug->ival ) ) return 0;
      printf ( "%s", logstring );
#endif
      return 0;
    case MLOG_ALERT: //Alerts get logged unconditionally to all log channels
      printf ( "ALERT: " );
      printf ( "%s", logstring );
      return 0;
    }
}

//technically vfprintf followed by fsync should be enough, but for some reason on my system it can take more than 1 minute before data gets actually written to disk. So until the mystery of such a huge delay is solved, we use write() so data gets written to dist immediately
int m_printf_file ( int loglevel, char * logstring )
{
  switch ( loglevel )
    {
    case MLOG_INFO:
      // check if INFO logging enabled
      if ( !* ( log_info->ival ) ) return 0;
      write ( fileno ( fileloginfo_stream ), logstring, strlen ( logstring ) );
      return 0;
    case MLOG_TRAFFIC:
      if ( !* ( log_traffic->ival ) ) return 0;
      write ( fileno ( filelogtraffic_stream ), logstring, strlen ( logstring ) );
      return 0;
    case MLOG_DEBUG:
      if ( !* ( log_debug->ival ) ) return 0;
      write ( fileno ( filelogdebug_stream ), logstring, strlen ( logstring ) );
      return 0;
    case MLOG_ALERT: //Alerts get logged unconditionally to all log channels
      write ( fileno ( filelogdebug_stream ), "ALERT: ", strlen ( logstring ) );
      return 0;
    }
}

#ifndef WITHOUT_SYSLOG
int m_printf_syslog ( int loglevel, char * logstring)
{
  switch ( loglevel )
    {
    case MLOG_INFO:
      // check if INFO logging enabled
      if ( !* ( log_info->ival ) ) return 0;
      syslog ( LOG_INFO, logstring );
      return 0;
    case MLOG_TRAFFIC:
      if ( !* ( log_traffic->ival ) ) return 0;
      syslog ( LOG_INFO, logstring );
      return 0;
    case MLOG_DEBUG:
      if ( !* ( log_debug->ival ) ) return 0;
      syslog ( LOG_INFO, logstring );
      return 0;
    case MLOG_ALERT: //Alerts get logget unconditionally to all log channels
      syslog ( LOG_INFO, "ALERT: " );
      syslog ( LOG_INFO, logstring );
      return 0;
    }
}
#endif

unsigned long long starttimeGet ( int mypid )
{
  char pidstring[8];
  char path[32] = "/proc/";
  sprintf ( pidstring, "%d", mypid );
  strcat ( path, pidstring );
  strcat ( path, "/stat" );

  unsigned long long starttime;
  FILE *stream;

  if ( ( stream = fopen ( path, "r" ) ) == 0 )
    {
      M_PRINTF ( MLOG_INFO, "fopen: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
      return 1;
    };

  fscanf ( stream, "%*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %llu",
	   &starttime );

  fclose ( stream );
  return starttime;
}

//make a copy of dlist for future iterations. We don't iterate through dlist itself because that would require to lock a mutex for too long
dlist * dlist_copy()
{
  pthread_mutex_lock ( &dlist_mutex );
  dlist* del;
  dlist *temp = first_rule->next;
  dlist *copy_temp = copy_first;
  while ( temp != 0 )
    {

      if ( !copy_temp->next )
        {
          //grow copy of dlist
          if ( ( copy_temp->next = malloc ( sizeof ( dlist ) ) ) == NULL )
            {
              M_PRINTF ( MLOG_INFO, "malloc: %s in %s:%d\n", strerror ( errno ), __FILE__, __LINE__ );
              die();
            }
          copy_temp->next->prev = copy_temp;
          copy_temp->next->next = NULL;
        }
      copy_temp = copy_temp->next;
      copy_temp->path[0] = 0;
      strcpy ( copy_temp->path, temp->path );
      strcpy ( copy_temp->perms, temp->perms );
      strcpy ( copy_temp->pid, temp->pid );
      copy_temp->is_active = temp->is_active;
      copy_temp->nfmark_out = temp->nfmark_out;

      temp = temp->next;
    }
  pthread_mutex_unlock ( &dlist_mutex );
  //lets see if copy dlist needs to be shrunk
  copy_temp = copy_temp->next;
  while ( copy_temp != 0 )
    {
      del = copy_temp;
      //prev element should point not to us but to the next element
      copy_temp->prev->next = copy_temp->next;
      copy_temp = copy_temp->next;
      free ( del );
    }
  return copy_first;
}

//Add new element to dlist
int dlist_add ( char *path, char *pid, char *perms, mbool active, char *sha, unsigned long long stime, off_t size, int nfmark, unsigned char first_instance)
{
  static int rule_ordinal_count = 0;
  int retnfmark;

  pthread_mutex_lock ( &dlist_mutex );
  dlist *temp = first_rule;

  if (!strcmp(path, KERNEL_PROCESS))  //make sure it is not a duplicate from the user
    {
      while ( temp->next != NULL ) //find a KERNEL PROCESS entry
        {
          temp = temp->next;
          if (strcmp(temp->path, KERNEL_PROCESS)) continue;
          if (!strcmp(temp->pid, pid))  //same IP, quit
            {
              pthread_mutex_unlock ( &dlist_mutex );
              return;
            }
        }
    }
  temp = first_rule;
  //find the last element in dlist i.e. the one that has .next == NULL...
  while ( temp->next != NULL )
    {
      temp = temp->next;
    }
  //last element's .next should point now to our newly created one
  if ( ( temp->next = malloc ( sizeof ( dlist ) ) ) == NULL )
    {
      M_PRINTF ( MLOG_INFO, "malloc: %s in %s:%d\n", strerror ( errno ), __FILE__, __LINE__ );
      die();
    }
  // new element's prev field should point to the former last element...
  temp->next->prev = temp;
  // point temp to the newly added element...
  temp = temp->next;
  //initialize fields
  temp->next = NULL;
  strcpy ( temp->path, path );
  strcpy ( temp->pid, pid );
  strcpy ( temp->perms, perms );
  temp->is_active = active;
  temp->stime = stime;
  assert(sha != NULL);
  memcpy ( temp->sha, sha, DIGEST_SIZE );
  temp->exesize = size;
  if (nfmark == 0)
    {
      temp->nfmark_in = NFMARKIN_BASE + nfmark_count;
      retnfmark = temp->nfmark_out = NFMARKOUT_BASE +  nfmark_count;
      nfmark_count++;
    }
  else // nfmark > 0 => assign parent's nfmark
    {
      //either nfmark is for in or out traffic
      if (nfmark >= NFMARKIN_BASE)
        {
          temp->nfmark_in = nfmark;
          retnfmark = temp->nfmark_out = nfmark - NFMARK_DELTA;
        }
      else
        {
          retnfmark = temp->nfmark_out = nfmark;
          temp->nfmark_in = nfmark + NFMARK_DELTA;
        }
      nfmark_count++;
    }
  temp->ordinal_number = ++rule_ordinal_count;
  temp->first_instance = first_instance; //obsolete member,can be purged
  if (temp->is_active && strcmp(temp->path, KERNEL_PROCESS))
    {
      strcpy(temp->pidfdpath,"/proc/");
      strcat(temp->pidfdpath, temp->pid);
      strcat(temp->pidfdpath, "/fd/");
      if ((temp->dirstream = opendir ( temp->pidfdpath )) == NULL)
        {
          M_PRINTF ( MLOG_DEBUG, "opendir: %s in %s:%d\n", strerror ( errno ), __FILE__, __LINE__ );
          exit(0);
        }
    }

  //add to cache regardless if pid is active
  //TODO we only need cache for active rules - should socket_cache be allocated/removed dynamically?
//    if (current){
//	pthread_mutex_lock(&cache_mutex);
//	cache_item *cache_temp = first_cache;
//	while (cache_temp->next != NULL){
//	    cache_temp =cache_temp->next;
//	}
//	if ((cache_temp->next = malloc (sizeof (cache_item))) == NULL){perror("malloc");}
//	cache_temp->next->prev = cache_temp;
//	//make cache temp point to the newly created baby
//	cache_temp = cache_temp->next;
//	strcpy(cache_temp->path, temp->path);
//	strcpy(cache_temp->pid, temp->pid);
//	cache_temp->sockets[0][0] = MAGIC_NO;
//	pthread_mutex_unlock(&cache_mutex);
//    }
  if ((temp->sockets_cache = (int*)malloc(sizeof(int)*MAX_CACHE)) == NULL)
    {
      perror("malloc");
    }
  *temp->sockets_cache = MAGIC_NO;

  pthread_mutex_unlock ( &dlist_mutex );
  return retnfmark;
}

//Remove element from dlist...
void dlist_del ( char *path, char *pid )
{
  mbool was_active;
  pthread_mutex_lock ( &dlist_mutex );
  dlist *temp = first_rule->next;
  while ( temp != NULL )
    {
      if ( !strcmp ( temp->path, path ) && !strcmp ( temp->pid, pid ) )
        {
          //free cache entry first
          free(temp->sockets_cache);
          //free dirstream
          closedir (temp->dirstream);
          //remove the item
          temp->prev->next = temp->next;
          if ( temp->next != NULL )
            temp->next->prev = temp->prev;
          nfmark_to_delete_in = temp->nfmark_in;
          nfmark_to_delete_out = temp->nfmark_out;
          was_active = temp->is_active;
          free ( temp );

          //remove tracking for this app's active connection only if this app was active
          if (was_active)
            {
              pthread_mutex_lock(&condvar_mutex);
              predicate = TRUE;
              pthread_mutex_unlock(&condvar_mutex);
              pthread_cond_signal(&condvar);
            }
          pthread_mutex_unlock ( &dlist_mutex );
          fe_list();
          return;
        }
      temp = temp->next;
    }
  M_PRINTF ( MLOG_INFO, "%s with PID %s was not found in dlist\n", path, pid );
  pthread_mutex_unlock ( &dlist_mutex );
}

int parsecache_in(int socket, char *path, char *pid)
{
  int i;
  int retval;
  dlist *temp;
  pthread_mutex_lock(&dlist_mutex);
  temp = first_rule;
  while (temp->next != NULL)
    {
      temp = temp->next;
      if(!temp->is_active) continue;
      i = 0;
      while (temp->sockets_cache[i] != MAGIC_NO)
        {
          if (i >= MAX_CACHE-1) break;
          if (temp->sockets_cache[i] == socket)  //found match
            {
              if (!strcmp(temp->perms, ALLOW_ONCE) || !strcmp(temp->perms, ALLOW_ALWAYS)) retval = CACHE_TRIGGERED_ALLOW;
              else retval = CACHE_TRIGGERED_DENY;
              strcpy(path, temp->path);
              strcpy(pid, temp->pid);
	      if (temp->stime != starttimeGet(atoi (temp->pid))) {return SPOOFED_PID;}
              nfmark_to_set_in = temp->nfmark_out;
              rule_ordinal_in = temp->ordinal_number;
              pthread_mutex_unlock(&dlist_mutex);
              return retval;
            }
          i++;
        }
    }
  pthread_mutex_unlock(&dlist_mutex);
  return GOTO_NEXT_STEP;
}

int parsecache_out(int socket, char *path, char *pid)
{
  int i;
  int retval;
  dlist *temp;
  pthread_mutex_lock(&dlist_mutex);
  temp = first_rule;
  while (temp->next != NULL)
    {
      temp = temp->next;
      if(!temp->is_active) continue;
      i = 0;
      while (temp->sockets_cache[i] != MAGIC_NO)
        {
          if (i >= MAX_CACHE-1) break;
          if (temp->sockets_cache[i] == socket)  //found match
            {
              if (!strcmp(temp->perms, ALLOW_ONCE) || !strcmp(temp->perms, ALLOW_ALWAYS)) retval = CACHE_TRIGGERED_ALLOW;
	      else {retval = CACHE_TRIGGERED_DENY;}
              strcpy(path, temp->path);
              strcpy(pid, temp->pid);
	      if (temp->stime != starttimeGet(atoi (temp->pid))) {return SPOOFED_PID;}
              nfmark_to_set_out = temp->nfmark_out;
              rule_ordinal_out = temp->ordinal_number;
              pthread_mutex_unlock(&dlist_mutex);
              return retval;
            }
          i++;
        }
    }
  pthread_mutex_unlock(&dlist_mutex);
  return GOTO_NEXT_STEP;
}

//scan active /proc/pid entries (ignoring kernel processes) and build a correlation of PIDs to sockets
void* cachebuildthread ( void *pid )
{
  DIR *mdir;
  struct dirent *m_dirent;
  int proc_pid_fd_pathlen;
  char proc_pid_fd_path[32], proc_pid_exe[32];
  struct timespec refresh_timer,dummy;
  refresh_timer.tv_sec=0;
  refresh_timer.tv_nsec=1000000000/4;
  dlist *rule;
  struct timeval time;
  int i, delta;

  while(1)
    {
      nanosleep(&refresh_timer, &dummy);

      gettimeofday(&time, NULL);
      pthread_mutex_lock(&lastpacket_mutex);
      delta = time.tv_sec - lastpacket.tv_sec;
      pthread_mutex_unlock(&lastpacket_mutex);
      if (delta > 1)
        {
          continue;
        }
      pthread_mutex_lock(&dlist_mutex);
      rule = first_rule;
      while (rule->next != NULL)
        {
	  rule = rule->next;
	  if (!rule->is_active || !strcmp(rule->path, KERNEL_PROCESS)) continue;
	  proc_pid_fd_pathlen = strlen(rule->pidfdpath);
	  strcpy(proc_pid_fd_path, rule->pidfdpath);
	  rewinddir(rule->dirstream);
          i = 0;
          errno=0;
	  while (m_dirent = readdir ( rule->dirstream ))
            {
	      proc_pid_fd_path[proc_pid_fd_pathlen]=0;
	      strcat(proc_pid_fd_path, m_dirent->d_name);
	      memset (proc_pid_exe, 0 , sizeof(proc_pid_exe));
	      if (readlink ( proc_pid_fd_path, proc_pid_exe, SOCKETBUFSIZE ) == -1)  //not a symlink but . or ..
                {
                  errno=0;
		  continue;
                }
	      if (proc_pid_exe[7] != '[') continue; //not a socket
              char *end;
	      end = strrchr(&proc_pid_exe[8],']'); //put 0 instead of ]
              *end = 0;
	      rule->sockets_cache[i] = atoi(&proc_pid_exe[8]);
              i++;
            }
	  rule->sockets_cache[i] = MAGIC_NO;
          if (errno==0)
            {
              continue; //readdir reached EOF, thus errno hasn't changed from 0
            }
          //else
	  M_PRINTF ( MLOG_DEBUG, "readdir: %s in %s:%d\n", strerror ( errno ), __FILE__, __LINE__ );
	}
      pthread_mutex_unlock(&dlist_mutex);
    }
}

void* nfqoutudpthread ( void *ptr )
{
  ptr = 0;
  //endless loop of receiving packets and calling a handler on each packet
  int rv;
  char buf[4096] __attribute__ ( ( aligned ) );
  while ( ( rv = recv ( nfqfd_udp, buf, sizeof ( buf ), 0 ) ) && rv >= 0 )
    {
      nfq_handle_packet ( globalh_out_udp, buf, rv );
    }


}

void* nfqoutrestthread ( void *ptr )
{
  ptr = 0;
  //endless loop of receiving packets and calling a handler on each packet
  int rv;
  char buf[4096] __attribute__ ( ( aligned ) );
  while ( ( rv = recv ( nfqfd_rest, buf, sizeof ( buf ), 0 ) ) && rv >= 0 )
    {
      nfq_handle_packet ( globalh_out_rest, buf, rv );
    }


}


void* nfqinputthread ( void *ptr )
{
  ptr = 0;
//endless loop of receiving packets and calling a handler on each packet
  int rv;
  char buf[4096] __attribute__ ( ( aligned ) );
  while ( ( rv = recv ( nfqfd_input, buf, sizeof ( buf ), 0 ) ) && rv >= 0 )
    {
      nfq_handle_packet ( globalh_in, buf, rv );
    }
}

void* rulesdumpthread ( void *ptr )
{
  ptr = 0;
  mkfifo ( "/tmp/lpfwrulesdump.fifo", 0777 );
  int fifofd;
  if ( ( fifofd = open ( "/tmp/lpfwrulesdump.fifo", O_RDWR ) ) == -1 )
    {
      perror ( "open fifo" );
    }

  char buf;
  int retval;
  while ( 1 )
    {
      if ( ( retval = read ( fifofd, &buf, 1 ) ) > 0 ) goto dump;
      sleep ( 1 );
      continue;
dump:
      ;
      FILE *fd;
      if ( ( fd = fopen ( "/tmp/lpfwrulesdump.txt", "w" ) ) == NULL )
        {
          perror ( "open text" );
          continue;
        }

      dlist *temp;
      pthread_mutex_lock ( &dlist_mutex );
      temp = first_rule->next;
      char nfmarkstr[16];
      while ( temp != NULL )
        {

          fputs ( temp->path, fd );
          fputc ( '\n', fd );
          fputs ( temp->pid, fd );
          fputc ( '\n', fd );
          fputs ( temp->perms, fd );
          fputc ( '\n', fd );
          fputc ( temp->is_active, fd );
          fputc ( '\n', fd );
          sprintf(nfmarkstr, "%d", temp->nfmark_out);
          fputs (nfmarkstr, fd);
          fputc ( '\n', fd );
          fputc ( '\n', fd );

          temp = temp->next;
        }
      pthread_mutex_unlock ( &dlist_mutex );
      fclose ( fd );
    }
}

//scan procfs and remove/mark inactive in dlist those apps that are no longer running
void* refreshthread ( void* ptr )
{
  dlist *rule, *prev, *temp_rule;
  ptr = 0;     //to prevent gcc warnings of unused variable
  char proc_pid_exe[32] = "/proc/";
  char exe_path[PATHSIZE];

  while ( 1 )
    {
      loop:
      pthread_mutex_lock ( &dlist_mutex );
      rule = first_rule;
      while ( rule->next != NULL )
        {
	  rule = rule->next;
	  //kernel processes don't have /proc/PID entries
	  if (!rule->is_active || !strcmp(rule->path, KERNEL_PROCESS)) continue;
	  proc_pid_exe[6]=0;
	  strcat ( proc_pid_exe, rule->pid );
	  strcat ( proc_pid_exe, "/exe" );
	  memset ( exe_path, 0, PATHSIZE );
	  //readlink doesn't fail if PID is running
	  if ( readlink ( proc_pid_exe, exe_path, PATHSIZE ) != -1 )
            {
	      continue;
	    }
	  else
	    {
              M_PRINTF ( MLOG_DEBUG, "readlink: %s in %s:%d\n", strerror ( errno ), __FILE__, __LINE__ );

	      //Only delete *ALWAYS rule if there is at least one more rule in dlist with the same PATH
	      //If the rule is the only one in dlist with such PATH, simply toggle its_active flag
	      if ( !strcmp ( rule->perms, ALLOW_ALWAYS ) || !strcmp ( rule->perms, DENY_ALWAYS ) )
                {
		  temp_rule = first_rule->next;
		  while ( temp_rule != NULL ) //scan the whole dlist again
                    {
		      if ( !strcmp ( temp_rule->path, rule->path ) && ( temp_rule != rule ) ) //Make sure we don't find our own rule :)
                        {
			  // TODO dlist_del is redundant we could keep a pointer to self in each dlist element and simply free(temp->self)
			  // is there really a need for dlistdel? apart from the fact that frontend deletes by path :(
			  char path[PATHSIZE];
			  char pid[PIDLENGTH];
			  strcpy (path, rule->path);
			  strcpy (pid, rule->pid);
			  pthread_mutex_unlock ( &dlist_mutex );
			  dlist_del ( path, pid );
			  goto loop;
			}
		      temp_rule=temp_rule->next;
		      continue;
		    }
		  //no PATH match
		  strcpy ( rule->pid, "0" );
		  rule->is_active = FALSE;
		  //nfmarks will be used by the next instance of app
		  rule->nfmark_in = NFMARKIN_BASE + nfmark_count;
		  rule->nfmark_out = NFMARKOUT_BASE +  nfmark_count;
                  nfmark_count++;
                  fe_list();
		  continue;
		}
	    }
	}
      pthread_mutex_unlock ( &dlist_mutex );
      sleep ( REFRESH_INTERVAL );
    }
}

//Read RULESFILE into dlist
void rules_load()
{
  FILE *stream;
  char path[PATHSIZE];
  char laststring[PATHSIZE];
  char line[PATHSIZE];
  char *result;
  char perms[PERMSLENGTH];
  char ip[INET_ADDRSTRLEN+1];//plus trailing /n and 0
  unsigned long sizeint;
  char sizestring[16];
  char shastring[DIGEST_SIZE * 2 + 2];
  struct stat m_stat;
  unsigned char digest[DIGEST_SIZE];
  unsigned char hexchar[3] = "";


  if ( stat ( rules_file->filename[0], &m_stat ) == -1 )
    {
      M_PRINTF ( MLOG_INFO, "CONFIG doesnt exist..creating" );
      if ( ( stream = fopen ( rules_file->filename[0], "w+" ) ) == NULL )
        {
          M_PRINTF ( MLOG_INFO, "fopen: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
          return;
        }
    }
  if ( ( stream = fopen ( rules_file->filename[0], "r" ) ) == NULL )
    {
      M_PRINTF ( MLOG_INFO, "fopen RULESFILE: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
      return;
    }

  while ( 1 )
    {
      //fgets reads <newline> into the string and terminates with /0
      if ( fgets ( path, PATHSIZE, stream ) == 0 ) break;
      path[strlen ( path ) - 1] = 0; //remove newline
      if (!strcmp(path, KERNEL_PROCESS)) //separate treatment for kernel process
        {
          if ( fgets ( ip, INET_ADDRSTRLEN+1, stream ) == 0 ) break; //read IP address
          ip[strlen ( ip ) - 1] = 0;
          if ( fgets ( perms, PERMSLENGTH, stream ) == 0 ) break;
          perms[strlen ( perms ) - 1] = 0;
          if ( fgets ( laststring, PATHSIZE, stream ) == 0 ) break; //read last newline
          dlist_add( path, ip , perms, FALSE, digest, 0, 0, 0, TRUE);
          continue;
        }
      if ( fgets ( perms, PERMSLENGTH, stream ) == 0 ) break;
      perms[strlen ( perms ) - 1] = 0;
      if ( fgets ( sizestring, 16, stream ) == 0 ) break;
      sizestring[strlen ( sizestring ) - 1] = 0;
      sizeint = atol ( sizestring );
      if ( fgets ( shastring, DIGEST_SIZE * 2 + 2, stream ) == 0 ) break;

      memset ( digest, 0, DIGEST_SIZE );
      int i = 0;
      for ( i; i < DIGEST_SIZE; ++i )
        {
          hexchar[0] = shastring[i * 2];
          hexchar[1] = shastring[i * 2 + 1];
          sscanf ( hexchar, "%x", ( unsigned int * ) &digest[i] );
        }
      if ( fgets ( laststring, PATHSIZE, stream ) == 0 ) break; //read last newline

      dlist_add ( path, "0", perms, FALSE, digest, 2, ( off_t ) sizeint, 0, TRUE);
    }
  if ( fclose ( stream ) == EOF )
    {
      M_PRINTF ( MLOG_INFO, "fclose: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
    }
}

//Write to RULESFILE only entries that have ALLOW/DENY_ALWAYS permissions
void rulesfileWrite()
{
  FILE *fd;
  struct stat m_stat;
  FILE *stream;
  int i;
  unsigned char shastring[DIGEST_SIZE * 2 + 1] = "";
  unsigned char shachar[3] = "";
  char sizestring[16];

  //rewrite/create the file regardless of whether it already exists
  if ( ( fd = fopen ( rules_file->filename[0], "w" ) ) == NULL )
    {
      M_PRINTF ( MLOG_INFO, "open: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
      return;
    }

  pthread_mutex_lock ( &dlist_mutex );
  dlist* temp = first_rule->next;
  dlist* temp2;

loop:
  while ( temp != NULL )
    {
      if ( ( !strcmp ( temp->perms, ALLOW_ALWAYS ) ) || ( !strcmp ( temp->perms, DENY_ALWAYS ) ) )
        {
          //now check if same path with perms ALWAYS wasn't present in previous rules and if not then add  this rules index to dlist
          //ignore inkernel rules, though
          if (!strcmp(temp->path, KERNEL_PROCESS)) goto inkernel;
          temp2 = temp->prev;
          while ( temp2 != NULL )
            {
              if ( !strcmp ( temp2->path, temp->path ) )
                {
                  if ( ( !strcmp ( temp2->perms, ALLOW_ALWAYS ) ) || ( !strcmp ( temp2->perms, DENY_ALWAYS ) ) )
                    {
                      temp = temp->next;
                      goto loop;
                    }
                }
              temp2 = temp2->prev;
            }
inkernel:
          if (!strcmp(temp->path, KERNEL_PROCESS))
            {
              if ( fputs ( temp->path, fd ) == EOF )
                {
                  M_PRINTF ( MLOG_INFO, "fputs: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
                }
              fputc ( '\n', fd );
              if ( fputs ( temp->pid, fd ) == EOF )
                {
                  M_PRINTF ( MLOG_INFO, "fputs: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
                }
              fputc ( '\n', fd );
              if ( fputs ( temp->perms, fd ) == EOF )
                {
                  M_PRINTF ( MLOG_INFO, "fputs: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
                }
              fputc ( '\n', fd );
              fputc ( '\n', fd );
              fsync ( fileno ( fd ) );
              temp = temp->next;
              continue;
            }

          if ( fputs ( temp->path, fd ) == EOF )
            {
              M_PRINTF ( MLOG_INFO, "fputs: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
            }
          fputc ( '\n', fd );
          fputs ( temp->perms, fd );
          fputc ( '\n', fd );
          sprintf ( sizestring, "%ld", ( long ) temp->exesize );
          fputs ( sizestring, fd );
          fputc ( '\n', fd );

          shastring[0] = 0;
          for ( i = 0; i < DIGEST_SIZE; ++i )
            {
              //pad single digits with a leading zero
              sprintf ( shachar, "%02x", temp->sha[i] );
              //The next line causes gdb to go nutty
              strcat ( shastring, shachar );
            }
          shastring[DIGEST_SIZE * 2] = 0;

          fputs ( shastring, fd );
          fputc ( '\n', fd );
          fputc ( '\n', fd );


          //don't proceed until data is written to disk
          fsync ( fileno ( fd ) );
        }
      temp = temp->next;
    }
  pthread_mutex_unlock ( &dlist_mutex );
  fclose ( fd );
}

//if another rule with this path is in dlist already, check if our process is fork()ed or a new instance
int path_find_in_dlist ( int *nfmark_to_set, char *path, char *pid, unsigned long long *stime)
{
  pthread_mutex_lock ( &dlist_mutex );
  dlist* temp = first_rule->next;
  while ( temp != NULL )
    {
      if ( !strcmp ( temp->path, path ) )
        {
          if (!temp->is_active) //rule in dlist has been added from rulesfile and hasn't seen traffic yet.
            //Exesize and shasum our process once
            {
              struct stat exestat;
              if ( stat ( path, &exestat ) == -1 )
                {
                  M_PRINTF ( MLOG_INFO, "stat: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
                  pthread_mutex_unlock ( &dlist_mutex );
                  return DROP;
                }
              if ( temp->exesize != exestat.st_size )
                {
                  M_PRINTF ( MLOG_INFO, "Exe sizes dont match.  %s in %s, %d\n", path, __FILE__, __LINE__ );
                  pthread_mutex_unlock ( &dlist_mutex );
                  return EXESIZE_DONT_MATCH;
                }

              //TODO mutex will be held for way too long here, find a way to decrease time
              char sha[DIGEST_SIZE];
              FILE *stream;
              if ((stream = fopen ( path, "r" )) == NULL)
                {
                  M_PRINTF ( MLOG_INFO, "fopen: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
                  pthread_mutex_unlock ( &dlist_mutex );
                  return DROP;
                }
              sha512_stream ( stream, ( void * ) sha );
              fclose ( stream );
              if ( memcmp ( temp->sha, sha, DIGEST_SIZE ) )
                {
                  M_PRINTF ( MLOG_INFO, "Shasums dont match. Impersonation attempt detected by %s in %s, %d\n", temp->path, __FILE__, __LINE__ );
                  pthread_mutex_unlock ( &dlist_mutex );
                  return SHA_DONT_MATCH;
                }

              strcpy ( temp->pid, pid ); //update entry's PID and inode
              temp->is_active = TRUE;
              temp->stime = *stime;
              strcpy(temp->pidfdpath,"/proc/");
              strcat(temp->pidfdpath, temp->pid);
              strcat(temp->pidfdpath, "/fd/");
              if ((temp->dirstream = opendir ( temp->pidfdpath )) == NULL)
                {
                  M_PRINTF ( MLOG_DEBUG, "opendir: %s in %s:%d\n", strerror ( errno ), __FILE__, __LINE__ );
                  exit(0);
                }

              int retval;
              if ( !strcmp ( temp->perms, ALLOW_ONCE ) || !strcmp ( temp->perms, ALLOW_ALWAYS ) )
                {
                  retval = PATH_FOUND_IN_DLIST_ALLOW;
                }
              else if ( !strcmp ( temp->perms, DENY_ONCE ) || !strcmp ( temp->perms, DENY_ALWAYS ) )
                {
                  retval = PATH_FOUND_IN_DLIST_DENY;
                }
              else
                {
                  M_PRINTF ( MLOG_INFO, "should never get here. Please report %s,%d\n", __FILE__, __LINE__ );
                }
              pthread_mutex_unlock ( &dlist_mutex );
              //notify fe that the rule has an active PID now
              fe_list();
              return retval;
            }
          else if ( temp->is_active )
            {

//determine if this is new instance or fork()d child
// --------------------------
// Here is how to determine if a process with the same PATH is either a new instance or a fork()ed process.
//
// 1. Get new process's(NP) PPID.
// 2. Is there an entry in dlist with the same PATH as NP AND PID == PPID?
// 3. If no then we have a new instance, go to step A1
// 4. If yes, we have a fork()ed process, go to step B1
//
// A1. Are there any entries in dlist with the same PATH as NP AND *ALWAYS perms? If yes, then create new entry in dlist copy parent's perms and all other attributer over to NP and continue;
// A2. If No, i.e. there either aren't any entries in dlist with the same PATH as NP OR there are entries with the same path as NP AND *ONCE perms, then query user.
//
// B1. Create new entry in dlist copy parent's perms and all other attributes over to NP and continue.
// --------------------------

//get new process's PPID
              FILE *stream;
              char proc_stat_path[32] = "/proc/";
              strcat ( proc_stat_path, pid );
              strcat ( proc_stat_path, "/stat" );

              if ( ( stream = fopen ( proc_stat_path, "r" ) ) == 0 )
                {
                  M_PRINTF ( MLOG_INFO, "fopen: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
                  return PROCFS_ERROR;
                };

              char dummy1[32];
              char dummy2[32];
              char dummy3[32];
              char ppid[16];

              fscanf ( stream, "%s %s %s %s", dummy1, dummy2, dummy3, ppid );

//first copy parent's attributes

              char tempperms[PERMSLENGTH];
              char tempsha [DIGEST_SIZE];
              char temppid [PIDLENGTH-1];
              off_t parent_size = temp->exesize;
              unsigned long long saved_stime = temp->stime;
              strcpy ( tempperms, temp->perms );
              strcpy ( temppid, temp->pid );
              memcpy ( tempsha, temp->sha, DIGEST_SIZE );
              int tempnfmark;
              tempnfmark = temp->nfmark_out;

//is it a fork()ed child? the "parent" above may not be the actual parent of this fork, e.g. there may be
//two or three instances of an app running aka three "parents". We have to rescan dlist to ascertain

	      dlist * temp = first_rule->next;
              while ( temp != NULL )
                {
                  if ( !strcmp ( temp->path, path ) && !strcmp ( temp->pid, ppid ) ) //we have a fork()ed child
                    {
                      int retval;
                      if ( !strcmp ( temp->perms, ALLOW_ALWAYS ) || !strcmp ( temp->perms, ALLOW_ONCE ) )
                        {

                          retval = FORKED_CHILD_ALLOW;

                        }
                      else if ( !strcmp ( temp->perms, DENY_ALWAYS ) || !strcmp ( temp->perms, DENY_ONCE ) )
                        {
                          retval =  FORKED_CHILD_DENY;
                        }

                      char tempperms2[PERMSLENGTH];
                      char tempsha2 [DIGEST_SIZE];
                      off_t parent_size2 = temp->exesize;
                      strcpy ( tempperms2, temp->perms );
                      memcpy ( tempsha2, temp->sha, DIGEST_SIZE );

                      pthread_mutex_unlock ( &dlist_mutex );

                      unsigned long long stime;
                      stime = starttimeGet ( atoi ( pid ) );

                      *nfmark_to_set = dlist_add ( path, pid, tempperms2, TRUE, tempsha2, stime, parent_size2, 0, FALSE );
                      fe_list();
                      return retval;
                    }
                  temp = temp->next;
                }
              pthread_mutex_unlock ( &dlist_mutex );

              //we have a new instance, need to ascertain that app instantiated from unmodified binary


              /*
                          //----------------OPTION 1----------------check shasum. Painful, may take 5 secs each time a user starts
                       //his web browser
                          char sha2[DIGEST_SIZE+1];
                          FILE *stream2;
                          memset(sha2, 0, DIGEST_SIZE+1);
                          stream2 = fopen(path, "r");
                          sha512_stream(stream2, (void *) sha2);
                          fclose(stream2);
                          if (strcmp(tempsha, sha2)){
                              M_PRINTF(MLOG_INFO, "Shasums dont match. Impersonation attempt detected by %s in %s, %d\n", path, __FILE__, __LINE__);
                              return SHA_DONT_MATCH;
                          }
              */

              /*
              //----------OPTION 2 -----------check if parent's /PID/exe doesnt have  " (deleted)" appended
              //The attacker might have deleted the binary and replaced it with his own
              //NB. it is impossible to modify an executable's file on disk while it is running, thus this method is secure (yet it is possible to delete it even while process is running)
              */

              //now make sure parent's exepath doesn't have " (deleted)" on its tail
              char exepath[32] = "/proc/";
              strcat ( exepath, temppid );
              strcat ( exepath, "/exe" );
              char exepathbuf[PATHSIZE];
              memset ( exepathbuf, 0, PATHSIZE );
              readlink ( exepath, exepathbuf, PATHSIZE-1 );

              int ssize;
              ssize = strlen ( exepathbuf );
              if ( !strcmp ( &exepathbuf[ssize-10], " (deleted)" ) )
                {
                  M_PRINTF ( MLOG_ALERT, "Red alert!!!! Executable has been changed...  %s, %s, %d\n",exepath, __FILE__, __LINE__ );
                  return EXE_HAS_BEEN_CHANGED;

                }

              //If exe hasnt been modified/deleted than taking its size is redundant, just use parent's size

              M_PRINTF ( MLOG_DEBUG, "Adding to dlist: %s, %s, %s\n", path, pid, tempperms );

              //See if we need to query user or silently add to dlist
              pthread_mutex_lock ( &dlist_mutex );
	      dlist * temp2 = first_rule->next;

// A1. Are there any entries in dlist with the same PATH as NP AND *ALWAYS perms? If yes, then create new entry in dlist copy parent's perms and all other attributes over to NP and continue;
// A2. If No, i.e. there either aren't any entries in dlist with the same PATH as NP OR there are entries with the same path as NP AND *ONCE perms, then query user.

              while ( temp2 != NULL )
                {
                  if ( !strcmp ( temp2->path, path ) )
                    {
                      if ( !strcmp ( temp2->perms, ALLOW_ALWAYS ) )
                        {
                          pthread_mutex_unlock ( &dlist_mutex );
                          dlist_add ( path, pid, tempperms, TRUE, tempsha, *stime, parent_size, tempnfmark ,FALSE);
                          fe_list();

                          *nfmark_to_set = tempnfmark;
                          return NEW_INSTANCE_ALLOW;
                        }
                      else if ( !strcmp ( temp2->perms, DENY_ALWAYS ) )
                        {
                          pthread_mutex_unlock ( &dlist_mutex );
                          dlist_add ( path, pid, tempperms, TRUE, tempsha, *stime, parent_size, tempnfmark, FALSE );
                          fe_list();
                          return NEW_INSTANCE_DENY;
                        }
                    }
                  temp2 = temp2->next;
                }
              // we need to send new query to user
              goto quit;

            } // else if (temp->current_pid == '1')
        } //  if (!strcmp(temp->path, path)) {
      temp = temp->next;
    } //while (temp != NULL)

quit:
  pthread_mutex_unlock ( &dlist_mutex );
  //if the path is not in dlist or is a new instance of an *ONCE rule
  return GOTO_NEXT_STEP;
}

//scan only those /proc entries that are already in the dlist
// and only those that have a current PID (meaning the app has already sent a packet)
int socket_find_from_pids_in_dlist ( int *mysocket, char *m_path, char *m_pid, int *nfmark_to_set)
{
  char find_socket[32]; //contains the string we are searching in /proc/PID/fd/1,2,3 etc.  a-la socket:[1234]
  char path[32];
  char path2[32];
  char socketbuf[32];
  char exepathbuf[PATHSIZE];
  DIR * m_dir;
  struct dirent *m_dirent;
  char socketstr[16];

  sprintf ( socketstr, "%d", *mysocket );  //convert inode from int to string

  strcpy ( find_socket, "socket:[" );
  strcat ( find_socket, socketstr );
  strcat ( find_socket, "]" );

  pthread_mutex_lock ( &dlist_mutex );
  dlist * temp = first_rule->next;

  while ( temp != NULL )
    {
      //find entry with a known PID, i.e. this entry has already seen active packets in this session
      //and also ignore kernel processes
      if (!temp->is_active || !strcmp(temp->path, KERNEL_PROCESS))
        {
          temp = temp->next;
          continue;
        }
      strcpy ( path, "/proc/" );
      strcat ( path, temp->pid );
      strcat ( path, "/fd/" );
      if ( ! ( m_dir = opendir ( path ) ) )
        {
          //This condition can happen if the PID is still in the dlist, the process exited and refresh_thread hasn't yet purged it out of the dlist
          M_PRINTF ( MLOG_DEBUG, "opendir for %s %s: %s,%s,%d\n", temp->path, path, strerror ( errno ), __FILE__, __LINE__ );
          temp = temp->next;
          closedir ( m_dir );
          continue;
        }
      while ( m_dirent = readdir ( m_dir ) )
        {
          strcpy ( path2, path );
          strcat ( path2, m_dirent->d_name ); //path2 contains /proc/PID/fd/1,2,3 etc. which are symlinks
          memset ( socketbuf, 0, SOCKETBUFSIZE );
          readlink ( path2, socketbuf, SOCKETBUFSIZE ); //no trailing 0
          //TODO socketbuf[readlink's retval] = 0 instead of memset
          if ( strcmp ( find_socket, socketbuf ) == 0 )
            {
              //return link /proc/<pid>/exe
              strcpy ( path, "/proc/" );
              strcat ( path, temp->pid );
              strcat ( path, "/exe" );
              memset ( exepathbuf, 0, PATHSIZE );
              readlink ( path, exepathbuf, PATHSIZE );
              //TODO exepathbuf[readlink's retval] = 0 instead of memset
              strcpy(m_path, exepathbuf);
              strcpy (m_pid, temp->pid);
              closedir ( m_dir );

	      unsigned long long stime;
              stime = starttimeGet ( atoi ( temp->pid ) );
              if ( temp->stime != stime )
                {
		  M_PRINTF ( MLOG_INFO, "SPOOFED_PID in %s %s %d", temp->path,  __FILE__, __LINE__ );
		  return SPOOFED_PID;
                }

              if ( !strcmp ( temp->perms, ALLOW_ONCE ) || !strcmp ( temp->perms, ALLOW_ALWAYS ) )
                {
                  *nfmark_to_set = temp->nfmark_out;

                  pthread_mutex_unlock ( &dlist_mutex );
                  return SOCKET_FOUND_IN_DLIST_ALLOW;
                }
              if ( !strcmp ( temp->perms, DENY_ONCE ) || !strcmp ( temp->perms, DENY_ALWAYS ) )
                {
                  pthread_mutex_unlock ( &dlist_mutex );
                  return SOCKET_FOUND_IN_DLIST_DENY;
                }
            }
        }
      closedir ( m_dir );
      temp = temp->next;
    }
  pthread_mutex_unlock ( &dlist_mutex );
  return GOTO_NEXT_STEP;
}

//scan /proc to find which PID the socket belongs to
int socket_find_in_proc ( int *mysocket, char *m_path, char *m_pid, unsigned long long *stime )
{
  //vars for scanning through /proc dir
  struct dirent *proc_dirent, *fd_dirent;
  DIR *proc_DIR, *fd_DIR;
  // holds path to /proc/<pid>/fd/<number_of_inode_opened>
  char path[32];
  char fdpath[32];
  // buffers to hold readlink()ed values of /proc/<pid>/exe and /proc/<pid>/fd/<inode>
  char exepathbuf[PATHSIZE];
  char socketbuf[SOCKETBUFSIZE];

  //convert inode from int to string
  char socketstr[16];
  sprintf ( socketstr, "%d", *mysocket ); //convert int to char* for future use
  char find_socket[32] = "socket:[";
  strcat ( find_socket, socketstr );
  strcat ( find_socket, "]" );

  proc_DIR = opendir ( "/proc" );
  do
    {
      proc_dirent = readdir ( proc_DIR );
      if ( !proc_dirent )
        {
          //perror("procdirent");
          break;
        } //EOF reached or some error
      if ( ( 47 < proc_dirent->d_name[0] ) && ( proc_dirent->d_name[0] < 58 ) ) // starts with ASCII 1 through 9
        {
          path[0] = 0; //empty the path
          strcpy ( path, "/proc/" );
          strcat ( path, proc_dirent->d_name );
          strcat ( path, "/fd" );
          //we may get a NULL retval if process has exited since readdir(proc_DIR) call and path doesnt exist anymore
          fd_DIR = opendir ( path );
          if ( !fd_DIR )
            {
              cap_t cap = cap_get_proc();
              printf("Running with capabilities: %s\n", cap_to_text(cap, NULL));
              cap_free(cap);
              //PID quit while scanning /proc
              M_PRINTF ( MLOG_INFO, "opendir(%s):%s,%s,%d\n", path, strerror ( errno ), __FILE__, __LINE__ );
              continue;

            } // permission denied or some other error
          do
            {
              fd_dirent = readdir ( fd_DIR );
              if ( !fd_dirent ) //EOF
                {
                  //perror("fddirent");
                  closedir ( fd_DIR );
                  break;
                }
              //make sure theres no . in the path
              if ( ! ( fd_dirent->d_name[0] == 46 ) )
                {
                  fdpath[0] = 0;
                  strcat ( fdpath, path );
                  strcat ( fdpath, "/" );
                  strcat ( fdpath, fd_dirent->d_name );
                  memset ( socketbuf, 0, SOCKETBUFSIZE );
                  readlink ( fdpath, socketbuf, SOCKETBUFSIZE ); //no trailing 0
                  if ( strcmp ( find_socket, socketbuf ) == 0 ) //strcmp return 0 when match
                    //we found our socket!!!!
                    {
                      //immediately get starttime
                      *stime  = starttimeGet ( atoi ( proc_dirent->d_name ) );

                      //return link /proc/<pid>/exe
                      strcpy ( path, "/proc/" );
                      strcat ( path, proc_dirent->d_name );
                      strcat ( path, "/exe" );
                      memset ( exepathbuf, 0, PATHSIZE );
                      readlink ( path, exepathbuf, PATHSIZE - 1 );


                      closedir ( fd_DIR );
                      closedir ( proc_DIR );
                      strcpy ( m_path, exepathbuf );
                      strcpy ( m_pid, proc_dirent->d_name );
                      return GOTO_NEXT_STEP;
                    }
                }
            }
          while ( fd_dirent );
        }
    }
  while ( proc_dirent );
  closedir ( proc_DIR );
  return SOCKET_NONE_PIDFD;
}

//if there are more than one entry in /proc/net/raw for icmp then it's impossible to tell which app is sending the packet
int icmp_check_only_one_inode ( int *m_inodeint )
{
  int loop = 0;
  int inodeint;
  int readbytes = 1;

  char inode[8];

  while ( 1 )
    {
      lseek ( procnetrawfd, 206 + 110 * loop, SEEK_SET );
      readbytes = read ( procnetrawfd, inode, 8 );
      //in case there was icmp packet but no /proc/net/raw entry - report
      if ( ( loop == 0 ) && ( readbytes == 0 ) )
        {
          M_PRINTF ( MLOG_INFO, "ICMP packet without /proc/net/raw entry" );
          return ICMP_NO_ENTRY;
        }
      //if there are two lines in the file, we drop the packet
      if ( loop > 0 )
        {
          if ( readbytes == 0 ) break; //break while loop
          //else the are more than one line
          return ICMP_MORE_THAN_ONE_ENTRY;
        }
      int i;
      for ( i = 0; i < 8; ++i )
        {
          if ( inode[i] == 32 )
            {
              inode[i] = 0; // 0x20 space, see /proc/net/ucp
              break;
            }
        }
      *m_inodeint = atoi ( inode );
      ++loop;
    }
  M_PRINTF ( MLOG_DEBUG, "(icmp)inode %d", inodeint );
  return 0;
}

int socket_check_kernel_udp(int *socket)
{
  //sometimes kernel sockets have inode numbers and are indistinguishable from user sockets.
  //The ony diffrnc is they have uid=0 (but so are root's)
  //rescan /proc/net to see if this socket might be kernel's or (root's)

  char sockstr[12];
  int sockstr_sz;

  sprintf(sockstr,"%d", *socket);
  //add space to the end of string for easier strcmp
  sockstr_sz = strlen(sockstr);
  sockstr[sockstr_sz] = 32;
  sockstr[sockstr_sz+1] = 0;

  char uid;
  FILE *mudpinfo, *mudp6info;
  char * membuf;
  int bytesread;

  if ((membuf=(char*)malloc(MEMBUF_SIZE)) == NULL) perror("malloc");
  memset(membuf,0, MEMBUF_SIZE);
  if ( ( mudpinfo = fopen ( UDPINFO, "r" ) ) == NULL )
    {
      M_PRINTF ( MLOG_INFO, "fopen: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
      return PROCFS_ERROR;
    }
  fseek(mudpinfo,0,SEEK_SET);
  errno = 0;
  if (bytesread = fread(membuf, sizeof(char), MEMBUF_SIZE , mudpinfo))
    {
      if (errno != 0) perror("fread udpinfo");
    }
  fclose(mudpinfo);
  int i = 0;
  char proc_sockstr[12];
  proc_sockstr[0] = 1; //initialize

  while(proc_sockstr[0] != 0)
    {
      memcpy(proc_sockstr, &membuf[220+128*i], 12);
      if (strncmp(proc_sockstr, sockstr, sockstr_sz))
        {
          i++;
          continue;
        }
      //else match
      memcpy(&uid, &membuf[209+128*i],1);
      free(membuf);
      if (uid != '0')
        {
          return SOCKET_NONE_PIDFD;
        }
      else return INKERNEL_SOCKET_FOUND;
    }
  //not found in /proc/net/tcp, search in /proc/net/tcp6

  memset(membuf,0, MEMBUF_SIZE);
  if ( ( mudp6info = fopen ( UDP6INFO, "r" ) ) == NULL )
    {
      M_PRINTF ( MLOG_INFO, "fopen: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
      return PROCFS_ERROR;
    }
  fseek(mudp6info,0,SEEK_SET);
  errno = 0;
  if (bytesread = fread(membuf, sizeof(char), MEMBUF_SIZE , mudp6info))
    {
      if (errno != 0) perror("fread tcpinfo");
    }
  fclose(mudp6info);
  i = 0;
  proc_sockstr[0] = 1;
  while(proc_sockstr[0] != 0)
    {
      memcpy(proc_sockstr, &membuf[284+171*i], 12);
      if (strncmp(proc_sockstr, sockstr, sockstr_sz))
        {
          i++;
          continue;
        }
      //else match
      memcpy(&uid, &membuf[273+171*i],1);
      free(membuf);
      if (uid != '0')
        {
          return SOCKET_NONE_PIDFD;
        }
      else return INKERNEL_SOCKET_FOUND;
    }
  return SOCKET_NONE_PIDFD;
}


int socket_check_kernel_tcp(int *socket)
{
  //sometimes kernel sockets have inode numbers and are indistinguishable from user sockets.
  //The ony diffrnc is they have uid=0 (but so are root's)
  //rescan /proc/net to see if this socket might be kernel's or (root's)

  char sockstr[12];
  int sockstr_sz;

  sprintf(sockstr,"%d", *socket);
  //add space to the end of string for easier strcmp
  sockstr_sz = strlen(sockstr);
  sockstr[sockstr_sz] = 32;
  sockstr[sockstr_sz+1] = 0;

  char uid;
  FILE *mtcpinfo, *mtcp6info;
  char * membuf;
  int bytesread;

  if ((membuf=(char*)malloc(MEMBUF_SIZE)) == NULL) perror("malloc");
  memset(membuf,0, MEMBUF_SIZE);
  if ( ( mtcpinfo = fopen ( TCPINFO, "r" ) ) == NULL )
    {
      M_PRINTF ( MLOG_INFO, "fopen: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
      return PROCFS_ERROR;
    }
  fseek(mtcpinfo,0,SEEK_SET);
  errno = 0;
  if (bytesread = fread(membuf, sizeof(char), MEMBUF_SIZE , mtcpinfo))
    {
      if (errno != 0) perror("fread tcpinfo");
    }
  fclose(mtcpinfo);
  int i = 0;
  char proc_sockstr[12];
  proc_sockstr[0] = 1; //initialize

  while(proc_sockstr[0] != 0)
    {
      memcpy(proc_sockstr, &membuf[165+150*i+76], 12);
      if (strncmp(proc_sockstr, sockstr, sockstr_sz))
        {
          i++;
          continue;
        }
      //else match
      memcpy(&uid, &membuf[230+150*i],1);
      free(membuf);
      if (uid != '0')
        {
          return SOCKET_NONE_PIDFD;
        }
      else return INKERNEL_SOCKET_FOUND;
    }
  //not found in /proc/net/tcp, search in /proc/net/tcp6

  memset(membuf,0, MEMBUF_SIZE);
  if ( ( mtcp6info = fopen ( TCP6INFO, "r" ) ) == NULL )
    {
      M_PRINTF ( MLOG_INFO, "fopen: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
      return PROCFS_ERROR;
    }
  fseek(mtcp6info,0,SEEK_SET);
  errno = 0;
  if (bytesread = fread(membuf, sizeof(char), MEMBUF_SIZE , mtcp6info))
    {
      if (errno != 0) perror("fread tcpinfo");
    }
  fclose(mtcp6info);
  i = 0;
  proc_sockstr[0] = 1;
  while(proc_sockstr[0] != 0)
    {
      memcpy(proc_sockstr, &membuf[284+171*i], 12);
      if (strncmp(proc_sockstr, sockstr, sockstr_sz))
        {
          i++;
          continue;
        }
      //else match
      memcpy(&uid, &membuf[273+171*i],1);
      free(membuf);
      if (uid != '0')
        {
          return SOCKET_NONE_PIDFD;
        }
      else return INKERNEL_SOCKET_FOUND;
    }
  return SOCKET_NONE_PIDFD;
}


//find in procfs which socket corresponds to source port
int port2socket_udp ( int *portint, int *socketint )
{
  char buffer[5];
  char procport[12];
  char socketstr[12];
  long m_socketint;
  int not_found_once=0;
  int bytesread_udp = 0;
  int bytesread_udp6 = 0;
  int i = 0;

  struct timespec timer,dummy;
  timer.tv_sec=0;
  timer.tv_nsec=1000000000/4;
  //convert portint to a hex string of 4 all-caps chars with leading zeroes if necessary
  char porthex[5];
  sprintf (porthex, "%04X", *portint );

  goto dont_fread;

do_fread:
  memset(udp_membuf,0, MEMBUF_SIZE);
  fseek(udpinfo,0,SEEK_SET);
  errno = 0;
  if (bytesread_udp = fread(udp_membuf, sizeof(char), MEMBUF_SIZE , udpinfo))
    {
      if (errno != 0) perror("READERORRRRRRR");
    }
  M_PRINTF (MLOG_DEBUG2, "udp bytes read: %d\n", bytesread_udp);

  memset(udp6_membuf, 0, MEMBUF_SIZE);
  fseek(udp6info,0,SEEK_SET);
  errno = 0;
  if (bytesread_udp6 = fread(udp6_membuf, sizeof(char), MEMBUF_SIZE , udp6info))
    {
      if (errno != 0) perror("6READERORRRRRRR");
    }
  M_PRINTF (MLOG_DEBUG2, "udp6 bytes read: %d\n", bytesread_udp6);

dont_fread:
  ;
  char newline[2] = {'\n','\0'};
  char *token;
  token = strtok(udp_membuf, newline); //skip the first line (column headers)
  while ((token = strtok(NULL, newline)) != NULL)  //take a line until EOF
    {
      sscanf(token, "%*s %*8s:%4s %*s %*s %*s %*s %*s %*s %*s %ld \n", buffer, &m_socketint);
      if (!strcmp (porthex, buffer))
        goto endloop;
    }
  // else EOF reached with no match, check if it was IPv6 socket

  token = strtok(udp6_membuf, newline); //skip the first line (column headers)
  while ((token = strtok(NULL, newline)) != NULL)  //take a line until EOF
    {
      sscanf(token, "%*s %*32s:%4s %*s %*s %*s %*s %*s %*s %*s %ld \n", buffer, &m_socketint);
      if (!strcmp (porthex, buffer))
        goto endloop;
    }

  //else EOF reached with no match, if it was 1st iteration then reread proc file
  if (not_found_once)
    {
      return SRCPORT_NOT_FOUND_IN_PROC;
    }
  //else
  nanosleep(&timer, &dummy);
  not_found_once=1;
  goto do_fread;

endloop:
  *socketint = m_socketint;
  if (*socketint == 0) return INKERNEL_SOCKET_FOUND;
  //else
  return GOTO_NEXT_STEP;
}



//find in procfs which socket corresponds to source port
int  port2socket_tcp ( int *portint, int *socketint )
{
  char buffer[5];
  char procport[12];
  char socketstr[12];
  long m_socketint;
  int not_found_once=0;
  int bytesread_tcp = 0;
  int bytesread_tcp6 = 0;
  int i = 0;

  struct timespec timer,dummy;
  timer.tv_sec=0;
  timer.tv_nsec=1000000000/4;
  //convert portint to a hex string of 4 all-caps chars with leading zeroes if necessary
  char porthex[5];
  sprintf (porthex, "%04X", *portint );

  goto dont_fread;

do_fread:
  memset(tcp_membuf,0, MEMBUF_SIZE);
  fseek(tcpinfo,0,SEEK_SET);
  errno = 0;
  if (bytesread_tcp = fread(tcp_membuf, sizeof(char), MEMBUF_SIZE , tcpinfo))
    {
      if (errno != 0) perror("fread tcpinfo");
    }
  M_PRINTF (MLOG_DEBUG2, "tcp bytes read: %d\n", bytesread_tcp);

  memset(tcp6_membuf, 0, MEMBUF_SIZE);
  fseek(tcp6info,0,SEEK_SET);
  errno = 0;
  if (bytesread_tcp6 = fread(tcp6_membuf, sizeof(char), MEMBUF_SIZE , tcp6info))
    {
      if (errno != 0) perror("fread tcp6info");
    }
  M_PRINTF (MLOG_DEBUG2, "tcp6 bytes read: %d\n", bytesread_tcp6);

dont_fread:
  ;
  char newline[2] = {'\n','\0'};
  char *token;
  token = strtok(tcp_membuf, newline); //skip the first line (column headers)
  while ((token = strtok(NULL, newline)) != NULL)  //take a line until EOF
    {
      sscanf(token, "%*s %*8s:%4s %*s %*s %*s %*s %*s %*s %*s %ld \n", buffer, &m_socketint);
      if (!strcmp (porthex, buffer))
        goto endloop;
    }
  // else EOF reached with no match, check if it was IPv6 socket

  token = strtok(tcp6_membuf, newline); //skip the first line (column headers)
  while ((token = strtok(NULL, newline)) != NULL)  //take a line until EOF
    {
      sscanf(token, "%*s %*32s:%4s %*s %*s %*s %*s %*s %*s %*s %ld \n", buffer,& m_socketint);
      if (!strcmp (porthex, buffer))
        goto endloop;
    }

  //else EOF reached with no match, if it was 1st iteration then reread proc file
  if (not_found_once)
    {
      return SRCPORT_NOT_FOUND_IN_PROC;
    }
  //else
  nanosleep(&timer, &dummy);
  not_found_once=1;
  goto do_fread;

endloop:
  *socketint = m_socketint;
  if (*socketint == 0) return INKERNEL_SOCKET_FOUND;
  //else
  return GOTO_NEXT_STEP;
}

//Handler for TCP packets for INPUT NFQUEUE
int packet_handle_tcp_in ( int socketint, int *nfmark_to_set, char *path, char *pid, unsigned long long *stime)
{
  int retval;
  //returns GOTO_NEXT_STEP => OK to go to the next step, otherwise  it returns one of the verdict values
  if ((retval = parsecache_in(socketint, path, pid)) != GOTO_NEXT_STEP)
    {
      M_PRINTF (MLOG_DEBUG2, "(cache)");
      goto out;
    }
  if ( (retval = socket_find_from_pids_in_dlist ( &socketint, path, pid, nfmark_to_set ) ) != GOTO_NEXT_STEP ) goto out;
  retval = socket_find_in_proc ( &socketint, path, pid, stime );
  if (retval == SOCKET_NONE_PIDFD)
    {
      retval = socket_check_kernel_tcp(&socketint);
      goto out;
    }
  else if (retval != GOTO_NEXT_STEP) goto out;

  if ( (retval = path_find_in_dlist ( nfmark_to_set, path, pid, stime) ) != GOTO_NEXT_STEP) goto out;
out:
  return retval;
}

//Handler for TCP packets for OUTPUT NFQUEUE
int packet_handle_tcp_out ( int socket, int *nfmark_to_set, char *path, char *pid, unsigned long long *stime)
{
  int retval;
  //returns GOTO_NEXT_STEP => OK to go to the next step, otherwise  it returns one of the verdict values
  if ((retval = parsecache_out(socket, path, pid)) != GOTO_NEXT_STEP)
    {
      M_PRINTF (MLOG_DEBUG2, "(cache)");
      goto out;
    }
  if ( (retval = socket_find_from_pids_in_dlist ( &socket, path, pid, nfmark_to_set ) ) != GOTO_NEXT_STEP ) goto out;
  retval = socket_find_in_proc ( &socket, path, pid, stime );
  if (retval == SOCKET_NONE_PIDFD)
    {
      retval = socket_check_kernel_tcp(&socket);
      goto out;
    }
  else if (retval != GOTO_NEXT_STEP) goto out;

  if ( (retval = path_find_in_dlist ( nfmark_to_set, path, pid, stime) ) != GOTO_NEXT_STEP) goto out;
out:
  return retval;
}

//Handler for UDP packets
int packet_handle_udp_in ( int socketint, int *nfmark_to_set, char *path, char *pid, unsigned long long *stime)
{
  int retval;
  //returns GOTO_NEXT_STEP => OK to go to the next step, otherwise  it returns one of the verdict values
  if ((retval = parsecache_in(socketint, path, pid)) != GOTO_NEXT_STEP)
    {
      M_PRINTF (MLOG_DEBUG2, "(cache)");
      goto out;
    }
  if ( (retval = socket_find_from_pids_in_dlist ( &socketint, path, pid, nfmark_to_set )) != GOTO_NEXT_STEP) goto out;
  retval = socket_find_in_proc ( &socketint, path, pid, stime );
  if (retval == SOCKET_NONE_PIDFD)
    {
      retval = socket_check_kernel_udp(&socketint);
      goto out;
    }
  else if (retval != GOTO_NEXT_STEP) goto out;



  if ( (retval = path_find_in_dlist ( nfmark_to_set, path, pid, stime) ) != GOTO_NEXT_STEP) goto out;
out:
  return retval;
}

//Handler for UDP packets
int packet_handle_udp_out ( int socket, int *nfmark_to_set, char *path, char *pid, unsigned long long *stime)
{
  int retval;
  //returns GOTO_NEXT_STEP => OK to go to the next step, otherwise  it returns one of the verdict values
  if ((retval = parsecache_out(socket, path, pid)) != GOTO_NEXT_STEP)
    {
      M_PRINTF (MLOG_DEBUG2, "(cache)");
      goto out;
    }
  if ( (retval = socket_find_from_pids_in_dlist ( &socket, path, pid, nfmark_to_set )) != GOTO_NEXT_STEP) goto out;
  retval = socket_find_in_proc ( &socket, path, pid, stime );
  if (retval == SOCKET_NONE_PIDFD)
    {
      retval = socket_check_kernel_udp(&socket);
      goto out;
    }
  else if (retval != GOTO_NEXT_STEP) goto out;



  if ( (retval = path_find_in_dlist ( nfmark_to_set, path, pid, stime) ) != GOTO_NEXT_STEP) goto out;
out:
  return retval;
}

/* Not in use atm b/c trafficthread calculates traffic
void increase_allowed_traffic_out(int out_packet_size)
{
    pthread_mutex_lock ( &dlist_mutex );
    dlist *temp = first;
    while ( temp->next != NULL )
    {
	temp = temp->next;
	if (temp->ordinal_number != rule_ordinal_out) continue;
	temp->out_allow_counter += out_packet_size;
	printf ("Traffic: %lu ", temp->out_allow_counter);
	pthread_mutex_unlock ( &dlist_mutex );
	return;
    }
    pthread_mutex_unlock ( &dlist_mutex );
    printf ("Ordinal number %d not found \n", rule_ordinal_out);
}
*/

int is_tcp_port_in_table (int port)
{
  int i = 0;

  while (tcp_table[i*2] != MAGIC_NO)
    {
      if (i >= (MEMBUF_SIZE / (sizeof(int)*2)) - 1) break;
      if (tcp_table[i*2] != port)
        {
          i++;
          continue;
        }
      else
        {
          int retval;
          retval = tcp_table[i*2+1];
          return retval;
        }
    }
  //it wasn't found reinject it into the NFQUEUE again
  return -1;

  i = 0;
  while (tcp6_table[i*2] != MAGIC_NO)
    {
      if (i >= (MEMBUF_SIZE / (sizeof(int)*2)) - 1) break;
      if (tcp6_table[i*2] != port)
        {
          i++;
          continue;
        }
      else
        {
          int retval2;
          retval2 = tcp6_table[i*2+1];
          return retval2;
        }
    }
  //it wasn't found reinject it into the NFQUEUE again
  return -1;
}


int is_udp_port_in_table (int port)
{
  int i = 0;
  while (udp_table[i*2] != MAGIC_NO)
    {
      if (i >= (MEMBUF_SIZE / (sizeof(int)*2)) - 1) break;
      if (udp_table[i*2] != port)
        {
          i++;
          continue;
        }
      else
        {
          return udp_table[i*2+1];
        }
    }
  //it wasn't found reinject it into the NFQUEUE again
  return -1;

  i = 0;
  while (udp6_table[i*2] != MAGIC_NO)
    {
      if (i >= (MEMBUF_SIZE / (sizeof(int)*2)) - 1) break;
      if (udp6_table[i*2] != port)
        {
          i++;
          continue;
        }
      else
        {

          int retval2;
          retval2 = udp6_table[i*2+1];
          return retval2;
        }
    }
  //it wasn't found reinject it into the NFQUEUE again
  return -1;
}

void print_traffic_log(int proto, int direction, char *ip, int srcport, int dstport, char *path, char *pid, int verdict)
{
  char m_logstring[PATHSIZE];
  if (direction == DIRECTION_IN)
    {
      strcpy(m_logstring,">");
      if (proto == PROTO_TCP)
        {
          strcat(m_logstring,"TCP ");
        }
      else if (proto == PROTO_UDP)
        {
          strcat (m_logstring, "UDP ");
        }
      else if (proto == PROTO_ICMP)
        {
          strcat (m_logstring, "ICMP ");
        }
      char port[8];
      sprintf (port,"%d",dstport);
      strcat (m_logstring, "dst ");
      strcat (m_logstring, port);
      strcat (m_logstring, " src ");
      strcat (m_logstring, ip);
      strcat (m_logstring,":");
      sprintf(port, "%d", srcport);
      strcat (m_logstring, port);
      strcat (m_logstring, " ");
    }
  else if (direction == DIRECTION_OUT)
    {
      strcpy(m_logstring,"<");
      if (proto == PROTO_TCP)
        {
          strcat(m_logstring,"TCP ");
        }
      else if (proto == PROTO_UDP)
        {
          strcat (m_logstring, "UDP ");
        }
      else if (proto == PROTO_ICMP)
        {
          strcat (m_logstring, "ICMP ");
        }
      char port[8];
      sprintf (port,"%d",srcport);
      strcat (m_logstring, "src ");
      strcat (m_logstring, port);
      strcat (m_logstring, " dst ");
      strcat (m_logstring, ip);
      strcat (m_logstring,":");
      sprintf(port, "%d", dstport);
      strcat (m_logstring, port);
      strcat (m_logstring, " ");
    }
  strcat (m_logstring, path);
  strcat (m_logstring, " ");
  strcat (m_logstring, pid);
  strcat (m_logstring, " ");

  switch ( verdict )
    {
    case ACCEPT:
    case SOCKET_FOUND_IN_DLIST_ALLOW:
    case PATH_FOUND_IN_DLIST_ALLOW:
    case NEW_INSTANCE_ALLOW:
    case FORKED_CHILD_ALLOW:
    case CACHE_TRIGGERED_ALLOW:
    case INKERNEL_RULE_ALLOW:

      strcat (m_logstring, "allow\n");
      break;


    case DROP:
      strcat (m_logstring, "drop\n");
      break;
    case PORT_NOT_FOUND:
      strcat (m_logstring, "packet's source port not found in /proc/net/*. This means that the remote machine has probed our port\n" );
      break;
    case SENT_TO_FRONTEND:
      strcat (m_logstring,  "(asking frontend) drop\n" );
      break;
    case SOCKET_FOUND_IN_DLIST_DENY:
    case PATH_FOUND_IN_DLIST_DENY:
    case NEW_INSTANCE_DENY:
    case FORKED_CHILD_DENY:
    case CACHE_TRIGGERED_DENY:
    case INKERNEL_RULE_DENY:
      strcat (m_logstring,  "deny\n" );
      break;
    case SOCKET_NONE_PIDFD:
      strcat (m_logstring,  "(no process associated with packet) drop\n" );
      break;
    case FRONTEND_NOT_LAUNCHED:
      strcat (m_logstring, "(frontend not active) drop\n" );
      break;
    case FRONTEND_BUSY:
      strcat (m_logstring, "(frontend busy) drop\n" );
      break;
    case UNSUPPORTED_PROTOCOL:
      strcat (m_logstring, "(unsupported protocol) drop\n" );
      break;
    case ICMP_MORE_THAN_ONE_ENTRY:
      strcat (m_logstring, "More than one program is using icmp, dropping\n" );
      break;
    case ICMP_NO_ENTRY:
      strcat (m_logstring, "icmp packet received by there is no icmp entry in /proc. Very unusual. Please report\n" );
      break;
    case SHA_DONT_MATCH:
      strcat (m_logstring, "Red alert. Some app is trying to impersonate another\n" );
      break;
    case SPOOFED_PID:
      strcat (m_logstring, "Attempt to spoof PID detected\n" );
      break;
    case EXESIZE_DONT_MATCH:
      strcat (m_logstring, "Red alert. Executable's size don't match the records\n" );
      break;
    case INODE_HAS_CHANGED:
      strcat (m_logstring, "Process inode has changed, This means that a process was killed and another with the same PID was immediately started. Smacks of somebody trying to hack your system\n" );
      break;
    case EXE_HAS_BEEN_CHANGED:
      strcat (m_logstring, "While process was running, someone changed his binary file on disk. Definitely an attempt to compromise the firewall\n" );
      break;
    case SRCPORT_NOT_FOUND_IN_PROC:
      strcat (m_logstring, "source port not found in procfs\n" );
      break;
    }
  M_PRINTF(MLOG_TRAFFIC, "%s", m_logstring);
}

int packet_handle_icmp(int *nfmark_to_set, char *path, char *pid, unsigned long long *stime)
{
  int retval, socketint;

  if (( retval = icmp_check_only_one_inode ( &socketint ) ) != GOTO_NEXT_STEP)  goto out;
  if (( retval = socket_find_from_pids_in_dlist ( &socketint, path, pid, nfmark_to_set ) ) != GOTO_NEXT_STEP) goto out;
  if (( retval = socket_find_in_proc ( &socketint, path, pid, stime ) )!= GOTO_NEXT_STEP) goto out;
  if (( retval = path_find_in_dlist (nfmark_to_set, path, pid, stime) )!= GOTO_NEXT_STEP) goto out;
  if ( !fe_active_flag_get() )
out:
    return retval;
}

int  nfq_handle_in ( struct nfq_q_handle *qh, struct nfgenmsg *nfmsg, struct nfq_data *nfad, void *mdata )
{
  pthread_mutex_lock(&lastpacket_mutex);
  gettimeofday(&lastpacket, NULL);
  pthread_mutex_unlock(&lastpacket_mutex);

  struct iphdr *ip;
  int id;
  struct nfqnl_msg_packet_hdr *ph = nfq_get_msg_packet_hdr ( ( struct nfq_data * ) nfad );
  if ( ph ) id = ntohl ( ph->packet_id );
  nfq_get_payload ( ( struct nfq_data * ) nfad, (char**)&ip );

  char daddr[INET_ADDRSTRLEN], saddr[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &(ip->daddr), daddr, INET_ADDRSTRLEN);
  inet_ntop(AF_INET, &(ip->saddr), saddr, INET_ADDRSTRLEN);

  int verdict;
  u_int16_t sport_netbo, dport_netbo, sport_hostbo, dport_hostbo;
  char path[PATHSIZE], pid[PIDLENGTH];
  unsigned long long stime;
  int proto;
  int m_socketint;
  switch ( ip->protocol )
    {
    case IPPROTO_TCP:
      proto = PROTO_TCP;
      // ihl is IP header length in 32bit words, multiply a word by 4 to get length in bytes
      struct tcphdr *tcp;
      tcp = ( struct tcphdr* ) ( (char*)ip + ( 4 * ip->ihl ) );
      sport_netbo = tcp->source;
      dport_netbo = tcp->dest;
      sport_hostbo = ntohs ( tcp->source );
      dport_hostbo = ntohs ( tcp->dest );

      if ((m_socketint = is_tcp_port_in_table(dport_hostbo)) == -1) //not found in cache
        {
          verdict = PORT_NOT_FOUND;
          goto kernel_verdict;
        }

      fe_was_busy_in = fe_awaiting_reply? TRUE: FALSE;
      if ((verdict = packet_handle_tcp_in ( m_socketint, &nfmark_to_set_in, path, pid, &stime )) == GOTO_NEXT_STEP || verdict == INKERNEL_SOCKET_FOUND)
        {
          if (verdict == INKERNEL_SOCKET_FOUND)  //see if this is an inkernel rule
            {
              pthread_mutex_lock(&dlist_mutex);
	      dlist *temp = first_rule;
              while(temp->next != NULL)
                {
                  temp = temp->next;
                  if (strcmp(temp->path, KERNEL_PROCESS)) continue;
                  //else
                  if (!strcmp(temp->pid, saddr))
                    {
                      if (!strcmp(temp->perms, ALLOW_ALWAYS) || !strcmp(temp->perms, ALLOW_ONCE))
                        {
                          temp->is_active = TRUE;
                          nfmark_to_set_in = temp->nfmark_out;
                          verdict = INKERNEL_RULE_ALLOW;
                          pthread_mutex_unlock(&dlist_mutex);
                          goto kernel_verdict;
                        }
                      else if (!strcmp(temp->perms, DENY_ALWAYS) || !strcmp(temp->perms, DENY_ONCE))
                        {
                          verdict = INKERNEL_RULE_DENY;
                          pthread_mutex_unlock(&dlist_mutex);
                          goto kernel_verdict;
                        }
                    }
                }
              pthread_mutex_unlock(&dlist_mutex);
              //not found in in-kernel list, drop the bumb
              verdict = SOCKET_NONE_PIDFD;
              goto kernel_verdict;
            }
          if (fe_was_busy_in)
            {
              verdict = FRONTEND_BUSY;
              break;
            }
          else verdict = fe_active_flag_get() ? fe_ask_in(path,pid,&stime, saddr, sport_hostbo, dport_hostbo ) : FRONTEND_NOT_LAUNCHED;
        }
      break;

    case IPPROTO_UDP:
      proto = PROTO_UDP;
      struct udphdr *udp;
      udp = ( struct udphdr * ) ( (char*)ip + ( 4 * ip->ihl ) );
      sport_netbo = udp->source;
      dport_netbo = udp->dest;
      sport_hostbo = ntohs ( udp->source );
      dport_hostbo = ntohs ( udp->dest );

      if ((m_socketint = is_udp_port_in_table(dport_hostbo)) == -1) //not found in cache
        {
          verdict = PORT_NOT_FOUND;
          goto kernel_verdict;
        }

      fe_was_busy_in = fe_awaiting_reply? TRUE: FALSE;
      if ((verdict = packet_handle_udp_in ( m_socketint, &nfmark_to_set_in, path, pid, &stime )) == GOTO_NEXT_STEP || verdict == INKERNEL_SOCKET_FOUND)
        {
          if (verdict == INKERNEL_SOCKET_FOUND)  //see if this is an inkernel rule
            {
              pthread_mutex_lock(&dlist_mutex);
	      dlist *temp = first_rule;
              while(temp->next != NULL)
                {
                  temp = temp->next;
                  if (strcmp(temp->path, KERNEL_PROCESS)) continue;
                  //else
                  if (!strcmp(temp->pid, saddr))
                    {
                      if (!strcmp(temp->perms, ALLOW_ALWAYS) || !strcmp(temp->perms, ALLOW_ONCE))
                        {
                          temp->is_active = TRUE;
                          nfmark_to_set_in = temp->nfmark_out;
                          verdict = INKERNEL_RULE_ALLOW;
                          pthread_mutex_unlock(&dlist_mutex);
                          goto kernel_verdict;
                        }
                      else if (!strcmp(temp->perms, DENY_ALWAYS) || !strcmp(temp->perms, DENY_ONCE))
                        {
                          verdict = INKERNEL_RULE_DENY;
                          pthread_mutex_unlock(&dlist_mutex);
                          goto kernel_verdict;
                        }
                    }
                }
              pthread_mutex_unlock(&dlist_mutex);
              //not found in in-kernel list, ask user reuse struct's fields
              strcpy(path, KERNEL_PROCESS);
              strcpy(pid, saddr);
              stime = sport_hostbo;
            }
          if (fe_was_busy_in)
            {
              verdict = FRONTEND_BUSY;
              break;
            }
          else verdict = fe_active_flag_get() ? fe_ask_in(path,pid,&stime, saddr, sport_hostbo, dport_hostbo) : FRONTEND_NOT_LAUNCHED;
        }
      break;
    case IPPROTO_ICMP:
      ;
      M_PRINTF ( MLOG_TRAFFIC, ">ICMP src %s ", saddr);
      fe_was_busy_in = fe_awaiting_reply? TRUE: FALSE;
      if ((verdict = packet_handle_icmp (&nfmark_to_set_in, path, pid, &stime )) == GOTO_NEXT_STEP)
        {
          if (fe_was_busy_in)
            {
              verdict = FRONTEND_BUSY;
              break;
            }
          else verdict = fe_active_flag_get() ? fe_ask_in(path,pid,&stime, saddr, sport_hostbo, dport_hostbo) : FRONTEND_NOT_LAUNCHED;
        }
      break;
    default:
      M_PRINTF ( MLOG_INFO, "IN unsupported protocol detected No. %d (lookup in /usr/include/netinet/in.h)\n", ip->protocol );
      M_PRINTF ( MLOG_INFO, "see FAQ on how to securely let this protocol use the internet" );
      verdict = UNSUPPORTED_PROTOCOL;
    }

kernel_verdict:
  print_traffic_log(proto, DIRECTION_IN, daddr, sport_hostbo, dport_hostbo, path, pid, verdict);

  if (verdict < ALLOW_VERDICT_MAX)
    {
      nfq_set_verdict ( ( struct nfq_q_handle * ) qh, id, NF_ACCEPT, 0, NULL );

      nfct_set_attr_u32(ct_in, ATTR_ORIG_IPV4_DST, ip->daddr);
      nfct_set_attr_u32(ct_in, ATTR_ORIG_IPV4_SRC, ip->saddr);
      nfct_set_attr_u8 (ct_in, ATTR_L4PROTO, ip->protocol);
      nfct_set_attr_u8 (ct_in, ATTR_L3PROTO, AF_INET);
      nfct_set_attr_u16(ct_in, ATTR_PORT_SRC, sport_netbo);
      nfct_set_attr_u16(ct_in, ATTR_PORT_DST, dport_netbo) ;


      //EBUSY returned, when there's too much activity in conntrack. Requery the packet
      while (nfct_query(setmark_handle_in, NFCT_Q_GET, ct_in) == -1)
        {
          if (errno == EBUSY)
            {
              M_PRINTF ( MLOG_INFO, "nfct_query GET %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
              continue;
            }
          if (errno == EILSEQ)
            {
              M_PRINTF ( MLOG_DEBUG, "nfct_query GET %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
              continue;
            }
          else
            {
              M_PRINTF ( MLOG_INFO, "nfct_query GET %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
              break;
            }
        }
      return 0;
    }

  // else
  nfq_set_verdict ( ( struct nfq_q_handle * ) qh, id, NF_DROP, 0, NULL );
  return 0;

}

//this function is invoked each time a packet arrives to OUTPUT NFQUEUE
int  nfq_handle_out_rest ( struct nfq_q_handle *qh, struct nfgenmsg *nfmsg, struct nfq_data *nfad, void *mdata )
{
  pthread_mutex_lock(&lastpacket_mutex);
  gettimeofday(&lastpacket, NULL);
  pthread_mutex_unlock(&lastpacket_mutex);

  struct iphdr *ip;
  u_int32_t id;
  struct nfqnl_msg_packet_hdr *ph = nfq_get_msg_packet_hdr ( ( struct nfq_data * ) nfad );
  if ( !ph )
    {
      printf ("ph == NULL, should never happen, please report");
      return 0;
    }
  id = ntohl ( ph->packet_id );
  nfq_get_payload ( ( struct nfq_data * ) nfad, (char**)&ip );
  char daddr[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &(ip->daddr), daddr, INET_ADDRSTRLEN);
  int verdict;
  char path[PATHSIZE], pid[PIDLENGTH];
  unsigned long long stime;
  switch (ip->protocol)
    {
    case IPPROTO_ICMP:
      fe_was_busy_out = fe_awaiting_reply? TRUE: FALSE;
      if ((verdict = packet_handle_icmp (&nfmark_to_set_out, path, pid, &stime )) == GOTO_NEXT_STEP)
        {
          if (fe_was_busy_out)
            {
              verdict = FRONTEND_BUSY;
              break;
            }
          else verdict = fe_active_flag_get() ? fe_ask_out(path,pid,&stime) : FRONTEND_NOT_LAUNCHED;
        }
      break;
    default:
      M_PRINTF ( MLOG_INFO, "OUT unsupported protocol detected No. %d (lookup in /usr/include/netinet/in.h)\n", ip->protocol );
      M_PRINTF ( MLOG_INFO, "see FAQ on how to securely let this protocol use the internet" );
      nfq_set_verdict ( ( struct nfq_q_handle * ) qh, id, NF_DROP, 0, NULL );
      return 0;
    }


  print_traffic_log(PROTO_ICMP, DIRECTION_OUT, daddr, 0, 0, path, pid, verdict);
  if (verdict < ALLOW_VERDICT_MAX)
    {
      nfq_set_verdict ( ( struct nfq_q_handle * ) qh, id, NF_ACCEPT, 0, NULL );
      return 0;

//Fix assigning icmp mark when Netfilter devs reply to my mailing list post
      nfct_set_attr_u32(ct_out_icmp, ATTR_ORIG_IPV4_DST, ip->daddr);
      nfct_set_attr_u32(ct_out_icmp, ATTR_ORIG_IPV4_SRC, ip->saddr);
      nfct_set_attr_u8 (ct_out_icmp, ATTR_L4PROTO, ip->protocol);
      nfct_set_attr_u8 (ct_out_icmp, ATTR_L3PROTO, AF_INET);
      // nfct_set_attr_u16(ct_out_icmp, ATTR_PORT_SRC, sport_netbyteorder);
      // nfct_set_attr_u16(ct_out_icmp, ATTR_PORT_DST, dport_netbyteorder) ;

      //EBUSY returned, when there's too much activity in conntrack. Requery the packet
      while (nfct_query(setmark_handle_out, NFCT_Q_GET, ct_out_icmp) == -1)
        {
          if (errno == EBUSY)
            {
              M_PRINTF ( MLOG_DEBUG, "nfct_query GET %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
              continue;
            }
          if (errno == EILSEQ)
            {
              M_PRINTF ( MLOG_DEBUG, "nfct_query GET %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
              continue;
            }
          else
            {
              M_PRINTF ( MLOG_DEBUG, "nfct_query GET %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
              break;
            }
        }


    }
  //else if verdict > ALLOW_VERDICT_MAX
  nfq_set_verdict ( ( struct nfq_q_handle * ) qh, id, NF_DROP, 0, NULL );
  return 0;
}



//this function is invoked each time a packet arrives to OUTPUT NFQUEUE
int  nfq_handle_out_udp ( struct nfq_q_handle *qh, struct nfgenmsg *nfmsg, struct nfq_data *nfad, void *mdata )
{
  pthread_mutex_lock(&lastpacket_mutex);
  gettimeofday(&lastpacket, NULL);
  pthread_mutex_unlock(&lastpacket_mutex);

  struct iphdr *ip;
  u_int32_t id;
  struct nfqnl_msg_packet_hdr *ph = nfq_get_msg_packet_hdr ( ( struct nfq_data * ) nfad );
  if ( !ph )
    {
      printf ("ph == NULL, should never happen, please report");
      return 0;
    }
  id = ntohl ( ph->packet_id );
  nfq_get_payload ( ( struct nfq_data * ) nfad, (char**)&ip );
  char daddr[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &(ip->daddr), daddr, INET_ADDRSTRLEN);
  out_packet_size = ntohs(ip->tot_len);
  int verdict;
  u_int16_t sport_netbyteorder, dport_netbyteorder;
  char path[PATHSIZE], pid[PIDLENGTH];
  unsigned long long stime;

  int bytesread_udp, bytesread_udp6;
  char newline[2] = {'\n','\0'};
  int pause = 2;
  char *token;
  int port, m_socketint, found_flag;
  int i = 0;

  struct udphdr *udp;
  udp = ( struct udphdr * ) ( (char*)ip + ( 4 * ip->ihl ) );
  sport_netbyteorder = udp->source;
  dport_netbyteorder = udp->dest;
  int srcudp = ntohs ( udp->source );

  if ((m_socketint = is_udp_port_in_table(srcudp)) != -1) //found in cache
    {
      //   printf("udptable cache hit \n");
      goto found2;
    }

  struct timespec timer2,dummy2;
  timer2.tv_sec=0;
  timer2.tv_nsec=1000000000/pause;
  nanosleep(&timer2, &dummy2);

  memset(udp_smallbuf,0, 4096);
  fseek(udpinfo,0,SEEK_SET);
  found_flag = 0;
  while ((bytesread_udp = read(udpinfo_fd, udp_smallbuf,  4060)) > 0)
    {
      udp_stats++;
      if (bytesread_udp == -1)
        {
          perror ("read");
          break;
        }
      token = strtok(udp_smallbuf, newline); //skip the first line (column headers)
      while ((token = strtok(NULL, newline)) != NULL)
        {
          //take a line until EOF
          sscanf(token, "%*s %*8s:%4X %*s %*s %*s %*s %*s %*s %*s %d \n", &port, &m_socketint);
          udp_table[i*2] = port;
          udp_table[i*2+1] = m_socketint;
          if (srcudp != port)
            {
              i++;
              continue;
            }
          //else
          found_flag = 1;
          i++;
        }
    }
  udp_table[i*2] = MAGIC_NO;
  if (found_flag) goto found2;

  nfq_set_verdict ( ( struct nfq_q_handle * ) qh, id, NF_DROP, 0, NULL );
  return 0;

found2:
  ;

  fe_was_busy_out = fe_awaiting_reply? TRUE: FALSE;
  if ((verdict = packet_handle_udp_out ( m_socketint, &nfmark_to_set_out, path, pid, &stime ))
      == GOTO_NEXT_STEP || verdict == INKERNEL_SOCKET_FOUND)
    {
      if (verdict == INKERNEL_SOCKET_FOUND)  //see if this is an inkernel rule
        {
          pthread_mutex_lock(&dlist_mutex);
	  dlist *temp = first_rule;
          while(temp->next != NULL)
            {
              temp = temp->next;
              if (strcmp(temp->path, KERNEL_PROCESS)) continue;
              //else
              if (!strcmp(temp->pid, daddr))
                {
                  if (!strcmp(temp->perms, ALLOW_ALWAYS) || !strcmp(temp->perms, ALLOW_ONCE))
                    {
                      temp->is_active = TRUE;
                      nfmark_to_set_out = temp->nfmark_out;
                      verdict = INKERNEL_RULE_ALLOW;
                      pthread_mutex_unlock(&dlist_mutex);
                      goto kernel_verdict;
                    }
                  else if (!strcmp(temp->perms, DENY_ALWAYS) || !strcmp(temp->perms, DENY_ONCE))
                    {
                      verdict = INKERNEL_RULE_DENY;
                      pthread_mutex_unlock(&dlist_mutex);
                      goto kernel_verdict;
                    }
                }
            }
          pthread_mutex_unlock(&dlist_mutex);
          //not found in in-kernel list, ask user
          strcpy(path, KERNEL_PROCESS);
          strcpy(pid, daddr);
          stime = ntohs (udp->dest);
        }
      if (fe_was_busy_out)
        {
          verdict = FRONTEND_BUSY;
        }
      else verdict = fe_active_flag_get() ? fe_ask_out(path,pid,&stime) : FRONTEND_NOT_LAUNCHED;
    }

kernel_verdict:
  print_traffic_log(PROTO_UDP, DIRECTION_OUT, daddr, srcudp, ntohs ( udp->dest ), path, pid, verdict);
  if (verdict < ALLOW_VERDICT_MAX)
    {
      nfq_set_verdict ( ( struct nfq_q_handle * ) qh, id, NF_ACCEPT, 0, NULL );

      nfct_set_attr_u32(ct_out_udp, ATTR_ORIG_IPV4_DST, ip->daddr);
      nfct_set_attr_u32(ct_out_udp, ATTR_ORIG_IPV4_SRC, ip->saddr);
      nfct_set_attr_u8 (ct_out_udp, ATTR_L4PROTO, ip->protocol);
      nfct_set_attr_u8 (ct_out_udp, ATTR_L3PROTO, AF_INET);
      nfct_set_attr_u16(ct_out_udp, ATTR_PORT_SRC, sport_netbyteorder);
      nfct_set_attr_u16(ct_out_udp, ATTR_PORT_DST, dport_netbyteorder) ;

      //EBUSY returned, when there's too much activity in conntrack. Requery the packet
      while (nfct_query(setmark_handle_out, NFCT_Q_GET, ct_out_udp) == -1)
        {
#ifdef DEBUG2
          M_PRINTF ( MLOG_DEBUG2, "nfct_query GET %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
#endif
          if (errno == EBUSY)
            {
              M_PRINTF ( MLOG_DEBUG, "nfct_query GET %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
              continue;
            }
          if (errno == EILSEQ)
            {
              M_PRINTF ( MLOG_DEBUG, "nfct_query GET %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
              continue;
            }
          else
            {
              M_PRINTF ( MLOG_DEBUG, "nfct_query GET %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
              break;
            }
        }

      return 0;
    }
  //else if verdict > ALLOW_VERDICT_MAX
  nfq_set_verdict ( ( struct nfq_q_handle * ) qh, id, NF_DROP, 0, NULL );
  return 0;
}


//this function is invoked each time a packet arrives to OUTPUT NFQUEUE
int  nfq_handle_out_tcp ( struct nfq_q_handle *qh, struct nfgenmsg *nfmsg, struct nfq_data *nfad, void *mdata )
{
  pthread_mutex_lock(&lastpacket_mutex);
  gettimeofday(&lastpacket, NULL);
  pthread_mutex_unlock(&lastpacket_mutex);

  struct iphdr *ip;
  u_int32_t id;
  struct nfqnl_msg_packet_hdr *ph = nfq_get_msg_packet_hdr ( ( struct nfq_data * ) nfad );
  if ( !ph )
    {
      printf ("ph == NULL, should never happen, please report");
      return 0;
    }
  id = ntohl ( ph->packet_id );
  nfq_get_payload ( ( struct nfq_data * ) nfad, (char**)&ip );
  char daddr[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &(ip->daddr), daddr, INET_ADDRSTRLEN);
  int verdict;
  u_int16_t sport_netbyteorder, dport_netbyteorder;
  char path[PATHSIZE]= {0};
  char pid[PIDLENGTH]= {0};
  unsigned long long stime;

  int bytesread_tcp, bytesread_tcp6;
  char newline[2] = {'\n','\0'};
  char *token;
  int port, socket, found_flag;
  int i = 0;

  // ihl field is IP header length in 32-bit words, multiply by 4 to get length in bytes
  struct tcphdr *tcp;
  tcp = ( struct tcphdr* ) ((char*)ip + ( 4 * ip->ihl ) );
  sport_netbyteorder = tcp->source;
  dport_netbyteorder = tcp->dest;
  int srctcp = ntohs ( tcp->source );

  if ((socket = is_tcp_port_in_table(srctcp)) != -1) //found in cache
    {
      // printf("tcptable cache hit \n");
      goto found2;
    }

  struct timespec timer2,dummy2;
  timer2.tv_sec=0;
  timer2.tv_nsec=1000000000/2;
  nanosleep(&timer2, &dummy2);

  memset(tcp_smallbuf,0, 4096);
  fseek(tcpinfo,0,SEEK_SET);
  found_flag = 0;
  while ((bytesread_tcp = read(tcpinfo_fd, tcp_smallbuf, 4060)) > 0)
    {
      tcp_stats++;
      if (bytesread_tcp == -1)
        {
          perror ("read");
          break;
        }
      token = strtok(tcp_smallbuf, newline); //skip the first line (column headers)
      while ((token = strtok(NULL, newline)) != NULL)
        {
          //take a line until EOF
          sscanf(token, "%*s %*8s:%4X %*s %*s %*s %*s %*s %*s %*s %d \n", &port, &socket);
          tcp_table[i*2] = port;
          tcp_table[i*2+1] = socket;
          if (srctcp != port)
            {
              i++;
              continue;
            }
          //else
          found_flag = 1;
          i++;
        }
    }
  tcp_table[i*2] = MAGIC_NO;
  if (found_flag) goto found2;

  //the packet has no inode associated with it
  nfq_set_verdict ( ( struct nfq_q_handle * ) qh, id, NF_DROP, 0, NULL );
  return 0;

found2:
  //remember f/e's state before we process
  fe_was_busy_out = fe_awaiting_reply? TRUE: FALSE;
  if ((verdict = packet_handle_tcp_out ( socket, &nfmark_to_set_out, path, pid, &stime ))
      == GOTO_NEXT_STEP || verdict == INKERNEL_SOCKET_FOUND)
    {
      if (verdict == INKERNEL_SOCKET_FOUND)  //see if this is an inkernel rule
        {

          pthread_mutex_lock(&dlist_mutex);
	  dlist *temp = first_rule;
          while(temp->next != NULL)
            {
              temp = temp->next;
              if (strcmp(temp->path, KERNEL_PROCESS)) continue;
              //else
              if (!strcmp(temp->pid, daddr))
                {
                  if (!strcmp(temp->perms, ALLOW_ALWAYS) || !strcmp(temp->perms, ALLOW_ONCE))
                    {
                      temp->is_active = TRUE;
                      nfmark_to_set_out = temp->nfmark_out;
                      verdict = INKERNEL_RULE_ALLOW;
                      pthread_mutex_unlock(&dlist_mutex);
                      goto kernel_verdict;
                    }
                  else if (!strcmp(temp->perms, DENY_ALWAYS) || !strcmp(temp->perms, DENY_ONCE))
                    {
                      verdict = INKERNEL_RULE_DENY;
                      pthread_mutex_unlock(&dlist_mutex);
                      goto kernel_verdict;
                    }
                }
            }
          pthread_mutex_unlock(&dlist_mutex);
          //not found in in-kernel list, drop
          verdict = SOCKET_NONE_PIDFD;
          goto kernel_verdict;
        }
      //drop if f/e was busy before we started processing
      if (fe_was_busy_out)
        {
          verdict = FRONTEND_BUSY;
        }
      else verdict = fe_active_flag_get() ? fe_ask_out(path,pid,&stime) : FRONTEND_NOT_LAUNCHED;
    }

kernel_verdict:
  print_traffic_log(PROTO_TCP, DIRECTION_OUT, daddr, srctcp, ntohs ( tcp->dest ), path, pid, verdict);

  if (verdict < ALLOW_VERDICT_MAX)
    {
      nfq_set_verdict ( ( struct nfq_q_handle * ) qh, id, NF_ACCEPT, 0, NULL );

      nfct_set_attr_u32(ct_out_tcp, ATTR_ORIG_IPV4_DST, ip->daddr);
      nfct_set_attr_u32(ct_out_tcp, ATTR_ORIG_IPV4_SRC, ip->saddr);
      nfct_set_attr_u8 (ct_out_tcp, ATTR_L4PROTO, ip->protocol);
      nfct_set_attr_u8 (ct_out_tcp, ATTR_L3PROTO, AF_INET);
      nfct_set_attr_u16(ct_out_tcp, ATTR_PORT_SRC, sport_netbyteorder);
      nfct_set_attr_u16(ct_out_tcp, ATTR_PORT_DST, dport_netbyteorder) ;

      //EBUSY returned, when there's too much activity in conntrack. Requery the packet
      while (nfct_query(setmark_handle_out, NFCT_Q_GET, ct_out_tcp) == -1)
        {
          if (errno == EBUSY)
            {
              M_PRINTF ( MLOG_DEBUG, "nfct_query GET %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
              continue;
            }
          if (errno == EILSEQ)
            {
              M_PRINTF ( MLOG_DEBUG, "nfct_query GET %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
              continue;
            }
          else
            {
              M_PRINTF ( MLOG_DEBUG, "nfct_query GET %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
              break;
            }
        }

      return 0;
    }

//else if verdict > ALLOW_VERDICT_MAX
  nfq_set_verdict ( ( struct nfq_q_handle * ) qh, id, NF_DROP, 0, NULL );
  return 0;
}





void init_log()
{

  if ( !strcmp ( logging_facility->sval[0], "file" ) )
    {
//         if (log_info->ival) {
//             if ((fileloginfofd = fopen(log_file->filename[0], "w")) == 0) perror("fopen");
//         }
//         if (log_traffic->ival) {
//             if ((filelogtrafficfd = fopen(log_file->filename[0], "w")) == 0) perror("fopen");
//         }
//         if (log_debug->ival) {
//             if ((filelogdebugfd = fopen(log_file->filename[0], "w")) == 0) perror("fopen");
//         };

//all chennels log to the same file, if need be the commented section above can be used to specify separate files
      if ( ( fileloginfo_stream = fopen ( log_file->filename[0], "w" ) ) == 0 ) perror ( "fopen" );
      filelogtraffic_stream = fileloginfo_stream;
      filelogdebug_stream = fileloginfo_stream;
      m_printf = &m_printf_file;
      return;
    }
  else if ( !strcmp ( logging_facility->sval[0], "stdout" ) )
    {
      m_printf = &m_printf_stdout;
      return;
    }
#ifndef WITHOUT_SYSLOG
  else if ( !strcmp ( logging_facility->sval[0], "syslog" ) )
    {
      openlog ( "lpfw", 0, 0 );
      m_printf = &m_printf_syslog;
    }
#endif
}

void pidfile_check()
{
  // use stat() to check if PIDFILE exists.
  //TODO The check is quick'n'dirty. Consider making more elaborate check later
  struct stat m_stat;
  FILE *pidfd;
  FILE *procfd;
  FILE *newpid;
  char pidbuf[8];
  char procstring[20];
  char procbuf[20];
  char srchstr[2] = {0x0A, 0};
  int pid;
  int newpidfd;
  char *ptr;
  char pid2str[8];
  //stat() returns 0 if file exists
  if ( stat ( pid_file->filename[0], &m_stat ) == 0 )
    {
      if ( ( pidfd = fopen ( pid_file->filename[0], "r" ) ) == NULL )
        {
          M_PRINTF ( MLOG_INFO, "fopen PIDFILE: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
          exit(0);
        };
      fgets ( pidbuf, 8, pidfd );
      fclose ( pidfd );
      pidbuf[7] = 0;
      pid = atoi ( pidbuf );
      if ( pid > 0 )
        {
          if ( kill ( pid, 0 ) == 0 ) //PID is running
            {
              // check that this pid belongs to lpfw
              strcpy ( procstring, "/proc/" );
              strcat ( procstring, pidbuf );
              strcat ( procstring, "/comm" );
              if ( ( procfd = fopen ( procstring, "r" )) == NULL)
                {
                  M_PRINTF ( MLOG_INFO, "fopen: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
                  exit(0);
                }
              //let's replace 0x0A with 0x00
              fgets ( procbuf, 19, procfd );
              fclose(procfd);
              ptr = strstr ( procbuf, srchstr );
              *ptr = 0;
              //compare the actual string, if found => carry on
              if ( !strcmp ( "lpfw", procbuf ) )
                {
                  //make sure that the running instance is NOT out instance
                  //(can happen when PID of previously crashed lpfw coincides with ours)
                  if ( ( pid_t ) pid != getpid() )
                    {
                      M_PRINTF ( MLOG_INFO, "lpfw is already running\n" );
                      die();
                    }
                }
            }
        }
    }
  else
    {
      M_PRINTF ( MLOG_DEBUG, "stat: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
    }


  //else if pidfile doesn't exist/contains dead PID, create/truncate it and write our pid into it
  if ( ( newpid = fopen ( pid_file->filename[0], "w" ) ) == NULL )
    {
      M_PRINTF ( MLOG_DEBUG, "creat PIDFILE: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
      return;
    }
  sprintf ( pid2str, "%d", ( int ) getpid() );
  ssize_t size;
  newpidfd = fileno(newpid);
  if ( ( size = write ( newpidfd, pid2str, 8 ) == -1 ) )
    {
      M_PRINTF ( MLOG_INFO, "write: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
      return;
    }
  //close(newpidfd);
  fclose ( newpid );
}

void checkRoot()
{
  uid_t pid;
  pid = getuid();
  if ( ( int ) pid != 0 )
    {
      printf ( "Leopard Flower should be run as root. Exiting...\n" );
      die();
    }
}

//initiate message queue and send to first lpfw instance, our pid, tty and display and quit.
int frontend_mode ( int argc, char *argv[] )
{
  key_t ipckey;
  int mqd;
  msg_struct_creds msg;
  //remove memory garbage
  memset (&msg, 0, sizeof(msg_struct_creds));
  msg.type = 1;


  if ( ( ipckey = ftok ( TMPFILE, FTOKID_CREDS ) ) == -1 )
    {
      printf ( "ftok: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
    };
  if ( ( mqd = msgget ( ipckey, 0 ) ) == -1 )
    {
      printf ( "msgget: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
      exit ( 0 );
    };

  if ( ( msg.creds.uid = getuid() ) == 0 )
    {
#ifndef DEBUG
      printf ( "You are trying to run lpfw's frontend as root. Such possibility is disabled due to securitty reasons. Please rerun as a non-priviledged user\n" );
      return -1;
#endif
    }

  strncpy ( msg.creds.tty, ttyname ( 0 ), TTYNAME - 1 );
  if ( !strncmp ( msg.creds.tty, "/dev/tty", 8 ) )
    {
      printf ( "You are trying to run lpfw's frontend from a tty terminal. Such possibility is disabled in this version of lpfw due to security reasons. Try to rerun this command from within an X terminal\n" );
      return -1;
    }

  char *display;
  if ( ( display = getenv ( "DISPLAY" ) ) == NULL )
    {
      printf ( "DISPLAY environment variable is not set (tip:usually it looks like  :0.0\n" );
      return -1;
    }
  strncpy ( msg.creds.display, display, DISPLAYNAME - 1 );

  int cli_args; //number of arguments that need to be passed to frontend
  cli_args = argc-2; //first two parms are path and --cli/--gui/--guipy
  strncpy (msg.creds.params[0], argv[1], 16);

  int i =0;
  if ( cli_args > 0 && cli_args < 5 ) //4 parms max - the last parm should be 0
    {
      msg.creds.params[1][0] = cli_args; //first parm has the total number of parms for lpfwcli (itself excluding)
      for ( i=0; i<cli_args; ++i )
        {
          strncpy ( msg.creds.params[i+2], argv[2+i], 16 );
        }
    }
  msg.creds.params[i+2][0] = 0; //the last parm should be 0

  if ( msgsnd ( mqd, &msg, sizeof ( msg_struct_creds ), 0 ) == -1 )
    {

      printf ( "msgsnd: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
    }

  //we need to sleep a little because lpfw is extracting out path from /proc/PID/exe
  //if we quit immediately, this information won't be available
  sleep ( 3 );
  return 0;
}

void TEST_FAILED_handler (int signal)
{

  if ( remove ( pid_file->filename[0] ) != 0 )
    {
      M_PRINTF ( MLOG_INFO, "remove PIDFILE: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
    }
  //release netfilter_queue resources
  M_PRINTF ( MLOG_INFO,"deallocating nfqueue resources...\n" );
  if ( nfq_close ( globalh_out_tcp ) == -1 )
    {
      M_PRINTF ( MLOG_INFO,"error in nfq_close\n" );
    }
  if ( nfq_close ( globalh_out_udp ) == -1 )
    {
      M_PRINTF ( MLOG_INFO,"error in nfq_close\n" );
    }
  printf("TEST FAILED");
  return;
}

void TEST_SUCCEEDED_handler (int signal)
{

  if ( remove ( pid_file->filename[0] ) != 0 )
    {
      M_PRINTF ( MLOG_INFO, "remove PIDFILE: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
    }
  //release netfilter_queue resources
  M_PRINTF ( MLOG_INFO,"deallocating nfqueue resources...\n" );
  if ( nfq_close ( globalh_out_tcp ) == -1 )
    {
      M_PRINTF ( MLOG_INFO,"error in nfq_close\n" );
    }
  if ( nfq_close ( globalh_out_udp ) == -1 )
    {
      M_PRINTF ( MLOG_INFO,"error in nfq_close\n" );
    }
  printf("test finished successfully\n");
  return;
}


void SIGTERM_handler ( int signal )
{

  if ( remove ( pid_file->filename[0] ) != 0 )
    M_PRINTF ( MLOG_INFO, "remove PIDFILE: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );

  rulesfileWrite();
  //release netfilter_queue resources
  M_PRINTF ( MLOG_INFO,"deallocating nfqueue resources...\n" );
  if ( nfq_close ( globalh_out_tcp ) == -1 )
    {
      M_PRINTF ( MLOG_INFO,"error in nfq_close\n" );
    }
  if ( nfq_close ( globalh_out_udp ) == -1 )
    {
      M_PRINTF ( MLOG_INFO,"error in nfq_close\n" );
    }
  exit(0);
  return;
}

/*command line parsing contributed by Ramon Fried*/
int parse_comline_args(int argc, char* argv[])
{
  // if the parsing of the arguments was unsuccessful
  int nerrors;

  // Define argument table structs
  logging_facility = arg_str0 ( NULL, "logging-facility",
#ifndef WITHOUT_SYSLOG
                                "<file>,<stdout>,<syslog>"
#else
                                "<file>,<stdout>"
#endif
                                , "Divert loggin to..." );
  rules_file = arg_file0 ( NULL, "rules-file", "<path to file>", "Rules output file" );
  pid_file = arg_file0 ( NULL, "pid-file", "<path to file>", "PID output file" );
  log_file = arg_file0 ( NULL, "log-file", "<path to file>", "Log output file" );

#ifndef WITHOUT_SYSVIPC
  cli_path = arg_file0 ( NULL, "cli-path", "<path to file>", "Path to CLI frontend" );
  gui_path = arg_file0 ( NULL, "gui-path", "<path to file>", "Path to GUI frontend" );
  pygui_path = arg_file0 ( NULL, "pygui-path", "<path to file>", "Path to Python-based GUI frontend" );
#endif

  log_info = arg_int0 ( NULL, "log-info", "<1/0 for yes/no>", "Info messages logging" );
  log_traffic = arg_int0 ( NULL, "log-traffic", "<1/0 for yes/no>", "Traffic logging" );
  log_debug = arg_int0 ( NULL, "log-debug", "<1/0 for yes/no>", "Debug messages logging" );
#ifdef DEBUG
  struct arg_lit *test = arg_lit0 ( NULL, "test", "Run unit test" );
#endif
  struct arg_lit *cli = arg_lit0 ( NULL, "cli", "Start ncurses frontend" );
  struct arg_lit *gui = arg_lit0 ( NULL, "gui", "Start standalone GUI frontend" );
  struct arg_lit *pygui = arg_lit0 ( NULL, "pygui", "Start Python-based frontend" );

  struct arg_lit *help = arg_lit0 ( NULL, "help", "Display help screen" );
  struct arg_lit *version = arg_lit0 ( NULL, "version", "Display the current version" );
  struct arg_end *end = arg_end ( 30 );
  void *argtable[] = {logging_facility, rules_file, pid_file, log_file,
#ifndef WITHOUT_SYSVIPC
                      cli_path, gui_path, pygui_path,
#endif
                      log_info, log_traffic, log_debug, help, version,
#ifdef DEBUG
                      test,
#endif
                      cli, gui, pygui, end
                     };

  // Set default values
  logging_facility->sval[0] = "stdout";
  rules_file->filename[0] = RULESFILE;
  pid_file->filename[0] = PIDFILE;
  log_file->filename[0] = LPFW_LOGFILE;

#ifndef WITHOUT_SYSVIPC

  char *clipath;
  clipath = malloc(PATHSIZE-16);
  strcpy (clipath, owndir);
  strcat(clipath, "lpfwcli");
  cli_path->filename[0] = clipath;

  char *guipath;
  guipath = malloc(PATHSIZE-16);
  strcpy (guipath, owndir);
  strcat(guipath, "lpfwgui");
  gui_path->filename[0] = guipath;

  char *pyguipath;
  pyguipath = malloc(PATHSIZE -16);
  strcpy (pyguipath, owndir);
  strcat(pyguipath,"lpfwgui.py");
  pygui_path->filename[0] = pyguipath;
#endif

  * ( log_info->ival ) = 1;
  * ( log_traffic->ival ) = 1;
#ifdef DEBUG
  * ( log_debug->ival ) = 1;
#else
  * ( log_debug->ival ) = 0;
#endif

  if ( arg_nullcheck ( argtable ) != 0 )
    {
      printf ( "Error: insufficient memory\n" );
      die(1);
    }

  nerrors = arg_parse ( argc, argv, argtable );

  if ( nerrors == 0 )
    {
      if ( help->count == 1 )
        {
          printf ( "Leopard Flower:\n Syntax and help:\n" );
          arg_print_glossary ( stdout, argtable, "%-43s %s\n" );
          exit (0);
        }
      else if ( version->count == 1 )
        {
          printf ( "%s\n", VERSION );
          exit (0);
        }
    }/* --leave this for future debugging purposes
	printf("\nArguments detected:\n");
	printf("--ipc-method = %s \n", ipc_method->sval[0]);
	printf("--login-facility = %s \n", logging_facility->sval[0]);
	printf("--rules_file = %s \n", rules_file->filename[0]);
	printf("--pid-file = %s \n", pid_file->filename[0]);
	printf("--log-file = %s \n", log_file->filename[0]);
	printf("--log-info = %d \n", log_info->count);
	printf("--log-error = %d \n", log_error->count);
	printf("--log-debug = %d \n", log_debug->count);
	printf("--help = %d \n", help->count);
	printf("--version = %d \n", version->count);
*/
  else if ( nerrors > 0 )
    {
      arg_print_errors ( stdout, end, "Leopard Flower" );
      printf ( "Leopard Flower:\n Syntax and help:\n" );
      arg_print_glossary ( stdout, argtable, "%-43s %s\n" );
      exit (1);
    }

  // Free memory - don't do this cause args needed later on
  //  arg_freetable(argtable, sizeof (argtable) / sizeof (argtable[0]));
}

/* chack that we have the 5 needed capabilities and if we do, then drop all the other capabilities */
void capabilities_setup()
{
  //=======  Capabilities check
  cap_t cap_current;
  cap_current = cap_get_proc();
  if (cap_current == NULL)
    {
      perror("cap_get_proc()");
    }

  cap_flag_value_t value;
  cap_get_flag(cap_current, CAP_SYS_PTRACE, CAP_PERMITTED, &value);
  if (value == CAP_CLEAR)
    {
      printf ("CAP_SYS_PTRACE is not permitted \n");
      exit(0);
    }
  cap_get_flag(cap_current, CAP_NET_ADMIN, CAP_PERMITTED, &value);
  if (value == CAP_CLEAR)
    {
      printf ("CAP_NET_ADMIN is not permitted \n");
      exit(0);
    }
  cap_get_flag(cap_current, CAP_DAC_READ_SEARCH, CAP_PERMITTED, &value);
  if (value == CAP_CLEAR)
    {
      printf ("CAP_DAC_READ_SEARCH is not permitted \n");
      exit(0);
    }
  cap_get_flag(cap_current, CAP_SETUID, CAP_PERMITTED, &value);
  if (value == CAP_CLEAR)
    {
      printf ("CAP_SETUID is not permitted \n");
      exit(0);
    }
  cap_get_flag(cap_current, CAP_SETGID, CAP_PERMITTED, &value);
  if (value == CAP_CLEAR)
    {
      printf ("CAP_SETGID is not permitted \n");
      exit(0);
    }

  cap_clear(cap_current);
  const cap_value_t caps_list[] = {CAP_SYS_PTRACE, CAP_NET_ADMIN, CAP_DAC_READ_SEARCH, CAP_SETUID, CAP_SETGID};
  cap_set_flag(cap_current, CAP_PERMITTED, 5, caps_list, CAP_SET);
  if (cap_set_proc(cap_current) == -1)
    {
      printf("cap_set_proc: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }

#ifdef DEBUG
  cap_t cap = cap_get_proc();
  printf("Running with capabilities: %s\n", cap_to_text(cap, NULL));
  cap_free(cap);
#endif
}

/* Create group lpfwuser. Backend and frontend both should belong to this group to communicate over sysvmsgq */
void setgid_lpfwuser()
{
  //First we need to create/(check existence of) lpfwuser group and add ourselves to it
  errno = 0;
  struct group *m_group;
  m_group = getgrnam("lpfwuser");
  if (!m_group)
    {
      if (errno == 0)
        {
          printf("lpfwuser group does not exit, creating...\n");
          if (system("groupadd lpfwuser") == -1)
            {
              printf("error in system(groupadd)\n");
              return;
            }
          //get group id again after group creation
          errno = 0;
          m_group = getgrnam("lpfwuser");
          if(!m_group)
            {
              if (errno == 0)
                {
                  printf ("lpfwuser group still doesn't exist even though we've just created it");
                }
              else
                {
                  perror ("getgrnam");
                }
            }
          lpfwuser_gid = m_group->gr_gid;
        }
      else
        {
          printf("Error in getgrnam\n");
          perror ("getgrnam");
        }
      return;
    }
  //when debugging, we add user who launches frontend to lpfwuser group, hence disable this check
#ifndef DEBUG
  if (!(m_group->gr_mem[0] == NULL))
    {
      printf ("lpfwuser group contains users. This group should not contain any users. This is a security issue. Please remove all user from that group and restart application. Exitting\n");
      exit(0);
    }
#endif
  lpfwuser_gid = m_group->gr_gid;

  //enable CAP_SETGID in effective set
  cap_t cap_current;
  cap_current = cap_get_proc();
  if (cap_current == NULL)
    {
      printf("cap_get_proc: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
  const cap_value_t caps_list[] = {CAP_SETGID};
  cap_set_flag(cap_current,  CAP_EFFECTIVE, 1, caps_list, CAP_SET);
  if (cap_set_proc(cap_current) == -1)
    {
      printf("cap_get_proc: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }

  //setgid and immediately remove CAP_SETGID from both perm. and eff. sets
  if (setgid(lpfwuser_gid) == -1)
    {
      printf("setgid: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
      return;
    }
  cap_set_flag(cap_current,  CAP_PERMITTED, 1, caps_list, CAP_CLEAR);
  cap_set_flag(cap_current,  CAP_EFFECTIVE, 1, caps_list, CAP_CLEAR);
  if (cap_set_proc(cap_current) == -1)
    {
      printf("cap_set_proc: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
}

/* Setuid() lpfw to root */
void setuid_root()
{
  //enable CAP_SETUID in effective set
  cap_t cap_current;
  cap_current = cap_get_proc();
  if (cap_current == NULL)
    {
      printf("cap_get_proc: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
  const cap_value_t caps_list[] = {CAP_SETUID};
  cap_set_flag(cap_current,  CAP_EFFECTIVE, 1, caps_list, CAP_SET);
  if (cap_set_proc(cap_current) == -1)
    {
      printf("cap_set_proc: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
  //setuid and immediately remove CAP_SETUID from both perm. and eff. sets
  if (setuid(0) == -1)
    {
      perror ("setuid ");
      return;
    }
  //we still neet to setuid in fe_reg_thread so leave this CAP in permitted set
  //cap_set_flag(cap_current,  CAP_PERMITTED, 1, caps_list, CAP_CLEAR);
  cap_set_flag(cap_current,  CAP_EFFECTIVE, 1, caps_list, CAP_CLEAR);
  if (cap_set_proc(cap_current) == -1)
    {
      printf("cap_set_proc: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
}

void setup_signal_handlers()
{
    //install SIGTERM handler
    struct sigaction sa;
    sa.sa_handler = SIGTERM_handler;
    sigemptyset ( &sa.sa_mask );
    if ( sigaction ( SIGTERM, &sa, NULL ) == -1 )
      {
	perror ( "sigaction" );
      }
}

void save_own_path()
{
    int ownpid = getpid();
    char ownpidstr[16];
    sprintf(ownpidstr, "%d", ownpid );
    char exepath[PATHSIZE] = "/proc/";
    strcat(exepath, ownpidstr);
    strcat(exepath, "/exe");
    memset(ownpath,0,PATHSIZE);
    if (readlink(exepath,ownpath,PATHSIZE-1) == -1)
      {
	printf("readlink: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
      }

    int basenamelength;
    basenamelength = strlen ( strrchr ( ownpath, '/' ) +1 );
    strncpy ( owndir, ownpath, strlen ( ownpath )-basenamelength );
}

void init_iptables()
{
    if ( system ( "iptables -I OUTPUT 1 -p all -m state --state NEW -j NFQUEUE --queue-num 11223" ) == -1 )
      {
	M_PRINTF ( MLOG_INFO, "system: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
      }
    if ( system ( "iptables -I OUTPUT 1 -p tcp -m state --state NEW -j NFQUEUE --queue-num 11220" ) == -1 )
      {
	M_PRINTF ( MLOG_INFO, "system: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
      }
    if ( system ( "iptables -I OUTPUT 1 -p udp -m state --state NEW -j NFQUEUE --queue-num 11222" ) == -1 )
      {
	M_PRINTF ( MLOG_INFO, "system: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
      }
    if ( system ( "iptables -I INPUT 1 -p all -m state --state NEW -j NFQUEUE --queue-num 11221" ) == -1 )
      {
	M_PRINTF ( MLOG_INFO, "system: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
      }
    if ( system ( "iptables -I OUTPUT 1 -d localhost -j ACCEPT" ) == -1 )
      {
	M_PRINTF ( MLOG_INFO, "system: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
      }
    if ( system ( "iptables -I INPUT 1 -d localhost -j ACCEPT" ) == -1 )
      {
	M_PRINTF ( MLOG_INFO, "system: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
      }



}

void init_nfq_handlers()
{
    //-----------------Register queue handler-------------
    globalh_out_tcp = nfq_open();
    if ( !globalh_out_tcp )
      {
	M_PRINTF ( MLOG_INFO, "error during nfq_open\n" );
      }
    if ( nfq_unbind_pf ( globalh_out_tcp, AF_INET ) < 0 )
      {
	M_PRINTF ( MLOG_INFO, "error during nfq_unbind\n" );
      }
    if ( nfq_bind_pf ( globalh_out_tcp, AF_INET ) < 0 )
      {
	M_PRINTF ( MLOG_INFO, "error during nfq_bind\n" );
      }
    struct nfq_q_handle * globalqh_tcp = nfq_create_queue ( globalh_out_tcp, NFQNUM_OUTPUT_TCP, &nfq_handle_out_tcp, NULL );
    if ( !globalqh_tcp )
      {
	M_PRINTF ( MLOG_INFO, "error in nfq_create_queue. Please make sure that any other instances of Leopard Flower are not running and restart the program. Exitting\n" );
	exit (0);
      }
    //copy only 40 bytes of packet to userspace - just to extract tcp source field
    if ( nfq_set_mode ( globalqh_tcp, NFQNL_COPY_PACKET, 40 ) < 0 )
      {
	M_PRINTF ( MLOG_INFO, "error in set_mode\n" );
      }
    if ( nfq_set_queue_maxlen ( globalqh_tcp, 200 ) == -1 )
      {
	M_PRINTF ( MLOG_INFO, "error in queue_maxlen\n" );
      }
    nfqfd_tcp = nfq_fd ( globalh_out_tcp);
    M_PRINTF ( MLOG_DEBUG, "nfqueue handler registered\n" );
    //--------Done registering------------------

    //-----------------Register queue handler-------------
    globalh_out_udp = nfq_open();
    if ( !globalh_out_udp )
      {
	M_PRINTF ( MLOG_INFO, "error during nfq_open\n" );
      }
    if ( nfq_unbind_pf ( globalh_out_udp, AF_INET ) < 0 )
      {
	M_PRINTF ( MLOG_INFO, "error during nfq_unbind\n" );
      }
    if ( nfq_bind_pf ( globalh_out_udp, AF_INET ) < 0 )
      {
	M_PRINTF ( MLOG_INFO, "error during nfq_bind\n" );
      }
    struct nfq_q_handle * globalqh_udp = nfq_create_queue ( globalh_out_udp, NFQNUM_OUTPUT_UDP, &nfq_handle_out_udp, NULL );
    if ( !globalqh_udp )
      {
	M_PRINTF ( MLOG_INFO, "error in nfq_create_queue. Please make sure that any other instances of Leopard Flower are not running and restart the program. Exitting\n" );
	exit (0);
      }
    //copy only 40 bytes of packet to userspace - just to extract tcp source field
    if ( nfq_set_mode ( globalqh_udp, NFQNL_COPY_PACKET, 40 ) < 0 )
      {
	M_PRINTF ( MLOG_INFO, "error in set_mode\n" );
      }
    if ( nfq_set_queue_maxlen ( globalqh_udp, 200 ) == -1 )
      {
	M_PRINTF ( MLOG_INFO, "error in queue_maxlen\n" );
      }
    nfqfd_udp = nfq_fd ( globalh_out_udp );
    M_PRINTF ( MLOG_DEBUG, "nfqueue handler registered\n" );
    //--------Done registering------------------

    globalh_out_rest = nfq_open();
    if ( !globalh_out_rest )
      {
	M_PRINTF ( MLOG_INFO, "error during nfq_open\n" );
      }
    if ( nfq_unbind_pf ( globalh_out_rest, AF_INET ) < 0 )
      {
	M_PRINTF ( MLOG_INFO, "error during nfq_unbind\n" );
      }
    if ( nfq_bind_pf ( globalh_out_rest, AF_INET ) < 0 )
      {
	M_PRINTF ( MLOG_INFO, "error during nfq_bind\n" );
      }
    struct nfq_q_handle * globalqh_rest = nfq_create_queue ( globalh_out_rest, NFQNUM_OUTPUT_REST, &nfq_handle_out_rest, NULL );
    if ( !globalqh_rest )
      {
	M_PRINTF ( MLOG_INFO, "error in nfq_create_queue. Please make sure that any other instances of Leopard Flower are not running and restart the program. Exitting\n" );
	exit (0);
      }
    //copy only 40 bytes of packet to userspace - just to extract tcp source field
    if ( nfq_set_mode ( globalqh_rest, NFQNL_COPY_PACKET, 40 ) < 0 )
      {
	M_PRINTF ( MLOG_INFO, "error in set_mode\n" );
      }
    if ( nfq_set_queue_maxlen ( globalqh_rest, 200 ) == -1 )
      {
	M_PRINTF ( MLOG_INFO, "error in queue_maxlen\n" );
      }
    nfqfd_rest = nfq_fd ( globalh_out_rest );
    M_PRINTF ( MLOG_DEBUG, "nfqueue handler registered\n" );
    //--------Done registering------------------





    //-----------------Register queue handler for INPUT chain-----
    globalh_in = nfq_open();
    if ( !globalh_in )
      {
	M_PRINTF ( MLOG_INFO, "error during nfq_open\n" );
      }
    if ( nfq_unbind_pf ( globalh_in, AF_INET ) < 0 )
      {
	M_PRINTF ( MLOG_INFO, "error during nfq_unbind\n" );
      }
    if ( nfq_bind_pf ( globalh_in, AF_INET ) < 0 )
      {
	M_PRINTF ( MLOG_INFO, "error during nfq_bind\n" );
      }
    struct nfq_q_handle * globalqh_input = nfq_create_queue ( globalh_in, NFQNUM_INPUT, &nfq_handle_in, NULL );
    if ( !globalqh_input )
      {
	M_PRINTF ( MLOG_INFO, "error in nfq_create_queue. Please make sure that any other instances of Leopard Flower are not running and restart the program. Exitting\n" );
	exit (0);
      }
    //copy only 40 bytes of packet to userspace - just to extract tcp source field
    if ( nfq_set_mode ( globalqh_input, NFQNL_COPY_PACKET, 40 ) < 0 )
      {
	M_PRINTF ( MLOG_INFO, "error in set_mode\n" );
      }
    if ( nfq_set_queue_maxlen ( globalqh_input, 30 ) == -1 )
      {
	M_PRINTF ( MLOG_INFO, "error in queue_maxlen\n" );
      }
    nfqfd_input = nfq_fd ( globalh_in );
    M_PRINTF ( MLOG_DEBUG, "nfqueue handler registered\n" );
    //--------Done registering------------------



}

void init_dlist()
{
    //initialze dlist first(reference) element
    if ( ( first_rule = ( dlist * ) malloc ( sizeof ( dlist ) ) ) == NULL )
      {
	M_PRINTF ( MLOG_INFO, "malloc: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
	die();
      }
    first_rule->prev = NULL;
    first_rule->next = NULL;

  #ifndef WITHOUT_SYSVIPC
    //initialze dlist copy's first(reference) element
    if ( ( copy_first = ( dlist * ) malloc ( sizeof ( dlist ) ) ) == NULL )
      {
	M_PRINTF ( MLOG_INFO, "malloc: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
	die();
      }
    copy_first->prev = NULL;
    copy_first->next = NULL;
  #endif
}

void open_proc_net_files()
{
    if ( ( tcpinfo = fopen ( TCPINFO, "r" ) ) == NULL )
      {
	M_PRINTF ( MLOG_INFO, "fopen: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
	exit (PROCFS_ERROR);
      }
    if ( ( tcp6info = fopen ( TCP6INFO, "r" ) ) == NULL )
      {
	M_PRINTF ( MLOG_INFO, "fopen: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
	exit (PROCFS_ERROR);
      }
    if ( ( udpinfo = fopen ( UDPINFO, "r" ) ) == NULL )
      {
	M_PRINTF ( MLOG_INFO, "fopen: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
	exit (PROCFS_ERROR);
      }
    if ( ( udp6info = fopen (UDP6INFO, "r" ) ) == NULL )
      {
	M_PRINTF ( MLOG_INFO, "fopen: %s,%s,%d\n", strerror ( errno ), __FILE__, __LINE__ );
	exit (PROCFS_ERROR);
      }
    procnetrawfd = open ( "/proc/net/raw", O_RDONLY );
    tcpinfo_fd = fileno(tcpinfo);
    tcp6info_fd = fileno(tcp6info);
    udpinfo_fd = fileno(udpinfo);
    udp6info_fd = fileno(udp6info);
}

int main ( int argc, char *argv[] )
{

#ifndef WITHOUT_SYSVIPC
  //argv[0] is the  path of the executable
  if ( argc >= 2 )
    {
      if (!strcmp (argv[1],"--cli")  || !strcmp(argv[1],"--gui") || !strcmp(argv[1],"--pygui"))
        {
          return frontend_mode ( argc, argv );
        }
    }
#endif

  if (argc == 2 && ( !strcmp(argv[1], "--help") || !strcmp(argv[1], "--version")))
    {
      parse_comline_args(argc, argv);
      return 0;
    }

  capabilities_setup();
  setuid_root();
  setgid_lpfwuser();
  setup_signal_handlers();

#ifndef WITHOUT_SYSVIPC
  save_own_path();
#endif

  parse_comline_args(argc, argv);
  init_log();
  pidfile_check();
  init_conntrack();
  init_iptables();

#ifdef DEBUG
  uid_t uid, euid;
  uid = getuid();
  euid = geteuid();
  printf (" orig uid euid %d %d \n", uid, euid);
#endif

  //enable CAP in effective set
  cap_t cap_current;
  cap_current = cap_get_proc();
  if (cap_current == NULL)
    {
      M_PRINTF(MLOG_INFO,"cap_get_proc: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
  const cap_value_t caps_list[] = {CAP_NET_ADMIN, CAP_DAC_READ_SEARCH, CAP_SYS_PTRACE};
  cap_set_flag(cap_current,  CAP_EFFECTIVE, 3, caps_list, CAP_SET);
  if (cap_set_proc(cap_current) == -1)
    {
      M_PRINTF(MLOG_INFO, "cap_get_proc: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }

#ifndef WITHOUT_SYSVIPC
  init_msgq();
#endif

  init_nfq_handlers();
  init_dlist();
  rules_load();
  open_proc_net_files();

  if (pthread_create ( &refresh_thread, NULL, refreshthread, NULL ) != 0) {perror ("pthread_create"); exit(0);}
  if (pthread_create ( &cachebuild_thread, NULL, cachebuildthread, NULL ) != 0) {perror ("pthread_create"); exit(0);}
  if (pthread_create ( &conntrack_export_thread, NULL, conntrackexporthread, NULL ) != 0) {perror ("pthread_create"); exit(0);}
  if (pthread_create ( &trafficdestroy_thread, NULL, trafficdestroythread, NULL ) != 0) {perror ("pthread_create"); exit(0);}
  if (pthread_create ( &ct_del_thread, NULL, ct_delthread, NULL )!= 0) {perror ("pthread_create"); exit(0);}

  if (pthread_create ( &nfqinput_thread, NULL, nfqinputthread, NULL) != 0) {perror ("pthread_create"); exit(0);}
  if (pthread_create ( &nfqout_udp_thread, NULL, nfqoutudpthread, NULL) != 0) {perror ("pthread_create"); exit(0);}
  if (pthread_create ( &nfqout_rest_thread, NULL, nfqoutrestthread, NULL) != 0) {perror ("pthread_create"); exit(0);}

#ifdef DEBUG
  pthread_create ( &rulesdump_thread, NULL, rulesdumpthread, NULL );

  if (argc > 1 && !strcmp (argv[1], "--test"))
    {
      pthread_create ( &unittest_thread, NULL, run_tests, NULL );
    }
#endif

  //endless loop of receiving packets and calling a handler on each packet
  int rv;
  char buf[4096] __attribute__ ( ( aligned ) );
  while ( ( rv = recv ( nfqfd_tcp, buf, sizeof ( buf ), 0 ) ) && rv >= 0 )
    {
      nfq_handle_packet ( globalh_out_tcp, buf, rv );
    }
}
// kate: indent-mode cstyle; space-indent on; indent-width 4;
