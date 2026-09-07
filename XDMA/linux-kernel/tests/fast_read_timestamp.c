
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <ctype.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/mman.h>

#define FATAL do { fprintf(stderr, "Error at line %d, file %s (%d) [%s]\n", \
        __LINE__, __FILE__, errno, strerror(errno)); exit(1); } while(0)

#define MAP_SIZE 4096UL
#define MAP_MASK (MAP_SIZE - 1)

#define BAR0_PHY 0xfcc00000

static inline void *fixup_addr(void *addr, size_t size);

int main(int argc, char **argv) {
    int fd;
    void *map_base, *virt_addr;
    unsigned long read_result, write_val;
    off_t target;
    int access_type = 'w';
    char fmt_str[128];
    size_t data_size;
#if 0
    if(argc < 2) {
        fprintf(stderr, "\nUsage:\t%s { address } [ type [ data ] ]\n"
                "\taddress : memory address to act upon\n"
                "\ttype    : access operation type : [b]yte, [h]alfword, [w]ord, [l]ong\n"
                "\tdata    : data to be written\n\n",
                argv[0]);
        exit(1);
    }
    target = strtoul(argv[1], 0, 0);

    if(argc > 2)
        access_type = tolower(argv[2][0]);
#endif

    if((fd = open("/dev/mem", O_RDWR | O_SYNC)) == -1) FATAL;
    printf("/dev/mem opened.\n");
    fflush(stdout);

    /* Map one page */
    map_base = mmap(0, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, BAR0_PHY & ~MAP_MASK);
    if(map_base == (void *) -1) FATAL;


    //先读一下时戳，判断是否已经使能
    virt_addr = map_base + (BAR0_PHY & MAP_MASK);
    virt_addr = fixup_addr(virt_addr, sizeof(uint64_t));
    *((uint64_t *) virt_addr) = 0xE0;

    while(*((uint64_t *)virt_addr)&0x1 != 1);
    data_size = sizeof(uint64_t);
    virt_addr = fixup_addr(virt_addr, data_size) + 0xE0;
    read_result = *((uint64_t *) virt_addr);

    //if (read_result == 0) {
        virt_addr = map_base + (BAR0_PHY & MAP_MASK);
        virt_addr = fixup_addr(virt_addr, sizeof(uint64_t)) + 0xB8;
        *((uint64_t *) virt_addr) = 0x0;
        sleep(1);
        *((uint64_t *) virt_addr) = 0x1;
    //}
    sleep(1);

    for (int i = 0; i < 1000; i++) {
        virt_addr = map_base + (BAR0_PHY & MAP_MASK);
        virt_addr = fixup_addr(virt_addr, sizeof(uint64_t));
        *((uint64_t *) virt_addr) = 0xE0;

        while(*((uint64_t *)virt_addr)&0x1 != 1);
        data_size = sizeof(uint64_t);
        virt_addr = fixup_addr(virt_addr, data_size) + 0xE0;
        read_result = *((uint64_t *) virt_addr);

        printf("%p: 0x%lx\n", virt_addr, read_result);
    }

    if(munmap(map_base, MAP_SIZE) == -1) FATAL;
    close(fd);

    return 0;
}

static inline void *fixup_addr(void *addr, size_t size)
{
#ifdef FORCE_STRICT_ALIGNMENT
    unsigned long aligned_addr = (unsigned long)addr;
    aligned_addr &= ~(size - 1);
    addr = (void *)aligned_addr;
#endif
    return addr;
}
