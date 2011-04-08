/**
 * MyBlobFS - virtual file system driver for mounting of table rows as local files
 *
 * Copyright (C) 2001-2005  Miklos Szeredi <miklos@szeredi.hu>
 * Copyright (C) 2008 Olexandr Melnyk <me@omelnyk.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#define FUSE_USE_VERSION 25

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <fuse_opt.h>
#include <unistd.h>
#include <mysql/mysql.h>

/**
 * Maximum length of a SQL query
 */
#define MAX_QUERY_LENGTH 1024

/**
 * Maximum length of the filename
 */
#define MAX_FILENAME_LENGTH 16

/**
 * Maximum size of file
 */
#define MAX_FILE_SIZE 10486750

/**
 * Macro for short options definition
 */
#define MYBLOBFS_OPT_KEY(t, p, v) { t, offsetof(struct options, p), v }

/**
 * Structure for command-line options
 */
struct options
{
    char *hostname;
    unsigned int port;
    char *username;
    int rq_password;
    char *database;
    char *table;
    char *name_field;
    char *data_field;
};

/**
 * Table name
 */
static char *my_table;

/**
 * Name of the field containing filenames. Field must be declared as either:      
 * an integer or a string
 */
static char *my_name_field;

/**
 * Name of the field with file contents. Field can be declared using any data
 * type from standard MySQL distribution
 */
static char *my_data_field;

/**
 * MySQL connection information
 */
static MYSQL mysql;

/**
 * Query pattern for fetching directory names
 */
static char *readdir_qp = "SELECT %s FROM %s ORDER BY %s";

/**
 * Query pattern for checking if file exists, getting its size and reading it
 */
static char *read_qp = "SELECT %s FROM %s WHERE %s = %s";

/**
 * Function call pattern that returns value length
 */
static char *size_fp = "LENGTH(%s)";

/**
 * Returns if str consists only of decimal digits
 */
my_bool is_uint(const char *str)
{
    int i;

    for (i = 0; i < strlen(str); i++)
    {
        if (!isdigit(str[i]))
        {
            return 0;
        }
    }

    return 1;
}

/**
 * Returns if str is a valid MySQL identifier, which can be used without
 * hyphens
 */
my_bool is_valid_ident(const char *str)
{
    int i;

    for (i = 0; i < strlen(str); i++)
    {
        if (!isalnum(str[i]) || (str[i] != '_'))
        {
            return 0;
        }
    }

    return 1;
}

/**
 * Returns is path is a valid file or directory path
 */
my_bool is_valid_path(const char *path)
{
    if (strcmp(path, "/") == 0)
    {
        return 1;
    }

    if (path[0] != '/')
    {
        return 0;
    }

    if (!is_uint(path + 1))
    {
        return 0;
    }

    return 1;
}

/**
 * Returns stat info about specified row. Both directory and files are
 * read-only. We don't verify that a particular file exists since it
 * would result in an extra query per file (resulting in a huge database
 * load when listing files in a directory)
 */
static int my_getattr(const char *path, struct stat *stbuf)
{
    char *query, *filename, *my_data_field_size;
    int result;
    MYSQL_RES *res;
    MYSQL_ROW row;

    if (!is_valid_path(path))
    {
        return -ENOENT;
    }

    memset(stbuf, 0, sizeof(struct stat));

    //
    // Path points to the only directory, return its attributes
    //

    if (strcmp(path, "/") == 0)
    {
        stbuf->st_mode = S_IFDIR | 0555;
        stbuf->st_nlink = 2;
        stbuf->st_uid = getuid();
        stbuf->st_gid = getgid();
        return 0;
    }

    //
    // Copy filename part from path
    //

    filename = (char*) malloc(strlen(path) + 1);
    if (filename == NULL)
    {
        return -ENOENT;
    }

    strcpy(filename, path + 1);

    //
    // Query file size from the database
    //

    my_data_field_size = (char*) malloc(strlen(size_fp) + strlen(my_data_field) + 1);

    if (my_data_field_size == NULL)
    {
        free(filename);
        return;
    }

    sprintf(my_data_field_size, size_fp, my_data_field);

    query = (char*) malloc(strlen(read_qp) + strlen(my_data_field_size) + strlen(my_table) +
            strlen(my_name_field) + strlen(filename));

    if (query == NULL)
    {
        free(my_data_field_size);        
        free(filename);
        return;
    }

    sprintf(query, read_qp, my_data_field_size, my_table, my_name_field, filename);
    mysql_real_query(&mysql, query, (unsigned int) strlen(query));
    res = mysql_use_result(&mysql);

    free(query);
    free(my_data_field_size);
    free(filename);    

    if (res == NULL)
    {
        return -ENOENT;
    }

    //
    // Return file size
    //

    row = mysql_fetch_row(res);

    if (row == NULL)
    {
        result = -ENOENT;
    }
    else
    {
        stbuf->st_mode = S_IFREG | 0555;
        stbuf->st_nlink = 1;
        stbuf->st_size = atoi(row[0]);
        stbuf->st_uid = getuid();
        stbuf->st_gid = getgid();
        result = 0;
    }

    mysql_free_result(res);

    return result;
}

/**
 * Returns list of name field values of all rows. Only path "/" is supported
 */
static int my_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t offset, struct fuse_file_info *fi)
{
    char *query;
    MYSQL_RES *res;
    MYSQL_ROW row;

    if (strcmp(path, "/") != 0)
    {
        return -ENOENT;
    }

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    query = (char*) malloc(strlen(readdir_qp) + strlen(my_name_field) + strlen(my_table) +
            strlen(my_name_field));

    if (query == NULL)
    {
        return -ENOENT;
    }

    sprintf(query, readdir_qp, my_name_field, my_table, my_name_field);
    mysql_real_query(&mysql, query, (unsigned int) strlen(query));
    res = mysql_use_result(&mysql);

    if (res == NULL)
    {
        return -ENOENT;
    }

    while (row = mysql_fetch_row(res))
    {
        filler(buf, row[0], NULL, 0);
    }

    mysql_free_result(res);

    return 0;
} 

/**
 * Allows to open directory "/" and files inside it, which have corresponding
 * rows
 */
static int my_open(const char* path, struct fuse_file_info *fi)
{
    char *query, *filename;
    int result;
    MYSQL_RES *res;
    MYSQL_ROW row;

    if (!is_valid_path(path))
    {
        return -ENOENT;
    }

    if (fi->flags & O_RDONLY == 0)
    {
        return -ENOENT;
    }

    //
    // Path points to the only directory, allow to open it
    //

    if (strcmp(path, "/") == 0)
    {
        return 0;
    }

    //
    // Copy filename part from path
    //

    filename = (char*) malloc(strlen(path) + 1);
    if (filename == NULL)
    {
        return -ENOENT;
    }

    strcpy(filename, path + 1);

    //
    // Query if file exists in the database
    //

    query = (char*) malloc(strlen(read_qp) + 1 + strlen(my_table) +
            strlen(my_name_field) + strlen(filename));

    if (query == NULL)
    {
        free(filename);
        return -ENOENT;
    }

    sprintf(query, read_qp, "1", my_table, my_name_field, filename);
    mysql_real_query(&mysql, query, (unsigned int) strlen(query));
    res = mysql_use_result(&mysql);

    free(query);
    free(filename);    

    if (res == NULL)
    {
        return -ENOENT;
    }

    //
    // Return if file exists
    //

    row = mysql_fetch_row(res);

    if (row == NULL)
    {
        result = -ENOENT;
    }
    else
    {
        result = 0;
    }

    mysql_free_result(res);

    return result;
}

/**
 * Returns size bytes from the data field value of record identified by path,
 * starting from byte offset
 */
static int my_read(const char *path, char *buf, size_t size, off_t offset,
                   struct fuse_file_info *fi)
{
    char *query, *filename;
    unsigned long *lengths, len;
    MYSQL_RES *res;
    MYSQL_ROW row;

    //
    // Check if path is valid and points to a file
    //

    if (!is_valid_path(path))
    {
        return -ENOENT;
    }

    if (strcmp(path, "/") == 0)
    {
        return -ENOENT;
    }

    //
    // Copy filename part from path
    //

    filename = (char*) malloc(strlen(path) + 1);
    if (filename == NULL)
    {
        return -ENOENT;
    }

    strcpy(filename, path + 1);

    //
    // Query file contents from the database
    //

    query = (char*) malloc(strlen(read_qp) + strlen(my_data_field) + strlen(my_table) +
            strlen(my_name_field) + strlen(filename));

    if (query == NULL)
    {
        free(filename);
        return -ENOENT;
    }

    sprintf(query, read_qp, my_data_field, my_table, my_name_field, filename);
    mysql_real_query(&mysql, query, (unsigned int) strlen(query));
    res = mysql_use_result(&mysql);

    free(query);
    free(filename);    

    if (res == NULL)
    {
        return -ENOENT;
    }

    //
    // Copy part of the file, specified by offset and size
    //

    row = mysql_fetch_row(res);
    if (row != NULL)
    {
        lengths = mysql_fetch_lengths(res);
        len = lengths[0];

        if (offset <= len)
        {
            if (offset + size > len)
            {
                size = len - offset;
            }

            memcpy(buf, row[0] + offset, size);
        }
        else
        {
            size = 0;
        }    
    }
    else
    {

        size = -ENOENT;
    }

    mysql_free_result(res);

    return size;
}

/**
 * Operations implemented by MyBLOBFS
 */
static struct fuse_operations my_oper =
{
    .getattr = my_getattr,
    .readdir = my_readdir,
    .open    = my_open,
    .read    = my_read
};

/**
 * Custom command-line options to be parsed
 */
static struct fuse_opt hello_opts[] =
{
    MYBLOBFS_OPT_KEY("--host=%s", hostname,    0),
    MYBLOBFS_OPT_KEY("--port=%u", port,        0),
    MYBLOBFS_OPT_KEY("--user=%s", username,    0),
    MYBLOBFS_OPT_KEY("-p",    rq_password, 1),
    MYBLOBFS_OPT_KEY("--database=%s", database,    0),
    MYBLOBFS_OPT_KEY("--table=%s", table,       0),
    MYBLOBFS_OPT_KEY("--name-field=%s", name_field,  0),
    MYBLOBFS_OPT_KEY("--data-field=%s", data_field,  0),

    FUSE_OPT_END
};

/**
 * Program entry point
 */
int main(int argc, char *argv[])
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct options opts;
    char *pwd, *password;
    int ret;
    
    //
    // Parse command-line options
    //

    memset(&opts, 0, sizeof(struct options));

    if (fuse_opt_parse(&args, &opts, hello_opts, NULL) == -1)
    {
        return 0;
    }

    if (opts.port == 0)
    {
        opts.port =  3306;
    }

    //
    // Read password from command-line, if -p flag was specifed
    //

    if (opts.rq_password)
    {
        pwd = getpass("Enter password: ");
        if (pwd == NULL)
        {
            puts("Out of memory");
            return 0;
        }

        password = (char*) malloc(strlen(pwd) + 1);
        if (password == NULL)
        {
            puts("Out of memory");
            return 0;
        }

        strcpy(password, pwd);
    }


    //
    // Try to connect to MySQL database
    //

    mysql_init(&mysql);
    if (mysql_real_connect(&mysql, opts.hostname, opts.username, password, 
                            opts.database, opts.port, NULL, 0) == NULL)
    {
        puts(mysql_error(&mysql));
        return 0;
    }

    free(password);

    //
    // Copy table and field names to global variables
    //

    my_table = (char*) malloc(strlen(opts.table) + 1);
    if (my_table == NULL)
    {
        puts("Out of memory");
        return 0;
    }

    strcpy(my_table, opts.table);

    my_name_field = (char*) malloc(strlen(opts.name_field) + 1);
    if (my_name_field == NULL)
    {
        puts("Out of memory");
        return 0;
    }

    strcpy(my_name_field, opts.name_field);

    my_data_field = (char*) malloc(strlen(opts.data_field) + 1);
    if (my_data_field == NULL)
    {
        puts("Out of memory");
        return 0;
    }

    strcpy(my_data_field, opts.data_field);

    //
    // Give control to FUSE library
    //

    ret = fuse_main(args.argc, args.argv, &my_oper);

    if (ret)
    {
        puts("");
    }

    free(my_data_field);
    free(my_name_field);
    free(my_table);

    fuse_opt_free_args(&args);

    return 0;
}