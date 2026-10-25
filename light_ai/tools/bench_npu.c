#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <rknn_api.h>

static int ppm_read(const char *path, unsigned char **data, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    char magic[3];
    if (fscanf(f, "%2s", magic) != 1 || strcmp(magic, "P6")) {
        fprintf(stderr, "%s: not P6\n", path); fclose(f); return -1;
    }
    int vals[3], vi = 0, c;
    while (vi < 3) {
        c = fgetc(f);
        if (c == '#') { while ((c = fgetc(f)) != '\n' && c != EOF); continue; }
        if (c == EOF) break;
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') continue;
        ungetc(c, f);
        if (fscanf(f, "%d", &vals[vi++]) != 1) break;
    }
    fgetc(f);
    *w = vals[0]; *h = vals[1];
    size_t n = (size_t)(*w) * (*h) * 3;
    *data = malloc(n);
    if (fread(*data, 1, n, f) != n) { fprintf(stderr, "short read\n"); fclose(f); return -1; }
    fclose(f);
    return 0;
}

static void resize_nn(const unsigned char *src, int sw, int sh,
                      unsigned char *dst, int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        int sy = y * sh / dh;
        for (int x = 0; x < dw; x++) {
            int sx = x * sw / dw;
            memcpy(dst + ((size_t)y * dw + x) * 3,
                   src + ((size_t)sy * sw + sx) * 3, 3);
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 4) {
        printf("usage: %s model.rknn image.ppm iters [cores 1|3|7]\n", argv[0]);
        return 1;
    }
    int iters = atoi(argv[3]);
    int core = argc > 4 ? atoi(argv[4]) : 7;

    unsigned char *img; int iw, ih;
    if (ppm_read(argv[2], &img, &iw, &ih)) return 1;
    printf("image: %dx%d\n", iw, ih);

    FILE *mf = fopen(argv[1], "rb");
    if (!mf) { perror(argv[1]); return 1; }
    fseek(mf, 0, SEEK_END); long msz = ftell(mf); fseek(mf, 0, SEEK_SET);
    void *mdata = malloc(msz);
    if (fread(mdata, 1, msz, mf) != (size_t)msz) { perror("model"); return 1; }
    fclose(mf);

    rknn_context ctx;
    if (rknn_init(&ctx, mdata, msz, 0, NULL) != RKNN_SUCC) {
        fprintf(stderr, "rknn_init failed\n"); return 1;
    }
    rknn_core_mask mask = RKNN_NPU_CORE_0_1_2;
    if (core == 1) mask = RKNN_NPU_CORE_0;
    if (core == 3) mask = RKNN_NPU_CORE_0_1;
    if (rknn_set_core_mask(ctx, mask) != RKNN_SUCC)
        fprintf(stderr, "warn: core mask %d not set, using auto\n", core);

    rknn_input_output_num io_num;
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    printf("model inputs=%d outputs=%d\n", io_num.n_input, io_num.n_output);

    rknn_tensor_attr iattr;
    memset(&iattr, 0, sizeof(iattr));
    iattr.index = 0;
    rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &iattr, sizeof(iattr));
    int ih_m, iw_m;
    if (iattr.fmt == RKNN_TENSOR_NCHW) { ih_m = iattr.dims[2]; iw_m = iattr.dims[3]; }
    else { ih_m = iattr.dims[1]; iw_m = iattr.dims[2]; }
    printf("input: %dx%dx%d fmt=%d type=%d\n", ih_m, iw_m, iattr.dims[3],
           iattr.fmt, iattr.type);

    unsigned char *inbuf = malloc((size_t)iw_m * ih_m * 3);
    resize_nn(img, iw, ih, inbuf, iw_m, ih_m);

    rknn_input in; memset(&in, 0, sizeof(in));
    in.index = 0; in.type = iattr.type; in.fmt = RKNN_TENSOR_NHWC;
    in.buf = inbuf; in.size = iw_m * ih_m * 3; in.pass_through = 0;

    rknn_output out; memset(&out, 0, sizeof(out));
    out.index = 0; out.want_float = 0; out.is_prealloc = 0;

    for (int i = 0; i < 10; i++) {   // прогрев
        rknn_inputs_set(ctx, 1, &in);
        rknn_run(ctx, NULL);
        rknn_outputs_get(ctx, 1, &out, NULL);
        rknn_outputs_release(ctx, 1, &out);
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < iters; i++) {
        rknn_inputs_set(ctx, 1, &in);
        rknn_run(ctx, NULL);
        rknn_outputs_get(ctx, 1, &out, NULL);
        rknn_outputs_release(ctx, 1, &out);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double el = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("pure infer: %.2f ms/frame (avg over %d iters, cores=%d)\n",
           el / iters, iters, core);

    rknn_destroy(ctx);
    return 0;
}
