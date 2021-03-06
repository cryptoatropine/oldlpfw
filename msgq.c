#include <sys/ipc.h>
#include <syslog.h>
#include <string.h>
#include <stdlib.h> //for exit
#include <pthread.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <errno.h>
#include <malloc.h>
#include <dirent.h> //for ino_t
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/capability.h>
#include "argtable/argtable2.h"
#include "common/defines.h"
#include "common/includes.h"
#include "lpfw.h"
#include "msgq.h"

#define M_PRINTF(loglevel, ...) \
    pthread_mutex_lock(&logstring_mutex); \
    snprintf (logstring, PATHSIZE, __VA_ARGS__); \
    m_printf (loglevel, logstring); \
    pthread_mutex_unlock(&logstring_mutex); \
 
int awaiting_reply_from_fe;
int mqd_d2ftraffic;


//message queue id - communication link beteeen daemon and frontend
int mqd_d2f, mqd_f2d, mqd_d2flist, mqd_d2fdel, mqd_creds, mqd_d2ftraffic;
struct msqid_ds *msgqid_d2f, *msgqid_f2d, *msgqid_d2flist, *msgqid_d2fdel, *msgqid_creds, *msgqid_d2ftraffic;

pthread_t command_thread, regfrontend_thread;

//flag to show that frontend is already processing some "add" query
int awaiting_reply_from_fe = FALSE;
//struct of what was sent to f.e.dd
dlist sent_to_fe_struct;

// register frontend when "lpfw --cli" is invoked.The thread is restarted by invoking pthread_create
// towards the end of it
void*  fe_reg_thread(void* ptr)
{
  ptr = 0;
  //TODO: Paranoid anti spoofing measures: only allow one msg_struct_creds packet on the queue first get the current struct

  //block until message is received
interrupted:
  if (msgrcv(mqd_creds, &msg_creds, sizeof (msg_struct_creds), 0, 0) == -1)
    {
      M_PRINTF(MLOG_DEBUG, "msgrcv: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
      goto interrupted;
    }
  //extract last sender's PID and check the binary path is the same path as this lpfw instance
  msgctl(mqd_creds, IPC_STAT, msgqid_creds);
  pid_t pid;
  char procpath[32] = "/proc/";
  char exepath[PATHSIZE];
  char pidstring[8];
  pid = msgqid_creds->msg_lspid;
  sprintf(pidstring, "%d", (int)pid); //convert int to char*
  strcat(procpath, pidstring);
  strcat(procpath, "/exe");
  memset(exepath, 0, PATHSIZE);

  //lpfw --cli sleeps only 3 secs, after which procpath isnt available, so no breakpoints before
  //the next line
  if (readlink(procpath, exepath, PATHSIZE - 1) == -1)
    {
      M_PRINTF(MLOG_INFO, "readlink: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
#ifdef DEBUG
  printf("%s, %s\n",  exepath, ownpath);
#endif
  if (strcmp(exepath, ownpath))
    {
      M_PRINTF(LOG_ALERT, "Can't start frontend because it's not located in the sane folder as lpfw\n");
      return ;
    }
  //The following checks are already performed by frontend_register(). This is redundant, but again, those hackers are unpredictable
#ifndef DEBUG
  if (msg_creds.creds.uid  == 0)
    {
      M_PRINTF (LOG_INFO, "You are trying to run lpfw's frontend as root. Such possibility is disabled due to security reasons. Please rerun as an unpriviledged user\n");
      return ;
    }
#endif

  if (!strncmp(msg_creds.creds.tty, "/dev/tty", 8))
    {
      M_PRINTF (LOG_INFO, "You are trying to run lpfw's frontend from a tty terminal. Such possibility is disabled in this version of lpfw due to security reasons. Try to rerun this command from within an X terminal\n");
      return ;
    }

  //fork, setuid exec xterm and wait for its termination
  //probably some variables become unavailable in child
  pid_t child_pid;
  child_pid =  fork();
  if (child_pid == 0)  //child process
    {
      child_close_nfqueue();
      /* no need to setgid on child since gid==lpfwuser is inherited from parent
      if (setgid(lpfwuser_gid) == -1)
      {
                M_PRINTF(MLOG_INFO, "setgid: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
      }
      */

      //enable CAP_SETUID in effective set
      cap_t cap_current;
      cap_current = cap_get_proc();
      if (cap_current == NULL)
        {
          M_PRINTF(MLOG_INFO, "cap_get_proc: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
        }
      const cap_value_t caps_list[] = {CAP_SETUID};
      cap_set_flag(cap_current,  CAP_EFFECTIVE, 1, caps_list, CAP_SET);
      if (cap_set_proc(cap_current) == -1)
        {
          M_PRINTF(MLOG_INFO, "cap_get_proc: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
        }
      //setuid and immediately remove CAP_SETUID from both perm. and eff. sets
      if (setuid(msg_creds.creds.uid) == -1)
        {
          M_PRINTF(MLOG_INFO, "setuid: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
        }
      //no need to drop privs, they are all zeroed out upon setuid()

      struct stat path_stat;

      /* lpfwcli is now started independently, keep this just in case

      //check that frontend file exists and launch it
      if (!strcmp (msg_creds.creds.params[0], "--cli"))
        {
          if (stat(cli_path->filename[0], &path_stat) == -1 )
            {
              M_PRINTF(MLOG_INFO, "stat: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
              if (errno == ENOENT)
                {
                  M_PRINTF(MLOG_INFO, "Unable to find %s\n", cli_path->filename[0]);
                }
              return;
            }

          //6th arg here should be pathtofrontend
          execl("/usr/bin/xterm", "/usr/bin/xterm", "-display", msg_creds.creds.display,
                "+hold",
                "-e", cli_path->filename[0],"magic_number",
                msg_creds.creds.params[1][0]?msg_creds.creds.params[2]:(char*)0, //check if there are any parms and if yes,process the first one
                msg_creds.creds.params[3][0]?msg_creds.creds.params[3]:(char*)0, //check if the parm is the last one
                msg_creds.creds.params[4][0]?msg_creds.creds.params[4]:(char*)0,
                msg_creds.creds.params[5][0]?msg_creds.creds.params[5]:(char*)0,
                (char*)0);
          //if exec returns here it means there was an error
          M_PRINTF(MLOG_INFO, "execl: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
        }
	*/
      if (!strcmp (msg_creds.creds.params[0], "--gui"))
        {
          if (stat(gui_path->filename[0], &path_stat) == -1 )
            {
              M_PRINTF(MLOG_INFO, "stat: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
              if (errno == ENOENT)
                {
                  M_PRINTF(MLOG_INFO, "Unable to find %s\n", gui_path->filename[0]);
                }
              return;

            }
          execl (gui_path->filename[0], gui_path->filename[0], (char*)0);
          //if exec returns here it means there was an error
          M_PRINTF(MLOG_INFO, "execl: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
        }
      else if (!strcmp (msg_creds.creds.params[0], "--pygui"))
        {
          if (stat(pygui_path->filename[0], &path_stat) == -1 )
            {
              M_PRINTF(MLOG_INFO, "stat: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
              if (errno == ENOENT)
                {
                  M_PRINTF(MLOG_INFO, "Unable to find %s\n", pygui_path->filename[0]);
                }
              return;
            }
          execl ("/usr/bin/python", "python",pygui_path->filename[0], (char*)0);
          //if exec returns here it means there was an error
          M_PRINTF(MLOG_INFO, "execl: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);

        }
    }
  if (child_pid > 0)  //parent
    {
      int status;
      //wait until chils terminates
      if (wait(&status) == (pid_t)-1)
        {
          perror("wait");
        }
      //frontend should unregister itself upon exit, else it's crashed
      if (fe_active_flag_get())
        {
          M_PRINTF(MLOG_INFO, "Frontend apparently crashed, unregistering...\n");
	  awaiting_reply_from_fe = FALSE;
          fe_active_flag_set(FALSE);
        }
      M_PRINTF(MLOG_INFO, "frontend exited\n");
      pthread_create(&regfrontend_thread, NULL, fe_reg_thread, NULL);
      return;

    }
  if (child_pid == -1)
    {
      perror("fork");
    }

}

// wait for commands from frontend
void* commandthread(void* ptr)
{
  ptr = 0;
  dlist *temp;

  // N.B. continue statement doesn't apply to switch it causes to jump to while()
  while (1)
    {
      //block until message is received from frontend:
interrupted:
      if (msgrcv(mqd_f2d, &msg_f2d, sizeof (msg_struct), 0, 0) == -1)
        {
          M_PRINTF(MLOG_DEBUG	, "msgrcv: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
          sleep(1); //avoid overwhelming the log
	  goto interrupted;
        }

#ifdef DEBUG
      struct timeval time_struct;
      gettimeofday(&time_struct, NULL);
      M_PRINTF(MLOG_DEBUG, "Received command %d @ %d %d\n", msg_f2d.item.command, (int) time_struct.tv_sec, (int) time_struct.tv_usec);
#endif

      switch (msg_f2d.item.command)
        {
        case F2DCOMM_LIST:
          ;
          //TODO a memory leak here, because dlist_copy mallocs memory that is never freed
          temp = (dlist *) dlist_copy();

          temp = temp->next;
          //check if the list is empty and let frontend know
          if (temp == NULL)
            {
              strcpy(msg_d2flist.item.path, "EOF");
              if (msgsnd(mqd_d2flist, &msg_d2flist, sizeof (msg_struct), 0) == -1)
                {
                  M_PRINTF(MLOG_INFO, "msgsnd: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
                }
              M_PRINTF(MLOG_DEBUG, "sent EOF\n");
              continue;
            }
          while (temp != NULL)
            {
              strcpy(msg_d2flist.item.path, temp->path);
              strcpy(msg_d2flist.item.pid, temp->pid);
              strcpy(msg_d2flist.item.perms, temp->perms);
              msg_d2flist.item.is_active = temp->is_active;
              msg_d2flist.item.nfmark_out = temp->nfmark_out;
              if (msgsnd(mqd_d2flist, &msg_d2flist, sizeof (msg_struct), 0) == -1)
                {
                  M_PRINTF(MLOG_INFO, "msgsnd: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
                }
              if (temp->next == NULL)
                {
                  strcpy(msg_d2flist.item.path, "EOF");
                  if (msgsnd(mqd_d2flist, &msg_d2flist, sizeof (msg_struct), 0) == -1)
                    {
                      M_PRINTF(MLOG_INFO, "msgsnd: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
                    }
                  break;
                }
              temp = temp->next;
            };
          continue;

        case F2DCOMM_DELANDACK:
          dlist_del(msg_f2d.item.path, msg_f2d.item.pid);
          continue;

        case F2DCOMM_WRT:
#ifdef DEBUG
          gettimeofday(&time_struct, NULL);
          M_PRINTF(MLOG_DEBUG, "Before writing  @%d %d\n", (int) time_struct.tv_sec, (int) time_struct.tv_usec);
#endif
          rulesfileWrite();
#ifdef DEBUG
          gettimeofday(&time_struct, NULL);
          M_PRINTF(MLOG_DEBUG, "After  writing @ %d %d\n", (int) time_struct.tv_sec, (int) time_struct.tv_usec);
#endif
          continue;

        case F2DCOMM_ADD:
          ;
          if (!strcmp(msg_f2d.item.perms,"IGNORED"))
            {
	      awaiting_reply_from_fe = FALSE;
              continue;
            }
#ifdef DEBUG
          gettimeofday(&time_struct, NULL);
          M_PRINTF(MLOG_DEBUG, "Before adding  @%d %d\n", (int) time_struct.tv_sec, (int) time_struct.tv_usec);
#endif

          //TODO come up with a way to calculate sha without having user to wait when the rule appears
          //chaeck if the app is still running

          if (!strcmp(msg_f2d.item.path, KERNEL_PROCESS))  //don't set fe_awaiting_reply flags
            {
              dlist_add(KERNEL_PROCESS, msg_f2d.item.pid, msg_f2d.item.perms, TRUE, "", 0, 0, 0 ,TRUE);
              continue;
            }

          char exepath[32] = "/proc/";
          strcat(exepath, sent_to_fe_struct.pid);
          strcat(exepath, "/exe");
          char exepathbuf[PATHSIZE];
          memset ( exepathbuf, 0, PATHSIZE );
          readlink (exepath, exepathbuf, PATHSIZE-1 );
          if (strcmp(exepathbuf, sent_to_fe_struct.path))
            {
              M_PRINTF(MLOG_INFO, "Frontend asked to add a process that is no longer running,%s,%d\n", __FILE__, __LINE__);
	      awaiting_reply_from_fe = FALSE;
              continue;
            }

          //if perms are *ALWAYS we need both exesize and sha512
          char sha[DIGEST_SIZE] = "";
          struct stat exestat;
          if (!strcmp(msg_f2d.item.perms,ALLOW_ALWAYS) || !strcmp(msg_f2d.item.perms,DENY_ALWAYS))
            {

              //Calculate the size of the executable
              if (stat(sent_to_fe_struct.path, &exestat) == -1 )
                {
                  M_PRINTF(MLOG_INFO, "stat: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
                }

              //Calculate sha of executable
              FILE *stream;
              memset(sha, 0, DIGEST_SIZE+1);
              stream = fopen(sent_to_fe_struct.path, "r");
              sha512_stream(stream, (void *) sha);
              fclose(stream);
            }
          //check if we were really dealing with the correct process all along
          unsigned long long stime;
          stime = starttimeGet ( atoi ( sent_to_fe_struct.pid ) );
          if ( sent_to_fe_struct.stime != stime )
            {
              M_PRINTF ( MLOG_INFO, "Red alert!!!Start times don't match %s %s %d", temp->path,  __FILE__, __LINE__ );
	      awaiting_reply_from_fe = FALSE;
              continue;
            }

//TODO SECURITY. We should check now that /proc/PID inode wasn't changed while we were shasumming and exesizing

          dlist_add(sent_to_fe_struct.path, sent_to_fe_struct.pid, msg_f2d.item.perms, TRUE, sha, sent_to_fe_struct.stime, exestat.st_size, 0 ,TRUE);
#ifdef DEBUG
          gettimeofday(&time_struct, NULL);
          M_PRINTF(MLOG_DEBUG,"After  adding @ %d %d\n", (int) time_struct.tv_sec, (int) time_struct.tv_usec);
#endif
	  awaiting_reply_from_fe = FALSE;
          continue;

        case F2DCOMM_REG:
          if (fe_active_flag_get())
            {
              M_PRINTF(MLOG_ALERT, "Red alert!!! There was an attempt to register a frontend when one is already active\n");
              continue;
            }
	  struct msqid_ds msqid_f2d;
	  if (msgctl(mqd_f2d, IPC_STAT, &msqid_f2d) == -1)
	  {
	      M_PRINTF(MLOG_DEBUG, "msgctl: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
	  }
	  fe_pid = msqid_f2d.msg_lspid;
	  fe_active_flag_set(TRUE);
          M_PRINTF(MLOG_INFO, "Registered frontend\n");
          continue;

        case F2DCOMM_UNREG:
          if (!fe_active_flag_get())
            {
              M_PRINTF(MLOG_ALERT, "Red alert!!! There was an attempt to unregister a frontend when none is active\n");
              continue;
            }
          fe_active_flag_set(FALSE);
	  awaiting_reply_from_fe = FALSE;
          M_PRINTF(MLOG_INFO, "Unregistered frontend\n");
          continue;

        default:
          M_PRINTF(MLOG_INFO, "unknown command in commandthread \n");
        }
    }
}

void init_msgq()
{

  msgqid_d2f = malloc(sizeof (struct msqid_ds));
  msgqid_f2d = malloc(sizeof (struct msqid_ds));
  msgqid_d2flist = malloc(sizeof (struct msqid_ds));
  //msgqid_d2fdel = malloc(sizeof (struct msqid_ds)); //not in use
  msgqid_creds = malloc(sizeof (struct msqid_ds));
  msgqid_d2ftraffic = malloc(sizeof (struct msqid_ds));

  //TODO some capabilities may be needed here, in cases when TMPFILE was created by a different user
  // or message queue with the same ID was created by a different user. Needs investigation.

  key_t ipckey_d2f, ipckey_f2d, ipckey_d2flist, ipckey_d2fdel, ipckey_creds, ipckey_d2ftraffic;
  if (remove(TMPFILE) != 0)
    {
      M_PRINTF(MLOG_DEBUG, "remove: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
  if (creat(TMPFILE,

#ifdef DEBUG       //make world readable to avoid permission cock-ups during debugging
            0666
#else
	    0660 //lpfwuser group members may RDWR
#endif

           ) == 1)
    {
      M_PRINTF(MLOG_INFO, "creat: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
  //-----------------------------------
  if ((ipckey_d2f = ftok(TMPFILE, FTOKID_D2F)) == -1)
    {
      M_PRINTF(MLOG_INFO, "ftok: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
  M_PRINTF(MLOG_DEBUG, "D2FKey: %d\n", ipckey_d2f);

  if ((ipckey_f2d = ftok(TMPFILE, FTOKID_F2D)) == -1)
    {
      M_PRINTF(MLOG_INFO, "ftok: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
  M_PRINTF(MLOG_DEBUG, "Key: %d\n", ipckey_f2d);

  if ((ipckey_d2flist = ftok(TMPFILE, FTOKID_D2FLIST)) == -1)
    {
      M_PRINTF(MLOG_INFO, "ftok: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
  M_PRINTF(MLOG_DEBUG, "Key: %d\n", ipckey_d2flist);

  if ((ipckey_d2fdel = ftok(TMPFILE, FTOKID_D2FDEL)) == -1)
    {
      M_PRINTF(MLOG_INFO, "ftok: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
  M_PRINTF(MLOG_DEBUG, "Key: %d\n", ipckey_d2fdel);

  if ((ipckey_creds = ftok(TMPFILE, FTOKID_CREDS)) == -1)
    {
      M_PRINTF(MLOG_INFO, "ftok: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
  M_PRINTF(MLOG_DEBUG, "Key: %d\n", ipckey_creds);

  if ((ipckey_d2ftraffic = ftok(TMPFILE, FTOKID_D2FTRAFFIC)) == -1)
    {
      M_PRINTF(MLOG_INFO, "ftok: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
  M_PRINTF(MLOG_DEBUG, "Key: %d\n", ipckey_d2ftraffic);

  /* Set up the message queue to communicate between daemon and GUI*/
  //we need to first get the Qid, then use this id to delete Q
  //then create it again, thus ensuring the Q is cleared

//WORLD_ACCESS perms on msgq to facilitate debugging
#define GROUP_ACCESS 0660
#define WORLD_ACCESS 0666
#define OTHERS_ACCESS 0662 //write to msgq

  int perm_bits, creds_bits;

#ifdef DEBUG
  perm_bits = WORLD_ACCESS;
  creds_bits = WORLD_ACCESS;
#else
  perm_bits = GROUP_ACCESS;
  creds_bits = OTHERS_ACCESS;
#endif

//creds_bits require special treatment b/c when user launches ./lpfw --gui, we don't know in advance
//what the user's UID is. So we allow any user to invoke the frontend.

  if ((mqd_d2f = msgget(ipckey_d2f, IPC_CREAT | perm_bits)) == -1)
    {
      M_PRINTF(MLOG_INFO, "msgget: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
  //remove queue
  msgctl(mqd_d2f, IPC_RMID, 0);
  //create it again
  if ((mqd_d2f = msgget(ipckey_d2f, IPC_CREAT | perm_bits)) == -1)
    {
      M_PRINTF(MLOG_INFO, "msgget: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
  M_PRINTF(MLOG_DEBUG, "Message identifier %d\n", mqd_d2f);
  //----------------------------------------------------
  if ((mqd_d2flist = msgget(ipckey_d2flist, IPC_CREAT | perm_bits)) == -1)
    {
      M_PRINTF(MLOG_INFO, "msgget: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
  //remove queue
  msgctl(mqd_d2flist, IPC_RMID, 0);
  //create it again
  if ((mqd_d2flist = msgget(ipckey_d2flist, IPC_CREAT | perm_bits)) == -1)
    {
      M_PRINTF(MLOG_INFO, "msgget: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
  M_PRINTF(MLOG_DEBUG, "Message identifier %d\n", mqd_d2flist);

  //---------------------------------------------------------

  if ((mqd_f2d = msgget(ipckey_f2d, IPC_CREAT | perm_bits)) == -1)
    {
      M_PRINTF(MLOG_INFO, "msgget: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
  //remove queue
  msgctl(mqd_f2d, IPC_RMID, 0);
  //create it again
  if ((mqd_f2d = msgget(ipckey_f2d, IPC_CREAT | perm_bits)) == -1)
    {
      M_PRINTF(MLOG_INFO, "msgget: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
  M_PRINTF(MLOG_DEBUG, "Message identifier %d\n", mqd_f2d);

  //------------------------------------------------------
  if ((mqd_d2fdel = msgget(ipckey_d2fdel, IPC_CREAT | perm_bits)) == -1)
    {
      M_PRINTF(MLOG_INFO, "msgget: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
  //remove queue
  msgctl(mqd_d2fdel, IPC_RMID, 0);
  //create it again
  if ((mqd_d2fdel = msgget(ipckey_d2fdel, IPC_CREAT | perm_bits)) == -1)
    {
      M_PRINTF(MLOG_INFO, "msgget: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
  M_PRINTF(MLOG_DEBUG, "Message identifier %d\n", mqd_d2fdel);

  //------------------------------------------------------
  //This particular message queue should be writable by anyone, hence permission 0002
  //because we don't know in advance what user will be invoking the frontend

  if ((mqd_creds = msgget(ipckey_creds, IPC_CREAT | creds_bits)) == -1)
    {
      M_PRINTF(MLOG_INFO, "msgget: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
  //remove queue
  msgctl(mqd_creds, IPC_RMID, 0);
  //create it again
  if ((mqd_creds = msgget(ipckey_creds, IPC_CREAT | creds_bits)) == -1)
    {
      M_PRINTF(MLOG_INFO, "msgget: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
  M_PRINTF(MLOG_DEBUG, "Creds msgq id %d\n", mqd_creds);

  //-------------------------------------------------

  if ((mqd_d2ftraffic = msgget(ipckey_d2ftraffic, IPC_CREAT | perm_bits)) == -1)
    {
      M_PRINTF(MLOG_INFO, "msgget: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
  //remove queue
  msgctl(mqd_d2ftraffic, IPC_RMID, 0);
  //create it again
  if ((mqd_d2ftraffic = msgget(ipckey_d2ftraffic, IPC_CREAT | perm_bits)) == -1)
    {
      M_PRINTF(MLOG_INFO, "msgget: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
  M_PRINTF(MLOG_DEBUG, "Traffic msgq id %d\n", mqd_d2ftraffic);

  //------------------------------------------------------------

  pthread_create(&command_thread, NULL, commandthread, NULL);
  pthread_create(&regfrontend_thread, NULL, fe_reg_thread, NULL);

}

//obsolete func
int notify_frontend(int command, char *path, char *pid, unsigned long long stime)
{

  switch (command)
    {
    case D2FCOMM_ASK_OUT:
      //prepare a msg and send it to frontend
      strcpy(msg_d2f.item.path, path);
      strcpy(msg_d2f.item.pid, pid);
      msg_d2f.item.stime = stime;
      msg_d2f.item.command = D2FCOMM_ASK_OUT;
      //pthread_mutex_lock(&mutex_msgq);
      if (msgsnd(mqd_d2f, &msg_d2f, sizeof (msg_struct), IPC_NOWAIT) == -1)
        {
          M_PRINTF(MLOG_INFO, "msgsnd: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
        }
      return 4;

    case D2FCOMM_LIST:
      msg_d2f.item.command = D2FCOMM_LIST;
      if (msgsnd(mqd_d2f, &msg_d2f, sizeof (msg_struct), IPC_NOWAIT) == -1)
        {
          M_PRINTF(MLOG_INFO, "msgsnd: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
        }
      return -1;
    }
}

//Ask frontend
int  fe_ask_out(char *path, char *pid, unsigned long long *stime)
{
  if (pthread_mutex_trylock(&msgq_mutex) != 0) return FRONTEND_BUSY;
  if (awaiting_reply_from_fe)
    {
      if (pthread_mutex_unlock(&msgq_mutex)) perror ("mutexunlock");
      return FRONTEND_BUSY;
    }

  //first remember what we are sending
  strcpy(sent_to_fe_struct.path, path);
  strcpy(sent_to_fe_struct.pid, pid);
  sent_to_fe_struct.stime = *stime;

  //prepare a msg and send it to frontend
  strcpy(msg_d2f.item.path, path);
  strcpy(msg_d2f.item.pid, pid);
  msg_d2f.item.command = D2FCOMM_ASK_OUT;
  if (msgsnd(mqd_d2f, &msg_d2f, sizeof (msg_struct), IPC_NOWAIT) == -1)
    {
      M_PRINTF(MLOG_INFO, "msgsnd: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
  awaiting_reply_from_fe = TRUE;
  if (pthread_mutex_unlock(&msgq_mutex)) perror ("mutexunlock");
  return SENT_TO_FRONTEND;
}

//Ask frontend if new incoming connection should be allowed
int fe_ask_in(char *path, char *pid, unsigned long long *stime, char *ipaddr, int sport, int dport)
{
  if (pthread_mutex_trylock(&msgq_mutex) != 0) return FRONTEND_BUSY;
  if (awaiting_reply_from_fe)
    {
      if (pthread_mutex_unlock(&msgq_mutex)) perror ("mutexunlock");
      return FRONTEND_BUSY;
    }

  //first remember what we are sending
  strcpy(sent_to_fe_struct.path, path);
  strcpy(sent_to_fe_struct.pid, pid);
  sent_to_fe_struct.stime = *stime;

  //prepare a msg and send it to frontend
  strcpy(msg_d2f.item.path, path);
  strcpy(msg_d2f.item.pid, pid);
  msg_d2f.item.command = D2FCOMM_ASK_IN;
  //the following fields of struct will be simply re-used. Not nice, but what's wrong with re-cycling?
  strncpy(msg_d2f.item.perms, ipaddr, sizeof(msg_d2f.item.perms));
  msg_d2f.item.stime = sport;
  msg_d2f.item.inode = dport;

  if (msgsnd(mqd_d2f, &msg_d2f, sizeof (msg_struct), IPC_NOWAIT) == -1)
    {
      M_PRINTF(MLOG_INFO, "msgsnd: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
  awaiting_reply_from_fe = TRUE;
  if (pthread_mutex_unlock(&msgq_mutex)) perror ("mutexunlock");
  return SENT_TO_FRONTEND;
}

int fe_list()
{
  msg_d2f.item.command = D2FCOMM_LIST;
  if (msgsnd(mqd_d2f, &msg_d2f, sizeof (msg_struct), IPC_NOWAIT) == -1)
    {
      M_PRINTF(MLOG_INFO, "msgsnd: %s,%s,%d\n", strerror(errno), __FILE__, __LINE__);
    }
}
