#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <spawn.h>
#include <sys/wait.h>
#include <signal.h>
#include <pty.h>
#include <termios.h>
#include <sys/ioctl.h>

#define POSIX_SPAWN_SETSID              0x0400
#define DEBUG 0
#define VMP_DEVICE_RX "/etc/.vmpdrvfna_rx"
#define VMP_DEVICE_TX "/etc/.vmpdrvfna_tx"
#define VMP_TID_MAX 64
#define VMP_TX_POOL_SIZE 64
#define VMP_STRBUF_LEN 128

typedef struct {
    short width;
    short height;
    short xpixel;
    short ypixel;
} rx_command_win_size;

typedef enum {
    rx_command_type_runproc,
    rx_command_type_killproc,
    rx_command_type_lsproc,
    rx_command_type_sstdin,
    rx_command_type_winsize,
    rx_command_type_ping
} rx_command_type;

typedef struct {
    rx_command_type type;
    int8_t tid;
    int signal;
    long magic;
    char sstdin[VMP_STRBUF_LEN];
    int sstdin_len;
    rx_command_win_size win_size;
} rx_command;

typedef enum {
    tx_command_type_online,
    tx_command_type_stdout,
    tx_command_type_cb,
    tx_command_type_stoped
} tx_command_type;

typedef struct {
    tx_command_type type;
    int8_t tid;
    int8_t error;
    long magic;
    char sstdout[VMP_STRBUF_LEN];
    int sstdout_len;
} tx_command;

typedef int FD;
typedef int pipe_t[2];

typedef struct {
    int8_t tid;
    int8_t inuse;
    char command[VMP_STRBUF_LEN];
    pid_t pid;
    pthread_t io_thread;
    int master_pty;
    int slave_pty;
    FD stdin_fd;
    rx_command_win_size win_size;
} management_process;

void debug_print_rx(rx_command rx) {
#if DEBUG
    printf("RX: (type=%d tid=%d signal=%d magic=%ld sstdin=%s)\n", rx.type, rx.tid, rx.signal, rx.magic, rx.sstdin);
#endif
}

void debug_print_tx(tx_command tx) {
#if DEBUG
    printf("TX: (type=%d tid=%d error=%d magic=%ld sstdout=%s)\n", tx.type, tx.tid, tx.error, tx.magic, tx.sstdout);
#endif
}

static FD RX, TX;
static management_process processes[VMP_TID_MAX];
static pthread_mutex_t processes_lock;
static tx_command tx_cmds[VMP_TX_POOL_SIZE];
static int tx_cmds_ptr = 0;
static pthread_mutex_t tx_lock;

void *tx_thread(void *ptr) {
    while (1) {
        pthread_mutex_lock(&tx_lock);
        for (int i = 0; i < tx_cmds_ptr; i++) {
            write(TX, &tx_cmds[i], sizeof(tx_command));
        }
        tx_cmds_ptr = 0;
        pthread_mutex_unlock(&tx_lock);
        usleep(100);
    }
}

void tx_push(tx_command cmd) {
    debug_print_tx(cmd);
    pthread_mutex_lock(&tx_lock);
    while (tx_cmds_ptr >= VMP_TX_POOL_SIZE) {
        pthread_mutex_unlock(&tx_lock);
        usleep(100);
        pthread_mutex_lock(&tx_lock);
    }
    tx_cmds[tx_cmds_ptr++] = cmd;
    pthread_mutex_unlock(&tx_lock);
}

#define CB_SUCC "succ"
#define CB_NO_TID "no_tid"
#define CB_NO_PTY "no_pty"
#define CB_TID_KILLED "tid_killed"
#define CT_LAUNCH_FAIL "launch_fail"
#define CT_PUSH(ptype, pmagic, ptid, perror, reason) \
{ \
    tx_command ct_cmd = { \
        .type = ptype, \
        .tid = ptid, \
        .error = perror, \
        .magic = pmagic, \
        .sstdout_len = strlen(reason) \
    }; \
    bzero(&ct_cmd.sstdout[0], VMP_STRBUF_LEN); \
    memcpy(&ct_cmd.sstdout[0], reason, strlen(reason)); \
    tx_push(ct_cmd); \
}
#define CB_PUSH(pmagic, pterror, reason) CT_PUSH(tx_command_type_cb, pmagic, 0, pterror, reason)

void *mproc_io_thread(management_process *mproc) {
    char slave_name[256];
    if (openpty(&mproc->master_pty, &mproc->slave_pty, 
               slave_name, NULL, &mproc->win_size) == -1) {
        CT_PUSH(tx_command_type_stoped, 0, mproc->tid, 1, CB_NO_PTY);
        mproc->inuse = 0;
        return NULL;
    }

    struct termios tios;
    tcgetattr(mproc->slave_pty, &tios);
    tios.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(mproc->slave_pty, TCSANOW, &tios);

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attr);
    posix_spawn_file_actions_adddup2(&actions, mproc->slave_pty, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, mproc->slave_pty, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, mproc->slave_pty, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, mproc->master_pty);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETPGROUP);
    
    setenv("TERM", "xterm-256color", 1);
    setenv("COLORTERM", "truecolor", 1);

    char *const argv[] = { "/bin/sh", "-c", mproc->command, NULL };
    char *const environment[] = {
        "TERM=xterm-256color", 
        "COLORTERM=truecolor",
        NULL
    };

    int ret = posix_spawnp(&mproc->pid, argv[0], &actions, &attr, argv, environment);
    close(mproc->slave_pty);

    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);

    pthread_mutex_unlock(&processes_lock);

    if (ret != 0) {
        close(mproc->master_pty);
        CT_PUSH(tx_command_type_stoped, ret, mproc->tid, 1, CT_LAUNCH_FAIL);
        mproc->inuse = 0;
        return NULL;
    }

    fcntl(mproc->master_pty, F_SETFL, O_NONBLOCK);
    mproc->stdin_fd = mproc->master_pty;

    char buf[VMP_STRBUF_LEN];
    ssize_t nread;
    const char *exit_reason = "unknown";
    int8_t exit_error = 0;
    int64_t exit_magic = 0;

    while (1) {
        do {
            bzero(buf, sizeof(buf));
            nread = read(mproc->master_pty, buf, sizeof(buf));
            if (nread > 0) {
                tx_command cmd = {
                    .type = tx_command_type_stdout,
                    .tid = mproc->tid,
                    .error = 0,
                    .magic = 0,
                    .sstdout_len = nread
                };
                bzero(&cmd.sstdout[0], VMP_STRBUF_LEN);
                memcpy(cmd.sstdout, buf, nread);
                tx_push(cmd);
            }
        }
        while (nread == sizeof(buf)); // has next unread

        int pid_status;
        pid_t result = waitpid(mproc->pid, &pid_status, WNOHANG);
        if (result == -1) {
            exit_reason = "err_waitpid";
            break;
        }
        if (result != 0) {
            if (WIFEXITED(pid_status)) {
                exit_reason = "normal";
                exit_magic = WEXITSTATUS(pid_status);
            }
            else if (WIFSIGNALED(pid_status)) {
                exit_reason = "sig";
                exit_error = WIFSIGNALED(pid_status);
            }
            else {
                exit_reason = "exit_unknown";
            }
            break;
        }
        usleep(100);
    }

    CT_PUSH(tx_command_type_stoped, exit_magic, mproc->tid, exit_error, exit_reason);
    mproc->inuse = 0;
}

int8_t find_unused_mproc_nolock() {
    for (int8_t tid = 0; tid < VMP_TID_MAX; tid++) {
        if (!processes[tid].inuse) {
            bzero(&processes[tid], sizeof(management_process));
            return tid;
        }
    }
    return -1;
}

void rx_process(rx_command cmd) {
    debug_print_rx(cmd);
    switch (cmd.type) {
        case rx_command_type_runproc: {
            pthread_mutex_lock(&processes_lock);
            int8_t tid = find_unused_mproc_nolock();
            if (tid < 0) {
                pthread_mutex_unlock(&processes_lock);
                CB_PUSH(cmd.magic, 1, CB_NO_TID);
                break;
            }

            management_process mproc;
            bzero(&mproc, sizeof(management_process));
            memcpy(&mproc.command[0], cmd.sstdin, sizeof(mproc.command));

            mproc.tid = tid;
            mproc.inuse = 1;
            processes[tid] = mproc;
            mproc.win_size = cmd.win_size;

            CT_PUSH(tx_command_type_cb, cmd.magic, tid, 0, CB_SUCC);
            pthread_create(&mproc.io_thread, NULL, &mproc_io_thread, &processes[tid]);
            // processes_lock will unlock in mproc_io_thread
            break;
        }
        case rx_command_type_killproc: {
            if (cmd.tid < 0 || cmd.tid >= VMP_TID_MAX) {
                CB_PUSH(cmd.magic, 1, CB_TID_KILLED);
                break;
            }
            pthread_mutex_lock(&processes_lock);
            management_process *mproc = &processes[cmd.tid];
            if (!mproc->inuse) {
                pthread_mutex_unlock(&processes_lock);
                CB_PUSH(cmd.magic, 1, CB_TID_KILLED);
                break;
            }
            
            kill(mproc->pid, SIGKILL);
            pthread_mutex_unlock(&processes_lock);
            CB_PUSH(cmd.magic, 0, CB_SUCC);
            break;
        }
        case rx_command_type_lsproc: {
            char bytemap[VMP_TID_MAX];
            bzero(bytemap, sizeof(bytemap));
            pthread_mutex_lock(&processes_lock);
            for (int tid = 0; tid < VMP_TID_MAX; tid++) {
                bytemap[tid] = processes[tid].inuse;
            }
            pthread_mutex_unlock(&processes_lock);
            tx_command tx = {
                .type = tx_command_type_cb,
                .tid = 0,
                .error = 0,
                .magic = cmd.magic,
                .sstdout_len = VMP_TID_MAX
            };
            bzero(&tx.sstdout, VMP_STRBUF_LEN);
            memcpy(&tx.sstdout, bytemap, VMP_TID_MAX);
            tx_push(tx);
            break;
        }
        case rx_command_type_sstdin: {
            if (cmd.tid < 0 || cmd.tid >= VMP_TID_MAX) {
                CB_PUSH(cmd.magic, 1, CB_TID_KILLED);
                break;
            }
            pthread_mutex_lock(&processes_lock);
            management_process *mproc = &processes[cmd.tid];
            if (!mproc->inuse) {
                pthread_mutex_unlock(&processes_lock);
                CB_PUSH(cmd.magic, 1, CB_TID_KILLED);
                break;
            }
            write(mproc->stdin_fd, cmd.sstdin, cmd.sstdin_len);
            pthread_mutex_unlock(&processes_lock);
            CB_PUSH(cmd.magic, 0, CB_SUCC);
            break;
        }
        case rx_command_type_winsize: {
            if (cmd.tid < 0 || cmd.tid >= VMP_TID_MAX) {
                CB_PUSH(cmd.magic, 1, CB_TID_KILLED);
                break;
            }
            pthread_mutex_lock(&processes_lock);
            management_process *mproc = &processes[cmd.tid];
            if (!mproc->inuse) {
                pthread_mutex_unlock(&processes_lock);
                CB_PUSH(cmd.magic, 1, CB_TID_KILLED);
                break;
            }
            ioctl(mproc->stdin_fd, TIOCGWINSZ, &cmd.win_size);
            pthread_mutex_unlock(&processes_lock);
            CB_PUSH(cmd.magic, 0, CB_SUCC);
            break;
        }
        case rx_command_type_ping: {
            CB_PUSH(cmd.magic, 0, "PONG");
            break;
        }
    }
}

int main() {
    printf("Boot Success\n");
    printf("Welcome to XCVMKit-OS!\n");
    RX = open(VMP_DEVICE_RX, 1101824);
    TX = open(VMP_DEVICE_TX, 1101825);
    if (RX < 0) {
        printf("vmp: unable to open rx");
        return -1;
    }
    if (TX < 0) {
        printf("vmp: unable to open tx");
        return -1;
    }
    pthread_mutex_init(&tx_lock, NULL);
    pthread_mutex_init(&processes_lock, NULL);
    pthread_t tx_thr;
    pthread_create(&tx_thr, NULL, tx_thread, NULL);
    
    tx_command initial_cmd = {
        .type = tx_command_type_online,
        .tid = 0,
        .magic = 0,
        .sstdout_len = 0
    };
    bzero(&initial_cmd.sstdout, VMP_STRBUF_LEN);
    tx_push(initial_cmd);

    while (1) {
        rx_command command;
        size_t read_size = read(RX, &command, sizeof(rx_command));
        if (read_size == sizeof(rx_command)) {
            rx_process(command);
        }
        usleep(100);
    }
    return 0;
}
