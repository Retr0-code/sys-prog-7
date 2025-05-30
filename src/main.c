#include <stdio.h>
#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <getopt.h>
#include <unistd.h>
#include <threads.h>
#include <sys/stat.h>
#include <sys/mman.h>

#define MAX_THREADS 256
#define MAX_FILENAME 256
#define USAGE_FMT "Usage: %s -p <pattern> [-d <directory>, -i]\n"

mtx_t print_mutex;

typedef int (*case_function_t)(int x);

int no_case_change(int x) { return x; }

ssize_t naive_search(
    const char *text,
    size_t text_len,
    const char *pattern,
    size_t pat_len,
    case_function_t callback)
{
    if (pat_len == 0 || text_len < pat_len)
        return -1;

    for (ssize_t i = 0; i <= text_len - pat_len; ++i)
    {
        ssize_t j = 0;
        for (; j < pat_len; ++j)
            if ((*callback)(text[i + j]) != (*callback)(pattern[j]))
                break;

        if (j == pat_len)
            return i;
    }
    return -1;
}

typedef struct
{
    char *filename;
    const char *pattern;
    size_t pat_len;
    case_function_t case_func;
} thrd_search_args_t;

int thread_search(thrd_search_args_t *targ)
{
    const char *filename = targ->filename;
    const char *pattern = targ->pattern;
    size_t pat_len = targ->pat_len;
    case_function_t case_func = targ->case_func;

    int fd = open(filename, O_RDONLY);
    if (fd < 0)
    {
        free(targ->filename);
        free(targ);
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size == 0)
    {
        close(fd);
        free(targ->filename);
        free(targ);
        return -1;
    }

    size_t filesize = (size_t)st.st_size;
    char *data = mmap(NULL, filesize, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED)
    {
        close(fd);
        free(targ->filename);
        free(targ);
        return -1;
    }

    size_t offset = 0;
    while (offset <= filesize - pat_len)
    {
        ssize_t pos = naive_search(data + offset, filesize - offset, pattern, pat_len, case_func);
        if (pos < 0)
            break;

        mtx_lock(&print_mutex);
        printf("%s:%zu\n", filename, offset + (size_t)pos);
        fflush(stdout);
        mtx_unlock(&print_mutex);

        offset += (size_t)pos + 1;
    }

    munmap(data, filesize);
    close(fd);
    free(targ->filename);
    free(targ);
    return 0;
}

void search_directory(const char *dirpath, const char *pattern, size_t pat_len, case_function_t case_func)
{
    DIR *dir = opendir(dirpath);
    if (!dir)
    {
        perror("opendir");
        return;
    }

    struct dirent *entry;
    thrd_t threads[MAX_THREADS];
    int thread_count = 0;

    while ((entry = readdir(dir)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char *path = malloc(MAX_FILENAME + 1 + strlen(dirpath));
        if (sprintf(path, "%s/%s", dirpath, entry->d_name) < 0)
        {
            perror("sprintf");
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
            search_directory(path, pattern, pat_len, case_func);
            free(path);
        }
        else if (S_ISREG(st.st_mode))
        {
            thrd_search_args_t *targ = malloc(sizeof(thrd_search_args_t));
            if (!targ)
            {
                perror("malloc");
                free(path);
                continue;
            }
            targ->filename = path;
            targ->pattern = pattern;
            targ->pat_len = pat_len;
            targ->case_func = case_func;

            if (thrd_create(&threads[thread_count], thread_search, targ) != thrd_success)
            {
                perror("thrd_create");
                free(targ->filename);
                free(targ);
                continue;
            }
            ++thread_count;

            if (thread_count == MAX_THREADS)
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

    for (int i = 0; i < thread_count; ++i)
        thrd_join(threads[i], NULL);
}

int main(int argc, char **argv)
{
    const char *optstring = "p:d:i";
    int option = 0;
    char *dirpath = NULL;
    char *pattern = NULL;
    int case_insensetive = 0;
    size_t pat_len = 0;
    while ((option = getopt(argc, argv, optstring)) != -1)
    {
        switch (option)
        {
            case 'p':
                if (!optarg)
                {
                    fprintf(stderr, USAGE_FMT, argv[0]);
                    return -1;
                }

                pat_len = strlen(optarg);
                pattern = malloc(pat_len + 1);
                strncpy(pattern, optarg, pat_len + 1);
                break;

            case 'd':
                if (!optarg)
                {
                    fprintf(stderr, "Using default path ~/files");
                    break;
                }

                size_t dir_len = strlen(optarg) + 1;
                dirpath = malloc(dir_len);
                strncpy(dirpath, optarg, dir_len);
                break;

            case 'i':
                case_insensetive = 1;
                break;

            default:
                fprintf(stderr, USAGE_FMT, argv[0]);
                return -1;
        }
    }

    case_function_t search_func = &no_case_change;
    if (case_insensetive)
        search_func = tolower;

    if (mtx_init(&print_mutex, mtx_plain) != thrd_success)
    {
        fprintf(stderr, "Failed to initialize mutex\n");
        return -1;
    }

    search_directory(dirpath, pattern, pat_len, search_func);

    free(dirpath);
    free(pattern);
    mtx_destroy(&print_mutex);
    return 0;
}
