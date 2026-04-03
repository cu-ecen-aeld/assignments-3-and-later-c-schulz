#include <stdio.h>
#include <syslog.h>
#include <string.h>

int main(int argc, char **argv)
{
    openlog(NULL, 0, LOG_USER);

    char* usage = argv[0];
    strcat(usage, " <writefile> <writestr>");

    if (argc != 3)
    {
        char* err = "Wrong number of arguments given. Usage: ";
        strcat(err, usage);

        syslog(LOG_ERR, err);
        return 1;
    }

    const char* writefile = argv[1];
    const char* writestr  = argv[2];
    if ((strlen(writefile) == 0) || (strlen(writestr) == 0))
    {
        char* err = "At least one parameter was not specified. Usage: ";
        strcat(err, usage);

        syslog(LOG_ERR, err);
        return 1;
    }

    char* info = "Writing ";
    strcat(info, writestr);
    strcat(info, " to ");
    strcat(info, writefile);
    strcat(info, ".");
    syslog(LOG_DEBUG, info);

    // no need to create directory, assume directory already exists
    FILE *fptr = fopen(writefile, "w");
    if (!fptr)
    {
        syslog(LOG_ERR, "File could not be opened.");
        return 1;
    }

    const int rv = fprintf(fptr, writestr);
    if (rv < 0)
    {
        syslog(LOG_ERR, "Error writing to file.");
        fclose(fptr);
        return 1;
    }

    fclose(fptr);
    return 0;
}