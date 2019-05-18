/*
 * updaterd.c
 *
 *  Created on: Apr 12, 2017
 *      Author: Jie Yan 
 */
#include <unistd.h> //*symbolic constants*:should always be the FIRST to include?
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <sys/socket.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <execinfo.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <syslog.h>

//POSIX message queue includes
#include <fcntl.h> /* Defines O_* constants */
#include <sys/stat.h> /* Defines mode constants */
#include <mqueue.h>

//include this header for machine-dependent debugging information
#ifndef __x86_64__
#include <ucontext.h>
#endif

#include "updaterd.h"
#include "cJSON.h"

//Macros
#define	MODULE_NAME "SysUpdater"
#define UPDATER_VERSION "updater-1.0.0"
#define UPDATER_NAME "updater.socket"
#define UPDATER_MAX_CON 10

#define UPDATER_READ_AVAILABLE (1<<0)
#define UPDATER_WRITE_AVAILABLE (1<<1)
#define UPDATER_NOTIFY_AVAILABLE (1<<2)

#define TRUE 1
#define FALSE 0
#define MAX_MSG_LENGTH 1024
#define MAX_CMD_LENGTH 256
#define MAX_FILES 2
#define MAX_FILE_PATH_LENGTH 128
#define MAX_FILE_NAME_LENGTH 64
#define MAX_CPY_BUF_LENGTH 2048
#define MAX_BACKTRACE_DEPTH 32
#define UPDATER_DEFAULT_TIME_LIMIT 30
#define UPDATER_TIME_LIMIT_ENV_NAME "UPDATER_TIME_LIMIT"

//message queue macro defines
#define	FILE_MODE (S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH) //filtered by umask
#define READ_FLAG (O_RDONLY | O_CREAT | O_EXCL)
#define WRITE_FLAG (O_WRONLY | O_CREAT | O_EXCL)
#define MAX_MESSAGE_QUEUE_MSG_LEN sizeof(msg_params)
#define MAX_MESSAGE_QUEUE_MSG_NUM 60
#define UPDATER_MQ_READ_NAME "/param_mq110.mq"
#define UPDATER_MQ_WRITE_NAME "/param_mq119.mq"

#define REPORT_ERR(msg, args...) syslog(LOG_ERR,"%s:#%d %s"msg,__FILE__, __LINE__, ##args);
#define REPORT_INFO(msg, args...) syslog(LOG_INFO,"%s:#%d %s"msg,__FILE__, __LINE__, ##args);
#define MAX(a,b) ({const typeof(a) __a = a;const typeof(b) __b = b; __a > __b ? __a : __b;})

//global type defines
typedef struct updater_state_t{
	//updater related
	const char* updater_verison;
	updater_status updater_state;

	//processing info
	updater_type updater_type;
	int updater_rate;
	const char* updater_description;

	//interfacing
	int serv_socket_fd;
	mqd_t serv_msg_in_fd;
	mqd_t serv_msg_out_fd;

	//worker thread or process
	int priority;
	pthread_t exe_pthread_id;
	pid_t exe_process_id;

}updater_state_t;

//global variables
updater_state_t updater_cur_state;

//internal type declaration
typedef enum{
	FILE_TYPE_GZ = 0,
	FILE_TYPE_BZ2,
	FILE_TYPE_XZ,
	FILE_TYPE_TXT,
	FILE_TYPE_INVALID
}file_type;

//internal functions declarations
/* init functions */
static int updater_proc_init(void);
static int updater_proc_init_handler(void);
static int updater_proc_init_state(void);
static int updater_proc_init_socket(void);
static int updater_proc_init_msgQ(void);

/* utility functions*/
static char* getDescription(updater_status state);
static void setCurState(updater_status state);
static void setCurStateJson(cJSON* state);
static cJSON* exec_json(const char*json_str);
static cJSON* forge_resp(updater_response resp);
static cJSON* forge_brief_status(void);
static cJSON* forge_verbose_status(void);
static int check_file_existence(char* path);
static int check_same_directory(char* path1,char* path2);
static cJSON* process_update_cmd(cJSON* cmd_json);
static long get_file_size(char* path);
static void* updater_worker_thread(void* arg);
static void update_rate(char*src,char*dst);
static int cp_file_tmp2final(char* dst_dir, char* file);
static void sig_handler(int signo, siginfo_t* siginfo,void* context);
static void cleanup(void);

int main(int argc,char** argv){
	int result = -1;
	unsigned int msg_prio = 0;
	int on_exit = FALSE;
	cJSON* resp_msg = NULL;
	msg_params msg_back;
	char* json_string;
	fd_set rd_fd_set;
	int recv_len;
	char recv_buffer[MAX_MSG_LENGTH];
	char send_buffer[MAX_MSG_LENGTH];
	struct sockaddr_un recv_addr;
	struct stat fd_stat;
	pid_t pid;
	struct sigaction sa;
	int fd0,fd1,fd2;
	socklen_t recv_addr_len = sizeof(recv_addr);
	
	/* Initialize the log file */
	openlog(MODULE_NAME, LOG_CONS|LOG_PID, LOG_DAEMON);

	/* fork to go into the background */
	if((pid = fork()) < 0 ){
		REPORT_ERR("failed to fork a new process~!");
		exit(pid);
	} else if(pid > 0){
		//this is parent
		exit(0);
	}
	/* becomes a session leader to lose controlling TTY */
	setsid();
	
	/* ensure future opens won’t allocate controlling TTYs. */
	sa.sa_handler = SIG_IGN;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	if (sigaction(SIGHUP, &sa, NULL) < 0){
		REPORT_ERR("can’t ignore SIGHUP~!");
	}
	
	/* change the current working directory to the root */
	if (chdir("/") < 0){
		REPORT_ERR("cannot change the current directory to the root~!");
	}

	/* close all open file descriptors */
	close(0);
	close(1);
	close(2);

	/* attach file descriptors 0, 1, and 2 to /dev/null */
	fd0 = open("/dev/null", O_RDWR);
	fd1 = dup(0);
	fd2 = dup(0);

	/* Initialize the log file */
	openlog(MODULE_NAME, LOG_CONS|LOG_PID, LOG_DAEMON);

	result = updater_proc_init();
	if(result == -1){
		REPORT_ERR("cannot initialize updater!");
		exit(result);
	}

	FD_ZERO(&rd_fd_set);
	while(!on_exit){
		if(updater_cur_state.serv_socket_fd != -1){
			fstat(updater_cur_state.serv_socket_fd,&fd_stat);
			fcntl(updater_cur_state.serv_socket_fd,F_SETFL,fd_stat.st_mode | O_NONBLOCK);
			FD_SET(updater_cur_state.serv_socket_fd,&rd_fd_set);
		}
		if(updater_cur_state.serv_msg_in_fd != -1){
			fstat(updater_cur_state.serv_msg_in_fd,&fd_stat);
			fcntl(updater_cur_state.serv_msg_in_fd,F_SETFL,fd_stat.st_mode | O_NONBLOCK);
			FD_SET(updater_cur_state.serv_msg_in_fd,&rd_fd_set);
		}
		result = select(MAX(updater_cur_state.serv_socket_fd,updater_cur_state.serv_msg_in_fd)+1,&rd_fd_set,NULL,NULL,NULL);
		if(result == -1){
			if(errno == EINTR){
				continue;
			}else{
				REPORT_ERR(" %s",strerror(errno));
				on_exit = TRUE;
				continue;
			}
		}
		if(FD_ISSET(updater_cur_state.serv_socket_fd,&rd_fd_set)){
			recv_len = recvfrom(updater_cur_state.serv_socket_fd, recv_buffer, MAX_MSG_LENGTH,0,
							(struct sockaddr*)&recv_addr, &recv_addr_len);
			if(recv_len > 0){
				resp_msg = exec_json(recv_buffer);
			}else{
				resp_msg = forge_resp(OPT_FAILED_ZERO_LENGTH);
			}
			if (resp_msg != NULL) {
				json_string = cJSON_Print(resp_msg);
				strncpy(send_buffer, json_string, MAX_MSG_LENGTH);
				free(json_string);
				sendto(updater_cur_state.serv_socket_fd, send_buffer,
						(strlen(send_buffer) + 1), 0,
						(struct sockaddr*) &recv_addr, recv_addr_len);
				cJSON_Delete(resp_msg);
				resp_msg = NULL;
			}
		}else if (FD_ISSET(updater_cur_state.serv_msg_in_fd,&rd_fd_set)){
			recv_len = mq_receive(updater_cur_state.serv_msg_in_fd,recv_buffer,MAX_MSG_LENGTH,&msg_prio);
			if(recv_len > 0){
				resp_msg = exec_json(((msg_params*)recv_buffer)->data);
			}else{
				resp_msg = forge_resp(OPT_FAILED_ZERO_LENGTH);
			}
			if(resp_msg != NULL){
				json_string = cJSON_Print(resp_msg);
				strncpy(msg_back.data, json_string, MAX_PARAM_MQ_LEN);
				msg_back.data_len = strlen(json_string) + 1;
				free(json_string);
				mq_send(updater_cur_state.serv_msg_out_fd,
						(const char*)&msg_back,sizeof(msg_back),msg_prio);
				cJSON_Delete(resp_msg);
				resp_msg = NULL;
			}
		}
	}
	cleanup();
	exit(0);
}
static int updater_proc_init(void){
	int result = -1;

	result = updater_proc_init_handler();
	if(result != 0){
		REPORT_ERR("cannot initialize handlers!");
		return -1;
	}

	result = updater_proc_init_state();
	if(result != 0){
		REPORT_ERR("cannot initialize state!");
		return -1;
	}

	result = updater_proc_init_socket();
	if(result != 0){
		REPORT_ERR("cannot initialize socket(OPTIONAL)!");
		if(updater_cur_state.serv_socket_fd != -1){
			shutdown(updater_cur_state.serv_socket_fd,SHUT_RDWR);
			close(updater_cur_state.serv_socket_fd);
			updater_cur_state.serv_socket_fd = -1;
		}
	}

	result = updater_proc_init_msgQ();
	if(result != 0){
		REPORT_ERR("cannot initialize message queue!");
		return -1;
	}
	setCurState(UPDATER_INITIALIZED);
	return 0;
}
static void sig_handler(int signo, siginfo_t* siginfo,void* context){
#ifndef __x86_64__
	ucontext_t * sig_context = (ucontext_t *)context;
#endif
	void * backtrace_stk[MAX_BACKTRACE_DEPTH];
	int backtrace_num = 0;
	int i = 0;
	char** backtrace_strings = NULL;
	backtrace_num = backtrace(backtrace_stk,MAX_BACKTRACE_DEPTH);
	backtrace_strings = backtrace_symbols(backtrace_stk,backtrace_num);
	REPORT_ERR("\n---- UPDATER MODULE ----");
	REPORT_ERR("\n---- %s ----",strsignal(signo));
	REPORT_ERR("\nSigno = %d,Code = %d",signo,siginfo->si_code);
	if(signo == SIGSEGV){
		if(siginfo->si_code == SEGV_MAPERR){
			REPORT_ERR( "\nAddress not mapped to object");
		}else if (siginfo->si_code == SEGV_ACCERR) {
			REPORT_ERR( "\nInvalid permissions for mapped object");
		}
	}
	REPORT_ERR("\nError Code = %d",siginfo->si_errno);
	REPORT_ERR("\nFaulty Address = 0x%08lX",(long)(siginfo->si_addr));
#ifndef __x86_64__
	REPORT_ERR("\nLink Register = 0x08lX",(long)(sig_context->uc_mcontext.arm_lr));
#endif
	REPORT_ERR("\n---- Backtrace(%d) ----",backtrace_num);
	for (i = 0; i < backtrace_num; i++){
		REPORT_ERR("\n%s", backtrace_strings[i]);
	}
	REPORT_ERR("\n---- END ----\n");
	signal(SIGSEGV,SIG_DFL);
	signal(SIGFPE,SIG_DFL);
	free(backtrace_strings);
	exit(-1);
}
static int updater_proc_init_handler(void){
	int result;
	struct sigaction sig_action;
	sig_action.sa_sigaction = sig_handler;
	sig_action.sa_flags = SA_RESTART | SA_SIGINFO;
	sigemptyset(&sig_action.sa_mask);
	result = sigaction(SIGSEGV,&sig_action,NULL);
	if(result == -1){
		REPORT_ERR(" %s",strerror(errno));
		return -1;
	}
	result = sigaction(SIGFPE, &sig_action, NULL);
	if (result == -1) {
		REPORT_ERR(" %s",strerror(errno));
		return -1;
	}
	sig_action.sa_handler = SIG_IGN;
	sig_action.sa_flags = 0;
	sigemptyset(&sig_action.sa_mask);

	result = sigaction(SIGALRM,&sig_action,NULL);
	if (result == -1) {
		REPORT_ERR(" %s",strerror(errno));
		return -1;
	}

	result = sigaction(SIGCHLD,&sig_action, NULL);
	if (result == -1) {
		REPORT_ERR(" %s",strerror(errno));
		return -1;
	}

	return 0;

}
static int updater_proc_init_state(void){
	updater_cur_state.updater_verison = UPDATER_VERSION;
	updater_cur_state.updater_state = UPDATER_INITIALIZING;
	updater_cur_state.updater_type = UPDATER_PENDIND;
	updater_cur_state.updater_rate = 0;
	updater_cur_state.updater_description = getDescription(UPDATER_INITIALIZING);
	updater_cur_state.serv_socket_fd = -1;
	updater_cur_state.serv_msg_in_fd = -1;
	updater_cur_state.serv_msg_out_fd = -1;
	updater_cur_state.priority = -1;
	updater_cur_state.exe_pthread_id = -1;
	updater_cur_state.exe_process_id = -1;
	return 0;
}
static int updater_proc_init_socket(void){
	int result = -1;
	int local_un_len = 0;
	struct sockaddr_un local_un;
	int serv_sock_fd;
	serv_sock_fd = socket(AF_UNIX,SOCK_DGRAM,0);
	if(serv_sock_fd == -1){
		REPORT_ERR(" %s",strerror(errno));
		return -1;
	}
	local_un.sun_family = AF_UNIX;
	strncpy(local_un.sun_path,UPDATER_NAME,sizeof(local_un.sun_path)-1);
	local_un_len = offsetof(struct sockaddr_un,sun_path)+strlen(local_un.sun_path)+1;
	unlink(local_un.sun_path);
	result = bind(serv_sock_fd,(struct sockaddr *)&local_un,local_un_len);
	if(result == -1){
		REPORT_ERR(" %s",strerror(errno));
		close(serv_sock_fd);
		return -1;
	}
	updater_cur_state.serv_socket_fd = serv_sock_fd;
	return 0;
}
static int updater_proc_init_msgQ(void){
	mqd_t read_msg_queue;
	mqd_t write_msg_queue;
	struct mq_attr  attrs_mq;
	attrs_mq.mq_msgsize = MAX_MESSAGE_QUEUE_MSG_LEN;
	attrs_mq.mq_maxmsg = MAX_MESSAGE_QUEUE_MSG_NUM;

	mq_unlink(UPDATER_MQ_READ_NAME);

	read_msg_queue = mq_open(UPDATER_MQ_READ_NAME,READ_FLAG,FILE_MODE,&attrs_mq);
	if(read_msg_queue == (mqd_t)-1){
		REPORT_ERR(" %s",strerror(errno));
		return -1;
	}

	mq_unlink(UPDATER_MQ_WRITE_NAME);

	write_msg_queue = mq_open(UPDATER_MQ_WRITE_NAME, WRITE_FLAG, FILE_MODE,
			&attrs_mq);
	if (write_msg_queue == (mqd_t) -1) {
		REPORT_ERR(" %s",strerror(errno));
		return -1;
	}
	updater_cur_state.serv_msg_in_fd = read_msg_queue;
	updater_cur_state.serv_msg_out_fd = write_msg_queue;
	return 0;
}
static char* getDescription(updater_status state) {
	switch (state) {
	case UPDATER_SUCCESS:
		return "updated successfully!";
	case UPDATER_INITIALIZING:
		return "updater is currently initializing";
	case UPDATER_INITIALIZED:
		return "updater is initialized";
	case UPDATER_NOT_START:
		return "updater has received update request,yet started";
	case UPDATER_RUNNING:
		return "updater is processing";
	case UPDATER_FAILED_SYS:
		return strerror(errno);
	case UPDATER_FAILED_SRC_CHECKSUM:
		return "source file CRC check failed";
	case UPDATER_FAILED_DST_CHECKSUM:
		return "destination file CRC check failed after displacement";
	case UPDATER_FAILED_FORMAT_NOT_SUPPORTED:
		return "unsupported file format";
	case UPDATER_FAILED_SINGLE_FILE_IN_PACKAGE:
		return "single file in package";
	case UPDATER_FAILED_NO_CHECKSUM_FILE:
		return "no txt checksum file found";
	case UPDATER_FAILED_UNEQUAL_IN_LENGTH:
		return "dst file is unequal in length to the src file";
	case UPDATER_FAILED_TIMEOUT:
		return "kernel/u-boot update failed to accomplish in time";
	case UPDATER_FAILED_SHELL:
		return "kernel/u-boot update shell terminates abnormally";
	default:
		return "invalid updater state";
	}
}

static void setCurState(updater_status state){
	updater_cur_state.updater_state = state;
	updater_cur_state.updater_description = getDescription(state);
	if((int)state < 3){
		updater_cur_state.updater_type = UPDATER_PENDIND;
		updater_cur_state.priority = -1;
	}
	if(state == UPDATER_SUCCESS){
		updater_cur_state.updater_rate = 100;
	}
}
static void setCurStateJson(cJSON* state){
	cJSON* type_elem = cJSON_GetObjectItem(state,"type");
	cJSON* priority_elem = cJSON_GetObjectItem(state,"priority");
	updater_cur_state.updater_type = (updater_type)type_elem->valueint;
	updater_cur_state.priority = priority_elem->valueint;
	updater_cur_state.updater_rate = 0;
}
static cJSON* exec_json(const char*json_str){
	cJSON* resp_json = NULL;
	cJSON* parsed_json = cJSON_Parse(json_str);
	cJSON* type_elem = NULL;
	cJSON* priority_elem = NULL;

	if(parsed_json == NULL){
		resp_json = forge_resp(OPT_FAILED_JSON_FORMAT);
		return resp_json;
	}
	type_elem = cJSON_GetObjectItem(parsed_json,"type");
	priority_elem = cJSON_GetObjectItem(parsed_json,"priority");
	if(type_elem == NULL || priority_elem == NULL){
		resp_json = forge_resp(OPT_FAILED_CMD_FORMAT);
		cJSON_Delete(parsed_json);
		return resp_json;
	}
	switch((updater_type)(type_elem->valueint)){
	case UPDATER_APP:
		if (priority_elem->valueint <= 0 || priority_elem->valueint > 99) {
			resp_json = forge_resp(OPT_FAILED_INVALID_PRIO);
			break;
		}
		if ((int) updater_cur_state.updater_state < 3
				&& updater_cur_state.updater_type == UPDATER_PENDIND) {
			resp_json = process_update_cmd(parsed_json);
			return resp_json;
		} else {
			resp_json = forge_resp(OPT_FAILED_IN_OPERANT);
		}
		break;
	case UPDATER_KERNEL:
	case UPDATER_UBOOT:
		if(priority_elem->valueint < -20 || priority_elem->valueint > 19){
			resp_json = forge_resp(OPT_FAILED_INVALID_PRIO);
			break;
		}
		if((int)updater_cur_state.updater_state < 3 &&
				updater_cur_state.updater_type == UPDATER_PENDIND){
			resp_json = process_update_cmd(parsed_json);
			return resp_json;
		}else{
			resp_json = forge_resp(OPT_FAILED_IN_OPERANT);
		}
		break;
	case UPDATER_STATUS:
		if((updater_status_resp_type)priority_elem->valueint == STATE_RESP_VERBOSE){
			resp_json = forge_verbose_status();
		}else{
			resp_json = forge_brief_status();
		}
		break;
	default:
		resp_json = forge_resp(OPT_FAILED_INVALID_TYPE);
		break;
	}
	cJSON_Delete(parsed_json);
	return resp_json;
}
static cJSON* forge_resp(updater_response resp){
	cJSON* resp_json = cJSON_CreateObject();
	cJSON_AddNumberToObject(resp_json,"result",resp);
	return resp_json;
}
static cJSON* forge_brief_status(){
	cJSON* resp_json = cJSON_CreateObject();
	cJSON_AddNumberToObject(resp_json,"state",updater_cur_state.updater_state);
	cJSON_AddNumberToObject(resp_json,"rate",updater_cur_state.updater_rate);
	return resp_json;
}
static cJSON* forge_verbose_status(){
	cJSON* resp_json = cJSON_CreateObject();
	cJSON_AddStringToObject(resp_json,"version",updater_cur_state.updater_verison);
	cJSON_AddNumberToObject(resp_json,"type",updater_cur_state.updater_type);
	cJSON_AddNumberToObject(resp_json,"state",updater_cur_state.updater_state);
	cJSON_AddNumberToObject(resp_json,"priority",updater_cur_state.priority);
	cJSON_AddNumberToObject(resp_json,"rate",updater_cur_state.updater_rate);
	cJSON_AddStringToObject(resp_json,"description",updater_cur_state.updater_description);
	return resp_json;
}
static void update_alrm_chld_handler(int signo,siginfo_t* info,void* context){
	struct sigaction act;
	int ret_code;
	REPORT_ERR("Signal %s received!Code = %d!\n",strsignal(signo),info->si_code);
	switch(signo)
	{
	case SIGALRM:
	{
		if ((int) updater_cur_state.updater_state == UPDATER_RUNNING
				|| updater_cur_state.updater_type != UPDATER_PENDIND) {
			setCurState(UPDATER_FAILED_TIMEOUT);
			if(updater_cur_state.exe_process_id != -1){
				kill(updater_cur_state.exe_process_id,SIGKILL);
			}
		}
	}
	break;
	case SIGCHLD:
	{
		if(info->si_pid != updater_cur_state.exe_process_id ||
				info->si_code == CLD_KILLED ||
				info->si_code == CLD_CONTINUED){
			return;
		}if(updater_cur_state.exe_process_id == -1){
			setCurState(UPDATER_FAILED_SYS);
		}else{
			waitpid(updater_cur_state.exe_process_id,&ret_code,0);
			if(WIFEXITED(ret_code)){
				if(WEXITSTATUS(ret_code) == 0){
					setCurState(UPDATER_SUCCESS);
				}else{
					setCurState(UPDATER_FAILED_SHELL);
				}
			}else{
				setCurState(UPDATER_FAILED_SYS);
			}
		}

	}
	break;
	default:
	//should never reach here
	break;
	}
	alarm(0);
	updater_cur_state.exe_process_id = -1;
	act.sa_handler = SIG_IGN;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	sigaction(SIGALRM,&act,NULL);
	sigaction(SIGCHLD,&act,NULL);
}
static cJSON* process_update_cmd(cJSON* cmd_json){
	int result = 0;
	int ret_code = 0;
	cJSON* resp;
	pthread_attr_t proc_thread_attr;
	pthread_t proc_thread_id;
	pid_t proc_process_id;
	struct sigaction proc_info_act;
	unsigned int wait_time = UPDATER_DEFAULT_TIME_LIMIT;
	char* wait_time_string = NULL;
	const union sigval extra = {.sival_int = 1};
	struct sched_param proc_thread_sched;
	cJSON* type_elem = cJSON_GetObjectItem(cmd_json,"type");
	cJSON* priority_elem = cJSON_GetObjectItem(cmd_json,"priority");
	cJSON* source_elem = cJSON_GetObjectItem(cmd_json,"source");
	cJSON* destination_elem = cJSON_GetObjectItem(cmd_json,"destination");
	if((updater_type)type_elem->valueint == UPDATER_APP){
		if(source_elem == NULL || destination_elem == NULL ||
				strlen(source_elem->valuestring) == 0 ||
				strlen(destination_elem->valuestring) == 0 ||
				strlen(source_elem->valuestring) >= MAX_FILE_PATH_LENGTH ||
				strlen(destination_elem->valuestring) >= MAX_FILE_PATH_LENGTH){
			resp = forge_resp(OPT_FAILED_CMD_FORMAT);
			return resp;
		}else if (check_file_existence(source_elem->valuestring) == 0 ||
				check_file_existence(destination_elem->valuestring) == 0){
			resp = forge_resp(OPT_FAILED_FILE_NONEXISTENT);
			return resp;
		}
	}else{
		if(source_elem == NULL ||
				strlen(source_elem->valuestring) == 0){
			resp = forge_resp(OPT_FAILED_CMD_FORMAT);
			return resp;
		}else if(check_file_existence(source_elem->valuestring) == 0){
			resp = forge_resp(OPT_FAILED_FILE_NONEXISTENT);
			return resp;
		}

		proc_info_act.sa_sigaction = update_alrm_chld_handler;
		sigemptyset(&proc_info_act.sa_mask);
		proc_info_act.sa_flags = SA_RESTART | SA_SIGINFO;

		proc_process_id = fork();
		if(proc_process_id < 0){
			resp = forge_resp(OPT_FAILED_PROCESS_START);
			return resp;
		}else if(proc_process_id == 0){
			raise(SIGSTOP);
			setpriority(PRIO_PROCESS,getpid(),priority_elem->valueint);
			execlp("/bin/sh","sh","-c",source_elem->valuestring,(char*)0);
		}else if(proc_process_id > 0){
			//parent
			waitpid(updater_cur_state.exe_process_id,&ret_code,WSTOPPED);
			sigaction(SIGALRM,&proc_info_act,NULL);
			sigaction(SIGCHLD,&proc_info_act,NULL);
			if((wait_time_string = getenv(UPDATER_TIME_LIMIT_ENV_NAME))!= NULL){
				wait_time = (unsigned int)atoi(wait_time_string);
			}
			updater_cur_state.exe_process_id = proc_process_id;
			setCurStateJson(cmd_json);
			setCurState(UPDATER_RUNNING);
			resp = forge_resp(OPT_SUCCESS);
			alarm(wait_time);
			sigqueue(updater_cur_state.exe_process_id,SIGCONT,extra);
			waitpid(updater_cur_state.exe_process_id,&ret_code,WCONTINUED);
			return resp;
		}

	}

	//set Updater State
	setCurState(UPDATER_NOT_START);
	
	//set Thread Attributes
	result |= pthread_attr_init(&proc_thread_attr);
	result |= pthread_attr_setinheritsched(&proc_thread_attr, PTHREAD_EXPLICIT_SCHED);
	result |= pthread_attr_setschedpolicy(&proc_thread_attr, SCHED_FIFO);
	//caution:a single set as below does not take effect
	//pthread_attr_setschedpolicy(&proc_thread_attr,SCHED_FIFO);
	result |= pthread_attr_setdetachstate(&proc_thread_attr,PTHREAD_CREATE_JOINABLE);
	proc_thread_sched.__sched_priority = priority_elem->valueint;
	result = pthread_attr_setschedparam(&proc_thread_attr,&proc_thread_sched);
	if(result != 0){
		REPORT_ERR("%s\n",strerror(result));
		setCurState(UPDATER_FAILED_SYS);
		pthread_attr_destroy(&proc_thread_attr);
		resp = forge_resp(OPT_FAILED_THREAD_CONFIG);
		return resp;
	}

	REPORT_INFO( "real uid:%d\n", getuid());
	REPORT_INFO( "real gid:%d\n", getgid());
	REPORT_INFO( "effective uid:%d\n", geteuid());
	REPORT_INFO( "effective gid:%d\n", getegid());

	result = pthread_create(&proc_thread_id,&proc_thread_attr,updater_worker_thread,cmd_json);
	if(result != 0){
		REPORT_ERR(" %s",strerror(result));
		pthread_attr_destroy(&proc_thread_attr);
		resp = forge_resp(OPT_FAILED_THREAD_START);
		setCurState(UPDATER_INITIALIZED);
		return resp;
	}

	//set Updater State
	updater_cur_state.exe_pthread_id = proc_thread_id;
	resp = forge_resp(OPT_SUCCESS);
	return resp;
}
static int check_same_directory(char* path1,char* path2){
	int res1 = -1;
	int res2 = -1;
	struct stat file_status_1;
	struct stat file_status_2;
	res1 = stat(path1,&file_status_1);
	res2 = stat(path2,&file_status_2);
	if(res1 == -1 || res2 == -1){
		return -1;
	}
	if(file_status_1.st_ino == file_status_2.st_ino){
		return 1;
	}else{
		return 0;
	}
}
static int check_file_existence(char* path){
	int result = -1;
	struct stat file_status;
	result = stat(path,&file_status);
	if(result == 0){
		return 1;
	}else{
		return 0;
	}
}
static long get_file_size(char* path){
	int result = -1;
	struct stat file_status;
	result = stat(path, &file_status);
	if (result == 0) {
		return file_status.st_size;
	} else {
		return -1;
	}
}
static file_type get_file_type(char* path){
	char* dot_pos = strrchr(path,'.');
	if(dot_pos == NULL){
		return FILE_TYPE_INVALID;
	}else if (strcmp(dot_pos,".gz") == 0){
		return FILE_TYPE_GZ;
	}else if (strcmp(dot_pos,".bz2") == 0){
		return FILE_TYPE_BZ2;
	}else if (strcmp(dot_pos,".xz") == 0){
		return FILE_TYPE_XZ;
	}else if (strcmp(dot_pos,".txt") == 0){
		return FILE_TYPE_TXT;
	}else{
		return FILE_TYPE_INVALID;
	}
}
static void* updater_worker_thread(void* arg){
	int result = -1;
	int i = 0;
	int pos = 0;
	FILE* cmd_pipe = NULL;
	char cmd[MAX_CMD_LENGTH];
	char resp[MAX_FILE_NAME_LENGTH+5];
	char restored_cwd[MAX_FILE_PATH_LENGTH];
	cJSON* arg_json = (cJSON*)arg;
	cJSON* source_elem = cJSON_GetObjectItem(arg_json,"source");
	cJSON* destination_elem = cJSON_GetObjectItem(arg_json,"destination");
	char file_paths[2][MAX_FILE_PATH_LENGTH];
	char file_names[MAX_FILES][MAX_FILE_NAME_LENGTH];
	
	setCurState(UPDATER_RUNNING);
	//update updater state,FIRST!
	setCurStateJson(arg_json);
	
	strncpy(file_paths[0],source_elem->valuestring,MAX_FILE_PATH_LENGTH);
	strncpy(file_paths[1],destination_elem->valuestring,MAX_FILE_PATH_LENGTH);
	pos = strlen(file_paths[1]);
	if(file_paths[1][pos-1] != '/'){
		file_paths[1][pos] = '/';
		file_paths[1][pos+1] = '\0';
	}
	switch (get_file_type(file_paths[0])) {
	case FILE_TYPE_GZ:
		snprintf(cmd, MAX_CMD_LENGTH, "tar -xvzf %s", file_paths[0]);
		break;
	case FILE_TYPE_BZ2:
		snprintf(cmd, MAX_CMD_LENGTH, "tar -xvjf %s", file_paths[0]);
		break;
	case FILE_TYPE_XZ:
		snprintf(cmd, MAX_CMD_LENGTH, "tar -xvJf %s", file_paths[0]);
		break;
	default:
		setCurState(UPDATER_FAILED_FORMAT_NOT_SUPPORTED);
		cJSON_Delete(arg_json);
		pthread_exit(&result);
	}
	cmd_pipe = popen(cmd,"r");
	for(i = 0; i < MAX_FILES;i++){
		if(fgets(file_names[i],MAX_FILE_NAME_LENGTH,cmd_pipe) != NULL){
			pos = strlen(file_names[i]);
			if(file_names[i][pos-1] == '\n'){
				file_names[i][pos-1] = '\0';
			}
		}else{
			break;
		}
	}

	if(i != MAX_FILES){
		setCurState(UPDATER_FAILED_SINGLE_FILE_IN_PACKAGE);
		cJSON_Delete(arg_json);
		pthread_exit(&result);
	}
	pclose(cmd_pipe);

	if(get_file_type(file_names[0]) == FILE_TYPE_TXT){
		snprintf(cmd, MAX_CMD_LENGTH, "md5sum -c %s", file_names[0]);
	}else if (get_file_type(file_names[1]) == FILE_TYPE_TXT){
		snprintf(cmd, MAX_CMD_LENGTH, "md5sum -c %s", file_names[1]);
	}else{
		setCurState(UPDATER_FAILED_NO_CHECKSUM_FILE);
		cJSON_Delete(arg_json);
		pthread_exit(&result);
	}

	cmd_pipe = popen(cmd,"r");
	fgets(resp,sizeof(resp),cmd_pipe);
	pclose(cmd_pipe);
	pos = strlen(resp);
	if(resp[pos-3] != 'O' || resp[pos-2] != 'K'){
		setCurState(UPDATER_FAILED_SRC_CHECKSUM);
		cJSON_Delete(arg_json);
		pthread_exit(&result);
	}
	getcwd(restored_cwd,MAX_FILE_PATH_LENGTH);
	result = check_same_directory(file_paths[1],restored_cwd);
	if(result == -1){
		setCurState(UPDATER_FAILED_SYS);
		cJSON_Delete(arg_json);
		pthread_exit(&result);
	}else if(result == 1){
		setCurState(UPDATER_SUCCESS);
		cJSON_Delete(arg_json);
		pthread_exit(&result);
	}
	if(get_file_type(file_names[0]) == FILE_TYPE_TXT){
		result = cp_file_tmp2final(file_paths[1],file_names[0]);
		if(result == -1){
			cJSON_Delete(arg_json);
			pthread_exit(&result);
		}
		result = cp_file_tmp2final(file_paths[1],file_names[1]);
		if(result == -1){
			cJSON_Delete(arg_json);
			pthread_exit(&result);
		}
	}else{
		result = cp_file_tmp2final(file_paths[1],file_names[1]);
		if(result == -1){
			cJSON_Delete(arg_json);
			pthread_exit(&result);
		}
		result = cp_file_tmp2final(file_paths[1],file_names[0]);
		if(result == -1){
			cJSON_Delete(arg_json);
			pthread_exit(&result);
		}
	}

	if(get_file_type(file_names[0]) == FILE_TYPE_TXT){
		snprintf(cmd, MAX_CMD_LENGTH, "md5sum -c %s%s",file_paths[1],file_names[0]);
	}else if (get_file_type(file_names[1]) == FILE_TYPE_TXT){
		snprintf(cmd, MAX_CMD_LENGTH, "md5sum -c %s%s",file_paths[1],file_names[1]);
	}else{
		setCurState(UPDATER_FAILED_NO_CHECKSUM_FILE);
		cJSON_Delete(arg_json);
		pthread_exit(&result);
	}

	result |= chdir(file_paths[1]);
	cmd_pipe = popen(cmd,"r");
	fgets(resp,sizeof(resp),cmd_pipe);
	pclose(cmd_pipe);
	pos = strlen(resp);
	if(resp[pos-3] != 'O' || resp[pos-2] != 'K'){
		setCurState(UPDATER_FAILED_SRC_CHECKSUM);
		result |= chdir(restored_cwd);
		cJSON_Delete(arg_json);
		pthread_exit(&result);
	}
	result |= chdir(restored_cwd);
	setCurState(UPDATER_SUCCESS);
	cJSON_Delete(arg_json);
	pthread_exit(&result);
}
static void update_rate(char*src,char*dst){
	int rate = 0;
	rate = (int)((get_file_size(dst) * 100) / get_file_size(src));
	updater_cur_state.updater_rate = rate;
}
static int cp_file_tmp2final(char* dst_dir, char* file){
        int is_update_rate = 0;
        updater_status res = UPDATER_FAILED_SYS;
        char buf[MAX_CPY_BUF_LENGTH];
        size_t char_read_current = 0;
        size_t char_write_sum = 0;
        char dst_file_path[MAX_FILE_PATH_LENGTH+MAX_FILE_NAME_LENGTH];
        FILE* src_file = NULL;
        FILE* dst_file = NULL;
        if(dst_dir == NULL || file == NULL){
    	    res = UPDATER_FAILED_SYS;
    	    goto error_handling;
        }
        //cpy md5 checksum file,which is often small&tiny
        src_file = fopen(file,"r");
        if(src_file == NULL){
    	    res = UPDATER_FAILED_SYS;
    	    goto error_handling;
        }
        snprintf(dst_file_path,MAX_FILE_PATH_LENGTH+MAX_FILE_NAME_LENGTH,"%s%s",dst_dir,file);
        dst_file = fopen(dst_file_path,"w+");
        if(dst_file == NULL){
    	    res = UPDATER_FAILED_SYS;
    	    goto error_handling;
        }
        if(get_file_type(file) != FILE_TYPE_TXT){
    	    is_update_rate = 1;
        }
        while ((char_read_current = fread(buf, sizeof(char), MAX_CPY_BUF_LENGTH, src_file))){
            usleep(50);
            if (fwrite(buf, sizeof(char), char_read_current, dst_file) != char_read_current){
                res = UPDATER_FAILED_SYS;
                goto error_handling;
            }
            char_write_sum += char_read_current;
            fflush(dst_file);
            if(is_update_rate == 1){
                update_rate(file,dst_file_path);
            }
            usleep(50);
        }
        if(char_write_sum != get_file_size(file)){
    	    res = UPDATER_FAILED_UNEQUAL_IN_LENGTH;
            goto error_handling;
        }
        fclose(src_file);
        fclose(dst_file);
        return 0;
error_handling:

	setCurState(res);

        if(src_file != NULL){
    	    fclose(src_file);
    	    src_file = NULL;
        }
        if(dst_file != NULL){
    	    fclose(dst_file);
    	    dst_file = NULL;
        }
        return -1;
}
static void cleanup(void){
	int result;
	int* reVal = &result;
	if(updater_cur_state.serv_socket_fd != -1){
		shutdown(updater_cur_state.serv_socket_fd,SHUT_RDWR);
		close(updater_cur_state.serv_socket_fd);
	}
	if(updater_cur_state.serv_msg_in_fd != -1){
		mq_close(updater_cur_state.serv_msg_in_fd);
		mq_unlink(UPDATER_MQ_READ_NAME);
	}
	if(updater_cur_state.serv_msg_out_fd != -1){
		mq_close(updater_cur_state.serv_msg_out_fd);
		mq_unlink(UPDATER_MQ_WRITE_NAME);
	}
	if(updater_cur_state.exe_pthread_id != -1){
		pthread_join(updater_cur_state.exe_pthread_id,(void*)&reVal);
		pthread_detach(updater_cur_state.exe_pthread_id);
	}
}
