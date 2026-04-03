#include <stdio.h>
#include <syslog.h>
#include <string.h>

int main(int argc, char **argv)
{
    openlog(NULL, 0, LOG_USER);

    if (argc != 3)
    {
        syslog(LOG_ERR, "Wrong number of arguments given. Usage: %s <writefile> <writestr>", argv[0]);
        closelog();
        return 1;
    }

    const char* writefile = argv[1];
    const char* writestr  = argv[2];
    if ((strlen(writefile) == 0) || (strlen(writestr) == 0))
    {
        syslog(LOG_ERR, "At least one parameter was not specified. Usage: %s <writefile> <writestr>", argv[0]);
        closelog();
        return 1;
    }

    syslog(LOG_DEBUG, "Writing %s to %s.", writestr, writefile);

    // no need to create directory, assume directory already exists
    FILE *fptr = fopen(argv[1], "w");
    if (!fptr)
    {
        syslog(LOG_ERR, "File could not be opened.");
        closelog();
        return 1;
    }

    const int rv = fputs(writestr, fptr);
    if (rv < 0)
    {
        syslog(LOG_ERR, "Error writing to file.");
        fclose(fptr);
        closelog();
        return 1;
    }

    fclose(fptr);
    closelog();
    return 0;
}