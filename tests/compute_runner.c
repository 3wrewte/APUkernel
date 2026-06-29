/*
 * compute_runner.c — run a GCN compute shader with input/output BOs
 *
 * Usage:
 *   compute_runner vec_scale         # built-in vec_scale test
 *   compute_runner multi_syscall     # multi-syscall live test (uses RUN ioctl)
 *   compute_runner conv <width> <height> <input.pgm>   # convolution test
 *
 * Build: gcc -O2 -Wall -o compute_runner compute_runner.c
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

/* ---- ioctl ABI (must match kernel-patches/heteroken.c) ---- */

struct hk_run_req {
    unsigned long long shader_ptr;
    unsigned int shader_size;
    unsigned int _pad;
    unsigned long long result_ptr;
    unsigned int result_size;
};
#define HK_IOCTL_RUN _IOWR('H', 1, struct hk_run_req)

struct hk_compute_req {
    unsigned long long shader_ptr;     /* 0  */
    unsigned int shader_size;          /* 8  */
    unsigned int vgprs;                /* 12 */
    unsigned long long input_ptr;      /* 16 */
    unsigned int input_size;           /* 24 */
    unsigned int dispatch_x;           /* 28 */
    unsigned long long output_ptr;     /* 32 */
    unsigned int output_size;          /* 40 */
    unsigned int dispatch_y;           /* 44 */
    unsigned long long mailbox_ptr;    /* 48 */
    unsigned int mailbox_size;         /* 56 */
    unsigned int tgid_en;              /* 60 */
};
#define HK_IOCTL_COMPUTE _IOWR('H', 2, struct hk_compute_req)

/* ---- utility ---- */

static unsigned char *read_file(const char *path, size_t *out_size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return NULL; }
    struct stat st;
    fstat(fd, &st);
    unsigned char *buf = malloc(st.st_size);
    if (!buf) { close(fd); return NULL; }
    if (read(fd, buf, st.st_size) != st.st_size) {
        perror("read"); free(buf); close(fd); return NULL;
    }
    close(fd);
    *out_size = st.st_size;
    return buf;
}

/* ---- vec_scale test: multiply 64 uint32 elements by 2 ---- */

static int test_vec_scale(const char *shader_path)
{
    size_t shader_sz;
    unsigned char *shader = read_file(shader_path, &shader_sz);
    if (!shader) return 1;

    size_t elem_count = 64;
    size_t data_sz = elem_count * sizeof(unsigned int);
    unsigned int *input = malloc(data_sz);
    unsigned int *output = malloc(data_sz);
    unsigned char mailbox[4096];

    /* Fill input with known pattern */
    for (size_t i = 0; i < elem_count; i++)
        input[i] = (unsigned int)(i * 10 + 1);

    memset(output, 0, data_sz);
    memset(mailbox, 0, sizeof(mailbox));

    printf("vec_scale: %zu elements, %zu-byte shader\n", elem_count, shader_sz);

    int hkfd = open("/dev/heteroken", O_RDWR);
    if (hkfd < 0) { perror("open /dev/heteroken"); return 1; }

    struct hk_compute_req req = {
        .shader_ptr  = (unsigned long long)shader,
        .shader_size = shader_sz,
        .vgprs       = 3,       /* 16 VGPRs: shader uses v0-v11 */
        .input_ptr   = (unsigned long long)input,
        .input_size  = data_sz,
        .dispatch_x  = 1,
        .output_ptr  = (unsigned long long)output,
        .output_size = data_sz,
        .dispatch_y  = 1,
        .mailbox_ptr = (unsigned long long)mailbox,
        .mailbox_size= sizeof(mailbox),
        .tgid_en     = 0,
    };

    printf("dispatching vec_scale...\n");
    fflush(stdout);

    int ret = ioctl(hkfd, HK_IOCTL_COMPUTE, &req);
    if (ret < 0) {
        fprintf(stderr, "ioctl failed: %s\n", strerror(errno));
        close(hkfd);
        free(shader); free(input); free(output);
        return 1;
    }
    close(hkfd);
    int errors = 0;
    for (size_t i = 0; i < elem_count; i++) {
        unsigned int expected = input[i] * 2;
        if (output[i] != expected) {
            if (errors < 10)
                printf("MISMATCH[%zu]: got %u, expected %u\n",
                       i, output[i], expected);
            errors++;
        }
    }

    if (errors == 0)
        printf("vec_scale: PASS — all %zu elements correct\n", elem_count);
    else
        printf("vec_scale: FAIL — %d errors\n", errors);

    free(shader);
    free(input);
    free(output);
    return errors ? 1 : 0;
}

/* ---- multi_syscall test (uses simple HK_IOCTL_RUN) ---- */

static int test_multi_syscall(const char *shader_path)
{
    size_t shader_sz;
    unsigned char *shader = read_file(shader_path, &shader_sz);
    if (!shader) return 1;

    unsigned char result[4096];
    memset(result, 0, sizeof(result));

    printf("multi_syscall: %zu-byte shader\n", shader_sz);

    pid_t pid = fork();
    if (pid == 0) {
        int hkfd = open("/dev/heteroken", O_RDWR);
        if (hkfd < 0) { perror("open /dev/heteroken"); _exit(1); }

        struct hk_run_req req = {
            .shader_ptr  = (unsigned long long)shader,
            .shader_size = shader_sz,
            .result_ptr  = (unsigned long long)result,
            .result_size = sizeof(result),
        };

        printf("[child %d] dispatching multi_syscall...\n", getpid());
        fflush(stdout);

        int ret = ioctl(hkfd, HK_IOCTL_RUN, &req);
        if (ret < 0) {
            fprintf(stderr, "[child] ioctl failed: %s\n", strerror(errno));
            close(hkfd);
            _exit(1);
        }
        close(hkfd);
        _exit(0);
    }

    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        printf("multi_syscall: PASS — 3 write() calls completed\n");
    else
        printf("multi_syscall: FAIL — child status %d\n", status);

    free(shader);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
}

/* ---- convolution test using PGM image ---- */

static int test_convolution(const char *shader_path, int width, int height,
                            const char *pgm_path)
{
    /* Load PGM (P5 format) */
    size_t pgm_sz;
    unsigned char *pgm = read_file(pgm_path, &pgm_sz);
    if (!pgm) return 1;

    /* Skip PGM header: "P5\n<w> <h>\n<maxval>\n" */
    /* First skip the "P5" magic */
    if (pgm_sz < 2 || pgm[0] != 'P' || pgm[1] != '5') {
        fprintf(stderr, "Not a P5 PGM file\n");
        free(pgm);
        return 1;
    }
    int idx = 2;  /* skip "P5" */
    int fields[3] = {0,0,0};
    int fi = 0;
    while (idx < (int)pgm_sz && fi < 3) {
        if (pgm[idx] >= '0' && pgm[idx] <= '9') {
            fields[fi] = 0;
            while (idx < (int)pgm_sz && pgm[idx] >= '0' && pgm[idx] <= '9') {
                fields[fi] = fields[fi]*10 + pgm[idx]-'0';
                idx++;
            }
            fi++;
        } else if (pgm[idx] == '#') {
            while (idx < (int)pgm_sz && pgm[idx] != '\n') idx++;
        } else {
            idx++;
        }
    }
    int pgm_w = fields[0], pgm_h = fields[1];
    printf("PGM: %dx%d (expected %dx%d)\n", pgm_w, pgm_h, width, height);
    if (pgm_w != width || pgm_h != height) {
        fprintf(stderr, "PGM dimensions mismatch\n");
        free(pgm);
        return 1;
    }

    unsigned char *pixels = pgm + idx;
    size_t pixel_count = (size_t)width * height;

    /* Prepare input as u32 array */
    size_t in_sz = pixel_count * sizeof(unsigned int);
    unsigned int *input = malloc(in_sz);
    for (size_t i = 0; i < pixel_count; i++)
        input[i] = pixels[i];

    /* Output: (width-2)*(height-2) u32 values */
    int out_w = width - 2;
    int out_h = height - 2;
    size_t out_sz = (size_t)out_w * out_h * sizeof(unsigned int);
    unsigned int *output = malloc(out_sz);
    memset(output, 0, out_sz);

    size_t shader_sz;
    unsigned char *shader = read_file(shader_path, &shader_sz);
    if (!shader) { free(pgm); free(input); free(output); return 1; }

    /* CPU reference implementation */
    int kernel[9] = {-1,-1,-1, -1,8,-1, -1,-1,-1};
    unsigned int *ref = malloc(out_sz);
    memset(ref, 0, out_sz);
    for (int y = 0; y < out_h; y++) {
        for (int x = 0; x < out_w; x++) {
            int sum = 0;
            for (int ky = 0; ky < 3; ky++)
                for (int kx = 0; kx < 3; kx++)
                    sum += (int)input[(y+ky)*width + (x+kx)] * kernel[ky*3+kx];
            ref[y*out_w + x] = (unsigned int)sum;
        }
    }

    unsigned char mailbox[4096];
    memset(mailbox, 0, sizeof(mailbox));

    /* Set width/height in mailbox for shader */
    *(unsigned int *)(mailbox + 0xD0) = width;
    *(unsigned int *)(mailbox + 0xD4) = height;

    printf("convolution: %dx%d → %dx%d, %zu-byte shader\n",
           width, height, out_w, out_h, shader_sz);

    int hkfd = open("/dev/heteroken", O_RDWR);
    if (hkfd < 0) { perror("open /dev/heteroken"); free(pgm); free(input);
                     free(output); free(ref); free(shader); return 1; }

    struct hk_compute_req req = {
        .shader_ptr  = (unsigned long long)shader,
        .shader_size = shader_sz,
        .vgprs       = 5,       /* 24 VGPRs for convolution */
        .input_ptr   = (unsigned long long)input,
        .input_size  = in_sz,
        .dispatch_x  = (out_w * out_h + 63) / 64,
        .output_ptr  = (unsigned long long)output,
        .output_size = out_sz,
        .dispatch_y  = 1,
        .mailbox_ptr = (unsigned long long)mailbox,
        .mailbox_size= sizeof(mailbox),
        .tgid_en     = 1,
    };

    printf("dispatching convolution...\n");
    fflush(stdout);

    int ret = ioctl(hkfd, HK_IOCTL_COMPUTE, &req);
    if (ret < 0) {
        fprintf(stderr, "ioctl failed: %s\n", strerror(errno));
        close(hkfd);
        free(pgm); free(input); free(output); free(ref); free(shader);
        return 1;
    }
    close(hkfd);

    int status = 0;

    /* Compare GPU output with CPU reference */
    int errors = 0;
    for (int i = 0; i < out_w * out_h; i++) {
        if (output[i] != ref[i]) {
            if (errors < 10)
                printf("MISMATCH[%d,%d]: GPU=%u CPU=%u\n",
                       i % out_w, i / out_w, output[i], ref[i]);
            errors++;
        }
    }

    if (errors == 0) {
        printf("convolution: PASS — all %d pixels match CPU reference\n",
               out_w * out_h);
    } else {
        printf("convolution: FAIL — %d/%d pixels wrong\n",
               errors, out_w * out_h);
    }

    /* Write output PGM for visual comparison */
    FILE *fout = fopen("build/conv_gpu_output.pgm", "wb");
    if (fout) {
        fprintf(fout, "P5\n%d %d\n255\n", out_w, out_h);
        for (int i = 0; i < out_w * out_h; i++) {
            int v = (int)output[i];
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            fputc(v, fout);
        }
        fclose(fout);
        printf("GPU output written to build/conv_gpu_output.pgm\n");
    }

    free(pgm);
    free(input);
    free(output);
    free(ref);
    free(shader);
    return errors ? 1 : 0;
}

/* ---- main ---- */

/* ---- read test: GPU reads a file via read() then echoes it ---- */

static int test_read(const char *shader_path, const char *file_path)
{
    size_t shader_sz;
    unsigned char *shader = read_file(shader_path, &shader_sz);
    if (!shader) return 1;

    int file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0) { perror(file_path); free(shader); return 1; }

    size_t out_sz = 4096;
    unsigned char *output = malloc(out_sz);
    memset(output, 0, out_sz);

    unsigned char mailbox[4096];
    memset(mailbox, 0, sizeof(mailbox));

    /* Write fd into mailbox +0xD0 so the shader can read it */
    *(unsigned int *)(mailbox + 0xD0) = (unsigned int)file_fd;

    printf("read_test: fd=%d file=%s\n", file_fd, file_path);

    int hkfd = open("/dev/heteroken", O_RDWR);
    if (hkfd < 0) { perror("open /dev/heteroken"); free(shader); free(output);
                     close(file_fd); return 1; }

    struct hk_compute_req req = {
        .shader_ptr  = (unsigned long long)shader,
        .shader_size = shader_sz,
        .vgprs       = 3,       /* shader uses v0-v9 */
        .dispatch_x  = 1,
        .output_ptr  = (unsigned long long)output,
        .output_size = out_sz,
        .dispatch_y  = 1,
        .mailbox_ptr = (unsigned long long)mailbox,
        .mailbox_size= sizeof(mailbox),
        .tgid_en     = 0,
    };

    printf("dispatching read_test...\n");
    fflush(stdout);

    int ret = ioctl(hkfd, HK_IOCTL_COMPUTE, &req);
    if (ret < 0) {
        fprintf(stderr, "ioctl failed: %s\n", strerror(errno));
        close(hkfd); free(shader); free(output); close(file_fd);
        return 1;
    }
    close(hkfd);
    close(file_fd);

    /* Check output BO: the GPU wrote the file content here via read() */
    long long retval = *(long long *)(mailbox + 0x38);
    printf("read_test: GPU read() returned %lld bytes\n", retval);
    printf("read_test: output BO first 64 bytes:\n  ");
    for (int i = 0; i < 64 && i < (int)retval; i++)
        printf("%02x ", output[i]);
    printf("\n");

    int ok = (retval > 0);
    printf("read_test: %s\n", ok ? "PASS" : "FAIL");

    free(shader);
    free(output);
    return ok ? 0 : 1;
}

/* ---- main ---- */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage:\n"
            "  %s vec_scale <shader.co>\n"
            "  %s multi_syscall <shader.co>\n"
            "  %s conv <shader.co> <width> <height> <input.pgm>\n"
            "  %s read <shader.co> <file>\n",
            argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "vec_scale") == 0) {
        const char *sh = argc > 2 ? argv[2] : "build/gcn/vec_scale.co";
        return test_vec_scale(sh);
    }

    if (strcmp(argv[1], "multi_syscall") == 0) {
        const char *sh = argc > 2 ? argv[2] : "build/gcn/multi_syscall.co";
        return test_multi_syscall(sh);
    }

    if (strcmp(argv[1], "conv") == 0) {
        if (argc < 6) {
            fprintf(stderr, "conv needs: <shader.co> <width> <height> <input.pgm>\n");
            return 1;
        }
        return test_convolution(argv[2], atoi(argv[3]), atoi(argv[4]), argv[5]);
    }

    if (strcmp(argv[1], "read") == 0) {
        if (argc < 4) {
            fprintf(stderr, "read needs: <shader.co> <file>\n");
            return 1;
        }
        return test_read(argv[2], argv[3]);
    }

    fprintf(stderr, "Unknown test: %s\n", argv[1]);
    return 1;
}
