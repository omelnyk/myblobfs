/**
 * MyBlobFS - virtual user-space file system driver for mounting MySQL table
 *   rows as local files for read-only access
 *
 * Portions Copyright (C) 2001-2005  Miklos Szeredi <miklos@szeredi.hu>
 * Copyright (C) 2008, 2009 Olexandr Melnyk <me@omelnyk.net>
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
 * Macro for short command-line options definition
 */
#define MYBLOBFS_OPT_KEY(t, p, v) { t, offsetof(struct options, p), v }

/**
 * Structure for command-line options
 */
struct options
{
	/**
	 * Hostname of MySQL server
	 */
    char *hostname;

	/**
	 * Remote port 
	 */
    unsigned int port;

	/**
	 * Name of MySQL user
	 */
    char *username;

	/**
	 * Whether to prompt user for password
	 */
    int rq_password;

	/**
	 * Database name
	 */
    char *database;

	/**
	 * Table name
	 */
    char *table;

	/**
	 * Name of the field containing filename
	 */
    char *name_field;

	/**
	 * Name of the field with file content
	 */
    char *data_field;
};

/**
 * Custom command-line options
 */
static struct fuse_opt hello_opts[] =
{
    MYBLOBFS_OPT_KEY("--host=%s",       hostname,    0),
    MYBLOBFS_OPT_KEY("--port=%u",       port,        0),
    MYBLOBFS_OPT_KEY("--user=%s",       username,    0),
    MYBLOBFS_OPT_KEY("-p",              rq_password, 1),
    MYBLOBFS_OPT_KEY("--database=%s",   database,    0),
    MYBLOBFS_OPT_KEY("--table=%s",      table,       0),
    MYBLOBFS_OPT_KEY("--name-field=%s", name_field,  0),
    MYBLOBFS_OPT_KEY("--data-field=%s", data_field,  0),

    FUSE_OPT_END
};

/**
 * Table name
 */
static char *my_table;

/**
 * Name of the field containing filename. Field must be declared as either: 
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
 * Query pattern for fetching file names
 */
static char *readdir_qp = "SELECT %s FROM %s ORDER BY %s";

/**
 * Query pattern for checking if file exists, getting its size and reading it
 */
static char *read_qp = "SELECT %s FROM %s WHERE %s = %s";

/**
 * Function call pattern that returns file size
 */
static char *size_fp = "LENGTH(%s)";

/**
 * Returns if str consists only of one or more decimal digits
 */
my_bool is_uint(const char *str)
{
    int i;

	if (strlen(str) == 0)
	{
		return 0;
	}

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
 * being enclosed in hyphens
 */
my_bool is_valid_ident(const char *str)
{
    int i;

	if (strlen(str) == 0)
	{
		return 0;
	}

    for (i = 0; i < strlen(str); i++)
    {
        if (!isalnum(str[i]) && (str[i] != '_'))
        {
            return 0;
        }
    }

    return 1;
}

/**
 * Returns if path is a valid relative file or directory path. Valid paths
 * are: "/" (file system root directory) and "/id" (file representing record
 * with primary key "id", where "id" is an unsigned integer value)
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
 * Returns stat info of the specified file
 *
 * TODO: For memory/connection errors use error code other than -ENOENT
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
    // Path points to the only directory, use its static attributes
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
    // Path points to one of the files, get its attributes from the database
    //

    result = 0;

    filename = (char*) malloc(strlen(path) + 1);
    if (filename != NULL)
    {
        strcpy(filename, path + 1);

        //
        // Query file size from the database
        //

        my_data_field_size = (char*) malloc(strlen(size_fp) + strlen(my_data_field) + 1);

        if (my_data_field_size != NULL)
        {

            sprintf(my_data_field_size, size_fp, my_data_field);

            query = (char*) malloc(strlen(read_qp) + strlen(my_data_field_size) + 
                    strlen(my_table) + strlen(my_name_field) + strlen(filename));

            if (query != NULL)
            {
                sprintf(query, read_qp, my_data_field_size, my_table, my_name_field, filename);
                mysql_real_query(&mysql, query, (unsigned int) strlen(query));
                res = mysql_use_result(&mysql);

                if (res != NULL)
                {

                    //
                    // If specified filename has a corresponding row in the
                    // database, return its information. Else, report that
                    // there is no such file
                    //

                    row = mysql_fetch_row(res);

                    if (row != NULL)
                    {
                        stbuf->st_mode = S_IFREG | 0555;
                        stbuf->st_nlink = 1;
                        stbuf->st_size = atoi(row[0]);
                        stbuf->st_uid = getuid();
                        stbuf->st_gid = getgid();
                    }
                    else
                    {
                        result = -ENOENT;
                    }
 
                    mysql_free_result(res);
                }
                else
                {
                    result = -ENOENT;
                }

                free(query);
             }
            else
            {   
                result = -ENOENT;
            }

            free(my_data_field_size);
        }
        else
        {
            result = ENOENT;
        }

        free(filename);
    }
    else
    {
        result = -ENOENT;
    }

    return result;

}

/**
 * Returns list of all files in the specified directory. The only supported
 * directory path is "/"
 *
 * TODO: If path is correct but points to a file, return -ENOTDIR instead of
 *   -ENOENT
 */
static int my_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
    off_t offset, struct fuse_file_info *fi)
{
    char *query;
    MYSQL_RES *res;
    MYSQL_ROW row;
	int result;

	//
	// Make sure that the only directory ("/") was requested
	//

    if (strcmp(path, "/") != 0)
    {
        return -ENOENT;
    }

	//
	// Add two virtual directories: "." and ".."
	//

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

	//
	// Query list of files from the database
	//

    query = (char*) malloc(strlen(readdir_qp) + strlen(my_name_field) + 
        strlen(my_table) + strlen(my_name_field));

    if (query != NULL)
	{
    	sprintf(query, readdir_qp, my_name_field, my_table, my_name_field);
    	mysql_real_query(&mysql, query, (unsigned int) strlen(query));
    	res = mysql_use_result(&mysql);

    	if (res != NULL)
		{
	    	while (row = mysql_fetch_row(res))
    		{
    	    	filler(buf, row[0], NULL, 0);
    		}

	    	mysql_free_result(res);

			result = 0;
		}
		else
    	{
        	result = -ENOENT;
    	}

		free(query);
	}
	else
    {
        result = -ENOMEM;
    }

    return result;
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

	//
	// Check for path validity and disallow write requests
	//

    if (!is_valid_path(path))
    {
        return -ENOENT;
    }

    if (fi->flags & O_RDONLY == 0)
    {
        return -EROFS;
    }

    //
    // Path points to the only directory, allow to open it
    //

    if (strcmp(path, "/") == 0)
    {
        return 0;
    }

    //
    // Query if file exists in the database
    //

    filename = (char*) malloc(strlen(path) + 1);
    if (filename != NULL)
	{
    	strcpy(filename, path + 1);

	    query = (char*) malloc(strlen(read_qp) + 1 + strlen(my_table) +
            strlen(my_name_field) + strlen(filename));

	    if (query != NULL)
	    {
		    sprintf(query, read_qp, "1", my_table, my_name_field, filename);
		    mysql_real_query(&mysql, query, (unsigned int) strlen(query));
		    res = mysql_use_result(&mysql);
	
		    if (res != NULL)
			{
	
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
			}
			else
		    {
		        result = -EAGAIN;
		    }

			free(query);	
	    }
		else
		{
			result = -ENOMEM;
		}
	
		free(filename);
	}
	else
	{
        result = -ENOMEM;
    }

    return result;
}

/**
 * Returns size bytes from the file identified by path, starting from byte offset 
 *
 * TODO: chop the desired block of data using SQL, rather than on the client side
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
        return -EISDIR;
    }

    //
    // Query file content from the database
   	//

    filename = (char*) malloc(strlen(path));
    if (filename != NULL)
	{
    	strcpy(filename, path + 1);

    	query = (char*) malloc(strlen(read_qp) + strlen(my_data_field) +
    	   strlen(my_table) + strlen(my_name_field) + strlen(filename) + 1);

    	if (query != NULL)
		{
    		sprintf(query, read_qp, my_data_field, my_table, my_name_field, filename);
    		mysql_real_query(&mysql, query, (unsigned int) strlen(query));
    		res = mysql_use_result(&mysql);

    		if (res != NULL)
			{
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
			}
			else
			{
				size = -ENOMEM;
			}
			
			free(query);
		}
		else
    	{
    	    size = -ENOMEM;
    	}
	
    	free(filename);
	}
	else
	{
        size = -ENOMEM;
    }

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
 * Program entry point
 *
 * TODO: Make sure that all memory is always free()'d
 */
int main(int argc, char *argv[])
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct options opts;
    char *password = NULL;
    int ret, res, error;

    //
    // Parse command-line options
    //

    memset(&opts, 0, sizeof(struct options));

    if (fuse_opt_parse(&args, &opts, hello_opts, NULL) == -1)
    {
        return 0;
    }

    if (opts.database != NULL)
    {
        if (opts.table != NULL)
        {
            if (opts.name_field != NULL)
            {
                if (opts.data_field != NULL)
                {
                   	my_table = (char*) malloc(strlen(opts.table) + 1);
                	if (my_table != NULL)
                	{
                   		my_name_field = (char*) malloc(strlen(opts.name_field) + 1);
                		if (my_name_field != NULL)
                		{
                		    my_data_field = (char*) malloc(strlen(opts.data_field) + 1);
                			if (my_data_field != NULL)
                			{
                   				if (opts.port == 0)
                   				{
                       				opts.port = 3306; // FIXME
                   				}

                	    		//
                   				// Read password from command line, if -p flag was specifed
                   				//

                		    	if (opts.rq_password)
                   				{
                   					password = getpass("Enter password: ");
                   				}

                       			if (!opts.rq_password || password != NULL)
                       			{
                    				//
                    				// Copy command-line option values to global variables
                    				//

                    				strcpy(my_table, opts.table);
                    				strcpy(my_name_field, opts.name_field);
                    				strcpy(my_data_field, opts.data_field);
 
                   					//
                    				// Try to connect to MySQL database
                    				//

                					//
                					// TODO: Password should be freed even in case of MySQL error
                					//

                			    	mysql_init(&mysql);
                					if (mysql_real_connect(&mysql, opts.hostname, 
                					  opts.username, password, opts.database, opts.port,
                					  NULL, 0) != NULL)
                    				{
                						if (!is_valid_ident(my_table))
                    					{
                      						puts("Error: Illegal characters in table name identifier");
                							error = 1;
                    					}

                						//
                						// Verify table and field names validity
                						//

                						error = 0;
	
                						if (!is_valid_ident(my_table))
                    					{
                      						puts("Error: Illegal characters in table name identifier");
                							error = 1;
                    					}

                						if (!is_valid_ident(my_name_field))
                    					{
                      						puts("Error: Illegal characters in ""name"" field identifier");
                							error = 1;
                    					}

                						if (!is_valid_ident(my_data_field))
                    					{
                      						puts("Error: Illegal characters in ""data"" field identifier");
                							error = 1;
                    					}

                						if (!error)
                						{
                							//
                							// Give control to FUSE library
                							//

                   							ret = fuse_main(args.argc, args.argv, &my_oper);
                    						if (ret)
                       						{
                        						puts("");
                    						}
                						}
                                    }
                					else
                					{
                	        			puts(mysql_error(&mysql));
                					}

                                    if (password != NULL)
                                    {
                					    free(password);
                                    }
                    			}

                				free(my_data_field);
                			}
                			else
                			{
                      			puts("Out of memory");
                			}

                			free(my_name_field);
                		}
                    	else
                    	{
                      		puts("Out of memory");
                    	}

                		free(my_table);
                	}
                    else
                    {
                      	puts("Out of memory");
                    }
                }
                else
                {
                    puts("Name field must be specified");
                }
            }
            else
            {
                puts("Date field must be specified");
            }
        }
        else
        {
            puts("Table name must be specified");
        }
    }
    else
    {
        puts("Database name must be specified");
    }

    fuse_opt_free_args(&args);

    return 0;
}
