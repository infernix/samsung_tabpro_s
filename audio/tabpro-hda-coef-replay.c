/*
 * Replay the minimal, ordered Realtek HDA coefficient sequence that
 * initializes the Samsung Galaxy TabPro S speaker amplifiers.
 *
 * Input format:
 *   sequence-number  coefficient-index  coefficient-value
 *
 * This source is released under the repository's Unlicense.
 */
#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct hda_verb_ioctl {
    uint32_t verb;
    uint32_t res;
};

struct coefficient_write {
    unsigned int sequence;
    unsigned int index;
    unsigned int value;
};

enum {
    first_sequence = 33,
    last_sequence = 312,
    expected_writes = last_sequence - first_sequence + 1,
};

#define HDA_IOCTL_VERB_WRITE _IOWR('H', 0x11, struct hda_verb_ioctl)
#define HDA_VERB(nid, verb, param) \
    (((uint32_t)(nid) << 24) | ((uint32_t)(verb) << 8) | (uint32_t)(param))

static int send_verb(int fd, uint32_t nid, uint32_t verb, uint32_t param)
{
    struct hda_verb_ioctl command = {
        .verb = HDA_VERB(nid, verb, param),
        .res = 0,
    };

    return ioctl(fd, HDA_IOCTL_VERB_WRITE, &command);
}

static int read_trace(const char *path,
                      struct coefficient_write writes[expected_writes])
{
    FILE *trace;
    unsigned int sequence;
    unsigned int index;
    unsigned int value;
    size_t count = 0;
    int fields;

    trace = fopen(path, "r");
    if (!trace) {
        fprintf(stderr, "Cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    while ((fields = fscanf(trace, "%u %x %x",
                            &sequence, &index, &value)) == 3) {
        if (count >= expected_writes) {
            fprintf(stderr, "Trace contains more than %u writes.\n",
                    expected_writes);
            fclose(trace);
            return -1;
        }
        if (sequence != first_sequence + count ||
            index > 0xff || value > 0xffff) {
            fprintf(stderr,
                    "Invalid trace entry at position %zu: "
                    "sequence=%u index=%#x value=%#x\n",
                    count + 1, sequence, index, value);
            fclose(trace);
            return -1;
        }

        writes[count].sequence = sequence;
        writes[count].index = index;
        writes[count].value = value;
        count++;
    }

    if (fields != EOF || ferror(trace)) {
        fprintf(stderr, "Malformed coefficient trace: %s\n", path);
        fclose(trace);
        return -1;
    }
    fclose(trace);

    if (count != expected_writes) {
        fprintf(stderr, "Trace contains %zu writes; expected %u.\n",
                count, expected_writes);
        return -1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    struct coefficient_write writes[expected_writes];
    const char *device;
    const char *trace_path;
    size_t i;
    int fd;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s DEVICE TRACE_TSV\n", argv[0]);
        return 2;
    }

    device = argv[1];
    trace_path = argv[2];

    if (read_trace(trace_path, writes) < 0)
        return 1;

    fd = open(device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", device, strerror(errno));
        return 1;
    }

    for (i = 0; i < expected_writes; i++) {
        if (send_verb(fd, 0x20, 0x500, writes[i].index) < 0 ||
            send_verb(fd, 0x20, 0x400, writes[i].value) < 0) {
            fprintf(stderr,
                    "HDA ioctl failed at sequence %u "
                    "(index %#x, value %#x): %s\n",
                    writes[i].sequence, writes[i].index,
                    writes[i].value, strerror(errno));
            close(fd);
            return 1;
        }

        /*
         * Keep the codec's indirect amplifier-control interface from being
         * overrun. This delay was used for the successful hardware test.
         */
        usleep(2000);
    }

    close(fd);
    printf("Applied %u coefficient writes from sequence %u through %u.\n",
           expected_writes, first_sequence, last_sequence);
    return 0;
}
