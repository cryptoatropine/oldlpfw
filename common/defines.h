#define TRUE 1
#define FALSE 0

#define TYPE_D2FLIST  100
#define MSGQNUM_D2F_CHAR 101
#define MSGQNUM_D2F_INT  102
#define MSGQNUM_F2D_CHAR 1001
#define MSGQNUM_F2D_INT  1002

#define PATHSIZE 1024 //length of IPC message. It is usually the path to the program that is sent in IPC messages.
#define PIDLENGTH 16
#define PERMSLENGTH 16
#define SOCKETBUFSIZE 32 // entries in /proc/<pid>/fd are in the form of socket:[1234567] - max length 16 chars
#define NFQNUM_OUTPUT 11220 //arbitrary number used for iptables rule
#define NFQNUM_INPUT 11221
#define MAX_LINE_LENGTH 1024 //max lc ength of a line in configfile/rulesfile
#define DIGEST_SIZE 64
#define TTYNAME 16
#define DISPLAYNAME 32
#define NFMARK_BASE 11331 //netfilter marks to be put on packets start with this base number (to avoid possible collision with other programs that use netfilter's marks
#define MEMBUF_SIZE 65536*3 //buffer size to fread() /proc/net/tcp*,udp*
#define MAX_CACHE 1024*2 //maximum number of /proc/net/* sockets to put in cache
#define CACHE_EOL_MAGIC 38

#define TMPFILE "/tmp/lpfw" //a file is needed to create IPC key for daemon <> frontend message queue
#define LPFWCLI_LOG "/tmp/lpfwcli.log"
#define PIDFILE "/tmp/lpfw.pid"
#define RULESFILE "/etc/lpfw.rules"
#define LPFW_LOGFILE "/tmp/lpfw.log"

#define TCPINFO "/proc/net/tcp"
#define UDPINFO "/proc/net/udp"
#define TCP6INFO "/proc/net/tcp6"
#define UDP6INFO "/proc/net/udp6"
#define ICMPINFO "/proc/net/raw"

#define ALLOW_ONCE "ALLOW ONCE"
#define ALLOW_ALWAYS "ALLOW ALWAYS"
#define DENY_ONCE "DENY ONCE"
#define DENY_ALWAYS "DENY ALWAYS"
#define KERNEL_PROCESS "KERNEL PROCESS"

#define MLOG_INFO 1
#define MLOG_TRAFFIC 2
#define MLOG_DEBUG 3
#define MLOG_DEBUG2 4
#define MLOG_ALERT 5
#define MLOG_ERROR 6
#define MLOG_DEBUG3 7

// max height to which windows can be stretched
#define UWMAX 1
#define TWMAX 1
#define SWMAX 1
#define HWMAX 1
// kate: indent-mode cstyle; space-indent on; indent-width 4; 