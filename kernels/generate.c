#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lz4hc.h>
#include <xxhash.h>

char *read_file(const char *fname, size_t *size_out) {
    FILE *f = fopen(fname, "r");
    if (!f) {
        fprintf(stderr, "Could not open '%s'!\n", fname);
        exit(EXIT_FAILURE);
    }

    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    char *buf = malloc(size + 1);
    buf[size] = '\0';
    fseek(f, 0, SEEK_SET);

    if (fread(buf, size, 1, f) != 1) {
        fprintf(stderr, "Could not read '%s'!\n", fname);
        exit(EXIT_FAILURE);
    }

    fclose(f);
    *size_out = size;
    return buf;
}

void dump_hex(FILE *f, const char *name, const char *data, size_t size) {
    fprintf(f, "const char %s[] = {\n", name);
    for (size_t i = 0; i < size; ++i)
        fprintf(f, "%s0x%02x%s%s",
                i % 8 == 0 ? "    " : "",
                (unsigned) (uint8_t) data[i],
                i + 1 < size? "," : "",
                (i % 8 == 7 || i + 1 == size) ? "\n" : " ");

    fprintf(f, "};\n\n");
}

void append(FILE *f, const char *filename, const char *prefix, char *dict, int dict_size) {
    size_t size;
    char *buf = read_file(filename, &size);

    int compressed_size = LZ4_compressBound(size);
    char *compressed = malloc(compressed_size);

    unsigned long long hash = XXH64(buf, size, 0);

    LZ4_streamHC_t stream;
    memset(&stream, 0, sizeof(LZ4_streamHC_t));
    LZ4_resetStreamHC_fast(&stream, LZ4HC_CLEVEL_MAX);
    if (dict)
        LZ4_loadDictHC(&stream, dict, dict_size);
    compressed_size = LZ4_compress_HC_continue(&stream, buf,
            compressed, size, compressed_size);

    fprintf(f, "const int %s_size_uncompressed = %zu;\n", prefix, size);
    fprintf(f, "const int %s_size_compressed   = %i;\n", prefix, compressed_size);
    fprintf(f, "const size_t  %s_hash          = %lluull;\n\n", prefix, hash);
    dump_hex(f, prefix, compressed, compressed_size);
    free(buf);
    free(compressed);

}

int main(int argc, char **argv) {
    (void) argc; (void) argv;

    FILE *f = fopen("kernels.c", "w");
    if (!f) {
        fprintf(stderr, "Could not open 'kernels.c'!");
        exit(EXIT_FAILURE);
    }


    fprintf(f, "#include \"kernels.h\"\n\n");

    size_t kernels_dict_size;
    char *kernels_dict = read_file("kernels.dict", &kernels_dict_size);

    append(f, "kernels.dict", "kernels_dict", NULL, 0);
    append(f, "kernels_50.ptx", "kernels_50", kernels_dict, kernels_dict_size);
    append(f, "kernels_70.ptx", "kernels_70", kernels_dict, kernels_dict_size);

    fprintf(f, "const char *kernels_list = \"");
    size_t size = 0;
    char *buf = read_file("kernels_70.ptx", &size),
         *ptr = buf;
    while (ptr) {
        ptr = strstr(ptr, ".entry ");
        if (!ptr)
            break;
        ptr += 7;
        char *next = strstr(ptr, "(");
        if (!next)
            break;
        fwrite(ptr, next-ptr, 1, f);
        fputc(',', f);
        ptr = next;
    }
    fprintf(f, "\";\n\n");

    append(f, "llvm_kernels.ll", "llvm_kernels", kernels_dict, kernels_dict_size);


    f = fopen("kernels.h", "w");
    if (!f) {
        fprintf(stderr, "Could not open 'kernels.h'!");
        exit(EXIT_FAILURE);
    }

    fprintf(f, "#pragma once\n\n");
    fprintf(f, "#include <stddef.h>\n\n");
    fprintf(f, "#if defined(__cplusplus)\n");
    fprintf(f, "extern \"C\" {\n");
    fprintf(f, "#endif\n\n");
    fprintf(f, "extern const int    kernels_dict_size_uncompressed;\n");
    fprintf(f, "extern const int    kernels_dict_size_compressed;\n");
    fprintf(f, "extern const size_t kernels_dict_hash;\n");
    fprintf(f, "extern const char   kernels_dict[];\n\n");
    fprintf(f, "extern const int    kernels_50_size_uncompressed;\n");
    fprintf(f, "extern const int    kernels_50_size_compressed;\n");
    fprintf(f, "extern const size_t kernels_50_hash;\n");
    fprintf(f, "extern const char   kernels_50[];\n\n");
    fprintf(f, "extern const int    kernels_70_size_uncompressed;\n");
    fprintf(f, "extern const int    kernels_70_size_compressed;\n");
    fprintf(f, "extern const size_t kernels_70_hash;\n");
    fprintf(f, "extern const char   kernels_70[];\n\n");
    fprintf(f, "extern const char   *kernels_list;\n\n");
    fprintf(f, "extern const int    llvm_kernels_size_uncompressed;\n");
    fprintf(f, "extern const int    llvm_kernels_size_compressed;\n");
    fprintf(f, "extern const size_t llvm_kernels_hash;\n");
    fprintf(f, "extern const char   llvm_kernels[];\n\n");
    fprintf(f, "#if defined(__cplusplus)\n");
    fprintf(f, "}\n");
    fprintf(f, "#endif");
    fclose(f);

    free(kernels_dict);

    return EXIT_SUCCESS;
}
