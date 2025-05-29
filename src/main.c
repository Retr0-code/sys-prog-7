#include <stdio.h>
#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <threads.h>
#include <sys/stat.h>
#include <sys/mman.h>

typedef struct
{
    const char *pattern;
    size_t pat_len;
    int delta1[256];
    int *delta2;
} bm_search_t;

typedef struct
{
    char *filename;
    bm_search_t *bm;
} thrd_search_args_t;

mtx_t print_mutex;

// Case-insensitive char comparison helper
static inline unsigned char to_lower_uc(unsigned char c)
{
    return (unsigned char)tolower(c);
}

// Preprocessing for delta1 table (bad character rule)
void bm_preprocess_delta1(bm_search_t *bm)
{
    for (int i = 0; i < 256; ++i)
        bm->delta1[i] = (int)bm->pat_len;

    for (size_t i = 0; i < bm->pat_len - 1; ++i)
        bm->delta1[to_lower_uc((unsigned char)bm->pattern[i])] = (int)(bm->pat_len - 1 - i);
}

// Helper function for suffixes used in delta2 preprocessing
static void suffixes(const char *pat, int *suff, size_t len)
{
    size_t f = 0;
    size_t g = len - 1;
    suff[len - 1] = len;
    for (size_t i = len - 2; i >= 0; --i)
    {
        if (i > g && suff[i + len - 1 - f] < i - g)
            suff[i] = suff[i + len - 1 - f];

        else
        {
            if (i < g)
                g = i;

            f = i;
            while (g >= 0 && to_lower_uc((unsigned char)pat[g]) == to_lower_uc((unsigned char)pat[g + len - 1 - f]))
                --g;

            suff[i] = f - g;
        }
    }
}

void bm_preprocess_delta2(bm_search_t *bm)
{
    size_t len = bm->pat_len;
    int *suff = malloc(len * sizeof(int));
    if (!suff)
    {
        perror("malloc");
        exit(-1);
    }
    suffixes(bm->pattern, suff, len);

    for (size_t i = 0; i < len; ++i)
        bm->delta2[i] = len;

    size_t j = 0;
    for (size_t i = len - 1; i >= 0; --i)
        if (suff[i] == i + 1)
            for (; j < len - 1 - i; ++j)
                if (bm->delta2[j] == len)
                    bm->delta2[j] = len - 1 - i;

    for (size_t i = 0; i <= len - 2; ++i)
        bm->delta2[len - 1 - suff[i]] = len - 1 - i;

    free(suff);
}

bm_search_t *bm_init(const char *pattern)
{
    bm_search_t *bm = malloc(sizeof(bm_search_t));
    if (!bm)
    {
        perror("malloc");
        exit(-1);
    }
    bm->pattern = pattern;
    bm->pat_len = strlen(pattern);
    bm->delta2 = malloc(bm->pat_len * sizeof(int));
    if (!bm->delta2)
    {
        perror("malloc");
        free(bm);
        exit(-1);
    }
    bm_preprocess_delta1(bm);
    bm_preprocess_delta2(bm);
    return bm;
}

void bm_free(bm_search_t *bm)
{
    if (bm)
    {
        free(bm->delta2);
        free(bm);
    }
}

ssize_t bm_search(bm_search_t *bm, const char *text, size_t text_len)
{
    ssize_t i = 0;
    while (i <= text_len - bm->pat_len)
    {
        ssize_t j = bm->pat_len - 1;
        while (j >= 0 && to_lower_uc((unsigned char)bm->pattern[j]) == to_lower_uc((unsigned char)text[i + j]))
            --j;

        if (j < 0)
            return i;

        else
        {
            int bc_shift = bm->delta1[(unsigned char)to_lower_uc((unsigned char)text[i + j])] - ((int)bm->pat_len - 1 - j);
            int gs_shift = bm->delta2[j];
            int shift = bc_shift > gs_shift ? bc_shift : gs_shift;

            if (shift < 1)
                shift = 1;

            i += shift;
        }
    }
    return -1;
}

int thread_search(thrd_search_args_t *targ)
{
    const char *filename = targ->filename;
    bm_search_t *bm = targ->bm;

    int fd = open(filename, O_RDONLY);
    if (fd < 0)
    {
        free(targ->filename);
        free(targ);
        return 0;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size == 0)
    {
        close(fd);
        free(targ->filename);
        free(targ);
        return 0;
    }

    size_t filesize = st.st_size;
    char *data = mmap(NULL, filesize, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED)
    {
        close(fd);
        free(targ->filename);
        free(targ);
        return 0;
    }

    ssize_t offset = 0;
    while (offset <= (filesize - bm->pat_len))
    {
        ssize_t pos = bm_search(bm, data + offset, filesize - offset);
        if (pos < 0)
            break;

        // Lock mutex for printing
        mtx_lock(&print_mutex);
        printf("%s:%zd\n", filename, offset + pos);
        fflush(stdout);
        mtx_unlock(&print_mutex);

        offset += pos + 1; // continue searching after the found position
    }

    munmap(data, filesize);
    close(fd);
    free(targ->filename);
    free(targ);
    return 0;
}

// Recursively find files in directory and spawn threads for searching
void search_directory(const char *dirpath, bm_search_t *bm)
{
    DIR *dir = opendir(dirpath);
    if (!dir)
    {
        perror("opendir");
        return;
    }

    struct dirent *entry;
    thrd_t threads[256];
    int thread_count = 0;
    while ((entry = readdir(dir)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char *path = NULL;
        if (asprintf(&path, "%s/%s", dirpath, entry->d_name) < 0)
        {
            perror("asprintf");
            continue;
        }

        struct stat st;
        if (stat(path, &st) < 0)
        {
            free(path);
            continue;
        }

        if (S_ISDIR(st.st_mode))
        {
            // Recurse into subdirectory
            search_directory(path, bm);
            free(path);
        }
        else if (S_ISREG(st.st_mode))
        {
            // Spawn thread for regular file
            thrd_search_args_t *targ = malloc(sizeof(thrd_search_args_t));
            if (!targ)
            {
                perror("malloc");
                free(path);
                continue;
            }
            targ->filename = path;
            targ->bm = bm;

            if (thrd_create(&threads[thread_count], thread_search, targ) != thrd_success)
            {
                perror("thrd_create");
                free(targ->filename);
                free(targ);
                continue;
            }
            ++thread_count;

            // Join threads if too many
            if (thread_count == 256)
            {
                for (int i = 0; i < thread_count; ++i)
                    thrd_join(threads[i], NULL);

                thread_count = 0;
            }
        }
        else
            free(path);
    }
    closedir(dir);

    for (int i = 0; i < thread_count; i++)
        thrd_join(threads[i], NULL);
}

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s <directory> <pattern>\n", argv[0]);
        return -1;
    }

    if (mtx_init(&print_mutex, mtx_plain) != thrd_success)
    {
        fprintf(stderr, "Failed to initialize mutex\n");
        return -1;
    }

    bm_search_t *bm = bm_init(argv[2]);
    search_directory(argv[1], bm);
    bm_free(bm);

    mtx_destroy(&print_mutex);
    return 0;
}
