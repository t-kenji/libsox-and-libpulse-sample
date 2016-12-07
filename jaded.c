#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <mqueue.h>
#include <time.h>
#include <sys/stat.h>


#define DEBUG(fmt,...) do{fprintf(stderr,"%s:%d " fmt "\n",__FILE__,__LINE__,##__VA_ARGS__);}while(0)


struct sound_play_req_t {
    uint32_t play_id;
    char source[256];
    int be_overwrite;
};

struct sound_volume_req_t {
    uint16_t volume;
};


int main(int argc, char **argv)
{
    enum {
        ARGV_SELF,
        ARGV_MQ_NAME,
        ARGV_COMMAND,
        ARGV_PATH,
        ARGV_VOLUME = ARGV_PATH,
        ARGV_LENGTH
    };
    mqd_t mq;
    char name[256];
    char *command;
    struct sound_play_req_t play_req;
    struct sound_volume_req_t vol_req;

    if (argc <= ARGV_COMMAND) {
        DEBUG("invalid argument");
        printf("usage: %s name command path\n", argv[ARGV_SELF]);
        return 1;
    }

    snprintf(name, sizeof(name), "/aerosmith-%s", argv[ARGV_MQ_NAME]);
    mq = mq_open(name, O_WRONLY);
    if (mq == (mqd_t) -1) {
        DEBUG("mq_open failed: %d", errno);
        return 2;
    }

    command = argv[ARGV_COMMAND];
    if (strcmp(command, "play") == 0) {
        play_req.play_id = (uint32_t) time(NULL);
        strcpy(play_req.source, argv[ARGV_PATH]);
        play_req.be_overwrite = 1;
        if (mq_send(mq, (const char *) &play_req, sizeof(play_req), 0) != 0) {
            DEBUG("mq_send failed: %d", errno);
        }
    } else if (strcmp(command, "add") == 0) {
        play_req.play_id = (uint32_t) time(NULL);
        strcpy(play_req.source, argv[ARGV_PATH]);
        play_req.be_overwrite = 0;
        if (mq_send(mq, (const char *) &play_req, sizeof(play_req), 0) != 0) {
            DEBUG("mq_send failed: %d", errno);
        }
    } else if (strcmp(command, "stop") == 0) {
        play_req.play_id = 0;
        memset(play_req.source, 0, sizeof(play_req.source));
        play_req.be_overwrite = 1;
        if (mq_send(mq, (const char *) &play_req, sizeof(play_req), 0) != 0) {
            DEBUG("mq_send failed: %d", errno);
        }
    } else if (strcmp(command, "change") == 0) {
        vol_req.volume = atoi(argv[ARGV_VOLUME]);
        if (mq_send(mq, (const char *) &vol_req, sizeof(vol_req), 0) != 0) {
            DEBUG("mq_send failed: %d", errno);
        }
    } else if (strcmp(command, "quit") == 0) {
        play_req.play_id = 0xFFFFFFFF;
        memset(play_req.source, 0, sizeof(play_req.source));
        play_req.be_overwrite = 1;
        if (mq_send(mq, (const char *) &play_req, sizeof(play_req), 0) != 0) {
            DEBUG("mq_send failed: %d", errno);
        }
    } else {
        DEBUG("unknown command: %s", command);
    }

    mq_close(mq);

    return 0;
}
