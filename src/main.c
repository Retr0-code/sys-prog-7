/**
 * @file main.c
 * @author Korneev Nikita
 * @brief Example program for pattern recursive search.
 * @version 1.0
 * @date 2025-05-30
 * 
 * @copyright Copyright (c) 2025
 * 
 */

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
#define DEFAULT_RECURSION_DEPTH (1 << 10)
#define USAGE_FMT "Usage: %s -p <pattern> [-d <directory>, -i, -r <depth>]\n"

/// Mutex for stdout/stderr blocking
static mtx_t print_mutex;

/// @brief Type for callback function
typedef int (*case_function_t)(int x);

/**
 * @brief Does not change input character.
 * 
 * @param x - character.
 * @return unchanged `x` parameter.
 */
int no_case_change(int x) { return x; }

/**
 * @brief Performs a linear "naive" search in memory region.
 * 
 * @param data      - file contents mapped to memory region.
 * @param data_len  - length of the file contents.
 * @param pattern   - pattern to search for.
 * @param pat_len   - length of the pattern.
 * @param callback  - case-(in)sensitivity callback function.
 * @return -1 on error and index on success.
 */
ssize_t naive_search(
    const char *data,
    size_t data_len,
    const char *pattern,
    size_t pat_len,
    case_function_t callback)
{
    if (pat_len == 0 || data_len < pat_len)
        return -1;

    for (ssize_t i = 0; i <= data_len - pat_len; ++i)
    {
        ssize_t j = 0;
        for (; j < pat_len; ++j)
            if ((*callback)(data[i + j]) != (*callback)(pattern[j]))
                break;

        if (j == pat_len)
            return i;
    }
    return -1;
}

/**
 * @brief Arguments for `thread_search`.
 * 
 */
typedef struct
{
    char *filename;             /// Path to file.
    const char *pattern;        /// Pattern to search for.
    size_t pat_len;             /// Length of the pattern.
    case_function_t case_func;  /// Case-(in)sensitivity callback function.
} thrd_search_args_t;

/**
 * @brief Maps file and starts search.
 * 
 * @param targ - thread arguments see `thrd_search_args_t`
 * @return On error -1 and 0 on success
 */
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

/**
 * @brief Performs recursive search of pattern in specified directory.
 * 
 * @param dirpath   - directory to start with.
 * @param pattern   - patter for search.
 * @param depth     - recursion depth.
 * @param pat_len   - pattern length.
 * @param case_func - callback function for case-(in)sensitivity.
 */
void search_directory(
    const char *dirpath,
    const char *pattern,
    size_t depth,
    size_t pat_len,
    case_function_t case_func)
{
    if (!(depth--))
    {
        fprintf(stderr, "Reached max recursion depth at %s\n", dirpath);
        return;
    }

    // Opens directory given in option dirpath
    DIR *dir = opendir(dirpath);
    if (!dir)
    {
        perror("opendir");
        return;
    }

    struct dirent *entry;
    thrd_t threads[MAX_THREADS];
    int thread_count = 0;

    // Reading entries (dirs & files) from directory (dirpath)
    while ((entry = readdir(dir)) != NULL)
    {
        // Ignore cwd and up level directory
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        // Allocate path to entry container
        char *path = malloc(MAX_FILENAME + 1 + strlen(dirpath));
        if (!path)
        {
            perror("malloc");
            continue;
        }

        // Construct path to entry
        if (sprintf(path, "%s/%s", dirpath, entry->d_name) < 0)
        {
            free(path);
            perror("sprintf");
            continue;
        }

        // Read attributes of entry
        struct stat st;
        if (stat(path, &st) < 0)
        {
            free(path);
            continue;
        }

        // If entry is a dir, search recursively
        if (S_ISDIR(st.st_mode))
        {
            search_directory(path, pattern, depth, pat_len, case_func);
            free(path);
        }
        else if (S_ISREG(st.st_mode))
        {
            // If entry is file, start searching it for the pattern
            thrd_search_args_t *targ = malloc(sizeof(thrd_search_args_t));
            if (!targ)
            {
                perror("malloc");
                free(path);
                continue;
            }
            targ->filename = path; // Passing ownership of path to thread_search function
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

            // Limiting threads amount, by waiting all already started threads
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

    // Waiting and closing all threads
    for (int i = 0; i < thread_count; ++i)
        thrd_join(threads[i], NULL);
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, USAGE_FMT, argv[0]);
        return -1;
    }

    // Initialization and parsing of parameters
    const char *optstring = "p:d:ir:";
    int option = 0;
    char *dirpath = NULL;
    char *pattern = NULL;
    int case_insensetive = 0;
    size_t pat_len = 0;
    size_t depth = DEFAULT_RECURSION_DEPTH;
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
                fprintf(stderr, "Using default path ~/files\n");
                break;
            }

            size_t dir_len = strlen(optarg) + 1;
            dirpath = malloc(dir_len);
            strncpy(dirpath, optarg, dir_len);
            break;

        case 'i':
            case_insensetive = 1;
            break;

        case 'r':
            if (optarg)
            {
                depth = atoi(optarg);
                depth = depth ? depth : DEFAULT_RECURSION_DEPTH;
            }

            break;

        default:
            fprintf(stderr, USAGE_FMT, argv[0]);
            return -1;
        }
    }

    // Setting "case" function
    case_function_t search_func = &no_case_change;
    if (case_insensetive)
        search_func = tolower;

    if (mtx_init(&print_mutex, mtx_plain) != thrd_success)
    {
        fprintf(stderr, "Failed to initialize mutex\n");
        return -1;
    }

    // Start recursive search
    search_directory(dirpath, pattern, depth, pat_len, search_func);

    free(dirpath);
    free(pattern);
    mtx_destroy(&print_mutex);
    return 0;
}
