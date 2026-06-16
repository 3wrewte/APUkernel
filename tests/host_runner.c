/*
 * host_runner.c — load a .co shader and run it on GPU
 *
 * Usage: host_runner <shader.co>
 *
 * The .co file is raw shader binary (extract .text from .hsaco via
 * llvm-objcopy -O binary -j .text shader.hsaco shader.co).
 *
 * This program:
 *   1. Reads the .co file
 *   2. fork() — child gets CoW copy of mm (real task_struct)
 *   3. Child: opens /dev/heteroken, ioctl(HK_IOCTL_RUN, shader)
 *      → kernel dispatches shader on GPU, waits for IH completion
 *      → copies result back
 *   4. Child prints result, exits
 *   5. Parent: waitpid(), reports exit status
 *
 * Build: gcc -o host_runner host_runner.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/stat.h>

struct hk_run_req {
    unsigned long long shader_ptr;
    unsigned int shader_size;
    unsigned int _pad;
    unsigned long long result_ptr;
    unsigned int result_size;
};

#define HK_IOCTL_RUN _IOWR('H', 1, struct hk_run_req)

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <shader.co>\n", argv[0]);
        return 1;
    }

    /* Read shader file */
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        perror("open shader");
        return 1;
    }

    struct stat st;
    fstat(fd, &st);
    if (st.st_size == 0 || st.st_size > 4096) {
        fprintf(stderr, "shader size %ld invalid (must be 1..4096)\n", st.st_size);
        close(fd);
        return 1;
    }

    /* shader must be dword-aligned */
    size_t shader_size = (st.st_size + 3) & ~3;
    unsigned char *shader = calloc(1, shader_size);
    if (!shader) {
        perror("calloc");
        close(fd);
        return 1;
    }
    if (read(fd, shader, st.st_size) != st.st_size) {
        perror("read shader");
        free(shader);
        close(fd);
        return 1;
    }
    close(fd);

    /* Allocate result buffer */
    size_t result_size = 256;
    unsigned char *result = calloc(1, result_size);
    if (!result) {
        perror("calloc result");
        free(shader);
        return 1;
    }

    printf("host_runner: %zu-byte shader from %s\n", shader_size, argv[1]);

    /* fork: child becomes the GCN task */
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        free(shader);
        free(result);
        return 1;
    }

    if (pid == 0) {
        /* Child: open /dev/heteroken and dispatch */
        int hkfd = open("/dev/heteroken", O_RDWR);
        if (hkfd < 0) {
            perror("open /dev/heteroken");
            _exit(1);
        }

        struct hk_run_req req = {
            .shader_ptr  = (unsigned long long)shader,
            .shader_size = shader_size,
            .result_ptr  = (unsigned long long)result,
            .result_size = result_size,
        };

        printf("host_runner [child %d]: dispatching to GPU...\n", getpid());
        fflush(stdout);

        int ret = ioctl(hkfd, HK_IOCTL_RUN, &req);
        if (ret < 0) {
            fprintf(stderr, "host_runner [child]: ioctl failed: %s\n",
                    strerror(errno));
            close(hkfd);
            _exit(1);
        }
        close(hkfd);

        /* Print result as string if printable, hex dump otherwise */
        int printable = 1;
        for (size_t i = 0; i < 32 && i < result_size; i++) {
            if (result[i] != 0 && (result[i] < 0x20 || result[i] > 0x7e)
                && result[i] != '\n' && result[i] != '\t') {
                printable = 0;
                break;
            }
        }
        if (printable) {
            printf("host_runner [child %d]: GPU output: %s\n", getpid(), result);
        } else {
            printf("host_runner [child %d]: GPU output (hex):", getpid());
            for (size_t i = 0; i < 32 && i < result_size; i++)
                printf(" %02x", result[i]);
            printf(" ...\n");
        }
        fflush(stdout);
        _exit(0);
    }

    /* Parent: wait for child */
    int status;
    waitpid(pid, &status, 0);
    free(shader);
    free(result);

    if (WIFEXITED(status)) {
        printf("host_runner: child %d exited with status %d\n",
               pid, WEXITSTATUS(status));
        return WEXITSTATUS(status);
    } else {
        printf("host_runner: child %d terminated by signal %d\n",
               pid, WTERMSIG(status));
        return 1;
    }
}
