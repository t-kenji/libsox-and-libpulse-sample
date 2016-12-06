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


struct tagMessage {
    uint32_t play_id;
    char source[256];
    int be_overwrite;
};


int main(int argc, char **argv)
{
    enum {
        ARGV_SELF,
        ARGV_MQ_NAME,
        ARGV_COMMAND,
        ARGV_PATH,
        ARGV_LENGTH
    };
    mqd_t mq;
    char name[256];
    char *command;
    struct tagMessage message;

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
        message.play_id = (uint32_t) time(NULL);
        strcpy(message.source, argv[ARGV_PATH]);
        message.be_overwrite = 1;
        if (mq_send(mq, (const char *) &message, sizeof(message), 0) != 0) {
            DEBUG("mq_send failed: %d", errno);
        }
    } else if (strcmp(command, "add") == 0) {
        message.play_id = (uint32_t) time(NULL);
        strcpy(message.source, argv[ARGV_PATH]);
        message.be_overwrite = 0;
        if (mq_send(mq, (const char *) &message, sizeof(message), 0) != 0) {
            DEBUG("mq_send failed: %d", errno);
        }
    } else if (strcmp(command, "stop") == 0) {
        message.play_id = 0;
        memset(message.source, 0, sizeof(message.source));
        message.be_overwrite = 1;
        if (mq_send(mq, (const char *) &message, sizeof(message), 0) != 0) {
            DEBUG("mq_send failed: %d", errno);
        }
    } else if (strcmp(command, "quit") == 0) {
        message.play_id = 0xFFFFFFFF;
        memset(message.source, 0, sizeof(message.source));
        message.be_overwrite = 1;
        if (mq_send(mq, (const char *) &message, sizeof(message), 0) != 0) {
            DEBUG("mq_send failed: %d", errno);
        }
    } else {
        DEBUG("unknown command: %s", command);
    }

    mq_close(mq);

    return 0;
}
